#!/usr/bin/env python3
"""Compare the tmuf module's pictures with the game's, view by view.

  refcompare.py VIEWS OUT [--only NAME...] [--ours-only] [--trace NAME [--steps]]
                [--trace-lightmap]

VIEWS: one view per line, "name | map | ex ey ez | tx ty tz | fov" (map
relative to data/games/tmuf/GameData/Tracks, the eye and the point it looks
at in the game's world, fov vertical in degrees; '#' starts a comment).

For each map the game renders all its views in one run (refshot.py: an idle
replay of the map, the camera held at each pose in turn) into OUT/ref/, the
module renders the same views (TM_TEST_CAMERA) into OUT/ours/, and
OUT/compare/NAME.png shows the game's, ours, and their difference side by
side. OUT/report.txt lists the mean difference per view. Game pictures are
cached: --ours-only reuses them. --trace NAME records the game's draw calls
of that view (refshot.c: shaders, constants, textures, states, render
targets; --steps: the render target after every draw) into OUT/trace/NAME,
--trace-lightmap the game's lightmap computation of each map into
OUT/trace/lightmap_MAP (trace_summary.py reads both).
"""
import argparse
import os
import shutil
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../../.."))
TRACKS = os.path.join(ROOT, "data/games/tmuf/GameData/Tracks")
BUILD = os.path.join(ROOT, "build-tmuf")
SCRATCH_CFG = os.path.join(HERE, ".cfg")

sys.path.insert(0, HERE)
from refshot import read_ppm  # noqa: E402


def parse_views(path):
    views = []
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("|")]
        name, map_rel = parts[0], parts[1]
        eye = [float(x) for x in parts[2].split()]
        target = [float(x) for x in parts[3].split()]
        views.append({"name": name, "map": map_rel, "eye": eye, "target": target, "fov": float(parts[4])})
    return views


def game_pose(eye, target, fov):
    """CHmsCamera location: rows of a rotation whose columns are left, up and
    forward, then the eye."""
    f = np.array(target, float) - np.array(eye, float)
    f /= np.linalg.norm(f)
    x = np.cross([0.0, 1.0, 0.0], f)
    x /= np.linalg.norm(x)
    y = np.cross(f, x)
    m = np.stack([x, y, f], axis=1)  # columns
    return list(m.reshape(-1)) + list(eye) + [fov]


def write_ppm_png(img, path):
    h, w, _ = img.shape
    raw = f"P6\n{w} {h}\n255\n".encode() + np.clip(img, 0, 255).astype(np.uint8).tobytes()
    subprocess.run(["magick", "ppm:-", path], input=raw, check=True)


def config_dir():
    """A copy of the user's frametee config with the race camera selected and
    the module set as the game's references are shot: no HUD (the game's
    videos have none), no antialiasing (the references' settings)."""
    dst = os.path.join(SCRATCH_CFG, "frametee")
    os.makedirs(dst, exist_ok=True)
    src = os.path.expanduser("~/.config/frametee/config.toml")
    text = open(src).read() if os.path.exists(src) else ""
    forced = {"editor_camera_mode": '"chase"', "countdown": "false", "race_time": "false", "speed": "false",
              "challenge_info": "false", "antialiasing": "1"}
    out, in_tmuf, seen = [], False, False
    for line in text.splitlines():
        if line.startswith("["):
            if in_tmuf and not seen:
                out += [f'"{k}" = {v}' for k, v in forced.items()]
                seen = True
            in_tmuf = line.strip() == '[game."tmuf"]'
        key = line.split("=")[0].strip().strip('"') if "=" in line else None
        if in_tmuf and key in forced:
            continue
        out.append(line)
    if not seen:
        if not in_tmuf:
            out.append('[game."tmuf"]')
        out += [f'"{k}" = {v}' for k, v in forced.items()]
    open(os.path.join(dst, "config.toml"), "w").write("\n".join(out) + "\n")
    return SCRATCH_CFG


def render_ours(view, size, path, cfg):
    tmp = path + ".ppm"
    env = dict(os.environ, XDG_CONFIG_HOME=cfg, TM_TEST_TICK=os.environ.get("REFCOMPARE_TICK", "1000"),
               TM_TEST_CAMERA=" ".join(str(v) for v in view["eye"] + view["target"] + [view["fov"]]))
    subprocess.run([os.path.join(BUILD, "frametee"), "--game", "tmuf", "--level",
                    os.path.join(TRACKS, view["map"]), "--screenshot", tmp, "--frames", "20",
                    "--size", f"{size[0]}x{size[1]}"], cwd=BUILD, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=300)
    img = read_ppm(tmp)
    if img is not None:
        os.remove(tmp)
        write_ppm_png(img, path)
    return img


def load_png(path):
    raw = subprocess.run(["magick", path, "-depth", "8", "ppm:-"], capture_output=True).stdout
    # the header's four fields, then exactly one whitespace byte before the pixels
    # (which may themselves start with whitespace bytes)
    fields, at = [], 0
    while len(fields) < 4:
        while raw[at:at + 1].isspace():
            at += 1
        end = at
        while not raw[end:end + 1].isspace():
            end += 1
        fields.append(raw[at:end])
        at = end
    w, h = int(fields[1]), int(fields[2])
    return np.frombuffer(raw[at + 1:at + 1 + w * h * 3], np.uint8).reshape(h, w, 3).astype(np.int32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("views")
    ap.add_argument("out")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--ours-only", action="store_true", help="reuse the game's pictures in OUT/ref")
    ap.add_argument("--trace", help="also trace the game's draw calls of this view into OUT/trace/NAME")
    ap.add_argument("--steps", nargs="?", const=1, type=int, help="the traced view's render target after every draw (N: after the first N)")
    ap.add_argument("--trace-lightmap", action="store_true", help="trace each map's lightmap computation")
    ap.add_argument("--prefix", help="the game's Wine prefix (refshot.py's default when not given)")
    ap.add_argument("--display", help="the X display the game runs on (refshot.py's default when not given)")
    args = ap.parse_args()
    views = parse_views(args.views)
    if args.only:
        views = [v for v in views if v["name"] in args.only]
    out = os.path.abspath(args.out)
    for d in ("ref", "ours", "compare", "work"):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    # the game's pictures, one run per map
    if not args.ours_only:
        by_map = {}
        for v in views:
            by_map.setdefault(v["map"], []).append(v)
        for map_rel, vs in by_map.items():
            work = os.path.join(out, "work", os.path.basename(map_rel).split(".")[0])
            os.makedirs(work, exist_ok=True)
            replay = os.path.join(work, "idle.Replay.Gbx")
            hold, skip = 24, 150
            seconds = (skip + hold * len(vs)) / 24.0 + 10
            subprocess.run([os.path.join(HERE, "idle_replay"), os.path.join(TRACKS, map_rel), replay,
                            str(seconds)], env=dict(os.environ, TMUF_PACKS=os.path.join(ROOT, "data/games/tmuf/Packs")),
                           check=True)
            with open(os.path.join(work, "poses.txt"), "w") as f:
                for v in vs:
                    f.write(" ".join(f"{x:.6f}" for x in game_pose(v["eye"], v["target"], v["fov"])) + "\n")
            env = dict(os.environ, TMUF_REFSHOT_HOLD=str(hold), TMUF_REFSHOT_SKIP=str(skip))
            names = [v["name"] for v in vs]
            if args.trace in names:
                trace = os.path.join(out, "trace", args.trace)
                os.makedirs(trace, exist_ok=True)
                env["TMUF_REFSHOT_TRACE"] = "Z:" + trace.replace("/", "\\")
                env["TMUF_REFSHOT_TRACE_VIEW"] = str(names.index(args.trace))
                if args.steps:
                    env["TMUF_REFSHOT_TRACE_STEPS"] = str(args.steps)
            if args.trace_lightmap:
                trace = os.path.join(out, "trace", "lightmap_" + os.path.basename(work))
                shutil.rmtree(trace, ignore_errors=True)
                os.makedirs(trace)
                env["TMUF_REFSHOT_TRACE_LOAD"] = "Z:" + trace.replace("/", "\\")
                env.setdefault("TMUF_REFSHOT_TRACE_RT_MAX", "400")
            subprocess.run([sys.executable, os.path.join(HERE, "refshot.py"), "--replay", replay, "--out", work,
                            "--pose", os.path.join(work, "poses.txt"), "--timeout", "3600"] + (["--log"] if os.environ.get("REFCOMPARE_LOG") else [])
                           + (["--prefix", args.prefix] if args.prefix else []) + (["--display", args.display] if args.display else []),
                           env=env, check=False)
            for i, v in enumerate(vs):
                img = read_ppm(os.path.join(work, "frames", f"view_{i:03d}.ppm"))
                if img is None:
                    print(f"{v['name']}: the game did not render the view", file=sys.stderr)
                    continue
                write_ppm_png(img, os.path.join(out, "ref", v["name"] + ".png"))

    # ours, and the comparison
    cfg = config_dir()
    report = []
    for v in views:
        ref_path = os.path.join(out, "ref", v["name"] + ".png")
        if not os.path.exists(ref_path):
            continue
        ref = load_png(ref_path)
        ours = render_ours(v, (ref.shape[1], ref.shape[0]), os.path.join(out, "ours", v["name"] + ".png"), cfg)
        if ours is None or ours.shape != ref.shape:
            print(f"{v['name']}: no picture of ours", file=sys.stderr)
            continue
        diff = np.abs(ref - ours)
        mean = float(diff.mean())
        report.append(f"{v['name']:24s} mean abs difference {mean:6.2f}")
        side = np.concatenate([ref, ours, np.clip(diff * 3, 0, 255)], axis=1)
        write_ppm_png(side, os.path.join(out, "compare", v["name"] + ".png"))
    open(os.path.join(out, "report.txt"), "w").write("\n".join(report) + "\n")
    print("\n".join(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
