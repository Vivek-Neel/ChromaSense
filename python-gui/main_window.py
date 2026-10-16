"""
ChromaSense — Main Application Window
=====================================
PyQt5 dashboard that:
  • Auto-detects available COM ports
  • Connects to ESP32 over Bluetooth SPP via BluetoothWorker
  • Displays live telemetry (lux, CCT, battery, XYZ, CIE xy)
  • Renders a real-time CIE 1931 chromaticity diagram

Layout: CIE diagram fills the left side at a fixed 1:1 aspect ratio,
telemetry/control panels in a fixed-width column on the right.

Worker runs in a QThread; data arrives via Qt signals.
"""

import csv
import os
import sys
from datetime import datetime

import serial.tools.list_ports
from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtGui import QFont, QPixmap
from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget, QFrame,
    QVBoxLayout, QHBoxLayout,
    QLabel, QPushButton, QStatusBar, QComboBox, QSizePolicy,
)

from bt_worker import BluetoothWorker
from cie_widget import CIEWidget

# ── Serial config ─────────────────────────────────────────────
BAUD = 115200

# ── Theme colours ─────────────────────────────────────────────
BG          = "#1a1a2e"
PANEL_BG    = "#16213e"
ACCENT      = "#00f5d4"
AMBER       = "#ffbe0b"
GREEN       = "#06d6a0"
TEXT_DIM    = "#8892a4"
TEXT_MAIN   = "#e0e0e0"
BORDER      = "#2a2a4a"
INPUT_BG    = "#0d0d1a"
DANGER_BG   = "#e63946"
DANGER_HOV  = "#c1121f"
SUCCESS_HOV = "#048a68"
REC_BG      = "#ff3864"
REC_HOV     = "#d61f4b"

# ── Connect button style — two states ─────────────────────────
_BTN_BASE = """
    QPushButton {{
        background:{bg}; color:white;
        border-radius:8px; padding:10px 18px;
        font-size:13px; font-weight:600;
    }}
    QPushButton:hover {{ background:{hover}; }}
"""
BTN_STYLE_CONNECT    = _BTN_BASE.format(bg=GREEN,     hover=SUCCESS_HOV)
BTN_STYLE_DISCONNECT = _BTN_BASE.format(bg=DANGER_BG, hover=DANGER_HOV)
BTN_STYLE_LOG_START  = _BTN_BASE.format(bg=PANEL_BG,  hover=BORDER)
BTN_STYLE_LOG_STOP   = _BTN_BASE.format(bg=REC_BG,    hover=REC_HOV)

BTN_STYLE_DARK_OFF = _BTN_BASE.format(bg="#333355", hover="#444477")
BTN_STYLE_DARK_ON  = _BTN_BASE.format(bg="#F5A623", hover="#E6951A")

# Fixed width of the right-hand data/control column
RIGHT_COL_WIDTH = 560


# ── Colour gradients for live-value colour coding ───────────────
# CCT: warm-to-cool slider convention (not physical blackbody colour)
CCT_STOPS = [
    (1000,  (255,  51,   0)),   # deep red-orange (candle)
    (2000,  (255, 140,  26)),   # tungsten orange
    (2700,  (255, 180, 107)),   # warm white
    (3500,  (255, 217, 160)),
    (4500,  (255, 242, 224)),
    (5500,  (255, 255, 255)),   # daylight white
    (6500,  (207, 232, 255)),   # cool white
    (8000,  (159, 201, 255)),
    (10000, (111, 168, 255)),   # blue (overcast sky)
]

# Battery: red -> amber -> green
BATT_STOPS = [
    (0,   (230,  57,  70)),   # red
    (25,  (255, 190,  11)),   # amber
    (60,  (6,   214, 160)),   # green
    (100, (6,   214, 160)),
]


def _interpolate_stops(value: float, stops):
    """Linearly interpolate an RGB colour between (value, (r,g,b)) stops.
    Clamps to the first/last stop outside the defined range."""
    if value <= stops[0][0]:
        r, g, b = stops[0][1]
        return r, g, b
    if value >= stops[-1][0]:
        r, g, b = stops[-1][1]
        return r, g, b

    for (v0, c0), (v1, c1) in zip(stops, stops[1:]):
        if v0 <= value <= v1:
            t = (value - v0) / (v1 - v0) if v1 != v0 else 0.0
            r = c0[0] + (c1[0] - c0[0]) * t
            g = c0[1] + (c1[1] - c0[1]) * t
            b = c0[2] + (c1[2] - c0[2]) * t
            return r, g, b

    # Should be unreachable given the clamps above.
    r, g, b = stops[-1][1]
    return r, g, b


def cct_to_hex(kelvin: float) -> str:
    r, g, b = _interpolate_stops(kelvin, CCT_STOPS)
    return f"#{int(r):02x}{int(g):02x}{int(b):02x}"


def batt_to_hex(percent: float) -> str:
    r, g, b = _interpolate_stops(percent, BATT_STOPS)
    return f"#{int(r):02x}{int(g):02x}{int(b):02x}"


# ── Helper: styled metric panel ───────────────────────────────
def make_panel(title: str):
    """Create a styled metric panel with title + value label."""
    panel = QFrame()
    panel.setStyleSheet(f"""
        QFrame {{
            background: {PANEL_BG};
            border: 1px solid {BORDER};
            border-radius: 10px;
        }}
    """)
    vbox = QVBoxLayout(panel)
    vbox.setContentsMargins(14, 10, 14, 10)
    vbox.setSpacing(2)

    lbl_title = QLabel(title)
    lbl_title.setStyleSheet(
        f"color:{TEXT_DIM}; font-size:10px; font-weight:700; "
        f"letter-spacing:1.2px; border:none;"
    )
    lbl_title.setAlignment(Qt.AlignCenter)

    lbl_value = QLabel("—")
    lbl_value.setStyleSheet(
        f"color:{ACCENT}; font-size:26px; font-weight:700; border:none;"
    )
    lbl_value.setAlignment(Qt.AlignCenter)

    vbox.addWidget(lbl_title)
    vbox.addWidget(lbl_value)
    return panel, lbl_value


# ── Helper: forces a single child widget to stay square ────────
class SquareContainer(QWidget):
    """Keeps its child widget square and centered within available space."""
    def __init__(self, child: QWidget, parent=None):
        super().__init__(parent)
        self._child = child
        child.setParent(self)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

    def resizeEvent(self, event):
        side = max(1, min(self.width(), self.height()))
        x = (self.width() - side) // 2
        y = (self.height() - side) // 2
        self._child.setGeometry(x, y, side, side)
        super().resizeEvent(event)


# ── CSV log fields, in write order ──────────────────────────────
LOG_FIELDS = ["timestamp", "lux", "cct", "x", "y", "X", "Y", "Z",
              "zy", "gain", "sat", "batt"]


# ── Main window ───────────────────────────────────────────────
class MainWindow(QMainWindow):
    """Top-level application window."""

    def __init__(self):
        super().__init__()
        self.setWindowTitle("ChromaSense — CIE 1931 Real-Time Dashboard")
        self.setMinimumSize(1270, 750)
        self.resize(1520, 900)
        self.setStyleSheet(f"background:{BG};")
        self.worker = None

        self.log_file = None
        self.dark_mode = False
        self.log_writer = None
        self.log_path = None
        self.log_count = 0

        # Avoid redundant setStyleSheet calls when colour is unchanged
        self._last_cct_color = None
        self._last_batt_color = None

        self._build_ui()

        # Auto-refresh COM port list every 3 s
        self._port_timer = QTimer(self)
        self._port_timer.timeout.connect(self._refresh_ports)
        self._port_timer.start(3000)

    # ── UI construction ───────────────────────────────────────
    def _build_ui(self):
        """Two-column layout: CIE graph left, telemetry/controls right."""
        root = QWidget()
        self.setCentralWidget(root)
        outer = QHBoxLayout(root)
        outer.setContentsMargins(16, 16, 16, 16)
        outer.setSpacing(14)

        self.cie = CIEWidget()
        self.cie.setMinimumSize(200, 200)
        self.cie_container = SquareContainer(self.cie)
        outer.addWidget(self.cie_container, stretch=1)

        right_col = self._build_right_column()
        right_wrap = QWidget()
        right_wrap.setFixedWidth(RIGHT_COL_WIDTH)
        right_wrap.setLayout(right_col)
        outer.addWidget(right_wrap, stretch=0)

        # Status bar
        self.status = QStatusBar()
        self.status.setStyleSheet(f"color:{TEXT_DIM}; font-size:11px;")
        self.setStatusBar(self.status)
        self.status.showMessage("Select a COM port and click Connect.")

    def _build_header(self):
        """Title block for the right column."""
        header = QFrame()
        header.setStyleSheet("""
            QFrame {
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                    stop:0 #16213e, stop:1 #0f3460);
                border-radius: 10px;
            }
        """)
        layout = QVBoxLayout(header)
        layout.setContentsMargins(16, 14, 16, 14)
        layout.setSpacing(6)

        top_row = QHBoxLayout()
        top_row.setSpacing(10)

        title_block = QVBoxLayout()
        title_block.setSpacing(2)
        title_block.setContentsMargins(0, 0, 0, 0)

        title = QLabel("ChromaSense")
        title.setStyleSheet(
            f"color:{ACCENT}; font-size:20px; font-weight:800; "
            f"letter-spacing:1px;"
        )
        subtitle = QLabel("Remote Color Sensor")
        subtitle.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:10px; letter-spacing:0.5px;"
        )
        title_block.addWidget(title)
        title_block.addWidget(subtitle)

        top_row.addLayout(title_block, stretch=1)

        logo_label = QLabel()
        logo_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "oth_logo.jpg"
        )
        if os.path.exists(logo_path):
            pixmap = QPixmap(logo_path)
            logo_label.setPixmap(
                pixmap.scaledToHeight(36, Qt.SmoothTransformation)
            )
        else:
            logo_label.setText("OTH\nREGENSBURG")
            logo_label.setStyleSheet(
                "color:white; font-size:9px; font-weight:700; "
                "letter-spacing:1px;"
            )
        logo_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        top_row.addWidget(logo_label)

        layout.addLayout(top_row)

        caption = QLabel("CIE 1931 Real-Time Dashboard")
        caption.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:10px; letter-spacing:0.5px;"
        )
        layout.addWidget(caption)

        return header

    def _build_right_column(self):
        col = QVBoxLayout()
        col.setSpacing(10)
        col.setContentsMargins(0, 0, 0, 0)

        col.addWidget(self._build_header())

        # Row 1: lux | CCT
        row1 = QHBoxLayout()
        row1.setSpacing(10)
        p, self.lbl_lux = make_panel("ILLUMINANCE")
        row1.addWidget(p)
        p, self.lbl_cct = make_panel("COLOUR TEMP")
        self.lbl_cct.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:26px; font-weight:700; border:none;"
        )
        row1.addWidget(p)
        col.addLayout(row1)

        # Row 2: CIE xy | battery
        row2 = QHBoxLayout()
        row2.setSpacing(10)
        p, self.lbl_xy = make_panel("CIE xy CHROMATICITY")
        self.lbl_xy.setStyleSheet(
            f"color:{TEXT_MAIN}; font-size:16px; font-weight:600; border:none;"
        )
        row2.addWidget(p)
        p, self.lbl_batt = make_panel("BATTERY")
        self.lbl_batt.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:26px; font-weight:700; border:none;"
        )
        row2.addWidget(p)
        col.addLayout(row2)

        # Row 3: X | Y | Z (raw 24-bit)
        col.addLayout(self._build_xyz_row())

        # Row 4: COM port selector
        col.addWidget(self._build_port_selector())

        # Row 5: Connect / disconnect
        self.btn_connect = QPushButton("▶  Connect")
        self.btn_connect.setStyleSheet(BTN_STYLE_CONNECT)
        self.btn_connect.clicked.connect(self._toggle_connection)
        col.addWidget(self.btn_connect)

        # Row 6: log | dark mode
        row6 = QHBoxLayout()
        row6.setSpacing(10)
        self.btn_log = QPushButton("●  Start Logging")
        self.btn_log.setStyleSheet(BTN_STYLE_LOG_START)
        self.btn_log.clicked.connect(self._toggle_logging)
        row6.addWidget(self.btn_log)

        self.btn_dark = QPushButton("🌙  Enable Dark Mode")
        self.btn_dark.setStyleSheet(BTN_STYLE_DARK_OFF)
        self.btn_dark.clicked.connect(self._toggle_dark_mode)
        row6.addWidget(self.btn_dark)
        col.addLayout(row6)

        self.lbl_log_status = QLabel("Not logging")
        self.lbl_log_status.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:10px; border:none;"
        )
        self.lbl_log_status.setAlignment(Qt.AlignCenter)
        col.addWidget(self.lbl_log_status)

        col.addStretch(1)
        return col

    def _build_xyz_row(self):
        """Build the row of 3 XYZ tristimulus panels (raw 24-bit counts)."""
        row = QHBoxLayout()
        row.setSpacing(10)

        for attr, label, color in [
            ("lbl_X", "X  (Raw 24-bit)", ACCENT),
            ("lbl_Y", "Y  (Raw 24-bit)", GREEN),
            ("lbl_Z", "Z  (Raw 24-bit)", AMBER),
        ]:
            p, lbl = make_panel(label)
            lbl.setStyleSheet(
                f"color:{color}; font-size:18px; font-weight:700; border:none;"
            )
            setattr(self, attr, lbl)
            row.addWidget(p)

        return row

    def _build_port_selector(self):
        panel = QFrame()
        panel.setStyleSheet(f"""
            QFrame {{
                background:{PANEL_BG};
                border: 1px solid {BORDER};
                border-radius: 10px;
            }}
        """)
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(12, 8, 12, 8)
        layout.setSpacing(6)

        title = QLabel("COM PORT")
        title.setStyleSheet(
            f"color:{TEXT_DIM}; font-size:10px; font-weight:700; "
            f"letter-spacing:1.2px; border:none;"
        )
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)

        self.port_combo = QComboBox()
        self.port_combo.setStyleSheet(f"""
            QComboBox {{
                background:{INPUT_BG}; color:{TEXT_MAIN};
                border:1px solid {BORDER};
                border-radius:5px; padding:4px 8px;
                font-size:12px;
            }}
            QComboBox::drop-down {{ border:none; }}
            QComboBox QAbstractItemView {{
                background:{INPUT_BG}; color:{TEXT_MAIN};
                selection-background-color:{PANEL_BG};
            }}
        """)
        self._refresh_ports()
        layout.addWidget(self.port_combo)

        btn_refresh = QPushButton("⟳  Refresh Ports")
        btn_refresh.setStyleSheet(f"""
            QPushButton {{
                background:{PANEL_BG}; color:{TEXT_DIM};
                border:1px solid {BORDER};
                border-radius:5px; padding:4px 8px;
                font-size:11px;
            }}
            QPushButton:hover {{ color:{ACCENT}; border-color:{ACCENT}; }}
        """)
        btn_refresh.clicked.connect(self._refresh_ports)
        layout.addWidget(btn_refresh)

        return panel

    # ── COM port detection ────────────────────────────────────
    def _refresh_ports(self):
        current = self.port_combo.currentText().split(" ")[0]
        self.port_combo.blockSignals(True)
        self.port_combo.clear()

        ports = serial.tools.list_ports.comports()
        if ports:
            for p in sorted(ports):
                self.port_combo.addItem(f"{p.device}  —  {p.description}")
        else:
            self.port_combo.addItem("No ports found")

        for i in range(self.port_combo.count()):
            if self.port_combo.itemText(i).startswith(current):
                self.port_combo.setCurrentIndex(i)
                break

        self.port_combo.blockSignals(False)

    def _selected_port(self):
        text = self.port_combo.currentText()
        if text and "No ports" not in text:
            return text.split("  —  ")[0].strip()
        return None

    # ── Worker management ─────────────────────────────────────
    def _start_worker(self, port: str):
        self.worker = BluetoothWorker(port=port, baud=BAUD)
        self.worker.data_received.connect(self._on_data)
        self.worker.connection_lost.connect(self._on_lost)
        self.worker.start()

    def _set_button_state(self, connected: bool):
        if connected:
            self.btn_connect.setText("⏹  Disconnect")
            self.btn_connect.setStyleSheet(BTN_STYLE_DISCONNECT)
            self.port_combo.setEnabled(False)
        else:
            self.btn_connect.setText("▶  Connect")
            self.btn_connect.setStyleSheet(BTN_STYLE_CONNECT)
            self.port_combo.setEnabled(True)

    def _toggle_connection(self):
        if self.worker and self.worker.isRunning():
            self.worker.stop()
            self.worker = None
            self._set_button_state(connected=False)
            self.dark_mode = False
            self.btn_dark.setText("🌙  Enable Dark Mode")
            self.btn_dark.setStyleSheet(BTN_STYLE_DARK_OFF)
            self.status.showMessage("Disconnected.")
            if self.log_file:
                self._stop_logging()
            return

        port = self._selected_port()
        if not port:
            self.status.showMessage("⚠  No COM port selected.")
            return
        self._start_worker(port)
        self._set_button_state(connected=True)
        self.status.showMessage(f"Connecting to {port}…")

    # ── Dark mode ──────────────────────────────────────────────
    def _toggle_dark_mode(self):
        if not self.worker or not self.worker.isRunning():
            self.status.showMessage("⚠  Connect first before toggling dark mode.")
            return

        self.dark_mode = not self.dark_mode
        if self.dark_mode:
            self.worker.send("DARK")
            self.btn_dark.setText("☀  Enable Light Mode")
            self.btn_dark.setStyleSheet(BTN_STYLE_DARK_ON)
            self.status.showMessage("Dark mode ON — backlight off, sensor active.")
        else:
            self.worker.send("LIGHT")
            self.btn_dark.setText("🌙  Enable Dark Mode")
            self.btn_dark.setStyleSheet(BTN_STYLE_DARK_OFF)
            self.status.showMessage("Dark mode OFF — display restored.")

    # ── CSV logging ────────────────────────────────────────────
    def _toggle_logging(self):
        if self.log_file:
            self._stop_logging()
        else:
            self._start_logging()

    def _start_logging(self):
        log_dir = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "logs"
        )
        os.makedirs(log_dir, exist_ok=True)

        stamp = datetime.now().strftime("%Y-%m-%d_%H%M%S")
        self.log_path = os.path.join(log_dir, f"chromasense_log_{stamp}.csv")

        self.log_file = open(self.log_path, "w", newline="", encoding="utf-8")
        self.log_writer = csv.DictWriter(self.log_file, fieldnames=LOG_FIELDS)
        self.log_writer.writeheader()
        self.log_count = 0

        self.btn_log.setText("■  Stop Logging")
        self.btn_log.setStyleSheet(BTN_STYLE_LOG_STOP)
        self.lbl_log_status.setText(f"0 rows → {os.path.basename(self.log_path)}")

    def _stop_logging(self):
        if self.log_file:
            self.log_file.close()
        path = self.log_path
        count = self.log_count

        self.log_file = None
        self.log_writer = None
        self.log_path = None

        self.btn_log.setText("●  Start Logging")
        self.btn_log.setStyleSheet(BTN_STYLE_LOG_START)
        self.lbl_log_status.setText(
            f"Saved {count} rows" if path else "Not logging"
        )

    def _log_packet(self, packet: dict):
        if not self.log_writer:
            return
        row = {field: packet.get(field, "") for field in LOG_FIELDS}
        row["timestamp"] = datetime.now().isoformat(timespec="milliseconds")
        self.log_writer.writerow(row)
        self.log_count += 1
        if self.log_count % 10 == 0:
            self.log_file.flush()
            self.lbl_log_status.setText(
                f"{self.log_count} rows → {os.path.basename(self.log_path)}"
            )

    # ── Data slot ─────────────────────────────────────────────
    def _on_data(self, packet: dict):
        # Guard: skip malformed packets missing any core measurement key.
        # batt / gain / sat are optional and handled with .get().
        required = {'lux', 'cct', 'x', 'y', 'X', 'Y', 'Z'}
        if not required.issubset(packet):
            return

        self.lbl_lux.setText(f"{packet['lux']:.0f} lx")

        # cct = 0 means off-locus (no valid correlated colour temperature)
        cct = packet['cct']
        if cct > 0:
            self.lbl_cct.setText(f"{cct:.0f} K")
            color = cct_to_hex(cct)
        else:
            self.lbl_cct.setText("—")
            color = TEXT_DIM
        if color != self._last_cct_color:
            self.lbl_cct.setStyleSheet(
                f"color:{color}; font-size:26px; font-weight:700; border:none;"
            )
            self._last_cct_color = color

        batt = packet.get('batt')
        if batt is not None:
            self.lbl_batt.setText(f"{batt:.0f}%")
            color = batt_to_hex(batt)
        else:
            self.lbl_batt.setText("—")
            color = TEXT_DIM
        if color != self._last_batt_color:
            self.lbl_batt.setStyleSheet(
                f"color:{color}; font-size:26px; font-weight:700; border:none;"
            )
            self._last_batt_color = color

        # Raw 24-bit tristimulus counts
        self.lbl_X.setText(f"{packet['X']}")
        self.lbl_Y.setText(f"{packet['Y']}")
        self.lbl_Z.setText(f"{packet['Z']}")

        self.lbl_xy.setText(
            f"x = {packet['x']:.4f}\ny = {packet['y']:.4f}"
        )
        self.cie.update_point(packet['x'], packet['y'], packet['cct'])

        gain_str = packet.get('gain', '—')
        sat_str  = "  ⚠ SAT" if packet.get('sat', 0) else ""
        port = self._selected_port() or "—"
        rec_str = "  ● REC" if self.log_writer else ""
        self.status.showMessage(
            f"{port}  |  REAL TCS  |  G:{gain_str}{sat_str}{rec_str}"
        )

        # Sync button state with firmware (may have been toggled via BtnB)
        fw_dark = bool(packet.get('dark', 0))
        if fw_dark != self.dark_mode:
            self.dark_mode = fw_dark
            if self.dark_mode:
                self.btn_dark.setText("☀  Enable Light Mode")
                self.btn_dark.setStyleSheet(BTN_STYLE_DARK_ON)
            else:
                self.btn_dark.setText("🌙  Enable Dark Mode")
                self.btn_dark.setStyleSheet(BTN_STYLE_DARK_OFF)

        self._log_packet(packet)

    def _on_lost(self, msg: str):
        self.status.showMessage(f"⚠  Connection lost: {msg}")
        self._set_button_state(connected=False)

    # ── Clean shutdown ────────────────────────────────────────
    def closeEvent(self, event):
        self._port_timer.stop()
        if self.worker and self.worker.isRunning():
            self.worker.stop()
        if self.log_file:
            self._stop_logging()
        event.accept()


# ── Entry point ───────────────────────────────────────────────
def main():
    app = QApplication(sys.argv)
    app.setFont(QFont("Segoe UI", 10))
    win = MainWindow()
    win.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()