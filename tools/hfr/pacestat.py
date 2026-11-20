#!/usr/bin/env python3
"""Frame pacing metrics from an SMB2_PACE_LOG=1 run (docs/hfr.md section 7).

usage: pacestat.py log.txt [first_tick last_tick] [--steps]

Reads the [pp] (present), [pt] (tick) and [pd] (D3D12 frame statistics)
records.  A present's display refresh is the PresentRefreshCount of the first
frame-statistics sample that names it; its display time comes from the
vblank grid fitted to the (SyncRefreshCount, SyncQPCTime) samples.

Metrics (for the presents of the tick range):
  speed sd     content advance per refresh between consecutive shown frames
               (ticks / refreshes; ideal = refresh period / 16.67 ms), its
               standard deviation over the shown frames: 0 = even motion
  residual     display time - content time (ms), detrended: sd and p99-p1
  cadence      refreshes each shown frame stays on screen (histogram)
  shown fps    distinct frames on screen per second
  per tick     presents per tick (histogram)
  lat          present call return -> display (ms)
  input        tick start (logic, input already read) -> display of the
               first frame showing that tick fully (content >= tick), and of
               the first frame showing any of it (content > tick - 1)
GL has no display feedback: only the present-side numbers are printed.
"""
import sys, statistics, collections, math

T_MS = 1000.0 / 60.0


def pct(v, q):
    v = sorted(v)
    if not v:
        return float('nan')
    return v[min(len(v) - 1, max(0, int(q * (len(v) - 1) + 0.5)))]


def main():
    dcfile = None
    if '--dc' in sys.argv:
        k = sys.argv.index('--dc')
        dcfile = sys.argv[k + 1]
        del sys.argv[k:k + 2]
    per_arg = None
    if '--period' in sys.argv:   # refresh period in ms (dispcap times jitter)
        k = sys.argv.index('--period')
        per_arg = float(sys.argv[k + 1]) * 1e6
        del sys.argv[k:k + 2]
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    steps_out = '--steps' in sys.argv
    path = args[0]
    lo = int(args[1]) if len(args) > 1 else None
    hi = int(args[2]) if len(args) > 2 else None
    pres, ticks, disp, gpu = [], {}, [], {}
    for l in open(path, 'rb').read().decode('latin1').splitlines():
        if l.startswith('[pp] '):
            f = l.split()
            pres.append(dict(tick=int(f[1]), j=int(f[2]), c=float(f[3]), pid=int(f[4]),
                             t0=int(f[5]), t1=int(f[6]), si=int(f[7]), plan=float(f[8]),
                             flags=int(f[9]) if len(f) > 9 else 0))
        elif l.startswith('[pg] '):
            f = l.split()
            gpu[int(f[1])] = (int(f[2]), int(f[3]))
        elif l.startswith('[pacelog] begin') and 'qpc0' in l:
            f = l.split()
            qpc0, qf = int(f[4]), int(f[6])
        elif l.startswith('[pt] '):
            f = l.split()
            ticks[int(f[1])] = (int(f[2]), int(f[3]))
        elif l.startswith('[pd] '):
            f = l.split()
            disp.append(dict(t=int(f[1]), pc=int(f[2]), prc=int(f[3]), src=int(f[4]),
                             sync=int(f[5]), mode=int(f[6])))
    if lo is None:
        lo = min(p['tick'] for p in pres)
    if hi is None:
        hi = max(p['tick'] for p in pres)
    sel = [p for p in pres if lo <= p['tick'] <= hi and p['c'] > 0]
    print('presents %d in ticks %d-%d' % (len(sel), lo, hi))
    # presents per tick: count by the tick number the present belongs to
    per = collections.Counter()
    for p in sel:
        per[int(round(p['c'] + 0.4999))] += 1
    hist = collections.Counter(per[t] for t in range(lo + 1, hi))
    print('presents per tick:', dict(sorted(hist.items())))
    if dcfile:
        # display side (tools/hfr/dispcap): desktop frames and the stamped
        # present id (low 16 bits); refresh numbers from the frame times
        dc = []
        for l in open(dcfile, 'rb').read().decode('latin1').splitlines():
            f = l.split()
            if len(f) == 5 and f[0] == '[dc]' and f[4] != '-1':
                t = (int(f[1]) - qpc0) * 1e9 / qf
                dc.append((t, int(f[4])))
        if dc:
            dd = sorted(b[0] - a[0] for a, b in zip(dc, dc[1:]) if b[0] > a[0])
            dd = [x for x in dd if x > 2e6]
            low = [x for x in dd if x < dd[0] * 1.5]
            per0 = per_arg or low[len(low) // 2]   # one refresh (some frames stay just one)
            t0 = dc[0][0]
            disp = []
            seen = set()
            for t, i16 in dc:
                r = int(round((t - t0) / per0))
                disp.append(dict(t=t, pc=('dc', i16), prc=r, src=r, sync=int(t), mode=-4))
            for p in pres:
                p['pid'] = ('dc', p['pid'] & 0xFFFF)
            print('display side: %d desktop frames from dispcap, refresh ~%.3f ms' % (len(dc), per0 / 1e6))
    if not disp:
        iv = [(b['t1'] - a['t1']) / 1e6 for a, b in zip(sel, sel[1:])]
        if iv:
            print('present interval ms: mean %.2f sd %.2f p1 %.2f p99 %.2f' %
                  (statistics.mean(iv), statistics.pstdev(iv), pct(iv, .01), pct(iv, .99)))
        return
    # vblank grid: refresh count -> time, least squares over the samples
    # around the window
    t_lo = min(p['t0'] for p in sel) - 2e9
    t_hi = max(p['t1'] for p in sel) + 2e9
    g = [(d['src'], d['sync']) for d in disp if d['sync'] and t_lo <= d['sync'] <= t_hi]
    g = sorted(set(g))
    n = len(g)
    mx = sum(x for x, _ in g) / n
    my = sum(y for _, y in g) / n
    sxx = sum((x - mx) ** 2 for x, _ in g)
    per_ns = sum((x - mx) * (y - my) for x, y in g) / sxx
    res = [y - (my + (x - mx) * per_ns) for x, y in g]
    print('refresh %.3f Hz (period %.4f ms), vblank fit residual sd %.3f ms, %d samples' %
          (1e9 / per_ns, per_ns / 1e6, statistics.pstdev(res) / 1e6, n))
    modes = collections.Counter(d['mode'] for d in disp if t_lo <= d['t'] <= t_hi)
    print('composition mode samples (0 composed, 1 overlay, 2 none, -1 unknown):', dict(modes))

    def rtime(r):
        return my + (r - mx) * per_ns
    first = {}
    for d in disp:
        if d['pc'] not in first:
            first[d['pc']] = d['prc']
    shown = [p for p in sel if p['pid'] in first]
    missing = len(sel) - len(shown)
    for p in shown:
        p['r'] = first[p['pid']]
        p['d'] = rtime(p['r'])
    shown.sort(key=lambda p: (p['r'], p['pid']))
    # a later present on the same refresh replaces the earlier (never seen)
    uniq = []
    for p in shown:
        if uniq and uniq[-1]['r'] == p['r']:
            uniq[-1] = p
        else:
            uniq.append(p)
    ideal = per_ns / 1e6 / T_MS
    sp, cad, dc_dr = [], collections.Counter(), collections.Counter()
    for a, b in zip(uniq, uniq[1:]):
        dr = b['r'] - a['r']
        dc = b['c'] - a['c']
        sp.append(dc / dr)
        cad[dr] += 1
        dc_dr[(round(dc, 2), dr)] += 1
    # residual: display time vs content time, detrended
    rr = [p['d'] / 1e6 - p['c'] * T_MS for p in uniq]
    xs = list(range(len(rr)))
    mxx = sum(xs) / len(xs)
    myy = sum(rr) / len(rr)
    b1 = sum((x - mxx) * (y - myy) for x, y in zip(xs, rr)) / max(1e-9, sum((x - mxx) ** 2 for x in xs))
    rd = [y - (myy + (x - mxx) * b1) for x, y in zip(xs, rr)]
    dur = (uniq[-1]['d'] - uniq[0]['d']) / 1e9
    print('shown %d of %d presents (%d never shown / replaced), %.1f distinct frames/s' %
          (len(uniq), len(sel), missing + len(shown) - len(uniq), (len(uniq) - 1) / dur if dur > 0 else 0))
    print('content speed per refresh (ideal %.4f): mean %.4f sd %.4f min %.3f max %.3f' %
          (ideal, statistics.mean(sp), statistics.pstdev(sp), min(sp), max(sp)))
    print('residual display-content: sd %.3f ms, p1..p99 %.2f ms' %
          (statistics.pstdev(rd), pct(rd, .99) - pct(rd, .01)))
    print('cadence (refreshes on screen: frames):', dict(sorted(cad.items())))
    print('top (content step, refreshes):', sorted(dc_dr.items(), key=lambda x: -x[1])[:8])
    gs = [p for p in shown if p['pid'] in gpu and gpu[p['pid']][0]]
    if gs:
        a = [(gpu[p['pid']][0] - p['t0']) / 1e6 for p in gs]
        b = [(gpu[p['pid']][1] - gpu[p['pid']][0]) / 1e6 for p in gs]
        c = [(p['d'] - gpu[p['pid']][1]) / 1e6 for p in gs]
        print('present call -> GPU start ms: p50 %.2f p99 %.2f | GPU ms p50 %.2f p99 %.2f | GPU end -> shown ms: p1 %.2f p50 %.2f p99 %.2f' %
              (pct(a, .5), pct(a, .99), pct(b, .5), pct(b, .99), pct(c, .01), pct(c, .5), pct(c, .99)))
    pl = [p for p in shown if p['plan'] > 0]
    if pl:
        e = collections.Counter(round((p['d'] - p['plan']) / per_ns) for p in pl)
        print('shown - planned refresh (grid pacer):', dict(sorted(e.items())))
    lat = [(p['d'] - p['t1']) / 1e6 for p in shown]
    print('present -> display ms: mean %.2f p50 %.2f p99 %.2f' %
          (statistics.mean(lat), pct(lat, .5), pct(lat, .99)))
    full, any_ = [], []
    for t in range(lo + 1, hi):
        if t not in ticks or not ticks[t][0]:
            continue
        s = ticks[t][0]
        fa = [p['d'] for p in uniq if p['c'] >= t - 1e-6]
        fb = [p['d'] for p in uniq if p['c'] > t - 1 + 1e-6]
        if fa:
            full.append((min(fa) - s) / 1e6)
        if fb:
            any_.append((min(fb) - s) / 1e6)
    if full:
        print('input: tick start -> tick fully shown ms: mean %.2f p50 %.2f p99 %.2f; first sight mean %.2f p50 %.2f' %
              (statistics.mean(full), pct(full, .5), pct(full, .99), statistics.mean(any_), pct(any_, .5)))
    # content latency: display time - the moment the content belongs to
    # (tick N's start + alpha of a tick), i.e. tick start -> on screen
    cl = []
    for p in uniq:
        n = int(math.ceil(p['c'] - 1e-9))
        if n in ticks and ticks[n][0]:
            cl.append((p['d'] - (ticks[n][0] - (n - p['c']) * T_MS * 1e6)) / 1e6)
    if cl:
        print('content latency (tick start -> its state on screen) ms: mean %.2f p50 %.2f p99 %.2f' %
              (statistics.mean(cl), pct(cl, .5), pct(cl, .99)))
    logic = [(ticks[t][1] - ticks[t][0]) / 1e6 for t in range(lo, hi) if t in ticks and ticks[t][0]]
    if logic:
        print('tick logic+draw ms: mean %.2f p50 %.2f p99 %.2f' % (statistics.mean(logic), pct(logic, .5), pct(logic, .99)))
    if steps_out:
        for p in uniq:
            print('step r %d c %.4f tick %d j %d si %d d %.3f' % (p['r'], p['c'], p['tick'], p['j'], p['si'], p['d'] / 1e6))


if __name__ == '__main__':
    main()
