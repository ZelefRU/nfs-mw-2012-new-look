"""PropInstanceList (resource type 0x218) of NFS Most Wanted 2012 track units: read, move, add.

Layout (found 2026-10-19, TRK_UNIT*.BNDL, 32-bit PC):
  header 16 bytes: u32 version (2), u32 offset of the instance array (16), u32 count, u32 0
  instance 0x60 bytes: 4x4 matrix (rows: right, up, forward, position; w of each row 1.0),
    +0x40 PropObject* (0 in the file: an import fills it), +0x44 0, +0x48 u32 the instance's id, +0x4C float 1.0,
    +0x50 u32 offset of its extra data (0: none), +0x54..0x5F 0
  then the extra blocks ("InstanceAdditiveAndAO": a header [0x101, name ptr, ptr, ptr], data, the name), with
  offsets from the start of the resource; then the import table (16-aligned): per instance the PropObject id and the
  offset of its +0x40 field.
Adding an instance puts 0x60 bytes (a multiple of 16: alignments kept) after the array: every offset at or past that
point moves by 0x60.
"""
import struct

INST = 0x60


class Props:
    def __init__(self, data, impoff, nimp):
        self.b = bytearray(data[:impoff] if nimp else data)   # (none: the whole is the header, its import offset 0)
        self.ver, self.arr, self.n, _ = struct.unpack_from('<4I', data, 0)
        self.imps = [struct.unpack_from('<QI', data, impoff + 16 * k) for k in range(nimp)]
        self.end = self.arr + self.n * INST

    def inst(self, i):
        o = self.arr + i * INST
        m = list(struct.unpack_from('<16f', self.b, o))
        iid, = struct.unpack_from('<I', self.b, o + 0x48)
        extra, = struct.unpack_from('<I', self.b, o + 0x50)
        obj = next((rid for rid, off in self.imps if off == o + 0x40), None)
        return dict(i=i, m=m, pos=(m[12], m[13], m[14]), id=iid, extra=extra, obj=obj)

    def _relocate(self, at, by):
        """before inserting `by` bytes at `at`: every offset field pointing at or past `at` moves by `by` (the
        instances' extra pointers, the extra blocks' own pointers and their name's pointer, the imports' offsets)"""
        mv = lambda v: v + by if v and v >= at else v
        blocks = set()
        for i in range(self.n):
            o = self.arr + i * INST + 0x50
            v, = struct.unpack_from('<I', self.b, o)
            if v:
                blocks.add(v)
                struct.pack_into('<I', self.b, o, mv(v))
        for e in sorted(blocks):
            hdr = list(struct.unpack_from('<4I', self.b, e))
            sp, = struct.unpack_from('<I', self.b, hdr[1]) if hdr[1] else (0,)
            if hdr[1]:
                struct.pack_into('<I', self.b, hdr[1], mv(sp))
            struct.pack_into('<4I', self.b, e, hdr[0], mv(hdr[1]), mv(hdr[2]), mv(hdr[3]))
        self.imps = [(rid, off + by if off >= at else off) for rid, off in self.imps]

    def add(self, src, matrix=None, iid=None, extra=False):
        """a copy of instance `src` (its PropObject), at `matrix` (16 floats), with a new id; no extra block"""
        s = self.inst(src)
        at = self.end
        new = bytearray(INST)
        struct.pack_into('<16f', new, 0, *(matrix or s['m']))
        struct.pack_into('<I', new, 0x48, iid if iid is not None else s['id'])
        struct.pack_into('<f', new, 0x4C, struct.unpack_from('<f', self.b, self.arr + src * INST + 0x4C)[0])
        self._relocate(at, INST)
        self.b[at:at] = new
        self.n += 1
        self.end += INST
        struct.pack_into('<I', self.b, 8, self.n)
        self.imps.append((s['obj'], at + 0x40))
        self.imps.sort(key=lambda t: t[1])
        return self.n - 1

    def move_matrix(self, i, m):
        struct.pack_into('<16f', self.b, self.arr + i * INST, *m)

    def remove(self, i):
        """instance i out (the ones after it come one place forward; its extra block stays, unused)"""
        o = self.arr + i * INST
        self.imps = [(rid, off) for rid, off in self.imps if not (o <= off < o + INST)]
        struct.pack_into('<I', self.b, o + 0x50, 0)
        self._relocate(o + INST, -INST)
        del self.b[o:o + INST]
        self.n -= 1
        self.end -= INST
        struct.pack_into('<I', self.b, 8, self.n)

    def move(self, i, pos):
        o = self.arr + i * INST
        struct.pack_into('<3f', self.b, o + 48, *pos)

    def build(self):
        body = bytes(self.b)
        if not self.imps:
            return body, 0, 0
        pad = (-len(body)) % 16
        body += b'\0' * pad
        impoff = len(body)
        tab = b''.join(struct.pack('<QII', rid, off, 0) for rid, off in self.imps)
        return body + tab, impoff, len(self.imps)
