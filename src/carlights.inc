// ---------------------------------------------------------------- traffic cars' lights, tied to the cars
// Traffic cars are drawn with the damage vertex shader (g_damageConstants, no bones: the player's car has bones and
// its own lights; no wheel constants: the wheels). Every part of one car has the same object matrix (gObjectCB.world:
// rows = the car's axes, row 3 = its position; z = forward, y = up, origin on the road, from the player's car and
// its headlight spot: the spot at (0.04, 0.73, 2.21)). Each such draw of the main view copies its 64 matrix bytes
// into a staging buffer (one per frame, a ring of 4); a couple of frames later the CPU reads one (never waiting),
// merges equal matrices into cars, follows the cars from read to read (their speed) and puts them where they are
// now (the read is late: position + speed x the frames since). Each car gets 2 headlights (spots, forward) and 2
// tail lights (red, backwards) as lamps: lit like the map lamps (off by day with them), no glow, no shadow maps.
// Where the lights sit comes from the car's own mesh: every traffic draw's mesh (index + vertex range) is read back
// once (the position element from its input layout) and its bounds kept; a car's bounds = those of its parts, so a
// truck gets its lights at its own front / back / width / height. Only cars that have been seen driving (forward,
// steadily, for a while) get lights: parked cars stay dark, also when knocked about.

// the input layouts: where a vertex's position is
typedef HRESULT(STDMETHODCALLTYPE* CreateILFn)(ID3D11Device*, const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**);
static CreateILFn oCreateIL;
struct IlPos { ID3D11InputLayout* il; int fmt, off, slot; };
static IlPos g_ilPos[2048];
static volatile LONG g_ilPosN;
static CRITICAL_SECTION g_ilLock;
static volatile LONG g_ilLockInit;
static HRESULT STDMETHODCALLTYPE HkCreateIL(ID3D11Device* dev, const D3D11_INPUT_ELEMENT_DESC* d, UINT n, const void* code, SIZE_T size, ID3D11InputLayout** out)
{
    HRESULT hr = oCreateIL(dev, d, n, code, size, out);
    if (FAILED(hr) || !out || !*out || !d) return hr;
    UINT slotOff[16] = {};
    for (UINT i = 0; i < n; ++i) {
        UINT s = d[i].InputSlot < 16 ? d[i].InputSlot : 0;
        UINT o = d[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT ? slotOff[s] : d[i].AlignedByteOffset;
        UINT sz = 4;
        switch (d[i].Format) {
        case DXGI_FORMAT_R32G32B32A32_FLOAT: case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT: sz = 16; break;
        case DXGI_FORMAT_R32G32B32_FLOAT: sz = 12; break;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT: case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R32G32_FLOAT: sz = 8; break;
        default: break;
        }
        slotOff[s] = o + sz;
        if (d[i].SemanticName && !_stricmp(d[i].SemanticName, "POSITION") && d[i].SemanticIndex == 0) {
            if (!InterlockedCompareExchange(&g_ilLockInit, 2, 0)) { InitializeCriticalSection(&g_ilLock); g_ilLockInit = 1; }
            while (g_ilLockInit != 1) Sleep(0);
            EnterCriticalSection(&g_ilLock);
            if (g_ilPosN < 2048) { IlPos& e = g_ilPos[g_ilPosN]; e.il = *out; e.fmt = d[i].Format; e.off = (int)o; e.slot = (int)s; InterlockedIncrement(&g_ilPosN); }
            LeaveCriticalSection(&g_ilLock);
            break;
        }
    }
    return hr;
}
static const IlPos* IlFind(ID3D11InputLayout* il)
{
    LONG n = g_ilPosN;
    for (LONG i = n - 1; i >= 0; --i) if (g_ilPos[i].il == il) return &g_ilPos[i];
    return 0;
}

static int CarCount(int what);
// the traffic vertex shaders' code, once (how their positions are unpacked)
static void CarVsDump(const void* code, SIZE_T size)
{
    static int n;
    typedef HRESULT(WINAPI* DisFn)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
    static DisFn dis;
    if (n >= 3) return;
    if (!dis) { HMODULE m = GetModuleHandleA("d3dcompiler_47.dll"); if (m) dis = (DisFn)GetProcAddress(m, "D3DDisassemble"); }
    ID3DBlob* b = 0;
    if (!dis || FAILED(dis(code, size, 0, 0, &b)) || !b) return;
    char p[MAX_PATH];
    sprintf(p, "%sNewLook_carvs.txt", g_dir);
    FILE* f = fopen(p, n ? "a" : "w");
    if (f) { fprintf(f, "==== traffic vertex shader %d\n", n); fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, f); fclose(f); }
    b->Release();
    ++n;
}

// a mesh's bounds, read back once: the index range first, then the vertices it uses
struct MeshB {
    ID3D11Buffer *ib, *vb; UINT ibOff, start, count, vbOff, stride; INT base; int ifmt, pfmt, poff;
    int state;              // 0 free, 1 index copy sent, 2 vertex copy sent, 3 done, 4 no use
    ID3D11Buffer* stage; unsigned sentFrame; UINT vmin, vmax; unsigned short* idx16; UINT* idx32;
    float bmin[3], bmax[3];
};
static const int kMeshes = 1024;
static MeshB g_mesh[kMeshes];
static int g_meshLogged, g_meshJobsThisFrame;
static int g_meshWhy[8];   // 0 no layout, 1 not indexed, 2 position format, 3 index range, 4 vertex range, 5 map failed, 6 done
static unsigned g_meshJobFrame;
static float Half(unsigned short h)
{
    unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.0f * 6.1035e-5f : (e == 31 ? 0 : ldexpf(1 + m / 1024.0f, (int)e - 15));
    return s ? -v : v;
}
// the mesh of this draw (its slot in g_mesh, -1: none); a new one starts its read-back (a few a frame)
static int MeshOf(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (d.kind != DK_INDEXED && d.kind != DK_INDEXED_INSTANCED) { ++g_meshWhy[1]; return -1; }
    UINT count = d.a, start = d.kind == DK_INDEXED ? d.b : d.c;
    ID3D11InputLayout* il = 0;
    ctx->IAGetInputLayout(&il);
    if (!il) return -1;
    const IlPos* ip = IlFind(il);
    il->Release();
    if (!ip) { ++g_meshWhy[0]; return -1; }
    ID3D11Buffer *ib = 0, *vb = 0;
    DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0, stride = 0, vbOff = 0;
    ctx->IAGetIndexBuffer(&ib, &ifmt, &ibOff);
    ctx->IAGetVertexBuffers(ip->slot, 1, &vb, &stride, &vbOff);
    int r = -1;
    if (ib && vb && stride) {
        unsigned h = (unsigned)(size_t)ib * 31u ^ (unsigned)(size_t)vb * 17u ^ start * 13u ^ count * 7u ^ (unsigned)d.base * 3u ^ ibOff ^ vbOff;
        for (int q = 0; q < 16 && r < 0; ++q) {
            int slot = (int)((h + q) & (kMeshes - 1));
            MeshB& m = g_mesh[slot];
            if (m.state && m.ib == ib && m.vb == vb && m.start == start && m.count == count && m.base == d.base && m.ibOff == ibOff && m.vbOff == vbOff && m.stride == stride) { r = slot; break; }
            if (m.state) continue;
            if (g_meshJobFrame != g_frame) { g_meshJobFrame = g_frame; g_meshJobsThisFrame = 0; }
            if (g_meshJobsThisFrame >= 3) break;
            bool i16 = ifmt == DXGI_FORMAT_R16_UINT;
            bool okFmt = ip->fmt == DXGI_FORMAT_R32G32B32_FLOAT || ip->fmt == DXGI_FORMAT_R32G32B32A32_FLOAT || ip->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                ip->fmt == DXGI_FORMAT_R16G16B16A16_SNORM;
            memset(&m, 0, sizeof(m));
            m.ib = ib; m.vb = vb; m.ibOff = ibOff; m.start = start; m.count = count; m.base = d.base; m.vbOff = vbOff; m.stride = stride;
            m.ifmt = i16 ? 2 : 4; m.pfmt = ip->fmt; m.poff = ip->off;
            m.state = 4;
            r = slot;
            if (!okFmt || (!i16 && ifmt != DXGI_FORMAT_R32_UINT) || count < 3 || count > 600000) { ++g_meshWhy[okFmt ? 3 : 2]; if (!okFmt && g_meshWhy[2] < 4) Log("car mesh: position format %d", ip->fmt); break; }
            D3D11_BUFFER_DESC sd = {};
            sd.ByteWidth = count * m.ifmt; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            D3D11_BUFFER_DESC ibd;
            ib->GetDesc(&ibd);
            UINT from = ibOff + start * m.ifmt;
            if (from + sd.ByteWidth > ibd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) { m.stage = 0; ++g_meshWhy[3]; break; }
            D3D11_BOX box = { from, 0, 0, from + sd.ByteWidth, 1, 1 };
            ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, ib, 0, &box);
            ib->AddRef(); vb->AddRef();
            m.state = 1; m.sentFrame = g_frame;
            ++g_meshJobsThisFrame;
        }
    }
    if (ib) ib->Release();
    if (vb) vb->Release();
    return r;
}
static void MeshDone(MeshB& m, int state)
{
    if (m.stage) { m.stage->Release(); m.stage = 0; }
    if (m.state == 1 || m.state == 2) { m.ib->Release(); m.vb->Release(); }
    free(m.idx16); free(m.idx32); m.idx16 = 0; m.idx32 = 0;
    m.state = state;
}
// the read-backs that have arrived (never waits)
static void MeshWork(ID3D11DeviceContext* ctx)
{
    for (int i = 0; i < kMeshes; ++i) {
        MeshB& m = g_mesh[i];
        if ((m.state != 1 && m.state != 2) || g_frame - m.sentFrame < 2) continue;
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT hr = ctx->Map(m.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) { ++g_meshWhy[5]; MeshDone(m, 4); continue; }
        if (m.state == 1) {
            UINT mn = 0xffffffff, mx = 0;
            if (m.ifmt == 2) { m.idx16 = (unsigned short*)malloc(m.count * 2); if (m.idx16) memcpy(m.idx16, mp.pData, m.count * 2); }
            else { m.idx32 = (UINT*)malloc(m.count * 4); if (m.idx32) memcpy(m.idx32, mp.pData, m.count * 4); }
            ctx->Unmap(m.stage, 0);
            m.stage->Release(); m.stage = 0;
            if (!m.idx16 && !m.idx32) { MeshDone(m, 4); continue; }
            for (UINT k = 0; k < m.count; ++k) {
                UINT v = m.idx16 ? m.idx16[k] : m.idx32[k];
                if (v == (m.idx16 ? 0xffffu : 0xffffffffu)) continue;   // a strip cut
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (mn > mx) { MeshDone(m, 4); continue; }
            INT first = (INT)mn + m.base;
            D3D11_BUFFER_DESC vbd;
            m.vb->GetDesc(&vbd);
            UINT from = m.vbOff + (UINT)first * m.stride, bytes = (mx - mn + 1) * m.stride;
            D3D11_BUFFER_DESC sd = {};
            sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (first < 0 || bytes > (16u << 20) || from + bytes > vbd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) {
                if (g_meshWhy[4] < 4) Log("car mesh: vertex range %d + %u x %u, offset %u, buffer %u", first, mx - mn + 1, m.stride, m.vbOff, vbd.ByteWidth);
                m.stage = 0; ++g_meshWhy[4]; MeshDone(m, 4); continue;
            }
            D3D11_BOX box = { from, 0, 0, from + bytes, 1, 1 };
            ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, m.vb, 0, &box);
            m.vmin = mn; m.vmax = mx; m.state = 2; m.sentFrame = g_frame;
            continue;
        }
        float bmin[3] = { 1e9f, 1e9f, 1e9f }, bmax[3] = { -1e9f, -1e9f, -1e9f };
        const unsigned char* vbase = (const unsigned char*)mp.pData;
        for (UINT k = 0; k < m.count; ++k) {
            UINT v = m.idx16 ? m.idx16[k] : m.idx32[k];
            if (v == (m.idx16 ? 0xffffu : 0xffffffffu)) continue;
            v -= m.vmin;
            const unsigned char* e = vbase + v * m.stride + m.poff;
            float p[3];
            if (m.pfmt == DXGI_FORMAT_R16G16B16A16_FLOAT) { const unsigned short* h = (const unsigned short*)e; for (int j = 0; j < 3; ++j) p[j] = Half(h[j]); }
            else if (m.pfmt == DXGI_FORMAT_R16G16B16A16_SNORM) { const short* h = (const short*)e; for (int j = 0; j < 3; ++j) p[j] = fmaxf(h[j] / 32767.0f, -1.0f) * 10.0f; }   // x 10 as the shader
            else memcpy(p, e, 12);
            for (int j = 0; j < 3; ++j) { if (p[j] < bmin[j]) bmin[j] = p[j]; if (p[j] > bmax[j]) bmax[j] = p[j]; }
        }
        ctx->Unmap(m.stage, 0);
        memcpy(m.bmin, bmin, 12); memcpy(m.bmax, bmax, 12);
        if (g_meshLogged < 200) {
            ++g_meshLogged;
            Log("car mesh %d: fmt %d stride %u idx %u verts %u..%u -> x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f", i, m.pfmt, m.stride, m.count,
                m.vmin, m.vmax, bmin[0], bmax[0], bmin[1], bmax[1], bmin[2], bmax[2]);
        }
        ++g_meshWhy[6];
        MeshDone(m, 3);
    }
    static unsigned last;
    if (g_frame - last > 1800) {
        last = g_frame;
        Log("car meshes: done %d | no layout %d, not indexed %d, position format %d, index range %d, vertex range %d, map %d | cars %d (lit %d, with bounds %d)",
            g_meshWhy[6], g_meshWhy[0], g_meshWhy[1], g_meshWhy[2], g_meshWhy[3], g_meshWhy[4], g_meshWhy[5], g_carN, CarCount(0), CarCount(1));
    }
}

static const int kCarDraws = 1024, kCarStages = 4, kCars = 128;
static ID3D11Buffer* g_carStage[kCarStages];
static unsigned g_carStageFrame[kCarStages];   // frame + 1 written, 0 = free
static double g_carStageTime[kCarStages];      // when (s)
static int g_carStageN[kCarStages], g_carStageCur = -1;
static short g_carStageMesh[kCarStages][kCarDraws];   // each draw's mesh (g_mesh slot, -1: unknown)
// this frame's matrices on the GPU too (t46): the lamp-fix pass puts every car light on its car as drawn this frame
static ID3D11Buffer* g_carMatBuf;
// drive: seconds seen driving (forward, steady); lit once that is long enough. b: its bounds (local; hasB: known)
// still: seconds seen standing; parked once long enough (then it needs a long drive to be lit: a knocked car rolls)
struct Car { float p[3], x[3], z[3], v[3]; unsigned seen, age; double t; float drive, sp, still; bool lit, hasB, parked; float bmin[3], bmax[3]; };
static const float* CarFixOf(const Car& C);
static int g_carAim = -1;      // the car aimed at while editing (its lights blink)

static double NowS() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return g_qpf.QuadPart ? (double)c.QuadPart / g_qpf.QuadPart : 0; }
static Car g_car[kCars];

static void CarAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& dc)
{
    if (g_p[P_TRAFFIC].v <= 0) return;
    VsInfo* e = DrawVs(ctx);   // (1.65: asked once a draw)
    if (!e || !(e->carBits & 2) || (e->carBits & 5) || e->wSlot < 0 || !MainTarget(ctx)) return;
    if (!g_carMatBuf) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = kCarDraws * 64; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = kCarDraws * 4;
        if (FAILED(g_dev->CreateBuffer(&d, 0, &g_carMatBuf)) || FAILED(g_dev->CreateShaderResourceView(g_carMatBuf, &sv, &g_carMatSrv))) {
            if (g_carMatBuf) g_carMatBuf->Release();
            g_carMatBuf = 0; g_carMatSrv = 0;
            return;
        }
    }
    if (g_carMatFrame != g_frame + 1) { g_carMatFrame = g_frame + 1; g_carMatN = 0; }
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    if ((UINT)e->wOff + 64 > bd.ByteWidth) { cb->Release(); return; }
    D3D11_BOX box = { (UINT)e->wOff, 0, 0, (UINT)e->wOff + 64, 1, 1 };
    // to the GPU list of this frame
    if (g_carMatN < kCarDraws) { ctx->CopySubresourceRegion(g_carMatBuf, 0, g_carMatN * 64, 0, 0, cb, 0, &box); ++g_carMatN; }
    // and to this frame's staging buffer (the CPU follows the cars with it)
    if (g_carStageCur < 0 || g_carStageFrame[g_carStageCur] != g_frame + 1) {
        g_carStageCur = -1;
        for (int k = 0; k < kCarStages; ++k) if (!g_carStageFrame[k]) { g_carStageCur = k; break; }
        if (g_carStageCur >= 0 && !g_carStage[g_carStageCur]) {
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = kCarDraws * 64; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_dev->CreateBuffer(&d, 0, &g_carStage[g_carStageCur]))) g_carStageCur = -1;
        }
        if (g_carStageCur >= 0) { g_carStageFrame[g_carStageCur] = g_frame + 1; g_carStageN[g_carStageCur] = 0; g_carStageTime[g_carStageCur] = NowS(); }
    }
    if (g_carStageCur >= 0) {
        int& n = g_carStageN[g_carStageCur];
        if (n < kCarDraws) { ctx->CopySubresourceRegion(g_carStage[g_carStageCur], 0, n * 64, 0, 0, cb, 0, &box); g_carStageMesh[g_carStageCur][n] = (short)MeshOf(ctx, dc); ++n; }
    }
    cb->Release();
}

// the oldest finished capture: its cars, followed
static void CarsReadOne(ID3D11DeviceContext* ctx, int k);
static void CarsRead(ID3D11DeviceContext* ctx)
{
    MeshWork(ctx);
    for (int round = 0; round < kCarStages; ++round) {   // every finished capture, oldest first
        int k = -1;
        for (int q = 0; q < kCarStages; ++q)
            if (g_carStageFrame[q] && g_carStageFrame[q] + 1 < g_frame + 1 && (k < 0 || g_carStageFrame[q] < g_carStageFrame[k])) k = q;
        if (k < 0) return;
        unsigned before = g_carStageFrame[k];
        CarsReadOne(ctx, k);
        if (g_carStageFrame[k] == before) return;   // not ready yet
    }
}

static void CarsReadOne(ID3D11DeviceContext* ctx, int k)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (ctx->Map(g_carStage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;
    unsigned when = g_carStageFrame[k] - 1;
    double tw = g_carStageTime[k];
    const float* f = (const float*)m.pData;
    static float cars[kCarDraws][16], cb[kCarDraws][6];
    static signed char cbs[kCarDraws];   // the car's bounds: 1 all its parts known, 0 none yet, -1 some missing
    int nc = 0;
    for (int i = 0; i < g_carStageN[k]; ++i) {
        const float* w = f + i * 16;
        if (!(w[12] == w[12]) || fabsf(w[5]) < 0.5f) continue;   // upright cars only
        int c = 0;
        for (; c < nc; ++c)
            if (fabsf(cars[c][12] - w[12]) < 0.05f && fabsf(cars[c][13] - w[13]) < 0.05f && fabsf(cars[c][14] - w[14]) < 0.05f) break;
        if (c == nc) { memcpy(cars[nc], w, 64); cbs[nc] = 0; for (int j = 0; j < 3; ++j) { cb[nc][j] = 1e9f; cb[nc][3 + j] = -1e9f; } ++nc; }
        int mi = g_carStageMesh[k][i];
        if (mi >= 0 && g_mesh[mi].state == 3) {
            for (int j = 0; j < 3; ++j) { cb[c][j] = fminf(cb[c][j], g_mesh[mi].bmin[j]); cb[c][3 + j] = fmaxf(cb[c][3 + j], g_mesh[mi].bmax[j]); }
            if (cbs[c] == 0) cbs[c] = 1;
        } else if (mi < 0 || g_mesh[mi].state != 4) cbs[c] = -1;
    }
    ctx->Unmap(g_carStage[k], 0);
    g_carStageFrame[k] = 0;
    // follow: each car to the known one nearest to where that should be by now (within 3 m, the same heading);
    // the closest pairs first, so a car driving past a parked one does not take its place
    struct Pair { float d2; short c, t; };
    static Pair pr[8192];
    int np = 0;
    for (int c = 0; c < nc; ++c) {
        const float* w = cars[c];
        for (int t = 0; t < g_carN && np < 8192; ++t) {
            const Car& T = g_car[t];
            if (T.z[0] * w[8] + T.z[1] * w[9] + T.z[2] * w[10] < 0.7f) continue;
            float dt = fminf((float)(tw - T.t), 1.5f);
            float dx = T.p[0] + T.v[0] * dt - w[12], dy = T.p[1] + T.v[1] * dt - w[13], dz = T.p[2] + T.v[2] * dt - w[14];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 3.0f * 3.0f) { pr[np].d2 = d2; pr[np].c = (short)c; pr[np].t = (short)t; ++np; }
        }
    }
    struct ByD2 { static int cmp(const void* a, const void* b) { float x = ((const Pair*)a)->d2, y = ((const Pair*)b)->d2; return x < y ? -1 : x > y ? 1 : 0; } };
    qsort(pr, np, sizeof(Pair), ByD2::cmp);
    static int carOf[kCarDraws];
    static bool used[kCars];
    memset(used, 0, sizeof(used));
    for (int c = 0; c < nc; ++c) carOf[c] = -1;
    for (int q = 0; q < np; ++q)
        if (carOf[pr[q].c] < 0 && !used[pr[q].t]) { carOf[pr[q].c] = pr[q].t; used[pr[q].t] = true; }
    for (int c = 0; c < nc; ++c) {
        if (carOf[c] >= 0) continue;
        const float* w = cars[c];
        int best = -1;
        float bd = 40.0f * 40.0f;
        for (int t = 0; t < g_carN; ++t) {
            const Car& T = g_car[t];
            if (used[t] || !T.lit || tw - T.t < 0.3 || T.z[0] * w[8] + T.z[1] * w[9] + T.z[2] * w[10] < 0.7f) continue;
            float dt = fminf((float)(tw - T.t), 1.5f);
            float dx = T.p[0] + T.v[0] * dt - w[12], dy = T.p[1] + T.v[1] * dt - w[13], dz = T.p[2] + T.v[2] * dt - w[14];
            float d2 = dx * dx + dy * dy + dz * dz;
            float hz = sqrtf(T.z[0] * T.z[0] + T.z[2] * T.z[2]) + 1e-6f;
            float ox = w[12] - T.p[0], oz = w[14] - T.p[2];
            float ahead = (ox * T.z[0] + oz * T.z[2]) / hz, side = fabsf(ox * T.z[2] - oz * T.z[0]) / hz;
            if (ahead < -2.0f || ahead > 40.0f || side > 1.8f) continue;
            if (T.hasB && cbs[c] == 1 && (fabsf((cb[c][5] - cb[c][2]) - (T.bmax[2] - T.bmin[2])) > 0.3f || fabsf((cb[c][3] - cb[c][0]) - (T.bmax[0] - T.bmin[0])) > 0.3f)) continue;
            if (d2 < bd) { bd = d2; best = t; }
        }
        if (best >= 0) {
            carOf[c] = best; used[best] = true;
            Car& T = g_car[best];   // its speed starts again from here
            memcpy(T.p, w + 12, 12); memset(T.v, 0, sizeof(T.v)); T.sp = 0; T.t = tw;
        }
    }
    for (int c = 0; c < nc; ++c) {
        const float* w = cars[c];
        int best = carOf[c];
        if (best < 0) {
            if (g_carN >= kCars) {
                double old = 1.0;
                for (int t = 0; t < g_carN; ++t) if (!used[t] && tw - g_car[t].t > old) { old = tw - g_car[t].t; best = t; }
                if (best < 0) continue;
            } else best = g_carN++;
            used[best] = true;
            memset(&g_car[best], 0, sizeof(Car));
            memcpy(g_car[best].p, w + 12, 12);
            g_car[best].seen = when; g_car[best].t = tw;
        }
        Car& C = g_car[best];
        float dt = (float)(tw - C.t);
        if (dt > 0.001f && C.age > 0) {
            float k = C.age == 1 ? 1.0f : 0.35f;
            for (int j = 0; j < 3; ++j) { float v = (w[12 + j] - C.p[j]) / dt; C.v[j] += (v - C.v[j]) * k; }
            float sp = sqrtf(C.v[0] * C.v[0] + C.v[2] * C.v[2]);
            if (C.age == 1) C.sp = sp;
            // driving: forward along its own axis, without a hard jolt (a knocked car slides sideways or stops hard)
            float along = sp > 0.1f ? (C.v[0] * w[8] + C.v[2] * w[10]) / (sp * sqrtf(w[8] * w[8] + w[10] * w[10]) + 1e-6f) : 0;
            // a jolt: the speed far from its half-second average (a knock from rest, a hard slide to a stop);
            // traffic speeds up / slows down gently. Sideways (a slide) also counts against it.
            C.sp += (sp - C.sp) * fminf(dt / 0.5f, 1.0f);
            bool jolt = fabsf(sp - C.sp) > 3.0f;
            if (sp > 2.0f && along > 0.9f && !jolt) C.drive += dt;
            else if (!C.lit && (jolt || (sp > 2.0f && along < 0.7f))) C.drive = 0;
            if (sp < 0.5f) { C.still += dt; if (C.still > 1.0f && !C.lit) C.parked = true; } else C.still = 0;
            if (C.drive > (C.parked ? 2.5f : 0.25f)) C.lit = true;
        }
        if (cbs[c] == 1) {
            float L = cb[c][5] - cb[c][2], W = cb[c][3] - cb[c][0], H = cb[c][4] - cb[c][1];
            bool sane = L > 2.8f && L < 18.0f && W > 1.2f && W < 3.4f && H > 0.8f && H < 5.0f && cb[c][1] > -0.8f && cb[c][1] < 0.8f;
            if (sane) { C.hasB = true; memcpy(C.bmin, cb[c], 12); memcpy(C.bmax, cb[c] + 3, 12); }
        }
        memcpy(C.p, w + 12, 12); memcpy(C.x, w, 12); memcpy(C.z, w + 8, 12);
        C.seen = when; C.t = tw; ++C.age;
    }
    for (int t = 0; t < g_carN; ++t)
        if (tw - g_car[t].t > (g_car[t].lit ? 60.0 : 4.0)) { g_car[t] = g_car[--g_carN]; --t; }   // out of view: a lit car is kept a minute
}

// the lights of a car, now (its last read position moved on by its speed)
// (pos is only a first guess: the lamp-fix pass puts the lights on the car as drawn this frame; dir[i] holds where the
// car should be now and its heading, for the pass to find it; lo[i] the light on the car, local)
static int CarLights(int c, float (*pos)[4], float (*col)[4], float (*dir)[4], float (*lo)[4])
{
    const Car& C = g_car[c];
    if (C.age < 2 || !C.lit) return 0;   // a parked car (never seen driving) stays dark
    float dt = (float)(NowS() - C.t);
    bool standing = C.v[0] * C.v[0] + C.v[2] * C.v[2] < 1.0f;
    if (dt > (standing ? 20.0f : 3.5f)) return 0;
    if (dt > 1.5f) dt = 1.5f;   // out of view: moved on by its speed, then held
    float p[3] = { C.p[0] + C.v[0] * dt, C.p[1] + C.v[1] * dt, C.p[2] + C.v[2] * dt };
    if (g_frame - C.seen > 6 && g_camKnown) {
        float m[3] = { p[0], p[1] + 0.7f, p[2] };
        if (OnScreen(m) && Dist2(m, g_cam[3]) > 4.0f * 4.0f) return 0;
    }
    float pw = g_p[P_TRAFFIC].v;
    // local: x right, y up, z forward. From the car's bounds if known (a truck: higher, wider, further out), else a car's
    float kL[4][3] = { { 0.65f, 0.65f, 2.1f }, { -0.65f, 0.65f, 2.1f }, { 0.6f, 0.85f, -2.2f }, { -0.6f, 0.85f, -2.2f } };
    if (C.hasB) {
        float cx = (C.bmin[0] + C.bmax[0]) * 0.5f, hw = (C.bmax[0] - C.bmin[0]) * 0.5f, H = C.bmax[1] - fmaxf(C.bmin[1], 0.0f);
        float hx = fmaxf(hw - 0.3f, 0.4f), tx = fmaxf(hw - 0.25f, 0.4f);
        float hy = fminf(0.47f * H, 1.15f), ty = fminf(0.6f * H, 1.2f);
        float hz = C.bmax[2] - 0.2f, tz = C.bmin[2] + 0.15f;
        float v[4][3] = { { cx + hx, hy, hz }, { cx - hx, hy, hz }, { cx + tx, ty, tz }, { cx - tx, ty, tz } };
        memcpy(kL, v, sizeof(kL));
    }
    const float* o = CarFixOf(C);   // the model's own correction (set by hand)
    if (o) {
        kL[0][0] += o[0]; kL[1][0] -= o[0]; kL[0][1] += o[1]; kL[1][1] += o[1]; kL[0][2] += o[2]; kL[1][2] += o[2];
        kL[2][0] += o[3]; kL[3][0] -= o[3]; kL[2][1] += o[4]; kL[3][1] += o[4]; kL[2][2] -= o[5]; kL[3][2] -= o[5];
    }
    float head = atan2f(C.z[0], C.z[2]);
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 3; ++j) pos[i][j] = p[j] + C.x[j] * kL[i][0] + (j == 1 ? kL[i][1] : 0) + C.z[j] * kL[i][2];
        pos[i][3] = 1;   // as a map lamp: off by day with them
        memcpy(lo[i], kL[i], 12);
        memcpy(dir[i], p, 12); dir[i][3] = head;   // for the fix pass: the car's place now and heading (the GPU finds its matrix)
        bool hd = i < 2;
        const Param* q = &g_p[hd ? P_TR_HEAD : P_TR_TAIL];
        float base[3];
        if (hd) {   // warmth: xenon (blue-white) .. halogen (yellow)
            float w = q[1].v;
            base[0] = 33 * (0.80f + 0.20f * w); base[1] = 33 * (0.90f - 0.08f * w); base[2] = 33 * (1.00f - 0.45f * w);
        } else { base[0] = 6; base[1] = 0.3f; base[2] = 0.15f; }
        float sat = hd ? q[2].v : q[1].v, br = q[0].v * pw;
        if (c == g_carAim) br *= 1.0f + 0.8f * sinf((float)NowS() * 8.0f);   // the car being edited blinks
        float lum = 0.2126f * base[0] + 0.7152f * base[1] + 0.0722f * base[2];
        for (int j = 0; j < 3; ++j) col[i][j] = fmaxf(lum + (base[j] - lum) * sat, 0.0f) * br;
        col[i][3] = 0;
        lo[i][3] = cosf(q[3].v * 0.0174533f);
    }
    return 4;
}

// ---- per model corrections of the light places (menu: CAR LIGHTS, "aimed car" items). A model is known by its
// size (length / width / height, within 12 cm: the far version of a model is a little different). Saved in
// NewLook_carlights.txt: L W H, then headlights out / up / forward, tail lights out / up / back (m).
struct CarFix { float L, W, H, o[6]; };
static CarFix g_fix[256];
static int g_fixN = -1;
static bool g_carEditOn;
static char g_carEditMsg[128];
static void CarDims(const Car& C, float* d) { d[0] = C.bmax[2] - C.bmin[2]; d[1] = C.bmax[0] - C.bmin[0]; d[2] = C.bmax[1] - C.bmin[1]; }
static void CarFixLoad()
{
    g_fixN = 0;
    char p[MAX_PATH];
    sprintf(p, "%sNewLook_carlights.txt", g_dir);
    FILE* f = fopen(p, "r");
    if (!f) return;
    char line[256];
    while (g_fixN < 256 && fgets(line, sizeof(line), f)) {
        CarFix& F = g_fix[g_fixN];
        if (sscanf(line, "%f %f %f %f %f %f %f %f %f", &F.L, &F.W, &F.H, &F.o[0], &F.o[1], &F.o[2], &F.o[3], &F.o[4], &F.o[5]) == 9) ++g_fixN;
    }
    fclose(f);
}
static void CarFixSave()
{
    char p[MAX_PATH];
    sprintf(p, "%sNewLook_carlights.txt", g_dir);
    FILE* f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "# NewLook: traffic light places per car model. length width height (m) | headlights out up forward | tail lights out up back\n");
    for (int i = 0; i < g_fixN; ++i) {
        const CarFix& F = g_fix[i];
        fprintf(f, "%.2f %.2f %.2f   %.2f %.2f %.2f   %.2f %.2f %.2f\n", F.L, F.W, F.H, F.o[0], F.o[1], F.o[2], F.o[3], F.o[4], F.o[5]);
    }
    fclose(f);
}
static int CarFixFind(const Car& C, bool make)
{
    if (!C.hasB) return -1;
    if (g_fixN < 0) CarFixLoad();
    float d[3];
    CarDims(C, d);
    int best = -1;
    float bd = 1e9f;
    for (int i = 0; i < g_fixN; ++i) {
        float e = fmaxf(fabsf(g_fix[i].L - d[0]), fmaxf(fabsf(g_fix[i].W - d[1]), fabsf(g_fix[i].H - d[2])));
        if (e < 0.12f && e < bd) { bd = e; best = i; }
    }
    if (best < 0 && make && g_fixN < 256) { best = g_fixN++; CarFix& F = g_fix[best]; F.L = d[0]; F.W = d[1]; F.H = d[2]; memset(F.o, 0, sizeof(F.o)); }
    return best;
}
static const float* CarFixOf(const Car& C) { int i = CarFixFind(C, false); return i >= 0 ? g_fix[i].o : 0; }

// every frame: while one of the "aimed car" items is selected in the shown menu, the car nearest to the middle of the
// screen is the one edited; the items show its correction, and a change goes to its model (saved at once)
static bool CarEditTick(int sel, bool shown)   // true: the menu text changed
{
    bool dirty = false;
    g_carEditOn = shown && sel >= P_TR_FIX && sel < P_TR_FIX + 6 && g_p[P_TRAFFIC].v > 0;
    static float snap[6];
    static int editing = -2;
    if (!g_carEditOn) { g_carAim = -1; editing = -2; return false; }
    int best = -1;
    float bd = 0.25f * 0.25f;
    for (int c = 0; c < g_carN; ++c) {
        const Car& C = g_car[c];
        if (!C.lit || !C.hasB || g_frame - C.seen > 10 || Dist2(C.p, g_cam[3]) > 150.0f * 150.0f) continue;
        float m[3] = { C.p[0], C.p[1] + 0.7f, C.p[2] }, u, v;
        if (!ProjectUV(m, &u, &v)) continue;
        float d2 = (u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f);
        if (d2 < bd) { bd = d2; best = c; }
    }
    g_carAim = best;
    if (best < 0) {
        if (strcmp(g_carEditMsg, "aim at a lit traffic car (the middle of the screen)")) dirty = true;
        strcpy(g_carEditMsg, "aim at a lit traffic car (the middle of the screen)");
        editing = -2;
        return dirty;
    }
    float d[3];
    CarDims(g_car[best], d);
    int fi = CarFixFind(g_car[best], false);
    if (fi != editing) {   // another model: show its correction
        editing = fi;
        for (int i = 0; i < 6; ++i) snap[i] = g_p[P_TR_FIX + i].v = fi >= 0 ? g_fix[fi].o[i] : 0.0f;
        dirty = true;
    } else {
        bool ch = false;
        for (int i = 0; i < 6; ++i) ch |= g_p[P_TR_FIX + i].v != snap[i];
        if (ch) {
            if (fi < 0) fi = editing = CarFixFind(g_car[best], true);
            if (fi >= 0) {
                for (int i = 0; i < 6; ++i) snap[i] = g_fix[fi].o[i] = g_p[P_TR_FIX + i].v;
                CarFixSave();
            }
        }
    }
    char was[128];
    strcpy(was, g_carEditMsg);
    sprintf(g_carEditMsg, "editing the blinking car: model %.2f x %.2f x %.2f m%s", d[0], d[1], d[2], fi >= 0 ? " (own places, saved)" : "");
    if (strcmp(was, g_carEditMsg)) dirty = true;
    return dirty;
}

static int CarCount(int what)
{
    int n = 0;
    for (int i = 0; i < g_carN; ++i) n += what == 0 ? g_car[i].lit : g_car[i].hasB;
    return n;
}

static int CarMatCount() { return g_carMatFrame == g_frame + 1 ? g_carMatN : 0; }
