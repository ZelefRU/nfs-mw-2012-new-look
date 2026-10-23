"""Bake the world editor's edits of props into the game's track units (HAWAII/TRK_UNIT*.BNDL).

  bake.py            what would be done (nothing written)
  bake.py --write    do it (the game must be closed): units rewritten (their originals kept in worldtools/orig on first
                     change), the baked lines taken out of scripts/NewLook_world.txt (the old file kept beside it)

NewLook_world.txt lines (the world editor, Ctrl+J):
  obj x y z tag [hide] [move dx dy dz turn deg scale s]     an object where the game puts it (cm), its axes' tag
  copy x y z tag n move dx dy dz turn deg scale s           a copy of the object at x y z (Ctrl+D)
A prop is found by its place (within 2 cm) and its axes' tag (as NewLook computes it); other objects (the city's
static models, pieces of them) are not props: their lines stay (the mod keeps drawing them edited).
The edit on a matrix (as NewLook): each axis row turned about the vertical (x' = c x + s z, z' = -s x + c z) and
scaled, the position moved.
"""
import sys, os, re, math, shutil, struct, time, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import bndw, props

GAME = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
HAWAII = os.path.join(GAME, 'HAWAII')
WORLD = os.path.join(GAME, 'scripts', 'NewLook_world.txt')
if '--world' in sys.argv: WORLD = sys.argv[sys.argv.index('--world') + 1]   # (tests)
ORIG = os.path.join(os.path.dirname(__file__), 'orig')


def tag(X):   # NewLook's WeTag of the axes (FNV-1a over round(x * 256))
    h = 2166136261
    for i in range(3):
        for j in range(3):
            a = int(math.floor(X[i][j] * 256 + 0.5)) & 0xffffffff
            h = ((h ^ a) * 16777619) & 0xffffffff
    return h


def axes(m):
    return [m[0:3], m[4:7], m[8:11]]


def edited(m, off, yaw, scale):
    a = math.radians(yaw)
    c, s = math.cos(a), math.sin(a)
    out = list(m)
    for r in range(3):
        x, y, z = m[r * 4: r * 4 + 3]
        out[r * 4: r * 4 + 3] = [(c * x + s * z) * scale, y * scale, (-s * x + c * z) * scale]
    out[12] = m[12] + off[0]; out[13] = m[13] + off[1]; out[14] = m[14] + off[2]
    return out


def parse(path):
    edits = []
    for n, line in enumerate(open(path, encoding='utf-8', errors='replace')):
        t = line.split()
        if not t or t[0] not in ('obj', 'copy'):
            continue
        e = dict(line=n, kind=t[0], pos=tuple(float(v) for v in t[1:4]), tag=int(t[4], 16), hide=False,
                 off=(0.0, 0.0, 0.0), yaw=0.0, scale=1.0, part=False, cn=0)
        k = 5
        if t[0] == 'copy':
            e['cn'] = int(t[5]); k = 6
        while k < len(t):
            w = t[k]
            if w == 'hide': e['hide'] = True; k += 1
            elif w == 'part': e['part'] = True; k += 7
            elif w == 'move': e['off'] = tuple(float(v) for v in t[k + 1:k + 4]); k += 4
            elif w == 'turn': e['yaw'] = float(t[k + 1]); k += 2
            elif w == 'scale': e['scale'] = float(t[k + 1]); k += 2
            else: k += 1
        edits.append(e)
    return edits


def game_running():
    try:
        out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq NFS13.exe'], capture_output=True, text=True).stdout
        return 'NFS13.exe' in out
    except Exception:
        return False


def main():
    write = '--write' in sys.argv
    edits = parse(WORLD)
    # every prop of every unit
    units = {}
    index = {}   # (x cm, y cm, z cm) -> [(unit, i, matrix)]
    for f in sorted(os.listdir(HAWAII)):
        if not f.startswith('TRK_UNIT') or not f.endswith('.BNDL'):
            continue
        b = bndw.Bundle(os.path.join(HAWAII, f))
        r = next((x for x in b.res if x.type == 0x218), None)
        if not r:
            continue
        p = props.Props(r.data(0), r.impoff, r.nimp)
        units[f] = (b, r, p)
        for i in range(p.n):
            m = p.inst(i)['m']
            index.setdefault(tuple(int(math.floor(v * 100 + 0.5)) for v in m[12:15]), []).append((f, i, m))

    def find(e):
        q = [int(math.floor(v * 100 + 0.5)) for v in e['pos']]
        cands = []
        for dx in (-2, -1, 0, 1, 2):
            for dy in (-2, -1, 0, 1, 2):
                for dz in (-2, -1, 0, 1, 2):
                    cands += index.get((q[0] + dx, q[1] + dy, q[2] + dz), [])
        exact = [c for c in cands if tag(axes(c[2])) == e['tag']]
        if exact:
            return exact[0]
        return cands[0] if len(cands) == 1 else None

    plan = {}      # unit -> {'move': {i: m}, 'del': set, 'add': [(src i, m)]}
    baked, kept = set(), []
    nid = 0
    for f, (b, r, p) in units.items():
        for i in range(p.n):
            nid = max(nid, p.inst(i)['id'])
    nid += 1
    for e in edits:
        if e['part']:
            kept.append((e, 'a piece of a model (not a prop)')); continue
        hit = find(e)
        if not hit:
            kept.append((e, 'no prop there (a static model, or a prop moved by the game)')); continue
        f, i, m = hit
        pl = plan.setdefault(f, dict(move={}, dele=set(), add=[]))
        if e['kind'] == 'copy':
            pl['add'].append((i, edited(m, e['off'], e['yaw'], e['scale']), nid, e)); nid += 1
        else:
            if e['hide']:
                pl['dele'].add(i)
            elif e['off'] != (0, 0, 0) or e['yaw'] or e['scale'] != 1:
                pl['move'][i] = (edited(m, e['off'], e['yaw'], e['scale']), e)
        baked.add(e['line'])

    for f, pl in sorted(plan.items()):
        print('%s: %d moved, %d removed, %d copies added' % (f, len(pl['move']), len(pl['dele']), len(pl['add'])))
        for i, (m, e) in sorted(pl['move'].items()):
            print('   move   prop %d -> %.2f %.2f %.2f' % (i, m[12], m[13], m[14]))
        for i in sorted(pl['dele']):
            print('   remove prop %d at %.2f %.2f %.2f' % ((i,) + tuple(units[f][2].inst(i)['pos'])))
        for i, m, iid, e in pl['add']:
            print('   copy   of prop %d -> %.2f %.2f %.2f (id %x)' % (i, m[12], m[13], m[14], iid))
    for e, why in kept:
        print('kept in NewLook_world.txt (line %d, %s at %.2f %.2f %.2f): %s' % ((e['line'] + 1, e['kind']) + e['pos'] + (why,)))
    if not plan:
        print('nothing to bake')
        return
    if not write:
        print('\n(nothing written: bake.bat --write, with the game closed)')
        return
    if game_running():
        print('the game is running: close it first'); sys.exit(1)
    os.makedirs(ORIG, exist_ok=True)
    stamp = time.strftime('%Y%m%d_%H%M%S')
    for f, pl in plan.items():
        b, r, p = units[f]
        for i, (m, e) in pl['move'].items():
            p.move_matrix(i, m)
        for i, m, iid, e in pl['add']:
            p.add(i, m, iid)
        for i in sorted(pl['dele'], reverse=True):
            p.remove(i)
        data, io, ni = p.build()
        b.set_data(r, 0, data, io, ni)
        src = os.path.join(HAWAII, f)
        if not os.path.exists(os.path.join(ORIG, f)):
            shutil.copy2(src, os.path.join(ORIG, f))
        tmp = src + '.new'
        b.write(tmp)
        chk = bndw.Bundle(tmp)
        rr = next(x for x in chk.res if x.type == 0x218)
        q = props.Props(rr.data(0), rr.impoff, rr.nimp)
        assert q.n == p.n and all(q.inst(k)['obj'] is not None for k in range(q.n)), f
        os.replace(tmp, src)
        print('written', f, '(%d props)' % q.n)
    shutil.copy2(WORLD, WORLD[:-4] + '_before_bake_%s.txt' % stamp)
    lines = open(WORLD, encoding='utf-8', errors='replace').read().splitlines(True)
    open(WORLD, 'w', encoding='utf-8').write(''.join(l for n, l in enumerate(lines) if n not in baked))
    print('NewLook_world.txt: %d baked lines taken out (the old file: NewLook_world_before_bake_%s.txt)' % (len(baked), stamp))


main()
