#!/usr/bin/env python3
"""Settings dialog for Shoal (Linux). Edits ~/.config/shoal/settings.conf."""
import os, subprocess, sys
try:
    from PyQt5 import QtWidgets as W, QtGui as G, QtCore as C
except ImportError:
    from PySide6 import QtWidgets as W, QtGui as G, QtCore as C

PATH = os.path.join(os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config"), "shoal", "settings.conf")
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULTS = dict(Speed=42, SpeedVariability=5, FishCoverage=50, Randomness=0, Size=2.4, SizeVariation=100, SizeBell=1,
                TailPercent=33, TailShade=-40, DepthEffect=1, RockCount=5, RestrictToMonitor=0, ShowFps=0, VSync=1,
                FrameRateMode=1, ColourSort=0, ColourAvoid=0)
DEFAULT_COLOURS = ["#dce6f0", "#ffcd28", "#ff781e", "#286ee6", "#28c8be"]

def load():
    v, colours = dict(DEFAULTS), {}
    try:
        for line in open(PATH):
            line = line.split("   #")[0].strip()
            if line.startswith("#") or "=" not in line:
                continue
            k, val = [s.strip() for s in line.split("=", 1)]
            if k.lower().startswith("colour") and k[6:].isdigit():
                colours[int(k[6:])] = val if val.startswith("#") else "#" + val
            elif k in v:
                try: v[k] = float(val) if k == "Size" else int(float(val))
                except ValueError: pass
    except OSError:
        pass
    cols = [colours[i] for i in sorted(colours)] or list(DEFAULT_COLOURS)
    return v, cols[:10]

def save(v, cols):
    os.makedirs(os.path.dirname(PATH), exist_ok=True)
    with open(PATH, "w") as f:
        f.write("# Shoal settings. Written by shoal-settings.py; same names as the Windows registry values.\n")
        for k in DEFAULTS:
            f.write(f"{k}={v[k]}\n")
        f.write(f"ColourCount={len(cols)}\n")
        for i, c in enumerate(cols):
            f.write(f"Colour{i}={c}\n")

class Dialog(W.QDialog):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Shoal Settings")
        self.v, self.cols = load()
        self.w = {}
        form = W.QFormLayout()
        def slider(key, label, lo, hi, tip=""):
            row = W.QHBoxLayout()
            s = W.QSlider(C.Qt.Horizontal); s.setRange(lo, hi); s.setValue(int(self.v[key])); s.setMinimumWidth(260)
            n = W.QLabel(str(s.value())); n.setMinimumWidth(40)
            s.valueChanged.connect(lambda x, n=n: n.setText(str(x)))
            row.addWidget(s); row.addWidget(n)
            if tip: s.setToolTip(tip)
            form.addRow(label, row); self.w[key] = s
        def check(key, label):
            c = W.QCheckBox(label); c.setChecked(bool(self.v[key])); form.addRow("", c); self.w[key] = c
        slider("Speed", "Speed (px/s)", 1, 1000)
        slider("SpeedVariability", "Speed variability", 0, 10)
        slider("FishCoverage", "Fish amount", 0, 100, "Share of screen covered by fish (log scale)")
        slider("Randomness", "Randomness", 0, 100, "0 = tight shoals, 100 = few groups")
        sz = W.QDoubleSpinBox(); sz.setRange(1, 10); sz.setDecimals(1); sz.setSingleStep(0.1); sz.setSuffix(" % of screen height")
        sz.setValue(float(self.v["Size"])); form.addRow("Fish size", sz); self.w["Size"] = sz
        slider("SizeVariation", "Size variability (%)", 100, 300)
        check("SizeBell", "Bell-curve size spread")
        slider("TailPercent", "Tail size (%)", 10, 100)
        slider("TailShade", "Tail shade", -100, 100, "Negative darkens toward the tail, positive lightens")
        slider("RockCount", "Rocks", 0, 10)
        check("DepthEffect", "3D depth and shadows")
        check("RestrictToMonitor", "Keep fish on their own monitor")
        check("ColourSort", "Sort fish by colour")
        check("ColourAvoid", "Different colours avoid each other")
        check("ShowFps", "Show FPS counter")
        check("VSync", "VSync")
        fm = W.QComboBox(); fm.addItems(["1/2x", "1x", "2x", "3x", "4x", "Unlimited"]); fm.setCurrentIndex(int(self.v["FrameRateMode"]))
        fm.setToolTip("Frame rate when VSync is off (multiple of the monitor refresh rate)")
        form.addRow("Frame rate (no VSync)", fm); self.w["FrameRateMode"] = fm

        self.swatches = W.QHBoxLayout()
        sw = W.QWidget(); sw.setLayout(self.swatches)
        add = W.QPushButton("Add..."); add.clicked.connect(self.add_colour)
        crow = W.QVBoxLayout(); crow.addWidget(sw); crow.addWidget(add, 0, C.Qt.AlignLeft)
        form.addRow("Colours\n(click to change,\nright-click to remove)", crow)
        self.refresh_swatches()

        buttons = W.QDialogButtonBox(W.QDialogButtonBox.Ok | W.QDialogButtonBox.Cancel | W.QDialogButtonBox.RestoreDefaults)
        prev = buttons.addButton("Preview", W.QDialogButtonBox.ActionRole)
        buttons.accepted.connect(self.accept_save); buttons.rejected.connect(self.reject)
        buttons.button(W.QDialogButtonBox.RestoreDefaults).clicked.connect(self.restore)
        prev.clicked.connect(self.preview)
        lay = W.QVBoxLayout(self); lay.addLayout(form); lay.addWidget(buttons)

    def refresh_swatches(self):
        while self.swatches.count():
            self.swatches.takeAt(0).widget().deleteLater()
        for i, c in enumerate(self.cols):
            b = W.QPushButton(); b.setFixedSize(28, 28); b.setStyleSheet(f"background:{c};border:1px solid #555")
            b.setContextMenuPolicy(C.Qt.CustomContextMenu)
            b.clicked.connect(lambda _, i=i: self.pick(i))
            b.customContextMenuRequested.connect(lambda _, i=i: self.remove(i))
            self.swatches.addWidget(b)
        self.swatches.addStretch()

    def pick(self, i):
        c = W.QColorDialog.getColor(G.QColor(self.cols[i]), self)
        if c.isValid(): self.cols[i] = c.name(); self.refresh_swatches()

    def remove(self, i):
        if len(self.cols) > 1: del self.cols[i]; self.refresh_swatches()

    def add_colour(self):
        if len(self.cols) >= 10: return
        c = W.QColorDialog.getColor(G.QColor("#ffffff"), self)
        if c.isValid(): self.cols.append(c.name()); self.refresh_swatches()

    def restore(self):
        for k, wd in self.w.items(): self.set(wd, DEFAULTS[k])
        self.cols = list(DEFAULT_COLOURS); self.refresh_swatches()

    @staticmethod
    def set(wd, val):
        if isinstance(wd, W.QCheckBox): wd.setChecked(bool(val))
        elif isinstance(wd, W.QComboBox): wd.setCurrentIndex(int(val))
        else: wd.setValue(val)

    def collect(self):
        out = {}
        for k, wd in self.w.items():
            if isinstance(wd, W.QCheckBox): out[k] = int(wd.isChecked())
            elif isinstance(wd, W.QComboBox): out[k] = wd.currentIndex()
            elif isinstance(wd, W.QDoubleSpinBox): out[k] = round(wd.value(), 1)
            else: out[k] = wd.value()
        return out

    def accept_save(self):
        save(self.collect(), self.cols); self.accept()

    def preview(self):
        save(self.collect(), self.cols)
        exe = os.path.join(HERE, "shoal")
        if not os.path.exists(exe): exe = "shoal"
        subprocess.Popen([exe])

app = W.QApplication(sys.argv)
d = Dialog()
sys.exit(0)  if (d.exec_() if hasattr(d, "exec_") else d.exec()) else sys.exit(1)
