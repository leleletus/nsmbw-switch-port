#!/usr/bin/env python3
# [r41m] tabla_m.py <bench_dir>...: tabla comparativa por escena (fps dibujados · fps juego · ms juego/fot REAL ·
# render ms/fot) con r31t/tools/por_escena_real.py, fiesta (r36r/tools/fiesta.py), audio por escena (AID/s, underruns
# nuevos; r38c/tools/hilos.py), underruns totales, relojes, demos, imagen y líneas [VIGIA].
import subprocess, sys, re, os
T = '/home/djvemo/nsmbw-port/r9'
ESC = ['desierto4j', 'agua_pinguino', 'cielo_paratroopas', 'nubes_plataformas', 'castillo']

def run(*a):
    return subprocess.run(['python3', '-I', *a], capture_output=True, text=True).stdout

rows = {}
for d in sys.argv[1:]:
    d = d.rstrip('/'); n = os.path.basename(d); L = open(d + '/nwii.log', 'rb').read().decode('utf-8', 'replace')
    r = rows[n] = {}
    esc = None
    for ln in run(T + '/r31t/tools/por_escena_real.py', d).splitlines():
        if ln.startswith('== '): esc = ln[3:].strip()
        elif ln.startswith('bench_') and esc:
            f = ln.split()
            r[esc] = f'{f[1]} · {f[2]} · {f[4]} · {f[6]}'
    for ln in run(T + '/r36r/tools/fiesta.py', d).splitlines():
        m = re.search(r'fiesta .*fps juego\s+([\d.]+) dib\s+([\d.]+) \| render draw\s+([\d.]+).*trabajador\s+([\d.]+)', ln)
        if m: r['fiesta'] = f'{m[2]} · {m[1]} · – · {m[3]} (trab {m[4]})'
    for ln in run(T + '/r38c/tools/hilos.py', d).splitlines():
        f = ln.split()
        if f and (f[0] in ESC or f[0] == 'castillo/fiesta') and '|' in ln:
            p = ln.split('|')
            aid = p[2].split()[0]; und = p[-1].split()[-1]
            r['aud_' + f[0]] = f'{aid}/{und}'
    u = re.findall(r'underruns (\d+)', L)
    r['und_total'] = u[-1] if u else '?'
    r['relojes'] = ' '.join(f'{c}×{v}' for v, c in sorted({x: L.count(x) for x in set(re.findall(r'cpu \d+ MHz, gpu \d+ MHz', L))}.items()))
    r['demos'] = str(len(re.findall(r'^\[DEMO\] start', L, re.M)))
    r['vigia'] = str(len(re.findall(r'^\[VIGIA\] #', L, re.M)))
    try:
        rs = open(d + '/resumen.txt').read()
        r['resumen'] = ' '.join(x.strip() for x in rs.splitlines() if re.search('windows, mean|IMAGEN|CRASH|CONGELADA|RELOJ|FATAL', x))
    except OSError: r['resumen'] = '?'

names = list(rows)
print('| escena | ' + ' | '.join(names) + ' |'); print('|---' * (len(names) + 1) + '|')
for e in ESC + ['fiesta']:
    print(f'| {e} | ' + ' | '.join(rows[n].get(e, '–') for n in names) + ' |')
print('\nAudio por escena (AID/s / underruns nuevos):')
for e in ESC + ['castillo/fiesta']:
    print(f'| {e} | ' + ' | '.join(rows[n].get('aud_' + e, '–') for n in names) + ' |')
for k in ['und_total', 'demos', 'vigia', 'relojes', 'resumen']:
    print(f'| {k} | ' + ' | '.join(rows[n][k] for n in names) + ' |')
