/* GS back end on the GPU (OpenGL 3.3 core via WGL).
 *
 * - Each GS frame buffer the game draws to is a render target: an RGBA8
 *   texture + FBO of (FBW*64) x 1024 GS pixels at the internal scale. Z buffers
 *   are DEPTH32F textures keyed on ZBP; Z is written from the shader.
 * - Draws arrive as batches with one gs_draw_state; the fragment shader does
 *   texturing (texelFetch, GS wrap modes, bilinear by hand), TFX, fog, alpha
 *   test (two passes when AFAIL keeps something) and builds the GS blend
 *   (A-B)*C+D with dual-source blending.
 * - Local memory (gs_vram) stays the truth for everything not drawn on the
 *   GPU. Host uploads are mirrored into overlapping targets of the same
 *   layout; anything that reads memory a dirty target covers (texture decode,
 *   CLUT load, transfers) first downloads that target. Textures that are a
 *   target are sampled straight from it (copied first if it is also the one
 *   being drawn to).
 * - Decoded textures are cached on TEX0/CLUT/TEXA and a per-page write
 *   generation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "rt.h"
#include "gs.h"
#include "config.h"
#include <x86intrin.h>
#include <math.h>

#ifndef APIENTRY
#define APIENTRY
#endif
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#define GL_RGBA32F 0x8814
#define GL_TEXTURE0 0x84C0
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA8 0x8058
#define GL_DEPTH_COMPONENT32F 0x8CAC
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FUNC_ADD 0x8006
#define GL_FUNC_SUBTRACT 0x800A
#define GL_FUNC_REVERSE_SUBTRACT 0x800B
#define GL_SRC1_COLOR 0x88F9
#define GL_ARRAY_BUFFER 0x8892
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_READ_ONLY 0x88B8
#define GL_STREAM_DRAW 0x88E0
#define GL_DEPTH_COMPONENT 0x1902

#define GLF(X) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glUniform2i, (GLint, GLint, GLint)) \
    X(void, glUniform4i, (GLint, GLint, GLint, GLint, GLint)) \
    X(void, glUniform4ui, (GLint, GLuint, GLuint, GLuint, GLuint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void *, glMapBuffer, (GLenum, GLenum)) \
    X(GLboolean, glUnmapBuffer, (GLenum)) \
    X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, glBufferSubData, (GLenum, ptrdiff_t, GLsizeiptr, const void *)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
    X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glGenQueries, (GLsizei, GLuint *)) \
    X(void, glBeginQuery, (GLenum, GLuint)) \
    X(void, glEndQuery, (GLenum)) \
    X(void, glGetQueryObjectui64v, (GLuint, GLenum, unsigned long long *))
#define X(r, n, a) static r (APIENTRY *n) a;
GLF(X)
#undef X

/* ---- window --------------------------------------------------------------- */
static HWND wnd;
static HDC dc;
static int shown, S = 2;
static BOOL (WINAPI *swap_interval_fn)(int);
static int vsync_interval, frames_since_check;
static double swap_wait_ms;
static HGLRC glrc;
static u64 n_presents, n_present_gaps, n_present_gaps_late; static double present_gap_max;
static void choose_vsync(void);

/* borderless fullscreen on the window's monitor (F11 or Alt+Enter toggles) */
static RECT saved_rect; static int is_full;
static void set_fullscreen(HWND h, int on) {
    if (on == is_full) return;
    if (on) {
        GetWindowRect(h, &saved_rect);
        MONITORINFO mi = { sizeof mi };
        GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(h, NULL, saved_rect.left, saved_rect.top, saved_rect.right - saved_rect.left,
                     saved_rect.bottom - saved_rect.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    is_full = on;
}

int menu_on_key(WPARAM vk, LPARAM l); int menu_save_pending(void);
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        if (w == VK_F11) { set_fullscreen(h, !is_full); return 0; }
        if (menu_on_key(w, l)) return 0;   /* Esc opens the in-game menu; while open it takes the keys */
        break;
    case WM_SYSKEYDOWN: if (w == VK_RETURN && (l & (1 << 29))) { set_fullscreen(h, !is_full); return 0; } break;
    case WM_CLOSE: LOG("gs: window closed\n"); menu_save_pending(); exit(0);   /* settings changed in an open menu are kept */
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(h, m, w, l);
}

/* The window goes on the Dell AW3418DW: the 3440x1440 monitor, else any
 * non-primary one. Device names (\\.\DISPLAYn) change between boots. */
static RECT mon_rect; static int mon_found; static char mon_name[32];
static BOOL CALLBACK mon_cb(HMONITOR m, HDC d, LPRECT r, LPARAM p) {
    (void)d; (void)r; (void)p;
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(m, (MONITORINFO *)&mi)) return TRUE;
    int w = mi.rcMonitor.right - mi.rcMonitor.left, h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int dell = w == 3440 && h == 1440;
    if (getenv("GS_PRIMARY_MONITOR")) {   /* debug (start-up only): put the window on the primary monitor */
        if (mi.dwFlags & MONITORINFOF_PRIMARY) { mon_rect = mi.rcWork; mon_found = 1; snprintf(mon_name, sizeof mon_name, "%s", mi.szDevice); }
        return TRUE;
    }
    if (dell || (!mon_found && !(mi.dwFlags & MONITORINFOF_PRIMARY))) {
        mon_rect = mi.rcWork; mon_found = 1 + dell;
        snprintf(mon_name, sizeof mon_name, "%s", mi.szDevice);
    }
    return TRUE;
}

/* menu.c: window state and settings it changes (EE thread, which owns the window) */
int gsgl_window_shown(void) { return wnd && shown; }
int gsgl_is_fullscreen(void) { return is_full; }
void gsgl_set_fullscreen(int on) { if (wnd && shown) set_fullscreen(wnd, on); }
static volatile int vsync_recheck, scale_req;
void gsgl_recheck_vsync(void) { vsync_recheck = 1; }
void gsgl_request_scale(int s) { scale_req = s; }
int gsgl_scale(void) { return scale_req ? scale_req : S; }
int gsgl_has_focus(void) { return wnd && shown && GetForegroundWindow() == wnd; }

void gsgl_pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

/* ---- GL objects -------------------------------------------------------------- */
static GLuint prog, cprog, vao, vbo;
static struct {
    GLint rtsize, half, yoff, tex, tme, fst, tfx, tcc, linear, rt24, aem, tsize, tmap, wrap, region, ta0;
    GLint atst, aref, afail_pass, fge, fogcol, abe, csel, pabe, fba, abd, sgn, fix;
    GLint mip[7], mxl, mmin, lcm, mipl, mipk;
} U;
static GLint cu_dst, cu_src, cu_tex, cu_alpha1;

static const char *vs_src =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec3 a_tex;\n"
    "layout(location=3) in float a_fog;\n"
    "uniform vec2 u_rtsize; uniform float u_half; uniform float u_yoff;\n"
    "noperspective out vec4 v_col; noperspective out vec3 v_tex; noperspective out float v_fog; noperspective out float v_z;\n"
    "void main() {\n"
    "  vec2 p = vec2(a_pos.x + u_half, a_pos.y + u_yoff + u_half) / u_rtsize * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p, 0.0, 1.0);\n"
    "  v_col = a_col; v_tex = a_tex; v_fog = a_fog; v_z = a_pos.z;\n"
    "}\n";

static const char *fs_src =
    "#version 330 core\n"
    "noperspective in vec4 v_col; noperspective in vec3 v_tex; noperspective in float v_fog; noperspective in float v_z;\n"
    "layout(location=0, index=0) out vec4 o_col;\n"
    "layout(location=0, index=1) out vec4 o_fac;\n"
    "uniform sampler2D u_tex, u_mip1, u_mip2, u_mip3, u_mip4, u_mip5, u_mip6;\n"
    "uniform int u_mxl, u_mmin, u_lcm, u_mipl; uniform float u_mipk;\n"
    "uniform int u_tme, u_fst, u_tfx, u_tcc, u_linear, u_rt24, u_aem;\n"
    "uniform vec2 u_tsize; uniform vec4 u_tmap; uniform ivec2 u_wrap; uniform ivec4 u_region; uniform float u_ta0;\n"
    "uniform int u_atst, u_aref, u_afail_pass, u_fge; uniform vec3 u_fogcol;\n"
    "uniform int u_abe, u_csel, u_pabe, u_fba; uniform ivec4 u_abd; uniform ivec2 u_sgn; uniform float u_fix;\n"
    "int wrapc(int c, int m, int mn, int mx, int sz) {\n"
    "  if (m == 0) return c & (sz - 1);\n"
    "  if (m == 1) return clamp(c, 0, sz - 1);\n"
    "  if (m == 2) return clamp(c, mn, mx);\n"
    "  return (c & mn) | mx;\n"
    "}\n"
    "vec4 fetchl(int l, ivec2 c) {\n"
    "  ivec2 sz = max(ivec2(u_tsize) >> l, ivec2(1));\n"
    "  ivec4 rg = (l == 0 || u_wrap.x == 3) ? u_region : (u_region >> l);\n"
    "  c.x = wrapc(c.x, u_wrap.x, rg.x, rg.y, sz.x);\n"
    "  c.y = wrapc(c.y, u_wrap.y, rg.z, rg.w, sz.y);\n"
    "  vec4 t;\n"
    "  if (l == 0) t = texelFetch(u_tex, ivec2(vec2(c) * u_tmap.xy + u_tmap.zw), 0);\n"
    "  else if (l == 1) t = texelFetch(u_mip1, c, 0); else if (l == 2) t = texelFetch(u_mip2, c, 0);\n"
    "  else if (l == 3) t = texelFetch(u_mip3, c, 0); else if (l == 4) t = texelFetch(u_mip4, c, 0);\n"
    "  else if (l == 5) t = texelFetch(u_mip5, c, 0); else t = texelFetch(u_mip6, c, 0);\n"
    "  t = floor(t * 255.0 + 0.5);\n"
    "  if (u_rt24 == 1 && l == 0) t.a = (u_aem == 1 && t.r + t.g + t.b == 0.0) ? 0.0 : u_ta0;\n"
    "  return t;\n"
    "}\n"
    "vec4 fetch(ivec2 c) { return fetchl(0, c); }\n"
    "vec4 samplel(int l, vec2 uv0, bool lin) {\n"
    "  vec2 uv = uv0 / float(1 << l);\n"
    "  if (!lin) return fetchl(l, ivec2(floor(uv)));\n"
    "  vec2 p = uv - 0.5; ivec2 i = ivec2(floor(p)); vec2 f = p - floor(p);\n"
    "  return floor(mix(mix(fetchl(l, i), fetchl(l, i + ivec2(1, 0)), f.x), mix(fetchl(l, i + ivec2(0, 1)), fetchl(l, i + ivec2(1, 1)), f.x), f.y));\n"
    "}\n"
    "bool atest(float a) {\n"
    "  int ia = int(a);\n"
    "  if (u_atst == 0) return false; if (u_atst == 1) return true;\n"
    "  if (u_atst == 2) return ia < u_aref; if (u_atst == 3) return ia <= u_aref;\n"
    "  if (u_atst == 4) return ia == u_aref; if (u_atst == 5) return ia >= u_aref;\n"
    "  if (u_atst == 6) return ia > u_aref; return ia != u_aref;\n"
    "}\n"
    "void main() {\n"
    "  vec4 col = floor(v_col);\n"
    "  if (u_tme == 1) {\n"
    "    vec2 uv = u_fst == 1 ? v_tex.xy : v_tex.xy / v_tex.z * u_tsize;\n"
    "    vec4 t;\n"
    "    float lod = u_lcm == 1 ? u_mipk : log2(1.0 / max(abs(u_fst == 1 ? 1.0 : v_tex.z), 1e-30)) * float(1 << u_mipl) + u_mipk;\n"
    "    if (u_mxl == 0 || lod <= 0.0) t = samplel(0, uv, u_linear == 1);\n"
    "    else {\n"
    "      bool inlin = u_mmin == 4 || u_mmin == 5, between = u_mmin == 3 || u_mmin == 5;\n"
    "      lod = min(lod, float(u_mxl));\n"
    "      if (between) {\n"
    "        int l0 = int(floor(lod)); int l1 = min(l0 + 1, u_mxl); float f = lod - floor(lod);\n"
    "        t = floor(mix(samplel(l0, uv, inlin), samplel(l1, uv, inlin), f));\n"
    "      } else t = samplel(min(int(floor(lod + 0.5)), u_mxl), uv, inlin);\n"
    "    }\n"
    "    vec4 r;\n"
    "    if (u_tfx == 0) { r.rgb = floor(t.rgb * col.rgb / 128.0); r.a = u_tcc == 1 ? floor(t.a * col.a / 128.0) : col.a; }\n"
    "    else if (u_tfx == 1) { r.rgb = t.rgb; r.a = u_tcc == 1 ? t.a : col.a; }\n"
    "    else { r.rgb = floor(t.rgb * col.rgb / 128.0) + col.a; r.a = u_tcc == 1 ? (u_tfx == 2 ? t.a + col.a : t.a) : col.a; }\n"
    "    col = min(r, vec4(255.0));\n"
    "  }\n"
    "  if (u_fge == 1) { float f = floor(v_fog * 255.0 + 0.5); col.rgb = floor((col.rgb * f + u_fogcol * (255.0 - f)) / 256.0); }\n"
    "  bool pass = atest(col.a);\n"
    "  if (u_afail_pass == 0 && !pass) discard;\n"
    "  if (u_afail_pass == 1 && pass) discard;\n"
    "  float aout = (u_fba == 1 && col.a < 128.0) ? col.a + 128.0 : col.a;\n"
    "  if (u_abe == 1 && !(u_pabe == 1 && col.a < 128.0)) {\n"
    "    float f = u_csel == 0 ? col.a / 128.0 : (u_csel == 2 ? u_fix / 128.0 : 1.0);\n"
    "    float ks = float(u_abd.x) * f + float(u_abd.y);\n"
    "    float kd = float(u_abd.z) * f + float(u_abd.w);\n"
    "    if (u_sgn.x == 1) ks = -ks;\n"
    "    if (u_sgn.y == 1) kd = -kd;\n"
    "    o_col = vec4(col.rgb * max(ks, 0.0) / 255.0, aout / 255.0);\n"
    "    o_fac = vec4(vec3(max(kd, 0.0)), 0.0);\n"
    "  } else { o_col = vec4(col.rgb / 255.0, aout / 255.0); o_fac = vec4(0.0); }\n"
    "  gl_FragDepth = v_z;\n"
    "}\n";

static const char *cvs_src =
    "#version 330 core\n"
    "uniform vec4 u_dst; uniform vec4 u_src; out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "  gl_Position = vec4(mix(u_dst.xy, u_dst.zw, c), 0.0, 1.0);\n"
    "  v_uv = mix(u_src.xy, u_src.zw, c);\n"
    "}\n";
static const char *cfs_src =
    "#version 330 core\n"
    "uniform sampler2D u_tex; uniform int u_alpha1; in vec2 v_uv; out vec4 o;\n"
    "void main() { o = texture(u_tex, v_uv); if (u_alpha1 == 1) o.a = 1.0; }\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); LOG("gs: shader error:\n%s\n", log); return 0; }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); LOG("gs: link error:\n%s\n", log); return 0; }
    return p;
}

static GLuint new_tex(int w, int h, GLenum ifmt, GLenum fmt, GLenum type) {
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* a scratch RGBA texture at least w x h, for uploads and the display fallback */
static GLuint stage_tex; static int stage_w, stage_h;
static void stage_ensure(int w, int h) {
    if (w <= stage_w && h <= stage_h) return;
    if (stage_tex) glDeleteTextures(1, &stage_tex);
    stage_w = w > stage_w ? w : stage_w; stage_h = h > stage_h ? h : stage_h;
    stage_tex = new_tex(stage_w, stage_h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
}
/* a 1x FBO for reading targets back */
static GLuint rd_tex, rd_fbo; static int rd_w, rd_h;
static void rd_ensure(int w, int h) {
    if (w <= rd_w && h <= rd_h) return;
    if (rd_tex) glDeleteTextures(1, &rd_tex);
    if (!rd_fbo) glGenFramebuffers(1, &rd_fbo);
    rd_w = w > rd_w ? w : rd_w; rd_h = h > rd_h ? h : rd_h;
    rd_tex = new_tex(rd_w, rd_h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glBindFramebuffer(GL_FRAMEBUFFER, rd_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rd_tex, 0);
}

/* ---- page generations (texture cache invalidation) ------------------------ */
static u32 page_gen[512];
static void bump_blocks(u32 b0, u32 b1) {
    for (u32 p = b0 >> 5; p <= (b1 >> 5) && p < (b0 >> 5) + 512; p++) page_gen[p & 511]++;
}
static u32 gen_of(u32 b0, u32 b1) {
    u32 g = 0;
    for (u32 p = b0 >> 5; p <= (b1 >> 5) && p < (b0 >> 5) + 512; p++) g += page_gen[p & 511] * (p + 1);
    return g;
}

u32 gsgl_vram_gen(u32 b0, u32 b1) { return gen_of(b0, b1); }   /* gs.c CLUT cache */

/* ---- render targets ---------------------------------------------------------- */
#define TH 1024          /* allocated height of every target in GS pixels */
#define NT 32
typedef struct {
    int used;
    u32 fbp, fbw, psm, w, hmax;
    GLuint tex, fbo;
    int dirty;
    u32 clean;               /* page rows already written back since the last draw */
    u64 synced_frame; u32 synced_rows;   /* rows written back for textures this frame (sync_for_texture) */
    u32 want_rows;                       /* page rows textures read this frame: captured at present */
    struct { GLuint pbo; u32 rows, y0, h, bytes; int pending; u64 frame; } cap[2];   /* async captures (sync_for_texture) */
    GLuint ztex; u32 zbp;    /* depth attached now */
} target;
static target tg[NT];
typedef struct { int used; u32 zbp, w; GLuint tex; } ztarget;
static ztarget zt[16];

static int compat(u32 a, u32 b) {
    if ((a == PSMCT32 || a == PSMCT24) && (b == PSMCT32 || b == PSMCT24)) return 1;
    return a == b;
}
static int is_color_psm(u32 p) { return p == PSMCT32 || p == PSMCT24 || p == PSMCT16 || p == PSMCT16S; }
static u32 ppr_of(u32 fbw) { return fbw ? fbw : 1; }
static u32 page_h(u32 psm) { u32 pw, ph; gs_page_dims(psm, &pw, &ph); return ph; }
static void t_range(const target *t, u32 *b0, u32 *b1) {
    gs_rect_blocks(t->psm, t->fbp, t->fbw, 0, 0, t->w, t->hmax ? t->hmax : 1, b0, b1);
}
static int overlaps(const target *t, u32 b0, u32 b1) {
    if (!t->hmax) return 0;
    u32 t0, t1; t_range(t, &t0, &t1);
    return !(b1 < t0 || b0 > t1);
}

/* Find a target of the same layout that contains base bp at a page-row offset. */
static target *find_target(u32 bp, u32 bw, u32 psm, u32 *yoff) {
    for (int i = 0; i < NT; i++) {
        target *t = &tg[i];
        if (!t->used || t->fbw != bw || !compat(t->psm, psm)) continue;
        if (bp == t->fbp) { *yoff = 0; return t; }
        u32 row = 32 * ppr_of(bw);
        if (bp > t->fbp && (bp - t->fbp) % row == 0) {
            u32 y = (bp - t->fbp) / row * page_h(psm);
            if (y < t->hmax) { *yoff = y; return t; }
        }
    }
    return 0;
}

static void use_fbo(GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
}

/* draw rgba pixels (w x h, rows top-down = GS order) into target rows at x, y */
static void blit_rgba(target *t, u32 x, u32 y, u32 w, u32 h, const u32 *pix) {
    stage_ensure(w, h);
    glBindTexture(GL_TEXTURE_2D, stage_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    use_fbo(t->fbo, t->w * S, TH * S);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glUseProgram(cprog);
    glUniform4f(cu_dst, (float)x / t->w * 2 - 1, (float)y / TH * 2 - 1, (float)(x + w) / t->w * 2 - 1, (float)(y + h) / TH * 2 - 1);
    glUniform4f(cu_src, 0, 0, (float)w / stage_w, (float)h / stage_h);
    glUniform1i(cu_alpha1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* local memory (target layout) -> RGBA8 for the rows [y0, y1) */
static void target_upload(target *t, u32 x0, u32 y0, u32 w, u32 h) {
    if (y0 >= TH) return;
    if (y0 + h > TH) h = TH - y0;
    if (x0 + w > t->w) w = t->w - x0;
    if (!w || !h) return;
    u32 *pix = malloc((size_t)w * h * 4);
    int c16 = t->psm == PSMCT16 || t->psm == PSMCT16S;
    for (u32 y = 0; y < h; y++) {
        u32 *row = pix + y * w;
        gs_read_row(t->psm, t->fbp, t->fbw, x0, y0 + y, w, row);
        if (c16) for (u32 x = 0; x < w; x++) {
            u32 v = row[x];
            row[x] = ((v & 31) << 3) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 19) | ((v & 0x8000) ? 0x80000000u : 0);
        }
    }
    blit_rgba(t, x0, y0, w, h, pix);
    free(pix);
}

/* read target rows [y0, y0+h) at 1x into rgba (w = t->w) */
static void target_read(target *t, u32 x0, u32 y0, u32 w, u32 h, u32 *out) {
    if (S == 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    } else {
        rd_ensure(w, h);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rd_fbo);
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(x0 * S, y0 * S, (x0 + w) * S, (y0 + h) * S, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, rd_fbo);
        x0 = 0; y0 = 0;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

static u64 n_downloads, n_dl_rows, n_targets_made;
/* write target rows back to local memory. Only the page rows [pr0, pr1) that
 * were drawn since the last write-back are read from the GPU. */
static void target_download_rows(target *t, u32 pr0, u32 pr1) {
    if (!t->dirty || !t->hmax) { t->dirty = 0; return; }
    u32 ph = page_h(t->psm), nrows = (t->hmax + ph - 1) / ph;
    if (pr1 > nrows) pr1 = nrows;
    int s16 = t->psm == PSMCT16 || t->psm == PSMCT16S;
    for (u32 r = pr0; r < pr1; ) {
        if (t->clean & (1u << r)) { r++; continue; }
        u32 r2 = r;
        while (r2 < pr1 && !(t->clean & (1u << r2))) r2++;
        u32 w = t->w, y0 = r * ph, y1 = r2 * ph;
        if (y1 > t->hmax) y1 = t->hmax;
        u32 h = y1 - y0;
        u32 *pix = malloc((size_t)w * h * 4);
        target_read(t, 0, y0, w, h, pix);
        for (u32 y = 0; y < h; y++) {
            const u32 *row = pix + (size_t)y * w;
            if (!s16) gs_write_row32(t->psm, t->fbp, t->fbw, y0 + y, w, row);
            else {
                u32 tmp[2048], n = w > 2048 ? 2048 : w;
                for (u32 x = 0; x < n; x++) {
                    u32 c = row[x];
                    tmp[x] = ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10) | ((c >> 31) << 15);
                }
                gs_write_row(t->psm, t->fbp, t->fbw, 0, y0 + y, n, tmp);
            }
        }
        free(pix);
        u32 b0, b1; gs_rect_blocks(t->psm, t->fbp, t->fbw, 0, y0, w, h, &b0, &b1); bump_blocks(b0, b1);
        for (u32 k = r; k < r2; k++) t->clean |= 1u << k;
        n_dl_rows += r2 - r;
        if (g_log_gs > 1) LOG("gs: download target %x rows %u-%u\n", t->fbp, y0, y1);
        r = r2;
    }
    n_downloads++;
    if (nrows >= 32 ? t->clean == 0xffffffffu : t->clean == (1u << nrows) - 1) t->dirty = 0;
}
static void target_download(target *t) { target_download_rows(t, 0, 32); }

/* page rows of t that memory blocks [b0, b1] can touch */
static void rows_for_blocks(const target *t, u32 b0, u32 b1, u32 *pr0, u32 *pr1) {
    u32 row = 32 * ppr_of(t->fbw);
    *pr0 = b0 > t->fbp ? (b0 - t->fbp) / row : 0;
    *pr1 = b1 >= t->fbp ? (b1 - t->fbp) / row + 1 : 0;
}

static void target_free(target *t) {
    for (int j = 0; j < 2; j++) if (t->cap[j].pbo) glDeleteBuffers(1, &t->cap[j].pbo);
    glDeleteTextures(1, &t->tex);
    glDeleteFramebuffers(1, &t->fbo);
    if (g_log_gs) LOG("gs: drop target %x fbw %u psm %x\n", t->fbp, t->fbw, t->psm);
    memset(t, 0, sizeof *t);
}

/* targets other than keep that overlap [b0, b1] give their drawing back to memory and go */
static void evict_overlapping(target *keep, u32 b0, u32 b1) {
    for (int i = 0; i < NT; i++)
        if (tg[i].used && &tg[i] != keep && overlaps(&tg[i], b0, b1)) { target_download(&tg[i]); target_free(&tg[i]); }
}

static u32 round_rows(u32 psm, u32 y) {
    u32 ph = page_h(psm);
    y = (y + ph - 1) / ph * ph;
    return y > TH ? TH : y;
}

static target *target_create(u32 fbp, u32 fbw, u32 psm, u32 rows) {
    target *t = 0;
    for (int i = 0; i < NT; i++) if (!tg[i].used) { t = &tg[i]; break; }
    if (!t) { t = &tg[0]; target_download(t); target_free(t); }
    t->used = 1; t->fbp = fbp; t->fbw = fbw; t->psm = psm;
    t->w = (fbw ? fbw : 1) * 64; t->hmax = round_rows(psm, rows);
    u32 b0, b1; t_range(t, &b0, &b1);
    evict_overlapping(t, b0, b1);
    t->tex = new_tex(t->w * S, TH * S, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LOG("gs: target FBO incomplete\n");
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    target_upload(t, 0, 0, t->w, t->hmax);
    n_targets_made++;
    if (g_log_gs) LOG("gs: new target %x fbw %u psm %x rows %u\n", fbp, fbw, psm, t->hmax);
    return t;
}

static void target_grow(target *t, u32 rows) {
    rows = round_rows(t->psm, rows);
    if (rows <= t->hmax) return;
    u32 b0, b1; gs_rect_blocks(t->psm, t->fbp, t->fbw, 0, t->hmax, t->w, rows - t->hmax, &b0, &b1);
    evict_overlapping(t, b0, b1);
    target_upload(t, 0, t->hmax, t->w, rows - t->hmax);
    t->hmax = rows;
}

static GLuint depth_for(target *t, u32 zbp) {
    for (int i = 0; i < 16; i++) if (zt[i].used && zt[i].zbp == zbp && zt[i].w == t->w) return zt[i].tex;
    int k = 0;
    for (int i = 0; i < 16; i++) if (!zt[i].used) { k = i; break; }
    if (zt[k].used) glDeleteTextures(1, &zt[k].tex);
    zt[k].used = 1; zt[k].zbp = zbp; zt[k].w = t->w;
    zt[k].tex = new_tex(t->w * S, TH * S, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT);
    /* clear it */
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, zt[k].tex, 0);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(1); glClearDepth(0.0); glClear(GL_DEPTH_BUFFER_BIT);
    t->ztex = zt[k].tex; t->zbp = zbp;
    if (g_log_gs) LOG("gs: new z buffer %x for target %x\n", zbp, t->fbp);
    return zt[k].tex;
}

/* ---- texture cache --------------------------------------------------------- */
#define NTC 512
typedef struct {
    int used;
    u32 tbp, tbw, tpsm, tw, th, cbp, cpsm, csa, clut_hash, ta0, ta1, aem;
    u32 gen; u64 stamp;
    u64 vhash;                     /* hash of the local memory blocks it was decoded from (0 = none) */
    GLuint tex;
} tcache;
static tcache tc[NTC];
static u64 tc_clock, n_decodes, n_same_content;

/* Mat Hoffman 2 uploads its textures again every frame (one per VIF1
 * interrupt), mostly with the same bytes: the page generation then changes
 * but the content does not. Hashing the blocks (a linear read) is much
 * cheaper than the swizzled decode and the GL upload it saves. */
static u64 vram_hash(u32 b0, u32 b1) {
    if (b1 < b0 || (u64)(b1 - b0 + 1) * 256 > (1u << 20)) return 0;
    const u64 *p = (const u64 *)(gs_vram + (size_t)(b0 & 16383) * 256);
    size_t n = (size_t)(b1 - b0 + 1) * 32;
    if ((b0 & 16383) * 256 + n * 8 > GS_VRAM_SIZE) return 0;
    u64 h = 0x9e3779b97f4a7c15ull ^ ((u64)b0 << 32 | b1);
    for (size_t i = 0; i < n; i += 4) {
        h = (h ^ p[i]) * 0xff51afd7ed558ccdull; h ^= h >> 29;
        h += p[i + 1] * 0xc4ceb9fe1a85ec53ull;
        h = (h ^ p[i + 2]) * 0x9e3779b97f4a7c15ull; h ^= h >> 31;
        h += p[i + 3];
    }
    return h | 1;
}

static void sync_for_texture(u32 b0, u32 b1);
#define TC_KEY_EQ(e, s) ((e)->tbp == (s)->tbp && (e)->tbw == (s)->tbw && (e)->tpsm == (s)->tpsm && (e)->tw == (s)->tw && \
    (e)->th == (s)->th && (e)->cbp == (s)->cbp && (e)->cpsm == (s)->cpsm && (e)->csa == (s)->csa && \
    (e)->clut_hash == (s)->clut_hash && (e)->ta0 == (s)->ta0 && (e)->ta1 == (s)->ta1 && (e)->aem == (s)->aem)
/* hint: the entry a key's hash found last time. A key has at most one entry
 * (a match is decoded in place), so a matching hint is what the scan finds. */
static u16 tc_hint[1024];
static GLuint texture_from_memory(const gs_draw_state *s) {
    u32 b0, b1; gs_rect_blocks(s->tpsm, s->tbp, s->tbw, 0, 0, 1u << s->tw, 1u << s->th, &b0, &b1);
    sync_for_texture(b0, b1);
    u32 g = gen_of(b0, b1);
    u32 hk = s->tbp * 2654435761u ^ s->tbw * 40503u ^ s->tpsm * 97u ^ s->tw * 13u ^ s->th * 7u ^ s->cbp * 2246822519u ^
             s->cpsm * 31u ^ s->csa * 101u ^ s->clut_hash ^ s->ta0 * 3u ^ s->ta1 * 5u ^ s->aem * 11u;
    hk = (hk ^ (hk >> 15)) & 1023;
    tcache *free_e = 0, *old = &tc[0];
    {
        tcache *e = &tc[tc_hint[hk]];
        if (e->used && TC_KEY_EQ(e, s)) {
            e->stamp = ++tc_clock;
            if (e->gen == g) return e->tex;
            old = e;
            goto decode;
        }
    }
    for (int i = 0; i < NTC; i++) {
        tcache *e = &tc[i];
        if (!e->used) { if (!free_e) free_e = e; continue; }
        if (TC_KEY_EQ(e, s)) {
            e->stamp = ++tc_clock;
            tc_hint[hk] = (u16)i;
            if (e->gen == g) return e->tex;
            old = e; free_e = 0;   /* same texture, stale: decode into it */
            goto decode;
        }
        if (e->stamp < old->stamp) old = e;
    }
    if (free_e) old = free_e;
    tc_hint[hk] = (u16)(old - tc);
decode:;
    tcache *e = old;
    u64 vh = vram_hash(b0, b1);
    if (vh && e->used && e->vhash == vh && TC_KEY_EQ(e, s)) {   /* uploaded again, same bytes */
        e->gen = g; n_same_content++;
        return e->tex;
    }
    e->vhash = vh;
    u32 w = 1u << s->tw, h = 1u << s->th;
    if (!e->used || e->tw != s->tw || e->th != s->th) {
        if (e->used) glDeleteTextures(1, &e->tex);
        e->tex = new_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    }
    e->used = 1; e->tbp = s->tbp; e->tbw = s->tbw; e->tpsm = s->tpsm; e->tw = s->tw; e->th = s->th;
    e->cbp = s->cbp; e->cpsm = s->cpsm; e->csa = s->csa; e->clut_hash = s->clut_hash;
    e->ta0 = s->ta0; e->ta1 = s->ta1; e->aem = s->aem; e->gen = g; e->stamp = ++tc_clock;
    u32 *pix = malloc((size_t)w * h * 4);
    gs_decode_texture(s, pix);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
    free(pix);
    n_decodes++;
    return e->tex;
}

/* ---- batching ------------------------------------------------------------------ */
#define MAXV 65536
static gs_vertex vb[MAXV];
static u32 nv;
static gs_draw_state cur;
static int have;
static GLuint fb_tex; static int fb_w, fb_h;   /* copy of a target that is drawn and sampled at once */
static u64 n_batches, n_draw_verts, n_culled, n_mip_batches, n_mip_skip0;
/* 1x1 black: level 0 of a mipmapped batch that never samples it */
static GLuint dummy_tex(void) {
    static GLuint t;
    if (!t) { u32 z = 0; t = new_tex(1, 1, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); glBindTexture(GL_TEXTURE_2D, t); glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &z); }
    return t;
}

/* Uniforms of the draw program: set only when the value changes (the GL
 * driver was most of the frame time with ~25 glUniform calls per batch). */
static u32 ucache[128][4]; static u8 uvalid[128];
static int uchg(GLint loc, u32 a, u32 b, u32 c, u32 d) {
    if (loc < 0) return 0;
    if (loc >= 128) return 1;
    u32 *e = ucache[loc];
    if (uvalid[loc] && e[0] == a && e[1] == b && e[2] == c && e[3] == d) return 0;
    e[0] = a; e[1] = b; e[2] = c; e[3] = d; uvalid[loc] = 1;
    return 1;
}
static inline u32 fb(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static void U1i(GLint l, GLint a) { if (uchg(l, (u32)a, 0, 0, 0)) glUniform1i(l, a); }
static void U2i(GLint l, GLint a, GLint b) { if (uchg(l, (u32)a, (u32)b, 0, 0)) glUniform2i(l, a, b); }
static void U4i(GLint l, GLint a, GLint b, GLint c, GLint d) { if (uchg(l, (u32)a, (u32)b, (u32)c, (u32)d)) glUniform4i(l, a, b, c, d); }
static void U1f(GLint l, float a) { if (uchg(l, fb(a), 0, 0, 0)) glUniform1f(l, a); }
static void U2f(GLint l, float a, float b) { if (uchg(l, fb(a), fb(b), 0, 0)) glUniform2f(l, a, b); }
static void U3f(GLint l, float a, float b, float c) { if (uchg(l, fb(a), fb(b), fb(c), 0)) glUniform3f(l, a, b, c); }
static void U4f(GLint l, float a, float b, float c, float d) { if (uchg(l, fb(a), fb(b), fb(c), fb(d))) glUniform4f(l, a, b, c, d); }

/* Vertex ring: batches are appended to one big buffer, orphaned when full,
 * instead of a glBufferData per batch. Returns the first vertex index. */
#define VBO_BYTES (16u << 20)
static u32 vbo_off = VBO_BYTES;   /* first put allocates */
static GLint vbo_put(const gs_vertex *v, u32 n) {
    u32 bytes = n * (u32)sizeof(gs_vertex);
    if (bytes > VBO_BYTES) bytes = VBO_BYTES / sizeof(gs_vertex) * sizeof(gs_vertex);
    if (vbo_off + bytes > VBO_BYTES) { glBufferData(GL_ARRAY_BUFFER, VBO_BYTES, NULL, GL_STREAM_DRAW); vbo_off = 0; }
    glBufferSubData(GL_ARRAY_BUFFER, vbo_off, bytes, v);
    GLint first = (GLint)(vbo_off / sizeof(gs_vertex));
    vbo_off += bytes;
    return first;
}

void gsgl_draw(const gs_draw_state *st, const gs_vertex *v, u32 n, int same) {
    /* same: cur already holds st (only gsgl_draw sets cur, from the state passed) */
    if (have && (nv + n > MAXV || (!same && memcmp(st, &cur, sizeof cur)))) gsgl_flush();
    if (!have) { cur = *st; have = 1; }
    memcpy(vb + nv, v, n * sizeof *v);
    nv += n;
}

/* COLCLAMP = 0 (colour wrap): the blend result is kept mod 256 instead of being
 * clamped. The game's shadow volumes need it: into a 16-bit buffer filled with
 * 0x78 it adds 0x08 for front faces and 0xf8 (-8 mod 256) for back faces, then
 * keeps bit 7 by writing 0 with FBMSK 0x80808080. Partial FBMSK bits (not a
 * whole channel) need the same treatment: such a batch is drawn into a float
 * copy of the target (no clamp), then a resolve pass writes back
 * (new mod 256 or clamped) & ~FBMSK | old & FBMSK. */
static GLuint wrap_tex, wrap_fbo, old_tex, wprog; static int wrap_w, wrap_h, old_w, old_h;
static GLint wu_dst, wu_src, wu_tex, wu_old, wu_wrap, wu_mask; static u64 n_wrap, n_bitmask;
/* Present with an area filter: each window pixel averages the source texels it
 * covers (up to 4x4 bilinear taps = 8x8 texels), so a picture rendered at
 * scale 2-4 is properly supersampled instead of reduced by one bilinear tap.
 * When the picture is enlarged it is one bilinear tap, as before. */
static GLuint pprog; static GLint pu_dst, pu_src, pu_tex;
static const char *pfs_src =
    "#version 330 core\n"
    "uniform sampler2D u_tex; in vec2 v_uv; out vec4 o;\n"
    "void main() {\n"
    "  vec2 ts = vec2(textureSize(u_tex, 0));\n"
    "  vec2 fp = max(abs(vec2(dFdx(v_uv.x), dFdy(v_uv.y))), abs(vec2(dFdy(v_uv.x), dFdx(v_uv.y)))) * ts;\n"
    "  ivec2 n = ivec2(clamp(ceil(fp / 2.0), vec2(1.0), vec2(4.0)));\n"
    "  vec2 st = fp / ts / vec2(n);\n"
    "  vec2 base = v_uv - 0.5 * fp / ts + 0.5 * st;\n"
    "  if (n == ivec2(1)) base = v_uv;\n"
    "  vec3 acc = vec3(0.0);\n"
    "  for (int y = 0; y < n.y; y++) for (int x = 0; x < n.x; x++) acc += texture(u_tex, base + st * vec2(x, y)).rgb;\n"
    "  o = vec4(acc / float(n.x * n.y), 1.0);\n"
    "}\n";
static const char *wfs_src =
    "#version 330 core\n"
    "uniform sampler2D u_tex, u_old; uniform int u_wrap; uniform uvec4 u_mask; in vec2 v_uv; out vec4 o;\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 v = floor(texelFetch(u_tex, p, 0) * 255.0 + 0.5);\n"
    "  v = u_wrap == 1 ? mod(v, 256.0) : clamp(v, 0.0, 255.0);\n"
    "  uvec4 n = uvec4(v), d = uvec4(floor(texelFetch(u_old, p, 0) * 255.0 + 0.5));\n"
    "  o = vec4((n & ~u_mask) | (d & u_mask)) / 255.0;\n"
    "}\n";
/* rect = scissor in target pixels (scaled): x, y, w, h */
/* The scratch textures only grow: a larger attachment than the target is fine
 * (same origin, the viewport and scissor stay the target's). */
static void wrap_begin(target *t, GLuint ztex, const GLboolean *cm, int need_old, const int *rect, GLuint batch_tex) {
    int w = t->w * S, h = TH * S;
    if (!wrap_fbo) glGenFramebuffers(1, &wrap_fbo);
    if (!wrap_tex || w > wrap_w || h > wrap_h) {
        if (wrap_tex) glDeleteTextures(1, &wrap_tex);
        if (w < wrap_w) w = wrap_w;
        if (h < wrap_h) h = wrap_h;
        wrap_tex = new_tex(w, h, GL_RGBA32F, GL_RGBA, GL_FLOAT); wrap_w = w; wrap_h = h;
        glBindFramebuffer(GL_FRAMEBUFFER, wrap_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, wrap_tex, 0);
    }
    if (need_old) {
        if (!old_tex || w > old_w || h > old_h) {
            int ow = w > old_w ? w : old_w, oh = h > old_h ? h : old_h;
            if (old_tex) glDeleteTextures(1, &old_tex);
            old_tex = new_tex(ow, oh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); old_w = ow; old_h = oh;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
        glBindTexture(GL_TEXTURE_2D, old_tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, rect[0], rect[1], rect[0], rect[1], rect[2], rect[3]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, wrap_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ztex, 0);
    glColorMask(1, 1, 1, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, wrap_fbo);
    glBlitFramebuffer(rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3], rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3],
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, wrap_fbo);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    /* new_tex and the old copy rebound unit 0: give the batch its texture back */
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, batch_tex);
}
static void wrap_end(target *t, const GLboolean *cm, int wrap, u32 mask, int need_old) {
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    glUseProgram(wprog);
    glUniform4f(wu_dst, -1, -1, 1, 1); glUniform4f(wu_src, 0, 0, 1, 1);
    glUniform1i(wu_wrap, wrap);
    glUniform4ui(wu_mask, need_old ? mask & 0xff : 0, need_old ? (mask >> 8) & 0xff : 0, need_old ? (mask >> 16) & 0xff : 0, need_old ? mask >> 24 : 0);
    glActiveTexture(GL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, need_old ? old_tex : wrap_tex);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, wrap_tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static GLenum zfunc(u32 t) { static const GLenum f[4] = { GL_NEVER, GL_ALWAYS, GL_GEQUAL, GL_GREATER }; return f[t & 3]; }

static u64 ft_tex, ft_state, ft_draw, n_selfcopy, ft_present, ft_from_cyc; static u64 ft_from = ~0ull;
void gsgl_flush(void) {
    if (!have) return;
    have = 0;
    u32 n = nv; nv = 0;
    if (!n) return;
    gs_draw_state st = cur;
    int srect[4] = { 0, 0, 0, 0 };
    if (g_log_gs > 1)
        LOG("gs: batch %u verts prim %u fb %x/%u/%x msk %x tme %u tex %x/%u/%x %ux%u fst %u tfx %u abe %u(%u%u%u%u) z %u/%u/%u sc %u,%u-%u,%u v0 %.1f,%.1f c %02x%02x%02x%02x st %.3f,%.3f,%.3f\n",
            n, st.prim, st.fbp, st.fbw, st.fpsm, st.fbmsk, st.tme, st.tbp, st.tbw, st.tpsm, 1u << st.tw, 1u << st.th, st.fst, st.tfx,
            st.abe, st.alpha_a, st.alpha_b, st.alpha_c, st.alpha_d, st.zbp, st.ztst, st.zmsk, st.scx0, st.scy0, st.scx1, st.scy1,
            vb[0].x, vb[0].y, vb[0].r, vb[0].g, vb[0].b, vb[0].a, vb[0].s, vb[0].t, vb[0].q);
    if (g_log_gs > 1 && st.tme && st.mxl)
        LOG("gs:   mip mxl %u mmin %u mmag %u lcm %u l %u k %03x  levels %x/%u %x/%u %x/%u %x/%u\n", st.mxl, st.mmin, st.mmag, st.lcm, st.mipl, st.mipk,
            st.miptbp[0], st.miptbw[0], st.miptbp[1], st.miptbw[1], st.miptbp[2], st.miptbw[2], st.miptbp[3], st.miptbw[3]);
    if (g_log_gs > 2) for (u32 i = 0; i < n; i++)
        LOG("   v%u %.2f,%.2f z %.6g c %02x%02x%02x%02x stq %g,%g,%g\n", i, vb[i].x, vb[i].y, vb[i].z, vb[i].r, vb[i].g, vb[i].b, vb[i].a, vb[i].s, vb[i].t, vb[i].q);
    if (!is_color_psm(st.fpsm)) {
        static int once; if (!once++) LOG("gs: draw to frame psm %x not supported\n", st.fpsm);
        return;
    }
    /* bounding box: a batch wholly outside the scissor draws nothing, so it is
     * dropped before its texture is looked up (no decode, no write-back) */
    float xmin = vb[0].x, xmax = vb[0].x, ymin = vb[0].y, ymax = vb[0].y;
    for (u32 i = 1; i < n; i++) {
        float x = vb[i].x, y = vb[i].y;
        if (x < xmin) xmin = x; if (x > xmax) xmax = x;
        if (y < ymin) ymin = y; if (y > ymax) ymax = y;
    }
    if (xmax < (float)st.scx0 - 1 || xmin > (float)st.scx1 + 1 || ymax < (float)st.scy0 - 1 || ymin > (float)st.scy1 + 1) {
        n_culled++;
        return;
    }
    if (ymax < 0) ymax = 0;
    u32 rows = (u32)ymax + 1;
    if (rows > st.scy1 + 1) rows = st.scy1 + 1;
    if (rows > TH) rows = TH;

    u32 yoff = 0;
    target *t = find_target(st.fbp, st.fbw, st.fpsm, &yoff);
    if (!t) t = target_create(st.fbp, st.fbw, st.fpsm, rows);
    else if (yoff + rows > t->hmax) target_grow(t, yoff + rows);

    u64 tc0 = __rdtsc();
    /* texture */
    GLuint tex = 0; float tmx = 1, tmy = 1, tox = 0, toy = 0; int rt24 = 0;
    /* Mipmapping (TEX1 MXL > 0, MMIN 2..5). The game uploads only the levels
     * from the needed level of detail down (vif1_intc_handler 0x107668), so a
     * distant object's TEX0 base (level 0) is often TBP 0. If no vertex can
     * reach level 0 (LOD >= 1 everywhere: Q is linear in screen space, so the
     * largest |Q| is at a vertex), level 0 is not decoded at all. */
    GLuint miptex[7] = { 0 };
    u32 mxl_use = st.tme && st.mxl && st.mmin >= 2 ? st.mxl : 0;
    float mipk = (float)((st.mipk & 0x800) ? (int)st.mipk - 0x1000 : (int)st.mipk) / 16.0f;
    int skip0 = 0;
    if (mxl_use) {
        float lodmin;
        if (st.lcm || st.fst) lodmin = mipk;
        else {
            float qmax = 0;
            for (u32 i = 0; i < n; i++) { float a = fabsf(vb[i].q); if (a > qmax) qmax = a; }
            lodmin = qmax > 0 ? log2f(1.0f / qmax) * (float)(1u << st.mipl) + mipk : 1e9f;
        }
        skip0 = lodmin >= 1.0f;
        if (skip0) n_mip_skip0++;
        n_mip_batches++;
    }
    if (st.tme) {
        u32 tyoff = 0;
        target *src = is_color_psm(st.tpsm) ? find_target(st.tbp, st.tbw, st.tpsm, &tyoff) : 0;
        if (src) {
            tex = src->tex; tmx = tmy = (float)S; toy = (float)(tyoff * S);
            rt24 = st.tpsm == PSMCT24;
            if (src == t) {   /* sampling the target being drawn: work from a copy */
                int w = t->w * S, h = TH * S;
                if (!fb_tex || fb_w != w || fb_h != h) {
                    if (fb_tex) glDeleteTextures(1, &fb_tex);
                    fb_tex = new_tex(w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); fb_w = w; fb_h = h;
                }
                glBindFramebuffer(GL_READ_FRAMEBUFFER, t->fbo);
                glBindTexture(GL_TEXTURE_2D, fb_tex);
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, t->hmax * S); n_selfcopy++;
                tex = fb_tex;
            }
        } else tex = skip0 ? dummy_tex() : texture_from_memory(&st);
        /* GS mipmapping: levels 1..MXL from MIPTBP1/2, halving in size */
        for (u32 k = 1; k <= mxl_use; k++) {
            gs_draw_state m = st;
            m.tbp = st.miptbp[k - 1]; m.tbw = st.miptbw[k - 1];
            m.tw = st.tw > k ? st.tw - k : 0; m.th = st.th > k ? st.th - k : 0;
            miptex[k] = texture_from_memory(&m);
        }
    }

    ft_tex += __rdtsc() - tc0; tc0 = __rdtsc();
    /* target + depth */
    use_fbo(t->fbo, t->w * S, TH * S);
    int need_z = (st.ztst != 1) || !st.zmsk;
    if (need_z) {
        GLuint z = depth_for(t, st.zbp);
        if (t->ztex != z || t->zbp != st.zbp) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, z, 0);
            t->ztex = z; t->zbp = st.zbp;
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(zfunc(st.ztst));
        glDepthMask(!st.zmsk);
    } else glDisable(GL_DEPTH_TEST);

    glEnable(GL_SCISSOR_TEST);
    {
        int x0 = (int)st.scx0, x1 = (int)st.scx1 + 1, y0 = (int)st.scy0 + (int)yoff, y1 = (int)st.scy1 + 1 + (int)yoff;
        if (x1 > (int)t->w) x1 = t->w;
        if (y1 > TH) y1 = TH;
        if (x1 < x0) x1 = x0;
        if (y1 < y0) y1 = y0;
        glScissor(x0 * S, y0 * S, (x1 - x0) * S, (y1 - y0) * S);
        srect[0] = x0 * S; srect[1] = y0 * S; srect[2] = (x1 - x0) * S; srect[3] = (y1 - y0) * S;
    }
    u32 m = st.fbmsk;
    int is16 = st.fpsm == PSMCT16 || st.fpsm == PSMCT16S;
    u32 full = is16 ? 0xf8 : 0xff;        /* a 16-bit pixel keeps bits 3-7 of each channel */
    int ma = (m >> 24) != 0xff;
    if (st.fpsm == PSMCT24) ma = 0;
    if (is16) ma = !(m & 0x80000000u);
    GLboolean cm[4] = { (m & full) != full, ((m >> 8) & full) != full, ((m >> 16) & full) != full, (GLboolean)ma };
    glColorMask(cm[0], cm[1], cm[2], cm[3]);
    /* FBMSK bits inside a written channel: merge with the old pixel in the resolve pass */
    u32 bitmask = 0;
    for (int c = 0; c < 3; c++) { u32 b = (m >> (8 * c)) & full; if (cm[c] && b) bitmask |= b << (8 * c); }
    if (!is16 && st.fpsm != PSMCT24 && cm[3] && (m >> 24)) bitmask |= m & 0xff000000u;

    glUseProgram(prog);
    U2f(U.rtsize, (float)t->w, (float)TH);
    U1f(U.half, 0.5f / S);
    U1f(U.yoff, (float)yoff);
    U1i(U.tme, st.tme);
    U1i(U.mxl, (int)mxl_use);
    if (mxl_use) {
        U1i(U.mmin, (int)st.mmin); U1i(U.lcm, (int)st.lcm); U1i(U.mipl, (int)st.mipl); U1f(U.mipk, mipk);
        for (u32 k = 1; k <= mxl_use; k++) { glActiveTexture(GL_TEXTURE0 + k); glBindTexture(GL_TEXTURE_2D, miptex[k]); }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    if (st.tme) {
        U1i(U.fst, st.fst); U1i(U.tfx, st.tfx); U1i(U.tcc, st.tcc);
        int lin = st.mmag;   /* magnification filter stands in for both */
        U1i(U.linear, lin);
        U1i(U.rt24, rt24); U1i(U.aem, st.aem); U1f(U.ta0, (float)st.ta0);
        U2f(U.tsize, (float)(1u << st.tw), (float)(1u << st.th));
        U4f(U.tmap, tmx, tmy, tox, toy);
        U2i(U.wrap, st.wms, st.wmt);
        U4i(U.region, st.minu, st.maxu, st.minv, st.maxv);
    }
    U1i(U.atst, st.atst); U1i(U.aref, st.aref);
    U1i(U.fge, st.fge);
    U3f(U.fogcol, (float)(st.fogcol & 0xff), (float)((st.fogcol >> 8) & 0xff), (float)((st.fogcol >> 16) & 0xff));
    U1i(U.fba, st.fba);
    if (st.abe) {
        int as = st.alpha_a == 0, bs = st.alpha_b == 0, ds = st.alpha_d == 0;
        int ad = st.alpha_a == 1, bd = st.alpha_b == 1, dd = st.alpha_d == 1;
        int neg_s = (as - bs) < 0 && !ds, neg_d = (ad - bd) < 0 && !dd;
        U1i(U.abe, 1);
        U1i(U.csel, st.alpha_c);
        U1i(U.pabe, st.pabe);
        U4i(U.abd, as - bs, ds, ad - bd, dd);
        U2i(U.sgn, neg_s, neg_d);
        U1f(U.fix, (float)st.alpha_fix);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(neg_s ? GL_FUNC_REVERSE_SUBTRACT : neg_d ? GL_FUNC_SUBTRACT : GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_SRC1_COLOR, GL_ONE, GL_ZERO);
        if (st.alpha_c == 1) { static int once; if (!once++) LOG("gs: blend with destination alpha (approximated as 0x80)\n"); }
    } else {
        U1i(U.abe, 0);
        glDisable(GL_BLEND);
    }
    if (st.date) { static int once; if (!once++) LOG("gs: destination alpha test not supported\n"); }

    static int no_wrap = -1; if (no_wrap < 0) no_wrap = getenv("GS_NO_WRAP") != 0;   /* GS_NO_WRAP=1: clamp as before (comparison) */
    int wrap = st.abe && !st.colclamp && !no_wrap;
    int merge = wprog && (wrap || bitmask);
    if (merge) {
        wrap_begin(t, need_z ? t->ztex : 0, cm, bitmask != 0, srect, tex);
        if (wrap && !n_wrap++) LOG("gs: colour wrap (COLCLAMP 0) drawing at frame %llu\n", (unsigned long long)g_frame);
        if (bitmask && !n_bitmask++) LOG("gs: FBMSK %08x (bits inside channels) at frame %llu\n", st.fbmsk, (unsigned long long)g_frame);
    }

    ft_state += __rdtsc() - tc0; tc0 = __rdtsc();
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    GLint first = vbo_put(vb, n);
    GLenum mode = st.prim == 1 ? GL_LINES : GL_TRIANGLES;

    /* alpha test: pass 1 draws what passes; pass 2 what fails, with AFAIL's masks */
    int two = st.atst != 1 && st.afail != 0;
    if (st.atst != 0 || !two) {
        U1i(U.afail_pass, 0);
        glDrawArrays(mode, first, n);
    }
    if (two) {
        U1i(U.afail_pass, 1);
        switch (st.afail) {
        case 1: glColorMask(cm[0], cm[1], cm[2], cm[3]); glDepthMask(0); break;   /* FB_ONLY */
        case 2: glColorMask(0, 0, 0, 0); break;                                   /* ZB_ONLY */
        case 3: glColorMask(cm[0], cm[1], cm[2], 0); glDepthMask(0); break;       /* RGB_ONLY */
        }
        glDrawArrays(mode, first, n);
    }
    if (merge) wrap_end(t, cm, wrap, bitmask, bitmask != 0);
    t->dirty = 1; t->clean = 0;
    ft_draw += __rdtsc() - tc0;
    n_batches++; n_draw_verts += n;
}

/* ---- coherence with local memory -------------------------------------------- */
void gsgl_sync_vram(u32 b0, u32 b1) {
    gsgl_flush();   /* always: skipping it when no target overlaps changed the picture (2026-11-22) */
    for (int i = 0; i < NT; i++)
        if (tg[i].used && tg[i].dirty && overlaps(&tg[i], b0, b1)) {
            u32 r0, r1; rows_for_blocks(&tg[i], b0, b1, &r0, &r1);
            target_download_rows(&tg[i], r0, r1);
        }
}

/* Texture decode from memory a target covers. Exact would be a write-back
 * every time the target was drawn to since. Woodward Camp textures meshes
 * from TBP 0 (inside frame buffer 0) while drawing into that same buffer, ~20
 * times a frame, each a GPU stall. Those texels are whatever is being drawn,
 * so a write-back once per frame per target is used instead; GS_EXACT_SYNC=1
 * restores the exact behaviour. Not checked against hardware. */
static u64 n_sync_skipped, n_async_reads;
/* Asynchronous variant of the above (default): when present() ends a frame,
 * the page rows textures read from dirty targets during that frame are read
 * back into a pixel buffer object without waiting; the next frame's first
 * texture read from that target copies them into local memory. A synchronous
 * glReadPixels here made the CPU wait for the GPU about once a frame, which
 * was the in-game stutter (490 of 8340 frames over 17 ms -> 16). */
#define CAP_EVERY 2   /* capture every 2nd frame into one of two buffers, use it 2 frames later */
static void async_issue(target *t) {
    if (!t->want_rows || !t->dirty || !t->hmax) { t->want_rows = 0; return; }
    if (g_frame % CAP_EVERY) return;                 /* keep collecting rows until the next capture frame */
    int k = (int)((g_frame / CAP_EVERY) & 1);
    if (t->cap[k].pending) return;                   /* not consumed yet: skip this capture */
    u32 ph = page_h(t->psm), lo = 31, hi = 0;
    for (u32 r = 0; r < 32; r++) if (t->want_rows & (1u << r)) { if (r < lo) lo = r; if (r > hi) hi = r; }
    u32 y0 = lo * ph, y1 = (hi + 1) * ph;
    if (y1 > t->hmax) y1 = t->hmax;
    if (y1 <= y0) { t->want_rows = 0; return; }
    u32 bytes = t->w * (y1 - y0) * 4;
    if (!t->cap[k].pbo) glGenBuffers(1, &t->cap[k].pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, t->cap[k].pbo);
    if (t->cap[k].bytes < bytes) { glBufferData(GL_PIXEL_PACK_BUFFER, bytes, NULL, GL_STREAM_READ); t->cap[k].bytes = bytes; }
    target_read(t, 0, y0, t->w, y1 - y0, (u32 *)0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    t->cap[k].rows = t->want_rows; t->cap[k].y0 = y0; t->cap[k].h = y1 - y0; t->cap[k].pending = 1; t->cap[k].frame = g_frame;
    t->want_rows = 0;
    n_async_reads++;
}
/* copy the newest capture that is at least CAP_EVERY frames old into local memory */
static void async_consume(target *t) {
    int k = -1;
    for (int j = 0; j < 2; j++)
        if (t->cap[j].pending && g_frame >= t->cap[j].frame + CAP_EVERY && (k < 0 || t->cap[j].frame > t->cap[k].frame)) k = j;
    if (k < 0) return;
    for (int j = 0; j < 2; j++) if (t->cap[j].pending && t->cap[j].frame <= t->cap[k].frame) t->cap[j].pending = 0;   /* older ones are superseded */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, t->cap[k].pbo);
    const u32 *pix = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
    if (pix) {
        u32 ph = page_h(t->psm);
        int s16 = t->psm == PSMCT16 || t->psm == PSMCT16S;
        for (u32 y = 0; y < t->cap[k].h; y++) {
            u32 gy = t->cap[k].y0 + y, r = gy / ph;
            if (!(t->cap[k].rows & (1u << r)) || (t->clean & (1u << r))) continue;   /* not asked for, or memory already exact */
            const u32 *row = pix + (size_t)y * t->w;
            if (!s16) gs_write_row32(t->psm, t->fbp, t->fbw, gy, t->w, row);
            else for (u32 x = 0; x < t->w; x++) {
                u32 c = row[x];
                gs_write_pixel(t->psm, t->fbp, t->fbw, x, gy,
                               ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10) | ((c >> 31) << 15));
            }
        }
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        u32 b0, b1; gs_rect_blocks(t->psm, t->fbp, t->fbw, 0, t->cap[k].y0, t->w, t->cap[k].h, &b0, &b1); bump_blocks(b0, b1);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}
static void async_issue_all(void) {
    for (int i = 0; i < NT; i++) if (tg[i].used) async_issue(&tg[i]);
}

static void sync_for_texture(u32 b0, u32 b1) {
    /* 1 exact (default since mipmapping: the TBP 0 reads were level 0 of
     * mipmapped textures that are never sampled, now not decoded at all);
     * 0 GS_ASYNC_TEX_SYNC=1: write-back 2-4 frames late via PBO;
     * 2 GS_SYNC_PER_FRAME=1: once per frame, synchronous */
    static int mode = -1;
    if (mode < 0) mode = getenv("GS_ASYNC_TEX_SYNC") ? 0 : getenv("GS_SYNC_PER_FRAME") ? 2 : 1;
    gsgl_flush();
    for (int i = 0; i < NT; i++) {
        target *t = &tg[i];
        if (!t->used || !t->dirty || !overlaps(t, b0, b1)) continue;
        u32 r0, r1; rows_for_blocks(t, b0, b1, &r0, &r1);
        if (mode == 1) { target_download_rows(t, r0, r1); continue; }
        if (t->synced_frame != g_frame + 1) { t->synced_frame = g_frame + 1; t->synced_rows = 0; if (mode == 0) async_consume(t); }
        if (mode == 0) {
            for (u32 r = r0; r < r1 && r < 32; r++) t->want_rows |= 1u << r;
            n_sync_skipped++;
            continue;
        }
        for (u32 r = r0; r < r1 && r < 32; r++) {
            if (t->synced_rows & (1u << r)) { if (!(t->clean & (1u << r))) n_sync_skipped++; continue; }
            target_download_rows(t, r, r + 1);
            t->synced_rows |= 1u << r;
        }
    }
}

void gsgl_vram_written(u32 psm, u32 bp, u32 bw, u32 x, u32 y, u32 w, u32 h) {
    gsgl_flush();
    u32 b0, b1; gs_rect_blocks(psm, bp, bw, x, y, w, h, &b0, &b1);
    bump_blocks(b0, b1);
    for (int i = 0; i < NT; i++) {
        target *t = &tg[i];
        if (!t->used || !overlaps(t, b0, b1)) continue;
        if (t->fbw == bw && compat(t->psm, psm)) {
            if (bp == t->fbp) target_upload(t, x, y, w, h);
            else {
                u32 row = 32 * ppr_of(bw), ph = page_h(t->psm);
                u32 r0 = b0 > t->fbp ? (b0 - t->fbp) / row * ph : 0, r1 = (b1 - t->fbp) / row * ph + ph;
                if (r1 > t->hmax) r1 = t->hmax;
                if (r1 > r0) target_upload(t, 0, r0, t->w, r1 - r0);
            }
        } else {
            /* rows under the write were synced before it (clean); write back the rest */
            target_download(t);
            target_free(t);
        }
    }
}

/* ---- presentation ------------------------------------------------------------- */
static const char *shot_dir; static int shot_every = 30; static u64 shot_frame, shot_from, shot_to = ~0ull;
static void write_shot(const u32 *pix, u32 w, u32 h) {
    char name[512];
    snprintf(name, sizeof name, "%s/gs_%05llu_%ux%u.ppm", shot_dir, (unsigned long long)shot_frame, w, h);
    FILE *f = fopen(name, "wb");
    if (!f) { static int once; if (!once++) LOG("gs: cannot write screenshot %s\n", name); return; }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (u32 i = 0; i < w * h; i++) { u8 c[3] = { (u8)pix[i], (u8)(pix[i] >> 8), (u8)(pix[i] >> 16) }; fwrite(c, 1, 3, f); }
    fclose(f);
}

/* GS_FRAME_TIMES=1 (debug): per presented frame, the GPU time (timer queries,
 * read 4 frames later), the graphics thread's busy time between swaps (minus
 * its idle sleeps) and the swap wait; counts and worst values at exit, frames
 * over 16 ms logged with --log-frames. */
#define GL_TIME_ELAPSED 0x88BF
#define GL_QUERY_RESULT 0x8866
static int ftimes;
static GLuint fq_ids[4]; static int fq_n, fq_open;
static u64 ft_last_swap_end, ft_idle_at_swap;
static u64 ft_n, ft_nb, ft_gpu12, ft_gpu16, ft_busy12, ft_busy16, ft_swap8;
static double ft_gpu_max, ft_busy_max, ft_swap_max, ft_gpu_sum, ft_busy_sum;
static double qpc_ms(u64 t) { static double f; if (!f) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (double)q.QuadPart; } return (double)t * 1000.0 / f; }
static void ftimes_before_swap(u64 now) {
    extern u64 g_gst_idle_ticks; extern int g_log_frames;
    if (fq_open) { glEndQuery(GL_TIME_ELAPSED); fq_open = 0; }
    if (!ft_last_swap_end || g_frame < 600) return;
    double busy = qpc_ms(now - ft_last_swap_end - (g_gst_idle_ticks - ft_idle_at_swap));
    ft_nb++; ft_busy_sum += busy;
    if (busy > 12) ft_busy12++; if (busy > 16) ft_busy16++; if (busy > ft_busy_max) ft_busy_max = busy;
    if (g_log_frames && busy > 16) LOG("frames: graphics thread busy %.1f ms at frame %llu\n", busy, (unsigned long long)g_frame);
}
static u64 ft_cyc0, ft_cyc1, ft_cyc_frames, ft_vcyc0, ft_vcyc1, ft_wall0, ft_wall1;   /* graphics thread CPU cycles (QueryThreadCycleTime) from frame 6500 */
static void ftimes_after_swap(u64 t0, u64 t1) {
    if (g_frame >= 6500) {
        extern HANDLE g_gst_vu_thread;   /* split graphics threads: the VU stage's cycles too */
        ULONG64 c, cv = 0; QueryThreadCycleTime(GetCurrentThread(), &c);
        if (g_gst_vu_thread) QueryThreadCycleTime(g_gst_vu_thread, &cv);
        if (!ft_cyc0) { ft_cyc0 = c; ft_vcyc0 = cv; ft_wall0 = t1; } else { ft_cyc1 = c; ft_vcyc1 = cv; ft_wall1 = t1; ft_cyc_frames++; }
    }
    extern u64 g_gst_idle_ticks; extern int g_log_frames;
    if (!fq_ids[0]) glGenQueries(4, fq_ids);
    double sw = qpc_ms(t1 - t0);
    if (g_frame >= 600) { if (sw > 8) ft_swap8++; if (sw > ft_swap_max) ft_swap_max = sw; }
    if (fq_n >= 4) {   /* the query issued 4 frames ago */
        unsigned long long ns = 0; glGetQueryObjectui64v(fq_ids[fq_n & 3], GL_QUERY_RESULT, &ns);
        double g = (double)ns / 1e6;
        if (g_frame >= 600) {
            ft_n++; ft_gpu_sum += g;
            if (g > 12) ft_gpu12++; if (g > 16) ft_gpu16++; if (g > ft_gpu_max) ft_gpu_max = g;
            if (g_log_frames && g > 16) LOG("frames: GPU %.1f ms for a frame before %llu\n", g, (unsigned long long)g_frame);
        }
    }
    glBeginQuery(GL_TIME_ELAPSED, fq_ids[fq_n & 3]); fq_open = 1; fq_n++;
    ft_last_swap_end = t1; ft_idle_at_swap = g_gst_idle_ticks;
}
static void ftimes_report(void) {
    if (!ft_n || !ft_nb) return;
    {   extern u64 g_ft_kind[8]; static const char *nm[6] = { "vif1+vu1", "gif", "vu1 start", "present", "flush", "other" };
        if (ft_cyc_frames) LOG("gs: graphics thread %.2f Mcycles per frame from frame 6500\n", (double)(ft_cyc1 - ft_cyc0) / ft_cyc_frames / 1e6);
        if (ft_cyc_frames) LOG("gs: %.3f ms wall time per presented frame from frame 6500 (with --turbo: the pipeline's speed)\n", qpc_ms(ft_wall1 - ft_wall0) / ft_cyc_frames);
        if (ft_cyc_frames && ft_vcyc1) LOG("gs: VU stage %.2f Mcycles per frame from frame 6500 (the line above is the GS stage)\n", (double)(ft_vcyc1 - ft_vcyc0) / ft_cyc_frames / 1e6);
        LOG("gs: graphics thread ms per frame (whole run):");
        for (int k = 0; k < 6; k++) LOG(" %s %.2f", nm[k], qpc_ms(g_ft_kind[k]) / (double)fq_n);
        LOG("\n"); }
    LOG("gs: frame times (from frame 600): GPU avg %.1f ms, %llu over 12, %llu over 16, worst %.1f; "
        "graphics thread busy avg %.1f ms, %llu over 12, %llu over 16, worst %.1f; swap waits %llu over 8 ms, worst %.1f\n",
        ft_gpu_sum / ft_n, (unsigned long long)ft_gpu12, (unsigned long long)ft_gpu16, ft_gpu_max,
        ft_busy_sum / ft_nb, (unsigned long long)ft_busy12, (unsigned long long)ft_busy16, ft_busy_max,
        (unsigned long long)ft_swap8, ft_swap_max);
}

static void present_rect_(u32 fbp, u32 fbw, u32 psm, u32 dbx, u32 dby, u32 w, u32 h);
void gsgl_present_rect(u32 fbp, u32 fbw, u32 psm, u32 dbx, u32 dby, u32 w, u32 h) {
    if (ft_from == ~0ull) ft_from = getenv("FT_FROM") ? strtoull(getenv("FT_FROM"), 0, 10) : 0;
    if (g_frame == ft_from) { ft_tex = ft_state = ft_draw = ft_present = n_selfcopy = 0; ft_from_cyc = __rdtsc(); }
    u64 c = __rdtsc(); present_rect_(fbp, fbw, psm, dbx, dby, w, h); ft_present += __rdtsc() - c;
}
/* a new internal resolution (in-game menu): every target gives its drawing back
 * to GS memory and goes, depth buffers too; they come back at the new scale
 * from memory as the game draws (one frame at PS2 detail) */
static void apply_scale(int s) {
    gsgl_flush();
    for (int i = 0; i < NT; i++) if (tg[i].used) { target_download(&tg[i]); target_free(&tg[i]); }
    for (int i = 0; i < 16; i++) if (zt[i].used) { glDeleteTextures(1, &zt[i].tex); memset(&zt[i], 0, sizeof zt[i]); }
    LOG("gs: 3D resolution %dx -> %dx\n", S, s);
    S = s;
}

/* the in-game menu over the picture (menu.c draws the bitmap) */
void menu_size(int *w, int *h); void menu_render(u32 *pix); int menu_version(void);
static void menu_overlay(int cw, int ch) {
    static GLuint mt; static int mver = -1, mw, mh;
    if (cw <= 0 || ch <= 0) return;
    if (!mt) { menu_size(&mw, &mh); mt = new_tex(mw, mh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE); }
    if (menu_version() != mver) {
        mver = menu_version();
        u32 *pix = malloc((size_t)mw * mh * 4);
        if (!pix) return;
        menu_render(pix);
        glBindTexture(GL_TEXTURE_2D, mt);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        free(pix);
    }
    /* 90% of the window height at most, never wider than 90% of it */
    float hh = ch * 0.9f, ww = hh * mw / mh;
    if (ww > cw * 0.9f) { ww = cw * 0.9f; hh = ww * mh / mw; }
    if (hh > mh * 1.5f) { hh = mh * 1.5f; ww = hh * mw / mh; }
    float x0 = -ww / cw, x1 = ww / cw, y0 = hh / ch, y1 = -hh / ch;
    glViewport(0, 0, cw, ch);
    glEnable(GL_BLEND);   /* batches leave their own equation (subtract, dual-source): set all of it */
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);   /* window alpha stays opaque */
    glUseProgram(cprog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, mt);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glUniform4f(cu_dst, x0, y0, x1, y1);
    glUniform4f(cu_src, 0, 0, 1, 1);
    glUniform1i(cu_alpha1, 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
}

static void present_rect_(u32 fbp, u32 fbw, u32 psm, u32 dbx, u32 dby, u32 w, u32 h) {
    if (scale_req && scale_req != S) apply_scale(scale_req);
    gsgl_flush();
    async_issue_all();   /* this frame's texture-read rows: read back without waiting, used next frame */
    shot_frame++;
    int shot = shot_dir && shot_frame % shot_every == 0 && g_frame >= shot_from && g_frame <= shot_to;
    GLuint src = 0; float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    if (fbw && w && h && w <= 2048 && h <= 2048) {
        u32 yoff = 0;
        target *t = is_color_psm(psm) ? find_target(fbp, fbw, psm, &yoff) : 0;
        if (t) {
            src = t->tex;
            u0 = (float)dbx / t->w; u1 = (float)(dbx + w) / t->w;
            v0 = (float)(dby + yoff) / TH; v1 = (float)(dby + yoff + h) / TH;
            if (shot) {
                u32 *pix = malloc((size_t)w * h * 4);
                target_read(t, dbx, dby + yoff, w, h, pix);
                write_shot(pix, w, h); free(pix);
                /* GS_SHOT_TARGET=<fbp hex>: also shoot that target, taken as a 512x224 PSMCT16S buffer of FBW 8 (the shadow mask) */
                const char *e = getenv("GS_SHOT_TARGET");
                u32 oy = 0; target *o = e ? find_target((u32)strtoul(e, 0, 16), 8, PSMCT16S, &oy) : 0;
                if (o) { pix = malloc((size_t)512 * 224 * 4); target_read(o, 0, oy, 512, 224, pix); shot_frame++; write_shot(pix, 512, 224); shot_frame--; free(pix); }
            }
        } else {
            u32 *pix = malloc((size_t)w * h * 4);
            for (u32 y = 0; y < h; y++)
                for (u32 x = 0; x < w; x++) {
                    u32 v = gs_read_pixel(psm, fbp, fbw, dbx + x, dby + y);
                    if (psm == PSMCT16 || psm == PSMCT16S)
                        v = ((v & 31) << 3) | (((v >> 5) & 31) << 11) | (((v >> 10) & 31) << 19);
                    pix[y * w + x] = v | 0xff000000u;
                }
            stage_ensure(w, h);
            glBindTexture(GL_TEXTURE_2D, stage_tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pix);
            if (shot) write_shot(pix, w, h);
            free(pix);
            src = stage_tex; u1 = (float)w / stage_w; v1 = (float)h / stage_h;
        }
    }
    if (!shown) return;
    RECT cr; GetClientRect(wnd, &cr);
    int cw = cr.right, ch = cr.bottom;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, cw, ch);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
    glColorMask(1, 1, 1, 1);
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (src) {
        extern int ws_active(void);
        int ws = ws_active();                     /* once: the EE thread may change it meanwhile */
        int an = ws ? 16 : 4, ad = ws ? 9 : 3;   /* 4:3, or 16:9 with widescreen (as the cameras were made) */
        int vw = cw, vh = cw * ad / an;
        if (vh > ch) { vh = ch; vw = ch * an / ad; }
        if (g_cfg.stretch) { vw = cw; vh = ch; }
        glViewport((cw - vw) / 2, (ch - vh) / 2, vw, vh);
        glUseProgram(cprog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, g_cfg.smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, g_cfg.smooth ? GL_LINEAR : GL_NEAREST);
        static int bil = -1; if (bil < 0) bil = getenv("GS_PRESENT_BILINEAR") != 0;   /* old filter, for comparison */
        if (g_cfg.smooth && pprog && !bil) {          /* area filter (supersampling at scale 2-4) */
            glUseProgram(pprog);
            glUniform4f(pu_dst, -1, 1, 1, -1);
            glUniform4f(pu_src, u0, v0, u1, v1);
        } else {
            glUniform4f(cu_dst, -1, 1, 1, -1);       /* GS row 0 at the top */
            glUniform4f(cu_src, u0, v0, u1, v1);
            glUniform1i(cu_alpha1, 1);
        }
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    { extern volatile int g_menu_visible; if (g_menu_visible) menu_overlay(cw, ch); }   /* in-game menu (menu.c) */
    {   /* GS_SHOT_WINDOW=1: with GS_SHOT, also save the presented window (debugging the present filter) */
        static int ww = -1; if (ww < 0) ww = getenv("GS_SHOT_WINDOW") != 0;
        if (ww && shot && cw > 0 && ch > 0) {
            u32 *pix = malloc((size_t)cw * ch * 4), *flip = malloc((size_t)cw * ch * 4);
            glReadPixels(0, 0, cw, ch, GL_RGBA, GL_UNSIGNED_BYTE, pix);
            for (int y = 0; y < ch; y++) memcpy(flip + (size_t)y * cw, pix + (size_t)(ch - 1 - y) * cw, (size_t)cw * 4);
            shot_frame += 2; write_shot(flip, (u32)cw, (u32)ch); shot_frame -= 2;
            free(pix); free(flip);
        }
    }
    if (frames_since_check++ % 120 == 0 || vsync_recheck) { vsync_recheck = 0; choose_vsync(); }
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
    if (ftimes) ftimes_before_swap((u64)t0.QuadPart);
    SwapBuffers(dc);
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    if (ftimes) ftimes_after_swap((u64)t0.QuadPart, (u64)t1.QuadPart);
    swap_wait_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart;
    {   /* time between presents as the screen gets them: a gap over 20 ms is a frame shown twice */
        static LARGE_INTEGER last; static int have;
        extern volatile int g_menu_open;
        int menu = g_menu_open;                   /* once: it may close meanwhile */
        if (menu) have = -1;             /* the in-game menu's presents are not frames, nor the gap after it */
        else if (have > 0 && g_frame > 60) {
            double gap = (double)(t1.QuadPart - last.QuadPart) * 1000.0 / fq.QuadPart;
            n_presents++;
            if (gap > 20) {
                n_present_gaps++; if (g_frame > 6600) n_present_gaps_late++;
                extern int g_log_frames;
                if (g_log_frames) LOG("frames: present gap %.1f ms at frame %llu\n", gap, (unsigned long long)g_frame);
            }
            if (gap > present_gap_max && g_frame > 6600) present_gap_max = gap;
        }
        if (!menu) have = 1;
        last = t1;
    }
}

/* Frame pacing by the display. With vsync off the game paced itself to the
 * PS2's 59.94 Hz while a 60 Hz monitor scans out at 60.00: the two drift, and
 * every ~16 s a frame is shown twice or skipped, plus uneven frame timing.
 * If the window's monitor runs at 60 Hz (or 120/180/240 Hz), swap with
 * interval refresh/60 and let SwapBuffers pace the game (pace() in kernel.c
 * then does not sleep). Other refresh rates, a hidden window, turbo or
 * NO_VSYNC=1 keep the timer pacing with vsync off. Checked again every 120
 * presents, so moving the window to another monitor switches mode. */
static int display_hz(void) {
    HMONITOR m = MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXA mi; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(m, (MONITORINFO *)&mi)) return 0;
    DEVMODEA dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return (int)dm.dmDisplayFrequency;
}
static void choose_vsync(void) {
    static int env_off = -1;
    if (env_off < 0) env_off = getenv("NO_VSYNC") != 0;
    int no_vsync = env_off || !g_cfg.vsync;   /* g_cfg.vsync: the in-game menu may change it */
    int want = 0, hz = shown ? display_hz() : 0;
    if (shown && !g_turbo && !no_vsync && swap_interval_fn)
        for (int k = 1; k <= 4; k++) if (hz >= 59 * k && hz <= 61 * k) { want = k; break; }
    if (want != vsync_interval) {
        swap_interval_fn(want);
        vsync_interval = want;
        LOG("gs: %s (display %d Hz)\n", want ? "frames paced by vsync" : "vsync off, timer pacing", hz);
    }
}
int gsgl_vsync_paced(void) { return vsync_interval > 0; }
/* the graphics thread (gsthread.c) takes the context over from the EE thread */
void gsgl_unbind_context(void) { wglMakeCurrent(NULL, NULL); }
void gsgl_bind_context(void) { if (!wglMakeCurrent(dc, glrc)) LOG("gs: cannot make the GL context current on the graphics thread\n"); }
double gsgl_swap_wait_ms(void) { return swap_wait_ms; }

void gsgl_stats(void) {
    LOG("gs: %llu batches, %llu vertices, %llu targets made, %llu downloads (%llu page rows), %llu texture decodes (%llu skipped: same bytes), %llu batches off screen\n",
        (unsigned long long)n_batches, (unsigned long long)n_draw_verts, (unsigned long long)n_targets_made,
        (unsigned long long)n_downloads, (unsigned long long)n_dl_rows, (unsigned long long)n_decodes,
        (unsigned long long)n_same_content, (unsigned long long)n_culled);
    LOG("gs: %llu texture reads used an earlier write-back, %llu async read-backs\n", (unsigned long long)n_sync_skipped, (unsigned long long)n_async_reads);
    LOG("gs: %llu mipmapped batches, %llu of them never reach level 0 (not decoded)\n", (unsigned long long)n_mip_batches, (unsigned long long)n_mip_skip0);
    if (n_presents) LOG("gs: %llu presents, %llu gaps over 20 ms (%llu after frame 6600, longest %.1f ms there)\n",
        (unsigned long long)n_presents, (unsigned long long)n_present_gaps, (unsigned long long)n_present_gaps_late, present_gap_max);
    if (ft_from_cyc) {
        double all = (double)(__rdtsc() - ft_from_cyc);
        LOG("gs: since FT_FROM: texture %.1f%%, state %.1f%%, draw %.1f%%, present %.1f%% of wall cycles; %llu self-copies\n",
            100 * ft_tex / all, 100 * ft_state / all, 100 * ft_draw / all, 100 * ft_present / all, (unsigned long long)n_selfcopy);
    }
}

/* ---- start-up ------------------------------------------------------------------- */
int gsgl_init(int show_window, int scale) {
    S = scale < 1 ? 1 : scale > 4 ? 4 : scale;
    shown = show_window;
    shot_dir = getenv("GS_SHOT");
    if (getenv("GS_SHOT_EVERY")) shot_every = atoi(getenv("GS_SHOT_EVERY"));
    if (shot_every < 1) shot_every = 1;
    if (getenv("GS_FRAME_TIMES")) { extern int g_ft_on; ftimes = 1; g_ft_on = 1; atexit(ftimes_report); }
    /* GS_SHOT_FROM / GS_SHOT_TO=<frame>: only shoot inside that frame range */
    if (getenv("GS_SHOT_FROM")) shot_from = strtoull(getenv("GS_SHOT_FROM"), 0, 10);
    if (getenv("GS_SHOT_TO")) shot_to = strtoull(getenv("GS_SHOT_TO"), 0, 10);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "mh2recomp"; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.style = CS_OWNDC;
    RegisterClassA(&wc);
    RECT r = { 0, 0, g_cfg.window_w ? g_cfg.window_w : 1280, g_cfg.window_h ? g_cfg.window_h : (g_cfg.widescreen ? 720 : 960) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top, x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    EnumDisplayMonitors(NULL, NULL, mon_cb, 0);
    if (mon_found) {
        int mw = mon_rect.right - mon_rect.left, mh = mon_rect.bottom - mon_rect.top;
        if (wh > mh) { ww = ww * mh / wh; wh = mh; }
        x = mon_rect.left + (mw - ww) / 2; y = mon_rect.top + (mh - wh) / 2;
    }
    wnd = CreateWindowA("mh2recomp", "Mat Hoffman's Pro BMX 2", WS_OVERLAPPEDWINDOW | (shown ? WS_VISIBLE : 0),
                        x, y, ww, wh, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) return 0;
    if (shown && g_cfg.fullscreen) set_fullscreen(wnd, 1);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    dc = GetDC(wnd);
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 0;
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy || !wglMakeCurrent(dc, legacy)) return 0;
    typedef HGLRC (WINAPI *create_fn)(HDC, HGLRC, const int *);
    create_fn create = (create_fn)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { LOG("gs: wglCreateContextAttribsARB missing\n"); return 0; }
    const int attrs[] = { 0x2091, 3, 0x2092, 3, 0x9126, 1, 0 };   /* 3.3 core */
    HGLRC core = create(dc, NULL, attrs);
    glrc = core;
    if (!core) { LOG("gs: no OpenGL 3.3 core context\n"); return 0; }
    wglMakeCurrent(dc, core);
    wglDeleteContext(legacy);
    swap_interval_fn = (BOOL (WINAPI *)(int))(void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval_fn) swap_interval_fn(0);   /* choose_vsync() decides at the first present */
#define X(r, n, a) n = (r (APIENTRY *) a)(void *)wglGetProcAddress(#n); if (!n) { LOG("gs: missing %s\n", #n); return 0; }
    GLF(X)
#undef X
    LOG("gs: OpenGL %s (%s), scale %dx, window on %s%s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
        S, mon_found ? mon_name : "the default monitor", mon_found == 2 ? " (Dell 3440x1440)" : "");

    prog = link(vs_src, fs_src);
    cprog = link(cvs_src, cfs_src);
    if (!prog || !cprog) return 0;
#define UL(f) U.f = glGetUniformLocation(prog, "u_" #f)
    UL(rtsize); UL(half); UL(yoff); UL(tex); UL(tme); UL(fst); UL(tfx); UL(tcc); UL(linear); UL(rt24); UL(aem);
    UL(tsize); UL(tmap); UL(wrap); UL(region); UL(ta0); UL(atst); UL(aref); UL(afail_pass); UL(fge); UL(fogcol);
    UL(abe); UL(csel); UL(pabe); UL(fba); UL(abd); UL(sgn); UL(fix);
    UL(mxl); UL(mmin); UL(lcm); UL(mipl); UL(mipk);
#undef UL
    for (int k = 1; k <= 6; k++) { char nm[16]; snprintf(nm, sizeof nm, "u_mip%d", k); U.mip[k] = glGetUniformLocation(prog, nm); }
    cu_dst = glGetUniformLocation(cprog, "u_dst"); cu_src = glGetUniformLocation(cprog, "u_src");
    cu_tex = glGetUniformLocation(cprog, "u_tex"); cu_alpha1 = glGetUniformLocation(cprog, "u_alpha1");
    glUseProgram(prog); glUniform1i(U.tex, 0);
    for (int k = 1; k <= 6; k++) glUniform1i(U.mip[k], k);   /* mip levels 1..6 on texture units 1..6 */
    glUseProgram(cprog); glUniform1i(cu_tex, 0);
    if ((pprog = link(cvs_src, pfs_src))) {
        pu_dst = glGetUniformLocation(pprog, "u_dst"); pu_src = glGetUniformLocation(pprog, "u_src");
        pu_tex = glGetUniformLocation(pprog, "u_tex");
        glUseProgram(pprog); glUniform1i(pu_tex, 0);
    } else LOG("gs: area-filter present shader failed; presenting with one bilinear tap\n");
    if ((wprog = link(cvs_src, wfs_src))) {
        wu_dst = glGetUniformLocation(wprog, "u_dst"); wu_src = glGetUniformLocation(wprog, "u_src");
        wu_tex = glGetUniformLocation(wprog, "u_tex");
        wu_old = glGetUniformLocation(wprog, "u_old"); wu_wrap = glGetUniformLocation(wprog, "u_wrap"); wu_mask = glGetUniformLocation(wprog, "u_mask");
        glUseProgram(wprog); glUniform1i(wu_tex, 0); glUniform1i(wu_old, 1);
    } else LOG("gs: colour wrap shader failed; COLCLAMP 0 clamps\n");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(gs_vertex), (void *)offsetof(gs_vertex, x));
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(gs_vertex), (void *)offsetof(gs_vertex, r));
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(gs_vertex), (void *)offsetof(gs_vertex, s));
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(gs_vertex), (void *)offsetof(gs_vertex, fog));
    for (int i = 0; i < 4; i++) glEnableVertexAttribArray(i);
    if (shown) { glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); SwapBuffers(dc); }
    gsgl_pump();
    return 1;
}
