// ---------------------------------------------------------------- world editor (Ctrl+J): the game's own objects hidden, moved,
// turned, scaled. Only what is drawn: their collision stays where it was (DECOR_PLAN.md).
// Every prop the game draws has its world matrix in its vertex shader's constants ("world", VsInfo wSlot / wOff). The
// game refills those buffers with Map(WRITE_DISCARD) for every draw; while the editor is open or any edit exists, it
// writes into a copy of ours (fast to read) that goes into the driver's memory at Unmap. At a draw the matrix is read
// from that copy: its position (1 cm) and the tag of its axes make the object's key, the same in every pass (the
// picture, the game's and our shadow maps, reflections). An edited key: hidden = the draw is skipped; moved = drawn
// with a constant buffer of ours holding the changed matrix (put back after our shadow replays of it).
// Picking: one frame every G-buffer draw writes its number into an ID target, the one seen at the cross wins.
// Saved to NewLook_world.txt at every change.

static bool Pressed(int vk, bool repeat);
// the message line of the menu (g_lampMsg), cut to fit
static void WeMsg(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_lampMsg, sizeof(g_lampMsg) - 1, fmt, ap);
    va_end(ap);
    g_lampMsg[sizeof(g_lampMsg) - 1] = 0;
}

// ---- copies of the game's constant buffers
struct CbShadow {
    ID3D11Buffer* b;              // held (its pointer cannot become another resource while we know it)
    UINT size;
    unsigned char* cpu;           // 0: not one we copy (not a constant buffer, too big, immutable)
    void* mapped;                 // the driver's memory between Map and Unmap
    unsigned ver;                 // +1 at every fill
    bool valid, pending;
    unsigned keyVer; short keyOff; unsigned char lay; unsigned long long key;   // the key of the last matrix read from it
    unsigned added;               // the frame it was added
};
static const int kCbsSlots = 2048;
static CbShadow g_cbs[kCbsSlots];
static int g_cbsN;               // (g_cbsOn, newlook.cpp: the game's writes go through our copies)
static unsigned g_cbsSizes[8], g_cbsSizeN;   // the sizes seen (the log)

static CbShadow* CbsFind(const void* r)
{
    if (!g_cbsN) return 0;
    unsigned h = ((unsigned)(size_t)r >> 4) * 2654435761u;
    for (int k = 0; k < kCbsSlots; ++k) {
        CbShadow& c = g_cbs[(h + k) & (kCbsSlots - 1)];
        if (c.b == r) return &c;
        if (!c.b) return 0;
    }
    return 0;
}

static CbShadow* CbsAdd(ID3D11Buffer* b)
{
    if (g_cbsN >= kCbsSlots * 3 / 4) return 0;
    unsigned h = ((unsigned)(size_t)b >> 4) * 2654435761u;
    CbShadow* s = 0;
    for (int k = 0; k < kCbsSlots && !s; ++k) {
        CbShadow& c = g_cbs[(h + k) & (kCbsSlots - 1)];
        if (!c.b || c.b == b) s = &c;
    }
    if (!s || s->b == b) return s;
    memset(s, 0, sizeof(*s));
    s->b = b;
    b->AddRef();
    ++g_cbsN;
    D3D11_BUFFER_DESC bd;
    b->GetDesc(&bd);
    s->size = bd.ByteWidth;
    if ((bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd.ByteWidth >= 16 && bd.ByteWidth <= 65536 &&
        (bd.Usage == D3D11_USAGE_DYNAMIC || bd.Usage == D3D11_USAGE_DEFAULT)) {
        s->cpu = (unsigned char*)_aligned_malloc(bd.ByteWidth, 16);
        if (s->cpu) memset(s->cpu, 0, bd.ByteWidth);
        bool known = false;
        for (unsigned k = 0; k < g_cbsSizeN; ++k) known |= g_cbsSizes[k] == bd.ByteWidth;
        if (!known && g_cbsSizeN < 8) g_cbsSizes[g_cbsSizeN++] = bd.ByteWidth;
    }
    s->keyOff = -1;
    s->added = g_frame;
    return s;
}

// 1.27: what a constant buffer holds now, for a shadow replay drawn later (sunshadow.inc): 1 = its copy (data, size);
// 2 = its own contents do not change (immutable, or watched two frames and never written); 0 = not known (watched now)
static int WeCbNow(ID3D11Buffer* b, const unsigned char** data, UINT* size)
{
    if (!g_cbsCopy || !b) return 0;
    CbShadow* s = CbsFind(b);
    if (!s) { CbsAdd(b); return 0; }
    if (!s->cpu) {
        D3D11_BUFFER_DESC bd;
        b->GetDesc(&bd);
        return bd.Usage == D3D11_USAGE_IMMUTABLE ? 2 : 0;
    }
    if (s->pending) return 0;
    if (s->valid) { *data = s->cpu; *size = s->size; return 1; }
    return s->ver == 0 && g_frame - s->added >= 2 ? 2 : 0;
}

// buffers only we still hold (the game let them go): dropped; the table built again (at Present: nothing is mapped)
static void CbsPurge(bool all)
{
    static CbShadow keep[kCbsSlots];
    int n = 0;
    for (int k = 0; k < kCbsSlots; ++k) {
        CbShadow& c = g_cbs[k];
        if (!c.b) continue;
        ULONG refs = c.b->AddRef();
        c.b->Release();
        if (all || refs <= 2) { c.b->Release(); if (c.cpu) _aligned_free(c.cpu); }
        else keep[n++] = c;
    }
    memset(g_cbs, 0, sizeof(g_cbs));
    g_cbsN = 0;
    for (int i = 0; i < n; ++i) {
        unsigned h = ((unsigned)(size_t)keep[i].b >> 4) * 2654435761u;
        for (int k = 0; k < kCbsSlots; ++k) {
            CbShadow& c = g_cbs[(h + k) & (kCbsSlots - 1)];
            if (!c.b) { c = keep[i]; ++g_cbsN; break; }
        }
    }
}

// from the Map / Unmap / UpdateSubresource hooks (texreplace.inc)
static void CbsOnMap(ID3D11Resource* r, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* m)
{
    if (!g_cbsCopy || type != D3D11_MAP_WRITE_DISCARD || !m) return;
    CbShadow* s = CbsFind(r);
    if (!s || !s->cpu || s->pending) return;
    s->mapped = m->pData;
    m->pData = s->cpu;
    s->pending = true;
}
static void CbsOnUnmap(ID3D11Resource* r)
{
    CbShadow* s = CbsFind(r);
    if (!s || !s->pending) return;
    memcpy(s->mapped, s->cpu, s->size);
    s->pending = false;
    s->valid = true;
    ++s->ver;
}
static void CbsOnUpdate(ID3D11Resource* r, const D3D11_BOX* box, const void* data)
{
    if (!g_cbsCopy || !data) return;
    CbShadow* s = CbsFind(r);
    if (!s || !s->cpu) return;
    if (!box) { memcpy(s->cpu, data, s->size); s->valid = true; }
    else if (box->right <= s->size && box->left < box->right) memcpy(s->cpu + box->left, data, box->right - box->left);
    ++s->ver;
}

// ---- edits
struct WeEdit {
    unsigned long long key;
    int q[3]; unsigned rh;        // where the game puts it (cm) and its axes' tag: the key, as saved
    float off[3], yaw, scale;     // moved by (m), turned about the vertical (deg), scaled
    unsigned char hidden, part;   // part: one piece of a model the game draws in one go (key: the model's + the piece's box)
    unsigned long long model;     // part: its model's key
    float pmin[3], pmax[3];       // part: its box in the model's own units (mm steps)
    unsigned char copy;           // 1.58: a copy of the object at q / rh (key: WeCopyKey(its key, cn)); off / yaw / scale
    unsigned short cn;            //   from that object as the game puts it; hidden = deleted
    unsigned long long src;       //   the copied object's key
};
static unsigned long long WeCopyKey(unsigned long long src, unsigned cn) { unsigned long long h = src ^ 0xc2b2ae3d27d4eb4full; h ^= cn; h *= 1099511628211ull; return h ? h : 1; }
static const int kWeMax = 4096;
static WeEdit g_weEd[kWeMax];
static int g_weN;
static short g_weIdx[8192];       // key -> edit + 1
static char g_wePath[MAX_PATH];
static bool g_weApply = true, g_weShowHidden;   // (g_weOn: newlook.cpp)
static int g_weMoved, g_weHidden;   // counts (the menu)
static unsigned long long g_wePartModel[2048];   // models with edited parts (open addressing, 0 = empty)
static unsigned g_weVer = 1;                     // +1 at every change of the edits (the meshes' part lists follow)
static unsigned long long g_weCopySrc[2048];     // 1.58: objects with copies (open addressing, 0 = empty)
static int g_weCopies;

static bool WeIsMoved(const WeEdit& e) { return e.off[0] != 0 || e.off[1] != 0 || e.off[2] != 0 || e.yaw != 0 || e.scale != 1; }

static void WeIndex()
{
    memset(g_weIdx, 0, sizeof(g_weIdx));
    memset(g_wePartModel, 0, sizeof(g_wePartModel));
    memset(g_weCopySrc, 0, sizeof(g_weCopySrc));
    ++g_weVer;
    g_weMoved = g_weHidden = 0;
    g_weCopies = 0;
    for (int i = 0; i < g_weN; ++i) {
        unsigned h = (unsigned)(g_weEd[i].key ^ (g_weEd[i].key >> 29));
        for (int k = 0; k < 8192; ++k) { short& x = g_weIdx[(h + k) & 8191]; if (!x) { x = (short)(i + 1); break; } }
        if (g_weEd[i].part) {
            unsigned long long mk = g_weEd[i].model;
            unsigned hm = (unsigned)(mk ^ (mk >> 31));
            for (int k = 0; k < 2048; ++k) { unsigned long long& x = g_wePartModel[(hm + k) & 2047]; if (!x || x == mk) { x = mk; break; } }
        }
        if (g_weEd[i].copy) {
            unsigned long long sk = g_weEd[i].src;
            unsigned hs = (unsigned)(sk ^ (sk >> 31));
            for (int k = 0; k < 2048; ++k) { unsigned long long& x = g_weCopySrc[(hs + k) & 2047]; if (!x || x == sk) { x = sk; break; } }
            ++g_weCopies;
            continue;
        }
        if (g_weEd[i].hidden) ++g_weHidden;
        if (WeIsMoved(g_weEd[i])) ++g_weMoved;
    }
}

static WeEdit* WeFind(unsigned long long key)
{
    if (!g_weN) return 0;
    unsigned h = (unsigned)(key ^ (key >> 29));
    for (int k = 0; k < 8192; ++k) {
        short x = g_weIdx[(h + k) & 8191];
        if (!x) return 0;
        if (g_weEd[x - 1].key == key) return &g_weEd[x - 1];
    }
    return 0;
}

static bool WeHasCopies(unsigned long long sk)
{
    if (!g_weCopies) return false;
    unsigned hs = (unsigned)(sk ^ (sk >> 31));
    for (int k = 0; k < 2048; ++k) { unsigned long long x = g_weCopySrc[(hs + k) & 2047]; if (x == sk) return true; if (!x) return false; }
    return false;
}
static bool WeHasParts(unsigned long long mk)
{
    unsigned hm = (unsigned)(mk ^ (mk >> 31));
    for (int k = 0; k < 2048; ++k) { unsigned long long x = g_wePartModel[(hm + k) & 2047]; if (x == mk) return true; if (!x) return false; }
    return false;
}

static unsigned long long WeKeyPack(const int* q, unsigned rh)
{
    unsigned long long h = 1469598103934665603ull;
    const unsigned char* p = (const unsigned char*)q;
    for (int i = 0; i < 12; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    p = (const unsigned char*)&rh;
    for (int i = 0; i < 4; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

// a part's key: its model's key and its box (mm)
static unsigned long long WePartKey(unsigned long long model, const float* mn, const float* mx)
{
    unsigned long long h = model ^ 0x9e3779b97f4a7c15ull;
    for (int j = 0; j < 6; ++j) {
        int q = (int)floorf((j < 3 ? mn[j] : mx[j - 3]) * 1000 + 0.5f);
        const unsigned char* p = (const unsigned char*)&q;
        for (int i = 0; i < 4; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    }
    return h ? h : 1;
}

// the matrix in the game's constants: layout 1 = axes in rows 0-2, position in row 3 (w 1); 2 = the transposed one
static bool WeRead(const float* f, unsigned char* lay, float X[3][3], float T[3])
{
    if (fabsf(f[15] - 1) < 1e-3f && fabsf(f[3]) < 1e-4f && fabsf(f[7]) < 1e-4f && fabsf(f[11]) < 1e-4f) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) X[i][j] = f[i * 4 + j];
        T[0] = f[12]; T[1] = f[13]; T[2] = f[14];
        *lay = 1;
    } else if (fabsf(f[15] - 1) < 1e-3f && fabsf(f[12]) < 1e-4f && fabsf(f[13]) < 1e-4f && fabsf(f[14]) < 1e-4f) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) X[i][j] = f[j * 4 + i];
        T[0] = f[3]; T[1] = f[7]; T[2] = f[11];
        *lay = 2;
    } else {   // (1.17b) the game keeps something else in the w column: rows 0-2 axes, row 3 position, w left alone
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) X[i][j] = f[i * 4 + j];
        T[0] = f[12]; T[1] = f[13]; T[2] = f[14];
        *lay = 4;
    }
    for (int i = 0; i < 3; ++i) {
        float l2 = X[i][0] * X[i][0] + X[i][1] * X[i][1] + X[i][2] * X[i][2];
        if (!(l2 > 1e-6f && l2 < 1e6f)) { *lay = 0; return false; }
    }
    return fabsf(T[0]) < 1e6f && fabsf(T[1]) < 1e6f && fabsf(T[2]) < 1e6f;
}
static void WeWrite(float* f, unsigned char lay, const float X[3][3], const float T[3])
{
    if (lay == 1 || lay == 4) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) f[i * 4 + j] = X[i][j];
        f[12] = T[0]; f[13] = T[1]; f[14] = T[2];
    } else {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) f[j * 4 + i] = X[i][j];
        f[3] = T[0]; f[7] = T[1]; f[11] = T[2];
    }
}

static void WeTag(const float X[3][3], const float T[3], int* q, unsigned* rh)
{
    for (int j = 0; j < 3; ++j) q[j] = (int)floorf(T[j] * 100 + 0.5f);
    unsigned h = 2166136261u;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { int a = (int)floorf(X[i][j] * 256 + 0.5f); h = (h ^ (unsigned)a) * 16777619u; }
    *rh = h;
}

// 1.65: what a world matrix tells (its key, tag, place, scale), remembered by its 64 bytes: the same objects come with
// the same matrices every frame (~9000 draws a frame each paid 12 floorf and ~100 64-bit multiplies, a call each on
// 32-bit code, for the same answer). 4 places per hash, the oldest one replaced.
struct WeMemo { unsigned m[16]; unsigned long long key; int q[3]; unsigned rh; float T[3], scale; unsigned char lay, ok, valid; };
static WeMemo g_weMemo[8192];
static unsigned g_weMemoTurn;
static const WeMemo& WeMemoOf(const float* f)
{
    const unsigned* u = (const unsigned*)f;
    unsigned h = 0x9e3779b9u;
    for (int i = 0; i < 16; ++i) { h = (h ^ u[i]) * 0x85ebca6bu; h ^= h >> 13; }
    unsigned b = h & (8191 & ~3u);
    for (int k = 0; k < 4; ++k) { const WeMemo& m = g_weMemo[b + k]; if (m.valid && !memcmp(m.m, u, 64)) return m; }
    WeMemo& m = g_weMemo[b + (g_weMemoTurn++ & 3)];
    memcpy(m.m, u, 64);
    m.valid = 1; m.key = 0; m.rh = 0; m.scale = 1;
    memset(m.q, 0, sizeof(m.q)); memset(m.T, 0, sizeof(m.T));
    float X[3][3];
    m.ok = WeRead(f, &m.lay, X, m.T);
    if (!m.ok) return m;
    WeTag(X, m.T, m.q, &m.rh);
    float m2 = 0;
    for (int i = 0; i < 3; ++i) { float l2 = X[i][0] * X[i][0] + X[i][1] * X[i][1] + X[i][2] * X[i][2]; if (l2 > m2) m2 = l2; }
    m.scale = sqrtf(m2);
    bool ident = fabsf(m.T[0]) + fabsf(m.T[1]) + fabsf(m.T[2]) < 1e-3f;
    for (int i = 0; i < 3 && ident; ++i) for (int j = 0; j < 3; ++j) if (fabsf(X[i][j] - (i == j ? 1.0f : 0.0f)) > 1e-4f) ident = false;
    if (ident) m.lay = 3;
    else m.key = WeKeyPack(m.q, m.rh);
    return m;
}

// 0: not one object (the baked world is drawn with no matrix of its own: identity at the origin)
static unsigned long long WeKeyOf(const float* f, unsigned char* lay)
{
    const WeMemo& m = WeMemoOf(f);
    *lay = m.lay;
    return m.key;
}

// the edit on a matrix: scaled and turned about the vertical through the pivot (its own origin if none), then moved
static void WeApplyEditAbout(const WeEdit& e, float X[3][3], float T[3], const float* P)
{
    float a = e.yaw * 0.0174533f, c = cosf(a), s = sinf(a), k = e.scale;
    auto rot = [&](float* v) { float x = v[0], z = v[2]; v[0] = (c * x + s * z) * k; v[1] *= k; v[2] = (-s * x + c * z) * k; };
    for (int i = 0; i < 3; ++i) rot(X[i]);
    if (P) { float r[3] = { T[0] - P[0], T[1] - P[1], T[2] - P[2] }; rot(r); for (int j = 0; j < 3; ++j) T[j] = P[j] + r[j]; }
    for (int j = 0; j < 3; ++j) T[j] += e.off[j];
}
static void WeApplyEdit(const WeEdit& e, float X[3][3], float T[3]) { WeApplyEditAbout(e, X, T, 0); }
// a point of the model's own space in the world
static void WeLocalToWorld(const float X[3][3], const float T[3], const float* c, float* w)
{
    for (int j = 0; j < 3; ++j) w[j] = c[0] * X[0][j] + c[1] * X[1][j] + c[2] * X[2][j] + T[j];
}

// ---- saving
static void WeFmtCm(char* o, int q) { sprintf(o, "%s%d.%02d", q < 0 ? "-" : "", (q < 0 ? -q : q) / 100, (q < 0 ? -q : q) % 100); }

static void WeSave()
{
    FILE* f = fopen(g_wePath, "w");
    if (!f) { Log("world editor: %s could not be written", g_wePath); return; }
    fprintf(f, "# NewLook world edits (in game: Ctrl+J). One game object per line: where the game puts it (x y z in m) and the tag\n"
               "# of its axes, [part: the box of one piece of it, its own units], then what is done to it: hide, and/or\n"
               "# move dx dy dz (m), turn (deg, about the vertical), scale.\n"
               "# copy x y z tag n move .. turn .. scale ..: a copy (Ctrl+D) of the object at x y z, placed from it\n"
               "# (worldtools/bake.bat writes props' edits and copies into the game's track units)\n");
    for (int i = 0; i < g_weN; ++i) {
        const WeEdit& e = g_weEd[i];
        char x[16], y[16], z[16];
        WeFmtCm(x, e.q[0]); WeFmtCm(y, e.q[1]); WeFmtCm(z, e.q[2]);
        if (e.copy) {   // 1.58: a copy: of the object at x y z (tag), its number, its place from that object
            fprintf(f, "copy %s %s %s %08x %u move %.3f %.3f %.3f turn %.2f scale %.4f\n", x, y, z, e.rh, e.cn, e.off[0], e.off[1], e.off[2], e.yaw, e.scale);
            continue;
        }
        fprintf(f, "obj %s %s %s %08x", x, y, z, e.rh);
        if (e.part) fprintf(f, " part %.3f %.3f %.3f %.3f %.3f %.3f", e.pmin[0], e.pmin[1], e.pmin[2], e.pmax[0], e.pmax[1], e.pmax[2]);
        if (e.hidden) fprintf(f, " hide");
        if (WeIsMoved(e)) fprintf(f, " move %.3f %.3f %.3f turn %.2f scale %.4f", e.off[0], e.off[1], e.off[2], e.yaw, e.scale);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void WeLoad()
{
    g_weN = 0;
    FILE* f = fopen(g_wePath, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f) && g_weN < kWeMax) {
            double x, y, z;
            unsigned rh;
            int used = 0;
            unsigned cn = 0;
            bool isCopy = sscanf(line, "copy %lf %lf %lf %x %u%n", &x, &y, &z, &rh, &cn, &used) >= 5;
            if (!isCopy && sscanf(line, "obj %lf %lf %lf %x%n", &x, &y, &z, &rh, &used) < 4) continue;
            WeEdit e;
            memset(&e, 0, sizeof(e));
            e.copy = isCopy; e.cn = (unsigned short)cn;
            e.q[0] = (int)floor(x * 100 + 0.5); e.q[1] = (int)floor(y * 100 + 0.5); e.q[2] = (int)floor(z * 100 + 0.5);
            e.rh = rh;
            e.scale = 1;
            const char* p = line + used;
            char w[16];
            int n = 0;
            while (sscanf(p, "%15s%n", w, &n) == 1) {
                p += n;
                if (!strcmp(w, "hide")) e.hidden = 1;
                else if (!strcmp(w, "part") && sscanf(p, "%f %f %f %f %f %f%n", &e.pmin[0], &e.pmin[1], &e.pmin[2], &e.pmax[0], &e.pmax[1], &e.pmax[2], &n) == 6) { p += n; e.part = 1; }
                else if (!strcmp(w, "move") && sscanf(p, "%f %f %f%n", &e.off[0], &e.off[1], &e.off[2], &n) == 3) p += n;
                else if (!strcmp(w, "turn") && sscanf(p, "%f%n", &e.yaw, &n) == 1) p += n;
                else if (!strcmp(w, "scale") && sscanf(p, "%f%n", &e.scale, &n) == 1) p += n;
            }
            if (!(e.scale > 0.01f && e.scale < 100)) e.scale = 1;
            e.key = WeKeyPack(e.q, e.rh);
            if (e.copy) { e.src = e.key; e.key = WeCopyKey(e.src, e.cn); e.part = 0; e.hidden = 0; g_weEd[g_weN++] = e; continue; }
            if (e.part) { e.model = e.key; e.key = WePartKey(e.model, e.pmin, e.pmax); }
            if (e.hidden || WeIsMoved(e)) g_weEd[g_weN++] = e;
        }
        fclose(f);
    }
    WeIndex();
    Log("world editor: %d edits (%d hidden, %d moved, %d copies) from %s", g_weN, g_weHidden, g_weMoved, g_weCopies, g_wePath);
}

// ---- undo: the state of one object before a change
struct WeUndo { unsigned long long key; bool had; WeEdit e; char what[24]; unsigned grp; };
static unsigned g_weGrp;   // the action now going on (its steps are undone together)
static WeUndo g_weUndo[256], g_weRedo[256];
static int g_weUndoN, g_weRedoN;

static WeUndo WeState(unsigned long long key, const char* what)
{
    WeUndo u;
    memset(&u, 0, sizeof(u));
    u.key = key;
    const WeEdit* e = WeFind(key);
    u.had = e != 0;
    if (e) u.e = *e;
    strncpy(u.what, what, 23);
    return u;
}
static void WePush(unsigned long long key, const char* what)
{
    if (g_weUndoN == 256) { memmove(g_weUndo, g_weUndo + 1, sizeof(WeUndo) * 255); --g_weUndoN; }
    g_weUndo[g_weUndoN] = WeState(key, what);
    g_weUndo[g_weUndoN++].grp = g_weGrp;
    g_weRedoN = 0;
}

// the edit of an object, made if it has none (its key parts from the pick)
static WeEdit* WeEditOf(unsigned long long key, const int* q, unsigned rh)
{
    WeEdit* e = WeFind(key);
    if (e || g_weN >= kWeMax) return e;
    e = &g_weEd[g_weN++];
    memset(e, 0, sizeof(*e));
    e->key = key; memcpy(e->q, q, 12); e->rh = rh; e->scale = 1;
    WeIndex();
    return WeFind(key);
}
// an edit that does nothing any more goes away; counts again; saved
static void WeTidy()
{
    for (int i = 0; i < g_weN; ++i)
        if (g_weEd[i].copy ? g_weEd[i].hidden : (!g_weEd[i].hidden && !WeIsMoved(g_weEd[i]))) { g_weEd[i] = g_weEd[--g_weN]; --i; }
    WeIndex();
    WeSave();
}

static void WeUndoStep(bool redo)
{
    WeUndo* from = redo ? g_weRedo : g_weUndo;
    int& n = redo ? g_weRedoN : g_weUndoN;
    if (!n) { WeMsg(redo ? "world editor: nothing to redo" : "world editor: nothing to undo"); return; }
    WeUndo* to = redo ? g_weUndo : g_weRedo;
    int& m = redo ? g_weUndoN : g_weRedoN;
    unsigned grp = from[n - 1].grp;
    int steps = 0;
    char what[24];
    while (n && from[n - 1].grp == grp) {   // every object of that action
        WeUndo u = from[--n];
        strcpy(what, u.what);
        if (m == 256) { memmove(to, to + 1, sizeof(WeUndo) * 255); --m; }
        to[m] = WeState(u.key, u.what);
        to[m++].grp = grp;
        WeEdit* e = WeFind(u.key);
        if (u.had) {
            if (e) *e = u.e;
            else if (g_weN < kWeMax) g_weEd[g_weN++] = u.e;
        } else if (e) { if (e->copy) e->hidden = 1; else { e->hidden = 0; e->off[0] = e->off[1] = e->off[2] = 0; e->yaw = 0; e->scale = 1; } }
        WeIndex();
        ++steps;
    }
    WeTidy();
    WeMsg("world editor: %s %s%s (%d more)", redo ? "redone" : "undone", what, steps > 1 ? " (several objects)" : "", n);
}

// which instance a draw is (sunshadow.inc: its kept maps know their objects): its mesh (index / vertex buffer and range)
// and its world matrix from our copy of the game's constants (placed: one was there; T: its position)
static unsigned long long WeInstKey(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned long long* mesh, float* T, bool* placed, float* scale)
{
    *scale = 1;
    ID3D11Buffer* ib = 0; DXGI_FORMAT fmt; UINT ibOff = 0;
    ctx->IAGetIndexBuffer(&ib, &fmt, &ibOff);
    if (ib) ib->Release();
    ID3D11Buffer* vb = 0; UINT stride = 0, vbOff = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &vbOff);
    if (vb) vb->Release();
    // (1.65: two 32-bit lanes, a word at a time: FNV-64 a byte at a time cost ~70 64-bit multiplies, each a call here)
    unsigned h1 = 0x811c9dc5u, h2 = 0x9e3779b9u;
    auto mix = [&](unsigned v) {
        unsigned k = v * 0xcc9e2d51u; k = (k << 15) | (k >> 17); k *= 0x1b873593u;
        h1 ^= k; h1 = (h1 << 13) | (h1 >> 19); h1 = h1 * 5 + 0xe6546b64u;
        h2 += v * 0x85ebca6bu; h2 ^= h2 >> 13; h2 *= 0xc2b2ae35u;
    };
    auto fin = [&]() { unsigned a = h1 ^ (h1 >> 16), c = h2 ^ (h2 >> 15); a *= 0x85ebca6bu; c *= 0x27d4eb2fu; a ^= a >> 13; c ^= c >> 16;
                       return ((unsigned long long)a << 32) | c; };
    mix((unsigned)(size_t)ib); mix(ibOff); mix((unsigned)(size_t)vb); mix(vbOff); mix(d.kind); mix(d.a); mix(d.b); mix(d.c); mix((unsigned)d.base);
    *mesh = fin();
    *placed = false;
    if (e && e->wSlot >= 0 && e->wSlot <= 13 && e->wOff >= 0) {
        ID3D11Buffer* cb = 0;
        ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
        if (cb) {
            cb->Release();
            CbShadow* s = CbsFind(cb);
            if (!s) CbsAdd(cb);
            else if (s->cpu && s->valid && !s->pending && (UINT)e->wOff + 64 <= s->size) {
                const WeMemo& m = WeMemoOf((const float*)(s->cpu + e->wOff));
                if (m.ok) {
                    memcpy(T, m.T, 12);
                    *scale = m.scale;
                    mix((unsigned)m.q[0]); mix((unsigned)m.q[1]); mix((unsigned)m.q[2]); mix(m.rh);
                    *placed = true;
                }
            }
        }
    }
    unsigned long long h = fin();
    return h ? h : 1;
}

// ---- per draw (WE_DRAW / WE_SKIP / WE_SUBST: newlook.cpp)
struct WeStats { unsigned draws, world, lay[5], noData, keyed, skipped, subst, pickRec, maskDraws; double cpu; };
static WeStats g_weStat;
static unsigned long long g_weKey;   // the key of the draw now going on (0: none)
static CbShadow* g_weKeyCb;
static VsInfo* g_weKeyVs;
static ID3D11Buffer* g_weSaved;      // the game's buffer, put back after a substituted draw
static int g_weSavedSlot;
static unsigned g_weSavedPs;         // pixel-shader slots that had the same buffer
static ID3D11Buffer* g_wePool[8];
static UINT g_wePoolSize[8];
static unsigned char* g_weTmp;
static unsigned long long g_weSel;   // the selected object (its edit's key: the model's, or the part's)
static unsigned long long g_weSelObj;   // its model's key
static bool g_weSelIsPart, g_wePartWant;   // a part selected; the model's pieces wanted (middle button)
static float g_weSelPmin[3], g_weSelPmax[3];
// several selected (Ctrl + click adds / removes one): the set; the globals above are its last one (the one picked last)
struct WeSelItem { unsigned long long sel, obj; bool isPart; float pmin[3], pmax[3]; int q[3]; unsigned rh; float T[3], X[3][3]; };
static WeSelItem g_weSet[64];
static int g_weSetN;
static unsigned long long g_wePendModel;   // a model whose pieces are being read for a pick (a pick then finding another one stops)
static bool g_weSplitFail;           // a draw of the selected model could not be split
static int g_weSelQ[3]; static unsigned g_weSelRh;
static float g_weSelT[3], g_weSelX[3][3];   // where the game puts it
static void WeSelGet(WeSelItem& it)
{
    it.sel = g_weSel; it.obj = g_weSelObj; it.isPart = g_weSelIsPart;
    memcpy(it.pmin, g_weSelPmin, 12); memcpy(it.pmax, g_weSelPmax, 12); memcpy(it.q, g_weSelQ, 12); it.rh = g_weSelRh;
    memcpy(it.T, g_weSelT, 12); memcpy(it.X, g_weSelX, 36);
}
static void WeSelPut(const WeSelItem& it)
{
    g_weSel = it.sel; g_weSelObj = it.obj; g_weSelIsPart = it.isPart;
    memcpy(g_weSelPmin, it.pmin, 12); memcpy(g_weSelPmax, it.pmax, 12); memcpy(g_weSelQ, it.q, 12); g_weSelRh = it.rh;
    memcpy(g_weSelT, it.T, 12); memcpy(g_weSelX, it.X, 36);
}
static void WeSelClear() { g_weSetN = 0; g_weSel = g_weSelObj = 0; g_weSelIsPart = false; }
// is a model in the set, and as pieces (their meshes wanted)?
static bool WeSetHas(unsigned long long obj, bool* asPart)
{
    bool any = false;
    if (asPart) *asPart = false;
    for (int i = 0; i < g_weSetN; ++i) if (g_weSet[i].obj == obj) { any = true; if (g_weSet[i].isPart && asPart) *asPart = true; }
    return any;
}
static unsigned g_weSelSeen;         // frame it was last drawn
static volatile int g_wePickReq;     // 1 asked, 2 this frame records, 3 waiting for the read-back

static ID3D11Buffer* WePoolFor(UINT size)
{
    for (int k = 0; k < 8; ++k) if (g_wePool[k] && g_wePoolSize[k] == size) return g_wePool[k];
    for (int k = 0; k < 8; ++k) {
        if (g_wePool[k]) continue;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = size; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_wePool[k]))) { g_wePool[k] = 0; return 0; }
        g_wePoolSize[k] = size;
        return g_wePool[k];
    }
    return 0;
}

static bool WeGbuf(ID3D11DeviceContext* ctx, ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv, UINT* w, UINT* h);

// ---- a model's pieces: the game drew several things (boxes on a roof) as one mesh with one matrix. Its indices and
// vertices are read back once (a few a frame, never waiting), split into islands (triangles joined by a vertex, or by a
// vertex at the very same place: texture seams), each with its box (the model's own units) and the runs of triangles
// that are in a row in the index list. A model with edited parts is then drawn run group by run group.
struct WeRun { UINT first, count; int isl; };
struct WeMesh {
    ID3D11Buffer *ib, *vb; UINT ibOff, start, count, vbOff, stride; INT base; int ifmt, pfmt, poff; bool strip;
    int state;              // 0 free, 1 indices sent, 2 vertices sent, 3 ready, 4 cannot
    ID3D11Buffer* stage; unsigned sentFrame; UINT vmin, vmax; void* idx;
    int nIsl, nRun; float* box; WeRun* run;
    unsigned treatVer; unsigned long long treatKey; short* treat;   // each island's part edit (index, -1 none)
};
static const int kWeMeshes = 512;
static WeMesh g_weMesh[kWeMeshes];
static int g_weMeshJobs, g_weMeshDone, g_weMeshFail;
static unsigned g_weMeshJobFrame;
static WeMesh* g_weCurMesh;          // the draw now going on: its pieces (ready), the runs it covers
static int g_weRunFrom, g_weRunTo;
static UINT g_weDrawStart;
static bool g_weGroupHidden;         // a hidden part's group, drawn while hidden ones are shown

static WeMesh* WeMeshOf(ID3D11DeviceContext* ctx, const DrawCall& d, bool request)
{
    if (d.kind != DK_INDEXED) return 0;
    ID3D11Buffer *ib = 0, *vb = 0;
    DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0, stride = 0, vbOff = 0;
    ctx->IAGetIndexBuffer(&ib, &ifmt, &ibOff);
    ID3D11InputLayout* il = 0;
    ctx->IAGetInputLayout(&il);
    const IlPos* ip = il ? IlFind(il) : 0;
    if (il) il->Release();
    if (ip) ctx->IAGetVertexBuffers(ip->slot, 1, &vb, &stride, &vbOff);
    WeMesh* r = 0;
    if (ib && vb && stride) {
        unsigned h = (unsigned)(size_t)ib * 31u ^ (unsigned)(size_t)vb * 17u ^ d.b * 13u ^ d.a * 7u ^ (unsigned)d.base * 3u ^ ibOff ^ vbOff;
        for (int q = 0; q < 32 && !r; ++q) {
            WeMesh& m = g_weMesh[(h + q) & (kWeMeshes - 1)];
            if (m.state && m.ib == ib && m.vb == vb && m.start == d.b && m.count == d.a && m.base == d.base && m.ibOff == ibOff && m.vbOff == vbOff && m.stride == stride) { r = &m; break; }
            if (m.state) continue;
            if (!request) break;
            if (g_weMeshJobFrame != g_frame) { g_weMeshJobFrame = g_frame; g_weMeshJobs = 0; }
            if (g_weMeshJobs >= 3) break;
            D3D11_PRIMITIVE_TOPOLOGY top;
            ctx->IAGetPrimitiveTopology(&top);
            bool i16 = ifmt == DXGI_FORMAT_R16_UINT;
            bool okFmt = ip->fmt == DXGI_FORMAT_R32G32B32_FLOAT || ip->fmt == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                         ip->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT || ip->fmt == DXGI_FORMAT_R16G16B16A16_SNORM;
            memset(&m, 0, sizeof(m));
            m.ib = ib; m.vb = vb; m.ibOff = ibOff; m.start = d.b; m.count = d.a; m.base = d.base; m.vbOff = vbOff; m.stride = stride;
            m.ifmt = i16 ? 2 : 4; m.pfmt = ip->fmt; m.poff = ip->off;
            ib->AddRef(); vb->AddRef();   // held: the pointers stay these buffers
            m.state = 4;
            r = &m;
            m.strip = top == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            if (!okFmt || (top != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && !m.strip) || (!i16 && ifmt != DXGI_FORMAT_R32_UINT) || d.a < 3 || d.a > 1500000) {
                ++g_weMeshFail;
                if (g_weMeshFail < 8) Log("world editor: a mesh not split (position format %d, topology %d, index format %d, %u indices)", ip->fmt, top, ifmt, d.a);
                break;
            }
            D3D11_BUFFER_DESC sd = {}, ibd;
            sd.ByteWidth = d.a * m.ifmt; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ib->GetDesc(&ibd);
            UINT from = ibOff + d.b * m.ifmt;
            if (from + sd.ByteWidth > ibd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) { m.stage = 0; ++g_weMeshFail; break; }
            D3D11_BOX box = { from, 0, 0, from + sd.ByteWidth, 1, 1 };
            ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, ib, 0, &box);
            m.state = 1; m.sentFrame = g_frame;
            ++g_weMeshJobs;
        }
    }
    if (ib) ib->Release();
    if (vb) vb->Release();
    return r;
}

static int WeFindRoot(int* par, int a) { while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; } return a; }

// the islands of a mesh whose vertices have arrived
static void WeMeshBuild(WeMesh& m, const unsigned char* vdata)
{
    UINT nv = m.vmax - m.vmin + 1;
    const unsigned short* i16 = m.ifmt == 2 ? (const unsigned short*)m.idx : 0;
    const UINT* i32 = m.ifmt == 4 ? (const UINT*)m.idx : 0;
    auto raw = [&](UINT k) -> UINT { return i16 ? i16[k] : i32[k]; };
    auto ix = [&](UINT k) -> int { UINT v = raw(k); return v < m.vmin || v > m.vmax ? -1 : (int)(v - m.vmin); };
    // the real triangles in drawing order: their vertices, and where a draw of them starts / ends in the index list.
    // A strip (the game's props): the triangle at index k uses k, k+1, k+2, its winding set by k's parity since the last
    // cut; a draw of a part of the strip starts on the same parity (one index earlier if needed: that triangle is a
    // stitch between pieces, empty). Empty (stitch) triangles are left out.
    UINT nMax = m.strip ? (m.count >= 3 ? m.count - 2 : 0) : m.count / 3;
    int* tv = (int*)malloc((nMax ? nMax : 1) * 3 * sizeof(int));
    UINT* tpos = (UINT*)malloc((nMax ? nMax : 1) * sizeof(UINT));
    UINT* tstart = (UINT*)malloc((nMax ? nMax : 1) * sizeof(UINT));
    int* par = (int*)malloc(nv * sizeof(int));
    int* lab = (int*)malloc(nv * sizeof(int));
    int hsz = 1;
    while (hsz < (int)nv * 2) hsz <<= 1;
    int* hs = (int*)malloc(hsz * sizeof(int));
    int* tisl = (int*)malloc((nMax ? nMax : 1) * sizeof(int));
    auto freeAll = [&]() { free(tv); free(tpos); free(tstart); free(par); free(lab); free(hs); free(tisl); };
    if (!tv || !tpos || !tstart || !par || !lab || !hs || !tisl) { freeAll(); m.state = 4; return; }
    UINT nt = 0, cutAt = 0, cutV = i16 ? 0xffffu : 0xffffffffu;
    if (!m.strip) {
        for (UINT t = 0; t < nMax; ++t) {
            int a = ix(t * 3), b = ix(t * 3 + 1), c = ix(t * 3 + 2);
            if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) continue;
            tv[nt * 3] = a; tv[nt * 3 + 1] = b; tv[nt * 3 + 2] = c; tpos[nt] = t * 3; tstart[nt] = t * 3; ++nt;
        }
    } else {
        for (UINT k = 0; k + 2 < m.count; ++k) {
            if (raw(k) == cutV) { cutAt = k + 1; continue; }
            int a = ix(k), b = ix(k + 1), c = ix(k + 2);
            if (a < 0 || b < 0 || c < 0 || a == b || b == c || a == c) continue;
            tv[nt * 3] = a; tv[nt * 3 + 1] = b; tv[nt * 3 + 2] = c; tpos[nt] = k; tstart[nt] = ((k - cutAt) & 1) ? k - 1 : k; ++nt;
        }
    }
    for (UINT i = 0; i < nv; ++i) { par[i] = (int)i; lab[i] = -1; }
    // the same place = the same vertex (the position's x y z bytes)
    int psz = m.pfmt == DXGI_FORMAT_R16G16B16A16_FLOAT || m.pfmt == DXGI_FORMAT_R16G16B16A16_SNORM ? 6 : 12;
    for (int i = 0; i < hsz; ++i) hs[i] = -1;
    for (UINT v = 0; v < nv; ++v) {
        const unsigned char* p = vdata + v * m.stride + m.poff;
        unsigned h = 2166136261u;
        for (int b = 0; b < psz; ++b) h = (h ^ p[b]) * 16777619u;
        for (int k = 0; k < hsz; ++k) {
            int& slot = hs[(h + k) & (hsz - 1)];
            if (slot < 0) { slot = (int)v; break; }
            if (!memcmp(vdata + slot * m.stride + m.poff, p, psz)) { int a = WeFindRoot(par, (int)v), c = WeFindRoot(par, slot); if (a != c) par[a] = c; break; }
        }
    }
    for (UINT t = 0; t < nt; ++t) {
        int ra = WeFindRoot(par, tv[t * 3]), rb = WeFindRoot(par, tv[t * 3 + 1]);
        if (ra != rb) par[ra] = rb;
        int rc = WeFindRoot(par, tv[t * 3 + 2]); rb = WeFindRoot(par, tv[t * 3 + 1]);
        if (rc != rb) par[rc] = rb;
    }
    int nIsl = 0, nRun = 0;
    for (UINT t = 0; t < nt; ++t) {
        int r = WeFindRoot(par, tv[t * 3]);
        if (lab[r] < 0) lab[r] = nIsl++;
        tisl[t] = lab[r];
        if (t == 0 || tisl[t] != tisl[t - 1]) ++nRun;
    }
    m.box = (float*)malloc(sizeof(float) * 6 * (nIsl ? nIsl : 1));
    m.run = (WeRun*)malloc(sizeof(WeRun) * (nRun ? nRun : 1));
    m.treat = (short*)malloc(sizeof(short) * (nIsl ? nIsl : 1));
    if (!m.box || !m.run || !m.treat || !nt) { m.state = 4; freeAll(); return; }
    for (int i = 0; i < nIsl; ++i) { float* b = m.box + i * 6; b[0] = b[1] = b[2] = 1e30f; b[3] = b[4] = b[5] = -1e30f; }
    nRun = 0;
    for (UINT t = 0; t < nt; ++t) {
        int s = tisl[t];
        if (nRun && m.run[nRun - 1].isl == s) m.run[nRun - 1].count = tpos[t] + 3 - m.run[nRun - 1].first;
        else { m.run[nRun].first = tstart[t]; m.run[nRun].count = tpos[t] + 3 - tstart[t]; m.run[nRun].isl = s; ++nRun; }
        float* b = m.box + s * 6;
        for (int k = 0; k < 3; ++k) {
            const unsigned char* e = vdata + tv[t * 3 + k] * m.stride + m.poff;
            float p[3];
            if (m.pfmt == DXGI_FORMAT_R16G16B16A16_FLOAT) { const unsigned short* hh = (const unsigned short*)e; for (int j = 0; j < 3; ++j) p[j] = Half(hh[j]); }
            else if (m.pfmt == DXGI_FORMAT_R16G16B16A16_SNORM) { const short* hh = (const short*)e; for (int j = 0; j < 3; ++j) p[j] = fmaxf(hh[j] / 32767.0f, -1.0f) * 10.0f; }   // x 10 as the car shaders
            else memcpy(p, e, 12);
            for (int j = 0; j < 3; ++j) { if (p[j] < b[j]) b[j] = p[j]; if (p[j] > b[3 + j]) b[3 + j] = p[j]; }
        }
    }
    m.nIsl = nIsl; m.nRun = nRun;
    freeAll();
    m.state = 3;
    ++g_weMeshDone;
    if (g_weMeshDone <= 40) Log("world editor: mesh of %u triangles (%s, position format %d): %d pieces in %d runs", nt, m.strip ? "strip" : "list", m.pfmt, nIsl, nRun);
}

// the read-backs that have arrived (at Present, never waits)
static void WeMeshWork(ID3D11DeviceContext* ctx)
{
    for (int i = 0; i < kWeMeshes; ++i) {
        WeMesh& m = g_weMesh[i];
        if ((m.state != 1 && m.state != 2) || g_frame - m.sentFrame < 2) continue;
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT hr = ctx->Map(m.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) { m.stage->Release(); m.stage = 0; m.state = 4; ++g_weMeshFail; continue; }
        if (m.state == 1) {
            m.idx = malloc(m.count * m.ifmt);
            if (m.idx) memcpy(m.idx, mp.pData, m.count * m.ifmt);
            ctx->Unmap(m.stage, 0);
            m.stage->Release(); m.stage = 0;
            if (!m.idx) { m.state = 4; continue; }
            UINT mn = 0xffffffff, mx = 0;
            for (UINT k = 0; k < m.count; ++k) {
                UINT v = m.ifmt == 2 ? ((unsigned short*)m.idx)[k] : ((UINT*)m.idx)[k];
                if (v == (m.ifmt == 2 ? 0xffffu : 0xffffffffu)) continue;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            INT first = (INT)mn + m.base;
            D3D11_BUFFER_DESC vbd, sd = {};
            m.vb->GetDesc(&vbd);
            UINT from = m.vbOff + (UINT)first * m.stride, bytes = (mx - mn + 1) * m.stride;
            sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (mn > mx || first < 0 || bytes > (48u << 20) || from + bytes > vbd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) {
                m.stage = 0; m.state = 4; ++g_weMeshFail; free(m.idx); m.idx = 0; continue;
            }
            D3D11_BOX box = { from, 0, 0, from + bytes, 1, 1 };
            ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, m.vb, 0, &box);
            m.vmin = mn; m.vmax = mx; m.state = 2; m.sentFrame = g_frame;
            continue;
        }
        WeMeshBuild(m, (const unsigned char*)mp.pData);
        ctx->Unmap(m.stage, 0);
        m.stage->Release(); m.stage = 0;
        free(m.idx); m.idx = 0;
    }
}

// a piece's box centre inside a part's box (a little larger)?
static bool WeInBox(const float* box, const float* mn, const float* mx)
{
    for (int j = 0; j < 3; ++j) {
        float c = (box[j] + box[3 + j]) * 0.5f, e = (mx[j] - mn[j]) * 0.02f + 0.01f;
        if (c < mn[j] - e || c > mx[j] + e) return false;
    }
    return true;
}
// each piece's part edit for this model (kept until the edits change)
static void WeTreat(WeMesh& m, unsigned long long key)
{
    if (m.treatVer == g_weVer && m.treatKey == key) return;
    m.treatVer = g_weVer; m.treatKey = key;
    for (int i = 0; i < m.nIsl; ++i) m.treat[i] = -1;
    for (int k = 0; k < g_weN; ++k) {
        const WeEdit& e = g_weEd[k];
        if (!e.part || e.model != key) continue;
        for (int i = 0; i < m.nIsl; ++i) if (m.treat[i] < 0 && WeInBox(m.box + i * 6, e.pmin, e.pmax)) m.treat[i] = (short)k;
    }
}

// the camera from the game's own view of the main pass (ViewProjectionModified: rows x, y, depth with their offsets,
// then z scale, z bias, w scale, w bias): the editor works with NewLook's effects off (no camera record of ours then)
static CbShadow* g_weCamCb;
static unsigned g_weCamVer, g_weCamFrame;
static void WeCamFrom(const float* r)
{
    float ws = r[14], wb = r[15];
    if (fabsf(ws) < 1e-6f) return;   // (an orthographic view: not the main camera)
    float M[3][3] = { { r[0], r[1], r[2] }, { r[4], r[5], r[6] }, { r[8], r[9], r[10] } };
    float det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (fabsf(det) < 1e-12f) return;
    float I[3][3];
    I[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det; I[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det; I[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
    I[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det; I[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det; I[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det;
    I[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det; I[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det; I[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
    auto solve = [&](float x, float y, float d, float* p) {
        float b[3] = { x - r[3], y - r[7], d - r[11] };
        for (int i = 0; i < 3; ++i) p[i] = I[i][0] * b[0] + I[i][1] * b[1] + I[i][2] * b[2];
    };
    float E[3], d00[3], d10[3], d01[3];
    solve(0, 0, -wb / ws, E);              // the eye: w = 0
    float d1 = (1 - wb) / ws;              // depth where w = 1 (in front)
    solve(-1, 1, d1, d00); solve(1, 1, d1, d10); solve(-1, -1, d1, d01);   // screen (0,0), (1,0), (0,1)
    for (int j = 0; j < 3; ++j) {
        float a = d00[j] - E[j];
        g_cam[0][j] = -a; g_cam[1][j] = -(d10[j] - d00[j]); g_cam[2][j] = -(d01[j] - d00[j]); g_cam[3][j] = E[j];
    }
    g_camKnown = true;
    g_weCamFrame = g_frame;
}
static void WeCamNote(ID3D11DeviceContext* ctx, VsInfo* e)
{
    if (g_weCamFrame == g_frame) return;   // one a frame
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->vpSlot, 1, &cb);
    if (!cb) return;
    cb->Release();
    CbShadow* s = CbsFind(cb);
    if (!s) { CbsAdd(cb); return; }
    if (!s->cpu || !s->valid || s->pending || (UINT)e->vpOff + 64 > s->size || (s == g_weCamCb && s->ver == g_weCamVer)) return;
    g_weCamCb = s; g_weCamVer = s->ver;   // checked once per fill of the view's buffer
    ID3D11RenderTargetView* rtv[4] = {};
    ID3D11DepthStencilView* dsv = 0;
    UINT w, h;
    if (!WeGbuf(ctx, rtv, &dsv, &w, &h)) return;
    for (int k = 0; k < 4; ++k) if (rtv[k]) rtv[k]->Release();
    if (dsv) dsv->Release();
    WeCamFrom((const float*)(s->cpu + e->vpOff));
}

static bool WeBind(ID3D11DeviceContext* ctx, CbShadow* s, VsInfo* e, const WeEdit* med, const WeEdit* ped);
// before a game draw: its object's key; skip it (hidden) or draw it with our changed matrix
static int WeBefore(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    g_weKey = 0;
    g_weCopyFrom = 0;
    if (!Immediate(ctx)) return WE_DRAW;
    // CPU time: every 8th draw timed (x 8 in the log)
    struct Acc { LARGE_INTEGER t; bool on; ~Acc() { if (!on) return; LARGE_INTEGER q; QueryPerformanceCounter(&q); if (g_qpf.QuadPart) g_weStat.cpu += (q.QuadPart - t.QuadPart) * 8000.0 / g_qpf.QuadPart; } } acc;
    acc.on = (g_weStat.draws & 7) == 0;
    if (acc.on) QueryPerformanceCounter(&acc.t);
    ++g_weStat.draws;
    VsInfo* e = DrawVs(ctx);   // (1.65: asked once a draw)
    if (g_weOn && e && e->vpSlot >= 0 && e->vpSlot <= 13 && e->vpSlot != e->wSlot) WeCamNote(ctx, e);
    if (!e || e->wSlot < 0 || e->wSlot > 13 || e->car || e->wOff < 0) return WE_DRAW;
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    if (!cb) return WE_DRAW;
    cb->Release();           // (it stays bound)
    ++g_weStat.world;
    CbShadow* s = CbsFind(cb);
    if (!s) { CbsAdd(cb); ++g_weStat.noData; return WE_DRAW; }
    if (!s->cpu || !s->valid || s->pending || (UINT)e->wOff + 64 > s->size) { ++g_weStat.noData; return WE_DRAW; }
    if (s->keyVer != s->ver || s->keyOff != e->wOff) {
        s->key = WeKeyOf((const float*)(s->cpu + e->wOff), &s->lay);
        s->keyVer = s->ver; s->keyOff = e->wOff;
    }
    ++g_weStat.lay[s->lay < 5 ? s->lay : 0];
    static int dumps;
    if ((s->lay == 4 || s->lay == 0) && dumps < 12 && g_weOn) {   // what such a matrix holds (the log)
        const float* f = (const float*)(s->cpu + e->wOff);
        ++dumps;
        Log("world editor: matrix (layout %d, buffer %u bytes, world at %d): %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
            s->lay, s->size, e->wOff, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    }
    if (!s->key) return WE_DRAW;
    ++g_weStat.keyed;
    g_weKey = s->key; g_weKeyCb = s; g_weKeyVs = e;
    g_weCopyFrom = g_weApply && WeHasCopies(s->key) ? s->key : 0;
    g_weCurMesh = 0;
    bool inSetAsPart = false;
    if (g_weSetN && WeSetHas(s->key, &inSetAsPart) && s->key == g_weSelObj) g_weSelSeen = g_frame;
    WeEdit* ed = g_weApply ? WeFind(s->key) : 0;
    if (ed && ed->hidden && !(g_weOn && g_weShowHidden)) { ++g_weStat.skipped; g_weKey = 0; return WE_SKIP; }
    // its pieces: for a model with edited parts, and for the selected one when its parts are wanted (middle button)
    bool parts = g_weApply && WeHasParts(s->key);
    if (d.kind == DK_INDEXED && (parts || (g_weOn && (inSetAsPart || (g_wePartWant && s->key == g_wePendModel))))) {
        WeMesh* ms = WeMeshOf(ctx, d, true);
        if (ms && ms->state == 3) { g_weCurMesh = ms; g_weRunFrom = 0; g_weRunTo = ms->nRun; g_weDrawStart = d.b; }
        if (ms && ms->state == 4 && s->key == g_wePendModel) g_weSplitFail = true;
    }
    if (parts && g_weCurMesh) return WE_SPLIT;
    if (!ed || !WeIsMoved(*ed)) return WE_DRAW;
    return WeBind(ctx, s, e, ed, 0) ? WE_SUBST : WE_DRAW;
}

// a moved model / part: the game's constants with the matrix (and the previous frame's one) changed, in a buffer of
// ours bound instead of the game's (put back by WeAfter)
static bool WeBind(ID3D11DeviceContext* ctx, CbShadow* s, VsInfo* e, const WeEdit* med, const WeEdit* ped)
{
    ID3D11Buffer* our = WePoolFor(s->size);
    if (!our) return false;
    if (!g_weTmp) g_weTmp = (unsigned char*)_aligned_malloc(65536, 16);
    if (!g_weTmp) return false;
    memcpy(g_weTmp, s->cpu, s->size);
    short offs[2] = { e->wOff, e->wPrevOff };
    for (int k = 0; k < 2; ++k) {
        if (offs[k] < 0 || (UINT)offs[k] + 64 > s->size) continue;
        float X[3][3], T[3];
        unsigned char lay;
        float* f = (float*)(g_weTmp + offs[k]);
        if (!WeRead(f, &lay, X, T)) continue;
        if (med) WeApplyEdit(*med, X, T);
        if (ped) {   // a part: about its own centre
            float c[3], pv[3];
            for (int j = 0; j < 3; ++j) c[j] = (ped->pmin[j] + ped->pmax[j]) * 0.5f;
            WeLocalToWorld(X, T, c, pv);
            WeApplyEditAbout(*ped, X, T, pv);
        }
        WeWrite(f, lay, X, T);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(our, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, g_weTmp, s->size);
    ctx->Unmap(our, 0);
    ID3D11Buffer* cb = s->b;
    g_weSaved = cb; cb->AddRef();
    g_weSavedSlot = e->wSlot;
    ctx->VSSetConstantBuffers(e->wSlot, 1, &our);
    g_weSavedPs = 0;
    ID3D11Buffer* pcb[14] = {};
    ctx->PSGetConstantBuffers(0, 14, pcb);
    for (int k = 0; k < 14; ++k) {
        if (pcb[k] == cb) { ctx->PSSetConstantBuffers(k, 1, &our); g_weSavedPs |= 1u << k; }
        if (pcb[k]) pcb[k]->Release();
    }
    ++g_weStat.subst;
    return true;
}

static void WeAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d);
static void WeAfter(ID3D11DeviceContext* ctx);
// after the game's draw and all of ours after it (AfterDraw: our shadow replays drew it with the changed matrix too)
static void WeEnd(ID3D11DeviceContext* ctx, const DrawCall& d, int we)
{
    if (g_weKey) WeAfterDraw(ctx, d);
    if (we == WE_SUBST) WeAfter(ctx);
}

// a model with edited parts: drawn group by group of runs (each group: its part's edit, or the model's), every group
// through the whole draw path (our shadow replays included)
static void WeSplitDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    WeMesh* m = g_weCurMesh;
    CbShadow* s = g_weKeyCb;
    VsInfo* e = g_weKeyVs;
    unsigned long long key = g_weKey;
    if (!m || !s || !e) return;
    WeTreat(*m, key);
    const WeEdit* med = WeFind(key);
    bool showHidden = g_weOn && g_weShowHidden;
    int i = 0;
    while (i < m->nRun) {
        int t = m->treat[m->run[i].isl], j = i + 1;
        while (j < m->nRun && m->treat[m->run[j].isl] == t) ++j;
        const WeEdit* pe = t >= 0 && t < g_weN ? &g_weEd[t] : 0;
        UINT first = m->run[i].first, cnt = m->run[j - 1].first + m->run[j - 1].count - first;
        if (pe && pe->hidden && !showHidden) { ++g_weStat.skipped; i = j; continue; }
        DrawCall sd = { DK_INDEXED, cnt, d.b + first, 0, 0, d.base };
        const WeEdit* pm = pe && WeIsMoved(*pe) ? pe : 0;
        bool bound = (pm || (med && WeIsMoved(*med))) && WeBind(ctx, s, e, med, pm);
        g_weKey = key; g_weCurMesh = m; g_weRunFrom = i; g_weRunTo = j; g_weDrawStart = d.b;
        g_weGroupHidden = pe && pe->hidden;
        bool timed = BeforeDraw(ctx, sd);
        if (!SsBeforeGameDraw(ctx, sd)) oDrawIndexed(ctx, cnt, d.b + first, d.base);
        AfterDraw(ctx, sd, timed);
        WeAfterDraw(ctx, sd);
        g_weGroupHidden = false;
        if (bound) WeAfter(ctx);
        i = j;
    }
    g_weKey = 0;
}

// 1.58: the copies of the object just drawn: the same draw with each copy's matrix, through the whole draw path (our
// shadow replays included); picked and selected by their own keys
static void WeCopiesDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    unsigned long long src = g_weCopyFrom;
    g_weCopyFrom = 0;
    CbShadow* s = g_weKeyCb;
    VsInfo* e = g_weKeyVs;
    if (!src || !s || !e) return;
    for (int i = 0; i < g_weN; ++i) {
        const WeEdit c = g_weEd[i];
        if (!c.copy || c.src != src || c.hidden) continue;
        if (!WeBind(ctx, s, e, &c, 0)) continue;
        g_weKey = c.key; g_weKeyCb = s; g_weKeyVs = e; g_weCurMesh = 0;
        if (g_weSetN && WeSetHas(c.key, 0)) g_weSelSeen = g_frame;
        bool timed = BeforeDraw(ctx, d);
        if (!SsBeforeGameDraw(ctx, d)) Replay(ctx, d);
        AfterDraw(ctx, d, timed);
        WeAfterDraw(ctx, d);
        WeAfter(ctx);
    }
    g_weKey = 0;
}

static void WeAfter(ID3D11DeviceContext* ctx)
{
    if (!g_weSaved) return;
    ctx->VSSetConstantBuffers(g_weSavedSlot, 1, &g_weSaved);
    for (int k = 0; k < 14; ++k) if (g_weSavedPs & (1u << k)) ctx->PSSetConstantBuffers(k, 1, &g_weSaved);
    g_weSaved->Release();
    g_weSaved = 0;
    g_weSavedPs = 0;
}

// ---- pick (ID frame) and the selection mask
struct WePickEnt { unsigned long long key; int q[3]; unsigned rh; float T[3], X[3][3]; int isl, nIsl; float box[6]; };
static WePickEnt* g_wePick;
static int g_wePickN;
static unsigned g_wePickFrame;
static HalfRT g_weId, g_weMask;
static ID3D11Texture2D* g_weIdStage;
static ID3D11PixelShader *g_wePsId, *g_wePsOne, *g_wePsOverlay;
static ID3D11Buffer *g_weIdCb, *g_weOvCb;
static ID3D11DepthStencilState *g_weDsLess, *g_weDsOff;
static ID3D11BlendState* g_weBlendCh[3];   // writes R / G / B only
static unsigned g_weMaskFrame;
static bool g_weFailed;
static const char* kWeSrc = R"HLSL(
cbuffer WeId : register(b13) { float4 Id; };
float4 ps_id(float4 pos : SV_Position) : SV_Target { return Id.xxxx; }
float4 ps_one(float4 pos : SV_Position) : SV_Target { return 1; }
Texture2D<float4> tMask : register(t0);
cbuffer WeOv : register(b13) { float4 Rect; float4 Info; float4 Info2; float4 Seg[56]; float4 SegC[56]; };   // Info: screen w h, mask w h; Info2: segments, mask on
float4 ps_overlay(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float4 o = 0;
    if (Info2.y > 0.5) {
        int2 sz = int2(Info.zw), mp = int2(uv * Info.zw);
        float4 c = tMask.Load(int3(mp, 0)), mx = 0;
        [unroll] for (int y = -2; y <= 2; ++y)
            [unroll] for (int x = -2; x <= 2; ++x) mx = max(mx, tMask.Load(int3(clamp(mp + int2(x, y), int2(0, 0), sz - 1), 0)));
        float3 sel = float3(1.0, 0.62, 0.12);
        if (c.r > 0.5) o = float4(sel, 0.16);                         // the selected object, seen
        if (c.r < 0.5 && mx.r > 0.5) o = float4(sel, 1);              // its outline
        else if (c.g < 0.5 && mx.g > 0.5) o = float4(sel, 0.4);       // behind something
        if (c.b > 0.5) o = float4(lerp(o.rgb, float3(1, 0.12, 0.08), 0.7), max(o.a, 0.45));   // hidden objects, shown
        if (c.b < 0.5 && mx.b > 0.5) o = float4(1, 0.12, 0.08, 0.9);
    }
    float2 p = pos.xy;
    int n = (int)Info2.x;
    for (int k = 0; k < n; ++k) {
        float2 a = Seg[k].xy, b = Seg[k].zw, ab = b - a;
        float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-4));
        float d = length(p - a - ab * t);
        float w = saturate(SegC[k].a * 0.5 + 0.75 - d);
        o = lerp(o, float4(SegC[k].rgb, 1), w);
    }
    return o;
}
)HLSL";

static bool WeEnsure()
{
    if (g_wePsOverlay) return true;
    if (g_weFailed || !g_compile || !g_dev) return false;
    const char* entry[3] = { "ps_id", "ps_one", "ps_overlay" };
    ID3D11PixelShader** out[3] = { &g_wePsId, &g_wePsOne, &g_wePsOverlay };
    for (int k = 0; k < 3; ++k) {
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(kWeSrc, strlen(kWeSrc), "worldedit", 0, 0, entry[k], "ps_4_0", 0, 0, &code, &err)))
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, out[k]);
        else Log("world editor: shader %s: %s", entry[k], err ? (const char*)err->GetBufferPointer() : "?");
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = 16;
    g_dev->CreateBuffer(&bd, 0, &g_weIdCb);
    bd.ByteWidth = 16 * (3 + 56 * 2);
    g_dev->CreateBuffer(&bd, 0, &g_weOvCb);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    g_dev->CreateDepthStencilState(&dd, &g_weDsLess);
    dd.DepthEnable = FALSE;
    g_dev->CreateDepthStencilState(&dd, &g_weDsOff);
    for (int k = 0; k < 3; ++k) {
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bl.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1u << k);
        g_dev->CreateBlendState(&bl, &g_weBlendCh[k]);
    }
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    oCreateTex2D(g_dev, &sd, 0, &g_weIdStage);
    g_wePick = (WePickEnt*)calloc(16384, sizeof(WePickEnt));
    if (!g_wePsId || !g_wePsOne || !g_wePsOverlay || !g_weIdCb || !g_weOvCb || !g_weDsLess || !g_weDsOff || !g_weBlendCh[2] || !g_weIdStage || !g_wePick) {
        g_weFailed = true;
        Log("world editor: resources failed");
        return false;
    }
    return true;
}

// the main view's G-buffer bound? (its targets returned, referenced)
static bool WeGbuf(ID3D11DeviceContext* ctx, ID3D11RenderTargetView** rtv, ID3D11DepthStencilView** dsv, UINT* w, UINT* h)
{
    ctx->OMGetRenderTargets(4, rtv, dsv);
    ID3D11Resource* nres = 0;
    if (g_air.normal) { g_air.normal->GetResource(&nres); if (nres) nres->Release(); }
    bool gb = false;
    int nrt = 0;
    ID3D11Resource* r0 = 0;
    for (int k = 0; k < 4; ++k) if (rtv[k]) { ID3D11Resource* r = 0; rtv[k]->GetResource(&r); if (r == nres) gb = true; if (!r0) r0 = r; if (r) r->Release(); ++nrt; }
    if (!gb && !nres && nrt >= 3 && r0 && g_bbW) {   // the game's G-buffer not seen by our sun pass (effects off): 3+ targets, full size
        D3D11_RESOURCE_DIMENSION dim; r0->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D*)r0)->GetDesc(&td); gb = td.Width + 8 >= g_bbW; nres = r0; }
    }
    if (gb && *dsv && nres) { D3D11_TEXTURE2D_DESC nd; ((ID3D11Texture2D*)nres)->GetDesc(&nd); *w = nd.Width; *h = nd.Height; }
    if (!gb || !*dsv) {
        for (int k = 0; k < 4; ++k) if (rtv[k]) { rtv[k]->Release(); rtv[k] = 0; }
        if (*dsv) { (*dsv)->Release(); *dsv = 0; }
        return false;
    }
    return true;
}

// one replay of the draw with our pixel shader into one of our targets
static void WeReplayInto(ID3D11DeviceContext* ctx, const DrawCall& d, ID3D11RenderTargetView* to, ID3D11DepthStencilView* dsv,
                         ID3D11PixelShader* ps, ID3D11DepthStencilState* ds, ID3D11BlendState* bl)
{
    ctx->OMSetRenderTargets(1, &to, dsv);
    ctx->OMSetDepthStencilState(ds, 0);
    ctx->OMSetBlendState(bl, 0, 0xffffffff);
    oPSSetShader(ctx, ps, 0, 0);
    Replay(ctx, d);
}

// after a game draw that had a key: the pick frame's ID, the selection's mask
static void WeAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    unsigned long long key = g_weKey;
    g_weKey = 0;
    if (!key || !g_weOn) return;
    WeMesh* ms = g_weCurMesh;
    g_weCurMesh = 0;
    const WeEdit* ed = g_weShowHidden ? WeFind(key) : 0;
    bool hiddenShown = g_weShowHidden && g_weApply && ((ed && ed->hidden) || g_weGroupHidden);
    bool pick = g_wePickReq == 2 && g_wePickN < 16000;
    bool selWhole = false, selPart = false;
    for (int i = 0; i < g_weSetN; ++i) if (g_weSet[i].obj == key) { if (!g_weSet[i].isPart) selWhole = true; else if (ms) selPart = true; }
    bool sel = selWhole || selPart;
    if (!pick && !sel && !hiddenShown) return;
    if (!WeEnsure()) return;
    ID3D11RenderTargetView* rtv[4] = {};
    ID3D11DepthStencilView* dsv = 0;
    UINT w = 0, h = 0;
    if (!WeGbuf(ctx, rtv, &dsv, &w, &h)) return;
    if (g_weId.t) { D3D11_TEXTURE2D_DESC td; g_weId.t->GetDesc(&td); if (td.Width != w || td.Height != h) { FreeRT(g_weId); FreeRT(g_weMask); } }
    if (!g_weId.t && (!MakeRT(g_weId, w, h, DXGI_FORMAT_R32_FLOAT, false) || !MakeRT(g_weMask, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false))) {
        FreeRT(g_weId); FreeRT(g_weMask);
        g_weFailed = true;
        Log("world editor: %ux%u targets failed", w, h);
    }
    if (g_weId.t) {
        ID3D11PixelShader* sPs = 0; ctx->PSGetShader(&sPs, 0, 0);
        ID3D11Buffer* sCb = 0; ctx->PSGetConstantBuffers(13, 1, &sCb);
        ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ctx->OMGetDepthStencilState(&sDs, &sRef);
        ID3D11BlendState* sBl = 0; FLOAT bf[4]; UINT bm; ctx->OMGetBlendState(&sBl, bf, &bm);
        if (pick) {
            if (!g_wePickN) { float z[4] = {}; ctx->ClearRenderTargetView(g_weId.rtv, z); }
            WePickEnt base;
            memset(&base, 0, sizeof(base));
            base.key = key; base.isl = -1;
            unsigned char lay;
            if (g_weKeyCb && g_weKeyVs && WeRead((const float*)(g_weKeyCb->cpu + g_weKeyVs->wOff), &lay, base.X, base.T)) WeTag(base.X, base.T, base.q, &base.rh);
            int from = ms ? g_weRunFrom : 0, to = ms ? g_weRunTo : 1;
            for (int r = from; r < to && g_wePickN < 16000; ++r) {   // the whole draw, or each run of its pieces
                WePickEnt& pe = g_wePick[g_wePickN++];
                pe = base;
                DrawCall rd = d;
                if (ms) {
                    pe.isl = ms->run[r].isl; pe.nIsl = ms->nIsl;
                    memcpy(pe.box, ms->box + pe.isl * 6, 24);
                    rd.a = ms->run[r].count; rd.b = g_weDrawStart + ms->run[r].first;
                }
                float id[4] = { (float)g_wePickN, 0, 0, 0 };
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ctx->Map(g_weIdCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, id, 16); ctx->Unmap(g_weIdCb, 0); }
                ctx->PSSetConstantBuffers(13, 1, &g_weIdCb);
                WeReplayInto(ctx, rd, g_weId.rtv, dsv, g_wePsId, g_weDsLess, 0);
            }
            ++g_weStat.pickRec;
        }
        if (sel || hiddenShown) {
            if (g_weMaskFrame != g_frame) { float z[4] = {}; ctx->ClearRenderTargetView(g_weMask.rtv, z); g_weMaskFrame = g_frame; }
            if (sel) {
                if (!selWhole) {   // only the runs of the selected parts' pieces
                    for (int r = g_weRunFrom; r < g_weRunTo; ++r) {
                        bool in = false;
                        for (int i = 0; i < g_weSetN && !in; ++i)
                            in = g_weSet[i].obj == key && g_weSet[i].isPart && WeInBox(ms->box + ms->run[r].isl * 6, g_weSet[i].pmin, g_weSet[i].pmax);
                        if (!in) continue;
                        DrawCall rd = d;
                        rd.a = ms->run[r].count; rd.b = g_weDrawStart + ms->run[r].first;
                        WeReplayInto(ctx, rd, g_weMask.rtv, dsv, g_wePsOne, g_weDsLess, g_weBlendCh[0]);
                        WeReplayInto(ctx, rd, g_weMask.rtv, dsv, g_wePsOne, g_weDsOff, g_weBlendCh[1]);
                    }
                } else {
                    WeReplayInto(ctx, d, g_weMask.rtv, dsv, g_wePsOne, g_weDsLess, g_weBlendCh[0]);   // seen
                    WeReplayInto(ctx, d, g_weMask.rtv, dsv, g_wePsOne, g_weDsOff, g_weBlendCh[1]);    // all of it
                }
            }
            if (hiddenShown) WeReplayInto(ctx, d, g_weMask.rtv, dsv, g_wePsOne, g_weDsOff, g_weBlendCh[2]);
            ++g_weStat.maskDraws;
        }
        oPSSetShader(ctx, sPs, 0, 0);
        ctx->PSSetConstantBuffers(13, 1, &sCb);
        ctx->OMSetDepthStencilState(sDs, sRef);
        ctx->OMSetBlendState(sBl, bf, bm);
        ctx->OMSetRenderTargets(4, rtv, dsv);
        IUnknown* rel[] = { sPs, sCb, sDs, sBl };
        for (int k = 0; k < 4; ++k) if (rel[k]) rel[k]->Release();
    }
    for (int k = 0; k < 4; ++k) if (rtv[k]) rtv[k]->Release();
    if (dsv) dsv->Release();
}

// ---- the editor itself (input at Present)
static int g_weHover;                // 1-3 arrows x y z, 4 the ring (g_weDrag: the one held, newlook.cpp)
static bool g_wePickPart;            // this pick wants a piece (middle button)
static int g_wePickTries;
static unsigned g_weRepickAt;        // the model's pieces were not read yet: pick again then
static float g_weDragS0, g_weDragAng0, g_weDragOff0[3], g_weDragYaw0, g_wePtrX, g_wePtrY;
static bool g_weLmb;
static unsigned g_wePickCount;

static float g_weDragP0[3];
static float g_weDragItemOff0[64][3], g_weDragItemYaw0[64], g_weDragItemP0[64][3];   // each one at the grab
static bool g_wePickAdd;             // this pick adds to / removes from the set (Ctrl)
static void WeSelPivot(float* P)
{
    const WeEdit* e = WeFind(g_weSel);
    if (!g_weSelIsPart) { for (int j = 0; j < 3; ++j) P[j] = g_weSelT[j] + (e && g_weApply ? e->off[j] : 0); return; }
    // a part: its centre, where its model's edit puts it, moved by its own
    float X[3][3], T[3], c[3];
    memcpy(X, g_weSelX, 36); memcpy(T, g_weSelT, 12);
    const WeEdit* me = g_weApply ? WeFind(g_weSelObj) : 0;
    if (me) WeApplyEdit(*me, X, T);
    for (int j = 0; j < 3; ++j) c[j] = (g_weSelPmin[j] + g_weSelPmax[j]) * 0.5f;
    WeLocalToWorld(X, T, c, P);
    if (e && g_weApply) for (int j = 0; j < 3; ++j) P[j] += e->off[j];
}
// the set's centre (each one's pivot, averaged); the last one put back as the globals
static bool WeSetPivot(float* C)
{
    if (!g_weSetN) return false;
    WeSelItem keep;
    WeSelGet(keep);
    C[0] = C[1] = C[2] = 0;
    for (int i = 0; i < g_weSetN; ++i) {
        float p[3];
        WeSelPut(g_weSet[i]);
        WeSelPivot(p);
        for (int j = 0; j < 3; ++j) C[j] += p[j] / g_weSetN;
    }
    WeSelPut(keep);
    return true;
}
// the selection's edit, made if it has none
static WeEdit* WeEditSel()
{
    WeEdit* e = WeEditOf(g_weSel, g_weSelQ, g_weSelRh);
    if (e && g_weSelIsPart && !e->part) {
        e->part = 1; e->model = g_weSelObj;
        memcpy(e->pmin, g_weSelPmin, 12); memcpy(e->pmax, g_weSelPmax, 12);
        WeIndex();
        e = WeFind(g_weSel);
    }
    return e;
}

static bool g_weWasEnabled;
static void WeToggle()
{
    g_weOn = !g_weOn;
    // the effects off while editing (the game runs faster), back as they were after
    if (g_weOn) { g_weWasEnabled = g_enabled; g_enabled = false; g_camKnown = false; }
    else g_enabled = g_weWasEnabled;
    if (g_weOn && g_view == 24) g_view = 0;   // the lamp editor takes the same mouse
    g_weDrag = g_weHover = 0;
    if (!g_weOn) { WeSelClear(); g_wePartWant = false; g_wePickReq = 0; }
    WeMsg(g_weOn ? "WORLD EDITOR (effects off, Y: on): cross on an object, LMB (Ctrl+LMB / Ctrl+MMB: add more)" : "world editor closed (%d edits kept)", g_weN);
}

// one action on every one in the set (op 0 hide / show, 1 reset, 2 scale by f, 3 turn by dy deg): turning and scaling
// about the set's centre
static void WeSetApply(int op, float f, float dy, bool hideAll)
{
    float C[3];
    WeSetPivot(C);
    float a = dy * 0.0174533f, ca = cosf(a), sa = sinf(a);
    WeSelItem keep;
    WeSelGet(keep);
    ++g_weGrp;
    const char* what = op == 0 ? (hideAll ? "hide" : "show") : op == 1 ? "reset" : op == 2 ? "scale" : "turn";
    for (int i = 0; i < g_weSetN; ++i) {
        WeSelPut(g_weSet[i]);
        float p[3];
        WeSelPivot(p);
        WePush(g_weSel, what);
        WeEdit* e = WeEditSel();
        if (!e) { WeMsg("world editor: too many edits"); break; }
        if (op == 0) e->hidden = hideAll;
        if (op == 1) { e->hidden = 0; e->off[0] = e->off[1] = e->off[2] = 0; e->yaw = 0; e->scale = 1; }
        if (op == 2) {
            e->scale *= f;
            if (fabsf(e->scale - 1) < 1e-3f) e->scale = 1;
            for (int j = 0; j < 3; ++j) e->off[j] += (f - 1) * (p[j] - C[j]);
        }
        if (op == 3) {
            e->yaw = fmodf(e->yaw + dy + 720, 360);
            if (e->yaw > 180) e->yaw -= 360;
            float rx = p[0] - C[0], rz = p[2] - C[2];
            e->off[0] += ca * rx + sa * rz - rx;
            e->off[2] += -sa * rx + ca * rz - rz;
        }
        WeIndex();
    }
    WeSelPut(keep);
    WeTidy();
}

static bool WeKeys(bool shift, bool ctrl)
{
    bool changed = false;
    if (ctrl) {
        if (Pressed('D', false) && g_weSetN) {   // 1.58: copies of the selected objects (props: bake.bat puts them in the game)
            ++g_weGrp;
            WeSelItem made[64];
            int nm = 0, skipped = 0;
            for (int i = 0; i < g_weSetN && g_weN < kWeMax; ++i) {
                const WeSelItem& it = g_weSet[i];
                if (it.isPart) { ++skipped; continue; }
                const WeEdit* cur = WeFind(it.sel);
                unsigned long long src = cur && cur->copy ? cur->src : WeKeyPack(it.q, it.rh);
                unsigned cn = 1;
                for (int k = 0; k < g_weN; ++k) if (g_weEd[k].copy && g_weEd[k].src == src && g_weEd[k].cn >= cn) cn = g_weEd[k].cn + 1u;
                WeEdit c;
                memset(&c, 0, sizeof(c));
                c.copy = 1; c.cn = (unsigned short)cn; c.src = src; c.key = WeCopyKey(src, cn);
                memcpy(c.q, it.q, 12); c.rh = it.rh; c.scale = 1;
                if (cur && !cur->part) { memcpy(c.off, cur->off, 12); c.yaw = cur->yaw; c.scale = cur->scale; }
                c.off[0] += 1.0f;
                WePush(c.key, "copy");
                g_weEd[g_weN++] = c;
                WeSelItem n2 = it;
                n2.sel = n2.obj = c.key; n2.isPart = false;
                made[nm++] = n2;
            }
            WeIndex();
            if (nm) { memcpy(g_weSet, made, sizeof(WeSelItem) * nm); g_weSetN = nm; WeSelPut(g_weSet[0]); }
            WeTidy();
            WeMsg("world editor: %d cop%s made, 1 m along x (now selected: move them)%s", nm, nm == 1 ? "y" : "ies", skipped ? "; pieces cannot be copied" : "");
            changed = true;
        }
        if (Pressed('Z', false)) { WeUndoStep(false); changed = true; }
        if (Pressed('Y', false)) { WeUndoStep(true); changed = true; }
        return changed;
    }
    if (Pressed(VK_INSERT, false)) {
        g_weShowHidden = !g_weShowHidden;
        WeMsg("world editor: hidden objects %s", g_weShowHidden ? "SHOWN (red: click one, Del shows it again)" : "hidden again");
        changed = true;
    }
    if (Pressed(VK_HOME, false)) {
        g_weApply = !g_weApply;
        WeMsg("world editor: %s", g_weApply ? "edits ON" : "edits OFF (the game's own world, to compare)");
        changed = true;
    }
    if (!g_weSetN) return changed;
    WeEdit* cur = WeFind(g_weSel);
    bool del = Pressed(VK_DELETE, false), back = Pressed(VK_BACK, false);
    bool plus = Pressed(VK_OEM_PLUS, true) || Pressed(VK_ADD, true), minus = Pressed(VK_OEM_MINUS, true) || Pressed(VK_SUBTRACT, true);
    bool tl = Pressed(VK_OEM_4, true), tr = Pressed(VK_OEM_6, true);   // [ ]
    if (!del && !back && !plus && !minus && !tl && !tr) return changed;
    float f = plus ? (shift ? 1.01f : 1.05f) : minus ? 1 / (shift ? 1.01f : 1.05f) : 1;
    float dy = (tr ? 1 : tl ? -1 : 0) * (shift ? 1.0f : 15.0f);
    bool hideAll = !(cur && cur->hidden);
    WeSetApply(del ? 0 : back ? 1 : plus || minus ? 2 : 3, f, dy, hideAll);
    cur = WeFind(g_weSel);
    float sc = cur ? cur->scale : 1, yaw = cur ? cur->yaw : 0;
    WeTidy();
    const char* many = g_weSetN > 1 ? " (all selected)" : "";
    if (del) WeMsg("world editor: %s%s", hideAll ? "HIDDEN (Ins: show hidden ones, Ctrl+Z undo)" : "shown again", many);
    else if (back) WeMsg("world editor: reset%s", many);
    else WeMsg("world editor: scale %.2f, turn %.0f deg%s", sc, yaw, many);
    return true;
}

static void WeUpdate(bool active)
{
    if (!g_weOn) { if (g_weDrag) { g_weDrag = 0; WeTidy(); } return; }
    if (!g_camKnown || !g_hwnd || g_view == 24) return;   // (the lamp editor has the mouse)
    POINT ms;
    RECT rc;
    if (!GetCursorPos(&ms) || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    float cw = (float)rc.right, ch = (float)rc.bottom;
    POINT centre = { rc.right / 2, rc.bottom / 2 };
    ClientToScreen(g_hwnd, &centre);
    float cx = cw * 0.5f, cy = ch * 0.5f;   // the pointer: the cross at the centre (moved by the mouse while dragging)
    if (g_weDrag) {
        if (active) { g_wePtrX += ms.x - centre.x; g_wePtrY += ms.y - centre.y; SetCursorPos(centre.x, centre.y); }
        cx = g_wePtrX; cy = g_wePtrY;
    }
    float d[3];
    CamRay(cx / cw, cy / ch, d);
    bool lmb = active && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    static const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    float P[3];
    bool have = WeSetPivot(P);
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    g_weHover = 0;
    if (have && !g_weDrag) {
        float len = GizmoLen(P), u0, v0, best = 12;
        if (ProjectUV(P, &u0, &v0)) {
            for (int a = 0; a < 3; ++a) {
                float q[3] = { P[0] + axes[a][0] * len, P[1] + axes[a][1] * len, P[2] + axes[a][2] * len }, u1, v1;
                if (!ProjectUV(q, &u1, &v1)) continue;
                float dd = SegDist2D(cx, cy, u0 * cw, v0 * ch, u1 * cw, v1 * ch);
                if (dd < best) { best = dd; g_weHover = a + 1; }
            }
            float R = len * 1.5f, pu = 0, pv = 0;
            bool havePrev = false;
            for (int k = 0; k <= 32; ++k) {
                float a = k * 6.2831853f / 32, rp[3] = { P[0] + sinf(a) * R, P[1], P[2] + cosf(a) * R }, u, v;
                bool ok = ProjectUV(rp, &u, &v);
                if (ok && havePrev) {
                    float dd = SegDist2D(cx, cy, pu * cw, pv * ch, u * cw, v * ch);
                    if (dd < best) { best = dd; g_weHover = 4; }
                }
                havePrev = ok; pu = u; pv = v;
            }
        }
    }
    if (lmb && !g_weLmb) {
        if (g_weHover && have && !ctrl) {   // grab a handle: every one in the set, as it is now
            WeSelItem keep;
            WeSelGet(keep);
            ++g_weGrp;
            bool ok = true;
            for (int i = 0; i < g_weSetN && ok; ++i) {
                WeSelPut(g_weSet[i]);
                WeSelPivot(g_weDragItemP0[i]);
                WePush(g_weSel, g_weHover == 4 ? "turn" : "move");
                WeEdit* e = WeEditSel();
                if (!e) { ok = false; break; }
                memcpy(g_weDragItemOff0[i], e->off, 12);
                g_weDragItemYaw0[i] = e->yaw;
            }
            WeSelPut(keep);
            if (ok) {
                g_weDrag = g_weHover;
                memcpy(g_weDragP0, P, 12);
                g_wePtrX = cx; g_wePtrY = cy;
                SetCursorPos(centre.x, centre.y);
                if (g_weDrag <= 3) g_weDragS0 = AxisParam(P, axes[g_weDrag - 1], d);
                else { float hp[3]; g_weDragAng0 = RayLevel(d, P[1], hp) ? atan2f(hp[0] - P[0], hp[2] - P[2]) : 0; }
            }
        } else if (!g_wePickReq) {    // pick what is under the cross (Ctrl: add it to the set / take it out)
            g_wePickReq = 1;
            g_wePickAdd = ctrl;
            g_wePickPart = false; g_wePartWant = false; g_wePendModel = 0; g_weRepickAt = 0;
            WeMsg("world editor: picking...");
        }
    }
    static bool mmbDown;
    bool mmb = active && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (mmb && !mmbDown && !g_weDrag && !g_wePickReq) {   // middle button: one piece of the model under the cross
        g_wePickReq = 1;
        g_wePickPart = true; g_wePartWant = true; g_wePickTries = 0; g_wePendModel = 0; g_weRepickAt = 0;
        g_wePickAdd = ctrl; g_weSplitFail = false;
        WeMsg("world editor: picking a piece...");
    }
    mmbDown = mmb;
    if (lmb && g_weDrag) {
        const float* C = g_weDragP0;   // the set's centre at the grab
        float ds = 0, da = 0;
        bool turn = false;
        if (g_weDrag <= 3) {
            ds = AxisParam(C, axes[g_weDrag - 1], d) - g_weDragS0;
            if (fabsf(ds) > 500) ds = ds > 0 ? 500.0f : -500.0f;
        } else {
            float hp[3];
            if (RayLevel(d, C[1], hp)) { da = atan2f(hp[0] - C[0], hp[2] - C[2]) - g_weDragAng0; turn = true; }
        }
        float ca = cosf(da), sa = sinf(da);
        for (int i = 0; i < g_weSetN; ++i) {
            WeEdit* e = WeFind(g_weSet[i].sel);
            if (!e) continue;
            memcpy(e->off, g_weDragItemOff0[i], 12);
            if (g_weDrag <= 3) e->off[g_weDrag - 1] += ds;
            else if (turn) {   // each turned by the same angle, its pivot carried round the centre
                float y = fmodf(g_weDragItemYaw0[i] + da * 57.29578f + 720, 360);
                e->yaw = y > 180 ? y - 360 : y;
                float rx = g_weDragItemP0[i][0] - C[0], rz = g_weDragItemP0[i][2] - C[2];
                e->off[0] += ca * rx + sa * rz - rx;
                e->off[2] += -sa * rx + ca * rz - rz;
            }
        }
        WeIndex();
    }
    if (!lmb && g_weLmb && g_weDrag) {
        g_weDrag = 0;
        WeTidy();
        const WeEdit* e = WeFind(g_weSel);
        if (e) WeMsg("world editor: moved by %.2f %.2f %.2f m, turned %.0f deg, scale %.2f%s", e->off[0], e->off[1], e->off[2], e->yaw, e->scale,
                     g_weSetN > 1 ? " (the last picked; all selected moved)" : "");
        else WeMsg("world editor: back where it was");
    }
    g_weLmb = lmb;
}

// at Present: the pick's steps; the stats; the buffers let go
static void WeFrame()
{
    bool live = g_weOn || (g_weN && g_weApply);
    // (our sun shadows know their objects by the copies too; 1.65: the rain map's and the lamp faces' replays are recorded
    // with the copies, at night too)
    bool copy = live || (g_enabled && ((g_p[P_SS].v > 0.5f && !g_ssNight) || g_p[P_SSR + 1].v > 0 || (g_lampUse > 0 && g_p[P_LAMP_SHADOW_MAPS].v > 0.5f)));
    if (copy != g_cbsCopy) {
        g_cbsCopy = copy;
        for (int k = 0; k < kCbsSlots; ++k) { g_cbs[k].valid = false; g_cbs[k].keyOff = -1; }
        Log("world editor: constant-buffer copies %s", copy ? "on" : "off");
    }
    g_cbsOn = live;
    static DWORD nextPurge, nextStat;
    DWORD now = GetTickCount();
    if ((LONG)(now - nextPurge) >= 0) { nextPurge = now + 10000; CbsPurge(!copy); }
    static unsigned statFrame;
    if (live && (LONG)(now - nextStat) >= 0) {
        if (nextStat && g_weStat.draws && g_frame > statFrame) {
            char sz[96] = "";
            for (unsigned k = 0; k < g_cbsSizeN; ++k) sprintf(sz + strlen(sz), " %u", g_cbsSizes[k]);
            unsigned f = g_frame - statFrame;   // (1.65: the frames counted; it was 600, as if 24 a second: x2 at 50)
            Log("world editor (per frame, ~%u frames): draws %u, with a world matrix %u (no copy yet %u), layout rows %u / transposed %u / "
                "identity %u / rows with w used %u / other %u, objects drawn %u | hidden skipped %u, moved %u | pick %u, mask %u | buffers %d (sizes%s) | CPU %.2f ms",
                f, g_weStat.draws / f, g_weStat.world / f, g_weStat.noData / f, g_weStat.lay[1] / f, g_weStat.lay[2] / f, g_weStat.lay[3] / f,
                g_weStat.lay[4] / f, g_weStat.lay[0] / f, g_weStat.keyed / f, g_weStat.skipped / f, g_weStat.subst / f, g_weStat.pickRec, g_weStat.maskDraws / f,
                g_cbsN, sz, g_weStat.cpu / f);
        }
        memset(&g_weStat, 0, sizeof(g_weStat));
        nextStat = now + 25000;
        statFrame = g_frame;
    }
    if (g_dev && g_weOn) {
        ID3D11DeviceContext* mc = 0;
        g_dev->GetImmediateContext(&mc);
        if (mc) { WeMeshWork(mc); mc->Release(); }
    }
    if (g_weRepickAt && g_frame >= g_weRepickAt && !g_wePickReq) { g_weRepickAt = 0; g_wePickReq = 1; g_wePickPart = true; }
    if (!g_wePickReq || !g_dev) return;
    if (g_wePickReq == 1) {
        if (!WeEnsure()) { g_wePickReq = 0; WeMsg("world editor: not available (see the log)"); return; }
        g_wePickN = 0;
        g_wePickReq = 2;
        return;
    }
    ID3D11DeviceContext* ctx = 0;
    g_dev->GetImmediateContext(&ctx);
    if (!ctx) return;
    if (g_wePickReq == 2) {
        if (g_wePickN && g_weId.t) {
            D3D11_TEXTURE2D_DESC td;
            g_weId.t->GetDesc(&td);
            D3D11_BOX box = { td.Width / 2, td.Height / 2, 0, td.Width / 2 + 1, td.Height / 2 + 1, 1 };
            ctx->CopySubresourceRegion(g_weIdStage, 0, 0, 0, 0, g_weId.t, 0, &box);
            g_wePickReq = 3;
            g_wePickFrame = g_frame;
        } else { g_wePickReq = 0; WeMsg("world editor: no objects of their own drawn now (the baked city cannot be picked)"); }
    } else if (g_wePickReq == 3 && g_frame > g_wePickFrame + 1) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_weIdStage, 0, D3D11_MAP_READ, 0, &m))) {
            int id = (int)(*(const float*)m.pData + 0.5f);
            ctx->Unmap(g_weIdStage, 0);
            if (id < 1 || id > g_wePickN) { if (!g_wePickAdd) WeSelClear(); WeMsg("world editor: nothing of its own at the cross (the baked city or the sky; %d objects drawn)", g_wePickN); Log("world editor: pick: %s (id %d)", g_lampMsg, id); }
            else {
                const WePickEnt& pe = g_wePick[id - 1];
                WeSelItem before;
                WeSelGet(before);
                bool settled = true;
                g_weSel = g_weSelObj = pe.key;
                g_weSelIsPart = false;
                memcpy(g_weSelQ, pe.q, 12); g_weSelRh = pe.rh;
                memcpy(g_weSelT, pe.T, 12); memcpy(g_weSelX, pe.X, 36);
                int parts = 0;
                for (int i = 0; i < g_wePickN; ++i) if (g_wePick[i].key == pe.key) ++parts;
                float sx = sqrtf(pe.X[0][0] * pe.X[0][0] + pe.X[0][1] * pe.X[0][1] + pe.X[0][2] * pe.X[0][2]);
                ++g_wePickCount;
                if (g_wePickPart && pe.isl >= 0) {   // one piece: its box (mm steps), its key
                    g_wePendModel = 0;
                    for (int j = 0; j < 3; ++j) {
                        g_weSelPmin[j] = floorf(pe.box[j] * 1000 + 0.5f) / 1000;
                        g_weSelPmax[j] = floorf(pe.box[3 + j] * 1000 + 0.5f) / 1000;
                    }
                    g_weSelIsPart = true;
                    g_weSel = WePartKey(pe.key, g_weSelPmin, g_weSelPmax);
                    const WeEdit* e = WeFind(g_weSel);
                    WeMsg("world editor: PIECE %d of %d of the object at %.2f %.2f %.2f, %.1f x %.1f x %.1f%s - LMB: the whole object",
                          pe.isl + 1, pe.nIsl, pe.T[0], pe.T[1], pe.T[2], g_weSelPmax[0] - g_weSelPmin[0], g_weSelPmax[1] - g_weSelPmin[1],
                          g_weSelPmax[2] - g_weSelPmin[2], e ? (e->hidden ? ", HIDDEN" : ", edited") : "");
                } else if (g_wePickPart && g_wePendModel && pe.key != g_wePendModel) {   // the cross went to another object
                    g_wePendModel = 0;
                    settled = false;
                    WeMsg("world editor: the cross left the object while its pieces were read - MMB on it again");
                } else if (g_wePickPart && !g_weSplitFail && g_wePickTries < 8) {   // its pieces not read yet (asked for now): again soon
                    ++g_wePickTries;
                    g_wePendModel = pe.key;
                    g_weRepickAt = g_frame + 6;
                    settled = false;
                    WeMsg("world editor: reading the object's pieces... (keep the cross on it)");
                } else {
                    g_wePendModel = 0;
                    const WeEdit* e = WeFind(pe.key);
                    WeMsg("world editor: object at %.2f %.2f %.2f (%d draws, size x%.2f)%s - MMB: one piece of it",
                          pe.T[0], pe.T[1], pe.T[2], parts, sx, e ? (e->hidden ? ", HIDDEN" : ", edited") : "");
                    if (g_wePickPart) WeMsg("world editor: this object cannot be split (see the log) - the whole one selected");
                }
                if (!settled) WeSelPut(before);   // (the set as it was until the pieces are read)
                else {
                    WeSelItem it;
                    WeSelGet(it);
                    int at = -1;
                    for (int i = 0; i < g_weSetN; ++i) if (g_weSet[i].sel == it.sel) at = i;
                    if (!g_wePickAdd) { g_weSet[0] = it; g_weSetN = 1; }
                    else if (at >= 0) {   // Ctrl on one already in: out of the set
                        g_weSet[at] = g_weSet[--g_weSetN];
                        if (g_weSetN) WeSelPut(g_weSet[g_weSetN - 1]); else WeSelClear();
                        WeMsg("world editor: taken out of the selection, %d selected", g_weSetN);
                    } else if (g_weSetN < 64) {
                        g_weSet[g_weSetN++] = it;
                        WeMsg("world editor: added, %d selected (%s)", g_weSetN, it.isPart ? "a piece" : "an object");
                    } else WeMsg("world editor: 64 selected already");
                }
                Log("world editor: pick %u: %s; tag %08x", g_wePickCount, g_lampMsg, pe.rh);
            }
        }
        g_wePickReq = 0;
        g_textDirty = true;
    }
    ctx->Release();
}

// at Present, over the picture: the selection's outline, hidden ones shown, the gizmo, the cross
static void WeOverlay(IDXGISwapChain* sc, ID3D11DeviceContext* ctx)
{
    if (!g_weOn || !g_vsText || !WeEnsure()) return;
    ID3D11Texture2D* bb = 0;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    if (bb != g_bbTex || !g_bbRtv) {
        if (g_bbRtv) { g_bbRtv->Release(); g_bbRtv = 0; }
        g_dev->CreateRenderTargetView(bb, 0, &g_bbRtv);
        g_bbTex = bb;
    }
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    bb->Release();
    if (!g_bbRtv) return;
    float cb[4 * (3 + 56 * 2)] = {};
    float* rect = cb; float* info = cb + 4; float* info2 = cb + 8; float* seg = cb + 12; float* segc = cb + 12 + 56 * 4;
    rect[0] = -1; rect[1] = 1; rect[2] = 2; rect[3] = 2;
    float W = (float)bd.Width, H = (float)bd.Height;
    info[0] = W; info[1] = H;
    bool mask = g_weMask.t && g_weMaskFrame + 1 == g_frame;
    if (mask) { D3D11_TEXTURE2D_DESC md; g_weMask.t->GetDesc(&md); info[2] = (float)md.Width; info[3] = (float)md.Height; }
    info2[1] = mask ? 1.0f : 0.0f;
    int n = 0;
    auto add = [&](float x0, float y0, float x1, float y1, float r, float g, float b, float wdt) {
        if (n >= 56) return;
        seg[n * 4] = x0; seg[n * 4 + 1] = y0; seg[n * 4 + 2] = x1; seg[n * 4 + 3] = y1;
        segc[n * 4] = r; segc[n * 4 + 1] = g; segc[n * 4 + 2] = b; segc[n * 4 + 3] = wdt;
        ++n;
    };
    float px = W * 0.5f, py = H * 0.5f;   // the cross (the drag pointer while dragging)
    RECT rc;
    if (g_weDrag && g_hwnd && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) { px = g_wePtrX / rc.right * W; py = g_wePtrY / rc.bottom * H; }
    float cs = H * 0.012f;
    add(px - cs, py, px + cs, py, 1, 0.25f, 0.2f, 1.5f);
    add(px, py - cs, px, py + cs, 1, 0.25f, 0.2f, 1.5f);
    float P[3], u0, v0;
    if (g_camKnown && WeSetPivot(P)) {
        float len = GizmoLen(P);
        if (ProjectUV(P, &u0, &v0)) {
            static const float col[3][3] = { { 1, 0.25f, 0.2f }, { 0.35f, 1, 0.3f }, { 0.3f, 0.55f, 1 } };
            for (int a = 0; a < 3; ++a) {
                float q[3] = { P[0], P[1], P[2] }, u1, v1;
                q[a] += len;
                if (!ProjectUV(q, &u1, &v1)) continue;
                bool hot = g_weHover == a + 1 || g_weDrag == a + 1;
                float r = hot ? 1 : col[a][0], g = hot ? 1 : col[a][1], b = hot ? 0.6f : col[a][2];
                add(u0 * W, v0 * H, u1 * W, v1 * H, r, g, b, 3);
                float dx = u1 - u0, dy = (v1 - v0) * H / W, l = sqrtf(dx * dx + dy * dy) + 1e-6f;   // the arrow head
                dx /= l; dy /= l;
                float hs = W * 0.008f;
                add(u1 * W, v1 * H, u1 * W - (dx * 1.6f - dy) * hs, v1 * H - (dy * 1.6f + dx) * hs, r, g, b, 3);
                add(u1 * W, v1 * H, u1 * W - (dx * 1.6f + dy) * hs, v1 * H - (dy * 1.6f - dx) * hs, r, g, b, 3);
            }
            float R = len * 1.5f, pu = 0, pv = 0;
            bool havePrev = false, hot = g_weHover == 4 || g_weDrag == 4;
            for (int k = 0; k <= 32; ++k) {
                float a = k * 6.2831853f / 32, rp[3] = { P[0] + sinf(a) * R, P[1], P[2] + cosf(a) * R }, u, v;
                bool ok = ProjectUV(rp, &u, &v);
                if (ok && havePrev) add(pu * W, pv * H, u * W, v * H, 1, hot ? 1 : 0.85f, hot ? 0.6f : 0.2f, 2);
                havePrev = ok; pu = u; pv = v;
            }
        }
    }
    info2[0] = (float)n;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_weOvCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, cb, sizeof(cb));
    ctx->Unmap(g_weOvCb, 0);
    // save
    ID3D11RenderTargetView* sRtv = 0; ID3D11DepthStencilView* sDsv = 0;
    ctx->OMGetRenderTargets(1, &sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16;
    ctx->RSGetViewports(&nVp, sVp);
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0;
    ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11ShaderResourceView* sSrv = 0; ctx->PSGetShaderResources(0, 1, &sSrv);
    ID3D11Buffer* sVcb = 0; ctx->VSGetConstantBuffers(13, 1, &sVcb);
    ID3D11Buffer* sPcb = 0; ctx->PSGetConstantBuffers(13, 1, &sPcb);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    ctx->OMSetRenderTargets(1, &g_bbRtv, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(g_vsText, 0, 0);
    oPSSetShader(ctx, g_wePsOverlay, 0, 0);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->OMSetBlendState(g_blendAlpha, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->RSSetState(g_rsNoCull);
    ID3D11ShaderResourceView* ms = mask ? g_weMask.srv : 0;
    ctx->PSSetShaderResources(0, 1, &ms);
    ctx->VSSetConstantBuffers(13, 1, &g_weOvCb);
    ctx->PSSetConstantBuffers(13, 1, &g_weOvCb);
    oDraw(ctx, 4, 0);
    // restore
    ctx->OMSetRenderTargets(1, &sRtv, sDsv);
    ctx->RSSetViewports(nVp, sVp);
    ctx->VSSetShader(sVs, 0, 0);
    oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl);
    ctx->IASetPrimitiveTopology(sTop);
    ctx->OMSetBlendState(sBl, sBf, sBm);
    ctx->OMSetDepthStencilState(sDs, sRef);
    ctx->RSSetState(sRs);
    ctx->PSSetShaderResources(0, 1, &sSrv);
    ctx->VSSetConstantBuffers(13, 1, &sVcb);
    ctx->PSSetConstantBuffers(13, 1, &sPcb);
    ID3D11DeviceChild* rel[] = { sRtv, sDsv, sVs, sPs, sIl, sBl, sDs, sRs, sSrv, sVcb, sPcb };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
}
