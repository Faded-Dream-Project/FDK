/* The wl EGL quad lane's GLSL passthrough pair (wayland_egl.c and
 * tests/test_gles_audit.c). Split out of gpu_shaders.h in the
 * 2026-11-11 zero-warning sweep — see that file's header for the
 * dual-flavor / precision-head story. QUAD_FS carries its own
 * precision #ifdef: it never depends on a caller prepending one. */

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
