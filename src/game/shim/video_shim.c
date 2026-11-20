/* VI / render-mode shim, plus the per-frame hook the game's render.c calls.
 *
 * video_finish_frame() stands in for the GC's VIWaitForRetrace + XFB flip at
 * the end of every frame: it pumps SDL events, refreshes the GC pad globals,
 * presents the frame and paces the loop to 60 Hz. */
#include "shim.h"
#include "gx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "perf.h"

/* ---- render modes (SDK GXNtsc480IntDf and friends) ---- */
#define SP_DEFAULT {{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6}}
#define VF_DEFAULT {8, 8, 10, 12, 10, 8, 8}
#define VF_PROG    {0, 0, 21, 22, 21, 0, 0}

GXRenderModeObj ntsc_interlaced_render_mode = {
    0 /* VI_TVMODE_NTSC_INT */, 640, 480, 480, 40, 0, 640, 480,
    1 /* VI_XFBMODE_DF */, 0, 0, SP_DEFAULT, VF_DEFAULT
};

GXRenderModeObj ntsc_progressive_render_mode = {
    2 /* VI_TVMODE_NTSC_PROG */, 640, 480, 480, 40, 0, 640, 480,
    0 /* VI_XFBMODE_SF */, 0, 0, SP_DEFAULT, VF_PROG
};

GXRenderModeObj pal_interlaced_render_mode = {
    4 /* VI_TVMODE_PAL_INT */, 640, 480, 480, 40, 23, 640, 528,
    1, 0, 0, SP_DEFAULT, VF_DEFAULT
};

GXRenderModeObj mpal_interlaced_render_mode = {
    8 /* VI_TVMODE_MPAL_INT */, 640, 480, 480, 40, 0, 640, 480,
    1, 0, 0, SP_DEFAULT, VF_DEFAULT
};

GXRenderModeObj overscan_adjusted_render_mode; /* filled by set_render_mode */

GXRenderModeObj *current_render_mode = &ntsc_interlaced_render_mode;

void GXAdjustForOverscan(GXRenderModeObj *rmin, GXRenderModeObj *rmout, u16 hor, u16 ver)
{
    *rmout = *rmin;
    rmout->fbWidth = rmin->fbWidth - hor * 2;
    rmout->efbHeight = rmin->efbHeight - ver * 2;
    if (rmin->xFBmode == 0 /* SF */ && !rmin->field_rendering)
        rmout->xfbHeight = rmin->xfbHeight - ver;
    else
        rmout->xfbHeight = rmin->xfbHeight - ver * 2;
    rmout->viWidth = rmin->viWidth - hor * 2;
    if (rmin->viTVmode == 4 /* PAL int */ || rmin->viTVmode == 8)
        rmout->viHeight = rmin->viHeight - ver * 2;
    else
        rmout->viHeight = rmin->viHeight - ver * 2;
    rmout->viXOrigin = rmin->viXOrigin + hor;
    rmout->viYOrigin = rmin->viYOrigin + ver;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 width, u16 height);
void GXSetDispCopySrc_cached(u16 left, u16 top, u16 width, u16 height)
{
    GXSetDispCopySrc(left, top, width, height);
}

void VIConfigure(GXRenderModeObj *rm)
{
    current_render_mode = rm;
}

u32 VIGetTvFormat(void)
{
    return 0; /* VI_NTSC */
}

u32 VIGetDTVStatus(void)
{
    return 0; /* no component cable -> no progressive prompt */
}

/* ---- per-frame hooks ---- */

/* gameplay telemetry view of decomp's struct Ball (include/ball.h) */
struct TelemBall { u32 pad0; float px, py, pz; float prevx, prevy, prevz; float vx, vy, vz; };
extern struct TelemBall balls[];

void gp_wait(void)
{
    /* GC: spin until the graphics processor drains its FIFO.  No-op here. */
}

/* ---- HFR in-between frames (docs/hfr.md) ----
 * Called once per tick after the frame was drawn, before the 60 Hz wait.
 * tick0 is when the previous tick's frame went on screen, next_ns when this
 * tick's frame will (gx_start_new_frame presents it).  The frames between
 * show the recorded frames at alpha = (present time - tick0) / period.
 * The game never waits for them: a frame is only started when it and the
 * tick frame's redraw still fit before next_ns, and g_hfr_kcap (the most
 * in-between frames per tick) drops whenever a tick still ran late (GPU
 * backpressure the estimates missed) and recovers slowly. */
extern int  hfr_active(void);
extern int  hfr_tick_valid(void);
extern void hfr_tick_end(void);
extern void hfr_render(float alpha);
extern int  hfr_final_pending(void);
extern double hfr_gpu_frame_est_ms(void);
extern int  g_hfr_cur_fps;
extern int  g_hfr_kcap;
extern int  pcs_effective_fps(void);
extern void platform_present_subframe(int j);
extern long g_dump_from_arg;
extern int g_pcs_vsync_on(void);

static int s_hfr_presented;     /* in-between frames of this tick */
static int s_hfr_skip;          /* no in-between frames this tick (F1 overlay ran) */
static double s_est_render = 1000000.0;   /* replay CPU time, running average (ns) */
static unsigned long long s_tick_start;   /* this tick's logic started (the previous wait's end) */

/* ---- display-grid pacer (V-Sync on, fixed refresh; docs/hfr.md section 4) ----
 * The frames go out on the display's refresh grid, one every q refreshes,
 * and each shows the content of the moment it is on screen:
 *     content(V) = N + (V - S_N - lat) / T     (tick N's logic starts at S_N)
 * so the scene advances by the same amount from one shown frame to the next.
 * Every frame is presented with the sync interval that puts it q refreshes
 * after the previous one (DXGI queues it), so a tick's frames can all be
 * drawn right after its logic although they reach the screen over the
 * following tick.  lat is the content's display latency: the smallest that
 * lets every frame be drawn after the logic of the tick it interpolates to
 * (W = logic + replay + GPU + margin before its refresh), slewed slowly
 * down and raised at once when a frame would not make it.  The display's
 * frame statistics say where each present really went; a frame that
 * landed elsewhere moves the predicted chain with it.
 * q = refresh / requested fps (1 or 2); used only while that rate fits a
 * tick, else the timer pacer shows as many frames as fit. */
extern int hfr_display_timing(unsigned long long* vb_ns, unsigned* vb_count, double* period_ns,
                              unsigned* shown_id, unsigned* shown_count);
extern void hfr_present_interval(int si);
extern void hfr_present_queue(int n);
extern unsigned hfr_present_id(void);
extern double hfr_present_done_est_ms(void);
extern int g_pace_si, g_pace_skip_present; extern double g_pace_plan; extern unsigned g_pace_flags;
extern double g_pace_content;
extern long g_dbg_frame_counter;

#define GRID_RING 32
#define L_HIST 120
static struct {
    int on;                  /* the chain below is valid */
    double lat;              /* content latency (ns) */
    double spos;             /* refresh position of the last slot (presented or skipped) */
    long long last_idx;      /* refresh the last grid present is predicted to show at */
    unsigned last_pid;       /* its present id */
    double q;                /* refreshes per frame */
    double margin;           /* GPU end before its refresh (ns) */
    struct { unsigned pid; long long idx, last; } ring[GRID_RING];   /* idx: unchecked prediction; last: predicted refresh */
    int rn;
    float lhist[L_HIST];     /* logic time per tick (ms) */
    int ln;
    int unfit, fit_calm, fit_hold;   /* the requested rate does not fit: timer pacer */
    int better;
    int misses, miss_win, late, checked, errs, off_n;
    unsigned long long off_until;
    unsigned long long dbg_last;
} G = { 0, 25e6, 0, 0, 0, 1.0, 1.0e6, {{0}}, 0, {0}, 0, 0, 0, 120 };

static double grid_logic_p90(void) {
    float v[L_HIST];
    int n = G.ln < L_HIST ? G.ln : L_HIST, i, j;
    if (n == 0) return 8e6;
    memcpy(v, G.lhist, sizeof(float) * (size_t)n);
    for (i = 1; i < n; i++) {   /* insertion sort, n <= 120 */
        float x = v[i];
        for (j = i - 1; j >= 0 && v[j] > x; j--) v[j + 1] = v[j];
        v[j + 1] = x;
    }
    return (double)v[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1] * 1e6;
}

/* returns 1 when it presented this tick's frames */
/* SMB2_PACE_DBG: why the grid pacer declined a tick (once a second) */
static void grid_decline(const char* why, double a, double b, double c, double d) {
    extern unsigned long long platform_get_time_ns(void);
    static unsigned long long last;
    unsigned long long now;
    if (!getenv("SMB2_PACE_DBG")) return;
    now = platform_get_time_ns();
    if (now - last < 1000000000ULL) return;
    last = now;
    printf("[grid] off: %s (%.2f %.2f %.2f %.2f)\n", why, a, b, c, d);
}

static int hfr_grid(unsigned long long next_ns, unsigned long long period_ns, int fps)
{
    extern unsigned long long platform_get_time_ns(void);
    extern void platform_wait_until_ns(unsigned long long target_ns);
    extern double g_hfr_present_est_ns;
    static int mode = -1;
    unsigned long long vb, now = platform_get_time_ns();
    unsigned vbc, sid, scnt, pid;
    double P, T = (double)period_ns, S = (double)(next_ns - period_ns), g, r, W, lat_req, x, refresh;
    int i, presented = 0, restarted;

    if (mode < 0) {
        const char* e = getenv("SMB2_PACE");
        mode = !(e && !strcmp(e, "timer"));
    }
    if (!mode || !g_pcs_vsync_on() || !hfr_display_timing(&vb, &vbc, &P, &sid, &scnt)) {
        grid_decline("no display timing", (double)mode, (double)g_pcs_vsync_on(), 0, 0);
        G.on = 0;   /* (the deeper flip queue stays: resizing drains the GPU) */
        return 0;
    }
    if (!(P > 2e6 && P < 50e6)) { grid_decline("period", P / 1e6, 0, 0, 0); G.on = 0; return 0; }   /* 20-500 Hz, else not believable */
    /* only where the display confirms the plan: a backend without
     * per-present feedback (GL), or one whose frames land elsewhere (a
     * composed window shows the newest frame at each refresh, whatever
     * the sync interval), keeps the timer pacer (30 s, then retries;
     * each failure doubles the wait) */
    if (!sid || now < G.off_until) { grid_decline("no feedback / off", (double)sid, (double)G.off_n, 0, 0); G.on = 0; return 0; }
    if (G.checked >= 120) {
        if (G.errs * 10 > G.checked) {
            int sh = G.off_n < 5 ? G.off_n : 5;
            G.off_until = now + (30000000000ULL << sh);   /* 30 s, doubling to 16 min */
            G.off_n++;
            G.on = 0;
            printf("[grid] %d of %d frames off their refresh: timer pacer for %d s\n", G.errs, G.checked, 30 << sh);
            G.checked = G.errs = 0;
            return 0;
        }
        G.checked = G.errs = 0;
    }
    refresh = 1e9 / P;
    if (refresh < 61.0) { G.on = 0; return 0; }
#define VT(idx) ((double)vb + (double)((long long)(idx) - (long long)vbc) * P)

    /* logic time of this tick (to the end of matching) */
    /* (from the tick's real start: a late tick must not look like long logic) */
    G.lhist[G.ln++ % L_HIST] = (float)(((double)now - (s_tick_start > (unsigned long long)S ? (double)s_tick_start : S)) / 1e6);
    /* GPU time of a frame after its present call: D3D12 timestamps; GL:
     * present -> GPU done (timestamp queries) */
    g = hfr_gpu_frame_est_ms() * 1e6;
    if (g <= 0.0) g = hfr_present_done_est_ms() * 1e6;
    g = g > 0.0 ? g * 1.1 + 300000.0 : 4e6;
    r = s_est_render + g_hfr_present_est_ns;
    W = grid_logic_p90() + r + g + G.margin;

    /* refreshes per frame: the requested rate exactly (a whole number of
     * refreshes, 1 or 2: measured on a 240 Hz display, frames with sync
     * interval 3-4 did not stay q refreshes apart; other ratios such as
     * 144 on 240 Hz cannot be even) */
    {
        double qf = refresh / (double)fps, L = grid_logic_p90(), k;
        int fit;
        if (qf < 1.0) qf = 1.0;
        G.q = floor(qf + 0.5);
        if (fabs(qf - G.q) > 0.01 * G.q || G.q > 2.0) { grid_decline("rate", qf, (double)fps, 0, 0); G.on = 0; return 0; }
        /* only when the rate fits a tick (CPU: logic + the replays; GPU: the
         * frames).  Otherwise the timer pacer, which shows as many frames
         * as fit: the grid would skip slots unevenly, and dropping to a
         * lower even rate is not what a full-rate request asks for.  Back
         * on the grid after 2 s with headroom; misses count as not
         * fitting, each such fall doubles the wait (up to 32 s). */
        k = T / (G.q * P);
        fit = L + k * r + 1e6 <= T && k * g <= T - 1e6;
        /* and the flip queue: a tick's last frame is shown about
         * lat = W + T - qP (+ half a refresh) after the tick starts, its
         * first is drawn at L + r + g; DXGI queues 3 presents */
        if (fit && (W + T - G.q * P + 0.5 * P - (L + r + g)) / (G.q * P) + 1.0 > 3.0) fit = 0;
        if (G.misses >= 3) { fit = 0; G.misses = 0; if (G.fit_hold < 1920) G.fit_hold *= 2; }
        if (!fit) {
            G.unfit = 1; G.fit_calm = 0;
        } else if (G.unfit) {
            if (L * 1.1 + k * r * 1.15 + 1.5e6 <= T && k * g * 1.15 <= T - 1.5e6) {
                if (++G.fit_calm >= G.fit_hold) G.unfit = 0;
            } else G.fit_calm = 0;
        }
        if (G.unfit) {
            grid_decline("does not fit: L r g W (ms)", L / 1e6, r / 1e6, g / 1e6, W / 1e6);
            G.on = 0;
            return 0;
        }
    }

    /* feedback: where the latest shown present really went */
    if (G.on && sid) {
        for (i = 0; i < GRID_RING; i++) {
            if (G.ring[i].pid == sid && G.ring[i].idx) {
                long long err = (long long)scnt - G.ring[i].idx;
                int j;
                G.ring[i].idx = 0;   /* checked */
                G.checked++;
                if (err) {
                    G.errs++;
                    for (j = 0; j < GRID_RING; j++)
                        if ((int)(G.ring[j].pid - sid) > 0) {
                            if (G.ring[j].idx) G.ring[j].idx += err;
                            G.ring[j].last += err;
                        }
                    G.last_idx += err;
                    G.spos += (double)err;
                    if (err > 0) {
                        G.late++;
                        if (G.margin < 1.5e6) G.margin += 250000.0;
                    }
                }
                break;
            }
        }
    }
    pid = hfr_present_id();
    if (G.on && pid != G.last_pid) G.on = 0;          /* presents outside the pacer */
    /* a hitch (loading: the tick ran over a whole tick late) restarts the
     * chain instead of raising the latency for many seconds */
    if (G.on && (double)now - S > T) G.on = 0;
    restarted = !G.on;
    if (!G.on) {
        /* (re)start: the first frame shows at the first refresh after it
         * can be ready (and after what the display shows now) */
        double ready = (double)now + r + g + G.margin;
        long long idx0 = (long long)vbc + (long long)ceil((ready - (double)vb) / P);
        if (sid && idx0 <= (long long)scnt) idx0 = (long long)scnt + 1;
        G.last_idx = idx0 - 1;
        G.spos = (double)idx0 - G.q;
        G.rn = 0;
        memset(G.ring, 0, sizeof G.ring);
        G.lat = 1e30;    /* set from the requirement below */
        hfr_present_queue(4);
    }

    /* the latency the next tick needs: its first frame (the first slot
     * after its logic, replay and GPU work) must interpolate to it, so the
     * slot before belongs to this tick: lat >= that slot - S_N */
    x = S + T + W;
#define LAT_REQ(sp0) (VT(0) + ((sp0) + floor(((x - VT(0)) / P - (sp0)) / G.q) * G.q) * P - S)
    if (!G.on) {
        /* the grid's phase is free at a start: the one that needs the least
         * latency (the next tick's first frame just after it can be ready) */
        int o, best = 0, n = (int)ceil(G.q);
        for (o = 1; o < n; o++)
            if (LAT_REQ(G.spos + o) < LAT_REQ(G.spos + best)) best = o;
        G.spos += best;
        G.better = 0;
    } else if (G.q >= 2.0 && G.q == floor(G.q)) {
        /* later (the display drifts against the tick clock): move the grid
         * by one refresh when that phase has needed clearly less for 2 s
         * (one frame stays a refresh longer) */
        if (LAT_REQ(G.spos + 1.0) < LAT_REQ(G.spos) - 2e6) {
            if (++G.better >= 120) { G.spos += 1.0; G.better = 0; }
        } else G.better = 0;
    }
    lat_req = LAT_REQ(G.spos);
#undef LAT_REQ
    {
        /* this tick: no slot of this tick before it can be ready */
        double ready = (double)now + r + g + G.margin;
        double k1 = floor(((ready - VT(0)) / P - G.spos) / G.q);
        double l1 = VT(0) + (G.spos + k1 * G.q) * P - (S - T);
        if (k1 >= 1.0 && l1 > lat_req) {
            if (!restarted && l1 > G.lat + G.q * P) { restarted = 1; G.lat = 1e30; }   /* hitch */
            else if (!restarted) lat_req = l1;
        }
    }
    lat_req += 300000.0;
    if (G.lat > 1e29 || G.lat < lat_req) G.lat = lat_req;          /* up at once */
    else G.lat -= (G.lat - lat_req) < 0.003 * T ? (G.lat - lat_req) : 0.003 * T;

    /* this tick's frames: slots with content in (N-1, N] */
    {
        double gbusy = 0.0;
        int guard = 0;
        if (!(G.lat > 0.0 && G.lat < 1e9)) G.lat = 2.0 * T;   /* (a bad estimate must not stall) */
        for (; guard < 64; guard++) {
            double sp = G.spos + G.q, V, alpha, ready;
            long long idx = llround(sp);
            unsigned long long t0;
            int si;
            if (idx <= G.last_idx) { G.spos = sp; continue; }
            V = VT(idx);
            alpha = 1.0 + (V - S - G.lat) / T;
            if (alpha > 1.0005) break;                  /* a later tick's */
            now = platform_get_time_ns();
            ready = ((double)now + r > gbusy ? (double)now + r : gbusy) + g + G.margin;
            if (alpha <= 0.0 || ready > V) {            /* cannot make it: skip the slot */
                G.spos = sp;
                if (alpha > 0.0 && !restarted) { G.misses++; G.miss_win++; }
                continue;
            }
            {
                /* DXGI queues 3 presents; a fourth would block the game
                 * thread (and the next tick): skip the slot instead */
                long long cur = (long long)floor(((double)now - VT(0)) / P);
                int k, pend = 0;
                for (k = 0; k < GRID_RING; k++)
                    if (G.ring[k].pid && G.ring[k].last > cur) pend++;
                if (pend >= 3) { G.spos = sp; if (!restarted) { G.misses++; G.miss_win++; } continue; }
            }
            if (alpha > 0.9995) alpha = 1.0;
            t0 = now;
            hfr_render((float)alpha);
            now = platform_get_time_ns();
            s_est_render = s_est_render * 0.9 + (double)(now - t0) * 0.1;
            si = (int)(idx - G.last_idx);
            if (si > 4) si = 4;
            hfr_present_interval(si);
            g_pace_content = (double)g_dbg_frame_counter + alpha;
            g_pace_si = si; g_pace_plan = V; g_pace_flags = 1;
            platform_present_subframe(presented + 1);
            hfr_present_interval(1);
            now = platform_get_time_ns();
            if (presented == 0) {
                extern double g_hfr_first_ms_acc; extern long g_hfr_first_n;
                g_hfr_first_ms_acc += (double)((double)now - S) / 1e6;
                g_hfr_first_n++;
            }
            presented++;
            G.last_idx = idx;
            G.spos = sp;
            G.last_pid = hfr_present_id();
            /* (a restart frame lands where it can: not checked) */
            G.ring[G.rn].pid = G.last_pid; G.ring[G.rn].idx = restarted ? 0 : idx; G.ring[G.rn].last = idx;
            G.rn = (G.rn + 1) % GRID_RING;
            gbusy = ((double)t0 + r > gbusy ? (double)t0 + r : gbusy) + g;
        }
    }
    G.on = 1;
    if (getenv("SMB2_PACE_DBG") && now - G.dbg_last > 1000000000ULL) {
        G.dbg_last = now;
        printf("[grid] refresh %.3f q %.3f lat %.2f req %.2f W %.2f (L90 %.2f r %.2f g %.2f m %.2f) misses %d late %d\n",
               refresh, G.q, G.lat / 1e6, lat_req / 1e6, W / 1e6, grid_logic_p90() / 1e6, r / 1e6, g / 1e6,
               G.margin / 1e6, G.miss_win, G.late);
        G.miss_win = 0; G.late = 0;
    }
    if (G.misses > 0 && (G.ln % 60) == 0) G.misses--;   /* misses age out */
    s_hfr_presented = presented;
    {
        /* a frame dump of this tick (tests): the tick's own frame at alpha
         * 1, presented at the next tick start as without the grid */
        extern int platform_dump_due_next(void);
        if (platform_dump_due_next()) {
            hfr_render(1.0f);
            G.on = 0;
        } else
            g_pace_skip_present = 1;     /* gx_start_new_frame: nothing more this tick */
    }
#undef VT
    return 1;
}

static void hfr_subframes(unsigned long long next_ns, unsigned long long period_ns)
{
    extern unsigned long long platform_get_time_ns(void);
    extern void platform_wait_until_ns(unsigned long long target_ns);
#define est_render s_est_render
    extern double g_hfr_present_est_ns;     /* present call time, every present (gx_core) */
    unsigned long long tick0 = next_ns - period_ns, now, t0, slot;
    int fps = pcs_effective_fps(), presented = 0, j;
    int vsync = g_pcs_vsync_on();
    /* frame dumps: render every in-between frame at the exact j/k phases
     * regardless of time (the dump itself takes longer than a slot) */
    int fixed = g_dump_from_arg > 0 || getenv("SMB2_HFR_FIXED") != NULL;

    g_hfr_cur_fps = fps;
    s_hfr_presented = 0;
    if (fps == 60 || !hfr_tick_valid() || s_hfr_skip) {
        s_hfr_skip = 0;
        if (hfr_final_pending()) hfr_render(1.0f);
        return;
    }
    if (!fixed && fps > 0 && hfr_grid(next_ns, period_ns, fps))
        return;
    if (fixed) {
        int k = fps > 0 ? (fps + 30) / 60 : 4;
        if (k < 2) k = 2;
        for (j = 1; j < k; j++) {
            extern double g_pace_content; extern long g_dbg_frame_counter;
            hfr_render((float)j / (float)k);
            g_pace_content = (double)g_dbg_frame_counter + (double)j / k;
            platform_present_subframe(j);
            presented++;
        }
    } else {
        /* Phases on the target's grid (tick0 + j/fps; exact j/k for
         * multiples of 60).  A phase the slow tick already passed is not
         * dropped: the frame goes out as soon as it is drawn, at the alpha
         * of its real present time, if it still fits before the tick
         * frame (which needs one more replay).  Uncapped: back to back.
         * With V-Sync the present waits for the display (pacing, not
         * cost), so only the replay counts; without, both do. */
        unsigned long long sub = fps > 0 ? 1000000000ULL / (unsigned long long)fps : 0;
        /* GPU timeline (D3D12 timestamps; 0 = unknown, GL): a frame's GPU
         * work starts when it is presented and the GPU is free; the tick
         * frame presented at tick0 keeps it busy until tick0 + gpu.  An
         * in-between frame must be finished on the GPU, and the tick
         * frame's replay done on the CPU, before next_ns. */
        double gpu = hfr_gpu_frame_est_ms() * 1e6;
        double gbusy = (double)tick0 + gpu;
        slot = tick0 + sub;
        for (j = 1; j <= g_hfr_kcap && j < 64; j++) {
            unsigned long long tp;
            double cpu = est_render + (vsync ? 0.0 : g_hfr_present_est_ns);
            double gstart, gend;
            float alpha;
            now = platform_get_time_ns();
            while (sub && slot + sub <= now)   /* skip whole missed phases */
                slot += sub;
            tp = (double)now + cpu > (double)slot ? now + (unsigned long long)cpu : slot;
            gstart = (double)tp > gbusy ? (double)tp : gbusy;
            gend = gstart + gpu;
            if (gend + 1000000.0 > (double)next_ns || (double)tp + cpu + 1000000.0 > (double)next_ns)
                break;                       /* leave the tick frame its slot */
            /* shown when its GPU work is done: phase from the GPU start */
            alpha = (float)((gstart - (double)tick0) / (double)period_ns);
            if (fps > 0 && fps % 60 == 0 && tp == slot && gstart == (double)tp) {
                int k = fps / 60;            /* exact phase j/k */
                alpha = (float)((slot - tick0 + sub / 2) / sub) / (float)k;
            }
            if (alpha > 1.0f) alpha = 1.0f;
            t0 = now;
            hfr_render(alpha);
            now = platform_get_time_ns();
            est_render = est_render * 0.9 + (double)(now - t0) * 0.1;
            if (sub && now < tp)
                platform_wait_until_ns(tp);

            {
                extern double g_pace_content; extern long g_dbg_frame_counter;
                g_pace_content = (double)g_dbg_frame_counter + alpha;
            }
            platform_present_subframe(j);
            now = platform_get_time_ns();
            if (j == 1) {
                /* latency statistic: tick start (input already read) to
                 * the first present showing this tick; 60 fps: next_ns */
                extern double g_hfr_first_ms_acc; extern long g_hfr_first_n;
                g_hfr_first_ms_acc += (double)(now - tick0) / 1e6;
                g_hfr_first_n++;
            }
            presented++;
            slot = (tp > slot ? tp : slot) + sub;
            gbusy = gend;
        }
    }
    s_hfr_presented = presented;
    /* the tick frame itself, presented at next_ns: drawn again after
     * in-between frames, and always with deferred recording */
    if (presented || hfr_final_pending()) {
        unsigned long long tr = platform_get_time_ns();
        hfr_render(1.0f);
        est_render = est_render * 0.9 + (double)(platform_get_time_ns() - tr) * 0.1;
    }
#undef est_render
}

/* after the tick's wait: a tick that still ended late lowers the cap.
 * Raising it again is a probe: one more in-between frame after `wait` calm
 * ticks; a probe that makes a tick late doubles the wait (up to 8 s), a
 * long calm stretch halves it again. */
static void hfr_adapt(long long late_ns)
{
    static int calm, wait = 30, since_raise = 1 << 30;
    since_raise++;
    if (late_ns > 1500000LL && s_hfr_presented == 0 && hfr_tick_valid()) {
        /* late with no in-between frame at all: recording + matching +
         * the replay cost more than this machine has; plain 60 fps for 2 s */
        extern void hfr_degrade(int ticks);
        hfr_degrade(120);
        calm = 0;
    } else if (late_ns > 2500000LL && s_hfr_presented > 0) {
        g_hfr_kcap = s_hfr_presented - 1;
        if (since_raise < 60 && wait < 480) wait *= 2;   /* the last probe failed */
        calm = 0;
    } else if (++calm >= wait && g_hfr_kcap < 63) {
        g_hfr_kcap++;
        since_raise = 0;
        calm = 0;
        if (wait > 30 && since_raise == 0 && g_hfr_kcap > s_hfr_presented + 1)
            wait /= 2;   /* the cap is not what limits: probing is free */
    }
}

void video_finish_frame(void)
{
    static unsigned frame_no;
    /* SMB2_PACE_LOG: this tick's start (the previous wait's end) and the
     * end of its logic + drawing */
    {
        extern int pace_log_on(void);
        extern void pace_log_tick(long tick, unsigned long long t_start, unsigned long long t_logic);
        extern unsigned long long platform_get_time_ns(void);
        extern long g_dbg_frame_counter;
        if (pace_log_on()) pace_log_tick(g_dbg_frame_counter + 1, s_tick_start, platform_get_time_ns());
    }

    {
        /* HFR: a main mode change is a camera cut; then the tick's frame
         * stream is complete (both no-ops when off) */
        extern s32 main_mode;
        extern void pc_hfr_cut(int reason);
        static s32 last_main_mode = -1;
        /* test hook: SMB2_HFR_SWITCH="tick:fps,tick:fps" changes the frame
         * rate setting at those ticks (live switching, docs/hfr.md) */
        {
            static const char* sw = (const char*)1;
            extern int pcs_debug_set_fps(int fps);
            if (sw == (const char*)1) sw = getenv("SMB2_HFR_SWITCH");
            if (sw) {
                const char* q = sw;
                while (*q) {
                    char* e;
                    long t = strtol(q, &e, 10);
                    if (*e != ':') break;
                    if ((unsigned)t == frame_no) pcs_debug_set_fps((int)strtol(e + 1, &e, 10));
                    else strtol(e + 1, &e, 10);
                    q = *e == ',' ? e + 1 : e;
                    if (!*e) break;
                }
            }
        }
        if (main_mode != last_main_mode) {
            last_main_mode = main_mode;
            pc_hfr_cut(2);
        }
        hfr_tick_end();
    }

    {
        /* audio: deferred sound DVD callbacks + 5 ms synth ticks of game time */
        extern void musyx_frame(void);
        PERF_BEGIN(ta);
        musyx_frame();
        PERF_END(ta, PERF_T_AUDIO);
    }

#ifdef _WIN32
    {
        extern volatile long g_shim_frame_pulse;
        g_shim_frame_pulse++;
    }
#endif
    /* SMB2_BALL_LOG=1: ball position / velocity every gameplay frame
     * (stick tests, docs/stick.md) */
    {
        static int ball_log = -1;
        extern s32 main_mode;
        if (ball_log < 0) ball_log = getenv("SMB2_BALL_LOG") && atoi(getenv("SMB2_BALL_LOG"));
        if (ball_log && main_mode == 2) {
            struct TelemBall *b = &balls[0];
            printf("[ballf] %u pos %.4f %.4f %.4f vel %.5f %.5f %.5f\n",
                   frame_no, b->px, b->py, b->pz, b->vx, b->vy, b->vz);
        }
    }
    /* test hook: SMB2_PC_ROLL=1 switches a game in its READY phase to the staff
     * roll (SMD_GAME_READY_MAIN -> SMD_GAME_ROLL_INIT), like a Gecko code on Dolphin */
    {
        static int roll = -1;
        extern s32 main_mode, sub_mode, sub_mode_request;
        if (roll < 0) roll = getenv("SMB2_PC_ROLL") && atoi(getenv("SMB2_PC_ROLL")) > 0;
        if (roll && main_mode == 2 && sub_mode == 0x31)
            sub_mode_request = 0x53;
    }
    /* once-a-second heartbeat so logs show where the game is */
    {
        extern s32 main_mode, sub_mode, main_mode_request, sub_mode_request;
        if (frame_no % 60 == 0) {
            printf("[frame %u] mode %d/%d (req %d/%d)\n",
                   frame_no, main_mode, sub_mode, main_mode_request, sub_mode_request);
            if (main_mode == 2) {
                /* gameplay telemetry for scripted-input aiming */
                struct TelemGoal { float px, py, pz; };
                extern void *stagedef;
                struct TelemBall *b = &balls[0];
                printf("[ball] pos %.2f %.2f %.2f vel %.3f %.3f %.3f\n",
                       b->px, b->py, b->pz, b->vx, b->vy, b->vz);
                if (stagedef) {
                    u8 *sd = (u8 *)stagedef;
                    s32 gc = *(s32 *)(sd + 0x18);
                    /* goals is a 32-bit GC-layout pointer (StageDef DPTR):
                     * read 4 bytes, not a native pointer, on x64 */
                    struct TelemGoal *g = (struct TelemGoal *)(uintptr_t)*(u32 *)(sd + 0x1C);
                    if (gc > 0 && g)
                        printf("[goal] n=%d pos %.2f %.2f %.2f\n", (int)gc, g->px, g->py, g->pz);
                }
            }
            fflush(stdout);
        }
    }

    input_poll_gc_pads();
    platform_poll_events();
    {
        /* window closed (SDL_QUIT) or Quit to Desktop chosen in a menu:
         * pc_quit.c exits once no memory card job is running */
        extern void pc_quit_request(const char *reason);
        extern void pc_quit_poll(void);
        if (platform_should_close())
            pc_quit_request("window closed");
        pc_quit_poll();
    }
    {
        /* F1: PC settings overlay (pc_settings_menu.c), modal over the
         * finished-but-unpresented frame */
        extern int pc_settings_overlay_take_request(void);
        extern void pc_settings_overlay_run(void);
        extern int g_gx_presents_frames;
        if (pc_settings_overlay_take_request() && g_gx_presents_frames) {
            pc_settings_overlay_run();
            s_hfr_skip = 1;   /* the overlay presented its own frames */
        }
        /* Controller Sticks screen requested by the Options page */
        extern int pc_stick_screen_take_request(void);
        extern void pc_stick_screen_run(int captured);
        if (pc_stick_screen_take_request() && g_gx_presents_frames) {
            pc_stick_screen_run(0);
            s_hfr_skip = 1;
        }
        /* updater (pc_update_ui.c): results of the background check, the
         * release notes box (requested at a safe moment by pc_quit.c's
         * pc_menu_quit_tick) and the Updates screen (Options page) */
        extern void pc_update_poll(void);
        extern int pc_update_ui_run_requested(void);
        pc_update_poll();
        if (g_gx_presents_frames && pc_update_ui_run_requested())
            s_hfr_skip = 1;
        /* Memory Card screen (Options page) / drop-in import notice */
        extern int pc_memcard_ui_run_requested(void);
        if (g_gx_presents_frames && pc_memcard_ui_run_requested())
            s_hfr_skip = 1;
    }
    /* gx_start_new_frame (GXCopyDisp) presents on PC once the game's render
     * loop is running; swap here only during early boot before that. */
    {
        extern int g_gx_presents_frames;
        if (!g_gx_presents_frames)
            platform_swap_buffers();
    }

    /* Pace to 60 Hz on the nanosecond clock: SDL_Delay(ms) can overshoot by
     * a whole scheduler tick (15.6 ms when Windows ignores timeBeginPeriod). */
    {
        extern unsigned long long platform_get_time_ns(void);
        extern void platform_wait_until_ns(unsigned long long target_ns);
        static unsigned long long next_ns;
        const unsigned long long period_ns = 16666667ULL;
        unsigned long long now_ns = platform_get_time_ns();
        frame_no++;
        if (next_ns == 0)
            next_ns = now_ns;
        next_ns += period_ns;
        if (hfr_active()) {
            hfr_subframes(next_ns, period_ns);
            now_ns = platform_get_time_ns();
            hfr_adapt((long long)(now_ns - next_ns));
        }
        if (now_ns < next_ns) {
            PERF_BEGIN(tp);
            platform_wait_until_ns(next_ns);
            PERF_END(tp, PERF_T_PACE);
        }
        else if (now_ns > next_ns + 100000000ULL)
            next_ns = now_ns; /* fell behind; don't try to catch up */
        s_tick_start = platform_get_time_ns();
    }
}

/* The pc_gx_* write-gather pipe sinks live in render_opengl.c: they must be
 * compiled WITHOUT the decomp's gx.h in scope, because there GXPosition3f32
 * etc. are static inlines that call pc_gx_* (self-recursion otherwise). */
