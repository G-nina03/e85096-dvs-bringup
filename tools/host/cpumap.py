#!/usr/bin/env python3
"""cpumap.py DIR [TOP]: per-CPU time split from tools/board/cpumap-cap.sh output (who ran where, hard/soft irq,
idle), each CPU's time between two trace events going to what was running then (11.30)."""
import collections
import re
import sys

D = sys.argv[1]
TOP = int(sys.argv[2]) if len(sys.argv) > 2 else 6
LINE = re.compile(r'^\s*(.+?)-(\d+)\s+(?:\(\s*([-\d]+)\)\s+)?\[(\d+)\]\s+\S+\s+([\d.]+):\s+(\w+):\s+(.*)$')

# tid -> label from the thread list taken after the trace
ps = {}
procs = {}
for ln in open(D + '/ps.txt').read().splitlines()[1:]:
    f = ln.split(None, 5)
    pid, tid, comm = int(f[0]), int(f[1]), f[4]
    args = f[5] if len(f) > 5 else ''
    if pid == tid:
        procs[pid] = comm
    ps[tid] = (pid, comm, args)


def label(tid, comm, tgid=None):
    if tid == 0:
        return 'idle'
    if tid not in ps and tgid in ps and 'ov5647-cap.py' in ps[tgid][2]:
        return 'dvslive:exited threads'           # web connection threads: the only short-lived ones
    if tid in ps:
        pid, c, args = ps[tid]
        if 'ov5647-cap.py' in args:
            return 'dvslive:' + ('main' if pid == tid else c + '/' + str(tid))
        return c if pid == tid else '%s:%s' % (procs.get(pid, '?'), c)
    return comm + ' (exited)'


# times go to (tid, comm) or 'irq:...'/'softirq:...'; tasks get their label once every tid's tgid is known
raw = collections.defaultdict(lambda: collections.defaultdict(float))
tgids = {}
last = {}
cur = {}
stack = collections.defaultdict(list)
first = {}
t0 = t1 = None
for ln in open(D + '/trace.txt', errors='replace'):
    m = LINE.match(ln)
    if not m:
        continue
    comm, tid, cpu, ts, ev, rest = m.group(1), int(m.group(2)), int(m.group(4)), float(m.group(5)), m.group(6), m.group(7)
    if m.group(3) and m.group(3).isdigit():
        tgids[tid] = int(m.group(3))
    t0 = ts if t0 is None else t0
    t1 = ts
    if cpu not in last:                  # before its first event: the task that event interrupted or switched out
        pv = re.search(r'prev_comm=(.*) prev_pid=(\d+)', rest) if ev == 'sched_switch' else None
        first[cpu] = (ts, (int(pv.group(2)), pv.group(1)) if pv else (tid, comm))
    else:
        who = stack[cpu][-1] if stack[cpu] else cur.get(cpu)
        if who:
            raw[cpu][who] += ts - last[cpu]
    last[cpu] = ts
    if ev == 'sched_switch':
        n = re.search(r'next_comm=(.*) next_pid=(\d+)', rest)
        cur[cpu] = (int(n.group(2)), n.group(1))
    else:
        if not stack[cpu]:
            cur[cpu] = (tid, comm)
        if ev == 'irq_handler_entry':
            stack[cpu].append('irq:' + re.search(r'name=(\S+)', rest).group(1))
        elif ev == 'softirq_entry':
            stack[cpu].append('softirq:' + re.search(r'action=(\w+)', rest).group(1))
        elif ev in ('irq_handler_exit', 'softirq_exit') and stack[cpu]:
            stack[cpu].pop()

span = t1 - t0
for cpu, (ts, who) in first.items():   # the ends of the window on CPUs with few events
    raw[cpu][who] += ts - t0
    raw[cpu][stack[cpu][-1] if stack[cpu] else cur.get(cpu, who)] += t1 - last[cpu]
acc = collections.defaultdict(lambda: collections.defaultdict(float))
for cpu, d in raw.items():
    for k, v in d.items():
        acc[cpu][k if isinstance(k, str) else label(k[0], k[1], tgids.get(k[0]))] += v


def stat(n):
    out = {}
    for ln in open('%s/stat%d' % (D, n)):
        f = ln.split()
        if re.match(r'cpu\d+$', f[0]):
            out[int(f[0][3:])] = [int(x) for x in f[1:8]]
    return out


s0, s1 = stat(0), stat(1)


def irqs(n):
    rows = open('%s/irq%d' % (D, n)).read().splitlines()
    ncpu = len(rows[0].split())
    out = {}
    for ln in rows[1:]:
        f = ln.split()
        if len(f) < ncpu + 1 or not f[0].endswith(':'):
            continue
        try:
            cnt = [int(x) for x in f[1:ncpu + 1]]
        except ValueError:
            continue
        out[f[0][:-1] + ' ' + ' '.join(f[ncpu + 1:])[-28:]] = cnt
    return out, ncpu


i0, ncpu = irqs(0)
i1, _ = irqs(1)


def freqs(n):
    out = {}
    for ln in open('%s/freq%d' % (D, n)):
        f = ln.split()
        out[f[0]] = (f[1], {int(f[i]): int(f[i + 1]) for i in range(2, len(f) - 1, 2)})
    return out


f0, f1 = freqs(0), freqs(1)
cpufreq = {}
for cpus, (gov, tis) in f1.items():
    d = {k: v - f0[cpus][1].get(k, 0) for k, v in tis.items()}
    tot = sum(d.values()) or 1
    avg = sum(k * v for k, v in d.items()) / tot / 1000
    top = max(d, key=d.get) // 1000
    for c in cpus.split(','):
        for x in (range(int(c.split('-')[0]), int(c.split('-')[1]) + 1) if '-' in c else [int(c)]):
            cpufreq[x] = '%s avg %.0f MHz (mostly %d)' % (gov, avg, top)

print('trace %.2f s, %d CPUs' % (span, ncpu))
for cpu in sorted(acc):
    a, b = s0[cpu], s1[cpu]
    d = [y - x for x, y in zip(a, b)]
    tot = sum(d) or 1
    busy = 100.0 * (tot - d[3] - d[4]) / tot
    traced_idle = 100 * acc[cpu].get('idle', 0) / span
    irq_s = sorted(((i1[k][cpu] - v[cpu], k) for k, v in i0.items() if k in i1 and i1[k][cpu] > v[cpu]), reverse=True)
    print('\nCPU%d  busy %.1f%% (/proc/stat), traced idle %.1f%%, %s' % (cpu, busy, traced_idle, cpufreq.get(cpu, '?')))
    items = sorted(((v, k) for k, v in acc[cpu].items() if k != 'idle'), reverse=True)
    for v, k in items[:TOP]:
        print('   %5.1f%%  %s' % (100 * v / span, k))
    rest = sum(v for v, k in items[TOP:])
    if rest:
        print('   %5.1f%%  (%d others)' % (100 * rest / span, len(items) - TOP))
    if irq_s:
        print('   irq/s: ' + ', '.join('%s %.0f' % (k.split()[0] if k.startswith('IPI') else k.split()[-1], n / span)
                                       for n, k in irq_s[:4]))

tot = collections.defaultdict(float)
for cpu in acc:
    for k, v in acc[cpu].items():
        if k != 'idle':
            tot[k] += v
print('\nall CPUs, %% of one core:')
for v, k in sorted(((v, k) for k, v in tot.items()), reverse=True)[:14]:
    where = ' '.join('%d:%.0f' % (c, 100 * acc[c][k] / span) for c in sorted(acc) if acc[c].get(k, 0) / span > 0.005)
    print('   %5.1f%%  %-40s cpus %s' % (100 * v / span, k[:40], where))
print('\n' + open(D + '/overrun').read().strip().replace(D, ''))
