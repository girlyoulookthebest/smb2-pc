/* SMB2_PACE_LOG=1: frame pacing log (docs/hfr.md section 7).
 *
 * Every present (content time, present id, CPU times), every tick (start,
 * end of logic) and every display event the backend reports (D3D12: DXGI
 * frame statistics polled on a thread) go to memory and are written to
 * stdout at exit, so logging does not disturb the timing it measures.
 * tools/hfr/pacestat.py turns the log into the pacing metrics. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
    char kind;          /* 'P' present, 'T' tick, 'G' GPU work of a present */
    signed char j;      /* present: in-between index (0 = tick frame) */
    signed char si;     /* present: sync interval passed */
    unsigned char flags;
    long tick;
    double c;           /* present: content time in ticks (tick T = T) */
    uint32_t pid;       /* present: backend present id (0 = none) */
    uint64_t a, b;      /* present: before / after the call; tick: start, logic end */
    double x;           /* present: planned display time (ns, 0 = none) */
} PaceRec;

static PaceRec* s_rec;
static long s_n, s_cap;
static int s_on = -1;
void (*g_pace_disp_dump)(void);   /* backend: writes its display records */

static void pace_log_dump(void) {
    long i;
    {
        extern uint64_t platform_counter_origin(uint64_t* freq);
        uint64_t f, o = platform_counter_origin(&f);
        printf("[pacelog] begin %ld qpc0 %llu freq %llu\n", s_n, (unsigned long long)o, (unsigned long long)f);
    }
    for (i = 0; i < s_n; i++) {
        const PaceRec* r = &s_rec[i];
        if (r->kind == 'P')
            printf("[pp] %ld %d %.4f %u %llu %llu %d %.0f %u\n", r->tick, r->j, r->c, r->pid,
                   (unsigned long long)r->a, (unsigned long long)r->b, r->si, r->x, r->flags);
        else if (r->kind == 'G')
            printf("[pg] %u %llu %llu\n", r->pid, (unsigned long long)r->a, (unsigned long long)r->b);
        else
            printf("[pt] %ld %llu %llu\n", r->tick, (unsigned long long)r->a, (unsigned long long)r->b);
    }
    if (g_pace_disp_dump) g_pace_disp_dump();
    printf("[pacelog] end\n");
    fflush(stdout);
}

int pace_log_on(void) {
    if (s_on < 0) {
        const char* e = getenv("SMB2_PACE_LOG");
        s_on = e && atoi(e) != 0;
        if (s_on) {
            s_cap = 1L << 20;
            s_rec = (PaceRec*)calloc((size_t)s_cap, sizeof *s_rec);
            if (!s_rec) s_on = 0;
            else atexit(pace_log_dump);
        }
    }
    return s_on;
}

void pace_log_present(long tick, int j, double c, uint32_t pid, uint64_t t0, uint64_t t1,
                      int si, double plan_ns, unsigned flags) {
    PaceRec* r;
    if (!pace_log_on() || s_n >= s_cap) return;
    r = &s_rec[s_n++];
    r->kind = 'P'; r->tick = tick; r->j = (signed char)j; r->c = c; r->pid = pid;
    r->a = t0; r->b = t1; r->si = (signed char)si; r->x = plan_ns; r->flags = (unsigned char)flags;
}

void pace_log_tick(long tick, uint64_t t_start, uint64_t t_logic) {
    PaceRec* r;
    if (!pace_log_on() || s_n >= s_cap) return;
    r = &s_rec[s_n++];
    r->kind = 'T'; r->tick = tick; r->a = t_start; r->b = t_logic;
}

void pace_log_gpu(unsigned pid, uint64_t t0, uint64_t t1) {
    PaceRec* r;
    if (!pace_log_on() || s_n >= s_cap) return;
    r = &s_rec[s_n++];
    r->kind = 'G'; r->pid = pid; r->a = t0; r->b = t1;
}
