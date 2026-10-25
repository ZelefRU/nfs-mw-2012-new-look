// ---------------------------------------------------------------- 1.82: the weather
// One state drives what was set by hand before: rain now (0..1) and how cloudy it is (0..1), by hand or by itself (a
// walk through clear, fair, cloudy, overcast, drizzle, rain, heavy rain: minutes a phase, rain only from overcast on).
// Rain comes and goes over ~20 s, clouds over ~40 s. From them, every frame:
// - the sky: our clouds' cover, type, cirrus, dark rain bellies and their shadows, between the sky's presets (clear ..
//   storm) by the cloudiness: overcast while it rains (the sun dimmed by the cloud shadows);
// - the ground: wet in a minute or two of rain (WeatherSoak), dry again over WeatherDry minutes when it stops; the
//   puddles fill over WeatherPuddles minutes of rain and drain slower than the ground dries;
// - the haze thicker in rain (and fog: WeatherFog);
// - the car's drops: rain landing only while it falls (carrain.inc: g_wxRain), by how hard.
// The settings it moves change in the menu as it goes; the ones set by hand are kept when it is turned on and put
// back when it is turned off.
// 1.89: kinds of weather (WeatherKind; 0: rain and clouds by hand): each where the clouds and rain go and what comes
// with it: the drops' size and fall, the wind and its gusts, the haze, the dawn's haze, fog, cirrus. A change flows
// over WeatherChange minutes (the clouds; the rain in half that). By itself it walks between the kinds along the ways
// weather goes (clear -> fair -> cloudy -> overcast -> drizzle -> rain ...; a storm clears through "after the rain"),
// each kind as common as the time of day makes it (a misty dawn only at dawn, fog at night and in the morning,
// storms in the afternoon); picking a kind in the menu goes there now (by itself: the walk goes on from it).
static const int P_WX = 208;   // 208 on, 209 rain by hand, 210 clouds by hand, 211 by itself, 212 minutes a phase,
                               // 213 soak minutes, 214 dry minutes, 215 puddles' minutes, 216 fog
static const int kWxOwned[] = { 65, 112, 181, 184, 16, 93, 99, 100, 101, 102,   // wet ground, puddles, their water x2, haze, the sky's 5
                                219, 222, 224, 226, 149 };                      // 1.89: drop size, wind, gusts, the drops' fall, the dawn's haze
static const int P_WX_KIND = 237, P_WX_CHANGE = 238;   // 1.89
static const int kWxN = sizeof(kWxOwned) / sizeof(kWxOwned[0]);
static float g_wxBase[kWxN];
static bool g_wxWasOn;
static float g_wxCloud, g_wxWet, g_wxPuddle, g_wxRainTo, g_wxCloudTo;
static int g_wxPhase = 2;
static double g_wxPhaseLeft;   // seconds
static unsigned g_wxLogT;
static float g_wxTestDt;       // (tests only: a fixed time step)
// a kind: where the clouds (0..1, the sky by WxSky) and the rain go; with it (x the settings set by hand): drop size,
// their fall, the wind, its gusts, the haze, the dawn's haze; fog (0..1, at least WeatherFog), cirrus (+); how common
// (over hours), its phase (x WeatherPhase minutes); where it may go next by itself (-1: no more)
struct WxPhase { const char* name; float cloud, rain, drop, fall, wind, gust, haze, dawn, fog, cirrus, share, minutes; int next[5]; };
static const WxPhase kWxPhases[] = {
    { "clear",                0.06f, 0,     1,    1,    0.6f, 0.7f, 0.75f, 0.4f, 0,     0.1f, 0.12f, 1.0f, { 2, 1, 3, -1 } },
    { "clear, misty dawn",    0.10f, 0,     1,    1,    0.3f, 0.4f, 1.3f,  2.2f, 0.15f, 0,    0.05f, 0.6f, { 0, 2, 6, -1 } },
    { "fair (cumulus)",       0.32f, 0,     1,    1,    1.0f, 1.0f, 1.0f,  1.0f, 0,     0,    0.11f, 1.0f, { 0, 4, 3, 1, -1 } },
    { "hazy, humid",          0.42f, 0,     1,    1,    0.5f, 0.5f, 1.9f,  1.6f, 0.1f,  0.35f, 0.07f, 0.8f, { 2, 4, 0, -1 } },
    { "cloudy",               0.58f, 0,     1,    1,    1.0f, 1.2f, 1.0f,  1.0f, 0,     0,    0.11f, 1.0f, { 2, 5, 3, 11, -1 } },
    { "overcast",             0.80f, 0,     1,    1,    1.0f, 1.0f, 1.2f,  1.0f, 0,     0,    0.10f, 1.0f, { 4, 7, 6, 8, -1 } },
    { "fog",                  0.82f, 0,     1,    1,    0.2f, 0.3f, 1.6f,  1.5f, 0.6f,  0,    0.06f, 0.6f, { 5, 4, 1, -1 } },
    { "drizzle",              0.86f, 0.25f, 0.6f, 0.75f, 0.8f, 0.8f, 1.4f, 1.0f, 0.1f,  0,    0.09f, 1.0f, { 5, 8, -1 } },
    { "rain",                 0.93f, 0.6f,  1.0f, 1,    1.0f, 1.0f, 1.0f,  1.0f, 0,     0,    0.13f, 1.0f, { 7, 9, 10, 11, -1 } },
    { "downpour",             1.0f,  1.0f,  1.4f, 1.1f, 1.0f, 1.0f, 1.0f,  1.0f, 0,     0,    0.07f, 0.7f, { 8, 10, -1 } },
    { "storm (wind, gusts)",  1.0f,  1.0f,  1.25f, 1.1f, 2.2f, 2.0f, 1.0f, 1.0f, 0,     0,    0.06f, 0.6f, { 9, 8, 11, -1 } },
    { "after the rain",       0.45f, 0,     1,    1,    1.2f, 1.3f, 1.3f,  1.3f, 0.05f, 0,    0.05f, 0.7f, { 2, 4, 3, -1 } },
};
static const int kWxPhaseN = sizeof(kWxPhases) / sizeof(kWxPhases[0]);
static float g_wxK[8] = { 1, 1, 1, 1, 1, 1, 0, 0 };   // now: drop, fall, wind, gust, haze, dawn, fog, cirrus (towards the kind's)
static int g_wxShown = -999;                          // the WeatherKind we set (another value there: picked by hand)
static float WxHourNow();                             // (sunshadow.inc: our clock, hours; -1: our sun off)
static float WxRnd();
// how common a kind is at this hour (x its share)
static float WxTod(int k, float h)
{
    if (h < 0) return k == 1 ? 0.0f : 1.0f;
    auto in = [&](float a, float b) { return h >= a && h < b; };
    switch (k) {
    case 1: return in(4, 9) ? 5.0f : 0.0f;                          // a misty dawn
    case 6: return in(0, 9) || in(21, 24) ? 2.0f : in(11, 17) ? 0.25f : 1.0f;   // fog
    case 10: return in(13, 21) ? 2.0f : 0.5f;                       // storms
    case 3: return in(11, 19) ? 1.5f : 0.7f;                        // hazy, humid
    }
    return 1.0f;
}
// the next kind by itself: one of its ways (or stays a while), by how common each is now
static int WxNextKind(int k)
{
    float h = WxHourNow(), w[8], sum = 0;
    int to[8], n = 0;
    for (int j = 0; j < 5 && kWxPhases[k].next[j] >= 0; ++j) { to[n] = kWxPhases[k].next[j]; w[n] = kWxPhases[to[n]].share * WxTod(to[n], h); sum += w[n++]; }
    to[n] = k; w[n] = 0.25f * sum * (WxTod(k, h) > 0 ? 1.0f : 0.0f); sum += w[n++];   // (stays: a fifth of the time, if it may now)
    if (sum <= 0) return kWxPhases[k].next[0];
    float r = WxRnd() * sum;
    for (int j = 0; j < n; ++j) { if (r < w[j]) return to[j]; r -= w[j]; }
    return to[n - 1];
}

static float WxRnd() { static unsigned s = 0x2545F491u; s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s >> 8) / 16777216.0f; }
static float WxSmooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

// the sky's 5 (cover, type, cirrus, rain bellies, cloud shadows) at a cloudiness: between sky.inc's presets clear, fair,
// cloudy, overcast, storm
static void WxSky(float c, float* o)
{
    static const float st[5] = { 0.0f, 0.3f, 0.55f, 0.78f, 1.0f };
    static const float W[5][5] = { { 0.12f, 0.5f, 0.35f, 0, 0.85f }, { 0.4f, 0.55f, 0.2f, 0, 0.85f }, { 0.65f, 0.6f, 0.1f, 0.15f, 0.85f },
                                   { 1.0f, 0.12f, 0, 0.35f, 0.9f }, { 0.95f, 0.95f, 0, 1.0f, 1.0f } };
    int k = 0;
    while (k < 3 && c > st[k + 1]) ++k;
    float t = (c - st[k]) / (st[k + 1] - st[k]);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    for (int j = 0; j < 5; ++j) o[j] = W[k][j] + (W[k + 1][j] - W[k][j]) * t;
}

// once a frame (Present)
static void WeatherTick()
{
    static double last;
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    double now = g_qpf.QuadPart ? (double)q.QuadPart / g_qpf.QuadPart : 0;
    float dt = (float)(last > 0 ? now - last : 0.016);
    last = now;
    dt = dt < 0 ? 0 : dt > 0.25f ? 0.25f : dt;
    if (g_wxTestDt > 0) dt = g_wxTestDt;   // (a test: its own time step)
    // 1.83: the wind: Wind (+ WindStorm in rain), towards WindDir; gusts: its strength and direction wander (a few slow
    // waves); the camera's velocity (from its record read back: the rain's streaks seen from a moving camera)
    {
        static double gt;
        gt += dt;
        float t = (float)fmod(gt, 3600.0), gu = g_p[224].v;
        float sp = g_p[222].v + (g_wxRain > 0 ? g_p[225].v * g_wxRain : 0.0f);
        float g = 1 + gu * (0.45f * sinf(t * 0.63f) * sinf(t * 0.23f + 1) + 0.35f * sinf(t * 1.7f + 2) * sinf(t * 0.41f) + 0.2f * sinf(t * 3.1f));
        float a = (g_p[223].v + gu * 14 * sinf(t * 0.17f) * sinf(t * 0.07f + 2)) * 3.14159265f / 180;
        sp *= g < 0.1f ? 0.1f : g;
        g_wind[0] = sinf(a) * sp; g_wind[1] = 0; g_wind[2] = cosf(a) * sp;
        static float lastCam[3];
        static unsigned lastCamFrame;
        if (g_camKnown && dt > 0) {
            if (lastCamFrame && g_frame - lastCamFrame < 5) {
                float k = 1 - expf(-dt / 0.12f);
                for (int j = 0; j < 3; ++j) {
                    float v = (g_cam[3][j] - lastCam[j]) / (dt * (g_frame - lastCamFrame));
                    if (fabsf(v) > 150) v = 0;   // (a cut, a reset)
                    g_camVel[j] += (v - g_camVel[j]) * k;
                }
            }
            memcpy(lastCam, g_cam[3], 12);
            lastCamFrame = g_frame;
        }
    }
    bool on = g_p[P_WX].v > 0.5f;
    if (on != g_wxWasOn) {   // turned on: the hand-set values kept (the state starts from them); off: put back
        if (on) {
            for (int k = 0; k < kWxN; ++k) g_wxBase[k] = UserValue(kWxOwned[k]);   // (1.96: a switched-off section's own)
            g_wxWet = g_p[65].v; g_wxPuddle = g_p[184].v * (g_p[112].v > 0 ? 1.0f : 0.0f) * 0.5f;
            g_wxRain = 0; g_wxCloud = g_p[93].v;
            g_wxPhase = 0;   // (by itself: from the dry kind nearest the clouds now; its time set at once)
            for (int k = 1; k < kWxPhaseN; ++k)
                if (kWxPhases[k].rain <= 0 && k != 1 && k != 6 && fabsf(kWxPhases[k].cloud - g_wxCloud) < fabsf(kWxPhases[g_wxPhase].cloud - g_wxCloud)) g_wxPhase = k;
            g_wxPhaseLeft = -1e9;
            g_wxShown = -999;   // (1.89: a kind in the menu: taken)
            for (int j = 0; j < 8; ++j) g_wxK[j] = j < 6 ? 1.0f : 0.0f;
            Log("weather: on (rain %.2f, clouds %.2f by hand; by itself %d)", g_p[P_WX + 1].v, g_p[P_WX + 2].v, g_p[P_WX + 3].v > 0.5f);
        } else {
            for (int k = 0; k < kWxN; ++k) *UserVal(kWxOwned[k]) = g_wxBase[k];
            g_wxRain = -1;
            Log("weather: off (the settings set by hand put back)");
        }
        g_wxWasOn = on;
    }
    LensTick(dt);   // 1.84: rain on the lens (rainfall.inc)
    if (!on) return;
    // where it goes (1.89): a kind picked in the menu (now); by itself: its walk; else the rain and clouds by hand
    bool self = g_p[P_WX + 3].v > 0.5f;
    int picked = (int)(g_p[P_WX_KIND].v + 0.5f);
    if (picked != g_wxShown) {   // picked by hand (or the ini's at the start): there now
        g_wxShown = picked;
        if (picked > 0) {
            g_wxPhase = picked - 1 < kWxPhaseN ? picked - 1 : kWxPhaseN - 1;
            g_wxPhaseLeft = 60.0 * g_p[P_WX + 4].v * kWxPhases[g_wxPhase].minutes * (0.6 + 0.8 * WxRnd());
            Log("weather: %s picked%s", kWxPhases[g_wxPhase].name, self ? " (by itself on from it)" : "");
        }
    }
    int kind = picked > 0 || self ? g_wxPhase : -1;
    if (self) {
        g_wxPhaseLeft -= dt;
        if (g_wxPhaseLeft <= 0) {
            if (g_wxPhaseLeft > -1e8) g_wxPhase = WxNextKind(g_wxPhase);
            g_wxPhaseLeft = 60.0 * g_p[P_WX + 4].v * kWxPhases[g_wxPhase].minutes * (0.6 + 0.8 * WxRnd());
            Log("weather: now %s (for %.1f min; our clock %.1f h)", kWxPhases[g_wxPhase].name, g_wxPhaseLeft / 60, WxHourNow());
        }
        kind = g_wxPhase;
        g_p[P_WX_KIND].v = (float)(kind + 1); g_wxShown = kind + 1;   // (the menu shows where it goes)
    }
    float kTo[8] = { 1, 1, 1, 1, 1, 1, 0, 0 };
    if (kind >= 0) {
        const WxPhase& w = kWxPhases[kind];
        g_wxCloudTo = w.cloud; g_wxRainTo = w.rain;
        float t[8] = { w.drop, w.fall, w.wind, w.gust, w.haze, w.dawn, w.fog, w.cirrus };
        memcpy(kTo, t, sizeof(t));
    } else {
        g_wxRainTo = g_p[P_WX + 1].v;
        g_wxCloudTo = g_wxRainTo > 0.01f ? fmaxf(g_p[P_WX + 2].v, 0.84f + 0.16f * g_wxRainTo) : g_p[P_WX + 2].v;
    }
    // clouds first; rain only under clouds thick enough (it comes as they close in, and stops before they open); the
    // change over WeatherChange minutes (~95% by then: 3 time constants; the rain twice as quick)
    float tc = fmaxf(g_p[P_WX_CHANGE].v, 0.05f) * 60 / 3;
    g_wxCloud += (g_wxCloudTo - g_wxCloud) * (1 - expf(-dt / tc));
    for (int j = 0; j < 8; ++j) g_wxK[j] += (kTo[j] - g_wxK[j]) * (1 - expf(-dt / tc));
    float rainCap = WxSmooth(0.7f, 0.84f, g_wxCloud);
    float rt = fminf(g_wxRainTo, rainCap);
    g_wxRain += (rt - g_wxRain) * (1 - expf(-dt / fmaxf(tc * 0.5f, 5.0f)));
    if (g_wxRain < 0.002f && rt <= 0) g_wxRain = 0;
    // the ground soaks (exponentially, faster in hard rain), dries at a steady pace; the puddles fill and drain
    float soak = fmaxf(g_p[P_WX + 5].v, 0.1f) * 60, dry = fmaxf(g_p[P_WX + 6].v, 0.1f) * 60, fill = fmaxf(g_p[P_WX + 7].v, 0.1f) * 60;
    if (g_wxRain > 0.02f) {
        g_wxWet += (1 - g_wxWet) * (1 - expf(-dt * 3 * (0.4f + 0.6f * g_wxRain) / soak));
        g_wxPuddle += dt * g_wxRain / fill * 1.2f;
    } else {
        g_wxWet -= dt / dry;
        g_wxPuddle -= dt / (dry * 1.5f);
    }
    g_wxWet = g_wxWet < 0 ? 0 : g_wxWet > 1 ? 1 : g_wxWet;
    g_wxPuddle = g_wxPuddle < 0 ? 0 : g_wxPuddle > 1 ? 1 : g_wxPuddle;
    if (g_qbForceRain) { g_wxRain = 0.9f; g_wxCloud = 1; g_wxWet = 1; g_wxPuddle = 1; }   // 1.96: the quick bench's rain (bench.inc)
    // what it drives
    g_p[65].v = g_wxWet;                                           // wet ground
    g_p[112].v = 0.85f * WxSmooth(0.05f, 0.6f, g_wxPuddle);         // puddles (small mirrors)
    g_p[184].v = WxSmooth(0.0f, 0.8f, g_wxPuddle);                  // their water
    g_p[181].v = 0.8f * WxSmooth(0.0f, 1.0f, g_wxPuddle);           // the big ones' water
    g_p[16].v = g_wxBase[4] * g_wxK[4] * (1 + 1.2f * g_wxRain) + 3.0f * fmaxf(g_p[P_WX + 8].v, g_wxK[6]);   // haze (fog: stage 4)
    // 1.89: what the kind brings (x the values set by hand; in the settings' ranges)
    auto clampP = [](int i, float v) { g_p[i].v = v < g_p[i].mn ? g_p[i].mn : v > g_p[i].mx ? g_p[i].mx : v; };
    clampP(219, g_wxBase[10] * g_wxK[0]);   // drop size
    clampP(226, g_wxBase[13] * g_wxK[1]);   // their fall
    clampP(222, g_wxBase[11] * g_wxK[2]);   // wind
    clampP(224, g_wxBase[12] * g_wxK[3]);   // gusts
    clampP(149, g_wxBase[14] * g_wxK[5]);   // the dawn's haze
    float sky[5];
    WxSky(g_wxCloud, sky);
    sky[2] = fminf(sky[2] + g_wxK[7], 1.0f);   // cirrus
    g_p[93].v = sky[0]; g_p[100].v = sky[1]; g_p[101].v = sky[2]; g_p[102].v = sky[3]; g_p[99].v = sky[4];
    if (g_frame - g_wxLogT >= 1800) {
        g_wxLogT = g_frame;
        Log("weather: rain %.2f (to %.2f), clouds %.2f (to %.2f), wet ground %.2f, puddles %.2f%s%s%s", g_wxRain, g_wxRainTo, g_wxCloud, g_wxCloudTo,
            g_wxWet, g_wxPuddle, kind >= 0 ? ", going to: " : "", kind >= 0 ? kWxPhases[kind].name : "", self ? " (by itself)" : "");
    }
}
