#!/usr/bin/env python3
"""Which of a map's materials each of the game's draw calls draws.

  trace_materials.py TRACE MAP [--draws]

TRACE is a refshot trace (refcompare.py --trace), MAP the .Challenge.Gbx it
shows. The textures a draw binds are matched to the files of the map's
materials (the `materials` tool: tmuf_track_visuals) by their pictures, as
tiny thumbnails (the game keeps its pictures upside down: both ways are
tried). The result, per pixel shader of the trace: the materials it draws,
so that each family of the game's shaders can be ported from its own
assembly (ps_N.asm) for exactly the materials that use it. --draws lists
every draw instead.
"""
import argparse
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../../.."))
PACKS = os.path.join(ROOT, "data/games/tmuf/Packs")
INSPECT = os.path.expanduser("~/software/tmuf_physics/build/tmuf_inspect")
CACHE = os.path.join(HERE, ".thumbs")

sys.path.insert(0, HERE)
from trace_summary import parse  # noqa: E402


def thumb(path):
    """8x8 grey thumbnail of a picture's first level, or None."""
    raw = subprocess.run(["magick", path + "[0]", "-alpha", "off", "-colorspace", "gray", "-resize", "8x8!",
                          "-depth", "8", "gray:-"], capture_output=True).stdout
    if len(raw) != 64:
        return None
    return np.frombuffer(raw, np.uint8).astype(np.float32).reshape(8, 8)


def file_thumb(path):
    """The thumbnail of a material's texture file, on disk or in the packs."""
    os.makedirs(CACHE, exist_ok=True)
    local = path
    if not os.path.isabs(path):
        local = os.path.join(CACHE, re.sub(r"[^A-Za-z0-9_.-]", "_", path))
        if not os.path.exists(local):
            subprocess.run([INSPECT, "catp", PACKS, path, local], capture_output=True,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
        if not os.path.exists(local):
            return None
    return thumb(local)


def materials(map_path):
    out = subprocess.run([os.path.join(HERE, "materials"), map_path], capture_output=True, text=True,
                         env=dict(os.environ, TMUF_PACKS=PACKS)).stdout
    mats, cur = [], None
    for line in out.splitlines():
        if not line.startswith("  "):
            f = line.split()
            cur = {"kind": f[0], "index": int(f[1]), "name": f[2], "textures": []}
            mats.append(cur)
        else:
            f = line.split()
            cur["textures"].append({"sampler": f[0], "file": f[5] if len(f) > 5 else "-"})
    return mats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("map")
    ap.add_argument("--draws", action="store_true")
    args = ap.parse_args()
    draws = parse(os.path.join(args.trace, "draws.txt"))
    mats = materials(args.map)

    # every file of every material, by thumbnail (both ways up)
    files = {}
    for m in mats:
        for t in m["textures"]:
            if t["file"] != "-" and t["file"] not in files:
                files[t["file"]] = file_thumb(t["file"])
    # the trace's textures
    traced = {}
    for name in os.listdir(args.trace):
        mm = re.match(r"tex_(\d+)\.dds$", name)
        if mm:
            traced[int(mm.group(1))] = thumb(os.path.join(args.trace, name))

    def match(tid):
        a = traced.get(tid)
        if a is None:
            return None
        best, best_err = None, 12.0
        for f, b in files.items():
            if b is None:
                continue
            err = min(np.abs(a - b).mean(), np.abs(a - b[::-1]).mean())
            if err < best_err:
                best, best_err = f, err
        return best

    matched = {tid: match(tid) for tid in traced}
    rt_counts = {}
    for d in draws:
        rt_counts[d["rt"]] = rt_counts.get(d["rt"], 0) + 1
    main_rt = max(rt_counts, key=rt_counts.get)

    by_ps = {}
    for d in draws:
        if d["rt"] != main_rt:
            continue
        found = {matched.get(t) for t in d["tex"].values()} - {None}
        # the materials holding the most of these files
        scored = []
        for m in mats:
            mf = {t["file"] for t in m["textures"]}
            n = len(found & mf)
            if n:
                scored.append((n, m["kind"], m["index"]))
        scored.sort(reverse=True)
        best = [f"{k}{i}" for n, k, i in scored if scored and n == scored[0][0]][:4]
        if args.draws:
            print(f"{d['index']:4d} vs {d['vs']:3d} ps {d['ps']:3d} | {' '.join(best) or '?'} | "
                  f"{' '.join(os.path.basename(f) for f in sorted(found))}")
        by_ps.setdefault(d["ps"], {}).setdefault(tuple(best), 0)
        by_ps[d["ps"]][tuple(best)] += 1
    if not args.draws:
        for ps, groups in sorted(by_ps.items()):
            print(f"ps {ps}:")
            for mats_key, n in sorted(groups.items(), key=lambda kv: -kv[1]):
                print(f"   {n:4d} draws  {' '.join(mats_key) or '?'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
