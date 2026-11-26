"""Bake Newer Super Mario Bros. Wii 1.30's code into a game version's code files, so the translator
sees the game as Newer's own loader leaves it in memory (this port cannot run code loaded at run
time: everything is translated ahead of time).

Usage:
  python newer_link.py <code dir> <NewerSMBW folder> <region tag, e.g. US_2> <out dir>

<code dir> has main.dol and the four .rel files (unpack_game_code.py). Writes the same files to
<out dir>, plus newer_seeds.txt (function starts for the function map) and newer_report.txt.

What Newer's Loader.bin does at boot, reproduced here (formats read from its disassembly):
  1. System<region>.bin - words written into the game. 0xFFFFFFFF ends; a word with the top bit set
     is an address followed by one value; otherwise a count N, an address and N values.
  2. DLCode<region>.bin - Newer's code and data, one flat image, loaded into a heap block.
  3. DLRelocs<region>.bin - "NewerREL", the offset of an address table, then (type<<24 | index,
     target) pairs. value = table[index]; value and target are relative to the DLCode block unless
     their top bit is set. Types: 1 ADDR32, 4 ADDR16_LO, 5 ADDR16_HI, 6 ADDR16_HA, 10 REL24.
Here DLCode gets a fixed address (0x81800000, the 2 MiB the runtime maps above MEM1) and becomes a
new text section and a new data section of main.dol. Words written into a REL go into its file at (address - load
address); a REL relocation that would later overwrite such a word is turned into R_DOLPHIN_NOP.
The riivolution XML's <memory> patches are applied too, except the two that install Loader.bin.
"""
import re, struct, sys
from pathlib import Path

DLCODE_BASE = 0x81800000
REL_NAMES = ("d_profileNP", "d_basesNP", "d_enemiesNP", "d_en_bossNP")


class Dol:
    def __init__(self, data):
        self.d = bytearray(data)
        self.off = list(struct.unpack(">18I", data[0:0x48]))
        self.addr = list(struct.unpack(">18I", data[0x48:0x90]))
        self.size = list(struct.unpack(">18I", data[0x90:0xD8]))

    def pos(self, a, n):
        for o, ad, s in zip(self.off, self.addr, self.size):
            if s and ad <= a and a + n <= ad + s:
                return o + a - ad
        return None

    def add_section(self, addr, blob, text):
        i = next(i for i in (range(7) if text else range(7, 18)) if self.size[i] == 0)
        while len(self.d) % 32: self.d.append(0)
        self.off[i], self.addr[i], self.size[i] = len(self.d), addr, len(blob)
        self.d += blob
        self.d[0:0x48] = struct.pack(">18I", *self.off)
        self.d[0x48:0x90] = struct.pack(">18I", *self.addr)
        self.d[0x90:0xD8] = struct.pack(">18I", *self.size)
        return i


class Rel:
    def __init__(self, name, data, load):
        self.name, self.d, self.load = name, bytearray(data), load
        cnt, info = struct.unpack(">II", data[0xC:0x14])
        self.sections = [struct.unpack(">II", data[info + 8 * i:info + 8 * i + 8]) for i in range(cnt)]
        self.end = max((o & ~1) + s for o, s in self.sections if o & ~1)
        self.imp_off, self.imp_size = struct.unpack(">I", data[0x28:0x2C])[0], struct.unpack(">I", data[0x2C:0x30])[0]
        self.written = set()  # file offsets of bytes Newer wrote

    def pos(self, a, n):
        p = a - self.load
        return p if 0x4C <= p and p + n <= self.end else None

    def is_text(self, a):
        p = a - self.load
        return any(o & 1 and (o & ~1) <= p < (o & ~1) + s for o, s in self.sections)

    def drop_relocations_over_writes(self):
        """Turn every relocation that lands on a byte Newer wrote into R_DOLPHIN_NOP (201), which
        keeps the running offset. Returns how many."""
        width = {1: 4, 2: 4, 3: 2, 4: 2, 5: 2, 6: 2, 7: 2, 10: 4, 11: 4, 12: 4, 13: 4}
        n = 0
        for i in range(self.imp_size // 8):
            _mod, rel = struct.unpack(">II", self.d[self.imp_off + 8 * i:self.imp_off + 8 * i + 8])
            base = pos = 0
            while True:
                delta, typ, sec, _add = struct.unpack(">HBBI", self.d[rel:rel + 8])
                if typ == 203: break
                pos += delta
                if typ == 202:
                    base, pos = self.sections[sec][0] & ~1, 0
                elif typ in width and any((base + pos + k) in self.written for k in range(width[typ])):
                    self.d[rel + 2] = 201
                    n += 1
                rel += 8
        return n


def parse_system(d):
    out, pos = [], 0
    while True:
        w = struct.unpack_from(">I", d, pos)[0]; pos += 4
        if w == 0xFFFFFFFF: return out
        if w & 0x80000000:
            out.append((w, [struct.unpack_from(">I", d, pos)[0]])); pos += 4
        else:
            a = struct.unpack_from(">I", d, pos)[0]; pos += 4
            out.append((a, list(struct.unpack_from(">%dI" % w, d, pos)))); pos += 4 * w


def parse_relocs(d):
    assert d[:8] == b"NewerREL"
    tab = struct.unpack_from(">I", d, 8)[0]
    table = struct.unpack_from(">%dI" % ((len(d) - tab) // 4), d, tab)
    return [(a >> 24, table[a & 0xFFFFFF], t) for a, t in
            (struct.unpack_from(">II", d, p) for p in range(0xC, tab, 8))]


def main():
    code_dir, newer, region, out = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4])
    loads = dict(zip(REL_NAMES, (int(x, 16) for x in sys.argv[5].split(","))))
    dol = Dol((code_dir / "main.dol").read_bytes())
    rels = [Rel(n, (code_dir / (n + ".rel")).read_bytes(), loads[n]) for n in REL_NAMES]
    code = bytearray((newer / "NewerRes" / ("DLCode%s.bin" % region)).read_bytes())
    report, game_writes = [], []

    def locate(a, n):
        if DLCODE_BASE <= a and a + n <= DLCODE_BASE + len(code):
            return code, a - DLCODE_BASE, None
        p = dol.pos(a, n)
        if p is not None: return dol.d, p, None
        for r in rels:
            p = r.pos(a, n)
            if p is not None: return r.d, p, r
        raise SystemExit("write to 0x%08X (%d bytes) is outside the game's code and data files" % (a, n))

    def write(a, data, why):
        buf, p, rel = locate(a, len(data))
        buf[p:p + len(data)] = data
        if rel: rel.written.update(range(p, p + len(data)))
        if buf is not code: game_writes.append((a, len(data), why))

    def read32(a):
        buf, p, _ = locate(a, 4)
        return struct.unpack_from(">I", buf, p)[0]

    # -- riivolution <memory> patches: the line whose `original` matches this version
    xml = (newer.parent / "riivolution" / "NewerSMBW.xml").read_text()
    for m in re.finditer(r'<memory offset="0x([0-9A-Fa-f]+)" value="([0-9A-Fa-f]+)" original="([0-9A-Fa-f]+)"', xml):
        a, val, orig = int(m.group(1), 16), bytes.fromhex(m.group(2)), bytes.fromhex(m.group(3))
        if val == bytes.fromhex("80001800"):
            continue  # "Inject Newer loader": replaced by this tool
        try:
            buf, p, _ = locate(a, len(orig))
        except SystemExit:
            continue
        if bytes(buf[p:p + len(orig)]) == orig:
            write(a, val, "xml")
            report.append("xml memory patch 0x%08X = %s (was %s)" % (a, val.hex(), orig.hex()))

    # -- 1. System file
    system = parse_system((newer / "NewerRes" / ("System%s.bin" % region)).read_bytes())
    for a, words in system:
        write(a, struct.pack(">%dI" % len(words), *words), "system")
    report.append("System file: %d patches, %d words" % (len(system), sum(len(w) for _, w in system)))

    # -- 3. relocations
    relocs = parse_relocs((newer / "NewerRes" / ("DLRelocs%s.bin" % region)).read_bytes())
    hooks = set()        # DLCode addresses the game branches to
    pointers = set()     # DLCode addresses stored as pointers (anywhere), or built by lis/addi
    game_pointers = set()  # game addresses Newer's data or code points at
    last_branch = 0
    for typ, value, target in relocs:
        v = value if value & 0x80000000 else DLCODE_BASE + value
        t = target if target & 0x80000000 else DLCODE_BASE + target
        if typ == 1:
            write(t, struct.pack(">I", v), "reloc")
        elif typ == 4:
            write(t, struct.pack(">H", v & 0xFFFF), "reloc")
        elif typ == 5:
            write(t, struct.pack(">H", v >> 16), "reloc")
        elif typ == 6:
            write(t, struct.pack(">H", ((v >> 16) + ((v >> 15) & 1)) & 0xFFFF), "reloc")
        elif typ == 10:
            write(t, struct.pack(">I", read32(t) | ((v - t) & 0x03FFFFFC)), "reloc")
            if not target & 0x80000000: last_branch = max(last_branch, target)
            if not value & 0x80000000 and target & 0x80000000: hooks.add(v)
        else:
            raise SystemExit("unknown relocation type %d" % typ)
        if typ in (1, 4) and not value & 0x80000000: pointers.add(v)
        if typ in (1, 4) and value & 0x80000000: game_pointers.add(v)
    report.append("Relocations: %d (%d hooks from the game into Newer's code)" % (len(relocs), len(hooks)))

    # -- function starts for the translator. Newer's text is the front of DLCode; its static
    # constructor table follows, then data. The image's first function is the constructor runner,
    # and the first address it builds (lis/ori, an ADDR16_HI relocation in its first 0x40 bytes) is
    # that table's start = the end of text. A pointer into text is taken as a function when the
    # word before it ends a function (blr, bctr, an unconditional b, or padding).
    text_end = DLCODE_BASE + min(v for typ, v, tg in relocs if typ == 5 and tg < 0x40 and not v & 0x80000000)
    if text_end & 3 or text_end < DLCODE_BASE + last_branch + 4 or \
            struct.unpack_from(">I", code, text_end - 4 - DLCODE_BASE)[0] != 0x4E800020:
        raise SystemExit("could not find the end of Newer's text (got 0x%08X)" % text_end)
    def starts_function(a):
        if a & 3 or not DLCODE_BASE <= a < text_end: return False
        if a == DLCODE_BASE: return True
        prev = struct.unpack_from(">I", code, a - 4 - DLCODE_BASE)[0]
        return prev in (0x4E800020, 0x4E800420, 0) or (prev >> 26 == 18 and not prev & 1)
    seeds = sorted(hooks | {a for a in pointers if starts_function(a)})
    skipped = sorted(a for a in pointers if DLCODE_BASE <= a < text_end and not starts_function(a))
    game_seeds = sorted(a for a in game_pointers if not a & 3 and (
        (dol.pos(a, 4) is not None and any(dol.addr[i] <= a < dol.addr[i] + dol.size[i] for i in range(7)))
        or any(r.is_text(a) for r in rels)))

    # Two sections, so the translator does not take pointers into Newer's data for code.
    split = text_end - DLCODE_BASE
    slot = dol.add_section(DLCODE_BASE, bytes(code[:split]), True)
    dslot = dol.add_section(text_end, bytes(code[split:]), False)
    report.append("DLCode: 0x%X bytes at 0x%08X: text 0x%X bytes (main.dol section %d), data from 0x%08X (section %d)"
                  % (len(code), DLCODE_BASE, split, slot, text_end, dslot))
    out.mkdir(parents=True, exist_ok=True)
    (out / "main.dol").write_bytes(dol.d)
    for r in rels:
        n = r.drop_relocations_over_writes()
        report.append("%s: %d bytes written, %d relocation(s) dropped" % (r.name, len(r.written), n))
        (out / (r.name + ".rel")).write_bytes(r.d)
    (out / "newer_seeds.txt").write_text("".join("%08X\n" % a for a in seeds))
    (out / "newer_game_seeds.txt").write_text("".join("%08X\n" % a for a in game_seeds))
    (out / "newer_game_writes.txt").write_text("".join("%08X %d %s\n" % w for w in sorted(set(game_writes))))
    report.append("Seeds: %d in Newer's code (%d pointers into its text skipped as not function starts), "
                  "%d game functions it points at" % (len(seeds), len(skipped), len(game_seeds)))
    (out / "newer_report.txt").write_text("\n".join(report) + "\n")
    print("\n".join(report))


if __name__ == "__main__":
    main()
