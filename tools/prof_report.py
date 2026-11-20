#!/usr/bin/env python3 -I
"""Summarize prof.txt (fast sampler) per thread.
usage: prof_report.py prof.txt skate3.elf [top_n] [--from S] [--to S] [--last S] [--thread NAME]
  The ELF must match the NRO that produced prof.txt.
  --from/--to: seconds since profiler start (v2 files have "T <ms>" markers); --last S keeps the
  final S seconds. Without markers (v1 files) the window is applied by sample position, assuming
  hz samples per second per thread.
  Output per thread: self time (pc), inclusive time (function anywhere in pc/lr/frame chain, v2),
  and the most common call chains of the top self entries."""
import re, subprocess, sys, collections

import os
A2L = os.environ.get('A2L') or os.path.join(os.environ.get('DEVKITPRO', '/opt/devkitpro'),
                                           'devkitA64/bin/aarch64-none-elf-addr2line')
args = sys.argv[1:]
opts = {}
pos = []
i = 0
while i < len(args):
    if args[i].startswith('--'):
        opts[args[i][2:]] = args[i + 1]; i += 2
    else:
        pos.append(args[i]); i += 1
prof, elf = pos[0], pos[1]
top = int(pos[2]) if len(pos) > 2 else 25
t_from = float(opts['from']) if 'from' in opts else None
t_to = float(opts['to']) if 'to' in opts else None
t_last = float(opts['last']) if 'last' in opts else None
only_thread = opts.get('thread')

anchor_rt = None; hz = 100; names = {}; samples = []  # (thread, t_ms or None, [addrs])
cur_t = None; has_t = False
for line in open(prof):
    if line.startswith('anchor='):
        anchor_rt = int(re.match(r'anchor=0x([0-9a-f]+)', line).group(1), 16)
        m = re.search(r'hz=(\d+)', line)
        if m: hz = int(m.group(1))
    elif line.startswith('N '):
        parts = line.rstrip('\n').split(' ', 2)
        if len(parts) == 3: names[int(parts[1])] = parts[2]
    elif line.startswith('T '):
        try: cur_t = int(line.split()[1]); has_t = True
        except (IndexError, ValueError): pass
    elif line.startswith('S '):
        f = line.split()
        if len(f) < 4: continue
        try: addrs = [int(x, 16) for x in f[2:]]
        except ValueError: continue
        samples.append((int(f[1]), cur_t, addrs))

# Time window.
if has_t:
    tmax = max(t for _, t, _ in samples if t is not None) / 1000.0
    lo = t_from if t_from is not None else (tmax - t_last if t_last is not None else 0)
    hi = t_to if t_to is not None else 1e18
    samples = [s for s in samples if s[1] is not None and lo <= s[1] / 1000.0 <= hi]
    print(f'window: {lo:.1f}s .. {min(hi, tmax):.1f}s of {tmax:.1f}s')
elif t_from is not None or t_to is not None or t_last is not None:
    per = collections.defaultdict(list)
    for s in samples: per[s[0]].append(s)
    out = []
    for t, lst in per.items():
        n = len(lst); total_s = n / hz
        lo = t_from if t_from is not None else (total_s - t_last if t_last is not None else 0)
        hi = t_to if t_to is not None else total_s
        out += lst[int(max(0, lo) * hz):int(min(total_s, hi) * hz)]
    samples = out
    print('window applied by sample position (no T markers in this file)')

nm = subprocess.run(['nm', elf], capture_output=True, text=True).stdout
anchor_elf = int(next(l for l in nm.splitlines() if l.endswith(' rex_switch2_sampler_anchor')).split()[0], 16)
base = anchor_rt - anchor_elf
addrs = sorted({a - base for _, _, al in samples for a in al})
out = subprocess.run([A2L, '-e', elf, '-f', '-C', '-s'], input='\n'.join(hex(a) for a in addrs) + '\n',
                     capture_output=True, text=True).stdout.splitlines()
sym = {a: out[2 * k] for k, a in enumerate(addrs)}

def short(name, n=90):
    name = re.sub(r'\(.*\)( const)?$', '', name)
    return name if len(name) <= n else name[:n - 3] + '...'

by_thread = collections.defaultdict(list)
for t, _, al in samples:
    by_thread[re.sub(r' \(.*', '', names.get(t, '?'))].append([sym[a - base] for a in al])

for t, lst in sorted(by_thread.items(), key=lambda kv: -len(kv[1])):
    if only_thread and not t.startswith(only_thread): continue
    n = len(lst)
    print(f'\n=== {t}: {n} samples ===')
    c = collections.Counter(s[0] for s in lst)
    print('  -- self (pc):')
    for f, k in c.most_common(top):
        print(f'  {100*k/n:5.1f}%  {short(f, 140)}')
    deep = sum(1 for s in lst if len(s) > 2)
    if deep:
        inc = collections.Counter()
        for s in lst:
            for f in set(s): inc[f] += 1
        print(f'  -- inclusive (anywhere on stack; {100*deep/n:.0f}% of samples have frame chains):')
        for f, k in inc.most_common(top + 15):
            print(f'  {100*k/n:5.1f}%  {short(f, 140)}')
    print('  -- call chains of the top self entries (leaf <- caller <- ...):')
    for f, k in c.most_common(8):
        cc = collections.Counter(' <- '.join(short(x, 60) for x in s[1:6]) for s in lst if s[0] == f)
        print(f'   {short(f, 70)} ({100*k/n:.1f}%):')
        for chain, m in cc.most_common(4):
            print(f'      {100*m/n:5.1f}%  {chain}')
