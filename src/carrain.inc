// ---------------------------------------------------------------- 1.74: rain drops on the player's car, simulated
// 32768 drops live in textures (128 x 256, ping-pong): their place on one of five "unfoldings" of the car (its top x z,
// its right and left sides z y, its front and back x y; car-local metres), radius, velocity, last place. Once a frame (at
// Present) passes move them. 1.81c: S.w = its plane + its age (s) / 10 (up to 4 s): a new drop fades in over 0.25 s
// (one dying at once on an edge or a seam, landing there again and again, flickered). 1.80, as real as we could make it:
// - the car's own shape: the player's car's parts (its draws in the main view) drawn once more from above, from either
//   side, from the front and from the back (orthographic, 1 cm) into a map of the depth along each: a drop knows the
//   slope under it (the hood, the windscreen, the roof's curve, the rear window, the doors' tumblehome), runs down it,
//   goes over an edge onto the next face (off the roof onto a side window) and drips off where the body ends;
// - rain lands where it falls: on each face by the rain's flux through it (its fall and the car's own speed: the
//   windscreen and the front take more when driving); splashes and the wheels' spray on the lower faces; where drops
//   already sit thick, it joins them (they grow) rather than making new ones;
// - merging: the drops in a grid of 16 mm cells (up to 4 a cell); one touching (or swept through) a bigger one flows
//   into it, and the bigger one takes the water and the momentum of every one flowing into it: both decide by the same
//   "into which" texture, so no water is lost (1.80's first try: each decided alone; 85% of the drops vanished);
// - a drop is held by its rim (~ r) against its weight (~ r^3): the big ones run. The paint holds unevenly (it
//   meanders) and less on a track another one wetted (they follow each other). Running, its rim's drag sets a steady
//   speed (cm/s small, tens of cm/s big); it sweeps up the fine droplets off a path not yet run (grows, speeds up) and
//   leaves water on its track; resting in the rain, the fine droplets round it gather into it; with no rain it dries;
// - the air past the car pushes them (drag ~ v^2 / r: the big ones go first), slowly in the wake (the rear window), up
//   over a face it hits (the windscreen); the car's acceleration (inertia), its tilt (gravity in its frame).
// Then each is drawn into an atlas of the five unfoldings (4 mm texels, its index, the nearest kept by depth) and its
// trail into a trail map that dries. PS_Sun finds the drops at a pixel (the atlas' 2 x 2 texels round it).

static const int kCrW = 128, kCrH = 256, kCrAtlasW = 2000, kCrAtlasH = 2250;
static const int kCrGridW = 500, kCrGridH = 563, kCrLayers = 4;   // 1.80: the merging grid: 16 mm cells, 4 drops a cell
static const int kCrMapW = 800, kCrMapH = 900;                     // 1.80: the car's shape: the atlas' layout at 1 cm
static HalfRT g_crS[2], g_crV[2];            // state: x y (plane m), radius (m), plane; velocity x y, last x y
static HalfRT g_crT;                         // 1.80: R32F: the drop each one flows into (index + 1; 0: none)
static HalfRT g_crGrid[kCrLayers];           // 1.80: R16_UINT: a cell's drops (index + 1), the smallest index first
static int g_crCur;
static ID3D11Texture2D* g_crAtlas;           // R16_UINT: drop index + 1
static ID3D11RenderTargetView* g_crAtlasRtv;
static ID3D11ShaderResourceView* g_crAtlasSrv;
static ID3D11Texture2D *g_crDepth, *g_crGridDepth;
static ID3D11DepthStencilView *g_crDsv, *g_crGridDsv;
static HalfRT g_crTrail;                     // R8: a wet trail, drying
static ID3D11VertexShader *g_crVsFull, *g_crVsDrop, *g_crVsGrid;
static ID3D11PixelShader *g_crPsSim, *g_crPsAtlas, *g_crPsTrail, *g_crPsDecay, *g_crPsGrid, *g_crPsTarget;
static ID3D11Buffer* g_crCb;
static ID3D11BlendState *g_crBlendMax, *g_crBlendDecay;
static ID3D11DepthStencilState* g_crDsLess;
static ID3D11RasterizerState* g_crRs;
static bool g_crFailed, g_crFresh = true;   // (g_crValid, g_crBlown: newlook.cpp)
static double g_crDryT;                      // seconds since the rain stopped (the drops dry; gone after a while)
static bool g_crJump;                        // 1.81: the car moved far at once (a reset, a teleport): the drops cleared
static unsigned g_crResets, g_crPaused, g_crGlassDraws;   // 1.81: (the log)
static double g_crCpuGlass, g_crCpuGlassMax, g_crCpuTickMax, g_crFrameMax;   // 1.81c: (the log) CPU ms: the glass a frame, the tick; the longest frame
static unsigned g_crLong;                                                    // 1.81c: frames over 40 ms
// the car's motion (car-local, smoothed) from the player's matrix read-backs (rain.inc)
static unsigned g_crSeen;
static double g_crLastT;
static float g_crLastPos[3], g_crVel[3], g_crAcc[3];
// 1.80: the car's shape (R32: D32 to draw, R32F to read); [g_crMapCur] in use, the other one baked into
static ID3D11Texture2D* g_crMap[2];
static ID3D11DepthStencilView* g_crMapDsv[2];
static ID3D11ShaderResourceView* g_crMapSrv[2];
static int g_crMapCur = -1;                  // (-1: none yet: the faces taken flat)
static bool g_crBakeArm, g_crBakeOpen, g_crBakeExact;
static unsigned g_crBakeTag, g_crBakeDrawn, g_crBakeRec, g_crBakeVer, g_crBakeLast, g_crBakeMatTag, g_crBakeMVer;
static unsigned g_crBakeOk, g_crBakeBad;
static float g_crBakeMat[16], g_crBakeM[5][16];
struct CrMapCb { int off; ID3D11Buffer* b[5]; unsigned ver[5]; };
static CrMapCb g_crMapCbs[16];
static int g_crMapCbN;
// each view's place in the map (texels): the atlas' layout at 1 cm
static const float kCrMapVp[5][4] = { { 0, 0, 240, 560 }, { 240, 0, 560, 300 }, { 240, 300, 560, 300 }, { 0, 560, 240, 300 }, { 240, 600, 240, 300 } };
static void SsFlush(ID3D11DeviceContext* ctx);
static bool SsRecord(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned short tgts, ID3D11PixelShader* ps);

static const char* kCarRainSrc = R"HLSL(
cbuffer DropCB : register(b0) { float4 D0, D1, D2, D3, D4, D5, D6, D7, D8, D9; };
// D0: dt, a random (its bits), spawn tries a slot a frame, the grid's layer (its passes); D1: gravity (car-local, m/s2),
// the air's drag k; D2: inertia (- the car's acceleration, car-local), rain 0..1; D3: the air past the car (car-local,
// m/s), mode (atlas: 0 shapes, 1 trails); D4: atlas size, state size; D5: the faces' shares of the rain (cumulative:
// top, right, left, front); D6: the rain past the car (car-local, m/s), splash and spray low down (m/s of rain); D7: the
// most rain a spot takes per face (top, sides, front, back); D8: the car's shape known, drops a 48 mm square holds at
// most, the grid's size; D9 (1.81, the controls): the paint's hold x, the running drag x, the size x, drying x
Texture2D<float4> tS : register(t0);
Texture2D<float4> tV : register(t1);
Texture2D<float> tT : register(t2);     // (the simulation) into which drop each one flows (index + 1)
Texture2D<uint> tG0 : register(t3);     // the grid's layers (the grid's passes: the layer before at t3)
Texture2D<uint> tG1 : register(t4);
Texture2D<uint> tG2 : register(t5);
Texture2D<uint> tG3 : register(t6);
Texture2D<float> tTr : register(t7);    // the wet tracks (the frame before)
Texture2D<float> tMap : register(t8);   // the car's shape (1 cm): depth along each face's view; 1: none
static const float kS = 0.004;
// (1.75: the body's own extent: the car's place is at its floor)
float2 PlaneMin(int pl) { return pl == 0 ? float2(-1.0, -2.4) : pl <= 2 ? float2(-2.4, 0.0) : float2(-1.0, 0.0); }
float2 PlaneMax(int pl) { return pl == 0 ? float2(1.0, 2.4) : pl <= 2 ? float2(2.4, 1.45) : float2(1.0, 1.45); }
float2 DropAtlas(int pl, float2 p)
{
    if (pl == 0) return float2((p.x + 1.2) / kS, (p.y + 2.8) / kS);
    float2 o = pl == 1 ? float2(600, 0) : pl == 2 ? float2(600, 750) : pl == 3 ? float2(0, 1400) : float2(600, 1500);
    float umin = pl <= 2 ? -2.8 : -1.2;
    return o + float2((p.x - umin) / kS, (2.0 - p.y) / kS);
}
void PlaneAxes(int pl, out float3 U, out float3 V)
{
    if (pl == 0) { U = float3(1, 0, 0); V = float3(0, 0, 1); }
    else if (pl <= 2) { U = float3(0, 0, 1); V = float3(0, 1, 0); }
    else { U = float3(1, 0, 0); V = float3(0, 1, 0); }
}
float3 PlaneN(int pl) { return pl == 0 ? float3(0, 1, 0) : pl == 1 ? float3(1, 0, 0) : pl == 2 ? float3(-1, 0, 0) : pl == 3 ? float3(0, 0, 1) : float3(0, 0, -1); }
uint Hash(uint x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
float Rnd(uint i, uint salt) { return (Hash(i * 0x9E3779B9U ^ Hash(salt * 0x85EBCA6BU + asuint(D0.y))) >> 8) / 16777216.0; }
float H2(int2 i, uint s) { return (Hash((uint)i.x * 73856093U ^ (uint)i.y * 19349663U ^ s * 83492791U) >> 8) / 16777216.0; }
float VN(float2 x, uint s)
{
    int2 i = int2(floor(x));
    float2 f = frac(x);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(H2(i, s), H2(i + int2(1, 0), s), f.x), lerp(H2(i + int2(0, 1), s), H2(i + int2(1, 1), s), f.x), f.y);
}
// how hard the paint holds a drop here (0.5 .. 1.5: wax, dirt, the clear coat's own unevenness)
float Pin(int pl, float2 p) { return 0.5 + 0.6 * VN(p / 0.006, pl) + 0.4 * VN(p / 0.0021, pl + 7); }
int2 Slot(uint j) { return int2(j % (uint)D4.z, j / (uint)D4.z); }
int2 CellOf(int pl, float2 p) { return int2(floor(DropAtlas(pl, p) * 0.25)); }
uint GridAt(int L, int2 c)
{
    if (any(c < 0) || c.x >= (int)D8.z || c.y >= (int)D8.w) return 0;
    int3 t = int3(c, 0);
    return L == 0 ? tG0.Load(t) : L == 1 ? tG1.Load(t) : L == 2 ? tG2.Load(t) : tG3.Load(t);
}
float MapAt(int2 t) { return tMap.Load(int3(clamp(t, int2(0, 0), int2(799, 899)), 0)); }
// face pl's surface at p: there (the body seen from that face's side, inside its bounds); n its normal, L its place
// (car-local). With no shape known yet: flat faces at rough heights.
bool Surface(int pl, float2 p, out float3 n, out float3 L)
{
    float3 U, V;
    PlaneAxes(pl, U, V);
    float3 N = PlaneN(pl);
    n = N;
    float o = pl == 0 ? 2.0 : pl <= 2 ? 1.3 : 3.0, rg = pl == 0 ? 2.5 : pl <= 2 ? 2.6 : 6.0;
    bool inR = all(p >= PlaneMin(pl)) && all(p <= PlaneMax(pl));
    if (D8.x < 0.5) { L = U * p.x + V * p.y + N * (pl == 0 ? 1.4 : pl <= 2 ? 0.95 : 2.3); return inR; }
    int2 ti = int2(floor(DropAtlas(pl, p) * 0.4));   // (1 cm)
    float d0 = MapAt(ti);
    L = U * p.x + V * p.y + N * (o - d0 * rg);
    if (!inR || d0 >= 0.99999) return false;
    // its slope over +-2 cm (one-sided where the body ends)
    float dl = MapAt(ti - int2(2, 0)), dr = MapAt(ti + int2(2, 0)), du = MapAt(ti - int2(0, 2)), dn = MapAt(ti + int2(0, 2));
    bool hl = dl < 0.99999, hr = dr < 0.99999, hu = du < 0.99999, hn = dn < 0.99999;
    float gx = hl && hr ? (dr - dl) * 0.5 : hr ? dr - d0 : hl ? d0 - dl : 0;
    float gy = hu && hn ? (dn - du) * 0.5 : hn ? dn - d0 : hu ? d0 - du : 0;
    // metres of depth per metre along the face (its texel v grows with V on the top, against it on the others)
    float Du = gx * rg / 0.02, Dv = gy * rg / 0.02 * (pl == 0 ? 1 : -1);
    n = normalize(N + U * Du + V * Dv);
    return true;
}

struct VOut { float4 pos : SV_Position; float4 a : TEXCOORD0; float4 b : TEXCOORD1; nointerpolation float id : TEXCOORD2; };
VOut vs_full(uint vid : SV_VertexID)
{
    VOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.a = 0; o.b = 0; o.id = 0;
    return o;
}

// ---- 1.80: the grid: each drop a point in its cell; a layer keeps the smallest index above the layer before's
VOut vs_grid(uint vid : SV_VertexID)
{
    VOut o;
    o.a = 0; o.b = 0; o.id = vid + 1;
    o.pos = float4(-9, -9, 0, 1);
    float4 S = tS.Load(int3(Slot(vid), 0));
    if (S.z <= 0) return o;
    int2 c = CellOf((int)(S.w + 0.5), S.xy);
    o.pos = float4((float2(c) + 0.5) / D8.zw * float2(2, -2) + float2(-1, 1), (vid + 1) / 65536.0, 1);
    return o;
}
uint ps_grid(VOut i) : SV_Target0
{
    if (D0.w > 0.5 && (uint)i.id <= tG0.Load(int3(int2(i.pos.xy), 0))) discard;
    return (uint)i.id;
}

// ---- 1.80: into which drop each one flows: the biggest one it touches (or was swept through, this frame's move) that
// is bigger than it (ties: the lower index). Only a drop in the grid (the 4 of its cell): the one it flows into must
// find it there.
float ps_target(VOut i) : SV_Target0
{
    int2 q = int2(i.pos.xy);
    uint self = (uint)(q.y * (int)D4.z + q.x);
    float4 S = tS.Load(int3(q, 0)), V = tV.Load(int3(q, 0));
    if (S.z <= 0) return 0;
    int pl = (int)(S.w + 0.5);
    int2 c0 = CellOf(pl, S.xy);
    bool stored = false;
    [unroll] for (int L0 = 0; L0 < 4; ++L0) stored = stored || GridAt(L0, c0) == self + 1;
    if (!stored) return 0;
    float best = 0, bestR = 0;
    [loop] for (int yy = -1; yy <= 1; ++yy)
        [loop] for (int xx = -1; xx <= 1; ++xx)
            [unroll] for (int L = 0; L < 4; ++L) {
                uint id = GridAt(L, c0 + int2(xx, yy));
                if (id == 0 || id == self + 1) continue;
                uint j = id - 1;
                float4 Sj = tS.Load(int3(Slot(j), 0));
                if (Sj.z <= 0 || abs(Sj.w - S.w) > 0.5) continue;
                if (!(Sj.z > S.z || (Sj.z == S.z && j < self)) || Sj.z <= bestR) continue;
                float4 Vj = tV.Load(int3(Slot(j), 0));
                float2 a = V.zw, b = S.xy, ab = b - a;   // this one's move and the other's: the nearest they came
                float d1 = length(Sj.xy - a - ab * saturate(dot(Sj.xy - a, ab) / max(dot(ab, ab), 1e-12)));
                float2 a2 = Vj.zw, ab2 = Sj.xy - a2;
                float d2 = length(S.xy - a2 - ab2 * saturate(dot(S.xy - a2, ab2) / max(dot(ab2, ab2), 1e-12)));
                if (min(d1, d2) < S.z + Sj.z) { best = id; bestR = Sj.z; }
            }
    return best;
}
)HLSL" R"HLSL(
// ---- the simulation: one pixel a drop
struct SimOut { float4 s : SV_Target0; float4 v : SV_Target1; };
SimOut ps_sim(VOut i)
{
    int2 q = int2(i.pos.xy);
    uint self = (uint)(q.y * (int)D4.z + q.x);
    float4 S = tS.Load(int3(q, 0)), V = tV.Load(int3(q, 0));
    float dt = D0.x;
    SimOut o;
    o.s = 0; o.v = 0;
    if (S.z <= 0) {   // empty: rain may land a new one here
        if (D2.w > 0.01 && Rnd(self, 1) < D0.z) {
            float r = Rnd(self, 2);
            int np = r < D5.x ? 0 : r < D5.y ? 1 : r < D5.z ? 2 : r < D5.w ? 3 : 4;
            float2 p = lerp(PlaneMin(np), PlaneMax(np), float2(Rnd(self, 3), Rnd(self, 4)));
            float3 n, lp;
            if (Surface(np, p, n, lp)) {
                float3 N = PlaneN(np);
                bool own = true;   // (the face a pixel there is shaded as: the one its normal is nearest)
                [unroll] for (int k = 0; k < 5; ++k) if (k != np && dot(n, PlaneN(k)) > dot(n, N)) own = false;
                // the rain through it (per unit of the face: its slant), splashes and spray low down
                float flux = max(-dot(D6.xyz, n), 0) / max(dot(n, N), 0.3) + (np > 0 ? D6.w * exp(-max(lp.y, 0) / 0.35) : 0);
                float most = np == 0 ? D7.x : np <= 2 ? D7.y : np == 3 ? D7.z : D7.w;
                // where drops sit thick already, the rain joins them (they grow) rather than making new ones
                int2 c0 = CellOf(np, p);
                float cnt = 0;
                [loop] for (int yy = -1; yy <= 1; ++yy)
                    [loop] for (int xx = -1; xx <= 1; ++xx)
                        [unroll] for (int L = 0; L < 4; ++L) cnt += GridAt(L, c0 + int2(xx, yy)) > 0 ? 1 : 0;
                // 1.81c: not on an edge, a seam, a panel's gap (the shape jumps within 1 cm): a drop would not stay there
                bool smooth = true;
                if (D8.x > 0.5) {
                    int2 ti = int2(floor(DropAtlas(np, p) * 0.4));
                    float d0 = MapAt(ti), rg = np == 0 ? 2.5 : np <= 2 ? 2.6 : 6.0;
                    [unroll] for (int k2 = 0; k2 < 4; ++k2) {
                        float dk = MapAt(ti + int2(k2 == 0 ? 1 : k2 == 1 ? -1 : 0, k2 == 2 ? 1 : k2 == 3 ? -1 : 0));
                        smooth = smooth && dk < 0.99999 && abs(dk - d0) * rg < 0.012;
                    }
                }
                if (own && smooth && Rnd(self, 5) * most < flux && cnt < D8.y * (0.5 + Rnd(self, 7))) {
                    float rad = lerp(0.0003, 0.0010, Rnd(self, 6) * Rnd(self, 16)) * D9.z;
                    o.s = float4(p, rad, np);
                    o.v = float4(0, 0, p);
                }
            }
        }
        return o;
    }
    int pl = (int)(S.w + 0.5);
    float age = min(S.w - pl + D0.x * 0.1, 0.4);
    float2 p = S.xy, vel = V.xy;
    float rad = S.z;
    // merging: flowing into a drop that flows on itself: kept this frame (it flows into the next one later)
    uint tg = (uint)(tT.Load(int3(q, 0)) + 0.5);
    if (tg > 0) {
        if (tT.Load(int3(Slot(tg - 1), 0)) < 0.5) return o;   // gone into it
    } else {   // the ones flowing into this one: their water, their momentum, their centre
        float vol = rad * rad * rad, v0 = vol;
        float2 cm = p * vol, mom = vel * vol;
        int2 c0 = CellOf(pl, p);
        [loop] for (int yy = -1; yy <= 1; ++yy)
            [loop] for (int xx = -1; xx <= 1; ++xx)
                [unroll] for (int L = 0; L < 4; ++L) {
                    uint id = GridAt(L, c0 + int2(xx, yy));
                    if (id == 0 || id == self + 1) continue;
                    int2 sj = Slot(id - 1);
                    if (abs(tT.Load(int3(sj, 0)) - (self + 1)) > 0.5) continue;
                    float4 Sj = tS.Load(int3(sj, 0)), Vj = tV.Load(int3(sj, 0));
                    float vj = Sj.z * Sj.z * Sj.z;
                    vol += vj; cm += Sj.xy * vj; mom += Vj.xy * vj;
                }
        if (vol > v0) { rad = pow(vol, 1.0 / 3.0); p = cm / vol; vel = mom / vol; }
    }
    float3 U, Vv;
    PlaneAxes(pl, U, Vv);
    float3 n, lp;
    if (!Surface(pl, p, n, lp)) return o;   // (off the body: dripped off)
    // the push (m/s2): gravity and inertia along the surface; the air along it (its drag ~ v^2 x area / mass ~ v^2 / r):
    // up over a face it hits, slow in the wake behind
    float3 air = D3.xyz;
    float an = dot(air, n);
    float3 airT = (air - n * an) * (an > 0 ? 0.3 : 1.0) + (float3(0, 1, 0) - n * n.y) * max(-an, 0) * 0.5;
    float3 f3 = D1.xyz + D2.xyz;
    f3 -= n * dot(f3, n);
    f3 += D1.w * length(airT) * airT / max(rad, 2e-4);
    float2 f = float2(dot(f3, U), dot(f3, Vv));
    // held by its rim (as an acceleration ~ 1 / r^2: a 1.4 mm drop runs on a vertical side where the paint holds
    // averagely); unevenly over the paint (it meanders: pulled to where it holds less), less on a wet track (pulled
    // onto it: they follow each other)
    int2 at = int2(DropAtlas(pl, p));
    float wet = saturate(tTr.Load(int3(at, 0)) * 1.5);
    float2 gWet = float2(tTr.Load(int3(at + int2(1, 0), 0)) - tTr.Load(int3(at - int2(1, 0), 0)),
                         tTr.Load(int3(at + int2(0, 1), 0)) - tTr.Load(int3(at - int2(0, 1), 0))) * 0.5;
    if (pl > 0) gWet.y = -gWet.y;   // (the atlas' v runs down on the sides)
    float e = 0.001;
    float2 gPin = float2(Pin(pl, p + float2(e, 0)) - Pin(pl, p - float2(e, 0)), Pin(pl, p + float2(0, e)) - Pin(pl, p - float2(0, e))) / (2 * e);
    float hold0 = 9.81 * pow(0.0014 / rad, 2);
    float hold = hold0 * Pin(pl, p) * (1 - 0.5 * wet) * D9.x;
    f += (-gPin * 0.0035 + gWet * 0.6) * hold0;
    float sp = length(vel), fl = length(f);
    if (sp < 1e-3 && fl < hold) vel = 0;   // held
    else {   // sliding: its rim's friction and drag; a steady speed (exact over the step)
        float2 dir = sp > 1e-3 ? vel / sp : f / max(fl, 1e-6);
        float c = 60 * pow(0.0015 / rad, 2) * D9.y;
        float2 vt = (f - dir * hold * 0.55) / c;
        vel = vt + (vel - vt) * exp(-c * dt);
        if (dot(vel, dir) < 0) vel = 0;   // (stopped: held again)
        float vl = length(vel);
        if (vl > 1.2) vel *= 1.2 / vl;
    }
    float2 np = p + vel * dt, prev = p;
    float moved = length(np - p);
    float3 n2, lp2;
    bool there = moved < 1e-7 ? true : Surface(pl, np, n2, lp2);
    if (!there) return o;   // over the edge where the body ends: dripped off
    if (moved >= 1e-7) {
        // over onto the face it now faces (the roof's curve onto a side window, the hood's nose onto the front)
        int best = pl;
        float bd = dot(n2, PlaneN(pl)) + 0.08;
        [unroll] for (int k = 0; k < 5; ++k) { float dk = dot(n2, PlaneN(k)); if (k != pl && dk > bd) { bd = dk; best = k; } }
        if (best != pl) {
            float3 U2, V2, n3, lp3;
            PlaneAxes(best, U2, V2);
            float2 p2 = float2(dot(lp2, U2), dot(lp2, V2));
            if (Surface(best, p2, n3, lp3) && length(lp3 - lp2) < 0.03) {
                float3 v3 = U * vel.x + Vv * vel.y;
                vel = float2(dot(v3, U2), dot(v3, V2));
                pl = best; np = p2; prev = p2;
            }
        }
    }
    // water: swept up off a path not run yet (the fine droplets there, while it rains), left on its track; resting in
    // the rain, the fine droplets round it gather into it now and then, as much as the rain lands there (a slant
    // facing the rain more, the sides from splashes; up to 2.8 mm); no rain: it dries by its rim
    rad += (D2.w * 1.2e-5 * (1 - wet) - 1.5e-6) * moved / (3.14159 * rad);
    float land = (max(-dot(D6.xyz, n), 0) + (pl > 0 ? D6.w * 3 * exp(-max(lp.y, 0) / 0.35) : 0)) / 7.0;
    if (D2.w > 0.01 && rad < 0.0028 * D9.z && Rnd(self, 9) < D2.w * dt * 4.5 * min(land, 2.0) * (0.3 + 0.7 * pow(rad / 0.0015, 2))) {
        float g = 0.00045 * D9.z;
        rad = pow(rad * rad * rad + g * g * g * (0.5 + 1.5 * Rnd(self, 10)), 1.0 / 3.0);
    }
    if (D2.w < 0.01) rad -= 6e-10 * D9.w / max(rad, 1e-4) * dt;
    rad = min(rad, 0.006);
    if (rad < 0.0002) return o;
    o.s = float4(np, rad, pl + age);
    o.v = float4(vel, prev);
    return o;
}
)HLSL" R"HLSL(
// ---- a drop drawn into the atlas (mode 0: its shape, index by the nearest; mode 1: its trail this frame)
VOut vs_drop(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    VOut o;
    int2 q = int2(inst % (uint)D4.z, inst / (uint)D4.z);
    float4 S = tS.Load(int3(q, 0)), V = tV.Load(int3(q, 0));
    o.id = inst + 1;
    o.a = 0; o.b = 0;
    o.pos = float4(-9, -9, 0, 1);
    if (S.z <= 0) return o;
    int pl = (int)(S.w + 0.5);
    float2 head = S.xy, tail = V.zw - S.xy;   // tail: to where it was a frame ago
    if (D3.w < 0.5) {   // its shape: a runlet behind a moving drop
        float tl = length(V.xy) * 0.1;
        tail = length(V.xy) > 1e-3 ? -normalize(V.xy) * min(tl, 0.025) : 0;   // (1.75: up to 2.5 cm)
    } else if (dot(tail, tail) < 1e-10 || dot(tail, tail) > 0.0025) return o;   // (no trail while held; none across faces)
    float r = D3.w < 0.5 ? S.z : S.z * 0.6;
    float2 a = DropAtlas(pl, head), b = DropAtlas(pl, head + tail);
    float e = D3.w < 0.5 ? max(r / kS, 0.75) * 3.5 + 1 : max(r / kS, 0.75) + 1;   // (1.80: its shape with its shadow round it)
    float2 mn = min(a, b) - e, mx = max(a, b) + e;
    float2 c = float2((vid & 1) ? mx.x : mn.x, (vid & 2) ? mx.y : mn.y);
    o.pos = float4(c / D4.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    o.a = float4(a, b);
    o.b = float4(r / kS, c, 0);
    return o;
}
float SegDist(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
    return length(p - a - ab * t);
}
uint ps_atlas(VOut i, out float depth : SV_Depth) : SV_Target0
{
    float d = SegDist(i.pos.xy, i.a.xy, i.a.zw) / max(i.b.x, 0.5);
    depth = min(d * 0.25, 0.999);
    return (uint)i.id;
}
float4 ps_trail(VOut i) : SV_Target0
{
    float d = SegDist(i.pos.xy, i.a.xy, i.a.zw) / max(i.b.x, 0.5);
    return saturate(1.5 - d);
}
float4 ps_decay(VOut i) : SV_Target0 { return 0; }
)HLSL";

// 1.81d: the shaders compiled on a worker (on the render thread it was a 0.66 s hitch when the rain first came)
static volatile LONG g_crShState;   // 0 not started, 1 compiling, 2 done, 3 failed
static DWORD WINAPI CarRainCompile(LPVOID)
{
    struct E { const char* entry; const char* target; void** out; bool vs; };
    E es[] = { { "vs_full", "vs_4_0", (void**)&g_crVsFull, true }, { "vs_drop", "vs_4_0", (void**)&g_crVsDrop, true },
               { "vs_grid", "vs_4_0", (void**)&g_crVsGrid, true },
               { "ps_sim", "ps_4_0", (void**)&g_crPsSim, false }, { "ps_atlas", "ps_4_0", (void**)&g_crPsAtlas, false },
               { "ps_trail", "ps_4_0", (void**)&g_crPsTrail, false }, { "ps_decay", "ps_4_0", (void**)&g_crPsDecay, false },
               { "ps_grid", "ps_4_0", (void**)&g_crPsGrid, false }, { "ps_target", "ps_4_0", (void**)&g_crPsTarget, false } };
    for (auto& e : es) {
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(kCarRainSrc, strlen(kCarRainSrc), "carrain", 0, 0, e.entry, e.target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            if (e.vs) g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), 0, (ID3D11VertexShader**)e.out);
            else g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, (ID3D11PixelShader**)e.out);
        } else Log("car rain: shader %s: %s", e.entry, err ? (const char*)err->GetBufferPointer() : "?");
        if (code) code->Release();
        if (err) err->Release();
    }
    bool ok = g_crVsFull && g_crVsDrop && g_crVsGrid && g_crPsSim && g_crPsAtlas && g_crPsTrail && g_crPsDecay && g_crPsGrid && g_crPsTarget;
    InterlockedExchange(&g_crShState, ok ? 2 : 3);
    return 0;
}
static bool CarRainEnsure()
{
    if (g_crPsTarget && g_crAtlasSrv && g_crMapSrv[1]) return true;
    if (g_crFailed || !g_dev || !g_compile) return false;
    if (InterlockedCompareExchange(&g_crShState, 1, 0) == 0) {
        HANDLE th = CreateThread(0, 0, CarRainCompile, 0, 0, 0);
        if (th) CloseHandle(th); else CarRainCompile(0);
    }
    if (g_crShState == 1) return false;   // (still compiling: no drops yet)
    bool ok = g_crShState == 2;
    for (int k = 0; k < 2 && ok; ++k) ok = MakeRT(g_crS[k], kCrW, kCrH, DXGI_FORMAT_R32G32B32A32_FLOAT, false) && MakeRT(g_crV[k], kCrW, kCrH, DXGI_FORMAT_R32G32B32A32_FLOAT, false);
    ok = ok && MakeRT(g_crT, kCrW, kCrH, DXGI_FORMAT_R32_FLOAT, false);
    for (int k = 0; k < kCrLayers && ok; ++k) ok = MakeRT(g_crGrid[k], kCrGridW, kCrGridH, DXGI_FORMAT_R16_UINT, false);
    ok = ok && MakeRT(g_crTrail, kCrAtlasW, kCrAtlasH, DXGI_FORMAT_R8_UNORM, false);
    if (ok) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = kCrAtlasW; d.Height = kCrAtlasH; d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_R16_UINT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ok = SUCCEEDED(g_dev->CreateTexture2D(&d, 0, &g_crAtlas)) && SUCCEEDED(g_dev->CreateRenderTargetView(g_crAtlas, 0, &g_crAtlasRtv)) &&
             SUCCEEDED(g_dev->CreateShaderResourceView(g_crAtlas, 0, &g_crAtlasSrv));
        d.Format = DXGI_FORMAT_D16_UNORM; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&d, 0, &g_crDepth)) && SUCCEEDED(g_dev->CreateDepthStencilView(g_crDepth, 0, &g_crDsv));
        d.Width = kCrGridW; d.Height = kCrGridH;
        ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&d, 0, &g_crGridDepth)) && SUCCEEDED(g_dev->CreateDepthStencilView(g_crGridDepth, 0, &g_crGridDsv));
        // the car's shape: D32 to draw its parts into, R32F to read
        d.Width = kCrMapW; d.Height = kCrMapH; d.Format = DXGI_FORMAT_R32_TYPELESS; d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {}; dv.Format = DXGI_FORMAT_D32_FLOAT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = DXGI_FORMAT_R32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        for (int k = 0; k < 2 && ok; ++k)
            ok = SUCCEEDED(g_dev->CreateTexture2D(&d, 0, &g_crMap[k])) && SUCCEEDED(g_dev->CreateDepthStencilView(g_crMap[k], &dv, &g_crMapDsv[k])) &&
                 SUCCEEDED(g_dev->CreateShaderResourceView(g_crMap[k], &sv, &g_crMapSrv[k]));
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 160; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = ok && SUCCEEDED(g_dev->CreateBuffer(&bd, 0, &g_crCb));
        D3D11_BLEND_DESC bl = {};
        bl.RenderTarget[0].BlendEnable = TRUE; bl.RenderTarget[0].RenderTargetWriteMask = 15;
        bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
        ok = ok && SUCCEEDED(g_dev->CreateBlendState(&bl, &g_crBlendMax));
        bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bl.RenderTarget[0].DestBlend = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_BLEND_FACTOR;
        bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        ok = ok && SUCCEEDED(g_dev->CreateBlendState(&bl, &g_crBlendDecay));
        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
        ok = ok && SUCCEEDED(g_dev->CreateDepthStencilState(&dd, &g_crDsLess));
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        ok = ok && SUCCEEDED(g_dev->CreateRasterizerState(&rd, &g_crRs));
    }
    if (!ok) { g_crFailed = true; Log("car rain: resources failed"); return false; }
    Log("car rain: %d drops, atlas %dx%d (4 mm), merging grid %dx%d x %d, the car's shape %dx%d (1 cm)", kCrW * kCrH, kCrAtlasW, kCrAtlasH,
        kCrGridW, kCrGridH, kCrLayers, kCrMapW, kCrMapH);
    return true;
}

// ---- 1.80: the car's shape, baked from its own draws
// the five views (the game's packed form, as RainMatrix: rows x, y, depth (world), then z scale / bias, w scale / bias):
// the player's part's matrix of the baking frame (else its last read-back)
static void CarMapMatrices()
{
    g_crBakeExact = g_crBakeMatTag == g_crBakeTag;
    const float* M = g_crBakeExact ? g_crBakeMat : g_playerMat;
    float a[3][3], P[3] = { M[12], M[13], M[14] };
    for (int r = 0; r < 3; ++r) {
        float l = sqrtf(M[r * 4] * M[r * 4] + M[r * 4 + 1] * M[r * 4 + 1] + M[r * 4 + 2] * M[r * 4 + 2]);
        for (int j = 0; j < 3; ++j) a[r][j] = l > 1e-5f ? M[r * 4 + j] / l : (r == j ? 1.0f : 0.0f);
    }
    if (a[1][1] < 0) for (int j = 0; j < 3; ++j) a[1][j] = -a[1][j];   // (as CarRainTick: y up)
    auto row = [&](float* m, const float* v, float s, float c) { for (int j = 0; j < 3; ++j) m[j] = v[j] * s; m[3] = c - s * (v[0] * P[0] + v[1] * P[1] + v[2] * P[2]); };
    const float *X = a[0], *Y = a[1], *Z = a[2];
    for (int v = 0; v < 5; ++v) {
        float* m = g_crBakeM[v];
        memset(m, 0, 64);
        if (v == 0) { row(m, X, 1 / 1.2f, 0); row(m + 4, Z, -1 / 2.8f, 0); row(m + 8, Y, -1, 2.0f); m[12] = 1 / 2.5f; }   // from above
        else if (v <= 2) { row(m, Z, 1 / 2.8f, 0); row(m + 4, Y, 2 / 3.0f, -1 / 3.0f); row(m + 8, X, v == 1 ? -1.0f : 1.0f, 1.3f); m[12] = 1 / 2.6f; }   // the sides
        else { row(m, X, 1 / 1.2f, 0); row(m + 4, Y, 2 / 3.0f, -1 / 3.0f); row(m + 8, Z, v == 3 ? -1.0f : 1.0f, 3.0f); m[12] = 1 / 6.0f; }   // front, back
        m[15] = 1;
    }
}

// SsFlush (sunshadow.inc), targets 11 .. 15: view v's depth target and viewport (0: not baking)
static ID3D11DepthStencilView* CarMapTarget(int v, D3D11_VIEWPORT* vp)
{
    if (!g_crBakeOpen || !g_crMapDsv[1]) return 0;
    D3D11_VIEWPORT q = { kCrMapVp[v][0], kCrMapVp[v][1], kCrMapVp[v][2], kCrMapVp[v][3], 0, 1 };
    *vp = q;
    return g_crMapDsv[g_crMapCur < 0 ? 0 : g_crMapCur ^ 1];
}
static ID3D11Buffer* CarMapCbFor(ID3D11DeviceContext* ctx, int off, int v)
{
    if (g_crBakeMVer != g_crBakeVer) { CarMapMatrices(); g_crBakeMVer = g_crBakeVer; }
    CrMapCb* c = 0;
    for (int k = 0; k < g_crMapCbN; ++k) if (g_crMapCbs[k].off == off) c = &g_crMapCbs[k];
    if (!c) {
        if (g_crMapCbN >= 16) return 0;
        c = &g_crMapCbs[g_crMapCbN++];
        memset(c, 0, sizeof(*c));
        c->off = off;
    }
    if (!c->b[v]) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 1024; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &c->b[v]))) { c->b[v] = 0; return 0; }
    }
    if (c->ver[v] != g_crBakeVer) {
        static char buf[1024];
        memset(buf, 0, sizeof(buf));
        memcpy(buf + off, g_crBakeM[v], 64);
        ctx->UpdateSubresource(c->b[v], 0, 0, buf, 0, 0);
        c->ver[v] = g_crBakeVer;
    }
    return c->b[v];
}

// rain.inc (every draw of a car with bones, not a wheel, in the main view): while a bake is on, the player's car's
// parts (near where it is) recorded to be drawn again into the shape map (SsFlush); its own part's matrix of now kept
static void CarMapRecord(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned long long key)
{
    if (!g_crBakeArm && !(g_crBakeOpen && g_crBakeTag == g_frame + 1)) return;
    if (!g_crMapDsv[1] || e->vpSlot < 0 || e->vpSlot == e->wSlot || e->vpOff + 64 > 1024) return;
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    if (!cb) return;
    const unsigned char* data = 0;
    UINT size = 0;
    int st = WeCbNow(cb, &data, &size);
    cb->Release();
    if (st != 1 || (UINT)e->wOff + 64 > size) return;
    const float* W = (const float*)(data + e->wOff);
    if (g_crBakeArm) {   // a bake starts: the other map cleared
        if (g_crBakeOpen) return;
        g_crBakeArm = false; g_crBakeOpen = true; g_crBakeTag = g_frame + 1; g_crBakeDrawn = 0; g_crBakeRec = 0; ++g_crBakeVer;
        ctx->ClearDepthStencilView(g_crMapDsv[g_crMapCur < 0 ? 0 : g_crMapCur ^ 1], D3D11_CLEAR_DEPTH, 1.0f, 0);
    }
    if (key == g_playerKey) { memcpy(g_crBakeMat, W, 64); g_crBakeMatTag = g_crBakeTag; }
    float ref[3] = { g_playerMat[12], g_playerMat[13], g_playerMat[14] };
    float tol = 3.5f + 0.1f * sqrtf(g_crVel[0] * g_crVel[0] + g_crVel[2] * g_crVel[2]);
    if (g_crBakeMatTag == g_crBakeTag) { memcpy(ref, g_crBakeMat + 12, 12); tol = 3.5f; }
    float dx = W[12] - ref[0], dy = W[13] - ref[1], dz = W[14] - ref[2];
    if (dx * dx + dy * dy + dz * dz > tol * tol) return;
    if (SsRecord(ctx, e, d, 0xF800, 0)) ++g_crBakeRec;
}

// the car's motion from its matrix read-backs: velocity, acceleration (m/s, m/s2, world), smoothed
static void CarRainMotion(double now)
{
    if (!g_playerSeen || g_playerSeen == g_crSeen) return;
    const float* p = g_playerMat + 12;
    if (g_crSeen && now > g_crLastT) {
        double dt = now - g_crLastT;
        if (dt > 0.004 && dt < 0.5) {
            float v[3], k = (float)(1 - exp(-dt / 0.15)), ka = (float)(1 - exp(-dt / 0.3));
            for (int j = 0; j < 3; ++j) v[j] = (float)((p[j] - g_crLastPos[j]) / dt);
            if (fabsf(v[0]) + fabsf(v[2]) >= 120) g_crJump = true;   // (a reset / a teleport)
            else {
                for (int j = 0; j < 3; ++j) {
                    float nv = g_crVel[j] + (v[j] - g_crVel[j]) * k;
                    g_crAcc[j] += ((nv - g_crVel[j]) / (float)dt - g_crAcc[j]) * ka;
                    g_crVel[j] = nv;
                }
            }
        }
    }
    memcpy(g_crLastPos, p, 12);
    g_crLastT = now;
    g_crSeen = g_playerSeen;
}

// once a frame (Present): the drops moved and drawn into the atlas and the trail map
static void CarRainTick(ID3D11DeviceContext* ctx)
{
    static double last;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    double now = g_qpf.QuadPart ? (double)q.QuadPart / g_qpf.QuadPart : 0;
    double dtd = last > 0 ? now - last : 0.016;
    last = now;
    float dt = (float)(dtd < 0 ? 0 : dtd > 0.1 ? 0.1 : dtd);
    CarRainMotion(now);
    {   // 1.81c: (the log) the frame's length, the glass passes' CPU time this frame
        float fms = (float)(dtd * 1000);
        if (fms > 40 && fms < 2000) ++g_crLong;
        if (fms < 2000 && fms > g_crFrameMax) g_crFrameMax = fms;
        if (g_crCpuGlass > g_crCpuGlassMax) g_crCpuGlassMax = g_crCpuGlass;
        g_crCpuGlass = 0;
    }
    struct TickT { LARGE_INTEGER a; ~TickT() { LARGE_INTEGER b; QueryPerformanceCounter(&b); double ms = g_qpf.QuadPart ? (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart : 0; if (ms > g_crCpuTickMax) g_crCpuTickMax = ms; } } tickT;
    QueryPerformanceCounter(&tickT.a);
    // 1.80: a bake of the car's shape this frame: its parts drawn (SsFlush), the map taken if it holds the car
    if (g_crBakeOpen) {
        SsFlush(ctx);
        float sp = sqrtf(g_crVel[0] * g_crVel[0] + g_crVel[1] * g_crVel[1] + g_crVel[2] * g_crVel[2]);
        if (g_crBakeDrawn >= 4 && (g_crBakeExact || sp < 0.5f)) { g_crMapCur = g_crMapCur < 0 ? 0 : g_crMapCur ^ 1; ++g_crBakeOk; }
        else ++g_crBakeBad;
        g_crBakeOpen = false;
        static unsigned logged;
        if (g_frame - logged > 1800 || g_crBakeOk + g_crBakeBad == 1) {
            logged = g_frame;
            Log("car rain: the car's shape baked %u times (%u not: %u of its draws recorded, %u drawn, its matrix of that frame %d)",
                g_crBakeOk, g_crBakeBad, g_crBakeRec, g_crBakeDrawn, g_crBakeExact);
        }
    }
    // (1.82: with the weather on, rain lands only while it falls, by how hard: weather.inc; else as the wet ground says)
    float slider = g_p[200].v, rain = g_wxRain >= 0 ? (g_wxRain > 0.01f ? g_wxRain : 0.0f) : (g_p[P_SSR + 1].v > 0 ? 1.0f : 0.0f);
    float landing = g_p[201].v, slide = g_p[202].v, runSpeed = g_p[203].v, trailT = g_p[204].v, size = g_p[205].v, wind = g_p[206].v, dryX = g_p[207].v;
    g_crDryT = rain > 0 ? 0 : g_crDryT + dt;
    // 1.81: the player's car lost a moment (its read-back late, a frame it was not found): the drops kept as they are
    // (paused; PS_Sun shows them by its pose of now); cleared only when it is lost 5 s, jumped far (a reset, a
    // teleport), the rain long gone or the drops off (1.80 cleared them on any 30 frames' loss: "the car lost")
    static unsigned logT;
    if (g_frame - logT >= 1800) {
        logT = g_frame;
        Log("car rain: drops cleared %u times, paused %u frames (the car not seen); its glass drawn again %u times; CPU most in a frame: "
            "the glass %.2f ms, the tick %.2f ms, the car's read-back %.2f ms; frames over 40 ms %u (the longest %.0f ms)", g_crResets, g_crPaused,
            g_crGlassDraws, g_crCpuGlassMax, g_crCpuTickMax, g_playerCpuRb, g_crLong, g_crFrameMax);
        g_crResets = g_crPaused = g_crGlassDraws = g_crLong = 0;
        g_crCpuGlassMax = g_crCpuTickMax = g_playerCpuRb = g_crFrameMax = 0;
    }
    bool keep = g_enabled && slider > 0 && !g_crJump && g_playerSeen && g_frame + 1 - g_playerSeen < 300 && (rain > 0 || (g_crValid && g_crDryT < 1200));
    if (!keep) { if (g_crValid) { g_crValid = false; g_crFresh = true; ++g_crResets; } g_crJump = false; return; }
    if (g_frame + 1 - g_playerSeen >= 30) { ++g_crPaused; return; }
    if (!CarRainEnsure()) return;
    if (g_frame - g_crBakeLast >= 45 && !g_crBakeOpen) { g_crBakeArm = true; g_crBakeLast = g_frame; }   // (its shape again: damage)
    // car-local: its axes from the matrix rows (x 0..2, y 4..6, z 8..10)
    float ax[3][3];
    for (int r = 0; r < 3; ++r) {
        float l = sqrtf(g_playerMat[r * 4] * g_playerMat[r * 4] + g_playerMat[r * 4 + 1] * g_playerMat[r * 4 + 1] + g_playerMat[r * 4 + 2] * g_playerMat[r * 4 + 2]);
        for (int j = 0; j < 3; ++j) ax[r][j] = l > 1e-5f ? g_playerMat[r * 4 + j] / l : (r == j ? 1.0f : 0.0f);
    }
    if (ax[1][1] < 0) for (int j = 0; j < 3; ++j) ax[1][j] = -ax[1][j];
    auto loc = [&](const float* w, float* o) { for (int r = 0; r < 3; ++r) o[r] = ax[r][0] * w[0] + ax[r][1] * w[1] + ax[r][2] * w[2]; };
    float g[3] = { 0, -9.81f, 0 }, gl[3], vl[3], al[3], wl[3];
    loc(g, gl); loc(g_crVel, vl); loc(g_crAcc, al); loc(g_wind, wl);   // (1.83: + the wind)
    // the fine beads blown off by the air at speed (from ~30 km/h), back with the rain when slow
    float spd = sqrtf(vl[0] * vl[0] + vl[2] * vl[2]);
    float blow = spd > 8 ? (spd - 8) * 0.04f * wind : 0;
    float fill = 0.05f * slider * rain;
    float exposure[3] = { 1.0f, 0.5f, 0.8f };
    for (int k = 0; k < 3; ++k) {
        g_crBlown[k] += (blow * exposure[k] * (1 - g_crBlown[k]) - fill * g_crBlown[k]) * dt;
        g_crBlown[k] = g_crBlown[k] < 0 ? 0 : g_crBlown[k] > 0.95f ? 0.95f : g_crBlown[k];
    }
    // 1.80: the rain past the car (it falls ~7 m/s; the car drives into it), its flux through each face (per m2 of the
    // face at most: a slant takes more per m2 of its unfolding), splashes off the ground and the wheels' spray
    float rw[3] = { g_wind[0], -7.0f, g_wind[2] }, rl[3];
    loc(rw, rl);
    float vr[3] = { rl[0] - vl[0], rl[1] - vl[1], rl[2] - vl[2] };
    float vrl = sqrtf(vr[0] * vr[0] + vr[1] * vr[1] + vr[2] * vr[2]);
    float splash = 0.3f + 0.05f * spd;
    float most[4] = { vrl * 1.8f, vrl * 1.8f + splash, vrl * 1.8f + splash, vrl * 1.8f + splash };
    float w[5] = { 9.6f * most[0], 6.96f * most[1], 6.96f * most[1], 2.9f * most[2], 2.9f * most[3] };
    float wsum = w[0] + w[1] + w[2] + w[3] + w[4];
    // ~17 drops a second per m2 and m/s of rain through it (heavy rain on a still car's top: ~120 / m2 / s)
    float tries = 17.0f * slider * landing * rain * wsum * dt / (float)(kCrW * kCrH);
    static unsigned rndN;
    unsigned rbits = (rndN++ + 1) * 2654435761u;
    float rf;
    memcpy(&rf, &rbits, 4);   // (its bits: asuint in the shader)
    float cb[40] = {
        dt, rf, tries, 0.0f,
        gl[0], gl[1], gl[2], 6.0e-5f * wind,   // (1.80: the air's drag: a 2 mm drop goes at ~45 km/h, a 0.5 mm one at ~90)
        -al[0], -al[1], -al[2], rain,
        wl[0] - vl[0], wl[1] - vl[1], wl[2] - vl[2], 0,   // (the air past the car: 1.83: the wind's too)
        (float)kCrAtlasW, (float)kCrAtlasH, (float)kCrW, (float)kCrH,
        w[0] / wsum, (w[0] + w[1]) / wsum, (w[0] + w[1] + w[2]) / wsum, (w[0] + w[1] + w[2] + w[3]) / wsum,
        vr[0], vr[1], vr[2], splash,
        most[0], most[1], most[2], most[3],
        g_crMapCur >= 0 ? 1.0f : 0.0f, 2.0f * slider * landing, (float)kCrGridW, (float)kCrGridH,
        1.0f / (slide > 0.05f ? slide : 0.05f), 1.0f / (runSpeed > 0.05f ? runSpeed : 0.05f), size, dryX };
    // save
    ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(8, sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16; ctx->RSGetViewports(&nVp, sVp);
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11ShaderResourceView* sVsrv[2] = {}; ctx->VSGetShaderResources(0, 2, sVsrv);
    ID3D11ShaderResourceView* sPsrv[9] = {}; ctx->PSGetShaderResources(0, 9, sPsrv);
    ID3D11Buffer* sVcb = 0; ctx->VSGetConstantBuffers(0, 1, &sVcb);
    ID3D11Buffer* sPcb = 0; ctx->PSGetConstantBuffers(0, 1, &sPcb);
    ID3D11ShaderResourceView* none[9] = {};
    ctx->PSSetShaderResources(73, 4, none);   // (ours for PS_Sun: written now)
    if (g_crFresh) {   // a start: no drops
        float z[4] = {};
        for (int k = 0; k < 2; ++k) { ctx->ClearRenderTargetView(g_crS[k].rtv, z); ctx->ClearRenderTargetView(g_crV[k].rtv, z); }
        ctx->ClearRenderTargetView(g_crTrail.rtv, z);
        ctx->ClearRenderTargetView(g_crAtlasRtv, z);
        g_crFresh = false;
        g_crBlown[0] = g_crBlown[1] = g_crBlown[2] = 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    auto upload = [&](float mode, float layer) { cb[15] = mode; cb[3] = layer; if (SUCCEEDED(ctx->Map(g_crCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, cb, sizeof(cb)); ctx->Unmap(g_crCb, 0); } };
    ctx->VSSetConstantBuffers(0, 1, &g_crCb);
    ctx->PSSetConstantBuffers(0, 1, &g_crCb);
    ctx->IASetInputLayout(0);
    ctx->RSSetState(g_crRs);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    UINT zero[4] = {};
    // 1) 1.80: the merging grid: the drops by cell, four layers (each the smallest index above the one before)
    D3D11_VIEWPORT gv = { 0, 0, (float)kCrGridW, (float)kCrGridH, 0, 1 };
    ctx->RSSetViewports(1, &gv);
    ctx->OMSetDepthStencilState(g_crDsLess, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->VSSetShader(g_crVsGrid, 0, 0);
    oPSSetShader(ctx, g_crPsGrid, 0, 0);
    ctx->VSSetShaderResources(0, 1, &g_crS[g_crCur].srv);
    for (int L = 0; L < kCrLayers; ++L) {
        upload(0, (float)L);
        ctx->PSSetShaderResources(3, 1, L ? &g_crGrid[L - 1].srv : none);
        ctx->ClearRenderTargetView(g_crGrid[L].rtv, (const float*)zero);
        ctx->ClearDepthStencilView(g_crGridDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &g_crGrid[L].rtv, g_crGridDsv);
        oDraw(ctx, kCrW * kCrH, 0);
        ctx->OMSetRenderTargets(0, 0, 0);
    }
    ctx->PSSetShaderResources(3, 1, none);
    ctx->VSSetShaderResources(0, 1, none);
    ctx->OMSetDepthStencilState(0, 0);
    upload(0, 0);
    // 2) into which drop each one flows
    D3D11_VIEWPORT sv = { 0, 0, (float)kCrW, (float)kCrH, 0, 1 };
    ctx->RSSetViewports(1, &sv);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_crVsFull, 0, 0);
    ID3D11ShaderResourceView* st[9] = { g_crS[g_crCur].srv, g_crV[g_crCur].srv, 0, g_crGrid[0].srv, g_crGrid[1].srv, g_crGrid[2].srv, g_crGrid[3].srv,
                                        g_crTrail.srv, g_crMapCur >= 0 ? g_crMapSrv[g_crMapCur] : 0 };
    ctx->OMSetRenderTargets(1, &g_crT.rtv, 0);
    ctx->PSSetShaderResources(0, 9, st);
    oPSSetShader(ctx, g_crPsTarget, 0, 0);
    oDraw(ctx, 3, 0);
    // 3) the simulation: the other state from this one
    int nx = g_crCur ^ 1;
    ID3D11RenderTargetView* simT[2] = { g_crS[nx].rtv, g_crV[nx].rtv };
    ctx->OMSetRenderTargets(2, simT, 0);
    st[2] = g_crT.srv;
    ctx->PSSetShaderResources(0, 9, st);
    oPSSetShader(ctx, g_crPsSim, 0, 0);
    oDraw(ctx, 3, 0);
    g_crCur = nx;
    ctx->PSSetShaderResources(0, 9, none);
    ID3D11ShaderResourceView* st2[2] = { g_crS[g_crCur].srv, g_crV[g_crCur].srv };
    D3D11_VIEWPORT av = { 0, 0, (float)kCrAtlasW, (float)kCrAtlasH, 0, 1 };
    ctx->RSSetViewports(1, &av);
    // 4) the trail map: dried a little (1.81: CarRainTrail, 3 s; it was 10 s: too long), this frame's paths added
    ctx->OMSetRenderTargets(1, &g_crTrail.rtv, 0);
    float k = expf(-dt / (trailT > 0.1f ? trailT : 0.1f)), bf[4] = { k, k, k, k };
    ctx->OMSetBlendState(g_crBlendDecay, bf, 0xffffffff);
    oPSSetShader(ctx, g_crPsDecay, 0, 0);
    oDraw(ctx, 3, 0);
    upload(1, 0);
    ctx->OMSetBlendState(g_crBlendMax, 0, 0xffffffff);
    ctx->VSSetShader(g_crVsDrop, 0, 0);
    ctx->VSSetShaderResources(0, 2, st2);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    oPSSetShader(ctx, g_crPsTrail, 0, 0);
    oDrawInstanced(ctx, 4, kCrW * kCrH, 0, 0);
    // 5) the atlas: the drops' shapes, the nearest kept (depth)
    upload(0, 0);
    ctx->ClearRenderTargetView(g_crAtlasRtv, (const float*)zero);
    ctx->ClearDepthStencilView(g_crDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ctx->OMSetRenderTargets(1, &g_crAtlasRtv, g_crDsv);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(g_crDsLess, 0);
    oPSSetShader(ctx, g_crPsAtlas, 0, 0);
    oDrawInstanced(ctx, 4, kCrW * kCrH, 0, 0);
    g_crValid = true;
    // restore
    ctx->VSSetShaderResources(0, 2, sVsrv); ctx->PSSetShaderResources(0, 9, sPsrv);
    ctx->OMSetRenderTargets(8, sRtv, sDsv);
    ctx->RSSetViewports(nVp, sVp);
    ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    ctx->OMSetBlendState(sBl, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
    ctx->VSSetConstantBuffers(0, 1, &sVcb); ctx->PSSetConstantBuffers(0, 1, &sPcb);
    IUnknown* rel[] = { sDsv, sVs, sPs, sIl, sBl, sDs, sRs, sVsrv[0], sVsrv[1], sVcb, sPcb };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
    for (int i = 0; i < 9; ++i) if (sPsrv[i]) sPsrv[i]->Release();
    for (int i = 0; i < 8; ++i) if (sRtv[i]) sRtv[i]->Release();
}

// 1.81: the drops on the player's car's glass. The game draws its glass after its lighting (PS_Sun saw the cabin through
// it; its drops there lay under the glass drawn over them): each of its glass draws (blended, of the player's car: near
// its part of this frame) is drawn once more with PS_CarGlass, blended over (dest x (1 - a) + c), with our constants
// and textures bound for it and the game's put back after
static ID3D11BlendState* g_crGlassBlend;
static void CarGlassDo(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d);
static void CarGlassAfterDraw(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d)
{
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    CarGlassDo(ctx, e, d);
    QueryPerformanceCounter(&b);
    if (g_qpf.QuadPart) g_crCpuGlass += (b.QuadPart - a.QuadPart) * 1000.0 / g_qpf.QuadPart;
}
static void CarGlassDo(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d)
{
    if (!g_crValid || !g_psCarGlass || !g_cb || !g_rainCb || g_p[200].v <= 0 || g_playerNowTag != g_frame + 1) return;
    ID3D11BlendState* bs = 0; FLOAT bf[4]; UINT bm;
    ctx->OMGetBlendState(&bs, bf, &bm);
    D3D11_BLEND_DESC bd = {};
    if (bs) bs->GetDesc(&bd);
    if (!bs || !bd.RenderTarget[0].BlendEnable) { if (bs) bs->Release(); return; }   // (glass: blended)
    ID3D11Buffer* cb = 0;
    ctx->VSGetConstantBuffers(e->wSlot, 1, &cb);
    const unsigned char* data = 0;
    UINT size = 0;
    int st = cb ? WeCbNow(cb, &data, &size) : 0;
    if (cb) cb->Release();
    if (st != 1 || (UINT)e->wOff + 64 > size) { bs->Release(); return; }
    const float* W = (const float*)(data + e->wOff);
    float dx = W[12] - g_playerNowT[0], dy = W[13] - g_playerNowT[1], dz = W[14] - g_playerNowT[2];
    if (dx * dx + dy * dy + dz * dz > 3.5f * 3.5f) { bs->Release(); return; }
    if (!g_crGlassBlend) {
        D3D11_BLEND_DESC gb = {};
        gb.IndependentBlendEnable = TRUE;
        gb.RenderTarget[0].BlendEnable = TRUE;
        gb.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; gb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; gb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        gb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; gb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; gb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        gb.RenderTarget[0].RenderTargetWriteMask = 7;   // (the colour; the other targets untouched)
        if (FAILED(g_dev->CreateBlendState(&gb, &g_crGlassBlend))) { g_crGlassBlend = 0; bs->Release(); return; }
    }
    // save
    ID3D11PixelShader* sPs = 0; ctx->PSGetShader(&sPs, 0, 0);
    ID3D11Buffer *sCb5 = 0, *sCb13 = 0; ctx->PSGetConstantBuffers(5, 1, &sCb5); ctx->PSGetConstantBuffers(13, 1, &sCb13);
    ID3D11ShaderResourceView *sSky[2] = {}, *sDrop[7] = {}; ctx->PSGetShaderResources(67, 2, sSky); ctx->PSGetShaderResources(73, 7, sDrop);
    ID3D11SamplerState* sSmp[3] = {}; ctx->PSGetSamplers(11, 3, sSmp);
    // ours
    ID3D11ShaderResourceView* sky[2] = { g_panoSrv[g_panoLast], g_probeSrv };
    ctx->PSSetShaderResources(67, 2, sky);
    CarRainBind(ctx);   // t73 .. t78
    ctx->PSSetShaderResources(79, 1, &g_camRec[g_frame & 1].srv);
    ctx->PSSetConstantBuffers(5, 1, &g_cb);
    ctx->PSSetConstantBuffers(13, 1, &g_rainCb);
    ID3D11SamplerState* smp[3] = { g_sPoint, g_sWrap, g_sText };
    ctx->PSSetSamplers(11, 3, smp);
    ctx->OMSetBlendState(g_crGlassBlend, 0, 0xffffffff);
    oPSSetShader(ctx, g_psCarGlass, 0, 0);
    Replay(ctx, d);
    ++g_crGlassDraws;
    // the game's again
    oPSSetShader(ctx, sPs, 0, 0);
    ctx->PSSetConstantBuffers(5, 1, &sCb5); ctx->PSSetConstantBuffers(13, 1, &sCb13);
    ctx->PSSetShaderResources(67, 2, sSky); ctx->PSSetShaderResources(73, 7, sDrop);
    ctx->PSSetSamplers(11, 3, sSmp);
    ctx->OMSetBlendState(bs, bf, bm);
    IUnknown* rel[] = { bs, sPs, sCb5, sCb13, sSky[0], sSky[1], sSmp[0], sSmp[1], sSmp[2] };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
    for (int i = 0; i < 7; ++i) if (sDrop[i]) sDrop[i]->Release();
}

static ID3D11ShaderResourceView* CarMapSrv() { return g_crMapCur >= 0 && g_crValid ? g_crMapSrv[g_crMapCur] : 0; }
// for PS_Sun (the sun pass): t73 atlas, t74 / t75 the drops, t76 the trails, t78 the car's shape (view 37)
static void CarRainBind(ID3D11DeviceContext* ctx)
{
    ID3D11ShaderResourceView* v[4] = { 0, 0, 0, 0 };
    if (g_crValid) { v[0] = g_crAtlasSrv; v[1] = g_crS[g_crCur].srv; v[2] = g_crV[g_crCur].srv; v[3] = g_crTrail.srv; }
    ctx->PSSetShaderResources(73, 4, v);
    ID3D11ShaderResourceView* map = CarMapSrv();
    ctx->PSSetShaderResources(78, 1, &map);
    ID3D11ShaderResourceView* pm = g_playerGpuFrame == g_frame + 1 ? g_playerGpuSrv : 0;   // 1.78: the player's car now
    ctx->PSSetShaderResources(77, 1, &pm);
}
