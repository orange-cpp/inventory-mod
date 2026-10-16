"""Reads Valve VPK (version 1/2) archives: lists the files and extracts one, without any other library.

    python -I tools/re/vpk.py <pak01_dir.vpk> list [substring]
    python -I tools/re/vpk.py <pak01_dir.vpk> get <path inside> <out file>

The _dir.vpk holds the tree (extension -> directory -> name) and small files; the data of the others is in
pak01_NNN.vpk next to it (archive index 0x7FFF means "in the _dir file, after the tree").
"""
import os
import struct
import sys


def read_cstring(f):
    out = bytearray()
    while True:
        c = f.read(1)
        if not c or c == b"\0":
            return out.decode("latin-1")
        out += c


class Vpk:
    def __init__(self, path):
        self.path = path
        self.entries = {}
        with open(path, "rb") as f:
            signature, version, tree_size = struct.unpack("<III", f.read(12))
            if signature != 0x55AA1234:
                raise ValueError("not a VPK")
            if version == 2:
                f.read(16)
            header = f.tell()
            while True:
                ext = read_cstring(f)
                if not ext:
                    break
                while True:
                    directory = read_cstring(f)
                    if not directory:
                        break
                    while True:
                        name = read_cstring(f)
                        if not name:
                            break
                        crc, preload, archive, offset, length, term = struct.unpack("<IHHIIH", f.read(18))
                        data = f.read(preload)
                        full = (directory + "/" if directory.strip() else "") + name + "." + ext
                        self.entries[full.lower()] = (archive, offset, length, data)
            self.data_start = header + tree_size

    def read(self, name):
        archive, offset, length, preload = self.entries[name.lower()]
        if length == 0:
            return preload
        if archive == 0x7FFF:
            path, offset = self.path, self.data_start + offset
        else:
            path = self.path.replace("_dir.vpk", "_%03d.vpk" % archive)
        with open(path, "rb") as f:
            f.seek(offset)
            return preload + f.read(length)


def main(argv):
    v = Vpk(argv[1])
    if argv[2] == "list":
        needle = argv[3].lower() if len(argv) > 3 else ""
        for name in sorted(v.entries):
            if needle in name:
                print(name)
    elif argv[2] == "get":
        with open(argv[4], "wb") as f:
            f.write(v.read(argv[3]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
