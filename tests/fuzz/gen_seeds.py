#!/usr/bin/env python3
"""Seeds the fuzz corpora from committed fixtures (docs/fuzzing.md).

Every seed comes from a file already in the tree (test fixtures,
shipped themes, test-app assets) so the corpus is reproducible and
adds no new provenance surface. Re-run after adding fixtures:

    python3 tests/fuzz/gen_seeds.py

Writes tests/fuzz/corpus/<harness>/ — one file per seed.
"""

import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def w(path, data):
    if isinstance(data, str):
        data = data.encode()
    with open(path, "wb") as f:
        f.write(data)


def theme_seeds(out):
    # Real faces: the shipped default + the fixture corpus.
    for src, name in [
        (".FDKThemes/faded-dream.fdk", "faded-dream.fdk"),
        ("tests/data/themes/alpha.fdk", "alpha.fdk"),
        ("tests/data/themes/beta.fdk", "beta.fdk"),
        ("tests/data/themes/broken.fdk", "broken.fdk"),
    ]:
        p = os.path.join(ROOT, src)
        if os.path.exists(p):
            shutil.copy(p, os.path.join(out, name))
    # Grammar skeletons: valid header + each section kind, and the
    # classic malformations (missing value, deep nesting, long key).
    w(os.path.join(out, "skeleton-min.fdk"), "[theme]\nname = x\n")
    w(os.path.join(out, "skeleton-sections.fdk"),
      "[theme]\nname = x\nversion = 1\n[colors]\nbg = #111111\n"
      "[metrics]\nradius = 4\n")
    w(os.path.join(out, "bad-no-value.fdk"), "[theme]\nname =\n")
    w(os.path.join(out, "bad-utf8.fdk"), b"[theme]\nname = \xff\xfe\x81\n")
    w(os.path.join(out, "bad-nul.fdk"), b"[theme]\nna\x00me = x\n")
    w(os.path.join(out, "long-line.fdk"),
      "[theme]\nname = " + "a" * 60000 + "\n")
    w(os.path.join(out, "bom.fdk"),
      b"\xef\xbb\xbf[theme]\nname = x\n")


def catalog_seeds(out):
    # Real .fmo files from the test apps' translation sets.
    for app in ["fdk-files", "fdk-edit", "fdk-term", "fdk-monitor"]:
        p = os.path.join(ROOT, "test-apps", app, "po", "ro.fmo")
        if os.path.exists(p):
            shutil.copy(p, os.path.join(out, app + "-ro.fmo"))
    # Magic + header shapes: right magic truncated, wrong magic,
    # version extremes (the format's own header fields).
    w(os.path.join(out, "empty.bin"), b"")
    w(os.path.join(out, "trunc-magic.bin"), b"FDK")
    w(os.path.join(out, "wrong-magic.bin"), b"XXXX" + b"\x00" * 32)
    w(os.path.join(out, "zeros.bin"), b"\x00" * 64)
    w(os.path.join(out, "ff.bin"), b"\xff" * 64)


def prefs_seeds(out):
    w(os.path.join(out, "basic.prefs"),
      "[window]\nx = 10\ny = 20\n[general]\ndark = true\nname = app\n")
    w(os.path.join(out, "comments.prefs"),
      "# comment\n// also comment\n\n[a]\n; not a comment marker\n")
    w(os.path.join(out, "bad-no-section.prefs"), "key = value\n")
    w(os.path.join(out, "bad-utf8.prefs"), b"[a]\nk = \xc3\x28\n")
    w(os.path.join(out, "bad-huge-value.prefs"),
      "[a]\nk = " + "v" * 70000 + "\n")
    w(os.path.join(out, "types.prefs"),
      "[t]\ni = -42\nf = 3.5\nb = false\nb2 = 1\ns = \"quoted\"\n")
    w(os.path.join(out, "dup-keys.prefs"), "[a]\nk = 1\nk = 2\nk = 3\n")


def c_array_bytes(header_path, names):
    """Extracts `static const fdk_u8 name[] = {...};` blobs from a
    committed fixture header — the suite's committed-bytes pattern."""
    src = open(os.path.join(ROOT, header_path)).read()
    out = {}
    for name in names:
        m = re.search(re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};",
                      src, re.S)
        if not m:
            continue
        body = m.group(1)
        vals = [int(t, 0) for t in re.findall(r"0x[0-9a-fA-F]+|\b\d+\b", body)]
        out[name] = bytes(vals)
    return out


def image_seeds(out):
    gifs = c_array_bytes("tests/test_gif_fixtures.h",
                         ["matrix_gif", "single_gif", "interlaced_gif",
                          "delays_gif"])
    for n, b in gifs.items():
        w(os.path.join(out, n + ".gif"), b)
    pngs = c_array_bytes("tests/test_png_bytes.h", ["test_png_bytes"])
    for n, b in pngs.items():
        w(os.path.join(out, n + ".png"), b)
    icons = c_array_bytes("tests/test_icon_pngs.h",
                          ["icon_png_folder_16", "icon_png_text_16"])
    for n, b in icons.items():
        w(os.path.join(out, n + ".png"), b)
    # Cross-format magics: PNG header only, GIF header only, and a
    # JPEG SOI (stb dispatches; refusal is fine, the walk matters).
    w(os.path.join(out, "png-header-only.png"),
      b"\x89PNG\r\n\x1a\n" + b"\x00" * 32)
    w(os.path.join(out, "gif-header-only.gif"), b"GIF89a" + b"\x00" * 16)
    w(os.path.join(out, "soi.jpg"), b"\xff\xd8\xff\xe0" + b"\x00" * 48)
    w(os.path.join(out, "zeros.bin"), b"\x00" * 64)


def svg_seeds(out):
    w(os.path.join(out, "eye.svg"),
      '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24">'
      '<path d="M12 7.5a4.5 4.5 0 0 1 4.5 4.5 4.5 4.5 0 0 1-4.5 4.5 '
      '4.5 4.5 0 0 1-4.5-4.5 4.5 4.5 0 0 1 4.5-4.5z" fill="#000"/></svg>')
    w(os.path.join(out, "empty.svg"), "<svg></svg>")
    w(os.path.join(out, "viewbox.svg"),
      '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">'
      '<rect x="10" y="10" width="80" height="80"/></svg>')
    w(os.path.join(out, "truncated.svg"), '<svg xmlns="http://')
    w(os.path.join(out, "bad-path.svg"),
      '<svg xmlns="http://www.w3.org/2000/svg"><path d="M this is not '
      'a path"/></svg>')
    w(os.path.join(out, "deep-nest.svg"),
      "<svg>" + "<g>" * 200 + "</g>" * 200 + "</svg>")
    w(os.path.join(out, "no-svg.svg"), "not an svg at all")
    # Real asset: the files app's icon.
    p = os.path.join(ROOT, "test-apps/fdk-files/icon.svg")
    if os.path.exists(p):
        shutil.copy(p, os.path.join(out, "fdk-files-icon.svg"))


def dbus_seeds(out):
    # A minimal valid METHOD_CALL header, little-endian: flags,
    # version 1, empty body fields — the shapes the wire allows.
    def frame(end, mtype, flags, body):
        h = bytes([end, mtype, flags, 1])
        h += (0).to_bytes(4, "little" if end == ord("l") else "big")
        h += body
        return h
    w(os.path.join(out, "call-le-empty.bin"), frame(ord("l"), 1, 0, b""))
    w(os.path.join(out, "call-be-empty.bin"), frame(ord("B"), 1, 0, b""))
    w(os.path.join(out, "signal-le.bin"), frame(ord("l"), 4, 1, b""))
    w(os.path.join(out, "bad-endian.bin"), frame(ord("X"), 1, 0, b""))
    w(os.path.join(out, "bad-type.bin"), frame(ord("l"), 0xEE, 0, b""))
    w(os.path.join(out, "short.bin"), b"l\x01\x00\x01")
    w(os.path.join(out, "zeros.bin"), b"\x00" * 32)
    w(os.path.join(out, "ff.bin"), b"\xff" * 32)


def main():
    base = os.path.join(ROOT, "tests/fuzz/corpus")
    seeds = {
        "fuzz_theme": theme_seeds,
        "fuzz_catalog": catalog_seeds,
        "fuzz_prefs": prefs_seeds,
        "fuzz_image": image_seeds,
        "fuzz_svg": svg_seeds,
        "fuzz_dbus": dbus_seeds,
    }
    for name, fn in seeds.items():
        out = os.path.join(base, name)
        os.makedirs(out, exist_ok=True)
        fn(out)
        n = len(os.listdir(out))
        print(f"{name}: {n} seeds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
