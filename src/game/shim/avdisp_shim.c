/* avdisp GMA model renderer, ported from the SMB2 asm
 * (decomp/build/GM2E8P/asm/auto_03_80053ABC_text.s):
 *
 *   g_init_gma_model_materials  0x8006A9C8  (strong def; overrides the weak
 *                                            logging fallback in endian_gma.c)
 *   g_init_gma_texture          0x80065E90
 *   g_avdisp_draw_model_now1    0x80069AE8  (opaque + translucent shapes)
 *   g_avdisp_draw_model_now2    0x80069C00  (opaque shapes only)
 *   g_avdisp_draw_model_now3    0x80069D14  (translucent shapes only)
 *   avdisp_draw_static_shape_now 0x8006A52C (inlined here as draw_shape)
 *   g_avdisp_set_ambient        0x80065C60 area
 *   avdisp_set_post_mult_color / avdisp_set_post_add_color
 *   load_gx_pos_nrm_mtx         0x80066900  (XF pos+nrm matrix upload)
 *
 * SMB1's matched C (ref/smb-decomp/src/avdisp.c) is the reference for
 * everything the two games share; SMB2 differences come from the asm and
 * are noted inline.
 *
 * GC draws shapes by calling pre-built big-endian GX display lists.  There
 * is no FIFO on PC, so parse_disp_list() interprets the stream and replays
 * it through the immediate-mode GXBegin/GXPosition3f32/... API.  Display
 * lists are not byte-swapped at load (endian_gma.c), so all multi-byte
 * reads here are explicit big-endian decodes.
 *
 * Materials are GX lighting channels + TEV stages (port of SMB1
 * build_tev_material), drawn by the backend's GXFX emulation.  The GC
 * display-list record/replay hint bits (model->flags 0x20/0x40) are
 * ignored: strips are re-emitted on every draw.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "shim.h"
#include "avdisp.h"   /* struct GMAModel/GMAShape/GMAExtraDispLists, GXTexObj */
#include "bitmap.h"   /* struct TPL, struct TPLTextureHeader */
#include "gx.h"       /* GXBegin + immediate-mode vertex sinks (pc_gx_*) */

/* ---- pc_port render API (render.h; not includable next to decomp hdrs) */
extern void GXInitTexObj(GXTexObj *obj, void *image, u16 width, u16 height,
                         int format, int wrapS, int wrapT, u8 mipmap);
extern void GXLoadTexObj_cached(GXTexObj *obj, int map);
extern void GXSetCullMode_cached(int mode);
extern void GXLoadPosMtxImm(Mtx mtx, int id);
extern void GXSetCurrentMtx(int id);
extern void GXLoadNrmMtxImm(Mtx mtx, int id);

/* decomp heap API (heap.c) */
extern int currentHeap;
extern void *alloc_from_heap_or_panic(int heap, u32 size, char *file, int line);

void OSReport(const char *fmt, ...);

/* GMAModel flags (ref smb-decomp gma.h) */
#define GCMF_16BIT     0x01
#define GCMF_STITCHING 0x04
#define GCMF_SKIN      0x08
#define GCMF_EFFECTIVE 0x10

/* GMAShape flags */
#define SHAPE_FLAG_DOUBLE_SIDED    (1 << 1)
#define SHAPE_FLAG_CUSTOM_BLEND_SRC (1 << 5)
#define SHAPE_FLAG_CUSTOM_BLEND_DST (1 << 6)
#define SHAPE_FLAG_SIMPLE_MATERIAL (1 << 7)   /* no tev layers */
#define SHAPE_FLAG_VERT_COLORS     (1 << 8)   /* vtxAttrs has GX_VA_CLR0 */

/* GC GXCullMode values (== pc_port render.h values) */
#define CULL_NONE  0
#define CULL_FRONT 1
#define CULL_BACK  2

/* The 0x20-byte GMA TEV layer (ref smb-decomp gma.h GMATevLayer; the SMB2
 * decomp has no struct for it).  Offsets from the asm: texObj at +0x08
 * (g_init_gma_model_materials), image data at +0x14 (g_init_gma_texture). */
struct GMATevLayer
{
    /*0x00*/ u32 flags;
    /*0x04*/ u16 texIndex;
    /*0x06*/ s8 lodBias;
    /*0x07*/ u8 maxAniso;
    /*0x08*/ GXTexObj * DPTR texObj;  /* 4-byte slot on x64 */
    /*0x0C*/ u8 fillerC[0x10 - 0x0C];
    /*0x10*/ u8 blendMode[4];      /* big-endian u32, not swapped on load:
                                    * bits 0-3 (byte 0x13 low nibble) TEV
                                    * color mode, bits 4-7 alpha mode */
    /*0x14*/ void * DPTR imageData;      /* tpl->fileData + imageOffset */
    /*0x18*/ u8 filler18[0x20 - 0x18];
};  /* size = 0x20 */

/* GMATevLayer flags used by g_init_gma_texture (ref smb-decomp gma.h) */
#define TEV_LAYER_WRAP_S_SHIFT 2      /* (flags >> 2) & 3: 1 REPEAT 2 MIRROR */
#define TEV_LAYER_WRAP_T_SHIFT 4
#define TEV_LAYER_FLAG_EDGE_LOD  (1 << 6)
#define TEV_LAYER_MAX_LOD_SHIFT  7    /* (flags >> 7) & 0xF, 0xF = auto */
#define TEV_LAYER_FLAG_MAGFILT_NEAR (1 << 11)

/* ---- draw state ------------------------------------------------------ */

static float s_ambient[3] = { 1.0f, 1.0f, 1.0f };       /* lbl_80190070[0..2] */
static float s_postMult[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float s_postAdd[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
/* lbl_80190070+0xC: material alpha (fn_80069860), lbl_80199354: light mask
 * (fn_80066BE0), lbl_80199344: COLOR0 attenuation fn (GX_AF_SPOT) */
static float s_materialAlpha = 1.0f;
static u32 s_lightMask = 1;
static int s_lightAttnFn = 1;
/* set while the current material does lighting + post colors in TEV; the
 * vertex color emitters then pass colors through unmodified */
static int s_post_in_tev;
/* lbl_8019008C / lbl_80199348..50: infinite (specular) light dir + color.
 * Stored only: the default GMA path has no specular layer that uses them. */
static float s_infLightDir[3] = { 0.0f, 0.0f, -1.0f };
static float s_infLightColor[3] = { 1.0f, 1.0f, 1.0f };

/* asm 0x80066BE0: swap the light mask used by lit GMA materials */
void fn_80066BE0(u32 lightMask)
{
    s_lightMask = lightMask;
}

/* asm 0x80066BF0: infinite light direction (normalized copy) */
void fn_80066BF0(float *dir)
{
    float l = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);

    if (l > 1e-6f) {
        s_infLightDir[0] = dir[0] / l;
        s_infLightDir[1] = dir[1] / l;
        s_infLightDir[2] = dir[2] / l;
    }
}

/* asm 0x80066C30: infinite light color */
void fn_80066C30(float r, float g, float b)
{
    s_infLightColor[0] = r;
    s_infLightColor[1] = g;
    s_infLightColor[2] = b;
}

void g_avdisp_set_ambient(float r, float g, float b)
{
    s_ambient[0] = r;
    s_ambient[1] = g;
    s_ambient[2] = b;
}

void avdisp_set_post_mult_color(float r, float g, float b, float a)
{
    s_postMult[0] = r;
    s_postMult[1] = g;
    s_postMult[2] = b;
    s_postMult[3] = a;
}

void avdisp_set_post_add_color(float r, float g, float b, float a)
{
    s_postAdd[0] = r;
    s_postAdd[1] = g;
    s_postAdd[2] = b;
    s_postAdd[3] = a;
}

/* ---- bone matrix pointer table (asm data_buffer @ sda21 0x8B40,
 *      matrix pool lbl_80199384, heap flag lbl_8019937C) ----------------
 *
 * The GC game keeps a pool of mtxCount Mtx (0x30 bytes each) plus a table
 * of Mtx pointers ("data_buffer") that the CPU skinning indexes with the
 * per-bone u16 from the skin stream.  pointer_range_advance (0x8006982C)
 * points the table at a caller-provided contiguous Mtx array (ape.c passes
 * its posed bone matrices), draw_poly (0x8006977C) at the model's own
 * stitching matrices.  On PC both the pool and the table are static, and
 * unset slots fall back to identity instead of GC's uninitialized memory. */

#define AVDISP_MTX_SLOTS 256            /* model->mtxCount is a u8 */

static float s_mtx_pool[AVDISP_MTX_SLOTS][12];      /* lbl_80199384 */
static float *s_mtx_slot[AVDISP_MTX_SLOTS];         /* data_buffer */
static const float s_identity_mtx[12] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
};

static void *s_custom_material_cb_peek(void);
static unsigned s_avdisp_flag_peek(void);
static void mtx_slot_reset(void)        /* pool entries -> identity */
{
    u32 i, j;

    for (i = 0; i < AVDISP_MTX_SLOTS; i++) {
        for (j = 0; j < 12; j++)
            s_mtx_pool[i][j] = s_identity_mtx[j];
        s_mtx_slot[i] = s_mtx_pool[i];
    }
}

/* asm 0x8006982C: data_buffer[i] = (u8 *)mtxs + i*0x30 for the model's
 * mtxCount bones; mtxs is one contiguous Mtx array (ape.c: Mtx mtxs[50]) */
void pointer_range_advance(void *model_, void *mtxs)
{
    struct GMAModel *model = model_;
    u32 i, n = model->mtxCount;

    for (i = 0; i < n && i < AVDISP_MTX_SLOTS; i++)
        s_mtx_slot[i] = (float *)((u8 *)mtxs + i * 0x30);
}

/* asm 0x8006977C: modelMtxs = model + (tevLayerCount << 5) + 0x40 (the
 * stitching matrix block after the tev layers).  mtxs == NULL: point the
 * slot table at the model's own matrices.  mtxs != NULL: it is an array
 * of Mtx POINTERS; copy the model's matrices out into them (the slot
 * table is untouched - that is what the asm does too). */
void draw_poly(void *model_, void *mtxs_)
{
    struct GMAModel *model = model_;
    u8 *modelMtxs = (u8 *)model + ((u32)model->tevLayerCount << 5) + 0x40;
    u32 i, j, n = model->mtxCount;

    if (mtxs_ == 0) {
        for (i = 0; i < n && i < AVDISP_MTX_SLOTS; i++)
            s_mtx_slot[i] = (float *)(modelMtxs + i * 0x30);
    } else {
        float **out = (float **)mtxs_;

        for (i = 0; i < n; i++) {
            const float *src = (const float *)(modelMtxs + i * 0x30);

            for (j = 0; j < 12; j++)
                out[i][j] = src[j];
        }
    }
}

/* asm 0x80069578: allocate the matrix pool + pointer table (count Mtx,
 * from the arena top when arg2 != 0, else from the heap) and point every
 * slot at its pool entry.  Static on PC; returns the slot table like the
 * asm returns data_buffer (avdisp_wrap.c stores it). */
void *fn_80069578(int count, int fromArena)
{
    (void)count;                        /* static pool covers u8 mtxCount */
    (void)fromArena;
    mtx_slot_reset();
    return s_mtx_slot;
}

/* asm 0x80069738: free pool + table if heap-allocated (lbl_8019937C).
 * Static storage on PC: just reset the slots. */
void g_free_some_memory(void)
{
    mtx_slot_reset();
}

/* ---- stitching (GCMF_STITCHING) per-slot matrices --------------------
 * SMB1 ref u_iteratively_multiply_model_matrices / u_set_transform_matrices
 * (avdisp.c:1132 / 1284): transformMtx[i] = mtxA * boneMtx[i]; each shape's
 * mtxIndices[k] != 0xFF binds transformMtx[idx] to GX matrix slot k+1
 * (slot 0 keeps the base mtxA that load_gx_pos_nrm_mtx(mtx, 0) set).  The
 * display list's per-vertex GX_VA_PNMTXIDX byte (GX matrix id = slot*3)
 * then picks the slot per vertex.  The backend draws a batch with one
 * modelview, so parse_disp_list CPU-transforms stitched vertices by the
 * shadowed slot matrix into view space (as the GC XF unit does) and draws
 * the batch with an identity modelview. */

static float s_transform_mtx[AVDISP_MTX_SLOTS][12];  /* u_transformMtxList */

#define STITCH_ID_MTX { 1.0f, 0.0f, 0.0f, 0.0f, \
                        0.0f, 1.0f, 0.0f, 0.0f, \
                        0.0f, 0.0f, 1.0f, 0.0f }
static float s_gx_slot_mtx[10][12] = {               /* GX PNMTX0..9 shadow */
    STITCH_ID_MTX, STITCH_ID_MTX, STITCH_ID_MTX, STITCH_ID_MTX,
    STITCH_ID_MTX, STITCH_ID_MTX, STITCH_ID_MTX, STITCH_ID_MTX,
    STITCH_ID_MTX, STITCH_ID_MTX,
};

/* 3x4 row-major concat: out = a * b (translation in column 3) */
static void mtx_concat34(const float *a, const float *b, float *out)
{
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++)
            out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] +
                             a[i * 4 + 1] * b[1 * 4 + j] +
                             a[i * 4 + 2] * b[2 * 4 + j];
        out[i * 4 + 3] = a[i * 4 + 0] * b[0 * 4 + 3] +
                         a[i * 4 + 1] * b[1 * 4 + 3] +
                         a[i * 4 + 2] * b[2 * 4 + 3] +
                         a[i * 4 + 3];
    }
}

/* SMB1 u_set_transform_matrices: bind the shape's bone matrices to GX
 * slots 1..8.  Shadow only - the vertices are CPU-transformed. */
static void stitch_bind_shape_mtxs(const u8 *mtxIndices)
{
    int k;

    for (k = 0; k < 8; k++)
        if (mtxIndices[k] != 0xFF) {
            const float *m = s_transform_mtx[mtxIndices[k]];
            float *dst = s_gx_slot_mtx[k + 1];
            int j;

            for (j = 0; j < 12; j++)
                dst[j] = m[j];
        }
}

/* ---- texture init (asm 0x80065E90 g_init_gma_texture; SMB1 ref
 *      init_tev_layer_texobj, avdisp.c:969) ---------------------------- */

static int count_bits16(u32 v)
{
    int n = 0;
    while (v != 0) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

static int decode_wrap(u32 field)   /* 0 CLAMP, 1 REPEAT, 2 MIRROR */
{
    if (field == 2)
        return 2;                   /* GX_MIRROR */
    if (field == 1)
        return 1;                   /* GX_REPEAT */
    return 0;                       /* GX_CLAMP */
}

/* DOL 0x80066C40: set the TEV colour (low nibble) and alpha (high nibble)
 * blend modes of the model's layers using texture `texIdx` (layer byte 0xD);
 * the original stores (word & 0xFF) | modes into the big-endian word at 0x10,
 * i.e. clears bytes 0x10-0x12 and ORs the modes into byte 0x13 */
void fn_80066C40(struct GMAModel *model, int texIdx, int colorMode, int alphaMode)
{
    struct GMATevLayer *layers = (struct GMATevLayer *)((u8 *)model + 0x40);
    u8 modes = (u8)(((alphaMode & 0xF) << 4) | (colorMode & 0xF));
    int i;

    for (i = 0; i < model->tevLayerCount; i++) {
        if (((u8 *)&layers[i])[0xD] != (u8)texIdx)
            continue;
        layers[i].blendMode[0] = layers[i].blendMode[1] = layers[i].blendMode[2] = 0;
        layers[i].blendMode[3] |= modes;
    }
}

void g_init_gma_texture(struct GMATevLayer *layer,
                        struct TPLTextureHeader *texHeader, struct TPL *tpl)
{
    u8 *image = tpl->fileData + texHeader->imageOffset;
    u32 flags = layer->flags;
    int wrapS, wrapT, maxMips, lod, fmt;
    u16 v;

    /* asm: format & 0x100 -> "cannot initialize" report, NULL texObj */
    if (texHeader->format & 0x100) {
        OSReport("avdisp: tex format %08x not initializable\n",
                 (unsigned)texHeader->format);
        layer->texObj = 0;
        return;
    }

    wrapS = decode_wrap((flags >> TEV_LAYER_WRAP_S_SHIFT) & 3);
    wrapT = decode_wrap((flags >> TEV_LAYER_WRAP_T_SHIFT) & 3);

    /* mip capability: both dimensions must be powers of two */
    if (count_bits16(texHeader->width) > 1 || count_bits16(texHeader->height) > 1) {
        maxMips = 0;
    } else {
        v = (texHeader->width < texHeader->height) ? texHeader->width
                                                   : texHeader->height;
        maxMips = 0;
        while (v > 4) {
            v >>= 1;
            maxMips++;
        }
    }

    lod = (flags >> TEV_LAYER_MAX_LOD_SHIFT) & 0xF;
    if (maxMips == 0 && (wrapS != 0 || wrapT != 0))
        wrapS = wrapT = 0;          /* asm: OSReport + clamp both */
    if (lod == 0xF)
        lod = maxMips;              /* 0xF = "auto" */
    if (texHeader->unkC < lod)
        lod = texHeader->unkC;      /* asm clamps against texHeader+0xC */

    fmt = texHeader->format & 0x1F;
    if ((fmt == 0x0 || fmt == 0xE) && lod != 0)
        lod--;                      /* I4 / CMPR lose one mip level */

    GXInitTexObj(layer->texObj, image, texHeader->width, texHeader->height,
                 fmt, wrapS, wrapT, lod != 0);
    layer->imageData = image;       /* asm: stw r29, 0x14(r31) */
    {   /* SMB1 ref avdisp.c:1027-1052 */
        extern void GXInitTexObjLOD(GXTexObj *, int, int, float, float, float, u8, u8, int);
        int mipped = lod != 0, minFilt, magFilt;
        if (flags & (1 << 11)) {            /* GMA_TEV_LAYER_FLAG_MAGFILT_NEAR */
            magFilt = 0;                    /* GX_NEAR */
            minFilt = mipped ? 4 : 2;       /* NEAR_MIP_LIN : NEAR_MIP_NEAR */
        } else {
            magFilt = 1;                    /* GX_LINEAR */
            minFilt = mipped ? 5 : 3;       /* LIN_MIP_LIN : LIN_MIP_NEAR */
        }
        GXInitTexObjLOD(layer->texObj, minFilt, magFilt, 0.0f, (float)lod,
                        layer->lodBias / 10.0f, 0, (flags & (1 << 6)) != 0, layer->maxAniso);
    }
}

/* ---- model/material init (asm 0x8006A9C8 g_init_gma_model_materials;
 *      SMB1 ref init_model, avdisp.c:1064) ---------------------------- */

/* Advance past a shape header + its trailing display lists (SMB1 ref
 * next_shape, avdisp.c:734; matches the inline asm loop). */
static struct GMAShape *next_shape(struct GMAShape *shape)
{
    u8 *p = (u8 *)shape + 0x60;

    if (shape->dispListFlags & (1 << 0))
        p += shape->dispListSizes[0];
    if (shape->dispListFlags & (1 << 1))
        p += shape->dispListSizes[1];
    if (shape->dispListFlags & 0xC) {   /* GMAExtraDispLists block */
        struct GMAExtraDispLists *extra = (struct GMAExtraDispLists *)p;

        p += 0x20 + extra->dispListSizes[0] + extra->dispListSizes[1];
    }
    return (struct GMAShape *)p;
}

static void init_shape_render_flags(struct GMAShape *shape, struct TPL *tpl)
{
    if (tpl == 0)
        shape->tevLayerCount = 0;       /* asm: stb 0, 0x12(shape) */
    if (shape->vtxAttrs & (1 << 11))    /* GX_VA_CLR0 (rlwinm. 0,r20,20) */
        shape->flags |= SHAPE_FLAG_VERT_COLORS;
    if (shape->tevLayerCount == 0)
        shape->flags |= SHAPE_FLAG_SIMPLE_MATERIAL;
}

GXTexObj *g_init_gma_model_materials(struct GMAModel *model, struct TPL *tpl,
                                     GXTexObj *texObjs)
{
    struct GMATevLayer *layers = (struct GMATevLayer *)((u8 *)model + 0x40);
    u32 shapeCount, i;
    GXTexObj *ret = texObjs;

    if (tpl == 0)
        model->tevLayerCount = 0;

    if (model->tevLayerCount != 0) {
        if (texObjs != 0) {
            model->texObjs = texObjs;
            ret = texObjs + model->tevLayerCount;
        } else {
            model->texObjs = alloc_from_heap_or_panic(
                currentHeap, model->tevLayerCount * sizeof(GXTexObj),
                "avdisp_shim.c", __LINE__);
        }
        for (i = 0; i < model->tevLayerCount; i++) {
            layers[i].texObj = &model->texObjs[i];  /* stw at layer+0x08 */
            if (layers[i].texIndex >= tpl->numTextures) {
                /* PC: a layer naming a texture the TPL doesn't have (0xFFFF in
                 * some community stages' hidden [NODISP] models).  The GC reads
                 * a header past the table and inits a never-drawn texture from
                 * it; hashing texels at that wild address crashed here, so the
                 * layer gets the empty texture instead. */
                GXInitTexObj(layers[i].texObj, 0, 8, 8, 6 /* RGBA8 */, 0, 0, 0);
                layers[i].imageData = 0;
                continue;
            }
            /* asm: r4 = tpl->texHeaders + (texIndex << 4), r5 = tpl */
            g_init_gma_texture(&layers[i], &tpl->texHeaders[layers[i].texIndex],
                               tpl);
        }
    } else {
        model->texObjs = 0;
    }

    /* shape render-flag pass (SMB1 ref set_shape_flags_in_model) */
    shapeCount = model->opaqueShapeCount + model->translucentShapeCount;
    if (model->flags & (GCMF_SKIN | GCMF_EFFECTIVE)) {
        /* 0x20 pre-header, then flat 0x60-stride shape headers */
        u8 *sp = (u8 *)model + model->headerSize;

        *(u32 *)(sp + 0x1C) = model->flags;     /* asm: stw flags, 0x1C */
        sp += 0x20;
        for (i = 0; i < shapeCount; i++, sp += 0x60)
            init_shape_render_flags((struct GMAShape *)sp, tpl);
    } else {
        struct GMAShape *shape =
            (struct GMAShape *)((u8 *)model + model->headerSize);

        for (i = 0; i < shapeCount; i++) {
            init_shape_render_flags(shape, tpl);
            shape = next_shape(shape);
        }
    }
    return ret;
}

/* asm 0x80069964: return the s16 bone-parent index table of a SKIN model
 * (mtxCount entries at preheader + [preheader+0x10]); bg_lava uses it to
 * chain fountain vertices. NULL for non-SKIN models. */
s16 *fn_80069964(struct GMAModel *model)
{
    if (model->flags & GCMF_SKIN) {
        u8 *ph = (u8 *)model + model->headerSize;

        return (s16 *)(ph + *(u32 *)(ph + 0x10));
    }
    OSReport("fn_80069964: model %p is not SKIN\n", (void *)model);
    return 0;
}

/* ---- display-list interpreter ---------------------------------------- */

static u16 rd_u16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

static s16 rd_s16(const u8 *p)
{
    return (s16)((p[0] << 8) | p[1]);
}

static float rd_f32(const u8 *p)
{
    union { u32 u; float f; } v;

    v.u = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
    return v.f;
}

/* Fixed-point scales of the game's vertex formats (gxsync.c:23-48):
 * GX_VTXFMT1 (16-bit models): POS s16 frac 13, NRM s16 frac 14,
 * TEX0-2 s16 frac 13; everything else is f32. */
#define POS_S16_SCALE (1.0f / 8192.0f)
#define NRM_S16_SCALE (1.0f / 16384.0f)
#define TEX_S16_SCALE (1.0f / 8192.0f)

struct DrawColor
{
    int useVtxColor;        /* vertex stream carries CLR0 */
    u8 r, g, b, a;          /* material color otherwise */
};

static u8 post_color_u8(float c, int i)
{
    float v = c * s_postMult[i] + s_postAdd[i] * 255.0f;

    if (v <= 0.0f)
        return 0;
    if (v >= 255.0f)
        return 255;
    return (u8)v;
}

static void emit_color(u32 r, u32 g, u32 b, u32 a)
{
    if (s_post_in_tev) {
        GXColor4u8((u8)r, (u8)g, (u8)b, (u8)a);
        return;
    }
    GXColor4u8(post_color_u8((float)r, 0), post_color_u8((float)g, 1),
               post_color_u8((float)b, 2), post_color_u8((float)a, 3));
}

/* Interpret one big-endian GX FIFO display-list stream and replay it
 * through the immediate-mode API.  vtxAttrs bits are GC GXAttr indices
 * (SMB1 ref gxutil_set_vtx_attrs, gxutil.c:39): bit 0 GX_VA_PNMTXIDX,
 * bits 1-8 GX_VA_TEX0..7MTXIDX (u8 each when direct), bit 9 GX_VA_POS,
 * bit 10 GX_VA_NRM, bit 11 GX_VA_CLR0, bit 12 GX_VA_CLR1 (RGBA8),
 * bits 13-20 GX_VA_TEX0..TEX7. */
extern void GXSetChanCtrl(int chan, u8 enable, int ambSrc, int matSrc,
                          u32 lightMask, int diffFn, int attnFn);
extern void g_GXSetTevOrder_wrapper(int stage, int coord, int map, int color);
extern void GXSetTevOp(int stage, int mode);
extern void GXSetNumTevStages_cached(u8 n);

/* Set while the current shape's material samples GX_TG_TEX1/
 * TEX2; the display-list replay then streams those coordinates too
 * (render_opengl.c gx_texcoord_n2f32). */
static int s_multi_tc;
extern void gx_set_multi_texcoord(int on);
extern void gx_texcoord_n2f32(int n, float s, float t);

static void parse_disp_list(const u8 *dl, u32 size, u32 vtxAttrs,
                            const struct DrawColor *col, int emitTex)
{
    const u8 *p = dl;
    const u8 *end = dl + size;
    int usePnMtx = (vtxAttrs & 1u) != 0;    /* GX_VA_PNMTXIDX present */
    const float *vm = s_gx_slot_mtx[0];     /* current per-vertex matrix */

    if (usePnMtx) {
        /* vertices get CPU-transformed into view space; draw the batches
         * with an identity modelview parked in scratch GX index 30 */
        static const Mtx s_id_mtx = {
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f, 0.0f },
        };
        GXLoadPosMtxImm((void *)s_id_mtx, 30);
        GXLoadNrmMtxImm((void *)s_id_mtx, 30);  /* normals are CPU-rotated too */
        GXSetCurrentMtx(30);
    }

    while (p < end) {
        u8 op = *p++;
        u32 prim, fmt, count, v, bit;
        int s16fmt;

        if (op == 0)
            continue;               /* 32-byte alignment padding */
        prim = op & 0xF8;
        fmt = op & 0x07;
        if (prim < 0x80 || prim > 0xB8) {
            shim_trace("avdisp: bad GX opcode %02x in display list\n", op);
            return;
        }
        if (p + 2 > end)
            return;
        count = rd_u16(p);
        p += 2;
        s16fmt = (fmt == 1);        /* GX_VTXFMT1 = compressed 16-bit */

        /* the backend takes the raw GC primitive opcode (0x80 quads,
         * 0x90 tris, 0x98 strip, 0xA0 fan, ...) as GXBegin's first arg */
        GXBegin((int)prim, (int)fmt, (u16)count);
        for (v = 0; v < count && p < end; v++) {
            for (bit = 0; bit <= 20; bit++) {
                if (!(vtxAttrs & (1u << bit)))
                    continue;
                if (bit == 0) {                     /* GX_VA_PNMTXIDX */
                    u32 slot = *p / 3;              /* GX mtx id -> slot */

                    if (slot < 10)
                        vm = s_gx_slot_mtx[slot];
                    p += 1;
                } else if (bit <= 8) {
                    p += 1;         /* TEXnMTXIDX: u8, ignored */
                } else if (bit == 9) {              /* GX_VA_POS */
                    float x, y, z;

                    if (s16fmt) {
                        x = rd_s16(p) * POS_S16_SCALE;
                        y = rd_s16(p + 2) * POS_S16_SCALE;
                        z = rd_s16(p + 4) * POS_S16_SCALE;
                        p += 6;
                    } else {
                        x = rd_f32(p);
                        y = rd_f32(p + 4);
                        z = rd_f32(p + 8);
                        p += 12;
                    }
                    if (usePnMtx) {                 /* -> view space */
                        float tx = vm[0] * x + vm[1] * y + vm[2]  * z + vm[3];
                        float ty = vm[4] * x + vm[5] * y + vm[6]  * z + vm[7];
                        float tz = vm[8] * x + vm[9] * y + vm[10] * z + vm[11];

                        x = tx;
                        y = ty;
                        z = tz;
                    }
                    GXPosition3f32(x, y, z);
                } else if (bit == 10) {             /* GX_VA_NRM */
                    float x, y, z;

                    if (s16fmt) {
                        x = rd_s16(p) * NRM_S16_SCALE;
                        y = rd_s16(p + 2) * NRM_S16_SCALE;
                        z = rd_s16(p + 4) * NRM_S16_SCALE;
                        p += 6;
                    } else {
                        x = rd_f32(p);
                        y = rd_f32(p + 4);
                        z = rd_f32(p + 8);
                        p += 12;
                    }
                    if (usePnMtx) {                 /* rotate by slot mtx */
                        float tx = vm[0] * x + vm[1] * y + vm[2]  * z;
                        float ty = vm[4] * x + vm[5] * y + vm[6]  * z;
                        float tz = vm[8] * x + vm[9] * y + vm[10] * z;

                        x = tx;
                        y = ty;
                        z = tz;
                    }
                    GXNormal3f32(x, y, z);
                } else if (bit == 11) {             /* GX_VA_CLR0, RGBA8 */
                    emit_color(p[0], p[1], p[2], p[3]);
                    p += 4;
                } else if (bit == 12) {             /* GX_VA_CLR1: unused */
                    p += 4;
                } else {                            /* GX_VA_TEX0..TEX7 */
                    float s, t;
                    /* VTXFMT1 compresses TEX0-2 only (gxsync.c) */
                    int tex16 = s16fmt && bit <= 15;

                    if (tex16) {
                        s = rd_s16(p) * TEX_S16_SCALE;
                        t = rd_s16(p + 2) * TEX_S16_SCALE;
                        p += 4;
                    } else {
                        s = rd_f32(p);
                        t = rd_f32(p + 4);
                        p += 8;
                    }
                    if (bit == 13 && emitTex)
                        GXTexCoord2f32(s, t);
                    else if (s_multi_tc && emitTex && bit <= 15)
                        gx_texcoord_n2f32((int)bit - 13, s, t);   /* TEX1/TEX2 */
                }
            }
            if (!col->useVtxColor)
                emit_color(col->r, col->g, col->b, col->a);
        }
        GXEnd();
    }

    if (usePnMtx)
        GXSetCurrentMtx(0);     /* back to the base matrix for other draws */
}

/* ---- shape drawing (asm 0x8006A52C avdisp_draw_static_shape_now;
 *      SMB1 ref draw_shape, avdisp.c:1304) ---------------------------- */

/* renderer hook (render_opengl.c texgen/fog block) and the
 * avdisp custom texture matrix (g_something_with_texture_scroll_3 =
 * SMB1 avdisp_set_custom_tex_mtx; NULL disables). */
extern int gx_tex_gxfx_active(void);
extern void GXSetTexCoordGen2(int dst, int func, int src, int mtx, int normalize, int postMtx);
extern void GXLoadTexMtxImm(const Mtx mtx, int id, int type);
static void *s_custom_material_cb;
static Mtx s_custom_tex_mtx;
static int s_custom_tex_mtx_on;
static int s_texgen_touched; /* normal path changed texgen 0 this model */
void g_something_with_texture_scroll_3(int id, Mtx mtx)
{
    (void)id;
    if (mtx == NULL) {
        s_custom_tex_mtx_on = 0;
        return;
    }
    memcpy(s_custom_tex_mtx, mtx, sizeof(Mtx));
    s_custom_tex_mtx_on = 1;
}

/* GMA blendFactors hold GC GX_BL_* values; render.h uses the same
 * numbering. */
static int map_blend_factor(u32 gc)
{
    return (int)(gc & 0xF);
}

/* GXFX (render_opengl.c) channel / TEV entry points */
extern void GXSetChanMatColor(int chan, GXColor color);
extern void GXSetChanAmbColor(int chan, GXColor color);
extern void GXSetTevColorIn_cached(int s, int a, int b, int c, int d);
extern void GXSetTevAlphaIn_cached(int s, int a, int b, int c, int d);
extern void GXSetTevColorOp_cached(int s, int op, int bias, int scale, u8 clamp, int out);
extern void GXSetTevAlphaOp_cached(int s, int op, int bias, int scale, u8 clamp, int out);
extern void GXSetTevKColor_cached(int id, GXColor color);
extern void GXSetTevKColorSel_cached(int s, int sel);
extern void GXSetTevKAlphaSel_cached(int s, int sel);
extern void GXSetTevSwapMode_cached(int s, int ras, int tex);
extern void GXSetTevSwapModeTable_cached(int id, int r, int g, int b, int a);

static u8 unit_to_u8(float f)
{
    if (f <= 0.0f)
        return 0;
    if (f >= 1.0f)
        return 255;
    return (u8)(f * 255.0f + 0.5f);
}

/* ---- TEV material (SMB1 ref avdisp.c build_tev_material and the
 *      build_*_layer helpers, 1586-1652 / 1763-2490; SMB2 asm
 *      g_some_GmaSomeStruct_func 0x80067018 colors, 0x80069000 chans) ----
 *
 * Ported without the GC material cache (every shape rebuilds its stages;
 * the GX setters are cached by the backend anyway):
 *  - COLOR0 lighting for lit shapes, C0 material color for unlit ones
 *  - one TEV stage per layer (diffuse / alpha blend / view specular), two
 *    for world specular; TEXMAP0 is the specular ramp, layers use 1..
 *  - the TEV material callback (lbl_80199320, e.g. the ball shell's
 *    ball_draw.c fn_1_4D82C) appends/patches stages via TevStageInfo
 *  - post multiply (K2) / post add (K3) stages
 *  - alpha blend layers take the alpha from a texture colour channel
 *    through the TEV swap tables (as fn_8006877C does) */
#define TEV_LAYER_VIEW_SPECULAR  (1u << 0)
#define TEV_LAYER_TYPE3          (1u << 1)
#define TEV_LAYER_ALPHA_BLEND    (1u << 13)
#define TEV_LAYER_WORLD_SPECULAR (1u << 15)
#define TEV_LAYER_WORLD_SPECULAR2 (1u << 20) /* asm type mask 0x10A003 */

static u32 s_avdisp_flag_320; /* lbl_80199320 (TEV material callback), below */
/* byte 0 of the GC material cache (lbl_80194160): set to 1 per model by
 * g_avdisp_smth_with_tex_materials (asm 0x80067558, only without a custom
 * material callback), cleared after each shape's material (0x80067A98).
 * The TEV material callback gets it as BallEnvArg.unk0: bg_wat2's caustics
 * (fn_1_8711C / fn_1_873C0) skip their TEV / texture setup for a shape when
 * it is 0 and the stage indices match the last call, so it must be 1 for
 * the first shape of each model, whose GX state another model overwrote. */
static int s_material_first;
extern void GXSetTexCoordGen(int dst, int func, int src, int mtx);
extern void GXSetNumTexGens(u8 n);
extern void GXSetTevColor(int id, GXColor color);

/* per-model material state (SMB1 s_materialCache.modelDir_rt_view, unk44,
 * unk48, unk4C), reset by material_begin_model */
static float s_modelDir[3] = { 0.0f, 0.0f, -1.0f };
static int s_texMtx0Loaded, s_ptTexMtx0Loaded, s_ptTexMtx12Loaded;

/* SMB1 init_some_texture: 16x4 I8 ramp, left tile white, right tile black
 * (world specular hemisphere mask, sampled through PTTEXMTX2) */
static GXTexObj s_rampTex;
static u8 s_rampImg[64] __attribute__((aligned(32)));
static int s_rampInit;

/* blend factors of a shape, GC GXBlendFactor encoding (SMB1 ref
 * init_tev_material_cache, avdisp.c:1513): default SRCALPHA/INVSRCALPHA,
 * overridable per shape from blendFactors.  (The asm also checks the
 * overrides lbl_80199340 / lbl_8019933C, which are only ever -1 = none.) */
static void shape_blend_factors(const struct GMAShape *shape, u32 *src, u32 *dst)
{
    *src = 4;                                       /* GC GX_BL_SRCALPHA */
    *dst = 5;                                       /* GC GX_BL_INVSRCALPHA */
    if (shape->flags & SHAPE_FLAG_CUSTOM_BLEND_SRC)
        *src = shape->blendFactors & 0xF;
    if (shape->flags & SHAPE_FLAG_CUSTOM_BLEND_DST)
        *dst = (shape->blendFactors >> 4) & 0xF;
}

/* blend src/dst of the GC material cache (lbl_80194160+0x24/+0x28) */
static int s_blendCache[2] = { -1, -1 };

/* asm g_avdisp_smth_with_tex_materials 0x800676A4-0x800676F8 (per model,
 * only without a custom material callback): cache and set the first
 * shape's blend */
static void material_begin_model_blend(struct GMAShape *first)
{
    u32 src, dst;

    shape_blend_factors(first, &src, &dst);
    s_blendCache[0] = (int)src;
    s_blendCache[1] = (int)dst;
    GXSetBlendMode_cached(1 /* GX_BM_BLEND */, map_blend_factor(src),
                          map_blend_factor(dst), 0 /* GX_LO_CLEAR */);
}

/* SMB1 init_tev_material_cache tail: view-space direction to the near
 * side of the model's bounding sphere */
static void material_begin_model(struct GMAModel *model)
{
    const float *m = s_gx_slot_mtx[0];
    const float *c = &model->boundSphereCenter.x;
    float d[3], l;

    d[0] = m[0] * c[0] + m[1] * c[1] + m[2]  * c[2] + m[3];
    d[1] = m[4] * c[0] + m[5] * c[1] + m[6]  * c[2] + m[7];
    d[2] = m[8] * c[0] + m[9] * c[1] + m[10] * c[2] + m[11];
    d[2] -= model->boundSphereRadius;
    l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l > 1e-6f) {
        s_modelDir[0] = d[0] / l;
        s_modelDir[1] = d[1] / l;
        s_modelDir[2] = d[2] / l;
    }
    s_texMtx0Loaded = s_ptTexMtx0Loaded = s_ptTexMtx12Loaded = 0;
}

static void vec_norm3(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

    if (l > 1e-12f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* Dolphin MTXLookAt(m, camPos = origin, up, target) */
static void lookat_origin(Mtx m, const float *up, const float *target)
{
    float look[3], right[3], u[3];
    int i;

    look[0] = -target[0];
    look[1] = -target[1];
    look[2] = -target[2];
    vec_norm3(look);
    right[0] = up[1] * look[2] - up[2] * look[1];
    right[1] = up[2] * look[0] - up[0] * look[2];
    right[2] = up[0] * look[1] - up[1] * look[0];
    vec_norm3(right);
    u[0] = look[1] * right[2] - look[2] * right[1];
    u[1] = look[2] * right[0] - look[0] * right[2];
    u[2] = look[0] * right[1] - look[1] * right[0];
    for (i = 0; i < 3; i++) {
        m[0][i] = right[i];
        m[1][i] = u[i];
        m[2][i] = look[i];
    }
    m[0][3] = m[1][3] = m[2][3] = 0.0f;
}

/* lookAt -> sphere-map projection: s = 0.5 + 0.5 right.r,
 * t = 0.5 - 0.5 up.r, q = 1 (the shared tail of u_compute_texmtx0/1) */
static void sphere_map_mtx(Mtx m)
{
    int i;

    for (i = 0; i < 3; i++) {
        m[0][i] *= 0.5f;
        m[1][i] *= -0.5f;
        m[2][i] = 0.0f;
    }
    m[0][3] = 0.5f;
    m[1][3] = 0.5f;
    m[2][3] = 1.0f;
}

/* TEXMTX0 = mtxA rotation: GX_TG_NRM texgens see model-space normals */
static void load_texmtx0(void)
{
    Mtx m;
    int i;

    if (s_texMtx0Loaded)
        return;
    for (i = 0; i < 3; i++) {
        m[i][0] = s_gx_slot_mtx[0][i * 4 + 0];
        m[i][1] = s_gx_slot_mtx[0][i * 4 + 1];
        m[i][2] = s_gx_slot_mtx[0][i * 4 + 2];
        m[i][3] = 0.0f;
    }
    GXLoadTexMtxImm(m, 30 /* GX_TEXMTX0 */, 0);
    s_texMtx0Loaded = 1;
}

/* SMB1 u_compute_texmtx0 */
static void compute_pttexmtx0(void)
{
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    Mtx m;

    lookat_origin(m, up, s_modelDir);
    sphere_map_mtx(m);
    GXLoadTexMtxImm(m, 64 /* GX_PTTEXMTX0 */, 0);
}

/* SMB1 u_compute_texmtx1and2: half vector between the infinite light and
 * the reversed model direction; PTTEXMTX1 maps the specular texture
 * around it, PTTEXMTX2 the ramp (s = 0.5 + 0.5 look.r, i.e. white on the
 * hemisphere facing the half vector) */
static void compute_pttexmtx12(void)
{
    float target[3], sp44[3], up[3];
    Mtx look, m;
    int i, j;

    for (i = 0; i < 3; i++) {
        sp44[i] = s_modelDir[i] * -0.9f;
        target[i] = (sp44[i] + s_infLightDir[i]) * 0.5f;
        up[i] = target[i] - sp44[i];
    }
    lookat_origin(look, up, target);
    memcpy(m, look, sizeof(Mtx));
    sphere_map_mtx(m);
    GXLoadTexMtxImm(m, 67 /* GX_PTTEXMTX1 */, 0);
    /* [0 0 .5 .5; 0 0 0 0; 0 0 0 1] * look */
    for (j = 0; j < 4; j++) {
        m[0][j] = 0.5f * look[2][j];
        m[1][j] = 0.0f;
        m[2][j] = 0.0f;
    }
    m[0][3] += 0.5f;
    m[2][3] = 1.0f;
    GXLoadTexMtxImm(m, 70 /* GX_PTTEXMTX2 */, 0);
}

static void tev_stage_simple(int stage, int coord, int map, int cin[4],
                             int ain[4], int out)
{
    g_GXSetTevOrder_wrapper(stage, coord, map, 4 /* GX_COLOR0A0 */);
    GXSetTevColorIn_cached(stage, cin[0], cin[1], cin[2], cin[3]);
    GXSetTevColorOp_cached(stage, 0, 0, 0, 1, out);
    GXSetTevAlphaIn_cached(stage, ain[0], ain[1], ain[2], ain[3]);
    GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, out);
}

/* asm fn_80067C54 (SMB2 diffuse layer): unlike SMB1, the layer word at
 * +0x10 selects how the texture combines with the previous result.
 * Color (bits 0-3): 0 prev*tex, 1 prev+tex, 2 prev-tex, 3 prev (texture
 * color unused), 4 lerp(prev, tex*ras, tex.a) over two stages; other
 * values leave the stage's color inputs as they were.  Alpha (bits 4-7):
 * 0 prev*tex, 1 prev+tex, 2 prev-tex, 3 prev, 4 tex, 5 1.0 (KASEL_1).
 * E.g. the Monkey Boat water (WAVE_GRID*) adds its second wave layer. */
static void build_diffuse_layer(struct TevStageInfo *tev,
                                struct GMATevLayer *layer, int colorIn,
                                int alphaIn)
{
    int colorMode = layer->blendMode[3] & 0xF;
    int alphaMode = (layer->blendMode[3] >> 4) & 0xF;
    int stage = tev->tevStage;

    g_GXSetTevOrder_wrapper(stage, tev->texCoordId, tev->texMapId, 4 /* GX_COLOR0A0 */);
    switch (colorMode) {
    case 0:     /* ZERO, TEXC, prev, ZERO */
        GXSetTevColorIn_cached(stage, 15, 8, colorIn, 15);
        GXSetTevColorOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    case 1:     /* ZERO, TEXC, ONE, prev: add */
    case 2:     /* same inputs, GX_TEV_SUB */
        GXSetTevColorIn_cached(stage, 15, 8, 12, colorIn);
        GXSetTevColorOp_cached(stage, colorMode == 2, 0, 0, 1, 0);
        break;
    case 3:     /* prev only */
        GXSetTevColorIn_cached(stage, 15, 15, 15, colorIn);
        GXSetTevColorOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    case 4:     /* tex*ras -> REG2 (alpha passes prev), then
                 * lerp(CPREV, C2, TEXA) on the same coord/map */
        GXSetTevColorIn_cached(stage, 15, 8, 10, 15);
        GXSetTevColorOp_cached(stage, 0, 0, 0, 1, 3 /* GX_TEVREG2 */);
        GXSetTevAlphaIn_cached(stage, 7, 7, 7, alphaIn);
        GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
        stage = ++tev->tevStage;
        g_GXSetTevOrder_wrapper(stage, tev->texCoordId, tev->texMapId, 4);
        GXSetTevColorIn_cached(stage, 0, 6, 9, 15);
        GXSetTevColorOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    default:
        break;
    }
    switch (alphaMode) {
    case 0:     /* ZERO, TEXA, prev, ZERO */
        GXSetTevAlphaIn_cached(stage, 7, 4, alphaIn, 7);
        GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    case 1:
    case 2:     /* TEXA, ZERO, ZERO, prev: add / sub */
        GXSetTevAlphaIn_cached(stage, 4, 7, 7, alphaIn);
        GXSetTevAlphaOp_cached(stage, alphaMode == 2, 0, 0, 1, 0);
        break;
    case 3:
        GXSetTevAlphaIn_cached(stage, 7, 7, 7, alphaIn);
        GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    case 4:
        GXSetTevAlphaIn_cached(stage, 7, 7, 7, 4);
        GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    case 5:
        GXSetTevKAlphaSel_cached(stage, 0 /* GX_TEV_KASEL_1 */);
        GXSetTevAlphaIn_cached(stage, 7, 7, 7, 6 /* KONST */);
        GXSetTevAlphaOp_cached(stage, 0, 0, 0, 1, 0);
        break;
    default:
        break;
    }
    tev->tevStage++;
    tev->texCoordId++;
}

static void build_tev_material(struct GMAModel *model, struct GMAShape *shape)
{
    struct GMATevLayer *layers = (struct GMATevLayer *)((u8 *)model + 0x40);
    struct TevStageInfo tev;
    int vc = (shape->flags & SHAPE_FLAG_VERT_COLORS) != 0;
    int colorIn = 10 /* GX_CC_RASC */, alphaIn = 5 /* GX_CA_RASA */;
    GXColor mat;

    memset(&tev, 0, sizeof(tev));
    tev.texMtxId = 0x2D;        /* GX_TEXMTX5: first free for callbacks
                                 * (asm g_some_GmaSomeStruct_func3 0x800677DC) */
    tev.texMapId = 1;           /* GX_TEXMAP0 is the specular ramp */
    tev.postTexMtxId = 0x49;    /* GX_PTTEXMTX3 */
    tev.unk18 = 4;
    tev.unk1C = 1;

    if (shape->flags & ((1u << 3) | SHAPE_FLAG_SIMPLE_MATERIAL)) {
        mat.r = shape->materialColor.r;
        mat.g = shape->materialColor.g;
        mat.b = shape->materialColor.b;
    } else {
        mat.r = mat.g = mat.b = 255;
    }
    {
        float a = (float)shape->alpha * s_materialAlpha;

        mat.a = a <= 0.0f ? 0 : (a >= 255.0f ? 255 : (u8)a);
    }

    if (shape->flags & 1u) {                        /* SHAPE_FLAG_UNLIT */
        if (vc) {
            GXSetChanCtrl(4 /* GX_COLOR0A0 */, 0, 1, 1, 0, 0, 2);
        } else {
            /* before the other GXFX setters: GXSetTevColor also toggles
             * the backend's 2D UI TEV mode, which they then clear */
            GXSetTevColor(1 /* GX_TEVREG0 */, mat);
            colorIn = 2;                            /* GX_CC_C0 */
            alphaIn = 1;                            /* GX_CA_A0 */
        }
    } else {
        GXColor amb;

        if (shape->flags & (1u << 3)) {             /* CUSTOM_MAT_AMB */
            amb.r = unit_to_u8(shape->ambientColor.r / 255.0f * s_ambient[0]);
            amb.g = unit_to_u8(shape->ambientColor.g / 255.0f * s_ambient[1]);
            amb.b = unit_to_u8(shape->ambientColor.b / 255.0f * s_ambient[2]);
        } else {
            amb.r = unit_to_u8(s_ambient[0]);
            amb.g = unit_to_u8(s_ambient[1]);
            amb.b = unit_to_u8(s_ambient[2]);
        }
        amb.a = 255;
        GXSetChanCtrl(2 /* GX_ALPHA0 */, 0, 0, vc, 0, 0, 2);
        GXSetChanCtrl(0 /* GX_COLOR0 */, 1, 0, vc, s_lightMask, 2 /* CLAMP */,
                      s_lightAttnFn);
        GXSetChanMatColor(4 /* GX_COLOR0A0 */, mat);
        GXSetChanAmbColor(4, amb);
    }

    if (shape->flags & SHAPE_FLAG_SIMPLE_MATERIAL) {
        int cin[4] = { 15, 15, 15, colorIn }, ain[4] = { 7, 7, 7, alphaIn };

        tev_stage_simple(tev.tevStage, 0xFF, 0xFF, cin, ain, 0);
        tev.tevStage++;
    } else {
        int li;

        for (li = 0; li < shape->tevLayerCount && li < 3; li++) {
            struct GMATevLayer *layer;
            u32 type;
            int texGenSrc = 4 + li;                 /* GX_TG_TEX0 + li */
            int layerMtx;

            if (shape->tevLayerIdxs[li] >= model->tevLayerCount)
                break;
            layer = &layers[shape->tevLayerIdxs[li]];
            /* only layers with flag 0x20000 use the custom (scroll) texture
             * matrix, which the asm keeps in GX_TEXMTX2 (0x8006757C,
             * fn_8006877C); others GX_IDENTITY, so a water scroll doesn't
             * swim the ground and trees */
            layerMtx = (layer->flags & 0x20000u) ? 36 /* TEXMTX2 */ : 60 /* GX_IDENTITY */;
            if (texGenSrc > 4 && !s_multi_tc) {
                s_multi_tc = 1;
                gx_set_multi_texcoord(1);
            }
            type = layer->flags & (TEV_LAYER_ALPHA_BLEND | TEV_LAYER_TYPE3 |
                                   TEV_LAYER_VIEW_SPECULAR |
                                   TEV_LAYER_WORLD_SPECULAR |
                                   TEV_LAYER_WORLD_SPECULAR2);
            GXLoadTexObj_cached(layer->texObj, tev.texMapId);
            if (type == 0) {                        /* diffuse */
                GXSetTexCoordGen(tev.texCoordId, 1, texGenSrc, layerMtx);
                build_diffuse_layer(&tev, layer, colorIn, alphaIn);
            } else if (type & TEV_LAYER_ALPHA_BLEND) {
                int cin[4] = { 15, 15, 15, colorIn }, ain[4] = { 7, 4, alphaIn, 7 };
                u32 alphaSrc = layer->flags & 0xC0000u;

                /* asm 0x800688A4: flags 0x40000/0x80000/0xC0000 point SWAP2
                 * at the texture's G/B/A, else SWAP1 (the game's RGBR
                 * table): the layer's alpha is the texture's red channel */
                if (alphaSrc != 0) {
                    GXSetTevSwapModeTable_cached(2, 0, 1, 2, (int)(alphaSrc >> 18));
                    GXSetTevSwapMode_cached(tev.tevStage, 0, 2);
                } else {
                    GXSetTevSwapMode_cached(tev.tevStage, 0, 1);
                }
                GXSetTexCoordGen(tev.texCoordId, 1, texGenSrc, layerMtx);
                tev_stage_simple(tev.tevStage, tev.texCoordId, tev.texMapId,
                                 cin, ain, 0);
                tev.tevStage++;
                tev.texCoordId++;
            } else if (type & TEV_LAYER_TYPE3) {
                int cin[4] = { 15, 15, 15, colorIn }, ain[4] = { 7, 7, 7, alphaIn };

                GXSetTexCoordGen(tev.texCoordId, 1, texGenSrc, layerMtx);
                tev_stage_simple(tev.tevStage, tev.texCoordId, tev.texMapId,
                                 cin, ain, 0);
                tev.unk20 = 1;
                tev.texCoordId2 = tev.texCoordId;
                tev.texMapId2 = tev.texMapId;
                tev.tevStage++;
                tev.texCoordId++;
            } else if (type & TEV_LAYER_VIEW_SPECULAR) {
                int cin[4] = { 15, 8, 14, colorIn }, ain[4] = { 7, 7, 7, alphaIn };
                /* asm fn_8006877C 0x80068B34 -> fn_80068040: SMB2 sets K0
                 * to a grey intensity, 0x80 (half) by default or the
                 * layer's byte +0x0C when the layer has flag 0x100000; not
                 * the shape's specular colour (that is SMB1, and SMB2's
                 * world-specular layer below). */
                u8 ki = (layer->flags & 0x100000u) ? layer->fillerC[0] : 0x80;
                GXColor k;

                k.r = k.g = k.b = k.a = ki;
                GXSetTevKColor_cached(0, k);
                load_texmtx0();
                if (!s_ptTexMtx0Loaded) {
                    compute_pttexmtx0();
                    s_ptTexMtx0Loaded = 1;
                }
                GXSetTevKColorSel_cached(tev.tevStage, 0x0C /* K0 */);
                GXSetTexCoordGen2(tev.texCoordId, 0 /* MTX3x4 */, 1 /* NRM */,
                                  30, 1, 64 /* GX_PTTEXMTX0 */);
                tev_stage_simple(tev.tevStage, tev.texCoordId, tev.texMapId,
                                 cin, ain, 0);
                tev.tevStage++;
                tev.texCoordId++;
            } else {                                /* world specular */
                int cinA[4] = { 15, 8, 14, 15 }, ainA[4] = { 7, 7, 7, alphaIn };
                int cinB[4] = { 15, 8, 6, colorIn }, ainB[4] = { 7, 7, 7, alphaIn };
                GXColor k = shape->specularColor;

                if (k.r == 0 && k.g == 0 && k.b == 0)
                    k.r = k.g = k.b = 255;
                k.r = unit_to_u8(k.r / 255.0f * s_infLightColor[0]);
                k.g = unit_to_u8(k.g / 255.0f * s_infLightColor[1]);
                k.b = unit_to_u8(k.b / 255.0f * s_infLightColor[2]);
                GXSetTevKColor_cached(1, k);
                load_texmtx0();
                if (!s_ptTexMtx12Loaded) {
                    if (!s_rampInit) {
                        memset(s_rampImg, 0xFF, 32);
                        memset(s_rampImg + 32, 0x00, 32);
                        GXInitTexObj(&s_rampTex, s_rampImg, 16, 4,
                                     1 /* GX_TF_I8 */, 0, 0, 0);
                        s_rampInit = 1;
                    }
                    GXLoadTexObj_cached(&s_rampTex, 0);
                    compute_pttexmtx12();
                    s_ptTexMtx12Loaded = 1;
                }
                GXSetTevKColorSel_cached(tev.tevStage, 0x0D /* K1 */);
                GXSetTexCoordGen2(tev.texCoordId, 0, 1, 30, 1, 70 /* PTTEXMTX2 */);
                tev_stage_simple(tev.tevStage, tev.texCoordId, 0, cinA, ainA,
                                 3 /* GX_TEVREG2 */);
                GXSetTexCoordGen2(tev.texCoordId + 1, 0, 1, 30, 1, 67 /* PTTEXMTX1 */);
                tev_stage_simple(tev.tevStage + 1, tev.texCoordId + 1,
                                 tev.texMapId, cinB, ainB, 0);
                tev.tevStage += 2;
                tev.texCoordId += 2;
            }
            tev.texMapId++;
            colorIn = 0;                            /* GX_CC_CPREV */
            alphaIn = 0;                            /* GX_CA_APREV */
        }
    }

    if (s_avdisp_flag_320 != 0) {                   /* material callback */
        struct BallEnvArg arg;

        arg.unk0 = s_material_first;
        arg.shape = (struct BallEnvShape *)shape;
        arg.unk8 = layers;
        arg.tev = tev;
        ((void (*)(struct BallEnvArg *))(uintptr_t)s_avdisp_flag_320)(&arg);
        tev = arg.tev;
    }

    if (s_postMult[0] != 1.0f || s_postMult[1] != 1.0f ||
        s_postMult[2] != 1.0f || s_postMult[3] != 1.0f) {
        int cin[4] = { 15, 0, 14, 15 }, ain[4] = { 7, 0, 6, 7 };
        GXColor k;

        k.r = unit_to_u8(s_postMult[0]);
        k.g = unit_to_u8(s_postMult[1]);
        k.b = unit_to_u8(s_postMult[2]);
        k.a = unit_to_u8(s_postMult[3]);
        GXSetTevKColor_cached(2, k);
        GXSetTevKColorSel_cached(tev.tevStage, 0x0E);       /* K2 */
        GXSetTevKAlphaSel_cached(tev.tevStage, 0x1E);       /* K2_A */
        tev_stage_simple(tev.tevStage, 0xFF, 0xFF, cin, ain, 0);
        tev.tevStage++;
    }
    if (s_postAdd[0] != 0.0f || s_postAdd[1] != 0.0f ||
        s_postAdd[2] != 0.0f || s_postAdd[3] != 0.0f) {
        int cin[4] = { 0, 15, 15, 14 }, ain[4] = { 0, 7, 7, 6 };
        GXColor k;

        k.r = unit_to_u8(s_postAdd[0]);
        k.g = unit_to_u8(s_postAdd[1]);
        k.b = unit_to_u8(s_postAdd[2]);
        k.a = unit_to_u8(s_postAdd[3]);
        GXSetTevKColor_cached(3, k);
        GXSetTevKColorSel_cached(tev.tevStage, 0x0F);       /* K3 */
        GXSetTevKAlphaSel_cached(tev.tevStage, 0x1F);       /* K3_A */
        tev_stage_simple(tev.tevStage, 0xFF, 0xFF, cin, ain, 0);
        tev.tevStage++;
    }
    GXSetNumTevStages_cached((u8)tev.tevStage);
    {   /* indirect stages appended by the material callback (water warp) */
        extern void GXSetNumIndStages(u8 n);
        GXSetNumIndStages((u8)tev.tevIndStage);
    }
    GXSetNumTexGens((u8)tev.texCoordId);
    s_post_in_tev = 1;
    s_material_first = 0;
}

/* Material setup shared by the static and deformable shape paths: build
 * the GX channel/TEV material (build_tev_material), fill the vertex color
 * fallback and set the blend mode.  Returns whether the shape samples
 * texture layers (i.e. its TEX0 coordinates must be emitted). */
static int bind_shape_material(struct GMAModel *model, struct GMAShape *shape,
                               struct DrawColor *col)
{
    int textured;
    u32 src, dst;

    s_post_in_tev = 0;
    if (s_multi_tc) {       /* previous shape's multi-layer material */
        s_multi_tc = 0;
        gx_set_multi_texcoord(0);
    }
    /* custom material callback (lbl_80199370, asm 0x80069F14):
     * a nonzero return means the caller already configured TEV/texgen/
     * textures/blend (shadow casters and receivers, ...) and the GMA
     * material setup is skipped.  Checked before gx_ui_tev_disable, which
     * would reset that caller-built GX TEV state.  Only honoured while the
     * GXFX TEV emulation is active (the caller's setup is then drawable).
     * The argument is the asm's sp+8 block {+4 shape, +8 the model's TEV
     * layers}; callbacks such as polydisp fn_1_993C dereference it. */
    struct { u32 unk0; struct GMAShape *shape; void *layers; } cbArg;
    cbArg.unk0 = 0;
    cbArg.shape = shape;
    cbArg.layers = (u8 *)model + 0x40;
    if (s_custom_material_cb != 0 && gx_tex_gxfx_active() &&
        ((int (*)(void *))s_custom_material_cb)(&cbArg) != 0) {
        col->useVtxColor = 0;
        col->r = col->g = col->b = col->a = 255;
        /* the GC vertex stream still carries TEX0 (the callback's texgens
         * read it, e.g. the boat wavy-mirror ripple coords); emit_* only
         * sends it when the shape has GX_VA_TEX0 */
        return 1;
    }

    {
        /* leave the 2D UI TEV mode (tex*C0+C1); 3D wants vtxcolor*tex */
        extern void gx_ui_tev_disable(void);
        gx_ui_tev_disable();
    }

    textured = !(shape->flags & SHAPE_FLAG_SIMPLE_MATERIAL) &&
               shape->tevLayerCount != 0;

    col->useVtxColor = (shape->vtxAttrs & (1 << 11)) != 0;
    col->r = shape->materialColor.r;
    col->g = shape->materialColor.g;
    col->b = shape->materialColor.b;
    col->a = shape->alpha;

    /* ---- avdisp texcoord/material state (SMB1 ref avdisp.c) ----
     * init_tev_material_cache loads TEXMTX1 (identity unless a custom tex
     * mtx is enabled) and build_diffuse_layer uses
     * GXSetTexCoordGen(id, MTX2x4, TEX0, GX_TEXMTX1); SMB2 uses GX_TEXMTX2
     * for it (asm 0x8006757C).  Set explicitly so a draw doesn't inherit
     * the previous draw's texgen/texmtx (e.g. a shadow projection). */
    {
        extern void GXLoadTexMtxImm(const Mtx mtx, int id, int type);
        extern void GXSetTexCoordGen(int dst, int func, int src, int mtx);
        static const Mtx s_avdispIdentityTexMtx = {
            {1.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f, 0.0f},
            {0.0f, 0.0f, 1.0f, 0.0f},
        };
        GXLoadTexMtxImm(s_avdispIdentityTexMtx, 36 /* GX_TEXMTX2 */, 1 /* GX_MTX3x4 */);
        GXSetTexCoordGen(0 /* GX_TEXCOORD0 */, 1 /* GX_TG_MTX2x4 */,
                         4 /* GX_TG_TEX0 */, 36 /* GX_TEXMTX2 */);
    }
    /* ---- end avdisp texcoord/material state ---- */
    /* TEXMTX2 = custom texture matrix (texture scroll,
     * g_something_with_texture_scroll_3) when enabled, else the identity
     * loaded above; texgen0 -> identity is restored at draw_model end */
    if (s_custom_tex_mtx_on)
        GXLoadTexMtxImm(s_custom_tex_mtx, 36 /* GX_TEXMTX2 */, 1);
    s_texgen_touched = 1;

    /* asm 0x80067838 (g_some_GmaSomeStruct_func3): GMA shape flag 0x4 =
     * drawn without fog (e.g. the boat race sky dome MOT_FAROBJ) */
    {
        extern void fn_80066674(int on);
        fn_80066674(!(shape->flags & (1u << 2)));
    }
    build_tev_material(model, shape);

    /* blend (asm g_something_with_GmaSomeStruct_GXBlendFactor 0x80067AD0,
     * called after the TEV material and its callback): only set when the
     * shape's factors differ from the material cache's (s_blendCache, set
     * per model from the first shape), so a TEV material callback's own
     * GXSetBlendMode survives the shapes that use the model's blend (the
     * additive ball environment pass, e.g. bg_wat2 fn_1_87A0C) */
    shape_blend_factors(shape, &src, &dst);
    if ((int)src != s_blendCache[0] || (int)dst != s_blendCache[1]) {
        GXSetBlendMode_cached(1 /* GX_BM_BLEND */, map_blend_factor(src),
                              map_blend_factor(dst), 0 /* GX_LO_CLEAR */);
        s_blendCache[0] = (int)src;
        s_blendCache[1] = (int)dst;
    }
    return textured;
}

static u32 s_avdisp_mode_374; /* lbl_80199374, defined below */
static u32 s_avdisp_flag_320; /* lbl_80199320 (TEV material callback), below */

/* The asm writes its cull values straight into the genMode cull field
 * (g_maybe_something_with_normals, 0x8006A894: slwi 14 into gx->0x204), i.e.
 * in HARDWARE encoding, where FRONT and BACK are swapped relative to the
 * GXCullMode API enum (GXSetCullMode swaps API FRONT<->BACK before writing).
 * GXSetCullMode_cached takes the API enum, so convert. */
static int avdisp_hw_cull_to_gx(u32 hw)
{
    static const u8 s_hw_to_gx[4] = { 0 /*NONE*/, 2 /*BACK*/, 1 /*FRONT*/, 3 /*ALL*/ };
    return s_hw_to_gx[hw & 3];
}

/* asm avdisp_draw_static_shape_now (0x8006A550/0x8006A60C, extra lists
 * 0x8006A64C/0x8006A65C): list 0 = lbl_80199374 ^ 2, list 1 =
 * lbl_80199374 ^ 1, hardware-encoded: hw 2 = API GX_CULL_FRONT and hw 1 =
 * API GX_CULL_BACK, the same order as SMB1's avdisp.c.  fn_80069AD0 sets
 * mode 3 to mirror. */
static int static_cull(int list)
{
    return avdisp_hw_cull_to_gx(s_avdisp_mode_374 ^ (list == 0 ? 2u : 1u));
}

static struct GMAShape *draw_shape(struct GMAModel *model,
                                   struct GMAShape *shape)
{
    struct DrawColor col;
    u8 *dl = (u8 *)shape + 0x60;
    int doubleSided = (shape->flags & SHAPE_FLAG_DOUBLE_SIDED) != 0;
    int textured = bind_shape_material(model, shape, &col);
    {
        extern long g_mtx_trace;
        if (g_mtx_trace > 0)
            fprintf(stderr, "[trace]  shape flags=%x vattr=%x col=%d,%d,%d a=%d vc=%d tex=%d dlf=%x sz=%u,%u blend=%x\n",
                    (unsigned)shape->flags, (unsigned)shape->vtxAttrs, col.r, col.g, col.b, col.a,
                    col.useVtxColor, textured, (unsigned)shape->dispListFlags,
                    (unsigned)shape->dispListSizes[0], (unsigned)shape->dispListSizes[1],
                    (unsigned)shape->blendFactors);
        if (g_mtx_trace > 0) { /* trace: tev layer flags */
            struct GMATevLayer *tl = (struct GMATevLayer *)((u8 *)model + 0x40);
            int li;
            fprintf(stderr, "[trace]  layers n=%d", shape->tevLayerCount);
            for (li = 0; li < shape->tevLayerCount && li < 4; li++)
                fprintf(stderr, " [%u]=%08x", shape->tevLayerIdxs[li], (unsigned)tl[shape->tevLayerIdxs[li]].flags);
            fprintf(stderr, " amb=%d,%d,%d\n", shape->ambientColor.r, shape->ambientColor.g, shape->ambientColor.b);
        }
    }

    if (model->flags & GCMF_STITCHING)      /* SMB1 ref avdisp.c:1318 */
        stitch_bind_shape_mtxs(shape->mtxIndices);

    /* display list 0 shows front faces, list 1 the back faces; extra
     * lists 2/3 repeat that for alternate-winding geometry */
    if (shape->dispListFlags & (1 << 0)) {
        GXSetCullMode_cached(doubleSided ? CULL_NONE : static_cull(0));
        parse_disp_list(dl, shape->dispListSizes[0], shape->vtxAttrs, &col, textured);
        dl += shape->dispListSizes[0];
    }
    if (shape->dispListFlags & (1 << 1)) {
        GXSetCullMode_cached(doubleSided ? CULL_NONE : static_cull(1));
        parse_disp_list(dl, shape->dispListSizes[1], shape->vtxAttrs, &col, textured);
        dl += shape->dispListSizes[1];
    }
    if (shape->dispListFlags & 0xC) {
        struct GMAExtraDispLists *extra = (struct GMAExtraDispLists *)dl;
        u8 *xdl = (u8 *)extra + 0x20;

        if (model->flags & GCMF_STITCHING)  /* SMB1 ref avdisp.c:1354 */
            stitch_bind_shape_mtxs(extra->mtxIndices);
        if (shape->dispListFlags & (1 << 2)) {
            GXSetCullMode_cached(doubleSided ? CULL_NONE : static_cull(0));
            parse_disp_list(xdl, extra->dispListSizes[0], shape->vtxAttrs,
                            &col, textured);
        }
        xdl += extra->dispListSizes[0];
        if (shape->dispListFlags & (1 << 3)) {
            GXSetCullMode_cached(doubleSided ? CULL_NONE : static_cull(1));
            parse_disp_list(xdl, extra->dispListSizes[1], shape->vtxAttrs,
                            &col, textured);
        }
        xdl += extra->dispListSizes[1];
        dl = xdl;
    }
    return (struct GMAShape *)dl;
}

/* ---- deformable (SKIN / EFFECTIVE) models ----------------------------
 *
 * Layout (derived from the SMB2 asm; offsets relative to the 0x20
 * "pre-header" at model + headerSize, all offsets already byte-swapped
 * by endian_gma.c):
 *   @0x00 vertex count            (skinning loop bound, asm 0x8006A0CC)
 *   @0x04 offset: skin source     (0x20/entry, asm 0x8006A08C)
 *   @0x08 offset: vertex array    (0x40/vertex f32, asm 0x80069EE0)
 *   @0x0C offset: indexed stream  (asm 0x8006A220)
 *   @0x1C model->flags            (written at init, read for GCMF_16BIT
 *                                  at asm 0x80069FC0)
 * then the flat 0x60-stride shape headers at pre-header + 0x20.
 *
 * f32 vertex (stride 0x40): pos @0x00, nrm @0x0C, tex0/1/2 @0x18/20/28,
 *   RGBA8 @0x30, boneCount u8 @0x34               (asm fn_8006A3D8)
 * s16 vertex (GCMF_16BIT):  pos @0x00, nrm @0x06, tex0/1/2 @0x0C/10/14,
 *   RGBA8 @0x18                                   (asm fn_8006A498)
 * index stream: repeated { count, count x byte-offset-into-vertex-array }
 *   as u32s (f32 models) or u16s (16-bit models); a shape's
 *   dispListSizes[i] is the TOTAL entry count of its stream i.
 *
 * The 16-bit payload is left big-endian at load (nothing CPU-writes it;
 * see endian_gma.c), so the s16 path decodes big-endian explicitly. */

struct DeformHeader
{
    /*0x00*/ u32 vtxCount;
    /*0x04*/ u32 skinSrcOff;
    /*0x08*/ u32 vtxArrayOff;
    /*0x0C*/ u32 idxStreamOff;
    /*0x10*/ u32 unk10[3];
    /*0x1C*/ u32 modelFlags;        /* = model->flags, set at init */
};

/* CPU linear-blend skinning (asm 0x8006A08C g_avdisp_smth_with_transforms,
 * locked-cache DMA loop on GC).  Skin source entry (0x20 bytes): bind-pose
 * pos f32x3 @0x00, nrm f32x3 @0x0C, u16 bone-matrix slot @0x18, f32 weight
 * @0x1C.  Each output vertex sums boneCount (u8 @ vertex+0x34) consecutive
 * source entries: position weighted by w, normal UNWEIGHTED (the asm
 * accumulates normals with ps_add, positions with ps_madds0-by-weight). */
static void skin_transform(u8 *ph)
{
    struct DeformHeader *h = (struct DeformHeader *)ph;
    const u8 *src = ph + h->skinSrcOff;
    u8 *dst = ph + h->vtxArrayOff;
    u32 i, k;
    for (i = 0; i < h->vtxCount; i++, dst += 0x40) {
        u32 nbones = dst[0x34];
        float px = 0.0f, py = 0.0f, pz = 0.0f;
        float nx = 0.0f, ny = 0.0f, nz = 0.0f;
        float *out = (float *)dst;

        for (k = 0; k < nbones; k++, src += 0x20) {
            const float *s = (const float *)src;
            u32 mi = *(const u16 *)(src + 0x18);
            float w = *(const float *)(src + 0x1C);
            const float *m = (mi < AVDISP_MTX_SLOTS && s_mtx_slot[mi] != 0)
                                 ? s_mtx_slot[mi] : s_identity_mtx;

            px += w * (m[0] * s[0] + m[1] * s[1] + m[2] * s[2] + m[3]);
            py += w * (m[4] * s[0] + m[5] * s[1] + m[6] * s[2] + m[7]);
            pz += w * (m[8] * s[0] + m[9] * s[1] + m[10] * s[2] + m[11]);
            nx += m[0] * s[3] + m[1] * s[4] + m[2] * s[5];
            ny += m[4] * s[3] + m[5] * s[4] + m[6] * s[5];
            nz += m[8] * s[3] + m[9] * s[4] + m[10] * s[5];
        }
        out[0] = px;
        out[1] = py;
        out[2] = pz;
        out[3] = nx;
        out[4] = ny;
        out[5] = nz;
    }
}

/* asm 0x8006A3D8: emit one f32 indexed stream (native-endian after the
 * load-time swap).  GX opcode 0x98 = TRIANGLESTRIP | VTXFMT0. */
static void emit_indexed_f32(const u32 *w, u32 entries, const u8 *vtxBase,
                             u32 vtxAttrs, const struct DrawColor *col,
                             int textured)
{
    const u32 *end = w + entries;
    int emitTex = textured && (vtxAttrs & (1u << 13));      /* GX_VA_TEX0 */

    while (w < end) {
        u32 count = *w++;
        u32 v;

        if (count == 0 || count > (u32)(end - w)) {
            shim_trace("avdisp: bad deformable strip count %u\n", count);
            return;
        }
        GXBegin(0x98, 0, (u16)count);
        for (v = 0; v < count; v++) {
            const float *p = (const float *)(vtxBase + *w++);

            GXPosition3f32(p[0], p[1], p[2]);
            GXNormal3f32(p[3], p[4], p[5]);
            if (emitTex)
                GXTexCoord2f32(p[6], p[7]);
            if (col->useVtxColor) {
                const u8 *c = (const u8 *)p + 0x30;

                emit_color(c[0], c[1], c[2], c[3]);
            } else {
                emit_color(col->r, col->g, col->b, col->a);
            }
        }
        GXEnd();
    }
}

/* asm 0x8006A498: the GCMF_16BIT variant - u16 stream entries and
 * compressed s16 vertices, both still big-endian in memory.  GX opcode
 * 0x99 = TRIANGLESTRIP | VTXFMT1. */
static void emit_indexed_s16(const u8 *stream, u32 entries, const u8 *vtxBase,
                             u32 vtxAttrs, const struct DrawColor *col,
                             int textured)
{
    const u8 *p = stream;
    const u8 *end = stream + entries * 2;
    int emitTex = textured && (vtxAttrs & (1u << 13));      /* GX_VA_TEX0 */

    while (p < end) {
        u32 count = rd_u16(p);
        u32 v;

        p += 2;
        if (count == 0 || count * 2 > (u32)(end - p)) {
            shim_trace("avdisp: bad deformable strip count %u\n", count);
            return;
        }
        GXBegin(0x99, 1, (u16)count);
        for (v = 0; v < count; v++) {
            const u8 *vp = vtxBase + rd_u16(p);

            p += 2;
            GXPosition3f32(rd_s16(vp + 0x0) * POS_S16_SCALE,
                           rd_s16(vp + 0x2) * POS_S16_SCALE,
                           rd_s16(vp + 0x4) * POS_S16_SCALE);
            GXNormal3f32(rd_s16(vp + 0x6) * NRM_S16_SCALE,
                         rd_s16(vp + 0x8) * NRM_S16_SCALE,
                         rd_s16(vp + 0xA) * NRM_S16_SCALE);
            if (emitTex)
                GXTexCoord2f32(rd_s16(vp + 0xC) * TEX_S16_SCALE,
                               rd_s16(vp + 0xE) * TEX_S16_SCALE);
            if (col->useVtxColor)
                emit_color(vp[0x18], vp[0x19], vp[0x1A], vp[0x1B]);
            else
                emit_color(col->r, col->g, col->b, col->a);
        }
        GXEnd();
    }
}

/* asm 0x80069EC0 avdisp_draw_deformable_shape: returns the stream pointer
 * advanced past this shape's entries.  The GC display-list record/replay
 * bits (model->flags 0x20/0x40, shape+0x48/+0x50 cache) are skipped: the
 * PC backend has no FIFO, so the strips are always re-emitted. */
static const u8 *draw_deformable_shape(struct GMAModel *model,
                                       struct GMAShape *shape,
                                       const u8 *vtxBase, const u8 *stream,
                                       int fmt16)
{
    struct DrawColor col;
    int textured = bind_shape_material(model, shape, &col);
    int doubleSided = (shape->flags & SHAPE_FLAG_DOUBLE_SIDED) != 0;
    u32 idx;

    if (fmt16)      /* the s16 emitter checks GX_VA_CLR1's bit (asm
                     * 0x8006A498 extrwi b=19), not CLR0's like f32 */
        col.useVtxColor = (shape->vtxAttrs & (1u << 12)) != 0;

    for (idx = 0; idx < 2; idx++) {
        u32 entries = shape->dispListSizes[idx];

        if (!(shape->dispListFlags & (1u << idx)))
            continue;
        /* NOT the static draw_shape convention: the asm (0x80069EF8,
         * 0x8006A05C) uses lbl_80199374 ^ 2 for list 0 and ^ 1 for list 1,
         * hardware-encoded (list0 = API GX_CULL_FRONT, list1 = GX_CULL_BACK;
         * swapped when fn_80069AD0 sets the mirror mode 3). */
        GXSetCullMode_cached(doubleSided ? CULL_NONE
                                         : avdisp_hw_cull_to_gx(s_avdisp_mode_374
                                                 ^ (idx == 0 ? 2u : 1u)));
        if (fmt16) {
            emit_indexed_s16(stream, entries, vtxBase, shape->vtxAttrs,
                             &col, textured);
            stream += entries * 2;
        } else {
            emit_indexed_f32((const u32 *)stream, entries, vtxBase,
                             shape->vtxAttrs, &col, textured);
            stream += entries * 4;
        }
    }
    return stream;
}

/* asm 0x8006A204 avdisp_draw_deformable_model (all shapes) and 0x8006A2BC
 * g_avdisp_draw_deformable_model2 (opaque only / translucent only). */
static void draw_deformable_model(struct GMAModel *model, u8 *ph,
                                  int drawOpaque, int drawTranslucent)
{
    struct DeformHeader *h = (struct DeformHeader *)ph;
    const u8 *vtxBase = ph + h->vtxArrayOff;
    const u8 *stream = ph + h->idxStreamOff;
    struct GMAShape *shape = (struct GMAShape *)(ph + 0x20);
    int fmt16 = (h->modelFlags & GCMF_16BIT) != 0;
    u32 i;

    for (i = 0; i < model->opaqueShapeCount; i++, shape++) {
        if (drawOpaque)
            stream = draw_deformable_shape(model, shape, vtxBase, stream,
                                           fmt16);
        /* else: the asm quirk at 0x8006A334 - the translucent-only pass
         * skips the opaque shape headers WITHOUT advancing the index
         * stream; mirrored faithfully */
    }
    if (!drawTranslucent)
        return;
    for (i = 0; i < model->translucentShapeCount; i++, shape++)
        stream = draw_deformable_shape(model, shape, vtxBase, stream, fmt16);
}

/* ---- model drawing (asm g_avdisp_draw_model_now1/2/3) ---------------- */

static void draw_model(struct GMAModel *model, int drawOpaque,
                       int drawTranslucent)
{
    struct GMAShape *shape;
    u32 i;

    if (model == 0 || model->magic != 0x47434D46u)  /* "GCMF" host-endian */
        return;
    {
        extern long g_mtx_trace;
        if (g_mtx_trace > 0)
            fprintf(stderr, "[trace] avdisp model=%p flags=%x op=%u tr=%u cb=%p dflag=%x\n",
                    (void *)model, (unsigned)model->flags, (unsigned)model->opaqueShapeCount,
                    (unsigned)model->translucentShapeCount, s_custom_material_cb_peek(),
                    (unsigned)s_avdisp_flag_peek());
        if (g_mtx_trace > 0) { /* trace: model name lookup */
            extern struct GMA *init_common_gma, *stage_gma, *g_bg_gma;
            struct GMA *gl3[3] = { init_common_gma, stage_gma, g_bg_gma };
            int gi; u32 mi;
            for (gi = 0; gi < 3; gi++)
                if (gl3[gi])
                    for (mi = 0; mi < gl3[gi]->numModels; mi++)
                        if (gl3[gi]->modelEntries[mi].model == model) {
                            /* (a local: LLVM 19 cannot select a DPTR load
                             * through a DPTR base inside ?:, see x64.md) */
                            const char *nm = gl3[gi]->modelEntries[mi].name;
                            fprintf(stderr, "[trace]  name gma%d #%u %s\n", gi, (unsigned)mi,
                                    nm ? nm : "?");
                        }
        }
    }

    material_begin_model(model);
    if (s_custom_material_cb == 0) {  /* asm 0x80069B5C-0x80069B70 */
        /* first shape: the deformable shape headers follow a 0x20 header */
        u8 *first = (u8 *)model + model->headerSize;
        if (model->flags & (GCMF_SKIN | GCMF_EFFECTIVE))
            first += 0x20;
        s_material_first = 1;
        material_begin_model_blend((struct GMAShape *)first);
    }
    if (model->flags & (GCMF_SKIN | GCMF_EFFECTIVE)) {
        u8 *ph = (u8 *)model + model->headerSize;

        /* asm now1 0x80069B58: CPU-skin unless flag 0x40 says the cached
         * display list already holds the result (never set on PC).  The
         * skinning writes f32s, so skip SKIN+16BIT models (not in the
         * game data, PC-only guard). */
        if ((model->flags & GCMF_SKIN) && !(model->flags & 0x40) &&
            !(model->flags & GCMF_16BIT))
            skin_transform(ph);
        draw_deformable_model(model, ph, drawOpaque, drawTranslucent);
    } else {
        /* SMB1 ref u_iteratively_multiply_model_matrices (avdisp.c:1132):
         * transformMtx[i] = mtxA * boneMtx[i].  The base mtxA is what the
         * game last loaded into GX slot 0 via load_gx_pos_nrm_mtx. */
        if (model->flags & GCMF_STITCHING) {
            u32 n = model->mtxCount;

            for (i = 0; i < n && i < AVDISP_MTX_SLOTS; i++)
                mtx_concat34(s_gx_slot_mtx[0], s_mtx_slot[i],
                             s_transform_mtx[i]);
            /* asm 0x8006A7D8 (g_iteratively_multiply_model_matrices tail):
             * the MODEL header's own u8 mtxIndices[8] @0x28 bind GX slots
             * 1..8 before any shape is drawn; shapes only override the
             * slots whose index != 0xFF.  Shapes with all-0xFF indices
             * (e.g. the ape's stitched parts) rely on these bindings. */
            stitch_bind_shape_mtxs((const u8 *)model + 0x28);
        }
        shape = (struct GMAShape *)((u8 *)model + model->headerSize);
        for (i = 0; i < model->opaqueShapeCount; i++) {
            if (drawOpaque)
                shape = draw_shape(model, shape);
            else
                shape = next_shape(shape);
        }
        for (i = 0; i < model->translucentShapeCount; i++) {
            if (drawTranslucent)
                shape = draw_shape(model, shape);
            else
                shape = next_shape(shape);
        }
    }

    if (s_multi_tc) {         /* multi-layer material: back to TEX0-only */
        s_multi_tc = 0;
        gx_set_multi_texcoord(0);
    }
    if (s_texgen_touched) {   /* don't leak TEXMTX2 to later draws */
        GXSetTexCoordGen2(0, 1, 4, 60 /* GX_IDENTITY */, 0, 125);
        s_texgen_touched = 0;
    }

    /* asm epilogue: model->flags &= ~0x60 (one-shot draw-hint bits) */
    model->flags &= ~0x60u;
}

void g_avdisp_draw_model_now1(struct GMAModel *model)  /* all shapes */
{
    draw_model(model, 1, 1);
}

void g_avdisp_draw_model_now2(struct GMAModel *model)  /* opaque only */
{
    draw_model(model, 1, 0);
}

void g_avdisp_draw_model_now3(struct GMAModel *model)  /* translucent only */
{
    draw_model(model, 0, 1);
}

/* ---- load_gx_pos_nrm_mtx (asm 0x80066900) ----------------------------
 * GC: writes mtx as XF position matrix id*12 and normal matrix id*9, i.e.
 * GC matrix slot GX_PNMTX0 + id*3.  The decomp calls it before every
 * avdisp draw (avdisp_wrap.c: load_gx_pos_nrm_mtx(mtxa->mtxA, 0)). */
void load_gx_pos_nrm_mtx(Mtx mtx, int id)
{
    GXLoadPosMtxImm(mtx, id * 3);
    GXLoadNrmMtxImm(mtx, id * 3);
    if (id >= 0 && id < 10) {       /* shadow for the stitching CPU path */
        const float *src = &mtx[0][0];
        float *dst = s_gx_slot_mtx[id];
        int j;

        for (j = 0; j < 12; j++)
            dst[j] = src[j];
    }
}

/* ---- small avdisp state (asm 0x80066A58/0x80069860/0x80069AC0/0x80069AD0) -- */

/* lbl_80199320: swap-and-return-old flag (read by asm material path) */
static u32 s_avdisp_flag_320;
u32 fn_80066A58(u32 v)
{
    u32 old = s_avdisp_flag_320;
    s_avdisp_flag_320 = v;
    return old;
}

/* lbl_80190070+0xC: material alpha, multiplied into lit GMA materials */
void fn_80069860(float f) { s_materialAlpha = f; }

/* lbl_80199370: custom material callback, called instead of the default
 * tex-material setup when non-NULL (asm draw_model_now1 0x80069F14). */
void *fn_80069AC0(void *cb)
{
    void *old = s_custom_material_cb;
    s_custom_material_cb = cb;
    return old;
}

/* lbl_80199374 = arg ? 3 : 0 (draw mode compared against 2 in asm) */
static u32 s_avdisp_mode_374;
void fn_80069AD0(u32 on) { s_avdisp_mode_374 = on ? 3 : 0; }

/* ---- avdisp init (asm 0x800694F4) -------------------------------------
 * GC: clears the cached GX settings, resets the post colors, the custom
 * material callback (lbl_80199370), the draw mode (lbl_80199374) and the
 * matrix pool pointers (fn_80069564), then resets the heap/DVD function
 * pointer tables.  On PC only the avdisp-local state exists; the GX
 * caches live in the backend and the function-pointer tables are set
 * right after by the caller (avdisp_wrap.c g_init_something_with_heaps). */
void fn_800694F4(void)
{
    avdisp_set_post_mult_color(1.0f, 1.0f, 1.0f, 1.0f);
    avdisp_set_post_add_color(0.0f, 0.0f, 0.0f, 0.0f);
    s_custom_material_cb = 0;
    s_avdisp_mode_374 = 0;
    mtx_slot_reset();
}

static void *s_custom_material_cb_peek(void) { return s_custom_material_cb; }
static unsigned s_avdisp_flag_peek(void) { return s_avdisp_flag_320; }

/* ---- g_smth_with_gpu_and_locked_cache_mtx (asm 0x8006698C) -------------
 * GC: streams mtxa (locked cache 0xE0000000) into XF position matrix 0 and
 * normal matrix 0.  The ball shell, ape parts and several bg/effect models
 * rely on it to set their modelview before avdisp draws them. */
void g_smth_with_gpu_and_locked_cache_mtx(void)
{
    extern struct MathutilData *mtxa;
    load_gx_pos_nrm_mtx(*(Mtx *)mtxa, 0);
}
