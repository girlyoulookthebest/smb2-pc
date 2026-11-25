/* High frame rate rendering by interpolated replay (docs/hfr.md).
 *
 * The game logic stays a fixed 60 Hz tick.  While HFR is on, a recording
 * proxy sits between gx_core.c and the GPU backend and keeps every backend
 * call of the tick's frame (state, clears, binds, EFB copies, draws with
 * their constant blocks and vertices).  Between two ticks the platform pacer
 * re-submits the recorded frames with their matrices, vertices, lights and
 * colours blended between the matching draws of the last two ticks, so
 * camera and object motion is shown at the display rate.  At the frame rate
 * setting 60 (the default) the proxy is not used and nothing is recorded. */
#ifndef GX_HFR_H
#define GX_HFR_H

#include "gx_gpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* presents per second while HFR runs (pacer target of the current tick) */
extern int g_hfr_cur_fps;

/* render_init: remember the backend, build the proxy */
void hfr_init(const GxGpu* real);
/* gx_start_new_frame, before the frame starts: the backend interface to use
 * for this tick (the proxy when the frame-rate setting is not 60, else the
 * backend itself).  Switching applies at tick boundaries. */
const GxGpu* hfr_select(void);
int  hfr_active(void);       /* the proxy is in use this tick */
void hfr_tick_begin(void);   /* gx_start_new_frame: a new frame stream starts */
void hfr_tick_end(void);     /* video_finish_frame: the tick's frame is done */
int  hfr_tick_valid(void);   /* last stream complete and replayable */
void hfr_render(float alpha);/* draw the frame at alpha (0 = previous tick, 1 = last) */
void hfr_note_present(void); /* present-interval statistics */
int  hfr_final_pending(void);/* the tick frame is not drawn yet (deferred recording) */
/* GXCopyTex through the synchronous path refreshed tex with the copy of the
 * GL rect (x,y,w,h) resampled to dw x dh in copy format cf; gpu_ok = the
 * texture holds exactly what copy_tex would write (natural sample format) */
void hfr_note_sync_copy(void* tex, int x, int y, int w, int h, int dw, int dh, int cf, int gpu_ok);

/* game-side hooks (decomp include/pc_hfr.h, PC build only) */
void pc_hfr_tag_push(unsigned kind, unsigned id);
void pc_hfr_tag_pop(void);
void pc_hfr_cut(int reason);
void pc_hfr_view(int camera, const float (*mtx)[4]);
void pc_hfr_node_alloc(void* node);
void pc_hfr_node_begin(void* node, void* drawFunc);
void pc_hfr_node_end(void);
void pc_hfr_texslots(const unsigned* ids);

#ifdef __cplusplus
}
#endif
#endif
