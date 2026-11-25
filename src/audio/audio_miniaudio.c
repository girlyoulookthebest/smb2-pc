/* miniaudio backend.
 *
 * SMB2_LINK_GAME build: the "DSP" side of the audio path (include/audio_dsp.h,
 * docs/audio.md).  A worker thread renders the queued commands tick by tick
 * into a FIFO that the miniaudio device callback drains, and optionally into
 * a WAV file (--audio-wav).  Without SMB2_LINK_GAME a standalone placeholder
 * backend (no game linked) is built instead. */
#ifdef SMB2_LINK_GAME

#include "audio.h"
#include "audio_dsp.h"
#define MA_IMPLEMENTATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#include <miniaudio.h>
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern const char *g_audio_wav_arg;
extern int g_audio_null_arg;

/* ------------------------------------------------------------------ */
/* command queue (game thread -> worker), single producer/consumer    */

enum {
    CMD_WRAP = 1, CMD_VSTART, CMD_VSET, CMD_VSTOP, CMD_SUPLOAD, CMD_SSTART,
    CMD_SSET, CMD_SSTOP, CMD_FREE, CMD_TICK, CMD_VAUX, CMD_AUXSET, CMD_RUN
};

typedef struct { uint32_t type, size; int32_t idx, pad; } CmdHdr;
typedef struct { const int16_t *pcm; uint32_t len, loopStart, loopLen; uint64_t step; float gl, gr; int32_t cat, pad; } CmdVStart;
typedef struct { uint64_t step; float gl, gr; } CmdSet;
typedef struct { uint32_t off, bytes; } CmdUpload;  /* + data */
typedef struct { int16_t coefs[16]; uint64_t step; float gl, gr; int32_t cat, pad; } CmdSStart;
typedef struct { float g[ADSP_AUX_GAINS]; } CmdVAux;
typedef struct { adsp_aux_fn fn[2]; void *user[2]; } CmdAuxSet;
typedef struct { adsp_fn fn; uint32_t bytes, pad; } CmdRun;   /* + arg bytes */

#define CMDQ_SIZE (16u << 20)
static uint8_t *s_q;
static uint32_t s_qHead;                 /* producer-local write position */
static atomic_uint s_qPub;               /* published write position */
static atomic_uint s_qTail;              /* consumer read position */
static unsigned s_qDropped;

static void *q_reserve(uint32_t type, int idx, uint32_t payload)
{
    uint32_t need = (uint32_t)((sizeof(CmdHdr) + payload + 7) & ~7u);
    uint32_t tail, used;
    CmdHdr *h;

    if (!s_q)
        return NULL;
    tail = atomic_load_explicit(&s_qTail, memory_order_acquire);
    used = s_qHead - tail;
    /* contiguous room; wrap with a marker when the end is too short */
    if ((s_qHead % CMDQ_SIZE) + need > CMDQ_SIZE) {
        uint32_t skip = CMDQ_SIZE - (s_qHead % CMDQ_SIZE);
        if (used + skip + need > CMDQ_SIZE)
            goto full;
        h = (CmdHdr *)(s_q + (s_qHead % CMDQ_SIZE));
        h->type = CMD_WRAP;
        h->size = skip;
        s_qHead += skip;
        used += skip;
    }
    if (used + need > CMDQ_SIZE)
        goto full;
    h = (CmdHdr *)(s_q + (s_qHead % CMDQ_SIZE));
    h->type = type;
    h->size = need;
    h->idx = idx;
    s_qHead += need;
    return h + 1;
full:
    if (s_qDropped++ % 1000 == 0)
        fprintf(stderr, "[audio] command queue full - dropped %u\n", s_qDropped);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* worker state                                                        */

typedef struct {
    int on, stopping;
    const int16_t *pcm;
    uint32_t len, loopStart, loopLen;
    uint64_t pos, step;
    float gl, gr, cl, cr;
    float ga[ADSP_AUX_GAINS], ca[ADSP_AUX_GAINS];   /* aux send target / current */
    int auxFresh;                                    /* started this tick */
    int cat;                                         /* ADSP_CAT_* */
} DVoice;

typedef struct {
    int on, stopping;
    uint8_t ring[ADSP_STREAM_BYTES];
    int16_t coefs[16];
    uint64_t pos, step;
    uint32_t decIdx;
    int32_t h1, h2, shift, c1, c2;
    int16_t s0, s1;
    float gl, gr, cl, cr;
    int cat;                                         /* ADSP_CAT_* */
} DStream;

static DVoice  s_voices[ADSP_MAX_VOICES];
static DStream s_streams[ADSP_MAX_STREAMS];
static atomic_uint s_masterBits;

/* Mixer (docs/audio.md "Mixer"): user volume per category, set from the game
 * thread, ramped over one tick on the worker.  Per-category output stats
 * (after the category volume, before master and limiter) for the summary
 * printed at exit. */
static atomic_uint s_catBits[ADSP_CATS] = { 0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u };
static float s_catCur[ADSP_CATS] = { 1.0f, 1.0f, 1.0f, 1.0f };
static float s_catPeak[ADSP_CATS];
static double s_catSq[ADSP_CATS];
/* SMB2_MIXLOG=1: the same stats per second of output ("[mixlog]" lines) */
static int s_mixLog = -1;
static float s_winPeak[ADSP_CATS];
static double s_winSq[ADSP_CATS];
static const char *const s_catNames[ADSP_CATS] = { "sfx", "music", "voice", "timer" };

/* MusyX aux buses (studio 0): callbacks, the DSP's triple-buffered s32
 * L/R/S aux buffers, the 32 kHz send mix of the current tick and the
 * 32 -> 48 kHz return interpolator state. */
static adsp_aux_fn s_auxFn[2];
static void *s_auxUser[2];
static int32_t s_auxBuf[2][3][3][ADSP_AUX_FRAMES];
static float s_auxBus[2][3][ADSP_AUX_FRAMES];
static float s_auxPrev[2][2];
static unsigned s_auxFrame;

/* output FIFO (worker -> device), interleaved stereo float */
#define OUT_FRAMES 16384u
static float s_out[OUT_FRAMES * 2];
static atomic_uint s_outW, s_outR;
static int s_outPrimed;
static unsigned s_outDroppedTicks, s_outUnderruns;

/* WAV dump */
static FILE *s_wav;
static uint32_t s_wavFrames;

/* stats */
static uint64_t s_renderedFrames, s_clipped;
static float s_peak;                    /* pre-limiter mix peak */
static float s_limMin = 1.0f;           /* deepest limiter gain */
static unsigned s_limTicks;             /* ticks with the limiter engaged */

static SDL_Thread *s_thread;
static SDL_sem *s_sem;
static atomic_int s_quit;

static ma_context s_ctx;
static ma_device s_dev;
static int s_ctxOk, s_devOk;
static int s_inited;

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t dataBytes = frames * 4;
    uint8_t h[44];
    uint32_t v;
#define P32(o, x) (v = (x), h[o] = v, h[o+1] = v >> 8, h[o+2] = v >> 16, h[o+3] = v >> 24)
#define P16(o, x) (h[o] = (x) & 0xFF, h[o+1] = ((x) >> 8) & 0xFF)
    memcpy(h, "RIFF", 4); P32(4, 36 + dataBytes); memcpy(h + 8, "WAVEfmt ", 8);
    P32(16, 16); P16(20, 1); P16(22, 2); P32(24, ADSP_RATE); P32(28, ADSP_RATE * 4);
    P16(32, 4); P16(34, 16); memcpy(h + 36, "data", 4); P32(40, dataBytes);
#undef P32
#undef P16
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

static inline int16_t stream_decode(DStream *st)
{
    uint32_t k = st->decIdx % ADSP_STREAM_SAMPLES;
    uint32_t frame = k / 14, j = k % 14;
    const uint8_t *fp = st->ring + frame * 8;
    int32_t nib, s;

    if (j == 0) {
        int p = (fp[0] >> 4) & 7;
        st->shift = fp[0] & 0xF;
        st->c1 = st->coefs[p * 2];
        st->c2 = st->coefs[p * 2 + 1];
    }
    nib = fp[1 + j / 2];
    nib = (j & 1) ? (nib & 0xF) : (nib >> 4);
    if (nib >= 8)
        nib -= 16;
    s = (((nib << st->shift) << 11) + 1024 + st->c1 * st->h1 + st->c2 * st->h2) >> 11;
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    st->h2 = st->h1;
    st->h1 = s;
    st->decIdx++;
    return (int16_t)s;
}

/* One voice's aux sends for this tick, rendered at the DSP's 32 kHz from
 * the same source position and linear interpolation as the main pass (a
 * copy of the state: the main pass below advances the voice). */
static void render_voice_aux(DVoice *vo, float catGain)
{
    float d[ADSP_AUX_GAINS], c[ADSP_AUX_GAINS];
    uint64_t pos = vo->pos, step = vo->step + (vo->step >> 1);   /* 48k step * 1.5 */
    int k, j, any = 0;

    if (vo->auxFresh)
        memcpy(vo->ca, vo->ga, sizeof(vo->ca));
    vo->auxFresh = 0;
    for (k = 0; k < ADSP_AUX_GAINS; k++) {
        if (!s_auxFn[k / 3])
            continue;
        if (vo->ga[k] != 0.0f || vo->ca[k] != 0.0f)
            any = 1;
    }
    if (!any) {
        memcpy(vo->ca, vo->ga, sizeof(vo->ca));
        return;
    }
    for (k = 0; k < ADSP_AUX_GAINS; k++) {
        d[k] = (vo->ga[k] - vo->ca[k]) / ADSP_AUX_FRAMES;
        c[k] = vo->ca[k];
    }
    for (j = 0; j < ADSP_AUX_FRAMES; j++) {
        uint32_t ip = (uint32_t)(pos >> 32), ip1;
        float frac, s;
        if (vo->loopLen) {
            uint32_t le = vo->loopStart + vo->loopLen;
            while (ip >= le) {
                pos -= (uint64_t)vo->loopLen << 32;
                ip -= vo->loopLen;
            }
            ip1 = (ip + 1 >= le) ? vo->loopStart : ip + 1;
        } else {
            if (ip >= vo->len)
                break;
            ip1 = ip + 1;
        }
        frac = (float)(uint32_t)pos * (1.0f / 4294967296.0f);
        s = vo->pcm[ip] + ((ip1 < vo->len ? vo->pcm[ip1] : 0) - vo->pcm[ip]) * frac;
        s *= catGain * (1.0f / 32768.0f);
        for (k = 0; k < ADSP_AUX_GAINS; k++) {
            c[k] += d[k];
            s_auxBus[k / 3][k % 3][j] += s * c[k];
        }
        pos += step;
    }
    memcpy(vo->ca, vo->ga, sizeof(vo->ca));
}

/* salHandleAuxProcessing + the DSP's aux mix (AX commands 4/5 as built by
 * salBuildCommandList, DOL 0x8004B418): in DSP frame n the aux sends are
 * written to buffer n % 3 and buffer (n + 1) % 3 is added back into the
 * main L/R/S; at the next interrupt the CPU runs the callback on buffer
 * n % 3 (salHandleAuxProcessing takes (frame + 2) % 3 before the frame
 * counter advances).  So a send comes back processed two frames (10 ms)
 * later, at unity gain.  A bus whose callback is NULL is skipped entirely
 * (no write, no read, no processing), its buffers keep their contents.
 * S is processed but, like the main S bus, not part of the L/R output. */
static void aux_process(float *mix)
{
    int b, c, j, i;
    unsigned w = s_auxFrame % 3, r = (s_auxFrame + 1) % 3;

    for (b = 0; b < 2; b++) {
        ADSP_AUX_INFO info;
        if (!s_auxFn[b])
            continue;
        /* send mix -> s32 at 16-bit scale (AX: (sample * vol) >> 15, floor) */
        for (c = 0; c < 3; c++)
            for (j = 0; j < ADSP_AUX_FRAMES; j++) {
                float x = s_auxBus[b][c][j] * 32768.0f;
                int32_t t;
                if (x >= 2147483648.0f) t = INT32_MAX;
                else if (x <= -2147483648.0f) t = INT32_MIN;
                else {
                    t = (int32_t)x;
                    if ((float)t > x)
                        t--;
                }
                s_auxBuf[b][w][c][j] = t;
            }
        /* return of the buffer processed two frames ago, 32 -> 48 kHz
         * (linear, one 32 kHz sample of delay) */
        for (c = 0; c < 2; c++) {
            const int32_t *x = s_auxBuf[b][r][c];
            float prev = s_auxPrev[b][c];
            for (i = 0; i < ADSP_TICK_FRAMES; i++) {
                int jj = i * 2 / 3, ph = i * 2 % 3;
                float x0 = jj ? (float)x[jj - 1] : prev, x1 = (float)x[jj], v;
                v = ph == 0 ? x0 : ph == 1 ? x0 + (x1 - x0) * (1.0f / 3.0f) : x0 + (x1 - x0) * (2.0f / 3.0f);
                mix[i * 2 + c] += v * (1.0f / 32768.0f);
            }
            s_auxPrev[b][c] = (float)x[ADSP_AUX_FRAMES - 1];
        }
        /* the "interrupt": process this frame's buffer in place */
        info.left = s_auxBuf[b][w][0];
        info.right = s_auxBuf[b][w][1];
        info.surround = s_auxBuf[b][w][2];
        s_auxFn[b](ADSP_AUX_REASON_BUFFERUPDATE, &info, s_auxUser[b]);
    }
    memset(s_auxBus, 0, sizeof(s_auxBus));
    s_auxFrame++;
}

static void render_tick(void)
{
    float mix[ADSP_TICK_FRAMES * 2];
    float cmix[ADSP_CATS][ADSP_TICK_FRAMES * 2];   /* per-category sub-mixes */
    float cg[ADSP_CATS][ADSP_TICK_FRAMES];         /* category gain ramps */
    int16_t pcm16[ADSP_TICK_FRAMES * 2];
    float master;
    uint32_t bits;
    int i, n, c;

    memset(mix, 0, sizeof(mix));
    memset(cmix, 0, sizeof(cmix));
    for (c = 0; c < ADSP_CATS; c++) {
        float target, d;
        bits = atomic_load_explicit(&s_catBits[c], memory_order_relaxed);
        memcpy(&target, &bits, 4);
        d = (target - s_catCur[c]) / ADSP_TICK_FRAMES;
        for (i = 0; i < ADSP_TICK_FRAMES; i++)
            cg[c][i] = s_catCur[c] + d * (i + 1);
        s_catCur[c] = target;
    }

    for (n = 0; n < ADSP_MAX_STREAMS; n++) {
        DStream *st = &s_streams[n];
        float dl, dr, *out, *g;
        if (!st->on)
            continue;
        out = cmix[st->cat];
        g = cg[st->cat];
        dl = (st->gl - st->cl) / ADSP_TICK_FRAMES;
        dr = (st->gr - st->cr) / ADSP_TICK_FRAMES;
        for (i = 0; i < ADSP_TICK_FRAMES; i++) {
            uint32_t need = (uint32_t)(st->pos >> 32) + 1;
            float frac, s;
            while (st->decIdx <= need) {
                st->s0 = st->s1;
                st->s1 = stream_decode(st);
            }
            frac = (float)(uint32_t)st->pos * (1.0f / 4294967296.0f);
            s = (st->s0 + (st->s1 - st->s0) * frac) * (1.0f / 32768.0f);
            st->cl += dl;
            st->cr += dr;
            out[i * 2] += s * st->cl * g[i];
            out[i * 2 + 1] += s * st->cr * g[i];
            st->pos += st->step;
        }
        st->cl = st->gl;
        st->cr = st->gr;
        if (st->stopping)
            st->on = 0;
    }

    for (n = 0; n < ADSP_MAX_VOICES; n++) {
        DVoice *vo = &s_voices[n];
        float dl, dr, *out, *g;
        if (!vo->on)
            continue;
        render_voice_aux(vo, s_catCur[vo->cat]);
        out = cmix[vo->cat];
        g = cg[vo->cat];
        dl = (vo->gl - vo->cl) / ADSP_TICK_FRAMES;
        dr = (vo->gr - vo->cr) / ADSP_TICK_FRAMES;
        for (i = 0; i < ADSP_TICK_FRAMES; i++) {
            uint32_t ip = (uint32_t)(vo->pos >> 32), ip1;
            float frac, s;
            if (vo->loopLen) {
                uint32_t le = vo->loopStart + vo->loopLen;
                while (ip >= le) {
                    vo->pos -= (uint64_t)vo->loopLen << 32;
                    ip -= vo->loopLen;
                }
                ip1 = (ip + 1 >= le) ? vo->loopStart : ip + 1;
            } else {
                if (ip >= vo->len) {
                    vo->on = 0;
                    break;
                }
                ip1 = ip + 1;
            }
            frac = (float)(uint32_t)vo->pos * (1.0f / 4294967296.0f);
            s = vo->pcm[ip] + ((ip1 < vo->len ? vo->pcm[ip1] : 0) - vo->pcm[ip]) * frac;
            s *= 1.0f / 32768.0f;
            vo->cl += dl;
            vo->cr += dr;
            out[i * 2] += s * vo->cl * g[i];
            out[i * 2 + 1] += s * vo->cr * g[i];
            vo->pos += vo->step;
        }
        vo->cl = vo->gl;
        vo->cr = vo->gr;
        if (vo->stopping)
            vo->on = 0;
    }

    for (c = 0; c < ADSP_CATS; c++)
        for (i = 0; i < ADSP_TICK_FRAMES * 2; i++) {
            float x = cmix[c][i], a = fabsf(x);
            mix[i] += x;
            if (a > s_catPeak[c])
                s_catPeak[c] = a;
            s_catSq[c] += (double)x * x;
            if (a > s_winPeak[c])
                s_winPeak[c] = a;
            s_winSq[c] += (double)x * x;
        }
    if (s_mixLog > 0 && (s_renderedFrames + ADSP_TICK_FRAMES) % ADSP_RATE == 0) {
        printf("[mixlog] t=%4.0f s", (double)(s_renderedFrames + ADSP_TICK_FRAMES) / ADSP_RATE);
        for (c = 0; c < ADSP_CATS; c++) {
            printf("  %s %.3f/%.4f", s_catNames[c], s_winPeak[c], sqrt(s_winSq[c] / (2.0 * ADSP_RATE)));
            s_winPeak[c] = 0.0f;
            s_winSq[c] = 0.0;
        }
        printf("\n");
    }

    aux_process(mix);

    bits = atomic_load_explicit(&s_masterBits, memory_order_relaxed);
    memcpy(&master, &bits, 4);

    /* Peak limiter with one tick (5 ms) of look-ahead.  The disc music is
     * mastered close to full scale, so music + SFX summed linearly would
     * clip; the GC DSP saturates instead.  Output is delayed one tick: the
     * gain ramps across the delayed tick toward a value that keeps both it
     * and the next tick below the ceiling, then releases slowly. */
    {
        static float delayed[ADSP_TICK_FRAMES * 2];
        static float delayedPeak;
        static float lim = 1.0f;
        const float ceil = 0.97f;
        float peak = 0.0f, target, g0, dg;
        for (i = 0; i < ADSP_TICK_FRAMES * 2; i++) {
            float a;
            mix[i] *= master;
            a = fabsf(mix[i]);
            if (a > peak)
                peak = a;
        }
        if (peak > s_peak)
            s_peak = peak;
        target = 1.0f;
        if (delayedPeak * target > ceil) target = ceil / delayedPeak;
        if (peak * target > ceil) target = ceil / peak;
        if (target > lim)    /* release: ~150 ms time constant */
            target = lim + (target - lim) * 0.033f;
        if (target < s_limMin)
            s_limMin = target;
        if (target < 0.999f)
            s_limTicks++;
        g0 = lim;
        dg = (target - lim) / ADSP_TICK_FRAMES;
        for (i = 0; i < ADSP_TICK_FRAMES; i++) {
            float g = g0 + dg * (i + 1);
            int c;
            for (c = 0; c < 2; c++) {
                float x = delayed[i * 2 + c] * g;
                if (x > 1.0f) { x = 1.0f; s_clipped++; }
                if (x < -1.0f) { x = -1.0f; s_clipped++; }
                delayed[i * 2 + c] = mix[i * 2 + c];   /* shift the delay line */
                mix[i * 2 + c] = x;
                pcm16[i * 2 + c] = (int16_t)lrintf(x * 32767.0f);
            }
        }
        lim = target;
        delayedPeak = peak;
    }
    s_renderedFrames += ADSP_TICK_FRAMES;

    if (s_wav) {
        fwrite(pcm16, sizeof(int16_t), ADSP_TICK_FRAMES * 2, s_wav);
        s_wavFrames += ADSP_TICK_FRAMES;
        if (s_wavFrames % ADSP_RATE < ADSP_TICK_FRAMES) {
            wav_header(s_wav, s_wavFrames);
            fflush(s_wav);
        }
    }

    /* hand to the device; keep latency bounded (drop when >200 ms ahead) */
    {
        uint32_t w = atomic_load_explicit(&s_outW, memory_order_relaxed);
        uint32_t r = atomic_load_explicit(&s_outR, memory_order_acquire);
        uint32_t fill = w - r;
        if (!s_devOk || fill + ADSP_TICK_FRAMES > OUT_FRAMES || fill > ADSP_RATE / 5) {
            s_outDroppedTicks++;
        } else {
            for (i = 0; i < ADSP_TICK_FRAMES; i++) {
                uint32_t o = ((w + i) % OUT_FRAMES) * 2;
                s_out[o] = mix[i * 2];
                s_out[o + 1] = mix[i * 2 + 1];
            }
            atomic_store_explicit(&s_outW, w + ADSP_TICK_FRAMES, memory_order_release);
        }
    }
}

static void worker_process(void)
{
    for (;;) {
        uint32_t tail = atomic_load_explicit(&s_qTail, memory_order_relaxed);
        uint32_t pub = atomic_load_explicit(&s_qPub, memory_order_acquire);
        CmdHdr *h;
        void *p;
        if (tail == pub)
            return;
        h = (CmdHdr *)(s_q + (tail % CMDQ_SIZE));
        p = h + 1;
        switch (h->type) {
        case CMD_VSTART: {
            CmdVStart *c = p;
            DVoice *vo = &s_voices[h->idx];
            vo->on = 1; vo->stopping = 0;
            vo->pcm = c->pcm; vo->len = c->len;
            vo->loopStart = c->loopStart; vo->loopLen = c->loopLen;
            vo->pos = 0; vo->step = c->step;
            vo->gl = vo->cl = c->gl; vo->gr = vo->cr = c->gr;
            vo->cat = (c->cat >= 0 && c->cat < ADSP_CATS) ? c->cat : ADSP_CAT_SFX;
            memset(vo->ga, 0, sizeof(vo->ga));
            memset(vo->ca, 0, sizeof(vo->ca));
            vo->auxFresh = 1;
            break;
        }
        case CMD_VSET: {
            CmdSet *c = p;
            DVoice *vo = &s_voices[h->idx];
            vo->step = c->step; vo->gl = c->gl; vo->gr = c->gr;
            break;
        }
        case CMD_VSTOP: {
            DVoice *vo = &s_voices[h->idx];
            if (vo->on) { vo->gl = vo->gr = 0; memset(vo->ga, 0, sizeof(vo->ga)); vo->stopping = 1; }
            break;
        }
        case CMD_VAUX: {
            CmdVAux *c = p;
            memcpy(s_voices[h->idx].ga, c->g, sizeof(c->g));
            break;
        }
        case CMD_AUXSET: {
            CmdAuxSet *c = p;
            s_auxFn[0] = c->fn[0]; s_auxUser[0] = c->user[0];
            s_auxFn[1] = c->fn[1]; s_auxUser[1] = c->user[1];
            break;
        }
        case CMD_RUN: {
            CmdRun *c = p;
            c->fn(c + 1);
            break;
        }
        case CMD_SUPLOAD: {
            CmdUpload *c = p;
            DStream *st = &s_streams[h->idx];
            if (c->off < ADSP_STREAM_BYTES) {
                uint32_t b = c->bytes;
                if (c->off + b > ADSP_STREAM_BYTES)
                    b = ADSP_STREAM_BYTES - c->off;
                memcpy(st->ring + c->off, c + 1, b);
            }
            break;
        }
        case CMD_SSTART: {
            CmdSStart *c = p;
            DStream *st = &s_streams[h->idx];
            st->on = 1; st->stopping = 0;
            memcpy(st->coefs, c->coefs, sizeof(st->coefs));
            st->pos = 0; st->step = c->step;
            st->decIdx = 0; st->h1 = st->h2 = 0; st->s0 = st->s1 = 0;
            st->shift = 0; st->c1 = st->c2 = 0;
            st->gl = c->gl; st->gr = c->gr;
            st->cl = st->cr = 0;   /* fade in over the first tick */
            st->cat = (c->cat >= 0 && c->cat < ADSP_CATS) ? c->cat : ADSP_CAT_MUSIC;
            break;
        }
        case CMD_SSET: {
            CmdSet *c = p;
            DStream *st = &s_streams[h->idx];
            st->step = c->step; st->gl = c->gl; st->gr = c->gr;
            break;
        }
        case CMD_SSTOP: {
            DStream *st = &s_streams[h->idx];
            if (st->on) { st->gl = st->gr = 0; st->stopping = 1; }
            break;
        }
        case CMD_FREE:
            free(*(void **)p);
            break;
        case CMD_TICK:
            render_tick();
            break;
        default:
            break;
        }
        atomic_store_explicit(&s_qTail, tail + h->size, memory_order_release);
    }
}

static int worker_main(void *arg)
{
    (void)arg;
    while (!atomic_load(&s_quit)) {
        SDL_SemWaitTimeout(s_sem, 50);
        worker_process();
    }
    worker_process();
    return 0;
}

static void device_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames)
{
    float *o = out;
    uint32_t r = atomic_load_explicit(&s_outR, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&s_outW, memory_order_acquire);
    uint32_t fill = w - r, i, n;
    (void)dev; (void)in;

    /* after an underrun, wait for 50 ms of audio before resuming */
    if (!s_outPrimed && fill < ADSP_RATE / 20) {
        memset(o, 0, frames * 2 * sizeof(float));
        return;
    }
    s_outPrimed = 1;
    n = fill < frames ? fill : frames;
    for (i = 0; i < n; i++) {
        uint32_t k = ((r + i) % OUT_FRAMES) * 2;
        o[i * 2] = s_out[k];
        o[i * 2 + 1] = s_out[k + 1];
    }
    if (n < frames) {
        memset(o + n * 2, 0, (frames - n) * 2 * sizeof(float));
        s_outPrimed = 0;
        s_outUnderruns++;
    }
    atomic_store_explicit(&s_outR, r + n, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* command API (game thread)                                           */

void adsp_voice_start(int v, const int16_t *pcm, uint32_t len, uint32_t loopStart,
                      uint32_t loopLen, uint64_t step, float gl, float gr, int cat)
{
    CmdVStart *c = q_reserve(CMD_VSTART, v, sizeof(*c));
    if (!c) return;
    c->pcm = pcm; c->len = len; c->loopStart = loopStart; c->loopLen = loopLen;
    c->step = step; c->gl = gl; c->gr = gr; c->cat = cat;
}

void adsp_voice_set(int v, uint64_t step, float gl, float gr)
{
    CmdSet *c = q_reserve(CMD_VSET, v, sizeof(*c));
    if (!c) return;
    c->step = step; c->gl = gl; c->gr = gr;
}

void adsp_voice_stop(int v) { q_reserve(CMD_VSTOP, v, 0); }

void adsp_voice_aux(int v, const float g[ADSP_AUX_GAINS])
{
    CmdVAux *c = q_reserve(CMD_VAUX, v, sizeof(*c));
    if (!c) return;
    memcpy(c->g, g, sizeof(c->g));
}

void adsp_aux_set(adsp_aux_fn a, void *ua, adsp_aux_fn b, void *ub)
{
    CmdAuxSet *c = q_reserve(CMD_AUXSET, 0, sizeof(*c));
    if (!c) return;
    c->fn[0] = a; c->user[0] = ua;
    c->fn[1] = b; c->user[1] = ub;
}

void adsp_run(adsp_fn fn, const void *arg, uint32_t bytes)
{
    CmdRun *c = q_reserve(CMD_RUN, 0, (uint32_t)sizeof(*c) + bytes);
    if (!c) return;
    c->fn = fn;
    c->bytes = bytes;
    memcpy(c + 1, arg, bytes);
}

void adsp_stream_upload(int s, uint32_t off, const void *src, uint32_t bytes)
{
    CmdUpload *c = q_reserve(CMD_SUPLOAD, s, sizeof(*c) + bytes);
    if (!c) return;
    c->off = off; c->bytes = bytes;
    memcpy(c + 1, src, bytes);
}

void adsp_stream_start(int s, const int16_t coefs[16], uint64_t step, float gl, float gr, int cat)
{
    CmdSStart *c = q_reserve(CMD_SSTART, s, sizeof(*c));
    if (!c) return;
    memcpy(c->coefs, coefs, sizeof(c->coefs));
    c->step = step; c->gl = gl; c->gr = gr; c->cat = cat;
}

void adsp_stream_set(int s, uint64_t step, float gl, float gr)
{
    CmdSet *c = q_reserve(CMD_SSET, s, sizeof(*c));
    if (!c) return;
    c->step = step; c->gl = gl; c->gr = gr;
}

void adsp_stream_stop(int s) { q_reserve(CMD_SSTOP, s, 0); }

void adsp_free(void *ptr)
{
    void **c = q_reserve(CMD_FREE, 0, sizeof(void *));
    if (c) *c = ptr;   /* if dropped, leak rather than free under the worker */
}

void adsp_tick(void) { q_reserve(CMD_TICK, 0, 0); }

void adsp_flush(void)
{
    if (!s_q)
        return;
    atomic_store_explicit(&s_qPub, s_qHead, memory_order_release);
    if (s_sem)
        SDL_SemPost(s_sem);
}

void adsp_set_master(float g)
{
    uint32_t bits;
    memcpy(&bits, &g, 4);
    atomic_store(&s_masterBits, bits);
}

void adsp_set_category_gain(int cat, float g)
{
    uint32_t bits;
    if (cat < 0 || cat >= ADSP_CATS)
        return;
    memcpy(&bits, &g, 4);
    atomic_store(&s_catBits[cat], bits);
}

/* ------------------------------------------------------------------ */
/* audio.h API                                                         */

bool audio_init(void)
{
    ma_device_config cfg;
    ma_backend nullBackend[1] = { ma_backend_null };

    if (s_inited)
        return true;
    s_inited = 1;
    adsp_set_master(1.0f);
    {
        const char *e = getenv("SMB2_MIXLOG");
        s_mixLog = (e && *e && *e != '0');
    }

    s_q = malloc(CMDQ_SIZE);
    s_sem = SDL_CreateSemaphore(0);
    s_thread = SDL_CreateThread(worker_main, "adsp", NULL);
    if (!s_q || !s_sem || !s_thread)
        fprintf(stderr, "[audio] worker setup failed\n");

    if (g_audio_wav_arg) {
        s_wav = fopen(g_audio_wav_arg, "wb");
        if (s_wav) {
            wav_header(s_wav, 0);
            printf("[audio] writing mixed output to %s\n", g_audio_wav_arg);
        } else
            fprintf(stderr, "[audio] cannot open %s\n", g_audio_wav_arg);
    }

    if (ma_context_init(g_audio_null_arg ? nullBackend : NULL, g_audio_null_arg ? 1 : 0,
                        NULL, &s_ctx) == MA_SUCCESS) {
        s_ctxOk = 1;
        cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = ADSP_RATE;
        cfg.periodSizeInFrames = 480;
        cfg.dataCallback = device_cb;
        if (ma_device_init(&s_ctx, &cfg, &s_dev) == MA_SUCCESS &&
            ma_device_start(&s_dev) == MA_SUCCESS) {
            s_devOk = 1;
            printf("[audio] device: %s (%s backend), %u Hz\n", s_dev.playback.name,
                   ma_get_backend_name(s_ctx.backend), s_dev.sampleRate);
        }
    }
    if (!s_devOk)
        printf("[audio] no output device - rendering without playback\n");
    atexit(audio_shutdown);
    return true;
}

void audio_shutdown(void)
{
    if (!s_inited)
        return;
    s_inited = 0;
    adsp_flush();
    if (s_thread) {
        atomic_store(&s_quit, 1);
        SDL_SemPost(s_sem);
        SDL_WaitThread(s_thread, NULL);
        s_thread = NULL;
    }
    if (s_devOk) {
        ma_device_uninit(&s_dev);
        s_devOk = 0;
    }
    if (s_ctxOk) {
        ma_context_uninit(&s_ctx);
        s_ctxOk = 0;
    }
    if (s_wav) {
        wav_header(s_wav, s_wavFrames);
        fclose(s_wav);
        s_wav = NULL;
    }
    printf("[audio] rendered %.2f s, mix peak %.3f, limiter active %.2f s (min gain %.3f), "
           "clipped samples %llu, device-dropped ticks %u, underruns %u, queue drops %u\n",
           (double)s_renderedFrames / ADSP_RATE, s_peak,
           (double)s_limTicks * ADSP_TICK_FRAMES / ADSP_RATE, s_limMin, (unsigned long long)s_clipped,
           s_outDroppedTicks, s_outUnderruns, s_qDropped);
    {
        int c;
        for (c = 0; c < ADSP_CATS; c++) {
            float g;
            uint32_t bits = atomic_load(&s_catBits[c]);
            memcpy(&g, &bits, 4);
            printf("[audio] mixer %-5s volume %3.0f%%: peak %.4f rms %.5f\n", s_catNames[c], g * 100.0f,
                   s_catPeak[c], s_renderedFrames ? sqrt(s_catSq[c] / (2.0 * (double)s_renderedFrames)) : 0.0);
        }
    }
    fflush(stdout);
}

/* --perf frame log: device underruns so far (choppy audio = game thread
 * not producing ticks fast enough) */
unsigned audio_underrun_count(void) { return s_outUnderruns; }

void audio_update(void) { }

void audio_set_master_volume(float v) { adsp_set_master(v < 0 ? 0 : v > 1 ? 1 : v); }
void audio_set_category_volume(int cat, float v) { adsp_set_category_gain(cat, v < 0 ? 0 : v > 1 ? 1 : v); }
void audio_set_music_volume(float v) { (void)v; }
void audio_set_sfx_volume(float v) { (void)v; }

int audio_get_device_count(void) { return s_devOk ? 1 : 0; }

const AudioDeviceInfo *audio_get_device_info(int index)
{
    static AudioDeviceInfo info;
    if (!s_devOk || index != 0)
        return NULL;
    info.id = 0;
    snprintf(info.name, sizeof(info.name), "%s", s_dev.playback.name);
    info.is_default = true;
    return &info;
}

bool audio_set_output_device(int id) { return id == 0 && s_devOk; }

#else /* !SMB2_LINK_GAME: placeholder backend */
#include "audio.h"
#define MA_IMPLEMENTATION
#include <miniaudio.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Audio context */
typedef struct {
    ma_engine engine;
    ma_context context;
    ma_device_info* playback_devices;
    ma_uint32 playback_device_count;
    ma_uint32 current_device_index;
    float master_volume;
    float music_volume;
    float sfx_volume;
    bool initialized;
} AudioBackend;

static AudioBackend g_audio = {0};

/* Sound handle for playing sounds */
typedef struct {
    ma_sound sound;
    bool active;
    uint32_t voice_id;
    float volume;
    float pan;
    float pitch;
    bool looping;
} SoundHandle;

#define MAX_SOUNDS 256
static SoundHandle g_sounds[MAX_SOUNDS] = {0};
static uint32_t g_next_voice_id = 1;

/* Sound groups loaded */
#define MAX_SOUND_GROUPS 64
static bool g_sound_groups_loaded[MAX_SOUND_GROUPS] = {0};

/* Audio device info cache */
static AudioDeviceInfo g_device_info[32] = {0};
static uint32_t g_device_count = 0;

bool audio_init(void) {
    if (g_audio.initialized) return true;

    ma_engine_config config = ma_engine_config_init();
    config.noAutoStart = MA_FALSE;

    if (ma_engine_init(&config, &g_audio.engine) != MA_SUCCESS) {
        fprintf(stderr, "Failed to initialize miniaudio engine\n");
        return false;
    }

    g_audio.master_volume = 1.0f;
    g_audio.music_volume = 1.0f;
    g_audio.sfx_volume = 1.0f;

    ma_engine_set_volume(&g_audio.engine, g_audio.master_volume);

    /* Enumerate devices */
    ma_context_config ctx_config = ma_context_config_init();
    if (ma_context_init(NULL, 0, &ctx_config, &g_audio.context) == MA_SUCCESS) {
        if (ma_context_get_devices(&g_audio.context, &g_audio.playback_devices, &g_audio.playback_device_count, NULL, NULL) == MA_SUCCESS) {
            g_device_count = (g_audio.playback_device_count > 32) ? 32 : g_audio.playback_device_count;
            for (uint32_t i = 0; i < g_device_count; i++) {
                g_device_info[i].id = i;
                strncpy(g_device_info[i].name, g_audio.playback_devices[i].name, 255);
                g_device_info[i].name[255] = '\0';
                g_device_info[i].is_default = (i == 0);
            }
            g_audio.current_device_index = 0;
        }
    }

    g_audio.initialized = true;
    printf("Audio system initialized (miniaudio)\n");
    return true;
}

void audio_shutdown(void) {
    if (!g_audio.initialized) return;

    /* Stop all sounds */
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            ma_sound_stop(&g_sounds[i].sound);
            ma_sound_uninit(&g_sounds[i].sound);
            g_sounds[i].active = false;
        }
    }

    if (g_audio.playback_devices) {
        /* Device info is freed by ma_context_uninit */
        g_audio.playback_devices = NULL;
    }

    ma_engine_uninit(&g_audio.engine);
    ma_context_uninit(&g_audio.context);
    g_audio.initialized = false;

    printf("Audio system shut down\n");
}

void audio_update(void) {
    /* nothing to do: miniaudio's engine mixes on its own thread */
    if (!g_audio.initialized) return;
}

static SoundHandle* alloc_sound_handle(void) {
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (!g_sounds[i].active) {
            g_sounds[i].voice_id = g_next_voice_id++;
            g_sounds[i].active = true;
            g_sounds[i].volume = 1.0f;
            g_sounds[i].pan = 0.0f;
            g_sounds[i].pitch = 1.0f;
            g_sounds[i].looping = false;
            return &g_sounds[i];
        }
    }
    return NULL;
}

static void free_sound_handle(SoundHandle* handle) {
    if (handle) {
        handle->active = false;
        handle->voice_id = 0;
    }
}

/* MusyX compatibility functions */
int32_t sndPushGroup(void* projData, int32_t groupId, void* sampData, void* sdirData, void* poolData) {
    /* placeholder: only marks the group as loaded */
    if (groupId >= 0 && groupId < MAX_SOUND_GROUPS) {
        g_sound_groups_loaded[groupId] = true;
        printf("Sound group %d loaded (simulated)\n", groupId);
        return groupId;
    }
    return -1;
}

void sndPopGroup(int32_t groupId) {
    if (groupId >= 0 && groupId < MAX_SOUND_GROUPS) {
        g_sound_groups_loaded[groupId] = false;
        printf("Sound group %d unloaded\n", groupId);
    }
}

uint32_t sndFXStartParaInfo(uint16_t fxId, int32_t volume, int32_t pan, int32_t surround, SND_PARAMETER_INFO* info) {
    /* Start a sound effect with parameters */
    SoundHandle* handle = alloc_sound_handle();
    if (!handle) return 0;

    /* Convert volume (0-127) to 0-1.0 */
    handle->volume = (float)volume / 127.0f * g_audio.sfx_volume;

    /* Convert pan (-64 to 63) to -1.0 to 1.0 */
    handle->pan = (float)pan / 64.0f;

    /* placeholder: plays sounds/fx_<fxId>.wav if it exists */
    char path[256];
    snprintf(path, sizeof(path), "sounds/fx_%04d.wav", fxId);

    if (ma_sound_init_from_file(&g_audio.engine, path, MA_SOUND_FLAG_DECODE, NULL, NULL, &handle->sound) == MA_SUCCESS) {
        ma_sound_set_volume(&handle->sound, handle->volume);
        ma_sound_set_pan(&handle->sound, handle->pan);
        ma_sound_start(&handle->sound);
        printf("Playing sound fxId=%d, voiceId=%u\n", fxId, handle->voice_id);
        return handle->voice_id;
    } else {
        /* no file: no sound */
        printf("Sound file not found: %s (generating tone)\n", path);
        free_sound_handle(handle);
        return 0;
    }
}

void sndFXKeyOff(uint32_t voiceId) {
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active && g_sounds[i].voice_id == voiceId) {
            ma_sound_stop(&g_sounds[i].sound);
            ma_sound_uninit(&g_sounds[i].sound);
            free_sound_handle(&g_sounds[i]);
            return;
        }
    }
}

void sndFXSetCtrl(uint32_t voiceId, uint16_t ctrl, int32_t value) {
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active && g_sounds[i].voice_id == voiceId) {
            switch (ctrl) {
                case 0: /* Volume */
                    g_sounds[i].volume = (float)value / 127.0f * g_audio.sfx_volume;
                    ma_sound_set_volume(&g_sounds[i].sound, g_sounds[i].volume);
                    break;
                case 1: /* Pan */
                    g_sounds[i].pan = (float)value / 64.0f;
                    ma_sound_set_pan(&g_sounds[i].sound, g_sounds[i].pan);
                    break;
                case 2: /* Pitch */
                    g_sounds[i].pitch = (float)value / 100.0f;
                    ma_sound_set_pitch(&g_sounds[i].sound, g_sounds[i].pitch);
                    break;
                default:
                    break;
            }
            return;
        }
    }
}

/* High-level sound API matching original game.
 * When the decomp is linked (SMB2_LINK_GAME), the game's own sound.c
 * provides these; the placeholders must not collide. */
#ifndef SMB2_LINK_GAME
void SoundGroupLoad(int groupId) {
    sndPushGroup(NULL, groupId, NULL, NULL, NULL);
}

void g_something_calls_sndPopGroup(void) {
    /* Placeholder - original game calls this */
}

void g_smth_calls_sndFXStartParaInfo(void* req) {
    /* Placeholder - original game calls this */
}

int32_t SoundReqDirect(uint32_t soundId) {
    /* Direct sound request - play and return voice ID */
    return sndFXStartParaInfo((uint16_t)soundId, 127, 0, 0, NULL);
}

void SoundOff(uint16_t soundId) {
    /* Stop all instances of this sound ID */
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            /* Would need to track which fxId each handle corresponds to */
            ma_sound_stop(&g_sounds[i].sound);
            ma_sound_uninit(&g_sounds[i].sound);
            free_sound_handle(&g_sounds[i]);
        }
    }
}

void SoundVol(uint16_t soundId, uint8_t volume) {
    float vol = (float)volume / 255.0f * g_audio.sfx_volume;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            ma_sound_set_volume(&g_sounds[i].sound, vol);
        }
    }
}

void SoundPan(uint16_t soundId, int8_t pan, int8_t span) {
    float p = (float)pan / 127.0f;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            ma_sound_set_pan(&g_sounds[i].sound, p);
        }
    }
}

void SoundPitch(uint16_t soundId, int16_t pitch) {
    float p = (float)pitch / 100.0f;
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            ma_sound_set_pitch(&g_sounds[i].sound, p);
        }
    }
}

void SoftStreamSEReq(uint32_t soundId) {
    /* Streamed sound effect request */
    SoundReqDirect(soundId);
}

void g_crossfade_music(void) {
    /* Music crossfade - placeholder */
}
#endif /* !SMB2_LINK_GAME */

void audio_set_master_volume(float volume) {
    g_audio.master_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    ma_engine_set_volume(&g_audio.engine, g_audio.master_volume);
}

void audio_set_music_volume(float volume) {
    g_audio.music_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
}

void audio_set_category_volume(int cat, float volume) { (void)cat; (void)volume; }

void audio_set_sfx_volume(float volume) {
    g_audio.sfx_volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_sounds[i].active) {
            ma_sound_set_volume(&g_sounds[i].sound, g_sounds[i].volume * g_audio.sfx_volume);
        }
    }
}

int audio_get_device_count(void) {
    return g_device_count;
}

const AudioDeviceInfo* audio_get_device_info(int index) {
    if (index >= 0 && index < g_device_count) {
        return &g_device_info[index];
    }
    return NULL;
}

bool audio_set_output_device(int device_id) {
    if (device_id < 0 || device_id >= g_device_count) return false;

    /* Reinitialize engine with new device */
    ma_engine_uninit(&g_audio.engine);

    ma_engine_config config = ma_engine_config_init();
    config.pPlaybackDeviceID = &g_audio.playback_devices[device_id].id;
    config.noAutoStart = MA_FALSE;

    if (ma_engine_init(&config, &g_audio.engine) != MA_SUCCESS) {
        fprintf(stderr, "Failed to reinitialize audio engine with device %d\n", device_id);
        return false;
    }

    ma_engine_set_volume(&g_audio.engine, g_audio.master_volume);
    g_audio.current_device_index = device_id;
    return true;
}
#endif /* SMB2_LINK_GAME */
