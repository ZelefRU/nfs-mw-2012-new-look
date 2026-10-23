// ---------------------------------------------------------------- lamp shadow maps (the game's own shadow casters)
// The game draws its sun shadow map every frame: three cascades in one atlas, the last (widest) one with every
// building, post and rail around the camera (no cars). Its vertex shaders take the view from ViewProjectionModified
// (x, y and depth rows; the 4th row packs depth: z = d * m.x + m.y, w = d * m.z + m.w). While the game draws that
// cascade, every draw is drawn once more with a lamp's view in our constants, into one face of that lamp's map.
// Maps: 32 lamps x 5 faces (no "up"), 256 x 256 depth each, one face refreshed a frame (the city stands still).
// The shader (LampMapVis) finds the face, searches blockers and filters by the lamp's size (soft further away).

static const int kLsSlots = 32, kLsFaces = 5, kLsRes = 256;
static const float kLsTan = 1.08f;                    // faces a bit wider than 90 degrees: filtering across seams
static const float kLsNear = 0.1f;
static ID3D11Texture2D* g_lsTex;
static ID3D11DepthStencilView* g_lsDsv[kLsSlots * kLsFaces];
static ID3D11RasterizerState* g_lsRs;
// done: 0 = to draw, 1 = being drawn, else the frame it was finished (+1). A finished face stays (the city stands still)
// unless the lamp moves, or the camera has come much closer than when it was drawn (the cascade's casters are the
// ones around the camera: near a lamp it has all of the lamp's surroundings).
struct LsSlot { int lamp; float pos[3], far_; unsigned done[kLsFaces]; float drawnDist[kLsFaces]; DWORD used; };
static LsSlot g_ls[kLsSlots];
static int g_lsFace = -1, g_lsFaceSlot = -1;         // the face drawn this frame
static unsigned g_lsRunFrame, g_lsDraws, g_lsLastDraws, g_lsFacesMade;
// A face is drawn over several frames: this frame the casters numbered [cursor, cursor + budget) of the cascade
// (the city stands still, so the list is about the same every frame); done when the cursor passes the end.
static unsigned g_lsCursor, g_lsIdx, g_lsLastIdx;
struct LsCb { ID3D11Buffer* b; int off; unsigned frame; };   // our view constants, by the offset of the view in them
static LsCb g_lsCb[16];
static int g_lsCbN;
static float g_lsM[16];                               // this frame's face view

static bool LsEnsure()
{
    if (g_lsSrv) return true;
    static bool failed;
    if (failed || !g_dev) return false;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = d.Height = kLsRes; d.MipLevels = 1; d.ArraySize = kLsSlots * kLsFaces; d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_lsTex))) { failed = true; Log("lamp shadow maps: texture failed"); return false; }
    for (int k = 0; k < kLsSlots * kLsFaces; ++k) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dv.Texture2DArray.FirstArraySlice = k; dv.Texture2DArray.ArraySize = 1;
        if (FAILED(g_dev->CreateDepthStencilView(g_lsTex, &dv, &g_lsDsv[k]))) { failed = true; Log("lamp shadow maps: views failed"); return false; }
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1; sv.Texture2DArray.ArraySize = kLsSlots * kLsFaces;
    if (FAILED(g_dev->CreateShaderResourceView(g_lsTex, &sv, &g_lsSrv))) { failed = true; Log("lamp shadow maps: SRV failed"); return false; }
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    rd.DepthBias = 0; rd.SlopeScaledDepthBias = 0;
    g_dev->CreateRasterizerState(&rd, &g_lsRs);
    for (int s = 0; s < kLsSlots; ++s) g_ls[s].lamp = -1;
    Log("lamp shadow maps: %d lamps x %d faces, %d x %d", kLsSlots, kLsFaces, kLsRes, kLsRes);
    return true;
}

// a face's axes: forward, right, up (the shader has the same table)
static void LsAxes(int face, float* f, float* r, float* u)
{
    static const float t[5][9] = {
        { 1, 0, 0,   0, 0, -1,  0, 1, 0 },
        { -1, 0, 0,  0, 0, 1,   0, 1, 0 },
        { 0, 0, 1,   1, 0, 0,   0, 1, 0 },
        { 0, 0, -1,  -1, 0, 0,  0, 1, 0 },
        { 0, -1, 0,  1, 0, 0,   0, 0, 1 } };
    memcpy(f, t[face], 12); memcpy(r, t[face] + 3, 12); memcpy(u, t[face] + 6, 12);
}

// the game's packed view for a lamp face: rows x, y, depth (world), then (z scale, z bias, w scale, w bias)
static void LsMatrix(const float* p, float farM, int face, float* m)
{
    float f[3], r[3], u[3];
    LsAxes(face, f, r, u);
    float s = 1.0f / kLsTan;
    for (int j = 0; j < 3; ++j) { m[j] = r[j] * s; m[4 + j] = u[j] * s; m[8 + j] = f[j]; }
    m[3] = -(m[0] * p[0] + m[1] * p[1] + m[2] * p[2]);
    m[7] = -(m[4] * p[0] + m[5] * p[1] + m[6] * p[2]);
    m[11] = -(m[8] * p[0] + m[9] * p[1] + m[10] * p[2]);
    float a = farM / (farM - kLsNear);
    m[12] = a; m[13] = -kLsNear * a; m[14] = 1; m[15] = 0;
}

// Which lamps have maps: the strongest map lamps sent this frame (lampIdx: map index or -1, in score order).
// Slots stay with their lamp while it is among them; a moved lamp gets its faces drawn again.
static void LsAssign(const int* lampIdx, const float (*pos)[4], int n, float farM)
{
    if (!LsEnsure()) return;
    DWORD now = GetTickCount();
    int want = 0;
    for (int k = 0; k < n && want < kLsSlots; ++k) {
        int li = lampIdx[k];
        if (li < 0) continue;
        ++want;
        int s = -1;
        for (int q = 0; q < kLsSlots; ++q) if (g_ls[q].lamp == li) { s = q; break; }
        if (s < 0) {   // a free slot, or the one unused the longest
            DWORD best = 0;
            for (int q = 0; q < kLsSlots; ++q) {
                if (g_ls[q].lamp < 0) { s = q; break; }
                DWORD age = now - g_ls[q].used;
                if (age > 200 && age >= best) { best = age; s = q; }
            }
            if (s < 0) continue;
            g_ls[s].lamp = li;
            memset(g_ls[s].done, 0, sizeof(g_ls[s].done));
            memcpy(g_ls[s].pos, pos[k], 12);
        }
        LsSlot& S = g_ls[s];
        float dx = S.pos[0] - pos[k][0], dy = S.pos[1] - pos[k][1], dz = S.pos[2] - pos[k][2];
        if (dx * dx + dy * dy + dz * dz > 1e-4f || S.far_ != farM) { memcpy(S.pos, pos[k], 12); S.far_ = farM; memset(S.done, 0, sizeof(S.done)); }
        S.used = now;
        if (g_camKnown) {
            float cd = sqrtf(Dist2(S.pos, g_cam[3]));
            for (int f = 0; f < kLsFaces; ++f)
                if (S.done[f] > 1 && S.drawnDist[f] > 60 && cd < S.drawnDist[f] * 0.5f) S.done[f] = 0;
        }
    }
}

static int LsSlotOf(int lamp)
{
    if (lamp < 0 || !g_lsSrv) return -1;
    for (int q = 0; q < kLsSlots; ++q) if (g_ls[q].lamp == lamp && (g_ls[q].done[0] | g_ls[q].done[1] | g_ls[q].done[2] | g_ls[q].done[3] | g_ls[q].done[4])) return q;
    return -1;
}

// a lamp's light radius changed etc.: every map again
static void LsInvalidate() { for (int q = 0; q < kLsSlots; ++q) memset(g_ls[q].done, 0, sizeof(g_ls[q].done)); }

// the face to draw this frame: one never drawn first, then the oldest
// the face to draw next: a face not drawn yet, of the lamp nearest to the camera; none: nothing to draw
static bool LsPickFace()
{
    g_lsFace = g_lsFaceSlot = -1;
    DWORD now = GetTickCount();
    float best = 1e30f;
    for (int q = 0; q < kLsSlots; ++q) {
        if (g_ls[q].lamp < 0 || now - g_ls[q].used > 500) continue;
        float d = g_camKnown ? Dist2(g_ls[q].pos, g_cam[3]) : 0;
        for (int f = 0; f < kLsFaces; ++f)
            if (g_ls[q].done[f] < 2 && d < best) { best = d; g_lsFace = f; g_lsFaceSlot = q; }
    }
    return g_lsFace >= 0;
}

static ID3D11Buffer* LsCbFor(ID3D11DeviceContext* ctx, int off)
{
    for (int k = 0; k < g_lsCbN; ++k)
        if (g_lsCb[k].off == off) {
            if (g_lsCb[k].frame != g_frame + 1) {   // this frame's face view in it
                static char buf[1024];
                memset(buf, 0, sizeof(buf));
                memcpy(buf + off, g_lsM, 64);
                ctx->UpdateSubresource(g_lsCb[k].b, 0, 0, buf, 0, 0);
                g_lsCb[k].frame = g_frame + 1;
            }
            return g_lsCb[k].b;
        }
    if (g_lsCbN >= 16) return 0;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 1024; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Buffer* b = 0;
    if (FAILED(g_dev->CreateBuffer(&bd, 0, &b))) return 0;
    g_lsCb[g_lsCbN].b = b; g_lsCb[g_lsCbN].off = off; g_lsCb[g_lsCbN].frame = 0;
    ++g_lsCbN;
    return LsCbFor(ctx, off);
}

// After every game draw: inside the sun map's widest cascade, the same draw again into this frame's lamp face.
static void LsAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (!g_sunShadowTex || !g_lsSrv || g_p[P_LAMP_SHADOW_MAPS].v < 0.5f) return;
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(0, 0, &dsv);
    if (!dsv) return;
    ID3D11Resource* res = 0;
    dsv->GetResource(&res);
    if (res) res->Release();
    if (res != g_sunShadowTex) { dsv->Release(); return; }
    D3D11_VIEWPORT vp; UINT nv = 1;
    ctx->RSGetViewports(&nv, &vp);
    if (!nv || vp.TopLeftY < vp.Height * 1.5f) { dsv->Release(); return; }   // not the widest cascade
    unsigned budget = (unsigned)g_p[P_LAMP_SHADOW_BUDGET].v;
    if (g_lsRunFrame != g_frame + 1) {   // the cascade starts
        bool first = !g_lsRunFrame;
        g_lsRunFrame = g_frame + 1;
        g_lsLastDraws = g_lsDraws; g_lsDraws = 0;
        g_lsLastIdx = g_lsIdx; g_lsIdx = 0;
        g_lsCursor += budget;
        bool slotGone = g_lsFace >= 0 && (g_ls[g_lsFaceSlot].lamp < 0 || g_ls[g_lsFaceSlot].done[g_lsFace] == 0);
        if (first || g_lsFace < 0 || g_lsCursor >= g_lsLastIdx || slotGone) {   // the face is finished: the next one
            if (g_lsFace >= 0 && !first && !slotGone) {
                LsSlot& D = g_ls[g_lsFaceSlot];
                D.done[g_lsFace] = g_frame + 2;
                D.drawnDist[g_lsFace] = g_camKnown ? sqrtf(Dist2(D.pos, g_cam[3])) : 0;
                ++g_lsFacesMade;
            }
            g_lsCursor = 0;
            if (LsPickFace()) {
                LsSlot& S = g_ls[g_lsFaceSlot];
                LsMatrix(S.pos, S.far_, g_lsFace, g_lsM);
                for (int k = 0; k < g_lsCbN; ++k) g_lsCb[k].frame = 0;   // the new view into our constants
                ctx->ClearDepthStencilView(g_lsDsv[g_lsFaceSlot * kLsFaces + g_lsFace], D3D11_CLEAR_DEPTH, 1.0f, 0);
                if (!S.done[g_lsFace]) S.done[g_lsFace] = 1;   // being drawn: counts as oldest-but-one
            }
        }
    }
    if (g_lsFace < 0) { dsv->Release(); return; }
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    VsInfo* e = VsFind(vs, false);
    if (vs) vs->Release();
    if (!e || e->vpSlot < 0 || e->car || e->vpSlot == e->wSlot || e->vpOff + 64 > 1024) { dsv->Release(); return; }
    unsigned idx = g_lsIdx++;
    if (idx < g_lsCursor || idx >= g_lsCursor + budget) { dsv->Release(); return; }   // another frame's share
    ID3D11Buffer* our = LsCbFor(ctx, e->vpOff);
    if (!our) { dsv->Release(); return; }
    // save, draw into the lamp face, restore
    ID3D11RenderTargetView* savedRtv = 0;
    ID3D11DepthStencilView* dummy = 0;
    ctx->OMGetRenderTargets(1, &savedRtv, &dummy);
    if (dummy) dummy->Release();
    ID3D11Buffer* savedCb = 0;
    ctx->VSGetConstantBuffers(e->vpSlot, 1, &savedCb);
    ID3D11RasterizerState* savedRs = 0;
    ctx->RSGetState(&savedRs);
    ctx->VSSetConstantBuffers(e->vpSlot, 1, &our);
    ctx->OMSetRenderTargets(0, 0, g_lsDsv[g_lsFaceSlot * kLsFaces + g_lsFace]);
    D3D11_VIEWPORT fv = { 0, 0, (float)kLsRes, (float)kLsRes, 0, 1 };
    ctx->RSSetViewports(1, &fv);
    ctx->RSSetState(g_lsRs);
    ID3D11PixelShader* savedPs = 0;
    bool cut = g_p[P_LAMP_SHADOW_CUT].v > 0.5f;
    if (!cut) { ctx->PSGetShader(&savedPs, 0, 0); oPSSetShader(ctx, 0, 0, 0); }   // depth only: no pixel shader
    Replay(ctx, d);
    if (!cut) { oPSSetShader(ctx, savedPs, 0, 0); if (savedPs) savedPs->Release(); }
    ++g_lsDraws;
    ctx->VSSetConstantBuffers(e->vpSlot, 1, &savedCb);
    ctx->OMSetRenderTargets(savedRtv ? 1 : 0, &savedRtv, dsv);
    if (savedRtv) savedRtv->Release();
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(savedRs);
    if (savedCb) savedCb->Release();
    if (savedRs) savedRs->Release();
    dsv->Release();
}
