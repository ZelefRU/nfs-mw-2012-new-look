// ---------------------------------------------------------------- bench (1.20; BENCH_PLAN.md)
// B: the selected setting; Ctrl+B: every effect of the registry, at every stored viewpoint (Ctrl+Shift+B stores
// DroneCam's free-camera pose) or here if there is none; B again stops.
// Each effect: its "off" state first (the padding is tuned there), then measured windows on / off / off / on / on /
// off, each after a settle time. Frame-cap proof: vsync is off while benching, and if the frame is still not
// GPU-bound (the GPU span well under the CPU frame) a padding pass of ours adds a fixed amount of GPU work, the same
// in "on" and "off", until it is: then the GPU frame span is the GPU's busy time and on - off is the effect's real
// cost. (The kernel's busy counter, = Task Manager, under-reports on this GPU: 24-60 % of the timestamped work; it is
// logged as a relative column only.) Medians per window, the spread between windows = the noise. A window whose game
// draw count strays from the effect's first window by more than 4 % (traffic, the camera) is measured again.

#include <winternl.h>
#include <d3dkmthk.h>

static const int kBenchSettle = 20, kBenchFrames = 60, kBenchCal = 90, kBenchWin = 6;
struct BenchRes {
    int valid, bad, bound;   // bad: windows that stayed unsteady; bound: the GPU was the bottleneck (span ~ CPU frame)
    double on, off, noise, modOn, modOff, cpuOn, cpuOff, waitOn, waitOff, kmtOn, kmtOff, pad, passOn[PR_COUNT], passOff[PR_COUNT];
    float onV[4], offV[4];
};
static BenchRes g_bench[kParams], g_benchMod;   // the last result per setting; the whole mod (Y)

// ---- the registry: an effect = the settings that switch it off (their off values: BenchOff)
struct FxDef { const char* name; short p[4]; };
static const FxDef kFx[] = {
    { "the whole mod", { -1, -2 } },
    { "sun shadows: our maps", { P_SS, -2 } },
    { "sun shadows: map resolution (level)", { P_SS + 2, -2 } },
    { "sun shadows: reach (to 90 m)", { P_SS + 3, -2 } },
    { "sun shadows: smoothing over frames", { P_SS + 4, -2 } },
    { "sun shadows: mask at half resolution (saves)", { P_SS_HALF, -2 } },
    { "sun shadows: the game's maps only past ours (saves)", { P_SS_SKIP, -2 } },
    { "sun shadows: kept maps redraw speed (to 50)", { P_SS + 7, -2 } },
    { "soft sun shadows (the game's maps)", { P_SOFT_SUN, -2 } },
    { "contact shadows", { 0, -2 } },
    { "ambient occlusion", { 6, -2 } },
    { "sky occlusion", { P_SKY_OCC, -2 } },
    { "bounce light", { P_BOUNCE, -2 } },
    { "haze", { 16, -2 } },
    { "light shafts", { 19, -2 } },
    { "sun glow", { 18, -2 } },
    { "lamps (all)", { P_LAMP, -2 } },
    { "lamps: shadow maps", { P_LAMP_SHADOW_MAPS, -2 } },
    { "lamps: screen-space shadows", { P_LAMP_SHADOW, -2 } },
    { "lamps: half resolution (saves)", { P_LAMP_RES, -2 } },
    { "lamps: glow in the air", { P_GLOW, -2 } },
    { "lamps: on every post", { P_AUTO_LAMPS, -2 } },
    { "reflections", { P_SSR, -2 } },
    { "reflections: wet ground", { P_SSR + 1, -2 } },
    { "reflections: resolution (to 30 %)", { P_SSR + 3, -2 } },
    { "headlight beams", { P_HL_BEAM, -2 } },
    { "the game's headlights", { P_HL_GAME, -2 } },
    { "traffic car lights", { P_TRAFFIC, -2 } },
    { "sky: ours", { P_SKY, -2 } },
    { "sky: cloud quality", { P_SKY + 13, -2 } },
    { "sky: cloud resolution", { P_SKY + 14, -2 } },
    { "colour grading", { P_LOOK, -2 } },
    { "grading: bloom", { P_LOOK + 14, -2 } },
    { "grading: clarity", { P_LOOK + 9, -2 } },
    { "grading: sharpen", { P_LOOK + 11, -2 } },
    { "car paint reflections", { 109, -2 } },
    { "the game's wet roads", { 110, -2 } },
    { "puddles", { 112, -2 } },
    { "relief", { P_REL, -2 } },
    { "parallax: roads", { P_POM, -2 } },
    { "parallax: buildings", { P_POM + 3, -2 } },
    { "lighting model (modern)", { P_PBR, -2 } },
    { "fast path", { P_FAST, -2 } },
};
static const int kFxN = sizeof(kFx) / sizeof(kFx[0]);

// the value that turns a setting off (or makes it cheapest)
static float BenchOff(int p)
{
    const Param& P = g_p[p];
    if (p == P_LAMP_SHADOW_BUDGET || p == P_LAMP_RADIUS || p == P_LAMP_REACH) return P.mn;
    return P.mn <= 0 ? 0 : P.mn;
}

// ---- viewpoints (DroneCam's free camera)
struct BenchPoint { float pose[5]; char name[48]; };
static BenchPoint g_bpt[32];
static int g_bptN;
static char g_bptPath[MAX_PATH];
typedef int(__cdecl* DcGetFn)(float*);
typedef void(__cdecl* DcSetFn)(const float*);
typedef void(__cdecl* DcHoldFn)(int);
static DcGetFn DcGet() { HMODULE m = GetModuleHandleA("version.dll"); return m ? (DcGetFn)GetProcAddress(m, "DroneCam_GetFreeCam") : 0; }
static DcSetFn DcSet() { HMODULE m = GetModuleHandleA("version.dll"); return m ? (DcSetFn)GetProcAddress(m, "DroneCam_SetFreeCam") : 0; }
static DcHoldFn DcHold() { HMODULE m = GetModuleHandleA("version.dll"); return m ? (DcHoldFn)GetProcAddress(m, "DroneCam_HoldFreeCam") : 0; }

static void BptLoad()
{
    g_bptN = 0;
    FILE* f = fopen(g_bptPath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_bptN < 32) {
        if (line[0] == '#') continue;
        BenchPoint& b = g_bpt[g_bptN];
        int n = 0;
        if (sscanf(line, "%f %f %f %f %f %n", &b.pose[0], &b.pose[1], &b.pose[2], &b.pose[3], &b.pose[4], &n) < 5) continue;
        strncpy(b.name, line + n, 47); b.name[47] = 0;
        for (char* c = b.name; *c; ++c) if (*c == '\n' || *c == '\r') *c = 0;
        ++g_bptN;
    }
    fclose(f);
}
static void BptSave()
{
    FILE* f = fopen(g_bptPath, "w");
    if (!f) return;
    fprintf(f, "# NFSLight bench viewpoints (Ctrl+Shift+B stores DroneCam's free camera): x y z yaw pitch (rad) name\n");
    for (int i = 0; i < g_bptN; ++i)
        fprintf(f, "%.3f %.3f %.3f %.5f %.5f %s\n", g_bpt[i].pose[0], g_bpt[i].pose[1], g_bpt[i].pose[2], g_bpt[i].pose[3], g_bpt[i].pose[4], g_bpt[i].name);
    fclose(f);
}
static void BptAdd()
{
    DcGetFn get = DcGet();
    float pose[5];
    if (!get) { strcpy(g_lampMsg, "bench viewpoint: DroneCam not found (version.dll with DroneCam_GetFreeCam)"); return; }
    if (!get(pose)) { strcpy(g_lampMsg, "bench viewpoint: turn DroneCam's free camera on (') and frame the view first"); return; }
    if (g_bptN >= 32) { strcpy(g_lampMsg, "bench viewpoint: 32 stored already"); return; }
    BenchPoint& b = g_bpt[g_bptN++];
    memcpy(b.pose, pose, sizeof(pose));
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf(b.name, "point%d_%.0f_%.0f", g_bptN, pose[0], pose[2]);
    BptSave();
    sprintf(g_lampMsg, "bench viewpoint %d stored (%.0f %.0f %.0f); %d in all - Ctrl+B runs them", g_bptN, pose[0], pose[1], pose[2], g_bptN);
    Log("bench: %s", g_lampMsg);
}

// ---- the kernel's GPU busy counter (relative column only)
typedef NTSTATUS(APIENTRY* KmtQueryFn)(const D3DKMT_QUERYSTATISTICS*);
static KmtQueryFn g_kmtQuery;
static LUID g_kmtLuid;
static int g_kmtNode = -1;
static LONGLONG KmtRunning()
{
    if (g_kmtNode == -1) {
        g_kmtNode = -2;
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        g_kmtQuery = gdi ? (KmtQueryFn)GetProcAddress(gdi, "D3DKMTQueryStatistics") : 0;
        IDXGIDevice* xd = 0; IDXGIAdapter* ad = 0;
        if (g_kmtQuery && g_dev && SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d; ad->GetDesc(&d);
            g_kmtLuid = d.AdapterLuid;
            g_kmtNode = 0;   // the 3D engine (node 0 on the user's RTX 3080, checked with ltest/busytest)
        }
        if (ad) ad->Release();
        if (xd) xd->Release();
    }
    if (g_kmtNode < 0) return -1;
    D3DKMT_QUERYSTATISTICS q;
    memset(&q, 0, sizeof(q));
    q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE; q.AdapterLuid = g_kmtLuid; q.hProcess = GetCurrentProcess(); q.QueryProcessNode.NodeId = g_kmtNode;
    return g_kmtQuery(&q) == 0 ? q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart : -1;
}

static double Median(double* v, int n)
{
    if (n <= 0) return 0;
    std::sort(v, v + n);
    return n & 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5;
}

// ---- the run
struct BenchWin { double gpu, cpu, mod, wait, draws, kmt, pass[PR_COUNT]; };
static int g_benchPhase = -1;              // -1 idle; 0 moving to a viewpoint; 1 calibrating (off); 2 settle; 3 measure
static int g_benchCalBound;
static bool g_padGrew;
static int g_benchFrame, g_benchWinI, g_benchRetry, g_benchFx, g_benchPt, g_benchPtN;
static bool g_benchAll, g_benchUncap;      // the whole registry; vsync off while benching
static int g_benchOne = -1;                // B: one setting (its index), else the registry
static float g_benchOrig[4], g_benchOnV[4], g_benchOffV[4];
static int g_benchP[4], g_benchPn;
static const char* g_benchName;
static BenchWin g_bw[kBenchWin];
static bool g_bwOk[kBenchWin];
static int g_bwBad;
static double g_bDrawsRef;
// this window's samples
static double g_sGpu[512], g_sCpu[512], g_sMod[512], g_sWait[512], g_sDraws[512], g_sPass[PR_COUNT];
static int g_sN, g_sNc;
static LONGLONG g_sKmt0; static LARGE_INTEGER g_sQpc0;
static LONG g_drawsAtFrame;
static double g_benchWaitMs;   // the CPU's wait in the last Present (the render-ahead queue full: GPU-bound)
static double g_waitRing[16];  // the last frames' waits
static int g_waitRingN;
static double RecentWait()
{
    double a[16];
    int n = g_waitRingN < 16 ? g_waitRingN : 16;
    memcpy(a, g_waitRing, sizeof(double) * n);
    return Median(a, n);
}
// padding: fixed GPU work of ours (units of ~256 loop steps over 1024 x 1024 pixels)
static float g_padUnits;
static double g_padMs;
static bool g_padOn;
static HalfRT g_padRT;
static ID3D11PixelShader* g_psPad;
static ID3D11Buffer* g_padCb;
static FILE* g_benchCsv;
static DWORD g_benchT0;
static const bool kWinOn[kBenchWin] = { true, false, false, true, true, false };
static const char* kPadSrc = R"HLSL(
cbuffer Pad : register(b13) { float4 Rect; float4 K; };
float4 ps_pad(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float a = uv.x * 3 + uv.y + K.y;
    [loop] for (int i = 0; i < (int)K.x; ++i) a = sin(a) * 1.0001 + cos(a * 0.7);
    return a * 1e-6;
}
)HLSL";

static const char* BenchName(int p) { return p < 0 ? "the whole mod" : g_p[p].label; }
static void BenchApply(int p, float v) { if (p < 0) g_enabled = v > 0.5f; else g_p[p].v = v; }
static void BenchSet(bool on) { for (int k = 0; k < g_benchPn; ++k) BenchApply(g_benchP[k], on ? g_benchOnV[k] : g_benchOffV[k]); }


static void BenchRestore();
// the next effect (or the next viewpoint, or done)
static void BenchNextFx()
{
    for (;;) {
        if (g_benchOne >= 0) { if (g_benchFx > 0) break; }
        else if (g_benchFx >= kFxN) {   // this viewpoint done: the next one
            ++g_benchPt;
            if (g_benchPt < g_benchPtN) { g_benchFx = 0; g_benchPhase = 0; g_benchFrame = 0; if (DcSet()) DcSet()(g_bpt[g_benchPt].pose); return; }
            break;
        }
        // set up this effect: its settings now (to put back) and the values compared
        g_benchPn = 0;
        if (g_benchOne >= 0) { g_benchP[0] = g_benchOne; g_benchPn = 1; g_benchName = BenchName(g_benchOne); }
        else {
            const FxDef& F = kFx[g_benchFx];
            for (int k = 0; k < 4 && F.p[k] != -2; ++k) g_benchP[g_benchPn++] = F.p[k];
            g_benchName = F.name;
        }
        bool differs = false;
        for (int k = 0; k < g_benchPn; ++k) {
            int p = g_benchP[k];
            float cur = p < 0 ? (g_enabled ? 1.0f : 0.0f) : g_p[p].v, off = p < 0 ? 0.0f : BenchOff(p);
            g_benchOrig[k] = cur;
            g_benchOffV[k] = off;
            g_benchOnV[k] = cur != off ? cur : (p < 0 ? 1.0f : g_p[p].def != off ? g_p[p].def : g_p[p].mx);   // off now: what turning it on costs
            if (g_benchOnV[k] != g_benchOffV[k]) differs = true;
        }
        if (!differs) { ++g_benchFx; continue; }   // nothing to compare
        Log("bench: %s: %s", g_benchName, "measuring");
        BenchSet(false);
        g_benchPhase = 1; g_benchFrame = 0; g_benchWinI = 0; g_benchRetry = 0; g_bwBad = 0; g_bDrawsRef = 0; g_benchCalBound = 0;
        g_sN = g_sNc = 0;
        sprintf(g_lampMsg, "bench: %s (%d of %d%s) ... hands off", g_benchName, g_benchFx + 1, g_benchOne >= 0 ? 1 : kFxN,
                g_benchPtN ? ", viewpoint" : "");
        return;
    }
    // finished
    g_benchPhase = -1;
    g_benchUncap = false;
    g_padOn = false;
    if (DcHold()) DcHold()(0);
    if (g_benchCsv) { fclose(g_benchCsv); g_benchCsv = 0; }
    BenchRestore();
    sprintf(g_lampMsg, "bench done in %.1f min: costs next to the settings, the table in NFSLight_bench_*.csv, the log (bench:)", (GetTickCount() - g_benchT0) / 60000.0);
    Log("bench: done");
}

static bool g_benchWasEnabled, g_benchWasEditor, g_benchEdEnabled;
// the mod's state before the run, put back after (the run needs the effects on and the world editor closed)
static void BenchRestore()
{
    if (g_benchWasEditor && !g_weOn) { g_enabled = g_benchEdEnabled; WeToggle(); }   // (the editor keeps what it will put back)
    else g_enabled = g_benchWasEnabled;
}
static void BenchStart(bool all, int one)
{
    g_benchWasEnabled = g_enabled; g_benchWasEditor = g_weOn; g_benchEdEnabled = g_weWasEnabled;
    if (g_weOn) WeToggle();   // (it switched the effects off; its own toggle puts them back)
    g_enabled = true;
    g_benchAll = all;
    g_benchOne = all ? -1 : one;
    g_benchFx = 0; g_benchPt = 0;
    g_benchPtN = all ? g_bptN : 0;
    g_benchUncap = true;
    g_benchT0 = GetTickCount();
    g_padUnits = 0;
    if (all) {
        SYSTEMTIME st; GetLocalTime(&st);
        char p[MAX_PATH];
        sprintf(p, "%sNFSLight_bench_%04d%02d%02d_%02d%02d.csv", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        g_benchCsv = fopen(p, "w");
        if (g_benchCsv) fprintf(g_benchCsv, "viewpoint,effect,off,on,gpu_off_ms,gpu_on_ms,cost_ms,noise_ms,gpu_bound,padding_ms,mod_passes_ms,cpu_frame_off,cpu_frame_on,mod_cpu_ms,kernel_busy_off_pct,kernel_busy_on_pct,unsteady_windows,top_passes\n");
        Log("bench: start: %d effects x %d viewpoint(s) -> %s", kFxN, g_benchPtN ? g_benchPtN : 1, p);
    } else Log("bench: start (%s)", BenchName(one));
    if (g_benchPtN) {
        if (!DcSet() || !DcHold()) { g_benchPtN = 0; Log("bench: DroneCam not found: here only"); }
        else { DcHold()(1); DcSet()(g_bpt[0].pose); g_benchPhase = 0; g_benchFrame = 0; strcpy(g_lampMsg, "bench: to viewpoint 1 ..."); return; }
    }
    BenchNextFx();
}

static void BenchStop()
{
    if (g_benchPhase < 0) return;
    if (g_benchPhase >= 1) for (int k = 0; k < g_benchPn; ++k) BenchApply(g_benchP[k], g_benchOrig[k]);
    g_benchPhase = -1;
    g_benchUncap = false;
    g_padOn = false;
    if (DcHold()) DcHold()(0);
    if (g_benchCsv) { fclose(g_benchCsv); g_benchCsv = 0; }
    BenchRestore();
    strcpy(g_lampMsg, "bench stopped");
}

// one finished frame's GPU numbers (from the profiler, a few frames late: the settle time covers it)
static void BenchSample(double frameMs, const double* pass)
{
    if (g_padOn) g_padMs = pass[PR_PAD];
    if (g_benchPhase != 3 || frameMs <= 0 || g_sN >= 512) return;
    g_sGpu[g_sN++] = frameMs - (g_padOn ? pass[PR_PAD] : 0);   // the padding's own time out (it wobbles with the clocks)
    for (int k = 0; k < PR_COUNT; ++k) g_sPass[k] += pass[k];
}
// once a frame (before Present): the CPU frame, our CPU time in it, the game's draws in it
static void BenchCpu(double ms, double modMs)
{
    LONG dc = g_drawCnt[0] + g_drawCnt[1] + g_drawCnt[2] + g_drawCnt[3];
    double draws = (double)(dc - g_drawsAtFrame);
    g_drawsAtFrame = dc;
    g_waitRing[g_waitRingN++ & 15] = g_benchWaitMs;
    if (g_benchPhase != 3 || g_sNc >= 512) return;
    g_sCpu[g_sNc] = ms; g_sWait[g_sNc] = g_benchWaitMs; g_sMod[g_sNc] = modMs; g_sDraws[g_sNc] = draws;
    ++g_sNc;
}

// the padding pass (at Present, before the overlay): g_padUnits of fixed GPU work
static void BenchPad(ID3D11DeviceContext* ctx)
{
    if (!g_padOn || g_padUnits < 0.05f || !g_vsText) return;
    if (!g_psPad) {
        ID3DBlob *code = 0, *err = 0;
        if (g_compile && SUCCEEDED(g_compile(kPadSrc, strlen(kPadSrc), "pad", 0, 0, "ps_pad", "ps_4_0", 0, 0, &code, &err)))
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, &g_psPad);
        if (code) code->Release();
        if (err) err->Release();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bd, 0, &g_padCb);
        if (!MakeRT(g_padRT, 1024, 1024, DXGI_FORMAT_R8_UNORM, false)) Log("bench: padding target failed");
        if (!g_psPad || !g_padCb || !g_padRT.rtv) { g_padOn = false; Log("bench: padding pass not available"); return; }
    }
    int draws = (int)ceilf(g_padUnits);
    if (draws > 400) draws = 400;
    float steps = 256.0f * g_padUnits / draws;
    float cb[8] = { -1, 1, 2, 2, steps, (float)(g_frame & 7), 0, 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_padCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, cb, 32);
    ctx->Unmap(g_padCb, 0);
    ID3D11RenderTargetView* sRtv = 0; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(1, &sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16; ctx->RSGetViewports(&nVp, sVp);
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0; ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11Buffer* sVcb = 0; ctx->VSGetConstantBuffers(13, 1, &sVcb);
    ID3D11Buffer* sPcb = 0; ctx->PSGetConstantBuffers(13, 1, &sPcb);
    D3D11_VIEWPORT vp = { 0, 0, 1024, 1024, 0, 1 };
    ctx->OMSetRenderTargets(1, &g_padRT.rtv, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(g_vsText, 0, 0);
    oPSSetShader(ctx, g_psPad, 0, 0);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->RSSetState(g_rsNoCull);
    ctx->VSSetConstantBuffers(13, 1, &g_padCb);
    ctx->PSSetConstantBuffers(13, 1, &g_padCb);
    ProfMark(ctx, PR_PAD, 0);
    for (int i = 0; i < draws; ++i) oDraw(ctx, 4, 0);
    ProfMark(ctx, PR_PAD, 1);
    ctx->OMSetRenderTargets(1, &sRtv, sDsv);
    ctx->RSSetViewports(nVp, sVp);
    ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl); ctx->IASetPrimitiveTopology(sTop);
    ctx->OMSetBlendState(sBl, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
    ctx->VSSetConstantBuffers(13, 1, &sVcb); ctx->PSSetConstantBuffers(13, 1, &sPcb);
    ID3D11DeviceChild* rel[] = { sRtv, sDsv, sVs, sPs, sIl, sBl, sDs, sRs, sVcb, sPcb };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
}

static void BenchWindowStart()
{
    g_sN = g_sNc = 0;
    memset(g_sPass, 0, sizeof(g_sPass));
    g_sKmt0 = KmtRunning();
    QueryPerformanceCounter(&g_sQpc0);
}

// a measured window done: its numbers; false = unsteady (the scene changed), measure it again
static bool BenchWindowEnd(BenchWin& w)
{
    int n = g_sN, nc = g_sNc;
    w.gpu = Median(g_sGpu, n);
    w.cpu = Median(g_sCpu, nc);
    w.wait = Median(g_sWait, nc);
    w.draws = Median(g_sDraws, nc);
    double modSum = 0;
    for (int i = 0; i < nc; ++i) modSum += g_sMod[i];
    w.mod = nc ? modSum / nc : 0;
    for (int k = 0; k < PR_COUNT; ++k) w.pass[k] = n ? g_sPass[k] / n : 0;
    LONGLONG k1 = KmtRunning();
    LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
    double wall = g_qpf.QuadPart ? (q1.QuadPart - g_sQpc0.QuadPart) * 1000.0 / g_qpf.QuadPart : 0;
    w.kmt = k1 >= 0 && g_sKmt0 >= 0 && wall > 0 ? (k1 - g_sKmt0) / 10000.0 / wall * 100 : -1;
    if (n < kBenchFrames / 2) return true;   // (too few GPU samples: the profiler lost them; nothing to judge by)
    if (!g_bDrawsRef) { g_bDrawsRef = w.draws; return true; }
    return fabs(w.draws - g_bDrawsRef) <= g_bDrawsRef * 0.04;
}

// the effect done: on / off from the windows' medians; results kept, logged, written
static void BenchFinish()
{
    double on[kBenchWin], off[kBenchWin];
    int nOn = 0, nOff = 0;
    BenchRes r;
    memset(&r, 0, sizeof(r));
    for (int i = 0; i < kBenchWin; ++i) {
        const BenchWin& w = g_bw[i];
        if (kWinOn[i]) on[nOn++] = w.gpu; else off[nOff++] = w.gpu;
        double* acc = kWinOn[i] ? r.passOn : r.passOff;
        for (int k = 0; k < PR_COUNT; ++k) acc[k] += w.pass[k] / 3;
        if (kWinOn[i]) { r.cpuOn += w.cpu / 3; r.modOn += w.mod / 3; r.waitOn += w.wait / 3; r.kmtOn += w.kmt / 3; }
        else { r.cpuOff += w.cpu / 3; r.modOff += w.mod / 3; r.waitOff += w.wait / 3; r.kmtOff += w.kmt / 3; }
    }
    double spreadOn = 0, spreadOff = 0;
    { double a[kBenchWin]; memcpy(a, on, sizeof(double) * nOn); std::sort(a, a + nOn); spreadOn = a[nOn - 1] - a[0]; }
    { double a[kBenchWin]; memcpy(a, off, sizeof(double) * nOff); std::sort(a, a + nOff); spreadOff = a[nOff - 1] - a[0]; }
    r.on = Median(on, nOn); r.off = Median(off, nOff);
    r.noise = (spreadOn + spreadOff) * 0.5;
    r.valid = 1; r.bad = g_bwBad;
    r.bound = r.waitOff >= 1.0;   // the CPU waited for the GPU in the off windows (padding included)
    r.pad = g_padOn ? g_padMs : 0;
    memcpy(r.onV, g_benchOnV, sizeof(r.onV)); memcpy(r.offV, g_benchOffV, sizeof(r.offV));
    double modP = 0;
    char passes[400], *q = passes;
    *q = 0;
    for (int k = 0; k < PR_COUNT; ++k) {
        if (k == PR_PAD || k == PR_GAMESUN || k == PR_OVERLAY) continue;
        double d = r.passOn[k] - r.passOff[k];
        if (k != PR_SHPASS) modP += d;
        if (fabs(d) > 0.05 && q - passes < 340) q += sprintf(q, " %s %+.2f", kProfNames[k], d);
    }
    double cost = r.on - r.off;
    Log("bench: %-40s %+7.2f ms (noise %.2f)  GPU %.2f -> %.2f ms%s%s  passes%s | CPU frame %.2f -> %.2f, mod CPU %+.2f ms | kernel busy %.0f -> %.0f %%%s",
        g_benchName, cost, r.noise, r.off, r.on, r.bound ? "" : " (NOT GPU-bound: lower bound)", r.pad > 0 ? " (padded)" : "", passes, r.cpuOff, r.cpuOn,
        r.modOn - r.modOff, r.kmtOff, r.kmtOn, r.bad ? "  UNSTEADY windows" : "");
    if (g_benchCsv) {
        const char* vp = g_benchPtN ? g_bpt[g_benchPt].name : "here";
        fprintf(g_benchCsv, "%s,\"%s\",%.3g,%.3g,%.3f,%.3f,%.3f,%.3f,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%d,\"%s\"\n", vp, g_benchName, r.offV[0], r.onV[0],
                r.off, r.on, cost, r.noise, r.bound, r.pad, modP, r.cpuOff, r.cpuOn, r.modOn - r.modOff, r.kmtOff, r.kmtOn, r.bad, passes);
        fflush(g_benchCsv);
    }
    if (g_benchPn == 1) { if (g_benchP[0] < 0) g_benchMod = r; else g_bench[g_benchP[0]] = r; }
    for (int k = 0; k < g_benchPn; ++k) BenchApply(g_benchP[k], g_benchOrig[k]);   // back to what it was
}

// every frame (Present)
static void BenchTick()
{
    if (g_benchPhase < 0) return;
    ++g_benchFrame;
    if (g_benchPhase == 0) {   // flying to a viewpoint: the world around it streams in
        if (g_benchFrame >= 150) { g_benchFrame = 0; BenchNextFx(); }
        return;
    }
    if (g_benchPhase == 1) {   // calibrating in the off state: is the GPU the bottleneck? if not, padding until it is
        // GPU-bound = the CPU waits in Present (the queue of frames is full). The GPU span alone cannot tell: a
        // CPU-limited game sends its draws over the whole frame and the GPU idles in between, so its span is about the
        // CPU frame either way.
        if (g_benchFrame == 1) { g_padOn = true; g_padGrew = false; g_padUnits *= 0.7f; }   // (the last effect's padding, a little less: it grows back if needed)
        if (g_benchFrame % 12 == 0) {
            double wait = RecentWait(), cpu = g_cpuFrameMs > 0 ? g_cpuFrameMs : 16.7;
            double perUnit = g_padUnits > 0.05f && g_padMs > 0.01 ? g_padMs / g_padUnits : 0;
            if (wait < 1.0) {   // not GPU-bound yet: more padding, about a third of the frame more each step
                double add = fmax(2.0, cpu * 0.3);
                if (perUnit > 0) g_padUnits += (float)(add / perUnit);
                else g_padUnits = g_padUnits < 0.05f ? 4.0f : g_padUnits * 2;
                if (g_padUnits > 4000) g_padUnits = 4000;
                g_benchCalBound = 0;
                g_padGrew = true;
            } else ++g_benchCalBound;
        }
        // done when GPU-bound for 2 checks (padding + 15 % margin), or after 400 frames anyway
        if (g_benchCalBound >= 2 || g_benchFrame >= 400) {
            if (g_padGrew && g_padUnits > 0.05f) g_padUnits *= 1.15f;
            Log("bench: %s: %s, padding %.1f units (%.2f ms), CPU wait in Present %.2f ms", g_benchName,
                g_benchCalBound >= 2 ? "GPU-bound" : "NOT GPU-bound after 400 frames", g_padUnits, g_padMs, RecentWait());
            g_benchPhase = 2; g_benchFrame = 0; g_benchWinI = 0; BenchSet(kWinOn[0]);
        }
        return;
    }
    if (g_benchPhase == 2) {   // settle after a switch
        if (g_benchFrame >= kBenchSettle) { g_benchPhase = 3; g_benchFrame = 0; BenchWindowStart(); }
        return;
    }
    // measuring
    if (g_benchFrame < kBenchFrames) return;
    bool ok = BenchWindowEnd(g_bw[g_benchWinI]);
    if (!ok && g_benchRetry < 2) {   // the scene changed: this window again
        ++g_benchRetry;
        g_benchPhase = 2; g_benchFrame = 0;
        return;
    }
    if (!ok) ++g_bwBad;
    g_benchRetry = 0;
    ++g_benchWinI;
    if (g_benchWinI < kBenchWin) {
        BenchSet(kWinOn[g_benchWinI]);
        g_benchPhase = 2; g_benchFrame = 0;
        sprintf(g_lampMsg, "bench: %s  %d/%d (%s) ... hands off (B stops)", g_benchName, g_benchWinI + 1, kBenchWin, kWinOn[g_benchWinI] ? "on" : "off");
        return;
    }
    BenchFinish();
    ++g_benchFx;
    if (g_benchOne >= 0) { g_benchFx = 1; }
    BenchNextFx();
}
