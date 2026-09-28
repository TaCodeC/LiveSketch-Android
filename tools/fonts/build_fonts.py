#!/usr/bin/env python3
"""Genera las fuentes de la interfaz, recortadas a lo que usa la app.

- Inter 4.1 (SIL OFL 1.1) en tres pesos, con el latín de Europa occidental y central.
- Los iconos de Lucide 1.48 (ISC) que usa la interfaz, en una fuente de iconos.

Uso:
    python3 tools/fonts/build_fonts.py [--inter DIR] [--lucide DIR]

DIR es la carpeta `package` de los paquetes npm inter-ui@4.1.1 y lucide-static@1.48.0.
Sin argumentos se descargan con `npm pack`. Necesita fonttools y brotli:
    python3 -m pip install fonttools brotli

Escribe app/src/main/assets/fonts/*.ttf y app/src/main/cpp/UI/Icons.h.
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile

from fontTools import subset
from fontTools.ttLib import TTFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
FONTS_DIR = ROOT / "app/src/main/assets/fonts"
ICONS_HEADER = ROOT / "app/src/main/cpp/UI/Icons.h"

INTER_PACKAGE = "inter-ui@4.1.1"
LUCIDE_PACKAGE = "lucide-static@1.48.0"

# Pesos de Inter: normal para el texto, seminegrita para títulos y botones, negrita para
# los rótulos cortos ("EN VIVO") y los títulos grandes.
INTER_WEIGHTS = ["Regular", "SemiBold", "Bold"]

# Latín básico y Latin-1 (español, portugués, francés...), Latin Extended-A (resto de
# Europa), puntuación general (– — ‘ ’ “ ” • … ‹ ›), €, ™, flechas, − y el carácter de
# sustitución. Los nombres de capa los escribe el usuario, por eso no se recorta más.
INTER_UNICODES = "U+0020-007E,U+00A0-017F,U+2000-206F,U+20AC,U+2122,U+2190-2193,U+2212,U+2215,U+FFFD"

# Iconos: nombre de la constante en C++ -> nombre del icono en Lucide.
ICONS = {
    "kWrench": "wrench",
    "kImageDown": "image-down",
    "kScan": "scan",
    "kRadio": "radio",
    "kBrush": "brush",
    "kEraser": "eraser",
    "kLayers": "layers",
    "kPipette": "pipette",
    "kUndo": "undo-2",
    "kRedo": "redo-2",
    "kPlus": "plus",
    "kCheck": "check",
    "kEllipsis": "ellipsis",
    "kCopy": "copy",
    "kMerge": "merge",
    "kPencil": "pencil",
    "kTrash": "trash-2",
    "kBrushCleaning": "brush-cleaning",
    "kChevronUp": "chevron-up",
    "kChevronDown": "chevron-down",
    "kChevronRight": "chevron-right",
    "kChevronLeft": "chevron-left",
    "kFilePlus": "file-plus",
    "kHand": "hand",
    "kPanelRight": "panel-right",
    "kTextSize": "a-large-small",
    "kInfo": "info",
    "kX": "x",
    "kCircleCheck": "circle-check",
    "kCircleAlert": "circle-alert",
    "kDroplet": "droplet",
    "kLogOut": "log-out",
    "kGripVertical": "grip-vertical",
    "kEye": "eye",
    "kEyeOff": "eye-off",
    "kUsers": "users",
}


def npm_pack(package: str, work: pathlib.Path) -> pathlib.Path:
    """Descarga un paquete de npm y devuelve su carpeta `package`."""
    out = subprocess.run(["npm", "pack", package, "--silent"], cwd=work, check=True,
                         capture_output=True, text=True).stdout.strip().splitlines()[-1]
    folder = work / package.split("@")[0]
    with tarfile.open(work / out) as archive:
        archive.extractall(folder, filter="data")
    return folder / "package"


def subset_font(source: pathlib.Path, target: pathlib.Path, unicodes: str) -> None:
    options = subset.Options()
    options.layout_features = []      # ImGui no usa GSUB/GPOS
    options.hinting = False           # se rasteriza sin hinting, como en iOS
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]   # conserva copyright y licencia
    options.notdef_outline = True
    options.recalc_bounds = True
    options.flavor = None             # TTF sin comprimir: stb_truetype no lee WOFF2
    font = subset.load_font(str(source), options)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=subset.parse_unicodes(unicodes))
    subsetter.subset(font)
    subset.save_font(font, str(target), options)


def utf8_escape(codepoint: int) -> str:
    return "".join(f"\\x{b:02x}" for b in chr(codepoint).encode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--inter", type=pathlib.Path, help="carpeta package de inter-ui")
    parser.add_argument("--lucide", type=pathlib.Path, help="carpeta package de lucide-static")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(tmp)
        inter = args.inter or npm_pack(INTER_PACKAGE, work)
        lucide = args.lucide or npm_pack(LUCIDE_PACKAGE, work)
        FONTS_DIR.mkdir(parents=True, exist_ok=True)

        for weight in INTER_WEIGHTS:
            target = FONTS_DIR / f"Inter-{weight}.ttf"
            subset_font(inter / "web" / f"Inter-{weight}.woff2", target, INTER_UNICODES)
            print(f"{target.relative_to(ROOT)}: {target.stat().st_size // 1024} KB")
        shutil.copyfile(inter / "LICENSE.txt", FONTS_DIR / "Inter-LICENSE.txt")

        codepoints = json.loads((lucide / "font/codepoints.json").read_text())
        missing = [name for name in ICONS.values() if name not in codepoints]
        if missing:
            print("Iconos que no existen en Lucide:", ", ".join(missing), file=sys.stderr)
            return 1
        used = {constant: codepoints[name] for constant, name in ICONS.items()}
        target = FONTS_DIR / "LucideIcons.ttf"
        subset_font(lucide / "font/lucide.ttf", target, ",".join(f"U+{cp:04X}" for cp in used.values()))
        print(f"{target.relative_to(ROOT)}: {target.stat().st_size // 1024} KB, {len(used)} iconos")
        shutil.copyfile(lucide / "LICENSE", FONTS_DIR / "Lucide-LICENSE.txt")

        # Comprobación: todos los iconos están en la fuente recortada.
        cmap = TTFont(str(target)).getBestCmap()
        absent = [ICONS[c] for c, cp in used.items() if cp not in cmap]
        if absent:
            print("Iconos que faltan en la fuente recortada:", ", ".join(absent), file=sys.stderr)
            return 1

    lines = [
        "#pragma once",
        "",
        "// Generado por tools/fonts/build_fonts.py: no editar a mano.",
        "// Iconos de Lucide (licencia ISC, assets/fonts/Lucide-LICENSE.txt) de la fuente",
        "// assets/fonts/LucideIcons.ttf. Cada constante es el carácter del icono en UTF-8.",
        "",
        "namespace icon {",
        "",
    ]
    width = max(len(c) for c in used)
    for constant, codepoint in used.items():
        lines.append(f'inline constexpr const char* {constant:<{width}} = "{utf8_escape(codepoint)}"; '
                     f"// {ICONS[constant]} U+{codepoint:04X}")
    lines += ["", "} // namespace icon", ""]
    ICONS_HEADER.write_text("\n".join(lines))
    print(f"{ICONS_HEADER.relative_to(ROOT)}: {len(used)} iconos")
    return 0


if __name__ == "__main__":
    sys.exit(main())
