"""Minimal PE32 loader for the recompiler."""
import struct
class Section:
    def __init__(s, name, va, vs, rp, rs, fl):
        s.name, s.va, s.vs, s.rp, s.rs, s.fl = name, va, vs, rp, rs, fl
    @property
    def exec(s): return bool(s.fl & 0x20000000)
class PE:
    def __init__(self, path, rebase=None, code_ranges=None):
        # code_ranges: [(lo_rva, hi_rva)] - the code part of executable sections, for images whose read-only data
        # is merged into .text (d3dx9_31.dll); None = every byte of an executable section is code
        self.code_ranges = code_ranges
        d = self.data = open(path, 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        _, nsec, self.timestamp, _, _, optsz, self.chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
        o = pe + 24
        self.entry_rva = struct.unpack_from('<I', d, o + 16)[0]
        self.base = struct.unpack_from('<I', d, o + 28)[0]
        self.size_image = struct.unpack_from('<I', d, o + 56)[0]
        self.size_headers = struct.unpack_from('<I', d, o + 60)[0]
        nr = struct.unpack_from('<I', d, o + 92)[0]
        self.dirs = [struct.unpack_from('<II', d, o + 96 + 8 * i) for i in range(nr)]
        so = o + optsz
        self.sections = []
        for i in range(nsec):
            nm, vs, va, rs, rp = struct.unpack_from('<8sIIII', d, so + 40 * i)
            fl = struct.unpack_from('<I', d, so + 40 * i + 36)[0]
            self.sections.append(Section(nm.rstrip(b'\0').decode('latin1'), va, vs, rp, rs, fl))
        # memory image (rva-indexed). A file whose length equals SizeOfImage is treated as an
        # already-mapped image (e.g. image.bin written by recomp.py), so the original PE is not needed.
        self.mapped = len(d) == self.size_image and any(s.rp != s.va for s in self.sections)
        if self.mapped:
            img = bytearray(d)
        else:
            img = bytearray(self.size_image)
            img[:self.size_headers] = d[:self.size_headers]
            for s in self.sections:
                n = min(s.rs, max(s.vs, s.rs))
                img[s.va:s.va + n] = d[s.rp:s.rp + n]
        self.img = bytes(img)
        self._relocs()
        if rebase is not None and rebase != self.base:
            delta = (rebase - self.base) & 0xffffffff
            for r in self.relocs:
                v = (struct.unpack_from('<I', img, r)[0] + delta) & 0xffffffff
                struct.pack_into('<I', img, r, v)
            self.base = rebase
            self.img = bytes(img)
        self._imports(); self._exports()
    def u32(self, rva): return struct.unpack_from('<I', self.img, rva)[0]
    def cstr(self, rva):
        e = self.img.index(b'\0', rva); return self.img[rva:e].decode('latin1')
    def section_of(self, rva):
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vs, s.rs): return s
    def is_code(self, va):
        rva = va - self.base
        if self.code_ranges is not None: return any(lo <= rva < hi for lo, hi in self.code_ranges)
        s = self.section_of(rva); return bool(s and s.exec)
    def is_data(self, va):
        """inside a section, but not code"""
        return self.section_of(va - self.base) is not None and not self.is_code(va)
    def _imports(self):
        self.imports = []  # (iat_va, dll, name)
        rva = self.dirs[1][0]
        while rva:
            oft, _, _, nm, ft = struct.unpack_from('<5I', self.img, rva)
            if not nm: break
            dll = self.cstr(nm); t = oft or ft; i = 0
            while True:
                v = self.u32(t + 4 * i)
                if not v: break
                name = '#%d' % (v & 0xffff) if v & 0x80000000 else self.cstr(v + 2)
                self.imports.append((self.base + ft + 4 * i, dll, name)); i += 1
            rva += 20
    def _exports(self):
        self.exports = {}; self.export_ords = {}   # name -> VA; name -> ordinal (unnamed exports are named '#N')
        rva, sz = self.dirs[0]
        if not rva: return
        nf, nn, af, anp, ao = struct.unpack_from('<5I', self.img, rva + 20)
        ordbase = struct.unpack_from('<I', self.img, rva + 16)[0]
        names = {}
        for i in range(nn):
            names[struct.unpack_from('<H', self.img, ao + 2 * i)[0]] = self.cstr(self.u32(anp + 4 * i))
        for i in range(nf):
            f = self.u32(af + 4 * i)
            if f and not (rva <= f < rva + sz):
                n = names.get(i, '#%d' % (ordbase + i))
                self.exports[n] = self.base + f; self.export_ords[n] = ordbase + i
    def _relocs(self):
        self.relocs = []
        rva, sz = self.dirs[5] if len(self.dirs) > 5 else (0, 0)
        end = rva + sz
        while rva and rva < end:
            page, bs = struct.unpack_from('<II', self.img, rva)
            if not bs: break
            for i in range((bs - 8) // 2):
                e = struct.unpack_from('<H', self.img, rva + 8 + 2 * i)[0]
                if e >> 12 == 3: self.relocs.append(page + (e & 0xfff))
            rva += bs
        self.reloc_set = set(self.relocs)
