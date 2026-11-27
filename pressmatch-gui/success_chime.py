"""Play the bundled fingerprint confirmation through the desktop audio server."""

from pathlib import Path
from shutil import which

from PyQt6.QtCore import QProcess


class SuccessChime:
    def __init__(self, parent):
        self._process = QProcess(parent)
        self._process.setStandardOutputFile(QProcess.nullDevice())
        self._process.setStandardErrorFile(QProcess.nullDevice())
        sound = str(Path(__file__).resolve().parent / "sounds" / "fingerprint-success.wav")
        if player := which("paplay"):
            self._process.setProgram(player)
            self._process.setArguments(["--client-name=PressMatch", "--property=media.role=event", sound])
        elif player := which("pw-play"):
            self._process.setProgram(player)
            self._process.setArguments(["--media-role=Notification", sound])

    def play(self) -> None:
        if self._process.program() and self._process.state() == QProcess.ProcessState.NotRunning:
            self._process.start()
