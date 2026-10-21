#!/usr/bin/env python3
"""Writes the small archives under tools/archive_fixtures/ that
tools/archive_smoke.cjs reads (HANDOFF sec. 110). Needs py7zr (pip install py7zr)."""
import gzip
import lzma
import os
import zipfile

import py7zr

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "archive_fixtures")


def content(blocks, seed):
    x = seed
    base = bytearray(16384)
    for i in range(16384):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        base[i] = (x >> 16) & 0xFF
    out = bytearray()
    for i in range(blocks):
        blk = bytearray(base)
        blk[i % 16384] ^= 0x5A
        blk[(i * 7919) % 16384] = i & 0xFF
        out += blk
    return bytes(out)


FILES = {
    "readme.txt": b"WASMIDI archive fixture\n",
    "midi/song.mid": content(512, 12345),
    "bank.sf2": content(40, 777),
}
SMALL_DICT = [{"id": lzma.FILTER_LZMA2, "dict_size": 1 << 16}]


def write_7z(name, filters, files=FILES):
    with py7zr.SevenZipFile(os.path.join(OUT, name), "w", filters=filters) as z:
        for path, data in files.items():
            z.writestr(data, path)


def main():
    os.makedirs(OUT, exist_ok=True)
    write_7z("lzma2.7z", SMALL_DICT)
    write_7z("lzma.7z", [{"id": lzma.FILTER_LZMA1, "dict_size": 1 << 16}])
    write_7z("copy.7z", [{"id": py7zr.FILTER_COPY}],
             {"readme.txt": FILES["readme.txt"], "midi/tiny.mid": content(4, 1)})
    write_7z("deflate.7z", [{"id": py7zr.FILTER_DEFLATE}])
    with zipfile.ZipFile(os.path.join(OUT, "mixed.zip"), "w") as z:
        z.writestr(zipfile.ZipInfo("readme.txt"), FILES["readme.txt"], zipfile.ZIP_STORED)
        z.writestr("midi/song.mid", FILES["midi/song.mid"], zipfile.ZIP_DEFLATED)
        z.writestr("bank.sf2", FILES["bank.sf2"], zipfile.ZIP_LZMA)
    with zipfile.ZipFile(os.path.join(OUT, "zip64.zip"), "w", compression=zipfile.ZIP_DEFLATED) as z:
        with z.open("midi/song.mid", "w", force_zip64=True) as f:
            f.write(FILES["midi/song.mid"])
    song = FILES["midi/song.mid"]
    with open(os.path.join(OUT, "song.mid.xz"), "wb") as f:
        f.write(lzma.compress(song, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, filters=SMALL_DICT))
    half = len(song) // 2
    with open(os.path.join(OUT, "multi.mid.xz"), "wb") as f:
        f.write(lzma.compress(song[:half], format=lzma.FORMAT_XZ, check=lzma.CHECK_SHA256, filters=SMALL_DICT))
        f.write(b"\0" * 8)
        f.write(lzma.compress(song[half:], format=lzma.FORMAT_XZ, check=lzma.CHECK_NONE, filters=SMALL_DICT))
    with open(os.path.join(OUT, "song.mid.gz"), "wb") as raw:
        with gzip.GzipFile("song.mid", "wb", fileobj=raw, mtime=0) as f:
            f.write(song)


if __name__ == "__main__":
    main()
