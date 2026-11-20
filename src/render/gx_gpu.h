/* Interface between the backend-neutral GX emulation (gx_core.c) and the
 * GPU API backends (render_opengl.c, render_d3d12.c).
 *
 * The core owns all GX state, vertex batching, texture decoding, EFB-copy
 * CPU work and the shader constant values; a backend only turns those into
 * API calls.  Coordinates handed to a backend are window (drawable) pixels
 * with GL's bottom-left origin; a top-left API flips y itself. */
#ifndef GX_GPU_H
#define GX_GPU_H

#include "render.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* primitive classes of a draw (lines are expanded to triangles by the core) */
enum { GXD_TRIANGLES, GXD_TRISTRIP, GXD_TRIFAN, GXD_QUADS, GXD_POINTS };

#define GXV_FLOATS 16             /* per vertex: pos3 nrm3 col4 tc0 tc1 tc2 */
#define GX_MAX_BATCH_VERTS 65536

/* Shader constants.  Both structs are laid out exactly like the HLSL
 * cbuffers (every member a multiple of 16 bytes, matrices column-major as GL
 * uploads them, which is also HLSL's default column_major packing). */
typedef struct {
    float proj[16];
    float mv[16];
    float nrm[16];         /* GXFX normal matrix (u_gx_nrm) */
    float texmtx[8][16];
    int   mode[4];         /* gx_mode (effective), ui_tev, has_tex, uvclamp */
    int   tg[4];           /* posmask, srcsel, nrmmask, normmask */
    int   tg2[4];          /* postmask, tex_valid, mtx2x4 mask (q = 1), 0 */
    int   acmp[4];         /* comp0, comp1, op, ref0 | ref1 << 8 */
    float tev_c0[4];
    float tev_c1[4];
    float fog[4];          /* type, start, end, 0 */
    float fog_color[4];
    float efb_scale[4];    /* window pixels per EFB pixel (x, y), D3D12 y nudge, 0 */
} GxDrawConsts;

typedef struct {
    float tg_post[8][16];  /* dual-tex post matrices per texgen */
    int   chan[2][4];      /* enable, matsrc, ambsrc, lightmask */
    int   chanfn[2][4];    /* diffuse fn, attn fn */
    float mat[4];
    float amb[4];
    float lpos[8][4];
    float ldir[8][4];
    float lcos[8][4];
    float ldist[8][4];
    float lcol[8][4];
    int   cin[8][4];
    int   ain[8][4];
    int   cop[8][4];
    int   aop[8][4];
    int   ord[8][4];
    float konst[8][4];
    float reg[4][4];
    int   misc[4];         /* tev_n, ind_on, ind_n, 0 */
    int   iord[4][4];
    float imtx[6][4];
    int   tind[8][4];
} GxTevConsts;

typedef struct {
    int prim;                  /* GXD_* */
    const float* verts;        /* GXV_FLOATS per vertex */
    int nverts;
    const float* rects;        /* 4 floats per vertex (2D quad UV rects) or NULL */
    const GxDrawConsts* dc;
    const GxTevConsts* tc;
    unsigned tc_serial;        /* changes whenever *tc changed */
    int depth_clamp;           /* no near/far clipping (ortho draws) */
    int no_cull;               /* quads and expanded lines are never culled */
} GxDrawCall;

struct GxDisplayTiming;
typedef struct GxGpu {
    const char* name;
    bool (*init)(int width, int height, bool vsync, int msaa);
    void (*shutdown)(void);
    void (*begin_frame)(void);
    void (*resize)(int width, int height);
    void (*present)(void);
    void (*set_vsync)(int on);
    /* state (window pixels, bottom-left origin) */
    void (*set_viewport)(int x, int y, int w, int h);
    void (*set_scissor)(int x, int y, int w, int h);   /* also enables it */
    void (*set_blend)(int type, int src, int dst, int op);
    void (*set_depth)(int compare, int func, int update);
    void (*set_cull)(int mode);
    /* Clear colour + depth of rect (NULL = whole target), ignoring the
     * scissor.  force = 0: obey the current depth write mask like glClear
     * (frame clear); force = 1: always clear both (EFB copy-clear). */
    void (*clear)(const int* rect, uint32_t rgba, float depth, int force);
    /* textures: rgba = linear RGBA8, top row first */
    void* (*tex_create)(const uint32_t* rgba, int w, int h, int wrapS, int wrapT, int mipmap);
    /* (re)define level 0 of *tex (created if NULL), LINEAR filter, no mips */
    void (*tex_image)(void** tex, const uint32_t* rgba, int w, int h);
    void (*tex_wrap)(void* tex, int wrapS, int wrapT);
    void (*tex_lod)(void* tex, int mipmap, int minFilt, int magFilt, float minLod,
                    float maxLod, float lodBias, int maxAniso);
    void (*tex_destroy)(void* tex);
    void (*bind_tex)(int stage, void* tex);
    void (*draw)(const GxDrawCall* d);
    /* EFB read for GXCopyTex: rows bottom-up (glReadPixels order); colour as
     * RGBA8, depth as float.  Returns 0 on failure. */
    int (*read_pixels)(int x, int y, int w, int h, int depth, void* out);
    /* GXCopyTex colour fast path (optional, NULL = unsupported): the GL-space
     * rect area-resampled to dw x dh on the GPU exactly like GXCopyTex's CPU
     * loop (box [i*w/dw, (i+1)*w/dw), >= 1 texel, integer mean, texels
     * outside the target = 0), output RGBA8 rows top-down (row 0 = top of
     * the rect).  Returns 0 on failure (caller falls back to read_pixels). */
    int (*read_pixels_box)(int x, int y, int w, int h, int dw, int dh, void* out);
    /* Asynchronous GXCopyTex (optional, NULL = unsupported): the same box
     * resample as read_pixels_box, converted on the GPU straight into the
     * texture *tex (created / resized to dw x dh as needed; it then samples
     * like a tex_image texture) as the copy format cf reads back through its
     * natural sample format (gx_core.c tex_copy_gpu_conv), plus a readback
     * of the raw resampled RGBA8 that completes later.  No CPU/GPU sync.
     * Returns a ticket (> 0), 0 on failure (nothing was done). */
    uint64_t (*copy_tex)(void** tex, int x, int y, int w, int h, int dw, int dh, int cf);
    /* cf | GXCOPY_NO_READBACK: only refresh *tex (HFR replays of a copy);
     * returns nonzero on success, there is no ticket to collect */
#define GXCOPY_NO_READBACK 0x10000
    /* Result of a copy_tex ticket: 0 = not finished yet (wait = 0), 1 = done:
     * raw resampled RGBA8 rows top-down in raw (dw*dh*4) and, when conv is
     * not NULL, the converted texels as stored in the texture; -1 = ticket
     * unknown / lost.  wait = 1 blocks until the GPU has finished it;
     * wait = -1 discards the ticket (its readback storage is reused).  Each
     * ticket is collected or discarded once. */
    int (*copy_tex_result)(uint64_t ticket, void* raw, void* conv, int wait);
    /* --perf memory log (optional): bytes of video memory in use by this
     * process (D3D12), or used on the whole GPU (GL, NVX_gpu_memory_info) */
    unsigned long long (*gpu_mem)(void);
    /* statistics (optional): mean GPU time per presented frame since the
     * last call, ms (0 = not available) */
    double (*gpu_frame_ms)(void);
    /* the finished frame (before present) as top-down RGB8, for frame dumps;
     * *rgb is owned by the backend */
    int (*read_frame)(unsigned char** rgb, int* w, int* h);
    int (*capture)(void);              /* F1 overlay: keep the game frame */
    void (*restore)(float brightness); /* draw it back (dimmed) */
    /* frame pacing (optional, docs/hfr.md section 4): id of the last
     * present (DXGI present count), 0 = not available */
    unsigned (*present_id)(void);
    /* the display's timing and the latest present it showed; returns 0
     * when unknown */
    int (*display_timing)(struct GxDisplayTiming* t);
    /* sync interval of the following presents (vsync on): the frame stays
     * on screen at least si refreshes (1-4) */
    void (*present_interval)(int si);
    /* presents that may wait for the display at once (flip queue depth,
     * 0 = the backend's default) */
    void (*present_queue)(int n);
    /* mean time from a present call's return to the GPU finishing that
     * frame since the last call, ms (GL; 0 = not available) */
    double (*present_done_ms)(void);
} GxGpu;

typedef struct GxDisplayTiming {
    unsigned long long vblank_ns;  /* a recent vertical blank, platform_get_time_ns clock */
    unsigned vblank_count;         /* its refresh number */
    double period_ns;              /* refresh period (measured) */
    unsigned shown_id;             /* latest present on screen (present_id numbering), 0 = none */
    unsigned shown_count;          /* refresh at which it was first shown */
} GxDisplayTiming;

extern const GxGpu gx_gpu_gl;
#ifdef SMB2_HAVE_D3D12
extern const GxGpu gx_gpu_d3d12;
#endif

/* render API extras used by the platform layer */
void render_present(void);
void render_set_vsync(int on);
int render_read_frame(unsigned char** rgb, int* w, int* h);
int render_is_gl(void);

#ifdef __cplusplus
}
#endif
#endif /* GX_GPU_H */
