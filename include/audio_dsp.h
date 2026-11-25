/* audio_dsp.h - the "DSP" half of the PC audio path (docs/audio.md).
 *
 * The game-thread MusyX synth (musyx_shim.c) sends commands through this
 * interface into a lock-free SPSC queue; the worker in audio_miniaudio.c
 * runs them in order and renders ADSP_TICK_FRAMES frames per tick.  The game
 * thread never blocks: a command that does not fit is dropped and counted.
 *
 * Plain C types only: included by both the game shim and the exe side. */
#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include <stdint.h>

#define ADSP_RATE          48000
#define ADSP_TICK_FRAMES   240          /* 5 ms at 48 kHz (MusyX synth tick) */
#define ADSP_MAX_VOICES    64
#define ADSP_MAX_STREAMS   10
#define ADSP_STREAM_BYTES  0x12780      /* ADPCM ring per stream (= smgr buffer) */
#define ADSP_STREAM_SAMPLES 0x20520     /* samples in that ring */
#define ADSP_AUX_GAINS     6            /* aux A L/R/S, aux B L/R/S */

/* Mixer categories (docs/audio.md, "Mixer"): every voice and stream belongs
 * to one; the worker scales it by that category's user volume. */
enum {
    ADSP_CAT_SFX = 0,     /* sound effects (default) */
    ADSP_CAT_MUSIC,       /* music streams */
    ADSP_CAT_VOICE,       /* announcer and monkey voices */
    ADSP_CAT_TIMER,       /* stage timer ticks / countdown beeps */
    ADSP_CATS
};

/* step: source samples per output frame, 32.32 fixed point */
static inline uint64_t adsp_step_for_rate(double srcRate)
{
    return (uint64_t)(srcRate / (double)ADSP_RATE * 4294967296.0 + 0.5);
}

/* Sample voices: pcm stays owned by the caller until adsp_free() of it. */
void adsp_voice_start(int v, const int16_t *pcm, uint32_t len, uint32_t loopStart,
                      uint32_t loopLen, uint64_t step, float gainL, float gainR, int cat);
void adsp_voice_set(int v, uint64_t step, float gainL, float gainR);
void adsp_voice_stop(int v);

/* DSP-ADPCM streams (ring of ADSP_STREAM_SAMPLES samples) */
void adsp_stream_upload(int s, uint32_t byteOff, const void *src, uint32_t bytes);
void adsp_stream_start(int s, const int16_t coefs[16], uint64_t step, float gainL, float gainR,
                       int cat);
void adsp_stream_set(int s, uint64_t step, float gainL, float gainR);
void adsp_stream_stop(int s);

/* Aux sends (MusyX aux A = reverb bus, aux B = chorus bus): target gains
 * g[0..2] = aux A L, R, S and g[3..5] = aux B L, R, S, ramped over a tick
 * (taken as-is if the voice was started in the same tick). */
void adsp_voice_aux(int v, const float g[ADSP_AUX_GAINS]);

/* MusyX aux processing (salHandleAuxProcessing, DOL 0x8004C398): the
 * worker mixes the voices' aux sends into s32 L/R/S buses of
 * ADSP_AUX_FRAMES samples at ADSP_AUX_RATE (16-bit sample scale), calls
 * fn(ADSP_AUX_REASON_BUFFERUPDATE, &ADSP_AUX_INFO, user) on them every tick
 * and mixes the processed buffers back into the output two ticks later, as
 * the GC's DSP does.  NULL fn = that bus is off. */
#define ADSP_AUX_RATE      32000
#define ADSP_AUX_FRAMES    160          /* 5 ms at 32 kHz */
#define ADSP_AUX_REASON_BUFFERUPDATE 0
typedef struct { int32_t *left, *right, *surround; } ADSP_AUX_INFO;
typedef void (*adsp_aux_fn)(uint8_t reason, void *info, void *user);
void adsp_aux_set(adsp_aux_fn auxA, void *userA, adsp_aux_fn auxB, void *userB);

/* Run fn(copy of arg[0..bytes)) on the worker, in command order (the MusyX
 * aux effects' Prepare/Update/Shutdown, which share state with the
 * callbacks). */
typedef void (*adsp_fn)(void *arg);
void adsp_run(adsp_fn fn, const void *arg, uint32_t bytes);

void adsp_free(void *p);            /* free() p on the worker, in command order */
void adsp_tick(void);               /* render one tick */
void adsp_flush(void);              /* publish queued commands to the worker */
void adsp_set_master(float gain);   /* user volume (audio_set_master_volume) */
/* user volume of one ADSP_CAT_* (audio_set_category_volume); applies to the
 * voices and streams already playing, ramped over one tick */
void adsp_set_category_gain(int cat, float gain);

#endif
