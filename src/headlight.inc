// ---------------------------------------------------------------- the player's headlight beams in the air
// The game draws its headlights as one projected spot light (deferred_lightProjectionMatrix) in the main view (and
// once more in the road reflection view, 1024^2): constants in cb1 (112 bytes: attenuation, colour, position -
// camera-relative - and the projection matrix rows), the beam pattern texture at t2, its shadow map at t15 with a
// comparison sampler at s15. Right after that draw in the main view its constants are copied (GPU side) and the
// two textures and the sampler kept; the lamp pass marches the view ray through the beam (PS_Lamps' glow target).

static ID3D11Buffer* g_hlCb;
static UINT g_hlCbSize;
static ID3D11ShaderResourceView* g_hlTex;
static ID3D11SamplerState* g_hlCmp;
static unsigned g_hlFrame;          // frame + 1 of the last capture

static void HlAfterDraw(ID3D11DeviceContext* ctx)
{
    if (LightKindOf(g_lastPS) != 2 || !MainTarget(ctx)) return;
    ID3D11Buffer* cb = 0;
    ctx->PSGetConstantBuffers(1, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    if (!g_hlCb || g_hlCbSize != bd.ByteWidth) {
        if (g_hlCb) g_hlCb->Release();
        g_hlCb = 0;
        D3D11_BUFFER_DESC d = {};
        d.ByteWidth = bd.ByteWidth; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&d, 0, &g_hlCb))) { g_hlCb = 0; cb->Release(); return; }
        g_hlCbSize = bd.ByteWidth;
        Log("headlights: the game's spot light found (constants %u bytes)", bd.ByteWidth);
    }
    ctx->CopyResource(g_hlCb, cb);
    cb->Release();
    if (g_hlTex) g_hlTex->Release();
    if (g_hlShadow) g_hlShadow->Release();
    if (g_hlCmp) g_hlCmp->Release();
    g_hlTex = g_hlShadow = 0; g_hlCmp = 0;
    ctx->PSGetShaderResources(2, 1, &g_hlTex);
    ctx->PSGetShaderResources(15, 1, &g_hlShadow);
    ctx->PSGetSamplers(15, 1, &g_hlCmp);
    g_hlFrame = g_frame + 1;
}

// The game's own headlight light on the surfaces, scaled (GameHeadlights): its additive draw gets a blend state that
// takes the source times a blend factor (ONE -> BLEND_FACTOR), the factor = the setting; restored right after.
struct HlBlend { ID3D11BlendState *orig, *scaled; };
static HlBlend g_hlBlend[8];
static int g_hlBlendN;
static ID3D11BlendState* g_hlSavedBl;
static FLOAT g_hlSavedBf[4];
static UINT g_hlSavedBm;
static bool g_hlScaled;

static void HlScaleBegin(ID3D11DeviceContext* ctx)
{
    g_hlScaled = false;
    if (!g_enabled || !Immediate(ctx) || !g_lightPSN || LightKindOf(g_lastPS) != 2) return;
    float s = g_p[P_HL_GAME].v;
    if (s > 0.999f && s < 1.001f) return;
    ctx->OMGetBlendState(&g_hlSavedBl, g_hlSavedBf, &g_hlSavedBm);
    ID3D11BlendState* use = 0;
    for (int k = 0; k < g_hlBlendN; ++k) if (g_hlBlend[k].orig == g_hlSavedBl) use = g_hlBlend[k].scaled;
    if (!use && g_hlBlendN < 8) {
        D3D11_BLEND_DESC d = {};
        if (g_hlSavedBl) g_hlSavedBl->GetDesc(&d);
        else { d.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO; d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
               d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
               d.RenderTarget[0].RenderTargetWriteMask = 15; }
        static bool said;
        if (!said) { said = true; Log("headlights: the game's light blends %d: src %d dest %d op %d", d.RenderTarget[0].BlendEnable,
                                      d.RenderTarget[0].SrcBlend, d.RenderTarget[0].DestBlend, d.RenderTarget[0].BlendOp); }
        int n = d.IndependentBlendEnable ? 8 : 1;
        bool ok = true;
        for (int r = 0; r < n; ++r) {
            D3D11_RENDER_TARGET_BLEND_DESC& b = d.RenderTarget[r];
            if (!b.BlendEnable) { b.BlendEnable = TRUE; b.SrcBlend = D3D11_BLEND_ONE; b.DestBlend = D3D11_BLEND_ZERO; b.BlendOp = D3D11_BLEND_OP_ADD;
                                  b.SrcBlendAlpha = D3D11_BLEND_ONE; b.DestBlendAlpha = D3D11_BLEND_ZERO; b.BlendOpAlpha = D3D11_BLEND_OP_ADD; }
            if (b.SrcBlend == D3D11_BLEND_ONE) b.SrcBlend = D3D11_BLEND_BLEND_FACTOR; else ok = false;
        }
        ID3D11BlendState* sc = 0;
        if (ok) g_dev->CreateBlendState(&d, &sc);
        g_hlBlend[g_hlBlendN].orig = g_hlSavedBl; g_hlBlend[g_hlBlendN].scaled = sc; ++g_hlBlendN;
        use = sc;
    }
    if (!use) { if (g_hlSavedBl) g_hlSavedBl->Release(); g_hlSavedBl = 0; return; }
    FLOAT f[4] = { s, s, s, s };
    ctx->OMSetBlendState(use, f, g_hlSavedBm);
    g_hlScaled = true;
}

static void HlScaleEnd(ID3D11DeviceContext* ctx)
{
    if (!g_hlScaled) return;
    ctx->OMSetBlendState(g_hlSavedBl, g_hlSavedBf, g_hlSavedBm);
    if (g_hlSavedBl) g_hlSavedBl->Release();
    g_hlSavedBl = 0;
    g_hlScaled = false;
}

// for the lamp pass: is a recent capture there (this frame's or the last one: the spot may come after our pass)
static bool HlReady() { return g_hlCb && g_hlTex && g_hlFrame + 2 >= g_frame + 1 && g_p[P_HL_BEAM].v > 0; }

static void HlBind(ID3D11DeviceContext* ctx, bool on)
{
    ID3D11Buffer* b = on ? g_hlCb : 0;
    ctx->PSSetConstantBuffers(8, 1, &b);
    ID3D11ShaderResourceView* t[2] = { on ? g_hlTex : 0, on ? g_hlShadow : 0 };
    ctx->PSSetShaderResources(43, 2, t);
    ID3D11SamplerState* s = on ? g_hlCmp : 0;
    ctx->PSSetSamplers(14, 1, &s);
}

// ---- the game's headlight light with a soft shadow: a copy of its shader (both variants: the position from the
// inverse view-projection, or from the view ray), the one shadow tap replaced by 16 on a Poisson disc (radius in
// texels: HeadlightShadowSoftness). Bound instead of the game's shader when the game binds one of those.
static const char* kHlSrc = R"HLSL(
cbuffer G0 : register(b0) { float4 IVP[4]; }
cbuffer G1 : register(b1) { float4 Att; float4 Col; float4 Pos; float4 M[4]; }
cbuffer Soft : register(b9) { float4 Hs; }   // x = shadow filter radius (texels), y = 1 / shadow map size, z = 1 = soft
Texture2D tDepth : register(t0);
Texture2D tNormal : register(t1);
Texture2D tProj : register(t2);
Texture2D tDiff : register(t3);
Texture2D tAdd : register(t4);
Texture2D<float> tShadow : register(t15);
SamplerState sDiff : register(s0);
SamplerState sNorm : register(s1);
SamplerState sDepth : register(s2);
SamplerState sProj : register(s3);
SamplerState sAdd : register(s4);
SamplerComparisonState sCmp : register(s15);

static const float2 kDisc[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870), float2(0.34495938, 0.29387760),
    float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464), float2(-0.38277543, 0.27676845), float2(0.97484398, 0.75648379),
    float2(0.44323325, -0.97511554), float2(0.53742981, -0.47373420), float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507), float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367), float2(0.14383161, -0.14100790) };

float4 Light(float2 uv, float3 P, float2 spos)
{
    float4 c = P.y * M[1] + P.x * M[0] + P.z * M[2] + M[3];
    float3 q = c.xyz / c.w;
    float inside = all(abs(c.xyz) < c.w) ? 1 : 0;
    float2 suv = q.xy * 0.5 + 0.5;
    float sh;
    if (Hs.z > 0.5) {
        float a = frac(52.9829189 * frac(dot(spos, float2(0.06711056, 0.00583715)))) * 6.2831853;
        float sn, cs;
        sincos(a, sn, cs);
        float2x2 rot = float2x2(cs, -sn, sn, cs);
        float r = Hs.x * Hs.y;
        sh = 0;
        [unroll] for (int k = 0; k < 16; ++k) sh += tShadow.SampleCmpLevelZero(sCmp, float2(suv.x, 1 - suv.y) + mul(rot, kDisc[k]) * r, q.z);
        sh /= 16;
    } else sh = tShadow.SampleCmpLevelZero(sCmp, float2(suv.x, 1 - suv.y), q.z);
    float3 pat = tProj.Sample(sProj, suv).rgb;
    float3 L = Pos.xyz - P;
    float d2 = dot(L, L);
    float dl = sqrt(d2 + 0.001);
    float att = saturate(1 / (Att.x * d2 + Att.y * dl + Att.z));
    float3 light = inside * pat * Col.rgb * (sh * att);
    L /= dl;
    float3 n = normalize(tNormal.Sample(sNorm, uv).xyz * 2 - 1);
    float ndlRaw = dot(n, L);
    float ndl = saturate(ndlRaw);
    float4 D = tDiff.Sample(sDiff, uv);
    float flag = tAdd.Sample(sAdd, uv).w > 0.992157 ? 1 : 0;
    float3 alb = D.rgb * D.rgb;
    float3 lam = ndl * alb * light * 0.318310;
    float vn = saturate(dot(n, normalize(-P)));
    float a1 = 1 - 0.5 * vn, a2 = 1 - 0.5 * ndlRaw;
    a1 = 1 - a1 * a1 * a1 * a1 * a1; a2 = 1 - a2 * a2 * a2 * a2 * a2;
    float3 ash = a1 * a2 * alb * 0.387508 * ndl;
    return float4(flag * (light * ash - lam) + lam, 0);
}

#ifdef VAR_A
float4 main(float4 pos : SV_Position, float4 v1 : TEXCOORD0) : SV_Target
{
    float2 uv = v1.xy / v1.z;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float depth = tDepth.Sample(sDepth, uv).x;
    float4 r = ndc.y * IVP[1] + ndc.x * IVP[0] + depth * IVP[2] + IVP[3];
    return Light(uv, r.xyz / r.w, pos.xy);
}
#else
float4 main(float4 pos : SV_Position, float4 v1 : TEXCOORD0, float4 v2 : TEXCOORD1) : SV_Target
{
    float depth = tDepth.Sample(sDepth, v1.xy).x;
    return Light(v1.xy, v2.xyz / (depth - v2.w), pos.xy);
}
#endif
)HLSL";

static ID3D11PixelShader* g_hlPS[2];   // [0] variant A (inverse view-projection), [1] variant B (view ray)
static ID3D11Buffer* g_hlSoftCb;

static void HlCompile()
{
    if (!g_compile || !g_dev) return;
    for (int v = 0; v < 2; ++v) {
        if (g_hlPS[v]) { g_hlPS[v]->Release(); g_hlPS[v] = 0; }
        D3D_SHADER_MACRO mac[2] = { { "VAR_A", "1" }, { 0, 0 } };
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(kHlSrc, strlen(kHlSrc), "headlight", v == 0 ? mac : 0, 0, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_hlPS[v]);
        else Log("headlight shader %d: %s", v, err ? (const char*)err->GetBufferPointer() : "failed");
        if (code) code->Release();
        if (err) err->Release();
    }
    if (!g_hlSoftCb) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, 0, &g_hlSoftCb);
    }
}

// PSSetShader: the game binds its headlight shader with a shadow map: ours instead (returns what to bind)
static ID3D11PixelShader* HlSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (!g_enabled || !ps || !Immediate(ctx) || g_p[P_HL_SOFT].v <= 0) return ps;
    LONG n = g_lightPSN;
    int var = -1;
    for (LONG i = 0; i < n; ++i) if (g_lightPS[i].ps == ps && g_lightPS[i].kind == 2 && g_lightPS[i].shadow) var = g_lightPS[i].var;
    if (var < 0) return ps;
    if (!g_hlPS[0] && !g_hlPS[1]) { static bool tried; if (!tried) { tried = true; HlCompile(); } }
    if (!g_hlPS[var] || !g_hlSoftCb) return ps;
    // the shadow map's size (for the radius in texels)
    float inv = 1.0f / 1024;
    ID3D11ShaderResourceView* sv = 0;
    ctx->PSGetShaderResources(15, 1, &sv);
    if (sv) {
        ID3D11Resource* r = 0; sv->GetResource(&r);
        ID3D11Texture2D* t = 0; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
        if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); inv = 1.0f / d.Width; t->Release(); }
        sv->Release();
    }
    float hs[4] = { g_p[P_HL_SOFT].v, inv, 1, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_hlSoftCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, hs, 16); ctx->Unmap(g_hlSoftCb, 0); }
    ctx->PSSetConstantBuffers(9, 1, &g_hlSoftCb);
    return g_hlPS[var];
}
