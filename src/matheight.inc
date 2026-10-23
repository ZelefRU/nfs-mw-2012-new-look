// ---------------------------------------------------------------- parallax on buildings (1.16)
// The game's common building / prop shader (fingerprint ec3734a655430424: Diffuse, Specular (x spec, y gloss), AO and
// LightMap on the second uv, Normal (BC1, tangent space with T = v4, B = v5, N = v2), a reflection cube, a Fresnel LUT,
// Tonemap) has no depth map. One is made from its normal map: the surface whose slopes the normal map holds,
// recovered by Frankot-Chellappa integration (FFT: H = -i (wu P + wv Q) / (wu^2 + wv^2)), at most 512^2, on a worker
// thread, the first time a normal map is drawn with this shader; kept on disk (NewLook_textures\height_auto\<normal
// map hash>.dds) for the next time. A file replace\<normal map hash>_height.dds is used instead when there is one.
// Two channels: R with the normal map's green as +slope along v, G as -slope (normal maps come in both conventions;
// the shader picks per pixel by comparing the game's bitangent with the direction v grows on the surface).

static const char* kBldPomSrc = R"HLSL(
cbuffer gGlobalCB : register(b0) { float4 C0[4]; };
cbuffer gMaterialCB : register(b2) { float4 M2[3]; };
cbuffer PomCB : register(b10) { float4 PM; };   // depth (m), on (1 = a height map at t10), fade distance (m), steps
Texture2D tDif : register(t0);
Texture2D tSpc : register(t1);
Texture2D tAO : register(t2);
Texture2D tLM : register(t3);
Texture2D tNrm : register(t4);
TextureCube tRef : register(t5);
Texture3D tLut : register(t6);
Texture2D tTon : register(t7);
Texture2D tHgt : register(t10);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);
SamplerState s4 : register(s4);
SamplerState s7 : register(s7);
SamplerState s9 : register(s9);
SamplerState s10 : register(s10);
SamplerState s14 : register(s14);
struct In {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float2 uv2 : TEXCOORD3;
    float3 v2 : TEXCOORD1;
    float4 v3 : TEXCOORD2;
    float3 v4 : TEXCOORD4;
    float3 v5 : TEXCOORD5;
    float3 v6 : TEXCOORD6;
};
struct Out { float4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; float4 o3 : SV_Target3; };

Out main(In i)
{
    float2 uv = i.uv;
    float2 dUx = ddx(uv), dUy = ddy(uv);
    if (PM.y > 0.5) {
        float3 dPx = -ddx(i.v6), dPy = -ddy(i.v6);
        float3 Ng = normalize(i.v2), V = normalize(i.v6);
        float3 p2 = cross(dPy, Ng), p1 = cross(Ng, dPx);
        float det = dot(dPx, p2);
        float amp = PM.x * saturate(1.5 - length(i.v6) / PM.z);
        if (abs(det) > 1e-12 && amp > 1e-5) {
            float3 gu = (p2 * dUx.x + p1 * dUy.x) / det, gv = (p2 * dUx.y + p1 * dUy.y) / det;
            float vn = max(dot(V, Ng), 0.12);
            float3 dirW = -V / vn;
            float2 du = float2(dot(gu, dirW), dot(gv, dirW));
            bool vUp = dot(i.v5, gv) > 0;     // which of the two heights matches this normal map's green
            int steps = (int)lerp(PM.w, 8, vn);
            float stepD = amp / steps, d = 0, prevD = 0, prevGap = -1, hitD = 0;
            [loop] for (int k = 0; k <= steps; ++k) {
                float2 hh = tHgt.SampleGrad(s10, uv + du * d, dUx, dUy).xy;
                float gap = (1 - (vUp ? hh.x : hh.y)) * amp - d;
                if (gap <= 0) { hitD = k ? lerp(prevD, d, prevGap / max(prevGap - gap, 1e-6)) : 0; break; }
                prevD = d; prevGap = gap; hitD = d;
                d += stepD;
            }
            uv += du * hitD;
        }
    }
    // the game's shader (rewritten from its disassembly)
    float3 n = (tNrm.SampleGrad(s4, uv, dUx, dUy).xyz - 0.5) * 2;
    float3 N = normalize(n.x * i.v4 + n.y * i.v5 + n.z * i.v2);
    Out o;
    o.o0.xyz = N * 0.5 + 0.5;
    float4 sp = tSpc.SampleGrad(s7, uv, dUx, dUy);
    o.o0.w = sp.y;
    float3 dif = tDif.SampleGrad(s0, uv, dUx, dUy).rgb * i.v3.xyz * M2[2].xyz;
    float4 ao = tAO.Sample(s1, i.uv2);
    float aoA = ao.y * i.v3.w;
    o.o1 = float4(sqrt(dif), aoA);
    float spec = saturate(((sp.x * 0.307841 + 0.686033) * sp.x + 0.006163) * sp.x + 0.001527);
    o.o2 = float4(spec.xxx, 1);
    float3 V = normalize(i.v6);
    float ndv = dot(V, N);
    float4 lut = tLut.Sample(s14, float3(ndv, sp.y, spec) * 0.9375 + 0.03125);
    float3 R = N * (2 * ndv) - V;
    float3 amb = sp.w * sp.w * ao.w * M2[1].xyz * M2[0].y * C0[0].x;
    float4 env = tRef.SampleLevel(s3, R, sp.y * 5.5);
    float3 envc = (env.w * C0[2].xyz + env.xyz * C0[3].xyz) * (lut.x * ((1 - spec) * lut.y + spec));
    float3 lm = (tLM.Sample(s9, i.uv2).rgb * (C0[0].x * M2[0].x) + amb) * dif;
    float3 r = envc * (aoA * aoA) + lm;
    o.o3 = float4(sqrt(r * (tTon.Sample(s2, float2(0.5, 0.5)).w * C0[1].x)), 0);
    return o;
}
)HLSL";

static const unsigned long long kBldPsHash = 0xec3734a655430424ull;
static ID3D11PixelShader* volatile g_bldPs;
static void* g_bldCode;
static SIZE_T g_bldSize;
static ID3D11PixelShader* g_bldPom;
static ID3D11Buffer *g_bldCbOn, *g_bldCbOff;
static float g_bldCbData[4];
static ID3D11SamplerState* g_hgtSmp;
static int g_bldState;
static bool g_bldBound;                    // ours is bound on the immediate context (BeforeDraw binds its height map)

// ---- heights by normal map
struct HgtEntry { unsigned long long hash; ID3D11ShaderResourceView* srv; int state; };   // 1 waiting, 2 made, 3 failed
static HgtEntry g_hgt[4096];
static HgtEntry* HgtFind(unsigned long long h, bool add)
{
    for (int k = 0; k < 64; ++k) {
        HgtEntry& e = g_hgt[(unsigned)(h + k * 7) & 4095];
        if (e.hash == h) return &e;
        if (!e.hash) { if (!add) return 0; e.hash = h; e.srv = 0; e.state = 0; return &e; }
    }
    return 0;
}
// the job for the worker: one normal map at a time
struct HgtJob { unsigned long long hash; int w, h; float* nx; float* ny; float* nz; unsigned char* out; volatile LONG done; };
static HgtJob g_job;
static volatile LONG g_jobBusy;
static ID3D11Texture2D* g_jobTex;           // the normal map waiting to be read (one at a time)
static unsigned long long g_jobHash;
static unsigned g_jobQueued;
static volatile LONG g_hgtMade, g_hgtLoaded, g_hgtFailed;

// FFT (radix 2, in place) of n complex values, stride s
static void Fft(float* re, float* im, int n, int s, bool inv)
{
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i * s]; re[i * s] = re[j * s]; re[j * s] = t; t = im[i * s]; im[i * s] = im[j * s]; im[j * s] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double a = 2 * 3.14159265358979 / len * (inv ? 1 : -1);
        float wr = (float)cos(a), wi = (float)sin(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; ++k) {
                int p = (i + k) * s, q = (i + k + len / 2) * s;
                float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti;
                float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
    if (inv) for (int i = 0; i < n; ++i) { re[i * s] /= n; im[i * s] /= n; }
}
static void Fft2(float* re, float* im, int w, int h, bool inv)
{
    for (int y = 0; y < h; ++y) Fft(re + y * w, im + y * w, w, 1, inv);
    for (int x = 0; x < w; ++x) Fft(re + x, im + x, h, w, inv);
}
// heights from slopes p = dh/du, q = dh/dv (texels), into 0..255 (2nd to 98th percentile)
static void Integrate(int w, int h, const float* p, const float* q, unsigned char* out, int stride)
{
    int n = w * h;
    float *pr = (float*)calloc(n, 4), *pi = (float*)calloc(n, 4), *qr = (float*)calloc(n, 4), *qi = (float*)calloc(n, 4);
    memcpy(pr, p, n * 4); memcpy(qr, q, n * 4);
    Fft2(pr, pi, w, h, false); Fft2(qr, qi, w, h, false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            float wu = 2 * 3.14159265f * (x <= w / 2 ? x : x - w) / w, wv = 2 * 3.14159265f * (y <= h / 2 ? y : y - h) / h;
            float d = wu * wu + wv * wv;
            if (d < 1e-12f) { pr[i] = pi[i] = 0; continue; }
            // H = -i (wu P + wv Q) / d  ->  (a + ib)(-i) = b - ia
            float ar = wu * pr[i] + wv * qr[i], ai = wu * pi[i] + wv * qi[i];
            pr[i] = ai / d; pi[i] = -ar / d;
        }
    Fft2(pr, pi, w, h, true);
    // the range: 2nd and 98th percentiles from a histogram
    float mn = 1e30f, mx = -1e30f;
    for (int i = 0; i < n; ++i) { mn = pr[i] < mn ? pr[i] : mn; mx = pr[i] > mx ? pr[i] : mx; }
    int hist[1024] = {};
    for (int i = 0; i < n; ++i) { int b = (int)((pr[i] - mn) / (mx - mn + 1e-9f) * 1023); ++hist[b]; }
    int lo = 0, hi = 1023, acc = 0;
    for (; lo < 1023 && acc + hist[lo] < n / 50; ++lo) acc += hist[lo];
    acc = 0;
    for (; hi > 0 && acc + hist[hi] < n / 50; --hi) acc += hist[hi];
    float a = mn + (mx - mn) * lo / 1023.0f, b = mn + (mx - mn) * hi / 1023.0f;
    for (int i = 0; i < n; ++i) { float v = (pr[i] - a) / (b - a + 1e-9f); out[i * stride] = (unsigned char)(v < 0 ? 0 : v > 1 ? 255 : v * 255 + 0.5f); }
    free(pr); free(pi); free(qr); free(qi);
}
static DWORD WINAPI HgtWorker(LPVOID)
{
    HgtJob& j = g_job;
    int n = j.w * j.h;
    float *p = (float*)malloc(n * 4), *q = (float*)malloc(n * 4), *qn = (float*)malloc(n * 4);
    for (int i = 0; i < n; ++i) {
        float z = j.nz[i] < 0.2f ? 0.2f : j.nz[i];
        p[i] = -j.nx[i] / z; q[i] = -j.ny[i] / z; qn[i] = -q[i];
    }
    Integrate(j.w, j.h, p, q, j.out, 2);       // R: green = +v slope
    Integrate(j.w, j.h, p, qn, j.out + 1, 2);  // G: green = -v slope
    free(p); free(q); free(qn);
    InterlockedExchange(&j.done, 1);
    return 0;
}

// BC1 / BC3 colour block -> 16 RGBA texels
static void Bc1Block(const unsigned char* b, unsigned char* px)
{
    unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    unsigned char c[4][3];
    c[0][0] = (c0 >> 11) * 255 / 31; c[0][1] = ((c0 >> 5) & 63) * 255 / 63; c[0][2] = (c0 & 31) * 255 / 31;
    c[1][0] = (c1 >> 11) * 255 / 31; c[1][1] = ((c1 >> 5) & 63) * 255 / 63; c[1][2] = (c1 & 31) * 255 / 31;
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1) { c[2][k] = (2 * c[0][k] + c[1][k]) / 3; c[3][k] = (c[0][k] + 2 * c[1][k]) / 3; }
        else { c[2][k] = (c[0][k] + c[1][k]) / 2; c[3][k] = 0; }
    }
    unsigned idx = b[4] | b[5] << 8 | b[6] << 16 | (unsigned)b[7] << 24;
    for (int t = 0; t < 16; ++t) { int s = (idx >> (2 * t)) & 3; px[t * 4] = c[s][0]; px[t * 4 + 1] = c[s][1]; px[t * 4 + 2] = c[s][2]; px[t * 4 + 3] = 255; }
}

static unsigned char* HgtMakeTexData(const unsigned char* rg, int w, int h, UINT* mips, D3D11_SUBRESOURCE_DATA* sub)
{
    // R8G8 with a box-filtered chain of levels
    size_t total = 0;
    int m = 0;
    for (int ww = w, hh = h; ; ww = ww > 1 ? ww / 2 : 1, hh = hh > 1 ? hh / 2 : 1) { total += (size_t)ww * hh * 2; ++m; if (ww == 1 && hh == 1) break; }
    unsigned char* all = (unsigned char*)malloc(total);
    memcpy(all, rg, (size_t)w * h * 2);
    unsigned char* prev = all;
    size_t off = (size_t)w * h * 2;
    int pw = w, ph = h;
    sub[0].pSysMem = all; sub[0].SysMemPitch = w * 2;
    for (int k = 1; k < m && k < 16; ++k) {
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        unsigned char* cur = all + off;
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 2; ++c) {
                    int x0 = x * 2 < pw ? x * 2 : pw - 1, x1 = x * 2 + 1 < pw ? x * 2 + 1 : pw - 1, y0 = y * 2 < ph ? y * 2 : ph - 1, y1 = y * 2 + 1 < ph ? y * 2 + 1 : ph - 1;
                    cur[(y * nw + x) * 2 + c] = (unsigned char)((prev[(y0 * pw + x0) * 2 + c] + prev[(y0 * pw + x1) * 2 + c] + prev[(y1 * pw + x0) * 2 + c] + prev[(y1 * pw + x1) * 2 + c] + 2) / 4);
                }
        sub[k].pSysMem = cur; sub[k].SysMemPitch = nw * 2;
        off += (size_t)nw * nh * 2;
        prev = cur; pw = nw; ph = nh;
    }
    *mips = m < 16 ? m : 16;
    return all;
}

static ID3D11ShaderResourceView* HgtCreate(const unsigned char* rg, int w, int h)
{
    D3D11_SUBRESOURCE_DATA sub[16];
    UINT mips;
    unsigned char* all = HgtMakeTexData(rg, w, h, &mips, sub);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = mips; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = 0;
    ID3D11ShaderResourceView* v = 0;
    if (SUCCEEDED(oCreateTex2D(g_dev, &d, sub, &t))) { oCreateSRV(g_dev, t, 0, &v); t->Release(); }
    free(all);
    return v;
}

// a height map from a file (the user's in replace\, or ours in height_auto\)
static ID3D11ShaderResourceView* HgtLoadFile(const char* path)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA sub[16];
    unsigned char* blob = TxLoadDds(path, &d, sub, 16);
    if (!blob) return 0;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* t = 0;
    ID3D11ShaderResourceView* v = 0;
    if (SUCCEEDED(oCreateTex2D(g_dev, &d, sub, &t))) { oCreateSRV(g_dev, t, 0, &v); t->Release(); }
    free(blob);
    return v;
}

// at PSSetShader creation time
static void BldNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (CodeHash(code, size) != kBldPsHash) return;
    g_bldPs = ps;
    free(g_bldCode);
    g_bldCode = malloc(size);
    if (g_bldCode) { memcpy(g_bldCode, code, size); g_bldSize = size; }
    Log("parallax: the building shader found (%p)", ps);
}

static bool BldEnsure()
{
    if (g_bldState) return g_bldState == 1;
    g_bldState = 2;
    if (!g_compile || !g_reflect || !g_bldCode) return false;
    ID3DBlob *code = 0, *err = 0;
    if (FAILED(g_compile(kBldPomSrc, strlen(kBldPomSrc), "bldpom", 0, 0, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        Log("parallax (buildings): compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    bool same = false;
    ID3D11ShaderReflection *ro = 0, *rg = 0;
    if (SUCCEEDED(g_reflect(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), (void**)&ro)) &&
        SUCCEEDED(g_reflect(g_bldCode, g_bldSize, __uuidof(ID3D11ShaderReflection), (void**)&rg))) {
        D3D11_SHADER_DESC a, b;
        ro->GetDesc(&a); rg->GetDesc(&b);
        same = a.InputParameters == b.InputParameters;
        for (UINT k = 0; k < b.InputParameters && same; ++k) {
            D3D11_SIGNATURE_PARAMETER_DESC p, q;
            ro->GetInputParameterDesc(k, &p); rg->GetInputParameterDesc(k, &q);
            same = p.Register == q.Register && p.Mask == q.Mask && p.SemanticIndex == q.SemanticIndex && !_stricmp(p.SemanticName, q.SemanticName);
            if (!same) Log("parallax (buildings): input %u differs: ours %s%u r%u m%x, the game's %s%u r%u m%x", k, p.SemanticName, p.SemanticIndex,
                           p.Register, p.Mask, q.SemanticName, q.SemanticIndex, q.Register, q.Mask);
        }
    }
    if (ro) ro->Release();
    if (rg) rg->Release();
    if (same) g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_bldPom);
    code->Release();
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, 0, &g_bldCbOn);
    g_dev->CreateBuffer(&bd, 0, &g_bldCbOff);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_hgtSmp);
    char p[MAX_PATH];
    sprintf(p, "%s\\height_auto", g_txDir);
    CreateDirectoryA(p, 0);
    if (!g_bldPom || !g_bldCbOn || !g_bldCbOff || !g_hgtSmp) { Log("parallax (buildings): not used (signature %s)", same ? "same" : "differs"); return false; }
    Log("parallax: our building shader ready");
    g_bldState = 1;
    return true;
}

static ID3D11PixelShader* BldSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (Immediate(ctx)) g_bldBound = false;
    if (!ps || ps != g_bldPs || !g_enabled || g_p[P_POM].v < 0.5f || g_p[P_POM + 3].v <= 0 || !Immediate(ctx) || !BldEnsure()) return ps;
    float on[4] = { g_p[P_POM + 3].v * 0.01f, 1, g_p[P_POM + 2].v, 20 };
    if (memcmp(on, g_bldCbData, 16)) {
        memcpy(g_bldCbData, on, 16);
        ctx->UpdateSubresource(g_bldCbOn, 0, 0, on, 0, 0);
        float off[4] = { 0, 0, 1, 8 };
        ctx->UpdateSubresource(g_bldCbOff, 0, 0, off, 0, 0);
    }
    g_bldBound = true;
    return g_bldPom;
}

// before a draw with our building shader: its normal map's height map bound (or parallax off for this draw)
static void BldBeforeDraw(ID3D11DeviceContext* ctx)
{
    if (!g_bldBound) return;
    ID3D11ShaderResourceView* nv = 0;
    ctx->PSGetShaderResources(4, 1, &nv);
    ID3D11ShaderResourceView* hv = 0;
    if (nv) {
        ID3D11Resource* r = 0;
        nv->GetResource(&r);
        nv->Release();
        TxInfo* e = 0;
        if (r && g_txLockInit && TxIsTex2D(r)) { EnterCriticalSection(&g_txLock); e = TxFind((ID3D11Texture2D*)r, false); LeaveCriticalSection(&g_txLock); }
        if (e && e->hash) {
            HgtEntry* h = HgtFind(e->hash, true);
            if (h && h->state == 2) hv = h->srv;
            else if (h && h->state == 0) {
                // a file first: the user's, then ours from an earlier run
                char p[MAX_PATH];
                sprintf(p, "%s\\replace\\%016llx_height.dds", g_txDir, e->hash);
                ID3D11ShaderResourceView* v = HgtLoadFile(p);
                if (!v) { sprintf(p, "%s\\height_auto\\%016llx.dds", g_txDir, e->hash); v = HgtLoadFile(p); }
                if (v) { h->srv = v; h->state = 2; hv = v; InterlockedIncrement(&g_hgtLoaded); }
                else if (!g_jobTex) {   // to be made: read on the next Present
                    h->state = 1;
                    g_jobTex = (ID3D11Texture2D*)r; g_jobTex->AddRef();
                    g_jobHash = e->hash;
                }
            }
        }
        if (r) r->Release();
    }
    ctx->PSSetShaderResources(10, 1, &hv);
    ctx->PSSetSamplers(10, 1, &g_hgtSmp);
    ctx->PSSetConstantBuffers(10, 1, hv ? &g_bldCbOn : &g_bldCbOff);
}

// once a frame: read a waiting normal map, start its integration, take a finished one
static void BldTick()
{
    if (!g_dev) return;
    if (g_jobBusy && g_job.done) {   // finished: our texture, and a file for the next time
        HgtEntry* h = HgtFind(g_job.hash, false);
        ID3D11ShaderResourceView* v = HgtCreate(g_job.out, g_job.w, g_job.h);
        if (h) { h->srv = v; h->state = v ? 2 : 3; }
        if (v) {
            InterlockedIncrement(&g_hgtMade);
            char p[MAX_PATH];
            sprintf(p, "%s\\height_auto\\%016llx.dds", g_txDir, g_job.hash);
            D3D11_SUBRESOURCE_DATA sub[16];
            UINT mips;
            unsigned char* all = HgtMakeTexData(g_job.out, g_job.w, g_job.h, &mips, sub);
            const void* data[16]; UINT pitch[16];
            for (UINT k = 0; k < mips; ++k) { data[k] = sub[k].pSysMem; pitch[k] = sub[k].SysMemPitch; }
            TxWriteDds(p, DXGI_FORMAT_R8G8_UNORM, g_job.w, g_job.h, mips, data, pitch);
            free(all);
        } else InterlockedIncrement(&g_hgtFailed);
        free(g_job.nx); free(g_job.ny); free(g_job.nz); free(g_job.out);
        memset(&g_job, 0, sizeof(g_job));
        InterlockedExchange(&g_jobBusy, 0);
    }
    if (!g_jobTex || g_jobBusy) return;
    // read the normal map back: the largest level of at most 512 on its long side
    ID3D11DeviceContext* ctx = 0;
    g_dev->GetImmediateContext(&ctx);
    D3D11_TEXTURE2D_DESC d;
    g_jobTex->GetDesc(&d);
    UINT lv = 0, w = d.Width, h = d.Height;
    while ((w > 512 || h > 512) && lv + 1 < d.MipLevels) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; ++lv; }
    bool pow2 = w >= 16 && h >= 16 && !(w & (w - 1)) && !(h & (h - 1));
    DXGI_FORMAT plain = TxSrgb(d.Format, false);
    bool bc1 = plain == DXGI_FORMAT_BC1_UNORM || plain == DXGI_FORMAT_BC1_TYPELESS, bc3 = plain == DXGI_FORMAT_BC3_UNORM || plain == DXGI_FORMAT_BC3_TYPELESS;
    bool rgba = plain == DXGI_FORMAT_R8G8B8A8_UNORM || plain == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    HgtEntry* he = HgtFind(g_jobHash, false);
    if (!pow2 || !(bc1 || bc3 || rgba)) { if (he) he->state = 3; g_jobTex->Release(); g_jobTex = 0; ctx->Release(); InterlockedIncrement(&g_hgtFailed); return; }
    D3D11_TEXTURE2D_DESC s = d;
    s.Width = w; s.Height = h; s.MipLevels = 1; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    ID3D11Texture2D* st = 0;
    if (SUCCEEDED(oCreateTex2D(g_dev, &s, 0, &st))) {
        oCopySub(ctx, st, 0, 0, 0, 0, g_jobTex, lv, 0);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            int n = w * h;
            g_job.w = w; g_job.h = h; g_job.hash = g_jobHash;
            g_job.nx = (float*)malloc(n * 4); g_job.ny = (float*)malloc(n * 4); g_job.nz = (float*)malloc(n * 4);
            g_job.out = (unsigned char*)malloc(n * 2);
            unsigned char px[64];
            for (UINT by = 0; by < (rgba ? h : h / 4); ++by) {
                const unsigned char* row = (const unsigned char*)m.pData + (size_t)by * m.RowPitch;
                if (rgba) {
                    for (UINT x = 0; x < w; ++x) {
                        int i = by * w + x;
                        g_job.nx[i] = row[x * 4] / 127.5f - 1; g_job.ny[i] = row[x * 4 + 1] / 127.5f - 1; g_job.nz[i] = row[x * 4 + 2] / 127.5f - 1;
                    }
                    continue;
                }
                for (UINT bx = 0; bx < w / 4; ++bx) {
                    Bc1Block(row + bx * (bc3 ? 16 : 8) + (bc3 ? 8 : 0), px);
                    for (int t = 0; t < 16; ++t) {
                        int i = (by * 4 + t / 4) * w + bx * 4 + t % 4;
                        g_job.nx[i] = px[t * 4] / 127.5f - 1; g_job.ny[i] = px[t * 4 + 1] / 127.5f - 1; g_job.nz[i] = px[t * 4 + 2] / 127.5f - 1;
                    }
                }
            }
            ctx->Unmap(st, 0);
            g_job.done = 0;
            InterlockedExchange(&g_jobBusy, 1);
            HANDLE th = CreateThread(0, 0, HgtWorker, 0, 0, 0);
            if (th) CloseHandle(th); else { g_jobBusy = 0; if (he) he->state = 3; }
        }
        st->Release();
    }
    g_jobTex->Release();
    g_jobTex = 0;
    ctx->Release();
}
