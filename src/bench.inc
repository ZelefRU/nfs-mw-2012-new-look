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
    fprintf(f, "# NewLook bench viewpoints (Ctrl+Shift+B stores DroneCam's free camera): x y z yaw pitch (rad) name\n");
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
    sprintf(g_lampMsg, "bench done in %.1f min: costs next to the settings, the table in NewLook_bench_*.csv, the log (bench:)", (GetTickCount() - g_benchT0) / 60000.0);
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
        sprintf(p, "%sNewLook_bench_%04d%02d%02d_%02d%02d.csv", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
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
static void QbSampleGpu(double frameMs, const double* pass);
static void QbSampleCpu(double ms);
static void QbTick();
static void BenchSample(double frameMs, const double* pass)
{
    QbSampleGpu(frameMs, pass);   // 1.96: the quick bench
    if (g_padOn) g_padMs = pass[PR_PAD];
    if (g_benchPhase != 3 || frameMs <= 0 || g_sN >= 512) return;
    g_sGpu[g_sN++] = frameMs - (g_padOn ? pass[PR_PAD] : 0);   // the padding's own time out (it wobbles with the clocks)
    for (int k = 0; k < PR_COUNT; ++k) g_sPass[k] += pass[k];
}
// once a frame (before Present): the CPU frame, our CPU time in it, the game's draws in it
static void BenchCpu(double ms, double modMs)
{
    QbSampleCpu(ms);
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
    QbTick();   // 1.96: the quick bench
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

// ---------------------------------------------------------------- 1.96: the quick bench (Ctrl+B)
// For players: what each section (sections.inc) costs here, by day, at night and in the rain (with WEATHER on), and
// what the quality presets give, in 2-3 minutes, hands off. 1.96b: the game's own clock set (gametime.inc: its midday
// 50000 s, 02:00 at night; not hooked: as it is now), held each frame; the rain at midday with a downpour forced and the
// ground soaked. Windows of ~1.4 s after ~0.6 s to settle: everything as set (a reference before and after each
// window), the mod off, each section that is on switched off in turn (by night those that change at night; in the rain
// those of the rain), by day the quality presets (last). A window's figures: the median frame (vsync off), the GPU's
// frame span (timestamps), our passes, the CPU's wait in Present (it waits: the GPU is the limit). A section's cost:
// the references round it minus its window, by the CPU's frame or the GPU's span, whichever says more (the game's own
// 60 FPS cap hides the first). A reference after a window that differs much from the one before (a section's caches
// filling again): the one before alone. The report: NewLook_bench_report.txt (and the log: "quick bench:"); the menu
// shows each section's cost (the most of the scenes) next to it.
enum { QB_DAY, QB_NIGHT, QB_RAIN, QB_NOW, QB_SCN };
static const char* kQbScn[QB_SCN] = { "day", "night", "rain", "now" };
static const int kQbWinMax = 64;
static int g_qbPhase = -1;               // -1 idle; 0 a scene settling; 1 a window settling; 2 measuring
static int g_qbScenes[QB_SCN], g_qbScenesN, g_qbSc;   // the scenes run, the one now (index into g_qbScenes)
static int g_qbPlan[QB_SCN][kQbWinMax], g_qbPlanN[QB_SCN], g_qbWin;   // windows: -1 reference, -2 the mod off, 0.. a section off, 100 + q a preset
static double g_qbMs[QB_SCN][kQbWinMax], g_qbGpu[QB_SCN][kQbWinMax], g_qbWait[QB_SCN][kQbWinMax], g_qbMod[QB_SCN][kQbWinMax];
static double g_qbPhaseT, g_qbStartT;
static int g_qbFrames;
static double g_qbS[600], g_qbSW[600], g_qbSG[600], g_qbSM[600];
static int g_qbSN, g_qbSGN;
static float g_qbCost[kSecN] = { -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f, -1e9f };
static char g_qbSummary[3][240];
static int g_qbCostSc[kSecN];                    // 1.96e: the scene of a section's cost (its most)
static double g_qbAllMs[QB_SCN], g_qbModMs[QB_SCN];   // 1.96e: per scene: the frame with NewLook, NewLook's whole cost
static float g_qbQual0[kQualN], g_qbTod0;
static bool g_qbWasEnabled, g_qbUltraOk, g_qbTodSet, g_qbTodUsed;   // (used: for the report, after the restore)
static SunPathState g_qbSp;   // 1.96b: our sun path before the bench (put back with the game's clock)
static int g_qbQualNow;

static double QbNow() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return g_qpf.QuadPart ? (double)q.QuadPart / g_qpf.QuadPart : 0; }
static const char* QbCostText(int s)
{
    static char b[96];
    float c = g_qbCost[s];
    if (c < 0.15f) sprintf(b, "costs ~0 here");
    else {
        // (1.96e: in the scene it was measured in (the street lamps': the night's, not the day's FPS); its share of
        // NewLook's whole cost there)
        int sc = g_qbCostSc[s];
        double ref = g_qbAllMs[sc], m = g_qbModMs[sc];
        char pc[24] = "";
        if (m > 0.3) sprintf(pc, ", %.0f%% of NewLook", 100 * c / m);
        if (ref > c) sprintf(b, "costs %.1f ms %s%s (%.0f -> %.0f FPS off)", c, sc == QB_NIGHT ? "at night" : "here", pc, 1000 / ref, 1000 / (ref - c));
        else sprintf(b, "costs %.1f ms %s%s", c, sc == QB_NIGHT ? "at night" : "here", pc);
    }
    return b;
}
// the adapter, its memory, the driver's version (the report, the log)
static void GpuDesc(char* out, int n)
{
    *out = 0;
    IDXGIDevice* xd = 0; IDXGIAdapter* ad = 0;
    if (g_dev && SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&xd)) && SUCCEEDED(xd->GetAdapter(&ad))) {
        DXGI_ADAPTER_DESC d;
        ad->GetDesc(&d);
        char name[128];
        WideCharToMultiByte(CP_ACP, 0, d.Description, -1, name, sizeof(name), 0, 0);
        LARGE_INTEGER umd = {};
        bool v = SUCCEEDED(ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd));
        _snprintf(out, n - 1, "%s (%u MB, vendor %04x), driver %u.%u.%u.%u", name, (unsigned)(d.DedicatedVideoMemory >> 20), d.VendorId,
                  v ? HIWORD(umd.HighPart) : 0, v ? LOWORD(umd.HighPart) : 0, v ? HIWORD(umd.LowPart) : 0, v ? LOWORD(umd.LowPart) : 0);
        out[n - 1] = 0;
    }
    if (ad) ad->Release();
    if (xd) xd->Release();
}
static unsigned FreeAddrMb() { MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms); GlobalMemoryStatusEx(&ms); return (unsigned)(ms.ullAvailVirtual >> 20); }

// a window's setting (and back): the mod, a section, a preset
static void QbSet(int w, bool on)
{
    if (w == -2) g_enabled = on;
    else if (w >= 0 && w < kSecN) { g_p[P_SEC0 + w].v = on ? 1.0f : 0.0f; SecEnforce(); }
    else if (w >= 100) {
        if (!on) QualApply(w - 100);
        else for (int i = 0; i < kQualN; ++i) *UserVal(kQual[i].p) = g_qbQual0[i];
    }
}
// a scene: the game's clock held at its hour (every frame), the rain
static void QbScene(int sc)
{
    if (g_qbTodSet) TodSet(sc == QB_NIGHT ? 7200.0f : 50000.0f);   // (02:00; its midday)
    g_qbForceRain = sc == QB_RAIN;
}
// a scene's windows: a reference before and after each
static void QbPlanScene(int sc, bool first)
{
    int items[40], ni = 0;
    if (sc != QB_RAIN) items[ni++] = -2;
    // the sections: by day all but the rain's (the street lamps not by the game's day: off); at night those that change
    // at night; in the rain the rain's, the wet ground, the reflections. The street lamps last (their caches fill again
    // for seconds after)
    static const int kNight[] = { S_SKY, S_SHADOW, S_AIR, S_CARL };
    static const int kRain[] = { S_RAIN, S_WET, S_REFL };
    for (int s = 0; s < kSecN; ++s) {
        if (s == S_LAMPS || g_p[P_SEC0 + s].v < 0.5f) continue;
        bool use = false;
        if (sc == QB_DAY || sc == QB_NOW) use = s != S_WX && s != S_RAIN;
        else if (sc == QB_NIGHT) { for (int k = 0; k < 4; ++k) use |= kNight[k] == s; }
        else for (int k = 0; k < 3; ++k) use |= kRain[k] == s;
        if (use) items[ni++] = s;
    }
    if ((sc == QB_NIGHT || sc == QB_NOW) && g_p[P_SEC0 + S_LAMPS].v > 0.5f) items[ni++] = S_LAMPS;
    int n = 0;
    g_qbPlan[sc][n++] = -1;
    for (int i = 0; i < ni; ++i) { g_qbPlan[sc][n++] = items[i]; g_qbPlan[sc][n++] = -1; }
    if (first)   // the presets (their own figures, no references between)
        for (int q = 1; q <= 4; ++q) if (q != g_qbQualNow && (q < 4 || g_qbUltraOk)) g_qbPlan[sc][n++] = 100 + q;
    g_qbPlanN[sc] = n;
    for (int i = 0; i < n; ++i) g_qbMs[sc][i] = g_qbGpu[sc][i] = g_qbWait[sc][i] = g_qbMod[sc][i] = 0;
}
static const char* QbWinName(int w)
{
    static char b[64];
    if (w == -1) return "everything as set";
    if (w == -2) return "the mod off (the game alone)";
    if (w >= 100) { sprintf(b, "quality %s", kQualNames[w - 100]); return b; }
    sprintf(b, "%s off", kSec[w].name);
    return b;
}

static void QbRestore()
{
    g_enabled = g_qbWasEnabled;
    for (int i = 0; i < kQualN; ++i) *UserVal(kQual[i].p) = g_qbQual0[i];
    if (g_qbTodSet) {   // (the game's clock and our path as they were: its light, read back late, held 1.5 s)
        TodSet(g_qbTod0);
        g_sp = g_qbSp;
        g_spHoldUntil = QbNow() + 1.5;
    }
    g_qbTodSet = false;
    g_qbForceRain = false;
    g_qbHold = false;
    g_benchUncap = false;
    SecEnforce();
}
static void QbStop(const char* why)
{
    if (g_qbPhase < 0) return;
    int sc = g_qbScenes[g_qbSc];
    if (g_qbPhase >= 1 && g_qbWin < g_qbPlanN[sc]) QbSet(g_qbPlan[sc][g_qbWin], true);
    QbRestore();
    g_qbPhase = -1;
    sprintf(g_lampMsg, "benchmark %s", why);
    Log("quick bench: %s", why);
}
static void QbStart()
{
    if (!g_profOk) { strcpy(g_lampMsg, "benchmark: the GPU timing is not ready yet"); return; }
    if (g_weOn) WeToggle();
    g_qbWasEnabled = g_enabled;
    g_enabled = true;
    for (int i = 0; i < kQualN; ++i) g_qbQual0[i] = UserValue(kQual[i].p);
    g_qbQualNow = QualMatch();
    g_qbUltraOk = FreeAddrMb() > 700;   // (32 bits: Ultra's shadow maps are 256 MB)
    g_qbTodSet = g_qbTodUsed = TodKnown();
    g_qbTod0 = TodGet();
    g_qbSp = g_sp;
    g_qbScenesN = 0;
    if (g_qbTodSet) { g_qbScenes[g_qbScenesN++] = QB_DAY; g_qbScenes[g_qbScenesN++] = QB_NIGHT; }
    else g_qbScenes[g_qbScenesN++] = QB_NOW;   // (the game's clock not found: as it is now)
    if (g_p[P_WX].v > 0.5f && g_p[P_SEC0 + S_WX].v > 0.5f) g_qbScenes[g_qbScenesN++] = QB_RAIN;
    for (int k = 0; k < g_qbScenesN; ++k) QbPlanScene(g_qbScenes[k], k == 0);
    for (int s = 0; s < kSecN; ++s) g_qbCost[s] = -1e9f;
    g_qbSummary[0][0] = g_qbSummary[1][0] = g_qbSummary[2][0] = 0;
    g_qbSc = 0;
    g_qbHold = true;
    g_benchUncap = true;
    QbScene(g_qbScenes[0]);
    g_qbPhase = 0;
    g_qbStartT = g_qbPhaseT = QbNow();
    g_qbFrames = 0;
    char gpu[256];
    GpuDesc(gpu, sizeof(gpu));
    int n = 0;
    for (int k = 0; k < g_qbScenesN; ++k) n += g_qbPlanN[g_qbScenes[k]];
    Log("quick bench: start: %d scene(s), %d windows; the game's clock %s (%.0f s); %s; screen %ux%u, the game draws %ux%u", g_qbScenesN, n,
        g_qbTodSet ? "set" : "not found: as it is", g_qbTod0, gpu, g_bbW, g_bbH, g_rtW, g_rtH);
}

// the samples (BenchCpu, BenchSample)
static void QbSampleCpu(double ms) { if (g_qbPhase == 2 && g_qbSN < 600) { g_qbS[g_qbSN] = ms; g_qbSW[g_qbSN] = g_benchWaitMs; ++g_qbSN; } }
static void QbSampleGpu(double frameMs, const double* pass)
{
    if (g_qbPhase != 2 || frameMs <= 0 || g_qbSGN >= 600) return;
    double mod = 0;
    static const int kMod[] = { PR_COPY, PR_HALF, PR_BLUR, PR_LAMPS, PR_SUN, PR_AIR, PR_LOOK, PR_EARLY, PR_PROBE, PR_SSMASK, PR_CLOUDS, PR_CSHADOW, PR_SKYDRAW };
    for (int k = 0; k < (int)(sizeof(kMod) / sizeof(kMod[0])); ++k) mod += pass[kMod[k]];
    g_qbSG[g_qbSGN] = frameMs; g_qbSM[g_qbSGN] = mod; ++g_qbSGN;
}

// the costs, the report
// the reference round window i (v: the CPU's frames or the GPU's spans): the ones before and after; the one after far
// from the one before (caches filling again after the window): the one before alone
static double QbRef(int sc, int i, double (*v)[kQbWinMax])
{
    double a = 0, b = 0;
    for (int k = i - 1; k >= 0; --k) if (g_qbPlan[sc][k] == -1 && v[sc][k] > 0) { a = v[sc][k]; break; }
    for (int k = i + 1; k < g_qbPlanN[sc]; ++k) if (g_qbPlan[sc][k] == -1 && v[sc][k] > 0) { b = v[sc][k]; break; }
    if (a > 0 && b > 0) return fabs(b - a) > (a * 0.06 > 0.8 ? a * 0.06 : 0.8) ? a : (a + b) / 2;
    return a > 0 ? a : b;
}
static double QbCost(int sc, int i)
{
    double c = g_qbMs[sc][i] > 0 ? QbRef(sc, i, g_qbMs) - g_qbMs[sc][i] : -1e9, g = g_qbGpu[sc][i] > 0 ? QbRef(sc, i, g_qbGpu) - g_qbGpu[sc][i] : -1e9;
    return c > g ? c : g;
}
static void QbReport()
{
    double cost[QB_SCN][kSecN], all[QB_SCN], allG[QB_SCN], off[QB_SCN], offG[QB_SCN], noise[QB_SCN], qual[QB_SCN][5];
    bool bound[QB_SCN] = {};
    for (int sc = 0; sc < QB_SCN; ++sc) {
        all[sc] = allG[sc] = off[sc] = offG[sc] = noise[sc] = 0;
        for (int s = 0; s < kSecN; ++s) cost[sc][s] = -1e9;
        for (int q = 0; q < 5; ++q) qual[sc][q] = 0;
    }
    for (int k = 0; k < g_qbScenesN; ++k) {
        int sc = g_qbScenes[k];
        double rmin = 1e9, rmax = 0, rsum = 0, gsum = 0, wsum = 0;
        int rn = 0;
        for (int i = 0; i < g_qbPlanN[sc]; ++i) {
            int w = g_qbPlan[sc][i];
            double ms = g_qbMs[sc][i];
            if (ms <= 0) continue;
            if (w == -1) { rmin = fmin(rmin, ms); rmax = fmax(rmax, ms); rsum += ms; gsum += g_qbGpu[sc][i]; wsum += g_qbWait[sc][i]; ++rn; }
            else if (w == -2) { off[sc] = ms; offG[sc] = g_qbGpu[sc][i]; }
            else if (w >= 100) qual[sc][w - 100] = ms;
            else cost[sc][w] = QbCost(sc, i);
        }
        all[sc] = rn ? rsum / rn : 0;
        allG[sc] = rn ? gsum / rn : 0;
        noise[sc] = rn > 1 ? (rmax - rmin) / 2 : 0;
        bound[sc] = rn && wsum / rn >= 1.0;
        qual[sc][g_qbQualNow] = all[sc];
    }
    double modC[QB_SCN];
    for (int sc = 0; sc < QB_SCN; ++sc) {
        modC[sc] = off[sc] > 0 ? fmax(all[sc] - off[sc], allG[sc] - offG[sc]) : 0;
        g_qbAllMs[sc] = all[sc]; g_qbModMs[sc] = modC[sc];
    }
    for (int s = 0; s < kSecN; ++s) {
        double c = -1e9;
        g_qbCostSc[s] = g_qbScenes[0];
        for (int k = 0; k < g_qbScenesN; ++k) if (cost[g_qbScenes[k]][s] > c) { c = cost[g_qbScenes[k]][s]; g_qbCostSc[s] = g_qbScenes[k]; }
        g_qbCost[s] = (float)c;
    }
    int s0 = g_qbScenes[0];
    // NewLook's whole cost: by the CPU's frame or the GPU's span, whichever says more (the game's own cap: its frame)
    double modCost = fmax(all[s0] - off[s0], allG[s0] - offG[s0]);
    bool capped = off[s0] > 0 && offG[s0] > 0 && offG[s0] < off[s0] * 0.85;
    // the report
    char path[MAX_PATH];
    sprintf(path, "%sNewLook_bench_report.txt", g_dir);
    FILE* f = fopen(path, "w");
    char gpu[256];
    GpuDesc(gpu, sizeof(gpu));
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (f) {
        fprintf(f, "NewLook 1.96h benchmark, %04d-%02d-%02d %02d:%02d (it took %.1f min)\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, (QbNow() - g_qbStartT) / 60);
        fprintf(f, "GPU: %s\n", gpu);
        fprintf(f, "Screen %ux%u; the game draws %ux%u%s\n", g_bbW, g_bbH, g_rtW, g_rtH,
                (double)g_rtW * g_rtH > 1.3 * g_bbW * g_bbH ? "  <- its ANTI-ALIASING IS ON: every effect costs that much more" : " (its anti-aliasing off)");
        fprintf(f, "The game's clock: %s\n", g_qbTodUsed ? "set by the bench (day: its midday, night: 02:00), then put back" :
                "not found (another exe or mod): measured at the time it is now; run it again by day / at night");
        fprintf(f, "Quality: %s; sections off:", kQualNames[g_qbQualNow]);
        int offN = 0;
        for (int s = 0; s < kSecN; ++s) if (g_p[P_SEC0 + s].v < 0.5f) fprintf(f, "%s %s", offN++ ? "," : "", kSec[s].name);
        fprintf(f, "%s\n\n", offN ? "" : " none");
        fprintf(f, "Frame time in ms (lower is better), FPS in brackets:%*s", 14, "");
        for (int k = 0; k < g_qbScenesN; ++k) fprintf(f, "%-16s", kQbScn[g_qbScenes[k]]);
        fprintf(f, "\n  %-62s", "the game alone (NewLook off)");
        for (int k = 0; k < g_qbScenesN; ++k) { int sc = g_qbScenes[k]; if (off[sc] > 0) fprintf(f, "%5.1f (%3.0f)     ", off[sc], 1000 / off[sc]); else fprintf(f, "%-16s", "-"); }
        fprintf(f, "\n  %-62s", "  its GPU work (the GPU's frame span)");
        for (int k = 0; k < g_qbScenesN; ++k) { int sc = g_qbScenes[k]; if (offG[sc] > 0) fprintf(f, "%5.1f           ", offG[sc]); else fprintf(f, "%-16s", "-"); }
        fprintf(f, "\n  %-62s", "with NewLook, as set now");
        for (int k = 0; k < g_qbScenesN; ++k) { int sc = g_qbScenes[k]; fprintf(f, "%5.1f (%3.0f)%s    ", all[sc], all[sc] > 0 ? 1000 / all[sc] : 0, bound[sc] ? " " : "*"); }
        fprintf(f, "\n  %-62s", "  its GPU work");
        for (int k = 0; k < g_qbScenesN; ++k) fprintf(f, "%5.1f           ", allG[g_qbScenes[k]]);
        fprintf(f, "\n  %-62s", "  (the noise: the reference windows' spread, +-)");
        for (int k = 0; k < g_qbScenesN; ++k) fprintf(f, "%5.2f           ", noise[g_qbScenes[k]]);
        if (capped) fprintf(f, "\n  (the game alone runs into its own frame cap: what NewLook costs is told by the GPU's work)");
        fprintf(f, "\n\nWhat each section costs (ms saved when it is switched off, its share of NewLook's cost there; '.' not measured):\n");
        for (int s = 0; s < kSecN; ++s) {
            fprintf(f, "  %-62s", kSec[s].name);
            for (int k = 0; k < g_qbScenesN; ++k) {
                double c = cost[g_qbScenes[k]][s];
                if (c < -1e8) fprintf(f, "%-16s", g_p[P_SEC0 + s].v < 0.5f ? "(off)" : ".");
                else if (fabs(c) <= noise[g_qbScenes[k]] + 0.1) fprintf(f, "%-16s", "~0");
                else if (modC[g_qbScenes[k]] > 0.3 && c > 0) fprintf(f, "%5.1f (%3.0f%%)    ", c, 100 * c / modC[g_qbScenes[k]]);
                else fprintf(f, "%5.1f           ", c);
            }
            fprintf(f, "\n");
        }
        fprintf(f, "\nQuality presets (%s):", kQbScn[s0]);
        for (int q = 1; q <= 4; ++q) {
            double ms = qual[s0][q];
            if (ms > 0) fprintf(f, "   %s %.1f ms (%.0f FPS)%s", kQualNames[q], ms, 1000 / ms, q == g_qbQualNow ? " <- now" : "");
            else if (q == 4 && !g_qbUltraOk) fprintf(f, "   Ultra: not tried (little address space left)");
        }
        fprintf(f, "\n\n");
        // the heaviest three
        int top[3] = { -1, -1, -1 };
        for (int t = 0; t < 3; ++t)
            for (int s = 0; s < kSecN; ++s) {
                if (g_qbCost[s] < 0.3f || s == top[0] || s == top[1]) continue;
                if (top[t] < 0 || g_qbCost[s] > g_qbCost[top[t]]) top[t] = s;
            }
        if (top[0] >= 0) {
            fprintf(f, "The heaviest here:");
            for (int t = 0; t < 3; ++t) if (top[t] >= 0) fprintf(f, "%s %s %.1f ms", t ? "," : "", kSec[top[t]].name, g_qbCost[top[t]]);
            fprintf(f, ".\nSwitch them off (or lower Quality) for more FPS: the menu's sections each have an on / off line.\n");
        } else fprintf(f, "No section costs much here.\n");
        if (!bound[s0])
            fprintf(f, "* the GPU was not the limit (the CPU, the game's own frame cap or a limiter in the driver): the costs show only in part.\n");
        if (modCost > 0) {
            double sum = 0;
            for (int s = 0; s < kSecN; ++s) if (cost[s0][s] > 0) sum += cost[s0][s];
            fprintf(f, "(By %s NewLook costs %.1f ms; its sections' costs add up to %.1f ms: some work they share, some only one of them saves.)\n",
                    kQbScn[s0], modCost, sum);
        }
        fprintf(f, "\nEvery window (ms; GPU frame span; mod passes on the GPU; the CPU's wait in Present):\n");
        for (int k = 0; k < g_qbScenesN; ++k) {
            int sc = g_qbScenes[k];
            for (int i = 0; i < g_qbPlanN[sc]; ++i)
                fprintf(f, "  %-6s %-56s %6.2f  %6.2f  %6.2f  %5.2f\n", kQbScn[sc], QbWinName(g_qbPlan[sc][i]), g_qbMs[sc][i], g_qbGpu[sc][i], g_qbMod[sc][i], g_qbWait[sc][i]);
        }
        fclose(f);
    }
    // the menu's lines
    sprintf(g_qbSummary[0], "benchmark (%s): the game alone %.1f ms (%.0f FPS%s), with NewLook %.1f ms (%.0f FPS): NewLook costs %.1f ms%s", kQbScn[s0],
            off[s0], off[s0] > 0 ? 1000 / off[s0] : 0, capped ? ", its cap" : "", all[s0], all[s0] > 0 ? 1000 / all[s0] : 0, modCost,
            bound[s0] ? "" : "  (* not GPU-bound)");
    char* q = g_qbSummary[1];
    q += sprintf(q, "   quality:");
    for (int k = 1; k <= 4; ++k) if (qual[s0][k] > 0) q += sprintf(q, "  %s %.0f FPS", kQualNames[k], 1000 / qual[s0][k]);
    for (int k = 1; k < g_qbScenesN; ++k) {
        int s1 = g_qbScenes[k];
        q += sprintf(q, "      %s: %.0f FPS", kQbScn[s1], all[s1] > 0 ? 1000 / all[s1] : 0);
    }
    sprintf(g_qbSummary[2], "   the costs are next to the sections; all of it in scripts\\NewLook_bench_report.txt");
    Log("quick bench: done in %.1f min -> %s", (QbNow() - g_qbStartT) / 60, path);
    Log("quick bench: %s | %s", g_qbSummary[0], g_qbSummary[1]);
    for (int s = 0; s < kSecN; ++s) if (g_qbCost[s] > -1e8f) Log("quick bench:   %-44s %+.2f ms", kSec[s].name, g_qbCost[s]);
}

// every frame (BenchTick)
static void QbTick()
{
    if (g_qbPhase < 0) return;
    ++g_qbFrames;
    double t = QbNow() - g_qbPhaseT;
    int sc = g_qbScenes[g_qbSc];
    QbScene(sc);   // (kept: the game's clock at the scene's hour, the rain forced)
    if (g_qbPhase == 0) {   // a scene: its light, exposure, shadows and clouds settle
        sprintf(g_lampMsg, "benchmark: %s, settling ... hands off (B stops)", kQbScn[sc]);
        if (t >= 4.0 && g_qbFrames >= 20) { g_qbPhase = 1; g_qbWin = 0; g_qbPhaseT = QbNow(); g_qbFrames = 0; QbSet(g_qbPlan[sc][0], false); }
        return;
    }
    int w = g_qbPlan[sc][g_qbWin];
    if (g_qbPhase == 1) {   // a window settling (a reference after a section back on: longer)
        double need = w >= 100 ? 1.5 : w == -1 && g_qbWin > 0 ? 1.0 : 0.6;
        if (t >= need && g_qbFrames >= 8) { g_qbPhase = 2; g_qbPhaseT = QbNow(); g_qbFrames = 0; g_qbSN = g_qbSGN = 0; }
        return;
    }
    // measuring
    int done = 0, all = 0;
    for (int k = 0; k < g_qbScenesN; ++k) all += g_qbPlanN[g_qbScenes[k]];
    for (int k = 0; k < g_qbSc; ++k) done += g_qbPlanN[g_qbScenes[k]];
    done += g_qbWin;
    sprintf(g_lampMsg, "benchmark: %s, %s (%d of %d) ... hands off (B stops)", kQbScn[sc], QbWinName(w), done + 1, all);
    if ((t < 1.4 || g_qbFrames < 12) && g_qbSN < 600) return;
    g_qbMs[sc][g_qbWin] = Median(g_qbS, g_qbSN);
    g_qbWait[sc][g_qbWin] = Median(g_qbSW, g_qbSN);
    g_qbGpu[sc][g_qbWin] = Median(g_qbSG, g_qbSGN);
    g_qbMod[sc][g_qbWin] = Median(g_qbSM, g_qbSGN);
    Log("quick bench: %-6s %-56s %6.2f ms (GPU %.2f, mod passes %.2f, wait %.2f; %d frames)", kQbScn[sc], QbWinName(w), g_qbMs[sc][g_qbWin],
        g_qbGpu[sc][g_qbWin], g_qbMod[sc][g_qbWin], g_qbWait[sc][g_qbWin], g_qbSN);
    QbSet(w, true);
    if (++g_qbWin < g_qbPlanN[sc]) {
        QbSet(g_qbPlan[sc][g_qbWin], false);
        g_qbPhase = 1; g_qbPhaseT = QbNow(); g_qbFrames = 0;
        return;
    }
    if (++g_qbSc < g_qbScenesN) {   // the next scene
        QbScene(g_qbScenes[g_qbSc]);
        g_qbPhase = 0; g_qbPhaseT = QbNow(); g_qbFrames = 0;
        return;
    }
    QbRestore();
    g_qbPhase = -1;
    QbReport();
    sprintf(g_lampMsg, "benchmark done: the costs are next to the sections (scripts\\NewLook_bench_report.txt)");
    g_textDirty = true;
}
