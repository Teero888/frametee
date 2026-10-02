#!/usr/bin/env python3
"""Reference frames from the real game, for comparing the tmuf module's
rendering against it.

Runs TmForever.exe under Wine on a private Xvfb display with refshot.dll
injected, has it shoot a replay to video (/shootvideo, the game's own
"save the video to an avi file", uncompressed) and extracts the frames.
With --pose the game's main camera is held at that pose for the whole video.

  refshot.py --replay R.Replay.Gbx --out DIR [--pose POSE] [--log]

POSE: "x0 x1 x2 y0 y1 y2 z0 z1 z2 tx ty tz fov" (see refshot.c). The game's
Wine prefix, game directory and launcher default to the tmuf_physics oracle
setup (see --help).
"""
import argparse
import glob

import numpy as np
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOME = os.path.expanduser("~")


def winpath(path):
    return "Z:" + os.path.abspath(path).replace("/", "\\")


def unixpath(path):
    return path[2:].replace("\\", "/") if path[1:2] == ":" else path


def read_ppm(path):
    """An RGB image from a binary PPM, None if it cannot be read."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return None
    # the header's four fields, then exactly one whitespace byte before the
    # pixels (a dark picture's first bytes may be whitespace too: 0x0a, 0x20)
    fields, at = [], 0
    while len(fields) < 4:
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        end = at
        while end < len(data) and not data[end:end + 1].isspace():
            end += 1
        if end == at:
            return None
        fields.append(data[at:end])
        at = end
    if fields[0] != b"P6":
        return None
    w, h = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) < w * h * 3:
        return None
    return np.frombuffer(pixels[:w * h * 3], np.uint8).reshape(h, w, 3).astype(np.int32)


def screen(frames):
    """What the game shows: the back buffer refshot.dll last read back."""
    return read_ppm(os.path.join(frames, "latest.ppm"))


def dialog(img):
    """Which dialog the game shows: the account connection (blue title), the
    video settings (orange title), or None."""
    if img is None:
        return None
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    # a title bar: a row with a long run of its colour
    def title(mask):
        return int(mask.sum(axis=1).max()) > img.shape[1] // 3
    # the orange title first: the globe behind the dialogs is blue too
    if title((r > 180) & (g > 100) & (g < 160) & (b < 40)):
        return "video"
    if title((b > 140) & (r < 60) & (g > 60) & (g < 120) & (b - g > 60)):
        return "account"
    return None


def key(env, name, window_name="TrackMania"):
    ids = subprocess.run(["xdotool", "search", "--name", window_name], env=env, capture_output=True,
                         text=True).stdout.split()
    for wid in ids:
        subprocess.run(["xdotool", "key", "--window", wid, name], env=env, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    return bool(ids)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--pose", help="file with the camera pose to hold")
    ap.add_argument("--log", action="store_true", help="write the cameras the game runs to OUT/cameras.log")
    ap.add_argument("--prefix", default=os.path.join(HOME, "software/tmuf_work/render/pfx"))
    # the oracle's game files, linked, with DXVK's d3d9.dll: the real GPU renders
    ap.add_argument("--game", default=os.path.join(HOME, "software/tmuf_work/render/game"))
    ap.add_argument("--launcher", default=os.path.join(HOME, "software/tmuf_physics/tools/oracle/build/launcher.exe"))
    ap.add_argument("--dll", default=os.path.join(HERE, "refshot.dll"))
    ap.add_argument("--display", default=":121")
    ap.add_argument("--timeout", type=float, default=900)
    ap.add_argument("--profile", default="steamuser")
    ap.add_argument("--keep-avi", action="store_true")
    ap.add_argument("--dxvk", action="store_true", help="render through DXVK (needs a display with DRI3)")
    ap.add_argument("--every", type=int, default=0, help="also keep every Nth presented frame")
    ap.add_argument("--stable", type=int, default=7,
                    help="the video is done when it has not grown for this many 3 s checks")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(out, "*.avi")) + glob.glob(os.path.join(out, "frame_*.png")):
        os.remove(f)
    docs = glob.glob(os.path.join(args.prefix, "drive_c/users/*/Documents/TrackMania"))[0]
    # the game writes Documents/TrackMania/VideoNN.avi whatever /out says
    for f in glob.glob(os.path.join(docs, "Video*.avi")):
        os.remove(f)
    replay_dir = os.path.join(docs, "Tracks", "Replays", "refshot")
    shutil.rmtree(replay_dir, ignore_errors=True)
    os.makedirs(replay_dir)
    shutil.copy(args.replay, os.path.join(replay_dir, "shot.Replay.Gbx"))

    frames = os.path.join(out, "frames")
    shutil.rmtree(frames, ignore_errors=True)
    os.makedirs(frames)
    env = dict(os.environ, DISPLAY=args.display, WINEPREFIX=args.prefix,
               # the game's own d3dx9_30 compiles its shaders (Wine's builtin one
               # miscompiles the SM1 HLSL: black surfaces)
               WINEDLLOVERRIDES=("d3d9=n,b" if args.dxvk else "d3d9=b") + ";d3dx9_30=n",
               WINEDEBUG=os.environ.get("REFSHOT_WINEDEBUG", "-all"),
               TMUF_REFSHOT_FRAMES=winpath(frames), TMUF_REFSHOT_EVERY=str(args.every))
    if args.log:
        env["TMUF_REFSHOT_LOG"] = winpath(os.path.join(out, "cameras.log"))
    if args.pose:
        env["TMUF_REFSHOT_POSE"] = winpath(args.pose)

    xvfb = subprocess.Popen(["Xvfb", args.display, "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1)
    if xvfb.poll() is not None or not os.path.exists("/tmp/.X11-unix/X" + args.display.lstrip(":")):
        print(f"Xvfb {args.display} did not start (is the display taken?)", file=sys.stderr)
        return 1
    try:
        proc = subprocess.Popen(
            ["wine", args.launcher, args.dll, "TmForever.exe", f"/profile={args.profile}",
             "/shootvideo=refshot\\shot.Replay.Gbx", f"/out={winpath(out)}"],
            cwd=args.game, env=env, stdout=subprocess.DEVNULL,
            stderr=open(os.path.join(out, "wine.log"), "w"))
        # the account dialog (Escape: offline), the video settings (Ok) and
        # Wine's compressor choice (Ok: uncompressed), answered for as long as
        # they show (a key can be lost while the game loads, and a trace slows
        # it down); then the video until it stops growing and the traces
        # (refshot.c) are done
        traces = [unixpath(env[k]) for k in ("TMUF_REFSHOT_TRACE", "TMUF_REFSHOT_TRACE_LOAD") if env.get(k)]
        # with poses, the views are what is wanted: refshot.dll saves them
        views = len([l for l in open(args.pose) if l.strip()]) if args.pose else 0
        start = time.time()
        last_size, stable, answered = -1, 0, set()
        compressed_at = 0.0
        while time.time() - start < args.timeout and proc.poll() is None:
            time.sleep(3)
            # the startup dialogs only before the video: a view's blue sky
            # can pass for the account dialog's title, and a key pressed
            # while recording aborts the video
            d = dialog(screen(frames)) if last_size <= 0 else None
            if d == "account":
                key(env, "Escape")
                answered.add(d)
            elif d == "video":
                key(env, "Return")
                answered.add(d)
            if key(env, "Return", "Video Compression"):
                answered.add("compression")
                compressed_at = time.time()
            # "Could not initialize the video codec": its Ok has the focus
            if "compression" in answered and time.time() - compressed_at > 8 and last_size < 4096:
                key(env, "Return")
                compressed_at = time.time()
            avis = glob.glob(os.path.join(docs, "Video*.avi"))
            size = sum(os.path.getsize(a) for a in avis)
            stable = stable + 1 if size == last_size and size > 0 else 0
            last_size = size
            tracing = any(not os.path.exists(os.path.join(t, "done.txt")) for t in traces)
            shot = views and all(os.path.exists(os.path.join(frames, f"view_{i:03d}.ppm")) for i in range(views))
            if (shot or (stable >= args.stable and not views)) and not tracing:
                break
        if not views and not glob.glob(os.path.join(docs, "Video*.avi")):
            print(f"no video; dialogs answered: {sorted(answered)}", file=sys.stderr)
    finally:
        subprocess.run(["wineserver", "-k"], env=env, stderr=subprocess.DEVNULL)
        xvfb.terminate()

    avis = [a for a in glob.glob(os.path.join(docs, "Video*.avi")) if os.path.getsize(a) > 0]
    if not avis:
        if args.pose:
            return 0
        print("no video was written", file=sys.stderr)
        return 1
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", avis[0], os.path.join(out, "frame_%04d.png")],
                   check=not args.pose)
    if not args.keep_avi:
        for a in avis:
            os.remove(a)
    print(f"{len(glob.glob(os.path.join(out, 'frame_*.png')))} frames in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
