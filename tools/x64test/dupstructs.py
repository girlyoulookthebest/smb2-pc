#!/usr/bin/env python3
"""Find struct types declared more than once (local copies in several TUs)
whose copies agree on i686 but not on x64.

usage: dupstructs.py <build64 dir with compile_commands.json> <llvm-mingw root>

Every C TU of the x64 build is compiled twice with -fsyntax-only and
-fdump-record-layouts-complete (as x86_64 and as i686), and the top-level
field offsets of each struct copy are mapped i686 -> x64.  Two copies with the
same name that put a field at the same i686 offset but different x64
offsets, or that have the same i686 size but different x64 sizes, are
reported: one of them is a 32-bit-only overlay of the real struct (e.g.
ball_draw.c's BackgroundInfo, ballEnvFunc at 0x94).  Unrelated structs that
merely share a name show up too; judge by the field names.  Configure the
build with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON first."""
import json, os, re, shlex, subprocess, sys, collections
from concurrent.futures import ThreadPoolExecutor

build, tc = sys.argv[1], sys.argv[2]
cc = json.load(open(os.path.join(build, 'compile_commands.json')))

def dump(e, tgt):
    a = shlex.split(e['command'])
    if tgt == 'i686':
        a[0] = os.path.join(tc, 'bin', 'i686-w64-mingw32-clang')
    out, i = [a[0]], 1
    while i < len(a):
        if a[i] in ('-o', '-c'):
            i += 2
            continue
        out.append(a[i]); i += 1
    r = subprocess.run(out + ['-fsyntax-only', '-Xclang', '-fdump-record-layouts-complete', '-w', e['file']],
                       cwd=e['directory'], capture_output=True, text=True)
    recs = {}
    for blk in r.stdout.split('*** Dumping AST Record Layout')[1:]:
        lines = blk.strip('\n').split('\n')
        m = re.match(r'\s*0 \| (.*)$', lines[0])
        if not m or 'toolchains' in m.group(1) or 'unnamed' in m.group(1):
            continue
        fields, size = [], None
        for l in lines[1:]:
            mm = re.match(r'\s*(\d+)(?::\d+-\d+)? \|( +)(.*)$', l)
            if mm:
                if len(mm.group(2)) == 3:
                    fields.append((int(mm.group(1)), mm.group(3).strip()))
                continue
            ms = re.search(r'\[sizeof=(\d+)', l)
            if ms:
                size = int(ms.group(1)); break
        recs.setdefault(m.group(1).strip(), (tuple(fields), size))
    return recs

def work(e):
    return e['file'], dump(e, 'x64'), dump(e, 'i686')

files = [e for e in cc if e['file'].endswith('.c') and shlex.split(e['command'])[0].endswith('clang')]
defs = collections.defaultdict(dict)
for f, x, i in ThreadPoolExecutor(os.cpu_count() or 4).map(work, files):
    for n, (fx, sx) in x.items():
        if n in i and n.startswith(('struct ', 'union ')):
            fi, si = i[n]
            defs[n].setdefault((fx, sx, fi, si), set()).add(os.path.basename(f))
bad = 0
for n, d in sorted(defs.items()):
    ks = list(d)
    probs = set()
    for a in range(len(ks)):
        for b in range(a + 1, len(ks)):
            A, B = ks[a], ks[b]
            ma = {io: (xo, s) for (io, s), (xo, _) in zip(A[2], A[0])}
            mb = {io: (xo, s) for (io, s), (xo, _) in zip(B[2], B[0])}
            for io in set(ma) & set(mb):
                if ma[io][0] != mb[io][0]:
                    probs.add('  i686 0x%X: x64 0x%X (%s) vs 0x%X (%s)' % (io, ma[io][0], ma[io][1], mb[io][0], mb[io][1]))
            if A[1] != B[1] and A[3] == B[3]:
                probs.add('  sizeof x64 %d vs %d (i686 both %d)' % (A[1], B[1], A[3]))
    if probs:
        bad += 1
        print(n)
        for k, v in d.items():
            print('   x64 size %d, i686 size %d: %s' % (k[1], k[3], ' '.join(sorted(v))[:200]))
        print('\n'.join(sorted(probs)[:12]))
print('%d struct names with disagreeing copies' % bad)
