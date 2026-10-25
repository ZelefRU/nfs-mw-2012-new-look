// ---------------------------------------------------------------- 1.87: the wind in the leaves
// The game's trees have no wind of their own. Their leaves (1.86's Shift+X census): a cut-out PS whose names hold
// "diffuseTintCol", its VS with 3 outputs (SV_Position, TEXCOORD0.xyz, TEXCOORD1.xyz), the tree's world matrix in cb1,
// the game's view-projection in cb0 (packed: rows x, y, depth; then z scale / bias, w scale / bias). Round each of their
// main-view draws a geometry shader of ours goes after that VS (built once per VS from its outputs: GsSourceFor; the
// pass-through checked by 1.86): each vertex's place in the world from its clip position (the VP inverted), moved by the
// wind, and only the move's difference added back in clip space (with no wind the vertex is exactly the game's):
// - sway: along the wind (weather.inc), growing with the height over the tree's base ((h / 8 m)^2), in gusts (slow
//   waves, each tree its own phase by its place), bending down a little as it leans;
// - flutter: small quick shakes, each leaf its own phase by its place, stronger in a stronger wind.
// WindLeaves, WindFlutter: how much (0: off).
static ID3D11VertexShader* g_windVs[16];
static ID3D11GeometryShader* g_windGs[16];
static bool g_windGsFailed[16];
static unsigned g_windVsN, g_windDraws, g_windFrames, g_windLog, g_windGsDraws;
static ID3D11Buffer* g_windCb;
static bool g_windSet;
static bool WindIsLeaves(ID3D11PixelShader* ps, VsInfo* e)
{
    if (!ps || !e || e->nOut != 3 || e->car) return false;
    // 1.96: the answer per pixel shader kept (asked again when a shader's name is added: it may be this one's)
    static struct { ID3D11PixelShader* ps; LONG names; bool leaves; } c[256];
    unsigned h = ((unsigned)(size_t)ps >> 4) * 2654435761u >> 24;
    LONG names = g_psNameCount;
    if (c[h].ps != ps || c[h].names != names) {
        const char* n = PsIsCut(ps) ? PsNameOf(ps) : 0;
        c[h].ps = ps; c[h].names = names; c[h].leaves = n && strstr(n, "diffuseTintCol");
    }
    return c[h].leaves;
}
static const char* kWindGsBody =
    "cbuffer GV : register(b0) { float4 gv[VPI + 4]; };\n"
    "cbuffer GO : register(b1) { float4 go[WI + 4]; };\n"
    "cbuffer WindCB : register(b2) { float4 W0, W1; };   // wind x, z (m/s), seconds, sway x; flutter x\n"
    "float3 ClipToWorld(float4 c, float4 r0, float4 r1, float4 r2, float4 zw)\n"
    "{\n"
    "    float d = abs(zw.z) > 1e-6 ? (c.w - zw.w) / zw.z : (c.z - zw.y) / zw.x;\n"
    "    float3 b = float3(c.x - r0.w, c.y - r1.w, d - r2.w);\n"
    "    float3 c0 = cross(r1.xyz, r2.xyz), c1 = cross(r2.xyz, r0.xyz), c2 = cross(r0.xyz, r1.xyz);\n"
    "    return (c0 * b.x + c1 * b.y + c2 * b.z) / dot(r0.xyz, c0);\n"
    "}\n"
    "float4 WorldToClip(float3 p, float4 r0, float4 r1, float4 r2, float4 zw)\n"
    "{\n"
    "    float d = dot(r2.xyz, p) + r2.w;\n"
    "    return float4(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, d * zw.x + zw.y, d * zw.z + zw.w);\n"
    "}\n"
    "float4 Wind(float4 c)\n"
    "{\n"
    "    float4 r0 = gv[VPI], r1 = gv[VPI + 1], r2 = gv[VPI + 2], zw = gv[VPI + 3];\n"
    "    float3 w = ClipToWorld(c, r0, r1, r2, zw);\n"
    "    float3 base = go[WI + 3].xyz;\n"
    "    float h = max(w.y - base.y, 0), t = W0.z;\n"
    "    float2 wd = W0.xy;\n"
    "    float ws = length(wd);\n"
    "    float k = saturate(h / 8); k *= k;\n"
    "    float ph = dot(base.xz, float2(0.37, 0.23));\n"
    "    float gust = 0.55 + 0.3 * sin(t * 0.9 + ph) + 0.15 * sin(t * 2.3 + ph * 1.7);\n"
    "    float2 sway = wd * (0.012 * k * gust * W0.w);\n"
    "    sway += (ws > 0.01 ? wd / ws : float2(0, 0)) * sin(t * 1.6 + ph * 2.1) * (0.0025 * ws * k * W0.w);\n"
    "    float3 m = float3(sway.x, -0.25 * dot(sway, sway), sway.y);\n"
    "    float fa = (0.004 + 0.0018 * ws) * W1.x * saturate(h / 1.5);\n"
    "    m += sin(t * float3(6.3, 7.7, 5.9) + dot(w, float3(2.1, 1.7, 2.6))) * fa;\n"
    "    if (!(dot(m, m) < 4)) return c;   // (a bad inverse: leave it)\n"
    "    float4 c0 = WorldToClip(w, r0, r1, r2, zw), c1 = WorldToClip(w + m, r0, r1, r2, zw);\n"
    "    return c + (c1 - c0);\n"
    "}\n"
    "[maxvertexcount(3)]\n"
    "void gs(triangle V i[3], inout TriangleStream<V> s)\n"
    "{\n"
    "    [unroll] for (int k = 0; k < 3; ++k) { V v = i[k]; v.pos = Wind(v.pos); s.Append(v); }\n"
    "    s.RestartStrip();\n"
    "}\n";
static ID3D11GeometryShader* WindGsFor(VsInfo* e, int slot)
{
    if (g_windGs[slot] || g_windGsFailed[slot]) return g_windGs[slot];
    g_windGsFailed[slot] = true;
    if (!g_compile || e->vpSlot != 0 || e->wSlot != 1 || (e->vpOff & 15) || (e->wOff & 15)) {
        Log("wind: the leaves' VS %p: its constants not where the GS reads them (view %d at %d, world %d at %d)", e->vs, e->vpSlot, e->vpOff, e->wSlot, e->wOff);
        return 0;
    }
    static char src[8192];
    int n = GsSourceFor(e, src, 4096);
    char* cut = n ? strstr(src, "[maxvertexcount") : 0;
    if (!cut) return 0;
    n = (int)(cut - src);
    n += sprintf(src + n, "#define VPI %d\n#define WI %d\n", e->vpOff / 16, e->wOff / 16);
    strcpy(src + n, kWindGsBody);
    ID3DBlob *code = 0, *err = 0;
    if (FAILED(g_compile(src, strlen(src), "windgs", 0, 0, "gs", "gs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)) || !code) {
        Log("wind: the leaves' GS did not compile: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return 0;
    }
    if (SUCCEEDED(g_dev->CreateGeometryShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_windGs[slot]))) {
        g_windGsFailed[slot] = false;
        Log("wind: the leaves' GS made for VS %p", e->vs);
    }
    code->Release();
    if (err) err->Release();
    return g_windGs[slot];
}
// before a game draw (BeforeDraw): a leaves' draw in the main view gets our GS and its constants
static void WindBeforeDraw(ID3D11DeviceContext* ctx)
{
    if (g_p[231].v <= 0 && g_p[232].v <= 0) return;   // (1.96: the wind off: nothing asked)
    VsInfo* e = DrawVs(ctx);
    if (!WindIsLeaves(g_lastPS, e) || !MainTarget(ctx)) return;
    ++g_windDraws;
    unsigned k = 0;
    while (k < g_windVsN && g_windVs[k] != e->vs) ++k;
    if (k == g_windVsN) {
        if (k >= 16) return;
        g_windVs[g_windVsN++] = e->vs;
        Log("wind: a leaves' VS found: %p (view: slot %d at %d, world: slot %d at %d)", e->vs, e->vpSlot, e->vpOff, e->wSlot, e->wOff);
    }
    ID3D11GeometryShader* gs = WindGsFor(e, k);
    if (!gs || !g_windCb) return;
    ID3D11GeometryShader* cur = 0;
    ctx->GSGetShader(&cur, 0, 0);
    if (cur) { cur->Release(); return; }   // (the game's own GS there: leave it)
    ID3D11Buffer* cb[2] = {};
    ctx->VSGetConstantBuffers(0, 2, cb);
    ID3D11Buffer* all[3] = { cb[0], cb[1], g_windCb };
    ctx->GSSetConstantBuffers(0, 3, all);
    for (int j = 0; j < 2; ++j) if (cb[j]) cb[j]->Release();
    ctx->GSSetShader(gs, 0, 0);
    g_windSet = true;
    ++g_windGsDraws;
}
// after it (AfterDraw, first): ours off again
static void WindAfterDraw(ID3D11DeviceContext* ctx)
{
    if (!g_windSet) return;
    g_windSet = false;
    ctx->GSSetShader(0, 0, 0);
    ID3D11Buffer* none[3] = {};
    ctx->GSSetConstantBuffers(0, 3, none);
}
// once a frame (Present): the wind's constants; the log
static void WindFrame(ID3D11DeviceContext* ctx)
{
    ++g_windFrames;
    if (ctx) {
        if (!g_windCb) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_windCb))) g_windCb = 0;
        }
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        float t = g_qpf.QuadPart ? (float)fmod((double)q.QuadPart / g_qpf.QuadPart, 3600.0) : 0.0f;
        float c[8] = { g_wind[0], g_wind[2], t, g_p[231].v, g_p[232].v, 0, 0, 0 };
        D3D11_MAPPED_SUBRESOURCE m;
        if (g_windCb && SUCCEEDED(ctx->Map(g_windCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, c, 32); ctx->Unmap(g_windCb, 0); }
    }
    if (g_frame - g_windLog >= 1800) {
        g_windLog = g_frame;
        Log("wind: the leaves: %u VS, %.1f draws a frame, %.1f with our GS", g_windVsN, g_windFrames ? (float)g_windDraws / g_windFrames : 0.0f,
            g_windFrames ? (float)g_windGsDraws / g_windFrames : 0.0f);
        g_windDraws = g_windFrames = g_windGsDraws = 0;
    }
}
