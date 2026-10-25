// ---- 1.94: the game's own reflections under our sky. Its materials (76 PSs of SHADERS1: windows (LitWindowColour),
// walls, decals, glass) bake a cube map's reflection into the G-buffer's 4th target (and the forward glass into the
// frame), tinted by gGlobalCB's SkyReflectionAverageColour (x the cube's a) and SkyReflectionAverageLight (x its rgb):
// the game's time of day, no weather: a storm over windows reflecting a sunset. For their draws the buffer is swapped
// for a copy with both turned to our sky's colour (the sky probe's mean, read back a few frames late, never waiting)
// at their own brightness, darker under a thick deck. Found by reflection at creation (both in one buffer).
struct SrSh { void* sh; short slot, offC, offL; };
static SrSh g_sr[256];
static volatile LONG g_srN;
static void SkyReflNote(const void* code, SIZE_T size, void* sh)
{
    if (!g_reflect || !code) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC bd;
        if (!cb || FAILED(cb->GetDesc(&bd))) continue;
        D3D11_SHADER_VARIABLE_DESC vc, vl;
        ID3D11ShaderReflectionVariable* a = cb->GetVariableByName("SkyReflectionAverageColour");
        ID3D11ShaderReflectionVariable* b = cb->GetVariableByName("SkyReflectionAverageLight");
        if (!a || !b || FAILED(a->GetDesc(&vc)) || FAILED(b->GetDesc(&vl))) continue;
        D3D11_SHADER_INPUT_BIND_DESC bind;
        if (FAILED(r->GetResourceBindingDescByName(bd.Name, &bind))) continue;
        LONG k = InterlockedIncrement(&g_srN) - 1;
        if (k >= 256) { g_srN = 256; break; }
        g_sr[k].slot = (short)bind.BindPoint; g_sr[k].offC = (short)vc.StartOffset; g_sr[k].offL = (short)vl.StartOffset;
        MemoryBarrier();
        g_sr[k].sh = sh;
        break;
    }
    r->Release();
}

static float g_srCol[3] = { 1, 1, 1 };   // our sky's colour (luminance 1)
static float g_srK = 1, g_srW;           // its brightness against the game's; how much (0: the game's)
static bool g_srColKnown, g_srOn;
static unsigned g_srVer = 1, g_srFrame, g_srDraws;
static ID3D11Texture2D* g_srStage[2];
static unsigned g_srStageFrame[2];
static int g_srStageNext;
struct SrCache { ID3D11Buffer* src; ID3D11Buffer* ours; UINT size, alloc, hash, ver; };
static SrCache g_srCache[64];
static ID3D11Buffer* g_srSaved;
static int g_srSlot = -1;

// once a frame: the probe's mean read back, the values to put in
static void SkyReflFrame(ID3D11DeviceContext* ctx)
{
    g_srFrame = g_frame + 1;
    bool skyOn = g_enabled && g_p[P_SKY].v >= 0.5f;
    float want = skyOn && g_sunActive ? g_p[239].v * (1 - g_sunG) : 0.0f;
    g_srW += (want - g_srW) * 0.05f;
    bool changed = false;
    if (g_probeTex && g_probeFrame) {
        D3D11_TEXTURE2D_DESC pd;
        g_probeTex->GetDesc(&pd);
        for (int k = 0; k < 2; ++k) {
            if (!g_srStageFrame[k] || g_frame + 1 - g_srStageFrame[k] < 3 || !g_srStage[k]) continue;
            D3D11_MAPPED_SUBRESOURCE m;
            float sum[3] = {};
            int got = 0;
            for (int f = 0; f < 6; ++f) {
                if (ctx->Map(g_srStage[k], f, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) break;
                const unsigned short* h = (const unsigned short*)m.pData;
                for (int j = 0; j < 3; ++j) { float v = HalfToFloat(h[j]); sum[j] += v == v && v > 0 ? v : 0; }
                ctx->Unmap(g_srStage[k], f);
                ++got;
            }
            if (got < 6) continue;
            g_srStageFrame[k] = 0;
            float L = sum[0] * 0.2126f + sum[1] * 0.7152f + sum[2] * 0.0722f;
            if (L > 1e-5f) {
                for (int j = 0; j < 3; ++j) {
                    float c = sum[j] / L;
                    c = c > 3 ? 3 : c;
                    g_srCol[j] = g_srColKnown ? g_srCol[j] + (c - g_srCol[j]) * 0.3f : c;
                }
                g_srColKnown = true;
                changed = true;
            }
        }
        int k = g_srStageNext;
        if ((g_frame % 15) == 0 && !g_srStageFrame[k]) {
            if (!g_srStage[k]) {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 6; td.Format = pd.Format; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(g_dev->CreateTexture2D(&td, 0, &g_srStage[k]))) g_srStage[k] = 0;
            }
            if (g_srStage[k] && pd.MipLevels > 1 && pd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                UINT last = pd.MipLevels - 1;
                D3D11_BOX b = { 0, 0, 0, 1, 1, 1 };
                for (UINT f = 0; f < 6; ++f) ctx->CopySubresourceRegion(g_srStage[k], f, 0, 0, 0, g_probeTex, D3D11CalcSubresource(last, f, pd.MipLevels), &b);
                g_srStageFrame[k] = g_frame + 1;
                g_srStageNext = k ^ 1;
            }
        }
    }
    // a thick deck: darker than the game's clear sky (as the sky's CloudDeck, from overcast on)
    float cover = skyOn ? g_p[P_SKY + 2].v : 0.0f, oc = (cover - 0.4f) / 0.55f;
    oc = (oc < 0 ? 0 : oc > 1 ? 1 : oc) * (0.6f + 0.4f * g_p[P_SKY + 11].v);
    float k = 1 - 0.55f * oc;
    if (fabsf(k - g_srK) > 0.002f) { g_srK = k; changed = true; }
    static float wDone;
    if (fabsf(g_srW - wDone) > 0.005f) { wDone = g_srW; changed = true; }
    if (changed) ++g_srVer;
    g_srOn = g_srColKnown && g_srW > 0.01f;
    if (g_frame % 900 == 0)
        Log("game's reflections (skyrefl.inc): %ld shaders, %u draws tinted (last 900 frames); our sky %.2f %.2f %.2f x %.2f, by %.2f",
            (long)g_srN, g_srDraws, g_srCol[0], g_srCol[1], g_srCol[2], g_srK, g_srW), g_srDraws = 0;
}
static void SrTint(float* v)
{
    float L = v[0] * 0.2126f + v[1] * 0.7152f + v[2] * 0.0722f;
    for (int j = 0; j < 3; ++j) v[j] += (L * g_srCol[j] * g_srK - v[j]) * g_srW;
}
static void SkyReflBefore(ID3D11DeviceContext* ctx)
{
    if (g_srFrame != g_frame + 1) SkyReflFrame(ctx);
    if (!g_srOn) return;
    void* ps = g_lastPS;
    LONG n = g_srN;
    static void* lastPs;   // (1.96: the last shader's entry kept: its draws come in runs)
    static LONG lastN = -1, lastI = -1;
    if (ps != lastPs || n != lastN) {
        lastPs = ps; lastN = n; lastI = -1;
        for (LONG i = 0; i < n && i < 256; ++i) if (g_sr[i].sh == ps) { lastI = i; break; }
    }
    if (lastI < 0) return;
    const SrSh* p = &g_sr[lastI];
    ID3D11Buffer* b = 0;
    ctx->PSGetConstantBuffers(p->slot, 1, &b);
    if (!b) return;
    const unsigned char* data = 0;
    UINT size = 0;
    UINT h = 0;   // (1.96: the copy's fill number, not a hash of its bytes)
    if (WeCbNow(b, &data, &size, &h) != 1 || (UINT)p->offC + 12 > size || (UINT)p->offL + 12 > size) { b->Release(); return; }
    SrCache& c = g_srCache[((size_t)b >> 4) & 63];
    if (c.src != b || c.hash != h || c.ver != g_srVer || c.size != size || !c.ours) {
        UINT need = (size + 15) & ~15u;
        if (!c.ours || c.alloc < need) {
            if (c.ours) c.ours->Release();
            c.ours = 0;
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = need; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_dev->CreateBuffer(&bd, 0, &c.ours))) { c.ours = 0; c.src = 0; b->Release(); return; }
            c.alloc = need;
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(c.ours, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { c.src = 0; b->Release(); return; }
        memcpy(m.pData, data, size);
        SrTint((float*)((unsigned char*)m.pData + p->offC));
        SrTint((float*)((unsigned char*)m.pData + p->offL));
        ctx->Unmap(c.ours, 0);
        c.src = b; c.hash = h; c.ver = g_srVer; c.size = size;
    }
    ctx->PSSetConstantBuffers(p->slot, 1, &c.ours);
    g_srSaved = b; g_srSlot = p->slot;
    ++g_srDraws;
}
static void SkyReflAfter(ID3D11DeviceContext* ctx)
{
    if (g_srSlot < 0) return;
    ctx->PSSetConstantBuffers(g_srSlot, 1, &g_srSaved);
    g_srSaved->Release();
    g_srSlot = -1;
}
