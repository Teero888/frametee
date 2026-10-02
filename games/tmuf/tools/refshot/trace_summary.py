#!/usr/bin/env python3
"""Summarise a refshot draw trace (refcompare.py --trace NAME).

  trace_summary.py TRACE_DIR                 one line per draw call
  trace_summary.py TRACE_DIR --draw N        everything about draw N
  trace_summary.py TRACE_DIR --sheet OUT.png the textures, labelled, on one sheet
  trace_summary.py TRACE_DIR --changes       per draw, the pixels it changed in
                                             its render target (a trace with
                                             TMUF_REFSHOT_TRACE_STEPS=1)
"""
import argparse
import os
import re
import subprocess
import sys


def parse(path):
    draws, cur = [], None
    for line in open(path):
        line = line.rstrip("\n")
        if line.startswith("draw "):
            f = line.split()
            cur = {"index": int(f[1]), "kind": f[2], "prims": int(f[4]), "vs": int(f[6]), "ps": int(f[8]),
                   "tex": {}, "texinfo": {}, "rs": {}, "vc": {}, "pc": {}, "rt": None, "lines": [line]}
            draws.append(cur)
            continue
        if cur is None:
            continue
        cur["lines"].append(line)
        f = line.split()
        if not f:
            continue
        if f[0] == "rt":
            cur["rt"] = int(f[2])
        elif f[0] == "tex":
            cur["tex"][int(f[1])] = int(f[3])
        elif f[0] == "texinfo":
            cur["texinfo"][int(f[1])] = " ".join(f[2:])
        elif f[0] == "rs":
            it = iter(f[1:])
            cur["rs"] = dict(zip(it, it))
        elif re.match(r"[vp]c\d+", f[0]):
            cur[f[0][:2]][int(f[0][2:])] = [float(x) for x in f[1:5]]
    return draws


def load_step(trace, index):
    path = os.path.join(trace, f"step_{index}.bmp")
    if not os.path.exists(path):
        return None
    raw = subprocess.run(["magick", path, "-depth", "8", "ppm:-"], capture_output=True).stdout
    parts = raw.split(maxsplit=4)
    if len(parts) < 5:
        return None
    import numpy as np
    w, h = int(parts[1]), int(parts[2])
    if len(parts[4]) < w * h * 3:
        return None
    return np.frombuffer(parts[4][:w * h * 3], np.uint8).reshape(h, w, 3).astype(np.int32)


def changes(trace, draws):
    import numpy as np
    last = {}
    for d in draws:
        img = load_step(trace, d["index"])
        if img is None:
            continue
        prev = last.get(d["rt"])
        if prev is None or prev.shape != img.shape:
            changed, mean = img.shape[0] * img.shape[1], float(img.mean())
        else:
            diff = np.abs(img - prev).max(axis=2)
            changed, mean = int(np.count_nonzero(diff)), float(diff.mean())
        last[d["rt"]] = img
        tex = " ".join(f"{s}:{t}" for s, t in sorted(d["tex"].items()))
        print(f"{d['index']:4d} rt {d['rt']} vs {d['vs']:3d} ps {d['ps']:3d} prims {d['prims']:6d} "
              f"changed {changed:7d} mean {mean:6.2f} | {tex}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--draw", type=int)
    ap.add_argument("--sheet")
    ap.add_argument("--changes", action="store_true")
    args = ap.parse_args()
    draws = parse(os.path.join(args.trace, "draws.txt"))
    texinfo = {}
    for d in draws:
        texinfo.update(d["texinfo"])
    if args.draw is not None:
        print("\n".join(draws[args.draw]["lines"]))
        return 0
    if args.sheet:
        files = sorted((int(re.findall(r"\d+", f)[0]), f) for f in os.listdir(args.trace) if f.startswith("tex_"))
        cmd = ["magick", "montage"]
        for t, f in files:
            cmd += ["-label", f"{t} {texinfo.get(t, '')[:40]}", os.path.join(args.trace, f + "[0]")]
        cmd += ["-geometry", "128x128+2+2", "-tile", "10x", "-pointsize", "9", args.sheet]
        subprocess.run(cmd, check=False)
        return 0
    if args.changes:
        return changes(args.trace, draws)
    for d in draws:
        rs = d["rs"]
        tex = " ".join(f"{s}:{t}" for s, t in sorted(d["tex"].items()))
        blend = f"blend {rs.get('src')}/{rs.get('dst')}" if rs.get("alphablend") == "1" else "opaque"
        at = f" atest {rs.get('ref')}" if rs.get("alphatest") == "1" else ""
        print(f"{d['index']:4d} {d['kind']:7s} {d['prims']:6d} vs {d['vs']:3d} ps {d['ps']:3d} {blend}{at} "
              f"z{rs.get('zenable')}{rs.get('zwrite')} cull {rs.get('cull')} | {tex}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
