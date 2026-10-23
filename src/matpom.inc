// ---------------------------------------------------------------- parallax on the game's materials (1.14)
// A material shader of the game is replaced by ours: the same inputs, textures and outputs (rewritten from its
// disassembly, instruction for instruction), with parallax occlusion mapping before the texture reads: the view ray
// goes into the surface by the material's own depth map until it meets it, and every texture is read there.
// The road / pavement shader (fingerprint 55f2f243af1bf610: ColourMap, DetailMap diffuse + normal, SpecColour, Normal,
// RoadDepth, puddle map, planar reflection) has a depth map of its own: RoadDepth (white = the surface, dark = cracks,
// joints, holes; the game uses it for where puddles stand). The directions of the texture on the surface (metres per
// texture unit) come from the screen derivatives of the world position (v6 = the eye minus the point) and of the
// texture coordinates (a cotangent frame), so the depth is in centimetres (param ParallaxDepthCm).
// Our shader is used only if its input signature matches the game's register for register (checked by reflection).

static const char* kRoadPomSrc = R"HLSL(
cbuffer gGlobalCB : register(b0) { float4 C0[2]; };
cbuffer PomCB : register(b10) { float4 PM; };   // depth (m), on, fade distance (m), steps (max)
Texture2D tCol : register(t0);
Texture2D tDet : register(t1);
Texture2D tSpc : register(t2);
Texture2D tNrm : register(t3);
Texture2D tDtN : register(t4);
Texture2D tRDp : register(t5);
Texture2D tPud : register(t6);
Texture2D tPln : register(t7);
Texture3D tLut : register(t8);
Texture2D tTon : register(t9);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);
SamplerState s4 : register(s4);
SamplerState s5 : register(s5);
SamplerState s9 : register(s9);
SamplerState s11 : register(s11);
SamplerState s12 : register(s12);
SamplerState s14 : register(s14);
struct In {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 uvD : TEXCOORD6;
    float3 v2 : TEXCOORD2;
    float4 v3 : TEXCOORD3;
    float4 v4 : TEXCOORD4;
    float3 v5 : TEXCOORD7;
    float3 v6 : TEXCOORD9;
    float4 v7 : TEXCOORD5;
};
struct Out { float4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; float4 o3 : SV_Target3; };

Out main(In i)
{
    float2 uv = i.uv, uvD = i.uvD;
    float2 dUx = ddx(uv), dUy = ddy(uv), dDx = ddx(uvD), dDy = ddy(uvD);
    float3 dPx = -ddx(i.v6), dPy = -ddy(i.v6);
    // parallax: the ray into the surface (metres of depth -> texture units by the cotangent frame), first meeting
    if (PM.y > 0.5) {
        float3 Ng = normalize(i.v2), V = normalize(i.v6);
        float3 p2 = cross(dPy, Ng), p1 = cross(Ng, dPx);
        float det = dot(dPx, p2);
        float dist = length(i.v6);
        float amp = PM.x * saturate(1.5 - dist / PM.z);
        if (abs(det) > 1e-12 && amp > 1e-5) {
            float3 gu = (p2 * dUx.x + p1 * dUy.x) / det, gv = (p2 * dUx.y + p1 * dUy.y) / det;
            float3 gu2 = (p2 * dDx.x + p1 * dDy.x) / det, gv2 = (p2 * dDx.y + p1 * dDy.y) / det;
            float vn = max(dot(V, Ng), 0.12);
            float3 dirW = -V / vn;                          // world shift per metre of depth
            float2 du = float2(dot(gu, dirW), dot(gv, dirW)), duD = float2(dot(gu2, dirW), dot(gv2, dirW));
            int steps = (int)lerp(PM.w, 8, vn);
            float2 dim;
            tRDp.GetDimensions(dim.x, dim.y);
            float stepD = amp / steps, d = 0, prevD = 0, prevGap = -1;   // gap: the surface's depth minus the ray's
            float hitD = 0;
            [loop] for (int k = 0; k <= steps; ++k) {
                float2 us = uv + du * d;
                float h = tRDp.SampleGrad(s9, us, dUx, dUy).y;
                // (1.15) the depth maps have a black frame 1-2 texels wide (no puddles at the joins of the tiles): a groove
                // at every repeat of the texture - the last 3 texels next to its edge count as the surface
                float2 fe = frac(us), ed = min(fe, 1 - fe) * dim;
                h = lerp(1, h, saturate((min(ed.x, ed.y) - 1.5) / 1.5));
                float gap = (1 - h) * amp - d;
                if (gap <= 0) { hitD = k ? lerp(prevD, d, prevGap / max(prevGap - gap, 1e-6)) : 0; break; }
                prevD = d; prevGap = gap; hitD = d;
                d += stepD;
            }
            uv += du * hitD;
            uvD += duD * hitD;
        }
    }
    // the game's shader from here (its registers in the comments)
    float3 B = cross(i.v2, i.v3.xyz) * i.v3.w;
    float4 nm = tNrm.SampleGrad(s4, uv, dUx, dUy);
    float3 n3 = nm.xyz - 0.5;
    float up = 2 * n3.z;
    float2 sl = float2(2 * n3.x * i.v3.x + 2 * n3.y * B.x, 2 * n3.x * i.v3.z + 2 * n3.y * B.z) / up;
    float4 dn = tDtN.SampleGrad(s1, uvD, dDx, dDy);
    float3 d3 = dn.xyz - 0.5;
    float gloss = nm.w * dn.w;
    sl += float2(2 * d3.x, -2 * d3.y) / (2 * d3.z);
    float3 nn = normalize(float3(sl.x + i.v2.x / i.v2.y, 1, sl.y + i.v2.z / i.v2.y));
    float3 diffN = nn - i.v2;
    float4 rd = tRDp.SampleGrad(s9, uv, dUx, dUy);
    float4 pd = tPud.Sample(s11, i.v5.xy);
    pd.x = rd.y * pd.x;
    float wet = pd.w * i.v5.z;
    float3 f = lerp(1, pd.xyz, wet);
    float3 N = f.x * diffN + i.v2;
    Out o;
    o.o0.xyz = N * 0.5 + 0.5;
    float g2 = gloss * f.y;
    o.o0.w = g2;
    float3 col = tCol.SampleGrad(s2, uv, dUx, dUy).rgb * tDet.SampleGrad(s0, uvD, dDx, dDy).rgb * 2;
    o.o1 = float4(sqrt(f.z * col), i.v7.w);
    float sp = f.x * (tSpc.SampleGrad(s3, uv, dUx, dUy).y - 0.02) + 0.02;
    o.o2 = float4(sp.xxx, 1);
    float ndv = dot(N, normalize(i.v6));
    float4 lut = tLut.Sample(s14, float3(ndv, g2, sp) * 0.9375 + 0.03125);
    float kk = ((1 - sp) * lut.y + sp) * lut.x * i.v5.z * pd.w;
    float2 ruv = float2(N.x, N.z) * C0[1].y + i.v4.xy / i.v4.w;
    float3 refl = tPln.SampleLevel(s12, ruv * 0.5 + 0.5, g2 * C0[1].z).rgb * kk;
    float e = tTon.Sample(s5, float2(0.5, 0.5)).w * C0[0].x;
    o.o3 = float4(sqrt((col * i.v7.xyz + refl) * e), 0);
    return o;
}
)HLSL";

static const unsigned long long kRoadPsHash = 0x55f2f243af1bf610ull;
static ID3D11PixelShader* volatile g_roadPs;      // the game's
static void* g_roadCode;                           // its code (for the signature check)
static SIZE_T g_roadSize;
static ID3D11PixelShader* g_roadPom;               // ours
static ID3D11Buffer* g_pomCb;
static float g_pomCbData[4];
static int g_pomState;                             // 0 not tried, 1 ready, 2 failed

static unsigned long long CodeHash(const void* code, SIZE_T size)
{
    unsigned long long h = 1469598103934665603ull;
    for (SIZE_T q = 0; q < size; ++q) { h ^= ((const unsigned char*)code)[q]; h *= 1099511628211ull; }
    return h;
}

// at pixel shader creation: is it one we replace?
static void MatNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (CodeHash(code, size) != kRoadPsHash) return;
    g_roadPs = ps;
    free(g_roadCode);
    g_roadCode = malloc(size);
    if (g_roadCode) { memcpy(g_roadCode, code, size); g_roadSize = size; }
    Log("parallax: the road / pavement shader found (%p)", ps);
}

// ours compiled, its inputs checked against the game's (same semantics, registers, masks)
static bool MatEnsure()
{
    if (g_pomState) return g_pomState == 1;
    g_pomState = 2;
    if (!g_compile || !g_reflect || !g_roadCode) return false;
    ID3DBlob *code = 0, *err = 0;
    if (FAILED(g_compile(kRoadPomSrc, strlen(kRoadPomSrc), "roadpom", 0, 0, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        Log("parallax: compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    bool same = false;
    ID3D11ShaderReflection *ro = 0, *rg = 0;
    if (SUCCEEDED(g_reflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&ro)) &&
        SUCCEEDED(g_reflect(g_roadCode, g_roadSize, __uuidof(ID3D11ShaderReflection), (void**)&rg))) {
        D3D11_SHADER_DESC a, b;
        ro->GetDesc(&a); rg->GetDesc(&b);
        same = a.InputParameters == b.InputParameters && a.OutputParameters == b.OutputParameters;
        for (UINT k = 0; k < b.InputParameters && same; ++k) {
            D3D11_SIGNATURE_PARAMETER_DESC p, q;
            ro->GetInputParameterDesc(k, &p); rg->GetInputParameterDesc(k, &q);
            same = p.Register == q.Register && p.Mask == q.Mask && p.SemanticIndex == q.SemanticIndex && !_stricmp(p.SemanticName, q.SemanticName);
            if (!same) Log("parallax: input %u differs: ours %s%u r%u m%x, the game's %s%u r%u m%x", k, p.SemanticName, p.SemanticIndex, p.Register, p.Mask,
                           q.SemanticName, q.SemanticIndex, q.Register, q.Mask);
        }
    }
    if (ro) ro->Release();
    if (rg) rg->Release();
    if (same) g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_roadPom);
    code->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, 0, &g_pomCb);
    if (!g_roadPom || !g_pomCb) { Log("parallax: not used (signature %s)", same ? "same" : "differs"); return false; }
    Log("parallax: our road / pavement shader ready");
    g_pomState = 1;
    return true;
}

// at PSSetShader: the game's road shader -> ours (with our constants at b10)
static ID3D11PixelShader* MatSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (!ps || ps != g_roadPs || !g_enabled || g_p[P_POM].v < 0.5f || !MatEnsure()) return ps;
    float c[4] = { g_p[P_POM + 1].v * 0.01f, 1, g_p[P_POM + 2].v, 24 };
    if (memcmp(c, g_pomCbData, 16)) { memcpy(g_pomCbData, c, 16); ctx->UpdateSubresource(g_pomCb, 0, 0, c, 0, 0); }
    ctx->PSSetConstantBuffers(10, 1, &g_pomCb);
    return g_roadPom;
}
