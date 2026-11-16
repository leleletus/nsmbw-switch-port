#!/usr/bin/env python3
"""repoint.py <viejo> <nuevo> <nro> [extra_env]: cambia un trabajo de la cola (misma posicion) a otro nombre/NRO
y le anade variables (formato 'NWII_X="1";NWII_Y="1"'). El .job y el .nro viejos van a .cola/retirados_r2/.
Solo para trabajos R2_ que aun no han empezado."""
import glob, os, shutil, sys
Q = "/home/djvemo/nsmbw-port/tools/switchctl/.cola"
old, new, nro = sys.argv[1:4]
extra = sys.argv[4] if len(sys.argv) > 4 else ""
assert old.startswith("R2_") and new.startswith("R2_")
jobs = [j for j in glob.glob(f"{Q}/*_{old}.job")]
assert len(jobs) == 1, jobs
job = jobs[0]
seq = os.path.basename(job).split("_", 1)[0]
txt = open(job).read().replace(old, new)
if extra:
    esc = extra.replace('"', '\\"').replace(";", "\\;").replace(",", "\\,")
    lines = txt.splitlines()
    for i, l in enumerate(lines):
        if l.startswith("export DEBUG_EXTRA="):
            lines[i] = l + "\\;" + esc
    txt = "\n".join(lines) + "\n"
os.makedirs(f"{Q}/retirados_r2", exist_ok=True)
shutil.copy(nro, f"{Q}/{new}.nro")
open(f"{Q}/{seq}_{new}.job", "w").write(txt)
shutil.move(job, f"{Q}/retirados_r2/")
if os.path.exists(f"{Q}/{old}.nro"):
    shutil.move(f"{Q}/{old}.nro", f"{Q}/retirados_r2/")
print(open(f"{Q}/{seq}_{new}.job").read())
