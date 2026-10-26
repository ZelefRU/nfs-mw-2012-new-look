// ---------------------------------------------------------------- grass on the lawns (1.97)
// The game's lawns are its terrain shaders (f0aaf6ee2b3884dc, d89be45b33ada3f2, ...: up to 3 tiling layers blended by a
// mask at maskTilingRatios; G-buffer o0 normal, o1 sqrt albedo + AO, o2 gloss, o3 sqrt baked light). 1.97c: found at
// creation by reflection (textures MaskTextureSampler, Tiling1..3TextureSampler, gMaterialCB's maskTilingRatios), each
// variant its own mask shader built from its input signature (1.97a knew f0aaf6ee alone; the lawns are d89be45b's).
// Which layers are grass: their textures' fingerprints (texreplace.inc; a2b6bb46a43d55e5 the lawn's). Stage 1: every
// terrain draw with a grass layer drawn once more (depth "equal", nothing of the game's written) into a mask: the grass
// share of the visible surface and its depth (x, y); view 38 shows it green in the G-buffer's albedo.

static const int P_GRASS = 261;
struct TerrPs { ID3D11PixelShader* ps; void* code; SIZE_T size; int tM, sM, t[3], cb, off; ID3D11PixelShader* mask; int state; };
static TerrPs g_terr[128];
static volatile LONG g_terrN;
static unsigned long long g_grassTex[64] = { 0xa2b6bb46a43d55e5ull };   // grass layers' textures
static int g_grassTexN = 1;
static ID3D11PixelShader* g_psGrassView;
static ID3D11Buffer* g_grassCb;
static float g_grassCbData[4];
static ID3D11DepthStencilState *g_grassDsEq, *g_grassDsOff;
static ID3D11BlendState* g_grassViewBlend;
static HalfRT g_grassMask;                          // x: the grass share, y: the surface's depth (the frame's size)
                                                    // (1.97k: x >= 0.004 on all of a lawn's draw, 1 on the blades: soft
                                                    // ground for the rain, t85; the blades' depth written into y)
static HalfRT g_grassMaskB;                         // 1.97k: its copy before the blades (their feet's check reads it)
static UINT g_grassMaskW, g_grassMaskH;
static unsigned g_grassMaskFrame, g_grassSunFrame;   // frame + 1: the mask cleared / the pass before the sun done
static int g_grassState;                            // the shared parts: 0 not tried, 1 ready, 2 failed
static unsigned g_grassTerrDraws, g_grassDraws;     // this log period: terrain draws, those with grass

static const char* kGrassViewSrc = R"HLSL(
Texture2D<float2> tMask : register(t0);
Texture2D<float> tDepth : register(t1);
float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 m = tMask.Load(int3(pos.xy, 0));
    // 1.97f: still the lawn at the end of the G-buffer (paving, kerbs, decals drawn over it later: another depth)
    float z = tDepth.Load(int3(pos.xy, 0));
    float w = abs(z - m.y) <= max(1e-6, abs(z) * 2e-5) ? m.x : 0;
    return float4(0.15, 1, 0.1, w * 0.85);
}
)HLSL";

// at pixel shader creation: a terrain shader? (its slots by reflection)
static void GrassNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (!g_reflect || !code || !ps) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    int tM = -1, sM = -1, t[3] = { -1, -1, -1 }, cb = -1, off = -1;
    for (UINT k = 0; k < sd.BoundResources; ++k) {
        D3D11_SHADER_INPUT_BIND_DESC b;
        if (FAILED(r->GetResourceBindingDesc(k, &b)) || !b.Name) continue;
        if (b.Type == D3D_SIT_TEXTURE) {
            if (!strcmp(b.Name, "MaskTextureSampler")) tM = b.BindPoint;
            for (int j = 0; j < 3; ++j) { char n[32]; sprintf(n, "Tiling%dTextureSampler", j + 1); if (!strcmp(b.Name, n)) t[j] = b.BindPoint; }
        } else if (b.Type == D3D_SIT_SAMPLER && !strcmp(b.Name, "MaskTextureSampler")) sM = b.BindPoint;
    }
    for (UINT c = 0; c < sd.ConstantBuffers && off < 0; ++c) {
        ID3D11ShaderReflectionConstantBuffer* cbr = r->GetConstantBufferByIndex(c);
        D3D11_SHADER_BUFFER_DESC bd;
        if (!cbr || FAILED(cbr->GetDesc(&bd))) continue;
        ID3D11ShaderReflectionVariable* v = cbr->GetVariableByName("maskTilingRatios");
        D3D11_SHADER_VARIABLE_DESC vd;
        if (!v || FAILED(v->GetDesc(&vd))) continue;
        D3D11_SHADER_INPUT_BIND_DESC b;
        if (FAILED(r->GetResourceBindingDescByName(bd.Name, &b))) continue;
        cb = b.BindPoint; off = vd.StartOffset;
    }
    r->Release();
    if (tM < 0 || sM < 0 || t[0] < 0 || off < 0 || cb == 10) return;
    LONG k = InterlockedIncrement(&g_terrN) - 1;
    if (k >= 128) { g_terrN = 128; return; }
    TerrPs& e = g_terr[k];
    e.code = malloc(size);
    if (e.code) { memcpy(e.code, code, size); e.size = size; }
    e.tM = tM; e.sM = sM; e.t[0] = t[0]; e.t[1] = t[1]; e.t[2] = t[2]; e.cb = cb; e.off = off; e.mask = 0; e.state = 0;
    MemoryBarrier();
    e.ps = ps;
    Log("grass: a terrain shader %016llx (%p): mask t%d s%d, layers t%d t%d t%d, tiling at b%d +%d", CodeHash(code, size), ps, tM, sM, t[0], t[1], t[2], cb, off);
}

// the shared parts (states, constants, the view's shader)
static bool GrassEnsure()
{
    if (g_grassState) return g_grassState == 1;
    g_grassState = 2;
    if (!g_compile || !g_reflect) return false;
    ID3DBlob *code = 0, *err = 0;
    if (SUCCEEDED(g_compile(kGrassViewSrc, strlen(kGrassViewSrc), "grassview", 0, 0, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_psGrassView);
        code->Release();
    } else if (err) { Log("grass: the view shader did not compile: %s", (const char*)err->GetBufferPointer()); err->Release(); }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, 0, &g_grassCb);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_EQUAL;
    g_dev->CreateDepthStencilState(&dd, &g_grassDsEq);
    dd.DepthEnable = FALSE; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_dev->CreateDepthStencilState(&dd, &g_grassDsOff);
    D3D11_BLEND_DESC bl = {};
    D3D11_RENDER_TARGET_BLEND_DESC& b = bl.RenderTarget[0];
    b.BlendEnable = TRUE; b.SrcBlend = D3D11_BLEND_SRC_ALPHA; b.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D11_BLEND_ZERO; b.DestBlendAlpha = D3D11_BLEND_ONE; b.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    g_dev->CreateBlendState(&bl, &g_grassViewBlend);
    if (!g_grassCb || !g_grassDsEq || !g_grassDsOff) { Log("grass: no states"); return false; }
    g_grassState = 1;
    return true;
}

// a terrain variant's mask shader: its inputs written out from its signature (register for register), the mask's uv
// from TEXCOORD0.xy (as the known variants), the tiling read from its constant buffer
static bool GrassMaskFor(TerrPs& e)
{
    if (e.state) return e.state == 1;
    e.state = 2;
    if (!e.code) return false;
    ID3D11ShaderReflection* rg = 0;
    if (FAILED(g_reflect(e.code, e.size, __uuidof(ID3D11ShaderReflection), (void**)&rg)) || !rg) return false;
    D3D11_SHADER_DESC gd;
    rg->GetDesc(&gd);
    D3D11_SIGNATURE_PARAMETER_DESC in[32];
    int n = 0;
    for (UINT k = 0; k < gd.InputParameters && n < 32; ++k) rg->GetInputParameterDesc(k, &in[n++]);
    static char src[8192];
    int L = 0;
    int reg = e.off / 16, comp = (e.off % 16) / 4;
    const char* sw = "xyzw";
    L += sprintf(src + L, "cbuffer M : register(b%d) { float4 C[%d]; };\ncbuffer GrassCB : register(b10) { float4 GF; };\n", e.cb, reg + 2);
    L += sprintf(src + L, "Texture2D tM : register(t%d);\nSamplerState sM : register(s%d);\nstruct In {\n", e.tM, e.sM);
    const char* uvName = 0;
    const char* posName = "e0";
    char names[32][8];
    for (int k = 0; k < n; ++k) {
        const D3D11_SIGNATURE_PARAMETER_DESC& p = in[k];
        int cnt = 0;
        for (int b = 0; b < 4; ++b) cnt += (p.Mask >> b) & 1;
        const char* ty = p.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : p.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" : "float";
        sprintf(names[k], "e%d", k);
        if (p.SystemValueType == D3D_NAME_POSITION) { L += sprintf(src + L, "    float4 %s : SV_Position;\n", names[k]); posName = names[k]; }
        else L += sprintf(src + L, "    %s%s %s : %s%u;\n", ty, cnt > 1 ? (cnt == 2 ? "2" : cnt == 3 ? "3" : "4") : "", names[k], p.SemanticName, p.SemanticIndex);
        if (!_stricmp(p.SemanticName, "TEXCOORD") && p.SemanticIndex == 0 && cnt >= 2) uvName = names[k];
    }
    if (!uvName) { rg->Release(); Log("grass: a terrain shader without TEXCOORD0.xy"); return false; }
    char ratio[64];
    if (comp < 3) sprintf(ratio, "float2(C[%d].%c, C[%d].%c)", reg, sw[comp], reg, sw[comp + 1]);
    else sprintf(ratio, "float2(C[%d].w, C[%d].x)", reg, reg + 1);
    L += sprintf(src + L, "};\nfloat4 main(In i) : SV_Target\n{\n    float3 m = tM.Sample(sM, i.%s.xy * %s).xyz;\n"
                          "    m /= max(dot(m, 1), 1e-4);\n    return float4(max(dot(m, GF.xyz), 0.004), i.%s.z, 0, 0);\n}\n", uvName, ratio, posName);
    ID3DBlob *code = 0, *err = 0;
    if (FAILED(g_compile(src, strlen(src), "grassmask", 0, 0, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        Log("grass: a mask shader did not compile: %s\n%s", err ? (const char*)err->GetBufferPointer() : "?", src);
        if (err) err->Release();
        rg->Release();
        return false;
    }
    bool same = false;
    ID3D11ShaderReflection* ro = 0;
    if (SUCCEEDED(g_reflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&ro))) {
        D3D11_SHADER_DESC a;
        ro->GetDesc(&a);
        same = a.InputParameters == gd.InputParameters;
        for (UINT k = 0; k < gd.InputParameters && same; ++k) {
            D3D11_SIGNATURE_PARAMETER_DESC p;
            ro->GetInputParameterDesc(k, &p);
            const D3D11_SIGNATURE_PARAMETER_DESC& q = in[k];
            same = p.Register == q.Register && p.Mask == q.Mask && p.SemanticIndex == q.SemanticIndex && !_stricmp(p.SemanticName, q.SemanticName);
            if (!same) Log("grass: input %u differs: ours %s%u r%u m%x, the game's %s%u r%u m%x", k, p.SemanticName, p.SemanticIndex, p.Register, p.Mask,
                           q.SemanticName, q.SemanticIndex, q.Register, q.Mask);
        }
        ro->Release();
    }
    rg->Release();
    if (same) g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &e.mask);
    code->Release();
    if (!e.mask) { Log("grass: a terrain variant (%p) not used: its inputs %s", e.ps, same ? "same, no shader" : "differ"); return false; }
    Log("grass: the mask shader for %p ready (%d inputs)", e.ps, n);
    e.state = 1;
    return true;
}

// ---------------------------------------------------------------- stage 2 (1.97g): the blades
// Before the game's sun draw (its G-buffer done): 1) every other screen pixel of the lawn (the mask, still the surface at
// the end: the final depth) back into the world by this frame's view (the terrain draw's ViewProjectionModified) and put
// into a top-down map round the camera (A: height, 1, grass share; B, C, D: the ground's o1 albedo, o3 baked light, o2);
// its mip chain fills the holes (a texel's values / A.y at the first level with any); 2) a world grid of clumps round the
// camera (fixed to the world: no swimming), each standing on the map where there is lawn, three blades of a few
// triangles made in the vertex shader, drawn into the game's G-buffer and depth (its depth test; the stencil the lawn's):
// the game's lighting, shadows and ours light them as the ground under them.
static const int P_GRASS_DIST = 262, P_GRASS_DENS = 263, P_GRASS_HEIGHT = 264;
static const UINT kGmSize = 512;
static float g_grassVP[16];
static unsigned g_grassVPFrame;                      // frame + 1: the view of this frame's main G-buffer read
static ID3D11RenderTargetView* g_grassGb[4];         // this frame's G-buffer targets and depth (held to the sun draw)
static ID3D11DepthStencilView* g_grassGbDsv;
static unsigned g_grassGbFrame;
static ID3D11Resource* g_grassGbRes[4];              // views of ours on its targets (to read the ground)
static ID3D11ShaderResourceView* g_grassGbSrv[4];
static ID3D11DepthStencilState* g_grassDsDraw;       // the terrain's depth test, written, no stencil
static ID3D11RasterizerState* g_grassRs;
static ID3D11Buffer* g_grassFcb;
static HalfRT g_gm[4];
static HalfRT g_gmC[4], g_gmP[4];                   // 1.97p: this frame's ground (no mips); the last frame's map (swapped)
static ID3D11VertexShader* g_vsGrassFull;
static ID3D11PixelShader* g_psGrassKeep;
static ID3D11Texture2D* g_grassDepthC;              // 1.97p: the depth copied for the blades' feet (when the depth read is
static ID3D11ShaderResourceView* g_grassDepthCSrv;  // the one they write: it could not be read while written)
static ID3D11SamplerState* g_grassSmp;
// 1.97o: the map's place for the rain's passes (t87: corner x, z, 1 / extent, on; the map A at t86): the lawns by their
// world place (the depth's match on the screen let puddles through)
static ID3D11Buffer* g_grassSoftBuf;
static ID3D11ShaderResourceView* g_grassSoftSrv;
static unsigned g_grassMapFrame;                    // frame + 1: the map made
static unsigned g_grassSoftN[3];                    // GrassSoftBind's calls, with no mask, with no map (the log)
static ID3D11VertexShader *g_vsGrassScatter, *g_vsGrassBlade;
static ID3D11PixelShader *g_psGrassScatter, *g_psGrassBlade;
static int g_grassDrawState;                         // 0 not tried, 1 ready, 2 failed
static unsigned g_grassInst;                         // the last frame's clumps (instances), for the log

static const char* kGrassDrawSrc = R"HLSL(
cbuffer GrassF : register(b0) { float4 R0, R1, R2, ZW, I0, I1, I2, SC, MP, CAM, PRM, WND, LV, LB, CR0, CR1; };
// CR0: the player's car's place, on (w); CR1: its side axis x, z, its forward axis x, z (unit, flat)
// SC: screen w, h, 1/w, 1/h; MP: the map's corner x, z, 1/extent, texels; CAM: eye, radius; PRM: spacing, grid, seconds,
// height; WND: wind x, z (m/s); LV: the ring's handover start, its outer radius, its level, the blades' width x
Texture2D<float2> tMask : register(t0);
Texture2D<float> tDepth : register(t1);
Texture2D tG1 : register(t2);
Texture2D tG3 : register(t3);
Texture2D tG2 : register(t4);
Texture2D tG0 : register(t5);
Texture2D tA : register(t0);
Texture2D tB : register(t1);
Texture2D tC : register(t2);
Texture2D tD : register(t3);
Texture2D<float2> tMaskB : register(t4);
Texture2D<float> tDepthB : register(t5);
SamplerState sLinG : register(s0);

struct SO { float4 pos : SV_Position; float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 c : TEXCOORD2; float4 d : TEXCOORD3; };
struct MO { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float4 d : SV_Target3; };
SO vsScatter(uint id : SV_VertexID)
{
    SO o = (SO)0;
    o.pos = float4(-2, -2, 0.5, 1);
    uint w2 = ((uint)SC.x + 1) >> 1;
    int2 px = int2(id % w2, id / w2) * 2;
    float2 m = tMask.Load(int3(px, 0));
    float z = tDepth.Load(int3(px, 0));
    bool lawn = m.x >= 0.02 && abs(z - m.y) <= max(1e-6, abs(z) * 2e-5);   // (not paving, kerbs drawn over it)
    // 1.97m: other ground seen (facing up: paving, roads, a car's roof) put in as not lawn: the map tells it from ground
    // not seen (behind the car the coarse levels had grown grass on the paving)
    float4 g0 = tG0.Load(int3(px, 0));
    if (!lawn && (z >= 0.99999 || g0.y * 2 - 1 < 0.7)) return o;
    float2 ndc = (px + 0.5) * SC.zw * float2(2, -2) + float2(-1, 1);
    float d = (ZW.y - z * ZW.w) / (z * ZW.z - ZW.x);
    float wc = d * ZW.z + ZW.w;
    float3 b = float3(ndc.x * wc - R0.w, ndc.y * wc - R1.w, d - R2.w);
    float3 p = float3(dot(I0.xyz, b), dot(I1.xyz, b), dot(I2.xyz, b));
    float2 uv = (p.xz - MP.xy) * MP.z;
    if (any(uv <= 0) || any(uv >= 1)) return o;
    o.pos = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0.5, 1);
    // A: height, seen, lawn, gloss code (o0.w); B, C: o1, o3; D: o2's rgb, the grass share (1.97m: the lawn's values
    // divided by A.z, the lawn's weight; the share was A.z)
    if (lawn) {
        o.a = float4(p.y, 1, 1, g0.w);
        o.b = tG1.Load(int3(px, 0));
        o.c = tG3.Load(int3(px, 0));
        o.d = float4(tG2.Load(int3(px, 0)).rgb, m.x);
    } else o.a = float4(0, 1, 0, 0);
    return o;
}
MO psScatter(SO i) { MO o; o.a = i.a; o.b = i.b; o.c = i.c; o.d = i.d; return o; }
// 1.97p: the map kept over the frames: this frame's ground (t0..t3) + the last map's (t4..t7, moved by LB.zw texels with
// the map's corner) x 0.8. Each frame's map was only what the screen showed that frame: the blades' heights and colours
// changed as the camera moved (blades jumping, their light switching in soft squares)
Texture2D tPA : register(t4);
Texture2D tPB : register(t5);
Texture2D tPC : register(t6);
Texture2D tPD : register(t7);
float4 vsFull(uint id : SV_VertexID) : SV_Position
{
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0.5, 1);
}
MO psKeep(float4 pos : SV_Position)
{
    int3 p = int3(pos.xy, 0), q = int3(int2(pos.xy) + int2(LB.zw), 0);
    MO o;
    o.a = tA.Load(p) + tPA.Load(q) * 0.8;
    o.b = tB.Load(p) + tPB.Load(q) * 0.8;
    o.c = tC.Load(p) + tPC.Load(q) * 0.8;
    o.d = tD.Load(p) + tPD.Load(q) * 0.8;
    return o;
}

uint H(int2 c, uint k)
{
    uint h = (uint)c.x * 73856093u ^ (uint)c.y * 19349663u ^ (k + 1u) * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; h *= 0x27d4eb2du; h ^= h >> 16;
    return h;
}
float Hf(int2 c, uint k) { return (H(c, k) & 0xffffu) / 65535.0; }
// 1.97l: smooth value noise (the lawn's patches of taller and shorter grass)
float VN(float2 p, uint k)
{
    int2 i = int2(floor(p));
    float2 f = p - floor(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(Hf(i, k), Hf(i + int2(1, 0), k), f.x), lerp(Hf(i + int2(0, 1), k), Hf(i + int2(1, 1), k), f.x), f.y);
}
float4 WorldToClip(float3 p)
{
    float d = dot(R2.xyz, p) + R2.w;
    return float4(dot(R0.xyz, p) + R0.w, dot(R1.xyz, p) + R1.w, d * ZW.x + ZW.y, d * ZW.z + ZW.w);
}
static const float2 kV[5] = { float2(-1, 0), float2(1, 0), float2(-0.75, 0.45), float2(0.75, 0.45), float2(0, 1) };
static const uint kI[9] = { 0, 1, 2, 1, 3, 2, 2, 3, 4 };
struct BO { float4 pos : SV_Position; float4 n : TEXCOORD0; float4 alb : TEXCOORD1; float4 lit : TEXCOORD2; float4 spec : TEXCOORD3; };
struct GO { float4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; float4 o3 : SV_Target3; float4 o4 : SV_Target4; };
BO vsBlade(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    BO o = (BO)0;
    int N = (int)PRM.y;
    float s = PRM.x;
    uint lk = (uint)LV.z * 256u;   // (1.97h: each ring its own clumps)
    int2 cell = int2(floor(CAM.xz / s)) - (N >> 1) + int2(inst % (uint)N, inst / (uint)N);
    float2 xz = (cell + float2(Hf(cell, lk), Hf(cell, lk + 1))) * s;
    float dist = length(xz - CAM.xz);
    // 1.97h: rings of coarser clumps further out, handed over dithered across the last fifth of a ring
    float band = LV.y * 0.2;
    if (dist > LV.y || dist > CAM.w) return o;
    // 1.97p: each clump grows in / shrinks out over a few tens of cm round its own distance in the band (it popped in and
    // out there as the camera moved)
    float hk = 1;
    if (LV.z > 0) {
        float lb = LV.x * 0.25;
        hk = dist < LV.x ? 0 : saturate((dist - (LV.x + (0.15 + 0.7 * Hf(cell, lk + 40)) * lb)) / (0.3 * lb) + 0.5);
    }
    if (LV.y < CAM.w) hk *= saturate(((LV.y - band + (0.15 + 0.7 * Hf(cell, lk + 41)) * band) - dist) / (0.3 * band) + 0.5);
    if (hk <= 0) return o;
    uint vpb = (uint)LB.y, k = vid / vpb, v = vid % vpb;   // (1.97i: the far rings' blades one triangle)
    uint q0 = lk + 3 + k * 7;
    float2 bp = xz + (float2(Hf(cell, q0 + 1), Hf(cell, q0 + 2)) - 0.5) * s * 0.9;
    float2 uv = (bp - MP.xy) * MP.z;
    if (any(uv <= 0) || any(uv >= 1)) return o;
    int2 t = int2(uv * MP.w);
    float4 a = 0;
    int L = 0;
    [loop] for (; L < 5; ++L) { a = tA.Load(int3(t >> L, L)); if (a.y > 1e-4) break; }
    if (a.z <= 1e-4) return o;   // nothing seen here, or no lawn
    float lawnK = a.z / a.y;     // the lawn's part of the ground seen round here
    float shK = saturate((tD.Load(int3(t >> L, L)).w / a.z - Hf(cell, lk + 2)) * 8);   // the grass share of the lawn there
    if (shK <= 0) return o;   // (1.97p: shorter near its edge, not on / off)
    float gy = a.x / a.z;
    // 1.97h: the blade's foot on the screen: where it is seen it must be lawn there (the coarse levels of the map spread
    // the lawn a metre past its kerbs); where it is hidden or off the screen the map is trusted near its own texels only
    float3 foot = float3(bp.x, gy, bp.y);
    float4 fc = WorldToClip(foot);
    bool seen = false, behind = false;
    if (fc.w > 0.05 && all(abs(fc.xy) < fc.w)) {
        int2 px = int2((fc.xy / fc.w * float2(0.5, -0.5) + 0.5) * SC.xy);
        float zp = tDepthB.Load(int3(px, 0));
        float dp = (ZW.y - zp * ZW.w) / (zp * ZW.z - ZW.x);   // the surface seen there, along the view
        float db = dot(R2.xyz, foot) + R2.w;
        float tol = 0.2 + db * 0.015;
        if (dp > db - tol) {
            float2 m = tMaskB.Load(int3(px, 0));
            if (m.x < 0.02 || abs(zp - m.y) > max(1e-6, abs(zp) * 2e-5) || dp > db + tol * 3) return o;
            seen = true;
        } else behind = dp < db - 0.5;   // (1.97l: behind a car, a post: no ground seen there; the map's holes filled)
    }
    if (!seen && (L > (behind ? 2 : 0) || (L > 0 && lawnK < 0.75))) return o;
    // 1.97j: the ground's colours from half a metre round (mip 2 or coarser): one texel's was the last screen pixel put
    // there, another each frame as the camera moved: the blades flickered ("a disco")
    // (1.97k: filtered: a texel's square showed, switching as the camera moved; the map's corner moves by 16 texels)
    float Lc = clamp(L, 2, 4);
    float4 ac = tA.SampleLevel(sLinG, uv, Lc);
    ac.z = max(ac.z, 1e-5);
    float4 gb = tB.SampleLevel(sLinG, uv, Lc) / ac.z, gc = tC.SampleLevel(sLinG, uv, Lc) / ac.z, gd = tD.SampleLevel(sLinG, uv, Lc) / ac.z;
    float ang = Hf(cell, q0) * 6.2832;
    float2 dir = float2(cos(ang), sin(ang));
    float fade = saturate((CAM.w - dist) / (CAM.w * 0.25));
    float ht = (0.14 + 0.24 * Hf(cell, q0 + 3)) * PRM.w * fade * hk * shK;
    // 1.97l: patches: taller and shorter grass a few metres across, now and then a tall tuft
    float patch = VN(bp / 4.5, 900) * 0.65 + VN(bp / 1.4, 901) * 0.35;
    ht *= lerp(0.45, 1.65, smoothstep(0.15, 0.85, patch));
    if (Hf(cell, lk + 50) < 0.04) ht *= 1.7;
    float wd = (0.012 + 0.012 * Hf(cell, q0 + 4)) * LV.w;
    float2 kv = kV[vpb < 9 ? (v == 2 ? 4 : v) : kI[v]];
    float tt = kv.y;
    float ph = dot(bp, float2(0.37, 0.23));
    float2 lean = float2(-dir.y, dir.x) * (Hf(cell, q0 + 5) - 0.5) * 0.5;
    lean += WND.xy * (0.035 * (0.65 + 0.35 * sin(PRM.z * 1.7 + ph * 2.0)));
    lean += float2(sin(PRM.z * 2.9 + ph * 5.1), cos(PRM.z * 2.3 + ph * 4.3)) * 0.05;
    float ll = length(lean);
    if (ll > 0.8) lean *= 0.8 / ll;
    // 1.97l: the player's car: the grass under it pressed flat, round it pushed aside (a box 2.1 x 4.6 m)
    if (CR0.w > 0.5 && abs(foot.y - CR0.y) < 2.5) {
        float2 rel = bp - CR0.xz;
        float2 l = float2(dot(rel, CR1.xy), dot(rel, CR1.zw));
        float2 q = abs(l) - float2(1.05, 2.3);
        float sd = length(max(q, 0)) + min(max(q.x, q.y), 0);
        float k = saturate(1 - sd / 0.7);
        float2 gl = q.x > q.y ? float2(sign(l.x), 0) : float2(0, sign(l.y));
        if (sd > 0) gl = max(q, 0) * sign(l) / max(length(max(q, 0)), 1e-4);
        float2 away = gl.x * CR1.xy + gl.y * CR1.zw;
        lean = lerp(lean, away * 1.3, k * k);
        ht *= lerp(1, 0.25, saturate(-sd / 0.25 + 0.2));
    }
    float3 p = foot + float3(dir.x, 0, dir.y) * (kv.x * wd);
    p.y += tt * ht * (1 - 0.35 * dot(lean, lean));
    p.xz += lean * ht * tt * tt;
    float3 T = normalize(float3(lean.x * 2 * tt * ht, ht + 1e-4, lean.y * 2 * tt * ht));
    float3 n = normalize(cross(float3(dir.x, 0, dir.y), T));
    // 1.97j: its side fixed in the world (it was turned to the camera: flipped as the camera moved), mostly up (the
    // lawn's light, the blade's shape a little)
    if (Hf(cell, q0 + 6) < 0.5) n = -n;
    n = normalize(n * 0.4 + float3(0, 1, 0));
    o.pos = WorldToClip(p);
    o.n = float4(n, ac.w / ac.z);
    float var = 0.85 + 0.3 * Hf(cell, q0 + 4);
    o.alb = float4(gb.rgb * lerp(0.88, 1.08, tt) * var, gb.a * lerp(0.75, 1, tt));   // (1.97i: the roots less dark)
    o.lit = float4(gc.rgb * lerp(0.85, 1, tt), gc.a);
    o.spec = float4(gd.rgb * 0.15, 1);   // (1.97h: matte: the ground's specular made the blades shine with the sky)
    return o;
}
GO psBlade(BO i)
{
    GO o;
    o.o0 = float4(normalize(i.n.xyz) * 0.5 + 0.5, i.n.w);   // (1.97i: w the ground's: 0 made them mirrors, 2^11)
    o.o1 = i.alb; o.o2 = i.spec; o.o3 = i.lit;
    o.o4 = float4(1, i.pos.z, 0, 0);   // (1.97k: the mask: soft ground here, its depth the blade's)
    return o;
}
)HLSL";

static void GrassGbFree()
{
    for (int k = 0; k < 4; ++k) if (g_grassGb[k]) { g_grassGb[k]->Release(); g_grassGb[k] = 0; }
    if (g_grassGbDsv) { g_grassGbDsv->Release(); g_grassGbDsv = 0; }
}

// at the main G-buffer's terrain draw (GrassAfterDraw): its view, targets and depth test, once a frame
static UINT g_grassStRef;   // 1.97m: the terrain draw's stencil value (written by the blades too)
static void GrassNoteGb(ID3D11DeviceContext* ctx, ID3D11RenderTargetView** rtv, ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* ds, UINT ref)
{
    if (g_grassGbFrame == g_frame + 1) return;
    GrassGbFree();
    for (int k = 0; k < 4; ++k) { g_grassGb[k] = rtv[k]; if (rtv[k]) rtv[k]->AddRef(); }
    g_grassGbDsv = dsv; if (dsv) dsv->AddRef();
    g_grassGbFrame = g_frame + 1;
    g_grassStRef = ref;
    if (!g_grassDsDraw && ds) {
        D3D11_DEPTH_STENCIL_DESC dd;
        ds->GetDesc(&dd);
        Log("grass: the terrain's stencil: on %d, read %02x write %02x, func %d pass %d, ref %u", (int)dd.StencilEnable, dd.StencilReadMask,
            dd.StencilWriteMask, (int)dd.FrontFace.StencilFunc, (int)dd.FrontFace.StencilPassOp, ref);
        // 1.97m: the blades write the lawn's stencil (they kept the car's where they covered it: the game's car pass,
        // by the stencil, drew the car over them: it showed through the grass)
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        if (!dd.StencilEnable) { dd.StencilEnable = TRUE; dd.StencilReadMask = 0xff; dd.StencilWriteMask = 0xff; }
        D3D11_DEPTH_STENCILOP_DESC so = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
        dd.FrontFace = dd.BackFace = so;
        if (dd.DepthFunc == D3D11_COMPARISON_EQUAL) dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        if (FAILED(g_dev->CreateDepthStencilState(&dd, &g_grassDsDraw))) g_grassDsDraw = 0;
        Log("grass: the terrain's depth test %d", (int)dd.DepthFunc);
    }
    VsInfo* e = DrawVs(ctx);
    if (e && e->vpSlot >= 0 && e->vpSlot < 14) {
        ID3D11Buffer* vcb = 0;
        ctx->VSGetConstantBuffers(e->vpSlot, 1, &vcb);
        const unsigned char* vd = 0;
        UINT vs = 0;
        if (vcb && WeCbNow(vcb, &vd, &vs) == 1 && (UINT)e->vpOff + 64 <= vs) { memcpy(g_grassVP, vd + e->vpOff, 64); g_grassVPFrame = g_frame + 1; }
        if (vcb) vcb->Release();
    }
}

static bool GrassDrawEnsure()
{
    if (g_grassDrawState) return g_grassDrawState == 1;
    g_grassDrawState = 2;
    struct { const char* entry; const char* prof; } sh[6] = { { "vsScatter", "vs_4_0" }, { "psScatter", "ps_4_0" }, { "vsBlade", "vs_4_0" }, { "psBlade", "ps_4_0" },
                                                              { "vsFull", "vs_4_0" }, { "psKeep", "ps_4_0" } };
    for (int k = 0; k < 6; ++k) {
        ID3DBlob *code = 0, *err = 0;
        if (FAILED(g_compile(kGrassDrawSrc, strlen(kGrassDrawSrc), "grassdraw", 0, 0, sh[k].entry, sh[k].prof, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            Log("grass: %s did not compile: %s", sh[k].entry, err ? (const char*)err->GetBufferPointer() : "?");
            if (err) err->Release();
            return false;
        }
        const void* c = code->GetBufferPointer();
        SIZE_T n = code->GetBufferSize();
        if (k == 0) g_dev->CreateVertexShader(c, n, 0, &g_vsGrassScatter);
        if (k == 1) g_dev->CreatePixelShader(c, n, 0, &g_psGrassScatter);
        if (k == 2) g_dev->CreateVertexShader(c, n, 0, &g_vsGrassBlade);
        if (k == 3) g_dev->CreatePixelShader(c, n, 0, &g_psGrassBlade);
        if (k == 4) g_dev->CreateVertexShader(c, n, 0, &g_vsGrassFull);
        if (k == 5) g_dev->CreatePixelShader(c, n, 0, &g_psGrassKeep);
        code->Release();
    }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16 * 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, 0, &g_grassFcb);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_grassRs);
    bool ok = MakeRT(g_gm[0], kGmSize, kGmSize, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    for (int k = 1; k < 4 && ok; ++k) ok = MakeRT(g_gm[k], kGmSize, kGmSize, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    for (int k = 0; k < 4 && ok; ++k) ok = MakeRT(g_gmC[k], kGmSize, kGmSize, k ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT, false)
                                          && MakeRT(g_gmP[k], kGmSize, kGmSize, k ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    ok = ok && g_vsGrassFull && g_psGrassKeep;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_grassSmp);
    {
        D3D11_BUFFER_DESC sb = {};
        sb.ByteWidth = 16; sb.Usage = D3D11_USAGE_DEFAULT; sb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        float zero[4] = {};
        D3D11_SUBRESOURCE_DATA si = { zero, 0, 0 };
        if (SUCCEEDED(g_dev->CreateBuffer(&sb, &si, &g_grassSoftBuf))) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vd.Buffer.NumElements = 1;
            g_dev->CreateShaderResourceView(g_grassSoftBuf, &vd, &g_grassSoftSrv);
        }
        if (!g_grassSoftSrv) Log("grass: no map place for the rain");
    }
    if (!ok || !g_grassSmp || !g_vsGrassScatter || !g_psGrassScatter || !g_vsGrassBlade || !g_psGrassBlade || !g_grassFcb || !g_grassRs) { Log("grass: the blades' parts not made"); return false; }
    Log("grass: the blades ready");
    g_grassDrawState = 1;
    return true;
}

// our view of a G-buffer target's texture
static ID3D11ShaderResourceView* GrassGbSrv(int k)
{
    if (!g_grassGb[k]) return 0;
    ID3D11Resource* r = 0;
    g_grassGb[k]->GetResource(&r);
    if (!r) return 0;
    r->Release();   // (held by the game's view)
    if (r != g_grassGbRes[k]) {
        if (g_grassGbSrv[k]) g_grassGbSrv[k]->Release();
        g_grassGbSrv[k] = 0; g_grassGbRes[k] = r;
        D3D11_RENDER_TARGET_VIEW_DESC vd;
        g_grassGb[k]->GetDesc(&vd);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = vd.Format; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(r, &sd, &g_grassGbSrv[k]))) { g_grassGbSrv[k] = 0; Log("grass: no view on G-buffer target %d (format %d)", k, (int)vd.Format); }
    }
    return g_grassGbSrv[k];
}

// 1.97l: the player's car (rain.inc's: its world matrix, rows the axes, the 4th its place): which axis is its length
// learnt from its motion (the one it moves along), until then the 3rd
static void GrassCar(float* c)
{
    static int fwAxis = 2;
    static float last[3];
    static unsigned lastSeen;
    if (!g_playerSeen || g_frame + 1 - g_playerSeen > 3) return;
    const float* m = g_playerMat;
    if (g_playerSeen != lastSeen) {
        float d[3] = { m[12] - last[0], m[13] - last[1], m[14] - last[2] };
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (lastSeen && dl > 0.05f && dl < 3) {
            float a0 = fabsf(d[0] * m[0] + d[1] * m[1] + d[2] * m[2]), a2 = fabsf(d[0] * m[8] + d[1] * m[9] + d[2] * m[10]);
            float l0 = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), l2 = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
            if (l0 > 1e-4f && l2 > 1e-4f) {
                int now = a0 / l0 > 1.5f * a2 / l2 ? 0 : a2 / l2 > 1.5f * a0 / l0 ? 2 : fwAxis;
                if (now != fwAxis) { fwAxis = now; Log("grass: the car's length along its axis %d", fwAxis); }
            }
        }
        last[0] = m[12]; last[1] = m[13]; last[2] = m[14]; lastSeen = g_playerSeen;
    }
    const float* f = m + fwAxis * 4;
    const float* s = m + (2 - fwAxis) * 4;
    float fl = sqrtf(f[0] * f[0] + f[2] * f[2]), sl = sqrtf(s[0] * s[0] + s[2] * s[2]);
    if (fl < 1e-4f || sl < 1e-4f) return;
    c[0] = m[12]; c[1] = m[13]; c[2] = m[14]; c[3] = 1;
    c[4] = s[0] / sl; c[5] = s[2] / sl; c[6] = f[0] / fl; c[7] = f[2] / fl;
}

// the frame's view (rows x, y, depth; z, w scale and bias), the 3x3 inverse of its rows, the eye
static bool GrassView(float* cb)
{
    const float* r = g_grassVP;
    if (fabsf(r[14]) < 1e-6f) return false;
    float M[3][3] = { { r[0], r[1], r[2] }, { r[4], r[5], r[6] }, { r[8], r[9], r[10] } };
    float det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (fabsf(det) < 1e-12f) return false;
    float I[3][3];
    I[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det; I[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det; I[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
    I[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det; I[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det; I[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det;
    I[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det; I[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det; I[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
    memcpy(cb, r, 64);
    for (int i = 0; i < 3; ++i) { cb[16 + i * 4] = I[i][0]; cb[17 + i * 4] = I[i][1]; cb[18 + i * 4] = I[i][2]; cb[19 + i * 4] = 0; }
    float b[3] = { -r[3], -r[7], -r[15] / r[14] - r[11] };   // the eye: x = y = 0, w = 0
    for (int i = 0; i < 3; ++i) cb[36 + i] = I[i][0] * b[0] + I[i][1] * b[1] + I[i][2] * b[2];
    return true;
}

// 1.97p: the depth for the blades' feet: the frame's depth, or its copy when it is the depth the blades write (a view
// read while its texture is bound for writing is dropped by D3D: the feet's check had read nothing)
static ID3D11ShaderResourceView* GrassFeetDepth(ID3D11DeviceContext* ctx)
{
    ID3D11Resource *a = 0, *b = 0;
    g_air.depth->GetResource(&a);
    g_grassGbDsv->GetResource(&b);
    bool same = a && a == b;
    if (b) b->Release();
    static int logged;
    if (!logged) { logged = 1; Log("grass: the depth read %s the depth the blades write", same ? "is" : "is not"); }
    ID3D11ShaderResourceView* v = g_air.depth;
    if (same) {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D*)a)->GetDesc(&d);
        if (g_grassDepthC) {
            D3D11_TEXTURE2D_DESC c;
            g_grassDepthC->GetDesc(&c);
            if (c.Width != d.Width || c.Height != d.Height || c.Format != d.Format) {
                if (g_grassDepthCSrv) g_grassDepthCSrv->Release();
                g_grassDepthC->Release();
                g_grassDepthC = 0; g_grassDepthCSrv = 0;
            }
        }
        static int failed;
        if (!g_grassDepthC && !failed) {
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = 0; d.CPUAccessFlags = 0; d.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd;
            g_air.depth->GetDesc(&sd);
            if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_grassDepthC)) || FAILED(g_dev->CreateShaderResourceView(g_grassDepthC, &sd, &g_grassDepthCSrv))) {
                if (g_grassDepthC) g_grassDepthC->Release();
                g_grassDepthC = 0; g_grassDepthCSrv = 0; failed = 1;
                Log("grass: no copy of the depth (format %d)", (int)d.Format);
            } else Log("grass: the depth copied for the feet (%ux%u, format %d)", d.Width, d.Height, (int)d.Format);
        }
        if (g_grassDepthC) { ctx->CopyResource(g_grassDepthC, a); v = g_grassDepthCSrv; }
        else v = 0;
    }
    if (a) a->Release();
    return v;
}

// before the sun draw: the map, then the blades into the G-buffer
static void GrassDraw(ID3D11DeviceContext* ctx)
{
    if (g_grassVPFrame != g_frame + 1 || g_grassGbFrame != g_frame + 1 || !g_grassGbDsv || !g_grassDsDraw || !g_air.depth || !g_grassMask.srv) return;
    for (int k = 0; k < 4; ++k) if (!g_grassGb[k]) return;
    if (!GrassDrawEnsure()) return;
    ID3D11ShaderResourceView *g0 = GrassGbSrv(0), *g1 = GrassGbSrv(1), *g2 = GrassGbSrv(2), *g3 = GrassGbSrv(3);
    if (!g0 || !g1 || !g2 || !g3) return;
    float cb[64] = {};
    if (!GrassView(cb)) return;
    // 1.97h: rings: the first to 10 m at the full density, each next twice as far with clumps twice as far apart (as many
    // instances each)
    float R = g_p[P_GRASS_DIST].v, dens = g_p[P_GRASS_DENS].v;
    if (R < 2) R = 2;
    float s = 0.16f / sqrtf(dens > 0.1f ? dens : 0.1f);
    float R0 = R < 10 ? R : 10.0f;
    int N = (int)(2 * R0 / s) + 2;
    if (N > 400) { N = 400; R0 = (N - 2) * s * 0.5f; }
    float ext = 2 * (R + 1);
    const float* cam = cb + 36;
    float W = (float)g_grassMaskW, Hh = (float)g_grassMaskH;
    float sc[4] = { W, Hh, 1 / W, 1 / Hh };
    memcpy(cb + 28, sc, 16);
    float tex = ext / kGmSize;
    cb[32] = floorf((cam[0] - ext * 0.5f) / (tex * 16)) * tex * 16; cb[33] = floorf((cam[2] - ext * 0.5f) / (tex * 16)) * tex * 16; cb[34] = 1 / ext; cb[35] = (float)kGmSize;
    cb[39] = R;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    cb[40] = s; cb[41] = (float)N; cb[42] = g_qpf.QuadPart ? (float)fmod((double)q.QuadPart / g_qpf.QuadPart, 3600.0) : 0.0f; cb[43] = g_p[P_GRASS_HEIGHT].v;
    cb[44] = g_wind[0]; cb[45] = g_wind[2];
    GrassCar(cb + 56);
    // 1.97p: the last frame's map: by how many texels its corner lies from this one's (none: far outside, read as 0)
    static float gmPrev[3];
    static unsigned gmPrevFrame;
    cb[54] = cb[55] = 1e5f;
    if (gmPrevFrame && g_frame + 1 - gmPrevFrame <= 2 && gmPrev[2] == ext) {
        float ox = floorf((cb[32] - gmPrev[0]) / tex + 0.5f), oz = floorf((cb[33] - gmPrev[1]) / tex + 0.5f);
        if (fabsf(ox) < kGmSize && fabsf(oz) < kGmSize) { cb[54] = ox; cb[55] = oz; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_grassFcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, cb, sizeof(cb));
    ctx->Unmap(g_grassFcb, 0);
    ProfMark(ctx, PR_GRASS, 0);
    // save
    ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(8, sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16; ctx->RSGetViewports(&nVp, sVp);
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ID3D11GeometryShader* sGs = 0;
    ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0); ctx->GSGetShader(&sGs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11ShaderResourceView* sVsrv[6] = {}; ctx->VSGetShaderResources(0, 6, sVsrv);
    static ID3D11ShaderResourceView* sPsrv[128];
    ctx->PSGetShaderResources(0, 128, sPsrv);
    ID3D11Buffer* sVcb = 0; ctx->VSGetConstantBuffers(0, 1, &sVcb);
    ID3D11Buffer* sPcb = 0; ctx->PSGetConstantBuffers(0, 1, &sPcb);
    ID3D11SamplerState* sVsm = 0; ctx->VSGetSamplers(0, 1, &sVsm);
    ID3D11ShaderResourceView* none[6] = {};
    static ID3D11ShaderResourceView* const nul[128] = {};
    // (the G-buffer and depth are bound for the sun draw's reading: off while we write them)
    ctx->PSSetShaderResources(0, 128, nul);
    // 1) the map
    float z[4] = {};
    for (int k = 0; k < 4; ++k) ctx->ClearRenderTargetView(g_gmC[k].rtv, z);
    ID3D11RenderTargetView* mr[4] = { g_gmC[0].rtv, g_gmC[1].rtv, g_gmC[2].rtv, g_gmC[3].rtv };
    ctx->OMSetRenderTargets(4, mr, 0);
    D3D11_VIEWPORT mv = { 0, 0, (float)kGmSize, (float)kGmSize, 0, 1 };
    ctx->RSSetViewports(1, &mv);
    ctx->RSSetState(g_grassRs);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(g_grassDsOff, 0);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->GSSetShader(0, 0, 0);
    ctx->VSSetShader(g_vsGrassScatter, 0, 0);
    oPSSetShader(ctx, g_psGrassScatter, 0, 0);
    ctx->VSSetConstantBuffers(0, 1, &g_grassFcb);
    ID3D11ShaderResourceView* si[6] = { g_grassMask.srv, g_air.depth, g1, g3, g2, g0 };
    ctx->VSSetShaderResources(0, 6, si);
    oDraw(ctx, ((g_grassMaskW + 1) / 2) * ((g_grassMaskH + 1) / 2), 0);
    ctx->VSSetShaderResources(0, 6, none);
    // 1.97p: + the last frame's map (moved with the corner) x 0.8: the map kept over the frames
    for (int k = 0; k < 4; ++k) { HalfRT t = g_gm[k]; g_gm[k] = g_gmP[k]; g_gmP[k] = t; }
    ID3D11RenderTargetView* kr[4] = { g_gm[0].rtv, g_gm[1].rtv, g_gm[2].rtv, g_gm[3].rtv };
    ctx->OMSetRenderTargets(4, kr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vsGrassFull, 0, 0);
    oPSSetShader(ctx, g_psGrassKeep, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_grassFcb);
    ID3D11ShaderResourceView* ki[8] = { g_gmC[0].srv, g_gmC[1].srv, g_gmC[2].srv, g_gmC[3].srv, g_gmP[0].srv, g_gmP[1].srv, g_gmP[2].srv, g_gmP[3].srv };
    ctx->PSSetShaderResources(0, 8, ki);
    oDraw(ctx, 3, 0);
    ctx->PSSetShaderResources(0, 8, nul);
    ctx->OMSetRenderTargets(0, 0, 0);
    gmPrev[0] = cb[32]; gmPrev[1] = cb[33]; gmPrev[2] = ext; gmPrevFrame = g_frame + 1;
    for (int k = 0; k < 4; ++k) ctx->GenerateMips(g_gm[k].srv);
    if (g_grassSoftBuf) {   // 1.97o: its place for the rain's passes
        float gs[4] = { cb[32], cb[33], cb[34], 1 };
        ctx->UpdateSubresource(g_grassSoftBuf, 0, 0, gs, 0, 0);
        g_grassMapFrame = g_frame + 1;
    }
    // 2) the blades (1.97k: the mask a fifth target: soft ground under the blades, their depth; its copy for the feet)
    ID3D11ShaderResourceView* feetD = GrassFeetDepth(ctx);   // (1.97p: before the depth is bound for writing)
    ctx->CopyResource(g_grassMaskB.t, g_grassMask.t);
    ID3D11RenderTargetView* br[5] = { g_grassGb[0], g_grassGb[1], g_grassGb[2], g_grassGb[3], g_grassMask.rtv };
    ctx->OMSetRenderTargets(5, br, g_grassGbDsv);
    ctx->VSSetSamplers(0, 1, &g_grassSmp);
    D3D11_VIEWPORT gv = { 0, 0, W, Hh, 0, 1 };
    ctx->RSSetViewports(1, &gv);
    ctx->OMSetDepthStencilState(g_grassDsDraw, g_grassStRef);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vsGrassBlade, 0, 0);
    oPSSetShader(ctx, g_psGrassBlade, 0, 0);
    ID3D11ShaderResourceView* bi[6] = { g_gm[0].srv, g_gm[1].srv, g_gm[2].srv, g_gm[3].srv, g_grassMaskB.srv, feetD };
    ctx->VSSetShaderResources(0, 6, bi);
    g_grassInst = 0;
    for (int l = 0; l < 6 && g_p[P_GRASS].v > 0.5f; ++l) {
        float Rl = R0 * (float)(1 << l);
        if (l && R0 * (float)(1 << (l - 1)) >= R) break;
        cb[40] = s * (float)(1 << l);
        cb[48] = l ? 0.8f * R0 * (float)(1 << (l - 1)) : 0.0f; cb[49] = Rl; cb[50] = (float)l; cb[51] = powf(1.6f, (float)l);
        // 1.97i: blades a clump: 5 near (3 triangles each), then 8, 12, 16 of one triangle (the far rings were thin)
        int nb = l ? (6 + 6 * l < 24 ? 6 + 6 * l : 24) : 7, vpb = l ? 3 : 9;   // (1.97j: 7, then 12, 18, 24)
        cb[52] = (float)nb; cb[53] = (float)vpb;
        if (FAILED(ctx->Map(g_grassFcb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) break;
        memcpy(m.pData, cb, sizeof(cb));
        ctx->Unmap(g_grassFcb, 0);
        oDrawInstanced(ctx, (UINT)(nb * vpb), (UINT)(N * N), 0, 0);
        g_grassInst += (UINT)(N * N);
    }
    ctx->VSSetShaderResources(0, 6, none);
    // restore
    ctx->OMSetRenderTargets(8, sRtv, sDsv);
    ctx->VSSetShaderResources(0, 6, sVsrv);
    ctx->PSSetShaderResources(0, 128, sPsrv);
    ctx->RSSetViewports(nVp, sVp);
    ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0); ctx->GSSetShader(sGs, 0, 0);
    ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    ctx->OMSetBlendState(sBl, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
    ctx->VSSetConstantBuffers(0, 1, &sVcb);
    ctx->PSSetConstantBuffers(0, 1, &sPcb);
    ctx->VSSetSamplers(0, 1, &sVsm);
    IUnknown* rel[] = { sDsv, sVs, sPs, sGs, sIl, sBl, sDs, sRs, sVcb, sVsm, sPcb };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
    for (int i = 0; i < 6; ++i) if (sVsrv[i]) sVsrv[i]->Release();
    for (int i = 0; i < 128; ++i) if (sPsrv[i]) { sPsrv[i]->Release(); sPsrv[i] = 0; }
    for (int i = 0; i < 8; ++i) if (sRtv[i]) sRtv[i]->Release();
    ProfMark(ctx, PR_GRASS, 1);
}

static TerrPs* GrassTerrOf(ID3D11PixelShader* ps)
{
    LONG n = g_terrN;
    for (LONG k = 0; k < n && k < 128; ++k) if (g_terr[k].ps == ps) return &g_terr[k];
    return 0;
}

static bool GrassIsTex(ID3D11ShaderResourceView* v)
{
    if (!v) return false;
    ID3D11Resource* r = 0;
    v->GetResource(&r);
    if (!r) return false;
    TxInfo* e = TxFind((ID3D11Texture2D*)r, false);
    r->Release();
    if (!e || !e->hash) return false;
    for (int k = 0; k < g_grassTexN; ++k) if (g_grassTex[k] == e->hash) return true;
    return false;
}

// 1.97n: before a draw (gamewet.inc): a lawn's? (a terrain shader, a grass layer bound; the grass on)
// 1.98: the lawns are found with the grass off too (no puddles on them); "Grass" draws the blades only
static bool GrassDryDraw(ID3D11DeviceContext* ctx)
{
    if (!g_terrN) return false;
    TerrPs* te = GrassTerrOf(g_lastPS);
    if (!te) return false;
    bool g = false;
    for (int k = 0; k < 3 && !g; ++k) {
        if (te->t[k] < 0) continue;
        ID3D11ShaderResourceView* v = 0;
        ctx->PSGetShaderResources(te->t[k], 1, &v);
        g = GrassIsTex(v);
        if (v) v->Release();
    }
    return g;
}

// after a terrain draw: its grass share into the mask where it is the visible surface
static void GrassAfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    TerrPs* te = GrassTerrOf(g_lastPS);
    if (!te || !g_enabled || !g_air.normal || !GrassEnsure() || !GrassMaskFor(*te)) return;
    ++g_grassTerrDraws;
    ID3D11ShaderResourceView* sv[3] = {};
    for (int k = 0; k < 3; ++k) if (te->t[k] >= 0) ctx->PSGetShaderResources(te->t[k], 1, &sv[k]);
    float f[4] = { GrassIsTex(sv[0]) ? 1.0f : 0.0f, GrassIsTex(sv[1]) ? 1.0f : 0.0f, GrassIsTex(sv[2]) ? 1.0f : 0.0f, 0 };
    for (int k = 0; k < 3; ++k) if (sv[k]) sv[k]->Release();
    if (f[0] + f[1] + f[2] <= 0) return;
    ++g_grassDraws;
    ID3D11RenderTargetView* rtv[4] = {};
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(4, rtv, &dsv);
    // 1.97d: the main G-buffer only (its normal target the one the game's lighting reads): the terrain is drawn into
    // reflections and maps too (512^2, 1024^2: the mask was made again at each size, several times a frame)
    ID3D11Resource* nres = 0;
    g_air.normal->GetResource(&nres);
    if (nres) nres->Release();
    bool gb = false;
    for (int k = 0; k < 4; ++k) if (rtv[k]) { ID3D11Resource* q = 0; rtv[k]->GetResource(&q); if (q == nres) gb = true; if (q) q->Release(); }
    if (rtv[0] && dsv && gb) {
        ID3D11Resource* r = 0;
        rtv[0]->GetResource(&r);
        D3D11_TEXTURE2D_DESC td = {};
        if (r) { ((ID3D11Texture2D*)r)->GetDesc(&td); r->Release(); }
        if (td.Width && (td.Width != g_grassMaskW || td.Height != g_grassMaskH || !g_grassMask.t)) {
            FreeRT(g_grassMask);
            FreeRT(g_grassMaskB);
            if (MakeRT(g_grassMask, td.Width, td.Height, DXGI_FORMAT_R32G32_FLOAT, false) && MakeRT(g_grassMaskB, td.Width, td.Height, DXGI_FORMAT_R32G32_FLOAT, false)) { g_grassMaskW = td.Width; g_grassMaskH = td.Height; Log("grass: mask %ux%u", td.Width, td.Height); }
            else { g_grassMaskW = g_grassMaskH = 0; Log("grass: no mask target"); }
        }
        if (g_grassMask.t) {
            if (g_grassMaskFrame != g_frame + 1) {
                float z[4] = {};
                ctx->ClearRenderTargetView(g_grassMask.rtv, z);
                g_grassMaskFrame = g_frame + 1;
            }
            if (memcmp(f, g_grassCbData, 16)) { memcpy(g_grassCbData, f, 16); ctx->UpdateSubresource(g_grassCb, 0, 0, f, 0, 0); }
            ID3D11PixelShader* sPs = 0; ctx->PSGetShader(&sPs, 0, 0);
            ID3D11Buffer* sCb = 0; ctx->PSGetConstantBuffers(10, 1, &sCb);
            ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ctx->OMGetDepthStencilState(&sDs, &sRef);
            ID3D11BlendState* sBl = 0; FLOAT bf[4]; UINT bm; ctx->OMGetBlendState(&sBl, bf, &bm);
            ctx->OMSetRenderTargets(1, &g_grassMask.rtv, dsv);
            ctx->OMSetDepthStencilState(g_grassDsEq, 0);
            ctx->OMSetBlendState(0, 0, 0xffffffff);
            oPSSetShader(ctx, te->mask, 0, 0);
            ctx->PSSetConstantBuffers(10, 1, &g_grassCb);
            Replay(ctx, d);
            oPSSetShader(ctx, sPs, 0, 0);
            ctx->PSSetConstantBuffers(10, 1, &sCb);
            ctx->OMSetDepthStencilState(sDs, sRef);
            ctx->OMSetBlendState(sBl, bf, bm);
            ctx->OMSetRenderTargets(4, rtv, dsv);
            GrassNoteGb(ctx, rtv, dsv, sDs, sRef);   // 1.97g: the view, targets and depth test for the blades
            IUnknown* rel[] = { sPs, sCb, sDs, sBl };
            for (int k = 0; k < 4; ++k) if (rel[k]) rel[k]->Release();
        }
    }
    for (int k = 0; k < 4; ++k) if (rtv[k]) rtv[k]->Release();
    if (dsv) dsv->Release();
}

// 1.97k: the mask for the rain's passes (t85): soft ground, no puddles, no wet sheen (none when not made this frame)
// 1.97o: + the map (t86) and its place (t87); this frame's or the last's (the wet gloss pass comes before the map's
// making), not only this frame's
static void GrassSoftBind(ID3D11DeviceContext* ctx)
{
    bool on = g_enabled;   // (1.98: the lawns with the grass off too)
    bool mask = on && g_grassMask.srv && g_grassMaskFrame && g_frame + 1 - g_grassMaskFrame <= 1;
    bool map = on && g_grassSoftSrv && g_gm[0].srv && g_grassMapFrame && g_frame + 1 - g_grassMapFrame <= 1;
    ++g_grassSoftN[0];
    if (!mask) ++g_grassSoftN[1];
    if (!map) ++g_grassSoftN[2];
    ID3D11ShaderResourceView* v[3] = { mask ? g_grassMask.srv : 0, map ? g_gm[0].srv : 0, map ? g_grassSoftSrv : 0 };
    ctx->PSSetShaderResources(85, 3, v);
}

// before the game's sun draw (the G-buffer done, its lighting not begun): view 38 paints the lawns green in the albedo
static ID3D11RenderTargetView* g_grassAlbRtv;
static ID3D11Resource* g_grassAlbRes;
static void GrassBeforeSun(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    g_grassSunFrame = g_frame + 1;
    static DWORD lastLog;
    if (GetTickCount() - lastLog > 20000) {
        lastLog = GetTickCount();
        Log("grass: %u terrain draws, %u with grass (since the last line), %d grass textures", g_grassTerrDraws, g_grassDraws, g_grassTexN);
        Log("grass: the rain's lawns bound %u times: %u with no mask, %u with no map", g_grassSoftN[0], g_grassSoftN[1], g_grassSoftN[2]);
        g_grassTerrDraws = g_grassDraws = 0;
        g_grassSoftN[0] = g_grassSoftN[1] = g_grassSoftN[2] = 0;
        Log("grass: %u clumps last frame, view %s, G-buffer %s", g_grassInst, g_grassVPFrame == g_frame + 1 ? "read" : "not read",
            g_grassGbFrame == g_frame + 1 ? "known" : "not known");
    }
    if (g_grassMaskFrame == g_frame + 1) GrassDraw(ctx);   // 1.97g (1.98: the lawns' map always, the blades with "Grass")
    GrassSoftBind(ctx);   // 1.97k: no puddles on the lawns (t85: the sun pass, the reflections)
    if (g_view != 38 || g_grassMaskFrame != g_frame + 1 || !g_air.depth || !g_psGrassView || !g_grassViewBlend || !g_air.diffuse || !g_grassMask.srv) return;
    ID3D11Resource* r = 0;
    g_air.diffuse->GetResource(&r);
    if (!r) return;
    if (r != g_grassAlbRes) {
        if (g_grassAlbRtv) g_grassAlbRtv->Release();
        g_grassAlbRtv = 0; g_grassAlbRes = r;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        g_air.diffuse->GetDesc(&sd);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = sd.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(r, &rd, &g_grassAlbRtv))) { g_grassAlbRtv = 0; Log("grass: no target view on the albedo"); }
    }
    r->Release();
    if (!g_grassAlbRtv) return;
    ID3D11RenderTargetView* rtv[8] = {};
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(8, rtv, &dsv);
    ID3D11ShaderResourceView* saved[16] = {};
    ctx->PSGetShaderResources(0, 16, saved);
    ID3D11PixelShader* sPs = 0; ctx->PSGetShader(&sPs, 0, 0);
    ID3D11DepthStencilState* sDs = 0; UINT sRef = 0; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11BlendState* sBl = 0; FLOAT bf[4]; UINT bm; ctx->OMGetBlendState(&sBl, bf, &bm);
    ctx->OMSetRenderTargets(1, &g_grassAlbRtv, 0);
    ID3D11ShaderResourceView* vin[2] = { g_grassMask.srv, g_air.depth };
    ctx->PSSetShaderResources(0, 2, vin);
    ctx->OMSetDepthStencilState(g_grassDsOff, 0);
    ctx->OMSetBlendState(g_grassViewBlend, 0, 0xffffffff);
    oPSSetShader(ctx, g_psGrassView, 0, 0);
    Replay(ctx, d);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
    oPSSetShader(ctx, sPs, 0, 0);
    ctx->OMSetDepthStencilState(sDs, sRef);
    ctx->OMSetBlendState(sBl, bf, bm);
    ctx->OMSetRenderTargets(8, rtv, dsv);
    ctx->PSSetShaderResources(0, 16, saved);
    IUnknown* rel[] = { sPs, sDs, sBl, dsv };
    for (int k = 0; k < 4; ++k) if (rel[k]) rel[k]->Release();
    for (int k = 0; k < 8; ++k) if (rtv[k]) rtv[k]->Release();
    for (int k = 0; k < 16; ++k) if (saved[k]) saved[k]->Release();
}
