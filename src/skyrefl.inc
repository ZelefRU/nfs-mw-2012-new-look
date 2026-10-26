// ---- 1.94: the game's own reflections under our sky. Its materials (76 PSs of SHADERS1: windows (LitWindowColour),
// walls, decals, glass) bake a cube map's reflection into the G-buffer's 4th target (and the forward glass into the
// frame), tinted by gGlobalCB's SkyReflectionAverageColour (x the cube's a) and SkyReflectionAverageLight (x its rgb):
// the game's time of day, no weather: a storm over windows reflecting a sunset. For their draws the buffer is swapped
// for a copy with both turned to our sky's colour (the sky probe's mean, read back a few frames late, never waiting)
// at their own brightness, darker under a thick deck. Found by reflection at creation (both in one buffer).
struct SrSh { void* sh; short slot, offC, offL; };
// 1.98m: the game's forward glass (the cars' windows: KeyLightColour + cubeReflectionSampler): its sun glint under our
// clouds as the deferred sun's: KeyLightColour x our clouds' transmittance at the camera x the deck's (it glinted in a storm)
struct KlSh { void* sh; short slot, off; bool sun; };   // (sun: 1.98o: the game's deferred sun, in its reflections' views only)
static KlSh g_kl[128];
static volatile LONG g_klN;
// 1.98n: and their cube map (the cars' paint and glass, cubeReflectionSampler): the game's own, a clear day's sky and
// sun in it whatever the weather; while our sky is on, our sky probe instead (a static cube only: not the game's views)
struct CrSh { void* sh; short slot; };
static CrSh g_cr[128];
static volatile LONG g_crN;
// 1.98t: the game's fog (GradientTextureA + LayerFogCoeffs / ScattCoeffs: its distance and height fog, coloured by the
// time of day's gradient: a sunset's pink whatever the weather): under a thick deck of ours a copy of that gradient
// instead, grey at its mean's brightness, dimmer in rain (read back a few frames late, never waiting)
struct GfSh { void* sh; short slot; };
static GfSh g_gf[64];
static volatile LONG g_gfN;
static void KlNote(ID3D11ShaderReflection* r, void* sh)
{
    D3D11_SHADER_INPUT_BIND_DESC bind;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    if (SUCCEEDED(r->GetResourceBindingDescByName("GradientTextureA", &bind)) && bind.Type == D3D_SIT_TEXTURE && bind.Dimension == D3D_SRV_DIMENSION_TEXTURE1D) {
        bool fog = false;
        for (UINT c = 0; c < sd.ConstantBuffers && !fog; ++c) {
            ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
            D3D11_SHADER_VARIABLE_DESC vd;
            ID3D11ShaderReflectionVariable* a = cb ? cb->GetVariableByName("LayerFogCoeffs") : 0;
            ID3D11ShaderReflectionVariable* b = cb ? cb->GetVariableByName("ScattCoeffs") : 0;
            fog = (a && SUCCEEDED(a->GetDesc(&vd))) || (b && SUCCEEDED(b->GetDesc(&vd)));
        }
        LONG k = fog ? InterlockedIncrement(&g_gfN) - 1 : -1;
        if (k >= 64) g_gfN = 64;
        else if (k >= 0) {
            g_gf[k].slot = (short)bind.BindPoint;
            MemoryBarrier();
            g_gf[k].sh = sh;
        }
    }
    bool cube = false;
    for (UINT i = 0; i < sd.BoundResources && !cube; ++i)
        if (SUCCEEDED(r->GetResourceBindingDesc(i, &bind)) && bind.Type == D3D_SIT_TEXTURE && bind.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE
            && bind.Name && !strcmp(bind.Name, "cubeReflectionSampler")) {
            cube = true;
            LONG k = InterlockedIncrement(&g_crN) - 1;
            if (k >= 128) { g_crN = 128; break; }
            g_cr[k].slot = (short)bind.BindPoint;
            MemoryBarrier();
            g_cr[k].sh = sh;
        }
    // 1.98o: and the game's deferred sun (G-buffer normals + cubeIrradianceSampler): the cars' cube map is the game's
    // own 512^2 view around the car, its city lit by that sun at full strength whatever the weather (ours lights the
    // main view only)
    D3D11_SHADER_INPUT_BIND_DESC b2;
    bool sun = SUCCEEDED(r->GetResourceBindingDescByName("deferred_normalBufferSampler", &b2)) && SUCCEEDED(r->GetResourceBindingDescByName("cubeIrradianceSampler", &b2));
    if (!cube && !sun) return;
    for (UINT c = 0; c < sd.ConstantBuffers; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC bd;
        D3D11_SHADER_VARIABLE_DESC vd;
        if (!cb || FAILED(cb->GetDesc(&bd))) continue;
        ID3D11ShaderReflectionVariable* v = cb->GetVariableByName("KeyLightColour");
        if (!v || FAILED(v->GetDesc(&vd)) || FAILED(r->GetResourceBindingDescByName(bd.Name, &bind))) continue;
        LONG k = InterlockedIncrement(&g_klN) - 1;
        if (k >= 128) { g_klN = 128; break; }
        g_kl[k].slot = (short)bind.BindPoint; g_kl[k].off = (short)vd.StartOffset; g_kl[k].sun = !cube;
        MemoryBarrier();
        g_kl[k].sh = sh;
        break;
    }
}
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
    KlNote(r, sh);
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
    g_srOn = g_srColKnown && g_srW > 0.01f && (int)(g_p[260].v + 0.5f) != 4;   // (1.96n: SkyTest 4)
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

static SrCache g_klCache[16];
static ID3D11Buffer* g_klSaved;
static int g_klSlot = -1;
static float g_klK = 1;
static unsigned g_klFrame, g_klDraws, g_klVer = 1;
static void KlBefore(ID3D11DeviceContext* ctx)
{
    if (g_klFrame != g_frame + 1) {   // (once a frame: the factor; the copies made anew when it moves)
        g_klFrame = g_frame + 1;
        bool on = g_enabled && g_p[P_SKY].v >= 0.5f;
        float k = on ? g_csSunHere * SkyDeckSunK() : 1.0f;
        k = k < 0 ? 0 : k > 1 ? 1 : k;
        if (fabsf(k - g_klK) > 0.003f) { g_klK = k; ++g_klVer; }
        if (g_frame % 900 == 0) Log("game's glass (skyrefl.inc): %ld shaders, %u draws, its sun x %.2f (last 900 frames)", (long)g_klN, g_klDraws, g_klK), g_klDraws = 0;
    }
    if (g_klK > 0.997f) return;
    void* ps = g_lastPS;
    LONG n = g_klN, i = 0;
    while (i < n && g_kl[i].sh != ps) ++i;
    if (i >= n || g_kl[i].slot == g_srSlot) return;
    const KlSh* p = &g_kl[i];
    if (p->sun && MainTarget(ctx)) return;   // (the main view: our sun pass)
    ID3D11Buffer* b = 0;
    ctx->PSGetConstantBuffers(p->slot, 1, &b);
    if (!b) return;
    const unsigned char* data = 0;
    UINT size = 0, h = 0;
    if (WeCbNow(b, &data, &size, &h) != 1 || (UINT)p->off + 12 > size) { b->Release(); return; }
    SrCache& c = g_klCache[((size_t)b >> 4) & 15];
    if (c.src != b || c.hash != h || c.ver != g_klVer || c.size != size || !c.ours) {
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
        float* kl = (float*)((unsigned char*)m.pData + p->off);
        for (int j = 0; j < 3; ++j) kl[j] *= g_klK;
        ctx->Unmap(c.ours, 0);
        c.src = b; c.hash = h; c.ver = g_klVer; c.size = size;
    }
    ctx->PSSetConstantBuffers(p->slot, 1, &c.ours);
    g_klSaved = b; g_klSlot = p->slot;
    ++g_klDraws;
}
// 1.98o: the game's effects (gVfxConstants with TonemapSampler: its sun sprite among them) not drawn in its reflections'
// views while our clouds hide the sun: the sun glinted in the car's glass in a storm from that view
static bool KlVfxSkip(ID3D11DeviceContext* ctx)
{
    if (g_klK >= 0.3f || !g_klN || !Immediate(ctx)) return false;
    void* ps = g_lastPS;
    static void* lastPs;
    static bool lastVfx;
    if (ps != lastPs) {
        lastPs = ps;
        const char* nm = PsNameOf((ID3D11PixelShader*)ps);
        lastVfx = strstr(nm, "gVfxConstants") && strstr(nm, "TonemapSampler");
    }
    return lastVfx && !MainTarget(ctx);
}
// 1.98t: the fog's gradient (see GfSh): a copy of ours. 1.98u: one for all: the game writes it to a new texture of a
// ring each frame (1.98t kept a copy per texture, 4 of them: the fog flickered between ours and the game's)
struct GfTex { ID3D11Texture1D* ours; ID3D11ShaderResourceView* srv; ID3D11Texture1D* stage[3]; unsigned stageFrame[3], frame; UINT w; DXGI_FORMAT fmt; int kind; bool ok; };
static GfTex g_gfT;
static ID3D11Resource* g_gfSeen[16];   // the game's gradients looked at: the same size and format as ours or not
static bool g_gfSeenOk[16];
static ID3D11ShaderResourceView* g_gfSaved;
static int g_gfSlot = -1;
static unsigned g_gfDraws;
static float SkyDeck()   // (the sky's SunDeck(), on the CPU)
{
    bool on = g_enabled && g_p[P_SKY].v >= 0.5f;
    float deck = on ? (g_p[P_SKY + 2].v - 0.6f) / 0.35f : 0.0f;
    return deck < 0 ? 0 : deck > 1 ? 1 : deck;   // (1.98x: as SunDeck: the cover alone; 1.98aa: from 0.6)
}
static unsigned short GfHalf(float x)   // (float -> half: normal range, else 0 / the largest)
{
    unsigned u; memcpy(&u, &x, 4);
    int e = (int)((u >> 23) & 255) - 127 + 15;
    return e <= 0 ? 0 : e >= 31 ? (unsigned short)(((u >> 16) & 0x8000) | 0x7BFF) : (unsigned short)(((u >> 16) & 0x8000) | (e << 10) | ((u >> 13) & 1023));
}
static void GfFill(ID3D11DeviceContext* ctx, GfTex& t, ID3D11Resource* src, float deck)
{
    // this frame's gradient copied; the one 2 frames old read (if ready), turned and put in ours
    int k = g_frame % 3;
    if (t.stage[k] && t.stageFrame[k] && g_frame + 1 - t.stageFrame[k] >= 2) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx->Map(t.stage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
            static float px[1024 * 4];
            UINT w = t.w;
            for (UINT x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    int cc = t.kind == 2 && c < 3 ? 2 - c : c;   // (BGRA)
                    px[x * 4 + c] = t.kind == 3 ? ((const float*)m.pData)[x * 4 + cc] : t.kind == 4 ? HalfToFloat(((const unsigned short*)m.pData)[x * 4 + cc])
                                  : ((const unsigned char*)m.pData)[x * 4 + cc] / 255.0f;
                }
            ctx->Unmap(t.stage[k], 0);
            t.stageFrame[k] = 0;
            double Y = 0;
            for (UINT x = 0; x < w; ++x) Y += px[x * 4] * 0.2126 + px[x * 4 + 1] * 0.7152 + px[x * 4 + 2] * 0.0722;
            float g = (float)(Y / w) * (0.8f - 0.25f * g_p[P_SKY + 11].v), a = deck * 0.9f;
            const float grey[3] = { 0.92f, 0.95f, 1.0f };
            static unsigned char out8[1024 * 4];
            static unsigned short out16[1024 * 4];
            for (UINT x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    float v = c < 3 ? px[x * 4 + c] + (g * grey[c] - px[x * 4 + c]) * a : px[x * 4 + c];   // (its alpha (the fog's amount) kept)
                    int cc = t.kind == 2 && c < 3 ? 2 - c : c;
                    if (t.kind == 3) px[x * 4 + c] = v;   // (in place: read before written)
                    else if (t.kind == 4) out16[x * 4 + cc] = GfHalf(v);
                    else { v = v < 0 ? 0 : v > 1 ? 1 : v; out8[x * 4 + cc] = (unsigned char)(v * 255 + 0.5f); }
                }
            const void* data = t.kind == 3 ? (const void*)px : t.kind == 4 ? (const void*)out16 : (const void*)out8;
            UINT pitch = w * (t.kind == 3 ? 16 : t.kind == 4 ? 8 : 4);
            ctx->UpdateSubresource(t.ours, 0, 0, data, pitch, pitch);   // (our own texture: the hook passes it on)
            t.ok = true;
        }
    }
    if (t.stage[k] && !t.stageFrame[k]) {
        ctx->CopySubresourceRegion(t.stage[k], 0, 0, 0, 0, src, 0, 0);
        t.stageFrame[k] = g_frame + 1;
    }
}
static void GfBefore(ID3D11DeviceContext* ctx)
{
    float deck = SkyDeck();
    if (deck < 0.02f) return;
    void* ps = g_lastPS;
    static void* lastPs;
    static LONG lastN = -1, lastI = -1;
    LONG n = g_gfN;
    if (ps != lastPs || n != lastN) {
        lastPs = ps; lastN = n; lastI = -1;
        for (LONG i = 0; i < n; ++i) if (g_gf[i].sh == ps) { lastI = i; break; }
    }
    if (lastI < 0) return;
    int slot = g_gf[lastI].slot;
    ID3D11ShaderResourceView* v = 0;
    ctx->PSGetShaderResources(slot, 1, &v);
    if (!v) return;
    ID3D11Resource* r = 0;
    v->GetResource(&r);
    GfTex& t = g_gfT;
    int j = 0;
    while (j < 16 && g_gfSeen[j] && g_gfSeen[j] != r) ++j;
    if (j == 16) { memset(g_gfSeen, 0, sizeof(g_gfSeen)); j = 0; }   // (more of them: looked at again)
    if (!g_gfSeen[j]) {   // (a texture not seen yet: its size and format; the first one makes ours and the read-backs)
        g_gfSeen[j] = r;
        ID3D11Texture1D* s = 0;
        D3D11_TEXTURE1D_DESC td = {};
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture1D), (void**)&s)) && s) { s->GetDesc(&td); s->Release(); }
        int f = (int)td.Format;
        int kind = !s || td.Width > 1024 || td.ArraySize != 1 ? 0 : f >= 27 && f <= 29 ? 1 : f == 87 || (f >= 90 && f <= 91) ? 2 : f >= 1 && f <= 2 ? 3 : f >= 9 && f <= 10 ? 4 : 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd;
        v->GetDesc(&vd);
        if (!t.ours && kind) {
            t.kind = kind; t.w = td.Width; t.fmt = td.Format;
            D3D11_TEXTURE1D_DESC od = td;
            od.MipLevels = 1; od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_SHADER_RESOURCE; od.CPUAccessFlags = 0; od.MiscFlags = 0;
            D3D11_SHADER_RESOURCE_VIEW_DESC nd = {};
            nd.Format = vd.Format; nd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE1D; nd.Texture1D.MipLevels = 1;
            if (FAILED(g_dev->CreateTexture1D(&od, 0, &t.ours)) || FAILED(g_dev->CreateShaderResourceView(t.ours, &nd, &t.srv))) t.kind = 0;
            D3D11_TEXTURE1D_DESC sd = od;
            sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (int q = 0; q < 3 && t.kind; ++q) if (FAILED(g_dev->CreateTexture1D(&sd, 0, &t.stage[q]))) t.kind = 0;
        }
        bool ok = t.kind && kind == t.kind && td.Width == t.w && td.Format == t.fmt;
        static int logged;
        if (logged < 3 && (!ok || !logged)) { ++logged; Log("game's fog: its gradient %u wide, %u mips, format %d (view %d) (shader %p): %s", td.Width, td.MipLevels, f, (int)vd.Format, ps,
                                                           ok ? "grey under a thick deck (one copy for all: a new texture each frame)" : "unlike ours / unknown: left alone"); }
        g_gfSeenOk[j] = ok;
    }
    if (!g_gfSeenOk[j]) { if (r) r->Release(); v->Release(); return; }
    if (t.frame != g_frame + 1) {
        t.frame = g_frame + 1;
        GfFill(ctx, t, r, deck);
        if (g_frame % 900 == 0) Log("game's fog (skyrefl.inc): %ld shaders, %u draws with our grey gradient (last 900 frames), the deck %.2f", (long)g_gfN, g_gfDraws, deck), g_gfDraws = 0;
    }
    if (r) r->Release();
    if (!t.ok) { v->Release(); return; }
    ctx->PSSetShaderResources(slot, 1, &t.srv);
    g_gfSaved = v; g_gfSlot = slot;
    ++g_gfDraws;
}
static void GfAfter(ID3D11DeviceContext* ctx)
{
    ctx->PSSetShaderResources(g_gfSlot, 1, &g_gfSaved);
    g_gfSaved->Release();
    g_gfSlot = -1;
}
static ID3D11ShaderResourceView* g_crSaved;
static int g_crSlot = -1;
// 1.98p: Shift+U also saves the cars' cube map (the game's 512^2 view around the car): its 6 faces at mip 0 as PNGs
// (NewLook_capture\carcube_N.png): what the cars reflect
static volatile LONG g_crDumpReq;
static void CrDump(ID3D11DeviceContext* ctx, ID3D11Resource* r)
{
    ID3D11Texture2D* t = 0;
    if (!r || FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t) return;
    D3D11_TEXTURE2D_DESC td;
    t->GetDesc(&td);
    bool half = td.Format >= DXGI_FORMAT_R16G16B16A16_TYPELESS && td.Format <= DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!half || td.ArraySize < 6) { Log("car cube: format %d, %u faces: not saved", (int)td.Format, td.ArraySize); t->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* st = 0;
    if (FAILED(g_dev->CreateTexture2D(&sd, 0, &st))) { t->Release(); return; }
    char dir[MAX_PATH];
    sprintf(dir, "%sNewLook_capture", g_dir);
    CreateDirectoryA(dir, 0);
    int W = td.Width, H = td.Height;
    unsigned char* rgb = (unsigned char*)malloc(W * H * 3);
    for (int f = 0; f < 6; ++f) {
        ctx->CopySubresourceRegion(st, 0, 0, 0, 0, t, D3D11CalcSubresource(0, f, td.MipLevels), 0);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) continue;
        double mean = 0;
        for (int y = 0; y < H; ++y) {
            const unsigned short* px = (const unsigned short*)((char*)m.pData + y * m.RowPitch);
            for (int x = 0; x < W; ++x)
                for (int c = 0; c < 3; ++c) {
                    float v = HalfToFloat(px[x * 4 + c]);
                    v = v == v && v > 0 ? v : 0;
                    mean += v;
                    float o = v / (1 + v);   // (as stored: the game's tone curve not applied)
                    rgb[(y * W + x) * 3 + c] = (unsigned char)(powf(o, 1 / 2.2f) * 255 + 0.5f);
                }
        }
        ctx->Unmap(st, 0);
        char path[MAX_PATH];
        sprintf(path, "%s\\carcube_%d.png", dir, f);
        WritePng(path, rgb, W, H);
        Log("car cube: face %d saved (mean %.3f)", f, mean / (W * H * 3));
    }
    free(rgb);
    st->Release();
    t->Release();
}
static void CrBefore(ID3D11DeviceContext* ctx)
{
    if (!g_enabled || g_p[P_SKY].v < 0.5f || !g_probeSrv || !g_probeFrame || g_frame + 1 - g_probeFrame > 30) return;
    void* ps = g_lastPS;
    static void* lastPs;
    static LONG lastN = -1, lastI = -1;
    LONG n = g_crN;
    if (ps != lastPs || n != lastN) {
        lastPs = ps; lastN = n; lastI = -1;
        for (LONG i = 0; i < n; ++i) if (g_cr[i].sh == ps) { lastI = i; break; }
    }
    if (lastI < 0) return;
    int slot = g_cr[lastI].slot;
    ID3D11ShaderResourceView* v = 0;
    ctx->PSGetShaderResources(slot, 1, &v);
    if (!v) return;
    ID3D11Resource* r = 0;
    v->GetResource(&r);
    if (g_crDumpReq && InterlockedExchange(&g_crDumpReq, 0)) CrDump(ctx, r);
    static ID3D11Resource* seen[32];
    static bool ours[32];
    int j = 0;
    while (j < 32 && seen[j] && seen[j] != r) ++j;
    bool swap = false;
    if (j < 32 && seen[j] == r) swap = ours[j];
    else {
        ID3D11Texture2D* t = 0;
        if (r) r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
        D3D11_TEXTURE2D_DESC td = {};
        if (t) { t->GetDesc(&td); t->Release(); }
        swap = t && !(td.BindFlags & D3D11_BIND_RENDER_TARGET);
        if (j < 32) { seen[j] = r; ours[j] = swap; }
        Log("game's car reflections: cube %p %ux%u, %u mips, format %d, bind %x, usage %d (shader %p): %s", r, td.Width, td.Height, td.MipLevels,
            (int)td.Format, td.BindFlags, (int)td.Usage, ps, swap ? "our sky probe instead" : "kept (the game draws it)");
    }
    if (r) r->Release();
    if (!swap) { v->Release(); return; }
    ctx->PSSetShaderResources(slot, 1, &g_probeSrv);
    g_crSaved = v; g_crSlot = slot;
}
static void CrAfter(ID3D11DeviceContext* ctx)
{
    ctx->PSSetShaderResources(g_crSlot, 1, &g_crSaved);
    g_crSaved->Release();
    g_crSlot = -1;
}
static void KlAfter(ID3D11DeviceContext* ctx)
{
    ctx->PSSetConstantBuffers(g_klSlot, 1, &g_klSaved);
    g_klSaved->Release();
    g_klSlot = -1;
}
