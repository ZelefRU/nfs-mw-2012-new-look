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
// 1.81: 16 copies in a ring (4 were: with the GPU 3-4 frames behind the CPU, a copy was written over before it could
// be read: the car "lost" for hundreds of frames, as the scene's load went)
static const int kPlayerStages = 16;
static ID3D11Buffer* g_playerStage[kPlayerStages];
static unsigned g_playerStageFrame[kPlayerStages];
static ID3D11Query* g_playerStageQ[kPlayerStages];   // 1.81c: an event after that frame's copies (asked without a flush)
static bool g_playerStageQd[kPlayerStages];
static double g_playerCpuRb;                         // 1.81c: (the log) the read-back's CPU time, the most in a frame
static int g_playerStageN[kPlayerStages];      // 1.77: car draws (bones, not a wheel) copied that frame: up to 64 candidates (1.81; 32 were full)
static unsigned long long g_playerStageKey[kPlayerStages][64];   // 1.78: each one's mesh (index / vertex buffers and range)
static unsigned g_playerStageCnt[kPlayerStages][64];
static float g_playerNowT[3];                        // 1.81: its part's place this frame (our copy of its constants)
static unsigned g_playerNowTag;             // 1.81: its size (indices / vertices drawn)
static unsigned g_playerKeyDrawn;                    // 1.81: the frame (+1) the player's part was last drawn
static unsigned g_playerSwitches, g_playerNoGpu;     // 1.81: (the log) its part changed; frames without its matrix on the GPU
static unsigned long long g_playerKey;               // the player's part (picked by RainBind), known by its mesh
static ID3D11Buffer* g_playerGpu;                    // its matrix this frame, copied on the GPU (t77)
static ID3D11ShaderResourceView* g_playerGpuSrv;
static unsigned g_playerReads, g_playerCands, g_playerRejects;   // (the log, now and then)
static float g_playerPickD;
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
    ID3D11DepthStencilView* dsv = 0;   // (1.65: asked once a draw, not held: bound for the draw)
    ID3D11Resource* res = DrawDsv(ctx, &dsv);
    if (!dsv) return;
    D3D11_VIEWPORT vp; UINT nv = 1;
    ctx->RSGetViewports(&nv, &vp);
    if (res != g_sunShadowTex || !nv || vp.TopLeftY < vp.Height * 1.5f || !g_camKnown || !RainEnsure()) { return; }
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
    VsInfo* e = DrawVs(ctx);   // (1.65: asked once a draw)
    if (!e || e->vpSlot < 0 || e->car || e->vpSlot == e->wSlot || e->vpOff + 64 > 1024) { return; }
    unsigned idx = g_rainIdx++;
    if (idx < g_rainCursor || idx >= g_rainCursor + g_rainBudget) { return; }
    ID3D11Buffer* our = RainCbFor(ctx, e->vpOff, false);
    if (!our) { return; }
    ID3D11PixelShader* ps = 0;
    ctx->PSGetShader(&ps, 0, 0);
    bool dropPs = ps && !PsIsCut(ps);   // nothing cut out: depth only
    // 1.65: recorded, drawn after the game's pass with the shadow replays (target 9): no depth target switch here (900
    // draws a frame while the map fills: ~16 ms of GPU in switches each time the map moved)
    if (SsRecord(ctx, e, d, 1 << 9, dropPs ? 0 : ps)) { if (ps) ps->Release(); return; }
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
}

// 1.84: the frame's car draws (bones, not a wheel) with their world matrices from our copy of the game's constants: the
// player's car found from these in the same frame (RainFrame), no read-back (the read-back's prediction drifted while
// copies came late: the car "lost", its pose off: its own shading, the dark patch under it, the drops all off)
static float g_pcW[64][16];
static unsigned long long g_pcKey[64];
static unsigned g_pcCnt[64];
static int g_pcN;
static unsigned g_pcFrame, g_playerCpuPicks, g_playerGroupN;
// 1.90: where the player's car sits in the camera's view (this frame's view-projection, from a car draw's own constants:
// no lag): its place in clip x, y and w (~ metres to the side, up, ahead), learnt while it is followed. A car that is
// lost (a car changed at a jack spot, a race's start, a teleport) is found again by that place (1.84 took the car
// nearest the camera: a parked car, a rival), and for 10 s after that a car steadily there replaces a wrong pick.
static float g_pcVP[16];
static unsigned g_pcVPFrame;
static float g_plSpot[3] = { 0, -2.2f, 6.0f };   // (a chase camera's, until learnt: ~1.3 m under it, 6 m ahead)
static unsigned g_plSpotN, g_plAcq, g_plRepick, g_plRepicks, g_plByKey, g_plBySpot, g_plToRoot;
static float g_plSpotErr = -1;
static bool PcSpot(const float* p, float* s)
{
    const float* r = g_pcVP;
    float d = r[8] * p[0] + r[9] * p[1] + r[10] * p[2] + r[11];
    s[0] = r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3];
    s[1] = r[4] * p[0] + r[5] * p[1] + r[6] * p[2] + r[7];
    s[2] = d * r[14] + r[15];
    return s[0] == s[0] && s[2] == s[2];
}
static float SpotErr(const float* s)
{
    float dx = s[0] - g_plSpot[0], dy = s[1] - g_plSpot[1], dw = s[2] - g_plSpot[2];
    return sqrtf((dx * dx + dy * dy) * 0.6f + dw * dw);
}
static void PlayerGpuEnsure()
{
    if (g_playerGpu) return;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = 64; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = 4;
    if (FAILED(g_dev->CreateBuffer(&d, 0, &g_playerGpu)) || FAILED(g_dev->CreateShaderResourceView(g_playerGpu, &sv, &g_playerGpuSrv))) {
        if (g_playerGpu) g_playerGpu->Release();
        g_playerGpu = 0; g_playerGpuSrv = 0;
    }
}
static bool MatSame(const float* a, const float* b)
{
    for (int i = 0; i < 15; ++i) if (fabsf(a[i] - b[i]) > 1e-3f * (1 + fabsf(a[i]))) return false;
    return true;
}
static void CarMapRecord(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned long long key);   // (carrain.inc)
static void CarGlassAfterDraw(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d);                      // (carrain.inc)
// the player's car (bones, not a wheel) in the main view: its world matrix into this frame's staging buffer
static void RainPlayerAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& dc)
{
    VsInfo* e = DrawVs(ctx);   // (1.65: asked once a draw)
    if (!e || !(e->carBits & 4) || (e->carBits & 1) || e->wSlot < 0 || !MainTarget(ctx)) return;
    // 1.77: every such draw of the frame (up to 32), not the first only: a traffic car near the camera drawn first was
    // taken for the player's (its drops, its shading and the rest went to that car); RainBind picks the one that is the
    // player's by where it was
    // 1.78: the draw's mesh: the player's part is known by it (1.77 took the candidate nearest to where the car was:
    // the car's parts have their own matrices (doors, bumpers: other origins): it jumped between parts, the car lost)
    unsigned long long key = 0;
    {
        ID3D11Buffer *ib = 0, *vb = 0; DXGI_FORMAT f; UINT io = 0, st = 0, vo = 0;
        ctx->IAGetIndexBuffer(&ib, &f, &io);
        ctx->IAGetVertexBuffers(0, 1, &vb, &st, &vo);
        key = ((unsigned long long)(size_t)ib * 0x9E3779B1u) ^ ((unsigned long long)(size_t)vb << 21) ^ ((unsigned long long)io << 7) ^ vo ^
              ((unsigned long long)dc.a << 40) ^ ((unsigned long long)dc.b << 13) ^ (unsigned long long)(unsigned)dc.base;
        if (ib) ib->Release();
        if (vb) vb->Release();
        if (!key) key = 1;
    }
    {   // 1.84: its world matrix now (our copy of the constants): the player's car is found from these (RainFrame)
        ID3D11Buffer* wcb = 0;
        ctx->VSGetConstantBuffers(e->wSlot, 1, &wcb);
        const unsigned char* data = 0;
        UINT size = 0;
        if (wcb && WeCbNow(wcb, &data, &size) == 1 && (UINT)e->wOff + 64 <= size) {
            if (g_pcFrame != g_frame + 1) { g_pcFrame = g_frame + 1; g_pcN = 0; }
            if (g_pcN < 64) { memcpy(g_pcW[g_pcN], data + e->wOff, 64); g_pcKey[g_pcN] = key; g_pcCnt[g_pcN] = dc.a; ++g_pcN; }
        }
        if (wcb) wcb->Release();
        if (g_pcVPFrame != g_frame + 1 && e->vpSlot >= 0 && !(e->vpOff & 15)) {   // 1.90: the main view's view-projection, once a frame
            ID3D11Buffer* vcb = 0;
            ctx->VSGetConstantBuffers(e->vpSlot, 1, &vcb);
            const unsigned char* vd = 0;
            UINT vs = 0;
            if (vcb && WeCbNow(vcb, &vd, &vs) == 1 && (UINT)e->vpOff + 64 <= vs) { memcpy(g_pcVP, vd + e->vpOff, 64); g_pcVPFrame = g_frame + 1; }
            if (vcb) vcb->Release();
        }
    }
    CarMapRecord(ctx, e, dc, key);   // 1.80: its shape (carrain.inc), now and then
    if (key == g_playerKey) {
        g_playerKeyDrawn = g_frame + 1;
        ID3D11Buffer* wcb = 0;
        ctx->VSGetConstantBuffers(e->wSlot, 1, &wcb);
        const unsigned char* data = 0;
        UINT size = 0;
        if (wcb && WeCbNow(wcb, &data, &size) == 1 && (UINT)e->wOff + 64 <= size) { memcpy(g_playerNowT, data + e->wOff + 48, 12); g_playerNowTag = g_frame + 1; }
        if (wcb) wcb->Release();
    }
    CarGlassAfterDraw(ctx, e, dc);   // 1.81: the drops on its glass (carrain.inc)
    if (key == g_playerKey && g_playerGpuFrame != g_frame + 1) {   // this frame's matrix of the player's part: for the shaders now
        if (!g_playerGpu) {
            D3D11_BUFFER_DESC d = {};
            d.ByteWidth = 64; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
            sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = 4;
            if (FAILED(g_dev->CreateBuffer(&d, 0, &g_playerGpu)) || FAILED(g_dev->CreateShaderResourceView(g_playerGpu, &sv, &g_playerGpuSrv))) {
                if (g_playerGpu) g_playerGpu->Release();
                g_playerGpu = 0; g_playerGpuSrv = 0;
            }
        }
        ID3D11Buffer* cb = 0;
        ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
        if (cb && g_playerGpu) {
            D3D11_BUFFER_DESC bd;
            cb->GetDesc(&bd);
            if ((UINT)e->wOff + 64 <= bd.ByteWidth) {
                D3D11_BOX box = { (UINT)e->wOff, 0, 0, (UINT)e->wOff + 64, 1, 1 };
                ctx->CopySubresourceRegion(g_playerGpu, 0, 0, 0, 0, cb, 0, &box);
                g_playerGpuFrame = g_frame + 1;
            }
        }
        if (cb) cb->Release();
    }
    int k = g_frame % kPlayerStages;
    if (g_playerStageFrame[k] != g_frame + 1) { g_playerStageFrame[k] = g_frame + 1; g_playerStageN[k] = 0; g_playerStageQd[k] = false; }
    if (g_playerStageN[k] >= 64) return;
    g_playerStageKey[k][g_playerStageN[k]] = key;
    g_playerStageCnt[k][g_playerStageN[k]] = dc.a;
    if (!g_playerStage[k]) {
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = 64 * 64; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateBuffer(&d, 0, &g_playerStage[k]))) return;
    }
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    if ((UINT)e->wOff + 64 <= bd.ByteWidth) {
        D3D11_BOX box = { (UINT)e->wOff, 0, 0, (UINT)e->wOff + 64, 1, 1 };
        ctx->CopySubresourceRegion(g_playerStage[k], 0, 64 * g_playerStageN[k], 0, 0, cb, 0, &box);
        ++g_playerStageN[k];
    }
    cb->Release();
}

// the player's car this frame (1.84: from its draws' constants; else the read-back), the constants for the shaders (b13);
// once a frame: before the sun pass's constants (newlook.cpp) and wherever the wetness binds them first
static void RainFrame(ID3D11DeviceContext* ctx)
{
    if (!g_rainCb) return;
    static unsigned filled;
    if (filled != g_frame + 1) {
        filled = g_frame + 1;
        // 1.84: the player's car from this frame's car draws: their matrices grouped (a car's parts on its root share
        // one); near where it was (carried on at its speed), the group with the most draws (then the most drawn, then
        // the nearest); lost a second: the group with the most draws nearest the camera. Its root: the car's pose of
        // now, for the shaders too (t77)
        if (g_pcFrame == g_frame + 1 && g_pcN > 0) {
            int grp[64], gN[64] = {};
            unsigned gC[64] = {};
            for (int i = 0; i < g_pcN; ++i) {
                grp[i] = i;
                for (int j = 0; j < i; ++j) if (grp[j] == j && MatSame(g_pcW[i], g_pcW[j])) { grp[i] = j; break; }
                ++gN[grp[i]]; gC[grp[i]] += g_pcCnt[i];
            }
            bool locked = g_playerSeen && g_frame + 1 - g_playerSeen < 60;
            float pred[3] = { g_playerMat[12], g_playerMat[13], g_playerMat[14] };
            if (locked && g_playerSeenOld && g_playerSeen > g_playerSeenOld) {
                float ahead = (float)(g_frame + 1 - g_playerSeen) / (float)(g_playerSeen - g_playerSeenOld);
                for (int j = 0; j < 3; ++j) pred[j] += (g_playerMat[12 + j] - g_playerMatOld[12 + j]) * ahead;
            }
            float reach = 3.0f + 0.5f * (float)(g_frame + 1 - g_playerSeen);
            // 1.90: each group's mesh (its biggest draw) and its place in the view
            bool vpOk = g_pcVPFrame == g_frame + 1;
            bool spotKnown = vpOk && g_plSpotN >= 60;
            unsigned long long gKey[64];
            float gSpot[64][3], gErr[64];
            bool hasKey[64] = {};
            for (int i = 0; i < g_pcN; ++i) {
                if (grp[i] != i) continue;
                int km = i;
                for (int j = i; j < g_pcN; ++j) if (grp[j] == i && g_pcCnt[j] > g_pcCnt[km]) km = j;
                gKey[i] = g_pcKey[km];
                gErr[i] = vpOk && PcSpot(g_pcW[i] + 12, gSpot[i]) ? SpotErr(gSpot[i]) : 1e9f;
            }
            // (1.91: only a group of 3+ draws holds by its mesh: 1.90 held on a part with its own matrix, a door, a wheel:
            // the car's underside and its boxes then off the car)
            for (int i = 0; i < g_pcN; ++i) if (g_playerKey && g_pcKey[i] == g_playerKey && gN[grp[i]] >= 3) hasKey[grp[i]] = true;
            // locked: its own mesh near where it should be (or anywhere: a teleport); else nearest there (its own parts: a
            // door, a bumper have their own matrices: the group with more draws when about as near); lost: the car at its
            // place in the view (not known yet: the one nearest the camera with the most draws, as 1.84)
            int best = -1;
            float bs = 1e30f, bd = 0;
            bool byKey = false;
            for (int i = 0; i < g_pcN; ++i) {
                if (grp[i] != i) continue;
                const float* f = g_pcW[i];
                if (!(f[15] == f[15]) || fabsf(f[5]) <= 0.3f) continue;
                float d2 = Dist2(f + 12, pred), sc;
                if (locked && hasKey[i]) sc = d2 - 1e12f;
                else if (locked) {
                    if (d2 > reach * reach) continue;
                    sc = d2 - 0.6f * (float)(gN[i] < 10 ? gN[i] : 10);
                } else if (vpOk && gErr[i] < 1e8f) sc = gErr[i] * gErr[i] + (gN[i] < 3 ? 100.0f : 0.0f);
                else sc = (g_camKnown ? Dist2(f + 12, g_cam[3]) : 0.0f) - 100.0f * (float)(gN[i] < 10 ? gN[i] : 10);
                if (sc < bs) { bs = sc; best = i; bd = d2; byKey = locked && hasKey[i]; }
            }
            if (best >= 0 && !locked) { g_plAcq = g_frame + 1; g_plRepick = 0; Log("player car: found again (%s; %d car draws)", vpOk ? (spotKnown ? "by its place in the view" : "by a chase camera's place, not learnt yet") : "the nearest the camera", g_pcN); }
            // a wrong pick (within 10 s of finding it again): another car sitting steadily at its place in the view while
            // the picked one is far from it: that one, after 20 frames
            if (best >= 0 && spotKnown && g_plAcq && g_frame + 1 - g_plAcq < 600 && gErr[best] < 1e8f) {
                int alt = -1;
                for (int i = 0; i < g_pcN; ++i)
                    if (grp[i] == i && i != best && gN[i] >= 3 && Dist2(g_pcW[i] + 12, g_pcW[best] + 12) > 9.0f && (alt < 0 || gErr[i] < gErr[alt])) alt = i;
                if (alt >= 0 && gErr[best] > 2.0f && gErr[alt] < 0.8f) {
                    if (++g_plRepick >= 20) {
                        Log("player car: another car at its place in the view (%.1f m off it, the picked one %.1f m): taken", gErr[alt], gErr[best]);
                        best = alt; byKey = false; g_plRepick = 0; ++g_plRepicks; g_plAcq = g_frame + 1;
                    }
                } else g_plRepick = 0;
            }
            // 1.91: a part picked (its own matrix, under 3 draws): the car's root, the nearest group of 3+ draws within 2 m
            // (not the one with the most: a rival rubbing its side is as near)
            if (best >= 0 && gN[best] < 3) {
                int root = -1;
                float rd = 4.0f;
                for (int i = 0; i < g_pcN; ++i) {
                    if (grp[i] != i || gN[i] < 3 || fabsf(g_pcW[i][5]) <= 0.3f) continue;
                    float d = Dist2(g_pcW[i] + 12, g_pcW[best] + 12);
                    if (d < rd) { rd = d; root = i; }
                }
                if (root >= 0) { ++g_plToRoot; best = root; byKey = false; bd = Dist2(g_pcW[best] + 12, pred); }
            }
            if (best >= 0) {
                if (byKey) ++g_plByKey; else if (vpOk && !locked) ++g_plBySpot;
                // its place in the view learnt while followed (near the known one: quickly; far, with no other car there:
                // slowly: a new camera)
                if (gErr[best] < 1e8f && gN[best] >= 3) {   // (1.91: not from a part)
                    float e = g_plSpotN >= 60 ? gErr[best] : 0.0f;
                    float alt = 1e9f;
                    for (int i = 0; i < g_pcN; ++i) if (grp[i] == i && i != best && Dist2(g_pcW[i] + 12, g_pcW[best] + 12) > 9.0f && gErr[i] < alt) alt = gErr[i];
                    float k = e < 2.0f ? 0.05f : alt > 2.5f ? 0.01f : 0.0f;
                    if (locked || g_plSpotN < 60) {
                        for (int j = 0; j < 3; ++j) g_plSpot[j] += (gSpot[best][j] - g_plSpot[j]) * (g_plSpotN ? k : 1.0f);
                        ++g_plSpotN;
                    }
                    g_plSpotErr = gErr[best];
                }
                int km = best;
                for (int i = 0; i < g_pcN; ++i) if (grp[i] == best && g_pcCnt[i] > g_pcCnt[km]) km = i;
                // (1.91: its mesh only from the car's root: a part's would hold the pick on that part)
                if (gN[best] >= 3) {
                    if (g_playerKey != g_pcKey[km]) { ++g_playerSwitches; g_plAcq = g_frame + 1; }   // (1.90: a new part / car: a wrong one may be replaced for 10 s)
                    g_playerKey = g_pcKey[km];
                    g_playerKeyDrawn = g_frame + 1;
                }
                memcpy(g_playerMatOld, g_playerMat, 64); g_playerSeenOld = g_playerSeen;
                memcpy(g_playerMat, g_pcW[best], 64); g_playerSeen = g_frame + 1;
                memcpy(g_playerNowT, g_pcW[best] + 12, 12); g_playerNowTag = g_frame + 1;
                g_playerPickD = locked ? sqrtf(bd) : -1;
                g_playerGroupN = gN[best];
                ++g_playerCpuPicks;
                PlayerGpuEnsure();
                if (g_playerGpu) { ctx->UpdateSubresource(g_playerGpu, 0, 0, g_pcW[best], 0, 0); g_playerGpuFrame = g_frame + 1; }
            }
        }
        // the player's car: the newest finished copy. 1.81c: this frame's copies (its car draws in the main view are done
        // by now) get an event; of the copies the GPU has done (the event asked without a flush) only the newest is
        // mapped, the older ones dropped (1.81b mapped every copy 2+ frames old without waiting: a busy one failed, and
        // made the driver flush mid-frame, 2-3 times a frame)
        LARGE_INTEGER rb0; QueryPerformanceCounter(&rb0);
        {
            int kc = g_frame % kPlayerStages;
            if (g_playerStageFrame[kc] == g_frame + 1 && !g_playerStageQd[kc]) {
                if (!g_playerStageQ[kc]) { D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 }; if (FAILED(g_dev->CreateQuery(&qd, &g_playerStageQ[kc]))) g_playerStageQ[kc] = 0; }
                if (g_playerStageQ[kc]) { ctx->End(g_playerStageQ[kc]); g_playerStageQd[kc] = true; }
            }
        }
        int newest = -1;
        bool done[kPlayerStages] = {};
        for (int k = 0; k < kPlayerStages; ++k) {
            if (!g_playerStageFrame[k] || !g_playerStageQd[k] || g_playerStageFrame[k] >= g_frame + 1) continue;
            done[k] = ctx->GetData(g_playerStageQ[k], 0, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (done[k] && (newest < 0 || g_playerStageFrame[k] > g_playerStageFrame[newest])) newest = k;
        }
        for (int k = 0; k < kPlayerStages; ++k) {
            if (!done[k]) continue;
            if (k != newest) { g_playerStageFrame[k] = 0; g_playerStageQd[k] = false; continue; }
            D3D11_MAPPED_SUBRESOURCE m;
            if (ctx->Map(g_playerStage[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
                // 1.78: the player's among the frame's car draws: its own part (the same mesh as before) near where the car
                // should be now (its last place carried on at its speed: within 2.5 m); lost for a second: its part
                // anywhere, else the frame's first car draw (as before 1.77)
                bool locked = g_playerSeen && g_frame + 1 - g_playerSeen < 60;
                float pred[3] = { g_playerMat[12], g_playerMat[13], g_playerMat[14] };
                if (locked && g_playerSeenOld && g_playerSeen > g_playerSeenOld) {
                    float ahead = (float)(g_playerStageFrame[k] - g_playerSeen) / (float)(g_playerSeen - g_playerSeenOld);
                    for (int j = 0; j < 3; ++j) pred[j] += (g_playerMat[12 + j] - g_playerMatOld[12 + j]) * ahead;
                }
                // 1.81: its part kept while that is drawn (a frame it was not among the candidates, or a little off
                // where it should be, had another part taken: a door, a bumper, with their own origins: the drops, the
                // shape, all off, "the car lost"); a new part only when it is no longer drawn: the biggest mesh there
                // (the body), not the nearest
                bool keyAlive = g_playerKey && g_playerKeyDrawn && g_frame + 1 - g_playerKeyDrawn < 20;
                int best = -1;
                float bd = 1e30f, bs = 1e30f;
                for (int i = 0; i < g_playerStageN[k]; ++i) {
                    const float* f = (const float*)m.pData + i * 16;
                    if (!(f[15] == f[15]) || fabsf(f[5]) <= 0.3f) continue;
                    bool same = g_playerStageKey[k][i] == g_playerKey;
                    float d = Dist2(f + 12, pred), sc;
                    if (locked) {
                        if (d > (same ? 6.0f * 6.0f : 2.5f * 2.5f) || (!same && keyAlive)) continue;
                        sc = same ? d - 1e9f : -(float)g_playerStageCnt[k][i];
                    } else sc = same ? -1.0f : (float)i;
                    if (sc < bs) { bs = sc; bd = d; best = i; }
                }
                ++g_playerReads; g_playerCands += g_playerStageN[k];
                if (best >= 0 && g_playerStageFrame[k] > g_playerSeen) {
                    const float* f = (const float*)m.pData + best * 16;
                    if (g_playerKey != g_playerStageKey[k][best]) ++g_playerSwitches;
                    g_playerKey = g_playerStageKey[k][best];
                    memcpy(g_playerMatOld, g_playerMat, 64); g_playerSeenOld = g_playerSeen; memcpy(g_playerMat, f, 64); g_playerSeen = g_playerStageFrame[k];
                    g_playerPickD = locked ? sqrtf(bd) : -1;
                } else if (best >= 0) ++g_playerRejects;
                if (g_frame % 900 == 0 && g_playerReads) {
                    Log("player car: %.1f car draws a frame, the last pick %.2f m from where it should be, frames with none near %u; seen %u frames ago; "
                        "its matrix on the GPU this frame %d; its part changed %u times, frames without its matrix %u; found from our constants' copies "
                        "%u times (its group: %u draws); 1.90: by its mesh %u, by its place in the view %u, wrong picks replaced %u, a part's pick moved to its car's root %u; its place in the view "
                        "%.2f %.2f %.2f (now %.2f off)", (float)g_playerCands / g_playerReads, g_playerPickD, g_playerRejects, g_frame + 1 - g_playerSeen,
                        g_playerGpuFrame == g_frame + 1, g_playerSwitches, g_playerNoGpu, g_playerCpuPicks, g_playerGroupN, g_plByKey, g_plBySpot, g_plRepicks, g_plToRoot,
                        g_plSpot[0], g_plSpot[1], g_plSpot[2], g_plSpotErr);
                    g_playerReads = g_playerCands = g_playerRejects = g_playerSwitches = g_playerNoGpu = g_playerCpuPicks = 0;
                    g_plByKey = g_plBySpot = g_plRepicks = g_plToRoot = 0;
                }
                ctx->Unmap(g_playerStage[k], 0);
                g_playerStageFrame[k] = 0;
                g_playerStageQd[k] = false;
            }
        }
        {
            LARGE_INTEGER rb1; QueryPerformanceCounter(&rb1);
            double ms = g_qpf.QuadPart ? (rb1.QuadPart - rb0.QuadPart) * 1000.0 / g_qpf.QuadPart : 0;
            if (ms > g_playerCpuRb) g_playerCpuRb = ms;
        }
        if (g_playerSeen && g_playerGpuFrame != g_frame + 1) ++g_playerNoGpu;
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
}
// the constants (b13) and the map (t51), bound where wetness is used
static void RainBind(ID3D11DeviceContext* ctx)
{
    if (!g_rainCb) return;
    RainFrame(ctx);
    ctx->PSSetConstantBuffers(13, 1, &g_rainCb);
    ID3D11ShaderResourceView* v = g_rainCur >= 0 ? g_rainSrv[g_rainCur] : 0;
    ctx->PSSetShaderResources(51, 1, &v);
    ID3D11ShaderResourceView* pm = g_playerGpuFrame == g_frame + 1 ? g_playerGpuSrv : 0;   // 1.78: t77
    ctx->PSSetShaderResources(77, 1, &pm);
}
