"""Exercise candidate sudo-rs with fake PAM methods inside isolated.sh."""
import errno
import os
from pathlib import Path
import pty
import pwd
import select
import signal
import sys
import termios
import time

ROOT = Path("/mnt/pressmatch-test")
PAMD = Path("/etc/pam.d")
FIXTURE = ROOT / "fixture.so"
EVENTS = ROOT / "case/events"
TOUCHES = ROOT / "case/touches"
USER = pwd.getpwnam(sys.argv[1])
PASSWORD = b"test-password\n"
COMMAND = ("/usr/bin/id", "-u")


def check(predicate, message, timeout=4):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(0.02)
    raise AssertionError(message)


class Case:
    def __init__(self, *, account_denied=False, fallback=False, options=(), command=COMMAND):
        EVENTS.write_text("")
        TOUCHES.unlink(missing_ok=True)
        os.mkfifo(TOUCHES, 0o600)
        self.touches = os.open(TOUCHES, os.O_RDWR | os.O_CLOEXEC)
        standard = (
            f"auth required {FIXTURE}\n"
            f"account required pam_{'deny' if account_denied else 'permit'}.so\n"
            "session required pam_permit.so\n"
        )
        for service in ("sudo", "sudo-i"):
            (PAMD / service).write_text(standard)
        for service in ("sudo-pressmatch-password", "sudo-pressmatch-password-i"):
            (PAMD / service).write_text(f"auth requisite {FIXTURE}\n")
        fingerprint = PAMD / "sudo-pressmatch-fingerprint"
        if fallback:
            fingerprint.unlink(missing_ok=True)
        else:
            fingerprint.write_text(f"auth required {FIXTURE} fingerprint\n")
        self.pid, self.terminal = pty.fork()
        if not self.pid:
            os.initgroups(USER.pw_name, USER.pw_gid)
            os.setgid(USER.pw_gid)
            os.setuid(USER.pw_uid)
            binary = str(ROOT / "sudo")
            os.execv(binary, (binary, "-k", *options, *command))
        self.output = bytearray()
        self.status = None

    def read(self):
        if not select.select([self.terminal], [], [], 0.02)[0]:
            return
        try:
            self.output.extend(os.read(self.terminal, 4096))
        except OSError as e:
            if e.errno != errno.EIO:
                raise

    def prompt(self, count=1):
        def seen():
            self.read()
            return self.output.count(b"Password:") >= count
        check(seen, "password prompt was not available")

    def events(self, value, count=1):
        check(lambda: EVENTS.read_text().splitlines().count(value) >= count, f"missing event: {value}")

    def touch(self, data):
        self.events("armed")
        os.write(self.touches, data)

    def type(self, value):
        os.write(self.terminal, value)

    def poll_exit(self):
        self.read()
        pid, status = os.waitpid(self.pid, os.WNOHANG)
        if pid:
            self.status = os.waitstatus_to_exitcode(status)
        return pid != 0

    def finish(self, *, succeeded, timeout=4):
        check(self.poll_exit, "sudo did not exit", timeout)
        assert (self.status == 0) == succeeded, self.status
        assert (b"\r\n0\r\n" in self.output) == succeeded
        settings = termios.tcgetattr(self.terminal)
        assert settings[3] & termios.ECHO and settings[3] & termios.ICANON, "TTY was left altered"

    def __enter__(self):
        return self

    def __exit__(self, error_type, *_):
        if error_type:
            print("Captured output:", bytes(self.output), "events:", EVENTS.read_text(), file=sys.stderr)
        if self.status is None:
            try:
                os.kill(self.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            os.waitpid(self.pid, 0)
        os.close(self.touches)
        os.close(self.terminal)


with Case() as case:
    case.prompt()
    case.events("armed")
    case.type(PASSWORD)
    case.finish(succeeded=True)
    case.events("cancelled")
print("password success cancels the fingerprint worker")

with Case() as case:
    case.prompt()
    case.type(b"wrong\n")
    case.prompt(2)
    case.touch(b"m")
    case.finish(succeeded=True)
print("wrong password leaves fingerprint active")

with Case() as case:
    case.prompt()
    case.touch(b"xxxxxm")
    case.events("fingerprint retry", 5)
    case.events("exhausted")
    case.type(PASSWORD)
    case.finish(succeeded=True)
print("five fingerprint mismatches leave the password prompt usable")

with Case() as case:
    case.prompt()
    case.type(b"wrong\n")
    case.prompt(2)
    case.type(b"wrong-again\n")
    check(lambda: (case.read(), b"Password authentication ended" in case.output)[1],
          "missing password exhaustion notice")
    case.touch(b"m")
    case.finish(succeeded=True)
print("password exhaustion leaves fingerprint active")

with Case() as case:
    case.prompt()
    case.type(b"wrong\n")
    case.prompt(2)
    case.type(b"wrong-again\n")
    case.touch(b"xxxxx")
    case.finish(succeeded=False)
print("exhausting both methods denies execution")

with Case() as case:
    case.prompt()
    case.type(b"partial-test-secret")
    case.touch(b"m")
    case.finish(succeeded=True)
    assert b"partial-test-secret" not in case.output
print("fingerprint interrupts password entry and restores the TTY")

with Case() as case:
    case.prompt()
    case.type(b"slow-test-password\n")
    case.events("password checking")
    case.touch(b"m")
    case.finish(succeeded=True, timeout=2.5)
print("fingerprint interrupts a password check in progress")

with Case() as case:
    case.prompt()
    case.events("armed")
    case.type(b"\x03")
    case.finish(succeeded=False)
    case.events("cancelled")
    assert case.status == -signal.SIGINT
print("Ctrl+C cancels both workers and preserves normal terminal settings")

with Case(account_denied=True) as case:
    case.prompt()
    case.touch(b"m")
    case.finish(succeeded=False)
print("the original PAM account policy still gates fingerprint success")

with Case(command=("/usr/bin/id",)) as case:
    case.finish(succeeded=False)
    assert not EVENTS.read_text()
print("sudoers still gates command execution before either worker starts")

with Case(options=("-n",)) as case:
    case.finish(succeeded=False)
    assert not EVENTS.read_text() and b"Password:" not in case.output
print("noninteractive sudo retains its existing behavior")

with Case(fallback=True) as case:
    case.prompt()
    case.type(PASSWORD)
    case.finish(succeeded=True)
    assert not EVENTS.read_text()
print("a missing parallel profile retains stock PAM authentication")
