// ---------------------------------------------------------------- lamps on every post (templates)
// A template is one post set up by hand: its mesh (how the game draws that kind of post) and its lamps in the
// post's own axes, with their colour / group. While driving, every 8th frame the draws of a template's mesh have
// their constants copied (the post's world matrix is in them) and read back a few frames later; every standing post
// of that kind without lamps gets the template's lamps, tied to it (so they go out when it is knocked down).
// Templates: NewLook_lamp_templates.txt. Make one: select a lamp tied to a post (picked this session), Shift+I.

struct TplBulb { float local[3], c[3]; int group; };
struct Template {
    UINT indexCount, startIndex, stride; INT baseVertex;   // the mesh
    int cbSlot, cbOff;                                     // where its position is in the VS constants
    int nb; TplBulb b[4];
};
static Template g_tpl[16];
static int g_tplCount, g_autoAdded, g_autoTied;
static char g_tplPath[MAX_PATH];

static void TplSave()
{
    FILE* f = fopen(g_tplPath, "w");
    if (!f) return;
    fprintf(f, "# NewLook lamp templates: mesh (index count, start index, base vertex, vertex stride), constants slot / offset of\n"
               "# its position, then per lamp: local x y z (in the post's axes), r g b, group. Made with Shift+I.\n");
    for (int t = 0; t < g_tplCount; ++t) {
        const Template& T = g_tpl[t];
        fprintf(f, "%u %u %d %u %d %d %d", T.indexCount, T.startIndex, T.baseVertex, T.stride, T.cbSlot, T.cbOff, T.nb);
        for (int k = 0; k < T.nb; ++k)
            fprintf(f, "  %.4f %.4f %.4f %.4f %.4f %.4f %d", T.b[k].local[0], T.b[k].local[1], T.b[k].local[2], T.b[k].c[0], T.b[k].c[1],
                    T.b[k].c[2], T.b[k].group);
        fprintf(f, "\n");
    }
    fclose(f);
}

static void TplLoad()
{
    g_tplCount = 0;
    FILE* f = fopen(g_tplPath, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_tplCount < 16) {
        if (line[0] == '#') continue;
        Template T;
        memset(&T, 0, sizeof(T));
        int n = 0;
        if (sscanf(line, "%u %u %d %u %d %d %d%n", &T.indexCount, &T.startIndex, &T.baseVertex, &T.stride, &T.cbSlot, &T.cbOff, &T.nb, &n) < 7)
            continue;
        const char* p = line + n;
        int ok = 0;
        for (int k = 0; k < T.nb && k < 4; ++k) {
            int used = 0;
            TplBulb& b = T.b[k];
            if (sscanf(p, "%f %f %f %f %f %f %d%n", &b.local[0], &b.local[1], &b.local[2], &b.c[0], &b.c[1], &b.c[2], &b.group, &used) < 7) break;
            p += used;
            ++ok;
        }
        T.nb = ok;
        if (ok) g_tpl[g_tplCount++] = T;
    }
    fclose(f);
    Log("lamp templates: %d", g_tplCount);
}

// Shift+I: the selected lamp's post becomes a template (all lamps tied to that post go into it)
static void TplMake()
{
    if (g_lampSel < 0 || g_lampSel >= g_mapCount || !g_map[g_lampSel].hasObj) { strcpy(g_lampMsg, "template: select a lamp tied to a post first"); return; }
    MapLamp& S = g_map[g_lampSel];
    if (!g_obHaveWin || !g_obHaveT || g_obT[0] != S.obj[0] || g_obT[1] != S.obj[1] || g_obT[2] != S.obj[2]) {
        strcpy(g_lampMsg, "template: tie this lamp to its post again first ([Shift+O], click the post) - the mesh is learnt then");
        return;
    }
    // its axes: from its instance in memory if found, else from its draw's constants at the pick
    const float* f = S.propState == PS_LIT ? S.propM : g_obM;
    if (f[13] == 0 && f[12] == 0) { strcpy(g_lampMsg, "template: no axes for this post - tie it again"); return; }
    if (g_obWin.dc.kind != DK_INDEXED || g_obTSlot < 0 || g_obTSlot > 7 || g_obTStride != 4 || g_obTOff < 48) {
        strcpy(g_lampMsg, "template: this object is drawn in a way templates do not handle");
        return;
    }
    Template T;
    memset(&T, 0, sizeof(T));
    T.indexCount = g_obWin.dc.a; T.startIndex = g_obWin.dc.b; T.baseVertex = g_obWin.dc.base; T.stride = g_obWin.vbStride[0];
    T.cbSlot = g_obTSlot; T.cbOff = g_obTOff;
    for (int i = 0; i < g_mapCount && T.nb < 4; ++i) {
        MapLamp& L = g_map[i];
        if (!L.hasObj || L.obj[0] != S.obj[0] || L.obj[1] != S.obj[1] || L.obj[2] != S.obj[2]) continue;
        TplBulb& b = T.b[T.nb++];
        float d[3] = { L.p[0] - f[12], L.p[1] - f[13], L.p[2] - f[14] };
        for (int r = 0; r < 3; ++r) b.local[r] = d[0] * f[r * 4] + d[1] * f[r * 4 + 1] + d[2] * f[r * 4 + 2];
        memcpy(b.c, L.c, 12);
        b.group = L.group;
    }
    // the same mesh again replaces its template
    int slot = g_tplCount;
    for (int t = 0; t < g_tplCount; ++t)
        if (g_tpl[t].indexCount == T.indexCount && g_tpl[t].startIndex == T.startIndex && g_tpl[t].baseVertex == T.baseVertex) slot = t;
    if (slot >= 16) { strcpy(g_lampMsg, "template: 16 templates already"); return; }
    g_tpl[slot] = T;
    if (slot == g_tplCount) ++g_tplCount;
    TplSave();
    sprintf(g_lampMsg, "template %d: %d lamp(s) on every post of this kind (mesh of %u indices)", slot + 1, T.nb, T.indexCount);
    Log("lamp templates: %s", g_lampMsg);
}

// ---- render side: copies of the constants of template-mesh draws, read back later
struct AutoCopy { ID3D11Buffer* st; UINT size; int tpl; unsigned frame; };
static AutoCopy g_autoCopy[96];
static int g_autoCopyN;

static int TplMatch(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (d.kind != DK_INDEXED) return -1;
    for (int t = 0; t < g_tplCount; ++t) {
        const Template& T = g_tpl[t];
        if (d.a != T.indexCount || d.b != T.startIndex || d.base != T.baseVertex) continue;
        ID3D11Buffer* vb = 0; UINT stride = 0, off = 0;
        ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
        if (vb) vb->Release();
        if (stride == T.stride) return t;
    }
    return -1;
}

static void AutoAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if ((g_frame & 7) || g_autoCopyN >= 96 || !Immediate(ctx) || g_frameSun) return;
    int t = TplMatch(ctx, d);
    if (t < 0) return;
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(g_tpl[t].cbSlot, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    AutoCopy& c = g_autoCopy[g_autoCopyN];
    if ((UINT)g_tpl[t].cbOff + 16 <= bd.ByteWidth) {
        if (!c.st || c.size != bd.ByteWidth) {
            if (c.st) c.st->Release();
            D3D11_BUFFER_DESC sd = {};
            sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            c.st = 0;
            if (FAILED(g_dev->CreateBuffer(&sd, 0, &c.st))) c.st = 0;
            c.size = bd.ByteWidth;
        }
        if (c.st) { ctx->CopyResource(c.st, cb); c.tpl = t; c.frame = g_frame; ++g_autoCopyN; }
    }
    cb->Release();
}

// A post that has lamps already, or had them (knocked down: it slides away upright before it falls, and must not
// get new lamps where it lands): any tied lamp's post within 2.5 m.
static bool AutoHasLamps(const float* T)
{
    for (int i = 0; i < g_mapCount; ++i) {
        if (!g_map[i].hasObj) continue;
        float dx = g_map[i].obj[0] - T[0], dy = g_map[i].obj[1] - T[1], dz = g_map[i].obj[2] - T[2];
        if (dx * dx + dz * dz < 2.5f * 2.5f && fabsf(dy) < 3.0f) return true;
    }
    return false;
}

// candidates: a post gets lamps only once seen at the very same place twice, a second or more apart (standing still)
struct AutoSeen { float T[3]; int tpl; DWORD first; };
static AutoSeen g_autoSeen[128];
static int g_autoSeenN;

static bool AutoStill(int t, const float* T)
{
    DWORD now = GetTickCount();
    for (int i = 0; i < g_autoSeenN; ++i) {   // forget old ones
        if (now - g_autoSeen[i].first > 20000) { g_autoSeen[i--] = g_autoSeen[--g_autoSeenN]; }
    }
    for (int i = 0; i < g_autoSeenN; ++i) {
        AutoSeen& a = g_autoSeen[i];
        if (a.tpl != t || memcmp(a.T, T, 12)) continue;
        if (now - a.first < 1000) return false;
        g_autoSeen[i] = g_autoSeen[--g_autoSeenN];
        return true;
    }
    int slot = g_autoSeenN < 128 ? g_autoSeenN++ : (int)(now % 128);
    memcpy(g_autoSeen[slot].T, T, 12); g_autoSeen[slot].tpl = t; g_autoSeen[slot].first = now;
    return false;
}

// a post found standing: its lamps from the template, unless it has them already
static void AutoPlace(int t, const float* f /* rows at f[0..11], position f[12..14] */)
{
    const Template& T = g_tpl[t];
    if (f[5] < 0.995f) return;   // not standing upright (knocked down, or moving)
    if (AutoHasLamps(f + 12)) return;   // done already (or knocked down, sliding)
    if (!AutoStill(t, f + 12)) return;  // not seen standing still yet
    for (int k = 0; k < T.nb; ++k) {
        float p[3];
        for (int j = 0; j < 3; ++j) p[j] = f[12 + j] + T.b[k].local[0] * f[j] + T.b[k].local[1] * f[4 + j] + T.b[k].local[2] * f[8 + j];
        // a lamp placed by hand there already: tie it instead of adding another
        int nearIdx = -1;
        float bd = 2.0f * 2.0f;
        for (int i = 0; i < g_mapCount; ++i) {
            if (g_map[i].hasObj) continue;
            float d = Dist2(g_map[i].p, p);
            if (d < bd) { bd = d; nearIdx = i; }
        }
        MapLamp* L;
        if (nearIdx >= 0) { L = &g_map[nearIdx]; ++g_autoTied; }
        else {
            if (g_mapCount >= kMaxMap) return;
            L = &g_map[g_mapCount++];
            MapLampClear(*L);
            memcpy(L->p, p, 12);
            memcpy(L->c, T.b[k].c, 12);
            L->group = T.b[k].group;
            ++g_autoAdded;
        }
        memcpy(L->obj, f + 12, 12);
        L->hasObj = 1;
        L->propState = PS_SEARCH;
    }
    g_mapDirty = true;
}

static void AutoPresent(ID3D11DeviceContext* ctx)
{
    // the copies of 3 frames ago (never waiting); then the slots are free again
    if (!g_autoCopyN) return;
    if (g_frame - g_autoCopy[0].frame < 3) return;
    int before = g_autoAdded + g_autoTied;
    for (int i = 0; i < g_autoCopyN; ++i) {
        AutoCopy& c = g_autoCopy[i];
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx->Map(c.st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;   // not yet: next frame
        float f[16];
        memcpy(f, (const char*)m.pData + g_tpl[c.tpl].cbOff - 48, 64);   // rows, then the position
        ctx->Unmap(c.st, 0);
        AutoPlace(c.tpl, f);
    }
    g_autoCopyN = 0;
    if (g_autoAdded + g_autoTied != before)
        sprintf(g_lampMsg, "auto lamps: %d added, %d existing tied (map has %d)", g_autoAdded, g_autoTied, g_mapCount);
}
