// ---------------------------------------------------------------- 1.81d: the hitch finder
// Where a long frame's time went (the user: big drops in FPS when turning the camera, even with our effects off). Per
// frame (Present to Present): the time inside the game's draws (the driver: its own work, a shader pair it compiles at
// its first draw), ours round them; the pixel-shader binds (with our air pass); the making of textures, views and shaders
// (the driver's, ours: our look at each); Map / Unmap / UpdateSubresource (the driver's: a wait for the GPU, ours: the
// constants' copies); Present (the driver's: vsync, the GPU's queue; ours: our passes there). The rest is the game's own
// (its simulation, streaming, its own waits) and the calls we do not hook. A frame over 50 ms (and 2.5 x the average)
// is logged with its parts (twice a second at most), and every 1800 frames how many there were. CPU time stamps (TSC),
// measured against the performance counter.
enum { HT_DRAW, HT_DRAWOURS, HT_PSSET, HT_TEXDRV, HT_TEX, HT_SRV, HT_SHDRV, HT_SH, HT_MAPDRV, HT_MAP, HT_PRESDRV, HT_PRES, HT_QUERY, HT_FLUSH, HT_COPY, HT_N };
// (1.82b: + GetData (the game waiting on its queries: the GPU behind), Flush, CopyResource / CopySubresourceRegion)
// 1.98g: the biggest of "ours round them" apart (nested in it; the render thread): our passes at the game's sun draw, the
// sky (clouds, probe), our sun shadows' casters, the lamps' shadow maps, the car lights, the rain, the grade; and how many
// draws the game makes a frame (traffic: more draws, each one through all of this); our sky shader's binds and draws
enum { HS_SUN, HS_SKY, HS_SS, HS_LS, HS_CAR, HS_RAIN, HS_GRADE, HS_N };
static unsigned long long g_hsT[HS_N];
static double g_hsSum[HS_N];
static volatile LONG g_hsDraws;
static double g_hsDrawsSum;
static LONG g_skySubN, g_skyBeforeN, g_skyDrawN, g_skyRemakeN;   // (sky.inc; summed, read by HsText)
struct HsScope {
    int c; unsigned long long t0;
    HsScope(int k) : c(k), t0(__rdtsc()) {}
    ~HsScope() { g_hsT[c] += __rdtsc() - t0; }
};
struct HtCat { volatile LONGLONG t, mx; volatile LONG n; };
static HtCat g_ht[HT_N];
static double g_htPerMs;                     // TSC ticks a ms (0: not measured yet)
static unsigned long long g_htFrameT;        // the last frame's start (Present)
static double g_htAvg;                       // ms (running)
static unsigned g_htHitches, g_htHitchesOff, g_htFrames;
static double g_htWorst, g_htWorstRest;
static unsigned long long g_htLastLog;
static double g_htSum[HT_N], g_htSumMs;      // 1.96: the parts summed since the last stats line (HtAvgText)
static unsigned g_htSumN;

static void HtAddMt(int c, unsigned long long dt)   // (any thread: the making of resources)
{
    InterlockedExchangeAdd64(&g_ht[c].t, (LONGLONG)dt);
    InterlockedIncrement(&g_ht[c].n);
    LONGLONG m;
    while ((LONGLONG)dt > (m = g_ht[c].mx) && InterlockedCompareExchange64(&g_ht[c].mx, (LONGLONG)dt, m) != m) {}
}
static inline void HtAdd(int c, unsigned long long dt)   // (the render thread)
{
    g_ht[c].t += (LONGLONG)dt;
    ++g_ht[c].n;
    if ((LONGLONG)dt > g_ht[c].mx) g_ht[c].mx = (LONGLONG)dt;
}
// a hook's whole time: the original call(s) inside it (HT_CALL) to drv, the rest to ours
struct HtScope {
    int ours, drv; bool mt; unsigned long long t0, in;
    HtScope(int o, int d, bool m) : ours(o), drv(d), mt(m), t0(__rdtsc()), in(0) {}
    ~HtScope()
    {
        unsigned long long all = __rdtsc() - t0, ou = all > in ? all - in : 0;
        if (mt) { if (drv >= 0 && in) HtAddMt(drv, in); HtAddMt(ours, ou); }
        else { if (drv >= 0 && in) HtAdd(drv, in); HtAdd(ours, ou); }
    }
};
#define HT_CALL(h, ...) do { unsigned long long ht0_ = __rdtsc(); __VA_ARGS__; (h).in += __rdtsc() - ht0_; } while (0)

// Present (its start): the frame that ended, logged if long; all counts from 0
static void HtFrameEnd(bool effectsOn)
{
    unsigned long long now = __rdtsc();
    static unsigned long long calT;
    static LARGE_INTEGER calQ;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (!calT) { calT = now; calQ = q; }
    else if (g_qpf.QuadPart && q.QuadPart - calQ.QuadPart > g_qpf.QuadPart * 2)
        g_htPerMs = (double)(now - calT) / ((q.QuadPart - calQ.QuadPart) * 1000.0 / g_qpf.QuadPart);
    if (g_htFrameT && g_htPerMs > 0) {
        double ms = (now - g_htFrameT) / g_htPerMs;
        auto T = [&](int c) { return g_ht[c].t / g_htPerMs; };
        auto M = [&](int c) { return g_ht[c].mx / g_htPerMs; };
        ++g_htFrames;
        if (ms > 50 && ms < 10000 && ms > 2.5 * g_htAvg) {
            double known = 0;
            for (int c = 0; c < HT_N; ++c) known += T(c);
            double rest = ms - known;
            ++g_htHitches;
            if (!effectsOn) ++g_htHitchesOff;
            if (ms > g_htWorst) { g_htWorst = ms; g_htWorstRest = rest; }
            if (now - g_htLastLog > (unsigned long long)(g_htPerMs * 500)) {
                g_htLastLog = now;
                Log("hitch: a frame of %.0f ms (on average %.1f; our effects %s): the game's draws %.1f ms (%ld, the longest %.1f), ours round them %.1f; "
                    "pixel-shader binds %.1f (%ld, with our air pass); textures made %ld: the driver %.1f (the longest %.1f), ours %.1f; views made %ld: %.1f; "
                    "shaders made %ld: the driver %.1f (the longest %.1f), ours %.1f; Map / Unmap / Update %ld: the driver %.1f (the longest %.1f), ours %.1f; "
                    "Present: the driver %.1f, ours before it %.1f; queries' results asked %ld times: %.1f (the longest %.1f); Flush %ld: %.1f; copies %ld: %.1f; "
                    "the rest (the game itself, calls not hooked) %.0f ms",
                    ms, g_htAvg, effectsOn ? "on" : "off", T(HT_DRAW), g_ht[HT_DRAW].n, M(HT_DRAW), T(HT_DRAWOURS), T(HT_PSSET), g_ht[HT_PSSET].n,
                    g_ht[HT_TEX].n, T(HT_TEXDRV), M(HT_TEXDRV), T(HT_TEX), g_ht[HT_SRV].n, T(HT_SRV), g_ht[HT_SH].n, T(HT_SHDRV), M(HT_SHDRV), T(HT_SH),
                    g_ht[HT_MAP].n, T(HT_MAPDRV), M(HT_MAPDRV), T(HT_MAP), T(HT_PRESDRV), T(HT_PRES), g_ht[HT_QUERY].n, T(HT_QUERY), M(HT_QUERY),
                    g_ht[HT_FLUSH].n, T(HT_FLUSH), g_ht[HT_COPY].n, T(HT_COPY), rest);
            }
        }
        if (ms < 1000) g_htAvg = g_htAvg > 0 ? g_htAvg + ((ms < 100 ? ms : 100) - g_htAvg) * 0.02 : ms;
        if (ms < 1000) {
            for (int c = 0; c < HT_N; ++c) g_htSum[c] += T(c);
            for (int c = 0; c < HS_N; ++c) g_hsSum[c] += g_hsT[c] / g_htPerMs;
            g_hsDrawsSum += g_hsDraws;
            g_htSumMs += ms; ++g_htSumN;
        }
        if (g_htFrames >= 1800) {
            if (g_htHitches) Log("hitches (the last %u frames): %u frames over 50 ms (%u with our effects off), the worst %.0f ms (%.0f of it the game's own / not hooked)",
                                 g_htFrames, g_htHitches, g_htHitchesOff, g_htWorst, g_htWorstRest);
            g_htFrames = g_htHitches = g_htHitchesOff = 0;
            g_htWorst = g_htWorstRest = 0;
        }
    }
    g_htFrameT = now;
    for (int c = 0; c < HT_N; ++c) { g_ht[c].t = 0; g_ht[c].mx = 0; g_ht[c].n = 0; }
    for (int c = 0; c < HS_N; ++c) g_hsT[c] = 0;
    g_hsDraws = 0;
}

// 1.98g: the parts of ours (HsScope) and the counts, a frame on average since the last stats line (before HtAvgText)
static void HsText(char* out, int n)
{
    *out = 0;
    if (g_htSumN) {
        double k = 1.0 / g_htSumN;
        _snprintf(out, n - 1, "ours: at the game's sun draw (our passes) %.2f, sky %.2f, sun-shadow casters %.2f, lamp shadow maps %.2f, "
                  "car lights %.2f, rain %.2f, grade %.2f | the game's draws a frame %.0f | our sky shader a frame: binds %.2f, draws %.2f, "
                  "cloud passes %.2f, cloud targets made %ld",
                  g_hsSum[HS_SUN] * k, g_hsSum[HS_SKY] * k, g_hsSum[HS_SS] * k, g_hsSum[HS_LS] * k, g_hsSum[HS_CAR] * k, g_hsSum[HS_RAIN] * k,
                  g_hsSum[HS_GRADE] * k, g_hsDrawsSum * k, g_skySubN * k, g_skyDrawN * k, g_skyBeforeN * k, g_skyRemakeN);
        out[n - 1] = 0;
    }
    for (int c = 0; c < HS_N; ++c) g_hsSum[c] = 0;
    g_hsDrawsSum = 0;
    g_skySubN = g_skyBeforeN = g_skyDrawN = g_skyRemakeN = 0;
}

// 1.96: an average frame's parts since the last call (the stats line every 10 s), then from 0 again
static void HtAvgText(char* out, int n)
{
    *out = 0;
    if (g_htSumN) {
        double k = 1.0 / g_htSumN, known = 0;
        for (int c = 0; c < HT_N; ++c) known += g_htSum[c];
        _snprintf(out, n - 1, "a frame %.2f ms (%u frames): the game's draws %.2f (the driver) + ours round them %.2f | pixel-shader binds %.2f | "
                  "Map / Update %.2f + ours %.2f | copies %.2f | queries' results %.2f | resources made %.2f | Present: the driver %.2f "
                  "(vsync, the GPU's queue), ours %.2f | the game's own %.2f",
                  g_htSumMs * k, g_htSumN, g_htSum[HT_DRAW] * k, g_htSum[HT_DRAWOURS] * k, g_htSum[HT_PSSET] * k, g_htSum[HT_MAPDRV] * k,
                  g_htSum[HT_MAP] * k, g_htSum[HT_COPY] * k, g_htSum[HT_QUERY] * k,
                  (g_htSum[HT_TEXDRV] + g_htSum[HT_TEX] + g_htSum[HT_SRV] + g_htSum[HT_SHDRV] + g_htSum[HT_SH]) * k,
                  g_htSum[HT_PRESDRV] * k, g_htSum[HT_PRES] * k, (g_htSumMs - known) * k);
        out[n - 1] = 0;
    }
    for (int c = 0; c < HT_N; ++c) g_htSum[c] = 0;
    g_htSumMs = 0; g_htSumN = 0;
}
