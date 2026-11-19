#!/usr/bin/env python3
"""Extract Gundam Operation Troy files from YOUR OWN copy of the game.

Supported inputs:
  * a Games on Demand (GOD) folder  (e.g. ".../424107D6/00007000/<id>" + "<id>.data/Data0000...")
  * an Xbox 360 disc image (.iso), XGD1/XGD2/XGD3 or a plain GDF image
  * an already-extracted game folder that contains default.xex

Usage:
  python tools/extract_game.py <input> [--out game] [--all]

By default only the code modules are written: default.xex and the DLL module
Contents/dlls/PathEngine_Xenon_Release.xex (that is all the build needs).
--all extracts every game file as well (the recomp reads these at runtime).

Nothing this script produces may be committed to the repository: the
output folder ("game/" by default) is listed in .gitignore.
"""

import argparse
import os
import shutil
import struct
import sys

SECTOR = 0x800
GDF_MAGIC = b"MICROSOFT*XBOX*MEDIA"

# SVOD (GOD) layout: data blocks of 0x800 bytes, one 0x1000 hash block before
# every 0x198 data blocks, plus one 0x1000 top-level hash block per data file.
SVOD_HASH_BLOCK = 0x1000
SVOD_BLOCKS_PER_L0 = 0x198
SVOD_BLOCKS_PER_FILE = 0x14388

# Partition offsets of the game data (GDF) inside retail disc images.
ISO_GDF_OFFSETS = (0x0, 0xFDA0000, 0x2080000, 0x18300000)


class GodReader:
    """Reads the GDF image stored inside a GOD/SVOD container."""

    def __init__(self, data_dir):
        names = sorted(n for n in os.listdir(data_dir) if n.startswith("Data"))
        if not names:
            raise ValueError(f"no Data#### files in {data_dir}")
        self.files = [open(os.path.join(data_dir, n), "rb") for n in names]

    def read(self, pos, size):
        out = bytearray()
        while size > 0:
            block, inner = divmod(pos, SECTOR)
            take = min(size, SECTOR - inner)
            file_index, rem = divmod(block, SVOD_BLOCKS_PER_FILE)
            offset = (rem * SECTOR
                      + (rem // SVOD_BLOCKS_PER_L0 + 1) * SVOD_HASH_BLOCK
                      + SVOD_HASH_BLOCK)
            f = self.files[file_index]
            f.seek(offset + inner)
            chunk = f.read(take)
            if len(chunk) != take:
                raise IOError("GOD data ended early; is a Data file missing?")
            out += chunk
            pos += take
            size -= take
        return bytes(out)


class IsoReader:
    def __init__(self, path):
        self.f = open(path, "rb")

    def read(self, pos, size):
        self.f.seek(pos)
        return self.f.read(size)


def find_gdf_base(reader, candidates):
    for base in candidates:
        try:
            if reader.read(base + 0x10000, len(GDF_MAGIC)) == GDF_MAGIC:
                return base
        except (IOError, IndexError):
            continue
    return None


def list_gdf(reader, base):
    """Returns [(path, sector, size, is_dir)] for every entry in the image."""
    vd = reader.read(base + 32 * SECTOR, SECTOR)
    root_sector, root_size = struct.unpack_from("<II", vd, 20)
    entries = []

    def walk_dir(sector, size, prefix, depth):
        if size == 0 or depth > 32:
            return
        data = reader.read(base + sector * SECTOR, size)
        seen = set()

        def visit(off):
            if off in seen or off + 14 > len(data):
                return
            seen.add(off)
            left, right, f_sector, f_size, attrib, name_len = struct.unpack_from(
                "<HHIIBB", data, off)
            if left == 0xFFFF and right == 0xFFFF:
                return
            name = data[off + 14:off + 14 + name_len].decode("latin-1")
            if left:
                visit(left * 4)
            path = f"{prefix}/{name}" if prefix else name
            is_dir = bool(attrib & 0x10)
            entries.append((path, f_sector, f_size, is_dir))
            if is_dir:
                walk_dir(f_sector, f_size, path, depth + 1)
            if right:
                visit(right * 4)

        visit(0)

    walk_dir(root_sector, root_size, "", 0)
    return entries


def copy_entry(reader, base, sector, size, dest):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    pos = base + sector * SECTOR
    with open(dest, "wb") as out:
        remaining = size
        while remaining > 0:
            take = min(remaining, 8 * 1024 * 1024)
            out.write(reader.read(pos, take))
            pos += take
            remaining -= take


def open_input(path):
    """Returns (reader, gdf_base) or (None, folder) for an extracted folder."""
    if os.path.isfile(path):
        if path.lower().endswith(".iso"):
            reader = IsoReader(path)
            base = find_gdf_base(reader, ISO_GDF_OFFSETS)
            if base is None:
                raise ValueError("not an Xbox 360 disc image (no GDF volume found)")
            return reader, base
        # A GOD header file was given directly: use its sibling .data folder.
        data_dir = path + ".data"
        if os.path.isdir(data_dir):
            reader = GodReader(data_dir)
            return reader, find_gdf_base(reader, (0,))
        raise ValueError(f"unrecognised file: {path}")

    if not os.path.isdir(path):
        raise ValueError(f"not found: {path}")

    for name in os.listdir(path):
        if name.lower() == "default.xex":
            return None, path

    for root, dirs, _files in os.walk(path):
        for d in dirs:
            if d.lower().endswith(".data"):
                reader = GodReader(os.path.join(root, d))
                base = find_gdf_base(reader, (0,))
                if base is None:
                    raise ValueError("GOD data found but no GDF volume inside")
                return reader, base
    raise ValueError("no GOD .data folder, ISO, or default.xex found")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="GOD folder, .iso file, or extracted game folder")
    ap.add_argument("--out", default="game", help="output folder (default: game)")
    ap.add_argument("--all", action="store_true", help="extract every game file")
    args = ap.parse_args()

    try:
        reader, base = open_input(args.input)
    except (ValueError, IOError) as e:
        sys.exit(f"error: {e}")

    os.makedirs(args.out, exist_ok=True)

    if reader is None:  # already-extracted folder
        src = base
        if args.all:
            shutil.copytree(src, args.out, dirs_exist_ok=True)
        else:
            for name in os.listdir(src):
                if name.lower() == "default.xex":
                    shutil.copy2(os.path.join(src, name),
                                 os.path.join(args.out, "default.xex"))
        print(f"copied from extracted folder into {args.out}")
        return

    entries = list_gdf(reader, base)
    if not any(e[0].lower() == "default.xex" for e in entries):
        sys.exit("error: default.xex not found in the game image")

    # The console's dashboard update is not part of the game.
    files = [e for e in entries
             if not e[3] and not e[0].lower().startswith("$systemupdate/")]
    if not args.all:
        # Code modules only: default.xex plus DLL modules such as
        # Contents/dlls/PathEngine_Xenon_Release.xex. This is all the build needs.
        files = [e for e in files if e[0].lower().endswith(".xex")]

    total = sum(e[2] for e in files)
    done = 0
    last_tenth = -1
    for path, sector, size, _ in files:
        out_name = "default.xex" if path.lower() == "default.xex" else path
        copy_entry(reader, base, sector, size, os.path.join(args.out, out_name))
        done += size
        if args.all:
            tenth = done * 10 // max(total, 1)
            if tenth != last_tenth:
                last_tenth = tenth
                print(f"{tenth * 10:3d}%  ({done / 1e6:.0f} of {total / 1e6:.0f} MB)", flush=True)
        else:
            print(f"wrote {os.path.join(args.out, out_name)} ({size} bytes)")
    if args.all:
        print(f"extracted {len(files)} files ({total / 1e6:.0f} MB) into {args.out}")

    with open(os.path.join(args.out, "default.xex"), "rb") as f:
        if f.read(4) != b"XEX2":
            sys.exit("warning: default.xex does not start with XEX2; the image may be damaged")


if __name__ == "__main__":
    main()
