/* The GPU lanes' GLSL sources, shared verbatim by the desktop-GL and
 * GLES flavors, the wayland quad lane, and tests/test_gles_audit.c
 * (the audit compiles THE REAL lane shaders against a real ES
 * context — a copy would drift). One GLSL ES 1.00 source runs on
 * both flavors: ES contexts get the precision statement prepended
 * by the caller (gp_compile / the audit), desktop-compat compiles
 * it as-is. attribute/varying/texture2D/gl_FragColor are ES 1.00
 * and desktop-compat legal; ES 3.x hosts GLSL ES 1.00 shaders by
 * spec. The ES precision prepend is LOAD-BEARING: GLSL ES 1.00
 * fragment shaders have no default float precision — the audit's
 * negative control compiles without it and must fail. */

static const char *GP_VS =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "attribute vec4 a_color;\n"
    "attribute vec4 a_aux0;\n"
    "attribute vec4 a_aux1;\n"
    "attribute vec4 a_aux2;\n"
    "attribute vec4 a_clip;\n"
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_aux0;\n"
    "varying vec4 v_aux1;\n"
    "varying vec4 v_aux2;\n"
    "varying vec4 v_clip;\n"
    "uniform vec2 u_vp;\n"
    "void main() {\n"
    "    v_uv = a_uv;\n"
    "    v_color = a_color;\n"
    "    v_aux0 = a_aux0;\n"
    "    v_aux1 = a_aux1;\n"
    "    v_aux2 = a_aux2;\n"
    "    v_clip = a_clip;\n"
    "    vec2 c = a_pos / u_vp * 2.0 - 1.0;\n"
    "    gl_Position = vec4(c.x, -c.y, 0.0, 1.0);\n"
    "}\n";

static const char *GP_FS_BODY =
    "varying vec2 v_uv;\n"
    "varying vec4 v_color;\n"
    "varying vec4 v_aux0;\n"
    "varying vec4 v_aux1;\n"
    "varying vec4 v_aux2;\n"
    "varying vec4 v_clip;\n"
    "uniform sampler2D u_tex;\n"
    "float sd_rect(vec2 p, vec2 b) {\n"
    "    vec2 d = abs(p) - b;\n"
    "    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0);\n"
    "}\n"
    "void main() {\n"
    "    if (gl_FragCoord.x < v_clip.x || gl_FragCoord.x > v_clip.z ||\n"
    "        gl_FragCoord.y < v_clip.y || gl_FragCoord.y > v_clip.w) {\n"
    "        discard;\n"
    "    }\n"
    "    float mode = v_aux0.x;\n"
    "    vec2 half_sz = v_aux2.zw * 0.5;\n"
    "    vec2 p = v_uv;\n" /* rect-local pixels */
    "    vec4 c = vec4(0.0);\n"
    "    if (mode < 0.5) {\n" /* fill: rect (radius) or circle */
    "        float d;\n"
    "        if (v_aux0.w < 0.5) {\n"
    /*  rounded box: sd of the box inset by r, MINUS r (the standard
        rounded-box SDF — without the -r the shape is the inset box) */
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y)) -\n"
    "                v_aux0.y;\n"
    "        } else {\n"
    "            d = length(p - half_sz) - min(half_sz.x, half_sz.y);\n"
    "        }\n"
    "        float a = clamp(0.5 - d, 0.0, 1.0);\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 1.5) {\n" /* stroke: ring at stroke_half */
    "        float d;\n"
    "        if (v_aux0.w < 0.5) {\n"
    "            d = sd_rect(p - half_sz, half_sz - vec2(v_aux0.y)) -\n"
    "                v_aux0.y;\n"
    "        } else {\n"
    /*  circle stroke: the quad is padded 1px (the emitter), the true
        radius rides aux0.y — undo the pad before the SDF */
    "            d = length(p - half_sz) -\n"
    "                (min(half_sz.x, half_sz.y) - v_aux0.y);\n"
    "        }\n"
    /*  Rect strokes: the CPU paints the boundary PIXEL ring, whose
        center sits half a pixel INSIDE the shape edge — the ring
        band below is inside-only and matches. Circle strokes: the
        CPU's coverage is the 2px triangle 1-|dist-R|, straddling the
        mathematical radius (the radial cross-section evidence:
        two half-coverage pixels, cpu 103/101 vs the old gpu 203/0) —
        the circle branch uses that triangle directly. */
    "        float a;\n"
    "        if (v_aux0.w < 0.5) {\n"
    "            float ring = abs(d + v_aux0.z) - v_aux0.z;\n"
    "            a = clamp(0.5 - ring, 0.0, 1.0);\n"
    "        } else {\n"
    "            a = clamp(1.0 - abs(d), 0.0, 1.0);\n"
    "        }\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 2.5) {\n" /* vertical gradient */
    "        float t = clamp(p.y / max(v_aux2.w, 1.0), 0.0, 1.0);\n"
    "        vec3 rgb = mix(v_color.rgb, v_aux1.rgb, t);\n"
    "        float alpha = mix(v_color.a, v_aux1.a, t);\n"
    "        c = vec4(rgb, alpha);\n"
    "    } else if (mode < 3.5) {\n" /* shadow: gaussian-ish falloff */
    "        float d = max(sd_rect(p - half_sz, half_sz), 0.0);\n"
    "        float sigma = max(v_aux0.z, 0.5);\n"
    "        float a = exp(-(d * d) / (2.0 * sigma * sigma));\n"
    "        c = vec4(v_color.rgb, v_color.a * a);\n"
    "    } else if (mode < 4.5) {\n" /* texture RGBA (uv 0..1) */
    "        vec4 t = texture2D(u_tex, v_uv);\n"
    "        c = vec4(t.rgb * v_color.rgb, t.a * v_color.a) *\n"
    "            v_aux2.x;\n" /* global alpha rides aux2.x */
    "    } else if (mode < 5.5) {\n" /* glyph coverage (probed mask format) */
    "        float l = texture2D(u_tex, v_uv).r;\n"
    "        c = vec4(v_color.rgb, v_color.a * l * v_aux2.x);\n"
    "    } else {\n"             /* mode 6: texture, OPAQUE (surface
                                     blits — the XRGB X byte is zero,
                                     alpha must not come from it) */
    "        vec4 t = texture2D(u_tex, v_uv);\n"
    "        c = vec4(t.rgb * v_color.rgb, v_color.a) * v_aux2.x;\n"
    "    }\n"
    "    gl_FragColor = c;\n"
    "}\n";

static const char *QUAD_VS =
    "attribute vec2 a_pos;\n"
    "varying vec2 v_uv;\n"
    "uniform vec2 u_size;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x * 2.0 - 1.0,\n"
    "                  1.0 - a_pos.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_pos;\n"
    "}\n";
static const char *QUAD_FS =
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0);\n"
    "}\n";
