// ---------------------------------------------------------------- object tools
// What did the game draw under the cross, and where does that object live in the game's memory? Research
// tools for tying a lamp to a breakable prop (a lamp post). Everything goes to NFSLight_objects.log.
//  1 pick: for one frame every main-view geometry draw before the lighting is drawn once more into the one
//    pixel under the cross (depth EQUAL, no writes) inside an occlusion query: the last one that passes is
//    what is seen there.
//  2 constants: the next frame, the draws of the same mesh get their vertex-shader constant buffers (and
//    instance data) copied, the one under the cross found again by query. The object's world position is
//    looked for among those floats (nearest to the clicked point).
//  3 scan: the game's memory is searched for that position (worker thread).
//  4 watch: 1 KB around every hit is compared every 250 ms for 3 minutes, changes logged (knock it down!);
//    once a second the mesh's draws are checked again: how many, and where.

static char g_objLogPath[MAX_PATH];
static CRITICAL_SECTION g_objLogLock;
static void OLog(const char* fmt, ...)
{
    static bool init;
    if (!init) { init = true; InitializeCriticalSection(&g_objLogLock); sprintf(g_objLogPath, "%sNFSLight_objects.log", g_dir); }
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_objLogLock);
    FILE* f = fopen(g_objLogPath, "a");
    if (f) { fprintf(f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf); fclose(f); }
    LeaveCriticalSection(&g_objLogLock);
}

struct DrawRec {
    DrawCall dc;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11InputLayout* il;
    ID3D11Buffer* ib;
    DXGI_FORMAT ibFmt;
    UINT ibOff;
    ID3D11Buffer* vb[2];
    UINT vbStride[2], vbOff[2];
    ID3D11Buffer* vscb[8];
    D3D11_PRIMITIVE_TOPOLOGY top;
    UINT rtW, rtH;
    int nrt;
    ID3D11Query* q;
    UINT64 samples;
};

static void RecCapture(ID3D11DeviceContext* ctx, const DrawCall& d, DrawRec& r)
{
    memset(&r, 0, sizeof(r));
    r.dc = d;
    ctx->VSGetShader(&r.vs, 0, 0);
    ctx->PSGetShader(&r.ps, 0, 0);
    ctx->IAGetInputLayout(&r.il);
    ctx->IAGetIndexBuffer(&r.ib, &r.ibFmt, &r.ibOff);
    ctx->IAGetVertexBuffers(0, 2, r.vb, r.vbStride, r.vbOff);
    ctx->VSGetConstantBuffers(0, 8, r.vscb);
    ctx->IAGetPrimitiveTopology(&r.top);
}

static void RecRelease(DrawRec& r)
{
    IUnknown* u[] = { r.vs, r.ps, r.il, r.ib, r.vb[0], r.vb[1], r.vscb[0], r.vscb[1], r.vscb[2], r.vscb[3], r.vscb[4], r.vscb[5],
                      r.vscb[6], r.vscb[7] };
    for (int i = 0; i < (int)(sizeof(u) / sizeof(u[0])); ++i) if (u[i]) u[i]->Release();
    ID3D11Query* q = r.q;
    memset(&r, 0, sizeof(r));
    r.q = q;
}

static void RecAddRef(DrawRec& r)
{
    IUnknown* u[] = { r.vs, r.ps, r.il, r.ib, r.vb[0], r.vb[1], r.vscb[0], r.vscb[1], r.vscb[2], r.vscb[3], r.vscb[4], r.vscb[5],
                      r.vscb[6], r.vscb[7] };
    for (int i = 0; i < (int)(sizeof(u) / sizeof(u[0])); ++i) if (u[i]) u[i]->AddRef();
}

// same mesh drawn the same way (another instance of the same prop draws exactly like this)
static bool SameMesh(const DrawRec& a, const DrawCall& d, ID3D11DeviceContext* ctx)
{
    if (a.dc.kind != d.kind || a.dc.a != d.a) return false;
    if (d.kind == DK_INDEXED && (a.dc.b != d.b || a.dc.base != d.base)) return false;
    if (d.kind == DK_INDEXED_INSTANCED && (a.dc.c != d.c || a.dc.base != d.base)) return false;
    if (d.kind == DK_DRAW && a.dc.b != d.b) return false;
    if (d.kind == DK_INSTANCED && a.dc.c != d.c) return false;
    ID3D11Buffer* ib = 0; DXGI_FORMAT f; UINT o;
    ctx->IAGetIndexBuffer(&ib, &f, &o);
    if (ib) ib->Release();
    if (ib != a.ib || o != a.ibOff) return false;
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    if (vs) vs->Release();
    return vs == a.vs;
}

enum { OB_IDLE, OB_QUERY, OB_QWAIT, OB_CB, OB_CBWAIT, OB_WATCH };
static volatile int g_obPhase;
static unsigned g_obPhaseFrame;           // frame the current phase is for
static float g_obPoint[4];                // world point under the cross at the click (w = 1 if known)
static float g_obU = 0.5f, g_obV = 0.5f;  // where on screen
static DrawRec* g_obRecs;
static int g_obRecCount;
static const int kObMaxRecs = 6000;
static ID3D11Query* g_obQueries[kObMaxRecs];
static ID3D11DepthStencilState* g_obDS;
static ID3D11BlendState* g_obBlendNone;
struct RsClone { ID3D11RasterizerState* src; ID3D11RasterizerState* clone; };
static RsClone g_obRs[512];
static int g_obRsCount;
static DrawRec g_obWin;                   // the picked draw (references held)
static bool g_obHaveWin;
static int g_obWinIndex, g_obWinOf;
struct CbMatch { DrawRec r; ID3D11Buffer* st[8]; UINT size[8]; ID3D11Buffer* inst; UINT instBytes, instStride; };
static CbMatch g_obMatch[128];
static int g_obMatchCount;
static bool g_obCheckOnly;                // a later check frame: no query, only the positions
static int g_obTSlot = -1, g_obTOff = -1, g_obTStride;   // where: VS cb slot (8 = instance data), byte offset, 4 or 16
static char g_obStatus[160] = "off";
static DWORD g_obWatchEnd, g_obNextCheck;
static volatile LONG g_obScanDone;
static int g_obConnectLamp = -1;        // the lamp the pick is for

static ID3D11RasterizerState* ScissorClone(ID3D11RasterizerState* rs)
{
    for (int i = 0; i < g_obRsCount; ++i) if (g_obRs[i].src == rs) return g_obRs[i].clone;
    D3D11_RASTERIZER_DESC d = {};
    if (rs) rs->GetDesc(&d);
    else { d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_BACK; d.DepthClipEnable = TRUE; }
    d.ScissorEnable = TRUE;
    ID3D11RasterizerState* c = 0;
    if (FAILED(g_dev->CreateRasterizerState(&d, &c))) return rs;
    if (g_obRsCount < 512) { g_obRs[g_obRsCount].src = rs; g_obRs[g_obRsCount].clone = c; ++g_obRsCount; }
    return c;
}

static void ObFreeClones()
{
    for (int i = 0; i < g_obRsCount; ++i) if (g_obRs[i].clone) g_obRs[i].clone->Release();
    g_obRsCount = 0;
}

static bool ObEnsure()
{
    if (g_obDS) return true;
    D3D11_DEPTH_STENCIL_DESC ds = {};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_EQUAL;
    D3D11_BLEND_DESC bd = {};
    for (int i = 0; i < 8; ++i) bd.RenderTarget[i].RenderTargetWriteMask = 0;
    bd.IndependentBlendEnable = TRUE;
    if (FAILED(g_dev->CreateDepthStencilState(&ds, &g_obDS)) || FAILED(g_dev->CreateBlendState(&bd, &g_obBlendNone))) {
        OLog("pick: state objects failed");
        return false;
    }
    if (!g_obRecs) g_obRecs = (DrawRec*)calloc(kObMaxRecs, sizeof(DrawRec));
    return g_obRecs != 0;
}

static ID3D11Query* ObQuery(int i)
{
    if (i >= kObMaxRecs) return 0;
    if (!g_obQueries[i]) {
        D3D11_QUERY_DESC qd = { D3D11_QUERY_OCCLUSION, 0 };
        if (FAILED(g_dev->CreateQuery(&qd, &g_obQueries[i]))) g_obQueries[i] = 0;
    }
    return g_obQueries[i];
}

// the same draw once more into the pixel under the cross, inside a query; nothing is written
static void ObQueryReplay(ID3D11DeviceContext* ctx, const DrawCall& d, ID3D11Query* q, UINT rtW, UINT rtH)
{
    ID3D11RasterizerState* rs = 0; ctx->RSGetState(&rs);
    D3D11_RECT sc[16]; UINT nsc = 16; ctx->RSGetScissorRects(&nsc, sc);
    ID3D11DepthStencilState* ds = 0; UINT ref = 0; ctx->OMGetDepthStencilState(&ds, &ref);
    ID3D11BlendState* bs = 0; FLOAT bf[4]; UINT bm = 0; ctx->OMGetBlendState(&bs, bf, &bm);
    ctx->RSSetState(ScissorClone(rs));
    LONG px = (LONG)(g_obU * rtW), py = (LONG)(g_obV * rtH);
    D3D11_RECT r = { px, py, px + 1, py + 1 };
    ctx->RSSetScissorRects(1, &r);
    ctx->OMSetDepthStencilState(g_obDS, 0);
    ctx->OMSetBlendState(g_obBlendNone, 0, 0xffffffff);
    ctx->Begin(q);
    Replay(ctx, d);
    ctx->End(q);
    ctx->RSSetState(rs);
    ctx->RSSetScissorRects(nsc, nsc ? sc : 0);
    ctx->OMSetDepthStencilState(ds, ref);
    ctx->OMSetBlendState(bs, bf, bm);
    if (rs) rs->Release();
    if (ds) ds->Release();
    if (bs) bs->Release();
}

// main-view geometry before the lighting: render targets of the screen size and a depth buffer
static bool ObGeometryDraw(ID3D11DeviceContext* ctx, UINT* w, UINT* h, int* nrt)
{
    if (!Immediate(ctx) || g_frameSun) return false;
    ID3D11RenderTargetView* rv[8] = {};
    ID3D11DepthStencilView* dv = 0;
    ctx->OMGetRenderTargets(8, rv, &dv);
    bool ok = rv[0] && dv;
    *nrt = 0;
    for (int i = 0; i < 8; ++i) if (rv[i]) ++*nrt;
    if (ok) {
        ID3D11Resource* res = 0;
        rv[0]->GetResource(&res);
        ID3D11Texture2D* t = 0;
        if (res) { res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); res->Release(); }
        ok = false;
        if (t) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); *w = td.Width; *h = td.Height; ok = !(g_bbW && td.Width + 8 < g_bbW); t->Release(); }
    }
    for (int i = 0; i < 8; ++i) if (rv[i]) rv[i]->Release();
    if (dv) dv->Release();
    return ok;
}

// ---- called after every draw
static void ObAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    int ph = g_obPhase;
    if ((ph != OB_QUERY && ph != OB_CB) || g_frame != g_obPhaseFrame) return;
    UINT w = 0, h = 0;
    int nrt = 0;
    if (!ObGeometryDraw(ctx, &w, &h, &nrt)) return;
    if (ph == OB_QUERY) {
        if (g_obRecCount >= kObMaxRecs) return;
        ID3D11Query* q = ObQuery(g_obRecCount);
        if (!q) return;
        DrawRec& r = g_obRecs[g_obRecCount++];
        RecCapture(ctx, d, r);
        r.rtW = w; r.rtH = h; r.nrt = nrt; r.q = q;
        ObQueryReplay(ctx, d, q, w, h);
    } else {
        if (!g_obHaveWin || g_obMatchCount >= 128 || !SameMesh(g_obWin, d, ctx)) return;
        CbMatch& m = g_obMatch[g_obMatchCount];
        ID3D11Query* q = ObQuery(g_obMatchCount);
        if (!q) return;
        ++g_obMatchCount;
        RecRelease(m.r);
        RecCapture(ctx, d, m.r);
        m.r.q = q;
        m.r.rtW = w; m.r.rtH = h;
        if (!g_obCheckOnly) ObQueryReplay(ctx, d, q, w, h);
        for (int k = 0; k < 8; ++k) {
            if (!m.r.vscb[k]) continue;
            D3D11_BUFFER_DESC bd;
            m.r.vscb[k]->GetDesc(&bd);
            if (!m.st[k] || m.size[k] != bd.ByteWidth) {
                if (m.st[k]) m.st[k]->Release();
                D3D11_BUFFER_DESC sd = {};
                sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                m.st[k] = 0;
                if (FAILED(g_dev->CreateBuffer(&sd, 0, &m.st[k]))) { m.st[k] = 0; continue; }
                m.size[k] = bd.ByteWidth;
            }
            ctx->CopyResource(m.st[k], m.r.vscb[k]);
        }
        // instance data of an instanced draw (second vertex stream)
        m.instBytes = 0;
        if ((d.kind == DK_INDEXED_INSTANCED || d.kind == DK_INSTANCED) && m.r.vb[1] && m.r.vbStride[1]) {
            UINT inst = d.b, start = d.e, stride = m.r.vbStride[1];
            UINT bytes = inst * stride;
            if (bytes > 65536) bytes = 65536 / stride * stride;
            D3D11_BUFFER_DESC vd;
            m.r.vb[1]->GetDesc(&vd);
            UINT off = m.r.vbOff[1] + start * stride;
            if (bytes && off + bytes <= vd.ByteWidth) {
                if (m.inst) m.inst->Release();
                D3D11_BUFFER_DESC sd = {};
                sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                m.inst = 0;
                if (SUCCEEDED(g_dev->CreateBuffer(&sd, 0, &m.inst))) {
                    D3D11_BOX box = { off, 0, 0, off + bytes, 1, 1 };
                    ctx->CopySubresourceRegion(m.inst, 0, 0, 0, 0, m.r.vb[1], 0, &box);
                    m.instBytes = bytes; m.instStride = stride;
                }
            }
        }
    }
}

static const char* KindName(int k)
{
    static const char* n[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" };
    return k >= 0 && k < 4 ? n[k] : "?";
}

static void RecLog(const char* tag, const DrawRec& r, int idx)
{
    OLog("%s #%d: %s count %u args %u %u %u base %d | VS %p PS %p (%s) IL %p | IB %p fmt %d off %u | VB0 %p stride %u off %u | "
         "VB1 %p stride %u off %u | RT %ux%u x%d | samples %llu",
         tag, idx, KindName(r.dc.kind), r.dc.a, r.dc.b, r.dc.c, r.dc.e, r.dc.base, r.vs, r.ps, PsNameOf(r.ps), r.il, r.ib, (int)r.ibFmt,
         r.ibOff, r.vb[0], r.vbStride[0], r.vbOff[0], r.vb[1], r.vbStride[1], r.vbOff[1], r.rtW, r.rtH, r.nrt, r.samples);
    OLog("%s #%d: VS cb 0-7: %p %p %p %p %p %p %p %p", tag, idx, r.vscb[0], r.vscb[1], r.vscb[2], r.vscb[3], r.vscb[4], r.vscb[5],
         r.vscb[6], r.vscb[7]);
}

static bool WaitQuery(ID3D11DeviceContext* ctx, ID3D11Query* q, UINT64* out)
{
    DWORD end = GetTickCount() + 250;   // the GPU may be a frame or two behind: wait by time, not by tries
    for (;;) {
        HRESULT hr = ctx->GetData(q, out, 8, 0);
        if (hr == S_OK) return true;
        if (FAILED(hr) || (LONG)(GetTickCount() - end) > 0) return false;
        Sleep(0);
    }
}

// ---- 3/4: memory scan and watch (worker thread)
struct ObHit { unsigned char* addr; int kind; unsigned char snap[1024]; int changes[256]; DWORD first[256], last[256]; bool burstLogged; };
static ObHit* g_obHits;
static volatile LONG g_obHitCount, g_obScanPct, g_obScanBusy;
static float g_obScanT[3];
static HMODULE g_selfModule;

static bool SafeCopy(void* dst, const void* src, size_t n)
{
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static int ScanRegion(const unsigned char* base, size_t size, const float* T, float tol)
{
    int found = 0;
    __try {
        const float* f = (const float*)base;
        size_t n = size / 4;
        for (size_t i = 0; i + 8 < n && g_obHitCount < 256; ++i) {
            float x = f[i];
            if (!(fabsf(x - T[0]) <= tol)) continue;
            int kind = 0;
            if (fabsf(f[i + 1] - T[1]) <= tol && fabsf(f[i + 2] - T[2]) <= tol) kind = 1;                  // x y z in a row
            else if (i + 8 < n && fabsf(f[i + 4] - T[1]) <= tol && fabsf(f[i + 8] - T[2]) <= tol) kind = 2;  // a column of a 4x4
            if (!kind) continue;
            LONG k = InterlockedIncrement(&g_obHitCount) - 1;
            ObHit& h = g_obHits[k];
            memset(&h, 0, sizeof(h));
            h.addr = (unsigned char*)(f + i);
            h.kind = kind;
            ++found;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

static DWORD WINAPI ObScanThread(void*)
{
    const float* T = g_obScanT;
    OLog("scan: looking for %.4f %.4f %.4f in the game's memory (x y z in a row, or a 4x4 column; tolerance 0.01)", T[0], T[1], T[2]);
    MEMORY_BASIC_INFORMATION mi;
    unsigned char* a = 0;
    size_t scanned = 0;
    HMODULE selfMod = 0;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&ObScanThread, &selfMod);
    while ((size_t)a < 0xFFFF0000u && VirtualQuery(a, &mi, sizeof(mi)) == sizeof(mi)) {
        unsigned char* next = (unsigned char*)mi.BaseAddress + mi.RegionSize;
        bool readable = mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                        (mi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_READ));
        if (readable && (HMODULE)mi.AllocationBase != selfMod && g_obHitCount < 256) {
            int n = ScanRegion((unsigned char*)mi.BaseAddress, mi.RegionSize, T, 0.01f);
            if (n) {
                char mod[MAX_PATH] = "";
                if (mi.Type == MEM_IMAGE) GetModuleFileNameA((HMODULE)mi.AllocationBase, mod, MAX_PATH);
                OLog("scan: %d hit(s) in region %p size %08lx type %s protect %08lx %s", n, mi.BaseAddress, (unsigned long)mi.RegionSize,
                     mi.Type == MEM_IMAGE ? "image" : mi.Type == MEM_MAPPED ? "mapped" : "private", mi.Protect, mod);
            }
            scanned += mi.RegionSize;
        }
        g_obScanPct = (LONG)((size_t)a / 42949673u);
        if (next <= a) break;
        a = next;
    }
    g_obScanDone = 1;
    int hits = g_obHitCount;
    OLog("scan: done, %u MB read, %d hits", (unsigned)(scanned >> 20), hits);
    for (int i = 0; i < hits; ++i) {
        ObHit& h = g_obHits[i];
        float ctxf[24];
        if (!SafeCopy(ctxf, h.addr - 32, sizeof(ctxf))) continue;
        char line[1024];
        int o = sprintf(line, "hit %d at %p (%s): ", i, h.addr, h.kind == 1 ? "x y z" : "4x4 column");
        for (int k = 0; k < 24; ++k) o += sprintf(line + o, "%s%.3g", k == 8 ? " [" : " ", ctxf[k]);
        OLog("%s", line);
        SafeCopy(h.snap, h.addr - 256, 1024);
    }
    // ---- watch: what changes around the hits (knock the object down now)
    DWORD start = GetTickCount();
    g_obWatchEnd = start + 180000;
    int watch = hits < 48 ? hits : 48;
    OLog("watch: comparing 1 KB around the first %d hits every 250 ms for 3 minutes. Knock the object down now.", watch);
    while ((LONG)(g_obWatchEnd - GetTickCount()) > 0) {
        Sleep(250);
        DWORD t = GetTickCount() - start;
        for (int i = 0; i < watch; ++i) {
            ObHit& h = g_obHits[i];
            unsigned char now[1024];
            if (!SafeCopy(now, h.addr - 256, 1024)) continue;
            int changed = 0;
            for (int k = 0; k < 256; ++k) {
                unsigned a0, a1;
                memcpy(&a0, h.snap + k * 4, 4);
                memcpy(&a1, now + k * 4, 4);
                if (a0 == a1) continue;
                ++changed;
                if (!h.changes[k]++) {
                    float f0, f1;
                    memcpy(&f0, &a0, 4); memcpy(&f1, &a1, 4);
                    h.first[k] = a0;
                    OLog("watch %6.2f s: hit %d %+5d: %08x -> %08x  (%g -> %g)", t / 1000.0, i, k * 4 - 256, a0, a1, f0, f1);
                }
                h.last[k] = a1;
            }
            if (changed >= 8 && !h.burstLogged) { h.burstLogged = true; OLog("watch %6.2f s: hit %d: %d values changed at once", t / 1000.0, i, changed); }
            memcpy(h.snap, now, 1024);
        }
    }
    OLog("watch: summary (offset from the hit: times changed, first -> last value)");
    for (int i = 0; i < watch; ++i)
        for (int k = 0; k < 256; ++k)
            if (g_obHits[i].changes[k]) {
                float f0, f1;
                memcpy(&f0, &g_obHits[i].first[k], 4); memcpy(&f1, &g_obHits[i].last[k], 4);
                OLog("  hit %d %+5d: %d x  %08x -> %08x  (%g -> %g)", i, k * 4 - 256, g_obHits[i].changes[k], g_obHits[i].first[k],
                     g_obHits[i].last[k], f0, f1);
            }
    OLog("watch: end");
    g_obScanBusy = 0;
    return 0;
}

// ---- the floats of a constant buffer copy nearest to the clicked point (the object's position)
static void FindPosition(const float* f, int n, int slot, float* best, int* bSlot, int* bOff, int* bStride, float* bd)
{
    for (int i = 0; i + 2 < n; ++i)
        for (int s = 0; s < 2; ++s) {
            int st = s ? 4 : 1;
            if (i + 2 * st >= n) continue;
            float x = f[i], y = f[i + st], z = f[i + 2 * st];
            if (!(fabsf(x) < 1e5f && fabsf(y) < 1e5f && fabsf(z) < 1e5f) || (x == 0 && y == 0 && z == 0)) continue;
            float cx = x - g_cam[3][0], cy = y - g_cam[3][1], cz = z - g_cam[3][2];
            if (cx * cx + cy * cy + cz * cz < 1.0f) continue;   // the camera's own position (view constants)
            float dx = x - g_obPoint[0], dy = y - g_obPoint[1], dz = z - g_obPoint[2];
            float d = dx * dx + dz * dz + 0.25f * dy * dy;   // a post's origin sits under the clicked point
            if (d < *bd) { *bd = d; best[0] = x; best[1] = y; best[2] = z; *bSlot = slot; *bOff = i * 4; *bStride = st * 4; }
        }
}

static void DumpFloats(const char* tag, const float* f, int n)
{
    for (int i = 0; i < n; i += 4) {
        char line[256];
        int o = sprintf(line, "%s [%3d]", tag, i * 4);
        for (int k = 0; k < 4 && i + k < n; ++k) o += sprintf(line + o, " %12.5g", f[i + k]);
        OLog("%s", line);
    }
}

// ---- once per frame (from Present): advance the steps
static void ObPresent(ID3D11DeviceContext* ctx)
{
    int ph = g_obPhase;
    if (ph == OB_QUERY && g_frame == g_obPhaseFrame + 1) {
        g_obPhase = OB_QWAIT;
        ph = OB_QWAIT;
        g_obPhaseFrame = g_frame;
        return;
    }
    if (ph == OB_QWAIT && g_frame >= g_obPhaseFrame + 1) {
        int win = -1, seen = 0;
        for (int i = 0; i < g_obRecCount; ++i) {
            DrawRec& r = g_obRecs[i];
            r.samples = 0;
            if (r.q && WaitQuery(ctx, r.q, &r.samples) && r.samples) { win = i; ++seen; }
        }
        OLog("pick: %d geometry draws before the lighting, %d cover the pixel under the cross (%.3f %.3f)", g_obRecCount, seen, g_obU, g_obV);
        for (int i = 0; i < g_obRecCount; ++i) if (g_obRecs[i].samples) RecLog(i == win ? "pick  WINNER" : "pick  covered", g_obRecs[i], i);
        if (win >= 0) {
            // how widely is each constant buffer of the winner shared (per object, or one for everything)?
            for (int k = 0; k < 8; ++k) {
                if (!g_obRecs[win].vscb[k]) continue;
                int same = 0;
                for (int i = 0; i < g_obRecCount; ++i) if (g_obRecs[i].vscb[k] == g_obRecs[win].vscb[k]) ++same;
                OLog("pick: VS cb%d %p is bound in %d of %d geometry draws", k, g_obRecs[win].vscb[k], same, g_obRecCount);
            }
            if (g_obHaveWin) RecRelease(g_obWin);
            g_obWin = g_obRecs[win];
            RecAddRef(g_obWin);
            g_obWin.q = 0;
            g_obHaveWin = true;
            g_obWinIndex = win; g_obWinOf = g_obRecCount;
            sprintf(g_obStatus, "picked draw %d of %d (%s), reading its constants", win, g_obRecCount, PsNameOf(g_obWin.ps));
            g_obMatchCount = 0;
            g_obCheckOnly = false;
            g_obPhase = OB_CB;
            g_obPhaseFrame = g_frame;   // this frame
        } else {
            strcpy(g_obStatus, "nothing drawn under the cross (sky?)");
            g_obPhase = OB_IDLE;
        }
        for (int i = 0; i < g_obRecCount; ++i) RecRelease(g_obRecs[i]);
        g_obRecCount = 0;
        ObFreeClones();
        return;
    }
    if (ph == OB_CB && g_frame == g_obPhaseFrame + 1) { g_obPhase = OB_CBWAIT; g_obPhaseFrame = g_frame; return; }
    if (ph == OB_CBWAIT && g_frame >= g_obPhaseFrame + 1) {
        bool check = g_obCheckOnly;
        int win = -1;
        for (int i = 0; i < g_obMatchCount; ++i) {
            CbMatch& m = g_obMatch[i];
            m.r.samples = 0;
            if (!check && m.r.q && WaitQuery(ctx, m.r.q, &m.r.samples) && m.r.samples) win = i;
        }
        if (!check) {
            OLog("constants: %d draws of the same mesh this frame, %d under the cross", g_obMatchCount, win);
            if (win < 0 && g_obMatchCount == 1) win = 0;
        }
        float best[3] = { 0, 0, 0 }, bd = 1e30f;
        int bSlot = -1, bOff = -1, bStride = 0, bestIdx = -1;
        char posLine[1024];
        int po = 0;
        for (int i = 0; i < g_obMatchCount; ++i) {
            CbMatch& m = g_obMatch[i];
            float mine[3] = { 0, 0, 0 }, md = 1e30f, mineM[16] = {};
            int ms = -1, mo = -1, mst = 0;
            for (int k = 0; k <= 8; ++k) {
                ID3D11Buffer* b = k < 8 ? m.st[k] : m.inst;
                UINT size = k < 8 ? (m.r.vscb[k] ? m.size[k] : 0) : m.instBytes;
                if (!b || !size) continue;
                D3D11_MAPPED_SUBRESOURCE mp;
                if (FAILED(ctx->Map(b, 0, D3D11_MAP_READ, 0, &mp))) continue;
                const float* f = (const float*)mp.pData;
                int n = (int)(size / 4);
                if (check) {
                    // a later check: the floats at the place the position was found
                    if (k == g_obTSlot && g_obTOff >= 0 && g_obTOff + 2 * g_obTStride + 4 <= (int)size) {
                        const char* c = (const char*)mp.pData + g_obTOff;
                        float x, y, z;
                        memcpy(&x, c, 4); memcpy(&y, c + g_obTStride, 4); memcpy(&z, c + 2 * g_obTStride, 4);
                        bool same = fabsf(x - g_obT[0]) < 0.01f && fabsf(y - g_obT[1]) < 0.01f && fabsf(z - g_obT[2]) < 0.01f;
                        if (po < 900) po += sprintf(posLine + po, " [%.2f %.2f %.2f%s]", x, y, z, same ? " = PICKED" : "");
                    }
                } else {
                    if (g_obPoint[3] > 0.5f) {
                        int before = ms;
                        float bdm = md;
                        FindPosition(f, n, k, mine, &ms, &mo, &mst, &md);
                        if (md < bdm && ms == k && mst == 4 && mo >= 48) memcpy(mineM, f + mo / 4 - 12, 64);   // its rows + position
                        (void)before;
                    }
                    if (i == win) {
                        char tag[64];
                        if (k < 8) sprintf(tag, "  VS cb%d", k);
                        else sprintf(tag, "  instance data (stride %u)", m.instStride);
                        OLog("constants of the picked draw:%s, %u bytes", tag, size);
                        DumpFloats(tag, f, n < 256 ? n : 256);
                    }
                }
                ctx->Unmap(b, 0);
            }
            if (!check && md < 1e29f) {
                OLog("constants: draw %d of the mesh: nearest floats to the clicked point: %.3f %.3f %.3f (cb%d +%d, stride %d)%s", i,
                     mine[0], mine[1], mine[2], ms, mo, mst, i == win ? "  <- under the cross" : "");
                // the one under the cross; if the query found none, the one standing nearest to the clicked point
                if (i == win || (win < 0 && md < bd)) { memcpy(best, mine, 12); bd = md; bSlot = ms; bOff = mo; bStride = mst; bestIdx = i; memcpy(g_obM, mineM, 64); }
            }
        }
        if (check) {
            OLog("check: %d draws of the picked mesh this frame, positions:%s", g_obMatchCount, po ? posLine : " (none drawn)");
            g_obPhase = g_obScanBusy ? OB_WATCH : OB_IDLE;
            return;
        }
        if (bd < 30.0f * 30.0f) {
            memcpy(g_obT, best, 12);
            g_obTSlot = bSlot; g_obTOff = bOff; g_obTStride = bStride;
            g_obHaveT = true;
            if (g_obConnectLamp >= 0 && g_obConnectLamp < g_mapCount) {   // remember it with the lamp
                MapLamp& L = g_map[g_obConnectLamp];
                memcpy(L.obj, best, 12);
                L.hasObj = 1;
                g_mapDirty = true;
                sprintf(g_lampMsg, "lamp %d tied to the object at %.1f %.1f %.1f", g_obConnectLamp + 1, best[0], best[1], best[2]);
                OLog("lamp %d tied to the object at %.4f %.4f %.4f", g_obConnectLamp + 1, best[0], best[1], best[2]);
                L.propAddr = 0; L.propState = PS_SEARCH; L.propNext = 0; L.propFade = 1;
                PropRequest(L.obj);
                sprintf(g_obStatus, "lamp %d tied; looking for the object in memory", g_obConnectLamp + 1);
                g_obPhase = OB_IDLE;   // no research scan when tying a lamp
                return;
            }
            OLog("constants: the picked object's position: %.4f %.4f %.4f (VS cb%d + %d bytes, stride %d), %.2f m from the clicked point, "
                 "draw %d of the mesh (%s)", best[0], best[1], best[2], bSlot, bOff, bStride, sqrtf(bd), bestIdx,
                 win >= 0 ? "under the cross" : "nearest to the clicked point");
            sprintf(g_obStatus, "object at %.1f %.1f %.1f, scanning memory", best[0], best[1], best[2]);
            if (!g_obHits) g_obHits = (ObHit*)calloc(256, sizeof(ObHit));
            if (g_obHits && !InterlockedExchange(&g_obScanBusy, 1)) {
                memcpy(g_obScanT, g_obT, 12);
                g_obHitCount = 0;
                g_obScanDone = 0;
                g_obScanPct = 0;
                g_obPhase = OB_WATCH;
                g_obNextCheck = GetTickCount() + 1000;
                CloseHandle(CreateThread(0, 0, ObScanThread, 0, 0, 0));
                return;
            }
        } else {
            OLog("constants: no position near the clicked point found (see the dump above)");
            strcpy(g_obStatus, "constants dumped, no position found (see NFSLight_objects.log)");
        }
        g_obPhase = OB_IDLE;
        return;
    }
    if (ph == OB_WATCH && g_obHaveWin && g_obHaveT && (LONG)(GetTickCount() - g_obNextCheck) >= 0) {
        // once a second: where are the draws of the picked mesh now?
        g_obNextCheck = GetTickCount() + 1000;
        g_obMatchCount = 0;
        g_obCheckOnly = true;
        g_obPhase = OB_CB;
        g_obPhaseFrame = g_frame;
        LONG left = (LONG)(g_obWatchEnd - GetTickCount());
        sprintf(g_obStatus, "watching: %ld hits, %ld s left - knock the object down", g_obHitCount, left > 0 ? left / 1000 : 0);
    }
    if (ph == OB_WATCH && !g_obScanBusy) { g_obPhase = OB_IDLE; sprintf(g_obStatus, "done - send NFSLight_objects.log"); }
    if (g_obPhase == OB_WATCH && !g_obScanDone) sprintf(g_obStatus, "scanning memory %ld %%", g_obScanPct);
}

// the click in connect mode: pick what is under the cross next frame
static void ObPickRequest()
{
    if (g_obPhase != OB_IDLE || g_obScanBusy) { strcpy(g_obStatus, "busy - wait until the watch ends"); return; }
    if (!ObEnsure()) return;
    g_obU = g_mouseU; g_obV = g_mouseV;
    memcpy(g_obPoint, g_aim, 16);
    g_obRecCount = 0;
    g_obPhase = OB_QUERY;
    g_obPhaseFrame = g_frame;       // the frame that starts after this Present
    strcpy(g_obStatus, "picking what is under the cross");
    OLog("==== pick at screen %.3f %.3f, clicked point %.3f %.3f %.3f (%s), camera %.2f %.2f %.2f", g_obU, g_obV, g_aim[0], g_aim[1], g_aim[2],
         g_aim[3] > 0.5f ? "valid" : "unknown", g_cam[3][0], g_cam[3][1], g_cam[3][2]);
    if (g_lampSel >= 0 && g_lampSel < g_mapCount) {
        OLog("     for lamp %d at %.3f %.3f %.3f", g_lampSel + 1, g_map[g_lampSel].p[0], g_map[g_lampSel].p[1], g_map[g_lampSel].p[2]);
        g_obConnectLamp = g_lampSel;
    } else g_obConnectLamp = -1;
    g_obConnect = false;   // one click per pick
}
