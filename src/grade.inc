// ---------------------------------------------------------------- the look: colour grading of the finished picture
// The game's post chain ends in one pass that adds the screen dirt (SamplerSplatter) and its colour cube; the HUD comes
// after it. Right after that pass the picture it wrote is copied (with mips: their small levels are the local and the
// whole picture's average) and PS_Grade draws over it: exposure, a film-like contrast curve, shadows / highlights,
// local contrast, haze removal, saturation / vibrance, white balance, split toning, sharpening, vignette.

static ID3D11PixelShader* g_finalPS[16];   // the game's final post passes
static ID3D11Texture2D* g_gradeTex;
static ID3D11ShaderResourceView* g_gradeSrv;
static ID3D11Buffer* g_cbGrade;
static UINT g_gradeW, g_gradeH;
static DXGI_FORMAT g_gradeFmt;
static unsigned g_gradeFrame;
static ID3D11Resource* g_gradeLastTarget;

static void NoteFinalPS(ID3D11PixelShader* ps, const char* names)
{
    if (!strstr(names, "SamplerSplatter")) return;
    for (int k = 0; k < 16; ++k) if (!g_finalPS[k]) { g_finalPS[k] = ps; Log("look: final post pass %p (%s)", ps, names); break; }
}

static bool IsFinalPS(ID3D11PixelShader* ps)
{
    if (!ps) return false;
    for (int k = 0; k < 16 && g_finalPS[k]; ++k) if (g_finalPS[k] == ps) return true;
    return false;
}

static DXGI_FORMAT Typeless(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return f;
    }
}

static bool GradeEnsure(UINT w, UINT h, DXGI_FORMAT texFmt, DXGI_FORMAT viewFmt)
{
    if (g_gradeSrv && g_gradeW == w && g_gradeH == h && g_gradeFmt == viewFmt) return true;
    if (g_gradeSrv) { g_gradeSrv->Release(); g_gradeSrv = 0; }
    if (g_gradeTex) { g_gradeTex->Release(); g_gradeTex = 0; }
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.SampleDesc.Count = 1;
    d.Format = texFmt;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_gradeTex))) { Log("look: copy texture failed (format %d)", texFmt); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = viewFmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = (UINT)-1;
    if (FAILED(g_dev->CreateShaderResourceView(g_gradeTex, &sv, &g_gradeSrv))) { Log("look: copy view failed"); g_gradeTex->Release(); g_gradeTex = 0; return false; }
    if (!g_cbGrade) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 96; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_cbGrade))) return false;
    }
    g_gradeW = w; g_gradeH = h; g_gradeFmt = viewFmt;
    Log("look: %ux%u, format %d", w, h, viewFmt);
    return true;
}

// the look presets (LookPreset): the values they set, in the order of the Look settings (exposure ... grain)
struct LookPreset { const char* name; float v[15]; float x[3]; };   // x: film curve, red saturation, highlight desaturation
static const LookPreset kLookPresets[] = {
    { "custom", {}, {} },
    // the reference shots (overcast, photographic): calm colours, neutral-cool white, bright soft highlights
    { "overcast photo", { 0.1f, 1.0f, 0.15f, 0.25f, 0.6f, 0.0f, -0.7f, -0.15f, 0.3f, 0.15f, 0.3f, 0.15f, 0.15f, 0.5f, 0.3f }, { 0, 1, 0 } },
    // the first defaults: punchy, clear
    { "clear and punchy", { 0.0f, 1.1f, 0.05f, -0.15f, 1.0f, 0.25f, 0.05f, 0.0f, 0.35f, 0.4f, 0.3f, 0.2f, 0.25f, 0.1f, 0.0f }, { 0, 1, 0 } },
    // warm evening film
    { "warm film", { 0.0f, 1.05f, 0.1f, -0.1f, 0.85f, 0.1f, 0.3f, 0.05f, 0.25f, 0.2f, 0.2f, 0.25f, 0.4f, 0.3f, 0.25f }, { 0.4f, 0.9f, 0.3f } },
    // like the DLSS5 shot: photographic tone (film curve, soft highlights, lifted shadows), calmer reds, neutral white,
    // crisp detail (CAS), little haze / glow
    // (tuned on the user's shot pair: mean / median / 90th / 99th percentile luminance and the car's colour match within ~5%)
    { "photo (DLSS-like)", { -0.3f, 1.0f, 0.3f, 0.05f, 0.85f, 0.1f, -0.08f, 0.0f, 0.25f, 0.15f, 0.5f, 0.08f, 0.08f, 0.06f, 0.03f }, { 0.4f, 0.85f, 0.15f } },
};
static const int kLookPresetCount = sizeof(kLookPresets) / sizeof(kLookPresets[0]);

static void LookApplyPreset(int k)
{
    if (k <= 0 || k >= kLookPresetCount) return;
    for (int i = 0; i < 15; ++i) g_p[P_LOOK + 1 + i].v = kLookPresets[k].v[i];
    for (int i = 0; i < 3; ++i) g_p[P_LOOK_X + i].v = kLookPresets[k].x[i];
    sprintf(g_lampMsg, "look: preset \"%s\"", kLookPresets[k].name);
}

// right after the game's final post pass drew
static void GradePass(ID3D11DeviceContext* ctx)
{
    if (!g_psGrade || !g_vsText || !g_cbText || g_p[P_LOOK].v < 0.5f || g_gradeFrame == g_frame + 1) return;
    ID3D11RenderTargetView* rtv = 0;
    ctx->OMGetRenderTargets(1, &rtv, 0);
    if (!rtv) return;
    ID3D11Resource* res = 0;
    rtv->GetResource(&res);
    ID3D11Texture2D* tex = 0;
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    D3D11_RENDER_TARGET_VIEW_DESC rd;
    rtv->GetDesc(&rd);
    D3D11_TEXTURE2D_DESC td = {};
    if (tex) tex->GetDesc(&td);
    bool ok = tex && td.SampleDesc.Count == 1 && rd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D &&
              GradeEnsure(td.Width, td.Height, Typeless(td.Format), rd.Format);
    if (ok) {
        CpuTimer cpu;
        ProfMark(ctx, PR_LOOK, 0);
        g_gradeFrame = g_frame + 1;
        g_gradeLastTarget = res;
        ctx->CopySubresourceRegion(g_gradeTex, 0, 0, 0, 0, res, rd.Texture2D.MipSlice, 0);
        ctx->GenerateMips(g_gradeSrv);
        float gc[24] = {
            g_p[P_LOOK + 1].v + g_p[P_NIGHT_EV].v * g_nightF, g_p[P_LOOK + 2].v, g_p[P_LOOK + 5].v, g_p[P_LOOK + 6].v,   // exposure (+ the night's), contrast, saturation, vibrance
            g_p[P_LOOK + 3].v, g_p[P_LOOK + 4].v, g_p[P_LOOK + 7].v, g_p[P_LOOK + 8].v,       // shadows, highlights, temperature, tint
            g_p[P_LOOK + 9].v, g_p[P_LOOK + 10].v, g_p[P_LOOK + 11].v, g_p[P_LOOK + 12].v,   // clarity, dehaze, sharpen, vignette
            g_p[P_LOOK + 13].v, log2f(td.Width / 48.0f), 1.0f / td.Width, 1.0f / td.Height,  // split toning, local average mip, texel
            g_view == 1 ? 1.0f : 0.0f, (float)(g_frame & 63), g_p[P_LOOK + 14].v, g_p[P_LOOK + 15].v,   // split screen, frame, bloom, grain
            g_p[P_LOOK_X].v, g_p[P_LOOK_X + 1].v, g_p[P_LOOK_X + 2].v,
            g_p[195].v > 0.5f ? 1 + g_p[196].v : 0.0f };   // 1.68: our anti-aliasing (FXAA), its softness                              // film curve, reds, highlight desaturation
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_cbGrade, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, gc, sizeof(gc)); ctx->Unmap(g_cbGrade, 0); }
        float rect[4] = { -1, 1, 2, 2 };
        // save
        D3D11_VIEWPORT sVp[16]; UINT nVp = 16; ctx->RSGetViewports(&nVp, sVp);
        ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
        ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
        D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
        ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
        ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
        ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
        ID3D11ShaderResourceView* sSrv = 0; ctx->PSGetShaderResources(0, 1, &sSrv);
        ID3D11SamplerState* sSmp = 0; ctx->PSGetSamplers(0, 1, &sSmp);
        ID3D11Buffer* sCb = 0; ctx->VSGetConstantBuffers(13, 1, &sCb);
        ID3D11Buffer* sPcb = 0; ctx->PSGetConstantBuffers(12, 1, &sPcb);
        ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(8, sRtv, &sDsv);
        // draw
        if (SUCCEEDED(ctx->Map(g_cbText, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, rect, 16); ctx->Unmap(g_cbText, 0); }
        D3D11_VIEWPORT vp = { 0, 0, (float)td.Width, (float)td.Height, 0, 1 };
        ctx->OMSetRenderTargets(1, &rtv, 0);
        ctx->RSSetViewports(1, &vp);
        ctx->VSSetShader(g_vsText, 0, 0);
        oPSSetShader(ctx, g_psGrade, 0, 0);
        ctx->IASetInputLayout(0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->OMSetBlendState(0, 0, 0xffffffff);
        ctx->OMSetDepthStencilState(0, 0);
        ctx->RSSetState(g_rsNoCull);
        ctx->PSSetShaderResources(0, 1, &g_gradeSrv);
        ctx->PSSetSamplers(0, 1, &g_sText);
        ctx->VSSetConstantBuffers(13, 1, &g_cbText);
        ctx->PSSetConstantBuffers(12, 1, &g_cbGrade);
        oDraw(ctx, 4, 0);
        // restore
        ID3D11ShaderResourceView* none = 0;
        ctx->PSSetShaderResources(0, 1, &none);
        ctx->OMSetRenderTargets(8, sRtv, sDsv);
        ctx->RSSetViewports(nVp, sVp);
        ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
        ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
        ctx->OMSetBlendState(sBl, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
        ctx->PSSetShaderResources(0, 1, &sSrv); ctx->PSSetSamplers(0, 1, &sSmp);
        ctx->VSSetConstantBuffers(13, 1, &sCb); ctx->PSSetConstantBuffers(12, 1, &sPcb);
        IUnknown* rel[] = { sVs, sPs, sIl, sBl, sDs, sRs, sSrv, sSmp, sCb, sPcb, sDsv };
        for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
        for (int i = 0; i < 8; ++i) if (sRtv[i]) sRtv[i]->Release();
        ProfMark(ctx, PR_LOOK, 1);
    }
    if (tex) tex->Release();
    if (res) res->Release();
    rtv->Release();
}
