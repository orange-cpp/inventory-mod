"""Small static-analysis helpers for the game's 32-bit DLLs (capstone + pefile, nothing else).

    python -I tools/re/pe_tools.py <dll> strings <substring>
    python -I tools/re/pe_tools.py <dll> xrefs <va|string:text>
    python -I tools/re/pe_tools.py <dll> rtti <class substring>
    python -I tools/re/pe_tools.py <dll> vtable <va> [count]
    python -I tools/re/pe_tools.py <dll> disasm <va> [max_instructions]
    python -I tools/re/pe_tools.py <dll> func <va>          (disassembles the whole function)

Addresses are virtual addresses at the file's preferred image base.  Cross references come from the base relocation
table: every absolute 32-bit address stored in the image is relocated, so the fixups that hold a given address are
exactly the places that reference it (code, vtables, data).  Install the libraries with
    python -m pip install --target %TEMP%\\relibs capstone pefile
(RE_LIBS overrides that path).
"""
import os
import struct
import sys

sys.path.insert(0, os.environ.get("RE_LIBS", os.path.join(os.environ.get("TEMP", "."), "relibs")))
import capstone  # noqa: E402
import pefile  # noqa: E402


class Image:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path, fast_load=True)
        self.pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"],
                         pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()  # indexed by RVA
        self.sections = [(s.Name.rstrip(b"\0").decode(), self.base + s.VirtualAddress, s.Misc_VirtualSize,
                          s.Characteristics) for s in self.pe.sections]
        self.cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.cs.detail = False
        self._relocs = None
        self._xrefs = None

    # ---- memory ---------------------------------------------------------------------------------------------------
    def read(self, va, n):
        rva = va - self.base
        return bytes(self.data[rva:rva + n])

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def section_of(self, va):
        for name, start, size, _ in self.sections:
            if start <= va < start + size:
                return name
        return None

    def is_code(self, va):
        for _, start, size, ch in self.sections:
            if start <= va < start + size:
                return bool(ch & 0x20000000)
        return False

    # ---- relocations / xrefs --------------------------------------------------------------------------------------
    def relocs(self):
        """Virtual addresses of every relocated 32-bit slot."""
        if self._relocs is None:
            self._relocs = []
            for block in getattr(self.pe, "DIRECTORY_ENTRY_BASERELOC", []):
                for entry in block.entries:
                    if entry.type == 3:  # IMAGE_REL_BASED_HIGHLOW
                        self._relocs.append(entry.rva + self.base)
        return self._relocs

    def xref_map(self):
        """target address -> list of slot addresses holding it."""
        if self._xrefs is None:
            self._xrefs = {}
            for slot in self.relocs():
                self._xrefs.setdefault(self.u32(slot), []).append(slot)
        return self._xrefs

    def xrefs_to(self, va):
        return sorted(self.xref_map().get(va, []))

    # ---- strings --------------------------------------------------------------------------------------------------
    def find_strings(self, needle, exact=False):
        """(va, text) of ASCII strings containing `needle` (the whole string when exact)."""
        out = []
        raw = bytes(self.data)
        pat = needle.encode()
        start = 0
        while True:
            i = raw.find(pat, start)
            if i < 0:
                break
            start = i + 1
            b = i
            while b > 0 and 32 <= raw[b - 1] < 127:
                b -= 1
            e = i
            while e < len(raw) and 32 <= raw[e] < 127:
                e += 1
            text = raw[b:e].decode()
            if exact and text != needle:
                continue
            if b == i or not exact:
                out.append((self.base + b, text))
        seen, uniq = set(), []
        for va, t in out:
            if va not in seen:
                seen.add(va)
                uniq.append((va, t))
        return uniq

    # ---- functions / disassembly ----------------------------------------------------------------------------------
    def function_start(self, va):
        """Walk back from `va` to the nearest 16-byte aligned function entry (after int3 padding or a ret)."""
        a = va & ~0xF
        for _ in range(0x4000):
            prev = self.read(a - 1, 1)
            if prev in (b"\xcc", b"\xc3") or self.read(a, 3) == b"\x55\x8b\xec":
                if self.read(a, 1) != b"\xcc":
                    return a
            a -= 0x10
        return va

    def disasm(self, va, max_insns=60, stop_at_ret=False):
        code = self.read(va, 0x2000)
        out = []
        for ins in self.cs.disasm(code, va):
            out.append(ins)
            if len(out) >= max_insns:
                break
            if stop_at_ret and ins.mnemonic in ("ret", "retn") and self.read(ins.address + ins.size, 1) in (b"\xcc", b"\x00"):
                break
        return out

    def format(self, va, max_insns=60, stop_at_ret=False):
        lines = []
        for ins in self.disasm(va, max_insns, stop_at_ret):
            note = ""
            for tok in ins.op_str.replace("[", " ").replace("]", " ").replace(",", " ").split():
                if tok.startswith("0x"):
                    try:
                        t = int(tok, 16)
                    except ValueError:
                        continue
                    if self.base <= t < self.base + self.pe.OPTIONAL_HEADER.SizeOfImage:
                        s = self.string_at(t)
                        if s:
                            note = f"   ; \"{s}\""
                        elif self.is_code(t):
                            note = f"   ; -> {t:#x}"
            lines.append(f"{ins.address:#010x}  {ins.mnemonic:6s} {ins.op_str}{note}")
        return "\n".join(lines)

    def string_at(self, va, maxlen=80):
        if self.section_of(va) is None:
            return None
        raw = self.read(va, maxlen)
        n = 0
        while n < len(raw) and 32 <= raw[n] < 127:
            n += 1
        if n >= 3 and (n == len(raw) or raw[n] == 0):
            return raw[:n].decode()
        return None

    # ---- RTTI -----------------------------------------------------------------------------------------------------
    def rtti_classes(self, needle=""):
        """[(class name, type descriptor va, vtable va)] for MSVC classes whose decorated name contains `needle`."""
        result = []
        for va, text in self.find_strings(".?AV" + needle if needle and not needle.startswith(".?") else needle or ".?AV"):
            if not text.startswith(".?A"):
                continue
            td = va - 8
            # a complete object locator holds the type descriptor at +12 and signature 0 at +0
            vtables = []
            for slot in self.xrefs_to(td):
                col = slot - 12
                if self.u32(col) == 0 and self.u32(col + 12) == td:
                    for ref in self.xrefs_to(col):
                        vtables.append(ref + 4)
            result.append((text, td, sorted(vtables)))
        return result

    def vtable_entries(self, vt, count=None):
        out = []
        i = 0
        while True:
            if count is not None and i >= count:
                break
            target = self.u32(vt + 4 * i)
            if not self.is_code(target):
                break
            out.append(target)
            i += 1
        return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    img = Image(argv[1])
    cmd = argv[2]
    args = argv[3:]

    def addr(text):
        if text.startswith("string:"):
            found = img.find_strings(text[7:], exact=True) or img.find_strings(text[7:])
            if not found:
                raise SystemExit(f"string not found: {text[7:]}")
            return found[0][0]
        return int(text, 16)

    if cmd == "strings":
        for va, text in img.find_strings(args[0]):
            print(f"{va:#010x} {img.section_of(va)}  {text}")
    elif cmd == "xrefs":
        va = addr(args[0])
        for slot in img.xrefs_to(va):
            sec = img.section_of(slot)
            where = f"in function {img.function_start(slot):#x}" if img.is_code(slot) else ""
            print(f"{slot:#010x} [{sec}] {where}")
    elif cmd == "rtti":
        for name, td, vts in img.rtti_classes(args[0] if args else ""):
            print(f"{name}  typedesc={td:#x}  vtables={[hex(v) for v in vts]}")
    elif cmd == "vtable":
        vt = addr(args[0])
        for i, t in enumerate(img.vtable_entries(vt, int(args[1]) if len(args) > 1 else None)):
            print(f"[{i:3d}] {t:#010x}")
    elif cmd == "disasm":
        print(img.format(addr(args[0]), int(args[1]) if len(args) > 1 else 60))
    elif cmd == "func":
        print(img.format(img.function_start(addr(args[0])) if False else addr(args[0]), 400, stop_at_ret=True))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
