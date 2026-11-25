/* MusyX shim: the game's sound-library API (snd*) on top of the PC "DSP"
 * (include/audio_dsp.h, rendered by src/audio/audio_miniaudio.c).
 *
 * Everything the game can observe (stream callbacks, voice lifetimes, macro
 * timing) is simulated here on the game thread, in fixed 5 ms ticks of game
 * time, so it is deterministic and independent of the audio device.  What is
 * and is not emulated: docs/audio.md. */
#include "shim.h"
#include "audio_dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* game-side types (layouts as in decomp sound.c)                      */

typedef struct
{
    u8 ctrl;
    union
    {
        u8 value7;
        u16 value14;
    } paraData;
} SND_PARAMETER;

typedef struct
{
    u8 numPara;
    SND_PARAMETER *paraArray;
} SND_PARAMETER_INFO;

typedef struct
{
    void *(*malloc)(u32 size);
    void (*free)(void *ptr);
} SND_HOOKS;

/* user: the game passes a struct pointer (smgr.c), so it is pointer-wide. */
typedef u32 (*StreamUpdateFn)(void *, u32, void *, u32, uptr);

void dvd_run_deferred_callbacks(void);

static inline u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

/* ------------------------------------------------------------------ */
/* global state                                                        */

#define MAX_HW_VOICES 48
#define MAX_GROUPS    16
#define MAX_STREAMS   ADSP_MAX_STREAMS
#define NO_VID        0xFFFFFFFFu

static float s_volMaster = 1.0f;     /* sndVolume */
static float s_volMaster2 = 1.0f;    /* sndMasterVolume */
static u32 s_rng = 0x12345678u;
static u32 s_nextVid = 1;
static u32 s_seq;
static int s_inited;

static u32 rng_next(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return s_rng >> 8;
}

static float master_gain(void) { return s_volMaster * s_volMaster2; }

/* sndOutputMode (DOL 0x8003DCDC; smgr calls it with 0 for mono, 2 for
 * stereo - MusyX's "surround" mode, where voices use their surround pan) */
static int s_outputMode = 1;   /* synthInit: stereo, surround pan off */
void fn_8003DCDC(int mode)
{
    if (mode >= 0 && mode <= 2)
        s_outputMode = mode;
}

/* MusyX output volume law, as salCalcVolume (DOL 0x8004C4F8) applies it to
 * every voice and stream (via hwSetVolume, DOL 0x80050B20).  The linear
 * product of all volume factors (macro/envelope volume, MIDI ctrl 7, faders;
 * for streams the stream volume) is looked up, linearly interpolated, in
 * musyx_vol_tab (DOL 0x80113EC8, ~(v/127)^2.2: half volume is -13 dB, not
 * -6 dB).  Pan and surround pan (span) then scale it with the piecewise
 * linear law in musyx_vol_tab[129..132] = { 0, 0.7079, 1, 1 } over the
 * coordinate 0..2 (pan 1..127; 64 = centre = -3 dB per side; span 0 = front):
 *   L = V * T(2 - p) * T(2 - s),  R = V * T(p) * T(2 - s),
 *   S = V * 0.7079 * T(s).
 * S goes to the DSP's separate surround mix, which the AX ucode (CRC
 * 4e8a8b21) does not put into the L/R output, so it is dropped here.
 * (voltab 1, dspDLSVolTab, is only selected by macro command 0x58, which
 * this game's banks do not use.) */
static const u32 s_volTabBits[129] = {
    0x00000000u, 0x380000FDu, 0x3920015Fu, 0x39D001ADu, 0x3A380175u, 0x3A9400FEu, 0x3AD801B4u, 0x3B12011Fu,
    0x3B40018Du, 0x3B7401F0u, 0x3B970123u, 0x3BB70174u, 0x3BDA01BFu, 0x3C008104u, 0x3C158126u, 0x3C2C017Bu,
    0x3C4401B8u, 0x3C5E01A8u, 0x3C7901DFu, 0x3C8B4111u, 0x3C9A8120u, 0x3CAAC13Fu, 0x3CBBC177u, 0x3CCDC18Au,
    0x3CE081B5u, 0x3CF441F1u, 0x3D046108u, 0x3D0F2120u, 0x3D1A412Au, 0x3D25C140u, 0x3D31E159u, 0x3D3E617Fu,
    0x3D4B4197u, 0x3D58A1B7u, 0x3D6681C5u, 0x3D74E1F4u, 0x3D81D0FEu, 0x3D897113u, 0x3D91511Eu, 0x3D99712Du,
    0x3DA1C143u, 0x3DAA514Fu, 0x3DB32160u, 0x3DBC417Fu, 0x3DC5918Au, 0x3DCF2171u, 0x3DD8F1D4u, 0x3DE301A8u,
    0x3DED51F8u, 0x3DF7E1B9u, 0x3E01611Cu, 0x3E06E8F3u, 0x3E0C9928u, 0x3E126916u, 0x3E185943u, 0x3E1E7147u,
    0x3E24A16Au, 0x3E2B0143u, 0x3E31797Du, 0x3E38198Fu, 0x3E3EE17Au, 0x3E45C183u, 0x3E4CD185u, 0x3E53F9A5u,
    0x3E5B51BDu, 0x3E62C9D1u, 0x3E6A69BEu, 0x3E7229E9u, 0x3E7A11EDu, 0x3E811106u, 0x3E852913u, 0x3E89591Du,
    0x3E8D9924u, 0x3E91ED18u, 0x3E965929u, 0x3E9AD539u, 0x3E9F6534u, 0x3EA4093Eu, 0x3EA8C155u, 0x3EAD9168u,
    0x3EB27158u, 0x3EB76966u, 0x3EBC7582u, 0x3EC19589u, 0x3EC6CD8Eu, 0x3ECC19A0u, 0x3ED1799Eu, 0x3ED6F1BBu,
    0x3EDC7DC3u, 0x3EE221C9u, 0x3EE7DDCAu, 0x3EEDADDAu, 0x3EF395E6u, 0x3EF991FFu, 0x3EFFA5F4u, 0x3F02EB0Bu,
    0x3F060B0Bu, 0x3F093911u, 0x3F0C7315u, 0x3F0FB928u, 0x3F130B28u, 0x3F166927u, 0x3F19D335u, 0x3F1D4941u,
    0x3F20CD42u, 0x3F245B4Bu, 0x3F27F951u, 0x3F2BA15Eu, 0x3F2F5760u, 0x3F331B69u, 0x3F36EB70u, 0x3F3AC97Eu,
    0x3F3EB378u, 0x3F42AB8Au, 0x3F46B192u, 0x3F4AC58Fu, 0x3F4EE59Bu, 0x3F5315A5u, 0x3F5751ADu, 0x3F5B9BBBu,
    0x3F5FF5C7u, 0x3F645DC8u, 0x3F68D1D9u, 0x3F6D57DEu, 0x3F71EBEAu, 0x3F768BF4u, 0x3F7B3DF3u, 0x3F800000u,
    0x3F800000u,
};
static const float s_panTab[4] = { 0.0f, 0.70789999f, 1.0f, 1.0f };   /* musyx_vol_tab[129..132] */

static float vol_curve(float v)
{
    union { u32 u; float f; } a, b;
    float x, fr;
    u32 i;
    if (!(v > 0.0f))
        return 0.0f;
    if (v >= 1.0f)
        v = 1.0f;
    x = v * 127.0f;
    i = (u32)x;
    fr = x - (float)i;
    a.u = s_volTabBits[i];
    b.u = s_volTabBits[i + 1];
    return a.f * (1.0f - fr) + b.f * fr;
}

static float pan_curve(float x)   /* x in 0..2 */
{
    u32 i = (u32)x;
    float fr = x - (float)i;
    return s_panTab[i] * (1.0f - fr) + s_panTab[i + 1] * fr;
}

static float pan_coord(int v)   /* 0..127 -> 0..2 (MusyX: (v - 1) / 63, 0 and 1 -> 0) */
{
    if (v > 127)
        v = 127;
    return v > 1 ? (float)(v - 1) / 63.0f : 0.0f;
}

/* vol: linear volume 0..1; pan, span: 0..127 */
static void pan_gains(int pan, int span, float vol, float *gl, float *gr)
{
    float V = vol_curve(vol);
    float p = pan_coord(pan < 0 ? 0 : pan);
    float front = pan_curve(2.0f - pan_coord(span < 0 ? 0 : span));
    *gl = V * pan_curve(2.0f - p) * front;
    *gr = V * pan_curve(p) * front;
}

/* Aux send gains L, R, S for send level a (0..1), in salCalcVolume's
 * operation order for its aux A / aux B outputs (stereo/surround studio):
 *   F = VA * T(2 - s);  L = F * T(2 - p);  R = F * T(p);  S = 0.7079 * (VA * T(s)) */
static void aux_gains(int pan, int span, float a, float *g)
{
    float VA = vol_curve(a);
    float p = pan_coord(pan < 0 ? 0 : pan);
    float s = pan_coord(span < 0 ? 0 : span);
    float F = VA * pan_curve(2.0f - s);
    g[0] = F * pan_curve(2.0f - p);
    g[1] = F * pan_curve(p);
    g[2] = s_panTab[1] * (VA * pan_curve(s));
}

/* ------------------------------------------------------------------ */
/* streams                                                             */

struct Stream
{
    int alloc, active;
    u8 *buffer;
    u32 samples;
    u32 frq;
    u8 vol, pan, span;
    StreamUpdateFn update;
    uptr user;
    s16 coefs[16];
    u64 posFix;      /* 32.32 samples played since activation */
    u64 written;     /* samples provided (ring starts full) */
    int cat;         /* mixer category (ADSP_CAT_*), from pc_snd_stream_file */
};
static struct Stream s_streams[MAX_STREAMS];

static void stream_gains(struct Stream *st, float *gl, float *gr)
{
    pan_gains(st->pan, st->span, (float)st->vol / 127.0f * master_gain(), gl, gr);
}

static void stream_push_params(int id)
{
    struct Stream *st = &s_streams[id];
    float gl, gr;
    if (!st->active)
        return;
    stream_gains(st, &gl, &gr);
    adsp_stream_set(id, adsp_step_for_rate(st->frq), gl, gr);
}

u32 sndStreamAllocEx(u8 prio, void *buffer, u32 samples, u32 frq, u8 vol, u8 pan, u8 span, u8 auxa, u8 auxb,
                     u8 studio, u32 flags, StreamUpdateFn update, uptr user, void *adpcmInfo)
{
    int i;
    (void)prio; (void)auxa; (void)auxb; (void)studio; (void)flags; (void)adpcmInfo;
    for (i = 0; i < MAX_STREAMS; i++) {
        struct Stream *st = &s_streams[i];
        if (st->alloc)
            continue;
        memset(st, 0, sizeof(*st));
        st->alloc = 1;
        st->buffer = buffer;
        st->samples = samples;
        st->frq = frq;
        st->vol = vol;
        st->pan = pan;
        st->span = span > 127 ? 127 : span;
        st->update = update;
        st->user = user;
        st->cat = ADSP_CAT_MUSIC;
        if (samples != ADSP_STREAM_SAMPLES)
            fprintf(stderr, "[snd] sndStreamAllocEx: unexpected ring size %u\n", samples);
        return (u32)i;
    }
    return NO_VID;
}

static struct Stream *stream_get(u32 id)
{
    return (id < MAX_STREAMS && s_streams[id].alloc) ? &s_streams[id] : NULL;
}

void sndStreamFrq(u32 id, u32 frq)
{
    struct Stream *st = stream_get(id);
    if (!st) return;
    st->frq = frq;
    stream_push_params(id);
}

void sndStreamADPCMParameter(u32 id, void *adpcmInfo)
{
    struct Stream *st = stream_get(id);
    if (!st || !adpcmInfo) return;
    /* StreamParams.coefs: already host-endian (sound.c swaps the DSP header) */
    memcpy(st->coefs, adpcmInfo, sizeof(st->coefs));
}

void fn_8003F718(u32 id, u8 vol, u8 pan, u8 span, u8 auxa, u8 auxb)
{
    struct Stream *st = stream_get(id);
    (void)auxa; (void)auxb;
    if (!st) return;
    st->vol = vol > 127 ? 127 : vol;
    st->pan = pan;
    st->span = span > 127 ? 127 : span;
    stream_push_params(id);
}

static void stream_upload(int id, u32 off, u32 len)
{
    struct Stream *st = &s_streams[id];
    u32 byteOff, bytes;
    if (len == 0 || off >= st->samples)
        return;
    if (off + len > st->samples)
        len = st->samples - off;
    byteOff = off / 14 * 8;
    bytes = (len + 13) / 14 * 8;
    if (byteOff + bytes > ADSP_STREAM_BYTES)
        bytes = ADSP_STREAM_BYTES - byteOff;
    adsp_stream_upload(id, byteOff, st->buffer + byteOff, bytes);
}

void sndStreamARAMUpdate(u32 id, u32 off1, u32 len1, u32 off2, u32 len2)
{
    if (!stream_get(id)) return;
    stream_upload(id, off1, len1);
    stream_upload(id, off2, len2);
}

u32 sndStreamActivate(u32 id)
{
    struct Stream *st = stream_get(id);
    float gl, gr;
    if (!st) return 0;
    st->active = 1;
    st->posFix = 0;
    st->written = st->samples;
    stream_gains(st, &gl, &gr);
    adsp_stream_start(id, st->coefs, adsp_step_for_rate(st->frq), gl, gr, st->cat);
    printf("[snd] stream %u start %u Hz vol %u pan %u span %u\n", id, st->frq, st->vol, st->pan, st->span);
    return 1;
}

void sndStreamDeactivate(u32 id)
{
    struct Stream *st = stream_get(id);
    if (!st) return;
    if (st->active) {
        printf("[snd] stream %u stop after %.2f s\n", id,
               (double)(st->posFix >> 32) / (st->frq ? st->frq : 1));
        adsp_stream_stop(id);
    }
    st->active = 0;
}

/* MusyX streamHandle (DOL 0x8003E1E8) only services streams on every 4th
 * synth frame (streamCallCnt/streamCallDelay = 3, set in streamInit), so the
 * update callbacks run at 50 Hz.  smgr.c counts those calls as its music
 * clock (1.2 * calls = 60 fps frames), which sel_ngc uses to cue the start
 * masks on the beat. */
static u8 s_streamCallCnt;
#define STREAM_CALL_DELAY 3

static void streams_tick(void)
{
    int i;
    int service = (s_streamCallCnt == 0);
    if (service)
        s_streamCallCnt = STREAM_CALL_DELAY;
    else
        s_streamCallCnt--;
    for (i = 0; i < MAX_STREAMS; i++) {
        struct Stream *st = &s_streams[i];
        u64 played, freeSmp;
        u32 start, len1, len2, ret;
        if (!st->active)
            continue;
        if (!service) {
            st->posFix += adsp_step_for_rate(st->frq) * ADSP_TICK_FRAMES;
            continue;
        }
        played = st->posFix >> 32;
        freeSmp = played + st->samples - st->written;
        if ((s64)freeSmp < 0)
            freeSmp = 0;
        if (freeSmp > st->samples)
            freeSmp = st->samples;
        start = (u32)(st->written % st->samples);
        len1 = (u32)freeSmp;
        len2 = 0;
        if (start + len1 > st->samples) {
            len2 = start + len1 - st->samples;
            len1 = st->samples - start;
        }
        ret = st->update ? st->update(st->buffer + start / 14 * 8, len1,
                                      len2 ? st->buffer : NULL, len2, st->user) : 0;
        if (!st->active)       /* callback stopped the stream */
            continue;
        if (ret > freeSmp)
            ret = (u32)freeSmp;
        st->written += ret;
        st->posFix += adsp_step_for_rate(st->frq) * ADSP_TICK_FRAMES;
    }
}

/* ------------------------------------------------------------------ */
/* sample groups                                                       */

struct Sample
{
    u16 id;
    u8 basePitch;
    u32 rate;
    s16 *pcm;
    u32 len, loopStart, loopLen;
};

struct FxEnt
{
    u16 fxId, objId;
    u8 prio, maxVoices, vol, pan, key;
};

struct Group
{
    u16 groupId;
    const u8 *pool;
    struct FxEnt *fx;
    int nfx;
    struct Sample *samps;
    int nsamps;
    s16 *pcmBlock;
};

static struct Group s_groups[MAX_GROUPS];
static int s_ngroups;

/* buffers to free on the worker once no voice can reference them */
static void *s_pendingFree[MAX_GROUPS * 2];
static int s_npendingFree;

static int adpcm_decode(const u8 *src, const s16 *coefs, s16 h1, s16 h2, s16 *dst, u32 n)
{
    u32 k;
    int c1 = 0, c2 = 0, shift = 0, y1 = h1, y2 = h2;
    for (k = 0; k < n; k++) {
        u32 frame = k / 14, j = k % 14;
        const u8 *fp = src + frame * 8;
        int nib, s;
        if (j == 0) {
            int p = (fp[0] >> 4) & 7;
            shift = fp[0] & 0xF;
            c1 = coefs[p * 2];
            c2 = coefs[p * 2 + 1];
        }
        nib = fp[1 + j / 2];
        nib = (j & 1) ? (nib & 0xF) : (nib >> 4);
        if (nib >= 8)
            nib -= 16;
        s = (((nib << shift) << 11) + 1024 + c1 * y1 + c2 * y2) >> 11;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        y2 = y1;
        y1 = s;
        dst[k] = (s16)s;
    }
    return 1;
}

int sndPushGroup(void *proj, u16 groupId, void *samp, void *sdir, void *pool)
{
    const u8 *pj = proj, *sd = sdir;
    struct Group *g;
    u32 off, total = 0, at = 0;
    int i, n, matched = 0;

    if (s_ngroups >= MAX_GROUPS || !proj || !sdir || !pool) {
        fprintf(stderr, "[snd] sndPushGroup(%u): rejected\n", groupId);
        return 0;
    }
    g = &s_groups[s_ngroups];
    memset(g, 0, sizeof(*g));
    g->groupId = groupId;
    g->pool = pool;

    /* FX tables: take every SFX group in the project (fx ids are unique
     * within a project; minigame projects carry several groups) */
    for (off = 0, n = 0; be32(pj + off) != 0xFFFFFFFFu && n < 64; n++) {
        u32 end = be32(pj + off);
        u16 gid = be16(pj + off + 4), type = be16(pj + off + 6);
        if (gid == groupId)
            matched = 1;
        if (type == 1) {
            const u8 *pt = pj + be32(pj + off + 8 + 5 * 4);
            int cnt = be16(pt), k;
            g->fx = realloc(g->fx, sizeof(struct FxEnt) * (g->nfx + cnt));
            for (k = 0; k < cnt; k++) {
                const u8 *e = pt + 4 + k * 10;
                struct FxEnt *f = &g->fx[g->nfx++];
                f->fxId = be16(e);
                f->objId = be16(e + 2);
                f->prio = e[4];
                f->maxVoices = e[5];
                f->vol = e[6];
                f->pan = e[7];
                f->key = e[8];
            }
        }
        if (end <= off)
            break;
        off = end;
    }

    /* samples */
    for (n = 0; be16(sd + n * 32) != 0xFFFF && n < 4096; n++)
        total += be32(sd + n * 32 + 16) & 0xFFFFFF;
    g->nsamps = n;
    g->samps = calloc(n ? n : 1, sizeof(struct Sample));
    g->pcmBlock = malloc((total ? total : 1) * sizeof(s16));
    for (i = 0; i < n; i++) {
        const u8 *e = sd + i * 32;
        struct Sample *s = &g->samps[i];
        u32 ns = be32(e + 16);
        const u8 *par = sd + be32(e + 28);
        s16 coefs[16];
        int k;
        s->id = be16(e);
        s->basePitch = e[12];
        s->rate = be16(e + 14);
        s->len = ns & 0xFFFFFF;
        s->loopStart = be32(e + 20);
        s->loopLen = be32(e + 24);
        s->pcm = g->pcmBlock + at;
        at += s->len;
        if ((ns >> 24) != 0) {          /* not DSP-ADPCM: unsupported */
            memset(s->pcm, 0, s->len * sizeof(s16));
            continue;
        }
        for (k = 0; k < 16; k++)
            coefs[k] = (s16)be16(par + 8 + k * 2);
        adpcm_decode((const u8 *)samp + be32(e + 4), coefs, (s16)be16(par + 6), (s16)be16(par + 4),
                     s->pcm, s->len);
        if (s->loopLen && s->loopStart + s->loopLen > s->len)
            s->loopLen = 0;
    }
    printf("[snd] sndPushGroup %u%s: %d fx, %d samples (%.1f s PCM)\n", groupId,
           matched ? "" : " (id not in proj)", g->nfx, n, total / 32000.0);
    s_ngroups++;
    return 1;
}

static struct Sample *group_sample(struct Group *g, u16 id)
{
    int i;
    for (i = 0; i < g->nsamps; i++)
        if (g->samps[i].id == id)
            return &g->samps[i];
    return NULL;
}

static const u8 *group_macro(struct Group *g, u16 id, u32 *ncmds)
{
    const u8 *p = g->pool;
    u32 off = be32(p);   /* macros offset */
    int guard;
    for (guard = 0; guard < 8192; guard++) {
        u32 sz = be32(p + off);
        if (sz == 0xFFFFFFFFu || sz < 8)
            return NULL;
        if (be16(p + off + 4) == id) {
            *ncmds = (sz - 8) / 8;
            return p + off + 8;
        }
        off += sz;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* SFX voices + SoundMacro interpreter                                 */

struct Voice
{
    int active;
    u32 vid, seq;
    struct Group *grp;
    u16 fxId;
    const u8 *cmds;
    u32 ncmds;
    u32 pc;
    int done;
    /* waiting */
    int waiting, waitKeyOff, waitSampleEnd;
    u32 waitTicks;            /* 0xFFFFFFFF = forever */
    int loopPc;
    u32 loopLeft;
    int keyedOff;
    /* params */
    u8 vel;
    int cat;                  /* mixer category (ADSP_CAT_*), from pc_snd_fx_cue */
    int note, detune;
    float vol, volTarget, volStep;
    int volRamp;
    int macroPan;
    u8 ctrl7, ctrl10, ctrl83;   /* volume, pan, surround pan */
    u8 ctrl5B, ctrl5D;          /* reverb (aux A), chorus (aux B) send */
    u16 bend, doppler;
    int bendUp, bendDown;
    /* sample */
    struct Sample *smp;
    int playing, everStarted, ended;
    u64 pos, step;
    u32 simLen, simLoopStart, simLoopLen;
    u64 stepSent;
    float lastGl, lastGr;
    float lastAux[ADSP_AUX_GAINS];
};

static struct Voice s_voices[MAX_HW_VOICES];

static void voice_kill(int i)
{
    struct Voice *v = &s_voices[i];
    if (v->active && v->playing)
        adsp_voice_stop(i);
    v->active = 0;
    v->playing = 0;
}

static u64 voice_step(struct Voice *v)
{
    double semis, ratio;
    if (!v->smp)
        return 0;
    semis = v->note - v->smp->basePitch + v->detune / 100.0;
    if (v->bend >= 0x2000)
        semis += (v->bend - 0x2000) / 8192.0 * v->bendUp;
    else
        semis += (v->bend - 0x2000) / 8192.0 * v->bendDown;
    ratio = pow(2.0, semis / 12.0) * pow(2.0, ((int)v->doppler - 0x2000) / 8192.0);
    return adsp_step_for_rate(v->smp->rate * ratio);
}

static float voice_lin_vol(struct Voice *v)
{
    return v->vol / 127.0f * (v->ctrl7 / 127.0f) * master_gain();
}

static void voice_gains(struct Voice *v, float *gl, float *gr)
{
    float g = voice_lin_vol(v);
    /* synth voice update (DOL 0x8003B9C0): mono forces centre / no span,
     * the span is only used in surround mode */
    if (s_outputMode == 0)
        pan_gains(64, 0, g, gl, gr);
    else
        pan_gains(v->ctrl10 + (v->macroPan - 64), s_outputMode == 2 ? v->ctrl83 : 0, g, gl, gr);
}

/* Aux send levels, synth voice update (DOL 0x8003B9C0) -> hwSetVolume
 * (0x80050B20) -> salCalcVolume (0x8004C4F8).  With the default input
 * mapping (inpInit, 0x8004FCE8) the post-fader reverb input is MIDI ctrl
 * 0x5B and the post-fader aux B input ctrl 0x5D, each read as v << 7
 * (inpGetMidiCtrl, 0x8004EE3C); the pre-fader inputs are unmapped (0) and
 * the voice's reverb scale/offset default to 128/0 (0x80043EAC):
 *   auxA = (1/127) * (128 * ((1/16383) * (vol * (ctrl5B << 7))))
 *   auxB = (1/16383) * (vol * (ctrl5D << 7))
 * hwSetVolume clamps both to <= 1, and salCalcVolume runs them through the
 * same volume table and pan/span law as the main volume (pan/span as for
 * the main output, i.e. centre/front in mono mode). */
static void voice_aux_gains(struct Voice *v, float *g)
{
    const float k127 = 0.007874016f, k16383 = 6.103888e-05f;   /* 0x3C010204, 0x38800200 */
    float vol = voice_lin_vol(v);
    float a = k127 * (128.0f * (k16383 * (vol * (float)(v->ctrl5B << 7))));
    float b = k16383 * (vol * (float)(v->ctrl5D << 7));
    int pan = 64, span = 0;
    if (a >= 1.0f) a = 1.0f;
    if (b >= 1.0f) b = 1.0f;
    if (s_outputMode != 0) {
        pan = v->ctrl10 + (v->macroPan - 64);
        span = s_outputMode == 2 ? v->ctrl83 : 0;
    }
    if (v->ctrl5B)
        aux_gains(pan, span, a, g);
    else
        g[0] = g[1] = g[2] = 0.0f;
    if (v->ctrl5D)
        aux_gains(pan, span, b, g + 3);
    else
        g[3] = g[4] = g[5] = 0.0f;
}

/* push changed aux gains (nothing is sent while a voice has no sends) */
static void voice_push_aux(int i, int force)
{
    struct Voice *v = &s_voices[i];
    float g[ADSP_AUX_GAINS];
    voice_aux_gains(v, g);
    if (force || memcmp(g, v->lastAux, sizeof(g)) != 0) {
        if (force && g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0) {
            memcpy(v->lastAux, g, sizeof(g));
            return;   /* adsp_voice_start already zeroed the DSP's sends */
        }
        adsp_voice_aux(i, g);
        memcpy(v->lastAux, g, sizeof(g));
    }
}

static int vol_from(struct Voice *v, int scale, int add)
{
    int x = v->vel * scale / 127 + add;
    return x < 0 ? 0 : x > 127 ? 127 : x;
}

static void voice_start_sample(int i, u16 sid, u32 offset)
{
    struct Voice *v = &s_voices[i];
    float gl, gr;
    struct Sample *s = group_sample(v->grp, sid);
    int k;
    if (!s) {   /* MusyX sample ids are global: try other groups */
        for (k = s_ngroups - 1; k >= 0 && !s; k--)
            s = group_sample(&s_groups[k], sid);
    }
    if (!s || s->len == 0) {
        v->ended = 1;
        return;
    }
    if (offset >= s->len)
        offset = 0;
    if (v->playing)
        adsp_voice_stop(i);   /* restart: the new start below replaces it */
    v->smp = s;
    v->simLen = s->len - offset;
    v->simLoopLen = (s->loopLen && s->loopStart >= offset) ? s->loopLen : 0;
    v->simLoopStart = v->simLoopLen ? s->loopStart - offset : 0;
    v->pos = 0;   /* relative to the start pointer handed to the DSP */
    v->step = voice_step(v);
    v->playing = 1;
    v->everStarted = 1;
    v->ended = 0;
    voice_gains(v, &gl, &gr);
    v->lastGl = gl;
    v->lastGr = gr;
    v->stepSent = v->step;
    adsp_voice_start(i, s->pcm + offset, v->simLen, v->simLoopStart, v->simLoopLen, v->step, gl, gr, v->cat);
    voice_push_aux(i, 1);
}

static void voice_set_ramp(struct Voice *v, int target, u32 ms)
{
    u32 ticks = (ms + ADSP_TICK_FRAMES * 1000 / ADSP_RATE - 1) / (ADSP_TICK_FRAMES * 1000 / ADSP_RATE);
    v->volTarget = (float)target;
    if (ticks == 0) {
        v->vol = (float)target;
        v->volRamp = 0;
    } else {
        v->volStep = (v->volTarget - v->vol) / (float)ticks;
        v->volRamp = (int)ticks;
    }
}

static int voice_alloc(void);
static void voice_init_from(int i, struct Voice *src, const u8 *cmds, u32 ncmds, u32 pc, int note);

/* Run macro commands until the voice waits or ends. */
static void voice_run(int i)
{
    struct Voice *v = &s_voices[i];
    int budget = 64;

    while (v->active && !v->done && !v->waiting && budget-- > 0) {
        const u8 *c;
        u32 w0, w1;
        int b1, b2, b3;
        if (v->pc >= v->ncmds) {
            v->done = 1;
            break;
        }
        c = v->cmds + v->pc * 8;
        w0 = be32(c);
        w1 = be32(c + 4);
        b1 = (w0 >> 8) & 0xFF;
        b2 = (w0 >> 16) & 0xFF;
        b3 = (w0 >> 24) & 0xFF;
        v->pc++;
        switch (w0 & 0x7F) {
        case 0x00:   /* END */
        case 0x01:   /* STOP */
            v->done = 1;
            break;
        case 0x05: { /* LOOP */
            u32 step = w1 & 0xFFFF, times = w1 >> 16;
            if ((b1 && v->keyedOff) || (b3 && v->ended))
                break;
            /* an endless loop with no exit condition after key-off would
             * never end (MusyX would cut it by voice release): fall out */
            if (times == 0xFFFF && v->keyedOff)
                break;
            if (times == 0xFFFF) {
                v->pc = step;
                break;
            }
            if (v->loopPc != (int)v->pc) {
                v->loopPc = (int)v->pc;
                v->loopLeft = times;
            }
            if (v->loopLeft > 0) {
                v->loopLeft--;
                v->pc = step;
            } else
                v->loopPc = -1;
            break;
        }
        case 0x06:   /* GOTO (treated as a jump within the same macro) */
            v->pc = w1 & 0xFFFF;
            break;
        case 0x07: { /* WAIT_MS */
            u32 ms = w1 >> 16;
            v->waitKeyOff = b1 != 0;
            v->waitSampleEnd = b3 != 0;
            if (b2 && ms != 0xFFFF && ms)   /* random wait 0..ms */
                ms = rng_next() % (ms + 1);
            v->waitTicks = (ms == 0xFFFF) ? 0xFFFFFFFFu : (ms + 4) / 5;
            if (v->waitKeyOff && v->keyedOff)
                break;
            if (v->waitSampleEnd && v->ended)
                break;
            if (v->waitTicks == 0)
                break;
            v->waiting = 1;
            break;
        }
        case 0x08: { /* PLAYMACRO */
            u32 nc;
            const u8 *mc = group_macro(v->grp, (u16)(w0 >> 16), &nc);
            int k;
            if (mc && (k = voice_alloc()) >= 0 && k != i)
                voice_init_from(k, v, mc, nc, w1 & 0xFFFF, v->note + (s8)b1);
            break;
        }
        case 0x0D:   /* SCALEVOLUME */
            v->vol = (float)vol_from(v, b1, (s8)b2);
            v->volRamp = 0;
            break;
        case 0x0E:   /* PANNING */
            v->macroPan = b1;
            break;
        case 0x0F:   /* ENVELOPE */
            voice_set_ramp(v, vol_from(v, b1, (s8)b2), w1 >> 16);
            break;
        case 0x10:   /* STARTSAMPLE */
            voice_start_sample(i, (u16)((w0 >> 8) & 0xFFFF), w1);
            break;
        case 0x11:   /* STOPSAMPLE */
            if (v->playing)
                adsp_voice_stop(i);
            v->playing = 0;
            break;
        case 0x12:   /* KEYOFF */
            v->keyedOff = 1;
            break;
        case 0x14:   /* FADE_IN */
            v->vol = 0;
            voice_set_ramp(v, vol_from(v, b1, (s8)b2), w1 >> 16);
            break;
        case 0x17: { /* RNDNOTE */
            int lo = b1, hi = b3, r;
            if (hi < lo) { int t = lo; lo = hi; hi = t; }
            r = lo + (int)(rng_next() % (u32)(hi - lo + 1));
            v->note = ((w1 >> 8) & 0xFF) ? v->note + r : r;
            v->detune = (s8)b2;
            break;
        }
        case 0x18:   /* ADDNOTE */
            v->note += (s8)b1;
            v->detune = (s8)b2;
            break;
        case 0x19:   /* SETNOTE */
            v->note = b1;
            v->detune = (s8)b2;
            break;
        case 0x33:   /* PITCHWHEELR */
            if (b1 <= 24) v->bendUp = b1;
            if (b2 <= 24) v->bendDown = b2;
            break;
        default:     /* 0x0C ADSR, 0x1C vibrato, 0x30/0x31 age, 0x38 ... */
            break;
        }
        /* note/bend changes take effect on the playing sample */
        if (v->playing && v->smp)
            v->step = voice_step(v);
    }
    if (v->done) {
        voice_kill(i);
    }
}

static void voice_init_from(int i, struct Voice *src, const u8 *cmds, u32 ncmds, u32 pc, int note)
{
    struct Voice *v = &s_voices[i];
    memset(v, 0, sizeof(*v));
    v->active = 1;
    v->vid = src->vid;
    v->seq = ++s_seq;
    v->grp = src->grp;
    v->fxId = src->fxId;
    v->cmds = cmds;
    v->ncmds = ncmds;
    v->pc = pc;
    v->loopPc = -1;
    v->vel = src->vel;
    v->cat = src->cat;
    v->note = note;
    v->vol = src->vel;
    v->macroPan = 64;
    v->ctrl7 = src->ctrl7;
    v->ctrl10 = src->ctrl10;
    v->ctrl83 = src->ctrl83;
    v->ctrl5B = src->ctrl5B;
    v->ctrl5D = src->ctrl5D;
    v->bend = src->bend;
    v->doppler = src->doppler;
    v->bendUp = v->bendDown = 2;
    voice_run(i);
}

static int voice_alloc(void)
{
    int i, oldest = -1;
    for (i = 0; i < MAX_HW_VOICES; i++) {
        if (!s_voices[i].active)
            return i;
        if (oldest < 0 || s_voices[i].seq < s_voices[oldest].seq)
            oldest = i;
    }
    voice_kill(oldest);
    return oldest;
}

static void voices_tick(void)
{
    int i;
    for (i = 0; i < MAX_HW_VOICES; i++) {
        struct Voice *v = &s_voices[i];
        float gl, gr;
        if (!v->active)
            continue;
        if (v->waiting) {
            if ((v->waitKeyOff && v->keyedOff) || (v->waitSampleEnd && v->ended))
                v->waiting = 0;
            else if (v->waitTicks != 0xFFFFFFFFu && --v->waitTicks == 0)
                v->waiting = 0;
        }
        if (!v->waiting)
            voice_run(i);
        if (!v->active)
            continue;
        if (v->volRamp > 0) {
            v->vol += v->volStep;
            if (--v->volRamp == 0)
                v->vol = v->volTarget;
        }
        if (v->playing) {
            voice_gains(v, &gl, &gr);
            if (gl != v->lastGl || gr != v->lastGr || v->step != v->stepSent) {
                adsp_voice_set(i, v->step, gl, gr);
                v->lastGl = gl;
                v->lastGr = gr;
                v->stepSent = v->step;
            }
            voice_push_aux(i, 0);
            /* mirror the DSP's sample position to know when it ends */
            v->pos += v->step * ADSP_TICK_FRAMES;
            if (v->simLoopLen) {
                u64 le = (u64)(v->simLoopStart + v->simLoopLen) << 32;
                while (v->pos >= le)
                    v->pos -= (u64)v->simLoopLen << 32;
            } else if ((v->pos >> 32) >= v->simLen) {
                v->playing = 0;
                v->ended = 1;
            }
        }
    }
}

static void voices_kill_group(struct Group *g)
{
    int i;
    for (i = 0; i < MAX_HW_VOICES; i++)
        if (s_voices[i].active && s_voices[i].grp == g)
            voice_kill(i);
}

int sndPopGroup(void)
{
    struct Group *g;
    if (s_ngroups <= 0)
        return 0;
    g = &s_groups[--s_ngroups];
    voices_kill_group(g);
    /* the DSP may still read the PCM during the current tick: free it on
     * the worker after the next tick (see musyx_frame) */
    if (s_npendingFree < (int)(sizeof(s_pendingFree) / sizeof(s_pendingFree[0])))
        s_pendingFree[s_npendingFree++] = g->pcmBlock;
    free(g->samps);
    free(g->fx);
    printf("[snd] sndPopGroup %u\n", g->groupId);
    memset(g, 0, sizeof(*g));
    return 1;
}

static u32 s_fxLogCount;

/* Mixer category of the next sndFXStartParaInfo (pc_snd_fx_cue, set by
 * sound.c from the game's sound table right before the call); anything
 * started without one (baseball, ...) is a sound effect. */
static int s_nextCat = ADSP_CAT_SFX;

void pc_snd_set_next_category(int cat)
{
    s_nextCat = (cat >= 0 && cat < ADSP_CATS) ? cat : ADSP_CAT_SFX;
}

void pc_snd_stream_category(u32 id, int cat)
{
    if (id < MAX_STREAMS)
        s_streams[id].cat = (cat >= 0 && cat < ADSP_CATS) ? cat : ADSP_CAT_MUSIC;
}

u32 sndFXStartParaInfo(u16 fid, u8 vol, u8 pan, u8 studio, SND_PARAMETER_INFO *info)
{
    struct FxEnt *fx = NULL;
    struct Group *g = NULL;
    const u8 *cmds;
    u32 ncmds, vid;
    int gi, k, i, count, oldest;
    struct Voice proto;
    int cat = s_nextCat;
    (void)studio;

    s_nextCat = ADSP_CAT_SFX;

    for (gi = s_ngroups - 1; gi >= 0 && !fx; gi--)
        for (k = 0; k < s_groups[gi].nfx; k++)
            if (s_groups[gi].fx[k].fxId == fid) {
                fx = &s_groups[gi].fx[k];
                g = &s_groups[gi];
                break;
            }
    if (!fx) {
        if (s_fxLogCount++ < 2000)
            printf("[snd-fx] start fx 0x%x: not in any pushed group\n", fid);
        return NO_VID;
    }
    if (fx->objId & 0xC000) {   /* layer / keymap: not supported */
        if (s_fxLogCount++ < 2000)
            printf("[snd-fx] start fx 0x%x: %s 0x%x unsupported\n", fid,
                   (fx->objId & 0x8000) ? "layer" : "keymap", fx->objId & 0x3FFF);
        return NO_VID;
    }
    cmds = group_macro(g, fx->objId, &ncmds);
    if (!cmds) {
        if (s_fxLogCount++ < 2000)
            printf("[snd-fx] start fx 0x%x: macro 0x%x missing\n", fid, fx->objId);
        return NO_VID;
    }

    /* per-fx voice limit: steal that fx's oldest voice */
    count = 0;
    oldest = -1;
    for (i = 0; i < MAX_HW_VOICES; i++) {
        if (s_voices[i].active && s_voices[i].fxId == fid && s_voices[i].grp == g) {
            count++;
            if (oldest < 0 || s_voices[i].seq < s_voices[oldest].seq)
                oldest = i;
        }
    }
    if (fx->maxVoices && count >= fx->maxVoices && oldest >= 0) {
        u32 ov = s_voices[oldest].vid;
        for (i = 0; i < MAX_HW_VOICES; i++)
            if (s_voices[i].active && s_voices[i].vid == ov)
                voice_kill(i);
    }

    vid = s_nextVid++;
    if (s_nextVid == NO_VID)
        s_nextVid = 1;

    memset(&proto, 0, sizeof(proto));
    proto.vid = vid;
    proto.grp = g;
    proto.fxId = fid;
    proto.cat = cat;
    proto.vel = (vol == 0xFF) ? fx->vol : vol;
    proto.ctrl7 = 127;
    proto.ctrl10 = (pan == 0xFF) ? fx->pan : pan;
    proto.bend = 0x2000;
    proto.doppler = 0x2000;
    if (info && info->paraArray) {
        for (k = 0; k < info->numPara; k++) {
            SND_PARAMETER *p = &info->paraArray[k];
            switch (p->ctrl) {
            case 7: proto.ctrl7 = p->paraData.value7; break;
            case 10: proto.ctrl10 = p->paraData.value7; break;
            case 0x83: proto.ctrl83 = p->paraData.value7 > 127 ? 127 : p->paraData.value7; break;
            case 0x5B: proto.ctrl5B = p->paraData.value7 > 127 ? 127 : p->paraData.value7; break;
            case 0x5D: proto.ctrl5D = p->paraData.value7 > 127 ? 127 : p->paraData.value7; break;
            case 0x80: proto.bend = p->paraData.value14 & 0x3FFF; break;
            case 0x84: proto.doppler = p->paraData.value14 & 0x3FFF; break;
            default: break;
            }
        }
    }
    i = voice_alloc();
    voice_init_from(i, &proto, cmds, ncmds, 0, fx->key);
    if (s_fxLogCount++ < 2000)
        printf("[snd-fx] start fx 0x%x macro 0x%x vel %u pan %u span %u rev %u cho %u -> vid %u\n", fid,
               fx->objId, proto.vel, proto.ctrl10, proto.ctrl83, proto.ctrl5B, proto.ctrl5D, vid);
    return vid;
}

u32 sndFXCheck(u32 vid)
{
    int i;
    if (vid == NO_VID)
        return NO_VID;
    for (i = 0; i < MAX_HW_VOICES; i++)
        if (s_voices[i].active && s_voices[i].vid == vid)
            return vid;
    return NO_VID;
}

int sndFXKeyOff(u32 vid)
{
    int i, found = 0;
    if (vid == NO_VID)
        return 0;
    for (i = 0; i < MAX_HW_VOICES; i++)
        if (s_voices[i].active && s_voices[i].vid == vid) {
            s_voices[i].keyedOff = 1;
            found = 1;
        }
    return found;
}

static void fx_set_ctrl(u32 vid, u8 ctrl, u16 value)
{
    int i;
    if (vid == NO_VID)
        return;
    for (i = 0; i < MAX_HW_VOICES; i++) {
        struct Voice *v = &s_voices[i];
        if (!v->active || v->vid != vid)
            continue;
        switch (ctrl) {
        case 7: v->ctrl7 = value > 127 ? 127 : (u8)value; break;
        case 10: v->ctrl10 = value > 127 ? 127 : (u8)value; break;
        case 0x83: v->ctrl83 = value > 127 ? 127 : (u8)value; break;
        case 0x5B: v->ctrl5B = value > 127 ? 127 : (u8)value; break;
        case 0x5D: v->ctrl5D = value > 127 ? 127 : (u8)value; break;
        case 0x80: v->bend = value & 0x3FFF; break;
        case 0x84: v->doppler = value & 0x3FFF; break;
        default: break;   /* mod etc.: ignored */
        }
        if (v->playing && v->smp)
            v->step = voice_step(v);
    }
}

void g_call_synthFXSetCtrl_with_irq_disabled(u32 vid, u8 ctrl, u8 value)
{
    fx_set_ctrl(vid, ctrl, value);
}

void g_call_synthFXSetCtrl14_with_irq_disabled(u32 vid, u8 ctrl, u16 value)
{
    fx_set_ctrl(vid, ctrl, value);
}

/* ------------------------------------------------------------------ */
/* init / volume / aux                                                 */

void sndSetHooks(SND_HOOKS *hooks) { (void)hooks; }

int sndInit(u8 voices, u8 music, u8 sfx, u8 studios, u32 flags, u32 aramSize)
{
    (void)voices; (void)music; (void)sfx; (void)studios; (void)flags; (void)aramSize;
    s_inited = 1;
    printf("[snd] sndInit (%u voices)\n", voices);
    return 0;
}

static void refresh_all_gains(void)
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++)
        stream_push_params(i);
    /* voices pick the new master up on their next tick */
}

void sndVolume(u8 volume, u16 time, u8 volGroup)
{
    (void)time; (void)volGroup;
    s_volMaster = (volume > 127 ? 127 : volume) / 127.0f;
    refresh_all_gains();
}

void sndMasterVolume(u8 volume, u16 time, u8 music, u8 fx)
{
    (void)time; (void)music; (void)fx;
    s_volMaster2 = (volume > 127 ? 127 : volume) / 127.0f;
    refresh_all_gains();
}

/* aux effects + sndSetAuxProcessingCallbacks: musyx_fx.c */

/* ------------------------------------------------------------------ */
/* per-frame service (video_finish_frame)                              */

void musyx_frame(void)
{
    static u32 frames, ticksDone;
    u32 target, n;

    dvd_run_deferred_callbacks();
    frames++;
    target = (u32)((u64)frames * 10 / 3);   /* 200 ticks/s at 60 fps */
    for (n = ticksDone; n < target; n++) {
        if (s_inited) {
            streams_tick();
            voices_tick();
        }
        adsp_tick();
        while (s_npendingFree > 0)
            adsp_free(s_pendingFree[--s_npendingFree]);
    }
    ticksDone = target;
    adsp_flush();
}

/* MusyX sndFXStartEx (DOL 0x8003DA48): sndFXStartParaInfo without params */
u32 sndFXStartEx(u16 fid, u8 vol, u8 pan, u8 studio)
{
    return sndFXStartParaInfo(fid, vol, pan, studio, NULL);
}
