import json
import serial
from PyQt5.QtCore import QThread, pyqtSignal


class BluetoothWorker(QThread):
    """
    Background thread for Bluetooth SPP communication.
    Reads JSON lines from the ESP32 and emits parsed packets.
    Supports sending commands back to the firmware.
    """

    data_received = pyqtSignal(dict)
    connection_lost = pyqtSignal(str)

    def __init__(self, port: str, baud: int = 115200):
        super().__init__()
        self.port = port
        self.baud = baud
        self._running = False
        self._ser = None

    def run(self):
        self._running = True
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=2)
            while self._running:
                try:
                    raw = self._ser.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8").strip()
                    if not line:
                        continue
                    packet = json.loads(line)
                    self.data_received.emit(packet)
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
        except serial.SerialException as e:
            self.connection_lost.emit(str(e))
        finally:
            if self._ser and self._ser.is_open:
                self._ser.close()
            self._ser = None

    def send(self, text: str):
        """Send a command string to the firmware over the open BT SPP link."""
        if self._ser and self._ser.is_open:
            self._ser.write((text + "\n").encode("utf-8"))
            self._ser.flush()

    def stop(self):
        self._running = False
        self.wait()