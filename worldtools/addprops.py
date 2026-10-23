"""Add prop copies to a track unit: addprops.py UNIT.BNDL out.BNDL src_index:dx:dy:dz [...]
Each copy: instance src_index's prop at its place + (dx, dy, dz) m, a new id (past every id in the city's units).
The unit is read from the original copy kept in worldtools/orig (made on first use)."""
import sys, os, shutil, struct
sys.path.insert(0, os.path.dirname(__file__))
import bndw, props

G = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'HAWAII') + os.sep
ORIG = os.path.join(os.path.dirname(__file__), 'orig')


def max_id():
    m = 0
    for f in os.listdir(G):
        if f.startswith('TRK_UNIT'):
            src = os.path.join(ORIG, f) if os.path.exists(os.path.join(ORIG, f)) else G + f
            for r in bndw.Bundle(src).res:
                if r.type == 0x218:
                    p = props.Props(r.data(0), r.impoff, r.nimp)
                    for i in range(p.n):
                        m = max(m, p.inst(i)['id'])
    return m


def main():
    unit, out = sys.argv[1], sys.argv[2]
    os.makedirs(ORIG, exist_ok=True)
    orig = os.path.join(ORIG, unit)
    if not os.path.exists(orig):
        shutil.copy2(G + unit, orig)
    b = bndw.Bundle(orig)
    r = next(r for r in b.res if r.type == 0x218)
    p = props.Props(r.data(0), r.impoff, r.nimp)
    nid = max_id() + 1
    for spec in sys.argv[3:]:
        i, dx, dy, dz = spec.split(':')
        s = p.inst(int(i))
        m = list(s['m'])
        m[12] += float(dx); m[13] += float(dy); m[14] += float(dz)
        k = p.add(int(i), m, nid)
        print('added', k, 'a copy of', i, 'obj %08x' % (s['obj'] & 0xffffffff), 'at %.2f %.2f %.2f' % tuple(m[12:15]), 'id %x' % nid)
        nid += 1
    data, io, ni = p.build()
    b.set_data(r, 0, data, io, ni)
    b.write(out)
    # check: read back
    c = bndw.Bundle(out)
    rr = next(x for x in c.res if x.type == 0x218)
    q = props.Props(rr.data(0), rr.impoff, rr.nimp)
    ok = all(q.inst(k)['obj'] is not None for k in range(q.n)) and q.n == p.n
    for k in range(q.n):   # the extra blocks still read their name
        e = q.inst(k)['extra']
        if e:
            hdr = struct.unpack_from('<4I', q.b, e)
            sp, = struct.unpack_from('<I', q.b, hdr[1])
            ok = ok and q.b[sp:sp + 21] == b'InstanceAdditiveAndAO'
    others = all(x.data(0) == y.data(0) and x.data(1) == y.data(1) for x, y in zip(b.res, c.res) if x is not r)
    print('read back: %d props, every one with its prop object and extra block: %s; the other resources unchanged: %s' % (q.n, ok, others))


main()
