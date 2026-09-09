#!/usr/bin/env python3
"""Compare frametee screenshots with DDNet's map_render tool.

Requires Pillow, a working Vulkan display for frametee, and a built DDNet
map_render (EGL). No repository assets or user preferences are modified.
Example:
  python3 scripts/compare_ddnet_maps.py --build /tmp/frametee-map-build \
      --ddnet ddnet --output /tmp/map-comparison ddnet/data/maps/dm1.map
"""
import argparse
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from PIL import Image, ImageChops


def dimensions(path):
    data = path.read_bytes()
    signature, version, _, _, types, items, raw, item_size, _ = struct.unpack_from('<4s8i', data)
    if signature not in (b'DATA', b'ATAD') or version not in (3, 4):
        raise ValueError(f'Unsupported map: {path}')
    offsets_start = 36 + types * 12
    items_start = offsets_start + items * 4 + raw * (8 if version == 4 else 4)
    for i in range(items):
        offset, = struct.unpack_from('<i', data, offsets_start + i * 4)
        type_id, size = struct.unpack_from('<2i', data, items_start + offset)
        if type_id >> 16 != 5 or size < 60:
            continue
        layer = struct.unpack_from('<15i', data, items_start + offset + 8)
        if layer[1] == 2 and layer[6] & 1:
            return layer[4], layer[5]
    raise ValueError(f'No game layer: {path}')


def run(command, cwd, env, log):
    with log.open('w') as out:
        subprocess.run(command, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT, check=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--ddnet', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--size', default='1200x900')
    parser.add_argument('--center', help='Optional x,y center in tiles')
    parser.add_argument('--width', type=float, help='Optional view width in tiles')
    parser.add_argument('maps', nargs='+', type=Path)
    args = parser.parse_args()
    build, ddnet, output = args.build.resolve(), args.ddnet.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    w, h = map(int, args.size.split('x'))
    aspect = w / h
    base_h = math.sqrt(1150 * 1000 / aspect)
    base_w = base_h * aspect
    if base_w > 1500:
        base_w, base_h = 1500, 1500 / aspect
    if base_h > 1050:
        base_w, base_h = 1050 * aspect, 1050
    report = []
    with tempfile.TemporaryDirectory(prefix='frametee-map-compare-') as config:
        prefs = Path(config) / 'frametee'
        prefs.mkdir()
        (prefs / 'config.toml').write_text('[graphics]\nlod_bias = 0.0\nbg_color = [0.0, 0.0, 0.0]\n[game."ddnet"]\nentities_view = false\nmap_detail = true\n')
        env = dict(os.environ, XDG_CONFIG_HOME=config)
        # Driver-owned allocations are outside the renderer's lifecycle tests.
        env['ASAN_OPTIONS'] = 'detect_leaks=0'
        for path in args.maps:
            path = path.resolve()
            mw, mh = dimensions(path)
            cx, cy = map(float, args.center.split(',')) if args.center else (mw / 2, mh / 2)
            width = args.width or mw
            zoom = width * 32 / base_w
            stem = path.stem
            ref = output / f'{stem}-ddnet.png'
            ppm = output / f'{stem}-frametee.ppm'
            target = output / f'{stem}-frametee.png'
            run([str(ddnet / 'build/map_render'), '-o', str(ref), '-w', str(w), '-h', str(h),
                 '-x', str(cx), '-y', str(cy), '-z', str(zoom), '-t', '0', str(path)],
                ddnet, env, output / f'{stem}-ddnet.log')
            # The game physics and camera are offset by 200 tiles from the map origin.
            view_x, view_y = cx + 200, cy + 200
            run([str(build / 'frametee'), '--game', 'ddnet', '--level', str(path), '--screenshot', str(ppm),
                 '--view', f'{view_x},{view_y},{width}', '--size', args.size, '--frames', '5'],
                build, env, output / f'{stem}-frametee.log')
            a, b = Image.open(ppm).convert('RGB'), Image.open(ref).convert('RGB')
            if a.size != b.size:
                raise ValueError(f'Capture sizes differ: {a.size} vs {b.size}')
            a.save(target)
            diff = ImageChops.difference(a, b)
            histogram = diff.histogram()
            mae = sum((i % 256) * n for i, n in enumerate(histogram)) / (w * h * 3)
            diff.save(output / f'{stem}-difference.png')
            row = dict(map=str(path), center=[cx, cy], width=width, time_ms=0, mean_rgb_error=mae)
            report.append(row)
            print(f'{stem}: mean RGB error {mae:.4f} / 255', flush=True)
    (output / 'comparison.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
