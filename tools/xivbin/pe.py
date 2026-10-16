"""Static analysis of ffxiv_dx11.exe: sections, functions, signatures, xrefs.

Nothing here runs the game. Functions come from the .pdata unwind table, so a
leaf function without unwind info is not found and callers fall back to a small
window around the address. Disassembly shells out to an llvm-objdump with the
x86 backend (macOS ships one as /usr/bin/objdump).
"""

import bisect
import os
import re
import shutil
import struct
import subprocess
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))

_OBJDUMP_CANDIDATES = [
    "llvm-objdump",
    "objdump",
    "/Library/Developer/CommandLineTools/usr/bin/llvm-objdump",
]


def find_objdump():
    for cand in _OBJDUMP_CANDIDATES:
        path = shutil.which(cand) or (cand if os.path.exists(cand) else None)
        if not path:
            continue
        out = subprocess.run([path, "--version"], capture_output=True, text=True).stdout
        if "x86-64" in out:
            return path
    raise SystemExit("no llvm-objdump with the x86-64 backend found")


class Image:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt_size = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        self.base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.sections = {}
        for i in range(nsec):
            off = opt + opt_size + i * 40
            name = d[off:off + 8].rstrip(b"\0").decode()
            vsize, rva, rawsize, rawptr = struct.unpack_from("<IIII", d, off + 8)
            self.sections[name] = (rva, vsize, rawptr, rawsize)
        self._funcs = None
        self._objdump = None

    # --- layout ---------------------------------------------------------

    def section(self, name):
        """(bytes, va) of a section's raw data."""
        rva, vsize, rawptr, rawsize = self.sections[name]
        return self.data[rawptr:rawptr + min(vsize, rawsize)], self.base + rva

    def va_to_off(self, va):
        rva = va - self.base
        for s_rva, vsize, rawptr, rawsize in self.sections.values():
            if s_rva <= rva < s_rva + max(vsize, rawsize):
                return rawptr + (rva - s_rva)
        return None

    def section_of(self, va):
        rva = va - self.base
        for name, (s_rva, vsize, _, rawsize) in self.sections.items():
            if s_rva <= rva < s_rva + max(vsize, rawsize):
                return name
        return None

    def _unwind_parent(self, unwind_rva):
        """The begin RVA of the entry an unwind record chains to, or None for a
        primary entry. MSVC splits a function into several .pdata entries, each
        after the first naming its parent: through UNW_FLAG_CHAININFO, with the
        parent's RUNTIME_FUNCTION after the unwind codes, or through an indirect
        entry, whose unwind RVA has its low bit set and points at the parent."""
        if unwind_rva & 1:
            off = self.va_to_off(self.base + (unwind_rva & ~1))
            return struct.unpack_from("<I", self.data, off)[0] if off is not None else None
        off = self.va_to_off(self.base + unwind_rva)
        if off is None:
            return None
        flags = self.data[off] >> 3
        if not flags & 0x4:  # UNW_FLAG_CHAININFO
            return None
        codes = self.data[off + 2]
        parent = off + 4 + 2 * ((codes + 1) & ~1)
        return struct.unpack_from("<I", self.data, parent)[0]

    @property
    def funcs(self):
        """Function ranges from .pdata, with each function's chained fragments
        merged into one range where they follow each other."""
        if self._funcs is None:
            pdata, _ = self.section(".pdata")
            entries = []
            parent_of = {}
            for i in range(0, len(pdata) - 11, 12):
                begin, end, unwind = struct.unpack_from("<III", pdata, i)
                if begin == 0:
                    break
                entries.append((begin, end))
                parent = self._unwind_parent(unwind)
                if parent is not None and parent != begin:
                    parent_of[begin] = parent

            def root(begin):
                seen = set()
                while begin in parent_of and begin not in seen:
                    seen.add(begin)
                    begin = parent_of[begin]
                return begin

            entries.sort()
            funcs = []
            last_root = None
            for begin, end in entries:
                r = root(begin)
                if funcs and r == last_root and begin == funcs[-1][1] - self.base:
                    funcs[-1] = (funcs[-1][0], self.base + end)
                else:
                    funcs.append((self.base + begin, self.base + end))
                    last_root = r
            self._funcs = (funcs, [f[0] for f in funcs])
        return self._funcs[0]

    def func_containing(self, va):
        funcs = self.funcs
        starts = self._funcs[1]
        i = bisect.bisect_right(starts, va) - 1
        if i >= 0 and funcs[i][0] <= va < funcs[i][1]:
            return funcs[i]
        return None

    # --- signatures -----------------------------------------------------

    def find_pattern(self, pattern, section=".text"):
        """All VAs matching an IDA-style pattern ('48 8D 0D ? ? ? ?')."""
        rx = b"".join(
            b"." if tok in ("?", "??") else re.escape(bytes([int(tok, 16)]))
            for tok in pattern.split()
        )
        text, va = self.section(section)
        return [va + m.start() for m in re.finditer(rx, text, re.S)]

    def rip_target(self, ins_va, disp_off, ins_len):
        """Target of a RIP-relative operand, e.g. (ins, 3, 7) for 48 8D 0D disp32."""
        disp = struct.unpack_from("<i", self.data, self.va_to_off(ins_va) + disp_off)[0]
        return ins_va + ins_len + disp

    def jump_table(self, targets_va, count, index_va=None, first_case=0):
        """Case values per target of an MSVC switch, as {target_va: [case, ...]}.

        MSVC emits `mov ecx, [base + 4*idx + targets_rva]` over a table of image-relative
        dwords, optionally behind `movzx eax, byte [base + idx + index_rva]`, a byte table
        that folds cases sharing a handler. `count` is the number of cases (the compare
        bound plus one) and `first_case` what the switch subtracted before indexing.
        """
        cases = {}
        for i in range(count):
            slot = i
            if index_va is not None:
                slot = self.data[self.va_to_off(index_va) + i]
            rva = struct.unpack_from("<I", self.data, self.va_to_off(targets_va) + 4 * slot)[0]
            cases.setdefault(self.base + rva, []).append(first_case + i)
        return cases

    # --- disassembly ----------------------------------------------------

    def disasm(self, start, stop):
        """Instruction lines 'addr:\\tmnemonic\\toperands' for [start, stop)."""
        if self._objdump is None:
            self._objdump = find_objdump()
        out = subprocess.run(
            [self._objdump, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
             f"--start-address={start:#x}", f"--stop-address={stop:#x}", self.path],
            capture_output=True, text=True).stdout
        return [l.strip() for l in out.splitlines() if re.match(r"\s*[0-9a-f]+:", l)]

    def disasm_func(self, va, fallback=0x80):
        """The function holding va, or a window around it for a leaf without unwind info."""
        f = self.func_containing(va)
        if f:
            return f, self.disasm(*f)
        return None, self.disasm(va, va + fallback)

    # --- cross references -----------------------------------------------

    def _scanner(self):
        binary = os.path.join(os.environ.get("TMPDIR", "/tmp"), "ffxiv_hub_xrefs")
        src = os.path.join(HERE, "xrefs.cpp")
        if not os.path.exists(binary) or os.path.getmtime(binary) < os.path.getmtime(src):
            cxx = os.environ.get("CXX", "c++")
            if subprocess.call([cxx, "-std=c++17", "-O2", src, "-o", binary]) != 0:
                raise SystemExit("failed to build the xref scanner")
        return binary

    def _candidates(self, lo, hi):
        """Byte positions whose rel32 could land in [lo, hi): (site, kind)."""
        rva, vsize, rawptr, rawsize = self.sections[".text"]
        out = subprocess.run(
            [self._scanner(), self.path, str(rawptr), str(min(vsize, rawsize)),
             str(self.base + rva), str(lo), str(hi)],
            capture_output=True, text=True, check=True).stdout
        return [(int(a, 16), k) for a, k in (l.split() for l in out.splitlines())]

    def xrefs(self, target, span=1):
        """Instructions whose RIP-relative operand, call or jmp lands in
        [target, target+span). Candidates are confirmed against the disassembly,
        so overlapping byte matches that are not real operands drop out."""
        lo, hi = target, target + span
        by_func = defaultdict(list)
        for site, _ in self._candidates(lo, hi):
            by_func[self.func_containing(site)].append(site)
        want = re.compile(r"(?:#|call|jmp|j\w+)\s+0x([0-9a-f]+)")
        results = []
        for func, sites in by_func.items():
            if func:
                lines = self.disasm(*func)
            else:
                lines = [l for s in sites for l in self.disasm(s - 12, s + 8)]
            seen = set()
            for l in lines:
                m = want.search(l.split(":", 1)[1])
                if m and lo <= int(m.group(1), 16) < hi and l not in seen:
                    seen.add(l)
                    results.append((func[0] if func else None, l))
        return sorted(set(results), key=lambda r: r[1])

    def _stream_disasm(self, start, stop):
        """Instruction lines for [start, stop), streamed from one objdump run."""
        if self._objdump is None:
            self._objdump = find_objdump()
        proc = subprocess.Popen(
            [self._objdump, "-d", "--no-show-raw-insn", "--x86-asm-syntax=intel",
             f"--start-address={start:#x}", f"--stop-address={stop:#x}", self.path],
            stdout=subprocess.PIPE, text=True)
        try:
            for l in proc.stdout:
                if re.match(r"\s*[0-9a-f]+:", l):
                    yield l.strip()
        finally:
            proc.stdout.close()
            proc.wait()

    def field_accesses(self, offset, writes_only=False):
        """Instructions addressing [reg + offset], stack slots ([rsp + offset]) left
        out, e.g. every read or write of GroupManager.Group.MemberCount at 0x7FDC.

        An offset of 0x80 or more is a disp32, so its bytes find the functions to
        disassemble. A smaller one is a one-byte disp8, too common a byte to search
        for, so the whole of .text is disassembled once (about ten seconds)."""
        operand = f"+ {offset:#x}]"
        store = re.compile(r"^(mov\w*|and|or|xor|add|sub|inc|dec|not|neg|set\w+)\s+[^,]*\[")

        def wanted(l):
            if operand not in l or f"[rsp {operand}" in l:
                return False
            if writes_only:
                ins = l.split(":", 1)[1].strip()
                dest = ins.split(",")[0]
                return bool(store.match(ins)) and operand in dest
            return True

        text, va = self.section(".text")
        results = []
        if offset < 0x80:
            for l in self._stream_disasm(va, va + len(text)):
                if wanted(l):
                    f = self.func_containing(int(l.split(":", 1)[0], 16))
                    results.append((f[0] if f else None, l))
            return results
        needle = struct.pack("<I", offset)
        funcs = set()
        for m in re.finditer(re.escape(needle), text):
            f = self.func_containing(va + m.start())
            if f:
                funcs.add(f)
        for f in sorted(funcs):
            for l in self.disasm(*f):
                if wanted(l):
                    results.append((f[0], l))
        return results
