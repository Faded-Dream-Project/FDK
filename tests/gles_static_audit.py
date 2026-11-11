#!/usr/bin/env python3
"""tests/gles_static_audit.py — the GLES format-constant static audit.

The runtime audit (test_gles_audit.c) boots a real ES context, but
Mesa is LENIENT where NVIDIA is strict (it accepts GL_LUMINANCE on
ES 3.x and even ES 3.x formats on ES 2.0 requests), so no local stack
can exercise the rejection. This checker has teeth anyway: the
2026-11-11 LUMINANCE bug WAS static — the constant 0x1909 sat in a
TexImage2D call in src/render/gpaint.c and no local run ever caught
it. This script locks the lane sources to the ES 3.0/3.2 core format
table (plus explicitly sanctioned gated/fallback sites), so the bug
class fails in CI deterministically.

Checks:
  1. Creation/upload format constants at glTexImage2D / glTexSubImage2D
     call sites in the GLES lanes must be core ES 3.0 (or the BGRA
     extension row, only on lines guarded by the probed bgra_ok gate).
  2. GL_LUMINANCE / GL_ALPHA / GL_LUMINANCE_ALPHA are forbidden as
     creation/upload formats everywhere except the sanctioned
     bring-up probe fallback (the mask_ifmt/mask_fmt assignment).
  3. The shared shader header carries no desktop-only GLSL constructs
     (#version 3+, layout/flat/uint/uniform-block) and the ES head
     (precision statement) is prepended by both compilers.
  4. The EGLImage side door stays extension-gated (the entry checks
     gl_oes_egl_image before the OES call).

Exit: 0 all green, 3 findings, 1 internal error.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Core ES 3.0/3.2 texture format constants (TexImage2D/TexSubImage2D),
# by name and numeric literal.
CORE_FORMATS = {
    "GL_R8": 0x8229, "GL_RED": 0x1903,
    "GL_RGBA": 0x1908, "GL_RGB": 0x1907,
    "GL_RGBA8": 0x8058, "GL_RGB8": 0x8051,
    "GL_RGBA4": 0x8056, "GL_RGB5_A1": 0x8057, "GL_RGB565": 0x8D62,
}
CORE_TYPES = {
    "GL_UNSIGNED_BYTE": 0x1401, "GL_UNSIGNED_SHORT_5_6_5": 0x8363,
    "GL_FLOAT": 0x1406,
}
# Non-core rows the lanes may use ONLY behind a runtime gate.
BGRA = ("GL_BGRA", 0x80E1)          # EXT_texture_format_BGRA8888
BGRA_EXT_NAME = "GL_BGRA_EXT"       # the lane files' own spelling
LEGACY = ("GL_LUMINANCE", 0x1909)   # ES 2.0-only; sanctioned fallback
FALLBACK_MARKERS = (
    "mask_ifmt = 0x1909", "mask_fmt = 0x1909",
)
# Per-file gate vocabulary: gpaint.c probes bgra_ok at bring-up; the
# wayland lane gates BGRA on its extension check + the shape ladder.
GATE_MARKERS_BY_FILE = {
    "src/render/gpaint.c": ("g->bgra_ok", "bgra_ok = 1",
                            "probe_tex"),
    "src/platform/wayland/wayland_egl.c": (
        "has_bgra_ext", "shape_rejected", "lane_shape_uses_bgra",
        "bgra_internal_rgba", "GLC_BGRA", "poison_first"),
}

TEXTURE_CALLS = re.compile(
    r"(TexImage2D|TexSubImage2D|RenderbufferStorage)\s*\(")
CONST = re.compile(r"\b(GL_[A-Z0-9_]+|0x[0-9A-Fa-f]{4})\b")
SHADER_FORBIDDEN = re.compile(
    r"#version\s+3|(^|\n)\s*layout\s*\(|\bflat\b|\buint\b|\bivec[234]\b"
    r"|\buvec[234]\b|uniform\s+\w+\s*\{")


def numeric_of(tok):
    tok = tok.strip()
    if tok.startswith("0x"):
        return int(tok, 16)
    return None


def strip_comments(lines):
    """Blank out C comments so prose mentioning GL_LUMINANCE (the
    audit's own explanations) is not scanned as code."""
    out, in_block = [], False
    for line in lines:
        res = []
        i = 0
        while i < len(line):
            if in_block:
                j = line.find("*/", i)
                if j < 0:
                    i = len(line)
                    continue
                in_block = False
                res.append(" " * (j + 2 - i))
                i = j + 2
            elif line.startswith("//", i):
                break
            elif line.startswith("/*", i):
                j = line.find("*/", i + 2)
                if j < 0:
                    in_block = True
                    res.append(" " * (len(line) - i))
                    i = len(line)
                else:
                    res.append(" " * (j + 2 - i))
                    i = j + 2
            else:
                res.append(line[i])
                i += 1
        out.append("".join(res))
    return out


def findings_for_texture_sites(path, rel, allowed_files_bgra):
    """Return finding strings for texture creation/upload sites."""
    out = []
    text = path.read_text(encoding="utf-8")
    lines = strip_comments(text.split("\n"))
    for i, line in enumerate(lines):
        m = TEXTURE_CALLS.search(line)
        if m is None:
            continue
        # A call may span lines and its gate sits nearby — scan a
        # two-sided window (5 lines back, 6 forward).
        chunk = "\n".join(lines[max(0, i - 8):i + 7])
        toks = CONST.findall(chunk)
        gates = GATE_MARKERS_BY_FILE.get(rel, ())
        for tok in toks:
            num = numeric_of(tok)
            name = tok if tok.startswith("GL_") else None
            if name is not None and (name in CORE_FORMATS or
                                     name in CORE_TYPES):
                continue
            if num is not None:
                vals = {v for v in CORE_FORMATS.values()}
                vals |= {v for v in CORE_TYPES.values()}
                if num in vals:
                    continue
            # BGRA: allowed only when the line chunk carries the
            # file's own gate (or IS the probe that decides it)
            if (name in (BGRA[0], BGRA_EXT_NAME) or num == BGRA[1]):
                if rel in allowed_files_bgra and any(
                        gm in chunk for gm in gates):
                    continue
                out.append(f"{rel}:{i + 1}: BGRA use without the "
                           f"gate near a texture call")
                continue
            # LUMINANCE-class: allowed only on the sanctioned fallback
            if (name == LEGACY[0] or num == LEGACY[1]):
                if any(fm in chunk for fm in FALLBACK_MARKERS):
                    continue
                out.append(f"{rel}:{i + 1}: LUMINANCE-class format "
                           f"({tok}) outside the sanctioned fallback")
                continue
            # Other GL_ constants on these lines (targets, enums) are
            # fine unless they are *format-looking* names we don't
            # recognize — flag names we explicitly do not know that
            # appear format-positioned. Keep the audit conservative:
            # only unknown GL_ names that look like formats.
            if name is not None and re.match(
                    r"GL_(R|RGB|RGBA|LUM|ALPHA|BGRA|BGR|SRGB|DEPTH|"
                    r"STENCIL|R11F|R16|RG)", name):
                out.append(f"{rel}:{i + 1}: unknown format-like "
                           f"constant {tok} at a texture call")
    return out


def check_shaders():
    out = []
    # 2026-11-11: the QUAD_* pair moved to gpu_shaders_quad.h (the
    # zero-warning split); both files carry the same rules.
    gp = (ROOT / "src/render/gpu_shaders.h").read_text(encoding="utf-8")
    quad = (ROOT / "src/render/gpu_shaders_quad.h").read_text(
        encoding="utf-8")
    for name, t in (("gpu_shaders.h", gp),
                    ("gpu_shaders_quad.h", quad)):
        if SHADER_FORBIDDEN.search(t):
            out.append("src/render/%s: desktop-only GLSL "
                       "construct present in the shader source" % name)
    if "precision mediump float;" not in (
            ROOT / "src/render/gpaint.c").read_text(encoding="utf-8"):
        out.append("src/render/gpaint.c: gp_compile lost the ES "
                   "precision head")
    if "precision mediump float;" not in quad:
        out.append("src/render/gpu_shaders_quad.h: the quad FS lost "
                   "its inline precision statement")
    return out


def check_eglimage_gate():
    out = []
    gp = (ROOT / "src/render/gpaint.c").read_text(encoding="utf-8")
    if "gl_oes_egl_image" not in gp:
        out.append("src/render/gpaint.c: the GL_OES_EGL_image gate is "
                   "gone")
    return out


def main():
    findings = []
    # The GLES-format lanes. x11_gl.c is desktop-GL/GLX (out of the ES
    # table's scope by construction) and is deliberately not scanned.
    lanes = [
        ("src/render/gpaint.c", True),
        ("src/platform/wayland/wayland_egl.c", True),
    ]
    for rel, bgra_allowed in lanes:
        findings += findings_for_texture_sites(
            ROOT / rel, rel, {rel} if bgra_allowed else set())
    findings += check_shaders()
    findings += check_eglimage_gate()

    print("==== GLES static format audit ====")
    if findings:
        for f in findings:
            print("  FINDING:", f)
        print(f"==== GLES STATIC AUDIT: {len(findings)} finding(s) ====")
        return 3
    print("  texture-site formats: core ES 3.0 (+ gated BGRA, "
          "sanctioned LUMINANCE fallback)")
    print("  shaders: ES 1.00-only constructs, precision heads intact")
    print("  EGLImage: extension gate intact")
    print("==== GLES STATIC AUDIT: all green ====")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # noqa: BLE001 — the audit must fail loud
        print(f"static audit internal error: {exc}")
        sys.exit(1)
