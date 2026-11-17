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
                FrameRateMode=1, ColourSort=0, ColourAvoid=0, BackgroundMode=0, BackgroundDim=0)
# Text settings (same names as the Plasma wallpaper's).
STR_DEFAULTS = dict(BackgroundColor="#000000", BackgroundImage="", Gpu="", Species="1,2,3,4,5,6,7,8,9,10")
DEFAULTS["SpeciesMode"] = 0
SPECIES = ["Classic", "Sardine", "Goldfish", "Koi", "Tuna", "Trout", "Pike", "Tetra", "Fantail", "Mackerel"]
DEFAULT_COLOURS = ["#dce6f0", "#ffcd28", "#ff781e", "#286ee6", "#28c8be"]

def gpus():
    """Display GPUs from sysfs as (PCI address, name, primary), named from pci.ids like the C++ code."""
    names = {}
    for path in ("/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"):
        try:
            vendor = None
            for line in open(path, errors="replace"):
                if not line.strip() or line.startswith("#"): continue
                if not line.startswith("\t"):
                    vendor = line[:4]; names[vendor] = line[6:].strip(); continue
                if not line.startswith("\t\t") and vendor:
                    names[vendor + line[1:5]] = line[7:].strip()
            break
        except OSError:
            pass
    short = {"10de": "NVIDIA", "1002": "AMD", "8086": "Intel"}
    out = []
    base = "/sys/class/drm"
    for card in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        if not card.startswith("card") or "-" in card: continue
        dev = os.path.join(base, card, "device")
        try:
            if not open(os.path.join(dev, "class")).read().startswith("0x03"): continue
            pci = os.path.basename(os.path.realpath(dev))
            ven = open(os.path.join(dev, "vendor")).read().strip()[2:]
            did = open(os.path.join(dev, "device")).read().strip()[2:]
            primary = open(os.path.join(dev, "boot_vga")).read().strip() == "1" if os.path.exists(os.path.join(dev, "boot_vga")) else False
        except OSError:
            continue
        model = names.get(ven + did, f"GPU {ven}:{did}")
        if "[" in model and "]" in model: model = model[model.index("[") + 1:model.rindex("]")]
        if pci not in [g[0] for g in out]:
            out.append((pci, f"{short.get(ven, names.get(ven, ''))} {model}".strip(), primary))
    return sorted(out, key=lambda g: (g[2], g[0]))

def load():
    v, colours = dict(DEFAULTS), {}
    v.update(STR_DEFAULTS)
    try:
        for line in open(PATH):
            line = line.split("   #")[0].strip()
            if line.startswith("#") or "=" not in line:
                continue
            k, val = [s.strip() for s in line.split("=", 1)]
            if k.lower().startswith("colour") and k[6:].isdigit():
                colours[int(k[6:])] = val if val.startswith("#") else "#" + val
            elif k in STR_DEFAULTS:
                v[k] = val
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
        for k in list(DEFAULTS) + list(STR_DEFAULTS):
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

        # Background: plain water or a picture (default: Plasma's wallpaper), as in the Plasma wallpaper.
        bm = W.QComboBox(); bm.addItems(["Water colour", "Picture"]); bm.setCurrentIndex(int(self.v["BackgroundMode"]))
        form.addRow("Background", bm); self.w["BackgroundMode"] = bm
        self.water = W.QPushButton(); self.water.setFixedSize(60, 24); self.water.clicked.connect(self.pick_water)
        self.set_water(self.v["BackgroundColor"]); form.addRow("Water colour", self.water)
        prow = W.QHBoxLayout()
        self.picture = W.QLineEdit(self.v["BackgroundImage"]); self.picture.setPlaceholderText("Plasma's default wallpaper")
        browse = W.QPushButton("Browse..."); browse.clicked.connect(self.browse)
        prow.addWidget(self.picture); prow.addWidget(browse)
        form.addRow("Picture", prow)
        slider("BackgroundDim", "Darken picture (%)", 0, 90)
        self.gpu = W.QComboBox(); self.gpu.addItem("Automatic (the dedicated GPU)", "")
        for pci, name, primary in gpus():
            self.gpu.addItem(f"{name} ({pci}){' - primary' if primary else ''}", pci)
        i = self.gpu.findData(self.v["Gpu"]); self.gpu.setCurrentIndex(max(i, 0))
        self.gpu.setToolTip("GPU that renders the fish. The primary GPU drives the screen; another one needs PRIME offload.")
        form.addRow("GPU", self.gpu)

        # Species (shapes drawn in docs/fish_gallery.png).
        chosen = {int(n) for n in self.v["Species"].replace(" ", "").split(",") if n.isdigit()} or set(range(1, 11))
        grid = W.QGridLayout(); self.species = []
        for i, name in enumerate(SPECIES):
            c = W.QCheckBox(f"{i + 1} {name}"); c.setChecked(i + 1 in chosen)
            grid.addWidget(c, i // 5, i % 5); self.species.append(c)
        sp = W.QWidget(); sp.setLayout(grid)
        form.addRow("Species", sp)
        smode = W.QComboBox(); smode.addItems(["Random mix", "One species per colour"]); smode.setCurrentIndex(int(self.v["SpeciesMode"]))
        smode.setToolTip("Mix the chosen species randomly, or give every fish of a colour the same species")
        form.addRow("Species mix", smode); self.w["SpeciesMode"] = smode

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

    def set_water(self, c):
        self.water_colour = c; self.water.setStyleSheet(f"background:{c};border:1px solid #555")

    def pick_water(self):
        c = W.QColorDialog.getColor(G.QColor(self.water_colour), self)
        if c.isValid(): self.set_water(c.name())

    def browse(self):
        f = W.QFileDialog.getOpenFileName(self, "Background picture", os.path.dirname(self.picture.text()) or os.path.expanduser("~"),
                                          "Images (*.png *.jpg *.jpeg *.webp *.avif *.bmp)")[0]
        if f: self.picture.setText(f); self.w["BackgroundMode"].setCurrentIndex(1)

    def restore(self):
        for k, wd in self.w.items(): self.set(wd, DEFAULTS[k])
        self.cols = list(DEFAULT_COLOURS); self.refresh_swatches()
        self.set_water(STR_DEFAULTS["BackgroundColor"]); self.picture.setText(""); self.gpu.setCurrentIndex(0)
        for c in self.species: c.setChecked(True)

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
        species = ",".join(str(i + 1) for i, c in enumerate(self.species) if c.isChecked()) or "1"
        out.update(BackgroundColor=self.water_colour, BackgroundImage=self.picture.text().strip(), Gpu=self.gpu.currentData() or "",
                   Species=species)
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
