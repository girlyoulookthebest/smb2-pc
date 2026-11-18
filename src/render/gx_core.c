/* GX emulation core: backend-neutral part of the renderer.
 *
 * Implements the GX API the decompiled game and the shims call (state,
 * matrices, immediate-mode vertices, textures, TEV/channels/indirect/texgen/
 * fog, EFB copies, frame kickoff) on top of a small GPU interface (gx_gpu.h)
 * implemented by render_opengl.c and render_d3d12.c.  See docs/d3d12.md. */
#include "render.h"
#include "gx_gpu.h"
#include "gx_hfr.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "perf.h"
#include "pc_settings.h"

/* GX core state */
typedef struct {
    int width;             /* drawable size */
    int height;

    /* State */
    Mtx44 projection_matrix;
    int projection_type;   /* 0 perspective, 1 ortho (GXSetProjection type) */
    Mtx modelview_matrices[32];
    Mtx texture_matrices[16];
    int current_tex_coord_gen[8];
    int current_cull_mode;
    int current_blend_mode;
    int current_blend_src;
    int current_blend_dst;
    int current_blend_op;
    uint8_t current_z_compare;
    int current_z_func;
    uint8_t current_z_update;
    uint8_t current_alpha_compare;
    int current_alpha_comp0;
    uint8_t current_alpha_ref0;
    int current_alpha_op;
    int current_alpha_comp1;
    uint8_t current_alpha_ref1;

    /* Vertex buffer for immediate mode emulation */
    float* vertex_buffer;
    /* per-vertex UV rect (umin, vmin, umax, vmax) of the 2D quad the vertex
     * belongs to; see the "2D quad UV clamp" note in gx_flush_batch */
    float* rect_buffer;
    int vertex_count;
    int batch_has_texcoord;   /* any GXTexCoord2f32 since this batch began */
    int current_prim_type;
    int current_vtx_fmt;
    int current_mtx_idx;   /* GXSetCurrentMtx: which modelview matrix draws use */
    float proj_vec[7];     /* last projection in GXGetProjectionv form */
    int vp_px[4];          /* current viewport in window pixels (bottom-left) */

    /* Texture bindings (backend handles) */
    void* bound_textures[8];

    /* UI TEV emulation: the 2D code (bitmap/sprite/textdraw/window/...)
     * configures stage 0 as out = TEXC*C0 + C1 / outA = TEXA*A0 + A1 and
     * sets C0/C1 via GXSetTevColor(1,mul)/(2,add) before each draw.  The 3D
     * path (avdisp) never calls GXSetTevColor, so the flag goes sticky-on at
     * GXSetTevColor and off at avdisp material setup / frame start. */
    float tev_c0[4];
    float tev_c1[4];
    int ui_tev;
} GxCore;

static GxCore* g_core = NULL;
static const GxGpu* g_gpu = NULL;

unsigned long long gx_gpu_mem_usage(void) {
    return g_gpu && g_gpu->gpu_mem ? g_gpu->gpu_mem() : 0;
}

/* shader constants built at every flush (see gx_gpu.h) */
static GxDrawConsts s_dc;
static GxTevConsts s_tevc;
static unsigned s_tc_serial = 1;

/* GXFX (GX channel/TEV emulation) hooks - defined at the end of this file */
static void gxfx_init_state(void);
static void gxfx_build(GxCore* gl);
static void gxfx_trace(void);
static void gxfx_leave(void);
static int gx_ui_tev_eff(const GxCore* gl);
/* gx_ui_tev_eff() of the batch being flushed (see there) */
static int s_ui_eff;
static void gxfx_load_nrm(const Mtx mtx, int id);
/* ---- texgen/fog/texcopy: forward decls (definitions at end of file) ---- */
static void tex_ext_build(GxCore* gl);
static void tex_rec_texgen(int dst, int func, int src, int normalize, int postMtx);
static int tex_load_post_mtx(const Mtx mtx, int id);
static int texcopy_init_obj(GXTexObj* obj, void* image, uint16_t w, uint16_t h,
                            int fmt, int wrapS, int wrapT, uint8_t mipmap);
static void texcopy_fix_obj(GXTexObj* obj);
static void gxfx_tev_order(int s, int coord, int map, int color);
static void gxfx_tev_cin(int s, int a, int b, int c, int d);
static void gxfx_tev_ain(int s, int a, int b, int c, int d);
void GXSetTevColorOp_cached(int s, int op, int bias, int scale, uint8_t clamp, int out);
void GXSetTevAlphaOp_cached(int s, int op, int bias, int scale, uint8_t clamp, int out);
static void gxfx_num_stages(int n);
static void gxfx_kcolor(int id, GXColor color);
static void gxfx_ksel(int s, int sel, int alpha);
static void gxfx_swap_mode(int s, int ras, int tex);
static void gxfx_swap_table(int id, int r, int g, int b, int a);
static void gxfx_tev_reg(int id, GXColor color);
static void gxind_build(int n_tev);
static void gxind_reset(void);
static int n_tev_clamped(void);

static void gx_flush_batch(GxCore* gl);

/* when >0, binds and flushes of 24x24 fmt-2 textures (ARAM font glyph pages)
 * print and decrement this - separate budget from g_mtx_trace because the 3D
 * portion of a frame exhausts that one before the 2D overlay draws */
long g_glyph_trace;
static GXTexObj* s_bound_obj[8];

/* GXEnd is an empty macro in the GC SDK: game code finishes a draw by
 * streaming the declared vertex count, never by calling GXEnd.  Pending
 * vertices must therefore be drawn before any state change takes effect,
 * not just at the next GXBegin. */
static void gx_flush_pending(GxCore* gl) {
    if (gl && gl->vertex_count) gx_flush_batch(gl);
}

/* HFR identity tags (gx_hfr.c): a tag change ends the pending batch */
void gx_hfr_flush(void) { gx_flush_pending(g_core); }

static void gx_set_viewport_px(GxCore* gl, int x, int y, int w, int h) {
    gl->vp_px[0] = x; gl->vp_px[1] = y; gl->vp_px[2] = w; gl->vp_px[3] = h;
    g_gpu->set_viewport(x, y, w, h);
}

static void core_resize(GxCore* gl, int width, int height) {
    gl->width = width;
    gl->height = height;
    g_gpu->resize(width, height);
    gx_set_viewport_px(gl, 0, 0, width, height);
    { extern void pc_out_rect_update(void); pc_out_rect_update(); }
}

/* The game issues viewport/scissor in GC EFB space (640x480); scale to the
 * actual window.  Flip Y in GC space first, then scale. */
#define GC_FB_W 640.0f
/* EFB height the game renders into: NTSC overscan adjust makes it 448, not
 * 480 (render.c set_render_mode -> GXSetDispCopySrc).  The VI shows all of
 * those lines in the 4:3 picture, so the whole EFB height maps to the
 * output box. */
static float s_gc_fb_h = 480.0f;
#define GC_FB_H s_gc_fb_h

/* The 640x480 EFB maps to this window-pixel rectangle (the whole
 * window, or a centred 4:3 / 16:9 box for the "Aspect ratio" setting).
 * Updated at frame start and on resize; everything that maps EFB to window
 * coordinates goes through OUT_SX/OUT_SY and the offsets. */
static float s_out_x, s_out_y, s_out_w = GC_FB_W, s_out_h = 480.0f;
static int s_out_full = 1;
void pc_out_rect_update(void) {
    GxCore* gl = g_core;
    float W, H, a;
    if (!gl) return;
    W = (float)gl->width;
    H = (float)gl->height;
    a = pc_settings_output_aspect();
    s_out_x = s_out_y = 0.0f;
    s_out_w = W;
    s_out_h = H;
    if (a > 0.0f && W > 0.0f && H > 0.0f) {
        if (W / H > a * 1.005f) {
            s_out_w = floorf(H * a + 0.5f);
            s_out_x = floorf((W - s_out_w) * 0.5f);
        } else if (W / H < a * 0.995f) {
            s_out_h = floorf(W / a + 0.5f);
            s_out_y = floorf((H - s_out_h) * 0.5f);
        }
    }
    s_out_full = s_out_w == W && s_out_h == H;
}
#define OUT_SX (s_out_w / GC_FB_W)
#define OUT_SY (s_out_h / GC_FB_H)

/* Map a GC EFB rectangle (top-left origin) to GL window pixels (bottom-left
 * origin) by rounding each edge, never origin and size separately: with a
 * non-integer EFB scale (448 lines -> 720 px) adjacent rects then tile
 * exactly and viewport, scissor and copy-clear agree on every row. */
static void pc_efb_rect_to_gl(float x, float y, float w, float h,
                              int* gx, int* gy, int* gw, int* gh) {
    float sx = OUT_SX, sy = OUT_SY;
    int x0 = (int)floorf(s_out_x + x * sx + 0.5f);
    int x1 = (int)floorf(s_out_x + (x + w) * sx + 0.5f);
    int y0 = (int)floorf(s_out_y + (GC_FB_H - y - h) * sy + 0.5f);
    int y1 = (int)floorf(s_out_y + (GC_FB_H - y) * sy + 0.5f);
    *gx = x0; *gy = y0; *gw = x1 - x0; *gh = y1 - y0;
}

extern long g_mtx_trace;

static void core_set_viewport(GxCore* gl, float x, float y, float w, float h, float near_z, float far_z) {
    int vx, vy, vw, vh;
    if (g_mtx_trace > 0) {
        g_mtx_trace--;
        fprintf(stderr, "[trace] viewport %g %g %g %g z %g %g\n", x, y, w, h, near_z, far_z);
    }
    if (!gl) return;
    gx_flush_pending(gl);
    pc_efb_rect_to_gl(x, y, w, h, &vx, &vy, &vw, &vh);
    gx_set_viewport_px(gl, vx, vy, vw, vh);
}

static int s_scissor_px[4], s_scissor_set;   /* last backend scissor (HFR switch) */
static void core_set_scissor(GxCore* gl, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    int cx, cy, cw, ch;
    if (!gl) return;
    if (g_mtx_trace > 0) {
        g_mtx_trace--;
        fprintf(stderr, "[trace] scissor %u %u %u %u\n", x, y, w, h);
    }
    gx_flush_pending(gl);
    pc_efb_rect_to_gl((float)x, (float)y, (float)w, (float)h, &cx, &cy, &cw, &ch);
    g_gpu->set_scissor(cx, cy, cw, ch);
    s_scissor_px[0] = cx; s_scissor_px[1] = cy; s_scissor_px[2] = cw; s_scissor_px[3] = ch;
    s_scissor_set = 1;
}

/* the post-copy EFB clear covers the whole EFB on GC, regardless of the
 * scissor box; outside the aspect box the window is black */
static void core_clear(GxCore* gl, uint32_t color, float depth, uint8_t stencil) {
    (void)stencil;
    if (!gl) return;
    gx_flush_pending(gl);
    if (g_mtx_trace > 0) {
        g_mtx_trace--;
        fprintf(stderr, "[trace] clear rgba=%08x depth=%g\n", color, depth);
    }
    if (!s_out_full) {
        int box[4];
        box[0] = (int)s_out_x; box[1] = (int)s_out_y;
        box[2] = (int)s_out_w; box[3] = (int)s_out_h;
        g_gpu->clear(NULL, 0x000000FFu, depth, 0);
        g_gpu->clear(box, color, depth, 0);
    } else {
        g_gpu->clear(NULL, color, depth, 0);
    }
}

extern long g_mtx_trace;

static void __attribute__((unused)) core_load_projection_matrix(GxCore* gl, const Mtx44 mtx, int type) {
    if (!gl) return;
    gx_flush_pending(gl);
    memcpy(gl->projection_matrix, mtx, sizeof(Mtx44));
    gl->projection_type = type;
    if (g_mtx_trace > 0) {
        g_mtx_trace--;
        fprintf(stderr, "[trace] proj type=%d diag %g %g %g m23=%g\n",
                type, mtx[0][0], mtx[1][1], mtx[2][2], mtx[2][3]);
    }
}

static void core_load_modelview_matrix(GxCore* gl, const Mtx mtx, int index) {
    if (!gl || index < 0 || index >= 32) return;
    gx_flush_pending(gl);
    memcpy(gl->modelview_matrices[index], mtx, sizeof(Mtx));
    if (g_mtx_trace > 0) {
        g_mtx_trace--;
        fprintf(stderr, "[trace] mvload idx=%d r0 %g %g %g %g  t %g %g %g\n",
                index, mtx[0][0], mtx[0][1], mtx[0][2], mtx[0][3],
                mtx[0][3], mtx[1][3], mtx[2][3]);
    }
}

static void core_load_texture_matrix(GxCore* gl, const Mtx mtx, int index) {
    if (!gl || index < 0 || index >= 16) return;
    gx_flush_pending(gl);
    memcpy(gl->texture_matrices[index], mtx, sizeof(Mtx));
}

/* ---- GC texture decoding ----
 * GC textures are stored in row-major tiles whose size depends on bpp:
 * 8x8 for 4bpp (I4), 8x4 for 8bpp (I8/IA4), 4x4 for 16bpp (IA8/RGB565/
 * RGB5A3), 4x4 double-cacheline for RGBA8 (AR plane then GB plane), and
 * 8x8 for CMPR (four 4x4 DXT1-style blocks TL,TR,BL,BR with big-endian
 * color words and MSB-first index pairs). */

static uint32_t gx_rgb565(uint16_t v) {
    uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static uint32_t gx_rgb5a3(uint16_t v) {
    uint32_t r, g, b, a;
    if (v & 0x8000) { /* RGB555, opaque */
        r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        a = 255;
    } else {          /* ARGB3444 */
        a = (v >> 12) & 7; r = (v >> 8) & 15; g = (v >> 4) & 15; b = v & 15;
        a = (a << 5) | (a << 2) | (a >> 1);
        r *= 0x11; g *= 0x11; b *= 0x11;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

#define GX_BE16(p) ((uint16_t)(((p)[0] << 8) | (p)[1]))

static uint32_t cmpr_mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb) {
    uint32_t d = wa + wb;
    uint32_t r = (( a        & 0xFF) * wa + ( b        & 0xFF) * wb) / d;
    uint32_t g = (((a >>  8) & 0xFF) * wa + ((b >>  8) & 0xFF) * wb) / d;
    uint32_t bl= (((a >> 16) & 0xFF) * wa + ((b >> 16) & 0xFF) * wb) / d;
    return r | (g << 8) | (bl << 16) | 0xFF000000u;
}

static void gx_decode_cmpr_block(uint32_t* out, int ow, int x0, int y0, int w, int h, const uint8_t* b) {
    uint16_t c0 = GX_BE16(b), c1 = GX_BE16(b + 2);
    uint32_t pal[4];
    int row, col;

    pal[0] = gx_rgb565(c0);
    pal[1] = gx_rgb565(c1);
    /* interpolate per channel: packed-field math can't divide by 3, the
     * blue field's remainder bleeds into the red byte */
    if (c0 > c1) {
        pal[2] = cmpr_mix(pal[0], pal[1], 2, 1);
        pal[3] = cmpr_mix(pal[0], pal[1], 1, 2);
    } else {
        pal[2] = cmpr_mix(pal[0], pal[1], 1, 1);
        pal[3] = 0; /* transparent */
    }
    for (row = 0; row < 4; row++) {
        uint8_t bits = b[4 + row];
        for (col = 0; col < 4; col++) {
            int x = x0 + col, y = y0 + row;
            if (x < w && y < h)
                out[y * ow + x] = pal[(bits >> (6 - col * 2)) & 3];
        }
    }
}

static uint32_t* gx_decode_texture(const void* data, int w, int h, uint32_t fmt) {
    const uint8_t* src = (const uint8_t*)data;
    uint32_t* out = (uint32_t*)calloc((size_t)w * h, 4);
    int tw, th, x, y, tx, ty;

    if (!out) return NULL;
    if (!src) return out;

    switch (fmt) {
    case GX_TF_I4:     tw = 8; th = 8; break;
    case GX_TF_I8:
    case GX_TF_IA4:    tw = 8; th = 4; break;
    case GX_TF_CMPR:   tw = 8; th = 8; break;
    default:           tw = 4; th = 4; break; /* IA8/RGB565/RGB5A3/RGBA8 */
    }

    for (y = 0; y < h; y += th)
    for (x = 0; x < w; x += tw) {
        switch (fmt) {
        case GX_TF_I4:
            for (ty = 0; ty < 8; ty++) for (tx = 0; tx < 8; tx++) {
                uint8_t b = src[ty * 4 + tx / 2];
                uint32_t i = (tx & 1) ? (b & 15) : (b >> 4);
                i *= 0x11;
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = i | (i << 8) | (i << 16) | (i << 24);
            }
            src += 32;
            break;
        case GX_TF_I8:
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 8; tx++) {
                uint32_t i = src[ty * 8 + tx];
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = i | (i << 8) | (i << 16) | (i << 24);
            }
            src += 32;
            break;
        case GX_TF_IA4:
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 8; tx++) {
                uint8_t b = src[ty * 8 + tx];
                uint32_t i = (b & 15) * 0x11, a = (b >> 4) * 0x11;
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = i | (i << 8) | (i << 16) | (a << 24);
            }
            src += 32;
            break;
        case GX_TF_IA8:
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 4; tx++) {
                uint32_t a = src[(ty * 4 + tx) * 2], i = src[(ty * 4 + tx) * 2 + 1];
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = i | (i << 8) | (i << 16) | (a << 24);
            }
            src += 32;
            break;
        case GX_TF_RGB565:
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 4; tx++) {
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = gx_rgb565(GX_BE16(src + (ty * 4 + tx) * 2));
            }
            src += 32;
            break;
        case GX_TF_RGB5A3:
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 4; tx++) {
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = gx_rgb5a3(GX_BE16(src + (ty * 4 + tx) * 2));
            }
            src += 32;
            break;
        case GX_TF_RGBA8: /* two 32-byte planes: A,R pairs then G,B pairs */
            for (ty = 0; ty < 4; ty++) for (tx = 0; tx < 4; tx++) {
                uint32_t a = src[(ty * 4 + tx) * 2], r = src[(ty * 4 + tx) * 2 + 1];
                uint32_t g = src[32 + (ty * 4 + tx) * 2], b = src[32 + (ty * 4 + tx) * 2 + 1];
                if (x + tx < w && y + ty < h)
                    out[(y + ty) * w + x + tx] = r | (g << 8) | (b << 16) | (a << 24);
            }
            src += 64;
            break;
        case GX_TF_CMPR: /* 4 DXT1-style blocks: TL, TR, BL, BR */
            gx_decode_cmpr_block(out, w, x,     y,     w, h, src);
            gx_decode_cmpr_block(out, w, x + 4, y,     w, h, src + 8);
            gx_decode_cmpr_block(out, w, x,     y + 4, w, h, src + 16);
            gx_decode_cmpr_block(out, w, x + 4, y + 4, w, h, src + 24);
            src += 32;
            break;
        default: /* unknown (palettized etc.): leave transparent black */
            src += 32;
            break;
        }
    }
    return out;
}

/* --perf memory log: textures created / texels uploaded in total, and the
 * ones alive now (see "Texture lifetime" below) */
long long g_tex_created, g_tex_created_texels, g_tex_alive, g_tex_alive_texels;

/* decode GC texels (tiled, big-endian exactly as on disc: pc_swap_tpl swaps
 * only TPL headers) to linear RGBA8 and create the backend texture */
static void* core_create_handle(const void* data, uint16_t w, uint16_t h, uint32_t fmt,
                                uint8_t wrapS, uint8_t wrapT, uint8_t mipmap) {
    void* handle;
    uint32_t* rgba;
    PERF_BEGIN(tc0);
    rgba = gx_decode_texture(data, w, h, fmt);
    handle = rgba ? g_gpu->tex_create(rgba, w, h, wrapS, wrapT, mipmap) : NULL;
    free(rgba);
    PERF_END(tc0, PERF_T_TEXCREATE);
    PERF_COUNT(PERF_C_TEXUPLOAD, 1);
    if (handle) {
        g_tex_created++;
        g_tex_created_texels += (long long)w * h;
    }
    return handle;
}

static void* tex_resolve(GXTexObj* obj);

static void core_bind_texture(GxCore* gl, int stage, GXTexObj* tex) {
    void* handle;
    if (!gl || stage < 0 || stage >= 8) return;
    gx_flush_pending(gl);

    handle = tex ? tex_resolve(tex) : NULL;
    g_gpu->bind_tex(stage, handle);
    gl->bound_textures[stage] = handle;
    s_bound_obj[stage] = tex;
    if (g_glyph_trace > 0 && tex && tex->width == 24 && tex->height == 24 &&
        tex->format == 2) {
        g_glyph_trace--;
        fprintf(stderr, "[glyph] bind stage=%d id=%p\n", stage, handle);
    }
}

/* Game matrices are GC row-major: Mtx is 3x4 (translation in [i][3]), Mtx44
 * is 4x4.  The shader constants want column-major mat4 (GL layout). */
static void mtx34_to_gl(const Mtx m, float out[16]) {
    for (int col = 0; col < 4; col++) {
        out[col * 4 + 0] = m[0][col];
        out[col * 4 + 1] = m[1][col];
        out[col * 4 + 2] = m[2][col];
        out[col * 4 + 3] = (col == 3) ? 1.0f : 0.0f;
    }
}

static void mtx44_to_gl(const Mtx44 m, float out[16]) {
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++)
            out[col * 4 + row] = m[row][col];
}

static void gx_build_matrices(GxCore* gl) {
    mtx44_to_gl(gl->projection_matrix, s_dc.proj);

    int mv = gl->current_mtx_idx;
    if (mv < 0 || mv >= 32) mv = 0;
    mtx34_to_gl(gl->modelview_matrices[mv], s_dc.mv);

    for (int i = 0; i < 8; i++) {
        /* GXSetTexCoordGen picks the matrix per coord: GX_TEXMTX0..9 are ids
         * 30,33..57; GX_IDENTITY (60) and anything unrecognized pass through */
        static const Mtx identity = {
            {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}
        };
        int id = gl->current_tex_coord_gen[i];
        const float (*m)[4] = identity;
        if (id >= 30 && id < 60)
            m = gl->texture_matrices[(id - 30) / 3];
        mtx34_to_gl(m, s_dc.texmtx[i]);
    }
}

/* per-run draw stats, printed by the platform frame dump */
long g_gl_draw_calls, g_gl_draw_verts;

/* when >0, matrix loads and batch flushes print and decrement this;
 * the platform frame dump arms it so the frame AFTER each dump is traced */
long g_mtx_trace;

/* called from the platform frame dump: show the matrices draws are using */
void render_debug_dump_matrices(void) {
    GxCore* gl = g_core;
    int i;
    if (!gl) return;
    fprintf(stderr, "[mtx] proj_vec: type=%g %g %g %g %g %g %g\n",
            gl->proj_vec[0], gl->proj_vec[1], gl->proj_vec[2],
            gl->proj_vec[3], gl->proj_vec[4], gl->proj_vec[5], gl->proj_vec[6]);
    for (i = 0; i < 4; i++)
        fprintf(stderr, "[mtx] proj[%d]: %g %g %g %g\n", i,
                gl->projection_matrix[i][0], gl->projection_matrix[i][1],
                gl->projection_matrix[i][2], gl->projection_matrix[i][3]);
    for (i = 0; i < 3; i++)
        fprintf(stderr, "[mtx] mv%d[%d]: %g %g %g %g\n",
                gl->current_mtx_idx, i,
                gl->modelview_matrices[gl->current_mtx_idx][i][0],
                gl->modelview_matrices[gl->current_mtx_idx][i][1],
                gl->modelview_matrices[gl->current_mtx_idx][i][2],
                gl->modelview_matrices[gl->current_mtx_idx][i][3]);
}

/* GX primitive enums (GXBegin's first arg) */
#define GX_PRIM_QUADS      0x80
#define GX_PRIM_TRIANGLES  0x90
#define GX_PRIM_TRISTRIP   0x98
#define GX_PRIM_TRIFAN     0xA0
#define GX_PRIM_LINES      0xA8
#define GX_PRIM_LINESTRIP  0xB0
#define GX_PRIM_POINTS     0xB8
#define MAX_BATCH_VERTS    GX_MAX_BATCH_VERTS

static int gx_prim_class(int prim) {
    switch (prim & 0xF8) {
        case GX_PRIM_TRIANGLES: return GXD_TRIANGLES;
        case GX_PRIM_TRISTRIP:  return GXD_TRISTRIP;
        case GX_PRIM_TRIFAN:    return GXD_TRIFAN;
        case GX_PRIM_POINTS:    return GXD_POINTS;
        case GX_PRIM_QUADS:     return GXD_QUADS;
        default:                return GXD_TRIANGLES;
    }
}

/* GXSetLineWidth: width in 1/6 EFB pixels (GX default 6 = one pixel).
 * Core-profile GL has no wide lines (glLineWidth > 1 is invalid), so
 * GX_LINES / GX_LINESTRIP batches are expanded on the CPU into quads the way
 * GX rasterizes them: an X-major line is widened along screen Y, a Y-major
 * line along screen X. */
static int s_gx_line_width = 6;
static float* s_line_scratch;

void GXSetLineWidth(uint8_t width, int texOffset) {
    (void)texOffset;
    if (g_core) gx_flush_pending(g_core);
    s_gx_line_width = width ? width : 1;
}
void GXSetLineWidth_cached(uint8_t width, int texOffset) {
    GXSetLineWidth(width, texOffset);
}

static float det3(float m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

/* Solve m * d = r (Cramer's rule).  Returns 0 if (near) singular. */
static int solve3(float m[3][3], const float r[3], float d[3]) {
    float det = det3(m);
    int c, k;
    if (fabsf(det) < 1e-20f) return 0;
    for (c = 0; c < 3; c++) {
        float t[3][3];
        memcpy(t, m, sizeof(t));
        for (k = 0; k < 3; k++) t[k][c] = r[k];
        d[c] = det3(t) / det;
    }
    return 1;
}

/* Expand the current line batch into GL_TRIANGLES (6 verts per segment) in
 * s_line_scratch.  Each corner is its endpoint displaced in object space by
 * the vector that moves the clip position by the wanted pixel offset while
 * keeping w (perspective) or z (ortho) fixed, so the shader's transform,
 * depth and fog stay those of the original endpoint.  Returns vert count. */
static int gx_expand_lines(GxCore* gl) {
    int strip = (gl->current_prim_type & 0xF8) == GX_PRIM_LINESTRIP;
    int nseg = strip ? gl->vertex_count - 1 : gl->vertex_count / 2;
    int mv = gl->current_mtx_idx, s, r, c, out = 0;
    float A[4][4], Mw[3][3], Mz[3][3];
    float (*M1)[3], (*M2)[3];
    int vp[4];
    float halfx, halfy;

    if (nseg <= 0) return 0;
    if (nseg * 6 > MAX_BATCH_VERTS) nseg = MAX_BATCH_VERTS / 6;
    if (!s_line_scratch)
        s_line_scratch = malloc((size_t)MAX_BATCH_VERTS * 16 * sizeof(float));
    if (!s_line_scratch) return 0;
    if (mv < 0 || mv >= 32) mv = 0;

    /* A = P * [MV; 0 0 0 1] */
    for (r = 0; r < 4; r++)
        for (c = 0; c < 4; c++)
            A[r][c] = gl->projection_matrix[r][0] * gl->modelview_matrices[mv][0][c] +
                      gl->projection_matrix[r][1] * gl->modelview_matrices[mv][1][c] +
                      gl->projection_matrix[r][2] * gl->modelview_matrices[mv][2][c] +
                      (c == 3 ? gl->projection_matrix[r][3] : 0.0f);
    for (c = 0; c < 3; c++) {
        Mw[0][c] = A[0][c]; Mw[1][c] = A[1][c]; Mw[2][c] = A[3][c];
        Mz[0][c] = A[0][c]; Mz[1][c] = A[1][c]; Mz[2][c] = A[2][c];
    }
    /* projection_type 0 = perspective: hold w; ortho: w is constant, hold z */
    M1 = gl->projection_type == 0 ? Mw : Mz;
    M2 = gl->projection_type == 0 ? Mz : Mw;

    memcpy(vp, gl->vp_px, sizeof(vp));
    if (vp[2] <= 0 || vp[3] <= 0) return 0;
    /* half width in window pixels (1/6 EFB px units, EFB 640x480 -> window) */
    halfx = 0.5f * (s_gx_line_width / 6.0f) * OUT_SX;
    halfy = 0.5f * (s_gx_line_width / 6.0f) * OUT_SY;

    for (s = 0; s < nseg; s++) {
        const float* e[2];
        float cl[2][4], scr[2][2], ox, oy;
        float q[4][16];
        int k;
        e[0] = &gl->vertex_buffer[(strip ? s : s * 2) * 16];
        e[1] = &gl->vertex_buffer[(strip ? s + 1 : s * 2 + 1) * 16];
        for (k = 0; k < 2; k++) {
            for (r = 0; r < 4; r++)
                cl[k][r] = A[r][0] * e[k][0] + A[r][1] * e[k][1] +
                           A[r][2] * e[k][2] + A[r][3];
            if (cl[k][3] <= 1e-6f) break;
            scr[k][0] = cl[k][0] / cl[k][3] * (vp[2] * 0.5f);
            scr[k][1] = cl[k][1] / cl[k][3] * (vp[3] * 0.5f);
        }
        if (k < 2) continue; /* endpoint behind the eye: drop the segment */
        if (fabsf(scr[1][0] - scr[0][0]) >= fabsf(scr[1][1] - scr[0][1])) {
            ox = 0.0f; oy = halfy;  /* X-major: widen vertically */
        } else {
            ox = halfx; oy = 0.0f;  /* Y-major: widen horizontally */
        }
        for (k = 0; k < 4; k++) {
            int src = k >> 1;  /* corners 0,1 from e0; 2,3 from e1 */
            float sign = (k & 1) ? -1.0f : 1.0f;
            float w = cl[src][3], rhs[3], d[3];
            memcpy(q[k], e[src], sizeof(q[k]));
            rhs[0] = sign * ox / (vp[2] * 0.5f) * w;
            rhs[1] = sign * oy / (vp[3] * 0.5f) * w;
            rhs[2] = 0.0f;
            if (solve3(M1, rhs, d) || solve3(M2, rhs, d)) {
                q[k][0] += d[0]; q[k][1] += d[1]; q[k][2] += d[2];
            }
        }
        /* corners e0+ e0- e1+ e1- -> triangles (0,1,2) (2,1,3) */
        memcpy(&s_line_scratch[(out + 0) * 16], q[0], sizeof(q[0]));
        memcpy(&s_line_scratch[(out + 1) * 16], q[1], sizeof(q[0]));
        memcpy(&s_line_scratch[(out + 2) * 16], q[2], sizeof(q[0]));
        memcpy(&s_line_scratch[(out + 3) * 16], q[2], sizeof(q[0]));
        memcpy(&s_line_scratch[(out + 4) * 16], q[1], sizeof(q[0]));
        memcpy(&s_line_scratch[(out + 5) * 16], q[3], sizeof(q[0]));
        out += 6;
    }
    return out;
}


static void gx_flush_batch_impl(GxCore* gl);
static void gx_flush_batch(GxCore* gl) {
    if (gl->vertex_count == 0) return;
    if (g_perf_on) {
        uint64_t t0 = perf_now();
        g_perf_c[PERF_C_DRAWS]++;
        g_perf_c[PERF_C_VERTS] += gl->vertex_count;
        gx_flush_batch_impl(gl);
        g_perf_t[PERF_T_FLUSH] += perf_ms(t0, perf_now());
    } else {
        gx_flush_batch_impl(gl);
    }
}

static void gx_flush_batch_impl(GxCore* gl) {
    int traced = 0;
    if (gl->vertex_count == 0) return;
    g_gl_draw_calls++;
    g_gl_draw_verts += gl->vertex_count;
    if (g_glyph_trace > 0 && s_bound_obj[0] && s_bound_obj[0]->width == 24 &&
        s_bound_obj[0]->height == 24 && s_bound_obj[0]->format == 2) {
        const float* v = gl->vertex_buffer;
        int mv = gl->current_mtx_idx;
        float e[3], c[4];
        int r;
        if (mv < 0 || mv >= 32) mv = 0;
        for (r = 0; r < 3; r++)
            e[r] = gl->modelview_matrices[mv][r][0] * v[0] +
                   gl->modelview_matrices[mv][r][1] * v[1] +
                   gl->modelview_matrices[mv][r][2] * v[2] +
                   gl->modelview_matrices[mv][r][3];
        for (r = 0; r < 4; r++)
            c[r] = gl->projection_matrix[r][0] * e[0] +
                   gl->projection_matrix[r][1] * e[1] +
                   gl->projection_matrix[r][2] * e[2] +
                   gl->projection_matrix[r][3];
        g_glyph_trace--;
        fprintf(stderr, "[glyph] flush prim=0x%x n=%d tex0=%p mv=%d "
                "v0 %g %g %g uv %g %g col %g %g %g %g "
                "ndc %g %g %g w=%g blend=%d/%d/%d z=%d,%d\n",
                gl->current_prim_type, gl->vertex_count,
                gl->bound_textures[0], mv,
                v[0], v[1], v[2], v[10], v[11], v[6], v[7], v[8], v[9],
                c[3] != 0 ? c[0]/c[3] : 9999, c[3] != 0 ? c[1]/c[3] : 9999,
                c[3] != 0 ? c[2]/c[3] : 9999, c[3],
                gl->current_blend_mode, gl->current_blend_src,
                gl->current_blend_dst,
                gl->current_z_compare, gl->current_z_func);
    }
    if (g_mtx_trace > 0) {
        traced = 1;
        g_mtx_trace--;
        int mv = gl->current_mtx_idx;
        int k, in = 0;
        float cx[4];
        if (mv < 0 || mv >= 32) mv = 0;
        /* project each vertex through modelview+projection; count how many
         * land inside the clip volume, and report v0's NDC position */
        for (k = 0; k < gl->vertex_count; k++) {
            const float* vtx = &gl->vertex_buffer[k * 16];
            float e[3], c[4];
            int r;
            for (r = 0; r < 3; r++)
                e[r] = gl->modelview_matrices[mv][r][0] * vtx[0] +
                       gl->modelview_matrices[mv][r][1] * vtx[1] +
                       gl->modelview_matrices[mv][r][2] * vtx[2] +
                       gl->modelview_matrices[mv][r][3];
            for (r = 0; r < 4; r++)
                c[r] = gl->projection_matrix[r][0] * e[0] +
                       gl->projection_matrix[r][1] * e[1] +
                       gl->projection_matrix[r][2] * e[2] +
                       gl->projection_matrix[r][3];
            if (c[3] > 0 && c[0] >= -c[3] && c[0] <= c[3] &&
                c[1] >= -c[3] && c[1] <= c[3] && c[2] >= -c[3] && c[2] <= c[3])
                in++;
            if (k == 0) memcpy(cx, c, sizeof(cx));
            if (gl->vertex_count <= 40 && c[3] != 0)
                fprintf(stderr, "[trace]   v%d ndc %g %g %g w=%g uv %g %g col %.2f %.2f %.2f %.2f\n",
                        k, c[0]/c[3], c[1]/c[3], c[2]/c[3], c[3],
                        vtx[10], vtx[11], vtx[6], vtx[7], vtx[8], vtx[9]);
        }
        fprintf(stderr, "[trace] flush prim=0x%x n=%d in=%d mv=%d  v0ndc %g %g %g w=%g  col %g %g %g %g  tex0=%p blend=%d z=%d,%d,%d pt=%d cull=%d\n",
                gl->current_prim_type, gl->vertex_count, in, mv,
                cx[3] != 0 ? cx[0]/cx[3] : 9999, cx[3] != 0 ? cx[1]/cx[3] : 9999,
                cx[3] != 0 ? cx[2]/cx[3] : 9999, cx[3],
                gl->vertex_buffer[6], gl->vertex_buffer[7], gl->vertex_buffer[8],
                gl->vertex_buffer[9],
                gl->bound_textures[0], gl->current_blend_mode,
                gl->current_z_compare, gl->current_z_func, gl->current_z_update,
                gl->projection_type, gl->current_cull_mode);
        { /* texcoord gen 0 matrix */
            int tid = gl->current_tex_coord_gen[0];
            if (tid >= 30 && tid < 60) {
                const float (*tm)[4] = gl->texture_matrices[(tid - 30) / 3];
                fprintf(stderr, "[trace]  tcg0=%d tm %g %g %g %g / %g %g %g %g\n", tid,
                        tm[0][0], tm[0][1], tm[0][2], tm[0][3], tm[1][0], tm[1][1], tm[1][2], tm[1][3]);
            }
        }
    }

    (void)traced;
    gx_build_matrices(gl);
    s_ui_eff = gx_ui_tev_eff(gl);
    gxfx_build(gl);
    tex_ext_build(gl); /* texgen-src / fog constants */
    gxfx_trace();
    s_dc.mode[1] = s_ui_eff;
    memcpy(s_dc.tev_c0, gl->tev_c0, sizeof(s_dc.tev_c0));
    memcpy(s_dc.tev_c1, gl->tev_c1, sizeof(s_dc.tev_c1));
    s_dc.mode[2] = gl->batch_has_texcoord && gl->bound_textures[0] != NULL;
    s_dc.mode[3] = 0;                         /* uvclamp: quads below */
    s_dc.efb_scale[0] = OUT_SX;
    s_dc.efb_scale[1] = OUT_SY;
    /* NDC y offset of 1/256 window pixel (D3D12 only, see render_d3d12.c) */
    s_dc.efb_scale[2] = gl->vp_px[3] > 0 ? (2.0f / 256.0f) / (float)gl->vp_px[3] : 0.0f;
    {
        GxDrawCall d;
        memset(&d, 0, sizeof d);
        d.dc = &s_dc;
        d.tc = &s_tevc;
        d.tc_serial = s_tc_serial;
        /* GL_DEPTH_CLAMP-style "no near/far clip" also disables near-plane
         * clipping, which turns any triangle straddling the camera plane (w
         * crossing 0) into a screen-wide smear.  Keep it only for ortho/2D
         * draws, which the game places hugging the near plane; perspective
         * draws need real clipping. */
        d.depth_clamp = gl->projection_type != 0;
        if ((gl->current_prim_type & 0xF8) == GX_PRIM_LINES ||
            (gl->current_prim_type & 0xF8) == GX_PRIM_LINESTRIP) {
            int n = gx_expand_lines(gl);
            if (n > 0) {
                d.prim = GXD_TRIANGLES;
                d.verts = s_line_scratch;
                d.nverts = n;
                d.no_cull = 1;
                g_gpu->draw(&d);
            }
            gl->vertex_count = 0;
            gl->batch_has_texcoord = 0;
            return;
        }
        d.prim = gx_prim_class(gl->current_prim_type);
        d.verts = gl->vertex_buffer;
        d.nverts = gl->vertex_count;
        if (d.prim == GXD_QUADS) {
            int quads = gl->vertex_count / 4;
            /* 2D quad UV clamp.  Sprites and glyphs are quads whose UV rect
             * is a cell of a shared atlas, drawn 1:1 on whole EFB pixels, so
             * on GC bilinear filtering never reaches the neighbouring cell.
             * At a higher resolution the outer window pixels sample up to
             * half an EFB pixel closer to the edge, so the fragment shader
             * clamps texcoord 0 to the quad's UV rect inset by half an EFB
             * pixel.  Ortho (2D) draws only. */
            int uvclamp = gl->projection_type == 1 && gl->batch_has_texcoord &&
                          gl->rect_buffer && quads > 0;
            if (uvclamp) {
                for (int q = 0; q < quads; q++) {
                    const float* v = &gl->vertex_buffer[q * 4 * 16];
                    float r[4] = { v[10], v[11], v[10], v[11] };
                    for (int k = 1; k < 4; k++) {
                        const float* w = v + k * 16;
                        if (w[10] < r[0]) r[0] = w[10];
                        if (w[11] < r[1]) r[1] = w[11];
                        if (w[10] > r[2]) r[2] = w[10];
                        if (w[11] > r[3]) r[3] = w[11];
                    }
                    for (int k = 0; k < 4; k++)
                        memcpy(&gl->rect_buffer[(q * 4 + k) * 4], r, sizeof(r));
                }
                d.rects = gl->rect_buffer;
            }
            s_dc.mode[3] = uvclamp;
            d.nverts = quads * 4;
            /* GX quads come from the 2D/sprite paths, whose winding doesn't
             * survive the GC->GL coordinate changes; never cull them */
            d.no_cull = 1;
        }
        g_gpu->draw(&d);
    }
    gl->vertex_count = 0;
    gl->batch_has_texcoord = 0;
}

static void core_begin(GxCore* gl, int prim_type, int vtx_fmt, uint16_t count) {
    if (!gl) return;

    gx_flush_batch(gl); /* leftover vertices from a batch missing its GXEnd */
    gl->current_prim_type = prim_type;
    gl->current_vtx_fmt = vtx_fmt;
    gl->batch_has_texcoord = 0;
    (void)count;
}

static void core_end(GxCore* gl) {
    if (!gl) return;
    gx_flush_batch(gl);
}

/* GX streams one vertex at a time, attributes in fixed order with position
 * first, and only the enabled attributes are sent.  So a position write
 * STARTS a vertex (with defaults for the rest) and the other attribute
 * writes update the vertex most recently started. */
static void core_position3f32(GxCore* gl, float x, float y, float z) {
    if (!gl) return;
    if (gl->vertex_count >= MAX_BATCH_VERTS) return;

    float* v = &gl->vertex_buffer[gl->vertex_count * 16];
    v[0] = x;    v[1] = y;    v[2] = z;
    v[3] = 0.0f; v[4] = 0.0f; v[5] = 1.0f;   /* normal */
    v[6] = 1.0f; v[7] = 1.0f; v[8] = 1.0f; v[9] = 1.0f; /* color: white */
    v[10] = -1.0f; v[11] = -1.0f;            /* texcoord: "untextured" */
    v[12] = v[13] = v[14] = v[15] = 0.0f;
    gl->vertex_count++;
}

static void core_normal3f32(GxCore* gl, float x, float y, float z) {
    if (!gl || gl->vertex_count == 0) return;

    float* v = &gl->vertex_buffer[(gl->vertex_count - 1) * 16];
    v[3] = x;
    v[4] = y;
    v[5] = z;
}

static void core_color4u8(GxCore* gl, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!gl || gl->vertex_count == 0) return;

    float* v = &gl->vertex_buffer[(gl->vertex_count - 1) * 16];
    v[6] = r / 255.0f;
    v[7] = g / 255.0f;
    v[8] = b / 255.0f;
    v[9] = a / 255.0f;
}

static void core_texcoord2f32(GxCore* gl, float s, float t) {
    if (!gl || gl->vertex_count == 0) return;

    float* v = &gl->vertex_buffer[(gl->vertex_count - 1) * 16];
    v[10] = s;
    v[11] = t;
    gl->batch_has_texcoord = 1;
}

static void core_set_blend_mode(GxCore* gl, int type, int src, int dst, int op) {
    if (!gl) return;
    gx_flush_pending(gl);

    gl->current_blend_mode = type;
    gl->current_blend_src = src;
    gl->current_blend_dst = dst;
    gl->current_blend_op = op;
    g_gpu->set_blend(type, src, dst, op);
}

static void core_set_z_mode(GxCore* gl, uint8_t compare_enable, int func, uint8_t update_enable) {
    if (!gl) return;
    gx_flush_pending(gl);

    gl->current_z_compare = compare_enable;
    gl->current_z_func = func;
    gl->current_z_update = update_enable;
    /* GX applies Z update independently of Z compare: compare off + update
     * on still writes depth (the minimap's circular depth mask relies on
     * this); backends keep the test on and use ALWAYS for "compare off" */
    g_gpu->set_depth(compare_enable, func, update_enable);
}

static void core_set_alpha_compare(GxCore* gl, int comp0, uint8_t ref0, int op, int comp1, uint8_t ref1) {
    if (!gl) return;

    /* the compare is a per-draw shader uniform: flush the batch built under
     * the previous state before changing it */
    if (!gl->current_alpha_compare || gl->current_alpha_comp0 != comp0 ||
        gl->current_alpha_ref0 != ref0 || gl->current_alpha_op != op ||
        gl->current_alpha_comp1 != comp1 || gl->current_alpha_ref1 != ref1)
        gx_flush_pending(gl);
    gl->current_alpha_compare = 1;
    gl->current_alpha_comp0 = comp0;
    gl->current_alpha_ref0 = ref0;
    gl->current_alpha_op = op;
    gl->current_alpha_comp1 = comp1;
    gl->current_alpha_ref1 = ref1;
}

static void core_set_cull_mode(GxCore* gl, int mode) {
    if (!gl) return;
    gx_flush_pending(gl);

    gl->current_cull_mode = mode;
    g_gpu->set_cull(mode);
}

/* ---- renderer selection / lifecycle ---- */
static GxCore s_core;

static const GxGpu* gx_pick_gpu(const char* name) {
#ifdef SMB2_HAVE_D3D12
    if (name && (!strcmp(name, "d3d12") || !strcmp(name, "dx12") || !strcmp(name, "D3D12")))
        return &gx_gpu_d3d12;
#endif
    (void)name;
    return &gx_gpu_gl;
}

static const GxGpu* s_base_gpu; /* the backend itself (g_gpu may be the HFR proxy) */
int render_is_gl(void) { return s_base_gpu == NULL || s_base_gpu == &gx_gpu_gl; }

bool render_init(const char* backend_name, int width, int height, bool vsync, int msaa) {
    GxCore* gl = &s_core;
    const GxGpu* gpu;
    if (g_core) return true;

    gpu = gx_pick_gpu(backend_name);
    memset(gl, 0, sizeof *gl);
    gl->width = width;
    gl->height = height;
    gl->vertex_buffer = malloc((size_t)GX_MAX_BATCH_VERTS * GXV_FLOATS * sizeof(float));
    gl->rect_buffer = malloc((size_t)GX_MAX_BATCH_VERTS * 4 * sizeof(float));
    if (!gl->vertex_buffer || !gl->rect_buffer) {
        free(gl->vertex_buffer); free(gl->rect_buffer);
        return false;
    }

    /* Initialize matrices */
    for (int i = 0; i < 32; i++) {
        MTXIdentity(gl->modelview_matrices[i]);
    }
    for (int i = 0; i < 16; i++) {
        MTXIdentity(gl->texture_matrices[i]);
    }
    for (int i = 0; i < 8; i++) {
        gl->current_tex_coord_gen[i] = 60; /* GX_IDENTITY */
    }
    MTXIdentity(gl->projection_matrix);
    gl->projection_type = 1; /* ortho until the game loads a projection */

    /* Default state (the backends start in the matching API state) */
    gl->current_cull_mode = GX_CULL_BACK;
    gl->current_blend_mode = GX_BM_NONE;
    gl->current_z_compare = 1;
    gl->current_z_func = GX_LEQUAL;
    gl->current_z_update = 1;
    gl->tev_c0[0] = gl->tev_c0[1] = gl->tev_c0[2] = gl->tev_c0[3] = 1.0f;
    gl->vp_px[2] = width;
    gl->vp_px[3] = height;

    if (!gpu->init(width, height, vsync, msaa)) {
        fprintf(stderr, "[render] %s backend failed to initialise\n", gpu->name);
        free(gl->vertex_buffer); free(gl->rect_buffer);
        return false;
    }
    s_base_gpu = gpu;
    g_gpu = gpu;
    hfr_init(gpu);   /* HFR recording proxy, selected per tick (docs/hfr.md) */
    g_core = gl;
    gxfx_init_state();
    printf("[render] backend: %s\n", gpu->name);
    return true;
}

void render_shutdown(void) {
    if (!g_core) return;
    g_gpu->shutdown();
    free(g_core->vertex_buffer);
    free(g_core->rect_buffer);
    g_core->vertex_buffer = g_core->rect_buffer = NULL;
    g_core = NULL;
}

void render_begin_frame(void) {
    if (!g_core) return;
    g_core->vertex_count = 0;
    g_gpu->begin_frame();
    /* matrices are uploaded lazily at flush time: the game swaps projection
     * and modelview many times inside one frame (3D pass, then ortho UI) */
}

void render_end_frame(void) {
    /* Flush any remaining vertices */
    if (g_core) gx_flush_batch(g_core);
}

/* A batch still pending at present time (GXEnd is a no-op, so the frame's
 * last batch waits for the next state change) is not flushed here; see
 * docs/d3d12.md, Known differences. */
void render_present(void) {
    if (g_core) {
        if (hfr_active()) {
            /* HFR pacer: how long a present call takes (every present) */
            extern double g_hfr_present_est_ns;
            uint64_t t0 = perf_now();
            g_gpu->present();
            g_hfr_present_est_ns = g_hfr_present_est_ns * 0.9 + perf_ms(t0, perf_now()) * 1e6 * 0.1;
        } else {
            g_gpu->present();
        }
        hfr_note_present();
    }
}

void render_set_vsync(int on) {
    if (g_gpu) g_gpu->set_vsync(on);
}

int render_read_frame(unsigned char** rgb, int* w, int* h) {
    if (!g_core) return 0;
    return g_gpu->read_frame(rgb, w, h);
}

static void pc_reapply_gx_view(void); /* PC settings: below */
void render_resize(int width, int height) {
    if (g_core) core_resize(g_core, width, height);
    pc_reapply_gx_view(); /* viewport/scissor follow the new size */
}

/* GX-compatible API implementations */
void GXInit(void) {
    /* Already initialized in render_init */
}

/* ---- frame kickoff (DOL asm gx_start_new_frame, 0x80064970) ----
 * GC: FIFO bookkeeping, then GXCopyDisp(xfb, clear=1) copies the EFB out and
 * clears it with the GXSetCopyClear color, then resets z/alpha-compare modes
 * (asm g_gx_reset_z_and_alpha_compare_modes, 0x80064B84).  PC: a backend clear. */
typedef struct { uint8_t r, g, b, a; } ShimGXColor;
static uint32_t s_copy_clear_rgba = 0x000000FF;
static float s_copy_clear_z = 1.0f;

void GXSetCopyClear(ShimGXColor c, uint32_t z) {
    s_copy_clear_rgba = ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) |
                        ((uint32_t)c.b << 8) | c.a;
    s_copy_clear_z = (float)(z & 0xFFFFFF) / 16777215.0f;
}

void GXSetCopyClear_cached(ShimGXColor c, uint32_t z) {
    GXSetCopyClear(c, z);
}

/* On GC the GXCopyDisp in here IS the present: it copies the finished EFB to
 * the XFB for scanout and then clears the EFB.  So on PC the swap must happen
 * here (the game calls this after drawing a frame, possibly more than once
 * per video_finish_frame), not at video_finish_frame time, or the clear below
 * erases the frame before it is ever shown. */
int g_gx_presents_frames; /* video_finish_frame stops swapping once set */
static void texcopy_poll(void);
static void tex_frame(GxCore* gl);

void gx_start_new_frame(void) {
    extern void platform_swap_buffers(void);
    g_gx_presents_frames = 1;
    texcopy_poll();
    platform_swap_buffers();
    if (g_core) {
        /* HFR (docs/hfr.md): the proxy records this tick's frame when the
         * frame-rate setting is not 60; switching applies here */
        const GxGpu* sel = hfr_select();
        if (sel && sel != g_gpu) {
            GxCore* gl = g_core;
            int i;
            gx_flush_pending(gl);
            g_gpu = sel;
            /* the proxy tracks the backend state from here: send it the
             * current state (the game's GX caches won't resend it) */
            g_gpu->set_viewport(gl->vp_px[0], gl->vp_px[1], gl->vp_px[2], gl->vp_px[3]);
            if (s_scissor_set)
                g_gpu->set_scissor(s_scissor_px[0], s_scissor_px[1], s_scissor_px[2], s_scissor_px[3]);
            g_gpu->set_blend(gl->current_blend_mode, gl->current_blend_src,
                             gl->current_blend_dst, gl->current_blend_op);
            g_gpu->set_depth(gl->current_z_compare, gl->current_z_func, gl->current_z_update);
            g_gpu->set_cull(gl->current_cull_mode);
            for (i = 0; i < 8; i++) g_gpu->bind_tex(i, gl->bound_textures[i]);
        }
    }
    if (hfr_active() && g_core) {
        /* HFR: the previous frame's last batch (drawn here anyway, by the
         * clear's flush) stays out of the new stream */
        gx_flush_pending(g_core);
        hfr_tick_begin();
    }
    if (g_core) tex_frame(g_core);   /* texture lifetime: frees, budget, deferred destroys */
    pc_out_rect_update();   /* aspect box for the new frame */
    if (g_core)
        core_clear(g_core, s_copy_clear_rgba,
                                s_copy_clear_z, 0);
    GXSetZMode(1, 3 /* GX_LEQUAL */, 1);
    GXSetAlphaCompare(7 /* ALWAYS */, 0, 0 /* AND */, 7, 0);
    {
        extern void gx_ui_tev_disable(void);
        gx_ui_tev_disable();
    }
}

/* Last viewport in GC EFB coordinates, for GXGetViewportv (sel_ngc's menu
 * background overlay copies exactly this rectangle). */
static float s_vp_gc[6] = { 0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f };

void GXGetViewportv(float *vp) {
    memcpy(vp, s_vp_gc, sizeof(s_vp_gc));
}

void GXSetViewport(float left, float top, float width, float height, float nearZ, float farZ) {
    s_vp_gc[0] = left; s_vp_gc[1] = top; s_vp_gc[2] = width;
    s_vp_gc[3] = height; s_vp_gc[4] = nearZ; s_vp_gc[5] = farZ;
    if (g_core) core_set_viewport(g_core, left, top, width, height, nearZ, farZ);
}

static uint32_t s_sc_l = 0, s_sc_t = 0, s_sc_w = 640, s_sc_h = 480;

void GXSetScissor(uint32_t left, uint32_t top, uint32_t width, uint32_t height) {
    s_sc_l = left; s_sc_t = top; s_sc_w = width; s_sc_h = height;
    if (g_core) core_set_scissor(g_core, left, top, width, height);
}

/* Game-side cached wrapper (gx.h): the GC version skips redundant writes;
 * here every call is forwarded so the backend state always matches. */
void GXSetScissor_cached(uint32_t left, uint32_t top, uint32_t width, uint32_t height) {
    GXSetScissor(left, top, width, height);
}

/* PC settings: a window resize between frames (settings overlay) must not
 * leave the backend viewport/scissor at the old size's mapping. */
static void pc_reapply_gx_view(void) {
    if (!g_core) return;
    core_set_viewport(g_core, s_vp_gc[0], s_vp_gc[1],
                                   s_vp_gc[2], s_vp_gc[3], s_vp_gc[4], s_vp_gc[5]);
    core_set_scissor(g_core, s_sc_l, s_sc_t, s_sc_w, s_sc_h);
}

void GXGetScissor(uint32_t* left, uint32_t* top, uint32_t* width, uint32_t* height) {
    if (left) *left = s_sc_l;
    if (top) *top = s_sc_t;
    if (width) *width = s_sc_w;
    if (height) *height = s_sc_h;
}

void GXSetDispCopySrc(uint16_t left, uint16_t top, uint16_t width, uint16_t height) {
    /* the display copy itself is not needed on PC; its height is the EFB
     * height the game renders into, which sets the EFB -> window scale */
    (void)left; (void)top; (void)width;
    if (height >= 64 && height <= 640 && (float)height != s_gc_fb_h) {
        s_gc_fb_h = (float)height;
        fprintf(stderr, "[video] EFB height %u\n", (unsigned)height);
        pc_out_rect_update();
    }
}

void GXSetDispCopyDst(uint16_t width, uint16_t height) {
    /* Display copy destination - not needed for PC */
}

void GXSetCopyFilter(uint8_t aa, uint8_t samplePattern[12][2], uint8_t vf, uint8_t vfilter[7]) {
    /* Copy filter - not needed for PC */
}

void GXSetBlendMode(int type, int srcFactor, int dstFactor, int op) {
    if (g_core) core_set_blend_mode(g_core, type, srcFactor, dstFactor, op);
}

void GXSetZMode(uint8_t compareEnable, int func, uint8_t updateEnable) {
    if (g_core) core_set_z_mode(g_core, compareEnable, func, updateEnable);
}

void GXSetZCompLoc(uint8_t beforeTex) {
    /* Z compare location - not needed for PC */
}

void GXSetAlphaCompare(int comp0, uint8_t ref0, int op, int comp1, uint8_t ref1) {
    if (g_core) core_set_alpha_compare(g_core, comp0, ref0, op, comp1, ref1);
}

void GXSetCullMode(int mode) {
    if (g_core) core_set_cull_mode(g_core, mode);
}

/* GXTexObj getters (the bg_* renderers re-init textures from these) */
uint16_t GXGetTexObjWidth(GXTexObj* obj)  { return obj ? obj->width : 0; }
uint16_t GXGetTexObjHeight(GXTexObj* obj) { return obj ? obj->height : 0; }
uint32_t GXGetTexObjFmt(GXTexObj* obj)    { return obj ? obj->format : 0; }
void*    GXGetTexObjData(GXTexObj* obj)   { return obj ? obj->image_data : NULL; }
uint8_t  GXGetTexObjWrapS(GXTexObj* obj)  { return obj ? obj->wrapS : 0; }
uint8_t  GXGetTexObjWrapT(GXTexObj* obj)  { return obj ? obj->wrapT : 0; }
uint8_t  GXGetTexObjMipMap(GXTexObj* obj) { return obj ? obj->mipmap : 0; }

/* gxcache.c variants: same semantics, the GC-side register cache is moot here */
void GXSetBlendMode_cached(int type, int srcFactor, int dstFactor, int op) {
    GXSetBlendMode(type, srcFactor, dstFactor, op);
}
void GXSetZMode_cached(uint8_t compareEnable, int func, uint8_t updateEnable) {
    GXSetZMode(compareEnable, func, updateEnable);
}
/* gxcache.c: report the current Z mode so callers can restore it (sprite.c
 * draw_sprite_draw_request with flag bit 21, nl2ngc.c). */
void g_read_something_from_prev_GX_settings(uint8_t *compareEnable, int *func, uint8_t *updateEnable) {
    *compareEnable = g_core ? g_core->current_z_compare : 1;
    *func = g_core ? g_core->current_z_func : GX_LEQUAL;
    *updateEnable = g_core ? g_core->current_z_update : 1;
}
void GXSetCullMode_cached(int mode) {
    GXSetCullMode(mode);
}

void GXSetNumChans(uint8_t n) {
    /* Number of color channels */
}

void GXSetNumTexGens(uint8_t n) {
    /* Number of texture coordinate generators */
}

void GXSetNumTevStages(uint8_t n) {
    gxfx_num_stages(n);
}

void GXSetTevOrder(int stage, int texCoord, int texMap, int color) {
    gxfx_tev_order(stage, texCoord, texMap, color);
}

void GXSetTevColorIn(int stage, int a, int b, int c, int d) {
    gxfx_tev_cin(stage, a, b, c, d);
}

void GXSetTevColorOp(int stage, int tevOp, int bias, int scale, int clamp, int outReg) {
    GXSetTevColorOp_cached(stage, tevOp, bias, scale, (uint8_t)clamp, outReg);
}

void GXSetTevAlphaIn(int stage, int a, int b, int c, int d) {
    gxfx_tev_ain(stage, a, b, c, d);
}

void GXSetTevAlphaOp(int stage, int tevOp, int bias, int scale, int clamp, int outReg) {
    GXSetTevAlphaOp_cached(stage, tevOp, bias, scale, (uint8_t)clamp, outReg);
}

/* GXSetTevColor: the 2D code sets GX_TEVREG0 (id 1, multiply color) and
 * GX_TEVREG1 (id 2, add color) before each sprite/glyph draw; stage 0 is
 * configured as out = TEXC*C0 + C1 (see GxCore.ui_tev).  Textbox and
 * banner text gets its colour from C0 alone (the textures are white). */
void GXSetTevColor(int id, GXColor color) {
    float v[4];
    float* dst;
    if (!g_core) return;
    v[0] = color.r / 255.0f;
    v[1] = color.g / 255.0f;
    v[2] = color.b / 255.0f;
    v[3] = color.a / 255.0f;
    gxfx_tev_reg(id, color);
    dst = (id == 1) ? g_core->tev_c0 : (id == 2) ? g_core->tev_c1 : NULL;
    if (!dst) return;
    if (!g_core->ui_tev || memcmp(dst, v, sizeof(v)) != 0) {
        gx_flush_pending(g_core);
        memcpy(dst, v, sizeof(v));
        g_core->ui_tev = 1;
    }
}

/* The 3D path (avdisp) never calls GXSetTevColor; its materials expect the
 * plain vertexcolor*texture pipeline.  avdisp material setup and the frame
 * boundary call this to leave UI TEV mode. */
void gx_ui_tev_disable(void) {
    gxfx_leave();
    if (g_core && g_core->ui_tev) {
        gx_flush_pending(g_core);
        g_core->ui_tev = 0;
    }
}

void GXSetTevKColor(int id, GXColor color) {
    gxfx_kcolor(id, color);
}

void GXSetTevKColorSel(int stage, int sel) {
    gxfx_ksel(stage, sel, 0);
}

void GXSetTevKAlphaSel(int stage, int sel) {
    gxfx_ksel(stage, sel, 1);
}

void GXSetTevSwapMode(int stage, int ras, int tex) { gxfx_swap_mode(stage, ras, tex); }
void GXSetTevSwapMode_cached(int stage, int ras, int tex) { gxfx_swap_mode(stage, ras, tex); }
void GXSetTevSwapModeTable(int id, int r, int g, int b, int a) { gxfx_swap_table(id, r, g, b, a); }
void GXSetTevSwapModeTable_cached(int id, int r, int g, int b, int a) { gxfx_swap_table(id, r, g, b, a); }

void GXSetTexCoordGen2(int dst, int func, int src, int mtx, int normalize, int postMtx) {
    tex_rec_texgen(dst, func, src, normalize, postMtx);
    if (g_core && dst >= 0 && dst < 8 &&
        g_core->current_tex_coord_gen[dst] != mtx) {
        gx_flush_pending(g_core);
        g_core->current_tex_coord_gen[dst] = mtx;
    }
}

void GXSetTexCoordGen(int dst, int func, int src, int mtx) {
    GXSetTexCoordGen2(dst, func, src, mtx, 0, 125 /* GX_PTIDENTITY */);
}

void GXSetTexCoordGen2_cached(int dst, int func, int src, int mtx, int normalize, int postMtx) {
    GXSetTexCoordGen2(dst, func, src, mtx, normalize, postMtx);
}

void GXLoadTexMtxImm(const Mtx mtx, int id, int type) {
    /* GX texmtx ids: GX_TEXMTX0=30, then every 3; GX_IDENTITY=60 */
    int slot = (id >= 30) ? (id - 30) / 3 : id;
    /* GX_PTTEXMTX0..19 (64..121): dual-tex post matrices, a separate bank */
    if (tex_load_post_mtx(mtx, id)) return;
    if (g_core) core_load_texture_matrix(g_core, mtx, slot);
}

void GXLoadPosMtxImm(const Mtx mtx, int id) {
    if (g_core) core_load_modelview_matrix(g_core, mtx, id);
}

void GXLoadNrmMtxImm(const Mtx mtx, int id) {
    /* GXFX: normal matrices are their own XF slots, separate from the
     * position matrix of the same id */
    gxfx_load_nrm(mtx, id);
}

/* ---- Texture lifetime ----
 * Backend textures are cached in records keyed by identity (image ptr, dims,
 * fmt, wraps, mip) and checked against a content key.  A record is retired
 * when its memory is rewritten, freed (pc_tex_mem_freed_hook) or, over the
 * budget, left unbound; a GXTexObj holds record id + generation, so binding a
 * stale one re-inits it from its image pointer, as the GC reads memory at
 * draw time.  EFB-copy textures (s_tc) use ids TC_ID_BASE + slot.
 * See docs/d3d12.md, Texture lifetime. */
#define TC_ID_BASE 0xFFFF0000u
_Static_assert(sizeof(GXTexObj) <= 32, "GXTexObj lives in the game's 32-byte GXTexObj");
#define TEX_HASH_SIZE 8192u
#define TEX_FREE_DELAY 256      /* retired records are reused FIFO after this many */
#define TEX_EVICT_AGE 1800      /* frames unbound before budget eviction */

typedef struct {
    void* handle;               /* backend texture; NULL = retired / free */
    const void* image;
    uint64_t key;               /* content key */
    uint32_t gen;               /* odd = live; bumped on retire and on reuse */
    uint32_t bytes;             /* GC texel bytes from image */
    uint32_t last_used;         /* s_tex_frame of the last init / bind */
    int hnext;                  /* identity hash chain: next index + 1, 0 = end */
    int fnext;                  /* free list: next index, -1 = end */
    uint16_t w, h;
    uint8_t fmt, wrapS, wrapT, mip;
    uint8_t lod_set, lod_min_f, lod_mag_f, lod_aniso;
    float lod_min, lod_max, lod_bias;
} TexRec;

static TexRec* s_tr;
static int s_ntr, s_ctr;
static int s_tr_free_head = -1, s_tr_free_tail = -1, s_tr_nfree;
static int s_th[TEX_HASH_SIZE];          /* identity hash -> record index + 1 */
static uint32_t s_tex_frame;
static void** s_tdq;                     /* retired handles awaiting destroy */
static int s_ntdq, s_ctdq;
static long long s_tex_budget = -1;      /* texels (SMB2_TEX_BUDGET in MB of RGBA8) */
static int s_tex_debug = -1;
static long long g_tex_retired[3];       /* superseded, freed, evicted */
long long g_tex_destroyed;

/* freed GC memory ranges reported by os_shim.c, applied before the next
 * texture init and at frame start */
#define TEX_MAX_FREED 64
static struct { uintptr_t a, b; } s_freed[TEX_MAX_FREED];
static int s_nfreed, s_freed_all;
static void tex_apply_freed(void);
static void tex_mem_freed(const void* p, uint32_t bytes);
/* called by os_shim.c (the game's heaps) */
void (*pc_tex_mem_freed_hook)(const void* p, uint32_t bytes) = tex_mem_freed;

/* texel bytes per 8x8 block of w*h, by GX format (approximate: ignores
 * tile padding, which only over-reads within the same allocation) */
static uint32_t gx_image_bytes(uint16_t w, uint16_t h, int fmt)
{
    uint32_t texels = (uint32_t)w * h;
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: return texels / 2;  /* I4, C4, CMPR */
    case 0x01: case 0x02: case 0x09: return texels;      /* I8, IA4, C8 */
    case 0x06:                       return texels * 4;  /* RGBA8 */
    default:                         return texels * 2;  /* 16bpp formats */
    }
}

static uint64_t texcache_key(const void *image, uint16_t w, uint16_t h,
                             int fmt, int wrapS, int wrapT, uint8_t mip)
{
    const uint8_t *p = (const uint8_t *)image;
    uint32_t len = gx_image_bytes(w, h, fmt);
    uint64_t k = 0xcbf29ce484222325ull;
    uint32_t i;
    /* sample three 64-byte windows: start, middle, end.  A glyph page whose
     * buffer is freed and reused for another glyph often has an identical
     * (blank) first tile row, so the head alone is not a safe fingerprint. */
    for (i = 0; i < 64 && i < len; i++)
        k = (k ^ p[i]) * 0x100000001b3ull;
    if (len > 128) {
        uint32_t mid = (len / 2) & ~3u;
        for (i = 0; i < 64 && mid + i < len; i++)
            k = (k ^ p[mid + i]) * 0x100000001b3ull;
    }
    if (len > 64) {
        uint32_t tail = len - 64;
        for (i = 0; i < 64; i++)
            k = (k ^ p[tail + i]) * 0x100000001b3ull;
    }
    k ^= (uint64_t)len * 0x9E3779B97F4A7C15ull;
    k ^= (uint64_t)(uintptr_t)image;
    k ^= (uint64_t)w << 32 | (uint64_t)h << 48;
    k ^= (uint64_t)(fmt & 0xFF) << 16 | (uint64_t)(wrapS & 3) << 24 |
         (uint64_t)(wrapT & 3) << 26 | (uint64_t)(mip & 1) << 28;
    return k ? k : 1;
}

static unsigned tex_ident_hash(const void* image, uint16_t w, uint16_t h, int fmt,
                               int wrapS, int wrapT, uint8_t mip) {
    uint64_t k = (uint64_t)(uintptr_t)image * 0x9E3779B97F4A7C15ull;
    k ^= ((uint64_t)w << 16 | h) * 0xC2B2AE3D27D4EB4Full;
    k ^= (uint64_t)((fmt & 0xFF) | (wrapS & 3) << 8 | (wrapT & 3) << 10 | (mip & 1) << 12) << 40;
    k ^= k >> 29;
    return (unsigned)(k % TEX_HASH_SIZE);
}

static void tex_dbg_init(void) {
    if (s_tex_debug < 0) {
        const char* e = getenv("SMB2_TEX_DEBUG");
        s_tex_debug = e ? atoi(e) : 0;
    }
    if (s_tex_budget < 0) {
        const char* e = getenv("SMB2_TEX_BUDGET");
        long long mb = e ? atoll(e) : 0;
        if (mb <= 0) mb = 256;
        s_tex_budget = mb * (1024 * 1024 / 4);
    }
}

static void tex_retire(int i, int why) {
    TexRec* r = &s_tr[i];
    int* pp;
    if (!r->handle) return;
    pp = &s_th[tex_ident_hash(r->image, r->w, r->h, r->fmt, r->wrapS, r->wrapT, r->mip)];
    while (*pp && *pp != i + 1) pp = &s_tr[*pp - 1].hnext;
    if (*pp) *pp = r->hnext;
    r->hnext = 0;
    if (s_ntdq == s_ctdq) {
        int nc = s_ctdq ? s_ctdq * 2 : 256;
        void** nq = (void**)realloc(s_tdq, (size_t)nc * sizeof *nq);
        if (nq) { s_tdq = nq; s_ctdq = nc; }
    }
    if (s_ntdq < s_ctdq) s_tdq[s_ntdq++] = r->handle;   /* else leaked, never freed early */
    g_tex_alive--;
    g_tex_alive_texels -= (long long)r->w * r->h;
    g_tex_retired[why]++;
    r->handle = NULL;
    r->gen++;          /* even: retired; GXTexObjs holding gen - 1 are stale */
    r->fnext = -1;
    if (s_tr_free_tail >= 0) s_tr[s_tr_free_tail].fnext = i; else s_tr_free_head = i;
    s_tr_free_tail = i;
    s_tr_nfree++;
}

static int tex_new_rec(void) {
    int i;
    if (s_tr_nfree > TEX_FREE_DELAY) {
        i = s_tr_free_head;
        s_tr_free_head = s_tr[i].fnext;
        if (s_tr_free_head < 0) s_tr_free_tail = -1;
        s_tr_nfree--;
        s_tr[i].gen++;  /* odd again */
        s_tr[i].lod_set = 0;
        return i;
    }
    if (s_ntr == s_ctr) {
        int nc = s_ctr ? s_ctr * 2 : 1024;
        TexRec* n = (TexRec*)realloc(s_tr, (size_t)nc * sizeof *n);
        if (!n) return -1;
        s_tr = n; s_ctr = nc;
    }
    i = s_ntr++;
    memset(&s_tr[i], 0, sizeof s_tr[i]);
    s_tr[i].gen = 1;
    return i;
}

static void tex_obj_set(GXTexObj* obj, void* image, uint16_t w, uint16_t h, int fmt,
                        int wrapS, int wrapT, uint8_t mip, uint32_t id, uint32_t gen) {
    obj->width = w;
    obj->height = h;
    obj->format = (uint32_t)fmt;
    obj->wrapS = (uint8_t)wrapS;
    obj->wrapT = (uint8_t)wrapT;
    obj->mipmap = mip;
    obj->tex_id = id;
    obj->tex_gen = gen;
    obj->image_data = image;
}

static void GXInitTexObj_impl(GXTexObj* obj, void* image, uint16_t width, uint16_t height, int format, int wrapS, int wrapT, uint8_t mipmap);
void GXInitTexObj(GXTexObj* obj, void* image, uint16_t width, uint16_t height, int format, int wrapS, int wrapT, uint8_t mipmap) {
    PERF_BEGIN(t0);
    PERF_COUNT(PERF_C_TEXINIT, 1);
    GXInitTexObj_impl(obj, image, width, height, format, wrapS, wrapT, mipmap);
    PERF_END(t0, PERF_T_TEXINIT);
}
static void GXInitTexObj_impl(GXTexObj* obj, void* image, uint16_t width, uint16_t height, int format, int wrapS, int wrapT, uint8_t mipmap) {
    uint64_t key;
    unsigned hs;
    int i;
    TexRec* r;
    void* handle;
    if (!g_core || !obj) return;
    tex_dbg_init();
    if (s_nfreed || s_freed_all) tex_apply_freed();
    /* memory last written by GXCopyTex -> its render target */
    if (texcopy_init_obj(obj, image, width, height, format, wrapS, wrapT, mipmap))
        return;
    tex_obj_set(obj, image, width, height, format, wrapS, wrapT, mipmap, 0, 0);
    if (!width || !height) return;
    mipmap = mipmap != 0;   /* the record identity (tex_retire rehashes it) */
    /* no texels (never seen from the game): transparent black, keyed like any
     * other texture at address 0 */
    key = image ? texcache_key(image, width, height, format, wrapS, wrapT, mipmap) : 1;
    hs = tex_ident_hash(image, width, height, format, wrapS, wrapT, mipmap);
    for (i = s_th[hs] - 1; i >= 0; i = s_tr[i].hnext - 1) {
        r = &s_tr[i];
        if (r->image == image && r->w == width && r->h == height && r->fmt == (uint8_t)format &&
            r->wrapS == (uint8_t)wrapS && r->wrapT == (uint8_t)wrapT && r->mip == mipmap)
            break;
    }
    if (i >= 0) {
        if (s_tr[i].key == key) {
            s_tr[i].last_used = s_tex_frame;
            obj->tex_id = (uint32_t)i + 1;
            obj->tex_gen = s_tr[i].gen;
            return;
        }
        tex_retire(i, 0);   /* the memory now holds other texels */
    }
    handle = core_create_handle(image, width, height, (uint32_t)format, (uint8_t)wrapS,
                                (uint8_t)wrapT, mipmap);
    if (!handle) return;
    i = tex_new_rec();
    if (i < 0) { g_gpu->tex_destroy(handle); return; }
    r = &s_tr[i];
    r->handle = handle;
    r->image = image;
    r->key = key;
    r->bytes = image ? gx_image_bytes(width, height, format) : 0;
    r->last_used = s_tex_frame;
    r->w = width; r->h = height;
    r->fmt = (uint8_t)format; r->wrapS = (uint8_t)wrapS; r->wrapT = (uint8_t)wrapT;
    r->mip = mipmap;
    r->hnext = s_th[hs];
    s_th[hs] = i + 1;
    g_tex_alive++;
    g_tex_alive_texels += (long long)width * height;
    obj->tex_id = (uint32_t)i + 1;
    obj->tex_gen = r->gen;
}

static void* texcopy_handle(unsigned e);

/* the backend texture of a GXTexObj, re-initing a stale one */
static void* tex_resolve(GXTexObj* obj) {
    uint32_t id = obj->tex_id;
    TexRec* r;
    if (!id) return NULL;
    if (id >= TC_ID_BASE) return texcopy_handle(id - TC_ID_BASE);
    if (id > (uint32_t)s_ntr) return NULL;
    r = &s_tr[id - 1];
    if (r->gen == obj->tex_gen && r->handle) {
        r->last_used = s_tex_frame;
        return r->handle;
    }
    /* retired since this object was inited (odd gens are issued, so an even
     * or newer gen is not one: garbage is left alone) */
    if ((obj->tex_gen & 1) && obj->tex_gen < r->gen && obj->image_data) {
        int lod = r->gen == obj->tex_gen + 1 && r->lod_set;
        uint8_t mf = r->lod_min_f, gf = r->lod_mag_f, an = r->lod_aniso;
        float mn = r->lod_min, mx = r->lod_max, bi = r->lod_bias;
        if (s_tex_debug & 2)
            fprintf(stderr, "[tex] stale texObj %p (img %p %ux%u fmt %u) re-inited\n",
                    (void*)obj, obj->image_data, obj->width, obj->height, obj->format);
        GXInitTexObj_impl(obj, obj->image_data, obj->width, obj->height, (int)obj->format,
                          obj->wrapS, obj->wrapT, obj->mipmap);
        if (!obj->tex_id || obj->tex_id >= TC_ID_BASE) return obj->tex_id ? tex_resolve(obj) : NULL;
        r = &s_tr[obj->tex_id - 1];
        if (lod && !r->lod_set && r->handle) {
            r->lod_set = 1;
            r->lod_min_f = mf; r->lod_mag_f = gf; r->lod_aniso = an;
            r->lod_min = mn; r->lod_max = mx; r->lod_bias = bi;
            g_gpu->tex_lod(r->handle, r->mip, mf, gf, mn, mx, bi, an);
        }
        return r->handle;
    }
    return NULL;
}

/* os_shim.c: [p, p + bytes) of GC heap memory was freed (bytes 0: all) */
static void tex_mem_freed(const void* p, uint32_t bytes) {
    uintptr_t a = (uintptr_t)p, b = a + bytes;
    int i;
    if (!bytes) { s_freed_all = 1; return; }
    for (i = 0; i < s_nfreed; i++)
        if (a <= s_freed[i].b && b >= s_freed[i].a) {   /* merge touching ranges */
            if (a < s_freed[i].a) s_freed[i].a = a;
            if (b > s_freed[i].b) s_freed[i].b = b;
            return;
        }
    if (s_nfreed == TEX_MAX_FREED) tex_apply_freed();
    s_freed[s_nfreed].a = a;
    s_freed[s_nfreed].b = b;
    s_nfreed++;
}

static void tex_apply_freed(void) {
    int i, j;
    for (i = 0; i < s_ntr; i++) {
        TexRec* r = &s_tr[i];
        uintptr_t a, b;
        if (!r->handle || !r->image) continue;
        a = (uintptr_t)r->image; b = a + r->bytes;
        if (s_freed_all) {   /* OSInitAlloc: every heap is gone */
            if (a >= 0x80000000u && a < 0x81800000u) tex_retire(i, 1);
            continue;
        }
        for (j = 0; j < s_nfreed; j++)
            if (a < s_freed[j].b && b > s_freed[j].a) {
                if (s_tex_debug & 4)
                    fprintf(stderr, "[tex] frame %u: %p %ux%u freed with [%p, %p)\n", s_tex_frame,
                            r->image, r->w, r->h, (void*)s_freed[j].a, (void*)s_freed[j].b);
                tex_retire(i, 1);
                break;
            }
    }
    s_nfreed = 0;
    s_freed_all = 0;
}

/* frame start: apply frees, evict over budget, destroy what was retired */
static void tex_frame(GxCore* gl) {
    int i, n = 0, s;
    s_tex_frame++;
    tex_dbg_init();
    if (s_nfreed || s_freed_all) tex_apply_freed();
    if (g_tex_alive_texels > s_tex_budget && (s_tex_frame & 63) == 0) {
        /* oldest first is not needed: anything this old is not in use */
        for (i = 0; i < s_ntr && g_tex_alive_texels > s_tex_budget; i++) {
            TexRec* r = &s_tr[i];
            if (r->handle && s_tex_frame - r->last_used > TEX_EVICT_AGE) tex_retire(i, 2);
        }
    }
    if ((s_tex_debug & 1) && s_tex_frame % 60 == 0)
        fprintf(stderr, "[tex] frame %u: alive %lld (%.1fM texels), retired superseded %lld "
                "freed %lld evicted %lld, destroyed %lld, records %d\n", s_tex_frame,
                g_tex_alive, g_tex_alive_texels / 1e6, g_tex_retired[0], g_tex_retired[1],
                g_tex_retired[2], g_tex_destroyed, s_ntr);
    if (!s_ntdq) return;
    gx_flush_pending(gl);
    for (i = 0; i < s_ntdq; i++) {
        void* h = s_tdq[i];
        for (s = 0; s < 8; s++) if (gl->bound_textures[s] == h) break;
        if (s < 8) { s_tdq[n++] = h; continue; }   /* still bound: keep */
        g_gpu->tex_destroy(h);
        g_tex_destroyed++;
    }
    s_ntdq = n;
}

/* GXInitTexObjLOD: GX filters / LOD clamp / bias / anisotropy applied to the
 * backend texture behind the object.  GX filter enum: 0 NEAR, 1 LINEAR,
 * 2 NEAR_MIP_NEAR, 3 LIN_MIP_NEAR, 4 NEAR_MIP_LIN, 5 LIN_MIP_LIN.
 * maxAniso: 0 = 1x, 1 = 2x, 2 = 4x. */
void GXInitTexObjLOD(GXTexObj* obj, int minFilt, int magFilt, float minLod, float maxLod,
                     float lodBias, uint8_t biasClamp, uint8_t doEdgeLod, int maxAniso) {
    (void)biasClamp; (void)doEdgeLod;
    void* h;
    if (!obj || !g_core) return;
    h = tex_resolve(obj);
    if (!h) return;
    if (minFilt < 0 || minFilt > 5) minFilt = 1;
    if (obj->tex_id < TC_ID_BASE) {   /* kept for re-inits after a retire */
        TexRec* r = &s_tr[obj->tex_id - 1];
        r->lod_set = 1;
        r->lod_min_f = (uint8_t)minFilt; r->lod_mag_f = (uint8_t)magFilt;
        r->lod_aniso = (uint8_t)maxAniso;
        r->lod_min = minLod; r->lod_max = maxLod; r->lod_bias = lodBias;
    }
    g_gpu->tex_lod(h, obj->mipmap, minFilt, magFilt, minLod, maxLod, lodBias, maxAniso);
}

void GXLoadTexObj(GXTexObj* obj, int map) {
    if (g_mtx_trace > 0 && obj)
        fprintf(stderr, "[trace] loadtex map=%d id=%08x %ux%u fmt=%u img=%p\n",
                map, obj->tex_id,
                obj->width, obj->height, obj->format, obj->image_data);
    texcopy_fix_obj(obj);
    if (g_core) core_bind_texture(g_core, map, obj);
}

void GXLoadTexObj_cached(GXTexObj* obj, int map) {
    GXLoadTexObj(obj, map);
}

void GXSetCurrentMtx(int id) {
    if (g_core && g_core->current_mtx_idx != id) {
        gx_flush_pending(g_core);
        g_core->current_mtx_idx = id;
    }
}

/* Game-side projection API.  g_something_with_GXSetProjectionv(m, type) sets
 * the projection from a full row-major Mtx44 (type: 0 perspective, 1 ortho);
 * GXGetProjectionv/g_smth_with_projection_mtx save and restore it through
 * the SDK's 7-float form { type, m00, a, m11, b, m22, m23 } where a/b are
 * m02/m12 for perspective and m03/m13 for ortho. */
void g_something_with_GXSetProjectionv(const Mtx44 m, int type) {
    Mtx44 canon;
    if (!g_core) return;
    gx_flush_pending(g_core); /* don't draw queued verts with the new proj */
    /* GC hardware reads only 6 elements + the type flag and synthesizes the
     * rest (ortho: w = 1), so the unused cells of the game's matrix can hold
     * stale perspective rows.  Rebuild the matrix canonically. */
    memset(canon, 0, sizeof canon);
    canon[0][0] = m[0][0];
    canon[1][1] = m[1][1];
    canon[2][2] = m[2][2];
    canon[2][3] = m[2][3];
    if (type) { /* ortho */
        canon[0][3] = m[0][3];
        canon[1][3] = m[1][3];
        canon[3][3] = 1.0f;
    } else {    /* perspective */
        canon[0][2] = m[0][2];
        canon[1][2] = m[1][2];
        canon[3][2] = -1.0f;
    }
    memcpy(g_core->projection_matrix, canon, sizeof(Mtx44));
    g_core->projection_type = type;
    g_core->proj_vec[0] = (float)type;
    g_core->proj_vec[1] = m[0][0];
    g_core->proj_vec[2] = type ? m[0][3] : m[0][2];
    g_core->proj_vec[3] = m[1][1];
    g_core->proj_vec[4] = type ? m[1][3] : m[1][2];
    g_core->proj_vec[5] = m[2][2];
    g_core->proj_vec[6] = m[2][3];
}

void GXGetProjectionv(float* p) {
    if (!g_core) return;
    memcpy(p, g_core->proj_vec, sizeof(g_core->proj_vec));
}

void g_smth_with_projection_mtx(const float* p) {
    Mtx44 m;
    int type = (int)p[0];

    memset(m, 0, sizeof m);
    m[0][0] = p[1];
    m[1][1] = p[3];
    m[2][2] = p[5];
    m[2][3] = p[6];
    if (type) { /* ortho */
        m[0][3] = p[2];
        m[1][3] = p[4];
        m[3][3] = 1.0f;
    } else {    /* perspective */
        m[0][2] = p[2];
        m[1][2] = p[4];
        m[3][2] = -1.0f;
    }
    g_something_with_GXSetProjectionv(m, type);
}

void GXBegin(int type, int vtxFmt, uint16_t nverts) {
    if (g_core) core_begin(g_core, type, vtxFmt, nverts);
}

void GXEnd(void) {
    if (g_core) core_end(g_core);
}

void GXPosition3f32(float x, float y, float z) {
    if (g_core) core_position3f32(g_core, x, y, z);
}

void GXNormal3f32(float x, float y, float z) {
    if (g_core) core_normal3f32(g_core, x, y, z);
}

void GXColor4u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (g_core) core_color4u8(g_core, r, g, b, a);
}

void GXTexCoord2f32(float s, float t) {
    if (g_core) core_texcoord2f32(g_core, s, t);
}

/* Write-gather pipe sinks the decomp's gx.h routes immediate-mode vertex
 * writes to.  They MUST live in a file that does not include the decomp's
 * gx.h: there GXPosition3f32 etc. are static inlines that call pc_gx_*,
 * so defining the sinks in such a file makes them call themselves. */
void pc_gx_pos3f(float x, float y, float z) { GXPosition3f32(x, y, z); }
void pc_gx_norm3f(float x, float y, float z) { GXNormal3f32(x, y, z); }
void pc_gx_color4u8(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { GXColor4u8(r, g, b, a); }
void pc_gx_tex2f(float s, float t) { GXTexCoord2f32(s, t); }
void pc_gx_pos3s16(int16_t x, int16_t y, int16_t z) { GXPosition3f32(x, y, z); }
void pc_gx_tex2s16(int16_t s, int16_t t) { GXTexCoord2f32(s, t); }
void pc_gx_fifo_s8(int8_t v) { (void)v; }

/* Matrix math helpers */
void C_MTXPerspective(Mtx44 m, float fovY, float aspect, float n, float f) {
    float ymax = n * tanf(fovY * 3.14159265f / 360.0f);
    float ymin = -ymax;
    float xmin = ymin * aspect;
    float xmax = ymax * aspect;

    float width = xmax - xmin;
    float height = ymax - ymin;
    float depth = f - n;
    float q = -n / depth;
    float qn = -f * n / depth;

    /* row-major, GC (SDK) depth convention: z/w = -1 at near, 0 at far.
     * Game code (GXGetProjectionv users) derives near/far from this form;
     * the vertex shader maps it to GL depth. */
    memset(m, 0, sizeof(Mtx44));
    m[0][0] = 2.0f * n / width;
    m[1][1] = 2.0f * n / height;
    m[2][2] = q;
    m[2][3] = qn;
    m[3][2] = -1.0f;
}

void C_MTXOrtho(Mtx44 m, float top, float bottom, float left, float right, float n, float f) {
    MTXIdentity(m);
    m[0][0] = 2.0f / (right - left);
    m[1][1] = 2.0f / (top - bottom);
    m[2][2] = -1.0f / (f - n);              /* GC (SDK) convention, as above */
    m[0][3] = -(right + left) / (right - left);
    m[1][3] = -(top + bottom) / (top - bottom);
    m[2][3] = -f / (f - n);
}

void C_MTXLookAt(Mtx m, const Vec* camPos, const Vec* up, const Vec* target) {
    Vec f, s, u;

    f.x = target->x - camPos->x;
    f.y = target->y - camPos->y;
    f.z = target->z - camPos->z;
    float len = sqrtf(f.x*f.x + f.y*f.y + f.z*f.z);
    if (len > 0) { f.x /= len; f.y /= len; f.z /= len; }

    s.x = f.y * up->z - f.z * up->y;
    s.y = f.z * up->x - f.x * up->z;
    s.z = f.x * up->y - f.y * up->x;
    len = sqrtf(s.x*s.x + s.y*s.y + s.z*s.z);
    if (len > 0) { s.x /= len; s.y /= len; s.z /= len; }

    u.x = s.y * f.z - s.z * f.y;
    u.y = s.z * f.x - s.x * f.z;
    u.z = s.x * f.y - s.y * f.x;

    m[0][0] = s.x; m[0][1] = s.y; m[0][2] = s.z; m[0][3] = -(s.x*camPos->x + s.y*camPos->y + s.z*camPos->z);
    m[1][0] = u.x; m[1][1] = u.y; m[1][2] = u.z; m[1][3] = -(u.x*camPos->x + u.y*camPos->y + u.z*camPos->z);
    m[2][0] = -f.x; m[2][1] = -f.y; m[2][2] = -f.z; m[2][3] = f.x*camPos->x + f.y*camPos->y + f.z*camPos->z;
}

void MTXIdentity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0][0] = 1.0f; m[1][1] = 1.0f; m[2][2] = 1.0f;
}

void MTXTrans(Mtx m, float x, float y, float z) {
    m[0][3] = x;
    m[1][3] = y;
    m[2][3] = z;
}

void MTXScale(Mtx m, float x, float y, float z) {
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

void MTXRotRad(Mtx m, int axis, float rad) {
    float c = cosf(rad);
    float s = sinf(rad);
    MTXIdentity(m);
    switch (axis) {
        case 0: /* X */
            m[1][1] = c; m[1][2] = -s;
            m[2][1] = s; m[2][2] = c;
            break;
        case 1: /* Y */
            m[0][0] = c; m[0][2] = s;
            m[2][0] = -s; m[2][2] = c;
            break;
        case 2: /* Z */
            m[0][0] = c; m[0][1] = -s;
            m[1][0] = s; m[1][1] = c;
            break;
    }
}

void MTXConcat(const Mtx a, const Mtx b, Mtx ab) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            ab[i][j] = a[i][0]*b[0][j] + a[i][1]*b[1][j] + a[i][2]*b[2][j];
        }
    }
}

void MTXInverse(const Mtx src, Mtx inv) {
    /* Simple inverse for orthonormal matrices (rotation + translation) */
    /* Transpose rotation part */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            inv[i][j] = src[j][i];
        }
    }
    /* Inverse translation */
    inv[0][3] = -(inv[0][0]*src[0][3] + inv[0][1]*src[1][3] + inv[0][2]*src[2][3]);
    inv[1][3] = -(inv[1][0]*src[0][3] + inv[1][1]*src[1][3] + inv[1][2]*src[2][3]);
    inv[2][3] = -(inv[2][0]*src[0][3] + inv[2][1]*src[1][3] + inv[2][2]*src[2][3]);
}

void MTXTranspose(const Mtx src, Mtx dst) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            dst[i][j] = src[j][i];
        }
    }
    dst[0][3] = dst[1][3] = dst[2][3] = 0.0f;
}

void MTXMultVec(const Mtx m, const Vec* src, Vec* dst) {
    dst->x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z + m[0][3];
    dst->y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z + m[1][3];
    dst->z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z + m[2][3];
}

void MTXMultVecSR(const Mtx m, const Vec* src, Vec* dst) {
    dst->x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z;
    dst->y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z;
    dst->z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z;
}
/* ======================================================================
 * GXFX: GX lighting channel + TEV emulation
 * ----------------------------------------------------------------------
 * Covers GXSetChanCtrl/MatColor/AmbColor, GX light objects, the normal
 * matrix, and a generic N-stage TEV (GXSetTevOrder/ColorIn/AlphaIn/ColorOp/
 * AlphaOp/KColor/Op) plus the alpha compare, for the decomp code that
 * configures GX directly (nl2ngc.c NL models: items/bananas/effects/stage
 * intro objects, polydisp, bg_*, ...).  NL binds its textures to maps
 * 7,6,... and relies on lighting with register colors.
 *
 * Mode selection (shader):  ui_tev (sprite/text path, GXSetTevColor) wins,
 * then "gx mode" (set by any channel/TEV setter below), else the legacy
 * vertexcolor*tex0 path that avdisp_shim relies on (avdisp material setup
 * and the frame boundary call gx_ui_tev_disable(), which leaves gx mode).
 * ====================================================================== */


/* layout we keep inside the SDK's opaque 64-byte GXLightObj */
typedef struct {
    float pos[3];
    float dir[3];
    float a[3];   /* angular (spot) attenuation coefficients */
    float k[3];   /* distance attenuation coefficients */
    uint8_t color[4];
} GxfxLight;

typedef struct { uint32_t dummy[16]; } GxfxLightObj;

static struct {
    int mode;            /* 1 = draws use the GX channel/TEV path */
    int dirty;
    /* lighting channels: [0] = COLOR0, [1] = ALPHA0 */
    int chan_enable[2], chan_matsrc[2], chan_ambsrc[2], chan_mask[2];
    int chan_diff[2], chan_attn[2];
    float mat[4], amb[4];
    GxfxLight lights[8];
    Mtx nrm[32];
    /* TEV */
    int n_stages;
    int cin[16][4], ain[16][4];
    int cop[16][4], aop[16][4];  /* op, bias, scale, clamp|outreg<<1 */
    int ord[16][3];              /* texcoord, texmap(-1), raster on */
    int ksel[16], kasel[16];
    int swap[16][2];             /* GXSetTevSwapMode: ras, tex table */
    int swaptab[4][4];           /* GXSetTevSwapModeTable: r, g, b, a src */
    float kcolor[4][4];
    float reg[4][4];             /* PREV, REG0..2 */
    /* indirect texturing (GXIND section) */
    int ind_n;                   /* GXSetNumIndStages */
    int ind_ord[4][2];           /* ind stage: texcoord, texmap */
    int ind_scale[4][2];         /* GXIndTexScale exponents (s, t) */
    float ind_mtx[3][2][3];      /* GX_ITM_0..2, s0.10-quantized */
    int ind_exp[3];              /* matrix scale exponent */
    int tev_ind[16][4];          /* packed per TEV stage, 0 = direct */
    int inited;
} s_gx;

/* TEV swap of stage s for the shaders (ord.w): the swizzles of the texture
 * (bits 0-7) and rasterized (bits 8-15) colour, 2 bits per output channel
 * r,g,b,a, each XOR the identity 0xE4 so that 0 = no swap. */
static int gxfx_swap_bits(int s) {
    int k, bits = 0;
    for (k = 0; k < 2; k++) {
        const int* t = s_gx.swaptab[s_gx.swap[s][k] & 3];
        int sw = t[0] | t[1] << 2 | t[2] << 4 | t[3] << 6;
        bits |= (sw ^ 0xE4) << (k ? 0 : 8);
    }
    return bits;
}

static void gxfx_touch(void) {
    if (g_core) gx_flush_pending(g_core);
    s_gx.dirty = 1;
}

/* any channel/TEV setter: the next draws are "configured GX" draws */
static void gxfx_enter(void) {
    if (!s_gx.mode || (g_core && g_core->ui_tev)) {
        gxfx_touch();
        s_gx.mode = 1;
        if (g_core) g_core->ui_tev = 0;
    } else {
        gxfx_touch();
    }
}

static void gxfx_set_tev_op_inputs(int s, int mode) {
    /* GXSetTevOp: stage 0 uses the rasterized color, later stages PREV */
    int carg = (s == 0) ? 10 : 0, aarg = (s == 0) ? 5 : 0;
    int c[4], a[4];
    switch (mode) {
    case 0: c[0]=15; c[1]=8;  c[2]=carg; c[3]=15;   a[0]=7; a[1]=4; a[2]=aarg; a[3]=7;    break; /* MODULATE */
    case 1: c[0]=carg; c[1]=8; c[2]=9; c[3]=15;     a[0]=7; a[1]=7; a[2]=7; a[3]=aarg;    break; /* DECAL */
    case 2: c[0]=carg; c[1]=12; c[2]=8; c[3]=15;    a[0]=7; a[1]=4; a[2]=aarg; a[3]=7;    break; /* BLEND */
    case 3: c[0]=15; c[1]=15; c[2]=15; c[3]=8;      a[0]=7; a[1]=7; a[2]=7; a[3]=4;       break; /* REPLACE */
    default: c[0]=15; c[1]=15; c[2]=15; c[3]=carg;  a[0]=7; a[1]=7; a[2]=7; a[3]=aarg;    break; /* PASSCLR */
    }
    memcpy(s_gx.cin[s], c, sizeof c);
    memcpy(s_gx.ain[s], a, sizeof a);
}

static void gxfx_defaults(void) {
    int i;
    s_gx.n_stages = 1;
    for (i = 0; i < 16; i++) {
        gxfx_set_tev_op_inputs(i, i == 0 ? 0 : 4);
        s_gx.cop[i][0] = s_gx.aop[i][0] = 0;
        s_gx.cop[i][1] = s_gx.aop[i][1] = 0;
        s_gx.cop[i][2] = s_gx.aop[i][2] = 0;
        s_gx.cop[i][3] = s_gx.aop[i][3] = 1;  /* clamp, out PREV */
        s_gx.ord[i][0] = i < 8 ? i : 0;
        s_gx.ord[i][1] = i < 8 ? i : -1;
        s_gx.ord[i][2] = 1;
        s_gx.ksel[i] = 0x0C;   /* GX_TEV_KCSEL_K0 */
        s_gx.kasel[i] = 0x1C;  /* GX_TEV_KASEL_K0_A */
        s_gx.swap[i][0] = s_gx.swap[i][1] = 0;
    }
    /* swap tables as the game leaves them: its GX-cache reset
     * g_clear_prev_GX_settings_something1 (asm 0x80006E54, run from the
     * avdisp init 0x800694F4, which the port replaces) sets
     * SWAP0 = RGBA, SWAP1 = RGBR, SWAP2 = RGBG, SWAP3 = RGBB, i.e. table
     * n > 0 takes alpha from channel n - 1 (not GXInit's RRRA/GGGA/BBBA).
     * The story-scene smoke (STORM_SMOKE, scnplay_effect.c) relies on it:
     * an opaque CMPR puff drawn with SWAP1, red = alpha. */
    for (i = 0; i < 4; i++) {
        s_gx.swaptab[i][0] = 0; s_gx.swaptab[i][1] = 1; s_gx.swaptab[i][2] = 2;
        s_gx.swaptab[i][3] = i == 0 ? 3 : i - 1;
    }
    for (i = 0; i < 2; i++) {
        s_gx.chan_enable[i] = 0;
        s_gx.chan_matsrc[i] = 1;  /* vertex */
        s_gx.chan_ambsrc[i] = 0;
        s_gx.chan_mask[i] = 0;
        s_gx.chan_diff[i] = 0;
        s_gx.chan_attn[i] = 2;
    }
    for (i = 0; i < 4; i++) s_gx.mat[i] = 1.0f;
    s_gx.amb[0] = s_gx.amb[1] = s_gx.amb[2] = 0.0f;
    s_gx.amb[3] = 1.0f;
    s_gx.dirty = 1;
}

static void gxfx_init_state(void) {
    if (!s_gx.inited) {
        int i;
        for (i = 0; i < 32; i++) {
            memset(s_gx.nrm[i], 0, sizeof(Mtx));
            s_gx.nrm[i][0][0] = s_gx.nrm[i][1][1] = s_gx.nrm[i][2][2] = 1.0f;
        }
        gxfx_defaults();
        s_gx.inited = 1;
    }
    s_gx.dirty = 1;
}

/* konst color for a stage from its K color/alpha selections */
static void gxfx_konst(int s, float out[4]) {
    int ks = s_gx.ksel[s], ka = s_gx.kasel[s];
    if (ks <= 7) {
        out[0] = out[1] = out[2] = (8 - ks) / 8.0f;
    } else if (ks >= 0x0C && ks <= 0x0F) {
        memcpy(out, s_gx.kcolor[ks - 0x0C], 3 * sizeof(float));
    } else if (ks >= 0x10 && ks <= 0x1F) {
        int id = (ks - 0x10) & 3, comp = (ks - 0x10) >> 2; /* R,G,B,A of K0..K3 */
        out[0] = out[1] = out[2] = s_gx.kcolor[id][comp];
    } else {
        out[0] = out[1] = out[2] = 1.0f;
    }
    if (ka <= 7) {
        out[3] = (8 - ka) / 8.0f;
    } else if (ka >= 0x10 && ka <= 0x1F) {
        int id = (ka - 0x10) & 3, comp = (ka - 0x10) >> 2;
        out[3] = s_gx.kcolor[id][comp];
    } else {
        out[3] = 1.0f;
    }
}

/* Whether a flushed batch takes the UI TEV shortcut (TEXC*C0 + C1).  The
 * shortcut models the 2D code's textured stage, so an untextured batch drawn
 * after the game set up the TEV through the setters (s_gx.mode) runs the
 * generic TEV instead (e.g. the speech-bubble tail, dialog.c fn_1_162260:
 * no texture, colour = C0, alpha = A0). */
static int gx_ui_tev_eff(const GxCore* gl) {
    int textured = gl->batch_has_texcoord && gl->bound_textures[0] != NULL;
    return gl->ui_tev && (textured || !s_gx.mode);
}

/* called at every batch flush: fill the GXFX parts of the shader constants */
static void gxfx_build(GxCore* gl) {
    int eff = s_gx.mode && !s_ui_eff;
    int i, valid = 0;
    s_dc.mode[0] = eff;
    /* alpha compare also applies to the UI TEV path, whatever the mode */
    s_dc.acmp[0] = gl->current_alpha_compare ? gl->current_alpha_comp0 : 7;
    s_dc.acmp[1] = gl->current_alpha_compare ? gl->current_alpha_comp1 : 7;
    s_dc.acmp[2] = gl->current_alpha_op;
    s_dc.acmp[3] = gl->current_alpha_ref0 | (gl->current_alpha_ref1 << 8);
    {
        int mv = gl->current_mtx_idx;
        if (mv < 0 || mv >= 32) mv = 0;
        mtx34_to_gl(s_gx.nrm[mv], s_dc.nrm);
    }
    for (i = 0; i < 8; i++)
        if (gl->bound_textures[i]) valid |= 1 << i;
    s_dc.tg2[1] = valid;
    if (!s_gx.dirty) return;
    s_gx.dirty = 0;
    s_tc_serial++;
    for (i = 0; i < 2; i++) {
        s_tevc.chan[i][0] = s_gx.chan_enable[i];
        s_tevc.chan[i][1] = s_gx.chan_matsrc[i];
        s_tevc.chan[i][2] = s_gx.chan_ambsrc[i];
        s_tevc.chan[i][3] = s_gx.chan_mask[i];
        s_tevc.chanfn[i][0] = s_gx.chan_diff[i];
        s_tevc.chanfn[i][1] = s_gx.chan_attn[i];
        s_tevc.chanfn[i][2] = s_tevc.chanfn[i][3] = 0;
    }
    memcpy(s_tevc.mat, s_gx.mat, sizeof(s_tevc.mat));
    memcpy(s_tevc.amb, s_gx.amb, sizeof(s_tevc.amb));
    for (i = 0; i < 8; i++) {
        const GxfxLight* L = &s_gx.lights[i];
        int j;
        for (j = 0; j < 3; j++) {
            s_tevc.lpos[i][j] = L->pos[j];
            s_tevc.ldir[i][j] = L->dir[j];
            s_tevc.lcos[i][j] = L->a[j];
            s_tevc.ldist[i][j] = L->k[j];
        }
        s_tevc.lpos[i][3] = s_tevc.ldir[i][3] = s_tevc.lcos[i][3] = s_tevc.ldist[i][3] = 0.0f;
        for (j = 0; j < 4; j++) s_tevc.lcol[i][j] = L->color[j] / 255.0f;
    }
    {
        int n = s_gx.n_stages < 1 ? 1 : (s_gx.n_stages > 8 ? 8 : s_gx.n_stages);
        for (i = 0; i < 8; i++) {
            s_tevc.ord[i][0] = s_gx.ord[i][0];
            s_tevc.ord[i][1] = s_gx.ord[i][1];
            s_tevc.ord[i][2] = s_gx.ord[i][2];
            s_tevc.ord[i][3] = gxfx_swap_bits(i);
            gxfx_konst(i, s_tevc.konst[i]);
        }
        s_tevc.misc[0] = n;
        memcpy(s_tevc.cin, s_gx.cin, sizeof(s_tevc.cin));
        memcpy(s_tevc.ain, s_gx.ain, sizeof(s_tevc.ain));
        memcpy(s_tevc.cop, s_gx.cop, sizeof(s_tevc.cop));
        memcpy(s_tevc.aop, s_gx.aop, sizeof(s_tevc.aop));
        memcpy(s_tevc.reg, s_gx.reg, sizeof(s_tevc.reg));
    }
    gxind_build(n_tev_clamped());
}

/* --trace-frame: one-line summary of the GX state */
static void gxfx_trace(void) {
    if (g_mtx_trace <= 0 || !s_gx.mode || !g_core || s_ui_eff) return;
    fprintf(stderr, "[trace]  gxfx n=%d ord0=%d,%d,%d cin0=%d,%d,%d,%d ain0=%d,%d,%d,%d "
            "chan=%d/%d/%d/%x fn=%d,%d mat=%.2f,%.2f,%.2f,%.2f amb=%.2f,%.2f,%.2f\n",
            s_gx.n_stages, s_gx.ord[0][0], s_gx.ord[0][1], s_gx.ord[0][2],
            s_gx.cin[0][0], s_gx.cin[0][1], s_gx.cin[0][2], s_gx.cin[0][3],
            s_gx.ain[0][0], s_gx.ain[0][1], s_gx.ain[0][2], s_gx.ain[0][3],
            s_gx.chan_enable[0], s_gx.chan_matsrc[0], s_gx.chan_ambsrc[0], s_gx.chan_mask[0],
            s_gx.chan_diff[0], s_gx.chan_attn[0],
            s_gx.mat[0], s_gx.mat[1], s_gx.mat[2], s_gx.mat[3],
            s_gx.amb[0], s_gx.amb[1], s_gx.amb[2]);
}

/* avdisp material setup / frame start: leave gx mode.  Also put the TEV
 * back roughly where the (unemulated) avdisp/GMA path would leave it on GC
 * (1 stage, texture * rasterized color, unlit vertex color), so the next
 * partially-configured GX draw doesn't inherit some earlier draw's setup. */
static void gxfx_leave(void) {
    int i;
    gxind_reset();
    /* every avdisp TEV stage sets its swap mode (0/0 except the alpha
     * layers, which the avdisp shim sets itself) */
    for (i = 0; i < 16; i++)
        gxfx_swap_mode(i, 0, 0);
    if (!s_gx.mode) return;
    if (g_core) gx_flush_pending(g_core);
    s_gx.mode = 0;
    s_gx.n_stages = 1;
    gxfx_set_tev_op_inputs(0, 0);
    s_gx.ord[0][0] = 0; s_gx.ord[0][1] = 0; s_gx.ord[0][2] = 1;
    s_gx.cop[0][0] = s_gx.cop[0][1] = s_gx.cop[0][2] = 0; s_gx.cop[0][3] = 1;
    s_gx.aop[0][0] = s_gx.aop[0][1] = s_gx.aop[0][2] = 0; s_gx.aop[0][3] = 1;
    for (i = 0; i < 2; i++) {
        s_gx.chan_enable[i] = 0;
        s_gx.chan_matsrc[i] = 1;
    }
    s_gx.dirty = 1;
}

/* ---- channels ---- */
static void gxfx_color4(float out[4], GXColor c) {
    out[0] = c.r / 255.0f; out[1] = c.g / 255.0f;
    out[2] = c.b / 255.0f; out[3] = c.a / 255.0f;
}

void GXSetChanMatColor(int chan, GXColor color) {
    float c[4];
    gxfx_color4(c, color);
    gxfx_enter();
    if (chan == 0 || chan == 4) memcpy(s_gx.mat, c, 3 * sizeof(float));  /* COLOR0 / COLOR0A0 */
    if (chan == 2 || chan == 4) s_gx.mat[3] = c[3];                      /* ALPHA0 / COLOR0A0 */
}

void GXSetChanAmbColor(int chan, GXColor color) {
    float c[4];
    gxfx_color4(c, color);
    gxfx_enter();
    if (chan == 0 || chan == 4) memcpy(s_gx.amb, c, 3 * sizeof(float));
    if (chan == 2 || chan == 4) s_gx.amb[3] = c[3];
}

void GXSetChanCtrl(int chan, uint8_t enable, int ambSrc, int matSrc,
                   uint32_t lightMask, int diffFn, int attnFn) {
    int lo, hi, i;
    gxfx_enter();
    if (chan == 4)      { lo = 0; hi = 1; }   /* COLOR0A0 */
    else if (chan == 0) { lo = 0; hi = 0; }   /* COLOR0 */
    else if (chan == 2) { lo = 1; hi = 1; }   /* ALPHA0 */
    else return;                              /* COLOR1/ALPHA1: unused by TEV here */
    for (i = lo; i <= hi; i++) {
        s_gx.chan_enable[i] = enable != 0;
        s_gx.chan_ambsrc[i] = ambSrc;
        s_gx.chan_matsrc[i] = matSrc;
        s_gx.chan_mask[i] = (int)(lightMask & 0xFF);
        s_gx.chan_diff[i] = diffFn;
        s_gx.chan_attn[i] = attnFn;
    }
}

void opti_GXSetChanCtrl(int chan, uint8_t enable, int ambSrc, int matSrc,
                        uint32_t lightMask, int diffFn, int attnFn) {
    GXSetChanCtrl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn);
}

void GXSetNumChans_cached(uint8_t n) { (void)n; }

/* ---- lights (Dolphin SDK GXInitLight* math) ---- */
void GXInitLightSpot(GxfxLightObj* obj, float cutoff, int spotFn) {
    GxfxLight* L = (GxfxLight*)obj;
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    if (cutoff <= 0.0f || cutoff > 90.0f) spotFn = 0;
    if (spotFn != 0) {
        float cr = cosf(cutoff * 3.14159265f / 180.0f), d;
        switch (spotFn) {
        case 1: a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;           /* FLAT */
        case 2: a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break; /* COS */
        case 3: a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break; /* COS2 */
        case 4: d = (1.0f - cr) * (1.0f - cr);                                   /* SHARP */
                a0 = cr * (cr - 2.0f) / d; a1 = 2.0f / d; a2 = -1.0f / d; break;
        case 5: d = (1.0f - cr) * (1.0f - cr);                                   /* RING1 */
                a0 = -4.0f * cr / d; a1 = 4.0f * (1.0f + cr) / d; a2 = -4.0f / d; break;
        case 6: d = (1.0f - cr) * (1.0f - cr);                                   /* RING2 */
                a0 = 1.0f - 2.0f * cr * cr / d; a1 = 4.0f * cr / d; a2 = -2.0f / d; break;
        default: a0 = 1.0f; a1 = a2 = 0.0f; break;
        }
    }
    L->a[0] = a0; L->a[1] = a1; L->a[2] = a2;
}

void GXInitLightDistAttn(GxfxLightObj* obj, float refDist, float refBright, int distFn) {
    GxfxLight* L = (GxfxLight*)obj;
    float k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (refDist < 0.0f || refBright <= 0.0f || refBright >= 1.0f) distFn = 0;
    switch (distFn) {
    case 1: k1 = (1.0f - refBright) / (refBright * refDist); break;                 /* GENTLE */
    case 2: k1 = 0.5f * (1.0f - refBright) / (refBright * refDist);                 /* MEDIUM */
            k2 = 0.5f * (1.0f - refBright) / (refBright * refDist * refDist); break;
    case 3: k2 = (1.0f - refBright) / (refBright * refDist * refDist); break;       /* STEEP */
    default: break;
    }
    L->k[0] = k0; L->k[1] = k1; L->k[2] = k2;
}

void GXInitLightAttnK(GxfxLightObj* obj, float k0, float k1, float k2) {
    GxfxLight* L = (GxfxLight*)obj;
    L->k[0] = k0; L->k[1] = k1; L->k[2] = k2;
}

void GXInitLightAttnA(GxfxLightObj* obj, float a0, float a1, float a2) {
    GxfxLight* L = (GxfxLight*)obj;
    L->a[0] = a0; L->a[1] = a1; L->a[2] = a2;
}

void GXInitLightColor(GxfxLightObj* obj, GXColor color) {
    GxfxLight* L = (GxfxLight*)obj;
    L->color[0] = color.r; L->color[1] = color.g;
    L->color[2] = color.b; L->color[3] = color.a;
}

void GXInitLightPos(GxfxLightObj* obj, float x, float y, float z) {
    GxfxLight* L = (GxfxLight*)obj;
    L->pos[0] = x; L->pos[1] = y; L->pos[2] = z;
}

void GXInitLightDir(GxfxLightObj* obj, float nx, float ny, float nz) {
    GxfxLight* L = (GxfxLight*)obj;
    L->dir[0] = -nx; L->dir[1] = -ny; L->dir[2] = -nz;
}

void GXGetLightPos(GxfxLightObj* obj, float* x, float* y, float* z) {
    GxfxLight* L = (GxfxLight*)obj;
    *x = L->pos[0]; *y = L->pos[1]; *z = L->pos[2];
}

void GXGetLightColor(GxfxLightObj* obj, GXColor* color) {
    GxfxLight* L = (GxfxLight*)obj;
    color->r = L->color[0]; color->g = L->color[1];
    color->b = L->color[2]; color->a = L->color[3];
}

void GXLoadLightObjImm(GxfxLightObj* obj, int lightId) {
    int i;
    for (i = 0; i < 8; i++)
        if (lightId & (1 << i)) break;
    if (i >= 8) return;
    gxfx_touch();
    memcpy(&s_gx.lights[i], obj, sizeof(GxfxLight));
}

/* ---- normal matrix ---- */
static void gxfx_load_nrm(const Mtx mtx, int id) {
    if (id < 0 || id >= 32) return;
    if (g_core) gx_flush_pending(g_core);
    memcpy(s_gx.nrm[id], mtx, sizeof(Mtx));
}

/* ---- TEV ---- */
static void gxfx_tev_order(int s, int coord, int map, int color) {
    if (s < 0 || s >= 16) return;
    gxfx_enter();
    s_gx.ord[s][0] = (coord >= 0 && coord < 8) ? coord : 0;
    s_gx.ord[s][1] = (map >= 0 && map < 8) ? map : -1;  /* GX_TEXMAP_NULL=0xFF */
    s_gx.ord[s][2] = (color != 0xFF);                    /* GX_COLOR_NULL */
}

static void gxfx_tev_cin(int s, int a, int b, int c, int d) {
    if (s < 0 || s >= 16) return;
    gxfx_enter();
    s_gx.cin[s][0] = a; s_gx.cin[s][1] = b; s_gx.cin[s][2] = c; s_gx.cin[s][3] = d;
}

static void gxfx_tev_ain(int s, int a, int b, int c, int d) {
    if (s < 0 || s >= 16) return;
    gxfx_enter();
    s_gx.ain[s][0] = a; s_gx.ain[s][1] = b; s_gx.ain[s][2] = c; s_gx.ain[s][3] = d;
}

static void gxfx_tev_op(int (*dst)[4], int s, int op, int bias, int scale, int clamp, int outReg) {
    if (s < 0 || s >= 16) return;
    gxfx_enter();
    dst[s][0] = op; dst[s][1] = bias; dst[s][2] = scale;
    dst[s][3] = (clamp ? 1 : 0) | ((outReg & 3) << 1);
}

void g_GXSetTevOrder_wrapper(int stage, int coord, int map, int color) { gxfx_tev_order(stage, coord, map, color); }
void GXSetTevColorIn_cached(int s, int a, int b, int c, int d) { gxfx_tev_cin(s, a, b, c, d); }
void GXSetTevAlphaIn_cached(int s, int a, int b, int c, int d) { gxfx_tev_ain(s, a, b, c, d); }
void GXSetTevColorOp_cached(int s, int op, int bias, int scale, uint8_t clamp, int out) {
    gxfx_tev_op(s_gx.cop, s, op, bias, scale, clamp, out);
}
void GXSetTevAlphaOp_cached(int s, int op, int bias, int scale, uint8_t clamp, int out) {
    gxfx_tev_op(s_gx.aop, s, op, bias, scale, clamp, out);
}
/* the game's GXSetTevOp twin (asm 0x800075A4): inputs only */
void g_GXSetTevColorIn_GXSetTevAlphaIn_wrapper(int stage, int mode) {
    if (stage < 0 || stage >= 16) return;
    gxfx_enter();
    gxfx_set_tev_op_inputs(stage, mode);
}
void GXSetTevOp(int stage, int mode) {
    g_GXSetTevColorIn_GXSetTevAlphaIn_wrapper(stage, mode);
    GXSetTevColorOp_cached(stage, 0, 0, 0, 1, 0);
    GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
}
static void gxfx_num_stages(int n) {
    if (n == s_gx.n_stages) return;
    gxfx_touch();
    s_gx.n_stages = n;
}
void GXSetNumTevStages_cached(uint8_t n) { gxfx_num_stages(n); }
static void gxfx_kcolor(int id, GXColor color) {
    if (id < 0 || id >= 4) return;
    gxfx_touch();
    gxfx_color4(s_gx.kcolor[id], color);
}
void GXSetTevKColor_cached(int id, GXColor color) { gxfx_kcolor(id, color); }
static void gxfx_ksel(int s, int sel, int alpha) {
    if (s < 0 || s >= 16) return;
    gxfx_touch();
    if (alpha) s_gx.kasel[s] = sel; else s_gx.ksel[s] = sel;
}
void GXSetTevKColorSel_cached(int s, int sel) { gxfx_ksel(s, sel, 0); }
void GXSetTevKAlphaSel_cached(int s, int sel) { gxfx_ksel(s, sel, 1); }
/* TEV swap (GXSetTevSwapMode / GXSetTevSwapModeTable, see gxfx_defaults) */
static void gxfx_swap_mode(int s, int ras, int tex) {
    if (s < 0 || s >= 16) return;
    if (s_gx.swap[s][0] == (ras & 3) && s_gx.swap[s][1] == (tex & 3)) return;
    gxfx_touch();
    s_gx.swap[s][0] = ras & 3;
    s_gx.swap[s][1] = tex & 3;
}
static void gxfx_swap_table(int id, int r, int g, int b, int a) {
    int* t;
    if (id < 0 || id >= 4) return;
    t = s_gx.swaptab[id];
    if (t[0] == (r & 3) && t[1] == (g & 3) && t[2] == (b & 3) && t[3] == (a & 3)) return;
    gxfx_touch();
    t[0] = r & 3; t[1] = g & 3; t[2] = b & 3; t[3] = a & 3;
}
static void gxfx_tev_reg(int id, GXColor color) {
    if (id < 0 || id >= 4) return;
    gxfx_touch();
    gxfx_color4(s_gx.reg[id], color);
}
/* ====================== end GXFX ===================================== */

/* ======================================================================
 * GXIND: GX indirect texturing
 * ----------------------------------------------------------------------
 * GXSetNumIndStages / GXSetIndTexOrder / GXSetIndTexCoordScale /
 * GXSetIndTexMtx / GXSetTevIndirect (+ GXSetTevDirect, GXSetTevIndWarp,
 * GXSetTevIndRepeat) and the game's cached twins.  Semantics follow
 * Dolphin's TEV shader generator:
 *   - the indirect lookup samples texmap[ind order] at
 *     texcoord[ind order] / 2^indScale, components (s,t,u) = (A,B,G) as
 *     8-bit integers, masked to the format (8/5/4/3 bits) and biased by
 *     -128 (8-bit) or +1 (other formats) per GX_ITB_* bit;
 *   - offset = M(2x3, s0.10) * (s,t,u) * 2^scaleExp, in texels of the
 *     TEV stage's own texture (so divided by its size), S/T dynamic
 *     matrices use uv_texels * s|t / 256;
 *   - the stage's coordinate is wrapped (GX_ITW_*), offset, and either
 *     replaces or (add-prev) accumulates into the running tevcoord.
 * Only the GXFX TEV path reads this state; the shader skips it entirely
 * while no TEV stage has an indirect setup (u_gx_ind_on == 0).
 * ====================================================================== */
static int n_tev_clamped(void) {
    return s_gx.n_stages < 1 ? 1 : (s_gx.n_stages > 8 ? 8 : s_gx.n_stages);
}

static void gxind_build(int n_tev) {
    int i, on = 0;
    for (i = 0; i < n_tev; i++)
        if (s_gx.tev_ind[i][1] | s_gx.tev_ind[i][2] | s_gx.tev_ind[i][3])
            on |= 1 << i;
    s_tevc.misc[1] = on;
    s_tevc.misc[2] = s_gx.ind_n;
    for (i = 0; i < 4; i++) {
        s_tevc.iord[i][0] = s_gx.ind_ord[i][0];
        s_tevc.iord[i][1] = s_gx.ind_ord[i][1];
        s_tevc.iord[i][2] = s_gx.ind_scale[i][0];
        s_tevc.iord[i][3] = s_gx.ind_scale[i][1];
    }
    for (i = 0; i < 3; i++) {
        s_tevc.imtx[i*2][0] = s_gx.ind_mtx[i][0][0];
        s_tevc.imtx[i*2][1] = s_gx.ind_mtx[i][0][1];
        s_tevc.imtx[i*2][2] = s_gx.ind_mtx[i][0][2];
        s_tevc.imtx[i*2][3] = (float)s_gx.ind_exp[i];
        s_tevc.imtx[i*2+1][0] = s_gx.ind_mtx[i][1][0];
        s_tevc.imtx[i*2+1][1] = s_gx.ind_mtx[i][1][1];
        s_tevc.imtx[i*2+1][2] = s_gx.ind_mtx[i][1][2];
        s_tevc.imtx[i*2+1][3] = 0.0f;
    }
    memcpy(s_tevc.tind, s_gx.tev_ind, sizeof(s_tevc.tind));
}

/* avdisp material setup / frame start: GC avdisp resets every stage it
 * builds to direct (GXSetTevDirect) and the indirect stage count to its
 * own (0 for plain materials). */
static void gxind_reset(void) {
    int i, any = s_gx.ind_n;
    for (i = 0; i < 16; i++)
        any |= s_gx.tev_ind[i][0] | s_gx.tev_ind[i][1] |
               s_gx.tev_ind[i][2] | s_gx.tev_ind[i][3];
    if (!any) return;
    gxfx_touch();
    s_gx.ind_n = 0;
    memset(s_gx.tev_ind, 0, sizeof(s_gx.tev_ind));
}

void GXSetNumIndStages(uint8_t n) {
    int v = n > 4 ? 4 : n;
    if (v == s_gx.ind_n) return;
    gxfx_touch();
    s_gx.ind_n = v;
}
void GXSetNumIndStages_cached(uint8_t n) { GXSetNumIndStages(n); }

void GXSetIndTexOrder(int indStage, int texCoord, int texMap) {
    if (indStage < 0 || indStage >= 4) return;
    texCoord = (texCoord >= 0 && texCoord < 8) ? texCoord : 0;
    texMap = (texMap >= 0 && texMap < 8) ? texMap : -1;   /* GX_TEXMAP_NULL */
    if (s_gx.ind_ord[indStage][0] == texCoord && s_gx.ind_ord[indStage][1] == texMap)
        return;
    gxfx_touch();
    s_gx.ind_ord[indStage][0] = texCoord;
    s_gx.ind_ord[indStage][1] = texMap;
}
void GXSetIndTexOrder_cached(int indStage, int texCoord, int texMap) {
    GXSetIndTexOrder(indStage, texCoord, texMap);
}

/* GX_ITS_1..GX_ITS_256 = 0..8: the coordinate is divided by 2^scale */
void GXSetIndTexCoordScale(int indStage, int scaleS, int scaleT) {
    if (indStage < 0 || indStage >= 4) return;
    scaleS = scaleS < 0 ? 0 : (scaleS > 8 ? 8 : scaleS);
    scaleT = scaleT < 0 ? 0 : (scaleT > 8 ? 8 : scaleT);
    if (s_gx.ind_scale[indStage][0] == scaleS && s_gx.ind_scale[indStage][1] == scaleT)
        return;
    gxfx_touch();
    s_gx.ind_scale[indStage][0] = scaleS;
    s_gx.ind_scale[indStage][1] = scaleT;
}

/* id: GX_ITM_0..2 = 1..3 (the hardware has no other static matrices);
 * elements are stored as s0.10 fixed point like the BP registers */
void GXSetIndTexMtx(int id, const float mtx[2][3], int scaleExp) {
    int i, j, k = id - 1;
    if (k < 0 || k >= 3 || !mtx) return;
    gxfx_touch();
    for (i = 0; i < 2; i++)
        for (j = 0; j < 3; j++) {
            float v = floorf(mtx[i][j] * 1024.0f + 0.5f);
            v = v < -1024.0f ? -1024.0f : (v > 1023.0f ? 1023.0f : v);
            s_gx.ind_mtx[k][i][j] = v / 1024.0f;
        }
    s_gx.ind_exp[k] = scaleExp < -17 ? -17 : (scaleExp > 46 ? 46 : scaleExp);
}
void g_GXSetIndTexMtx_wrapper(int id, const float mtx[2][3], int scaleExp) {
    GXSetIndTexMtx(id, mtx, scaleExp);
}

void GXSetTevIndirect(int stage, int indStage, int format, int biasSel, int mtxSel,
                      int wrapS, int wrapT, int addPrev, int utcLod, int alphaSel) {
    int v[4];
    (void)utcLod;  /* unnormalized-coord LOD: no PC equivalent */
    if (stage < 0 || stage >= 16) return;
    v[0] = (indStage & 3) | ((format & 3) << 2) | ((biasSel & 7) << 4) | ((alphaSel & 3) << 7);
    v[1] = mtxSel & 15;
    v[2] = (wrapS & 7) | ((wrapT & 7) << 3);
    v[3] = addPrev ? 1 : 0;
    if (!(v[1] | v[2] | v[3]))   /* no matrix, wrap or add: a direct stage */
        v[0] = 0;
    if (memcmp(s_gx.tev_ind[stage], v, sizeof(v)) == 0) return;
    gxfx_touch();
    memcpy(s_gx.tev_ind[stage], v, sizeof(v));
}
void GXSetTevIndirect_cached(int stage, int indStage, int format, int biasSel, int mtxSel,
                             int wrapS, int wrapT, int addPrev, int utcLod, int alphaSel) {
    GXSetTevIndirect(stage, indStage, format, biasSel, mtxSel, wrapS, wrapT,
                     addPrev, utcLod, alphaSel);
}

void GXSetTevDirect(int stage) {
    GXSetTevIndirect(stage, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
/* the game's cached GXSetTevDirect (asm: compares, then zeroes the stage) */
void g_GXSetTevIndirect_zero_if_different(int stage) { GXSetTevDirect(stage); }

/* SDK convenience wrappers (libogc/SDK GXTev.c) */
void GXSetTevIndWarp(int stage, int indStage, uint8_t signedOffset, uint8_t replaceMode, int matSel) {
    int wrap = replaceMode ? 6 /* GX_ITW_0 */ : 0 /* GX_ITW_OFF */;
    GXSetTevIndirect(stage, indStage, 0 /* GX_ITF_8 */,
                     signedOffset ? 7 /* GX_ITB_STU */ : 0 /* GX_ITB_NONE */,
                     matSel, wrap, wrap, 0, 0, 0);
}
void GXSetTevIndRepeat(int stage) {
    GXSetTevIndirect(stage, 0, 0, 0, 0, 6, 6, 1, 0, 0);
}
/* ====================== end GXIND ==================================== */

/* ======================================================================
 * Texgen/fog/texcopy block: (a) GX texgen sources (GX_TG_POS -> v_tcq),
 * (b) GX fog (DOL avdisp fog helpers + GXSetFog),
 * (c) EFB -> texture copies (GXSetTexCopySrc/Dst, GXCopyTex).
 * TEV itself is the GXFX block above; gx_tc() there reads v_tcq.
 * ====================================================================== */

static void tex_flush(void) { if (g_core) gx_flush_pending(g_core); }

/* texgen source per texcoord (GX_TG_POS = 0, GX_TG_TEX0 = 4 ...) */
static int s_tg_src[8] = {4, 4, 4, 4, 4, 4, 4, 4};

/* Per-vertex TEX1/TEX2 coordinates (vertex floats 12-13 / 14-15).  Only
 * the avdisp shim's multi-layer GMA materials emit them; while
 * s_multi_tc is off every texgen reads TEX0 as before, so callers that
 * name GX_TG_TEX1+ without streaming those coordinates are unaffected. */
static int s_multi_tc;

void gx_set_multi_texcoord(int on) {
    on = on != 0;
    if (on == s_multi_tc) return;
    if (g_core) gx_flush_pending(g_core);
    s_multi_tc = on;
}

void gx_texcoord_n2f32(int n, float s, float t) {
    float* v;
    if (!g_core || g_core->vertex_count == 0 || n < 1 || n > 2) return;
    v = &g_core->vertex_buffer[(g_core->vertex_count - 1) * 16];
    v[10 + 2 * n] = s;
    v[11 + 2 * n] = t;
}

/* per texgen: normalize flag and post matrix id (GX_PTIDENTITY = 125) */
static int s_tg_norm[8];
static int s_tg_post[8] = {125, 125, 125, 125, 125, 125, 125, 125};
/* per texgen: GX_TG_MTX3x4 (0) or GX_TG_MTX2x4 (1).  A 2x4 texgen uses only
 * the first two matrix rows and q = 1: the water caustics run the position
 * through mtxa-based 3x4 matrices with GX_TG_MTX2x4, and dividing by the
 * third row smeared them into streaks radiating from the camera. */
static int s_tg_func[8] = {1, 1, 1, 1, 1, 1, 1, 1};
/* GX_PTTEXMTX0..19 = ids 64, 67 .. 121 */
#define TEX_NUM_PTMTX 20
static Mtx s_ptmtx[TEX_NUM_PTMTX];
static int s_ptmtx_init;

static int tex_ptmtx_index(int id) {
    if (id < 64 || id > 121 || (id - 64) % 3 != 0) return -1;
    return (id - 64) / 3;
}

static void tex_rec_texgen(int dst, int func, int src, int normalize, int postMtx) {
    if (dst < 0 || dst >= 8) return;
    normalize = normalize != 0;
    if (s_tg_src[dst] == src && s_tg_norm[dst] == normalize &&
        s_tg_post[dst] == postMtx && s_tg_func[dst] == func) return;
    tex_flush();
    s_tg_func[dst] = func;
    s_tg_src[dst] = src;
    s_tg_norm[dst] = normalize;
    s_tg_post[dst] = postMtx;
}

static int tex_load_post_mtx(const Mtx mtx, int id) {
    int k = tex_ptmtx_index(id);
    if (k < 0) return 0;
    if (memcmp(s_ptmtx[k], mtx, sizeof(Mtx)) != 0 || !s_ptmtx_init) {
        tex_flush();
        memcpy(s_ptmtx[k], mtx, sizeof(Mtx));
    }
    s_ptmtx_init = 1;
    return 1;
}

/* avdisp_shim.c: honour the custom material callback only while the GX TEV
 * emulation is drawing (the caller's TEV setup is then what gets drawn) */
int gx_tex_gxfx_active(void) {
    return s_gx.mode != 0 && g_core && !g_core->ui_tev;
}

/* ---- fog: DOL g_something_with_fog / fn_80066520 /
 * g_something_with_raster_state (asm 0x800663E0..0x80066674).  The DOL
 * keeps type/start/end/color in lbl_8018FF40 and writes the BP fog regs;
 * raster_state(0/1) clears / restores the fog-select field. */
static int s_fog_type, s_fog_on;
static float s_fog_start, s_fog_end = 100.0f;
/* fn_80066674(0) zeroes only the fog-type field of the BP fog register for
 * the shape being drawn (GMA shape flag 0x4 = no fog); the stored type stays.
 * The field is reprogrammed by fn_80066674(1), by raster_state 0->1 and by a
 * GXSetFog that actually runs (g_something_with_fog skips unchanged params). */
static int s_fog_shape_off;
static float s_fog_color[4];

void g_something_with_fog(int type, float start, float end) {
    if (type == 0) {
        extern void g_something_with_raster_state(int on);
        g_something_with_raster_state(0);
        return;
    }
    if (s_fog_on && type == s_fog_type && start == s_fog_start && end == s_fog_end)
        return; /* asm 0x80066414..0x8006644C: unchanged params skip GXSetFog */
    tex_flush();
    s_fog_type = type;
    s_fog_start = start;
    s_fog_end = end;
    s_fog_on = 1;
    s_fog_shape_off = 0;
}

/* asm 0x80066674: per-shape fog on/off (g_some_GmaSomeStruct_func3 passes
 * !(shape->flags & 4)); a no-op while fog is globally off */
void fn_80066674(int on) {
    if (!s_fog_on) return;
    if ((on == 0) == (s_fog_shape_off != 0)) return;
    tex_flush();
    s_fog_shape_off = on == 0;
}

void fn_80066520(GXColor color) {
    tex_flush();
    s_fog_color[0] = color.r / 255.0f;
    s_fog_color[1] = color.g / 255.0f;
    s_fog_color[2] = color.b / 255.0f;
    s_fog_color[3] = 1.0f;
}

void g_something_with_raster_state(int on) {
    if ((on != 0) == (s_fog_on != 0)) return;
    tex_flush();
    s_fog_on = on != 0;
    s_fog_shape_off = 0;
}

void GXSetFog(int type, float startz, float endz, float nearz, float farz, GXColor color) {
    (void)nearz; (void)farz;
    tex_flush();
    s_fog_type = type;
    s_fog_start = startz;
    s_fog_end = endz;
    s_fog_on = type != 0;
    s_fog_shape_off = 0;
    fn_80066520(color);
}

/* ---- per-flush constants ---- */
static void tex_ext_build(GxCore* gl) {
    int i, posmask = 0, srcsel = 0, nrmmask = 0, normmask = 0, postmask = 0;
    float post[8][16];

    memset(post, 0, sizeof(post));
    for (i = 0; i < 8; i++) {
        int k = tex_ptmtx_index(s_tg_post[i]);
        if (s_tg_src[i] != 1 /* GX_TG_NRM */) continue;
        nrmmask |= 1 << i;
        if (s_tg_norm[i]) normmask |= 1 << i;
        if (k >= 0) {
            postmask |= 1 << i;
            mtx34_to_gl(s_ptmtx[k], post[i]);
        } else {
            memset(post[i], 0, sizeof(post[i]));
        }
    }
    s_dc.tg[2] = nrmmask;
    s_dc.tg[3] = normmask;
    s_dc.tg2[0] = postmask;
    if (memcmp(s_tevc.tg_post, post, sizeof(post)) != 0) {
        memcpy(s_tevc.tg_post, post, sizeof(post));
        s_tc_serial++;
    }
    if (s_multi_tc)
        for (i = 0; i < 8; i++)
            if (s_tg_src[i] == 5 /* GX_TG_TEX1 */ || s_tg_src[i] == 6 /* TEX2 */)
                srcsel |= (s_tg_src[i] - 4) << (2 * i);
    s_dc.tg[1] = srcsel;
    for (i = 0; i < 8; i++)
        if (s_tg_src[i] == 0 /* GX_TG_POS */) posmask |= 1 << i;
    s_dc.tg[0] = posmask;
    {
        int q1mask = 0;
        for (i = 0; i < 8; i++)
            if (s_tg_func[i] == 1 /* GX_TG_MTX2x4 */) q1mask |= 1 << i;
        s_dc.tg2[2] = q1mask;
    }
    /* fog is a perspective-only effect here (ortho fog bit 3 unsupported) */
    if (s_fog_on && !s_fog_shape_off && s_fog_type != 0 && !(s_fog_type & 8) && gl->projection_type == 0) {
        s_dc.fog[0] = (float)s_fog_type; s_dc.fog[1] = s_fog_start;
        s_dc.fog[2] = s_fog_end; s_dc.fog[3] = 0.0f;
    } else {
        s_dc.fog[0] = s_dc.fog[1] = s_dc.fog[2] = s_dc.fog[3] = 0.0f;
    }
    memcpy(s_dc.fog_color, s_fog_color, sizeof(s_dc.fog_color));
}

/* ---- texture buffer layout helpers ---- */
static int tex_fmt_bpp(int fmt, int* tw, int* th) {
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: case 0x20: case 0x30:     /* 4bpp */
        *tw = 8; *th = 8; return 4;
    case 0x01: case 0x02: case 0x09: case 0x11: case 0x22: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x39: case 0x3A:     /* 8bpp */
        *tw = 8; *th = 4; return 8;
    case 0x06: case 0x16: case 0x26:                           /* 32bpp */
        *tw = 4; *th = 4; return 32;
    default:                                                   /* 16bpp */
        *tw = 4; *th = 4; return 16;
    }
}

static uint32_t tex_level_bytes(int w, int h, int fmt) {
    int tw, th, bpp = tex_fmt_bpp(fmt, &tw, &th);
    uint32_t bw = (uint32_t)(w + tw - 1) / tw, bh = (uint32_t)(h + th - 1) / th;
    return bw * bh * (bpp == 32 ? 64 : 32);
}

uint32_t GXGetTexBufferSize(uint16_t width, uint16_t height, uint32_t format,
                            uint8_t mipmap, uint8_t max_lod) {
    uint32_t sum = 0;
    int w = width, h = height, lv;
    if (!mipmap) return tex_level_bytes(w, h, (int)format);
    for (lv = 0; lv < (max_lod ? max_lod : 1); lv++) {
        sum += tex_level_bytes(w, h, (int)format);
        if (w == 1 && h == 1) break;
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
    return sum;
}

/* ---- EFB copies ----
 * GC GXCopyTex converts an EFB rectangle into texel data in main memory
 * (box-filtered 2x when the dst mipmap flag is set) and optionally clears
 * that rectangle.  Here the rectangle is area-resampled to the dst size and
 * encoded into the real GC texel layout at dest (so CPU readers and the
 * regular decoder agree); GXInitTexObj hands out a dedicated backend texture
 * per dest address while the memory still holds exactly what the copy wrote
 * (checked by hash).  See docs/d3d12.md, EFB copies. */
static uint16_t s_cp_l, s_cp_t, s_cp_w = 640, s_cp_h = 480;
static uint16_t s_cp_dw = 640, s_cp_dh = 480;
static uint32_t s_cp_fmt = 4;

void GXSetTexCopySrc(uint16_t left, uint16_t top, uint16_t wd, uint16_t ht) {
    s_cp_l = left; s_cp_t = top; s_cp_w = wd; s_cp_h = ht;
}
void GXSetTexCopySrc_cached(uint16_t left, uint16_t top, uint16_t wd, uint16_t ht) {
    GXSetTexCopySrc(left, top, wd, ht);
}
void GXSetTexCopyDst(uint16_t wd, uint16_t ht, uint32_t fmt, uint8_t mipmap) {
    (void)mipmap; /* dst size already says it; resampling covers the 2x box */
    s_cp_dw = wd; s_cp_dh = ht; s_cp_fmt = fmt;
}
void GXSetTexCopyDst_cached(uint16_t wd, uint16_t ht, uint32_t fmt, uint8_t mipmap) {
    GXSetTexCopyDst(wd, ht, fmt, mipmap);
}

#define TEXCOPY_MAX 64
static struct {
    void* ptr;
    uint32_t bytes;
    uint64_t hash;
    uint16_t w, h;          /* copy dst size */
    int copy_fmt;
    int up_fmt, up_w, up_h; /* what the GPU texture currently holds */
    void* tex;
    long stamp;
    uint64_t pending;       /* async copy (g_gpu->copy_tex) ticket not landed in dest yet */
    uint64_t prehash;       /* dest bytes when that copy was made */
    long pending_frame;
} s_tc[TEXCOPY_MAX];
static void* texcopy_handle(unsigned e) { return e < TEXCOPY_MAX ? s_tc[e].tex : NULL; }
static long s_tc_stamp;
static int s_tc_npending;
static int s_copy_cpu_resample = -1;
static void tc_complete(int e, int wait);

static uint64_t tex_hash(const uint8_t* p, uint32_t n) {
    uint64_t k = 0xcbf29ce484222325ull;
    uint32_t i;
    PERF_COUNT(PERF_C_HASHBYTES, n);
    for (i = 0; i < n; i++) k = (k ^ p[i]) * 0x100000001b3ull;
    return k;
}

/* sampling format matching a copy format's texel layout */
static int tex_copy_sample_fmt(int cf) {
    switch (cf) {
    case 0x20: case 0x30:                         return 0; /* R4, Z4 -> I4 */
    case 0x27: case 0x28: case 0x29: case 0x2A:
    case 0x11: case 0x39: case 0x3A:              return 1; /* 8bit -> I8 */
    case 0x22:                                    return 2; /* RA4 -> IA4 */
    case 0x23: case 0x2B: case 0x2C: case 0x13:
    case 0x3C:                                    return 3; /* -> IA8 */
    case 0x26: case 0x16:                         return 6;
    default:                                      return cf & 0xF;
    }
}

static void tc_upload(int e, int fmt, int w, int h, int wrapS, int wrapT) {
    GxCore* gl = g_core;
    uint32_t* rgba;
    if (!gl) return;
    gx_flush_pending(gl);
    if (!s_tc[e].tex || fmt != s_tc[e].up_fmt || w != s_tc[e].up_w || h != s_tc[e].up_h) {
        PERF_COUNT(PERF_C_TEXUPLOAD, 1);
        rgba = gx_decode_texture(s_tc[e].ptr, w, h, (uint32_t)fmt);
        g_gpu->tex_image(&s_tc[e].tex, rgba, w, h);
        free(rgba);
        s_tc[e].up_fmt = fmt; s_tc[e].up_w = w; s_tc[e].up_h = h;
    }
    if (s_tc[e].tex) g_gpu->tex_wrap(s_tc[e].tex, wrapS, wrapT);
}

static int texcopy_init_obj(GXTexObj* obj, void* image, uint16_t w, uint16_t h,
                            int fmt, int wrapS, int wrapT, uint8_t mipmap) {
    int e;
    if (!image) return 0;
    for (e = 0; e < TEXCOPY_MAX; e++)
        if (s_tc[e].ptr == image) break;
    if (e == TEXCOPY_MAX) return 0;
    if (s_tc[e].pending) {
        /* async copy still in flight: its texture already holds the texels
         * in the copy's natural format; anything else needs the GC bytes */
        if (fmt == s_tc[e].up_fmt && w == s_tc[e].up_w && h == s_tc[e].up_h && s_tc[e].tex) {
            g_gpu->tex_wrap(s_tc[e].tex, wrapS, wrapT);
            goto use;
        }
        tc_complete(e, 1);
        if (s_tc[e].ptr != image) return 0;
    }
    if (tex_hash((const uint8_t*)image, s_tc[e].bytes) != s_tc[e].hash) {
        s_tc[e].ptr = NULL;  /* memory reused for something else */
        return 0;
    }
    tc_upload(e, fmt, w, h, wrapS, wrapT);
use:
    tex_obj_set(obj, image, w, h, fmt, wrapS, wrapT, mipmap, TC_ID_BASE + (uint32_t)e, 0);
    return 1;
}

/* GXLoadTexObj: a texObj initialised before the EFB copy that targets its
 * memory (e.g. background shadow groups set up at stage load) still carries
 * the texture decoded from the old bytes; on GC it reads memory at load
 * time, so swap in the copy's texture. */
static void texcopy_fix_obj(GXTexObj* obj) {
    int e;
    if (!obj || !obj->image_data) return;
    for (e = 0; e < TEXCOPY_MAX; e++)
        if (s_tc[e].ptr == obj->image_data) break;
    if (e == TEXCOPY_MAX || obj->tex_id == TC_ID_BASE + (uint32_t)e) return;
    texcopy_init_obj(obj, obj->image_data, obj->width, obj->height, (int)obj->format,
                     obj->wrapS, obj->wrapT, obj->mipmap);
}

static uint8_t tex_luma(const uint8_t* p) {
    int y = (66 * p[0] + 129 * p[1] + 25 * p[2] + 128) / 256 + 16;
    return (uint8_t)(y > 255 ? 255 : y);
}

/* one texel of the copy, as the byte(s) the GC writes for format cf */
static void tex_copy_texel(int cf, const uint8_t* p, uint8_t out[4]) {
    switch (cf) {
    case 0x00: case 0x01: out[0] = tex_luma(p); break;        /* I4, I8 */
    case 0x02: out[0] = (uint8_t)((p[3] & 0xF0) | (tex_luma(p) >> 4)); break; /* IA4 */
    case 0x03: out[0] = p[3]; out[1] = tex_luma(p); break;    /* IA8 */
    case 0x20: case 0x28: out[0] = p[0]; break;                /* R4, R8 */
    case 0x29: out[0] = p[1]; break;
    case 0x2A: out[0] = p[2]; break;
    case 0x27: out[0] = p[3]; break;
    case 0x22: out[0] = (uint8_t)((p[3] & 0xF0) | (p[0] >> 4)); break;  /* RA4 */
    case 0x23: out[0] = p[3]; out[1] = p[0]; break;           /* RA8 */
    case 0x2B: out[0] = p[1]; out[1] = p[0]; break;           /* RG8 */
    case 0x2C: out[0] = p[2]; out[1] = p[1]; break;           /* GB8 */
    case 0x04: {                                              /* RGB565 */
        uint16_t v = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
        out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; break; }
    case 0x05: {                                              /* RGB5A3 */
        uint16_t v;
        if (p[3] >= 0xE0)
            v = (uint16_t)(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
        else
            v = (uint16_t)(((p[3] >> 5) << 12) | ((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4));
        out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; break; }
    default:                                                  /* RGBA8-ish */
        out[0] = p[3]; out[1] = p[0]; out[2] = p[1]; out[3] = p[2]; break;
    }
}

/* depth copy formats: p holds the 24-bit z in p[0..2] (msb first) */
static int tex_is_zfmt(int cf) {
    return (cf >= 0x10 && cf <= 0x1F) || (cf >= 0x30 && cf <= 0x3F);
}

static void tex_copy_ztexel(int cf, const uint8_t* p, uint8_t out[4]) {
    switch (cf) {
    case 0x30: case 0x11: out[0] = p[0]; break;               /* Z4, Z8 (msb) */
    case 0x39: out[0] = p[1]; break;                           /* Z8M */
    case 0x3A: out[0] = p[2]; break;                           /* Z8L */
    case 0x3C: out[0] = p[1]; out[1] = p[2]; break;            /* Z16L */
    case 0x13: out[0] = p[0]; out[1] = p[1]; break;            /* Z16 */
    default: out[0] = 0xFF; out[1] = p[0]; out[2] = p[1]; out[3] = p[2]; break;
    }
}

/* encode a top-row-first RGBA8 (or Z) image into GC tiles at dst */
static void tex_encode(uint8_t* dst, const uint8_t* img, int w, int h, int cf) {
    int tw, th, bpp = tex_fmt_bpp(cf, &tw, &th);
    int bx, by, x, y, z = tex_is_zfmt(cf);
    static const uint8_t black[4] = {0, 0, 0, 0};
    for (by = 0; by < h; by += th)
    for (bx = 0; bx < w; bx += tw) {
        uint8_t* t = dst;
        for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            const uint8_t* p = (bx + x < w && by + y < h)
                ? img + ((size_t)(by + y) * w + bx + x) * 4 : black;
            uint8_t o[4] = {0, 0, 0, 0};
            int i = y * tw + x;
            if (z) tex_copy_ztexel(cf, p, o); else tex_copy_texel(cf, p, o);
            if (bpp == 4) {
                if (x & 1) t[i / 2] |= o[0] >> 4;
                else t[i / 2] = (uint8_t)(o[0] & 0xF0);
            } else if (bpp == 8) {
                t[i] = o[0];
            } else if (bpp == 16) {
                t[i * 2] = o[0]; t[i * 2 + 1] = o[1];
            } else {             /* AR plane then GB plane */
                t[i * 2] = o[0]; t[i * 2 + 1] = o[1];
                t[32 + i * 2] = o[2]; t[32 + i * 2 + 1] = o[3];
            }
        }
        dst += bpp == 32 ? 64 : 32;
    }
}

/* SMB2_COPY_CPU_RESAMPLE (A/B checks of the GPU copy paths):
 *   0 (default) async GPU copy (copy_tex) where the format allows, else the
 *     synchronous GPU resample (read_pixels_box), else the CPU path;
 *   1 = CPU path only (full-size readback + CPU resample);
 *   2 = verify: async copy completed at once, and the synchronous paths,
 *       compared with the CPU path ([copyverify] lines);
 *   3 = synchronous GPU resample, no async copies. */
/* the CPU EFB copy: full-size read_pixels, area resample W x H (bottom-up)
 * -> dw x dh (top-down) RGBA8 into img (colour alpha = 0xFF) */
static int copy_cpu_resample(int x0, int y0, int W, int H, int dw, int dh, int z, uint8_t* img) {
    uint8_t* src;
    int x, y;
    src = (uint8_t*)malloc((size_t)W * H * 4);
    if (!src) return 0;
    if (z) {
        float* d = (float*)src;  /* W*H floats fit in the RGBA buffer */
        if (!g_gpu->read_pixels(x0, y0, W, H, 1, d))
            memset(d, 0, (size_t)W * H * sizeof(float));
        for (y = H * W - 1; y >= 0; y--) {   /* in place, back to front */
            float f = d[y];
            uint32_t z24 = (uint32_t)((f < 0 ? 0 : f > 1 ? 1 : f) * 16777215.0f);
            src[y * 4 + 0] = (uint8_t)(z24 >> 16);
            src[y * 4 + 1] = (uint8_t)(z24 >> 8);
            src[y * 4 + 2] = (uint8_t)z24;
            src[y * 4 + 3] = 0xFF;
        }
    } else {
        if (!g_gpu->read_pixels(x0, y0, W, H, 0, src))
            memset(src, 0, (size_t)W * H * 4);
    }
    /* area resample W x H (bottom-up) -> dw x dh (top-down) */
    for (y = 0; y < dh; y++) {
        int ya = y * H / dh, yb = (y + 1) * H / dh;
        if (yb <= ya) yb = ya + 1;
        for (x = 0; x < dw; x++) {
            int xa = x * W / dw, xb = (x + 1) * W / dw, xx, yy, c;
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            if (xb <= xa) xb = xa + 1;
            for (yy = ya; yy < yb && yy < H; yy++)
            for (xx = xa; xx < xb && xx < W; xx++) {
                const uint8_t* p = src + ((size_t)(H - 1 - yy) * W + xx) * 4;
                for (c = 0; c < 4; c++) acc[c] += p[c];
                n++;
            }
            for (c = 0; c < 4; c++)
                img[((size_t)y * dw + x) * 4 + c] = (uint8_t)(n ? acc[c] / n : 0);
            if (!z) img[((size_t)y * dw + x) * 4 + 3] = 0xFF; /* EFB RGB8 has no alpha */
        }
    }
    free(src);
    return 1;
}

static void copy_verify(const char* what, const uint8_t* a, const uint8_t* b, int dw, int dh,
                        int x0, int y0, int W, int H, int cf) {
    static long checks, bad;
    long diff = 0, i;
    for (i = 0; i < (long)dw * dh * 4; i++) diff += a[i] != b[i];
    checks++;
    if (diff) bad++;
    if (diff || checks % 300 == 1)
        fprintf(stderr, "[copyverify] %s: rect %d,%d %dx%d -> %dx%d cf 0x%x: %ld bytes differ "
                "(%ld/%ld checks differ)\n", what, x0, y0, W, H, dw, dh, cf, diff, bad, checks);
}

/* copy formats whose GC bytes read back through their natural sample format
 * (tex_copy_sample_fmt) texel by texel: the backends' copy_conv shader
 * reproduces tex_copy_texel + gx_decode_texture for exactly these */
static int tex_copy_gpu_conv(int cf) {
    switch (cf) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
    case 0x20: case 0x22: case 0x23: case 0x26: case 0x27: case 0x28: case 0x29:
    case 0x2A: case 0x2B: case 0x2C:
        return 1;
    default:
        return 0;
    }
}

/* registry slot for dest: its entry, else the least recently copied one */
static int tc_slot(void* dest) {
    int e, oldest = 0;
    for (e = 0; e < TEXCOPY_MAX; e++) {
        if (s_tc[e].ptr == dest) return e;
        if (s_tc[e].stamp < s_tc[oldest].stamp) oldest = e;
    }
    if (s_tc[oldest].pending) tc_complete(oldest, 1);
    s_tc[oldest].up_fmt = -1;
    s_tc[oldest].ptr = NULL;
    return oldest;
}

/* Land an async copy in GC memory: encode the resampled texels into dest,
 * unless the game wrote something else there in the meantime (then the
 * entry stops being a copy).  wait = 0: only if the GPU is done. */
static void tc_complete(int e, int wait) {
    int dw = s_tc[e].w, dh = s_tc[e].h, r, i;
    uint8_t *img, *conv = NULL;
    if (!s_tc[e].pending) return;
    img = (uint8_t*)malloc((size_t)dw * dh * 4);
    if (s_copy_cpu_resample == 2) conv = (uint8_t*)malloc((size_t)dw * dh * 4);
    if (!img) { free(conv); return; }
    r = g_gpu->copy_tex_result(s_tc[e].pending, img, conv, wait);
    if (r == 0) { free(img); free(conv); return; }
    s_tc[e].pending = 0;
    s_tc_npending--;
    if (r == 1 && tex_hash((const uint8_t*)s_tc[e].ptr, s_tc[e].bytes) == s_tc[e].prehash) {
        for (i = 0; i < dw * dh; i++) img[i * 4 + 3] = 0xFF;   /* EFB RGB8 has no alpha */
        tex_encode((uint8_t*)s_tc[e].ptr, img, dw, dh, s_tc[e].copy_fmt);
        s_tc[e].hash = tex_hash((const uint8_t*)s_tc[e].ptr, s_tc[e].bytes);
        if (conv) {   /* the texture must hold what the GC bytes decode to */
            uint32_t* dec = gx_decode_texture(s_tc[e].ptr, dw, dh,
                                              (uint32_t)tex_copy_sample_fmt(s_tc[e].copy_fmt));
            if (dec) copy_verify("async conv", conv, (const uint8_t*)dec, dw, dh, 0, 0, 0, 0,
                                 s_tc[e].copy_fmt);
            free(dec);
        }
    } else {
        static int warned;
        if (r == 1 && warned++ < 5)
            fprintf(stderr, "[texcopy] %p rewritten before its async copy landed - dropped\n",
                    s_tc[e].ptr);
        s_tc[e].ptr = NULL;   /* no longer a copy (or the result was lost) */
    }
    free(img);
    free(conv);
}

/* once per frame: land finished async copies; wait for ones 3 frames old */
static long s_tc_frame;
static void texcopy_poll(void) {
    int e;
    s_tc_frame++;
    if (!s_tc_npending) return;
    for (e = 0; e < TEXCOPY_MAX; e++)
        if (s_tc[e].pending) tc_complete(e, s_tc_frame - s_tc[e].pending_frame >= 3);
}

/* earlier async copies into [dest, dest+bytes) land first (GC write order).
 * One that lies entirely inside the new copy's bytes would be overwritten by
 * it anyway: drop it instead of waiting for the GPU (stage select copies
 * into the same buffer every frame). */
static void tc_land_overlaps(void* dest, uint32_t bytes) {
    uintptr_t a = (uintptr_t)dest, b = a + bytes;
    int i;
    for (i = 0; i < TEXCOPY_MAX && s_tc_npending; i++) {
        uintptr_t pa = (uintptr_t)s_tc[i].ptr, pb = pa + s_tc[i].bytes;
        if (!s_tc[i].pending || pa >= b || pb <= a) continue;
        if (pa >= a && pb <= b) {
            g_gpu->copy_tex_result(s_tc[i].pending, NULL, NULL, -1);   /* discard */
            s_tc[i].pending = 0;
            s_tc_npending--;
            /* GC memory between the two copies kept the old bytes: the new
             * copy's prehash is taken from what is there now */
        } else {
            tc_complete(i, 1);
        }
    }
}

static int copy_async(void* dest, int x0, int y0, int W, int H, int dw, int dh, int cf) {
    int fmt = tex_copy_sample_fmt(cf), e, i;
    uint32_t bytes = tex_level_bytes(dw, dh, cf);
    uint64_t ticket;
    (void)i;
    e = tc_slot(dest);
    /* the GPU texture is refreshed in place in the format it is sampled in;
     * only the natural one is converted on the GPU */
    if (s_tc[e].ptr == dest && s_tc[e].up_fmt >= 0 && s_tc[e].up_w == dw &&
        s_tc[e].up_h == dh && s_tc[e].up_fmt != fmt)
        return 0;
    tc_land_overlaps(dest, bytes);
    ticket = g_gpu->copy_tex(&s_tc[e].tex, x0, y0, W, H, dw, dh, cf);
    if (!ticket) return 0;
    s_tc[e].ptr = dest;
    s_tc[e].bytes = bytes;
    s_tc[e].prehash = s_tc[e].hash = tex_hash((const uint8_t*)dest, bytes);
    s_tc[e].w = (uint16_t)dw;
    s_tc[e].h = (uint16_t)dh;
    s_tc[e].copy_fmt = cf;
    s_tc[e].stamp = ++s_tc_stamp;
    s_tc[e].up_fmt = fmt; s_tc[e].up_w = dw; s_tc[e].up_h = dh;
    s_tc[e].pending = ticket;
    s_tc[e].pending_frame = s_tc_frame;
    s_tc_npending++;
    g_gpu->tex_wrap(s_tc[e].tex, 0, 0);
    PERF_COUNT(PERF_C_TEXUPLOAD, 1);
    if (s_copy_cpu_resample == 2) {   /* verify: land now, compare with the CPU path */
        uint8_t* cpu = (uint8_t*)calloc((size_t)dw * dh, 4);
        tc_complete(e, 1);
        if (cpu && s_tc[e].ptr == dest && copy_cpu_resample(x0, y0, W, H, dw, dh, 0, cpu)) {
            uint8_t* enc = (uint8_t*)calloc(bytes ? bytes : 1, 1);
            if (enc) {
                tex_encode(enc, cpu, dw, dh, cf);
                copy_verify("async bytes", enc, (const uint8_t*)dest, (int)bytes / 4, 1, x0, y0, W, H, cf);
                free(enc);
            }
        }
        free(cpu);
    }
    return 1;
}

static void GXCopyTex_impl(void* dest, uint8_t clear);
void GXCopyTex(void* dest, uint8_t clear) {
    if (s_copy_cpu_resample < 0) {
        const char* e = getenv("SMB2_COPY_CPU_RESAMPLE");
        s_copy_cpu_resample = e ? atoi(e) : 0;
    }
    PERF_BEGIN(t0);
    PERF_COUNT(PERF_C_COPYTEX, 1);
    GXCopyTex_impl(dest, clear);
    PERF_END(t0, PERF_T_COPYTEX);
}
static void GXCopyTex_impl(void* dest, uint8_t clear) {
    GxCore* gl = g_core;
    float sx, sy;
    int x0, y0, W, H, dw = s_cp_dw, dh = s_cp_dh, cf = (int)s_cp_fmt;
    int z, y, e;
    uint8_t *img = NULL, *gpu_img = NULL;
    uint32_t bytes;

    if (!gl || !dest || !dw || !dh || !s_cp_w || !s_cp_h) return;
    gx_flush_pending(gl);
    z = tex_is_zfmt(cf);
    sx = OUT_SX;
    sy = OUT_SY;
    (void)sx; (void)sy;
    pc_efb_rect_to_gl(s_cp_l, s_cp_t, s_cp_w, s_cp_h, &x0, &y0, &W, &H);
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    if (!z && s_copy_cpu_resample != 1 && s_copy_cpu_resample != 3 && g_gpu->copy_tex &&
        tex_copy_gpu_conv(cf) && copy_async(dest, x0, y0, W, H, dw, dh, cf))
        goto clear;
    img = (uint8_t*)calloc((size_t)dw * dh, 4);
    if (!img) return;
    PERF_COUNT(PERF_C_READPIX, 1);
    /* colour: resample on the GPU and read back only dw x dh (same integer
     * box mean as the loop below) */
    if (!z && s_copy_cpu_resample != 1 && g_gpu->read_pixels_box &&
        g_gpu->read_pixels_box(x0, y0, W, H, dw, dh, img)) {
        for (y = 0; y < dw * dh; y++) img[y * 4 + 3] = 0xFF; /* EFB RGB8 has no alpha */
        if (s_copy_cpu_resample != 2) goto encode;
        gpu_img = img;   /* SMB2_COPY_CPU_RESAMPLE=2: verify against the CPU path */
        img = (uint8_t*)calloc((size_t)dw * dh, 4);
        if (!img) { img = gpu_img; gpu_img = NULL; goto encode; }
    }
    if (!copy_cpu_resample(x0, y0, W, H, dw, dh, z, img)) { free(img); return; }
    if (gpu_img) {
        copy_verify("sync box", gpu_img, img, dw, dh, x0, y0, W, H, cf);
        free(gpu_img);
    }
encode:
    bytes = tex_level_bytes(dw, dh, cf);
    tc_land_overlaps(dest, bytes);
    tex_encode((uint8_t*)dest, img, dw, dh, cf);
    free(img);

    /* registry entry for dest */
    e = tc_slot(dest);
    s_tc[e].ptr = dest;
    s_tc[e].bytes = bytes;
    s_tc[e].hash = tex_hash((const uint8_t*)dest, bytes);
    s_tc[e].w = (uint16_t)dw;
    s_tc[e].h = (uint16_t)dh;
    s_tc[e].copy_fmt = cf;
    s_tc[e].stamp = ++s_tc_stamp;
    {   /* refresh the texture in place: texObjs initialised once keep
         * seeing the latest copy, as GC texture memory would */
        int fmt = s_tc[e].up_fmt >= 0 && s_tc[e].up_w == dw && s_tc[e].up_h == dh
                ? s_tc[e].up_fmt : tex_copy_sample_fmt(cf);
        s_tc[e].up_fmt = -1;
        tc_upload(e, fmt, dw, dh, 0, 0);
        hfr_note_sync_copy(s_tc[e].tex, x0, y0, W, H, dw, dh, cf, 0);
    }
    if (g_mtx_trace > 0)
        fprintf(stderr, "[trace] copytex src %u,%u %ux%u -> %dx%d fmt=0x%x clear=%d dest=%p tex=%p\n",
                s_cp_l, s_cp_t, s_cp_w, s_cp_h, dw, dh, cf, clear, dest, s_tc[e].tex);

clear:
    if (clear) {
        int box[4];
        box[0] = x0; box[1] = y0; box[2] = W; box[3] = H;
        g_gpu->clear(box, s_copy_clear_rgba, s_copy_clear_z, 1);
    }
}
/* ================ texgen/fog/texcopy block end ================== */


/* ---- PC port settings overlay (F1): keep the finished game frame ----
 * pc_frame_capture() keeps the frame the game just drew (not yet presented);
 * pc_frame_restore() draws it back, scaled to the current drawable (the
 * window may have been resized from the overlay), dimmed by brightness. */
int pc_frame_capture(void) {
    if (!g_core) return 0;
    gx_flush_pending(g_core);
    return g_gpu->capture();
}

void pc_frame_restore(float brightness) {
    if (!g_core) return;
    gx_flush_pending(g_core);
    g_gpu->restore(brightness);
}
