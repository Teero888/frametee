"""Exercise SM64's Vulkan renderer and the engine's presentation shader together."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def read_ppm(path):
    with path.open("rb") as image:
        assert image.readline().strip() == b"P6", "expected binary PPM capture"
        width, height = map(int, image.readline().split())
        assert image.readline().strip() == b"255"
        pixels = image.read()
    assert len(pixels) == width * height * 3, "truncated render capture"
    return width, height, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path)
    parser.add_argument("--setup", required=True, type=Path)
    parser.add_argument("--editor-cameras", action="store_true", help="also test freecam/top-down (requires current runtime scene hooks)")
    args = parser.parse_args()
    exe = args.exe.resolve()
    setup = args.setup.resolve()
    with tempfile.TemporaryDirectory(prefix="frametee-sm64-render-") as temporary:
        directory = Path(temporary)
        import os
        env = dict(os.environ, XDG_CONFIG_HOME=str(directory), APPDATA=str(directory))
        (directory / "frametee").mkdir()
        modes = ("game", "freecam", "top_down") if args.editor_cameras else ("game",)
        mode_captures = []
        for mode in modes:
            (directory / "frametee" / "config.toml").write_text(
                f'[game]\nid = "sm64"\n[game."sm64"]\neditor_camera_mode = "{mode}"\n')
            captures = []
            for frames in (1, 30):
                capture = directory / f"{mode}-frame-{frames}.ppm"
                subprocess.run([str(exe), "--game", "sm64", "--level", str(setup),
                                "--size", "640x480", "--screenshot", str(capture),
                                "--frames", str(frames)], cwd=exe.parent, env=env,
                               check=True, timeout=60)
                width, height, pixels = read_ppm(capture)
                colored = sum(any(pixels[i:i + 3]) for i in range(0, len(pixels), 3))
                assert colored > width * height // 4, f"SM64 {mode} viewport is blank"
                captures.append((width, height, pixels))
            assert captures[0] == captures[1], f"paused {mode} rendering changes across frames"
            mode_captures.append(captures[0])
        assert len(set(mode_captures)) == len(modes), "camera modes do not change the rendered view"
    print("SM64 GPU presentation and repeated-frame consistency passed")


if __name__ == "__main__":
    main()
