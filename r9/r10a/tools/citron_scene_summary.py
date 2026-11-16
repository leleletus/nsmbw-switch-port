#!/usr/bin/env python3
"""citron_scene_summary.py <outdir> <title|map|lvl11> [--settle N]

Summary of one citron_scene.sh run (r9/base). Reads <outdir>/nwii.log (the NRO's log:
[NX] game ... every 5 s, [SCENE] from the _input_test NRO, [RPROF], [Gov], [NX] memory now)
and <outdir>/threads_raw.txt (per-thread CPU ticks of the citron process every ~5 s, each
sample tagged with how many "[NX] game" lines the log had at that moment).

Writes <outdir>/timeline.tsv (one row per 5 s window of the NRO) and prints the summary of
the target scene (after skipping the first N=2 windows of it, the load / fade-in):
fps per window, mean/min/max, citron per-thread CPU %, render_us/frame, GX/GL draws/frame,
[Gov] lines, clock factor.
"""
import collections
import os
import re
import statistics
import sys

SCENE_NAMES = {0: "BOOT", 1: "AUTO_SELECT", 2: "SELECT", 3: "WORLD_MAP", 4: "WORLD_9_DEMO",
               5: "STAGE", 6: "RESTART_CRSIN", 7: "CRSIN", 8: "MOVIE", 9: "GAMEOVER",
               10: "GAME_SETUP", 750: "INVALID"}

# citron host threads -> group
def group_of(comm):
    if comm.startswith("CPUCore_"):
        return "guest " + comm  # emulated Switch core (core 0: NRO main/render/GL driver, 1: game CPU)
    if comm in ("GPU", "GPUFencingThrea", "VulkanWorker", "TextureDecoder", "VSyncThread") or \
            comm.startswith("VkPipeline"):
        return "host GPU (Maxwell emu + Vulkan)"
    if comm in ("audio", "AudioRenderSyst", "DSP_AudioRender", "AudioMan", "DSP_OpusDecoder") or \
            comm.startswith("Audio") or comm.startswith("DSP_"):
        return "host audio (audren/DSP emu)"
    return "host other"


def parse_log(path, target):
    """Walk the log in order; every [NX] game line closes a window carrying the current state."""
    windows = []          # dicts
    rprof = []            # (state, dict)
    gov = []              # (window index, state, line)
    scene_events = []
    state = {"scene": -1, "phase": 0, "stage_count": 0, "title_done": False}
    mem = None
    num = lambda s: float(s)
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("[SCENE]"):
                m = re.search(r"frame=(\d+) t=([\d.]+) scene=(\d+)", line)
                if m:
                    sc = int(m.group(3))
                    if state["scene"] == 5 and sc != 5 and state["stage_count"] == 1:
                        state["title_done"] = True
                    if sc == 5:
                        state["stage_count"] += 1
                    state["scene"] = sc
                    scene_events.append((len(windows), int(m.group(1)), float(m.group(2)), sc))
                m = re.search(r"phase (\d+) starts", line)
                if m:
                    state["phase"] = int(m.group(1))
                    scene_events.append((len(windows), None, None, "phase %s" % m.group(1)))
                continue
            if line.startswith("[NX] game"):
                m = re.search(r"game ([\d.]+) fps, host loop ([\d.]+)/s(.*)", line)
                if not m:
                    continue
                rest = m.group(3)
                w = {"i": len(windows), "fps": num(m.group(1)), "host": num(m.group(2)),
                     "scene": state["scene"], "phase": state["phase"],
                     "target": in_target(state, target)}
                for key, rx in (("busy", r"busy max (\d+) ms"), ("clock", r"clock x([\d.]+)"),
                                ("slip", r"slip (\d+) ms"), ("interp", r"interp (\d+)/s")):
                    mm = re.search(rx, rest)
                    w[key] = float(mm.group(1)) if mm else None
                windows.append(w)
                continue
            if line.startswith("[RPROF]"):
                d = dict((k, float(v)) for k, v in re.findall(r"([a-z_/]+)=(-?[\d.]+)", line))
                rprof.append((len(windows), in_target(state, target), d))
                continue
            if line.startswith("[Gov]"):
                gov.append((len(windows), in_target(state, target), line.strip()))
                continue
            if line.startswith("[NX] memory now"):
                mem = line.strip()
    return windows, rprof, gov, scene_events, mem


def in_target(state, target):
    if target == "title":
        # first STAGE (the title course) until the first scene change after it
        return state["scene"] == 5 and state["stage_count"] == 1 and not state["title_done"]
    if target == "map":
        return state["phase"] >= 1 and state["scene"] == 3
    if target == "lvl11":
        return state["phase"] >= 2 and state["scene"] == 5
    return False


def parse_threads(path):
    """-> list of samples (t, nx, {tid: (comm, ticks, cpu)}, host) ; host = {temp, mhz[]}"""
    samples = []
    cur = None
    if not os.path.exists(path):
        return samples
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == "T":
                kv = dict(x.split("=", 1) for x in p[2:] if "=" in x)
                cur = (float(p[1]), int(kv.get("nx", 0) or 0), {}, {})
                samples.append(cur)
            elif p[0] == "H" and cur is not None:
                kv = dict(x.split("=", 1) for x in p[1:] if "=" in x)
                try:
                    cur[3]["temp"] = int(kv.get("temp") or 0) / 1000.0
                    cur[3]["mhz"] = [int(x) for x in kv.get("mhz", "").split(",") if x]
                except ValueError:
                    pass
            elif cur is not None and len(p) >= 3:
                try:
                    cpu = int(p[3]) if len(p) >= 4 else -1
                    cur[2][p[0]] = (p[1], int(p[2]), cpu)
                except ValueError:
                    pass
    return samples


def core_kind(cpu):
    # Core Ultra 5 125H: 0-7 P-cores (4 x HT), 8-15 E-cores, 16-17 LP-E cores
    if cpu < 0:
        return "?"
    return "P" if cpu < 8 else ("E" if cpu < 16 else "LP")


def thread_intervals(samples, hz=100.0):
    """-> list of (t0, t1, nx1, {comm: cpu%}) between consecutive samples."""
    out = []
    for a, b in zip(samples, samples[1:]):
        dt = b[0] - a[0]
        if dt <= 0:
            continue
        per = collections.Counter()
        for tid, (comm, ticks, _cpu) in b[2].items():
            if tid in a[2]:
                per[comm] += ticks - a[2][tid][1]
        out.append((a[0], b[0], b[1], {k: v / hz / dt * 100.0 for k, v in per.items()}))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    out, target = sys.argv[1], sys.argv[2]
    settle = 2
    if "--settle" in sys.argv:
        settle = int(sys.argv[sys.argv.index("--settle") + 1])
    log = os.path.join(out, "nwii.log")
    if not os.path.exists(log):
        print("no nwii.log in", out)
        return 1
    windows, rprof, gov, events, mem = parse_log(log, target)
    samples = parse_threads(os.path.join(out, "threads_raw.txt"))
    ivals = thread_intervals(samples)

    # window index -> thread interval: the interval whose end sample saw nx >= i+1 first
    def ival_for_window(i):
        for iv in ivals:
            if iv[2] >= i + 1:
                return iv
        return None

    # long gaps between samples = frozen slice (battery guard) or stalled sampler
    gaps = [(round(a[0]), round(b[0] - a[0], 1)) for a, b in zip(samples, samples[1:])
            if b[0] - a[0] > 9]
    try:
        pwarn = [l.strip() for l in open(os.path.join(out, "threads_raw.txt")) if l.startswith("W ")]
    except OSError:
        pwarn = []

    tgt = [w for w in windows if w["target"]]
    first_idx = tgt[0]["i"] if tgt else None
    steady = [w for w in tgt if w["i"] >= (first_idx or 0) + settle]

    # wall-clock second (from citron start) at which the target scene showed up
    reach_t = None
    if first_idx is not None:
        for s in samples:
            if s[1] >= first_idx + 1:
                reach_t = s[0]
                break

    lines = []
    P = lines.append
    P(f"== citron_scene {target}: {out}")
    try:
        P(open(os.path.join(out, "run_info.txt")).read().strip().splitlines()[0])
    except OSError:
        pass
    seq = " > ".join(
        (SCENE_NAMES.get(e[3], str(e[3])) + f"@{e[1]}f/{e[2]:.0f}s") if e[1] is not None else e[3]
        for e in events)
    P(f"scenes: {seq if seq else '(no [SCENE] lines: plain NRO?)'}")
    P(f"[NX] windows: {len(windows)} total, {len(tgt)} in the target scene, "
      f"{len(steady)} after skipping {settle} (load)")
    if reach_t is not None:
        P(f"target scene reached at ~{reach_t:.0f} s after citron start (window {first_idx})")
    else:
        P("TARGET SCENE NOT REACHED")
    if pwarn:
        P(f"WARNING: power profile left 'performance' during the run ({len(pwarn)}x): {pwarn[0]}  -> repeat the run")
    if gaps:
        P(f"WARNING: sampler gaps > 9 s (frozen slice / battery guard?): {gaps}")
    if steady:
        fps = [w["fps"] for w in steady]
        P(f"fps (5 s windows): mean {statistics.mean(fps):.2f}  median {statistics.median(fps):.2f}  "
          f"min {min(fps):.1f}  max {max(fps):.1f}  n={len(fps)}")
        P("fps per window: " + " ".join(f"{x:.1f}" for x in fps))
        hl = [w["host"] for w in steady]
        ck = [w["clock"] for w in steady if w["clock"] is not None]
        bz = [w["busy"] for w in steady if w["busy"] is not None]
        P(f"host loop/s mean {statistics.mean(hl):.1f}; guest clock factor mean x{statistics.mean(ck):.2f} "
          f"(min x{min(ck):.2f})" if ck else f"host loop/s mean {statistics.mean(hl):.1f}")
        if bz:
            P(f"render busy max (per window) median {statistics.median(bz):.0f} ms, max {max(bz):.0f} ms")
        # threads over the steady windows
        acc = collections.defaultdict(list)
        used = set()
        for w in steady:
            iv = ival_for_window(w["i"])
            if iv and id(iv) not in used:
                used.add(id(iv))
                for k, v in iv[3].items():
                    acc[k].append(v)
        n_iv = len(used)
        if n_iv:
            mean = {k: sum(v) / n_iv for k, v in acc.items()}
            P(f"citron threads, CPU % of one host core (mean of {n_iv} sampler intervals in the scene):")
            for k, v in sorted(mean.items(), key=lambda x: -x[1])[:16]:
                if v >= 0.5:
                    P(f"   {k:<18} {v:6.1f}%   [{group_of(k)}]")
            grp = collections.Counter()
            for k, v in mean.items():
                grp[group_of(k).split(" CPUCore")[0] if k.startswith("CPUCore_") else group_of(k)] += v
            P("   groups: " + ", ".join(f"{g} {v:.0f}%" for g, v in grp.most_common()))
            P(f"   total citron process {sum(mean.values()):.0f}%")
            # where the busy threads ran (last CPU at each sample) and host temp / MHz
            tw = [s for s in samples if s[1] >= first_idx + settle + 1] if first_idx is not None else []
            if tw:
                for comm in ("CPUCore_0", "CPUCore_1", "CPUCore_2", "GPU", "audio"):
                    kinds = collections.Counter()
                    mhz = []
                    for s in tw:
                        for tid, (c, _t, cpu) in s[2].items():
                            if c == comm:
                                kinds[core_kind(cpu)] += 1
                                if s[3].get("mhz") and 0 <= cpu < len(s[3]["mhz"]):
                                    mhz.append(s[3]["mhz"][cpu])
                    if kinds:
                        tot = sum(kinds.values())
                        P(f"   {comm:<10} ran on " + " ".join(f"{k} {v * 100 // tot}%" for k, v in kinds.most_common()) +
                          (f", at ~{statistics.mean(mhz):.0f} MHz" if mhz else ""))
                # cost per game frame, independent of the governor: thread CPU time / frames
                fmean = statistics.mean(w["fps"] for w in steady)
                for comm in ("CPUCore_1", "CPUCore_0"):
                    if comm in mean and fmean > 0:
                        ms = mean[comm] * 10.0 / fmean
                        mhz = []
                        for s in tw:
                            for tid, (c, _t, cpu) in s[2].items():
                                if c == comm and s[3].get("mhz") and 0 <= cpu < len(s[3]["mhz"]):
                                    mhz.append(s[3]["mhz"][cpu])
                        extra = f" = ~{ms * statistics.mean(mhz) / 1000:.0f} M host cycles" if mhz else ""
                        P(f"   {comm} per game frame: {ms:.1f} ms of host CPU{extra}")
                temps = [s[3]["temp"] for s in tw if s[3].get("temp")]
                if temps:
                    P(f"   package temp mean {statistics.mean(temps):.0f} C, max {max(temps):.0f} C")
    rs = [d for (wi, t, d) in rprof if t and (first_idx is None or wi >= first_idx + settle)]
    if rs:
        fr = sum(d.get("frames", 0) for d in rs) or 1
        wavg = lambda key: sum(d.get(key, 0) * d.get("frames", 0) for d in rs) / fr
        P(f"[RPROF] {len(rs)} lines, {int(fr)} frames: render_us/frame {wavg('render_us/frame'):.0f}, "
          f"render_max_us max {max(d.get('render_max_us', 0) for d in rs):.0f}, "
          f"gx_draws/frame {wavg('gx_draws/frame'):.0f}, gl_draws/frame {wavg('gl_draws/frame'):.0f}, "
          f"tex_uploads/line {statistics.mean(d.get('tex_uploads', 0) for d in rs):.1f}, "
          f"shaders new {sum(d.get('new', 0) for d in rs):.0f}")
    # per scene segment (consecutive windows with the same scene and phase)
    segs = []
    for w in windows:
        key = (w["scene"], w["phase"])
        if not segs or segs[-1][0] != key:
            segs.append((key, []))
        segs[-1][1].append(w)
    if segs:
        P("per scene segment (all windows, no settle): scene/phase  windows  fps mean  CPUCore_0 CPUCore_1")
        for (sc, ph), ws in segs:
            c0, c1 = [], []
            for w in ws:
                iv = ival_for_window(w["i"])
                if iv:
                    c0.append(iv[3].get("CPUCore_0", 0))
                    c1.append(iv[3].get("CPUCore_1", 0))
            P(f"   {SCENE_NAMES.get(sc, sc):<14}/{ph}  {len(ws):3d}  {statistics.mean(w['fps'] for w in ws):6.1f}"
              + (f"   {statistics.mean(c0):5.0f}% {statistics.mean(c1):5.0f}%" if c0 else ""))
    g_t = [g for g in gov if g[1]]
    P(f"[Gov] lines: {len(gov)} in the whole run, {len(g_t)} in the target scene")
    for g in g_t[:6]:
        P("   " + g[2][:150])
    if mem:
        P("last " + mem[:160])

    # timeline
    with open(os.path.join(out, "timeline.tsv"), "w") as f:
        cols = ["CPUCore_0", "CPUCore_1", "CPUCore_2", "CPUCore_3", "GPU", "VulkanWorker",
                "audio", "AudioRenderSyst", "DSP_AudioRender"]
        f.write("win\tt_wall\tscene\tphase\ttarget\tfps\thost_loop\tclock\tbusy_max_ms\t" +
                "\t".join(cols) + "\n")
        for w in windows:
            iv = ival_for_window(w["i"])
            th = iv[3] if iv else {}
            tw = f"{iv[1]:.0f}" if iv else ""
            f.write(f"{w['i']}\t{tw}\t{SCENE_NAMES.get(w['scene'], w['scene'])}\t{w['phase']}\t"
                    f"{int(w['target'])}\t{w['fps']}\t{w['host']}\t{w['clock']}\t{w['busy']}\t" +
                    "\t".join(f"{th.get(c, 0):.0f}" for c in cols) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
