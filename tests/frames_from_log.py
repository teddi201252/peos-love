#!/usr/bin/env python3
"""The frames a LÖVE game wrote to the kernel's log (sdl2/sdl2_peos.c, when
/starlight/config/love/frames.cfg is there) as PNG files: frame-<n>.png.

    frames_from_log.py debug.log out_dir
"""
import base64
import pathlib
import re
import struct
import sys
import zlib

LINE = re.compile(rb"love-frame (\d+): ([A-Za-z0-9+/=]+)\s*$")


def png(path, w, h, rgb):
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def main():
    log, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    parts = {}
    for line in log.read_bytes().splitlines():
        m = LINE.search(line)
        if m:
            parts.setdefault(int(m.group(1)), []).append(m.group(2))
    for n, chunks in sorted(parts.items()):
        try:
            data = base64.b64decode(b"".join(chunks))
            head = data.split(b"\n", 3)
            w, h = (int(v) for v in head[1].split())
            rgb = head[3][:w * h * 3]
            if len(rgb) < w * h * 3:
                print(f"frame {n}: cut short ({len(rgb)} of {w * h * 3} bytes)")
                continue
            png(out / f"frame-{n:03d}.png", w, h, rgb)
        except (ValueError, IndexError) as e:
            print(f"frame {n}: unreadable ({e})")
    print(f"{len(parts)} frames in {out}")


if __name__ == "__main__":
    main()
