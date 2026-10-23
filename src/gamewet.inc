// ---------------------------------------------------------------- the game's own wetness (GameWetness)
// The game's road / pavement / ground shaders (NewLook_wet.txt) read a "puddleMapSampler": rgb a tint, alpha where and
// how wet (times a per-vertex wetness): it darkens the albedo, flattens the normal, raises the gloss and mixes in the
// planar reflection (scaled by the same amount). It is wet everywhere the map says, under roofs too.
// GameWetness (param 110): 1 = the game's; 0 = a plain map (alpha 0: dry); between: a copy of the game's map with its
// alpha scaled (made once per map and value: a pass into an RGBA8 texture, with mips). The shader's map is swapped for
// the draw (BeforeDraw) and put back after it.

struct PuddlePs { ID3D11PixelShader* ps; int slot; };
static PuddlePs g_puddlePs[512];
static volatile LONG g_puddlePsN;
struct PuddleCopy { ID3D11ShaderResourceView* src; ID3D11ShaderResourceView* srv; ID3D11Texture2D* tex; float k; };
static PuddleCopy g_puddleCopy[32];
static int g_puddleCopyN;
static ID3D11ShaderResourceView* g_puddleDry;     // 1x1 (1, 1, 1, 0)
static ID3D11VertexShader* g_puddleVs;
static ID3D11PixelShader* g_puddlePsScale;
static ID3D11Buffer* g_puddleCb;
static ID3D11ShaderResourceView* g_puddleSaved;   // the game's map, put back after the draw
static int g_puddleSavedSlot = -1;

static const char* kPuddleSrc = R"HLSL(
cbuffer K : register(b0) { float4 Kp; };
Texture2D tSrc : register(t0);
SamplerState sL : register(s0);
float4 vs(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float4 ps(float4 pos : SV_Position) : SV_Target
{
    float2 size;
    tSrc.GetDimensions(size.x, size.y);
    float4 c = tSrc.Load(int3(pos.xy * size / Kp.yz, 0));
    return float4(c.rgb, c.a * Kp.x);
}
)HLSL";

// at creation: shaders with a puddle map (its texture slot)
static void PuddleNote(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (!g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_INPUT_BIND_DESC bd;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    int slot = -1;
    for (UINT i = 0; i < sd.BoundResources; ++i)
        if (SUCCEEDED(r->GetResourceBindingDesc(i, &bd)) && bd.Type == D3D_SIT_TEXTURE && !strcmp(bd.Name, "puddleMapSampler")) slot = (int)bd.BindPoint;
    r->Release();
    if (slot < 0) return;
    LONG k = InterlockedIncrement(&g_puddlePsN) - 1;
    if (k < 512) { g_puddlePs[k].ps = ps; g_puddlePs[k].slot = slot; } else g_puddlePsN = 512;
}
static int PuddleSlotOf(ID3D11PixelShader* ps)
{
    LONG n = g_puddlePsN;
    for (LONG i = 0; i < n; ++i) if (g_puddlePs[i].ps == ps) return g_puddlePs[i].slot;
    return -1;
}

static bool PuddleEnsure()
{
    if (g_puddleDry) return true;
    static bool failed;
    if (failed || !g_dev || !g_compile) return false;
    unsigned px = 0x00ffffff;   // (1, 1, 1, 0)
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA s = { &px, 4, 4 };
    ID3D11Texture2D* t = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, &s, &t)) || FAILED(g_dev->CreateShaderResourceView(t, 0, &g_puddleDry))) { failed = true; if (t) t->Release(); return false; }
    t->Release();
    for (int e = 0; e < 2; ++e) {
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(kPuddleSrc, strlen(kPuddleSrc), "puddle", 0, 0, e ? "ps" : "vs", e ? "ps_4_0" : "vs_4_0", 0, 0, &code, &err))) {
            if (e) g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_puddlePsScale);
            else g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_puddleVs);
        } else Log("puddle shader: %s", err ? (const char*)err->GetBufferPointer() : "failed");
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, 0, &g_puddleCb);
    return true;
}

// a copy of the game's puddle map with its alpha x k (made now, state saved and restored)
static ID3D11ShaderResourceView* PuddleScaled(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, float k)
{
    for (int i = 0; i < g_puddleCopyN; ++i)
        if (g_puddleCopy[i].src == src) {
            if (fabsf(g_puddleCopy[i].k - k) < 0.005f) return g_puddleCopy[i].srv;
            g_puddleCopy[i].srv->Release(); g_puddleCopy[i].tex->Release();
            g_puddleCopy[i] = g_puddleCopy[--g_puddleCopyN];
            break;
        }
    if (g_puddleCopyN >= 32 || !g_puddleVs || !g_puddlePsScale || !g_puddleCb) return 0;
    ID3D11Resource* r = 0;
    src->GetResource(&r);
    ID3D11Texture2D* st = 0;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&st); r->Release(); }
    if (!st) return 0;
    D3D11_TEXTURE2D_DESC sd;
    st->GetDesc(&sd);
    st->Release();
    UINT w = sd.Width > 1024 ? 1024 : sd.Width, h = sd.Height > 1024 ? 1024 : sd.Height;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET; d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* t = 0;
    ID3D11RenderTargetView* rtv = 0;
    ID3D11ShaderResourceView* srv = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, 0, &t)) || FAILED(g_dev->CreateRenderTargetView(t, 0, &rtv)) || FAILED(g_dev->CreateShaderResourceView(t, 0, &srv))) {
        if (rtv) rtv->Release();
        if (t) t->Release();
        return 0;
    }
    float kp[4] = { k, (float)w, (float)h, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_puddleCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, kp, 16); ctx->Unmap(g_puddleCb, 0); }
    // save
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11Buffer* sCb = 0; ctx->PSGetConstantBuffers(0, 1, &sCb);
    ID3D11ShaderResourceView* sSrv = 0; ctx->PSGetShaderResources(0, 1, &sSrv);
    ID3D11BlendState* sBs = 0; float sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBs, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(8, sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nv = 16; ctx->RSGetViewports(&nv, sVp);
    // the copy
    ctx->OMSetRenderTargets(1, &rtv, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rsNoCull);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_puddleVs, 0, 0);
    oPSSetShader(ctx, g_puddlePsScale, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_puddleCb);
    ctx->PSSetShaderResources(0, 1, &src);
    oDraw(ctx, 3, 0);
    ID3D11RenderTargetView* nr = 0;
    ctx->OMSetRenderTargets(1, &nr, 0);
    ctx->GenerateMips(srv);
    // restore
    ctx->OMSetRenderTargets(8, sRtv, sDsv);
    ctx->RSSetViewports(nv, sVp);
    ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    ctx->PSSetConstantBuffers(0, 1, &sCb); ctx->PSSetShaderResources(0, 1, &sSrv);
    ctx->OMSetBlendState(sBs, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
    IUnknown* rel[] = { sVs, sPs, sIl, sCb, sSrv, sBs, sDs, sRs, sDsv };
    for (int i = 0; i < 9; ++i) if (rel[i]) rel[i]->Release();
    for (int i = 0; i < 8; ++i) if (sRtv[i]) sRtv[i]->Release();
    rtv->Release();
    PuddleCopy& c = g_puddleCopy[g_puddleCopyN++];
    c.src = src; c.src->AddRef(); c.srv = srv; c.tex = t; c.k = k;
    Log("game wetness: puddle map %p copied at %.2f (%ux%u)", src, k, w, h);
    return srv;
}

// before a draw: a road / ground shader with a puddle map -> ours (none at 0, a scaled copy between)
static void GameWetBefore(ID3D11DeviceContext* ctx)
{
    float k = g_p[110].v;
    if (!g_enabled || k >= 0.999f || g_puddlePsN == 0) return;
    int slot = PuddleSlotOf(g_lastPS);
    if (slot < 0 || !PuddleEnsure()) return;
    ID3D11ShaderResourceView* cur = 0;
    ctx->PSGetShaderResources(slot, 1, &cur);
    if (!cur) return;
    ID3D11ShaderResourceView* ours = k <= 0.001f ? g_puddleDry : PuddleScaled(ctx, cur, k);
    if (ours) {
        g_puddleSaved = cur; g_puddleSavedSlot = slot;   // (the reference kept until put back)
        ctx->PSSetShaderResources(slot, 1, &ours);
    } else cur->Release();
}
// the game's gloss multiplied before its lighting (PS_WetGloss, drawn with the sky dome into the normal target's alpha)
static ID3D11RenderTargetView* g_glossRtv;
static ID3D11Resource* g_glossRes;
static ID3D11BlendState *g_glossBlend, *g_glossBlendAdd;
static void RainBind(ID3D11DeviceContext* ctx);
static void WetGlossPass(ID3D11DeviceContext* ctx, const DrawCall& d, UINT W, UINT H, ID3D11ShaderResourceView* vp)
{
    if (!g_psWetGloss || !g_psWetGlossAdd || !g_air.normal || !g_air.depth || !vp || g_p[P_SSR + 1].v <= 0 || g_p[111].v >= 0.999f) return;
    ID3D11Resource* r = 0;
    g_air.normal->GetResource(&r);
    if (!r) return;
    if (r != g_glossRes) {
        if (g_glossRtv) g_glossRtv->Release();
        g_glossRtv = 0; g_glossRes = r;
        // the texture is typeless: the target view takes the format the game reads it with
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        g_air.normal->GetDesc(&sd);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = sd.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; rd.Texture2D.MipSlice = 0;
        ID3D11Texture2D* tx = 0;
        r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tx);
        D3D11_TEXTURE2D_DESC td = {};
        if (tx) { tx->GetDesc(&td); tx->Release(); }
        HRESULT hr = g_dev->CreateRenderTargetView(r, &rd, &g_glossRtv);
        if (FAILED(hr)) { g_glossRtv = 0; Log("gloss fix: no target view on the normal buffer (%08lx, format %d / texture %d, bind %x, %ux%u)", hr, sd.Format, td.Format, td.BindFlags, td.Width, td.Height); }
        else Log("gloss fix: on the normal buffer (format %d, %ux%u)", sd.Format, td.Width, td.Height);
    }
    r->Release();
    if (!g_glossRtv) return;
    if (!g_glossBlend) {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;   // the gloss only: x what we return
        g_dev->CreateBlendState(&bd, &g_glossBlend);
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;   // + what we return
        g_dev->CreateBlendState(&bd, &g_glossBlendAdd);
    }
    // the game's own textures (its lighting may have bound the G-buffer already): binding the normal buffer as a
    // target unbinds it as a texture; they are put back after
    ID3D11ShaderResourceView* saved[16] = {};
    ctx->PSGetShaderResources(0, 16, saved);
    RainBind(ctx);
    ctx->OMSetRenderTargets(1, &g_glossRtv, 0);
    D3D11_VIEWPORT v = { 0, 0, (float)W, (float)H, 0, 1 };
    ctx->RSSetViewports(1, &v);
    ctx->OMSetBlendState(g_glossBlend, 0, 0xffffffff);
    ID3D11ShaderResourceView* in[4] = { vp, g_air.depth, g_air.spec, g_air.diffuse };
    ctx->PSSetShaderResources(52, 4, in);
    oPSSetShader(ctx, g_psWetGloss, 0, 0);      // the game's gloss x a
    Replay(ctx, d);
    ctx->OMSetBlendState(g_glossBlendAdd, 0, 0xffffffff);
    oPSSetShader(ctx, g_psWetGlossAdd, 0, 0);   // + b (our even wet gloss)
    Replay(ctx, d);
    ID3D11ShaderResourceView* none[4] = {};
    ctx->PSSetShaderResources(52, 4, none);   // (the depth goes back to being the depth target)
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ID3D11RenderTargetView* nr = 0;
    ctx->OMSetRenderTargets(1, &nr, 0);
    ctx->PSSetShaderResources(0, 16, saved);
    for (int k = 0; k < 16; ++k) if (saved[k]) saved[k]->Release();
}

static void GameWetAfter(ID3D11DeviceContext* ctx)
{
    if (g_puddleSavedSlot < 0) return;
    ctx->PSSetShaderResources(g_puddleSavedSlot, 1, &g_puddleSaved);
    g_puddleSaved->Release();
    g_puddleSaved = 0; g_puddleSavedSlot = -1;
}
