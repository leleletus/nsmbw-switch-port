#!/usr/bin/env python3
# [r41m] tirones.py <bench_dir>...: por escena (entre líneas [DEMO] start), intervalos de fotograma del JUEGO de las
# líneas [PIPE] (cubos <=18, <=22, <=30, <=36, >36 ms): % de fotogramas > 22 ms y nº de > 30 ms por minuto; máximo de
# «loop» del render; fps mínimo de ventana ([NX] game … fps); y resumen de [LOGF] (vaciado del registro).
import re, sys, os

def scan(d):
    L = open(os.path.join(d, 'nwii.log'), 'rb').read().decode('utf-8', 'replace').splitlines()
    esc = 'arranque'; R = {}; logf = []
    def r(e): return R.setdefault(e, {'n': 0, 'b': [0] * 5, 'loopmax': 0.0, 'fps': [], 'secs': 0})
    for ln in L:
        m = re.match(r'\[DEMO\] start (\S+)', ln)
        if m: esc = m[1]; continue
        if ln.startswith('[PIPE] game'):
            m = re.search(r'\(<=18:(\d+) <=22:(\d+) <=30:(\d+) <=36:(\d+) >36:(\d+)\)', ln)
            lm = re.search(r'loop avg=[\d.]+ max=([\d.]+)', ln)
            if m:
                x = r(esc); b = [int(v) for v in m.groups()]
                x['b'] = [p + q for p, q in zip(x['b'], b)]; x['n'] += sum(b); x['secs'] += 2
                if lm: x['loopmax'] = max(x['loopmax'], float(lm[1]))
        m = re.match(r'\[NX\] game ([\d.]+) fps', ln)
        if m: r(esc)['fps'].append(float(m[1]))
        if ln.startswith('[LOGF]'): logf.append(ln)
    return R, logf

for d in sys.argv[1:]:
    d = d.rstrip('/'); R, logf = scan(d)
    print('##', os.path.basename(d))
    print(f"{'escena':20s} {'fot':>6s} {'>22ms%':>7s} {'>30/min':>8s} {'>36':>5s} {'loopmax':>8s} {'fps min':>8s}")
    for e, x in R.items():
        if e == 'arranque' or not x['n']: continue
        b = x['b']; mins = max(x['secs'], 1) / 60
        print(f"{e:20s} {x['n']:6d} {100 * (b[2] + b[3] + b[4]) / x['n']:7.2f} {(b[3] + b[4]) / mins:8.1f} {b[4]:5d} "
              f"{x['loopmax']:8.1f} {min(x['fps'][1:-1] or x['fps'] or [0]):8.1f}")
    if logf:
        v = [float(m) for l in logf for m in re.findall(r'vaciado en render: n \d+ media ([\d.]+) ms', l)]
        mx = [float(m) for l in logf for m in re.findall(r'vaciado en render: n \d+ media [\d.]+ ms max ([\d.]+) ms', l)]
        w = [float(m) for l in logf for m in re.findall(r'write\(\) n \d+ max ([\d.]+) ms', l)]
        bg = [float(m) for l in logf for m in re.findall(r'fondo: n \d+ media [\d.]+ ms max ([\d.]+) ms', l)]
        srt = lambda a: sorted(a)[len(a) // 2] if a else 0
        print(f"[LOGF] {len(logf)} líneas, modo {logf[-1].split('|')[0].split()[1]} | vaciado en render: media de medias "
              f"{sum(v) / max(len(v), 1):.2f} ms, máx {max(mx or [0]):.2f} ms (mediana de máx {srt(mx):.2f}) | write() máx "
              f"{max(w or [0]):.2f} ms (mediana {srt(w):.2f}) | fondo máx {max(bg or [0]):.2f} ms | {logf[-1].split('|')[-1].strip()}")
