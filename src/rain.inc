// ---------------------------------------------------------------- where rain reaches: a map of what is above, from the top
// 0.89 decided "open to the sky" by marching up in the picture: a ceiling off the screen did not count (a band of dry
// road depending on where you look), a lamp or a passing car seen above the road made dry patches.
// Now: the game's widest sun-shadow cascade has the buildings, bridges, tunnels, posts and rails around the camera
// whose shadows can reach the view (no cars). While the game draws it, its draws are drawn once more (900 a frame, in
// turn), from straight above (orthographic), into a depth map 512 x 512 m around the camera (1024^2, 0.5 m): the
// height of the highest thing at every spot. Never cleared: what was seen once (a canopy behind you) stays; when the
// camera has gone 96 m the map is shifted onto it (a pass: depth cannot be copied with an offset). A surface is open
// to the rain if nothing is above it (its height >= that height - a little).
// 1.21 (the bench: "wet ground" cost 7 ms, 6 of them this map at 900 draws a frame, all the time): the draws go in at
// 900 a frame only until every one has gone in once after the map was made or moved; then 40 a frame (objects streamed
// in later still get in, slowly). Draws that cut nothing out go in depth only (no pixel shader).
// Cars are not in it: the seats under a car's roof are found with the cars' places (traffic from carlights.inc, the
// player's from its own draws): inside a car's cabin = dry; the road under a car stays wet.

static const int kRainRes = 1024;
static const float kRainHalf = 256.0f, kRainRange = 400.0f;   // 512 m, 0.5 m texels
static ID3D11Texture2D* g_rainTex[2];
static ID3D11DepthStencilView* g_rainDsv[2];
static ID3D11ShaderResourceView* g_rainSrv[2];
static ID3D11RasterizerState* g_rainRs;
static ID3D11DepthStencilState* g_rainDsAlways;
static ID3D11VertexShader* g_rainShiftVs;
static ID3D11PixelShader* g_rainShiftPs;
static ID3D11Buffer* g_rainShiftCb;
static ID3D11Buffer* g_rainCb;                 // b13: the map's place, the cars
static int g_rainCur = -1;                     // the map (-1: none yet)
static float g_rainC[3];                       // its centre x, z, top height
static float g_rainM[16];                      // its view (the game's packed form)
static unsigned g_rainRun, g_rainIdx, g_rainLastIdx, g_rainCursor;
static bool g_rainFilling = true;              // made / moved: not every draw has gone in yet (fast); else a trickle
static unsigned g_rainBudget = 900;
struct RainCbE { ID3D11Buffer* b; int off; };
static RainCbE g_rainCbs[16];
static int g_rainCbN;
// the player's car: its world matrix from its own draws (bones), read back a few frames late
static ID3D11Buffer* g_playerStage[4];
static unsigned g_playerStageFrame[4];
static float g_playerMat[16];
static unsigned g_playerSeen;
static float g_playerMatOld[16];   // 1.59: the one before (its motion: the player's car where it is now, a frame ago)
static unsigned g_playerSeenOld;

// moving the map with the camera: the old one copied over, shifted (depth: a copy cannot shift, a pass can)
static const char* kRainShiftSrc = R"HLSL(
cbuffer S : register(b0) { float4 Sh; };   // texel shift x, y, depth shift
Texture2D<float> tOld : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }
float ps(float4 pos : SV_Position) : SV_Depth
{
    int2 p = int2(pos.xy) + int2(Sh.xy);
    if (any(p < 0) || any(p >= 1024)) return 1;
    float d = tOld.Load(int3(p, 0));
    return d >= 1 ? 1 : saturate(d + Sh.z);
}
)HLSL";

static bool RainEnsure()
{
    if (g_rainSrv[1]) return true;
    static bool failed;
    if (failed || !g_dev || !g_compile) return false;
    for (int k = 0; k < 2; ++k) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = d.Height = kRainRes; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_TYPELESS;
        d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_rainTex[k])) || FAILED(g_dev->CreateDepthStencilView(g_rainTex[k], &dv, &g_rainDsv[k])) ||
            FAILED(g_dev->CreateShaderResourceView(g_rainTex[k], &sv, &g_rainSrv[k]))) { failed = true; Log("rain map: textures failed"); return false; }
    }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rainRs);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateDepthStencilState(&dd, &g_rainDsAlways);
    for (int e = 0; e < 2; ++e) {
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(kRainShiftSrc, strlen(kRainShiftSrc), "rainshift", 0, 0, e ? "ps" : "vs", e ? "ps_4_0" : "vs_4_0", 0, 0, &code, &err))) {
            if (e) g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_rainShiftPs);
            else g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_rainShiftVs);
        } else Log("rain map shift shader: %s", err ? (const char*)err->GetBufferPointer() : "failed");
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, 0, &g_rainShiftCb);
    bd.ByteWidth = 896;   // (1.47: + RN3; 1.49: + RN4; 1.59: + RP: the player's car a frame ago)
    g_dev->CreateBuffer(&bd, 0, &g_rainCb);
    Log("rain map: %d^2, %.0f m around the camera (kept: whatever the sun cascade ever had)", kRainRes, kRainHalf * 2);
    return true;
}

// the view from above, in the game's packed form: rows x, y, depth (world), then (z scale, z bias, w scale, w bias)
static void RainMatrix(const float* c, float* m)
{
    memset(m, 0, 64);
    m[0] = 1 / kRainHalf; m[3] = -c[0] / kRainHalf;   // x
    m[6] = 1 / kRainHalf; m[7] = -c[1] / kRainHalf;   // y = world z
    m[9] = -1; m[11] = c[2];                          // depth = top - height
    m[12] = 1 / kRainRange; m[13] = 0; m[14] = 0; m[15] = 1;   // orthographic: w = 1
}

static ID3D11Buffer* RainCbFor(ID3D11DeviceContext* ctx, int off, bool fresh)
{
    for (int k = 0; k < g_rainCbN; ++k)
        if (g_rainCbs[k].off == off) {
            if (fresh) {
                static char buf[1024];
                memset(buf, 0, sizeof(buf));
                memcpy(buf + off, g_rainM, 64);
                ctx->UpdateSubresource(g_rainCbs[k].b, 0, 0, buf, 0, 0);
            }
            return g_rainCbs[k].b;
        }
    if (g_rainCbN >= 16) return 0;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 1024; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* b = 0;
    if (FAILED(g_dev->CreateBuffer(&bd, 0, &b))) return 0;
    g_rainCbs[g_rainCbN].b = b; g_rainCbs[g_rainCbN].off = off;
    ++g_rainCbN;
    return RainCbFor(ctx, off, true);
}

// the map onto a new centre (the other texture), keeping what it holds
static void RainShift(ID3D11DeviceContext* ctx, const float* nc)
{
    float t = kRainHalf * 2 / kRainRes;
    int nb = g_rainCur ^ 1;
    if (g_rainShiftVs && g_rainShiftPs && g_rainShiftCb && g_rainDsAlways) {
        // texel x grows with x, texel y grows to -z; depth = (top - h) / range: a higher top -> deeper
        float sh[4] = { (nc[0] - g_rainC[0]) / t, -(nc[1] - g_rainC[1]) / t, (nc[2] - g_rainC[2]) / kRainRange, 0 };
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_rainShiftCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, sh, 16); ctx->Unmap(g_rainShiftCb, 0); }
        // save
        ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
        ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
        D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
        ID3D11Buffer* sCb = 0; ctx->PSGetConstantBuffers(0, 1, &sCb);
        ID3D11ShaderResourceView* sSrv = 0; ctx->PSGetShaderResources(0, 1, &sSrv);
        ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
        ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
        ID3D11RenderTargetView* sRtv = 0; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(1, &sRtv, &sDsv);
        D3D11_VIEWPORT sVp; UINT nv = 1; ctx->RSGetViewports(&nv, &sVp);
        // shift
        ctx->OMSetRenderTargets(0, 0, g_rainDsv[nb]);
        D3D11_VIEWPORT vp = { 0, 0, (float)kRainRes, (float)kRainRes, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        ctx->RSSetState(g_rainRs);
        ctx->OMSetDepthStencilState(g_rainDsAlways, 0);
        ctx->IASetInputLayout(0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_rainShiftVs, 0, 0);
        oPSSetShader(ctx, g_rainShiftPs, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_rainShiftCb);
        ctx->PSSetShaderResources(0, 1, &g_rainSrv[g_rainCur]);
        oDraw(ctx, 3, 0);
        ID3D11ShaderResourceView* none = 0;
        ctx->PSSetShaderResources(0, 1, &none);
        // restore
        ctx->OMSetRenderTargets(sRtv ? 1 : 0, &sRtv, sDsv);
        ctx->RSSetViewports(nv ? 1 : 0, &sVp);
        ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
        ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
        ctx->PSSetConstantBuffers(0, 1, &sCb); ctx->PSSetShaderResources(0, 1, &sSrv);
        ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
        IUnknown* rel[] = { sVs, sPs, sIl, sCb, sSrv, sDs, sRs, sRtv, sDsv };
        for (int k = 0; k < 9; ++k) if (rel[k]) rel[k]->Release();
    } else ctx->ClearDepthStencilView(g_rainDsv[nb], D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_rainCur = nb;
    memcpy(g_rainC, nc, 12);
}

// after every game draw: the widest sun cascade -> the same draw into the map (it keeps the highest of everything ever
// drawn there: the cascade only has the casters for the present view, so a canopy seen once stays)
static void RainAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (g_p[P_SSR + 1].v <= 0 || !g_sunShadowTex) return;
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(0, 0, &dsv);
    if (!dsv) return;
    ID3D11Resource* res = 0;
    dsv->GetResource(&res);
    if (res) res->Release();
    D3D11_VIEWPORT vp; UINT nv = 1;
    ctx->RSGetViewports(&nv, &vp);
    if (res != g_sunShadowTex || !nv || vp.TopLeftY < vp.Height * 1.5f || !g_camKnown || !RainEnsure()) { dsv->Release(); return; }
    if (g_rainRun != g_frame + 1) {   // the cascade starts
        g_rainRun = g_frame + 1;
        g_rainLastIdx = g_rainIdx; g_rainIdx = 0;
        g_rainCursor += g_rainBudget;
        if (g_rainCursor >= g_rainLastIdx) { g_rainCursor = 0; g_rainFilling = false; }   // every draw went in once
        g_rainBudget = g_rainFilling ? 900 : 40;
        float t = kRainHalf * 2 / kRainRes;
        float nc[3] = { floorf(g_cam[3][0] / t) * t, floorf(g_cam[3][2] / t) * t, floorf((g_cam[3][1] + kRainRange * 0.5f) / 10) * 10 };
        bool fresh = g_rainCur < 0;
        if (fresh) {
            g_rainCur = 0;
            memcpy(g_rainC, nc, 12);
            ctx->ClearDepthStencilView(g_rainDsv[0], D3D11_CLEAR_DEPTH, 1.0f, 0);
        } else if (fabsf(nc[0] - g_rainC[0]) > 96 || fabsf(nc[1] - g_rainC[1]) > 96 || fabsf(nc[2] - g_rainC[2]) > 120) {
            RainShift(ctx, nc);
            fresh = true;
        }
        if (fresh) {
            g_rainFilling = true; g_rainCursor = 0; g_rainBudget = 900;
            RainMatrix(g_rainC, g_rainM);
            for (int k = 0; k < g_rainCbN; ++k) RainCbFor(ctx, g_rainCbs[k].off, true);
        }
    }
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    VsInfo* e = VsFind(vs, false);
    if (vs) vs->Release();
    if (!e || e->vpSlot < 0 || e->car || e->vpSlot == e->wSlot || e->vpOff + 64 > 1024) { dsv->Release(); return; }
    unsigned idx = g_rainIdx++;
    if (idx < g_rainCursor || idx >= g_rainCursor + g_rainBudget) { dsv->Release(); return; }
    ID3D11Buffer* our = RainCbFor(ctx, e->vpOff, false);
    if (!our) { dsv->Release(); return; }
    ID3D11RenderTargetView* savedRtv = 0;
    ID3D11DepthStencilView* dummy = 0;
    ctx->OMGetRenderTargets(1, &savedRtv, &dummy);
    if (dummy) dummy->Release();
    ID3D11Buffer* savedCb = 0;
    ctx->VSGetConstantBuffers(e->vpSlot, 1, &savedCb);
    ID3D11RasterizerState* savedRs = 0;
    ctx->RSGetState(&savedRs);
    ctx->VSSetConstantBuffers(e->vpSlot, 1, &our);
    ctx->OMSetRenderTargets(0, 0, g_rainDsv[g_rainCur]);
    D3D11_VIEWPORT fv = { 0, 0, (float)kRainRes, (float)kRainRes, 0, 1 };
    ctx->RSSetViewports(1, &fv);
    ctx->RSSetState(g_rainRs);
    ID3D11PixelShader* ps = 0;
    ctx->PSGetShader(&ps, 0, 0);
    bool dropPs = ps && !PsIsCut(ps);   // nothing cut out: depth only
    if (dropPs) oPSSetShader(ctx, 0, 0, 0);
    Replay(ctx, d);   // cut-outs (leaves, fences) keep the game's pixel shader: their holes stay
    if (dropPs) oPSSetShader(ctx, ps, 0, 0);
    if (ps) ps->Release();
    ctx->VSSetConstantBuffers(e->vpSlot, 1, &savedCb);
    ctx->OMSetRenderTargets(savedRtv ? 1 : 0, &savedRtv, dsv);
    if (savedRtv) savedRtv->Release();
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(savedRs);
    if (savedCb) savedCb->Release();
    if (savedRs) savedRs->Release();
    dsv->Release();
}

// the player's car (bones, not a wheel) in the main view: its world matrix into this frame's staging buffer
static void RainPlayerAfterDraw(ID3D11DeviceContext* ctx)
{
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    VsInfo* e = VsFind(vs, false);
    if (vs) vs->Release();
    if (!e || !(e->carBits & 4) || (e->carBits & 1) || e->wSlot < 0 || !MainTarget(ctx)) return;
    int k = g_frame & 3;
    if (g_playerStageFrame[k] == g_frame + 1) return;   // one a frame
    if (!g_playerStage[k]) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = 64; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateBuffer(&d, 0, &g_playerStage[k]))) return;
    }
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    if ((UINT)e->wOff + 64 <= bd.ByteWidth) {
        D3D11_BOX box = { (UINT)e->wOff, 0, 0, (UINT)e->wOff + 64, 1, 1 };
        ctx->CopySubresourceRegion(g_playerStage[k], 0, 0, 0, 0, cb, 0, &box);
        g_playerStageFrame[k] = g_frame + 1;
    }
    cb->Release();
}

// the constants for the shaders (b13) and the map (t51); once a frame, bound where wetness is used
static void RainBind(ID3D11DeviceContext* ctx)
{
    if (!g_rainCb) return;
    static unsigned filled;
    if (filled != g_frame + 1) {
        filled = g_frame + 1;
        // the player's car: the newest finished copy
        for (int k = 0; k < 4; ++k) {
            if (!g_playerStageFrame[k] || g_playerStageFrame[k] + 2 > g_frame + 1) continue;
            D3D11_MAPPED_SUBRESOURCE m;
            if (ctx->Map(g_playerStage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
                const float* f = (const float*)m.pData;
                if (g_playerStageFrame[k] > g_playerSeen && f[15] == f[15] && fabsf(f[5]) > 0.3f) { memcpy(g_playerMatOld, g_playerMat, 64); g_playerSeenOld = g_playerSeen; memcpy(g_playerMat, f, 64); g_playerSeen = g_playerStageFrame[k]; }
                ctx->Unmap(g_playerStage[k], 0);
                g_playerStageFrame[k] = 0;
            }
        }
        float c[224] = {};
        c[0] = g_rainC[0]; c[1] = g_rainC[1]; c[2] = kRainHalf; c[3] = g_rainC[2];
        c[4] = kRainRange; c[5] = g_rainCur >= 0 ? 1.0f : 0.0f;
        int n = 0;
        // the cars near the camera: the player's, then the traffic's nearest (their cabins are dry)
        if (g_playerSeen && g_frame + 1 - g_playerSeen < 30) {
            float* q = c + 12 + n * 12;
            memcpy(q, g_playerMat + 12, 12); memcpy(q + 4, g_playerMat, 12); memcpy(q + 8, g_playerMat + 8, 12);
            // 1.59: its copy is a few frames old (read back without waiting): carried on to now at its last speed (the
            // same per frame), and where it was a frame ago (RP: the frames' history on its body found by its motion)
            unsigned gap = g_playerSeen - g_playerSeenOld;
            if (g_playerSeenOld && gap >= 1 && gap <= 4) {
                float ahead = (float)(g_frame + 1 - g_playerSeen);
                float* rp = c + 212;
                bool sane = true;
                for (int r = 0; r < 3; ++r) {   // rows: x axis (0), z axis (8), place (12)
                    int o = r == 0 ? 0 : r == 1 ? 8 : 12;
                    float* now = r == 0 ? q + 4 : r == 1 ? q + 8 : q;
                    float* was = r == 0 ? rp + 4 : r == 1 ? rp + 8 : rp;
                    for (int a = 0; a < 3; ++a) {
                        float v = (g_playerMat[o + a] - g_playerMatOld[o + a]) / gap;
                        if (r == 2 && fabsf(v) > 3.0f) sane = false;   // (over 3 m a frame: a reset, a teleport)
                        now[a] = g_playerMat[o + a] + v * ahead;
                        was[a] = now[a] - v;
                    }
                }
                for (int r = 0; r < 2; ++r) {   // the axes kept unit length
                    float* ax[2] = { r ? q + 8 : q + 4, r ? rp + 8 : rp + 4 };
                    for (int k2 = 0; k2 < 2; ++k2) {
                        float l = sqrtf(ax[k2][0] * ax[k2][0] + ax[k2][1] * ax[k2][1] + ax[k2][2] * ax[k2][2]);
                        if (l > 1e-4f) for (int a = 0; a < 3; ++a) ax[k2][a] /= l;
                    }
                }
                if (sane) rp[3] = 1.0f;   // RP0.w: known
                else { memcpy(q, g_playerMat + 12, 12); memcpy(q + 4, g_playerMat, 12); memcpy(q + 8, g_playerMat + 8, 12); }
            }
            c[211] = 1.0f;   // RN4.w: RC[0] is the player's car
            ++n;
        }
        static bool used[kCars];
        memset(used, 0, sizeof(used));
        while (n < 16) {
            int best = -1;
            float bd = 80.0f * 80.0f;
            for (int t = 0; t < g_carN; ++t) {
                if (used[t] || g_frame - g_car[t].seen > 10) continue;
                float d2 = g_camKnown ? Dist2(g_car[t].p, g_cam[3]) : 0;
                if (d2 < bd) { bd = d2; best = t; }
            }
            if (best < 0) break;
            used[best] = true;
            float* q = c + 12 + n * 12;
            memcpy(q, g_car[best].p, 12); memcpy(q + 4, g_car[best].x, 12); memcpy(q + 8, g_car[best].z, 12);
            ++n;
        }
        c[6] = (float)n;
        c[8] = g_p[P_SSR + 1].v; c[9] = g_p[111].v; c[10] = g_p[112].v; c[11] = g_p[P_NOISE + 6].v;   // wetness, dry gloss kept, puddles, wet gloss (1.43)
        c[7] = kRainHalf * 2 / kRainRes;
        c[204] = g_p[P_PUDDLE_BIG].v; c[205] = g_p[P_PUDDLE_BIG + 1].v; c[206] = g_p[P_PUDDLE_BIG + 2].v;   // 1.47: big matte puddles (RN3)
        c[207] = g_p[P_PUDDLE_BIG + 4].v;   // 1.48: the water in them
        c[208] = g_p[P_PUDDLE_BIG + 5].v; c[209] = g_p[P_PUDDLE_BIG + 6].v; c[210] = g_p[P_PUDDLE_BIG + 7].v;   // 1.49: the small ones (RN4)
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_rainCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, c, 896); ctx->Unmap(g_rainCb, 0); }
    }
    ctx->PSSetConstantBuffers(13, 1, &g_rainCb);
    ID3D11ShaderResourceView* v = g_rainCur >= 0 ? g_rainSrv[g_rainCur] : 0;
    ctx->PSSetShaderResources(51, 1, &v);
}
