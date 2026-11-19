#!/usr/bin/env python3
"""Match PsyQ library signatures (lab313ru/psx_psyq_signatures) against a PS-X EXE.

usage:
  psyq_match.py <sigs_dir> <exe> scan            # rank PsyQ versions by matches
  psyq_match.py <sigs_dir> <exe> emit <version>  # print splat symbol_addrs lines
Only signatures matching exactly one location are accepted.
"""
import glob
import json
import os
import re
import struct
import sys


def load_exe(path):
    d = open(path, "rb").read()
    t_addr = struct.unpack_from("<I", d, 0x18)[0]
    return d[0x800:], t_addr


def compile_sig(sig):
    parts = []
    for tok in sig.split():
        parts.append(b"." if tok == "??" else re.escape(bytes([int(tok, 16)])))
    return re.compile(b"".join(parts), re.DOTALL)


def match_version(sigs_dir, ver, code, base):
    found = {}  # name -> addr
    objs = 0
    for jf in sorted(glob.glob(os.path.join(sigs_dir, ver, "*.json"))):
        lib = os.path.basename(jf).replace(".json", "")
        for obj in json.load(open(jf)):
            if "sig" not in obj:
                continue
            sig = obj["sig"].strip()
            if len(sig.split()) < 12:
                continue
            hits = [m.start() for m in compile_sig(sig).finditer(code)]
            # also require 4-byte alignment
            hits = [h for h in hits if h % 4 == 0]
            if len(hits) != 1:
                continue
            objs += 1
            for lab in obj["labels"]:
                found.setdefault(lab["name"], (base + hits[0] + lab["offset"], lib, obj["name"]))
    return objs, found


def main():
    sigs_dir, exe, mode = sys.argv[1:4]
    code, base = load_exe(exe)
    if mode == "scan":
        vers = sorted(d for d in os.listdir(sigs_dir) if d.isdigit())
        for v in vers:
            objs, found = match_version(sigs_dir, v, code, base)
            print(f"{v:>5}: {objs:4d} objs, {len(found):4d} symbols")
    else:
        v = sys.argv[4]
        _, found = match_version(sigs_dir, v, code, base)
        seen = set()
        for name, (addr, lib, obj) in sorted(found.items(), key=lambda x: x[1][0]):
            if addr in seen:
                continue
            seen.add(addr)
            print(f"{name} = 0x{addr:08X}; // {lib}:{obj}")


if __name__ == "__main__":
    main()
