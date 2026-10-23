// ---------------------------------------------------------------- the sky
// The game's sky (NFSLight_sky.txt, shader "BaseCloudSampler..4 | BaseCloudColour", drawn first in the lighting run as
// a dome, 1080 vertices): sky = the key light's glow (pow(dot(v, KeyLightDir), SunColour.w) x SunColour) + a height
// gradient (GradientTextureA / B at v4.z, mixed by v4.w) x BaseCloudColour.w; then 4 cloud layers (256x64 strips at
// v2.xy, v2.zw, v3.xy, v3.zw): each over it by saturate((alpha - CloudParamsPS[i]) x CloudParamsPS2.y), coloured
// texture x BaseCloudColour.rgb. At night the game sets the thresholds to 0.455, 1, 1, 1 (3 layers off) and the
// cloud colour to ~0.02: a bare sky.
// v1 (TEXCOORD0) is the dome vertex in the dome's own space (its VS: o1.xyz = POSITION). The dome's world matrix
// (VS gObjectCB cb1[11..14]) is a uniform scale (400) at the camera: normalize(v1) is the view direction. Each frame
// the VS constants cb1[7..14] (viewProjection rows, world rows) are copied on the GPU into a buffer (no CPU wait): the
// cloud pass gets this frame's camera (world row 3) and last frame's viewProjection (to reproject its history).
// Cost: the march runs at an eighth of the size (one quarter-size pixel of each 2x2 a frame, in turn: the viewport is
// shifted by the pixel's place), a resolve pass builds the quarter-size picture from it and the last one
// (reprojected); (a branch per pixel would not help: the GPU runs pixels in groups of 32). The sun pass reads a
// 256^2 cloud-shadow map around the camera made once a frame.
//
// Ours (in its place, the same inputs): the game's sky and layers, the city's glow at the horizon, faint stars, and
// clouds after "The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn" (A. Schneider, SIGGRAPH 2015 / Nubis 2017):
// - a weather map over the world (2D, tiling): coverage, cloud type, cirrus;
// - per type a height profile in the cloud layer (1500..5000 m): stratus low and flat, cumulus flat-bottomed and
//   round, cumulonimbus tall;
// - shape: a Perlin-Worley 3D noise remapped by Worley fbm, cut by the coverage (remap(shape, 1 - cov, 1, 0, 1) x cov),
//   its edges eroded by a finer Worley fbm (wispy at the bottom, billowy at the top);
// - light: Beer's law over 5 samples toward the key light, the "powder" darkening of thick parts, a two-lobe
//   Henyey-Greenstein phase (silver lining toward the sun), the sky above / the ground (the city at night) below;
//   rain clouds absorb more (dark bellies);
// - a thin cirrus layer at 8 km (stretched fibres);
// - the march: big steps through empty air, small ones in cloud (back one step on entry), jittered each frame.
// The same coverage and shape shade the ground in the sun pass (CloudShadow): moving shadows, overcast days.
// Noises are made at start (tiling): weather 512^2 RGBA, shape 64^3 RGBA, detail 32^3 RGBA.

static const char* kCloudCommon = R"HLSL(
// ---- our clouds (sky.inc): shared by the sky draw and the sun pass's cloud shadows
cbuffer SkyCB : register(b11) { float4 SK0, SK1, SK2, SK3, SK4, SK5, SK6, SK7, SK8, SK9, SK10, SK11; };
// SK10 (1.29): our sun (SunPath): towards it (the light's direction); w: by day 1 (0: the game's key light: the moon)
Texture2D<float> tCloudShadow : register(t50);   // the cloud-shadow map (transmittance)
// SK0: game clouds more, cover, cloud brightness, time (s)      SK1: city glow colour, strength
// SK2: stars, wind, cloud size, on      SK3: camera, cloud shadows      SK4: cloud type, cirrus, rain, quality
// SK5: 1 / the sky target's size, the cloud pass is there, frame & 3
// SK6: the shadow map: origin x, z, ground height, texel (m; 0: none)
// SK7: history usable, this frame's pixel of 2x2 (x, y), 1 / the march target's width x 2; SK8.x: its aspect
Texture2D tCW : register(t47);   // weather: r coverage, g type, b cirrus, a fine
Texture3D tCS : register(t48);   // shape: r Perlin-Worley, gba Worley fbm octaves
Texture3D tCD : register(t49);   // detail: rgb Worley octaves
static const float CL0 = 1500, CL1 = 5000;
static float CLD = 1;   // how much fine detail (1 near .. 0 far: a pixel there covers more than the detail; it would sparkle)
float Remap(float v, float l0, float h0, float l1, float h1) { return l1 + (v - l0) * (h1 - l1) / max(h0 - l0, 1e-4); }
float3 CloudWind() { return float3(9, 0, 3.5) * SK0.w * SK2.y; }
// the weather where we are: coverage, type
float2 CloudWeather(float2 xz, SamplerState s)
{
    float4 w = tCW.SampleLevel(s, (xz + CloudWind().xz * 0.5) / (22000 * SK2.z), 0);
    float cov = saturate(SK0.y * 1.35 - 0.2 + (w.r - 0.5) * 1.1);
    if (SK0.y >= 0.98) cov = 1;
    float type = saturate(SK4.x + (w.g - 0.5) * 0.5);
    return float2(cov, type);
}
// the height profile of a cloud type (0 stratus, 0.5 cumulus, 1 cumulonimbus) at h (0..1 of the layer)
float CloudHeight(float h, float type)
{
    float st = smoothstep(0.0, 0.03, h) * (1 - smoothstep(0.06, 0.14, h));
    float cu = smoothstep(0.0, 0.04, h) * (1 - smoothstep(0.25, 0.55, h));
    float cb = smoothstep(0.0, 0.08, h) * (1 - smoothstep(0.6, 0.95, h));
    return type < 0.5 ? lerp(st, cu, type * 2) : lerp(cu, cb, type * 2 - 1);
}
// density (0..1); cheap: without the detail erosion
float CloudDensity(float3 P, bool cheap, SamplerState s)
{
    float h = (P.y - CL0) / (CL1 - CL0);
    if (h <= 0 || h >= 1) return 0;
    float2 wt = CloudWeather(P.xz, s);
    if (wt.x <= 0.001) return 0;
    float grad = CloudHeight(h / (0.35 + 0.65 * wt.x), wt.y);   // tall where the cover is dense, low little ones where thin
    if (grad <= 0.001) return 0;
    float3 q = (P + CloudWind()) / (2400 * SK2.z);
    float4 n = tCS.SampleLevel(s, q, 0);
    float wf = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
    float shape = saturate(Remap(n.r, wf - 1, 1, 0, 1)) * grad;
    float c = saturate(Remap(shape, 1 - wt.x, 1, 0, 1)) * wt.x;
    if (cheap || c <= 0 || CLD <= 0.01) return c * 5;
    float3 sw = (n.gba - 0.5) * 140 * (1 - h);   // swirl: the detail pushed about by the shape noise (curl-like)
    float3 dn = tCD.SampleLevel(s, (P + sw + CloudWind() * 1.6) / (330 * SK2.z), 0).rgb;
    float df = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
    float mod = lerp(df, 1 - df, saturate(h * 8));   // wispy at the bottom, billowy higher up
    return saturate(Remap(c, mod * 0.38 * CLD, 1, 0, 1)) * 5;   // x 5: the density after the cuts is small
}
// how much of the key light our clouds let through at a ground point
float CloudShadowCalc(float3 world, float3 toSun, SamplerState s)
{
    if (SK3.w <= 0 || SK0.y <= 0 || toSun.y < 0.03) return 1;
    float od = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        float y = lerp(CL0 + 150, CL0 + 2200, (k + 0.5) / 4);
        od += CloudDensity(world + toSun * ((y - world.y) / toSun.y), true, s);
    }
    float t = exp(-od * 0.7 * (1 + SK4.z));
    return lerp(1, t, SK3.w);
}
// the sun pass: from the map (where the ray to the light from this point meets the map's ground height)
float CloudShadow(float3 world, float3 toSun, SamplerState s)
{
    if (SK3.w <= 0 || SK0.y <= 0 || toSun.y < 0.03 || SK6.w <= 0) return 1;
    float2 g = world.xz + toSun.xz * ((SK6.z - world.y) / toSun.y);
    float2 uv = (g - SK6.xy) / (SK6.w * 256);
    if (any(uv < 0.01) || any(uv > 0.99)) return 1;
    return tCloudShadow.SampleLevel(s, uv, 0);
}
)HLSL";

static const char* kSkyMain = R"HLSL(
cbuffer gObjectCB : register(b1) { float4 BaseCloudColour, CloudParamsPS, CloudParamsPS2, KeyLightDir, SunColour; };
Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);
Texture1D tGA : register(t5); Texture1D tGB : register(t6);
Texture2D tClouds : register(t10);   // the cloud pass (a quarter of the size): rgb light, a transmittance
Buffer<float4> tVP : register(t11);   // this frame: viewProjection rows 0..3, the dome's world rows 4..7 (7: the camera)
Buffer<float4> tVPPrev : register(t12);   // the last frame's
Texture2D tHist : register(t13);      // the cloud pass's last result
Texture2D tMarch : register(t14);     // this frame's march (an eighth of the size)
Texture2D<float> tSceneDepth : register(t15);   // the game's depth (this frame's scene is drawn before the sky)
SamplerState sP : register(s9);
SamplerState s0 : register(s0); SamplerState s1 : register(s1); SamplerState s2 : register(s2); SamplerState s3 : register(s3);
SamplerState s5 : register(s5); SamplerState s6 : register(s6); SamplerState sW : register(s7); SamplerState sL : register(s8);
struct PSIn { float4 pos : SV_Position; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };

float Lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float Hash(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float HG(float c, float g) { float g2 = g * g; return (1 - g2) / pow(max(1 + g2 - 2 * g * c, 1e-4), 1.5); }   // x 4 pi

struct SkyIn { float3 d, L, grad, city, sunC, ambTop, ambBot; float day, night, cosT, phase; };
// the sky's sun (its glow, the clouds' light): the game's (KeyLightDir); by day with our sun on, ours (1.30: where it is,
// from the horizon up: the game's own never comes below ~28 deg)
float3 SkyKeyDir()
{
    float3 g = normalize(KeyLightDir.xyz);
    if (SK10.w <= 0) return g;
    return normalize(lerp(g, normalize(SK10.xyz), SK10.w));
}
SkyIn Setup(PSIn i)
{
    SkyIn s;
    s.d = normalize(i.t0.xyz);   // the dome: a sphere around the camera
    s.L = SkyKeyDir();
    s.grad = tGA.Sample(s5, i.t3.z).rgb * i.t3.w + tGB.Sample(s6, i.t3.z).rgb * (1 - i.t3.w);
    s.day = saturate((Lum(SunColour.rgb) - 0.03) / 0.3);   // the key light: the sun by day, the moon at night
    // 1.34: our night (SK9.y: our sun under the horizon, 0..1 by 8 deg): the game's sky (its twilight lasts ~3 h of our
    // clock past its sun) darker and cooler, no sun on the clouds; the city's glow and the stars come
    float on = SK9.y;
    s.grad = lerp(s.grad, Lum(s.grad) * float3(0.55, 0.65, 1.0) * 0.22, on);
    s.day = min(s.day, 1 - on);
    s.night = 1 - s.day;
    s.city = SK1.rgb * SK1.w * s.night;
    s.cosT = dot(s.d, s.L);
    s.phase = lerp(HG(s.cosT, 0.75), HG(s.cosT, -0.25), 0.35) * 0.55 + 0.25;
    s.sunC = SunColour.rgb * (0.9 * s.day + 0.25 * s.night) * (1 - 0.9 * on);   // the moon: a little (our night: none of the game's twilight sun)
    float rain = SK4.z;
    s.ambTop = s.grad * BaseCloudColour.w * 1.1 * (1 - rain * 0.35) + s.city * 0.3;
    s.ambBot = s.grad * BaseCloudColour.w * 0.5 * (1 - rain * 0.6) + s.city * 1.4;
    return s;
}

// ---- the cloud pass: cirrus + the volumetric clouds, out: light (rgb), transmittance (a)
float4 clouds(PSIn i) : SV_Target
{
    // this frame's quarter-size pixel of the 2x2: a quarter of a march pixel off its centre (the viewport cannot move
    // by a fraction at feature level 10): the dome point moved along its screen derivatives
    float2 sh = SK7.yz * 0.5 - 0.25;
    i.t0.xyz += ddx(i.t0.xyz) * sh.x + ddy(i.t0.xyz) * sh.y;
    // all of this cloud pixel behind the city (the sky has depth 1): nothing to march
    if (SK9.x > 0.5) {
        float2 c = (floor(i.pos.xy) * 2 + SK7.yz + 0.5) * SK8.zw, e = SK8.zw * 0.45;
        float z = max(max(tSceneDepth.SampleLevel(sP, c + float2(-e.x, -e.y), 0), tSceneDepth.SampleLevel(sP, c + float2(e.x, -e.y), 0)),
                      max(tSceneDepth.SampleLevel(sP, c + float2(-e.x, e.y), 0), tSceneDepth.SampleLevel(sP, c + float2(e.x, e.y), 0)));
        z = max(z, tSceneDepth.SampleLevel(sP, c, 0));
        if (z < 0.99999) return float4(0, 0, 0, 1);
    }
    SkyIn s = Setup(i);
    float3 d = s.d, L = s.L, cam = tVP.Load(7).xyz;   // the camera now (the dome's centre)
    float el = d.y;
    float3 col = 0;
    float Tall = 1;
    float jit = frac(Hash((floor(i.pos.xy) * 2 + SK7.yz) * 0.913 + 7.1) + SK8.y * 0.618034);   // a new start each visit (averaged by the resolve)
    // cirrus: a thin layer at 8 km, fibres along the wind
    if (SK4.y > 0 && el > 0.01) {
        float t = (8000 - cam.y) / el;
        float2 p = cam.xz + d.xz * t + CloudWind().xz * 2;
        float2 u = float2(p.x * 0.8 + p.y * 0.6, -p.x * 0.6 + p.y * 0.8) / (14000 * SK2.z);
        float n = tCW.SampleLevel(sW, u * float2(0.35, 2.2), 0).b * 0.85 + tCW.SampleLevel(sW, u * float2(0.9, 4), 0).a * 0.15;
        float a = saturate((n - 0.62 + SK4.y * 0.35) * 3) * SK4.y * smoothstep(0.01, 0.12, el) * exp(-t / 90000) * 0.75;
        col = (s.sunC * (0.35 + s.phase * 0.5) + s.ambTop * 1.2) * a;
        Tall = 1 - a;
    }
    // the volumetric clouds (in front of the cirrus)
    if (SK0.y > 0 && el > 0.003 && cam.y < CL0) {
        float t0 = (CL0 - cam.y) / el, t1 = min((CL1 - cam.y) / el, t0 + 30000);
        CLD = saturate(1.6 - t0 / 9000);   // full detail to ~5 km, none past ~14 km
        float q = SK4.w;
        float big = lerp(300, 150, q), small = lerp(80, 40, q);
        int maxIt = (int)lerp(72, 160, q);
        float sigma = 0.012 * (1 + SK4.z * 1.2);
        float t = t0 + big * jit;
        float T = 1;
        float3 vc = 0;
        int empty = 0;
        bool inCloud = false;
        [loop] for (int k = 0; k < maxIt && t < t1; ++k) {
            float3 P = cam + d * t;
            if (!inCloud) {
                if (CloudDensity(P, true, sW) > 0) { inCloud = true; t = max(t - big, t0); empty = 0; continue; }   // back one step
                t += big;
                continue;
            }
            float den = CloudDensity(P, false, sW);
            if (den > 0.003) {
                empty = 0;
                // toward the key light: 5 samples, further and further
                float od = 0, seg = 0;
                [unroll] for (int j = 1; j <= 5; ++j) {
                    float dist = 40 * j * j;
                    od += CloudDensity(P + L * dist, j > 3, sW) * (dist - seg);
                    seg = dist;
                }
                od *= sigma;
                // light scattered many times: octaves, each weaker, reaching deeper, less forward (a = b = c = 0.5)
                float ms = 0, aa = 1, bb = 1, cc = 1;
                [unroll] for (int o = 0; o < 3; ++o) {
                    ms += aa * exp(-od * bb) * lerp(lerp(HG(s.cosT, 0.75 * cc), HG(s.cosT, -0.25 * cc), 0.35) * 0.55 + 0.25, 1, 1 - cc);
                    aa *= 0.5; bb *= 0.5; cc *= 0.5;
                }
                float powder = 1 - exp(-den * small * sigma * 2.5);
                float pw = lerp(1, powder, saturate(0.8 - s.cosT * 0.6));    // dark edges of thick parts, not against the sun
                // the sky's light: less inside and under a lot of cloud (one look up)
                float up = CloudDensity(P + float3(0, 280, 0), true, sW);
                float ambV = exp(-up * 280 * sigma * 0.5);
                float h = saturate((P.y - CL0) / (CL1 - CL0) * 3);
                float3 Li = s.sunC * ms * pw * 2.2 + lerp(s.ambBot, s.ambTop, h) * (0.6 + 0.4 * ambV);
                float a = 1 - exp(-sigma * den * small);
                vc += T * a * Li;
                T *= 1 - a;
                if (T < 0.01) break;
            } else if (++empty > 6) inCloud = false;   // out of it: big steps again
            t += small;
        }
        float fade = exp(-t0 / 60000) * smoothstep(0.003, 0.05, el);
        T = lerp(1, T, fade);
        col = col * T + vc * fade;
        Tall *= T;
    }
    return float4(col * SK0.z, Tall);
}

// ---- the resolve (quarter size): this pixel's turn -> the march; else the last picture where this direction was
// Catmull-Rom from 9 bilinear taps (sharp: resampling the history every frame must not blur it)
float4 SampleCR(Texture2D t, float2 uv)
{
    float2 size;
    t.GetDimensions(size.x, size.y);
    float2 sp = uv * size, t1 = floor(sp - 0.5) + 0.5, f = sp - t1;
    float2 w0 = f * (-0.5 + f * (1 - 0.5 * f)), w1 = 1 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, t12 = (t1 + w2 / w12) / size, t0 = (t1 - 1) / size, t3 = (t1 + 2) / size;
    float4 r = 0;
    r += t.SampleLevel(sL, float2(t0.x, t0.y), 0) * w0.x * w0.y + t.SampleLevel(sL, float2(t12.x, t0.y), 0) * w12.x * w0.y + t.SampleLevel(sL, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    r += t.SampleLevel(sL, float2(t0.x, t12.y), 0) * w0.x * w12.y + t.SampleLevel(sL, float2(t12.x, t12.y), 0) * w12.x * w12.y + t.SampleLevel(sL, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    r += t.SampleLevel(sL, float2(t0.x, t3.y), 0) * w0.x * w3.y + t.SampleLevel(sL, float2(t12.x, t3.y), 0) * w12.x * w3.y + t.SampleLevel(sL, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    return max(r, 0);
}

// the resolve (quarter size): the last picture where this direction was (sharp), held within what this frame's march
// shows around (no ghosts, no flicker), and this pixel's own new march mixed in on its turn
float4 resolve(PSIn i) : SV_Target
{
    int2 ip = int2(i.pos.xy), mp = ip >> 1;
    bool mine = all((ip & 1) == int2(SK7.yz));
    float4 m = tMarch.Load(int3(mp, 0));
    float4 lo = m, hi = m;
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
        float4 n = tMarch.Load(int3(mp + int2(x, y), 0));
        lo = min(lo, n); hi = max(hi, n);
    }
    float4 cur = mine ? m : tMarch.SampleLevel(sL, (i.pos.xy - SK7.yz + 0.5) * SK7.w * float2(1, SK8.x), 0);
    float3 d = normalize(i.t0.xyz);
    if (SK7.x > 0.5) {
        float3 P = tVPPrev.Load(7).xyz + d * 400;
        float4 c = P.x * tVPPrev.Load(0) + P.y * tVPPrev.Load(1) + P.z * tVPPrev.Load(2) + tVPPrev.Load(3);
        float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (c.w > 0 && all(uv > 0.002) && all(uv < 0.998)) {
            float4 h = SampleCR(tHist, uv);
            float4 pad = (hi - lo) * 0.25 + float4(0.002, 0.002, 0.002, 0.02);
            h = clamp(h, lo - pad, hi + pad);
            return mine ? lerp(h, m, 0.35) : h;
        }
    }
    return cur;   // nothing to reproject: this frame's
}

// ---- the cloud-shadow map (256^2 around the camera): transmittance of the key light at the map's ground height
float shadowmap(PSIn i) : SV_Target
{
    float3 L = SK10.w > 0 ? normalize(lerp(normalize(KeyLightDir.xyz), SK10.xyz, SK10.w)) : normalize(KeyLightDir.xyz);   // (our sun: its light's direction)
    float2 g = SK6.xy + i.pos.xy * SK6.w;
    return CloudShadowCalc(float3(g.x, SK6.z, g.y), L, sW);
}

// ---- the sky draw
float4 main(PSIn i) : SV_Target
{
    SkyIn s = Setup(i);
    float3 d = s.d;
    float el = d.y;
    // the game's sky (its glow in the true direction)
    float3 sky = pow(saturate(dot(d, s.L)), SunColour.w) * SunColour.rgb * (1 - SK9.y) + s.grad * BaseCloudColour.w;
    float ov = saturate((SK0.y - 0.55) / 0.45) * saturate(SK0.y * 2);   // heavy cover: the light comes through grey
    sky = lerp(sky, Lum(sky) * float3(0.92, 0.95, 1.0) * (0.9 - SK4.z * 0.35), ov * 0.8);
    sky += s.city * exp(-max(el, 0) * 7) * 0.6;
    // 1.35: our sun's disc (SK9.z its strength, w warmth; towards SK10) and the moon's (SK11): the game's sprite is not drawn
    if (SK9.z > 0) {
        float c = dot(d, normalize(SK10.xyz));
        sky += lerp(float3(1, 0.96, 0.88), float3(1, 0.42, 0.1), SK9.w) * SK9.z * (smoothstep(0.9999692, 0.9999813, c) * 30 + pow(saturate(c), 3000) * 1.5);
    }
    if (SK11.w > 0) {
        float c = dot(d, SK11.xyz);
        sky += float3(0.8, 0.86, 1) * SK11.w * (smoothstep(0.9999692, 0.9999813, c) + pow(saturate(c), 4000) * 0.15);
    }
    // stars: high up, at night
    if (SK2.x > 0 && s.night > 0 && el > 0.05) {
        float2 sp = float2(atan2(d.z, d.x) * 300, asin(saturate(el)) * 300);
        float2 cell = floor(sp);
        float h = Hash(cell);
        if (h > 0.996) {
            float2 c = frac(sp) - 0.5 - (float2(Hash(cell + 3.1), Hash(cell + 7.7)) - 0.5) * 0.6;
            float tw = 0.7 + 0.3 * sin(SK0.w * (2 + h * 9) + h * 50);
            sky += exp(-dot(c, c) * 30) * (h - 0.996) * SK2.x * s.night * tw * smoothstep(0.05, 0.4, el);
        }
    }
    // the game's cloud layers, more of them
    float3 cc = BaseCloudColour.rgb * SK0.z;
    cc = lerp(cc, Lum(cc) * float3(0.55, 0.65, 1.0) * 0.25, SK9.y) + s.city * 0.8;   // (our night: the game's twilight clouds cooled, darker)
    float sharp = CloudParamsPS2.y;
    float4 thr = CloudParamsPS - SK0.x * 0.6;
    float4 c;
    c = t0.Sample(s0, i.t1.xy); sky = lerp(sky, c.rgb * cc, saturate((c.a - thr.x) * sharp));
    c = t1.Sample(s1, i.t1.zw); sky = lerp(sky, c.rgb * cc, saturate((c.a - thr.y) * sharp));
    c = t2.Sample(s2, i.t2.xy); sky = lerp(sky, c.rgb * cc, saturate((c.a - thr.z) * sharp));
    c = t3.Sample(s3, i.t2.zw); sky = lerp(sky, c.rgb * cc, saturate((c.a - thr.w) * sharp));
    // our clouds, from the cloud pass (a quarter of the size: sharp upsampling)
    if (SK5.z > 0.5) {
        float4 cl = SampleCR(tClouds, i.pos.xy * SK5.xy);
        sky = sky * saturate(cl.a) + cl.rgb;
    }
    return float4(sky, 1);
}
)HLSL";

static const char* CloudCommonSrc() { return kCloudCommon; }

static ID3D11PixelShader *g_skyPS, *g_cloudPS;
static ID3D11Texture2D* g_cloudTex;
static ID3D11RenderTargetView* g_cloudRtv;
static ID3D11ShaderResourceView* g_cloudSrv;
static UINT g_cloudW, g_cloudH;
static bool g_csOk;   // the shadow map was made (this or a recent frame)
static ID3D11PixelShader* g_csPS;          // the cloud-shadow map
static ID3D11PixelShader* g_resolvePS;     // the cloud picture from the march + the last one
static ID3D11Texture2D* g_cmTex;           // the march (an eighth of the size)
static ID3D11RenderTargetView* g_cmRtv;
static ID3D11ShaderResourceView* g_cmSrv;
static UINT g_cmW, g_cmH;
static bool g_skyDepthOk;   // the depth is bound for the march
static ID3D11Texture2D* g_csTex;
static ID3D11RenderTargetView* g_csRtv;
static ID3D11ShaderResourceView* g_csSrv;
static ID3D11Texture2D* g_cloudTex2;         // the cloud pass's other target (history)
static ID3D11RenderTargetView* g_cloudRtv2;
static ID3D11ShaderResourceView* g_cloudSrv2;
static ID3D11Buffer* g_vpBuf[2];              // the sky VS's viewProjection + world, this frame / the last
static ID3D11ShaderResourceView* g_vpSrv[2];
static int g_cloudCur;                        // which target is this frame's
static bool g_cloudHist;                      // the other holds a usable result
static float g_csOrigin[3];
static const float kCsTexel = 25;             // m a texel of the shadow map (256: 6.4 km)
static ID3D11Buffer* g_skyCb;
static ID3D11ShaderResourceView *g_skyW, *g_skyS, *g_skyD;
static ID3D11PixelShader* g_skyGame[8];   // the game's sky shaders of this kind
static volatile LONG g_skyGameN;

// at creation: the main sky shader (4 cloud layers, no secondary cloud layer)
static void SkyNoteGame(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name)
{
    if (!strstr(name, "BaseCloudSampler4") || !g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    D3D11_SHADER_INPUT_BIND_DESC bd;
    bool sec = SUCCEEDED(r->GetResourceBindingDescByName("SecondaryCloudSampler", &bd));
    bool ga = SUCCEEDED(r->GetResourceBindingDescByName("GradientTextureA", &bd)) && bd.BindPoint == 5;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    r->Release();
    if (sec || !ga || sd.InputParameters != 5) return;
    LONG k = InterlockedIncrement(&g_skyGameN) - 1;
    if (k < 8) { g_skyGame[k] = ps; Log("sky: the game's sky shader %p (replaced by ours)", ps); } else g_skyGameN = 8;
}

// ---- tiling noises
static unsigned SkyH(int x, int y, int z) { unsigned h = (unsigned)(x * 73856093 ^ y * 19349663 ^ z * 83492791); h ^= h >> 13; h *= 0x5bd1e995; h ^= h >> 15; return h; }
static float SkyR(int x, int y, int z, int s) { return (SkyH(x + s * 101, y + s * 37, z + s * 59) & 0xffff) / 65535.0f; }
static int Wrap(int a, int n) { return ((a % n) + n) % n; }
// Perlin gradient noise, period n
static float SkyPerlin(float x, float y, float z, int n)
{
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float fx = x - ix, fy = y - iy, fz = z - iz;
    auto g = [&](int a, int b, int c, float dx, float dy, float dz) {
        unsigned h = SkyH(Wrap(a, n), Wrap(b, n), Wrap(c, n)) % 12;
        static const float G[12][3] = { {1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},{1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},{0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1} };
        return G[h][0] * dx + G[h][1] * dy + G[h][2] * dz;
    };
    auto f = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    float u = f(fx), v = f(fy), w = f(fz);
    float x00 = g(ix, iy, iz, fx, fy, fz) + u * (g(ix + 1, iy, iz, fx - 1, fy, fz) - g(ix, iy, iz, fx, fy, fz));
    float x10 = g(ix, iy + 1, iz, fx, fy - 1, fz) + u * (g(ix + 1, iy + 1, iz, fx - 1, fy - 1, fz) - g(ix, iy + 1, iz, fx, fy - 1, fz));
    float x01 = g(ix, iy, iz + 1, fx, fy, fz - 1) + u * (g(ix + 1, iy, iz + 1, fx - 1, fy, fz - 1) - g(ix, iy, iz + 1, fx, fy, fz - 1));
    float x11 = g(ix, iy + 1, iz + 1, fx, fy - 1, fz - 1) + u * (g(ix + 1, iy + 1, iz + 1, fx - 1, fy - 1, fz - 1) - g(ix, iy + 1, iz + 1, fx, fy - 1, fz - 1));
    float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);   // about -1..1
}
// Worley: 1 - distance to the nearest point (one a cell, n cells, tiling), 0..1
static float SkyWorley(float x, float y, float z, int n, int seed)
{
    int ix = (int)floorf(x), iy = (int)floorf(y), iz = (int)floorf(z);
    float best = 9;
    for (int c = -1; c <= 1; ++c) for (int b = -1; b <= 1; ++b) for (int a = -1; a <= 1; ++a) {
        int X = ix + a, Y = iy + b, Z = iz + c;
        int wx = Wrap(X, n), wy = Wrap(Y, n), wz = Wrap(Z, n);
        float px = X + SkyR(wx, wy, wz, seed), py = Y + SkyR(wx, wy, wz, seed + 1), pz = Z + SkyR(wx, wy, wz, seed + 2);
        float d2 = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
        if (d2 < best) best = d2;
    }
    return 1 - fminf(sqrtf(best), 1.0f);
}
static float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static ID3D11ShaderResourceView* SkyTex3D(const unsigned char* px, int n)
{
    D3D11_TEXTURE3D_DESC d = {};
    d.Width = d.Height = d.Depth = n; d.MipLevels = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA s = { px, (UINT)n * 4, (UINT)(n * n * 4) };
    ID3D11Texture3D* t = 0;
    ID3D11ShaderResourceView* v = 0;
    if (SUCCEEDED(g_dev->CreateTexture3D(&d, &s, &t))) { g_dev->CreateShaderResourceView(t, 0, &v); t->Release(); }
    return v;
}
static void SkyNoises()
{
    DWORD t0 = GetTickCount();
    // weather 512^2: r coverage (fbm, 4 cells), g type (2 cells), b cirrus (fbm, 8 cells), a fine (32 cells)
    const int W = 512;
    unsigned char* w = (unsigned char*)malloc(W * W * 4);
    for (int y = 0; y < W; ++y) for (int x = 0; x < W; ++x) {
        float fx = (float)x / W, fy = (float)y / W, r = 0, b = 0, a = 0.5f;
        for (int o = 0; o < 5; ++o) { int n = 4 << o; r += a * SkyPerlin(fx * n, fy * n, 0.5f, n); a *= 0.5f; }
        a = 0.5f;
        for (int o = 0; o < 6; ++o) { int n = 8 << o; b += a * SkyPerlin(fx * n, fy * n, 3.5f, n); a *= 0.5f; }
        float g = SkyPerlin(fx * 2, fy * 2, 7.5f, 2) + 0.5f * SkyPerlin(fx * 4, fy * 4, 7.5f, 4);
        float fine = SkyPerlin(fx * 32, fy * 32, 9.5f, 32) + 0.5f * SkyPerlin(fx * 64, fy * 64, 9.5f, 64);
        unsigned char* p = w + (y * W + x) * 4;
        p[0] = (unsigned char)(Sat(r * 0.9f + 0.5f) * 255); p[1] = (unsigned char)(Sat(g * 0.7f + 0.5f) * 255);
        p[2] = (unsigned char)(Sat(b * 0.9f + 0.5f) * 255); p[3] = (unsigned char)(Sat(fine * 0.7f + 0.5f) * 255);
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = W; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = { w, W * 4 };
    ID3D11Texture2D* t2 = 0;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &t2))) { g_dev->CreateShaderResourceView(t2, 0, &g_skyW); t2->Release(); }
    free(w);
    // shape 64^3: r Perlin-Worley (Perlin fbm 4 cells, dilated by Worley fbm), g b a Worley 4 / 8 / 16 cells
    const int S = 64;
    unsigned char* s = (unsigned char*)malloc(S * S * S * 4);
    for (int z = 0; z < S; ++z) for (int y = 0; y < S; ++y) for (int x = 0; x < S; ++x) {
        float fx = (x + 0.5f) / S, fy = (y + 0.5f) / S, fz = (z + 0.5f) / S;
        float p = 0, a = 0.5f;
        for (int o = 0; o < 3; ++o) { int n = 4 << o; p += a * SkyPerlin(fx * n, fy * n, fz * n, n); a *= 0.5f; }
        p = Sat(p * 0.8f + 0.5f);
        float w1 = SkyWorley(fx * 4, fy * 4, fz * 4, 4, 11), w2 = SkyWorley(fx * 8, fy * 8, fz * 8, 8, 21), w3 = SkyWorley(fx * 16, fy * 16, fz * 16, 16, 31);
        float wf = w1 * 0.625f + w2 * 0.25f + w3 * 0.125f;
        float pw = Sat((p - (wf - 1)) / (1 - (wf - 1)));   // remap(perlin, worley - 1, 1, 0, 1)
        pw = Sat((pw - 0.55f) * 3.0f);   // spread over 0..1 (else the coverage cut leaves nearly nothing)
        unsigned char* q = s + ((z * S + y) * S + x) * 4;
        q[0] = (unsigned char)(pw * 255); q[1] = (unsigned char)(w1 * 255); q[2] = (unsigned char)(w2 * 255); q[3] = (unsigned char)(w3 * 255);
    }
    g_skyS = SkyTex3D(s, S);
    free(s);
    // detail 32^3: Worley 2 / 4 / 8 cells
    const int D = 32;
    unsigned char* dd = (unsigned char*)malloc(D * D * D * 4);
    for (int z = 0; z < D; ++z) for (int y = 0; y < D; ++y) for (int x = 0; x < D; ++x) {
        float fx = (x + 0.5f) / D, fy = (y + 0.5f) / D, fz = (z + 0.5f) / D;
        unsigned char* q = dd + ((z * D + y) * D + x) * 4;
        q[0] = (unsigned char)(SkyWorley(fx * 2, fy * 2, fz * 2, 2, 41) * 255);
        q[1] = (unsigned char)(SkyWorley(fx * 4, fy * 4, fz * 4, 4, 51) * 255);
        q[2] = (unsigned char)(SkyWorley(fx * 8, fy * 8, fz * 8, 8, 61) * 255);
        q[3] = 255;
    }
    g_skyD = SkyTex3D(dd, D);
    free(dd);
    Log("sky: cloud noises made in %lu ms", GetTickCount() - t0);
}

static void SkyCompile()
{
    if (!g_compile || !g_dev) return;
    size_t n = strlen(kCloudCommon) + strlen(kSkyMain) + 1;
    char* src = (char*)malloc(n);
    strcpy(src, kCloudCommon);
    strcat(src, kSkyMain);
    static const char* entry[4] = { "main", "clouds", "shadowmap", "resolve" };
    ID3D11PixelShader** out[4] = { &g_skyPS, &g_cloudPS, &g_csPS, &g_resolvePS };
    for (int e = 0; e < 4; ++e) {
        ID3DBlob *code = 0, *err = 0;
        if (SUCCEEDED(g_compile(src, strlen(src), "sky", 0, 0, entry[e], "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), 0, out[e]);
        else Log("sky shader %s: %s", entry[e], err ? (const char*)err->GetBufferPointer() : "failed");
        if (code) code->Release();
        if (err) err->Release();
    }
    free(src);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, 0, &g_skyCb);
    for (int k = 0; k < 2; ++k) {
        D3D11_BUFFER_DESC vb = {};
        vb.ByteWidth = 128; vb.Usage = D3D11_USAGE_DEFAULT; vb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = 8;
        if (SUCCEEDED(g_dev->CreateBuffer(&vb, 0, &g_vpBuf[k]))) g_dev->CreateShaderResourceView(g_vpBuf[k], &sv, &g_vpSrv[k]);
    }
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 256; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16_FLOAT; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, 0, &g_csTex))) { g_dev->CreateRenderTargetView(g_csTex, 0, &g_csRtv); g_dev->CreateShaderResourceView(g_csTex, 0, &g_csSrv); }
    }
    SkyNoises();
}
static void SkyEnsure() { static bool tried; if (!tried) { tried = true; SkyCompile(); } }

// the game's sky constants now and then in the log (at the draw: the game binds them after the shader), and the sky's
// vertex shader once (what its TEXCOORD0 is)
static void SkyAfterDraw(ID3D11DeviceContext* ctx)
{
    if (!g_skyDrawLog) return;
    g_skyDrawLog = false;
    static bool vsDone;
    if (!vsDone) {
        vsDone = true;
        ID3D11VertexShader* vs = 0;
        ctx->VSGetShader(&vs, 0, 0);
        if (vs) { VsDumpCode(vs, "the sky's vertex shader"); vs->Release(); }
    }
    ID3D11Buffer* cb = 0;
    ctx->PSGetConstantBuffers(1, 1, &cb);
    if (!cb) return;
    D3D11_BUFFER_DESC bd;
    cb->GetDesc(&bd);
    D3D11_BUFFER_DESC sd = {};
    sd.ByteWidth = bd.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = 0;
    if (bd.ByteWidth >= 80 && SUCCEEDED(g_dev->CreateBuffer(&sd, 0, &st))) {
        ctx->CopyResource(st, cb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const float* f = (const float*)m.pData;
            Log("sky: game cloud colour %.4f %.4f %.4f %.3f | params %.3f %.3f %.3f %.3f | %.3f %.3f | key light %.2f %.2f %.2f | sun %.4f %.4f %.4f %.1f",
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[12], f[13], f[14], f[16], f[17], f[18], f[19]);
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    cb->Release();
}

// the constants (the sky draw and the sun pass's cloud shadows; 1.46: probe: for the sky probe's face of that size,
// into dst: its clouds marched there at once (no 2x2 turns, no depth test), no discs)
static bool SkyFill(ID3D11DeviceContext* ctx, UINT w = 0, UINT h = 0, ID3D11Buffer* dst = 0, UINT probe = 0)
{
    if (!g_skyCb) return false;
    bool on = g_enabled && g_p[P_SKY].v >= 0.5f;
    float k[48] = {
        g_p[P_SKY + 1].v, on ? g_p[P_SKY + 2].v : 0.0f, g_p[P_SKY + 3].v, (float)fmod(NowS(), 7200.0),
        1.0f * 0.05f, 0.62f * 0.05f, 0.38f * 0.05f, g_p[P_SKY + 6].v,
        g_p[P_SKY + 7].v, g_p[P_SKY + 5].v, g_p[P_SKY + 4].v, on ? 1.0f : 0.0f,
        g_cam[3][0], g_cam[3][1], g_cam[3][2], on ? g_p[P_SKY + 8].v : 0.0f,
        g_p[P_SKY + 9].v, g_p[P_SKY + 10].v, g_p[P_SKY + 11].v, g_p[P_SKY + 13].v,
        w ? 1.0f / w : 0, h ? 1.0f / h : 0, w ? 1.0f : 0.0f, (float)(g_frame & 3),
        g_csOrigin[0], g_csOrigin[2], g_csOrigin[1], g_csSrv && g_csOk ? kCsTexel : 0.0f,
        g_cloudHist ? 1.0f : 0.0f, (float)(g_frame & 1), (float)((g_frame >> 1) & 1), g_cloudW ? 1.0f / (2 * ((g_cloudW + 1) / 2)) : 0.0f,
        g_cloudW ? (float)(2 * ((g_cloudW + 1) / 2)) / (2 * ((g_cloudH + 1) / 2)) : 1.0f, (float)((g_frame >> 2) & 1023), g_cloudW ? 1.0f / g_cloudW : 0.0f, g_cloudH ? 1.0f / g_cloudH : 0.0f,
        g_air.depth && g_skyDepthOk ? 1.0f : 0.0f, g_sunActive && g_p[P_SKY_NIGHT].v > 0.5f ? g_nightF : 0.0f,   // (SK9.y: our night)
        g_sunActive && g_sunG < 0.5f ? g_sunDisc[0] : 0.0f, g_sunDisc[1],                                         // (SK9.zw: our sun's disc)
        -g_sunSky[0], -g_sunSky[1], -g_sunSky[2], g_sunActive ? 1 - g_sunG : 0.0f,     // (1.33: the sun, at night under the horizon)
        g_moonDisc[0], g_moonDisc[1], g_moonDisc[2], 0.0f };   // (SK11: the moon's disc: 1.37 none, the game's moon moved to ours)
    if (probe) {
        k[20] = k[21] = 1.0f / probe; k[22] = 1;   // SK5: the face's own clouds (t10)
        k[28] = 0; k[29] = k[30] = 0.5f;           // SK7: no 2x2 turn (no shift)
        k[36] = 0; k[38] = 0;                      // SK9: no depth test; no sun disc (its highlight is the sun pass's)
        k[19] = 0;                                 // SK4.w: the clouds' lowest quality (1.52: blurred in reflections anyway)
    }
    ID3D11Buffer* b = dst ? dst : g_skyCb;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    memcpy(m.pData, k, 192);
    ctx->Unmap(b, 0);
    return true;
}

// the sun pass: the clouds for their shadows (b11, t47..49)
static void SkyBindForSun(ID3D11DeviceContext* ctx)
{
    SkyEnsure();
    if (!g_skyCb || !g_skyW) return;
    static unsigned filled;
    if (filled != g_frame + 1) { filled = g_frame + 1; SkyFill(ctx); }
    ctx->PSSetConstantBuffers(11, 1, &g_skyCb);
    ID3D11ShaderResourceView* v[3] = { g_skyW, g_skyS, g_skyD };
    ctx->PSSetShaderResources(47, 3, v);
    ctx->PSSetShaderResources(50, 1, &g_csSrv);
}

// ---- 1.46: the sky probe for the reflections: our sky all around (t68). The camera never looks up, so the panorama of
// what was seen (1.39) never had the sky above, and puddles reflected a guess of it. The sky's own draw replayed into a
// 128^2 cube: its VS (position = (v0 x the world's rows 11..13 + row 14) x viewProjection rows 7..10 as .xyww) gets a
// face's rows (no translation: the dome's directions), a face a frame (all six at first or when old); the cloud pass
// marched for the face at once (128^2: ~1/30 of the screen's march), the sky over it eased in (25%: a new jitter each
// time: no grain); mips for rough surfaces. (SK5..9 for it: SkyFill's probe.)
static const UINT kProbe = 64;   // (1.52: was 128: 0.6..1.3 ms a frame)
static ID3D11Texture2D *g_probeTex, *g_probeClTex;
static ID3D11RenderTargetView *g_probeRtv[6], *g_probeClRtv;
static ID3D11ShaderResourceView* g_probeClSrv;
static ID3D11Buffer *g_probeVsCb, *g_probeRows[6], *g_probeSkyCb;
static ID3D11BlendState* g_probeBlend;   // (made last: all made)
static int g_probeFace;
static bool SkyProbeMake()
{
    static bool tried;
    if (tried) return g_probeBlend != 0;
    tried = true;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = kProbe; td.MipLevels = 0; td.ArraySize = 6; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(g_dev->CreateTexture2D(&td, 0, &g_probeTex))) { Log("sky probe: no cube"); return false; }
    for (int f = 0; f < 6; ++f) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = td.Format; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.MipSlice = 0; rd.Texture2DArray.FirstArraySlice = f; rd.Texture2DArray.ArraySize = 1;
        if (FAILED(g_dev->CreateRenderTargetView(g_probeTex, &rd, &g_probeRtv[f]))) { Log("sky probe: no face view"); return false; }
    }
    td.ArraySize = 1; td.MipLevels = 1; td.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&td, 0, &g_probeClTex)) || FAILED(g_dev->CreateRenderTargetView(g_probeClTex, 0, &g_probeClRtv)) ||
        FAILED(g_dev->CreateShaderResourceView(g_probeClTex, 0, &g_probeClSrv))) { Log("sky probe: no cloud target"); return false; }
    // the faces' rows 7..14 (D3D's cube faces +x -x +y -y +z -z): viewProjection x, y = the face's right and up, z = w = its
    // axis, no translation; the world: its axes, at 0
    static const float kR[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { 1, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
    static const float kU[6][3] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
    static const float kF[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 128; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;   // (a copy source only)
    for (int f = 0; f < 6; ++f) {
        float r[32] = {};
        for (int i = 0; i < 3; ++i) { r[i * 4] = kR[f][i]; r[i * 4 + 1] = kU[f][i]; r[i * 4 + 3] = kF[f][i]; }
        r[16] = r[21] = r[26] = 1;
        D3D11_SUBRESOURCE_DATA s = { r };
        if (FAILED(g_dev->CreateBuffer(&bd, &s, &g_probeRows[f]))) { Log("sky probe: no rows"); return false; }
    }
    bd.ByteWidth = 240; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_probeVsCb))) { Log("sky probe: no VS constants"); return false; }
    bd.ByteWidth = 192; bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_probeSkyCb))) { Log("sky probe: no sky constants"); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE; sd.TextureCube.MipLevels = (UINT)-1;
    ID3D11ShaderResourceView* srv = 0;
    if (FAILED(g_dev->CreateShaderResourceView(g_probeTex, &sd, &srv))) { Log("sky probe: no cube view"); return false; }
    D3D11_BLEND_DESC bl = {};
    D3D11_RENDER_TARGET_BLEND_DESC& b = bl.RenderTarget[0];
    b.BlendEnable = TRUE; b.SrcBlend = b.SrcBlendAlpha = D3D11_BLEND_BLEND_FACTOR; b.DestBlend = b.DestBlendAlpha = D3D11_BLEND_INV_BLEND_FACTOR;
    b.BlendOp = b.BlendOpAlpha = D3D11_BLEND_OP_ADD; b.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(g_dev->CreateBlendState(&bl, &g_probeBlend))) { srv->Release(); Log("sky probe: no blend"); g_probeBlend = 0; return false; }
    g_probeSrv = srv;
    Log("sky probe: %u^2 cube for the reflections' sky (a face a frame)", kProbe);
    return true;
}
// at the sky draw (its state bound, the cloud pass's inputs too): a face (all six when none / old)
static void SkyProbePass(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (g_p[P_SSR].v <= 0 || g_p[P_SSR_SKY].v < 0.5f || !g_cloudPS || !g_skyPS || !SkyProbeMake()) return;
    ID3D11Buffer* vcb = 0;
    ctx->VSGetConstantBuffers(1, 1, &vcb);
    if (!vcb) return;
    D3D11_BUFFER_DESC bd;
    vcb->GetDesc(&bd);
    if (bd.ByteWidth < 240) { vcb->Release(); return; }
    bool all = !g_probeFrame || g_frame + 1 - g_probeFrame > 120;   // none yet / old (our sky or the reflections were off)
    if (!all && (g_frame & 1)) { vcb->Release(); return; }   // (1.52: a face every other frame)
    ID3D11RasterizerState* rs = 0;
    ctx->RSGetState(&rs);
    ctx->RSSetState(g_rsNoCull);   // (a face's view may be mirrored)
    D3D11_VIEWPORT pv = { 0, 0, (float)kProbe, (float)kProbe, 0, 1 };
    ctx->RSSetViewports(1, &pv);
    D3D11_BOX whole = { 0, 0, 0, 240, 1, 1 }, rows = { 0, 0, 0, 128, 1, 1 };
    ctx->CopySubresourceRegion(g_probeVsCb, 0, 0, 0, 0, vcb, 0, &whole);   // the game's (its cloud layers' places)
    ctx->VSSetConstantBuffers(1, 1, &g_probeVsCb);
    SkyFill(ctx, 0, 0, g_probeSkyCb, kProbe);
    ctx->PSSetConstantBuffers(11, 1, &g_probeSkyCb);
    float zero[4] = {}, bf[4];
    for (int j = 0; j < 4; ++j) bf[j] = all ? 1.0f : 0.25f;
    ID3D11ShaderResourceView* none = 0;
    for (int k = 0; k < (all ? 6 : 1); ++k) {
        int f = all ? k : g_probeFace;
        ctx->CopySubresourceRegion(g_probeVsCb, 0, 112, 0, 0, g_probeRows[f], 0, &rows);   // rows 7..14: the face's
        ctx->OMSetBlendState(0, 0, 0xffffffff);
        ctx->OMSetRenderTargets(1, &g_probeClRtv, 0);
        ctx->PSSetShaderResources(10, 1, &none);
        oPSSetShader(ctx, g_cloudPS, 0, 0);
        Replay(ctx, d);
        if (all) ctx->ClearRenderTargetView(g_probeRtv[f], zero);
        ctx->OMSetRenderTargets(1, &g_probeRtv[f], 0);
        ctx->PSSetShaderResources(10, 1, &g_probeClSrv);
        ctx->OMSetBlendState(g_probeBlend, bf, 0xffffffff);
        oPSSetShader(ctx, g_skyPS, 0, 0);
        Replay(ctx, d);
        ctx->PSSetShaderResources(10, 1, &none);
    }
    if (!all) g_probeFace = (g_probeFace + 1) % 6;
    ctx->OMSetRenderTargets(0, 0, 0);
    ctx->GenerateMips(g_probeSrv);
    ctx->OMSetBlendState(0, 0, 0xffffffff);
    ctx->VSSetConstantBuffers(1, 1, &vcb);
    ctx->PSSetConstantBuffers(11, 1, &g_skyCb);
    ctx->RSSetState(rs);
    if (rs) rs->Release();
    vcb->Release();
    g_probeFrame = g_frame + 1;
}

static void WetGlossPass(ID3D11DeviceContext* ctx, const DrawCall& d, UINT W, UINT H, ID3D11ShaderResourceView* vp);   // gamewet.inc
// PSSetShader: the game's sky -> ours (returns what to bind)
static ID3D11PixelShader* SkySubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps)
{
    if (!g_enabled || !ps || g_p[P_SKY].v < 0.5f || !Immediate(ctx)) return ps;
    LONG n = g_skyGameN;
    bool hit = false;
    for (LONG i = 0; i < n && !hit; ++i) hit = g_skyGame[i] == ps;
    if (!hit) return ps;
    SkyEnsure();
    if (!g_skyPS || !g_skyCb || !g_skyW || !g_skyS || !g_skyD) return ps;
    static DWORD lastLog;
    if (GetTickCount() - lastLog > 30000) { lastLog = GetTickCount(); g_skyDrawLog = true; }
    SkyFill(ctx);
    ctx->PSSetConstantBuffers(11, 1, &g_skyCb);
    ID3D11ShaderResourceView* v[3] = { g_skyW, g_skyS, g_skyD };
    ctx->PSSetShaderResources(47, 3, v);
    ctx->PSSetSamplers(7, 1, &g_sWrap);
    g_skyPending = g_cloudPS != 0;   // the draw: the cloud pass first (SkyBeforeDraw)
    return g_skyPS;
}

// before the sky draw (our shader bound): the same draw into the cloud target, a quarter of the size, with the cloud
// pass's shader; then the sky draw reads it
static void SkyBeforeDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (!g_skyPending) return;
    g_skyPending = false;
    ID3D11PixelShader* cur = 0;
    ctx->PSGetShader(&cur, 0, 0);
    if (cur) cur->Release();
    if (cur != g_skyPS) return;
    ID3D11RenderTargetView* rt[8] = {};
    ID3D11DepthStencilView* ds = 0;
    ctx->OMGetRenderTargets(8, rt, &ds);
    UINT W = 0, H = 0;
    if (rt[0]) {
        ID3D11Resource* r = 0; rt[0]->GetResource(&r);
        ID3D11Texture2D* t = 0; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
        if (t) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); W = td.Width; H = td.Height; t->Release(); }
    }
    // the cloud picture's size: a quarter, half, 80% or all of the screen's
    static const float kRes[4] = { 0.25f, 0.5f, 0.8f, 1.0f };
    int ri = (int)(g_p[P_SKY + 14].v + 0.5f);
    float sc = kRes[ri < 0 ? 0 : ri > 3 ? 3 : ri];
    UINT cw = (UINT)ceilf(W * sc), ch = (UINT)ceilf(H * sc);
    if (W && (cw != g_cloudW || ch != g_cloudH || !g_cloudTex)) {
        ID3D11DeviceChild* old[9] = { g_cloudSrv, g_cloudRtv, g_cloudTex, g_cloudSrv2, g_cloudRtv2, g_cloudTex2, g_cmSrv, g_cmRtv, g_cmTex };
        for (int k = 0; k < 9; ++k) if (old[k]) old[k]->Release();
        g_cloudSrv = g_cloudSrv2 = 0; g_cloudRtv = g_cloudRtv2 = 0; g_cloudTex = g_cloudTex2 = 0; g_cmSrv = 0; g_cmRtv = 0; g_cmTex = 0;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = cw; td.Height = ch; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, 0, &g_cloudTex))) { g_dev->CreateRenderTargetView(g_cloudTex, 0, &g_cloudRtv); g_dev->CreateShaderResourceView(g_cloudTex, 0, &g_cloudSrv); }
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, 0, &g_cloudTex2))) { g_dev->CreateRenderTargetView(g_cloudTex2, 0, &g_cloudRtv2); g_dev->CreateShaderResourceView(g_cloudTex2, 0, &g_cloudSrv2); }
        td.Width = g_cmW = (cw + 1) / 2; td.Height = g_cmH = (ch + 1) / 2;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, 0, &g_cmTex))) { g_dev->CreateRenderTargetView(g_cmTex, 0, &g_cmRtv); g_dev->CreateShaderResourceView(g_cmTex, 0, &g_cmSrv); }
        g_cloudW = cw; g_cloudH = ch;
        g_cloudHist = false;
    }
    ID3D11RenderTargetView* curRtv = g_cloudCur ? g_cloudRtv2 : g_cloudRtv;
    ID3D11ShaderResourceView* curSrv = g_cloudCur ? g_cloudSrv2 : g_cloudSrv;
    ID3D11ShaderResourceView* prevSrv = g_cloudCur ? g_cloudSrv : g_cloudSrv2;
    // this frame's camera / viewProjection, copied on the GPU from the sky VS's constants (cb1[7..14])
    bool vpOk = false;
    {
        ID3D11Buffer* vcb = 0;
        ctx->VSGetConstantBuffers(1, 1, &vcb);
        if (vcb && g_vpBuf[0] && g_vpBuf[1]) {
            D3D11_BUFFER_DESC bd;
            vcb->GetDesc(&bd);
            if (bd.ByteWidth >= 240) {
                D3D11_BOX box = { 112, 0, 0, 240, 1, 1 };
                ctx->CopySubresourceRegion(g_vpBuf[g_cloudCur], 0, 0, 0, 0, vcb, 0, &box);
                vpOk = true;
            }
        }
        if (vcb) vcb->Release();
    }
    if (!vpOk) g_cloudHist = false;
    bool ok = W && curRtv && curSrv && vpOk && g_cmRtv && g_resolvePS;
    // the shadow map's place: around the camera, on whole texels (no swimming)
    g_csOrigin[0] = floorf(g_cam[3][0] / kCsTexel) * kCsTexel - 128 * kCsTexel;
    g_csOrigin[2] = floorf(g_cam[3][2] / kCsTexel) * kCsTexel - 128 * kCsTexel;
    g_csOrigin[1] = g_cam[3][1];
    g_csOk = ok && g_csRtv && g_csPS;
    g_skyDepthOk = g_air.depth != 0;
    SkyFill(ctx, ok ? W : 0, ok ? H : 0);
    ctx->PSSetConstantBuffers(11, 1, &g_skyCb);
    ctx->PSSetSamplers(8, 1, &g_sText);
    if (ok) {
        UINT nvp = 16;
        D3D11_VIEWPORT vp[16];
        ctx->RSGetViewports(&nvp, vp);
        ID3D11BlendState* bs = 0; float bf[4]; UINT bm;
        ctx->OMGetBlendState(&bs, bf, &bm);
        ID3D11ShaderResourceView* none[4] = {};
        ctx->PSSetShaderResources(10, 4, none);
        ctx->OMSetBlendState(0, 0, 0xffffffff);
        ID3D11ShaderResourceView* in[3] = { g_vpSrv[g_cloudCur], g_vpSrv[g_cloudCur ^ 1], prevSrv };
        ctx->PSSetShaderResources(11, 3, in);
        // the march: an eighth of the size; its pixel centres on this frame's quarter-size pixel of each 2x2
        ctx->OMSetRenderTargets(1, &g_cmRtv, 0);   // (the game's depth unbound: it can be read now)
        ctx->PSSetShaderResources(15, 1, &g_air.depth);   // the scene's depth: skip what the city hides
        ctx->PSSetSamplers(9, 1, &g_sPoint);
        D3D11_VIEWPORT mv = { 0, 0, W * sc * 0.5f, H * sc * 0.5f, 0, 1 };   // half the cloud picture: the 2x2 shift is in the shader
        ctx->RSSetViewports(1, &mv);
        oPSSetShader(ctx, g_cloudPS, 0, 0);
        Replay(ctx, d);
        // the resolve: the quarter-size picture
        ctx->OMSetRenderTargets(1, &curRtv, 0);
        ctx->PSSetShaderResources(14, 1, &g_cmSrv);
        D3D11_VIEWPORT cv = { 0, 0, W * sc, H * sc, 0, 1 };
        ctx->RSSetViewports(1, &cv);
        oPSSetShader(ctx, g_resolvePS, 0, 0);
        Replay(ctx, d);
        ctx->PSSetShaderResources(14, 2, none);
        // the cloud-shadow map
        if (g_csOk) {
            ctx->OMSetRenderTargets(1, &g_csRtv, 0);
            D3D11_VIEWPORT sv = { 0, 0, 256, 256, 0, 1 };
            ctx->RSSetViewports(1, &sv);
            oPSSetShader(ctx, g_csPS, 0, 0);
            Replay(ctx, d);
        }
        ProfMark(ctx, PR_PROBE, 0);
        SkyProbePass(ctx, d);   // 1.46: the sky probe for the reflections (a face a frame)
        ProfMark(ctx, PR_PROBE, 1);
        WetGlossPass(ctx, d, W, H, g_vpSrv[g_cloudCur]);   // the game's gloss before its lighting (gamewet.inc)
        oPSSetShader(ctx, g_skyPS, 0, 0);
        ctx->OMSetRenderTargets(8, rt, ds);
        ctx->RSSetViewports(nvp, vp);
        ctx->OMSetBlendState(bs, bf, bm);
        if (bs) bs->Release();
        ctx->PSSetShaderResources(13, 1, none);
        ctx->PSSetShaderResources(10, 1, &curSrv);
        g_cloudHist = true;
        g_cloudCur ^= 1;
    }
    for (int k = 0; k < 8; ++k) if (rt[k]) rt[k]->Release();
    if (ds) ds->Release();
}

// weather presets: cover, type, cirrus, rain, cloud shadows
static void SkyApplyWeather(int k)
{
    static const float W[7][5] = {
        { 0, 0, 0, 0, 0 },                 // custom
        { 0.12f, 0.5f, 0.35f, 0, 0.85f },  // clear: a few small cumulus, some cirrus
        { 0.4f, 0.55f, 0.2f, 0, 0.85f },   // fair: cumulus
        { 0.65f, 0.6f, 0.1f, 0.15f, 0.85f },  // cloudy
        { 1.0f, 0.12f, 0, 0.35f, 0.9f },   // overcast: a stratus sheet
        { 0.85f, 0.95f, 0, 1.0f, 0.95f },  // storm: tall dark rain clouds
        { 0.05f, 0.5f, 0.9f, 0, 0.85f },   // cirrus
    };
    if (k <= 0 || k > 6) return;
    g_p[P_SKY + 2].v = W[k][0]; g_p[P_SKY + 9].v = W[k][1]; g_p[P_SKY + 10].v = W[k][2]; g_p[P_SKY + 11].v = W[k][3]; g_p[P_SKY + 8].v = W[k][4];
}
