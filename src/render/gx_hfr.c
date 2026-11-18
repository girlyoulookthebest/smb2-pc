/* High frame rate rendering: a GxGpu proxy records each 60 Hz tick's
 * backend command stream (hfr_tick_begin .. hfr_tick_end), pairs the draws
 * of the last two ticks (hfr_tick_end) and replays them with blended
 * transforms, vertices and colours (hfr_render).  Texture uploads are not
 * recorded: replays use the textures as the tick left them.
 * Design: docs/hfr.md. */
#include "gx_hfr.h"
#include "perf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

int g_hfr_cur_fps = 60;
double g_hfr_present_est_ns = 500000.0;   /* present call time, running average */
double g_hfr_first_ms_acc; long g_hfr_first_n;   /* latency statistic (video_shim.c) */
extern int pcs_effective_fps(void);
extern void gx_hfr_flush(void);   /* gx_core.c: draw the pending batch */

enum { OP_VIEWPORT, OP_SCISSOR, OP_BLEND, OP_DEPTH, OP_CULL, OP_CLEAR, OP_CLEAR_RECT,
       OP_BIND, OP_TEXWRAP, OP_TEXLOD, OP_DRAW, OP_COPYTEX };

typedef struct {
    int op;
    int a[7];
    float f[3];
    void* p;
} HfrOp;

enum { H_LIN, H_SAME, H_SLERP };

typedef struct {
    int prim, nverts, depth_clamp, no_cull;
    size_t vofs;          /* float offset into verts */
    long rofs;            /* float offset into rects, -1 = none */
    int dc, tc;           /* indices into dcs / tcs */
    unsigned tc_serial;
    unsigned tag;
    uint64_t key, key2;
    float pos[3];         /* first vertex through the modelview */
    float v0[3];          /* first vertex as submitted (model space for model draws) */
    float vl[3];          /* last vertex as submitted */
    float ori[9];         /* modelview 3x3, columns normalised (orientation) */
    unsigned char model;  /* v0 is in model space (non-identity modelview) */
    int match;            /* paired draw in the other stream, -1 */
    /* pair data, kept on the newer stream's draw */
    unsigned char how;    /* H_* */
    unsigned char vb;     /* blend vertex positions / normals / colours */
    unsigned char tcb;    /* blend the TEV constants (lights, colours) */
    unsigned char tcfg;   /* TEV configurations equal */
    int rot;              /* index into rots (H_SLERP) */
    unsigned char tmmask; /* texture matrices that differ in the pair */
    uint64_t vhash;       /* hash of the vertices (equal = same vertices) */
} HfrDraw;

typedef struct { float qp[4], qc[4], sp[3], sc[3]; } HfrRotPart;
typedef struct { HfrRotPart mv, nrm; int nrm_slerp; } HfrRot;

/* pointer set (textures) */
typedef struct { void** k; unsigned char* v; int cap, n; } PSet;

typedef struct {
    HfrOp* ops; int nops, cops;
    HfrDraw* draws; int ndraws, cdraws;
    GxDrawConsts* dcs; int ndcs, cdcs;
    GxTevConsts* tcs; uint64_t* tc_hash; int ntcs, ctcs; unsigned last_serial;
    float* verts; size_t nverts, cverts;
    float* rects; size_t nrects, crects;
    HfrRot* rots; int nrots, crots;
    struct KeyRef* kr; int ckr;   /* draws sorted by (full key, index), built at tick end */
    PSet texs;            /* textures bound (value: 1 = before its first copy, 2 = copied) */
    int open, complete, bad, cut, hazard;
    int matched, snapped, n3d, ncopies;
    unsigned long tick;
} HfrStream;

static const GxGpu* s_real;
static GxGpu s_proxy;
static int s_on;                   /* proxy selected for this tick */
static HfrStream s_st[2];
static int s_cur;                  /* stream being recorded / last recorded */
static int s_recording;
static unsigned long s_tick;
static int s_cut_req;              /* pc_hfr_cut during this tick */
static int s_cut_soft;             /* camera reset during this tick (soft cut) */
static PSet s_mod;                 /* textures re-uploaded during this tick */
/* Deferred recording: while a stream is recorded nothing is sent to the
 * backend until something needs the frame so far (an EFB copy or readback,
 * a texture re-upload, present); the tick's frame is drawn by the replay at
 * alpha 1 (hfr_render), so the main pass is not drawn twice.  SMB2_HFR_DEFER=0
 * sends everything at once (A/B checks). */
static int s_defer_cfg = -1;
static int s_defer;                /* this tick's stream is deferred */
static int s_fwd;                  /* first recorded op not sent yet */
static int s_final_pending;        /* the tick frame still has to be drawn */
static unsigned long s_stat_flushes;
static void flush_deferred(void);
static int keep(void);

/* backend state as last set by the core (for the stream prologue) */
static struct {
    int vp[4], vp_ok;
    int sc[4], sc_ok;
    int bl[4], bl_ok;
    int dp[3], dp_ok;
    int cull, cull_ok;
    void* tex[8];
} s_trk;

/* identity tags (pc_hfr_tag_push / pop) */
static unsigned s_tag_stack[16];
static int s_tag_depth;
static unsigned s_tag;

/* camera views (pc_hfr_view) per tick, for cut detection */
#define HFR_NCAM 8
static float s_view[2][HFR_NCAM][12];
static int s_view_ok[2][HFR_NCAM];
static float s_view_speed[HFR_NCAM];
static int s_vcur;

/* deferred texture destruction */
static struct HfrDq { void* tex; unsigned long tick; } *s_dq;
static int s_ndq, s_cdq;

static unsigned long s_stat_ticks, s_stat_draws, s_stat_matched, s_stat_snapped, s_stat_cuts,
                     s_stat_slerp, s_stat_vb, s_stat_copies, s_stat_copies_fb, s_stat_flushes, s_stat_hazard, s_stat_degraded,
                     s_stat_cut_hook, s_stat_cut_view, s_stat_cut_heur;

/* ---- pointer sets ---- */
static unsigned pset_hash(const void* key, int cap) {
    return (unsigned)(((uintptr_t)key >> 4) * 2654435761u) & (unsigned)(cap - 1);
}
static int pset_add(PSet* s, void* key, unsigned char val) {  /* returns the old value (0 = new) */
    unsigned h;
    if (!key) return 0;
    if (s->n * 2 >= s->cap) {
        int nc = s->cap ? s->cap * 2 : 256, i;
        void** nk = (void**)calloc((size_t)nc, sizeof(void*));
        unsigned char* nv = (unsigned char*)calloc((size_t)nc, 1);
        if (!nk || !nv) { free(nk); free(nv); return 0; }
        for (i = 0; i < s->cap; i++) if (s->k[i]) {
            unsigned j = pset_hash(s->k[i], nc);
            while (nk[j]) j = (j + 1) & (unsigned)(nc - 1);
            nk[j] = s->k[i]; nv[j] = s->v[i];
        }
        free(s->k); free(s->v);
        s->k = nk; s->v = nv; s->cap = nc;
    }
    h = pset_hash(key, s->cap);
    while (s->k[h]) {
        if (s->k[h] == key) { unsigned char o = s->v[h]; if (val > o) s->v[h] = val; return o; }
        h = (h + 1) & (unsigned)(s->cap - 1);
    }
    s->k[h] = key; s->v[h] = val; s->n++;
    return 0;
}
static int pset_get(const PSet* s, void* key) {
    unsigned h;
    if (!s->cap || !key) return 0;
    h = pset_hash(key, s->cap);
    while (s->k[h]) {
        if (s->k[h] == key) return s->v[h];
        h = (h + 1) & (unsigned)(s->cap - 1);
    }
    return 0;
}
static void pset_del(PSet* s, void* key) {   /* value 0 = absent (keeps the probe chain) */
    unsigned h;
    if (!s->cap || !key) return;
    h = pset_hash(key, s->cap);
    while (s->k[h]) {
        if (s->k[h] == key) { s->v[h] = 0; return; }
        h = (h + 1) & (unsigned)(s->cap - 1);
    }
}
static void pset_clear(PSet* s) {
    if (s->cap && s->n) { memset(s->k, 0, (size_t)s->cap * sizeof(void*)); memset(s->v, 0, (size_t)s->cap); }
    s->n = 0;
}

#define GROW(ptr, cap, need, type) do { \
    if ((size_t)(need) > (size_t)(cap)) { \
        size_t nc_ = (cap) ? (size_t)(cap) * 2 : 1024; \
        while (nc_ < (size_t)(need)) nc_ *= 2; \
        type* np_ = (type*)realloc((ptr), nc_ * sizeof(type)); \
        if (!np_) { s_st[s_cur].bad = 1; return; } \
        (ptr) = np_; (cap) = nc_; \
    } } while (0)

static void rec_op(int op, const int* a, int na, const float* f, int nf, void* p) {
    HfrStream* s = &s_st[s_cur];
    HfrOp* o;
    GROW(s->ops, s->cops, s->nops + 1, HfrOp);
    o = &s->ops[s->nops++];
    memset(o, 0, sizeof *o);
    o->op = op;
    if (na) memcpy(o->a, a, (size_t)na * sizeof(int));
    if (nf) memcpy(o->f, f, (size_t)nf * sizeof(float));
    o->p = p;
}

static uint64_t fnv(uint64_t h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    while (n--) { h ^= *p++; h *= 1099511628211ULL; }
    return h;
}

/* TEV configuration without the animated values (lights, konst, regs), of
 * the stages the draw runs only (GX keeps the setup of unused stages from
 * whatever was drawn before) */
static int tc_nstages(const GxTevConsts* t) {
    return t->misc[0] < 1 ? 1 : (t->misc[0] > 8 ? 8 : t->misc[0]);
}
static uint64_t tc_config_hash(const GxTevConsts* t) {
    uint64_t h = 1469598103934665603ULL;
    size_t n = (size_t)tc_nstages(t);
    h = fnv(h, t->chan, sizeof t->chan);
    h = fnv(h, t->chanfn, sizeof t->chanfn);
    h = fnv(h, t->cin, n * sizeof t->cin[0]);
    h = fnv(h, t->ain, n * sizeof t->ain[0]);
    h = fnv(h, t->cop, n * sizeof t->cop[0]);
    h = fnv(h, t->aop, n * sizeof t->aop[0]);
    h = fnv(h, t->ord, n * sizeof t->ord[0]);
    h = fnv(h, t->tind, n * sizeof t->tind[0]);
    h = fnv(h, t->misc, 2 * sizeof t->misc[0]);
    if (t->misc[1]) {   /* indirect stages only when a TEV stage reads them */
        int ni = t->misc[2] < 0 ? 0 : (t->misc[2] > 4 ? 4 : t->misc[2]);
        h = fnv(h, &t->misc[2], sizeof t->misc[2]);
        h = fnv(h, t->iord, (size_t)ni * sizeof t->iord[0]);
    }
    return h;
}

/* bound textures and texgen setup the draw reads: the GXFX
 * path reads the texture maps / coordinates of its active TEV and indirect
 * stages, the fixed paths texture 0 / coordinate 0 */
static uint64_t used_state_hash(uint64_t h, const GxDrawCall* d) {
    const GxTevConsts* t = d->tc;
    unsigned maps = 0, coords = 0;
    int k, tg[4];
    if (d->dc->mode[0]) {
        int n = tc_nstages(t);
        for (k = 0; k < n; k++)
            if (t->ord[k][1] >= 0 && t->ord[k][1] < 8) {
                maps |= 1u << t->ord[k][1];
                if (t->ord[k][0] >= 0 && t->ord[k][0] < 8) coords |= 1u << t->ord[k][0];
            }
        if (t->misc[1]) {
            int ni = t->misc[2] < 0 ? 0 : (t->misc[2] > 4 ? 4 : t->misc[2]);
            for (k = 0; k < ni; k++) {
                if (t->iord[k][1] >= 0 && t->iord[k][1] < 8) maps |= 1u << t->iord[k][1];
                if (t->iord[k][0] >= 0 && t->iord[k][0] < 8) coords |= 1u << t->iord[k][0];
            }
        }
    } else {
        maps = 7;   /* as before: texture maps 0-2 */
        coords = 1;
    }
    for (k = 0; k < 8; k++)
        if (maps & (1u << k)) h = fnv(h, &s_trk.tex[k], sizeof(void*));
    tg[0] = d->dc->tg[0] & (int)coords;
    tg[1] = 0;
    for (k = 0; k < 8; k++)
        if (coords & (1u << k)) tg[1] |= d->dc->tg[1] & (3 << (2 * k));
    tg[2] = d->dc->tg[2] & (int)coords;
    tg[3] = d->dc->tg[3] & (int)coords;
    h = fnv(h, &maps, sizeof maps);
    h = fnv(h, tg, sizeof tg);
    k = d->dc->tg2[2] & (int)coords;    /* MTX2x4 texgens */
    h = fnv(h, &k, sizeof k);
    return h;
}

/* ---- proxy ---- */
static bool px_init(int w, int h, bool vsync, int msaa) { return s_real->init(w, h, vsync, msaa); }
static void px_shutdown(void) { s_real->shutdown(); }
static void px_begin_frame(void) { s_real->begin_frame(); }
static void need_frame(void);
static void px_resize(int w, int h) { flush_deferred(); s_real->resize(w, h); }
static void px_present(void) { need_frame(); s_real->present(); }
static void px_set_vsync(int on) { s_real->set_vsync(on); }

static void px_set_viewport(int x, int y, int w, int h) {
    int a[4] = { x, y, w, h };
    memcpy(s_trk.vp, a, sizeof a); s_trk.vp_ok = 1;
    if (s_recording) { rec_op(OP_VIEWPORT, a, 4, NULL, 0, NULL); if (keep()) return; }
    s_real->set_viewport(x, y, w, h);
}
static void px_set_scissor(int x, int y, int w, int h) {
    int a[4] = { x, y, w, h };
    memcpy(s_trk.sc, a, sizeof a); s_trk.sc_ok = 1;
    if (s_recording) { rec_op(OP_SCISSOR, a, 4, NULL, 0, NULL); if (keep()) return; }
    s_real->set_scissor(x, y, w, h);
}
static void px_set_blend(int type, int src, int dst, int op) {
    int a[4] = { type, src, dst, op };
    memcpy(s_trk.bl, a, sizeof a); s_trk.bl_ok = 1;
    if (s_recording) { rec_op(OP_BLEND, a, 4, NULL, 0, NULL); if (keep()) return; }
    s_real->set_blend(type, src, dst, op);
}
static void px_set_depth(int c, int f, int u) {
    int a[3] = { c, f, u };
    memcpy(s_trk.dp, a, sizeof a); s_trk.dp_ok = 1;
    if (s_recording) { rec_op(OP_DEPTH, a, 3, NULL, 0, NULL); if (keep()) return; }
    s_real->set_depth(c, f, u);
}
static void px_set_cull(int mode) {
    s_trk.cull = mode; s_trk.cull_ok = 1;
    if (s_recording) { rec_op(OP_CULL, &mode, 1, NULL, 0, NULL); if (keep()) return; }
    s_real->set_cull(mode);
}
static void px_clear(const int* rect, uint32_t rgba, float depth, int force) {
    if (s_recording) {
        float f[2] = { depth, (float)force };
        if (rect) {
            rec_op(OP_CLEAR_RECT, rect, 4, f, 2, (void*)(uintptr_t)rgba);
        } else {
            int a[2] = { (int)rgba, force };
            rec_op(OP_CLEAR, a, 2, f, 1, NULL);
        }
        if (keep()) return;
    }
    s_real->clear(rect, rgba, depth, force);
}
static void* px_tex_create(const uint32_t* rgba, int w, int h, int ws, int wt, int mip) {
    return s_real->tex_create(rgba, w, h, ws, wt, mip);
}
static void px_tex_image(void** tex, const uint32_t* rgba, int w, int h) {
    if (*tex) { pset_add(&s_mod, *tex, 1); flush_deferred(); }
    s_real->tex_image(tex, rgba, w, h);
}
static void px_tex_wrap(void* tex, int ws, int wt) {
    if (s_recording) { int a[2] = { ws, wt }; rec_op(OP_TEXWRAP, a, 2, NULL, 0, tex); if (keep()) return; }
    s_real->tex_wrap(tex, ws, wt);
}
static void px_tex_lod(void* tex, int mip, int minf, int magf, float minl, float maxl,
                       float bias, int aniso) {
    if (s_recording) {
        int a[4] = { mip, minf, magf, aniso };
        float f[3] = { minl, maxl, bias };
        rec_op(OP_TEXLOD, a, 4, f, 3, tex);
        if (keep()) return;
    }
    s_real->tex_lod(tex, mip, minf, magf, minl, maxl, bias, aniso);
}
static void dq_flush(unsigned long upto) {
    int i, n = 0;
    for (i = 0; i < s_ndq; i++) {
        if (s_dq[i].tick <= upto) s_real->tex_destroy(s_dq[i].tex);
        else s_dq[n++] = s_dq[i];
    }
    s_ndq = n;
}
static void px_tex_destroy(void* tex) {
    int i;
    if (!tex) return;
    for (i = 0; i < 8; i++) if (s_trk.tex[i] == tex) s_trk.tex[i] = NULL;
    /* the kept streams may still reference it: destroyed two ticks later */
    if (s_ndq >= s_cdq) {
        int nc = s_cdq ? s_cdq * 2 : 64;
        struct HfrDq* np = (struct HfrDq*)realloc(s_dq, (size_t)nc * sizeof *s_dq);
        if (!np) { s_st[0].complete = s_st[1].complete = 0; s_real->tex_destroy(tex); return; }
        s_dq = np; s_cdq = nc;
    }
    s_dq[s_ndq].tex = tex;
    s_dq[s_ndq].tick = s_tick;
    s_ndq++;
}
static void px_bind_tex(int stage, void* tex) {
    if (stage >= 0 && stage < 8) s_trk.tex[stage] = tex;
    if (s_recording) {
        rec_op(OP_BIND, &stage, 1, NULL, 0, tex);
        pset_add(&s_st[s_cur].texs, tex, 1);   /* 1 = bound before any copy into it */
        if (keep()) return;
    }
    s_real->bind_tex(stage, tex);
}

static uint64_t px_copy_tex(void** tex, int x, int y, int w, int h, int dw, int dh, int cf) {
    uint64_t t;
    flush_deferred();
    t = s_real->copy_tex(tex, x, y, w, h, dw, dh, cf);
    if (t && s_recording && *tex) {
        HfrStream* s = &s_st[s_cur];
        int a[7] = { x, y, w, h, dw, dh, cf };
        /* sampled earlier in this tick without an earlier copy = the pass
         * reads the previous contents (feedback): not re-executed */
        int st = pset_add(&s->texs, *tex, 2);
        float f[1] = { st == 1 ? 1.0f : 0.0f };
        rec_op(OP_COPYTEX, a, 7, f, 1, *tex);
        s->ncopies++;
        if (s_defer) s_fwd = s->nops;   /* already done */
    }
    return t;
}

static float v3len(const float* v);
static uint64_t s_last_vhash;

static void rec_draw(const GxDrawCall* d) {
    HfrStream* s = &s_st[s_cur];
    HfrDraw* r;
    uint64_t h = 1469598103934665603ULL, h2;
    int k;
    size_t nv = (size_t)d->nverts * GXV_FLOATS;
    if (s->ntcs == 0 || s->last_serial != d->tc_serial) {
        GROW(s->tcs, s->ctcs, s->ntcs + 1, GxTevConsts);
        {   /* tc_hash grows alongside (same capacity) */
            uint64_t* nh = (uint64_t*)realloc(s->tc_hash, (size_t)s->ctcs * sizeof(uint64_t));
            if (!nh) { s->bad = 1; return; }
            s->tc_hash = nh;
        }
        s->tcs[s->ntcs] = *d->tc;
        s->tc_hash[s->ntcs] = tc_config_hash(d->tc);
        s->ntcs++;
        s->last_serial = d->tc_serial;
    }
    GROW(s->dcs, s->cdcs, s->ndcs + 1, GxDrawConsts);
    s->dcs[s->ndcs] = *d->dc;
    GROW(s->verts, s->cverts, s->nverts + nv, float);
    {   /* copy + hash in one pass (pairs compare hashes, not vertices) */
        const uint32_t* src = (const uint32_t*)d->verts;
        uint32_t* dst = (uint32_t*)(s->verts + s->nverts);
        uint64_t vh = 1469598103934665603ULL;
        size_t q;
        for (q = 0; q < nv; q++) { uint32_t w = src[q]; dst[q] = w; vh = (vh ^ w) * 1099511628211ULL; }
        s_last_vhash = vh;
    }
    GROW(s->draws, s->cdraws, s->ndraws + 1, HfrDraw);
    r = &s->draws[s->ndraws];
    memset(r, 0, sizeof *r);
    r->prim = d->prim;
    r->nverts = d->nverts;
    r->depth_clamp = d->depth_clamp;
    r->no_cull = d->no_cull;
    r->vofs = s->nverts;
    r->rofs = -1;
    if (d->rects) {
        size_t nr = (size_t)d->nverts * 4;
        GROW(s->rects, s->crects, s->nrects + nr, float);
        memcpy(s->rects + s->nrects, d->rects, nr * sizeof(float));
        r->rofs = (long)s->nrects;
        s->nrects += nr;
    }
    r->dc = s->ndcs;
    r->tc = s->ntcs - 1;
    r->tc_serial = d->tc_serial;
    r->match = -1;
    r->rot = -1;
    r->tag = s_tag;
    r->vhash = s_last_vhash;
    {
        const float* m = d->dc->mv;
        const float* v = d->verts;
        r->pos[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12];
        r->pos[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13];
        r->pos[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14];
        r->v0[0] = v[0]; r->v0[1] = v[1]; r->v0[2] = v[2];
        {
            const float* vl = v + (size_t)(d->nverts - 1) * GXV_FLOATS;
            r->vl[0] = vl[0]; r->vl[1] = vl[1]; r->vl[2] = vl[2];
        }
        for (k = 0; k < 3; k++) {
            float l = v3len(&m[k * 4]);
            l = l > 1e-8f ? 1.0f / l : 0.0f;
            r->ori[k * 3] = m[k * 4] * l; r->ori[k * 3 + 1] = m[k * 4 + 1] * l; r->ori[k * 3 + 2] = m[k * 4 + 2] * l;
        }
        r->model = !(m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[12] == 0.0f &&
                     m[13] == 0.0f && m[14] == 0.0f);
    }
    s->nverts += nv;
    s->ndcs++;
    /* keys: the loose one (identity, shape) and the full one (+ modes and
     * the textures / texgens / TEV configuration the draw reads: the setup
     * GX keeps from earlier draws changed the key whenever an earlier draw
     * changed - a dogfight monkey then fell to the loose key and broke into
     * pieces - or told copies of one model apart by their draw order) */
    h = fnv(h, &s_tag, sizeof s_tag);
    h = fnv(h, &d->prim, sizeof d->prim);
    h = fnv(h, &d->nverts, sizeof d->nverts);
    h = fnv(h, &d->depth_clamp, sizeof d->depth_clamp);
    h2 = h;
    h = fnv(h, d->dc->mode, sizeof d->dc->mode);
    h = used_state_hash(h, d);
    h = fnv(h, &s->tc_hash[r->tc], sizeof(uint64_t));
    r->key = h;
    r->key2 = h2;
    rec_op(OP_DRAW, &s->ndraws, 1, NULL, 0, NULL);
    s->ndraws++;
}

static void px_draw(const GxDrawCall* d) {
    if (s_recording && d->nverts > 0) {
        rec_draw(d);
        if (keep()) return;
    }
    s_real->draw(d);
}

/* the frame so far is needed: send what is still deferred.  The tick frame
 * itself (deferred to the alpha-1 replay) is drawn by the pacer after the
 * in-between frames; only consumers of the finished tick frame outside the
 * pacer (F1 capture) draw it here - an in-between present must not. */
static void need_frame(void) {
    flush_deferred();
}
static void need_tick_frame(void) {
    flush_deferred();
    if (hfr_final_pending()) hfr_render(1.0f);
}
static int px_read_pixels(int x, int y, int w, int h, int depth, void* out) {
    flush_deferred();
    return s_real->read_pixels(x, y, w, h, depth, out);
}
static int px_read_pixels_box(int x, int y, int w, int h, int dw, int dh, void* out) {
    flush_deferred();
    return s_real->read_pixels_box(x, y, w, h, dw, dh, out);
}
static int px_read_frame(unsigned char** rgb, int* w, int* h) { need_frame(); return s_real->read_frame(rgb, w, h); }
static int px_capture(void) { need_tick_frame(); return s_real->capture(); }
static void px_restore(float b) { s_real->restore(b); }

void hfr_init(const GxGpu* real) {
    s_real = real;
    s_proxy = *real;
    s_proxy.init = px_init;
    s_proxy.shutdown = px_shutdown;
    s_proxy.begin_frame = px_begin_frame;
    s_proxy.resize = px_resize;
    s_proxy.present = px_present;
    s_proxy.set_vsync = px_set_vsync;
    s_proxy.set_viewport = px_set_viewport;
    s_proxy.set_scissor = px_set_scissor;
    s_proxy.set_blend = px_set_blend;
    s_proxy.set_depth = px_set_depth;
    s_proxy.set_cull = px_set_cull;
    s_proxy.clear = px_clear;
    s_proxy.tex_create = px_tex_create;
    s_proxy.tex_image = px_tex_image;
    s_proxy.tex_wrap = px_tex_wrap;
    s_proxy.tex_lod = px_tex_lod;
    s_proxy.tex_destroy = px_tex_destroy;
    s_proxy.bind_tex = px_bind_tex;
    s_proxy.draw = px_draw;
    s_proxy.read_pixels = px_read_pixels;
    s_proxy.read_pixels_box = real->read_pixels_box ? px_read_pixels_box : NULL;
    s_proxy.copy_tex = real->copy_tex ? px_copy_tex : NULL;
    s_proxy.read_frame = px_read_frame;
    s_proxy.capture = px_capture;
    s_proxy.restore = px_restore;
}

int hfr_active(void) { return s_on; }

const GxGpu* hfr_select(void) {
    int want;
    if (!s_real) return NULL;
    want = pcs_effective_fps() != 60;
    if (want != s_on) {
        need_tick_frame();
        s_on = want;
        s_recording = 0;
        s_final_pending = 0;
        s_st[0].complete = s_st[1].complete = 0;
        s_st[0].ndraws = s_st[1].ndraws = 0;
        if (!want) {
            dq_flush((unsigned long)-1);
            g_hfr_cur_fps = 60;
        } else {
            /* backend state the core set before the switch is unknown to
             * the proxy: the first stream's prologue covers only what the
             * core sets again (it sets everything it draws with) */
            memset(&s_trk, 0, sizeof s_trk);
        }
        printf("[hfr] interpolated rendering %s (target %d fps)\n", want ? "on" : "off",
               pcs_effective_fps());
        fflush(stdout);
    }
    return s_on ? &s_proxy : s_real;
}

static void node_map_clear(void);

/* degraded: the tick ran late even without in-between frames (a scene too
 * heavy for recording + matching on this machine): record nothing for a
 * while - the frame is drawn directly, exactly like the 60 fps path */
static int s_degrade;
void hfr_degrade(int ticks) { if (ticks > s_degrade) s_degrade = ticks; }

/* ---- tick boundaries ---- */
void hfr_tick_begin(void) {
    HfrStream* s;
    int i;
    if (!s_on) return;
    if (s_degrade > 0 && !s_recording) {
        s_degrade--;
        s_stat_degraded++;
        s_st[0].complete = s_st[1].complete = 0;
        s_final_pending = 0;
        return;
    }
    if (s_recording) {
        /* a second frame kickoff inside one tick: keep only the last one */
        s_st[s_cur].open = 0;
    } else {
        s_cur ^= 1;
        s_tick++;
        dq_flush(s_tick >= 3 ? s_tick - 3 : 0);
        pset_clear(&s_mod);
    }
    s = &s_st[s_cur];
    s->nops = s->ndraws = s->ndcs = s->ntcs = s->nrots = 0;
    s->nverts = s->nrects = 0;
    s->complete = s->bad = s->cut = s->hazard = 0;
    s->matched = s->snapped = s->n3d = s->ncopies = 0;
    s->tick = s_tick;
    pset_clear(&s->texs);
    s->open = 1;
    if (s_defer_cfg < 0) {
        const char* e = getenv("SMB2_HFR_DEFER");
        s_defer_cfg = e ? atoi(e) != 0 : 1;
    }
    if (s_recording) flush_deferred();   /* second kickoff: the first frame stays as sent */
    s_defer = s_defer_cfg;
    s_fwd = 0;
    s_final_pending = 0;
    s_recording = 1;
    s_tag_depth = 0;
    s_tag = 0;
    node_map_clear();
    /* prologue: the backend state this frame starts from */
    if (s_trk.vp_ok) rec_op(OP_VIEWPORT, s_trk.vp, 4, NULL, 0, NULL);
    if (s_trk.sc_ok) rec_op(OP_SCISSOR, s_trk.sc, 4, NULL, 0, NULL);
    if (s_trk.bl_ok) rec_op(OP_BLEND, s_trk.bl, 4, NULL, 0, NULL);
    if (s_trk.dp_ok) rec_op(OP_DEPTH, s_trk.dp, 3, NULL, 0, NULL);
    if (s_trk.cull_ok) rec_op(OP_CULL, &s_trk.cull, 1, NULL, 0, NULL);
    for (i = 0; i < 8; i++) rec_op(OP_BIND, &i, 1, NULL, 0, s_trk.tex[i]);
}

/* ---- matrix helpers (column-major 4x4, m[c * 4 + r]) ---- */
static float v3len(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static float v3dist(const float* a, const float* b) {
    float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return v3len(d);
}

static void quat_from_cols(const float* c0, const float* c1, const float* c2, float q[4]) {
    /* rotation matrix with columns c0 c1 c2: m(r, c) = c_c[r] */
    float m00 = c0[0], m10 = c0[1], m20 = c0[2];
    float m01 = c1[0], m11 = c1[1], m21 = c1[2];
    float m02 = c2[0], m12 = c2[1], m22 = c2[2];
    float tr = m00 + m11 + m22, s, n;
    if (tr > 0.0f) {
        s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s; q[0] = (m21 - m12) / s; q[1] = (m02 - m20) / s; q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q[3] = (m21 - m12) / s; q[0] = 0.25f * s; q[1] = (m01 + m10) / s; q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q[3] = (m02 - m20) / s; q[0] = (m01 + m10) / s; q[1] = 0.25f * s; q[2] = (m12 + m21) / s;
    } else {
        s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q[3] = (m10 - m01) / s; q[0] = (m02 + m20) / s; q[1] = (m12 + m21) / s; q[2] = 0.25f * s;
    }
    n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n > 0.0f) { q[0] /= n; q[1] /= n; q[2] /= n; q[3] /= n; }
}

static void quat_slerp(const float* a, const float* b0, float t, float* o) {
    float b[4] = { b0[0], b0[1], b0[2], b0[3] };
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], wa, wb, n;
    int i;
    if (d < 0.0f) { d = -d; for (i = 0; i < 4; i++) b[i] = -b[i]; }
    if (d > 0.9995f) { wa = 1.0f - t; wb = t; }
    else {
        float th = acosf(d), sn = sinf(th);
        wa = sinf((1.0f - t) * th) / sn;
        wb = sinf(t * th) / sn;
    }
    for (i = 0; i < 4; i++) o[i] = wa * a[i] + wb * b[i];
    n = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2] + o[3] * o[3]);
    if (n > 0.0f) for (i = 0; i < 4; i++) o[i] /= n;
}

/* split the 3x3 of m into rotation (quaternion) and column scales.
 * Returns 0 when it is not a usable rotation * scale (degenerate, sheared). */
static int decompose(const float* m, float q[4], float s[3]) {
    float c[3][3];
    int i, k;
    float det;
    for (i = 0; i < 3; i++) {
        s[i] = v3len(&m[i * 4]);
        if (s[i] < 1e-8f) return 0;
        for (k = 0; k < 3; k++) c[i][k] = m[i * 4 + k] / s[i];
    }
    det = c[0][0] * (c[1][1] * c[2][2] - c[1][2] * c[2][1]) -
          c[1][0] * (c[0][1] * c[2][2] - c[0][2] * c[2][1]) +
          c[2][0] * (c[0][1] * c[1][2] - c[0][2] * c[1][1]);
    if (det < 0.0f) {   /* mirrored: fold the reflection into the scale */
        s[2] = -s[2];
        for (k = 0; k < 3; k++) c[2][k] = -c[2][k];
    }
    if (fabsf(fabsf(det) - 1.0f) > 0.05f) return 0;
    quat_from_cols(c[0], c[1], c[2], q);
    return 1;
}

static float quat_angle(const float* a, const float* b) {
    float d = fabsf(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    if (d > 1.0f) d = 1.0f;
    return 2.0f * acosf(d);
}

static int affine(const float* m) {
    return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f;
}

#define SLERP_MIN_ANGLE 0.05f   /* ~3 deg per tick: below, element-wise is exact enough */
#define JUMP_ANGLE 2.6f         /* ~150 deg per tick: a different object / a flip */

/* rotation analysis of a pair: 0 = element-wise, 1 = slerp (r filled),
 * -1 = jump (no interpolation) */
static int rot_analyze(const float* p, const float* c, HfrRotPart* r) {
    float ang;
    if (!affine(p) || !affine(c)) return 0;
    {   /* quick test: every normalised column turned less than ~2.5 deg
         * means the rotation is below the slerp threshold (no decomposition) */
        int k, small = 1;
        for (k = 0; k < 3 && small; k++) {
            const float* a = &p[k * 4];
            const float* b = &c[k * 4];
            float la = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
            float lb = b[0] * b[0] + b[1] * b[1] + b[2] * b[2];
            float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            if (!(d > 0.0f && d * d > 0.99905f * la * lb)) small = 0;   /* cos^2(1.75 deg) */
        }
        if (small) return 0;
    }
    if (!decompose(p, r->qp, r->sp) || !decompose(c, r->qc, r->sc)) return 0;
    if ((r->sp[2] < 0.0f) != (r->sc[2] < 0.0f)) return 0;
    ang = quat_angle(r->qp, r->qc);
    if (ang > JUMP_ANGLE) return -1;
    return ang > SLERP_MIN_ANGLE ? 1 : 0;
}

/* rotation slerp, scale and translation lerp */
static void rot_interp(float* o, const float* p, const float* c, float a, const HfrRotPart* r) {
    float q[4], s[3], x2, y2, z2, xy, xz, yz, wx, wy, wz;
    int i;
    quat_slerp(r->qp, r->qc, a, q);
    for (i = 0; i < 3; i++) s[i] = r->sp[i] + (r->sc[i] - r->sp[i]) * a;
    x2 = q[0] * q[0]; y2 = q[1] * q[1]; z2 = q[2] * q[2];
    xy = q[0] * q[1]; xz = q[0] * q[2]; yz = q[1] * q[2];
    wx = q[3] * q[0]; wy = q[3] * q[1]; wz = q[3] * q[2];
    o[0] = (1 - 2 * (y2 + z2)) * s[0]; o[1] = 2 * (xy + wz) * s[0]; o[2] = 2 * (xz - wy) * s[0];
    o[4] = 2 * (xy - wz) * s[1]; o[5] = (1 - 2 * (x2 + z2)) * s[1]; o[6] = 2 * (yz + wx) * s[1];
    o[8] = 2 * (xz + wy) * s[2]; o[9] = 2 * (yz - wx) * s[2]; o[10] = (1 - 2 * (x2 + y2)) * s[2];
    o[3] = o[7] = o[11] = 0.0f;
    for (i = 12; i < 15; i++) o[i] = p[i] + (c[i] - p[i]) * a;
    o[15] = 1.0f;
}

static void lerpn(float* out, const float* p, const float* c, float a, int n) {
    int i;
    for (i = 0; i < n; i++) out[i] = p[i] + (c[i] - p[i]) * a;
}

/* texture matrix: scroll translations that wrapped by whole repeats blend
 * the short way; other jumps keep the base value */
static void texmtx_interp(float* o, const float* p, const float* c, float a) {
    int i;
    for (i = 0; i < 16; i++) {
        float d = c[i] - p[i];
        if (fabsf(d) <= 0.5f) { o[i] = p[i] + d * a; continue; }
        if (i >= 12) {
            float r = d - floorf(d + 0.5f);
            if (fabsf(r) <= 0.25f) { o[i] = p[i] + r * a; continue; }
        }
    }
}

/* ---- matching ---- */
typedef struct KeyRef { uint64_t key; int idx; } KeyRef;

/* stable LSD radix sort by key (draw order kept within a key) */
static KeyRef* s_rtmp; static int s_crtmp;
static int radix_sort(KeyRef* a, int n) {
    int pass, i;
    KeyRef *src = a, *dst;
    if (n < 2) return 1;
    if (n > s_crtmp) {
        KeyRef* nt = (KeyRef*)realloc(s_rtmp, (size_t)n * 2 * sizeof(KeyRef));
        if (!nt) return 0;
        s_rtmp = nt; s_crtmp = n * 2;
    }
    dst = s_rtmp;
    for (pass = 0; pass < 8; pass++) {
        int cnt[256], sh = pass * 8, b;
        memset(cnt, 0, sizeof cnt);
        for (i = 0; i < n; i++) cnt[(src[i].key >> sh) & 255]++;
        for (b = 0; b < 256 && cnt[b] != n; b++) {}
        if (b < 256) continue;               /* all equal in this byte */
        {
            int sum = 0;
            for (b = 0; b < 256; b++) { int c = cnt[b]; cnt[b] = sum; sum += c; }
        }
        for (i = 0; i < n; i++) dst[cnt[(src[i].key >> sh) & 255]++] = src[i];
        { KeyRef* t = src; src = dst; dst = t; }
    }
    if (src != a) memcpy(a, src, (size_t)n * sizeof(KeyRef));
    return 1;
}
static KeyRef *s_krp, *s_krc; static int s_ckrp, s_ckrc;
typedef struct { float d; int i, j; } Cand;
static Cand* s_cand; static int s_ccand;
static int cand_cmp(const void* a, const void* b) {
    float x = ((const Cand*)a)->d, y = ((const Cand*)b)->d;
    return x < y ? -1 : x > y;
}

static float near_thresh(const HfrDraw* c) {
    return c->depth_clamp ? 2.0f : 0.5f + 0.05f * v3len(c->pos);
}
static float jump_thresh(const HfrDraw* c, int strict) {
    if (c->depth_clamp) return strict ? 32.0f : 160.0f;
    return strict ? 1.0f + 0.1f * v3len(c->pos) : 4.0f + 0.25f * v3len(c->pos);
}

/* orientation difference of two modelviews (sum of |delta| of the
 * normalised 3x3, ~1.7 per 30 deg) */
static float ori_diff(const HfrDraw* p, const HfrDraw* c) {
    float r = 0.0f;
    int k;
    for (k = 0; k < 9; k++) r += fabsf(p->ori[k] - c->ori[k]);
    return r;
}

/* pairing distance: the view-space move, plus (for model draws) how far
 * apart the meshes are in model space - the pieces of one model drawn with
 * one modelview differ there, the same piece never does */
static float pair_dist(const HfrDraw* p, const HfrDraw* c) {
    float d = v3dist(p->pos, c->pos);
    if (p->model && c->model) {
        d += 4.0f * (v3dist(p->v0, c->v0) + v3dist(p->vl, c->vl));
        /* orientation: identical meshes placed at other angles (tiles of
         * one object) are other pieces; ~ 1 unit per 30 deg of turn */
        d += 0.6f * ori_diff(p, c);
    }
    return d;
}

/* can these be the same object at all?  Model draws must be the same mesh:
 * equal model-space first vertex, or nearly (meshes the CPU deforms) */
static int pair_ok(const HfrDraw* p, const HfrDraw* c, float d, float jt) {
    if (d > jt) return 0;
    if (p->model && c->model) {
        if (v3dist(p->v0, c->v0) > 0.02f + 0.02f * v3len(c->v0)) return 0;
        if (v3dist(p->vl, c->vl) > 0.02f + 0.02f * v3len(c->vl)) return 0;
    }
    return 1;
}

static void pair(HfrStream* P, HfrStream* C, int j, int i) {
    P->draws[j].match = i;
    C->draws[i].match = j;
}

/* one more pair of an in-order run (ref: the previous pair's move, updated) */
static int run_step(const HfrDraw* p, const HfrDraw* c, float* ref, int have_ref) {
    float m[3] = { c->pos[0] - p->pos[0], c->pos[1] - p->pos[1], c->pos[2] - p->pos[2] };
    float d = v3len(m), e[3];
    if (p->model != c->model) return 0;
    if (p->model && (memcmp(p->v0, c->v0, sizeof p->v0) != 0 || memcmp(p->vl, c->vl, sizeof p->vl) != 0)) return 0;
    if (d > jump_thresh(c, 0)) return 0;
    if (d > near_thresh(c)) {
        if (!have_ref) return 0;
        e[0] = m[0] - ref[0]; e[1] = m[1] - ref[1]; e[2] = m[2] - ref[2];
        if (v3len(e) > near_thresh(c)) return 0;
    }
    ref[0] = m[0]; ref[1] = m[1]; ref[2] = m[2];
    return 1;
}

static void group_assign(HfrStream* P, HfrStream* C, const KeyRef* pg, int np,
                         const KeyRef* cg, int nc, int strict) {
    int n = np < nc ? np : nc, i, j, ok = !strict && np == nc;
    if (n > 0 && C->draws[cg[0].idx].depth_clamp) {
        /* 2D: identical quads (the same glyph, the same icon) are
         * interchangeable, so nearest-position pairing would send letters
         * of a new text flying to the old text's letters.  Pair in order
         * only when the whole group moved together (a sliding banner /
         * menu panel), else only what stayed where it was. */
        const HfrDraw* p0 = &P->draws[pg[0].idx];
        const HfrDraw* c0 = &C->draws[cg[0].idx];
        float dx = c0->pos[0] - p0->pos[0], dy = c0->pos[1] - p0->pos[1];
        if (ok && dx * dx + dy * dy <= 160.0f * 160.0f) {
            for (i = 1; ok && i < n; i++) {
                const HfrDraw* p = &P->draws[pg[i].idx];
                const HfrDraw* c = &C->draws[cg[i].idx];
                if (fabsf(c->pos[0] - p->pos[0] - dx) > 3.0f || fabsf(c->pos[1] - p->pos[1] - dy) > 3.0f)
                    ok = 0;
            }
            if (ok) {
                for (i = 0; i < n; i++) pair(P, C, pg[i].idx, cg[i].idx);
                return;
            }
        }
        /* else: unambiguous mutual nearest pairs (each moved much less
         * than the distance to any other candidate), or quads that stayed */
        if ((long)np * nc <= 4096) {
            for (i = 0; i < nc; i++) {
                const HfrDraw* c = &C->draws[cg[i].idx];
                float d1 = 3.4e38f, d2 = 3.4e38f, e1 = 3.4e38f;
                int bj = -1, k;
                for (j = 0; j < np; j++) {
                    float d = v3dist(P->draws[pg[j].idx].pos, c->pos);
                    if (d < d1) { d2 = d1; d1 = d; bj = j; } else if (d < d2) d2 = d;
                }
                if (bj < 0 || P->draws[pg[bj].idx].match >= 0) continue;
                if (!(d1 <= 1.0f || (d1 < 0.5f * d2 && d1 <= 160.0f))) continue;
                for (k = 0; k < nc; k++) {   /* mutual: no other C quad nearer to it */
                    float d = v3dist(P->draws[pg[bj].idx].pos, C->draws[cg[k].idx].pos);
                    if (k != i && d < e1) e1 = d;
                }
                if (d1 > 1.0f && !(d1 < 0.5f * e1)) continue;
                pair(P, C, pg[bj].idx, cg[i].idx);
            }
        } else {
            for (i = 0; i < nc && i < 256; i++) {   /* big groups: unmoved only */
                const HfrDraw* c = &C->draws[cg[i].idx];
                for (j = 0; j < np; j++) {
                    const HfrDraw* p = &P->draws[pg[j].idx];
                    if (p->match < 0 && v3dist(p->pos, c->pos) <= 1.0f) { pair(P, C, pg[j].idx, cg[i].idx); break; }
                }
            }
        }
        return;
    }
    /* in order when each pair is the same mesh (equal model-space vertex)
     * within jump range, or everything is close.  Both relative to the
     * group's mean move: a skinned (CPU-transformed) monkey passing the
     * camera moves farther in a tick than its pieces are apart (nearest-
     * first then blended vertices between pieces), and copies of one model
     * drawn in another order this tick (dogfight) are the same mesh in
     * jump range but each moved away from the group, with a nearer copy
     * of the same orientation at hand (a camera turn changes the
     * orientation of every copy alike). */
    if (ok && n > 0) {
        float mv[3] = { 0.0f, 0.0f, 0.0f };
        for (i = 0; i < n; i++) {
            const HfrDraw* p = &P->draws[pg[i].idx];
            const HfrDraw* c = &C->draws[cg[i].idx];
            mv[0] += c->pos[0] - p->pos[0]; mv[1] += c->pos[1] - p->pos[1]; mv[2] += c->pos[2] - p->pos[2];
        }
        mv[0] /= (float)n; mv[1] /= (float)n; mv[2] /= (float)n;
        for (i = 0; ok && i < n; i++) {
            const HfrDraw* p = &P->draws[pg[i].idx];
            const HfrDraw* c = &C->draws[cg[i].idx];
            float d = v3dist(p->pos, c->pos);
            float e[3] = { c->pos[0] - p->pos[0] - mv[0], c->pos[1] - p->pos[1] - mv[1],
                           c->pos[2] - p->pos[2] - mv[2] };
            int with_group = v3len(e) <= near_thresh(c);
            if (p->model && c->model) {
                /* model draws: the same mesh (equal model-space vertex), and
                 * not suddenly turned the other way */
                if (memcmp(p->v0, c->v0, sizeof p->v0) != 0 || memcmp(p->vl, c->vl, sizeof p->vl) != 0 ||
                    d > jump_thresh(c, 0)) {
                    ok = 0;
                } else if (!with_group && d > near_thresh(c) && ori_diff(p, c) > 1.0f) {
                    float own = pair_dist(p, c), od = ori_diff(p, c), jt = jump_thresh(c, 0);
                    int t, lo = i - 32 < 0 ? 0 : i - 32, hi = i + 33 > n ? n : i + 33;
                    for (t = lo; ok && t < hi; t++) {
                        const HfrDraw* q = &P->draws[pg[t].idx];
                        float dq;
                        if (t == i) continue;
                        dq = pair_dist(q, c);
                        if (dq + near_thresh(c) < own && ori_diff(q, c) < 0.5f * od && pair_ok(q, c, dq, jt))
                            ok = 0;
                    }
                }
            } else if (d > near_thresh(c) && (!with_group || d > jump_thresh(c, 0))) {
                ok = 0;
            }
        }
    }
    if (ok) {   /* same count, everything close: the order is the identity */
        for (i = 0; i < n; i++) pair(P, C, pg[i].idx, cg[i].idx);
        return;
    }
    if (!strict && n > 0) {
        /* draws appeared / disappeared (or were reordered) inside the
         * group: the runs at its start and end that are still in order
         * pair in order (a run starts at a pair that stayed close and goes
         * on while the same mesh moves like its predecessor), the rest
         * below.  Merged groups of one small mesh (the quads of two guns)
         * otherwise paired by proportional position or nearest piece. */
        float ref[3];
        int a = 0, b = 0;
        while (a < n && run_step(&P->draws[pg[a].idx], &C->draws[cg[a].idx], ref, a > 0)) a++;
        while (b < n - a && run_step(&P->draws[pg[np - 1 - b].idx], &C->draws[cg[nc - 1 - b].idx], ref, b > 0)) b++;
        if (a + b > 0) {
            for (i = 0; i < a; i++) pair(P, C, pg[i].idx, cg[i].idx);
            for (i = 0; i < b; i++) pair(P, C, pg[np - 1 - i].idx, cg[nc - 1 - i].idx);
            if (np - a - b > 0 && nc - a - b > 0)
                group_assign(P, C, pg + a, np - a - b, cg + a, nc - a - b, strict);
            return;
        }
    }
    if ((long)np * nc <= 4096) {
        /* nearest first, each draw used once */
        int nk = 0;
        if (np * nc > s_ccand) {
            Cand* nb = (Cand*)realloc(s_cand, (size_t)np * nc * sizeof(Cand));
            if (!nb) return;
            s_cand = nb; s_ccand = np * nc;
        }
        for (i = 0; i < nc; i++) {
            const HfrDraw* c = &C->draws[cg[i].idx];
            float jt = jump_thresh(c, strict);
            for (j = 0; j < np; j++) {
                float d = pair_dist(&P->draws[pg[j].idx], c);
                /* ties (identical positions) resolve in draw order */
                if (pair_ok(&P->draws[pg[j].idx], c, d, jt)) {
                    s_cand[nk].d = d + (float)abs(i - j) * 1e-6f;
                    s_cand[nk].i = cg[i].idx; s_cand[nk].j = pg[j].idx; nk++;
                }
            }
        }
        if (nk > 1) qsort(s_cand, (size_t)nk, sizeof(Cand), cand_cmp);
        for (i = 0; i < nk; i++) {
            if (C->draws[s_cand[i].i].match >= 0 || P->draws[s_cand[i].j].match >= 0) continue;
            pair(P, C, s_cand[i].j, s_cand[i].i);
        }
        return;
    }
    /* big groups (hundreds of tiles / particles): each draw takes the
     * nearest still free valid draw within a window around its
     * proportional position in the other list (draw order is roughly
     * kept), the same mesh found exactly when there is one */
    {
        int W = 32;
        for (i = 0; i < nc; i++) {
            const HfrDraw* c = &C->draws[cg[i].idx];
            float jt = jump_thresh(c, strict), best = 3.4e38f;
            int center = (int)((long)i * np / nc), lo = center - W, hi = center + W, bj = -1;
            if (lo < 0) lo = 0;
            if (hi > np) hi = np;
            for (j = lo; j < hi; j++) {
                const HfrDraw* p = &P->draws[pg[j].idx];
                float d;
                if (p->match >= 0) continue;
                d = pair_dist(p, c);
                if (d < best && pair_ok(p, c, d, jt)) { best = d; bj = j; }
            }
            if (bj >= 0) pair(P, C, pg[bj].idx, cg[i].idx);
        }
    }
}

/* merge-join the two streams' draws by key (loose = key2 over the still
 * unpaired draws) */
/* the stream's draws sorted by full key (once per stream: P reuses the
 * array it got as C one tick earlier) */
static int build_sorted(HfrStream* s) {
    int i;
    if (s->ndraws > s->ckr) {
        KeyRef* nk = (KeyRef*)realloc(s->kr, (size_t)s->ndraws * 2 * sizeof(KeyRef));
        if (!nk) return 0;
        s->kr = nk; s->ckr = s->ndraws * 2;
    }
    for (i = 0; i < s->ndraws; i++) { s->kr[i].key = s->draws[i].key; s->kr[i].idx = i; }
    return radix_sort(s->kr, s->ndraws);
}

static void join_groups(HfrStream* P, HfrStream* C, const KeyRef* kp, int np,
                        const KeyRef* kc, int nc, int loose) {
    int a = 0, b = 0;
    while (a < np && b < nc) {
        uint64_t k;
        int a2 = a, b2 = b;
        if (kp[a].key < kc[b].key) { a++; continue; }
        if (kp[a].key > kc[b].key) { b++; continue; }
        k = kp[a].key;
        while (a2 < np && kp[a2].key == k) a2++;
        while (b2 < nc && kc[b2].key == k) b2++;
        group_assign(P, C, kp + a, a2 - a, kc + b, b2 - b, loose);
        a = a2; b = b2;
    }
}

/* merge-join the two streams' draws by key (loose = key2 over the still
 * unpaired draws) */
static void match_pass(HfrStream* P, HfrStream* C, int loose) {
    int np = 0, nc = 0, i;
    if (!loose) {
        join_groups(P, C, P->kr, P->ndraws, C->kr, C->ndraws, 0);
        return;
    }
    if (P->ndraws > s_ckrp) {
        KeyRef* nk = (KeyRef*)realloc(s_krp, (size_t)P->ndraws * 2 * sizeof(KeyRef));
        if (!nk) return;
        s_krp = nk; s_ckrp = P->ndraws * 2;
    }
    if (C->ndraws > s_ckrc) {
        KeyRef* nk = (KeyRef*)realloc(s_krc, (size_t)C->ndraws * 2 * sizeof(KeyRef));
        if (!nk) return;
        s_krc = nk; s_ckrc = C->ndraws * 2;
    }
    for (i = 0; i < P->ndraws; i++)
        if (P->draws[i].match < 0) { s_krp[np].key = P->draws[i].key2; s_krp[np].idx = i; np++; }
    for (i = 0; i < C->ndraws; i++)
        if (C->draws[i].match < 0) { s_krc[nc].key = C->draws[i].key2; s_krc[nc].idx = i; nc++; }
    if (!radix_sort(s_krp, np) || !radix_sort(s_krc, nc)) return;
    join_groups(P, C, s_krp, np, s_krc, nc, 1);
}

/* vertices of a pair: 0 = same, 1 = blendable, -1 = jump / not blendable
 * (2D with other UVs: other glyphs / sprite frame) */
static int verts_cmp(const float* c, const float* p, int n, int is2d) {
    int i;
    if (memcmp(c, p, (size_t)n * GXV_FLOATS * sizeof(float)) == 0) return 0;
    for (i = 0; i < n; i++, c += GXV_FLOATS, p += GXV_FLOATS) {
        float d[3] = { c[0] - p[0], c[1] - p[1], c[2] - p[2] };
        if (is2d) {
            if (memcmp(c + 10, p + 10, 6 * sizeof(float)) != 0) return -1;
            if (v3len(d) > 160.0f) return -1;
        } else if (v3len(d) > 4.0f + 0.25f * v3len(c)) {
            return -1;
        }
    }
    return 1;
}

static int tc_blendable_differ(const GxTevConsts* a, const GxTevConsts* b) {
    return memcmp(a->lpos, b->lpos, sizeof a->lpos) || memcmp(a->ldir, b->ldir, sizeof a->ldir) ||
           memcmp(a->lcol, b->lcol, sizeof a->lcol) || memcmp(a->konst, b->konst, sizeof a->konst) ||
           memcmp(a->reg, b->reg, sizeof a->reg) || memcmp(a->mat, b->mat, sizeof a->mat) ||
           memcmp(a->amb, b->amb, sizeof a->amb) || memcmp(a->tg_post, b->tg_post, sizeof a->tg_post);
}

/* analyse one pair (newer draw d, older draw m): 0 = keep, 1 = jump */
static int pair_setup(HfrStream* P, HfrStream* C, HfrDraw* d, const HfrDraw* m) {
    const GxDrawConsts* cc = &C->dcs[d->dc];
    const GxDrawConsts* pc = &P->dcs[m->dc];
    d->how = H_LIN; d->vb = 0; d->tcb = 0; d->rot = -1; d->tmmask = 0;
    d->tcfg = C->tc_hash[d->tc] == P->tc_hash[m->tc];
    if (d->depth_clamp != m->depth_clamp) return 1;
    if (v3dist(d->pos, m->pos) > jump_thresh(d, 0)) return 1;
    if (d->nverts == m->nverts && d->vhash == m->vhash) {
        d->vb = 0;   /* same vertices */
    } else if (d->nverts == m->nverts) {
        int vr = verts_cmp(C->verts + d->vofs, P->verts + m->vofs, d->nverts, d->depth_clamp);
        if (vr < 0) return 1;
        d->vb = (unsigned char)vr;
    } else if (d->depth_clamp) {
        return 1;   /* 2D with another vertex count (text of another length): discrete */
    }
    if (d->tcfg) {
        /* consecutive draws mostly share their TEV blocks: compare once */
        static const GxTevConsts *la, *lb;
        static int lr;
        const GxTevConsts* ta = &C->tcs[d->tc];
        const GxTevConsts* tb = &P->tcs[m->tc];
        if (ta != la || tb != lb) { la = ta; lb = tb; lr = tc_blendable_differ(ta, tb); }
        d->tcb = (unsigned char)lr;
    }
    if (memcmp(cc, pc, sizeof *cc) == 0) {
        d->how = H_SAME;
        return 0;
    }
    {
        int t;
        d->tmmask = 0;
        for (t = 0; t < 8; t++)
            if (memcmp(pc->texmtx[t], cc->texmtx[t], sizeof pc->texmtx[t])) d->tmmask |= (unsigned char)(1 << t);
    }
    if (!d->depth_clamp) {
        HfrRot r;
        int k = rot_analyze(pc->mv, cc->mv, &r.mv);
        if (k < 0) return 1;
        if (k > 0) {
            r.nrm_slerp = rot_analyze(pc->nrm, cc->nrm, &r.nrm) > 0;
            if (C->nrots >= C->crots) {
                int ncap = C->crots ? C->crots * 2 : 256;
                HfrRot* nr = (HfrRot*)realloc(C->rots, (size_t)ncap * sizeof(HfrRot));
                if (!nr) return 0;
                C->rots = nr; C->crots = ncap;
            }
            C->rots[C->nrots] = r;
            d->rot = C->nrots++;
            d->how = H_SLERP;
        }
    }
    return 0;
}

/* camera views: did a camera seen in both ticks jump? */
static int s_view_moved, s_view_any, s_view_rot_ok;
static int view_cut(void) {
    int c, cut = 0, prev = s_vcur ^ 1;
    s_view_moved = 0;
    s_view_any = 0;
    for (c = 0; c < HFR_NCAM; c++) {
        const float* a = s_view[prev][c];
        const float* b = s_view[s_vcur][c];
        float ea[3], eb[3], qa[4], qb[4], sa[3], sb[3], d, ca[16], cb[16];
        int i, jump = 0;
        if (!s_view_ok[prev][c] || !s_view_ok[s_vcur][c]) continue;
        /* GC Mtx (row-major 3x4 [R|t]): eye = -R^T t */
        for (i = 0; i < 3; i++) {
            ea[i] = -(a[0 * 4 + i] * a[3] + a[1 * 4 + i] * a[7] + a[2 * 4 + i] * a[11]);
            eb[i] = -(b[0 * 4 + i] * b[3] + b[1 * 4 + i] * b[7] + b[2 * 4 + i] * b[11]);
        }
        memset(ca, 0, sizeof ca); memset(cb, 0, sizeof cb);
        for (i = 0; i < 3; i++) {   /* column i of R = (R[0][i], R[1][i], R[2][i]) */
            ca[i * 4 + 0] = a[0 * 4 + i]; ca[i * 4 + 1] = a[1 * 4 + i]; ca[i * 4 + 2] = a[2 * 4 + i];
            cb[i * 4 + 0] = b[0 * 4 + i]; cb[i * 4 + 1] = b[1 * 4 + i]; cb[i * 4 + 2] = b[2 * 4 + i];
        }
        d = v3dist(ea, eb);
        s_view_any = 1;
        s_view_rot_ok = decompose(ca, qa, sa) && decompose(cb, qb, sb);
        if (s_view_rot_ok && quat_angle(qa, qb) > 0.75f)
            jump = 1;   /* > 43 deg in one tick */
        if (d > 3.0f * s_view_speed[c] + 3.0f)
            jump = 1;   /* moved much more than its speed */
        /* after a camera reset (soft cut): a tighter test - a reset camera
         * that keeps moving smoothly (baseball's intro pan resets every
         * tick) is not a cut, a small reposition is */
        if (d > 1.5f * s_view_speed[c] + 0.5f ||
            (s_view_rot_ok && quat_angle(qa, qb) > 0.17f))
            s_view_moved = 1;
        s_view_speed[c] = jump ? 0.0f : d;
        cut |= jump;
    }
    return cut;
}

static double s_match_ms;
static double s_ph[4]; static int s_phn;
static void tick_end_inner(void);
void hfr_tick_end(void) {
    uint64_t t0;
    if (!s_on) return;
    t0 = perf_now();
    tick_end_inner();
    s_match_ms += perf_ms(t0, perf_now());
}
static void tick_end_inner(void) {
    HfrStream* s = &s_st[s_cur];
    HfrStream* p = &s_st[s_cur ^ 1];
    int i, prev_ok, cut_hook, cut_view;
    cut_view = view_cut();
    cut_hook = s_cut_req || (s_cut_soft && (s_view_moved || !s_view_any));
    s_cut_req = 0;
    s_cut_soft = 0;
    s_vcur ^= 1;
    memset(s_view_ok[s_vcur], 0, sizeof s_view_ok[s_vcur]);
    if (!s_recording) { s->complete = 0; return; }   /* no kickoff this tick */
    if (s->bad) flush_deferred();
    s_recording = 0;
    s->open = 0;
    s->complete = !s->bad;
    if (!s->complete) return;
    s_final_pending = s_defer && s_fwd < s->nops;
    for (i = 0; i < s->ndraws; i++) s->draws[i].match = -1;
    for (i = 0; i < p->ndraws; i++) p->draws[i].match = -1;
    prev_ok = p->complete && p->tick + 1 == s->tick;
    { uint64_t tq = perf_now(); if (!build_sorted(s)) prev_ok = 0; s_ph[0] += perf_ms(tq, perf_now()); }
    /* the previous stream can only be drawn if no texture it uses was
     * re-uploaded since (EFB-copy targets excepted: they lag either way) */
    if (prev_ok && s_mod.n) {
        int k;
        for (k = 0; k < s_mod.cap; k++)
            if (s_mod.k[k] && s_mod.v[k] && pset_get(&p->texs, s_mod.k[k])) { p->hazard = 1; break; }
        if (p->hazard) s_stat_hazard++;
    }
    for (i = 0; i < s->ndraws; i++) if (!s->draws[i].depth_clamp) s->n3d++;
    if (prev_ok && !cut_hook && !cut_view) {
        uint64_t tq = perf_now();
        match_pass(p, s, 0);
        s_ph[1] += perf_ms(tq, perf_now()); tq = perf_now();
        match_pass(p, s, 1);
        s_ph[2] += perf_ms(tq, perf_now()); tq = perf_now();
        for (i = 0; i < s->ndraws; i++) {
            HfrDraw* d = &s->draws[i];
            if (d->match < 0) continue;
            if (!d->depth_clamp) s->matched++;
            if (pair_setup(p, s, d, &p->draws[d->match])) {
                p->draws[d->match].match = -1;
                d->match = -1;
                if (!d->depth_clamp) s->snapped++;
            } else {
                if (d->how == H_SLERP) s_stat_slerp++;
                if (d->vb) s_stat_vb++;
            }
        }
        s_ph[3] += perf_ms(tq, perf_now());
        if (++s_phn >= 300 && getenv("SMB2_HFR_PROFILE")) {
            fprintf(stderr, "[hfrprof] sort %.3f pass0 %.3f pass1 %.3f setup %.3f ms/tick\n",
                    s_ph[0] / s_phn, s_ph[1] / s_phn, s_ph[2] / s_phn, s_ph[3] / s_phn);
            memset(s_ph, 0, sizeof s_ph); s_phn = 0;
        }
        /* a camera cut the hooks missed: most of the 3D frame jumped */
        if (s->matched >= 8 && s->snapped * 2 > s->matched) { s->cut = 1; s_stat_cut_heur++; }
    } else if (prev_ok) {
        s->cut = 1;
        if (cut_hook) s_stat_cut_hook++; else s_stat_cut_view++;
    }
    if (s->cut) {
        for (i = 0; i < s->ndraws; i++) s->draws[i].match = -1;
        for (i = 0; i < p->ndraws; i++) p->draws[i].match = -1;
    }
    {   /* SMB2_HFR_DEBUG=<tick>: list both streams' draws and pairs */
        static long dbg = -2;
        extern long g_dbg_frame_counter;
        if (dbg == -2) { const char* e = getenv("SMB2_HFR_DEBUG"); dbg = e ? atol(e) : -1; }
        if (dbg >= 0 && g_dbg_frame_counter + 1 == dbg) {
            int t;
            for (t = 0; t < 2; t++) {
                HfrStream* x = t ? s : p;
                for (i = 0; i < x->ndraws; i++) {
                    HfrDraw* d = &x->draws[i];
                    fprintf(stderr, "[hfrdbg] %c %4d key %016llx tag %08x n %3d %s pos %8.3f %8.3f %8.3f v0 %7.3f %7.3f %7.3f "
                            "m %d match %4d how %d vb %d\n", t ? 'C' : 'P', i, (unsigned long long)d->key, d->tag,
                            d->nverts, d->depth_clamp ? "2D" : "3D", d->pos[0], d->pos[1], d->pos[2],
                            d->v0[0], d->v0[1], d->v0[2], d->model, d->match, d->how, d->vb);
                    if (getenv("SMB2_HFR_DEBUG_FULL") && d->nverts <= 4 && !d->depth_clamp) {
                        const float* m = x->dcs[d->dc].mv;
                        const float* v = x->verts + d->vofs;
                        int q;
                        fprintf(stderr, "[hfrdbgm] %c %d mv", t ? 'C' : 'P', i);
                        for (q = 0; q < 16; q++) fprintf(stderr, " %.5f", m[q]);
                        for (q = 0; q < d->nverts; q++)
                            fprintf(stderr, " | %.5f %.5f %.5f", v[q * GXV_FLOATS], v[q * GXV_FLOATS + 1], v[q * GXV_FLOATS + 2]);
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    s_stat_ticks++;
    s_stat_draws += (unsigned long)s->n3d;
    s_stat_matched += (unsigned long)s->matched;
    s_stat_snapped += (unsigned long)s->snapped;
    s_stat_cuts += (unsigned long)s->cut;
}

int hfr_tick_valid(void) {
    return s_on && !s_recording && s_st[s_cur].complete;
}

/* ---- replay ---- */
static float* s_scratch; static size_t s_cscratch;
static GxTevConsts s_tcb;          /* blended TEV constants */
static unsigned s_tcb_serial = 0xF0000000u;
static int s_tcb_p = -1, s_tcb_c = -1, s_tcb_base = -1;

static double s_replay_ms; static unsigned long s_replays;

/* draw ops [from, to) of B (the newest stream C or the previous P) */
static void replay(HfrStream* B, int from, int to, float alpha, int interp, int base_is_p) {
    HfrStream* C = &s_st[s_cur];
    HfrStream* P = &s_st[s_cur ^ 1];
    GxDrawConsts dc;
    int i;
    s_tcb_p = s_tcb_c = s_tcb_base = -1;
    for (i = from; i < to; i++) {
        const HfrOp* o = &B->ops[i];
        switch (o->op) {
        case OP_VIEWPORT: s_real->set_viewport(o->a[0], o->a[1], o->a[2], o->a[3]); break;
        case OP_SCISSOR:  s_real->set_scissor(o->a[0], o->a[1], o->a[2], o->a[3]); break;
        case OP_BLEND:    s_real->set_blend(o->a[0], o->a[1], o->a[2], o->a[3]); break;
        case OP_DEPTH:    s_real->set_depth(o->a[0], o->a[1], o->a[2]); break;
        case OP_CULL:     s_real->set_cull(o->a[0]); break;
        case OP_CLEAR:    s_real->clear(NULL, (uint32_t)o->a[0], o->f[0], o->a[1]); break;
        case OP_CLEAR_RECT:
            s_real->clear(o->a, (uint32_t)(uintptr_t)o->p, o->f[0], (int)o->f[1]); break;
        case OP_BIND:     s_real->bind_tex(o->a[0], o->p); break;
        case OP_TEXWRAP:  s_real->tex_wrap(o->p, o->a[0], o->a[1]); break;
        case OP_TEXLOD:
            s_real->tex_lod(o->p, o->a[0], o->a[1], o->a[2], o->f[0], o->f[1], o->f[2], o->a[3]);
            break;
        case OP_COPYTEX:
            /* re-run the EFB copy from the interpolated offscreen pass (no
             * readback: GC memory keeps the tick's copy); a pass that reads
             * the copy's previous contents keeps the tick's result */
            if (o->f[0] == 0.0f && s_real->copy_tex) {
                void* t = o->p;
                if (s_real->copy_tex(&t, o->a[0], o->a[1], o->a[2], o->a[3], o->a[4], o->a[5],
                                     o->a[6] | GXCOPY_NO_READBACK))
                    s_stat_copies++;
            } else {
                s_stat_copies_fb++;
            }
            break;
        case OP_DRAW: {
            const HfrDraw* d = &B->draws[o->a[0]];
            GxDrawCall c;
            memset(&c, 0, sizeof c);
            c.prim = d->prim;
            c.nverts = d->nverts;
            c.verts = B->verts + d->vofs;
            c.rects = d->rofs >= 0 ? B->rects + d->rofs : NULL;
            c.dc = &B->dcs[d->dc];
            c.tc = &B->tcs[d->tc];
            c.tc_serial = d->tc_serial;
            c.depth_clamp = d->depth_clamp;
            c.no_cull = d->no_cull;
            if (interp && d->match >= 0) {
                const HfrDraw* pd = base_is_p ? d : &P->draws[d->match];
                const HfrDraw* cd = base_is_p ? &C->draws[d->match] : d;
                const GxDrawConsts* pc = &P->dcs[pd->dc];
                const GxDrawConsts* cc = &C->dcs[cd->dc];
                if (cd->how != H_SAME) {
                    int t;
                    dc = *c.dc;
                    if (pc->proj[11] == cc->proj[11] && pc->proj[15] == cc->proj[15])
                        lerpn(dc.proj, pc->proj, cc->proj, alpha, 16);
                    if (cd->how == H_SLERP && cd->rot >= 0) {
                        const HfrRot* r = &C->rots[cd->rot];
                        rot_interp(dc.mv, pc->mv, cc->mv, alpha, &r->mv);
                        if (r->nrm_slerp)
                            rot_interp(dc.nrm, pc->nrm, cc->nrm, alpha, &r->nrm);
                        else
                            lerpn(dc.nrm, pc->nrm, cc->nrm, alpha, 16);
                    } else {
                        lerpn(dc.mv, pc->mv, cc->mv, alpha, 16);
                        lerpn(dc.nrm, pc->nrm, cc->nrm, alpha, 16);
                    }
                    for (t = 0; cd->tmmask >> t; t++)
                        if ((cd->tmmask >> t) & 1)
                            texmtx_interp(dc.texmtx[t], pc->texmtx[t], cc->texmtx[t], alpha);
                    lerpn(dc.tev_c0, pc->tev_c0, cc->tev_c0, alpha, 4);
                    lerpn(dc.tev_c1, pc->tev_c1, cc->tev_c1, alpha, 4);
                    if (pc->fog[0] == cc->fog[0]) lerpn(&dc.fog[1], &pc->fog[1], &cc->fog[1], alpha, 2);
                    lerpn(dc.fog_color, pc->fog_color, cc->fog_color, alpha, 4);
                    c.dc = &dc;
                }
                if (cd->tcb) {
                    int ptc = pd->tc, ctc = cd->tc, btc = d->tc;
                    if (ptc != s_tcb_p || ctc != s_tcb_c || btc != s_tcb_base) {
                        const GxTevConsts* a = &P->tcs[ptc];
                        const GxTevConsts* b = &C->tcs[ctc];
                        int t;
                        s_tcb = B->tcs[btc];
                        lerpn(&s_tcb.lpos[0][0], &a->lpos[0][0], &b->lpos[0][0], alpha, 32);
                        lerpn(&s_tcb.ldir[0][0], &a->ldir[0][0], &b->ldir[0][0], alpha, 32);
                        lerpn(&s_tcb.lcol[0][0], &a->lcol[0][0], &b->lcol[0][0], alpha, 32);
                        lerpn(&s_tcb.konst[0][0], &a->konst[0][0], &b->konst[0][0], alpha, 32);
                        lerpn(&s_tcb.reg[0][0], &a->reg[0][0], &b->reg[0][0], alpha, 16);
                        lerpn(s_tcb.mat, a->mat, b->mat, alpha, 4);
                        lerpn(s_tcb.amb, a->amb, b->amb, alpha, 4);
                        for (t = 0; t < 8; t++)
                            if (memcmp(a->tg_post[t], b->tg_post[t], sizeof a->tg_post[t]))
                                texmtx_interp(s_tcb.tg_post[t], a->tg_post[t], b->tg_post[t], alpha);
                        s_tcb_p = ptc; s_tcb_c = ctc; s_tcb_base = btc;
                        if (++s_tcb_serial < 0xF0000000u) s_tcb_serial = 0xF0000000u;
                    }
                    c.tc = &s_tcb;
                    c.tc_serial = s_tcb_serial;
                }
                if (cd->vb && pd->nverts == cd->nverts) {
                    size_t nv = (size_t)d->nverts * GXV_FLOATS;
                    const float* cv = C->verts + cd->vofs;
                    const float* pv = P->verts + pd->vofs;
                    if (nv > s_cscratch) {
                        float* ns = (float*)realloc(s_scratch, nv * 2 * sizeof(float));
                        if (ns) { s_scratch = ns; s_cscratch = nv * 2; }
                    }
                    if (nv <= s_cscratch) {
                        float* ov = s_scratch;
                        int v;
                        memcpy(ov, c.verts, nv * sizeof(float));
                        c.verts = ov;
                        /* position, normal, colour; UVs as drawn */
                        for (v = 0; v < d->nverts; v++, ov += GXV_FLOATS, cv += GXV_FLOATS, pv += GXV_FLOATS)
                            lerpn(ov, pv, cv, alpha, 10);
                    }
                }
            }
            s_real->draw(&c);
            break;
        }
        }
    }
}

void hfr_render(float alpha) {
    HfrStream* C = &s_st[s_cur];
    HfrStream* P = &s_st[s_cur ^ 1];
    int interp, base_is_p;
    uint64_t t0 = perf_now();
    if (!hfr_tick_valid()) return;
    {
        int prev_ok = P->complete && P->tick + 1 == C->tick && !P->hazard;
        base_is_p = prev_ok && alpha < 0.5f;
        interp = alpha < 1.0f && prev_ok && !C->cut;
        if (alpha < 1.0f && getenv("SMB2_HFR_DEBUG2"))
            fprintf(stderr, "[hfrdbg2] tick %lu a %.2f P complete %d tick %lu hazard %d C cut %d nd %d/%d\n",
                    C->tick, alpha, P->complete, P->tick, P->hazard, C->cut, P->ndraws, C->ndraws);
    }
    replay(base_is_p ? P : C, 0, (base_is_p ? P : C)->nops, alpha, interp, base_is_p);
    if (alpha >= 1.0f) s_final_pending = 0;
    s_replay_ms += perf_ms(t0, perf_now());
    s_replays++;
}

int hfr_final_pending(void) { return s_on && s_final_pending && hfr_tick_valid(); }

/* deferred recording: draw what is recorded but not yet sent */
static void flush_deferred(void) {
    HfrStream* s = &s_st[s_cur];
    if (!s_recording || s_fwd >= s->nops) return;
    replay(s, s_fwd, s->nops, 1.0f, 0, 0);
    s_fwd = s->nops;
    s_stat_flushes++;
}
/* after recording an op: 1 = it stays deferred (not sent now) */
static int keep(void) {
    if (!s_defer) return 0;
    if (s_st[s_cur].bad) {   /* out of memory: send everything from now on */
        flush_deferred();
        s_defer = 0;
        return 0;
    }
    return 1;
}

void hfr_note_sync_copy(void* tex, int x, int y, int w, int h, int dw, int dh, int cf, int gpu_ok) {
    (void)x; (void)y; (void)w; (void)h; (void)dw; (void)dh; (void)cf; (void)gpu_ok;
    if (!s_on || !tex) return;
    /* refreshed by an EFB copy, not a texture upload: no hazard for the
     * previous stream (the copy lags by < 1 tick either way) */
    pset_del(&s_mod, tex);
    if (s_recording) s_stat_copies_fb++;
}

/* ---- game-side hooks (decomp include/pc_hfr.h) ---- */
static void tag_update(void) {
    int n = s_tag_depth < 16 ? s_tag_depth : 16;
    uint64_t h = fnv(1469598103934665603ULL, s_tag_stack, (size_t)n * sizeof(unsigned));
    s_tag = n ? (unsigned)(h ^ (h >> 32)) : 0;
}
void pc_hfr_tag_push(unsigned kind, unsigned id) {
    if (!s_recording) return;
    gx_hfr_flush();   /* the pending batch belongs to the outer scope */
    if (s_tag_depth < 16) s_tag_stack[s_tag_depth] = kind * 0x9E3779B1u ^ id;
    s_tag_depth++;
    tag_update();
}
void pc_hfr_tag_pop(void) {
    if (!s_recording || s_tag_depth <= 0) return;
    gx_hfr_flush();
    s_tag_depth--;
    tag_update();
}
/* ordering-table nodes (depth-sorted draws run later, from
 * ord_tbl_draw_nodes): a node keeps the tag it was queued under.  The node
 * allocator restarts every frame; the map is cleared at each tick begin. */
#define NODE_MAP 8192
static void* s_node_k[NODE_MAP];
static unsigned s_node_v[NODE_MAP];
static int s_node_n;
static void node_map_clear(void) {
    if (s_node_n) { memset(s_node_k, 0, sizeof s_node_k); s_node_n = 0; }
}
void pc_hfr_node_alloc(void* node) {
    unsigned h;
    if (!s_recording || !s_tag || !node || s_node_n >= NODE_MAP / 2) return;
    h = pset_hash(node, NODE_MAP);
    while (s_node_k[h] && s_node_k[h] != node) h = (h + 1) & (NODE_MAP - 1);
    if (!s_node_k[h]) s_node_n++;
    s_node_k[h] = node;
    s_node_v[h] = s_tag;
}
static unsigned node_tag(void* node) {
    unsigned h = pset_hash(node, NODE_MAP);
    while (s_node_k[h]) {
        if (s_node_k[h] == node) return s_node_v[h];
        h = (h + 1) & (NODE_MAP - 1);
    }
    return 0;
}
void pc_hfr_node_begin(void* node, void* func) {
    unsigned t;
    if (!s_recording) return;
    t = s_node_n ? node_tag(node) : 0;
    pc_hfr_tag_push(7, t ? t : (unsigned)((uintptr_t)func * 2654435761u));
}
void pc_hfr_node_end(void) { pc_hfr_tag_pop(); }

/* reason 1 (camera_clear) is soft: some modes reset a camera every tick
 * while it stays put (baseball); it only cuts when a camera view changed */
void pc_hfr_cut(int reason) {
    if (!s_on) return;
    if (reason == 1) s_cut_soft = 1;
    else s_cut_req = 1;
}
void pc_hfr_view(int camera, const float (*m)[4]) {
    if (!s_on || camera < 0 || camera >= HFR_NCAM || s_view_ok[s_vcur][camera]) return;
    memcpy(s_view[s_vcur][camera], m, 12 * sizeof(float));
    s_view_ok[s_vcur][camera] = 1;
}

/* ---- statistics: present intervals ----
 * Every 5 s: presents per second, interval mean / sd / percentiles, presents
 * later than 1.5x the target interval, replay cost, pairing statistics.
 * Percentiles come from a 0.05 ms histogram (0-100 ms). */
int g_hfr_kcap = 63;
/* GPU time per frame (backend timestamps, D3D12): running estimate for the
 * pacer, polled once per tick; 0 = unknown (GL: GL_TIME_ELAPSED would
 * include the CPU's submission gaps, so the GL pacer relies on present
 * blocking and the cap) */
static double s_gpu_est, s_gpu_win; static long s_gpu_win_n;
double hfr_gpu_frame_est_ms(void) {
    double m;
    if (!s_real || !s_real->gpu_frame_ms) return 0.0;
    m = s_real->gpu_frame_ms();
    if (m > 0.0) {
        s_gpu_est = s_gpu_est > 0.0 ? s_gpu_est * 0.8 + m * 0.2 : m;
        s_gpu_win += m; s_gpu_win_n++;
    }
    return s_gpu_est;
}
#define HIST_N 2000
static unsigned s_hist[HIST_N + 1];
static double hist_pct(unsigned long n, double q) {
    unsigned long want = (unsigned long)(q * (double)n), acc = 0;
    int i;
    for (i = 0; i <= HIST_N; i++) {
        acc += s_hist[i];
        if (acc > want) return i * 0.05;
    }
    return HIST_N * 0.05;
}
void hfr_note_present(void) {
    static uint64_t last, win;
    static double sum, sumsq, mx;
    static unsigned long n, late;
    uint64_t now;
    double dt, target;
    int b;
    if (!s_on) { last = 0; return; }
    now = perf_now();
    if (!last) { last = win = now; memset(s_hist, 0, sizeof s_hist); return; }
    dt = perf_ms(last, now);
    last = now;
    n++;
    sum += dt; sumsq += dt * dt;
    if (dt > mx) mx = dt;
    b = (int)(dt / 0.05);
    s_hist[b < 0 ? 0 : b > HIST_N ? HIST_N : b]++;
    target = g_hfr_cur_fps > 0 ? 1000.0 / g_hfr_cur_fps : 1000.0 / 60.0;
    if (dt > target * 1.5) late++;
    if (perf_ms(win, now) >= 5000.0) {
        double w = perf_ms(win, now), mean = sum / n;
        double sd = sqrt(sumsq / n - mean * mean > 0 ? sumsq / n - mean * mean : 0);
        double gms = s_gpu_win_n ? s_gpu_win / s_gpu_win_n : 0.0;
        s_gpu_win = 0; s_gpu_win_n = 0;
#ifdef SMB2_HAVE_D3D12
        if (getenv("SMB2_HFR_PROFILE")) {
            extern long g_dx_midsync;
            fprintf(stderr, "[hfrprof] d3d12 mid-frame drains %ld in 5 s\n", g_dx_midsync);
            g_dx_midsync = 0;
        }
#endif
        printf("[hfr] %.1f fps | gpu %.2f ms/frame | interval mean %.2f sd %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f ms, >1.5x target %lu | "
               "replay %.3f ms x %.2f/tick (cap %d) match %.2f ms first %.2f ms | 3D draws/tick %.0f matched %.1f%% snapped %.1f%% "
               "slerp %lu vblend %lu | copies %lu kept %lu flushes %lu | hazard %lu degraded %lu | cuts %lu (hook %lu view %lu heur %lu)\n",
               n * 1000.0 / w, gms, mean, sd, hist_pct(n, 0.50), hist_pct(n, 0.95), hist_pct(n, 0.99), mx, late,
               s_replays ? s_replay_ms / s_replays : 0.0,
               s_stat_ticks ? (double)s_replays / s_stat_ticks : 0.0, g_hfr_kcap,
               s_stat_ticks ? s_match_ms / s_stat_ticks : 0.0,
               g_hfr_first_n ? g_hfr_first_ms_acc / g_hfr_first_n : 16.67,
               s_stat_ticks ? (double)s_stat_draws / s_stat_ticks : 0.0,
               s_stat_draws ? 100.0 * s_stat_matched / s_stat_draws : 0.0,
               s_stat_matched ? 100.0 * s_stat_snapped / s_stat_matched : 0.0,
               s_stat_slerp, s_stat_vb, s_stat_copies, s_stat_copies_fb, s_stat_flushes, s_stat_hazard, s_stat_degraded,
               s_stat_cuts, s_stat_cut_hook, s_stat_cut_view, s_stat_cut_heur);
        fflush(stdout);
        n = late = 0; sum = sumsq = mx = 0;
        memset(s_hist, 0, sizeof s_hist);
        s_replay_ms = 0; s_replays = 0; s_match_ms = 0;
        g_hfr_first_ms_acc = 0; g_hfr_first_n = 0;
        s_stat_ticks = s_stat_draws = s_stat_matched = s_stat_snapped = s_stat_cuts = 0;
        s_stat_slerp = s_stat_vb = s_stat_copies = s_stat_copies_fb = s_stat_hazard = s_stat_flushes = 0;
        s_stat_degraded = 0;
        s_stat_cut_hook = s_stat_cut_view = s_stat_cut_heur = 0;
        win = now;
    }
}