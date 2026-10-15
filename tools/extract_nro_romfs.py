#!/usr/bin/env python3
"""Extracts the RomFS of a Switch NRO (its asset section) to a folder.

The upstream NetherSX2_nx release NRO carries NetherSX2's emulator cores and
their data files in its RomFS (cores/, res/<build>/); the tico build takes them
from there instead of from the Android APKs.

  extract_nro_romfs.py NRO OUTPUT [--list] [--only PREFIX ...]
"""
import argparse
import os
import struct
import sys

EMPTY = 0xFFFFFFFF


def read_romfs(nro):
    with open(nro, "rb") as f:
        data = f.read()
    if data[0x10:0x14] != b"NRO0":
        sys.exit(f"{nro}: not an NRO")
    (nro_size,) = struct.unpack_from("<I", data, 0x18)
    asset = nro_size
    if data[asset:asset + 4] != b"ASET":
        sys.exit(f"{nro}: no asset section")
    romfs_offset, romfs_size = struct.unpack_from("<QQ", data, asset + 0x28)
    if not romfs_size:
        sys.exit(f"{nro}: no RomFS")
    start = asset + romfs_offset
    return memoryview(data)[start:start + romfs_size]


def walk(romfs):
    """Yields (path, data) for every file in the RomFS."""
    header = struct.unpack_from("<10Q", romfs, 0)
    dir_meta, file_meta, data_offset = header[3], header[7], header[9]

    def directory(offset, prefix):
        _, _, child_dir, child_file, _, name_length = struct.unpack_from(
            "<6I", romfs, dir_meta + offset)
        name = bytes(romfs[dir_meta + offset + 0x18:
                           dir_meta + offset + 0x18 + name_length]).decode()
        path = f"{prefix}{name}/" if name else prefix
        entry = child_file
        while entry != EMPTY:
            _, sibling, file_offset, file_size, _, name_length = struct.unpack_from(
                "<IIQQII", romfs, file_meta + entry)
            name = bytes(romfs[file_meta + entry + 0x20:
                               file_meta + entry + 0x20 + name_length]).decode()
            begin = data_offset + file_offset
            yield path + name, romfs[begin:begin + file_size]
            entry = sibling
        entry = child_dir
        while entry != EMPTY:
            yield from directory(entry, path)
            (entry,) = struct.unpack_from("<I", romfs, dir_meta + entry + 4)

    yield from directory(0, "")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("nro")
    parser.add_argument("output")
    parser.add_argument("--list", action="store_true", help="only list the files")
    parser.add_argument("--only", nargs="*", default=[],
                        help="extract only paths starting with these prefixes")
    args = parser.parse_args()
    for path, content in walk(read_romfs(args.nro)):
        if args.only and not any(path.startswith(p) for p in args.only):
            continue
        if args.list:
            print(f"{len(content):>12}  {path}")
            continue
        target = os.path.join(args.output, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as f:
            f.write(content)


if __name__ == "__main__":
    main()
