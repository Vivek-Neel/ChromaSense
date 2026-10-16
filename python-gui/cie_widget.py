"""
ChromaSense — CIE 1931 Chromaticity Widget
==========================================
PyQt5 widget rendering the CIE 1931 chromaticity diagram:
  • Spectral locus (horseshoe boundary), computed and drawn
  • Background image fill for the visible colour gamut
  • Planckian locus (blackbody curve), currently hidden
  • Live measurement point, updated at 10 Hz via draw_idle()

Fixed 1:1 aspect ratio on resize; extra window space becomes margin.
"""

import os

import numpy as np
from scipy.interpolate import CubicSpline
import matplotlib
matplotlib.use("Qt5Agg")
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg as FigureCanvas

from PyQt5.QtWidgets import QWidget, QVBoxLayout, QHBoxLayout, QSizePolicy

# ── Theme constants (match main_window.py) ────────────────────
BG          = "#1a1a2e"
PANEL_BG    = "#16213e"
ACCENT      = "#00f5d4"
AXIS_COLOR  = "#8892a4"
SPINE_COLOR = "#2a2a4a"
GRID_COLOR  = "#333355"
LOCUS_BLACK = "black"

# ── CIE diagram coordinate bounds ─────────────────────────────
# Background image is cropped to exactly these bounds.
CIE_X_MIN, CIE_X_MAX = -0.05, 0.80
CIE_Y_MIN, CIE_Y_MAX = -0.06, 0.91

# ── Physical constants for Planck radiation law ───────────────
PLANCK_H = 6.626e-34   # J·s
LIGHT_C  = 3.0e8       # m/s
BOLTZ_K  = 1.381e-23   # J/K

# ── CIE 1931 2° Standard Observer CMF table (5 nm steps) ──────
# Columns: wavelength (nm), x_bar, y_bar, z_bar
# Source: CIE 1931 standard, 380–780 nm
_CMF = np.array([
    [380, 0.001368, 0.000039, 0.006450],
    [385, 0.002236, 0.000064, 0.010550],
    [390, 0.004243, 0.000120, 0.020050],
    [395, 0.007650, 0.000217, 0.036210],
    [400, 0.014310, 0.000396, 0.067850],
    [405, 0.023190, 0.000640, 0.110200],
    [410, 0.043510, 0.001210, 0.207400],
    [415, 0.077630, 0.002180, 0.371300],
    [420, 0.134380, 0.004000, 0.645600],
    [425, 0.214770, 0.007300, 1.039050],
    [430, 0.283900, 0.011600, 1.385600],
    [435, 0.328500, 0.016840, 1.622960],
    [440, 0.348280, 0.023000, 1.747060],
    [445, 0.348060, 0.029800, 1.782600],
    [450, 0.336200, 0.038000, 1.772110],
    [455, 0.318700, 0.048000, 1.744100],
    [460, 0.290800, 0.060000, 1.669200],
    [465, 0.251100, 0.073900, 1.528100],
    [470, 0.195360, 0.090980, 1.287640],
    [475, 0.142100, 0.112600, 1.041900],
    [480, 0.095640, 0.139020, 0.812950],
    [485, 0.057950, 0.169300, 0.616200],
    [490, 0.032010, 0.208020, 0.465180],
    [495, 0.014700, 0.258600, 0.353300],
    [500, 0.004900, 0.323000, 0.272000],
    [505, 0.002400, 0.407300, 0.212300],
    [510, 0.009300, 0.503000, 0.158200],
    [515, 0.029100, 0.608200, 0.111700],
    [520, 0.063270, 0.710000, 0.078250],
    [525, 0.109600, 0.793200, 0.057250],
    [530, 0.165500, 0.862000, 0.042160],
    [535, 0.225750, 0.914850, 0.029840],
    [540, 0.290400, 0.954000, 0.020300],
    [545, 0.359700, 0.980300, 0.013400],
    [550, 0.433450, 0.994950, 0.008750],
    [555, 0.512050, 1.000000, 0.005750],
    [560, 0.594500, 0.995000, 0.003900],
    [565, 0.678400, 0.978600, 0.002750],
    [570, 0.762100, 0.952000, 0.002100],
    [575, 0.842500, 0.915400, 0.001800],
    [580, 0.916300, 0.870000, 0.001650],
    [585, 0.978600, 0.816300, 0.001400],
    [590, 1.026300, 0.757000, 0.001100],
    [595, 1.056700, 0.694900, 0.001000],
    [600, 1.062200, 0.631000, 0.000800],
    [605, 1.045600, 0.566800, 0.000600],
    [610, 1.002600, 0.503000, 0.000340],
    [615, 0.938400, 0.441200, 0.000240],
    [620, 0.854450, 0.381000, 0.000190],
    [625, 0.751400, 0.321000, 0.000100],
    [630, 0.642400, 0.265000, 0.000050],
    [635, 0.541900, 0.217000, 0.000030],
    [640, 0.447900, 0.175000, 0.000020],
    [645, 0.360800, 0.138200, 0.000010],
    [650, 0.283500, 0.107000, 0.000000],
    [655, 0.218700, 0.081600, 0.000000],
    [660, 0.164900, 0.061000, 0.000000],
    [665, 0.121200, 0.044580, 0.000000],
    [670, 0.087400, 0.032000, 0.000000],
    [675, 0.063600, 0.023200, 0.000000],
    [680, 0.046770, 0.017000, 0.000000],
    [685, 0.032900, 0.011920, 0.000000],
    [690, 0.022700, 0.008210, 0.000000],
    [695, 0.015840, 0.005723, 0.000000],
    [700, 0.011359, 0.004102, 0.000000],
    [705, 0.008111, 0.002929, 0.000000],
    [710, 0.005790, 0.002091, 0.000000],
    [715, 0.004109, 0.001484, 0.000000],
    [720, 0.002899, 0.001047, 0.000000],
    [725, 0.002049, 0.000740, 0.000000],
    [730, 0.001440, 0.000520, 0.000000],
    [735, 0.001000, 0.000361, 0.000000],
    [740, 0.000690, 0.000249, 0.000000],
    [745, 0.000476, 0.000172, 0.000000],
    [750, 0.000332, 0.000120, 0.000000],
    [755, 0.000235, 0.000085, 0.000000],
    [760, 0.000166, 0.000060, 0.000000],
    [765, 0.000117, 0.000042, 0.000000],
    [770, 0.000083, 0.000030, 0.000000],
    [775, 0.000059, 0.000021, 0.000000],
    [780, 0.000042, 0.000015, 0.000000],
])


# ── Pure helper functions (no Qt dependencies) ───────────────
def _trapz(y, x):
    """Numerical integration — uses np.trapezoid (NumPy 2.0+) or np.trapz."""
    try:
        return np.trapezoid(y, x)
    except AttributeError:
        return np.trapz(y, x)


def _cmf_to_xy():
    """Convert CMF table to (x, y) chromaticity coords of the spectral
    locus. Cubic-spline interpolated to 0.1nm steps."""
    wl = _CMF[:, 0]
    wl_fine = np.arange(wl[0], wl[-1] + 0.1, 0.1)
    X = CubicSpline(wl, _CMF[:, 1])(wl_fine)
    Y = CubicSpline(wl, _CMF[:, 2])(wl_fine)
    Z = CubicSpline(wl, _CMF[:, 3])(wl_fine)
    s = X + Y + Z
    return X / s, Y / s


def _planckian_locus(t_min=1000, t_max=12000, steps=200):
    """
    Compute (x, y) chromaticity coordinates of the Planckian (blackbody) locus.

    Integrates Planck's radiation law against the CIE CMF functions
    for `steps` temperature values evenly spaced from t_min to t_max.

    Returns:
        xs, ys : np.ndarray — chromaticity coordinates
        temps  : np.ndarray — corresponding temperatures in Kelvin
    """
    temps = np.linspace(t_min, t_max, steps)
    wavelengths = _CMF[:, 0] * 1e-9   # nm → m
    xs, ys = [], []
    for T in temps:
        # Planck's radiation law (spectral radiance per wavelength)
        spec = 1.0 / (
            wavelengths ** 5
            * (np.exp(PLANCK_H * LIGHT_C / (wavelengths * BOLTZ_K * T)) - 1)
        )
        X = _trapz(spec * _CMF[:, 1], wavelengths)
        Y = _trapz(spec * _CMF[:, 2], wavelengths)
        Z = _trapz(spec * _CMF[:, 3], wavelengths)
        s = X + Y + Z
        xs.append(X / s)
        ys.append(Y / s)
    return np.array(xs), np.array(ys), temps


# ── Main widget ──────────────────────────────────────────────
class CIEWidget(QWidget):
    """PyQt5 widget rendering the CIE 1931 chromaticity diagram. Fixed
    1:1 aspect ratio; static elements drawn once, live point per update."""

    # Aspect ratio of the canvas (width / height) — square keeps diagram correct
    ASPECT_RATIO = 0.889

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self._cie_image = None
        self._load_cie_image()
        self._build_figure()

    # ── Resize: enforce aspect ratio ──────────────────────
    def resizeEvent(self, event):
        super().resizeEvent(event)
        w, h = self.width(), self.height()
        if w <= 0 or h <= 0:
            return
        # Find the largest size that fits while preserving aspect ratio
        if w / h > self.ASPECT_RATIO:
            canvas_w = int(h * self.ASPECT_RATIO)
            canvas_h = h
        else:
            canvas_w = w
            canvas_h = int(w / self.ASPECT_RATIO)
        self.canvas.setFixedSize(canvas_w, canvas_h)

    # ── Background image loading ──────────────────────────
    def _load_cie_image(self):
        """Load the cropped CIE reference image from disk."""
        here = os.path.dirname(os.path.abspath(__file__))
        candidates = [
            os.path.join(here, "cie_background.png"),
            os.path.join(here, "cie_background.jpg"),
            "cie_background.png",
            "cie_background.jpg",
        ]
        for path in candidates:
            if not os.path.exists(path):
                continue
            try:
                img = plt.imread(path)
                # Drop alpha channel if present
                if img.ndim == 3 and img.shape[2] == 4:
                    img = img[:, :, :3]
                # Normalise uint8 to 0–1 float
                if img.dtype == np.uint8:
                    img = img.astype(np.float32) / 255.0
                # Shift 4px right / 1px down, white fill on vacated edges
                dx, dy = 4, 1
                h, w = img.shape[:2]
                shifted = np.ones_like(img)
                shifted[dy:h, dx:w, ...] = img[0:h - dy, 0:w - dx, ...]
                img = shifted
                self._cie_image = img
                print(f"CIE background loaded: {path}")
                return
            except Exception as e:
                print(f"Failed to load {path}: {e}")
        print("CIE background image not found — falling back to computed fill.")

    # ── Figure construction ───────────────────────────────
    def _build_figure(self):
        # Outer layout centres the canvas with stretch above/below/left/right
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)

        h_row = QHBoxLayout()
        h_row.setContentsMargins(0, 0, 0, 0)
        h_row.setSpacing(0)

        self.fig = Figure(facecolor=BG)
        self.fig.set_tight_layout(True)
        self.canvas = FigureCanvas(self.fig)
        # Size is constrained dynamically in resizeEvent
        self.canvas.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)

        h_row.addStretch(1)
        h_row.addWidget(self.canvas)
        h_row.addStretch(1)

        outer.addStretch(1)
        outer.addLayout(h_row)
        outer.addStretch(1)

        # Build the diagram
        self.ax = self.fig.add_subplot(111)
        self.ax.set_aspect('equal', adjustable='box')
        self._draw_static()
        self._init_live_artists()
        self.canvas.draw()

    # ── Static elements (drawn once) ──────────────────────
    def _draw_static(self):
        ax = self.ax
        ax.set_facecolor("#ffffff")
        ax.tick_params(colors=AXIS_COLOR, labelsize=8)
        ax.xaxis.label.set_color(AXIS_COLOR)
        ax.yaxis.label.set_color(AXIS_COLOR)
        for spine in ax.spines.values():
            spine.set_edgecolor(SPINE_COLOR)

        # Background fill (image preferred, computed fallback)
        if self._cie_image is not None:
            ax.imshow(
                np.flipud(self._cie_image),
                extent=[-0.0444, 0.8009, -0.0491, 0.9009],
                origin='lower',
                aspect='auto',
                zorder=1,
                interpolation='bilinear',
            )
        else:
            self._draw_computed_fill(ax)

        # Spectral locus boundary (horseshoe outline)
        sx, sy = _cmf_to_xy()
        sx_closed = np.append(sx, sx[0])
        sy_closed = np.append(sy, sy[0])
        ax.plot(sx_closed, sy_closed, 'k-', linewidth=3.2, zorder=4,
                antialiased=True, solid_capstyle='round', solid_joinstyle='round')

        # Axes formatting
        ax.set_xlim(CIE_X_MIN, CIE_X_MAX)
        ax.set_ylim(CIE_Y_MIN, CIE_Y_MAX)
        ax.set_xlabel("x", fontsize=9, color=AXIS_COLOR)
        ax.set_ylabel("y", fontsize=9, color=AXIS_COLOR)
        ax.set_title("CIE 1931 Chromaticity Diagram",
                     color=ACCENT, fontsize=10, pad=8)
        ax.grid(False)

    def _init_live_artists(self):
        """Create the live measurement point artist."""
        self.live_point, = self.ax.plot(
            [], [], 'o',
            color='red', markersize=10,
            markeredgecolor=LOCUS_BLACK, markeredgewidth=2,
            zorder=10, label='Current Source',
        )
        # Live measurement point

    # ── Fallback computed fill ────────────────────────────
    def _draw_computed_fill(self, ax):
        """Computed scatter fill — used only if cie_background.png is missing."""
        xi = np.linspace(CIE_X_MIN, CIE_X_MAX, 300)
        yi = np.linspace(CIE_Y_MIN, CIE_Y_MAX, 300)
        xg, yg = np.meshgrid(xi, yi)
        xf, yf = xg.flatten(), yg.flatten()
        zf = 1 - xf - yf
        mask = (zf > 0) & (zf < 1) & (xf > 0) & (yf > 0)
        xf, yf, zf = xf[mask], yf[mask], zf[mask]

        # XYZ → linear sRGB matrix transform (IEC 61966-2-1)
        r =  3.2406 * xf - 1.5372 * yf - 0.4986 * zf
        g = -0.9689 * xf + 1.8758 * yf + 0.0415 * zf
        b =  0.0557 * xf - 0.2040 * yf + 1.0570 * zf
        rgb = np.clip(np.stack([r, g, b], axis=1), 0, 1)
        # sRGB gamma correction
        rgb = np.where(
            rgb <= 0.0031308,
            12.92 * rgb,
            1.055 * rgb ** (1 / 2.4) - 0.055,
        )
        rgb = np.clip(rgb, 0, 1)
        ax.scatter(xf, yf, c=rgb, s=2, zorder=1, linewidths=0)

    # ── Public API ────────────────────────────────────────
    def update_point(self, x: float, y: float, cct: float):
        """Update live measurement marker — called at 10 Hz from main window."""
        self.live_point.set_data([x], [y])
        self.canvas.draw_idle()