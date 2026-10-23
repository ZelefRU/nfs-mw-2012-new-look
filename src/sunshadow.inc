// ---------------------------------------------------------------- our own sun shadow maps (1.01..1.04)
// The game's sun shadows: three cascades fitted to the view every frame (their texel size changes as the camera
// moves: edges sharper or blurrier with the camera, shimmering), a sun direction that steps, little depth bias
// (stripes on walls). Ours: four square cascades around the camera (by default 13.5 / 45 / 90 / 270 m half size),
// their texel grid fixed in the world (snapped: no shimmer), the sun's own direction (the lighting's), drawn with the
// game's casters: while the game draws its shadow cascades, each draw is drawn once more into the cascades of ours it
// can reach, with the vertex shader's view matrix replaced by our cascade's (in the game's packed form).
//   near (0, 1): cleared and drawn every frame from the game's cascades 0 and 1 (everything near, the cars too);
//   far (2, 3): only what stands still, kept from frame to frame like the rain map (whatever was ever drawn stays:
//   the game's widest cascade has only the casters for the present view); a share of the draws a frame keeps them
//   up to date; cleared and drawn again only when the camera has gone a quarter of the cascade or the sun has moved.
// 1.01-1.03 drew everything through a geometry shader into all four at once: 60+ ms of GPU (a geometry shader is
// many times slower than plain drawing; 3700 draws a frame).
// The sun pass then reads a shadow mask made per pixel (PS_SunMask, NewLook.hlsl): the distance to the caster sets
// the penumbra as a real sun does (0.53 deg: a shadow is sharp where it starts and softer away from its caster),
// smoothed over frames (PS_SunTemporal).
// 1.25: the mask (and its smoothing) at half the frame's resolution, a quarter of the pixels (it was 1.1-1.4 ms at
// 5120 x 1440); PS_SunUp then makes it at the frame's resolution: each pixel averages the mask texels next to it that
// lie on its own surface (its plane, its N.L: no blending across edges or between a lit and a shaded face); a pixel
// none of them shares a surface with (thinner than two pixels: posts, wires far off) gets the full computation there.
// ShadowMaskHalf 0: the mask at full resolution as before (PS_SunUp then only copies).
// 1.26 (user: building shadows moved in jumps every few seconds, at sunset they jerk): a kept map holds the sun of when
// it was drawn (drawn again every 0.1 / 0.2 / 0.35 deg). The sun's move since then shifts the shadow of a caster t m away
// by (map's sun - sun now) x t; the mask knows t (its caster search) and reads the map that much off (SD): the shadows
// follow the sun every frame, and a map drawn again no longer jumps. No extra taps. The caster search is centred
// halfway along the shift for casters up to SP4.z m and wider by half of it (casters a newer sun brings in are found).
// SkipGameShadows: the game's own shadow map is read only past our widest cascade's inner part; a game shadow draw whose
// caster's whole shadow stays inside (its light-space disk) is drawn into ours only, not into the game's map.
// 1.27 (user: FPS still low; the log at x5 day speed: shadow pass 11-13 ms, kept maps drawn again ~0.5 times a frame):
// (1) our replays are recorded and drawn after the game's shadow pass, grouped by map (see SsFlush: every change of the
// bound depth target costs ~9 us on the user's GPU); (2) with SD the kept maps need drawing again far less often: the
// sun step allowed grows with the sun's height (shadowtest4).
// 1.28 (user: a crane leaves a trail that goes at a redraw; building shadows still jump every 20-30 s). Log 1.27 at x1:
// the kept maps were drawn again ~1 a second, nearly all because "a moving thing" was seen (the traffic: never in the
// kept maps anyway), and those redraws kept the map's old sun: the sun was renewed only at the 1.5-3 deg steps, every
// 35-70 s, where what SD leaves (a few %) showed as a jump. Now: (1) every redraw takes the sun of now; (2) cars do not
// count as moving things; (3) slow movers: a crane turning a little every few frames gets a new key every few frames,
// each still long enough to enter the kept maps: a trail. A kept instance whose place is taken by a nearby instance of
// the same mesh (the old one not drawn since) marks that spot of that mesh as moving (g_ssMover): instances of that mesh
// there go to the moving-things maps every frame, and the kept maps are drawn again (the trail goes).
// 1.21 (the bench: our maps cost 20-22 ms, 13 of them the far ones rebuilt all the time at 400 draws a frame): what
// stands still is kept, as modern engines cache static shadows. 0 (13.5 m) is drawn every frame (few draws; a knocked
// post's shadow follows at once). 1, 2, 3 keep their still part and draw it again only when the camera has gone a
// quarter of the cascade, the sun has moved 0.35 deg, or now and then (objects streamed in, a crane moved): into a
// spare map over a few frames (a share of the draws a frame), then swapped in. 1 also has a map of the cars, cleared
// and drawn every frame; the shaders take the nearer of the two. (A copy of the kept map with the cars drawn on it
// was the first plan: on the user's driver CopyResource from one depth texture into another gives zeros; into a
// staging one it works. ltest/shadowtest.)
// 1.22 (user: moving things in the city updated "once in five years", shadows came in late when the camera turned,
// steps as the sun moved): the kept maps know their objects. Every shadow draw is an instance (its mesh + its world
// matrix, read from our copy of the game's constants: worldedit.inc). An instance seen in two frames running is still:
// it goes into the kept maps once, the frame it is first still (a newly seen one at once, no waiting for a rebuild);
// an instance not yet still (a moving thing gets a new matrix every frame) is drawn every frame into a moving-things
// map (cascades 1, 2; the cars too), never into the kept ones. Kept maps are drawn again (into the spare, the still
// instances as they come, then swapped when a frame adds nothing) when the camera has gone a quarter of the cascade,
// the sun has moved 0.1 / 0.2 / 0.35 deg (1 / 2 / 3), a moving thing was seen (its old place in the kept map goes), or
// every minute. A share of the draws a frame (ShadowFarBudget) at most.
// The camera and the sun are known to the CPU a frame or two late (read back from the GPU): the cascades are placed
// around where the camera will be (its speed), which only decides what they cover; drawing and reading use the same
// cascade constants, so a guess that is a little off costs nothing but a margin.

static const int kSsCasc = 4;
static const float kSsDepthR = 2000.0f;          // metres either side of a cascade's centre along the sun
static ID3D11Texture2D *g_ssNearTex, *g_ssFarTex[3][2];       // 0 (every frame); kept 1, 2, 3 (in use / being built)
static ID3D11DepthStencilView *g_ssNearDsv, *g_ssFarDsv[3][2];
static ID3D11ShaderResourceView *g_ssNearSrv, *g_ssFarSrv[3][2];
static ID3D11Texture2D* g_ssC1Tex;               // 1: the moving things of this frame (the cars too; with its kept map: the nearer)
static ID3D11DepthStencilView* g_ssC1Dsv;
static ID3D11ShaderResourceView* g_ssC1Srv;
static ID3D11Texture2D* g_ssC2Tex;               // 2: the same
static ID3D11DepthStencilView* g_ssC2Dsv;
static ID3D11ShaderResourceView* g_ssC2Srv;
static const int kSsPacks = 7;                   // views to draw with: 0; kept f (cascade f+1): 1 + 2f in use, 2 + 2f being built
static int g_ssRes;
static ID3D11RasterizerState* g_ssRs;
static ID3D11DepthStencilState* g_ssDs;
static ID3D11Buffer* g_ssCb;                     // the cascades for the shaders: PS b7
static ID3D11SamplerState* g_ssCmp;              // PS s15: less-equal, bilinear
// the vertex shaders' view constants per cascade (zeros but the view at the shader's offset), made again when the
// cascade moves
struct SsVpCb { int off; ID3D11Buffer* b[kSsPacks]; unsigned ver[kSsPacks]; };
static SsVpCb g_ssVp[16];
static int g_ssVpN;
static unsigned g_ssVer[kSsPacks];               // a cascade's placement version
static float g_ssPack[kSsPacks][16];             // its view in the game's packed form
static HalfRT g_ssRaw, g_ssHist[2];              // the mask: this frame's, smoothed (this frame / the last, swapped)
static HalfRT g_ssUp;                            // the mask at the frame's resolution (visibility, cascade): the sun pass reads it
static UINT g_ssMaskW, g_ssMaskH, g_ssFullW, g_ssFullH;
static unsigned g_ssFrame;                       // frame + 1 the maps were started
static bool g_ssLive;                            // the near maps are being drawn this frame
static unsigned g_ssDrawnFrame;                  // frame + 1 of the near maps in the texture
static unsigned g_ssMaskFrame[2];                // frame + 1 each smoothed mask was made
static ID3D11Query* g_ssSubQ[3];                // (test harness only: timestamps after the mask, its smoothing, the frame-size pass)
static float g_ssCbData[4 * 28];                 // what the constants hold (SunShadowCB)
static const float kSsShiftReach = 200.0f;       // casters up to this far (m) are found when the sun has moved since a map was drawn
static unsigned g_ssGameSkipped;                 // the game's own shadow draws left out (SkipGameShadows)
static float g_ssSunStepFixed;                  // (test harness: one fixed sun step, deg, for all kept maps; 0 = by the sun's height)
static void SsFlush(ID3D11DeviceContext* ctx);   // (1.27: our recorded replays drawn)
static unsigned g_ssRecStat[3];                  // recorded, drawn at once (could not be kept), flushes (between log lines)
static float g_ssUpEdge = 0.35f;                 // PS_SunUp: texels around differing more: the pixel made at full resolution
static bool g_ssFailed;
// kept cascades 1, 2, 3 (index 0, 1, 2): placement, kept, drawn again in shares
struct SsFar { bool valid, building, budgetHit; int cur; float C[3], sun[3], bC[3], bSun[3], bRows[12], bSc[4]; unsigned built, buildFrames, added; };
static SsFar g_ssFar[3];
// the instances in each kept map (in use / being built)
struct SsKeySet { unsigned long long k[32768]; int n; };
static SsKeySet g_ssKeys[3][2];
static bool SsKeyHas(const SsKeySet& s, unsigned long long key)
{
    unsigned h = (unsigned)(key ^ (key >> 32)) * 2654435761u;
    for (int i = 0; i < 64; ++i) { unsigned long long v = s.k[(h + i) & 32767]; if (v == key) return true; if (!v) return false; }
    return true;   // (full around here: counted as in)
}
static void SsKeyAdd(SsKeySet& s, unsigned long long key)
{
    if (s.n > 26000) return;
    unsigned h = (unsigned)(key ^ (key >> 32)) * 2654435761u;
    for (int i = 0; i < 64; ++i) { unsigned long long& v = s.k[(h + i) & 32767]; if (v == key) return; if (!v) { v = key; ++s.n; return; } }
}
static void SsKeyClear(SsKeySet& s) { memset(s.k, 0, sizeof(s.k)); s.n = 0; }
// every instance seen: when last, how many frames running, still or not; the frame it went into each moving map
struct SsInst { unsigned long long key; unsigned last, run, dyn[2], nearF; bool still, noted; };   // noted: looked at as a mover once still; nearF: drawn into our nearest (frame + 1)
static SsInst g_ssInst[65536];
static unsigned long long g_ssNew[2][256];       // instances first seen in the last two frames (moving things show as never seen again)
static int g_ssNewN[2];
static unsigned g_ssMovingFrame;                 // a moving thing was last seen then
// spots where a mesh moves slowly (a crane): its instances there are moving things, never kept (1.28)
struct SsMover { unsigned long long mesh; float T[3], r; };
static SsMover g_ssMover[64];
static int g_ssMoverN;
static int g_ssBudget;                           // kept-map draws left this frame
// ---- caster culling (1.24): each mesh's radius around its own origin, read back once (indices, then the vertices
// they use; 4 meshes a frame, never waiting). A placed instance (a world matrix of its own) whose radius cannot reach
// a cascade's square, seen along the sun, is not drawn into it: the shadow map's box is unbounded along the sun, so
// "its projection across the sun misses the square" is exact. (The game's middle cascade is 100 m across; ours 0 is
// 27: most of its 200 draws a frame were drawn into ours for nothing.)
struct SsMeshR {
    unsigned long long mesh; float r; int state;   // 0 free, 1 indices sent, 2 vertices sent, 3 known, 4 cannot
    ID3D11Buffer *ib, *vb, *stage; UINT ibOff, start, count, vbOff, stride; INT base; int ifmt, pfmt, poff; bool indexed;
    unsigned sent; UINT vmin, vmax; void* idx;
    unsigned long long kKey[2]; float kT[2][3]; unsigned char kNext;   // its last two instances that became still (movers, 1.28)
};
static SsMeshR g_ssMesh[4096];
static int g_ssMeshJobs;
static unsigned g_ssMeshJobFrame, g_ssMeshKnown, g_ssCulled;
static SsMeshR* SsMeshFind(unsigned long long mesh, bool add)
{
    unsigned h = (unsigned)(mesh ^ (mesh >> 29)) * 2654435761u;
    for (int i = 0; i < 32; ++i) {
        SsMeshR& m = g_ssMesh[(h + i) & 4095];
        if (m.mesh == mesh) return &m;
        if (!m.mesh) { if (!add) return 0; memset(&m, 0, sizeof(m)); m.mesh = mesh; return &m; }
    }
    return 0;
}
// a new mesh: its indices (or vertex range) copied for the CPU
static void SsMeshRequest(ID3D11DeviceContext* ctx, SsMeshR& m, const DrawCall& d)
{
    m.state = 4;
    if (d.kind != DK_INDEXED && d.kind != DK_DRAW) return;
    ID3D11InputLayout* il = 0;
    ctx->IAGetInputLayout(&il);
    const IlPos* ip = il ? IlFind(il) : 0;
    if (il) il->Release();
    if (!ip) return;
    bool okFmt = ip->fmt == DXGI_FORMAT_R32G32B32_FLOAT || ip->fmt == DXGI_FORMAT_R32G32B32A32_FLOAT || ip->fmt == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                 ip->fmt == DXGI_FORMAT_R16G16B16A16_SNORM;
    if (!okFmt) return;
    ID3D11Buffer *ib = 0, *vb = 0;
    DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN;
    UINT ibOff = 0, stride = 0, vbOff = 0;
    ctx->IAGetVertexBuffers(ip->slot, 1, &vb, &stride, &vbOff);
    if (d.kind == DK_INDEXED) ctx->IAGetIndexBuffer(&ib, &ifmt, &ibOff);
    if (!vb || !stride || (d.kind == DK_INDEXED && (!ib || (ifmt != DXGI_FORMAT_R16_UINT && ifmt != DXGI_FORMAT_R32_UINT))) || d.a < 3 || d.a > 2000000) {
        if (ib) ib->Release(); if (vb) vb->Release();
        return;
    }
    m.ib = ib; m.vb = vb; m.ibOff = ibOff; m.start = d.b; m.count = d.a; m.base = d.kind == DK_INDEXED ? d.base : 0; m.vbOff = vbOff; m.stride = stride;
    m.ifmt = ifmt == DXGI_FORMAT_R16_UINT ? 2 : 4; m.pfmt = ip->fmt; m.poff = ip->off; m.indexed = d.kind == DK_INDEXED;
    D3D11_BUFFER_DESC sd = {}, bd;
    sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (m.indexed) {
        sd.ByteWidth = d.a * m.ifmt;
        ib->GetDesc(&bd);
        UINT from = ibOff + d.b * m.ifmt;
        if (from + sd.ByteWidth > bd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) { m.stage = 0; ib->Release(); vb->Release(); m.ib = m.vb = 0; return; }
        D3D11_BOX box = { from, 0, 0, from + sd.ByteWidth, 1, 1 };
        ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, ib, 0, &box);
        m.state = 1;
    } else {   // a plain draw: vertices start .. start + count
        m.vmin = d.b; m.vmax = d.b + d.a - 1;
        sd.ByteWidth = d.a * stride;
        vb->GetDesc(&bd);
        UINT from = vbOff + d.b * stride;
        if (sd.ByteWidth > (16u << 20) || from + sd.ByteWidth > bd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) { m.stage = 0; vb->Release(); m.vb = 0; return; }
        D3D11_BOX box = { from, 0, 0, from + sd.ByteWidth, 1, 1 };
        ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, vb, 0, &box);
        m.state = 2;
    }
    m.sent = g_frame;
}
static void SsMeshDone(SsMeshR& m, int state)
{
    if (m.stage) { m.stage->Release(); m.stage = 0; }
    if (m.ib) { m.ib->Release(); m.ib = 0; }
    if (m.vb) { m.vb->Release(); m.vb = 0; }
    free(m.idx); m.idx = 0;
    m.state = state;
    if (state == 3) ++g_ssMeshKnown;
}
// the read-backs that have arrived (never waits)
static void SsMeshWork(ID3D11DeviceContext* ctx)
{
    for (int i = 0; i < 4096; ++i) {
        SsMeshR& m = g_ssMesh[i];
        if ((m.state != 1 && m.state != 2) || g_frame - m.sent < 2) continue;
        D3D11_MAPPED_SUBRESOURCE mp;
        HRESULT hr = ctx->Map(m.stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr)) { SsMeshDone(m, 4); continue; }
        if (m.state == 1) {
            UINT mn = 0xffffffff, mx = 0, cut = m.ifmt == 2 ? 0xffffu : 0xffffffffu;
            for (UINT k = 0; k < m.count; ++k) {
                UINT v = m.ifmt == 2 ? ((const unsigned short*)mp.pData)[k] : ((const UINT*)mp.pData)[k];
                if (v == cut) continue;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            ctx->Unmap(m.stage, 0);
            m.stage->Release(); m.stage = 0;
            INT first = (INT)mn + m.base;
            D3D11_BUFFER_DESC vbd, sd = {};
            m.vb->GetDesc(&vbd);
            UINT from = m.vbOff + (UINT)first * m.stride, bytes = (mx - mn + 1) * m.stride;
            sd.ByteWidth = bytes; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (mn > mx || first < 0 || bytes > (16u << 20) || from + bytes > vbd.ByteWidth || FAILED(g_dev->CreateBuffer(&sd, 0, &m.stage))) { m.stage = 0; SsMeshDone(m, 4); continue; }
            D3D11_BOX box = { from, 0, 0, from + bytes, 1, 1 };
            ctx->CopySubresourceRegion(m.stage, 0, 0, 0, 0, m.vb, 0, &box);
            m.vmin = mn; m.vmax = mx; m.state = 2; m.sent = g_frame;
            continue;
        }
        // the vertices: the farthest from the origin (all of the range: a little more than the draw uses at worst)
        float r2 = 0;
        const unsigned char* vb = (const unsigned char*)mp.pData;
        for (UINT v = 0; v <= m.vmax - m.vmin; ++v) {
            const unsigned char* e = vb + v * m.stride + m.poff;
            float p[3];
            if (m.pfmt == DXGI_FORMAT_R16G16B16A16_FLOAT) { const unsigned short* hh = (const unsigned short*)e; for (int j = 0; j < 3; ++j) p[j] = Half(hh[j]); }
            else if (m.pfmt == DXGI_FORMAT_R16G16B16A16_SNORM) { const short* hh = (const short*)e; for (int j = 0; j < 3; ++j) p[j] = fmaxf(hh[j] / 32767.0f, -1.0f) * 10.0f; }
            else memcpy(p, e, 12);
            float d2 = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
            if (d2 == d2 && d2 > r2) r2 = d2;
        }
        ctx->Unmap(m.stage, 0);
        m.r = sqrtf(r2);
        SsMeshDone(m, m.r > 0 && m.r < 5000 ? 3 : 4);
    }
}
// could the instance's shadow reach the cascade drawn with these rows? (R: its radius in metres)
static bool SsReaches(const float* rows, const float* T, float R)
{
    float x = rows[0] * T[0] + rows[1] * T[1] + rows[2] * T[2] + rows[3];
    float y = rows[4] * T[0] + rows[5] * T[1] + rows[6] * T[2] + rows[7];
    float s = sqrtf(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);   // 1 / half size
    float lim = 1 + (R * 1.05f + 0.5f) * s;
    return fabsf(x) <= lim && fabsf(y) <= lim;
}
// 1.26: does a caster's whole shadow stay inside the widest cascade's inner part, where the game's own map is never read
// (the mask blends to the game's map in the outer 15% of the part it uses, and uses it alone past that)? rows: that
// cascade's placement; the margin also covers a sun up to a few degrees off the game's
static bool SsInsideWidest(const float* rows, const float* T, float R)
{
    float x = rows[0] * T[0] + rows[1] * T[1] + rows[2] * T[2] + rows[3];
    float y = rows[4] * T[0] + rows[5] * T[1] + rows[6] * T[2] + rows[7];
    float s = sqrtf(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);   // 1 / half size
    float tex = 2 / (s * g_ssRes);
    float lim = 1 - (g_ssCbData[64 + 14] * 0.5f + 3 * tex) * s;                 // as the mask picks its cascade (SP.z)
    float m = (R * 1.05f + 0.5f) * s;
    return fabsf(x) + m < 0.8f * lim && fabsf(y) + m < 0.8f * lim;
}

// (watch: a new one counts towards "a moving thing seen" if it is not seen again; not for cars: never kept)
static SsInst* SsInstOf(unsigned long long key, bool watch)
{
    unsigned h = (unsigned)(key ^ (key >> 29)) * 2654435761u;
    SsInst* old = 0;
    for (int i = 0; i < 16; ++i) {
        SsInst& e = g_ssInst[(h + i) & 65535];
        if (e.key == key) {
            if (e.last != g_frame) { e.run = e.last + 1 == g_frame ? e.run + 1 : 1; e.last = g_frame; if (e.run >= 2) e.still = true; }
            return &e;
        }
        if (!e.key || g_frame - e.last > 900) { if (!old) old = &e; if (!e.key) break; }
    }
    if (!old) return 0;
    memset(old, 0, sizeof(*old));
    old->key = key; old->last = g_frame; old->run = 1; old->dyn[0] = old->dyn[1] = ~0u;
    int b = g_frame & 1;
    if (watch && g_ssNewN[b] < 256) g_ssNew[b][g_ssNewN[b]++] = key;
    return old;
}
static const SsInst* SsInstPeek(unsigned long long key)
{
    unsigned h = (unsigned)(key ^ (key >> 29)) * 2654435761u;
    for (int i = 0; i < 16; ++i) {
        const SsInst& e = g_ssInst[(h + i) & 65535];
        if (e.key == key) return &e;
        if (!e.key) return 0;
    }
    return 0;
}
static bool SsIsMover(unsigned long long mesh, const float* T)
{
    for (int i = 0; i < g_ssMoverN; ++i) {
        const SsMover& m = g_ssMover[i];
        if (m.mesh != mesh) continue;
        float dx = T[0] - m.T[0], dy = T[1] - m.T[1], dz = T[2] - m.T[2];
        if (dx * dx + dy * dy + dz * dz < m.r * m.r) return true;
    }
    return false;
}
// an instance of mesh mr just became still at T: did it take the place of the mesh's last still one (that one not drawn
// for two frames: the same object, moved)? Then that spot of the mesh is a mover.
static void SsMoverCheck(SsMeshR& mr, unsigned long long key, const float* T, float R)
{
    float rad = R > 0 ? R * 1.5f + 1.0f : 3.0f;
    for (int j = 0; j < 2; ++j) {
        if (!mr.kKey[j] || mr.kKey[j] == key) continue;
        float dx = T[0] - mr.kT[j][0], dy = T[1] - mr.kT[j][1], dz = T[2] - mr.kT[j][2];
        if (dx * dx + dy * dy + dz * dz > rad * rad) continue;
        const SsInst* o = SsInstPeek(mr.kKey[j]);
        if (!o || !o->still || o->last + 2 > g_frame) continue;
        if (g_ssMoverN < 64 && !SsIsMover(mr.mesh, T)) {
            SsMover& m = g_ssMover[g_ssMoverN++];
            m.mesh = mr.mesh; memcpy(m.T, mr.kT[j], 12); m.r = rad + sqrtf(dx * dx + dy * dy + dz * dz);
            g_ssMovingFrame = g_frame;   // its old images in the kept maps: drawn again without them
            Log("sun shadows: a slow mover (a crane, a sign...): mesh %08x at %.1f %.1f %.1f, %.2f m since its last still place; within %.1f m it is a "
                "moving thing now", (unsigned)mr.mesh, T[0], T[1], T[2], sqrtf(dx * dx + dy * dy + dz * dz), m.r);
        }
        break;
    }
    memcpy(mr.kT[mr.kNext], T, 12);
    mr.kKey[mr.kNext] = key;
    mr.kNext ^= 1;
}
// read back: the camera (its record) and the sun's direction (the lighting's, from the game's constants)
struct SsStage { ID3D11Texture2D* cam; ID3D11Buffer* sun; unsigned frame; bool hasSun, hasCol; };
static SsStage g_ssSt[4];
static float g_ssSun[3], g_ssCamP[3], g_ssCamV[3], g_ssFwd[3];
// 1.23: how bright the game's key light is (its colour read back with its direction): day 1 .. night 0, as the shaders
// count it (key light luminance about 6 by day). At night our sun maps are not drawn (the game's moon shadows stay:
// the bench at night showed our maps + mask at ~9 ms for a light too weak to matter); in full day the lamps are off
// (unless "Map lamps by day"): ~4 ms of lighting nobody sees in the sun.
static float g_ssDay = -1;                       // -1 = not known yet
static bool g_ssNight;                           // with some hysteresis (g_ssFullDay: newlook.cpp)
static unsigned g_ssNightSince, g_ssDaySince;
static bool g_ssSunKnown, g_ssCamKnown, g_ssFwdKnown;
static unsigned g_ssCamF;                        // frame + 1 of the camera sample
// counts (logged now and then)
struct SsStat { unsigned game[3], drawsNear, drawsFar, cut, depthOnly, noVs, topo, rebuilds, why[5]; };   // why: first, camera, sun, moving thing, age
static SsStat g_ssStat;
static unsigned g_ssStatFrame;
static double g_ssCpuAcc;                        // CPU time of our shadow draws (ms, summed between log lines)
static bool g_ssPassOpen;                        // the shadow pass's GPU timing started this frame
static unsigned g_ssPassFrame;
static ID3D11Buffer* g_ssGameVp[3];              // the game's cascades' views, read back now and then
static unsigned g_ssGameVpFrame[3];
static float g_ssGameHalf[3];                    // their half sizes (m; 0 = not known yet)

// ---- 1.29 - 1.31: our sun's path (SunPath), its rise and set (SunRiseDeg), our moon (MoonShadows)
// The game's key light (hourly keyframes made by hand) is not a sun's path. 1.29 took its height for the time of day,
// 1.30 its azimuth by day: both stood still where the game's light turns. The log of 1.30 (17:44-17:54: four game days
// of 144.5 s at the user's day speed) shows what it is: the light swings to and fro on one line of the sky, its azimuth
// c + r cos(2 pi phase) within 0.65 deg all day and night (c -37.6, r 35.0: -2.6 at its dawn turn, -72.7 at its dusk
// turn), 28 deg up at both turns, up to 50 by day (going one way: the sun) and 40 at night (coming back: the moon); the
// sun comes on at phase ~0.92 (before the dawn turn) and goes off at ~0.535 (just after the dusk turn). So 1.30's sun
// set 10-20 s before the game's went off and at dawn waited 20 s at the horizon (no key light at all), and at night our
// maps took the game's moon (its line back: shadows "in strange places", a jump at dawn).
// 1.31: the phase of the game's day is a straight line in the game's time (its time: not while paused), fitted (least
// squares, weighted) to the last ~third of a day of phases its azimuth tells (two fit an azimuth: the one nearer the
// line; only where the azimuth moves well, not near the turns) and to the turns' marks (the azimuth 2.5 deg back from a
// turn: a known phase); between, and through the turns, the line runs on. The phase we show follows the line, never
// back, at most 3x ahead. At the start or after a jump: the branch from the way the azimuth goes (by day / night from
// the key light until then). Learned and kept (NewLook_sun.txt): the swing (c, r: its turns' azimuths, whatever the
// phase), the phases the sun comes on and goes off, the noon height, the day's length (the switches and the noon once
// the swing is known). Ours: the sun rises at the horizon when the game's comes on and sets when it goes off, on an arc
// (rises at c + 90 deg on the side the game's day starts, noon at c, height M, sets opposite):
//   theta = 180 u; towards it = cos(theta) E + sin(theta) (sin M up + cos M H); u 0..1 through its day.
// The moon (MoonShadows on): the same arc through the night (rises as the sun sets, sets as it rises). Both: the game's
// key light's colour (the sun's, the moon's), times smoothstep(0, SunRiseDeg, height); the game's key light out all
// the time (its direction is the game's line). MoonShadows off: at night the game's moon with its own shadows (1.30).
// Learned values come in eased (no jump). The sun pass: our light x w + the game's x g (P18-P20); the sky's glow, the
// air, the bounce light, the cloud shadows, the game's sun corona and flare follow ours.
struct SpSample { double t, ph, w; };   // the game's time, its phase (cycles counted), weight
struct SunPathState {
    bool init, loaded, wasDay, sure, moon, eased, onSeen, offSeen, swingKnown, turnHave[2];
    float c, r, on, off, M, k;           // learned: the swing's centre and half (deg), the sun's on / off phases, noon height; the day's way (1: azimuth down)
    double ph, rate;                     // the phase shown (0: the game's dawn turn, 0.5: its dusk turn); the day's speed kept (cycles a second)
    double gameT, fitA, fitB, fitT, fitSpan, out;   // the game's time; the line (phase = A + B (t - T)), its samples' span; the phase shown (cycles counted)
    SpSample smp[256];
    int smpN, smpHead;
    double smpLastT, initT, lostT, lastT, savedT, markT;   // (markT: the game's time of the last turn's mark; 0 none)
    float initAz, prev[3], col[3];
    float extAz, turn[2];                // the swing's turns: the furthest azimuth since the light last turned; the last turns (dawn's, dusk's)
    int extDir, still;                   // its way now (k-wise: 1 up towards the dawn turn, -1 down; 0 not known); readbacks it stood still
    float dayMaxEl, rej[2];
    int rejN[2];                         // a switch seen far from the learnt: where, how many times in a row
    float w, g, ourEl, ourAz;
    float saved[6], aC, aM, aOn, aOff;   // (aC...: what our path uses: the learned, eased in)
    float hour, dayCol[3];               // 1.32: the clock (hours); the game's noon sun's colour (learned)
    bool dayColHave;
    float gs[128][4], skyEl;
    float gd[128][4];                    // 1.34: the game's sun / moon disc over its day (its corona's direction, ~15 deg under its light)             // 1.33: the game's light over its day (towards it; w 1 the sun, 2 the moon, 0 not seen); our sun's height (under the horizon at night)
    DWORD nextLog;
};
static SunPathState g_sp;
static double g_spClock = -1;                    // (test harness: its own clock, s)
static float g_ssSunGame[3];                     // the game's key light (light -> scene), as read back
static unsigned g_flareDraws[4];                 // the game's flare draws with our VS (glare 1, 2; its sun's visibility; 1.37: its moon)
static float g_coronaGame[3];                    // the game's corona (light -> scene), as last seen (1.31: the flare; 1.34: the debug)
static bool g_coronaGameKnown;
static float SpWrap(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
static double SpWrap1(double x) { return x - floor(x); }        // 0..1
static double SpHalf(double x) { return x - floor(x + 0.5); }   // -0.5..0.5
static void SunPathFile(bool save)
{
    char p[MAX_PATH];
    sprintf(p, "%sNewLook_sun.txt", g_dir);
    if (save) {
        FILE* f = fopen(p, "w");
        if (f) {
            fprintf(f, "v2 %.2f %.2f %.4f %.4f %.2f %.1f %.0f\n// the game's key light (towards it): its azimuth's swing (centre, half: deg), the phases its sun "
                       "comes on and goes off (0: its dawn turn), its noon height (deg), a game day (s), its way by day (1: azimuth down) (learned; NewLook 1.31)\n",
                    g_sp.c, g_sp.r, g_sp.on, g_sp.off, g_sp.M, 1 / g_sp.rate, g_sp.k);
            fclose(f);
        }
        return;
    }
    g_sp.loaded = true;
    g_sp.c = -37.65f; g_sp.r = 35.05f; g_sp.on = 0.92f; g_sp.off = 0.535f; g_sp.M = 50; g_sp.rate = 1 / 144.5; g_sp.k = 1;   // (the user's game, 2026-10-18)
    FILE* f = fopen(p, "r");
    if (f) {
        char tag[16] = "";
        float c, r, on, off, m, P, k;
        if (fscanf(f, "%15s", tag) == 1 && !strcmp(tag, "v2") && fscanf(f, "%f %f %f %f %f %f %f", &c, &r, &on, &off, &m, &P, &k) == 7 &&
            r > 5 && r < 175 && on >= 0 && on < 1 && off >= 0 && off < 1 && m > 5 && m < 89 && P > 20 && P < 36000) {
            g_sp.c = c; g_sp.r = r; g_sp.on = on; g_sp.off = off; g_sp.M = m; g_sp.rate = 1.0 / P; g_sp.k = k < 0 ? -1.0f : 1.0f;
            g_sp.swingKnown = true;
        }
        fclose(f);
    }
    float s[6] = { g_sp.c, g_sp.r, g_sp.on, g_sp.off, g_sp.M, (float)(1 / g_sp.rate) };
    memcpy(g_sp.saved, s, sizeof(s));
    Log("sun path: the game's light swings about azimuth %.1f by %.1f; its sun on at phase %.3f, off at %.3f; noon %.1f deg up; a day %.1f s",
        g_sp.c, g_sp.r, g_sp.on, g_sp.off, g_sp.M, 1 / g_sp.rate);
}

// the phases an azimuth fits: a by day (the day's way), 1 - a at night; s: how fast the azimuth moves there (0 at the turns)
static void SpPhases(float az, double* a, float* s)
{
    float x = g_sp.k * SpWrap(az - g_sp.c) / g_sp.r;
    x = x < -1 ? -1 : x > 1 ? 1 : x;
    *a = acos((double)x) / 6.283185307179586;
    *s = sqrtf(1 - x * x);
}
static bool SpCrossed(double from, double to, double x) { return SpWrap1(from - x) > 0.5 && SpWrap1(to - x) < 0.5; }
// how far past a turn its mark is seen (the azimuth 2.5 deg back from it; phase)
static double SpTurnMark() { double x = 1 - 2.5 / (g_sp.r > 5 ? g_sp.r : 5); return acos(x < -1 ? -1 : x) / 6.283185307179586; }
// the line through the samples (weighted least squares; under 3 s of them: through them at the speed kept). Over
// more than a whole day the azimuth's unevenness (the same each day) bends it no more: the phase shown is off the
// game's by a constant at most (and the switches are learned in it)
static void SpFit()
{
    double Sw = 0, Sx = 0, Sxx = 0, Sy = 0, Sxy = 0, tr = g_sp.gameT, t0 = tr;
    for (int i = 0; i < g_sp.smpN; ++i) {
        const SpSample& q = g_sp.smp[(g_sp.smpHead - 1 - i) & 255];
        double x = q.t - tr;
        Sw += q.w; Sx += q.w * x; Sxx += q.w * x * x; Sy += q.w * q.ph; Sxy += q.w * x * q.ph;
        if (q.t < t0) t0 = q.t;
    }
    if (Sw <= 0) return;
    double span = tr - t0, den = Sw * Sxx - Sx * Sx, b = g_sp.rate;
    if (span > 3 && den > 1e-12) b = (Sw * Sxy - Sx * Sy) / den;
    if (b < 1.0 / 36000) b = 1.0 / 36000; else if (b > 1.0 / 20) b = 1.0 / 20;
    g_sp.fitA = (Sy - b * Sx) / Sw; g_sp.fitB = b; g_sp.fitT = tr; g_sp.fitSpan = span;
}
static void SpAdd(double ph, double w)
{
    SpSample& q = g_sp.smp[g_sp.smpHead & 255];
    q.t = g_sp.gameT; q.ph = ph; q.w = w;
    g_sp.smpHead = (g_sp.smpHead + 1) & 255;
    if (g_sp.smpN < 256) ++g_sp.smpN;
    SpFit();
    // the latest 3% of a day off the line by over 1.5% of a day (the azimuth's unevenness: under 1%): a new day speed,
    // the samples before the latest 4% dropped
    double recent = 0.03 / g_sp.fitB, keep = 0.04 / g_sp.fitB, sum = 0, n = 0;
    for (int i = 0; i < g_sp.smpN; ++i) {
        const SpSample& s = g_sp.smp[(g_sp.smpHead - 1 - i) & 255];
        if (g_sp.gameT - s.t > recent) break;
        sum += s.ph - (g_sp.fitA + g_sp.fitB * (s.t - g_sp.fitT)); ++n;
    }
    if (n >= 4 && fabs(sum / n) > 0.015 && g_sp.fitSpan > keep * 2) {
        int k = 0;
        while (k < g_sp.smpN && g_sp.gameT - g_sp.smp[(g_sp.smpHead - 1 - k) & 255].t <= keep) ++k;
        Log("sun path: the game's day off our line by %.3f lately (a new day speed?): its last %d samples kept", sum / n, k);
        g_sp.smpN = k;
        SpFit();
    }
}
// the start, a jump, a lost line: the way (the branch) found anew from the azimuth
static void SpRestart(float az, double t)
{
    g_sp.sure = false; g_sp.initAz = az; g_sp.initT = g_sp.gameT; g_sp.lostT = -1; g_sp.smpN = 0;
}
// a turn of the swing seen (i 0: dawn's, 1: dusk's; its azimuth): the swing from it and the other (seen, or as it was);
// at once until both are seen (then known), halfway after
static void SpTurn(int i, float az)
{
    g_sp.turn[i] = az;
    g_sp.turnHave[i] = true;
    float other = g_sp.turnHave[i ^ 1] ? g_sp.turn[i ^ 1] : SpWrap(g_sp.c + (i ? 1 : -1) * g_sp.k * g_sp.r);
    float h = i == 0 ? az : other, l = i == 0 ? other : az, c = SpWrap(l + SpWrap(h - l) / 2), r = fabsf(SpWrap(h - l)) / 2;
    if (r < 5) return;
    bool both = g_sp.turnHave[0] && g_sp.turnHave[1];
    float q = g_sp.swingKnown && both ? 0.5f : 1.0f;
    g_sp.c = SpWrap(g_sp.c + SpWrap(c - g_sp.c) * q);
    g_sp.r += (r - g_sp.r) * q;
    if (both) g_sp.swingKnown = true;
}

// one readback of the game's key light: dir (light -> scene), col its colour (0: not read)
static void SunPathStep(const float* dir, const float* col)
{
    if (!g_sp.loaded) SunPathFile(false);
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    double t = g_spClock >= 0 ? g_spClock : g_qpf.QuadPart ? (double)q.QuadPart / g_qpf.QuadPart : 0;
    float dt = g_sp.lastT > 0 ? (float)(t - g_sp.lastT) : 0;
    if (dt < 0) dt = 0; else if (dt > 0.25f) dt = 0.25f;
    g_sp.lastT = t;
    const float D = 57.29578f;
    float T[3] = { -dir[0], -dir[1], -dir[2] };   // towards the light
    float el = asinf(T[1] > 1 ? 1 : T[1] < -1 ? -1 : T[1]) * D, az = atan2f(T[0], T[2]) * D;
    float lum = col ? 0.2126f * col[0] + 0.7152f * col[1] + 0.0722f * col[2] : (g_ssDay > 0.05f ? 6.0f : 0.0f);
    bool day = lum > 0.6f;   // the sun (6+ by day); the moon ~0.01-0.5
    float step = 0;
    if (g_sp.init) {   // (by the cross product: acos of a float dot cannot tell steps under ~0.02 deg, the game's at x5 a frame)
        const float* p = g_sp.prev;
        float cx = p[1] * T[2] - p[2] * T[1], cy = p[2] * T[0] - p[0] * T[2], cz = p[0] * T[1] - p[1] * T[0];
        step = atan2f(sqrtf(cx * cx + cy * cy + cz * cz), p[0] * T[0] + p[1] * T[1] + p[2] * T[2]) * D;
    }
    bool first = !g_sp.init, jump = g_sp.init && step > 3;   // (a time jump; the sun / moon switch moves it ~1 deg)
    g_sp.still = !first && step < 1e-5f ? g_sp.still + 1 : 0;
    bool paused = g_sp.still >= 6;   // (while its time runs the game's light moves every frame)
    memcpy(g_sp.prev, T, 12);
    g_sp.init = true;
    double dtp = paused || first ? 0 : dt;
    g_sp.gameT += dtp;
    double a; float s;
    SpPhases(az, &a, &s);
    float wq = (s - 0.3f) / 0.3f;
    wq = wq < 0 ? 0 : wq > 1 ? 1 : wq;
    double wo = s * wq * wq * (3 - 2 * wq);   // how well the azimuth tells the phase here (none near the turns)
    double prevPh = g_sp.ph;
    if (first || jump) {
        SpRestart(az, t);
        g_sp.extDir = 0; g_sp.extAz = az; g_sp.markT = 0;   // (its turns: looked for anew)
        if (jump) Log("sun path: the game's light jumped %.1f deg: its phase from its azimuth again", step);
    }
    // not sure of the way yet: by day / night meanwhile; then the way the azimuth goes (its first 0.2 deg; 5 s still: by day / night)
    if (!g_sp.sure) {
        float dx = g_sp.k * SpWrap(az - g_sp.initAz);
        bool moved = fabsf(dx) > 0.2f, dayWay = moved ? dx < 0 : day;
        double p0 = dayWay ? a : 1 - a;
        if (moved || g_sp.gameT - g_sp.initT > 5) {
            g_sp.sure = true;
            g_sp.out = p0 + floor(g_sp.out - p0 + 0.5);   // (cycles counted on)
            g_sp.smpN = 0; g_sp.smpLastT = -1e9;
            SpAdd(g_sp.out, 1);
            if (!first) Log("sun path: the game's light goes the %s way: its phase %.3f", dayWay ? "day's" : "night's", p0);
        } else g_sp.out = p0 + floor(g_sp.out - p0 + 0.5);
        prevPh = SpWrap1(g_sp.out);
    }
    if (g_sp.sure && !paused && !first && !jump) {
        double pred = g_sp.fitA + g_sp.fitB * (g_sp.gameT - g_sp.fitT);
        // a sample where the azimuth tells well (1/200 of a day apart)
        double gap = 0.005 / g_sp.fitB;   // (256 of them: the last ~1.3 days)
        if (wo > 0.5 && g_sp.gameT - g_sp.smpLastT >= (gap > 0.2 ? gap : 0.2)) {
            g_sp.smpLastT = g_sp.gameT;
            double c1 = a + floor(pred - a + 0.5), c2 = (1 - a) + floor(pred - (1 - a) + 0.5), cu = fabs(c1 - pred) <= fabs(c2 - pred) ? c1 : c2;
            if (fabs(cu - pred) < 0.06) { SpAdd(cu, wo); g_sp.lostT = -1; }
            else if (g_sp.lostT < 0) g_sp.lostT = g_sp.gameT;
            else if (g_sp.gameT - g_sp.lostT > 1.5) {   // far from the line for a while (a jump in small steps, a new day speed)
                Log("sun path: lost the game's day (%.3f off its line): its phase from its azimuth again", cu - pred);
                SpRestart(az, t);
            }
        }
        // the swing's turns: where its azimuth turns back by 2.5 deg (a turn seen whole: the way known before it); its mark
        float d = g_sp.k * SpWrap(az - g_sp.extAz);
        int was = g_sp.extDir;
        if (g_sp.extDir >= 0) {
            if (d > 0) g_sp.extAz = az;
            else if (d < -2.5f) { if (g_sp.extDir > 0) SpTurn(0, g_sp.extAz); g_sp.extDir = -1; g_sp.extAz = az; }
        } else {
            if (d < 0) g_sp.extAz = az;
            else if (d > 2.5f) { SpTurn(1, g_sp.extAz); g_sp.extDir = 1; g_sp.extAz = az; }
        }
        if (g_sp.sure && was != 0 && was != g_sp.extDir) {
            // the line off (the mark far from it: a new day speed while the azimuth told nothing): anew from the mark, at
            // the speed of half a day since the last mark (mixed, but the samples to come set it right)
            double ref = (g_sp.extDir > 0 ? 0.5 : 0.0) + SpTurnMark(), ru = ref + floor(g_sp.out - ref + 0.5);
            double half = g_sp.markT > 0 ? g_sp.gameT - g_sp.markT : 0, rh = half > 2 ? 0.5 / half : 0;
            if (fabs(ru - pred) < 0.05) SpAdd(ru, 8);
            else {
                Log("sun path: the game's light turned at phase %.3f of our line, not %.3f%s: the line anew from here", SpWrap1(pred), SpWrap1(ref),
                    rh > 0 ? ", the day's speed from half a day" : "");
                if (rh > 1.0 / 36000 && rh < 1.0 / 20) g_sp.rate = rh;
                g_sp.smpN = 0;
                SpAdd(ru, 8);
            }
            g_sp.markT = g_sp.gameT;
        }
        // the day's speed kept: the line's, once it spans a while
        if (g_sp.fitSpan * g_sp.fitB > 0.3) g_sp.rate += (g_sp.fitB - g_sp.rate) * (dtp / 30 > 1 ? 1 : dtp / 30);
    }
    // the phase shown: the line's now, never back (a fifth of its speed on), at most 3x ahead; far off: at once
    if (g_sp.sure) {
        double now = g_sp.fitA + g_sp.fitB * (g_sp.gameT - g_sp.fitT), adv = g_sp.fitB * dtp, dd = now - (g_sp.out + adv);
        if (fabs(dd) > 0.1) g_sp.out = now;
        else g_sp.out += adv + (dd > 2 * adv ? 2 * adv : dd < -0.8 * adv ? -0.8 * adv : dd);
        if (g_sp.extDir != 0) {   // never further past the next turn than its azimuth came back from it (+0.4 deg): the line
            float back = fabsf(SpWrap(az - g_sp.extAz));   // may run on at a speed gone by where the azimuth tells little
            double x = 1 - (back + 0.4) / (g_sp.r > 5 ? g_sp.r : 5);
            double L = (g_sp.extDir < 0 ? 0.5 : 0.0) + acos(x < -1 ? -1 : x) / 6.283185307179586, over = SpWrap1(g_sp.out - L);
            if (over < 0.4) g_sp.out -= over;   // (past it; just after the last turn's mark: over 0.46 and more)
        }
    }
    g_sp.ph = SpWrap1(g_sp.out);
    if (g_sp.sure) {   // (the debug view: the game's light over its day)
        float* b = g_sp.gs[(int)(g_sp.ph * 128) & 127];
        b[0] = T[0]; b[1] = T[1]; b[2] = T[2]; b[3] = day ? 1.0f : 2.0f;
        if (g_coronaGameKnown) { float* q = g_sp.gd[(int)(g_sp.ph * 128) & 127]; q[0] = -g_coronaGame[0]; q[1] = -g_coronaGame[1]; q[2] = -g_coronaGame[2]; q[3] = b[3]; }
    }
    // learnt (the swing known, the line long: else the phase is off): the sun's switches, the noon height
    if (g_sp.sure && g_sp.swingKnown && g_sp.fitSpan > 20 && !paused && !first && !jump && g_sp.gameT - g_sp.initT > 20) {
        if (SpCrossed(prevPh, g_sp.ph, 0.75)) g_sp.dayMaxEl = 0;   // (a new day: its highest point)
        if (SpCrossed(prevPh, g_sp.ph, 0.5) && g_sp.dayMaxEl > 10 && fabsf(g_sp.dayMaxEl - g_sp.M) > 0.3f) g_sp.M = g_sp.dayMaxEl;
        if (g_sp.ph > 0.1 && g_sp.ph < 0.4 && day && el > g_sp.dayMaxEl) g_sp.dayMaxEl = el;
        if (day != g_sp.wasDay) {
            int i = day ? 0 : 1;
            float& sw = day ? g_sp.on : g_sp.off;
            bool& seen = day ? g_sp.onSeen : g_sp.offSeen;
            double dsw = SpHalf(g_sp.ph - sw);
            bool again = g_sp.rejN[i] > 0 && fabs(SpHalf(g_sp.ph - g_sp.rej[i])) < 0.02;   // (far, but where it was last time)
            if (fabs(dsw) < (seen ? 0.03 : 0.12) || (again && g_sp.rejN[i] >= 2)) {
                sw = (float)SpWrap1(sw + dsw * (seen && !again ? 0.5 : 1.0));
                seen = true;
                g_sp.rejN[i] = 0;
            } else {
                g_sp.rejN[i] = again ? g_sp.rejN[i] + 1 : 1;
                g_sp.rej[i] = (float)g_sp.ph;
                Log("sun path: the game's sun went %s at phase %.3f, far from %.3f: not learnt (%d)", day ? "on" : "off", g_sp.ph, sw, g_sp.rejN[i]);
            }
        }
        float now6[6] = { g_sp.c, g_sp.r, g_sp.on, g_sp.off, g_sp.M, (float)(1 / g_sp.rate) };
        bool moved = fabsf(SpWrap(now6[0] - g_sp.saved[0])) > 0.2f || fabsf(now6[1] - g_sp.saved[1]) > 0.2f || fabs(SpHalf(now6[2] - g_sp.saved[2])) > 0.002 ||
                     fabs(SpHalf(now6[3] - g_sp.saved[3])) > 0.002 || fabsf(now6[4] - g_sp.saved[4]) > 0.2f || fabsf(now6[5] - g_sp.saved[5]) > now6[5] * 0.03f;
        if (moved && t - g_sp.savedT > 20) {
            memcpy(g_sp.saved, now6, sizeof(now6));
            g_sp.savedT = t;
            SunPathFile(true);
            Log("sun path: learned: the game's light swings about azimuth %.1f by %.1f; its sun on at phase %.3f, off at %.3f; noon %.1f deg up; a day %.1f s",
                g_sp.c, g_sp.r, g_sp.on, g_sp.off, g_sp.M, 1 / g_sp.rate);
        }
    }
    if (col) memcpy(g_sp.col, col, 12);   // the game's key light's colour: the sun's by day, the moon's at night
    // what our path uses: the learned, eased in (the switches 0.2% of a day a second, the arc's azimuth and height 0.2 deg)
    if (!g_sp.eased) { g_sp.aC = g_sp.c; g_sp.aM = g_sp.M; g_sp.aOn = g_sp.on; g_sp.aOff = g_sp.off; g_sp.eased = true; }
    {
        float sp = dt * 0.002f, sa = dt * 0.2f, d;
        d = (float)SpHalf(g_sp.on - g_sp.aOn); g_sp.aOn = (float)SpWrap1(g_sp.aOn + (d > sp ? sp : d < -sp ? -sp : d));
        d = (float)SpHalf(g_sp.off - g_sp.aOff); g_sp.aOff = (float)SpWrap1(g_sp.aOff + (d > sp ? sp : d < -sp ? -sp : d));
        d = SpWrap(g_sp.c - g_sp.aC); g_sp.aC = SpWrap(g_sp.aC + (d > sa ? sa : d < -sa ? -sa : d));
        d = g_sp.M - g_sp.aM; g_sp.aM += d > sa ? sa : d < -sa ? -sa : d;
    }
    // 1.32: the clock (hours): 06:00 when the game's sun comes on, plus ClockShift; ours by it: the sun from SunriseHour
    // to SunsetHour on its arc (noon height SunNoonDeg, turned SunTurnDeg from across the game's line), the moon through
    // the night on the same arc. The sun's colour (SunColour on): the game's noon sun's (learned) through the air of its
    // height (Kasten-Young air mass; less blue and green low: orange at the horizon); the moon's: MoonLight, bluish.
    if (day && col && g_sp.ph > 0.15 && g_sp.ph < 0.35 && lum > 2) {   // (the game's noon sun)
        if (!g_sp.dayColHave) { memcpy(g_sp.dayCol, col, 12); g_sp.dayColHave = true; }
        float q = dt / 20 > 1 ? 1 : dt / 20;
        for (int j = 0; j < 3; ++j) g_sp.dayCol[j] += (col[j] - g_sp.dayCol[j]) * q;
    }
    double hour = 6 + 24 * SpWrap1(g_sp.ph - g_sp.aOn) + g_p[P_CLOCK].v;
    hour -= 24 * floor(hour / 24);
    float rise = g_p[P_SUNRISE].v, set = g_p[P_SUNSET].v;
    if (set < rise + 1) set = rise + 1;
    double dayLen = set - rise, hu = hour - rise;
    hu -= 24 * floor(hu / 24);
    bool sunUp = hu < dayLen, moonOn = g_p[P_MOON_SHADOWS].v > 0.5f, ours = sunUp || moonOn;
    double f = sunUp ? hu / dayLen : moonOn ? (hu - dayLen) / (24 - dayLen) : 1;   // (no moon of ours: the sun stays where it set)
    float noon = g_p[P_SUN_NOON].v, A = g_sp.aC + g_p[P_SUN_TURN].v;
    float th = (float)(f * 3.14159265358979), m = noon / D, ca = A / D, e = (A + g_sp.k * 90) / D;
    float H[3] = { sinf(ca), 0, cosf(ca) }, E[3] = { sinf(e), 0, cosf(e) };
    float Tu[3] = { cosf(th) * E[0] + sinf(th) * cosf(m) * H[0], sinf(th) * sinf(m), cosf(th) * E[2] + sinf(th) * cosf(m) * H[2] };
    float moonFade = 1;
    if (!sunUp && moonOn) {   // 1.36: the moon hangs (MoonHeightDeg; MoonTurnDeg from over the sun's noon), drifting MoonDriftDeg over
        // the night the sun's way; its light comes in over the night's first 8% and goes over its last
        float maz = (A + g_p[P_MOON_AZ].v - g_sp.k * g_p[P_MOON_DRIFT].v * (float)(f - 0.5)) / D, mel = g_p[P_MOON_EL].v / D;
        Tu[0] = cosf(mel) * sinf(maz); Tu[1] = sinf(mel); Tu[2] = cosf(mel) * cosf(maz);
        float a0 = (float)f / 0.08f, a1 = (float)(1 - f) / 0.08f, a = a0 < a1 ? a0 : a1;
        a = a < 0 ? 0 : a > 1 ? 1 : a;
        moonFade = a * a * (3 - 2 * a);
    }
    float l = sqrtf(Tu[0] * Tu[0] + Tu[1] * Tu[1] + Tu[2] * Tu[2]);
    if (l < 1e-6f) l = 1;
    float oel = asinf(Tu[1] / l) * D, fade = g_p[P_SUN_FADE].v;
    float lc[3];
    if (!sunUp) { float b = g_p[P_MOON_LIGHT].v * 0.1f; lc[0] = 0.75f * b; lc[1] = 0.85f * b; lc[2] = b; }   // (1.33: a tenth)
    else if (g_p[P_SUN_COL].v > 0.5f) {
        auto air = [](float x) { x = x < 0 ? 0 : x; return 1 / (sinf(x / 57.29578f) + 0.15f * powf(x + 3.885f, -1.253f)); };
        static const float k[3] = { 0.10f, 0.18f, 0.36f };
        // 1.35: its strength by its height like the game's sun (~sin(height)^1.3: 3 at 28 deg against 7 at 50), SunLowDim;
        // the air's colour only as a tint (orange low)
        float d0 = air(oel) - air(noon), tr[3];
        for (int j = 0; j < 3; ++j) tr[j] = expf(-k[j] * (d0 > 0 ? d0 : 0));
        float sn = sinf((oel > 0 ? oel : 0) / D) / sinf(noon / D), I = powf(sn > 1 ? 1 : sn, g_p[P_SUN_LOW].v);
        for (int j = 0; j < 3; ++j) lc[j] = (g_sp.dayColHave ? g_sp.dayCol[j] : 6.5f * (j == 0 ? 1.06f : j == 1 ? 1.0f : 0.88f)) * tr[j] / tr[0] * I;
    } else memcpy(lc, g_sp.col, 12);
    for (int j = 0; j < 3; ++j) { g_sunOurs[j] = -Tu[j] / l; g_sunCol[j] = lc[j]; }
    g_sp.hour = (float)hour;
    {   // 1.33: the sun for the sky's glow, the clouds' light, the corona and the flare: ours, at night on under the horizon
        // (with the moon's direction the sky drew the game's twilight sun at the moon); the night's share by its depth
        double fs = sunUp ? hu / dayLen : 1 + (hu - dayLen) / (24 - dayLen);
        float ts = (float)(fs * 3.14159265358979);
        float Sd[3] = { cosf(ts) * E[0] + sinf(ts) * cosf(m) * H[0], sinf(ts) * sinf(m), cosf(ts) * E[2] + sinf(ts) * cosf(m) * H[2] };
        float sl = sqrtf(Sd[0] * Sd[0] + Sd[1] * Sd[1] + Sd[2] * Sd[2]);
        if (sl < 1e-6f) sl = 1;
        for (int j = 0; j < 3; ++j) g_sunSky[j] = -Sd[j] / sl;
        g_sp.skyEl = asinf(Sd[1] / sl) * D;
        float nf = -g_sp.skyEl / 8;   // (0 at the horizon, 1 from 8 deg under it)
        nf = nf < 0 ? 0 : nf > 1 ? 1 : nf;
        g_nightF = nf * nf * (3 - 2 * nf);
    }
    float wT = fade > 0.01f ? (oel / fade) : (oel > 0 ? 1.0f : 0.0f);
    wT = wT < 0 ? 0 : wT > 1 ? 1 : wT;
    wT = wT * wT * (3 - 2 * wT);
    // the game's key light's share: 0 while ours shines (the sun; the moon with MoonShadows), else 1; 1.5 s between
    if (!sunUp && moonOn) wT = moonFade;
    g_sunSkyW = sunUp ? wT : 0.0f;
    {   // 1.36: the haze by the clock: HazeNight at night, up to HazeDawn at sunrise (over the hour before), down to HazeDay
        // over HazeDawnHours; HazeDay to HazeNight over the hour after sunset
        float hn = g_p[P_HAZE_NIGHT].v, hm = g_p[P_HAZE_DAWN].v, hd = g_p[P_HAZE_DAY].v, hl = g_p[P_HAZE_DAWN_H].v;
        double sinceRise = hour - rise, sinceSet = hour - set;
        sinceRise -= 24 * floor(sinceRise / 24); sinceSet -= 24 * floor(sinceSet / 24);
        auto sm = [](double x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return (float)(x * x * (3 - 2 * x)); };
        float hz;
        if (sinceRise >= 23) hz = hn + (hm - hn) * sm(sinceRise - 23);                    // the hour before sunrise
        else if (sinceRise < dayLen) {
            hz = hm + (hd - hm) * sm(sinceRise / (hl > 0.1f ? hl : 0.1f));                // the dawn's haze thinning
        } else hz = hd + (hn - hd) * sm(sinceSet);                                       // the hour after sunset, the night
        g_hazeMul = hz;
    }
    {   // 1.35: the discs in our sky (the game's sun / moon sprite is not drawn while ours shine); the sky's light for the
        // haze (the sun's colour at noon by our sun's height, from 4 deg under the horizon: the dawn's haze not black)
        float ll = 0.2126f * lc[0] + 0.7152f * lc[1] + 0.0722f * lc[2];
        g_sunDisc[0] = sunUp ? ll * wT : 0.0f;
        g_sunDisc[1] = sunUp && lc[0] > 1e-4f ? 1 - (lc[2] / lc[0] > 1 ? 1 : lc[2] / lc[0]) : 0.0f;
        for (int j = 0; j < 3; ++j) g_moonDisc[j] = Tu[j] / l;
        g_moonDisc[3] = !sunUp && moonOn ? 1.5f * wT : 0.0f;   // (1.37: the moon's place; the sky's disc of it no more: the game's moon moves there)
        const float* dc = g_sp.dayColHave ? g_sp.dayCol : 0;
        float dl = dc ? 0.2126f * dc[0] + 0.7152f * dc[1] + 0.0722f * dc[2] : 6.5f, ts = (g_sp.skyEl + 4) / 24;
        ts = ts < 0 ? 0 : ts > 1 ? 1 : ts;
        g_skyLum = dl * ts * ts * (3 - 2 * ts);
    }
    float gT = ours ? 0.0f : 1.0f;
    if (first) { g_sp.g = gT; g_sp.w = ours ? wT : 0; }
    else {
        float gs = dt / 1.5f, dg = gT - g_sp.g;
        g_sp.g += dg > gs ? gs : dg < -gs ? -gs : dg;
        g_sp.w = ours ? wT : (g_sp.w - dt / 1.5f > 0 ? g_sp.w - dt / 1.5f : 0);
    }
    g_sp.wasDay = day;
    g_sp.moon = !sunUp;
    g_sp.ourEl = oel; g_sp.ourAz = atan2f(Tu[0], Tu[2]) * D;
    g_sunW = g_sp.w;
    g_sunG = g_sp.g;
    g_sunActive = g_enabled && g_p[P_SUN_PATH].v > 0.5f && g_p[P_SS].v > 0.5f;
    if (!g_sunActive) { g_nightF = 1 - g_ssDay; g_hazeMul = 1; }   // (the game's day / night; its haze as set)
    DWORD now = GetTickCount();
    if (g_sunActive && (LONG)(now - g_sp.nextLog) >= 0) {
        g_sp.nextLog = now + 10000;
        int hm = (int)(g_sp.hour * 60 + 0.5) % 1440;
        Log("sun path: %02d:%02d; the game's %.1f deg up, azimuth %.1f (key light %.2f, %s); its day's phase %.3f (a day %.0f s%s%s); ours: the %s %.1f up, azimuth %.1f, "
            "its light %.2f (%.2f %.2f %.2f); the game's key light kept %.2f; flare draws ours %u / %u / %u", hm / 60, hm % 60, el, az, lum, day ? "the sun" : "the moon", g_sp.ph,
            1 / g_sp.rate, paused ? ", paused" : "", g_sp.sure ? "" : ", its way not sure yet", g_sp.moon ? "moon" : "sun", oel, g_sp.ourAz, g_sp.w, g_sunCol[0], g_sunCol[1],
            g_sunCol[2], g_sp.g, g_flareDraws[0], g_flareDraws[1], g_flareDraws[2]);
    }
}

// ---- 1.30: the game's sun corona (its scattering: the halo and rays around the sun, on its fog / sky shaders and
// some materials: SunCoronaDirection in gObjectCB, found by reflection at their creation) points at our sun by day: the
// buffer swapped for a copy with our direction for their draws (the sign the game uses against its key light kept)
struct CoronaSh { void* sh; short slot, off; };
static CoronaSh g_corona[32];
static volatile LONG g_coronaN;
static float g_coronaSign;                       // the game's corona against its key light (light -> scene): +1 / -1 (0 not known)
static ID3D11Buffer* g_coronaPool[8];
static UINT g_coronaPoolSize[8];
static int g_coronaPoolNext;
static ID3D11Buffer *g_coronaSavedPs, *g_coronaSavedVs;
static int g_coronaPsSlot = -1, g_coronaVsSlot = -1;
static unsigned g_coronaDraws;
static void CoronaNote(const void* code, SIZE_T size, void* sh)
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
        ID3D11ShaderReflectionVariable* v = cb->GetVariableByName("SunCoronaDirection");
        D3D11_SHADER_VARIABLE_DESC vd;
        if (!v || FAILED(v->GetDesc(&vd))) continue;
        D3D11_SHADER_INPUT_BIND_DESC b;
        if (FAILED(r->GetResourceBindingDescByName(bd.Name, &b))) continue;
        LONG k = InterlockedIncrement(&g_coronaN) - 1;
        if (k >= 32) { g_coronaN = 32; break; }
        g_corona[k].slot = (short)b.BindPoint; g_corona[k].off = (short)vd.StartOffset;
        MemoryBarrier();
        g_corona[k].sh = sh;
        break;
    }
    r->Release();
}
static const CoronaSh* CoronaFind(void* sh)
{
    LONG n = g_coronaN;
    for (LONG i = 0; i < n && i < 32; ++i) if (g_corona[i].sh == sh) return &g_corona[i];
    return 0;
}
// a copy of b with our sun at off (0: cannot)
static ID3D11Buffer* CoronaCopy(ID3D11DeviceContext* ctx, ID3D11Buffer* b, int off)
{
    const unsigned char* data = 0;
    UINT size = 0;
    if (WeCbNow(b, &data, &size) != 1 || (UINT)off + 12 > size) return 0;
    const float* gc = (const float*)(data + off);
    float d = gc[0] * g_ssSunGame[0] + gc[1] * g_ssSunGame[1] + gc[2] * g_ssSunGame[2];
    if (g_coronaSign == 0 && fabsf(d) > 0.3f) {
        g_coronaSign = d > 0 ? 1.0f : -1.0f;
        Log("sun corona: the game's %.2f %.2f %.2f against its key light %.2f %.2f %.2f: %s", gc[0], gc[1], gc[2], g_ssSunGame[0], g_ssSunGame[1],
            g_ssSunGame[2], d > 0 ? "the same way (light -> scene)" : "the other way (towards the light)");
    }
    float s = g_coronaSign != 0 ? g_coronaSign : -1.0f;
    {
        float l = sqrtf(gc[0] * gc[0] + gc[1] * gc[1] + gc[2] * gc[2]);
        if (l > 1e-6f) { for (int j = 0; j < 3; ++j) g_coronaGame[j] = s * gc[j] / l; g_coronaGameKnown = true; }
    }
    int k = g_coronaPoolNext++ & 7;
    UINT need = (size + 15) & ~15u;
    if (!g_coronaPool[k] || g_coronaPoolSize[k] < need) {
        if (g_coronaPool[k]) g_coronaPool[k]->Release();
        g_coronaPool[k] = 0;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = need; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_coronaPool[k]))) { g_coronaPool[k] = 0; return 0; }
        g_coronaPoolSize[k] = need;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_coronaPool[k], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, data, size);
    float* nd = (float*)((unsigned char*)m.pData + off);
    for (int j = 0; j < 3; ++j) nd[j] = s * g_sunSky[j];   // (1.33: the sun, at night under the horizon)
    ctx->Unmap(g_coronaPool[k], 0);
    return g_coronaPool[k];
}
static void CoronaBefore(ID3D11DeviceContext* ctx)
{
    g_coronaPsSlot = g_coronaVsSlot = -1;
    if (!g_coronaN || !g_sunActive || g_sunG >= 0.5f) return;
    const CoronaSh* p = CoronaFind(g_lastPS);
    if (!p) return;
    ID3D11Buffer* b = 0;
    ctx->PSGetConstantBuffers(p->slot, 1, &b);
    if (b) {
        ID3D11Buffer* nb = CoronaCopy(ctx, b, p->off);
        if (nb) { ctx->PSSetConstantBuffers(p->slot, 1, &nb); g_coronaSavedPs = b; g_coronaPsSlot = p->slot; }
        else b->Release();
    }
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    const CoronaSh* v = CoronaFind(vs);
    if (vs) vs->Release();
    if (v) {
        ID3D11Buffer* vb = 0;
        ctx->VSGetConstantBuffers(v->slot, 1, &vb);
        if (vb) {
            ID3D11Buffer* nb = CoronaCopy(ctx, vb, v->off);
            if (nb) { ctx->VSSetConstantBuffers(v->slot, 1, &nb); g_coronaSavedVs = vb; g_coronaVsSlot = v->slot; }
            else vb->Release();
        }
    }
    ++g_coronaDraws;
}
static void CoronaAfter(ID3D11DeviceContext* ctx)
{
    if (g_coronaPsSlot >= 0) { ctx->PSSetConstantBuffers(g_coronaPsSlot, 1, &g_coronaSavedPs); g_coronaSavedPs->Release(); g_coronaPsSlot = -1; }
    if (g_coronaVsSlot >= 0) { ctx->VSSetConstantBuffers(g_coronaVsSlot, 1, &g_coronaSavedVs); g_coronaSavedVs->Release(); g_coronaVsSlot = -1; }
}

// ---- 1.31: the game's sun flare (the user's frame trace with it on, 17:46:32): after the bloom, at half size, its
// glare quads (VS 1766 / 1803 of the shader dumps: screen quads, their light (k^3 x 4) x GenericVector4_B, k from
// -dot(GenericVector4_A, the quad's view ray / its normal): A is the sun's direction in view space (light -> scene)),
// times the sun's visibility (PS 2445 / 3094: GenericSamplerA at GenericVector4_D: a texel of a 32x32 target filled just
// before by PS 2493 / VS 1813: a point at the sun (POSITION: in view space, through `projection`), the share of 7x7
// depth samples around it not in front of it). For these draws (the shaders known by their DXBC checksums at creation)
// the VS is ours: the game's, with our sun (or moon) where the game's light was (its key light or its corona, matched
// in view space or world space; else as it was), turned into view space with this frame's camera record (t0: the
// camera's rays -(A + B u + C v), written by the sun pass), its glare faded with ours. Our constants at b5.
static const unsigned char kFlareSum[11][16] = {
    { 0x35, 0x54, 0xdf, 0xa3, 0xbc, 0x63, 0x62, 0xa9, 0xa6, 0x01, 0xd7, 0x95, 0x36, 0x3b, 0x0c, 0xd3 },   // PS 2445: the glare
    { 0x84, 0x41, 0x13, 0xf7, 0xe6, 0x11, 0x6d, 0x55, 0xb2, 0x21, 0x0e, 0x64, 0xfd, 0x3a, 0x48, 0x24 },   // PS 3094: the glare (vertex colour)
    { 0x6c, 0x57, 0x19, 0x6f, 0xb2, 0x4f, 0xd7, 0x61, 0x35, 0x79, 0x61, 0xc4, 0x64, 0x55, 0xd4, 0x3e },   // PS 2493: the sun's visibility
    { 0x59, 0x49, 0xc0, 0x9e, 0x38, 0x34, 0x2a, 0xe2, 0x9b, 0x90, 0x08, 0x81, 0x9c, 0xb5, 0x01, 0xe6 },   // VS 1766: the glare (view rays)
    { 0x4d, 0xd0, 0x28, 0xd5, 0x52, 0xf7, 0x7e, 0x9a, 0xd3, 0x3a, 0xeb, 0x21, 0x4e, 0x2a, 0x68, 0xeb },   // VS 1803: the glare (normals)
    { 0x07, 0x71, 0xde, 0x70, 0xaa, 0x5e, 0xb1, 0xa5, 0x0b, 0x31, 0xdf, 0xfc, 0x83, 0x96, 0xe1, 0x97 },   // VS 1813: the sun's visibility point
    { 0x47, 0x46, 0x81, 0x2c, 0xa6, 0x66, 0x50, 0x7c, 0x91, 0x8f, 0xce, 0x1c, 0x1f, 0xbb, 0x7a, 0x10 },   // PS 2623: an effect's quad (the sun / moon sprite with t1 the visibility)
    { 0x75, 0x7f, 0x53, 0x9e, 0x95, 0x8d, 0x1f, 0xd3, 0xc8, 0xfb, 0x4e, 0x0b, 0x95, 0x74, 0x0e, 0xd8 },   // PS 2757: the same, another
    { 0xd3, 0xab, 0x7f, 0xdf, 0xbd, 0xb2, 0x71, 0xba, 0xd0, 0x0e, 0xdf, 0x2e, 0x88, 0xc0, 0xa3, 0xf7 },   // PS 2561: an effect's mesh, textured (the moon)
    { 0x71, 0x7d, 0x7f, 0x91, 0x95, 0xbc, 0xff, 0xd6, 0x4b, 0xd3, 0x1d, 0xbc, 0x47, 0x1c, 0x03, 0x65 },   // PS 3030: the same, another
    { 0x3f, 0x16, 0x5c, 0xcf, 0xc7, 0xf7, 0x22, 0x93, 0x59, 0x63, 0xe9, 0x28, 0x27, 0x4b, 0x50, 0x33 } }; // VS 1859: an effect's mesh (worldViewProj)
static const char* kFlareName[11] = { "glare PS (2445)", "glare PS (3094)", "sun visibility PS (2493)", "glare VS (1766)", "glare VS (1803)", "sun visibility VS (1813)",
                                      "effect PS (2623)", "effect PS (2757)", "effect PS (2561)", "effect PS (3030)", "effect VS (1859)" };
static void* volatile g_flareSh[11];
static void* g_sunOccRes;                        // the 32x32 target of the sun's visibility (identity only)
static unsigned g_spriteSkips;
static volatile LONG g_flareN;                   // the game's flare PSs found
static ID3D11VertexShader* g_flareVs[4];         // ours for VS 1766, 1803, 1813; 1859 (the moon)
static ID3D11Buffer* g_flareCb;
static bool g_flareTried, g_flareOn, g_flareLogged[4];
static ID3D11VertexShader* g_flareSavedVs;
static ID3D11Buffer* g_flareSavedCb;
static ID3D11ShaderResourceView* g_flareSavedSrv;
static unsigned g_flareCbFrame;

static const char* kFlareCommon = R"HLSL(
cbuffer FlareCB : register(b5) { float4 FL0, FL1, FL2, FL3; };   // ours (light -> scene, world), its light; the game's key light; its corona (w: known); our moon (towards, w: up)
Texture2D<float4> tCamRec : register(t0);                    // this frame's camera record: rays -(A + B u + C v)
float3 ToView(float3 w)
{
    float3 A = tCamRec.Load(int3(0, 0, 0)).xyz, B = tCamRec.Load(int3(1, 0, 0)).xyz, C = tCamRec.Load(int3(2, 0, 0)).xyz;
    float3 f = normalize(-(A + 0.5 * B + 0.5 * C)), r = normalize(-B);
    r = normalize(r - f * dot(r, f));
    float3 u = normalize(C - f * dot(C, f) - r * dot(C, r));
    return float3(dot(w, r), dot(w, u), -dot(w, f));   // (x right, y up, z back)
}
// v the game's light (light -> scene; view or world space)? ours instead (its length kept), w: our light; else v, 1
float4 Ours(float3 v)
{
    float l = length(v);
    float3 n = v / max(l, 1e-6);
    float kv = max(dot(n, ToView(FL1.xyz)), FL2.w > 0.5 ? dot(n, ToView(FL2.xyz)) : -1);
    if (kv > 0.99) return float4(ToView(FL0.xyz) * l, FL0.w);
    float kw = max(dot(n, FL1.xyz), FL2.w > 0.5 ? dot(n, FL2.xyz) : -1);
    if (kw > 0.99) return float4(FL0.xyz * l, FL0.w);
    return float4(v, 1);
}
)HLSL";
static const char* kFlareGlare = R"HLSL(
cbuffer GlareG : register(b0) { float4 GenericVector4_A, GenericVector4_B; };
struct G1In { float3 pos : POSITION; float2 uv : TEXCOORD0; float4 col : TEXCOORD1; };
struct G1Out { float4 pos : SV_Position; float2 t0 : TEXCOORD0; float2 t2 : TEXCOORD2; float3 t1 : TEXCOORD1; float4 t3 : TEXCOORD3; };
G1Out VS_Glare1(G1In i)
{
    G1Out o;
    o.pos = float4(i.pos, 1);
    o.t0 = i.uv;
    o.t2 = i.pos.xy * float2(0.5, -0.5) + 0.5;
    float3 r = normalize(float3(i.pos.xy, -0.3)) + float3(0, 0, 1);
    r = max(abs(i.pos.y), abs(i.pos.x)) * r - float3(0, 0, 1);
    float4 A = Ours(GenericVector4_A.xyz);
    float k = saturate(-dot(A.xyz, r));
    o.t1 = k * k * k * 4 * GenericVector4_B.xyz * A.w;
    o.t3 = float4(i.col.xyz * i.col.w, i.col.w);
    return o;
}
struct G2In { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 n : NORMAL; };
struct G2Out { float4 pos : SV_Position; float2 t0 : TEXCOORD0; float2 t2 : TEXCOORD2; float3 t1 : TEXCOORD1; };
G2Out VS_Glare2(G2In i)
{
    G2Out o;
    o.pos = float4(i.pos, 1);
    o.t0 = i.uv;
    o.t2 = i.uv;
    float4 A = Ours(GenericVector4_A.xyz);
    float k = saturate(-dot(A.xyz, i.n)) * 0.3;
    o.t1 = k * k * k * 4 * GenericVector4_B.xyz * A.w;
    return o;
}
)HLSL";
static const char* kFlareOcc = R"HLSL(
cbuffer OccG : register(b0) { row_major float4x4 projection; };
cbuffer OccO : register(b1) { float4 gTextureDimensions; };
struct OccIn { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct OccOut { float4 pos : SV_Position; float3 t0 : TEXCOORD0; };
OccOut VS_SunOcc(OccIn i)
{
    OccOut o;
    o.pos = float4(i.uv, 0, 1);
    float3 p = -Ours(-i.pos).xyz;   // (a point towards the light: towards ours at its distance)
    float4 c = mul(float4(p, 1), projection);
    float3 s = c.xyz / c.w;
    float2 lo = gTextureDimensions.zw * 4;
    o.t0 = float3(min(1 - lo, max(lo, s.xy * float2(0.5, -0.5) + 0.5)), s.z);
    return o;
}
)HLSL";

// 1.37: the game's moon (the night trace, 23:02: a textured quad of an effect, VS 1859 / PS 2561 or 3030, drawn just after
// the sky): its VS, the same, but its corners within 12 deg of the game's moon (its sky's light or its key light; 1.38: by
// each corner, the mesh is in the sky's world) moved on the screen by the game's moon's place to ours (its size and look
// kept); our moon not up: out of sight
static const char* kFlareMoon = R"HLSL(
cbuffer MoonO : register(b1) { float4 gVfxConstants; row_major float4x4 worldViewProj; };
Texture2D<float4> ExposureVertexTexture : register(t15);
SamplerState ExposureVertexSampler : register(s15);
struct MIn { float3 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct MOut { float4 pos : SV_Position; float4 t0 : TEXCOORD0; float2 t1 : TEXCOORD1; };
float2 DirUv(float3 d, out bool front)   // where a direction (towards it) is on the screen (uv 0..1)
{
    float3 A = tCamRec.Load(int3(0, 0, 0)).xyz, B = tCamRec.Load(int3(1, 0, 0)).xyz, C = tCamRec.Load(int3(2, 0, 0)).xyz;
    float3 c12 = cross(C, -d);
    float det = dot(B, c12);
    front = false;
    if (abs(det) < 1e-12) return 0;
    float s = dot(B, cross(C, -A)) / det;
    front = s < 0;
    return float2(dot(-A, c12), dot(B, cross(-A, -d))) / det;
}
MOut VS_Moon(MIn i)
{
    MOut o;
    float4 c = mul(float4(i.pos, 1), worldViewProj);
    if (c.w > 1e-6) {   // (1.38: each corner: its world is the sky's, not the moon's middle)
        float2 ndc = c.xy / c.w, uv = ndc * float2(0.5, -0.5) + 0.5;
        float3 A = tCamRec.Load(int3(0, 0, 0)).xyz, B = tCamRec.Load(int3(1, 0, 0)).xyz, C = tCamRec.Load(int3(2, 0, 0)).xyz;
        float3 r = normalize(-(A + B * uv.x + C * uv.y)), g1 = -normalize(FL1.xyz), g2 = -normalize(FL2.xyz);
        float k1 = dot(r, g1), k2 = FL2.w > 0.5 ? dot(r, g2) : -1;
        if (max(k1, k2) > 0.978) {   // (within 12 deg of the game's moon: its quad)
            bool f0, f1;
            float2 gu = DirUv(k2 >= k1 ? g2 : g1, f0), ou = DirUv(normalize(FL3.xyz), f1);
            if (FL3.w > 0.5 && f0 && f1) c.xy += (ou - gu) * float2(2, -2) * c.w;
            else c = float4(0, 0, -1, 1);   // (out of sight)
        }
    }
    o.pos = c;
    float4 ex = ExposureVertexTexture.SampleLevel(ExposureVertexSampler, float2(0.5, 0.5), 0);
    float3 r0 = i.col.xyz * ex.w - i.col.xyz;
    r0 = gVfxConstants.x * r0 + i.col.xyz;
    float3 r1 = saturate(r0 * 0.5);
    float3 r2 = (r1 * 0.070706 - 0.317854) * r1 - 0.022711;
    r1 = saturate(sqrt(r1) * 1.271696 + r2);
    r2 = r0 * 0.5 - r1;
    r1 = gVfxConstants.y * r2 + r1;
    o.t0 = float4(gVfxConstants.y * (r0 - r1) + r1, i.col.w);
    o.t1 = i.uv;
    return o;
}
)HLSL";

static void FlareNote(const void* code, SIZE_T size, void* sh, bool vs)
{
    if (!code || size < 20 || memcmp(code, "DXBC", 4)) return;
    for (int i = 0; i < 11; ++i)
        if (((i >= 3 && i < 6) || i == 10) == vs && !memcmp((const char*)code + 4, kFlareSum[i], 16)) {
            g_flareSh[i] = sh;
            if (!vs && (i < 3 || i == 8 || i == 9)) InterlockedIncrement(&g_flareN);
            Log("sun flare: the game's %s found", kFlareName[i]);
        }
}
static bool FlareEnsure()
{
    if (g_flareCb) return true;
    if (g_flareTried || !g_dev || !g_compile) return false;
    g_flareTried = true;
    static const char* entry[4] = { "VS_Glare1", "VS_Glare2", "VS_SunOcc", "VS_Moon" };
    int made = 0;
    for (int k = 0; k < 4; ++k) {
        static char src[8192];
        _snprintf(src, sizeof(src) - 1, "%s%s", kFlareCommon, k < 2 ? kFlareGlare : k == 2 ? kFlareOcc : kFlareMoon);
        src[sizeof(src) - 1] = 0;
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(src, strlen(src), "flare", 0, 0, entry[k], "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) &&
            SUCCEEDED(g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_flareVs[k]))) ++made;
        else { g_flareVs[k] = 0; Log("sun flare: our %s: %s", entry[k], err ? (const char*)err->GetBufferPointer() : "failed"); }
        if (code) code->Release();
        if (err) err->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!made || FAILED(g_dev->CreateBuffer(&bd, 0, &g_flareCb))) { g_flareCb = 0; Log("sun flare: ours not made"); return false; }
    Log("sun flare: ours made (%d of 4 shaders)", made);
    return true;
}
// w (world) in the view of the camera record read back (a few frames old: for the log)
static void FlareToViewCpu(const float* w, float* o)
{
    float f[3], r[3], u[3];
    for (int j = 0; j < 3; ++j) { f[j] = -(g_cam[0][j] + 0.5f * g_cam[1][j] + 0.5f * g_cam[2][j]); r[j] = -g_cam[1][j]; u[j] = g_cam[2][j]; }
    #define FL_DOT(a, b) (a[0] * b[0] + a[1] * b[1] + a[2] * b[2])
    #define FL_NORM(a) { float l_ = sqrtf(FL_DOT(a, a)) + 1e-12f; a[0] /= l_; a[1] /= l_; a[2] /= l_; }
    FL_NORM(f);
    float rf = FL_DOT(r, f);
    for (int j = 0; j < 3; ++j) r[j] -= f[j] * rf;
    FL_NORM(r);
    float uf = FL_DOT(u, f), ur = FL_DOT(u, r);
    for (int j = 0; j < 3; ++j) u[j] -= f[j] * uf + r[j] * ur;
    FL_NORM(u);
    o[0] = FL_DOT(w, r); o[1] = FL_DOT(w, u); o[2] = -FL_DOT(w, f);
    #undef FL_DOT
    #undef FL_NORM
}
static void FlareBefore(ID3D11DeviceContext* ctx)
{
    void* ps = g_lastPS;
    if (!ps || (ps != g_flareSh[0] && ps != g_flareSh[1] && ps != g_flareSh[2] && ps != g_flareSh[8] && ps != g_flareSh[9])) return;
    if (!g_sunActive || g_sunG >= 0.5f) return;
    int cur = g_frame & 1, last = cur ^ 1;
    ID3D11ShaderResourceView* cam = g_camFrame[cur] == g_frame + 1 ? g_camRec[cur].srv : g_camFrame[last] == g_frame ? g_camRec[last].srv : 0;
    if (!cam) return;
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    int k = !vs ? -1 : vs == g_flareSh[3] ? 0 : vs == g_flareSh[4] ? 1 : vs == g_flareSh[5] ? 2 : vs == g_flareSh[10] ? 3 : -1;
    if (k < 0 || !FlareEnsure() || !g_flareVs[k]) { if (vs) vs->Release(); return; }
    if (g_flareCbFrame != g_frame + 1) {   // (once a frame)
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ctx->Map(g_flareCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { vs->Release(); return; }
        float* c = (float*)m.pData;
        for (int j = 0; j < 3; ++j) { c[j] = g_sunSky[j]; c[4 + j] = g_ssSunGame[j]; c[8 + j] = g_coronaGame[j]; }
        c[3] = g_sunSkyW; c[7] = 1; c[11] = g_coronaGameKnown ? 1.0f : 0.0f;
        for (int j = 0; j < 3; ++j) c[12 + j] = g_moonDisc[j];
        c[15] = g_moonDisc[3] > 0.01f ? 1.0f : 0.0f;
        ctx->Unmap(g_flareCb, 0);
        g_flareCbFrame = g_frame + 1;
    }
    if (!g_flareLogged[k] && g_camKnown) {   // what the game gives (its light in view space?), once
        g_flareLogged[k] = true;
        float kv[3], cv[3];
        FlareToViewCpu(g_ssSunGame, kv);
        FlareToViewCpu(g_coronaGame, cv);
        ID3D11Buffer* b = 0;
        ctx->VSGetConstantBuffers(0, 1, &b);
        const unsigned char* data = 0;
        UINT size = 0;
        if (k < 2 && b && WeCbNow(b, &data, &size) == 1 && size >= 32) {
            const float* A = (const float*)data;
            Log("sun flare: the game's glare %d: its direction %.3f %.3f %.3f (colour %.2f %.2f %.2f); its key light in view %.3f %.3f %.3f, its corona %.3f %.3f %.3f%s",
                k + 1, A[0], A[1], A[2], A[4], A[5], A[6], kv[0], kv[1], kv[2], cv[0], cv[1], cv[2], g_coronaGameKnown ? "" : " (not seen)");
        } else Log("sun flare: the game's %s drawn with ours; its key light in view %.3f %.3f %.3f, its corona %.3f %.3f %.3f", k == 3 ? "moon VS (1859)" : kFlareName[3 + k], kv[0], kv[1], kv[2], cv[0], cv[1], cv[2]);
        if (b) b->Release();
    }
    g_flareSavedVs = vs;   // (its reference kept until after the draw)
    ctx->VSGetConstantBuffers(5, 1, &g_flareSavedCb);
    ctx->VSGetShaderResources(0, 1, &g_flareSavedSrv);
    ctx->VSSetShader(g_flareVs[k], 0, 0);
    ctx->VSSetConstantBuffers(5, 1, &g_flareCb);
    ctx->VSSetShaderResources(0, 1, &cam);
    g_flareOn = true;
    ++g_flareDraws[k];
    if (k == 2) {   // (the sun's visibility target: its sprite reads it)
        ID3D11RenderTargetView* rtv = 0;
        ctx->OMGetRenderTargets(1, &rtv, 0);
        if (rtv) { ID3D11Resource* r = 0; rtv->GetResource(&r); g_sunOccRes = r; if (r) r->Release(); rtv->Release(); }
    }
}
// ---- 1.33: the debug (SunPathDebug): the paths over the sky and in a sky map (bottom right; up: where the camera looks):
// ours over the day (yellow the sun, pale blue the moon), the game's light as seen over its day (cyan the sun, magenta
// the moon) and its disc (1.34: where its sky draws the sun / moon, ~15 deg lower; the same colours, darker), each now a
// big dot; the horizon grey. Drawn over the finished picture at Present.
static const char* kSunDbgSrc = R"HLSL(
cbuffer SunDbgCB : register(b0) { float4 SD[360]; };   // 0 ours now (towards; w 1 sun, 2 moon), 1 the game's now, 2 x: aspect, 3 the game's disc now; 8.. ours (96), 104.. the game's light (128; w 0 not seen), 232.. its disc (128)
Texture2D<float4> tCam : register(t0);
Texture2D<float4> tPano : register(t1);   // 1.39b: the reflections' sky panorama (shown as a strip at the top)
TextureCube tProbe : register(t2);        // 1.46: the sky probe (a strip under it, the same way)
SamplerState sDbg : register(s0);
float3 Ray(float2 uv) { float3 A = tCam.Load(int3(0, 0, 0)).xyz, B = tCam.Load(int3(1, 0, 0)).xyz, C = tCam.Load(int3(2, 0, 0)).xyz; return normalize(-(A + B * uv.x + C * uv.y)); }
float3 Col(float w, bool ours) { return ours ? (w < 1.5 ? float3(1, 0.85, 0.1) : float3(0.75, 0.85, 1)) : (w < 1.5 ? float3(0.1, 0.9, 1) : float3(1, 0.25, 0.9)); }
float4 Mark(float3 d, float r0, float r1)
{
    float4 o = 0;
    [loop] for (int k = 0; k < 128; ++k) { float4 s = SD[232 + k]; if (s.w > 0.5 && dot(d, s.xyz) > r0) o = float4(Col(s.w, false) * 0.55, 0.8); }
    [loop] for (int i = 0; i < 128; ++i) { float4 s = SD[104 + i]; if (s.w > 0.5 && dot(d, s.xyz) > r0) o = float4(Col(s.w, false), 0.9); }
    if (SD[3].w > 0.5 && dot(d, SD[3].xyz) > r1) o = float4(Col(SD[3].w, false) * 0.55, 1);
    [loop] for (int j = 0; j < 96; ++j) { float4 s = SD[8 + j]; if (dot(d, s.xyz) > r0) o = float4(Col(s.w, true), 0.9); }
    if (dot(d, SD[1].xyz) > r1) o = float4(Col(SD[1].w, false), 1);
    if (dot(d, SD[0].xyz) > r1) o = float4(Col(SD[0].w, true), 1);
    return o;
}
float4 ps(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    if (uv.y < 0.12 && uv.x > 0.3 && uv.x < 0.7) {   // the panorama: north mid, 90 deg up at the top, 10 under at the bottom
        float4 c = tPano.Load(int3(int2(float2((uv.x - 0.3) / 0.4, uv.y / 0.12) * float2(1024, 256)), 0));
        if (c.a < 0.5) { float k = fmod(floor(pos.x / 8) + floor(pos.y / 8), 2); return float4((0.12 + 0.08 * k).xxx, 0.9); }   // (not seen)
        float3 t = c.rgb / c.a;
        return float4(t / (1 + t) * 1.6, 1);
    }
    if (uv.y < 0.24 && uv.x > 0.3 && uv.x < 0.7) {   // 1.46: the sky probe (what the reflections get where none was seen)
        float2 q = float2((uv.x - 0.3) / 0.4, (uv.y - 0.12) / 0.12);
        float az = (q.x - 0.5) * 6.2831853, el = radians(90 - q.y * 100);
        float4 c = tProbe.SampleLevel(sDbg, float3(cos(el) * sin(az), sin(el), cos(el) * cos(az)), 0);
        if (c.a < 0.5) { float k = fmod(floor(pos.x / 8) + floor(pos.y / 8), 2); return float4((0.12 + 0.08 * k).xxx, 0.9); }   // (none)
        return float4(c.rgb / (1 + c.rgb) * 1.6, 1);
    }
    float aspect = SD[2].x, R = 0.22;
    float2 p = float2((uv.x - (1 - R * 1.1 / aspect)) * aspect, uv.y - (1 - R * 1.1)) / R;
    if (dot(p, p) < 1) {   // the sky map: the horizon the rim, overhead the middle
        float rr = length(p), el = (1 - rr) * 1.5707963;
        float3 fw = Ray(float2(0.5, 0.5));
        float a = atan2(fw.x, fw.z) + atan2(p.x, -p.y);
        float3 d = float3(cos(el) * sin(a), sin(el), cos(el) * cos(a));
        float4 m = Mark(d, 0.99926, 0.99756);   // (2.2, 4 deg)
        if (m.a > 0) return m;
        if (abs(rr - 0.995) < 0.006 || (abs(p.x) < 0.006 && p.y < 0 && p.y > -0.25)) return float4(0.6, 0.6, 0.6, 0.9);
        return float4(0.03, 0.04, 0.06, 0.75);
    }
    float3 d = Ray(uv);
    float4 m = Mark(d, 0.999981, 0.99978);   // (0.35, 1.2 deg)
    if (m.a > 0) return m;
    if (abs(d.y) < 0.0015) return float4(0.6, 0.6, 0.6, 0.5);
    return 0;
}
)HLSL";
static ID3D11PixelShader* g_sdPs;
static ID3D11Buffer *g_sdCb, *g_sdRect;
static bool g_sdTried;
// towards our sun / moon at an hour (kind 1 the sun, 2 the moon), as SunPathStep puts them
static void SpOursAt(double hour, float* T, float* kind)
{
    const float D = 57.29578f;
    float rise = g_p[P_SUNRISE].v, set = g_p[P_SUNSET].v;
    if (set < rise + 1) set = rise + 1;
    double dayLen = set - rise, hu = hour - rise;
    hu -= 24 * floor(hu / 24);
    bool sunUp = hu < dayLen;
    double f = sunUp ? hu / dayLen : (hu - dayLen) / (24 - dayLen);
    float A = g_sp.aC + g_p[P_SUN_TURN].v, th = (float)(f * 3.14159265358979), m = g_p[P_SUN_NOON].v / D, ca = A / D, e = (A + g_sp.k * 90) / D;
    float H[3] = { sinf(ca), 0, cosf(ca) }, E[3] = { sinf(e), 0, cosf(e) };
    T[0] = cosf(th) * E[0] + sinf(th) * cosf(m) * H[0]; T[1] = sinf(th) * sinf(m); T[2] = cosf(th) * E[2] + sinf(th) * cosf(m) * H[2];
    if (!sunUp) {
        float maz = (A + g_p[P_MOON_AZ].v - g_sp.k * g_p[P_MOON_DRIFT].v * (float)(f - 0.5)) / D, mel = g_p[P_MOON_EL].v / D;
        T[0] = cosf(mel) * sinf(maz); T[1] = sinf(mel); T[2] = cosf(mel) * cosf(maz);
    }
    float l = sqrtf(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
    for (int j = 0; j < 3; ++j) T[j] /= l > 1e-6f ? l : 1;
    *kind = sunUp ? 1.0f : 2.0f;
}
static void SunDbgDraw(ID3D11DeviceContext* ctx, float aspect)
{
    if (!g_sdPs && !g_sdTried) {
        g_sdTried = true;
        ID3DBlob *code = 0, *err = 0;
        if (g_compile && SUCCEEDED(g_compile(kSunDbgSrc, strlen(kSunDbgSrc), "sundebug", 0, 0, "ps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_sdPs);
        else Log("sun debug shader: %s", err ? (const char*)err->GetBufferPointer() : "failed");
        if (code) code->Release();
        if (err) err->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 360 * 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, 0, &g_sdCb);
        float rect[4] = { -1, 1, 2, 2 };
        D3D11_SUBRESOURCE_DATA sd = { rect };
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_IMMUTABLE; bd.CPUAccessFlags = 0;
        g_dev->CreateBuffer(&bd, &sd, &g_sdRect);
    }
    int cur = g_frame & 1, last = cur ^ 1;
    ID3D11ShaderResourceView* cam = g_camFrame[cur] == g_frame + 1 ? g_camRec[cur].srv : g_camFrame[last] == g_frame || g_camFrame[last] == g_frame + 1 ? g_camRec[last].srv : 0;
    if (!g_sdPs || !g_sdCb || !g_sdRect || !cam) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_sdCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    float* c = (float*)m.pData;
    memset(c, 0, 360 * 16);
    for (int j = 0; j < 3; ++j) { c[j] = -g_sunOurs[j]; c[4 + j] = -g_ssSunGame[j]; }
    c[3] = g_sp.moon ? 2.0f : 1.0f; c[7] = g_sp.wasDay ? 1.0f : 2.0f; c[8] = aspect;
    for (int i = 0; i < 96; ++i) SpOursAt(i * 0.25, c + (8 + i) * 4, c + (8 + i) * 4 + 3);
    memcpy(c + 104 * 4, g_sp.gs, sizeof(g_sp.gs));
    memcpy(c + 232 * 4, g_sp.gd, sizeof(g_sp.gd));
    if (g_coronaGameKnown) { for (int j = 0; j < 3; ++j) c[12 + j] = -g_coronaGame[j]; c[15] = c[7]; }
    ctx->Unmap(g_sdCb, 0);
    ID3D11Buffer* sCb = 0;
    ctx->PSGetConstantBuffers(0, 1, &sCb);
    ctx->VSSetConstantBuffers(13, 1, &g_sdRect);
    oPSSetShader(ctx, g_sdPs, 0, 0);
    ctx->PSSetShaderResources(0, 1, &cam);
    ctx->PSSetShaderResources(1, 1, &g_panoSrv[g_panoLast]);
    ctx->PSSetShaderResources(2, 1, &g_probeSrv);   // 1.46
    ctx->PSSetConstantBuffers(0, 1, &g_sdCb);
    oDraw(ctx, 4, 0);
    { ID3D11ShaderResourceView* no[2] = {}; ctx->PSSetShaderResources(1, 2, no); }
    ctx->PSSetConstantBuffers(0, 1, &sCb);
    if (sCb) sCb->Release();
    ctx->VSSetConstantBuffers(13, 1, &g_cbText);
    oPSSetShader(ctx, g_psText, 0, 0);   // (the caller binds the text's texture again)
}

// 1.35: the game's sun / moon sprite (an effect's quad reading the sun's visibility at t1): not drawn while ours shine
// (our sky draws our sun's and moon's discs)
static bool SunSpriteSkip(ID3D11DeviceContext* ctx)
{
    void* ps = g_lastPS;
    if (!ps || (ps != g_flareSh[6] && ps != g_flareSh[7]) || !g_sunOccRes || !g_sunActive || g_sunG >= 0.5f || !Immediate(ctx)) return false;
    ID3D11ShaderResourceView* v = 0;
    ctx->PSGetShaderResources(1, 1, &v);
    if (!v) return false;
    ID3D11Resource* r = 0;
    v->GetResource(&r);
    bool sun = r == g_sunOccRes;
    if (r) r->Release();
    v->Release();
    if (sun && !g_spriteSkips++) Log("sun flare: the game's sun / moon sprite not drawn (ours instead)");
    return sun;
}

static void FlareAfter(ID3D11DeviceContext* ctx)
{
    g_flareOn = false;
    ctx->VSSetShader(g_flareSavedVs, 0, 0);
    ctx->VSSetConstantBuffers(5, 1, &g_flareSavedCb);
    ctx->VSSetShaderResources(0, 1, &g_flareSavedSrv);
    if (g_flareSavedVs) g_flareSavedVs->Release();
    if (g_flareSavedCb) g_flareSavedCb->Release();
    if (g_flareSavedSrv) g_flareSavedSrv->Release();
    g_flareSavedVs = 0; g_flareSavedCb = 0; g_flareSavedSrv = 0;
}

static void SsFreeMaps()
{
    IUnknown* r[] = { g_ssNearSrv, g_ssNearDsv, g_ssNearTex };
    for (int k = 0; k < 3; ++k) if (r[k]) r[k]->Release();
    g_ssNearSrv = 0; g_ssNearDsv = 0; g_ssNearTex = 0;
    for (int f = 0; f < 3; ++f) {
        for (int b = 0; b < 2; ++b) {
            IUnknown* q[] = { g_ssFarSrv[f][b], g_ssFarDsv[f][b], g_ssFarTex[f][b] };
            for (int k = 0; k < 3; ++k) if (q[k]) q[k]->Release();
            g_ssFarSrv[f][b] = 0; g_ssFarDsv[f][b] = 0; g_ssFarTex[f][b] = 0;
        }
        g_ssFar[f].valid = g_ssFar[f].building = false;
    }
    IUnknown* c1[] = { g_ssC1Srv, g_ssC1Dsv, g_ssC1Tex, g_ssC2Srv, g_ssC2Dsv, g_ssC2Tex };
    for (int k = 0; k < 6; ++k) if (c1[k]) c1[k]->Release();
    g_ssC1Srv = 0; g_ssC1Dsv = 0; g_ssC1Tex = 0; g_ssC2Srv = 0; g_ssC2Dsv = 0; g_ssC2Tex = 0;
    g_ssRes = 0;
    g_ssDrawnFrame = 0;
}

static int SsWantedRes() { int c = (int)(g_p[P_SS + 2].v + 0.5f); return c <= 0 ? 2048 : c == 1 ? 3072 : 4096; }

static bool SsDepthTex(UINT w, UINT h, ID3D11Texture2D** t, ID3D11DepthStencilView** dsv, ID3D11ShaderResourceView** srv)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.SampleDesc.Count = 1; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
    dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, 0, t)) && SUCCEEDED(g_dev->CreateDepthStencilView(*t, &dv, dsv)) &&
           SUCCEEDED(g_dev->CreateShaderResourceView(*t, &sv, srv));
}

static bool SsEnsure()
{
    if (g_ssFailed || !g_dev || !g_compile) return false;
    int res = SsWantedRes();
    if (g_ssNearTex && g_ssRes != res) SsFreeMaps();
    if (!g_ssNearTex) {
        bool ok = SsDepthTex(res, res, &g_ssNearTex, &g_ssNearDsv, &g_ssNearSrv) && SsDepthTex(res, res, &g_ssC1Tex, &g_ssC1Dsv, &g_ssC1Srv) &&
                  SsDepthTex(res, res, &g_ssC2Tex, &g_ssC2Dsv, &g_ssC2Srv);
        for (int f = 0; f < 6 && ok; ++f) ok = SsDepthTex(res, res, &g_ssFarTex[f >> 1][f & 1], &g_ssFarDsv[f >> 1][f & 1], &g_ssFarSrv[f >> 1][f & 1]);
        if (!ok) {
            Log("sun shadows: %d^2 x %d maps failed", res, kSsCasc);
            SsFreeMaps();
            if (res > 2048) { g_p[P_SS + 2].v = 0; return false; }   // try the smallest next time
            g_ssFailed = true;
            return false;
        }
        g_ssRes = res;
        Log("sun shadows: maps %d^2: 0, moving things 1 2, kept 1 2 3 twice (%.0f MB)", res, res * (double)res * 4 * 9 / 1048576.0);
    }
    if (!g_ssRs) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;                 // casters nearer the sun than the box: flattened onto it, still casting
        rd.SlopeScaledDepthBias = 1.0f; rd.DepthBiasClamp = 0.0002f;
        g_dev->CreateRasterizerState(&rd, &g_ssRs);
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
        g_dev->CreateDepthStencilState(&dd, &g_ssDs);
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(g_ssCbData); bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, 0, &g_ssCb);
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        g_dev->CreateSamplerState(&sd, &g_ssCmp);
        if (!g_ssRs || !g_ssDs || !g_ssCb || !g_ssCmp) { Log("sun shadows: setup failed"); g_ssFailed = true; return false; }
    }
    return true;
}

// a cascade's view for the vertex shaders whose view sits at `off` (up to date with the cascade)
static ID3D11Buffer* SsVpCbFor(ID3D11DeviceContext* ctx, int off, int k)
{
    if (off < 0 || off + 64 > 1024) return 0;
    SsVpCb* c = 0;
    for (int i = 0; i < g_ssVpN; ++i) if (g_ssVp[i].off == off) { c = &g_ssVp[i]; break; }
    if (!c) {
        if (g_ssVpN >= 16) return 0;
        c = &g_ssVp[g_ssVpN++];
        memset(c, 0, sizeof(*c));
        c->off = off;
    }
    if (!c->b[k]) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 1024; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &c->b[k]))) return 0;
        c->ver[k] = 0;
    }
    if (c->ver[k] != g_ssVer[k]) {
        static char buf[1024];
        memset(buf, 0, sizeof(buf));
        memcpy(buf + off, g_ssPack[k], 64);
        ctx->UpdateSubresource(c->b[k], 0, 0, buf, 0, 0);
        c->ver[k] = g_ssVer[k];
    }
    return c->b[k];
}

// ---- the camera and the sun, read back
// (in the sun pass, the immediate context: this frame's camera record and the game's light direction copied)
static void SsReadBack(ID3D11DeviceContext* ctx);
static void SsNote(ID3D11DeviceContext* ctx, ID3D11Texture2D* camRec, int oKeyDir, int oKeyCol)
{
    if (!camRec) return;   // (also with our sun maps off: the day / night state switches the lamps)
    static unsigned last;
    if (last == g_frame + 1) return;
    last = g_frame + 1;
    // (1.24b: read here too: at night the shadow pass of ours does not run, and the day / night state must still follow
    // the game's key light - 1.24 stayed in "night" once it got there)
    SsReadBack(ctx);
    SsStage& s = g_ssSt[g_frame & 3];
    if (!s.cam) {
        D3D11_TEXTURE2D_DESC d;
        camRec->GetDesc(&d);
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&d, 0, &s.cam))) return;
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &s.sun))) return;
    }
    ctx->CopyResource(s.cam, camRec);
    s.hasSun = false;
    ID3D11Buffer* cb0 = 0;
    if (oKeyDir >= 0) ctx->PSGetConstantBuffers(0, 1, &cb0);
    if (cb0) {
        D3D11_BUFFER_DESC bd;
        cb0->GetDesc(&bd);
        if ((UINT)(oKeyDir + 1) * 16 <= bd.ByteWidth) {
            D3D11_BOX box = { (UINT)oKeyDir * 16, 0, 0, (UINT)oKeyDir * 16 + 16, 1, 1 };
            ctx->CopySubresourceRegion(s.sun, 0, 0, 0, 0, cb0, 0, &box);
            s.hasSun = true;
            s.hasCol = false;
            if (oKeyCol >= 0 && (UINT)(oKeyCol + 1) * 16 <= bd.ByteWidth) {
                D3D11_BOX cbox = { (UINT)oKeyCol * 16, 0, 0, (UINT)oKeyCol * 16 + 16, 1, 1 };
                ctx->CopySubresourceRegion(s.sun, 0, 16, 0, 0, cb0, 0, &cbox);
                s.hasCol = true;
            }
        }
        cb0->Release();
    }
    s.frame = g_frame + 1;
}

static void SsReadBack(ID3D11DeviceContext* ctx)
{
    for (int pass = 0; pass < 4; ++pass) {
        // oldest first, so the newest ends up kept
        int best = -1;
        for (int k = 0; k < 4; ++k)
            if (g_ssSt[k].frame && g_ssSt[k].frame <= g_frame && (best < 0 || g_ssSt[k].frame < g_ssSt[best].frame)) best = k;
        if (best < 0) return;
        SsStage& s = g_ssSt[best];
        D3D11_MAPPED_SUBRESOURCE m;
        if (ctx->Map(s.cam, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) != S_OK) return;   // not done: newer ones neither
        const float* row0 = (const float*)m.pData;
        const float* row1 = (const float*)((const char*)m.pData + m.RowPitch);
        float p[3] = { row0[12], row0[13], row0[14] };
        float aim[4] = { row1[0], row1[1], row1[2], row1[3] };
        ctx->Unmap(s.cam, 0);
        if (p[0] == p[0] && fabsf(p[0]) < 1e6f) {
            unsigned f = s.frame;
            if (g_ssCamKnown && f > g_ssCamF && f - g_ssCamF < 12) {
                float inv = 1.0f / (float)(f - g_ssCamF), v[3], sp = 0;
                for (int j = 0; j < 3; ++j) { v[j] = (p[j] - g_ssCamP[j]) * inv; sp += v[j] * v[j]; }
                if (sp > 25.0f * 25.0f) memset(v, 0, sizeof(v));    // a jump (a cut, a reset): no speed from it
                for (int j = 0; j < 3; ++j) g_ssCamV[j] = g_ssCamV[j] * 0.5f + v[j] * 0.5f;
            } else memset(g_ssCamV, 0, sizeof(g_ssCamV));
            memcpy(g_ssCamP, p, 12);
            g_ssCamF = f;
            g_ssCamKnown = true;
            float fx = aim[0] - p[0], fz = aim[2] - p[2], fl = sqrtf(fx * fx + fz * fz);
            if (aim[3] > 0.5f && fl > 0.5f) { g_ssFwd[0] = fx / fl; g_ssFwd[1] = 0; g_ssFwd[2] = fz / fl; g_ssFwdKnown = true; }
            else {
                float vl = sqrtf(g_ssCamV[0] * g_ssCamV[0] + g_ssCamV[2] * g_ssCamV[2]);
                if (vl > 0.05f) { g_ssFwd[0] = g_ssCamV[0] / vl; g_ssFwd[1] = 0; g_ssFwd[2] = g_ssCamV[2] / vl; g_ssFwdKnown = true; }
            }
        }
        if (s.hasSun && ctx->Map(s.sun, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
            const float* d = (const float*)m.pData;
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 0.5f && l < 2.0f) {
                if (!g_ssSunKnown) Log("sun shadows: sun direction %.3f %.3f %.3f (light -> scene)", d[0] / l, d[1] / l, d[2] / l);
                for (int j = 0; j < 3; ++j) g_ssSun[j] = d[j] / l;
                g_ssSunKnown = true;
                {   // 1.26: how the game's sun moves (smoothly, or in steps), logged every 10 s while it moves
                    static float last[3]; static bool have; static unsigned frames, steps; static double total, biggest; static DWORD next;
                    if (have) {
                        float cx = last[1] * g_ssSun[2] - last[2] * g_ssSun[1], cy = last[2] * g_ssSun[0] - last[0] * g_ssSun[2],
                              cz = last[0] * g_ssSun[1] - last[1] * g_ssSun[0];
                        float sn = sqrtf(cx * cx + cy * cy + cz * cz);
                        double a = asin(sn > 1 ? 1 : sn) * 57.29578;
                        if (a > 2e-4) { ++steps; total += a; if (a > biggest) biggest = a; }
                    }
                    memcpy(last, g_ssSun, 12); have = true; ++frames;
                    DWORD now = GetTickCount();
                    if (!next) next = now + 10000;
                    if ((LONG)(now - next) >= 0) {
                        if (steps) Log("sun motion (%u frames): %.3f deg in %u steps, largest %.3f deg; the sun %.1f deg up", frames, total, steps, biggest,
                                       asin(-g_ssSun[1] > 1 ? 1 : -g_ssSun[1] < -1 ? -1 : -g_ssSun[1]) * 57.29578);
                        frames = steps = 0; total = biggest = 0; next = now + 10000;
                    }
                }
                // 1.29 / 1.31: our sun's (and moon's) path from the game's; our maps follow ours
                memcpy(g_ssSunGame, g_ssSun, 12);
                SunPathStep(g_ssSunGame, s.hasCol ? d + 4 : 0);
                if (g_sunActive && g_sunG < 0.5f) {   // (ours: the sun, our moon; else the game's: its moon)
                    memcpy(g_ssSun, g_sunOurs, 12);
                    if (g_ssSun[1] > -0.01745f) {   // (the maps' light at least 1 deg up: its light ~0 there)
                        float h = sqrtf(g_ssSun[0] * g_ssSun[0] + g_ssSun[2] * g_ssSun[2]) + 1e-9f, c = 0.99985f / h;
                        g_ssSun[0] *= c; g_ssSun[2] *= c; g_ssSun[1] = -0.01745f;
                    }
                }
                {   // a big turn of the maps' sun (ours <-> the game's, a time jump): the kept maps drawn anew, not shifted
                    static float lastSun[3]; static bool have;
                    if (have && lastSun[0] * g_ssSun[0] + lastSun[1] * g_ssSun[1] + lastSun[2] * g_ssSun[2] < 0.9986f)   // (3 deg)
                        for (int f = 0; f < 3; ++f) { g_ssFar[f].valid = false; g_ssFar[f].building = false; }
                    memcpy(lastSun, g_ssSun, 12); have = true;
                }
            }
            if (s.hasCol) {   // day / night from the key light (with hysteresis: no flicker at dusk)
                float lum = 0.2126f * d[4] + 0.7152f * d[5] + 0.0722f * d[6];
                float day = (lum - 0.5f) / 2;
                g_ssDay = day < 0 ? 0 : day > 1 ? 1 : day;
                bool wasNight = g_ssNight, wasFull = g_ssFullDay;
                bool dark = g_ssDay < 0.05f && !(g_sunActive && g_sunW > 0.005f) && g_p[P_MOON_SHADOWS].v < 0.5f;   // (MoonShadows: our maps at night too)
                if (dark) { if (!g_ssNightSince) g_ssNightSince = g_frame; if (g_frame - g_ssNightSince > 60) g_ssNight = true; }
                else { g_ssNightSince = 0; if (g_ssDay > 0.15f || (g_sunActive && g_sunW > 0.005f)) g_ssNight = false; }
                if (g_ssDay > 0.97f) { if (!g_ssDaySince) g_ssDaySince = g_frame; if (g_frame - g_ssDaySince > 60) g_ssFullDay = true; }
                else { g_ssDaySince = 0; if (g_ssDay < 0.9f) g_ssFullDay = false; }
                if (wasNight != g_ssNight || wasFull != g_ssFullDay)
                    Log("day / night: key light %.2f (day %.2f): %s", lum, g_ssDay, g_ssNight ? "NIGHT: our sun maps off (the game's)" :
                        g_ssFullDay ? "FULL DAY: lamps off" : "dusk / dawn: both on");
                if (wasNight && !g_ssNight) for (int f = 0; f < 3; ++f) { g_ssFar[f].valid = false; g_ssFar[f].building = false; }   // stale: drawn anew
                g_sunMoonMaps = g_p[P_MOON_SHADOWS].v > 0.5f && !g_ssNight;
            }
            ctx->Unmap(s.sun, 0);
        }
        s.frame = 0;
    }
}

static float SsHalf(int k)
{
    static const float div[4] = { 20, 6, 3, 1 };
    return g_p[P_SS + 3].v / div[k];
}

// the map's right and up for a sun direction (light -> scene)
static void SsBasis(const float* L, float* R, float* U)
{
    float up[3] = { 0, 1, 0 };
    if (fabsf(L[1]) > 0.995f) { up[0] = 0; up[1] = 0; up[2] = 1; }
    R[0] = up[1] * L[2] - up[2] * L[1]; R[1] = up[2] * L[0] - up[0] * L[2]; R[2] = up[0] * L[1] - up[1] * L[0];
    float rl = sqrtf(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
    for (int j = 0; j < 3; ++j) R[j] /= rl;
    U[0] = L[1] * R[2] - L[2] * R[1]; U[1] = L[2] * R[0] - L[0] * R[2]; U[2] = L[0] * R[1] - L[1] * R[0];
}

// cascade k around C for the sun L: square, along the sun, the texel grid fixed in the world: its rows (x, y, depth)
// and its sizes (half, texel, depth per metre, 1 / half)
static void SsRows(int k, const float* C, const float* L, float* r, float* sc)
{
    float R[3], U[3];
    SsBasis(L, R, U);
    float h = SsHalf(k), tex = 2 * h / g_ssRes;
    float cr = floorf((C[0] * R[0] + C[1] * R[1] + C[2] * R[2]) / tex) * tex;
    float cu = floorf((C[0] * U[0] + C[1] * U[1] + C[2] * U[2]) / tex) * tex;
    float cl = floorf((C[0] * L[0] + C[1] * L[1] + C[2] * L[2]) / 16) * 16;
    for (int j = 0; j < 3; ++j) { r[j] = R[j] / h; r[4 + j] = U[j] / h; r[8 + j] = L[j] / (2 * kSsDepthR); }
    r[3] = -cr / h; r[7] = -cu / h; r[11] = (kSsDepthR - cl) / (2 * kSsDepthR);
    sc[0] = h; sc[1] = tex; sc[2] = 1.0f / (2 * kSsDepthR); sc[3] = 1.0f / h;
}
// what the vertex shaders draw cascade k with: rows in the game's packed form, (z scale, z bias, w scale, w bias) = (1, 0, 0, 1)
static void SsSetPack(int k, const float* r)
{
    memcpy(g_ssPack[k], r, 48);
    g_ssPack[k][12] = 1; g_ssPack[k][13] = 0; g_ssPack[k][14] = 0; g_ssPack[k][15] = 1;
    ++g_ssVer[k];
}

static void SsGlobals()
{
    float R[3], U[3];
    SsBasis(g_ssSun, R, U);
    float* t = g_ssCbData + 64;
    t[0] = g_ssSun[0]; t[1] = g_ssSun[1]; t[2] = g_ssSun[2]; t[3] = 1;          // SL
    t[4] = R[0]; t[5] = R[1]; t[6] = R[2]; t[7] = 0;                             // SR
    t[8] = U[0]; t[9] = U[1]; t[10] = U[2]; t[11] = 0;                           // SU
    float pen = 0.00925f * g_p[P_SS + 1].v;                                      // penumbra per metre (the sun's 0.53 deg)
    t[12] = pen; t[13] = g_p[P_SS + 5].v * 0.01f; t[14] = g_p[P_SS + 6].v * pen; t[15] = g_p[P_SS + 4].v;   // SP
    t[16] = (float)g_ssRes; t[17] = 1.0f / g_ssRes; t[19] = (float)kSsCasc;                                // SP2
    t[26] = kSsShiftReach;                                                                                  // SP4.z
    // SD: per cascade, how far its shadows have moved per metre of caster distance since its map was drawn (its sun ->
    // the sun now), along that map's right and up; length in w. 0 for the nearest (drawn every frame with the sun now).
    float* sd = g_ssCbData + 92;
    memset(sd, 0, 16 * sizeof(float));
    for (int f = 0; f < 3; ++f) {
        const SsFar& F = g_ssFar[f];
        if (!F.valid) continue;
        float Rk[3], Uk[3], D[3];
        SsBasis(F.sun, Rk, Uk);
        for (int j = 0; j < 3; ++j) D[j] = F.sun[j] - g_ssSun[j];
        float* o = sd + (f + 1) * 4;
        o[0] = D[0] * Rk[0] + D[1] * Rk[1] + D[2] * Rk[2];
        o[1] = D[0] * Uk[0] + D[1] * Uk[1] + D[2] * Uk[2];
        o[3] = sqrtf(o[0] * o[0] + o[1] * o[1]);
    }
}

static void SsUpload(ID3D11DeviceContext* ctx)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_ssCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, g_ssCbData, sizeof(g_ssCbData)); ctx->Unmap(g_ssCb, 0); }
}

// the first draw into the game's shadow map this frame: 0 cleared and placed; the kept ones: swapped when built, a new
// build started when due; 1 as used = its kept map (the cars are drawn on it)
static void SsFrameStart(ID3D11DeviceContext* ctx)
{
    SsFlush(ctx);   // (anything still recorded: drawn into the maps as they are now)
    g_ssFrame = g_frame + 1;
    g_ssLive = false;
    if (g_p[P_SS].v < 0.5f || !SsEnsure()) return;
    SsReadBack(ctx);
    if (!g_ssSunKnown || !g_ssCamKnown) return;
    float lag = (float)(g_frame + 1) - (float)g_ssCamF;                       // frames since the camera sample
    if (lag < 0) lag = 0; else if (lag > 6) lag = 6;
    float cam[3], fwd[3] = { 0, 0, 0 };
    for (int j = 0; j < 3; ++j) cam[j] = g_ssCamP[j] + g_ssCamV[j] * lag;
    if (g_ssFwdKnown) memcpy(fwd, g_ssFwd, 12);
    {
        float C[3];
        for (int j = 0; j < 3; ++j) C[j] = cam[j] + fwd[j] * SsHalf(0) * 0.3f;   // a little ahead
        SsRows(0, C, g_ssSun, g_ssCbData, g_ssCbData + 48);
        SsSetPack(0, g_ssCbData);
    }
    ctx->ClearDepthStencilView(g_ssNearDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    SsMeshWork(ctx);   // meshes' radii read back (caster culling)
    // instances first seen two frames ago and not seen since: moving things (their kept-map image, if any, must go)
    {
        int b = g_frame & 1;   // (the ring of two frames ago: reused from now on)
        for (int i = 0; i < g_ssNewN[b]; ++i) {
            unsigned long long key = g_ssNew[b][i];
            unsigned h = (unsigned)(key ^ (key >> 29)) * 2654435761u;
            for (int j = 0; j < 16; ++j) {
                SsInst& e = g_ssInst[(h + j) & 65535];
                if (e.key == key) { if (!e.still && e.last + 1 < g_frame) g_ssMovingFrame = g_frame; break; }
                if (!e.key) break;
            }
        }
        g_ssNewN[b] = 0;
    }
    // degrees the sun may move before a kept map is drawn again (1.27): as far as the shift SD keeps its shadows exact
    // (shadowtest4: ~5% of the shadows' move stays, 1-2 map texels at ~0.5 deg with the sun 8 deg up, 1-2 deg at 25):
    // 2.9 deg x sin(the sun's height), x1.25 / x1.5 for the wider maps; never under the old 0.1 / 0.2 / 0.35, never
    // over 1.5 / 2 / 3
    float kSunStep[3];
    {
        static const float kMin[3] = { 0.1f, 0.2f, 0.35f }, kMax[3] = { 1.5f, 2.0f, 3.0f }, kMul[3] = { 1.0f, 1.25f, 1.5f };
        float sinEl = -g_ssSun[1] > 0 ? -g_ssSun[1] : 0;
        for (int f = 0; f < 3; ++f) {
            float s = 2.9f * sinEl * kMul[f];
            kSunStep[f] = g_ssSunStepFixed > 0 ? g_ssSunStepFixed : s < kMin[f] ? kMin[f] : s > kMax[f] ? kMax[f] : s;
            if (g_ssSunStepFixed <= 0) kSunStep[f] *= g_p[185].v;   // (1.53: ShadowSunStep: the shift between redraws stretched, the redraw jumped)
        }
    }
    g_ssBudget = (int)g_p[P_SS + 7].v;
    for (int f = 0; f < 3; ++f) {
        SsFar& F = g_ssFar[f];
        int k = f + 1;
        if (F.building) {
            ++F.buildFrames;
            // done when a whole frame added nothing (every still instance seen is in), or after 45 frames anyway
            if ((F.buildFrames >= 3 && !F.added && !F.budgetHit) || F.buildFrames >= 45) {
                memcpy(g_ssCbData + k * 12, F.bRows, 48);
                memcpy(g_ssCbData + 48 + k * 4, F.bSc, 16);
                memcpy(F.C, F.bC, 12); memcpy(F.sun, F.bSun, 12);
                F.cur ^= 1; F.valid = true; F.building = false; F.built = g_frame;
                SsSetPack(1 + 2 * f, F.bRows);   // in use (the moving things and newly still ones drawn with it)
                ++g_ssStat.rebuilds;
            }
        }
        F.added = 0; F.budgetHit = false;
        float h = SsHalf(k), D[3];
        for (int j = 0; j < 3; ++j) D[j] = cam[j] + fwd[j] * h * 0.3f;
        bool due = !F.valid, sunMoved = false;
        int why = 0;
        if (F.valid) {
            float dx = D[0] - F.C[0], dy = D[1] - F.C[1], dz = D[2] - F.C[2], q = 0.25f * h;
            if (dx * dx + dz * dz > q * q || fabsf(dy) > q) { due = true; why = 1; }
            float cs = F.sun[0] * g_ssSun[0] + F.sun[1] * g_ssSun[1] + F.sun[2] * g_ssSun[2];
            sunMoved = cs < cosf(kSunStep[f] * 0.0174533f);
            unsigned age = g_frame - F.built;
            if (!due && sunMoved) { due = true; why = 2; }
            if (!due && g_ssMovingFrame > F.built && age > (f == 2 ? 240u : 90u)) { due = true; why = 3; }   // a moving thing seen since: its old image goes
            if (!due && age > 1800) { due = true; why = 4; }
        }
        if (F.building || !due) continue;
        ++g_ssStat.why[why];
        memcpy(F.bC, D, 12);
        memcpy(F.bSun, g_ssSun, 12);   // (1.28: always the sun of now: SD keeps it right till the next)
        SsRows(k, F.bC, F.bSun, F.bRows, F.bSc);
        SsSetPack(2 + 2 * f, F.bRows);
        ctx->ClearDepthStencilView(g_ssFarDsv[f][F.cur ^ 1], D3D11_CLEAR_DEPTH, 1.0f, 0);
        SsKeyClear(g_ssKeys[f][F.cur ^ 1]);
        F.building = true; F.buildFrames = 0;
    }
    // the moving things of this frame
    ctx->ClearDepthStencilView(g_ssC1Dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->ClearDepthStencilView(g_ssC2Dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    SsGlobals();
    SsUpload(ctx);
    g_ssLive = true;
    g_ssDrawnFrame = g_frame + 1;
    static bool told;
    if (!told) {
        told = true;
        Log("sun shadows: live, cascades %.1f / %.1f / %.1f / %.1f m (half), camera %.1f %.1f %.1f, speed %.2f m/frame; 1-3 kept", SsHalf(0), SsHalf(1),
            SsHalf(2), SsHalf(3), cam[0], cam[1], cam[2], sqrtf(g_ssCamV[0] * g_ssCamV[0] + g_ssCamV[2] * g_ssCamV[2]));
    }
}

static void SsStatTick()
{
    if (g_ssStatFrame == g_frame + 1) return;
    g_ssStatFrame = g_frame + 1;
    for (int c = 0; c < 3; ++c) {
        if (!g_ssGameVpFrame[c] || g_frame <= g_ssGameVpFrame[c] + 2) continue;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11DeviceContext* ic = 0;
        g_dev->GetImmediateContext(&ic);
        if (ic && ic->Map(g_ssGameVp[c], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK) {
            const float* r = (const float*)m.pData;
            float sx = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            float half = sx > 0 ? 1 / sx : 0;
            if (fabsf(half - g_ssGameHalf[c]) > g_ssGameHalf[c] * 0.2f + 0.5f)
                Log("sun shadows: the game's cascade %d is %.0f m across (half %.1f); ours: %.1f / %.1f / %.1f / %.1f%s", c, half * 2, half,
                    SsHalf(0), SsHalf(1), SsHalf(2), SsHalf(3), c == 0 && half > 0 && half >= 2.5f * SsHalf(0) ? " (its middle one not drawn into our nearest)" : "");
            g_ssGameHalf[c] = half;
            ic->Unmap(g_ssGameVp[c], 0);
            g_ssGameVpFrame[c] = 0;
        }
        if (ic) ic->Release();
    }
    if (g_frame % 900 == 0 && g_p[P_SS].v > 0.5f && (g_ssStat.game[0] | g_ssStat.game[1] | g_ssStat.game[2])) {
        Log("sun shadows (last 900 frames, per frame): the game's cascades %u / %u / %u draws; ours near %u, far %u (cut-outs %u, depth only %u); "
            "far rebuilt %u times (over frames, swapped); skipped: no view %u, topology %u, out of reach %u; meshes known %u; CPU %.2f ms; "
            "the game's own draws left out %u (SkipGameShadows); recorded %u, drawn at once %u, flushes %u; kept maps drawn again for: "
            "a start %u, the camera %u, the sun %u, a moving thing %u, age %u; slow movers %d",
            g_ssStat.game[0] / 900, g_ssStat.game[1] / 900, g_ssStat.game[2] / 900, g_ssStat.drawsNear / 900, g_ssStat.drawsFar / 900,
            g_ssStat.cut / 900, g_ssStat.depthOnly / 900, g_ssStat.rebuilds, g_ssStat.noVs / 900, g_ssStat.topo / 900, g_ssCulled / 900,
            g_ssMeshKnown, g_ssCpuAcc / 900, g_ssGameSkipped / 900, g_ssRecStat[0] / 900, g_ssRecStat[1] / 900, g_ssRecStat[2] / 900,
            g_ssStat.why[0], g_ssStat.why[1], g_ssStat.why[2], g_ssStat.why[3], g_ssStat.why[4], g_ssMoverN);
        g_ssCulled = 0; g_ssGameSkipped = 0; memset(g_ssRecStat, 0, sizeof(g_ssRecStat));
        memset(&g_ssStat, 0, sizeof(g_ssStat));
        g_ssCpuAcc = 0;
    }
}

static bool PsIsCut(ID3D11PixelShader* ps);
// after every game draw: a draw into the game's sun shadow map is drawn once more into the cascades of ours it reaches
// ---- 1.27: our replays recorded, drawn after the game's shadow pass grouped by map
// On the user's GPU every change of the bound depth target costs ~9 us of GPU time (ltest/rtswitch.cpp: the same for
// D16 / D32, with or without a view to read it, slices of one array; a viewport change or the same target bound again
// cost nothing). Drawn as the game drew, each replay switched twice (ours, then the game's again): 100-450 replays a
// frame cost 2-12 ms. Recorded instead: the draw's state is kept, and the constants it reads are copied at the game's
// draw (from our CPU copies of the game's constant buffers, worldedit.inc) into buffers of ours; after the pass (the
// first draw not into the game's shadow map; or the mask pass, a new frame's maps, Present) all are drawn, one map after
// another: ~9 switches a frame. (Copying the game's buffers on the GPU instead costs ~4-5 us each: rtswitch.) A draw
// whose state cannot be kept (a geometry shader, a dynamic vertex buffer, constants we have no copy of yet) is drawn at
// once, as before.
static const int kSsRecMax = 1024;
struct SsRec {
    DrawCall d; VsInfo* e; unsigned short tgts;   // bit: 0 nearest, 1 / 2 moving things (1, 2), 3 + f kept f, 6 + f kept f being drawn again
    ID3D11InputLayout* il; ID3D11Buffer* vb[8]; UINT vbS[8], vbO[8]; ID3D11Buffer* ib; DXGI_FORMAT ibF; UINT ibO; D3D11_PRIMITIVE_TOPOLOGY top;
    ID3D11VertexShader* vs; ID3D11Buffer* vcb[14]; ID3D11ShaderResourceView* vsrv[16];
    ID3D11PixelShader* ps; ID3D11ShaderResourceView* psrv[8]; ID3D11SamplerState* psmp[8]; ID3D11Buffer* pcb[4];
};
static SsRec g_ssRec[kSsRecMax];
static int g_ssRecN;
static const UINT kSsPoolSize[5] = { 256, 1024, 4096, 16384, 65536 };
struct SsPool { ID3D11Buffer* b[kSsRecMax * 2]; int used; };
static SsPool g_ssPool[5];                       // our copies of the constants, per size (used once a flush, kept)

static void SsRecFree(SsRec& r)
{
    IUnknown* one[] = { r.il, r.ib, r.vs, r.ps };
    for (int i = 0; i < 4; ++i) if (one[i]) one[i]->Release();
    for (int i = 0; i < 8; ++i) { if (r.vb[i]) r.vb[i]->Release(); if (r.psrv[i]) r.psrv[i]->Release(); if (r.psmp[i]) r.psmp[i]->Release(); }
    for (int i = 0; i < 14; ++i) if (r.vcb[i]) r.vcb[i]->Release();
    for (int i = 0; i < 16; ++i) if (r.vsrv[i]) r.vsrv[i]->Release();
    for (int i = 0; i < 4; ++i) if (r.pcb[i]) r.pcb[i]->Release();
    memset(&r, 0, sizeof(r));
}

// a buffer holding what constant buffer b holds now (held: released with the record), or 0 (not known: drawn at once)
static ID3D11Buffer* SsCbKeep(ID3D11DeviceContext* ctx, ID3D11Buffer* b)
{
    const unsigned char* data = 0;
    UINT size = 0;
    int st = WeCbNow(b, &data, &size);
    if (st == 2) { b->AddRef(); return b; }
    if (st != 1) return 0;
    int c = 0;
    while (c < 5 && kSsPoolSize[c] < size) ++c;
    if (c == 5 || g_ssPool[c].used >= kSsRecMax * 2) return 0;
    ID3D11Buffer*& pb = g_ssPool[c].b[g_ssPool[c].used];
    if (!pb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = kSsPoolSize[c]; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &pb))) { pb = 0; return 0; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(pb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return 0;
    memcpy(m.pData, data, size);
    ctx->Unmap(pb, 0);
    ++g_ssPool[c].used;
    pb->AddRef();
    return pb;
}

static void SsFlush(ID3D11DeviceContext* ctx);
// the game's draw recorded for maps tgts (false: its state cannot be kept)
static bool SsRecord(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned short tgts, ID3D11PixelShader* ps)
{
    if (!g_cbsCopy || e->vpSlot < 0 || e->vpSlot >= 14 || !((e->cbMask >> e->vpSlot) & 1) || (e->cbMask >> 14)) return false;
    if (g_ssRecN >= kSsRecMax) SsFlush(ctx);
    ID3D11GeometryShader* gs = 0;
    ctx->GSGetShader(&gs, 0, 0);
    if (gs) { gs->Release(); return false; }
    SsRec& r = g_ssRec[g_ssRecN];
    memset(&r, 0, sizeof(r));
    ctx->IAGetVertexBuffers(0, 8, r.vb, r.vbS, r.vbO);
    ctx->IAGetIndexBuffer(&r.ib, &r.ibF, &r.ibO);
    bool ok = true;
    for (int i = 0; i < 9 && ok; ++i) {   // a vertex or index buffer the game may rewrite before we draw: at once
        ID3D11Buffer* b = i < 8 ? r.vb[i] : r.ib;
        if (!b) continue;
        D3D11_BUFFER_DESC bd;
        b->GetDesc(&bd);
        if (bd.Usage == D3D11_USAGE_DYNAMIC) ok = false;
    }
    if (ok) {
        ID3D11Buffer* cbs[14] = {};
        ctx->VSGetConstantBuffers(0, 14, cbs);
        for (int i = 0; i < 14; ++i) {
            if (ok && cbs[i] && i != e->vpSlot && ((e->cbMask >> i) & 1)) { r.vcb[i] = SsCbKeep(ctx, cbs[i]); if (!r.vcb[i]) ok = false; }
            if (cbs[i]) cbs[i]->Release();
        }
    }
    if (ok && ps) {
        ID3D11Buffer* cbs[4] = {};
        ctx->PSGetConstantBuffers(0, 4, cbs);
        for (int i = 0; i < 4; ++i) {
            if (ok && cbs[i]) { r.pcb[i] = SsCbKeep(ctx, cbs[i]); if (!r.pcb[i]) ok = false; }
            if (cbs[i]) cbs[i]->Release();
        }
    }
    if (!ok) { SsRecFree(r); ++g_ssRecStat[1]; return false; }
    r.d = d; r.e = e; r.tgts = tgts;
    ctx->IAGetInputLayout(&r.il);
    ctx->IAGetPrimitiveTopology(&r.top);
    ctx->VSGetShader(&r.vs, 0, 0);
    if (e->srvMask) ctx->VSGetShaderResources(0, 16, r.vsrv);
    if (ps) { r.ps = ps; ps->AddRef(); ctx->PSGetShaderResources(0, 8, r.psrv); ctx->PSGetSamplers(0, 8, r.psmp); }
    ++g_ssRecN;
    ++g_ssRecStat[0];
    return true;
}

// the recorded replays drawn, map after map; the game's state as it was
static void SsFlush(ID3D11DeviceContext* ctx)
{
    if (!g_ssRecN) return;
    ++g_ssRecStat[2];
    ID3D11InputLayout* sIl = 0; ID3D11Buffer* sVb[8] = {}; UINT sVbS[8] = {}, sVbO[8] = {}; ID3D11Buffer* sIb = 0; DXGI_FORMAT sIbF = DXGI_FORMAT_UNKNOWN; UINT sIbO = 0;
    D3D11_PRIMITIVE_TOPOLOGY sTop; ID3D11VertexShader* sVs = 0; ID3D11Buffer* sVcb[14] = {}; ID3D11ShaderResourceView* sVsrv[16] = {};
    ID3D11PixelShader* sPs = 0; ID3D11ShaderResourceView* sPsrv[8] = {}; ID3D11SamplerState* sPsmp[8] = {}; ID3D11Buffer* sPcb[4] = {};
    ID3D11RasterizerState* sRs = 0; ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0;
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16;
    ctx->IAGetInputLayout(&sIl); ctx->IAGetVertexBuffers(0, 8, sVb, sVbS, sVbO); ctx->IAGetIndexBuffer(&sIb, &sIbF, &sIbO); ctx->IAGetPrimitiveTopology(&sTop);
    ctx->VSGetShader(&sVs, 0, 0); ctx->VSGetConstantBuffers(0, 14, sVcb); ctx->VSGetShaderResources(0, 16, sVsrv);
    ctx->PSGetShader(&sPs, 0, 0); ctx->PSGetShaderResources(0, 8, sPsrv); ctx->PSGetSamplers(0, 8, sPsmp); ctx->PSGetConstantBuffers(0, 4, sPcb);
    ctx->RSGetState(&sRs); ctx->RSGetViewports(&nVp, sVp); ctx->OMGetDepthStencilState(&sDs, &sRef); ctx->OMGetRenderTargets(8, sRtv, &sDsv);
    ctx->RSSetState(g_ssRs);
    ctx->OMSetDepthStencilState(g_ssDs, 0);
    D3D11_VIEWPORT q = { 0, 0, (float)g_ssRes, (float)g_ssRes, 0, 1 };
    ctx->RSSetViewports(1, &q);
    ID3D11DepthStencilView* dsvOf[9] = { g_ssNearDsv, g_ssC1Dsv, g_ssC2Dsv, g_ssFarDsv[0][g_ssFar[0].cur], g_ssFarDsv[1][g_ssFar[1].cur],
                                         g_ssFarDsv[2][g_ssFar[2].cur], g_ssFarDsv[0][g_ssFar[0].cur ^ 1], g_ssFarDsv[1][g_ssFar[1].cur ^ 1],
                                         g_ssFarDsv[2][g_ssFar[2].cur ^ 1] };
    static const int kPackOf[9] = { 0, 1, 3, 1, 3, 5, 2, 4, 6 };
    unsigned all = 0;
    for (int i = 0; i < g_ssRecN; ++i) all |= g_ssRec[i].tgts;
    for (int t = 0; t < 9; ++t) {
        if (!((all >> t) & 1) || !dsvOf[t]) continue;
        ctx->OMSetRenderTargets(0, 0, dsvOf[t]);
        ID3D11InputLayout* curIl = (ID3D11InputLayout*)1; ID3D11VertexShader* curVs = (ID3D11VertexShader*)1; ID3D11PixelShader* curPs = (ID3D11PixelShader*)1;
        for (int i = 0; i < g_ssRecN; ++i) {
            SsRec& r = g_ssRec[i];
            if (!((r.tgts >> t) & 1)) continue;
            ID3D11Buffer* vp = SsVpCbFor(ctx, r.e->vpOff, kPackOf[t]);
            if (!vp) continue;
            if (r.il != curIl) { ctx->IASetInputLayout(r.il); curIl = r.il; }
            ctx->IASetVertexBuffers(0, 8, r.vb, r.vbS, r.vbO);
            ctx->IASetIndexBuffer(r.ib, r.ibF, r.ibO);
            ctx->IASetPrimitiveTopology(r.top);
            if (r.vs != curVs) { ctx->VSSetShader(r.vs, 0, 0); curVs = r.vs; }
            ID3D11Buffer* cb[14];
            memcpy(cb, r.vcb, sizeof(cb));
            cb[r.e->vpSlot] = vp;
            ctx->VSSetConstantBuffers(0, 14, cb);
            if (r.e->srvMask) ctx->VSSetShaderResources(0, 16, r.vsrv);
            if (r.ps != curPs) { oPSSetShader(ctx, r.ps, 0, 0); curPs = r.ps; }
            if (r.ps) { ctx->PSSetShaderResources(0, 8, r.psrv); ctx->PSSetSamplers(0, 8, r.psmp); ctx->PSSetConstantBuffers(0, 4, r.pcb); }
            Replay(ctx, r.d);
            if (t >= 3) ++g_ssStat.drawsFar; else ++g_ssStat.drawsNear;
        }
    }
    // the game's state again
    ctx->IASetInputLayout(sIl); ctx->IASetVertexBuffers(0, 8, sVb, sVbS, sVbO); ctx->IASetIndexBuffer(sIb, sIbF, sIbO); ctx->IASetPrimitiveTopology(sTop);
    ctx->VSSetShader(sVs, 0, 0); ctx->VSSetConstantBuffers(0, 14, sVcb); ctx->VSSetShaderResources(0, 16, sVsrv);
    oPSSetShader(ctx, sPs, 0, 0); ctx->PSSetShaderResources(0, 8, sPsrv); ctx->PSSetSamplers(0, 8, sPsmp); ctx->PSSetConstantBuffers(0, 4, sPcb);
    ctx->RSSetState(sRs); ctx->RSSetViewports(nVp, sVp); ctx->OMSetDepthStencilState(sDs, sRef);
    UINT nRtv = 0;
    for (UINT i = 0; i < 8; ++i) if (sRtv[i]) nRtv = i + 1;
    ctx->OMSetRenderTargets(nRtv, sRtv, sDsv);
    IUnknown* one[] = { sIl, sIb, sVs, sPs, sRs, sDs, sDsv };
    for (int i = 0; i < 7; ++i) if (one[i]) one[i]->Release();
    for (int i = 0; i < 8; ++i) { if (sVb[i]) sVb[i]->Release(); if (sPsrv[i]) sPsrv[i]->Release(); if (sPsmp[i]) sPsmp[i]->Release(); if (sRtv[i]) sRtv[i]->Release(); }
    for (int i = 0; i < 14; ++i) if (sVcb[i]) sVcb[i]->Release();
    for (int i = 0; i < 16; ++i) if (sVsrv[i]) sVsrv[i]->Release();
    for (int i = 0; i < 4; ++i) if (sPcb[i]) sPcb[i]->Release();
    for (int i = 0; i < g_ssRecN; ++i) SsRecFree(g_ssRec[i]);
    g_ssRecN = 0;
    for (int c = 0; c < 5; ++c) g_ssPool[c].used = 0;
}

// 1.26: called before the game's draw (the state is the same; ours restored after). True: the game's own draw is left
// out (SkipGameShadows: the caster is ours alone, its shadow inside our widest cascade's inner part).
static bool SsAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    SsStatTick();
    if (!g_sunShadowTex) return false;
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(0, 0, &dsv);
    ID3D11Resource* res = 0;
    if (dsv) dsv->GetResource(&res);
    if (res) res->Release();
    // GPU time of the whole shadow pass (the game's and ours): from its first draw to the first draw after it
    bool shadowDraw = dsv && res == g_sunShadowTex;
    if (shadowDraw && g_ssPassFrame != g_frame + 1) { g_ssPassFrame = g_frame + 1; g_ssPassOpen = true; ProfMark(ctx, PR_SHPASS, 0); }
    else if (!shadowDraw && g_ssPassOpen) { g_ssPassOpen = false; SsFlush(ctx); ProfMark(ctx, PR_SHPASS, 1); }
    if (!shadowDraw || g_p[P_SS].v < 0.5f || g_ssFailed || g_ssNight) { if (dsv) dsv->Release(); return false; }
    D3D11_VIEWPORT vp; UINT nv = 1;
    ctx->RSGetViewports(&nv, &vp);
    if (!nv || vp.Height < 1) { dsv->Release(); return false; }
    CpuTimer cpu;
    LARGE_INTEGER q0;
    QueryPerformanceCounter(&q0);
    struct Acc { LARGE_INTEGER t; ~Acc() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); if (g_qpf.QuadPart) g_ssCpuAcc += (t1.QuadPart - t.QuadPart) * 1000.0 / g_qpf.QuadPart; } } acc = { q0 };
    int gc = (int)(vp.TopLeftY / vp.Height + 0.5f);
    if (gc < 0) gc = 0; else if (gc > 2) gc = 2;
    ++g_ssStat.game[gc];
    if (g_ssFrame != g_frame + 1) SsFrameStart(ctx);
    if (!g_ssLive) { dsv->Release(); return false; }
    ID3D11VertexShader* vs = 0;
    ctx->VSGetShader(&vs, 0, 0);
    VsInfo* e = VsFind(vs, false);
    if (vs) vs->Release();
    if (!e || e->vpSlot < 0 || e->vpSlot == e->wSlot || e->vpOff + 64 > 1024) { ++g_ssStat.noVs; dsv->Release(); return false; }
    if (!g_ssGameVpFrame[gc] && (g_frame % 300 == 150 || !g_ssGameHalf[gc])) {   // the game's cascade's view: its size
        if (!g_ssGameVp[gc]) { D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; g_dev->CreateBuffer(&bd, 0, &g_ssGameVp[gc]); }
        ID3D11Buffer* cb = 0;
        ctx->VSGetConstantBuffers(e->vpSlot, 1, &cb);
        if (cb && g_ssGameVp[gc]) {
            D3D11_BUFFER_DESC bd; cb->GetDesc(&bd);
            if ((UINT)e->vpOff + 64 <= bd.ByteWidth) {
                D3D11_BOX box = { (UINT)e->vpOff, 0, 0, (UINT)e->vpOff + 64, 1, 1 };
                ctx->CopySubresourceRegion(g_ssGameVp[gc], 0, 0, 0, 0, cb, 0, &box);
                g_ssGameVpFrame[gc] = g_frame;
            }
        }
        if (cb) cb->Release();
    }
    D3D11_PRIMITIVE_TOPOLOGY top;
    ctx->IAGetPrimitiveTopology(&top);
    if (top != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && top != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) { ++g_ssStat.topo; dsv->Release(); return false; }
    // which instance this is: still (seen in two frames running) or not (new, or moving: a new matrix every frame)
    unsigned long long mesh = 0;
    float T[3] = { 0, 0, 0 };
    bool placed = false;
    float scale = 1;
    unsigned long long key = WeInstKey(ctx, e, d, &mesh, T, &placed, &scale);
    SsInst* in = SsInstOf(key, !e->car);
    // its reach (placed instances of a known mesh): the targets its shadow cannot reach are left out
    float R = -1;
    SsMeshR* mr = 0;
    if (placed) {
        mr = SsMeshFind(mesh, true);
        if (mr) {
            if (mr->state == 3) R = mr->r * scale;
            else if (mr->state == 0) {
                if (g_ssMeshJobFrame != g_frame) { g_ssMeshJobFrame = g_frame; g_ssMeshJobs = 0; }
                if (g_ssMeshJobs < 4) { ++g_ssMeshJobs; SsMeshRequest(ctx, *mr, d); }
            }
        }
    }
    bool still = !e->car && in && in->still;
    if (still && placed && mr) {   // a slow mover's spot: a moving thing (1.28)
        if (!in->noted) { in->noted = true; SsMoverCheck(*mr, key, T, R); }
        if (g_ssMoverN && SsIsMover(mesh, T)) still = false;
    }
    // the game's own draw left out when its shadow cannot reach where the game's map is read (both placements of our widest)
    bool skip = g_p[P_SS_SKIP].v > 0.5f && R >= 0 && g_ssFar[2].valid && SsInsideWidest(g_ssCbData + 36, T, R) &&
                (!g_ssFar[2].building || SsInsideWidest(g_ssFar[2].bRows, T, R));
    if (skip) ++g_ssGameSkipped;
    // where it goes: up to 1 + 2 + 3 + 3 maps
    struct Tgt { ID3D11DepthStencilView* dsv; int pack; bool kept; int id; };   // id: SsRec::tgts
    Tgt tg[9];
    int nt = 0;
    // 1.40: our nearest also gets the game's wider cascades' casters whose shadow reaches it (a building outside the game's
    // nearest, 18 m across against our 27, cast into ours: its shadow was missing near the camera), each once a frame
    bool near0 = e->car ? gc <= 1 : gc == 0 || (gc == 1 && g_ssGameHalf[0] < 2.5f * SsHalf(0)) || (in && R >= 0);
    if (near0 && R >= 0 && !SsReaches(g_ssCbData, T, R)) { near0 = false; ++g_ssCulled; }
    if (near0 && in && !e->car) { if (in->nearF == g_frame + 1) near0 = false; else in->nearF = g_frame + 1; }
    if (near0) tg[nt++] = { g_ssNearDsv, 0, false, 0 };
    // kept f gets: 1 <- the game's nearest and middle, 2 <- its middle and widest, 3 <- its widest
    bool elig[3] = { gc <= 1, gc >= 1, gc == 2 };
    for (int f = 0; f < 3; ++f) {
        if (!elig[f]) continue;
        SsFar& F = g_ssFar[f];
        if (R >= 0) {   // out of reach of this cascade (in use and being built: both placements)
            bool inUse = F.valid && SsReaches(g_ssCbData + (f + 1) * 12, T, R), inBuild = F.building && SsReaches(F.bRows, T, R);
            if (!inUse && !inBuild) { ++g_ssCulled; continue; }
        }
        if (!still) {   // moving (or not yet known still): into this frame's moving-things map (1, 2), once a frame
            if (f < 2 && F.valid && in && in->dyn[f] != g_frame) { in->dyn[f] = g_frame; tg[nt++] = { f ? g_ssC2Dsv : g_ssC1Dsv, 1 + 2 * f, false, 1 + f }; }
            continue;
        }
        if (F.valid && !SsKeyHas(g_ssKeys[f][F.cur], key)) {   // newly still / newly seen: into the kept map in use, now
            if (g_ssBudget > 0) { --g_ssBudget; SsKeyAdd(g_ssKeys[f][F.cur], key); tg[nt++] = { g_ssFarDsv[f][F.cur], 1 + 2 * f, true, 3 + f }; }
        }
        if (F.building && !SsKeyHas(g_ssKeys[f][F.cur ^ 1], key)) {
            if (g_ssBudget > 0) { --g_ssBudget; SsKeyAdd(g_ssKeys[f][F.cur ^ 1], key); ++F.added; tg[nt++] = { g_ssFarDsv[f][F.cur ^ 1], 2 + 2 * f, true, 6 + f }; }
            else F.budgetHit = true;
        }
    }
    if (!nt) { dsv->Release(); return skip; }
    ID3D11PixelShader* ps = 0;
    ctx->PSGetShader(&ps, 0, 0);
    bool dropPs = ps && !PsIsCut(ps);   // nothing cut out: depth only, no pixel shader (fast)
    if (dropPs) ++g_ssStat.depthOnly; else if (ps) ++g_ssStat.cut;
    {   // recorded: drawn after the pass with the others, by map (1.27)
        unsigned short tgts = 0;
        for (int t = 0; t < nt; ++t) tgts |= (unsigned short)(1u << tg[t].id);
        if (SsRecord(ctx, e, d, tgts, dropPs ? 0 : ps)) { if (ps) ps->Release(); dsv->Release(); return skip; }
    }
    // save
    ID3D11Buffer* sCb = 0; ctx->VSGetConstantBuffers(e->vpSlot, 1, &sCb);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RenderTargetView* sRtv = 0; ID3D11DepthStencilView* dummy = 0;
    ctx->OMGetRenderTargets(1, &sRtv, &dummy);
    if (dummy) dummy->Release();
    // draw into each
    if (dropPs) oPSSetShader(ctx, 0, 0, 0);
    ctx->RSSetState(g_ssRs);
    ctx->OMSetDepthStencilState(g_ssDs, 0);
    D3D11_VIEWPORT q = { 0, 0, (float)g_ssRes, (float)g_ssRes, 0, 1 };
    ctx->RSSetViewports(1, &q);
    for (int t = 0; t < nt; ++t) {
        ID3D11Buffer* cb = SsVpCbFor(ctx, e->vpOff, tg[t].pack);
        if (!cb) continue;
        ctx->OMSetRenderTargets(0, 0, tg[t].dsv);
        ctx->VSSetConstantBuffers(e->vpSlot, 1, &cb);
        Replay(ctx, d);
        if (tg[t].kept) ++g_ssStat.drawsFar; else ++g_ssStat.drawsNear;
    }
    // restore
    if (dropPs) oPSSetShader(ctx, ps, 0, 0);
    ctx->VSSetConstantBuffers(e->vpSlot, 1, &sCb);
    ctx->OMSetRenderTargets(sRtv ? 1 : 0, &sRtv, dsv);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(sRs);
    ctx->OMSetDepthStencilState(sDs, sRef);
    IUnknown* rel[] = { sCb, sRs, sDs, sRtv, ps };
    for (int k = 0; k < 5; ++k) if (rel[k]) rel[k]->Release();
    dsv->Release();
    return skip;
}

// ---- the mask (in the sun pass): this frame's, then smoothed over frames
static bool SsMaskTargets(UINT w, UINT h)
{
    bool half = g_p[P_SS_HALF].v > 0.5f;
    UINT mw = half ? (w + 1) / 2 : w, mh = half ? (h + 1) / 2 : h;
    if (g_ssHist[1].srv && g_ssUp.srv && g_ssMaskW == mw && g_ssMaskH == mh && g_ssFullW == w && g_ssFullH == h) return true;
    static bool failed;
    if (failed) return false;
    FreeRT(g_ssRaw); FreeRT(g_ssHist[0]); FreeRT(g_ssHist[1]); FreeRT(g_ssUp);
    g_ssMaskFrame[0] = g_ssMaskFrame[1] = 0;
    if (!MakeRT(g_ssRaw, mw, mh, DXGI_FORMAT_R16G16B16A16_FLOAT, false) || !MakeRT(g_ssHist[0], mw, mh, DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
        !MakeRT(g_ssHist[1], mw, mh, DXGI_FORMAT_R16G16B16A16_FLOAT, false) || !MakeRT(g_ssUp, w, h, DXGI_FORMAT_R16G16_FLOAT, false)) {
        Log("sun shadows: mask targets %ux%u (frame %ux%u) failed", mw, mh, w, h);
        FreeRT(g_ssRaw); FreeRT(g_ssHist[0]); FreeRT(g_ssHist[1]); FreeRT(g_ssUp);
        failed = true;
        return false;
    }
    g_ssMaskW = mw; g_ssMaskH = mh; g_ssFullW = w; g_ssFullH = h;
    Log("sun shadows: mask %ux%u (%s resolution), at the frame's %ux%u (%.0f MB)", mw, mh, half ? "half" : "full", w, h,
        (mw * mh * 8.0 * 3 + w * h * 4.0) / 1048576);
    return true;
}

// Makes the mask for this frame (raw, then smoothed) and binds what the sun pass reads: t57 the near maps, t61 t62 the
// far ones, t58 the smoothed mask, t59 the raw one, b7 the cascades, s15 the comparison sampler. False: the old way.
// 1.54: the city's sun shadow (t72, 1024^2 over the rain map's 512 m): per texel the height under which a point is in the
// shadow of something 10..250 m towards the sun in the rain map (the city's height from above, kept: whatever was ever
// drawn, behind the camera too). The mask asks it where our cascades found nothing: the game draws for its shadows only
// what its view needs, so buildings out of it cast nothing in ours (a lit square, buildings behind the camera)
static HalfRT g_cityRT;
static unsigned g_cityFrame;
static bool SsMaskPass(ID3D11DeviceContext* ctx, const DrawCall& dc, ID3D11ShaderResourceView* prevCam)
{
    SsFlush(ctx);   // (replays still recorded: drawn first)
    if (g_p[P_SS].v < 0.5f || !g_ssNearSrv || !g_psSunMask || !g_psSunTemporal || !g_psSunUp) return false;
    // maps drawn this frame, or a few frames ago (their constants still hold their placement)
    if (!g_ssDrawnFrame || g_frame + 1 - g_ssDrawnFrame > 8) return false;
    if (!SsMaskTargets(g_rtW, g_rtH)) return false;
    int cur = g_frame & 1, last = cur ^ 1;
    ID3D11ShaderResourceView* maps[2] = { g_ssFar[1].valid ? g_ssFarSrv[1][g_ssFar[1].cur] : 0, g_ssFar[2].valid ? g_ssFarSrv[2][g_ssFar[2].cur] : 0 };
    ID3D11ShaderResourceView* c1[3] = { g_ssFar[0].valid ? g_ssFarSrv[0][g_ssFar[0].cur] : 0, g_ssC1Srv, g_ssC2Srv };
    if (g_ssMaskFrame[cur] != g_frame + 1) {
        ProfMark(ctx, PR_SSMASK, 0);
        bool hist = prevCam && g_ssMaskFrame[last] == g_frame && g_p[P_SS + 4].v > 0;
        g_ssCbData[64 + 18] = hist ? 1.0f : 0.0f;
        g_ssCbData[64 + 19] = !g_ssFar[0].valid ? 1.0f : !g_ssFar[1].valid ? 2.0f : !g_ssFar[2].valid ? 3.0f : 4.0f;   // cascades usable (in a row)
        g_ssCbData[64 + 20] = (float)g_ssMaskW; g_ssCbData[64 + 21] = (float)g_ssMaskH;   // the mask's size, the frame's
        g_ssCbData[64 + 22] = (float)g_rtW; g_ssCbData[64 + 23] = (float)g_rtH;
        g_ssCbData[88] = g_ssUpEdge;
        g_ssCbData[91] = g_sunActive && g_sunG < 0.5f ? 1.0f : 0.0f;   // SP4.w: our sun (the game's map past ours points elsewhere)
        static bool cityFailed;
        if (!g_cityRT.srv && !cityFailed && g_p[186].v > 0.5f && !MakeRT(g_cityRT, 1024, 1024, DXGI_FORMAT_R32_FLOAT, false)) { cityFailed = true; Log("city shadow map failed"); }
        bool city = g_p[186].v > 0.5f && g_psCity && g_cityRT.srv && g_rainCur >= 0;
        g_ssCbData[94] = city && g_p[187].v < 0.5f ? 1.0f : 0.0f;   // SD[0].z: the city's shadow there (1.54)
        g_ssCbData[98] = (float)(int)(g_p[187].v + 0.5f);   // SD[1].z: 1.63 (test): only that map (1..4)
        SsUpload(ctx);
        if (city && ((g_frame & 1) || g_frame - g_cityFrame > 2)) {   // every other frame
            ctx->PSSetConstantBuffers(7, 1, &g_ssCb);
            ctx->OMSetRenderTargets(1, &g_cityRT.rtv, 0);
            D3D11_VIEWPORT cv = { 0, 0, 1024, 1024, 0, 1 };
            ctx->RSSetViewports(1, &cv);
            oPSSetShader(ctx, g_psCity, 0, 0);
            Replay(ctx, dc);
            g_cityFrame = g_frame;
        }
        { ID3D11RenderTargetView* nr0 = 0; ctx->OMSetRenderTargets(1, &nr0, 0); }
        { ID3D11ShaderResourceView* cs = city ? g_cityRT.srv : 0; ctx->PSSetShaderResources(72, 1, &cs); }
        D3D11_VIEWPORT mv = { 0, 0, (float)g_ssMaskW, (float)g_ssMaskH, 0, 1 };
        ctx->RSSetViewports(1, &mv);
        ctx->PSSetConstantBuffers(7, 1, &g_ssCb);
        ctx->PSSetSamplers(15, 1, &g_ssCmp);
        ID3D11ShaderResourceView* none[6] = {};
        ctx->PSSetShaderResources(57, 6, none);
        ctx->PSSetShaderResources(57, 1, &g_ssNearSrv);
        ctx->PSSetShaderResources(61, 2, maps);
        ctx->PSSetShaderResources(64, 3, c1);
        ctx->OMSetRenderTargets(1, &g_ssRaw.rtv, 0);
        oPSSetShader(ctx, g_psSunMask, 0, 0);
        Replay(ctx, dc);
        if (g_ssSubQ[0]) ctx->End(g_ssSubQ[0]);
        ID3D11ShaderResourceView* tin[2] = { g_ssRaw.srv, hist ? g_ssHist[last].srv : 0 };
        ID3D11ShaderResourceView* sPrev = 0;
        ctx->PSGetShaderResources(28, 1, &sPrev);
        ctx->OMSetRenderTargets(1, &g_ssHist[cur].rtv, 0);   // (the target first: the raw mask can be read only once unbound)
        ctx->PSSetShaderResources(28, 1, &prevCam);
        ctx->PSSetShaderResources(59, 2, tin);
        oPSSetShader(ctx, g_psSunTemporal, 0, 0);
        Replay(ctx, dc);
        if (g_ssSubQ[1]) ctx->End(g_ssSubQ[1]);
        ctx->PSSetShaderResources(28, 1, &sPrev);
        if (sPrev) sPrev->Release();
        ctx->PSSetShaderResources(59, 2, none);
        // at the frame's resolution: from the smoothed mask (t60), the maps still bound (pixels it cannot serve)
        D3D11_VIEWPORT fv = { 0, 0, (float)g_rtW, (float)g_rtH, 0, 1 };
        ctx->RSSetViewports(1, &fv);
        ctx->OMSetRenderTargets(1, &g_ssUp.rtv, 0);
        ctx->PSSetShaderResources(60, 1, &g_ssHist[cur].srv);
        oPSSetShader(ctx, g_psSunUp, 0, 0);
        Replay(ctx, dc);
        if (g_ssSubQ[2]) ctx->End(g_ssSubQ[2]);
        ctx->PSSetShaderResources(59, 2, none);
        g_ssMaskFrame[cur] = g_frame + 1;
        ProfMark(ctx, PR_SSMASK, 1);
    }
    ID3D11RenderTargetView* nr = 0;
    ctx->OMSetRenderTargets(1, &nr, 0);
    ctx->PSSetConstantBuffers(7, 1, &g_ssCb);
    ctx->PSSetSamplers(15, 1, &g_ssCmp);
    ID3D11ShaderResourceView* use[3] = { g_ssNearSrv, g_ssUp.srv, g_ssRaw.srv };
    ctx->PSSetShaderResources(57, 3, use);
    ctx->PSSetShaderResources(61, 2, maps);
    ctx->PSSetShaderResources(64, 3, c1);
    return true;
}

static void SsUnbind(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* none[6] = {};
    ctx->PSSetShaderResources(57, 6, none);
    ctx->PSSetShaderResources(64, 3, none);
}
