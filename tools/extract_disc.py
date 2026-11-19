#!/usr/bin/env python3
"""Extract files from a raw PS1 MODE2/2352 disc image (ISO9660).

Form1-only files are written as plain 2048-byte-per-sector data (trimmed to size).
Files containing Form2 sectors (XA audio, STR video) are written raw: 2352 bytes
per sector, so subheaders survive for later XA/MDEC decoding.

usage: extract_disc.py <image.bin> <out_dir>
"""
import json
import os
import struct
import sys

SECTOR = 2352


def read_sector(f, lba):
    f.seek(lba * SECTOR)
    return f.read(SECTOR)


def user_data(raw):
    return raw[24:24 + 2048]


def is_form2(raw):
    return bool(raw[18] & 0x20)


def parse_dir(f, lba, size, path, out):
    data = b"".join(user_data(read_sector(f, lba + i)) for i in range((size + 2047) // 2048))
    pos = 0
    while pos < len(data):
        ln = data[pos]
        if ln == 0:
            pos = (pos // 2048 + 1) * 2048  # skip to next sector
            continue
        rec = data[pos:pos + ln]
        ext_lba = struct.unpack_from("<I", rec, 2)[0]
        ext_size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen]
        pos += ln
        if name in (b"\x00", b"\x01"):
            continue
        name = name.decode("ascii", "replace").split(";")[0]
        full = f"{path}/{name}" if path else name
        if flags & 2:
            parse_dir(f, ext_lba, ext_size, full, out)
        else:
            out.append({"path": full, "lba": ext_lba, "size": ext_size})


def main():
    img, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    with open(img, "rb") as f:
        pvd = user_data(read_sector(f, 16))
        assert pvd[1:6] == b"CD001", "not ISO9660"
        root = pvd[156:156 + 34]
        files = []
        parse_dir(f, struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", files)
        for e in files:
            nsec = (e["size"] + 2047) // 2048
            sectors = [read_sector(f, e["lba"] + i) for i in range(nsec)]
            e["form2"] = any(is_form2(s) for s in sectors)
            dst = os.path.join(outdir, e["path"])
            os.makedirs(os.path.dirname(dst) or outdir, exist_ok=True)
            with open(dst, "wb") as o:
                if e["form2"]:
                    for s in sectors:
                        o.write(s)
                else:
                    o.write(b"".join(user_data(s) for s in sectors)[:e["size"]])
        meta = {
            "volume_id": pvd[40:72].decode("ascii", "replace").strip(),
            "files": sorted(files, key=lambda e: e["lba"]),
        }
    with open(os.path.join(outdir, "_manifest.json"), "w") as o:
        json.dump(meta, o, indent=1)
    print(f"{meta['volume_id']}: {len(files)} files -> {outdir}")


if __name__ == "__main__":
    main()
