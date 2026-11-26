/* Synthetic authentication module for the private PTY test namespace only. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <security/pam_modules.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CASE "/mnt/pressmatch-test/case/"
static volatile sig_atomic_t cancelled;

static void record(const char *event)
{
    int fd = open(CASE "events", O_WRONLY | O_APPEND | O_CLOEXEC);
    if (fd >= 0) {
        (void) write(fd, event, strlen(event));
        close(fd);
    }
}

static void cancel(int sig)
{
    (void) sig;
    cancelled = 1;
}

static int fingerprint(void)
{
    struct sigaction action = { .sa_handler = cancel };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    int fd = open(CASE "touches", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return PAM_AUTHINFO_UNAVAIL;

    record("armed\n");
    for (int tries = 0; tries < 5 && !cancelled; ++tries) {
        char touch;
        if (read(fd, &touch, 1) != 1)
            break;
        if (touch == 'm') {
            record("matched\n");
            close(fd);
            return PAM_SUCCESS;
        }
        record("fingerprint retry\n");
    }
    record(cancelled ? "cancelled\n" : "exhausted\n");
    close(fd);
    return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void) flags;
    if (argc == 1 && strcmp(argv[0], "fingerprint") == 0)
        return fingerprint();

    const void *raw;
    if (pam_get_item(pamh, PAM_CONV, &raw) != PAM_SUCCESS)
        return PAM_CONV_ERR;
    const struct pam_conv *conversation = raw;
    const struct pam_message message = { PAM_PROMPT_ECHO_OFF, "Password: " };
    const struct pam_message *messages[] = { &message };
    struct pam_response *response = NULL;
    int status = conversation->conv(1, messages, &response, conversation->appdata_ptr);
    if (status != PAM_SUCCESS || !response || !response->resp)
        return PAM_CONV_ERR;

    int valid = strcmp(response->resp, "test-password") == 0;
    int slow = strcmp(response->resp, "slow-test-password") == 0;
    explicit_bzero(response->resp, strlen(response->resp));
    free(response->resp);
    free(response);
    if (slow) {
        record("password checking\n");
        sleep(8);
    }
    return valid ? PAM_SUCCESS : PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void) pamh;
    (void) flags;
    (void) argc;
    (void) argv;
    return PAM_SUCCESS;
}
