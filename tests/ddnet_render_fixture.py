#!/usr/bin/env python3
"""Generate a visual fixture for compare_ddnet_maps.py without external assets.

Exercises all eight tile orientations, extended borders, vertex alpha/color,
repeated quad UVs, pivot rotation, Bézier envelopes, and a clipped parallax
group. Use --image-size 257x239 to exercise irregular tile sheet resizing.
"""
import argparse
from pathlib import Path
import struct
import zlib


def pack(values):
    return struct.pack(f'<{len(values)}i', *values)


class Datafile:
    def __init__(self):
        self.raw = []
        self.items = {}

    def blob(self, data):
        self.raw.append(data)
        return len(self.raw) - 1

    def item(self, kind, body):
        self.items.setdefault(kind, []).append(pack(body))

    def encode(self):
        items, raw = bytearray(), bytearray()
        types, offsets, raw_offsets, raw_sizes = [], [], [], []
        for kind, bodies in sorted(self.items.items()):
            types.extend([kind, len(offsets), len(bodies)])
            for index, body in enumerate(bodies):
                offsets.append(len(items))
                items += pack([kind << 16 | index, len(body)]) + body
        for data in self.raw:
            raw_offsets.append(len(raw))
            raw_sizes.append(len(data))
            raw += zlib.compress(data)
        meta = pack(types + offsets + raw_offsets + raw_sizes) + items
        header = [4, len(meta) + len(raw) + 20, len(meta) + 20,
                  len(types) // 3, len(offsets), len(self.raw), len(items), len(raw)]
        return b'DATA' + pack(header) + meta + raw


def point(time, values, curve=1):
    return [time, curve] + values + [0] * 16


def quad(x, y, width, height, uv=(0, 0, 1, 1), position_env=-1, color_env=-1):
    points = [x, y, x + width, y, x, y + height, x + width, y + height,
              x + width / 2, y + height / 2]
    colors = [255, 100, 100, 255, 100, 255, 100, 128,
              100, 100, 255, 255, 255, 255, 255, 200]
    texcoords = [uv[0], uv[1], uv[2], uv[1], uv[0], uv[3], uv[2], uv[3]]
    return ([int(v * 32768) for v in points] + colors +
            [int(v * 1024) for v in texcoords] + [position_env, 500, color_env, 500])


def fixture(width, height):
    data = Datafile()
    data.item(0, [1])
    data.item(1, [1, -1, -1, -1, -1])
    # Each tile has asymmetric RGBA so orientation and blending are visible.
    pixels = bytes(
        value for y in range(height) for x in range(width)
        for value in [32 + (x % 16) * 14, 32 + (y % 16) * 14,
                      255 if x % 16 < y % 16 else 40,
                      100 if (x % 16 + y % 16) % 7 < 3 else 255]
    )
    name, image = data.blob(b'fixture\0'), data.blob(pixels)
    data.item(2, [1, width, height, 0, name, image])
    # Empty names in DDNet's four-characters-per-integer encoding.
    empty_name = [-2139062144] * 7 + [-2139062272]
    data.item(3, [3, 3, 0, 2] + empty_name + [1])
    data.item(3, [3, 4, 2, 2] + empty_name + [1])
    points = (point(0, [0, 0, 0, 0], 5) +
              point(1000, [40 * 1024, 20 * 1024, 90 * 1024, 0]) +
              point(0, [1024] * 4) + point(1000, [512, 800, 1024, 512]))
    data.item(6, points)

    def quad_layer(quads, image_index):
        packed = pack([value for q in quads for value in q])
        data.item(5, [0, 3, 0, 1, len(quads), data.blob(packed), image_index])

    def tile_layer(flags, grid):
        data.item(5, [0, 2, 0, 2, 16, 12, flags, 255, 255, 255, 255,
                      -1, 500, 0 if flags == 0 else -1, data.blob(grid)])

    quad_layer([quad(-30, -30, 80, 80)], -1)
    grid = bytearray(16 * 12 * 4)
    for i, flags in enumerate([0, 1, 2, 3, 8, 9, 10, 11]):
        for y in range(2, 4):
            start = (y * 16 + i + 2) * 4
            grid[start:start + 4] = bytes([1, flags, 0, 0])
    # Nonempty edges and corners exercise border extension in wider views.
    for x, y in [(0, 0), (15, 0), (0, 11), (15, 11),
                 (0, 5), (15, 6), (5, 0), (6, 11)]:
        start = (y * 16 + x) * 4
        grid[start:start + 4] = bytes([2, 9, 0, 0])
    tile_layer(0, grid)
    tile_layer(1, bytes(16 * 12 * 4))
    quad_layer([
        quad(2, 5, 3, 2, uv=(-0.2, 0.1, 1.5, 1.3), position_env=0, color_env=1),
        quad(8, 6, 3, 3, position_env=0, color_env=1),
    ], 0)
    quad_layer([quad(2, 4, 8, 6)], -1)
    data.item(4, [2, 0, 0, 100, 100, 0, 4, 0, 0, 0, 0, 0])
    data.item(4, [2, 32, -32, 50, 75, 4, 1, 1, 5 * 32, 5 * 32, 5 * 32, 3 * 32])
    return data.encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image-size', default='256x256')
    parser.add_argument('--output', type=Path,
                        default=Path(__file__).with_name('data') / 'ddnet-render.map')
    args = parser.parse_args()
    width, height = map(int, args.image_size.split('x'))
    if width <= 0 or height <= 0:
        parser.error('Image dimensions must be positive')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(fixture(width, height))


if __name__ == '__main__':
    main()
