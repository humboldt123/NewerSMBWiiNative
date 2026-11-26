"""Read NSMBW-Maps/address-map.txt (NSMBW-Updated, by RoadrunnerWMC; originally Ninji and CLF78) and
convert addresses between game versions. Used by the multi-version tools (map_function_map.py, ...).

The file has one [VERSION] block per game version. Each block may "extend" another and then lists
inclusive address ranges of the parent version with a constant offset:
    [E1]
    extend P1
    80000000-800b4600: +0x0
    800b4630-800b463f: -0x20
An address of the parent version inside a range maps to address + offset in this version; an address
in no range has no counterpart (the map is strict on purpose: unmapped means "not portable").
P1 (PAL rev 1, the version this repo is written against) is the root: "80000000-*: +0x0".

    m = AddressMap.load()
    m.convert(0x801D2D60, "P1", "E1")   # -> 0x801D2C20, or None when unmapped
"""
import re
from pathlib import Path

DEFAULT_PATH = Path(__file__).resolve().parents[3] / "NSMBW-Maps" / "address-map.txt"
_RANGE = re.compile(r"^([0-9A-Fa-f]{8})-([0-9A-Fa-f]{8}|\*):\s*([+-])0x([0-9A-Fa-f]+)")


# d_basesNP's BSS in USA rev 2: the map says +0x40 from P1 0x80990800 (the console's BSS block), the
# translator's image has it at +0x30 like the data before it (up to d_enemiesNP's load address).
LAYOUT_FIX = {"E2": [(0x80990800, 0x809A2C9F, 0x30)]}
# The Newer build (E2N) is translated with NSMBW_REL_BSS_CONSOLE=1: each REL's BSS is at its console
# address, the aligned end of fixSize. Rows: our P1 (translator-layout) BSS range -> that address.
#   REL          P1 last section end   BSS size   E2 console BSS
#   d_profileNP  0x8076BD44            0x208      0x8076D4A0
#   d_basesNP    0x8098FF18            0x12484    0x80990840
#   d_enemiesNP  0x80B110D8            0xB4E0     0x80B11460
#   d_en_bossNP  0x80B891C8            0x487C     0x80B89AE0
LAYOUT_FIX["E2N"] = [(0x8076BD44, 0x8076BD44 + 0x208 - 1, 0x8076D4A0 - 0x8076BD44),
                     (0x8098FF18, 0x8098FF18 + 0x12484 - 1, 0x80990840 - 0x8098FF18),
                     (0x80B110D8, 0x80B110D8 + 0xB4E0 - 1, 0x80B11460 - 0x80B110D8),
                     (0x80B891C8, 0x80B891C8 + 0x487C - 1, 0x80B89AE0 - 0x80B891C8)]


class AddressMap:
    def __init__(self, parents, ranges):
        self.parents = parents  # version -> parent version (None for the root)
        self.ranges = ranges    # version -> [(start, end, offset)] in parent-version addresses

    @classmethod
    def load(cls, path=DEFAULT_PATH):
        parents, ranges, current = {}, {}, None
        for raw in Path(path).read_text(encoding="utf-8").splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            m = re.match(r"^\[(\w+)\]$", line)
            if m:
                current = m.group(1)
                parents[current] = None
                ranges[current] = []
                continue
            if line.startswith("extend "):
                parents[current] = line.split()[1]
                continue
            m = _RANGE.match(line)
            if m:
                start = int(m.group(1), 16)
                end = 0xFFFFFFFF if m.group(2) == "*" else int(m.group(2), 16)
                offset = int(m.group(4), 16) * (1 if m.group(3) == "+" else -1)
                ranges[current].append((start, end, offset))
        return cls(parents, ranges)

    def _down(self, addr, version):
        """Parent-version address -> `version` address, or None."""
        for start, end, offset in self.ranges[version]:
            if start <= addr <= end:
                return addr + offset
        return None

    def _up(self, addr, version):
        """`version` address -> parent-version address, or None (inverse of _down)."""
        hits = [a - o for s, e, o in self.ranges[version] for a in [addr] if s <= a - o <= e]
        return hits[0] if len(hits) == 1 else None

    def _chain(self, version):
        chain = [version]
        while self.parents[chain[-1]] is not None:
            chain.append(self.parents[chain[-1]])
        return chain  # version, parent, ..., root

    def convert(self, addr, src, dst):
        if src == dst:
            return addr
        a = addr
        for v in self._chain(src)[:-1]:  # up to the root
            a = self._up(a, v)
            if a is None:
                return None
        for v in reversed(self._chain(dst)[:-1]):  # down from the root
            a = self._down(a, v)
            if a is None:
                return None
        return a

    def flat_ranges(self, version):
        """[(P1 start, P1 end, offset)] for `version`, with the parent chain composed (E2 extends
        E1 extends P1): what a version that extends P1 directly has in self.ranges."""
        chain = self._chain(version)[:-1]  # version, parent, ... (root left out)
        flat = [(0x80000000, 0xFFFFFFFF, 0)]
        for v in reversed(chain):  # from the root's child down to `version`
            out = []
            for s, e, o in flat:
                for s2, e2, o2 in self.ranges[v]:
                    lo, hi = max(s, s2 - o), min(e, e2 - o)
                    if lo <= hi:
                        out.append((lo, hi, o + o2))
            flat = sorted(out)
        # The map describes a console, where a REL's BSS is a separate block after fixSize. The
        # translator puts it right after the last section instead, so inside a REL's BSS the shift
        # is the one its last section has. Our P1 addresses there are translator-layout ones.
        for lo, hi, off in LAYOUT_FIX.get(version, ()):
            out = []
            for s, e, o in flat:
                if e < lo or s > hi:
                    out.append((s, e, o)); continue
                if s < lo: out.append((s, lo - 1, o))
                out.append((max(s, lo), min(e, hi), off))
                if e > hi: out.append((hi + 1, e, o))
            flat = sorted(out)
        return flat

    def convert_p1(self, addr, version):
        """P1 address -> `version`, through flat_ranges (so with LAYOUT_FIX applied)."""
        for s, e, o in self.flat_ranges(version):
            if s <= addr <= e:
                return addr + o
        return None
