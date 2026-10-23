"""Bundle 2 (NFS Most Wanted 2012 PC) reader / writer.

Layout (as found in HAWAII/TRK_UNIT*.BNDL): a 0x28-byte header (magic 'bnd2', version, platform, debug-data offset,
resource count, entry table offset, 4 block bases, flags), the entry table (72 bytes per resource), then block 0 and
block 1 back to back, each resource's part of a block zlib-compressed on its own (level 9 gives the original bytes),
packed with no gaps. An entry: id (u64), uncompressed size per block (the top 4 bits: an alignment flag), compressed
size per block, offset per block, import table offset (inside block 0's data), type, import count, flags, stream.
A resource's imports: 16 bytes each at its import offset: the imported resource id (u64), the offset in the data where
the pointer goes (u32), 0.
"""
import struct, zlib

HDR = '<4sHHIIIIIIII'
ENT = 72


class Res:
    def __init__(self, raw, d, bases):
        self.id, = struct.unpack_from('<Q', raw, 0)
        self.unc = list(struct.unpack_from('<4I', raw, 8))
        self.disk = list(struct.unpack_from('<4I', raw, 24))
        self.offs = list(struct.unpack_from('<4I', raw, 40))
        self.impoff, self.type, self.nimp, self.flags, self.stream = struct.unpack_from('<IIHBB', raw, 56)
        self.raw = raw
        self.comp = [d[bases[b] + self.offs[b]: bases[b] + self.offs[b] + self.disk[b]] if self.disk[b] else b'' for b in range(4)]
        self.new = [None] * 4   # replaced uncompressed data per block

    def data(self, b):
        if self.new[b] is not None:
            return self.new[b]
        if not self.disk[b]:
            return b''
        return zlib.decompress(self.comp[b])

    def imports(self):
        b = self.data(0)
        return [struct.unpack_from('<QI', b, self.impoff + 16 * k) for k in range(self.nimp)]


class Bundle:
    def __init__(self, path):
        d = open(path, 'rb').read()
        h = struct.unpack_from(HDR, d, 0)
        assert h[0] == b'bnd2', h[0]
        self.hdr = list(h)
        self.pre = d[:h[5]]            # header + padding up to the entry table
        bases = h[6:10]
        self.res = [Res(d[h[5] + i * ENT: h[5] + (i + 1) * ENT], d, bases) for i in range(h[4])]
        tail = max(bases[b] + r.offs[b] + r.disk[b] for r in self.res for b in range(4) if r.disk[b])
        self.tail = d[tail:]           # whatever follows the blocks (debug data)
        self.size = len(d)

    def find(self, rid):
        for r in self.res:
            if (r.id & 0xffffffff) == (rid & 0xffffffff):
                return r
        return None

    def set_data(self, r, b, data, impoff=None, nimp=None):
        r.new[b] = data
        if impoff is not None:
            r.impoff = impoff
        if nimp is not None:
            r.nimp = nimp

    def write(self, path):
        n = len(self.res)
        blocks = [bytearray() for _ in range(4)]
        ents = []
        for r in self.res:
            unc, disk, offs = list(r.unc), list(r.disk), list(r.offs)
            for b in range(4):
                if r.new[b] is not None:
                    c = zlib.compress(r.new[b], 9)
                    unc[b] = (r.unc[b] & 0xf0000000) | len(r.new[b])
                else:
                    c = r.comp[b]
                offs[b] = len(blocks[b]) if c else 0
                disk[b] = len(c)
                blocks[b] += c
            e = bytearray(r.raw)
            struct.pack_into('<4I', e, 8, *unc)
            struct.pack_into('<4I', e, 24, *disk)
            struct.pack_into('<4I', e, 40, *offs)
            struct.pack_into('<IIHBB', e, 56, r.impoff, r.type, r.nimp, r.flags, r.stream)
            ents.append(bytes(e))
        h = list(self.hdr)
        pos = h[5] + n * ENT
        bases = []
        for b in range(4):
            bases.append(pos)
            pos += len(blocks[b])
        end = pos
        # empty blocks and the debug offset point at the end (as in the originals)
        for b in range(4):
            if not blocks[b]:
                bases[b] = end
        h[6:10] = bases
        if self.hdr[3] == self.size:
            h[3] = end + len(self.tail) if self.tail else end
        out = bytearray(self.pre)
        struct.pack_into(HDR, out, 0, *h)
        for e in ents:
            out += e
        for b in range(4):
            out += blocks[b]
        out += self.tail
        open(path, 'wb').write(out)
        return len(out)
