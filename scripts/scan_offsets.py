#!/usr/bin/env python3
"""Offline patch-day helper: scan a new swtor.exe / RemoteRenderer.dll for the mod's signatures and print a
ready-to-paste offsets.txt section (same rules as src/core/resolver.cpp: each pattern must match exactly once in
.text; optional RIP-relative resolve).

    python3 -I scripts/scan_offsets.py <file.exe|dll> [more files...]

Signatures live in scripts/signatures.txt:
    <module> | <name> | <pattern> | <offset> | <resolve: - or r> | <insnEnd>
Exports needed by name are checked too (lines starting with 'export').
"""
import sys, os, re, pefile

HERE = os.path.dirname(os.path.abspath(__file__))


def load_sigs():
    sigs, exports = [], []
    path = os.path.join(HERE, "signatures.txt")
    if not os.path.exists(path):
        return sigs, exports
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split("|")]
        if parts[0] == "export":
            exports.append((parts[1], parts[2]))
        else:
            mod, name, pat, off, res, end = (parts + ["0", "-", "4"])[:6]
            sigs.append((mod.lower(), name, pat, int(off, 0), res, int(end, 0)))
    return sigs, exports


def compile_pat(p):
    rx = b""
    for tok in p.split():
        rx += b"." if tok.startswith("?") else re.escape(bytes([int(tok, 16)]))
    return re.compile(rx, re.S)


def main():
    sigs, exports = load_sigs()
    for f in sys.argv[1:]:
        pe = pefile.PE(f, fast_load=False)
        name = os.path.basename(f)
        text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
        data = text.get_data()
        print(f"[{name} 0x{pe.OPTIONAL_HEADER.SizeOfImage:08x} 0x{pe.FILE_HEADER.TimeDateStamp:08x}]")
        for mod, n, pat, off, res, end in sigs:
            if mod != name.lower():
                continue
            hits = [m.start() for m in compile_pat(pat).finditer(data)][:3]
            if len(hits) != 1:
                print(f"# {n}: {len(hits)} matches - needs a new signature")
                continue
            rva = text.VirtualAddress + hits[0] + off
            if res == "r":
                disp = int.from_bytes(data[hits[0] + off:hits[0] + off + 4], "little", signed=True)
                rva = rva + end + disp
            print(f"{n} = rva:0x{rva:08x}")
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
            names = {e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
            for mod, ex in exports:
                if mod.lower() == name.lower() and ex not in names:
                    print(f"# MISSING EXPORT {ex}")
        print()


if __name__ == "__main__":
    main()
