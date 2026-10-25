// NewLook: replaces the sun part of the game's deferred sun pass.
// Runs right after the game's own sun pass, with the game's vertex shader, viewport and
// constant buffers still bound. tDest is a copy of what the game's pass produced; we take the
// game's sun light out of it again (recomputed exactly like the game does it) and put it back
// with better shadows, contact shadows and ambient occlusion.

cbuffer GameGlobal : register(b0) { float4 G[64]; }   // the game's gGlobalCB (layout per variant)
cbuffer GameObject : register(b1) { float4 O[8]; }    // the game's gObjectCB

cbuffer LightCB : register(b5)
{
    int4 L0;     // KeyLightColour, KeyLightDirection, ShadowMap_Constants, ShadowMap_Constants2 (float4 index)
    int4 L1;     // ShadowMap_ConstantsArray[4] (-1 = 1-tap variant), ShadowMap_ScaleOffset, WorldToLight0, ViewPosition
    int4 L2;     // deferred_cameraDirection, mask settings (gObjectCB, -1 = none), flags, view mode
    float4 Size; // render target w, h, shadow atlas w, h
    float4 P0;   // contact strength, contact length (m), contact thickness (m), sun size (x real sun)
    float4 P1;   // min blur (texels), max blur (texels), AO strength, AO radius (m)
    float4 P2;   // ambient scale, sun scale, contact bias (m), frame counter
    float4 P3;   // sharp distance (m), shadow darkness, sky tint in shadow, sun warmth
    float4 P4;   // ambient tint (warm > 0 > cool), haze amount, haze height (m), sun glow
    float4 P5;   // light shafts, haze blue, -, half-res result available
    float4 P6;   // bounce light strength, bounce radius (m), previous frame available (camera + lit scene), bounce history available
    float4 P7;   // bounce from all lights (1) or the sun only (0), history weight, lamp light distance limit (m), bounce texture bound
    float4 P8;   // lamp light strength, lamp detection threshold (x average brightness), lamp light radius (m), map lamps by day (0..1)
    float4 P9;   // lamp shadows (0..1), lamp shadow thickness (m), night ambient (0..1), lamp size for shadow maps (m, 0 = no maps)
    float4 P10;  // our soft sun shadows (1) or the game's (0), lamp light texture ready (t35), its width, height
    float4 P11;  // sky occlusion strength, its radius (m), car paint reflections, puddles
    float4 P12;  // reflections (strength), wet ground (0..1), reflection distance (m), reflection history available
    float4 P13;  // reflection target size / frame size (x, y), reflection target size (pixels)
    float4 P14;  // lamp glow in the air, city glow in the night haze, haze brightness, -
    float4 P15;  // headlight beams (strength; 0 = none this frame), headlight shadow map there, beam length (m), -
    float4 P16;  // modern lighting (1) or the game's (0), dielectric reflectance (x 4%), sky reflections, roughness scale
    float4 P17;  // relief: strength, depth scale, reach (m), -
    float4 P18;  // our sun (SunPath, 1.30): its direction (light -> scene), its light (0 below the horizon .. 1)
    float4 P19;  // its colour; on (1)
    float4 P20;  // the game's key light's share (1 at night: the moon; 0 by day: ours instead); it with our maps (1); the haze's sky light; the reflections' ray length
    float4 P21;  // 1.39: x = our sky's panorama (t67) for the reflections; 1.40: y = wet ground other than roads (x); 1.41: z = the panorama's fading a frame; 1.43: w = rays jittered
    float4 P22;  // 1.43: x = highlights' anti-shimmer, y = wet stones' relief kept, z = bright dots cut (x spread), w = smoothing (0 none, 1 by gloss, 2 always)
    float4 P23;  // 1.43: x = reflections' history (x); 1.44: y = wet ground's reflections' sharpness (its gloss); 1.45: z = the sky's level now
    float4 P24;  // 1.46: x = the reflections' history held to this frame's (x its spread), y = the sky probe there (t68); 1.47: z = the scene before the effects (t70); 1.49: w = the AO's history (t71)
    float4 P25;  // 1.66 (options to compare): x = sky occlusion cosine-weighted, y = AO multi-bounce, z = AO visibility bitmask; w = 1.96g SsrEdges
    float4 P26;  // 1.67: the reflected sky off screen: x = brightness, y = source (0 auto, 1 seen, 2 probe matched, 3 probe raw), z = the probe's colour match, w = the seen sky kept by the sky's level
    float4 P27;  // 1.70: car paint: x = clear coat, y = the colour's highlight soft, z = metallic flakes (the player's car)
    float4 P28;  // 1.74: x = the car's simulated drops there (t73 .. t76), y z w = fine beads blown off: top, sides, front / back
    float4 P29;  // 1.75: x = seconds (real time, wrapped at an hour)
    float4 P30, P31, P32, P33;   // 1.83: rain falling (rainfall.inc): see VS_Rain
};
// the key light our other passes use: by day ours (its light faded at the horizon), at night the game's (the moon)
float3 SunLd() { return (P19.w > 0.5 && P20.x < 0.5) ? P18.xyz : G[L0.y].xyz; }
float3 SunLc() { return P19.w > 0.5 ? P19.rgb * P18.w + G[L0.x].xyz * P20.x : G[L0.x].xyz; }

// Lamps found in the image (street lights, signs, windows, headlights), kept in world space by the plugin.
// Up to 256 lamps a frame, in a buffer (t33): per lamp [3k] xyz world position, w = (1 if from the lamp map, fades by day)
// + 2 x (the zones that act on it); [3k+1] rgb intensity (radiance x area), w = the zones it is in; [3k+2] x = its
// shadow map slot (-1 none).
// Screen tiles (t31, t32: 256 bits each, one bit per lamp): the lamps whose light reaches the tile (PS_Tiles).
Buffer<float4> tLamps : register(t33);
Texture2D<uint4> tTileA : register(t31);
Texture2D<uint4> tTileB : register(t32);
// 4 entries a lamp: position (w: map flag + 2 x zones acting), colour (w: zones it is in), x = shadow map slot (-1 none,
// -2 a car's light), y = its own light radius (0: the global one), z = 1 for a car's light (no glow in the air);
// d = a spot's direction (lamp -> lit points) and the cosine of its cone (w = 0: all around).
float4 LampP(int k) { return tLamps.Load(4 * k); }
float4 LampC(int k) { return tLamps.Load(4 * k + 1); }
float4 LampX(int k) { return tLamps.Load(4 * k + 2); }
float4 LampD(int k) { return tLamps.Load(4 * k + 3); }
// a lamp's light at a point (direction l: point -> lamp): its own radius window and the spot's cone
float LampWindow(float4 LX, float4 LD, float r2, float3 l)
{
    float R2 = LX.y > 0 ? LX.y * LX.y : P8.z * P8.z;
    float f = saturate(1 - r2 / R2);
    f *= f;
    if (LD.w > 0) { float c = saturate((dot(-l, LD.xyz) - LD.w) / (1 - LD.w)); f *= c * c; }
    return f;
}
// Lamp shadow maps (lampshadow.inc): 5 faces a lamp (+x -x +z -z -y), the game's depth (z = a + b / d), 256 x 256.
Texture2DArray<float> tLampSM : register(t34);
#define kLsTan 1.08
#define kLsNear 0.1
cbuffer LampCB : register(b6)
{
    int4 LampN;           // x = count, y z = screen tiles across / down
    float4 LampTgt[2];    // lamp editor targets: [0] the map lamp Shift+J deletes, [1] the found lamp Shift+K adds (w = 1 if any)
    float4 LampSel;       // selected map lamp: xyz, w = arrow length (0 = none)
    float4 LampUI;        // mouse cursor uv, arrow being dragged (1..3), arrow under the cursor (1..3)
    float4 LampScr;       // x = shown height of a target pixel / its width (the game renders 2x taller)
    float4 LampObj;       // the selected lamp's game object position (w = 1 if it is connected)
    // light zones (tunnels, garages): light from outside a zone does not get in, light from inside does not get out
    float4 ZoneC[16];     // centre, w = turn about the vertical (radians)
    float4 ZoneH[16];     // half sizes, w = soft edge width (m)
    int4 ZoneN;           // x = count, y = the selected one (-1 none)
};

// how much a point is inside a zone: 1 inside, 0 outside, a 1 m soft edge
float ZoneIn(int z, float3 world)
{
    float3 d = world - ZoneC[z].xyz;
    float sn, cs;
    sincos(ZoneC[z].w, sn, cs);
    float3 l = float3(d.x * cs - d.z * sn, d.y, d.x * sn + d.z * cs);
    float3 o = abs(l) - ZoneH[z].xyz;
    return saturate(0.5 - max(o.x, max(o.y, o.z)) / max(ZoneH[z].w, 0.1));
}
#define SharpM P3.x

#define F_SHADOW   1
#define F_SPEC     2
#define F_ADD      4
#define F_CLOUD    8
#define F_CMPGREAT 16
#define F_LAMBERT  32
#define F_ASHIKH   64
#define F_VFX      128
#define F_AIRLATER 256   // the air is drawn later, after the transparents (PS_Air)

Texture2D<float4> tDest    : register(t16);
Texture2D<float4> tDepth   : register(t17);
Texture2D<float4> tNormal  : register(t18);
Texture2D<float4> tDiffuse : register(t19);
Texture2D<float4> tSpec    : register(t20);
Texture2D<float4> tAdd     : register(t21);
Texture2D<float>  tShadow  : register(t22);
Texture2D<float4> tCloud   : register(t23);
Texture2D<float4> tVfx     : register(t24);   // particle (smoke) shadows
Texture2D<float4> tHalf    : register(t25);   // half-res pass: ao, shafts, contact, distance
Texture2D<float4> tGI      : register(t26);   // half-res bounce light, blurred: rgb, distance
Texture2D<float4> tGIRaw   : register(t27);   // half-res bounce light before the blur
Texture2D<float4> tCamPrev : register(t28);   // previous frame's view ray record (4x2): A, B, C, camera position
Texture2D<float4> tRadPrev : register(t29);   // previous frame's lit scene, half res: rgb, distance (mips: average)
Texture2D<float4> tGIHist  : register(t30);   // previous frame's bounce light: rgb, distance
Texture2D<float4> tLampLight : register(t35); // this frame's lamp light (PS_Lamps, at the shown resolution)
Texture2D<float4> tSsr     : register(t36);   // half-res reflections: rgb (x confidence), distance
Texture2D<float2> tSsrAux  : register(t69);   // 1.46: the ray's gloss, its length to what it found (1000: the sky). (1.42 .. 1.45 put
                                              // the gloss on w, x 1e5: past FP16's 65504: no smoothing by depth, no history at all)
TextureCube tSkyProbe      : register(t68);   // 1.46: our sky all around (sky.inc: a cube from the sky's own draw, a face a frame)
Texture2D<float4> tRefl    : register(t37);   // this frame's whole lit scene, half res, with mips (reflection source)
Texture2D<float4> tSsrHist : register(t38);   // the previous frame's smoothed reflections: rgb, distance
Texture2D<float4> tSkyPano : register(t67);   // 1.39: our sky as seen, all around (azimuth across, 90 .. -10 deg up; a = seen)
Texture2D<float4> tScene   : register(t39);   // this frame's lit scene at full size (sharp reflections; 1.47: before the game's effects)
Texture2D<float4> tEarly   : register(t70);   // 1.47: the scene before the game's effects (glass, halos, sprites), for the snapshot's [1]
Texture2D<float4> tCamCur  : register(t40);   // this frame's camera record (t28 holds the previous one there)

// which surfaces reflect (0..1): glossy ones; dry ones need more gloss; walls only glass-like gloss (windows), so
// bump-mapped facades do not shimmer
// car paint (clear coat): the game's G-buffer gives nearly everything a high gloss (N.w ~0.9: concrete, asphalt too);
// what marks paint (and chrome, glass) is a bright specular colour (roads and walls: near black)
float PaintOf(float4 Sp) { return P11.z * saturate((dot(Sp.rgb, float3(0.2126, 0.7152, 0.0722)) - 0.1) * 5); }
float SsrVis(float gloss, float ny, float wet)
{
    float thr = ny < -0.5 ? 0.85 : abs(ny) < 0.5 ? 0.78 : lerp(0.55, 0.35, saturate(wet * 2));   // ceilings: as walls
    return saturate((gloss - thr) / 0.2);
}

// The lamp light of a pixel from the smaller lamp light texture: its 4 nearest texels, weighted by place (bilinear) and
// by how close their surface's distance is to this pixel's (no light from the background on an object's edge).
Texture2D<float4> tGlow : register(t41);
Texture2D<float4> tLampBest : register(t42);   // the strongest lamp of a pixel: index + 1, its shadow x zone factor   // the lamps' glow in the air (PS_Lamps' second target), added with the air
float3 LampTexAt(Texture2D<float4> tl, float2 uv, float dist);
float3 LampLightAt(float2 uv, float dist) { return LampTexAt(tLampLight, uv, dist); }
float3 GlowAt(float2 uv, float dist) { return LampTexAt(tGlow, uv, dist); }
float3 LampTexAt(Texture2D<float4> tl, float2 uv, float dist)
{
    float2 sz = P10.zw;
    float2 p = uv * sz - 0.5;
    float2 f = frac(p);
    int2 b = (int2)floor(p);
    float3 acc = 0;
    float ws = 0, wb0 = 0;
    float3 nearest = 0;
    float tol = 0.015 * dist + 0.08;
    [unroll] for (int k = 0; k < 4; ++k) {
        int2 o = int2(k & 1, k >> 1);
        float4 t = tl.Load(int3(clamp(b + o, int2(0, 0), (int2)sz - 1), 0));
        float wb = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y);
        float w = wb * exp2(-abs(t.w - dist) / tol);
        acc += t.rgb * w; ws += w;
        if (wb > wb0) { wb0 = wb; nearest = t.rgb; }
    }
    return ws > 1e-4 ? acc / ws : nearest;   // no texel of this surface around: the nearest one
}

SamplerComparisonState sCmp : register(s10);
SamplerState sPoint : register(s11);
SamplerState sWrap  : register(s12);
SamplerState sLin   : register(s13);

struct PSIn
{
    float4 pos : SV_Position;
    float4 t0  : TEXCOORD0;   // xy = screen uv
    float4 t1  : TEXCOORD1;   // view ray: rel = t1.xyz / (depth - t1.w)
};

struct PSOut
{
    float4 c : SV_Target0;
#ifdef BLEND_OUT
    float4 k : SV_Target1;   // dual-source blending: result = c + dest * k (no copy of the frame needed)
#endif
#ifdef WRITE_DEPTH
    float d : SV_Depth;
#endif
};

static const float2 kPoisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),   float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),  float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790)
};

float Ign(float2 p)  // interleaved gradient noise, changes a little every frame
{
    p += P2.w * float2(5.588238, 5.588238);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float CmpTap(float2 uv, float z)
{
    return tShadow.SampleCmpLevelZero(sCmp, uv, z);
}

bool IsBlocker(float stored, float z)
{
    return (L2.z & F_CMPGREAT) ? (stored > z) : (stored < z);
}

// Where does world-relative direction `dir` (from the camera) land on screen? Solves
// t1 + X*dx + Y*dy = s*dir using the screen derivatives of the (affine) view ray.
bool Project(float3 dir, float4 ray, float4 X, float4 Y, float sgn, out float2 dpix, out float4 rayAt)
{
    float3 c0 = X.xyz, c1 = Y.xyz, c2 = -dir, b = -ray.xyz;
    float3 c12 = cross(c1, c2);
    float det = dot(c0, c12);
    dpix = 0;
    rayAt = ray;
    if (abs(det) < 1e-12) return false;
    float dx = dot(b, c12) / det;
    float dy = dot(c0, cross(b, c2)) / det;
    float s  = dot(c0, cross(c1, b)) / det;
    dpix = float2(dx, dy);
    rayAt = ray + X * dx + Y * dy;
    return s * sgn > 0;   // same side as the pixel's own ray (t1 = (depth - w) * rel)
}

// Scene distance from the camera at a pixel offset; also how far along the ray the sample sits.
float SceneDist(float2 pix, float4 rayAt)
{
    float2 uv = pix / Size.xy;
    if (any(uv < 0) || any(uv > 1)) return -1;
    float z = tDepth.SampleLevel(sPoint, uv, 0).x;
    float d = z - rayAt.w;
    if (abs(d) < 1e-9) return -1;
    return length(rayAt.xyz / d);
}

// 1.65: Project for the points of one ray q = o + d t, set up once: its solve's determinant and numerators are triple
// products, linear in q, so per point they are a + b t each (the same result with a tenth of the arithmetic). The marches
// below also read all their depth samples together (unrolled, no early exit; the first hit still decides): one after
// another, each waiting for the last, the reads' latency was most of their cost (PS_Lamps 6.6 -> 4.9 ms at 1440p).
struct RayProj { float2 pd, px, py; float ns; };
RayProj ProjSetup(float3 o, float3 d, float4 ray, float4 X, float4 Y, float sgn)
{
    RayProj r;
    float3 XY = cross(X.xyz, Y.xyz), RY = cross(ray.xyz, Y.xyz), XR = cross(X.xyz, ray.xyz);
    r.pd = -float2(dot(o, XY), dot(d, XY));
    r.px = float2(dot(o, RY), dot(d, RY));
    r.py = float2(dot(o, XR), dot(d, XR));
    r.ns = -dot(ray.xyz, XY) * sgn;
    return r;
}
// as Project(o + d t, ray, X, Y, sgn, dpix, rayAt)
bool ProjAt(RayProj r, float t, float4 ray, float4 X, float4 Y, out float2 dpix, out float4 rayAt)
{
    float det = r.pd.x + r.pd.y * t, id = 1 / det;
    dpix = float2(r.px.x + r.px.y * t, r.py.x + r.py.y * t) * id;
    rayAt = ray + X * dpix.x + Y * dpix.y;
    return abs(det) >= 1e-12 && r.ns * id > 0;
}

// Sun visibility (1 = lit) at any world point, from the game's cascaded shadow map (one tap).
float SunVisibility(float3 world)
{
    int w = L1.z;
    float3 ls = world.y * G[w + 1].xyz + world.x * G[w].xyz + world.z * G[w + 2].xyz + G[w + 3].xyz;
    float4 so0 = G[L1.y], so1 = G[L1.y + 1], so2 = G[L1.y + 2];
    float2 cA = ls.xy, cB = ls.xy * so0.xy + so1.xy, cC = ls.xy * so0.zw + so2.xy;
    float3 sc;
    if (max(abs(cA.x), abs(cA.y)) < 0.99) sc = float3(cA * float2(0.5, -0.166667) + float2(0.5, 0.166667), ls.z);
    else if (max(abs(cB.x), abs(cB.y)) < 0.99) sc = float3(cB * float2(0.5, -0.166667) + float2(0.5, 0.5), ls.z + so1.z);
    else if (max(abs(cC.x), abs(cC.y)) < 0.99) sc = float3(cC * float2(0.5, -0.166667) + float2(0.5, 0.833333), ls.z + so2.z);
    else return 1;   // outside every cascade: assume sunlit
    return tShadow.SampleCmpLevelZero(sCmp, sc.xy, sc.z);
}

// Optical depth of exponential height fog between the camera and a point (analytic).
float HazeDepth(float3 rel, float dist, float camY, float density, float heightM)
{
    float h0 = camY, dy = rel.y;
    float f0 = exp(-h0 / heightM);
    float k = abs(dy) > 0.01 ? (1 - exp(-dy / heightM)) / (dy / heightM) : 1;
    return density * dist * f0 * k;
}

// Haze, glow around the sun and light shafts on top of a colour seen at `rel` from the camera.
float MarchShafts(float3 rel, float dist, float noise)
{
    float3 V = rel / max(dist, 1e-3);
    float marchLen = min(dist, 250);
    float acc = 0;
    [loop] for (int m = 0; m < 12; ++m) {
        float t = (m + noise) / 12 * marchLen;
        acc += SunVisibility(G[L1.w].xyz + V * t);
    }
    return lerp(1, acc / 12, P5.x);
}

// Haze, glow around the sun and light shafts: the light the air adds; the colour behind is scaled by hazeT.
// camY: the camera height, passed in (read once per shader: d3dcompiler_47 mis-indexed G[L1.w] here)
// camP: the camera (camY its height); 1.91: the glow round the sun dimmed by our clouds between the camera and the sun
float3 AirLight(float3 rel, float dist, float noise, bool shaftsOn, float shaftsIn, float3 camP, out float hazeT, out float shafts)
{
    float camY = camP.y;
    hazeT = 1;
    shafts = 1;
    if (P4.y <= 0) return 0;
    float3 Lc = SunLc(), Ld = SunLd();
    float3 V = rel / max(dist, 1e-3);
    hazeT = exp(-HazeDepth(rel, dist, camY, P4.y * 0.0006, max(P4.z, 5)));
    if (shaftsIn >= 0) shafts = shaftsIn;               // from the half-res pass
    else if (shaftsOn && P5.x > 0) shafts = MarchShafts(rel, dist, noise);
    float3 warm = float3(1 + 0.18 * P3.w, 1 + 0.02 * P3.w, 1 - 0.30 * P3.w);
    warm /= dot(warm, float3(0.2126, 0.7152, 0.0722));
    float lum = dot(Lc, float3(0.2126, 0.7152, 0.0722));
    float cosT = dot(V, -normalize(Ld));
    float g = 0.75, g2 = g * g;
    float mie = (1 - g2) / (4 * 3.14159 * pow(1 + g2 - 2 * g * cosT, 1.5));     // forward scattering around the sun
    // (1.35: with our sun the sky's light by its height, not its own light: low at dawn the haze went black)
    float lumS = P19.w > 0.5 ? max(lum, P20.z) : lum;
    float3 sky = lerp(float3(1, 1, 1), float3(0.55, 0.72, 1.0), P5.y) * lumS * 0.12 * P14.z;   // blue air light
    // at night the air is lit by the city below (warm): far streets fade into a glowing haze, not into black
    float night = 1 - saturate((lumS - 0.5) / 2);
    float3 city = float3(1.0, 0.72, 0.45) * 0.035 * P14.y * night;
    // (1.91: a glow needs the sun's disc seen: behind a cloud (the cloud shadows' map at the camera), squared, and a
    // thick deck (SkyOvercastSun): little of it; the light the clouds spread is in `sky`)
    float cs = CloudShadow(camP, -normalize(Ld), sWrap);
    mie *= cs * cs * lerp(1, SK12.x * SK12.x, CloudDeck());
    return (sky + city) * lerp(0.6, 1, shafts) + Lc * warm * mie * P4.w * 0.25 * shafts;
}

// Depth-aware upsampling of a half-res target (w = distance).
float4 HalfFetch(Texture2D<float4> t, float2 pos, float dist)
{
    float2 hp = pos * 0.5 - 0.5;
    int2 b0 = int2(floor(hp));
    float2 f = hp - b0;
    int2 hmax = int2(Size.xy * 0.5) - 1;
    float4 acc = 0;
    float wsum = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        int2 o2 = int2(k & 1, k >> 1);
        float4 h = t.Load(int3(clamp(b0 + o2, 0, hmax), 0));
        float wb = (o2.x ? f.x : 1 - f.x) * (o2.y ? f.y : 1 - f.y);
        float wd = 1 / (1e-3 + abs(h.w - dist) / (dist * 0.02 + 0.05));
        acc += h * (wb * wd + 1e-5);
        wsum += wb * wd + 1e-5;
    }
    return acc / wsum;
}

// Contact shadow: short march towards the sun in screen space (1 = lit).
float ScreenContact(PSIn i, float3 rel, float3 n, float3 Ld, float dd, float dist, float noise, float4 X, float4 Y)
{
    if (P0.x <= 0) return 1;
    float3 toL = -normalize(Ld);
    float steps = 12;
    float stepLen = P0.y / steps;
    float3 start = rel + n * P2.z;
    float occ = 0;
    RayProj rp = ProjSetup(start, toL, i.t1, X, Y, dd);
    bool alive = true;
    [unroll] for (int s = 1; s <= 12; ++s) {   // (1.65: read together; the first hit is the highest occ)
        float t = (s - 1 + noise) * stepLen + P2.z;
        float2 dp; float4 rayAt;
        alive = alive && ProjAt(rp, t, i.t1, X, Y, dp, rayAt);
        float2 uv = (i.pos.xy + dp) / Size.xy;
        float d = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
        alive = alive && all(uv >= 0) && all(uv <= 1) && abs(d) >= 1e-9;
        float diffM = length(start + toL * t) - length(rayAt.xyz / d);
        if (alive && diffM > 0.02 && diffM < P0.z) occ = max(occ, 1 - (s - 1) / steps);
    }
    return 1 - P0.x * occ * saturate(1 - dist / 150.0);
}

float HalfContact(PSIn i, float3 rel, float3 n, float3 Ld, float dd, float dist, float noise, float4 X, float4 Y)
{
    float c = 1;
    [branch] if (dot(n, -Ld) > 0) c = ScreenContact(i, rel, n, Ld, dd, dist, noise, X, Y);
    return c;
}

// Ambient occlusion: a few samples in the hemisphere around the normal (1 = open).
// 1.66 (AOBitmask): ambient occlusion as a visibility bitmask (Therrien, Levesque, Gilet 2023). Per slice (a plane
// through the normal) 32 sectors of the half-circle above the surface, spaced so that each holds the same share of
// cosine-weighted sky ((1 - cos a) / 2 per sector): what a sample covers is the angles from its surface's front to
// its back (the front pushed away from the camera by a thickness), so light passes behind thin things (posts, rails,
// branches) instead of a whole horizon closing above them; the occlusion is the share of sectors set. 2 slices x
// 2 sides x 3 steps (12 depth reads, read together).
uint BitCount(uint v)   // (countbits is shader model 5)
{
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}
float ScreenAOBitmask(PSIn i, float3 rel, float3 n, float dd, float dist, float noise, float4 X, float4 Y)
{
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    float R = P1.w, thick = clamp(R * 0.5, 0.3, 0.5);
    float3 o = rel + n * 0.03;
    float occ = 0;
    [unroll] for (int sl = 0; sl < 2; ++sl) {
        float phi = (sl + noise) * 1.5707963;
        float3 t = tx * cos(phi) + ty * sin(phi);          // the slice: angles from +t (0) over n (pi/2) to -t (pi)
        uint bits = 0;
        [unroll] for (int side = 0; side < 2; ++side) {
            float3 dir = side ? -t : t;
            RayProj rp = ProjSetup(o, dir, i.t1, X, Y, dd);
            [unroll] for (int k = 0; k < 3; ++k) {
                float f = (k + frac(noise * 5.31 + sl * 0.37 + side * 0.61)) / 3;
                float r = R * (0.1 + 0.9 * f * f);
                float2 dp; float4 rayAt;
                bool ok = ProjAt(rp, r, i.t1, X, Y, dp, rayAt);
                float2 uv = (i.pos.xy + dp) / Size.xy;
                float z = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
                ok = ok && all(uv >= 0) && all(uv <= 1) && abs(z) >= 1e-9;
                float3 S = rayAt.xyz / z;                         // the surface seen there (camera-relative)
                float3 B = S + normalize(S) * thick;              // its back
                float3 dS = S - rel, dB = B - rel;
                // in front of the sample point (it is inside something) and near: it occludes
                ok = ok && length(o + dir * r) - length(S) > 0.02 && dot(dS, dS) < 4 * R * R;
                float aS = atan2(max(dot(dS, n), 0), dot(dS, t)), aB = atan2(max(dot(dB, n), 0), dot(dB, t));
                float u0 = (1 - cos(min(aS, aB))) * 0.5, u1 = (1 - cos(max(aS, aB))) * 0.5;
                uint s0 = (uint)clamp(floor(u0 * 32), 0, 32), s1 = (uint)clamp(ceil(u1 * 32), 0, 32);
                uint cnt = s1 > s0 ? s1 - s0 : 0;
                uint m = cnt >= 32 ? 0xFFFFFFFFu : ((1u << cnt) - 1) << s0;
                if (ok) bits |= m;
            }
        }
        occ += BitCount(bits) / 32.0;
    }
    return 1 - P1.z * saturate(occ * 0.5) * saturate(1 - dist / 200.0);
}

float ScreenAO(PSIn i, float3 rel, float3 n, float dd, float dist, float noise, float4 X, float4 Y)
{
    if (P1.z <= 0) return 1;
    if (P25.z > 0.5) return ScreenAOBitmask(i, rel, n, dd, dist, noise, X, Y);
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    float occ = 0;
    [unroll] for (int a = 0; a < 8; ++a) {
        float ang = (a + noise) * 0.785398;
        float rr = P1.w * (0.3 + 0.7 * frac(a * 0.618 + noise));
        float3 dir = normalize(tx * cos(ang) + ty * sin(ang) + n * (0.4 + 0.5 * frac(a * 0.37 + noise)));
        float3 q = rel + n * 0.03 + dir * rr;
        float2 dp; float4 rayAt;
        if (!Project(q, i.t1, X, Y, dd, dp, rayAt)) continue;
        float sd = SceneDist(i.pos.xy + dp, rayAt);
        if (sd < 0) continue;
        float diffM = length(q) - sd;
        if (diffM > 0.03 && diffM < P1.w * 1.5) occ += 1 - saturate(diffM / (P1.w * 1.5));
    }
    return 1 - P1.z * saturate(occ / 8 * 1.6) * saturate(1 - dist / 200.0);
}

float Lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- modern lighting (1.08, P16.x): GGX specular (Smith height-correlated visibility, Schlick Fresnel), Burley
// diffuse, and the sky reflected by rough surfaces (split sum). The game shades with normalised Blinn-Phong and a
// specular colour that is nearly black on roads, walls and stone: they had no highlight and no sheen at a grazing
// angle at all. Here every dielectric reflects at least 4% (P16.y x 0.04: stone, asphalt, wood, plastic); the game's
// specular colour is kept where it is stronger (paint, metal, glass). Roughness from the game's gloss exponent
// (power = 2^(11 (1 - a))): GGX alpha = sqrt(2 / (power + 2)), the usual Blinn-Phong match, x P16.w.
float PbrAlpha(float power) { return clamp(sqrt(2 / (power + 2)) * P16.w, 0.03, 1); }
// 1.96h: a pixel at a step in depth: a thing's edge, its neighbour across another surface (the 4 neighbours' distances:
// their second difference near the first, a step, not a slope: a far road stays a slope; and a jump past 3% + 5 cm).
// The normal's pixel-to-pixel spread there is the edge, not bumps: the anti-shimmer below made such pixels rough, and a
// rough reflection there is the blurred bright sky: the white 1-pixel rims round bins, signs, cars (the user found it).
float DepthStep(PSIn i, float depth)
{
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float2 du = ddx(i.t0.xy), dv = ddy(i.t0.xy);
    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return 0;
    float dist = length(i.t1.xyz / dd);
    float4 zs = float4(tDepth.SampleLevel(sPoint, i.t0.xy - du, 0).x, tDepth.SampleLevel(sPoint, i.t0.xy + du, 0).x,
                       tDepth.SampleLevel(sPoint, i.t0.xy - dv, 0).x, tDepth.SampleLevel(sPoint, i.t0.xy + dv, 0).x);
    float4 dn;
    dn.x = length((i.t1.xyz - X.xyz) / max(abs(zs.x - (i.t1.w - X.w)), 1e-9));
    dn.y = length((i.t1.xyz + X.xyz) / max(abs(zs.y - (i.t1.w + X.w)), 1e-9));
    dn.z = length((i.t1.xyz - Y.xyz) / max(abs(zs.z - (i.t1.w - Y.w)), 1e-9));
    dn.w = length((i.t1.xyz + Y.xyz) / max(abs(zs.w - (i.t1.w + Y.w)), 1e-9));
    dn = min(dn, 1e5);
    float tolE = dist * 0.03 + 0.05;
    float2 c1 = abs(dn.xz - dn.yw), c2 = abs(dn.xz + dn.yw - 2 * dist);
    float2 st = saturate((c2 / max(c1, 1e-4) - 0.35) * 4) * saturate(max(abs(dn.xz - dist), abs(dn.yw - dist)) / tolE - 1);
    return max(st.x, st.y);
}
// 1.43: highlights' anti-shimmer (specular anti-aliasing): where the normal turns fast from pixel to pixel (stones' and
// tiles' normal maps, far away) the highlight's lobe widened by that spread: no single bright pixels flickering.
// (1.96h: not at a depth step: edge = DepthStep)
float SpecAA(float power, float3 n, float edge)
{
    if (P22.x <= 0) return power;
    float3 dx = ddx(n), dy = ddy(n);
    float a2 = 2 / (power + 2) + min(P22.x * 0.5 * (dot(dx, dx) + dot(dy, dy)) * (1 - edge), 1);
    return max(2 / a2 - 2, 0.5);
}
float3 PbrF0(float4 Sp) { return max(Sp.rgb, 0.04 * P16.y); }
// D x V x F (multiply by N.L and the light)
float3 PbrSpecK(float3 n, float3 V, float3 l, float3 F0, float alpha)
{
    float3 H = normalize(V + l);
    float ndl = saturate(dot(n, l)), ndv = max(dot(n, V), 1e-3), ndh = saturate(dot(n, H)), vdh = saturate(dot(V, H));
    float a2 = alpha * alpha;
    float dd = ndh * ndh * (a2 - 1) + 1;
    float D = a2 / (3.14159265 * dd * dd);
    float vis = 0.5 / (ndl * sqrt(ndv * ndv * (1 - a2) + a2) + ndv * sqrt(ndl * ndl * (1 - a2) + a2) + 1e-5);
    float f = 1 - vdh, f5 = f * f * f * f * f;
    return D * vis * (F0 + (1 - F0) * f5);
}
// Burley's diffuse factor (x albedo / pi x N.L): rough surfaces a little brighter towards grazing light and view
float PbrDiffK(float3 n, float3 V, float3 l, float alpha)
{
    float ndl = saturate(dot(n, l)), ndv = saturate(dot(n, V));
    float ldh = saturate(dot(l, normalize(V + l)));
    float fd90 = 0.5 + 2 * sqrt(alpha) * ldh * ldh;
    float fl = 1 - ndl, fv = 1 - ndv;
    fl *= fl * fl * fl * fl; fv *= fv * fv * fv * fv;
    return (1 + (fd90 - 1) * fl) * (1 + (fd90 - 1) * fv);
}
// the environment's share reflected (split sum, Karis' analytic fit of its lookup table): F0 x A + B
float3 PbrEnv(float3 F0, float rough, float ndv)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022), c1 = float4(1, 0.0425, 1.04, -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
    return F0 * AB.x + AB.y;
}
TextureCube tGameIrr : register(t63);   // the game's sky-light cube (its sun pass reads it by the normal; 4 x c^2 = radiance)
#define F_IRR 512


// ---- rain on surfaces. Wet only where rain falls and stays:
// - facing up;
// - open to the sky: nothing above it in the rain map (rain.inc: the city's height from straight above, 25 cm), and
//   not inside a car's cabin (the cars' places: the road under a car stays wet);
// - by material (the albedo): green (grass, leaves) takes almost none, colourful porous ones (soil, brick) less; grey
//   (asphalt, concrete, stone) the most;
// - puddles: patches (value noise over the ground) on the grey ones: mirror-like, fully flat.
cbuffer RainCB : register(b13) { float4 RN0, RN1, RN2; float4 RC[48]; float4 RN3, RN4; float4 RP[3]; };   // map centre x, z, half size, top | range, valid, cars, texel | wetness, dry gloss | cars: place, x, z | 1.47: big matte puddles: amount, size, gloss; 1.48: water
// 1.73: how far a floor point is under the player's car (its footprint + 0.3 m, soft edges; floor height only): the sky
// is not reflected there (the game's own gloss lit the wet road under it with its sky, ours too: a pale glow under the car)
// 1.78: the player's car's matrix of THIS frame (t77: copied on the GPU from its own draw: rows x, y, z axes, place);
// without it the read-back's (RC[0]: a few frames old, carried on at its speed: drops slid along the road)
Buffer<float4> tPlayerMat : register(t77);
bool PlayerPose(out float3 P, out float3 ax, out float3 az)
{
    if (P29.y > 0.5) { ax = normalize(tPlayerMat.Load(0).xyz); az = normalize(tPlayerMat.Load(2).xyz); P = tPlayerMat.Load(3).xyz; return true; }
    P = RC[0].xyz; ax = normalize(RC[1].xyz); az = normalize(RC[2].xyz);
    return RN4.w > 0.5 && RN1.z > 0.5;
}
// a point on the player's car's body (its extent from its place at the floor), by a pose
bool InPlayerBody(float3 w, float3 P, float3 ax, float3 az)
{
    float3 l = w - P;
    float lx = dot(l, ax), lz = dot(l, az);
    return abs(lx) < 1.05 && abs(lz) < 2.45 && l.y > 0.05 && l.y < 1.5;
}
float UnderPlayer(float3 w)
{
    float3 Pp, ax, az;
    if (P33.z < 0.5 || !PlayerPose(Pp, ax, az)) return 0;   // (1.84: UnderCarDark off)
    float3 l = w - Pp;
    // 1.76: a rounded rectangle (corners 0.55 m), soft edges (it was a box: a dark rectangle in the puddles)
    float2 q = abs(float2(dot(l, ax), dot(l, az))) - float2(0.4, 1.7);
    float sd = length(max(q, 0)) + min(max(q.x, q.y), 0) - 0.55;
    return (1 - smoothstep(-0.3, 0.35, sd)) * saturate((0.4 - l.y) / 0.3);
}
Texture2D<float> tRain : register(t51);
float RainOpen(float3 world);
float RainOpenAt(float3 world) { return RainOpen(world); }
float RainOpen(float3 world)
{
    if (RN1.y < 0.5) return 1;
    float2 uv = float2((world.x - RN0.x) / RN0.z * 0.5 + 0.5, 0.5 - (world.z - RN0.y) / RN0.z * 0.5);
    if (any(uv < 0.01) || any(uv > 0.99)) return 1;
    // 1 + 18 samples on three rings (0.9, 2, 3.5 m), turned by a hash of the place (no stair steps from the 0.5 m
    // texels); a soft height test. Under a roof the wetness fades over ~3.5 m from its edge (rain blowing in)
    float2 hp = frac(world.xz * float2(0.1031, 0.1030));
    hp += dot(hp, hp.yx + 33.33);
    float ang = frac((hp.x + hp.y) * hp.x) * 6.2832;
    float m2u = 1 / (2 * RN0.z);   // metres -> uv
    float o = 0, wsum = 0;
    [unroll] for (int k = 0; k < 19; ++k) {
        int ring = k == 0 ? 0 : (k - 1) / 6 + 1;
        float r = ring == 0 ? 0 : ring == 1 ? 0.9 : ring == 2 ? 2.0 : 3.5;
        float a = ang + k * 1.0472 + ring * 0.37;
        float2 off = float2(cos(a), sin(a)) * r * float2(1, -1) * m2u;
        float top = RN0.w - tRain.SampleLevel(sPoint, uv + off, 0) * RN1.x;
        float w = ring == 0 ? 2.5 : ring == 1 ? 1 : ring == 2 ? 0.8 : 0.6;
        o += saturate((world.y - top + 0.6) / 0.5) * w;
        wsum += w;
    }
    float f = o / wsum;
    return f * f * (3 - 2 * f);
}
// ---- the game's gloss, before its lighting (drawn with the sky dome, right after the G-buffer): the game gives nearly
// every surface a high gloss with a blotchy pattern (its lighting reflects the sky in it: "clouds" on dry asphalt,
// under roofs too). Here its G-buffer gloss (the normal target's alpha) is multiplied: dry floors / walls by the dry
// gloss setting, wet floors keep it, car paint / metal (a bright specular colour) keep it. The place from the dome's
// direction and the depth (solved through this frame's viewProjection), the facing from the place's derivatives.
struct DomeIn { float4 pos : SV_Position; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };
Buffer<float4> tDomeVP : register(t52);       // the sky VS's viewProjection rows 0..3, the dome's world rows 4..7 (7: camera)
Texture2D<float> tGDepth : register(t53);     // the game's depth
Texture2D<float4> tGSpec : register(t54);     // the game's specular colour
float InCabin(float3 world);
float RainOpenAt(float3 world);
Texture2D<float4> tGAlb : register(t55);      // the game's albedo
float WetNoise(float2 p);
float BigPuddle(float3 world);
// the new gloss = the game's x a + b: wet floors get an even gloss of our own (higher in puddles: the game's blotchy
// gloss pattern, which its lighting fills with the sky, goes), dry ones keep DryGloss of the game's, paint / metal
// keep theirs. The wet amount follows the rain map's gradient and the material (grass takes little).
float2 GlossFix(DomeIn i)
{
    int2 p = int2(i.pos.xy);
    float z = tGDepth.Load(int3(p, 0));
    float3 d = normalize(i.t0.xyz);
    float4 R0 = tDomeVP.Load(0), R1 = tDomeVP.Load(1), R2 = tDomeVP.Load(2), R3 = tDomeVP.Load(3);
    float3 cam = tDomeVP.Load(7).xyz;
    float4 A = cam.x * R0 + cam.y * R1 + cam.z * R2 + R3, B = d.x * R0 + d.y * R1 + d.z * R2;
    float t = (A.z - z * A.w) / (z * B.w - B.z);
    float3 P = cam + d * t;
    float3 n = normalize(cross(ddy(P), ddx(P)));
    if (dot(n, d) > 0) n = -n;
    if (z >= 0.99999 || !(t > 0)) return float2(1, 0);
    float paint = saturate((dot(tGSpec.Load(int3(p, 0)).rgb, float3(0.2126, 0.7152, 0.0722)) - 0.04) * 25);
    float up = saturate((n.y - 0.7) * 5);
    float3 alb = tGAlb.Load(int3(p, 0)).rgb;
    float mx = max(alb.r, max(alb.g, alb.b)), mn = min(alb.r, min(alb.g, alb.b));
    float green = smoothstep(0.08, 0.25, (alb.g - max(alb.r, alb.b)) / max(alb.g, 1e-3));
    float grey = 1 - saturate((mx - mn) / max(mx, 1e-3) * 2.5 - 0.2);
    float asph = 1 - smoothstep(0.16, 0.3, Lum(alb));
    float mat = 1 - green * 0.95;
    float wet = RN2.x > 0 && up > 0 ? RainOpenAt(P) * (1 - InCabin(P)) * up * mat * saturate(RN2.x) : 0;
    float w = smoothstep(0, 0.8, wet);
    float nz = WetNoise(P.xz / 3.2) * 0.65 + WetNoise(P.xz / 0.9 + 17.3) * 0.35;
    // (1.47: puddles as Wetness's: the small mirrors and the big matte ones, on road-dark floors)
    float thrM = lerp(0.7, 0.42, saturate(RN4.x));   // (1.49: the small ones' size)
    float pm = smoothstep(thrM, thrM + 0.14, nz) * RN2.z, pb = BigPuddle(P);
    float puddle = saturate(max(pm, pb) * (1 - smoothstep(0.25, 0.65, Lum(alb))) * (1 - green));
    float rim = smoothstep(thrM - 0.1, thrM + 0.02, nz) * (1 - smoothstep(thrM, thrM + 0.14, nz)) * RN2.z * (1 - smoothstep(0.25, 0.65, Lum(alb))) * (1 - green);
    if (RN2.x > 0 && up > 0) w = max(w, saturate(rim * 1.6) * mat * up * RainOpenAt(P) * (1 - InCabin(P)));   // (1.72: the puddle's soaked rim)
    float target = lerp(RN2.w, lerp(RN3.z, RN4.y, saturate(pm / max(max(pm, pb), 1e-4))), puddle);   // wet asphalt: rough; puddles: their gloss (1.43: WetLightGloss)
    w *= 1 - 0.85 * UnderPlayer(P) * up;   // (1.73: no wet gloss under the player's car)
    float a = (1 - w) * RN2.y, bb = w * target;
    return lerp(float2(a, bb), float2(1, 0), paint);
}
float4 PS_WetGlossMul(DomeIn i) : SV_Target { return float4(0, 0, 0, GlossFix(i).x); }   // blend: dest.a x this
float4 PS_WetGlossAdd(DomeIn i) : SV_Target { return float4(0, 0, 0, GlossFix(i).y); }   // blend: dest.a + this

float InCabin(float3 world)
{
    [loop] for (int k = 0; k < (int)RN1.z; ++k) {
        float3 l = world - RC[k * 3].xyz;
        float lx = dot(l, RC[k * 3 + 1].xyz), lz = dot(l, RC[k * 3 + 2].xyz);   // (RC: after RN0..2)
        if (abs(lx) < 0.75 && lz > -1.3 && lz < 0.8 && l.y > 0.4 && l.y < 1.2) return 1;   // (1.90: from 0.4 m, was 0.25)
    }
    return 0;
}
// 1.50: on a car's body (the cars near the camera, RC: the player's and the traffic's): no relief there (their smooth
// normals over flat facets read as bumps: facets and dark lines round handles and seams)
// 1.90: from 0.3 m over its place (was 0.06: a car's place is at the road or a little under it, its suspension down:
// the road round it (2.9 x 8.7 m) was taken for its body: no wetness, puddles or reflections there, a sharp rectangle)
float OnCar(float3 world)
{
    [loop] for (int k = 0; k < (int)RN1.z; ++k) {
        float3 l = world - RC[k * 3].xyz;
        float lx = dot(l, RC[k * 3 + 1].xyz), lz = dot(l, RC[k * 3 + 2].xyz);
        if (abs(lx) < 1.45 && lz > -4.3 && lz < 4.4 && l.y > 0.3 && l.y < 3.3) return 1;
    }
    return 0;
}
// 1.59: the player's car (RC[0] when RN4.w): a point on it, where it was a frame ago (RP: its place and axes then, w:
// known): the frames' history on its body is found by the car's own motion, not by the point's place in the world (a
// door slides along its own plane: the history held the door's part half a metre back at speed: trails)
bool OnPlayer(float3 world)
{
    if (RN4.w < 0.5 || RP[0].w < 0.5) return false;
    float3 l = world - RC[0].xyz;
    float lx = dot(l, RC[1].xyz), lz = dot(l, RC[2].xyz);
    return abs(lx) < 1.45 && lz > -4.3 && lz < 4.4 && l.y > 0.2 && l.y < 3.3;   // (1.90: from 0.2 m: the road under it not carried with it)
}
float3 PlayerPrev(float3 world)
{
    float3 l = world - RC[0].xyz;
    float lx = dot(l, RC[1].xyz), lz = dot(l, RC[2].xyz);
    return RP[0].xyz + RP[1].xyz * lx + RP[2].xyz * lz + (l - RC[1].xyz * lx - RC[2].xyz * lz);
}
// the world point to look up in the last frame (the player's car: where it was)
// 1.60: not its wheels (they spin: the car's motion puts their history on another spoke: brighter wheels), nor the low
// parts that are not paint (tyres, lights, plastic): no history there (a point far off: the look-up fails)
float3 HistPoint(float3 world, float2 uv)
{
    if (!OnPlayer(world)) return world;
    if (world.y - RC[0].y < 0.8 && ((L2.z & F_SPEC) == 0 || PaintOf(tSpec.SampleLevel(sPoint, uv, 0)) < 0.3)) return world + float3(0, 1e4, 0);
    return PlayerPrev(world);
}
float WetHash(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float WetNoise(float2 p)
{
    float2 c = floor(p), f = frac(p);
    f = f * f * (3 - 2 * f);
    return lerp(lerp(WetHash(c), WetHash(c + float2(1, 0)), f.x), lerp(WetHash(c + float2(0, 1)), WetHash(c + 1), f.x), f.y);
}
// 1.47: big matte puddles (as the game's own wet patches: wide, a blurred reflection): a wider noise (12 m, 4 m, edges
// 1.2 m), cut by their size (RN3.y), x their amount (RN3.x); their gloss RN3.z
float BigPuddle(float3 world)
{
    if (RN3.x <= 0) return 0;
    float nz = WetNoise(world.xz / 12.0 + 41.7) * 0.65 + WetNoise(world.xz / 4.0 + 5.1) * 0.25 + WetNoise(world.xz / 1.2 + 9.3) * 0.1;
    float thr = lerp(0.72, 0.4, saturate(RN3.y));
    return smoothstep(thr, thr + 0.1, nz) * RN3.x;
}
// the game's exposure (the 1x1 HDRToneMapping texture's w, copied each frame): the debug views divide by it, so they
// look the same by day and by night (the game multiplies the whole image by it before its clip and LUT)
Texture2D<float4> tExpo : register(t56);
float DbgScale() { float w = tExpo.Load(int3(0, 0, 0)).w; return w > 1e-4 && w < 1e4 ? 1 / w : 1; }
// the surface's own normal from the depth (world space, facing the camera): the wetness by slope must not follow
// the normal map (tiles and cobbles tilt it per texel: blotches); (0, 0, 0) where it is not known
float3 GeoN(PSIn i, float depth)
{
    float dd = depth - i.t1.w;
    dd = abs(dd) < 1e-9 ? 1e-9 : dd;
    float3 r = i.t1.xyz / dd;
    float3 g = cross(ddy(r), ddx(r));
    float l = length(g);
    if (!(l > 1e-20) || abs(depth - i.t1.w) < 1e-9) return 0;
    g /= l;
    return dot(g, r) > 0 ? -g : g;
}
// 1.40: how up a surface faces, for the wetness: its own (geometric) normal where known, else the normal map's (leaves'
// normal maps face up: trees got wet)
float3 WetUp(float3 n, float3 gN) { return float3(0, dot(gN, gN) > 0.25 ? gN.y : n.y, 0); }
// the wetness 0..1 (WetGround x roads 1, other floors WetOther, grass 0.05) and puddles (Puddles: on asphalt-dark floors,
// 1.40: not scaled by WetGround, and on lighter asphalt too)
// (1.47: pGloss: the puddles' gloss there: the small ones mirrors (0.95), the big ones matte (PuddleBigGloss);
// 1.48: pWater: how much of it is standing water (the small ones 1, the big ones PuddleBigWater; 0: just damp))
float Wetness(float3 rel, float3 n, float4 ray, float4 X, float4 Y, float sgn, float2 pix, float3 alb, float3 world, out float puddle, out float covered, out float pGloss, out float pWater)
{
    puddle = 0; covered = 0; pGloss = 0.95; pWater = 1;
    float up = saturate((n.y - 0.7) * 5);
    if ((P12.y <= 0 && P11.w <= 0 && RN3.x <= 0) || up <= 0) return 0;
    float open = RainOpen(world) * (1 - InCabin(world));
    covered = up * (1 - open);   // a floor under a roof: dry (none of the game's own sheen either)
    float mx = max(alb.r, max(alb.g, alb.b)), mn = min(alb.r, min(alb.g, alb.b));
    float col = (mx - mn) / max(mx, 1e-3);
    float green = smoothstep(0.08, 0.25, (alb.g - max(alb.r, alb.b)) / max(alb.g, 1e-3));
    float grey = 1 - saturate(col * 2.5 - 0.2);
    float asph = 1 - smoothstep(0.16, 0.3, Lum(alb));   // dark asphalt (not paving slabs, tiles, kerbs)
    float mat = (1 - green * 0.95) * lerp(P21.y, 1, asph);
    float wet = saturate(P12.y) * up * open * mat;
    float nz = WetNoise(world.xz / 3.2) * 0.65 + WetNoise(world.xz / 0.9 + 17.3) * 0.35;
    float road = 1 - smoothstep(0.25, 0.65, Lum(alb));   // (1.41: soft, no colourfulness: the texels' detail made grain)
    float thrM = lerp(0.7, 0.42, saturate(RN4.x));   // (1.49: the small ones' size, gloss, water: RN4)
    float pm = smoothstep(thrM, thrM + 0.14, nz) * P11.w, pb = BigPuddle(world);   // P11.w: the small ones
    puddle = saturate(max(pm, pb) * road * (1 - green) * up * open);
    // 1.72: a puddle's rim: a band of soaked asphalt round it (wet, dark, glossy, not yet a mirror), as real puddles have
    float rim = smoothstep(thrM - 0.1, thrM + 0.02, nz) * (1 - smoothstep(thrM, thrM + 0.14, nz)) * P11.w * road * (1 - green) * up * open;
    wet = max(wet, saturate(rim * 1.6) * mat);
    float share = saturate(pm / max(max(pm, pb), 1e-4));   // the small mirrors' share
    pGloss = lerp(RN3.z, RN4.y, share);
    pWater = lerp(RN3.w, RN4.z, share);
    return wet;
}

// The view ray over the screen is affine: t1 = A + B*u + C*v (u, v = 0..1). Recorded every frame, so a
// world point can be found on the previous frame's screen.
float4 PS_Cam(PSIn i) : SV_Target0
{
    float4 B = ddx(i.t1) * 4, C = ddy(i.t1) * 2;          // drawn into a 4x2 target
    float4 A = i.t1 - B * (i.pos.x / 4) - C * (i.pos.y / 2);
    int x = int(i.pos.x);
    if (i.pos.y > 1) {   // second row: the surface at the screen centre
        float4 t1c = A + 0.5 * B + 0.5 * C;
        float dd = tDepth.SampleLevel(sPoint, float2(0.5, 0.5), 0).x - t1c.w;
        return abs(dd) < 1e-9 ? 0 : float4(t1c.xyz / dd + G[L1.w].xyz, 1);
    }
    return x == 0 ? A : x == 1 ? B : x == 2 ? C : float4(G[L1.w].xyz, 0);
}

// Where was a world point on the previous frame's screen? uv 0..1 and its distance to that camera.
bool Reproject(float3 world, float sgn, out float2 uv, out float d)
{
    float4 A = tCamPrev.Load(int3(0, 0, 0)), B = tCamPrev.Load(int3(1, 0, 0)), C = tCamPrev.Load(int3(2, 0, 0));
    float3 rel = world - tCamPrev.Load(int3(3, 0, 0)).xyz;
    d = length(rel);
    uv = -1;
    float3 c0 = B.xyz, c1 = C.xyz, c2 = -rel, b = -A.xyz;     // A + B*u + C*v = s*rel
    float3 c12 = cross(c1, c2);
    float det = dot(c0, c12);
    if (abs(det) < 1e-12) return false;
    uv = float2(dot(b, c12), dot(c0, cross(b, c2))) / det;
    float s = dot(c0, cross(c1, b)) / det;
    return s * sgn > 0 && all(uv > 0) && all(uv < 1);
}

// One bounce of light: the lit surfaces around a point, found on screen like the AO samples.
// Their light comes from the previous frame's finished image (every light: sun, street lamps, headlights,
// signs) found by reprojection; where that fails, from the sun alone, computed here.
// Returns the light arriving from them (radiance average, before the receiver's albedo).
float3 ScreenGI(PSIn i, float3 rel, float3 n, float dd, float4 X, float4 Y, out float2 src)
{
    src = 0;   // debug: share of samples lit from the previous frame / from the sun alone
    float3 Lc = SunLc(), Ld = SunLd();
    bool prev = P6.z > 0.5 && P7.x > 0.5;
    if (!prev && dot(Lc, float3(1, 1, 1)) <= 0) return 0;
    float3 cam = G[L1.w].xyz;
    // very bright spots (sun glints on paint) would splash: soft limit at 16x the average of the last frame
    float limit = prev ? 16 * Lum(tRadPrev.SampleLevel(sLin, float2(0.5, 0.5), 16).rgb) + 1e-4 : 1;
    float R = P6.y;
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    int2 p4 = int2(i.pos.xy) & 3;
    float j = (p4.x + p4.y * 4 + frac(P2.w * 0.618034)) / 16;   // 4x4 pattern (blur pass), shifted every frame (history)
    float flatSun = (!(L2.z & F_SHADOW) && L0.w >= 0) ? G[L0.w].y * G[L0.w].y : 1;
    float3 acc = 0;
    [loop] for (int a = 0; a < 8; ++a) {
        float u = frac(a * 0.618034 + j);
        float phi = (a + j) * 2.399963;                  // golden angle
        float r = R * (0.1 + 0.9 * frac(a * 0.381966 + j * 0.7));
        float3 dir = (tx * cos(phi) + ty * sin(phi)) * sqrt(u) + n * sqrt(1 - u);   // cosine-weighted
        float3 q = rel + n * 0.05 + dir * r;
        float2 dp; float4 rayAt;
        if (!Project(q, i.t1, X, Y, dd, dp, rayAt)) continue;
        float2 uv = (i.pos.xy + dp) / Size.xy;
        if (any(uv < 0) || any(uv > 1)) continue;
        float sdd = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
        if (abs(sdd) < 1e-9) continue;
        float3 srel = rayAt.xyz / sdd;                   // the surface seen behind the sample
        float th = length(q) - length(srel);             // > 0: the sample is inside it, the ray hit it
        if (th < 0.02 || th > 2 * R) continue;
        float3 d = srel - rel;
        float dl = length(d);
        if (dl < 0.01 || dl > 2 * R) continue;
        float3 ns = normalize(tNormal.SampleLevel(sPoint, uv, 0).xyz * 2 - 1);
        if (dot(ns, d) >= 0) continue;                   // its back side
        float fall = saturate(2 - dl / R);
        float2 puv; float pd;
        if (prev && Reproject(srel + cam, dd, puv, pd)) {
            float4 r = tRadPrev.SampleLevel(sPoint, puv, 0);
            if (abs(r.w - pd) < pd * 0.03 + 0.15) {      // the same surface was there
                acc += r.rgb / (1 + Lum(r.rgb) / limit) * fall;
                src.x += 1;
                continue;
            }
        }
        float ndlS = saturate(dot(ns, -Ld));
        if (ndlS <= 0) continue;
        float4 Ds = tDiffuse.SampleLevel(sPoint, uv, 0);
        float vis = (L2.z & F_SHADOW) ? SunVisibility(srel + cam + ns * 0.1 - Ld * 0.2) : flatSun;
        float alphaT = saturate(1 - (1 - Ds.w) * 0.8);
        acc += Ds.rgb * Ds.rgb * Lc * (ndlS * vis * alphaT * 0.318310) * fall;
        src.y += 1;
    }
    src /= 8;
    return acc / 8;
}

// Light of the detected lamps at a point (radiance-like: irradiance / pi, before the albedo).
// Light of the detected / mapped lamps at a point, shaded like the game shades its sun: the diffuse model plus
// normalised Blinn-Phong highlights with Fresnel (glossy paint keeps its gloss next to a lamp).
// V = view ray direction as the game uses it, Sp = specular buffer (rgb F0, w Fresnel), power = gloss exponent.
// Screen-space shadow of a lamp: a march from the pixel towards the lamp through the depth buffer (1 = lit).
// Only what is on screen can cast it; a march leaving the screen counts as lit. rel / lampRel: from the camera.
float LampVis(PSIn i, float3 rel, float3 n, float3 lampRel, float dd, float4 X, float4 Y, float noise)
{
    float3 toL = lampRel - rel;
    float L = length(toL);
    toL /= max(L, 1e-4);
    float maxT = L - 0.6;   // not the lamp's own fitting
    if (maxT < 0.2) return 1;
    float3 start = rel + n * 0.05;
    float thick = P9.y + length(rel) * 0.01;
    RayProj rp = ProjSetup(start, toL, i.t1, X, Y, dd);
    float prevT = 0, prevDiff = -1;
    bool alive = true, hit = false;
    [unroll] for (int s = 1; s <= 12; ++s) {
        float f = (s - 1 + noise) / 12;
        float t = 0.08 + f * f * maxT;   // denser near the pixel (contact), sparser towards the lamp
        float2 dp; float4 rayAt;
        alive = alive && ProjAt(rp, t, i.t1, X, Y, dp, rayAt);
        float2 uv = (i.pos.xy + dp) / Size.xy;
        float d = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
        alive = alive && all(uv >= 0) && all(uv <= 1) && abs(d) >= 1e-9;   // (SceneDist: off the screen, the sky)
        float diffM = length(start + toL * t) - length(rayAt.xyz / d);
        // in front of a surface within its thickness, or stepped from behind it to in front (the window grows with the step)
        hit = hit || (alive && ((diffM > 0.04 + t * 0.01 && diffM < thick) || (prevDiff > 0.04 && diffM <= 0 && prevDiff < thick + 0.5 * (t - prevT))));
        prevT = t; prevDiff = diffM;
    }
    return hit ? 1 - P9.x : 1;
}

// A point seen from a lamp: its face, uv and slice in the lamp's map, and its depth along the face (the plugin's
// LsAxes table). Above the lamp (no face): false.
bool LsFace(float3 v, float slot, out float3 uvs, out float d)
{
    float3 av = abs(v);
    float3 f, r, u;
    float face;
    uvs = 0; d = 0;
    if (av.y >= av.x && av.y >= av.z) {
        if (v.y > 0) return false;
        face = 4; f = float3(0, -1, 0); r = float3(1, 0, 0); u = float3(0, 0, 1);
    } else if (av.x >= av.z) {
        u = float3(0, 1, 0);
        if (v.x > 0) { face = 0; f = float3(1, 0, 0); r = float3(0, 0, -1); } else { face = 1; f = float3(-1, 0, 0); r = float3(0, 0, 1); }
    } else {
        u = float3(0, 1, 0);
        if (v.z > 0) { face = 2; f = float3(0, 0, 1); r = float3(1, 0, 0); } else { face = 3; f = float3(0, 0, -1); r = float3(-1, 0, 0); }
    }
    d = dot(f, v);
    float2 c = float2(dot(r, v), dot(u, v)) / (d * kLsTan);
    uvs = float3(0.5 + 0.5 * c.x, 0.5 - 0.5 * c.y, slot * 5 + face);
    return true;
}

float LsDepthAt(float3 uvs, float2 off, float a)
{
    float z = tLampSM.SampleLevel(sPoint, float3(saturate(uvs.xy + off), uvs.z), 0);
    return kLsNear * a / max(a - z, 1e-6);
}

// Shadow from a lamp's map (1 = lit). soft: blocker search, then a filter as wide as the lamp's size makes the
// penumbra at that distance (sharp at the foot of a post, soft further away); else 4 taps.
float LampMapVis(float3 world, float3 n, float3 lampPos, float slot, float noise, bool soft)
{
    float3 uvs; float d;
    if (!LsFace(world + n * 0.04 - lampPos, slot, uvs, d)) return 1;
    float a = P8.z / (P8.z - kLsNear);
    float texel = 1.0 / 256;
    float texelW = 2 * kLsTan * d * texel;          // a texel's width at the receiver, m
    float dr = d - (0.03 + d * 0.01 + texelW * 1.5);
    float sn, cs;
    sincos(noise * 6.2832, sn, cs);
    float2x2 rot = float2x2(cs, -sn, sn, cs);
    if (!soft) {
        float lit = 0;
        [unroll] for (int t = 0; t < 2; ++t) lit += LsDepthAt(uvs, mul(rot, kPoisson[t * 8]) * 1.2 * texel, a) >= dr ? 1 : 0;
        return lit / 2;
    }
    float search = clamp(P9.w * 0.5 / texelW + 1.5, 1.5, 10);
    float bs = 0, bn = 0;
    [unroll] for (int b = 0; b < 6; ++b) {
        float od = LsDepthAt(uvs, mul(rot, kPoisson[b * 2 + 1]) * search * texel, a);
        if (od < dr) { bs += od; bn += 1; }
    }
    if (bn == 0) return 1;
    float db = bs / bn;
    float pen = clamp(P9.w * (dr - db) / max(db, 0.1) / texelW, 1, 12);   // penumbra, texels
    float lit = 0;
    [unroll] for (int t = 0; t < 8; ++t) lit += LsDepthAt(uvs, mul(rot, kPoisson[t * 2]) * pen * texel, a) >= dr ? 1 : 0;
    return lit / 8;
}

// The pixel's strongest lamp (LampShade leaves it here): its index, its highlight (before shadow), its shadow and
// zone factor. The lamp pass (lower resolution) leaves that highlight out; the sun pass redraws it at full size.
static int gBestK;
static float gBestW, gBestVis, gBestZf;
static float3 gBestSpec;

float3 LampShade(float3 world, float3 n, float3 V, float3 albedo, float3 oneMinus, float4 Sp, float power, bool spec, float2 uv,
                 bool shadows, bool maps, PSIn i, float3 rel, float dd, float4 X, float4 Y, float noise)
{
    float3 sum = 0;
    gBestK = -1; gBestW = 0; gBestVis = 1; gBestZf = 1; gBestSpec = 0;
    // the 4 strongest lamps of the pixel get soft shadows (map + screen), the rest the map's hard shadow
    // (1.96: in vectors and named variables, not arrays: indexed at run time those live in scratch memory on AMD)
    float3 bc0 = 0, bc1 = 0, bc2 = 0, bc3 = 0, bl0 = 0, bl1 = 0, bl2 = 0, bl3 = 0;
    float4 bw = 0, bs = -1;
    bool pick = shadows || maps;
    float3 camPos = world - rel;
    float range2 = P8.z * P8.z;
    // lamps from the map are switched off by day (key light: about 6 by day in this game)
    float day = saturate((Lum(G[L0.x].xyz) - 0.5) / 2);
    float mapScale = lerp(1, P8.w, day);
    float ndv = saturate(dot(n, V));
    int zn = min(ZoneN.x, 16);
    // 1.96: the zones the point is in (zIn) and those it is at the soft edge of (zSoft) as bits
    uint zIn = 0, zSoft = 0;
    [loop] for (int z = 0; z < zn; ++z) {   // (1.65: the zones there are, not 16)
        float zv = ZoneIn(z, world);
        if (zv >= 1) zIn |= 1u << (uint)z;
        else if (zv > 0) zSoft |= 1u << (uint)z;
    }
    int2 tile = clamp(int2(uv * float2(LampN.yz)), 0, LampN.yz - 1);
    uint4 ta = tTileA.Load(int3(tile, 0)), tb = tTileB.Load(int3(tile, 0));
    [loop] for (int w = 0; w < 8; ++w) {
      uint4 tv = w < 4 ? ta : tb;
      int wc = w & 3;
      uint m = wc == 0 ? tv.x : wc == 1 ? tv.y : wc == 2 ? tv.z : tv.w;
      [loop] while (m != 0) {
        uint b = m & (~m + 1);   // the lowest bit left
        m ^= b;
        int k = w * 32 + (int)(asuint((float)b) >> 23) - 127;
        float4 LP = LampP(k), LC = LampC(k), LX = LampX(k), LD = LampD(k);
        float3 d = LP.xyz - world;
        float r2 = dot(d, d);
        if (r2 > (LX.y > 0 ? LX.y * LX.y : range2)) continue;
        // zones: only the light of the same side of every zone wall (LC.w = the zones the lamp is in)
        uint lm = (uint)LC.w, la = (uint)LP.w >> 1;   // zones it is in, zones that act on it
        if (((zIn ^ lm) & la & ~zSoft) != 0) continue;   // a wall between (the point well inside or outside a zone)
        float zf = 1;
        uint zl = la & zSoft;   // at a zone's soft edge: by how far in
        [loop] while (zl != 0) {
            uint zb = zl & (~zl + 1);
            zl ^= zb;
            int z2 = (int)(asuint((float)zb) >> 23) - 127;
            zf *= 1 - abs(ZoneIn(z2, world) - (float)((lm >> (uint)z2) & 1));
        }
        if (zf < 0.001) continue;
        float3 l = d * rsqrt(max(r2, 1e-6));
        float ndl = saturate(dot(n, l));
        if (ndl <= 0) continue;
        float3 Lc = LC.rgb * (LampWindow(LX, LD, r2, l) / (r2 + 0.25)) * (((uint)LP.w & 1) ? mapScale : 1);   // irradiance at normal incidence
        if (max(Lc.r, max(Lc.g, Lc.b)) < 1e-5) continue;   // off (map lamps by day) or too far: nothing to add
        float3 brdf = albedo * oneMinus * 0.318310, sTerm = 0;
        float pa = PbrAlpha(power);
        if (P16.x > 0.5) brdf *= PbrDiffK(n, V, l, pa);
        if (P16.x > 0.5) { sTerm = PbrSpecK(n, V, l, PbrF0(Sp), pa); brdf += sTerm; }
        else if (spec) {
            float3 H = normalize(V + l);
            float ndh = saturate(dot(n, H));
            float vdh = saturate(dot(V, H));
            float sp = exp2(log2(min(ndh + 0.0001, 1)) * power) * (power + 1) * 0.039789;
            sp /= saturate(max(ndv, ndl) * vdh) + 0.0013;
            float fr = saturate(1 - vdh);
            float f5 = fr * fr * fr * fr * fr;
            sTerm = (Sp.rgb + oneMinus * Sp.w * f5) * sp;
            brdf += sTerm;
        }
        float3 contrib = Lc * brdf * ndl * zf;
        float slot = maps ? LX.x : -1;
        float lw = Lum(contrib);
        if (lw > gBestW) { gBestW = lw; gBestK = k; gBestSpec = Lc * sTerm * ndl; gBestZf = zf; }
        if (pick) {
            float wv = min(min(bw.x, bw.y), min(bw.z, bw.w));
            // the one left out (this lamp, or the weakest of the 4 it replaces) gets the hard map shadow
            float3 oc = contrib, ol = LP.xyz - camPos;
            float os = slot;
            if (lw > wv) {
                int wi = bw.x == wv ? 0 : bw.y == wv ? 1 : bw.z == wv ? 2 : 3;   // (the first weakest)
                float4 sel = float4(wi == 0, wi == 1, wi == 2, wi == 3);
                oc = wi == 0 ? bc0 : wi == 1 ? bc1 : wi == 2 ? bc2 : bc3;
                ol = wi == 0 ? bl0 : wi == 1 ? bl1 : wi == 2 ? bl2 : bl3;
                os = dot(bs, sel);
                float3 nl = LP.xyz - camPos;
                if (wi == 0) { bc0 = contrib; bl0 = nl; }
                else if (wi == 1) { bc1 = contrib; bl1 = nl; }
                else if (wi == 2) { bc2 = contrib; bl2 = nl; }
                else { bc3 = contrib; bl3 = nl; }
                bw = sel > 0 ? lw : bw;
                bs = sel > 0 ? slot : bs;
            }
            if (os >= 0) oc *= LampMapVis(world, n, ol + camPos, os, noise, false);
            sum += oc;
        } else sum += contrib;
      }
    }
    // the 4 strongest: soft shadows (1.96: a loop, not 4 copies of both shadow searches: a smaller shader)
    if (pick)
        [loop] for (int q3 = 0; q3 < 4; ++q3) {
            float4 sel = float4(q3 == 0, q3 == 1, q3 == 2, q3 == 3);
            float w3 = dot(bw, sel);
            if (w3 > 0) {
                float3 c3 = q3 == 0 ? bc0 : q3 == 1 ? bc1 : q3 == 2 ? bc2 : bc3;
                float3 l3 = q3 == 0 ? bl0 : q3 == 1 ? bl1 : q3 == 2 ? bl2 : bl3;
                float s3 = dot(bs, sel);
                float vis = shadows ? LampVis(i, rel, n, l3, dd, X, Y, noise) : 1;
                if (s3 >= 0) vis *= LampMapVis(world, n, l3 + camPos, s3, noise, true);
                sum += c3 * vis;
                if (w3 == gBestW) gBestVis = vis;
            }
        }
    return sum;
}

// One lamp's highlight at a point (LampShade's maths, for the sun pass at full size): irradiance x specular x n.l.
float3 LampSpecOne(int k, float3 world, float3 n, float3 V, float4 Sp, float power, float3 oneMinus)
{
    float4 LP = LampP(k), LC = LampC(k), LX = LampX(k), LD = LampD(k);
    float3 d = LP.xyz - world;
    float r2 = dot(d, d);
    float3 l = d * rsqrt(max(r2, 1e-6));
    float ndl = saturate(dot(n, l));
    if (ndl <= 0) return 0;
    float day = saturate((Lum(G[L0.x].xyz) - 0.5) / 2);
    float3 Lc = LC.rgb * (LampWindow(LX, LD, r2, l) / (r2 + 0.25)) * (((uint)LP.w & 1) ? lerp(1, P8.w, day) : 1);
    if (P16.x > 0.5) return Lc * PbrSpecK(n, V, l, PbrF0(Sp), PbrAlpha(power)) * ndl;
    float ndv = saturate(dot(n, V));
    float3 H = normalize(V + l);
    float ndh = saturate(dot(n, H));
    float vdh = saturate(dot(V, H));
    float sp = exp2(log2(min(ndh + 0.0001, 1)) * power) * (power + 1) * 0.039789;
    sp /= saturate(max(ndv, ndl) * vdh) + 0.0013;
    float fr = saturate(1 - vdh);
    float f5 = fr * fr * fr * fr * fr;
    return Lc * (Sp.rgb + oneMinus * Sp.w * f5) * sp * ndl;
}

// Which lamps light which part of the screen: one pixel per screen tile, 256 bits (a lamp each). A lamp's light is a
// sphere (the light radius); it reaches the tile when the sphere is inside all four side planes of the tile's view
// pyramid. This frame's camera (record at t28) and view rays, so nothing lags behind when the camera turns.
struct TileOut { uint4 a : SV_Target0; uint4 b : SV_Target1; };
TileOut PS_Tiles(PSIn i)
{
    float3 X = ddx(i.t1.xyz) * 0.5, Y = ddy(i.t1.xyz) * 0.5;
    // forward = t1 x the sign of the ray scale (depth - w): negative in the game (and for the sky, as in PS_Editor)
    float dd = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x - i.t1.w;
    float sg = abs(dd) > 1e-9 ? sign(dd) : -1;
    float3 c = sg * i.t1.xyz;
    float3 r0 = sg * (i.t1.xyz - X - Y), r1 = sg * (i.t1.xyz + X - Y), r2 = sg * (i.t1.xyz + X + Y), r3 = sg * (i.t1.xyz - X + Y);
    float3 p0 = normalize(cross(r0, r1)), p1 = normalize(cross(r1, r2)), p2 = normalize(cross(r2, r3)), p3 = normalize(cross(r3, r0));
    p0 *= sign(dot(p0, c)); p1 *= sign(dot(p1, c)); p2 *= sign(dot(p2, c)); p3 *= sign(dot(p3, c));
    float3 cam = tCamPrev.Load(int3(3, 0, 0)).xyz;
    float R = P8.z;
    uint4 wa = 0, wb = 0;   // (1.96: no array indexed at run time: scratch memory on AMD)
    int cnt = min(LampN.x, 256);
    [loop] for (int w = 0; w * 32 < cnt; ++w) {
        uint word = 0;
        int k1 = min(cnt, w * 32 + 32);
        [loop] for (int k = w * 32; k < k1; ++k) {
            float3 rel = LampP(k).xyz - cam;
            float rk = LampX(k).y;
            float Rk = rk > 0 ? rk : R;
            if (dot(p0, rel) > -Rk && dot(p1, rel) > -Rk && dot(p2, rel) > -Rk && dot(p3, rel) > -Rk)
                word |= 1u << (uint)(k & 31);
        }
        int wc = w & 3;
        uint4 put = uint4(wc == 0 ? word : 0, wc == 1 ? word : 0, wc == 2 ? word : 0, wc == 3 ? word : 0);
        if (w < 4) wa |= put; else wb |= put;
    }
    TileOut o;
    o.a = wa;
    o.b = wb;
    return o;
}

// Output of the sun pass as result = dest * K + S: written directly (copy variant, dest read from the
// copy of the frame) or by the blender (BLEND_OUT, dual-source: no copy of the frame at all).
// ---- our sun shadows (sunshadow.inc): four square cascades around the camera, their texel grid fixed in the world:
// 0 in t57 (drawn every frame); 1 = t64 (its still objects, kept) + t65 (moving things and cars, every frame), 2 = t61
// (kept) + t66 (moving), 3 = t62 (kept): in shadow if either says so; the constants (b7) are the ones they were drawn with.
cbuffer SunShadowCB : register(b7)
{
    float4 SM[12];   // cascade k: rows 3k, 3k+1, 3k+2: world (x, y, z, 1) -> x, y (-1..1, y up), depth (0..1, away from the sun)
    float4 SC[4];    // cascade k: half size (m), texel (m), depth per metre, 1 / half size
    float4 SL;       // the sun's direction (light -> scene)
    float4 SR, SU;   // the maps' right and up (world)
    float4 SP;       // penumbra per metre of caster distance (the sun's angle), sharpest edge (m), widest penumbra searched (m), smoothing
    float4 SP2;      // map size (texels), 1 / size, history usable, cascades
    float4 SP3;      // mask size (pixels; half the frame's or the frame's), the frame's size
    float4 SP4;      // PS_SunUp: texels around that differ by more than x: the pixel made at full resolution; (test: mark those);
                     // casters up to z m away are found when the sun has moved since a map was drawn; w: our sun (SunPath) on
    float4 SD[4];    // cascade k: its shadows' move per metre of caster distance since its map was drawn (the sun then -> now),
                     // metres along the map's right, up; w its length (0 for 0: drawn every frame)
};
Texture2D<float> tSunSM : register(t57);
Texture2D<float> tSunC1 : register(t64);
Texture2D<float> tSunC1Cars : register(t65);
Texture2D<float> tSunC2Cars : register(t66);
Texture2D<float> tSunFar2 : register(t61);
Texture2D<float> tSunFar3 : register(t62);
Texture2D<float2> tSunMask : register(t58);   // at the frame's resolution: visibility, cascade (0..3 ours, 4 past them, -1 sky)
Texture2D<float4> tSunRaw : register(t59);    // mask resolution, this frame: visibility, caster distance (m), N.L, cascade
Texture2D<float4> tSunHist : register(t60);   // mask resolution, smoothed: visibility, frames + 64 x (cascade + 1), N.L, distance
SamplerComparisonState sSunCmp : register(s15);

float3 SsCoord(int k, float3 w) { float4 p = float4(w, 1); return float3(dot(SM[k * 3], p), dot(SM[k * 3 + 1], p), dot(SM[k * 3 + 2], p)); }
// a cascade's depth at uv (0..1, kept half a texel inside), and its comparison (1 = lit), bilinear
float SsDepth(int k, float2 uv)
{
    uv = clamp(uv, SP2.y * 0.5, 1 - SP2.y * 0.5);
    if (k == 0) return tSunSM.SampleLevel(sPoint, uv, 0);
    if (k == 1) return min(tSunC1.SampleLevel(sPoint, uv, 0), tSunC1Cars.SampleLevel(sPoint, uv, 0));
    return k == 2 ? min(tSunFar2.SampleLevel(sPoint, uv, 0), tSunC2Cars.SampleLevel(sPoint, uv, 0)) : tSunFar3.SampleLevel(sPoint, uv, 0);
}
float SsCmp(int k, float2 uv, float z)
{
    uv = clamp(uv, SP2.y * 0.5, 1 - SP2.y * 0.5);
    if (k == 0) return tSunSM.SampleCmpLevelZero(sSunCmp, uv, z);
    if (k == 1) return tSunC1.SampleCmpLevelZero(sSunCmp, uv, z) * tSunC1Cars.SampleCmpLevelZero(sSunCmp, uv, z);
    return k == 2 ? tSunFar2.SampleCmpLevelZero(sSunCmp, uv, z) * tSunC2Cars.SampleCmpLevelZero(sSunCmp, uv, z) : tSunFar3.SampleCmpLevelZero(sSunCmp, uv, z);
}
// 16 points spread evenly over a unit disk (golden angle, radius sqrt)
static const float2 kVogel16[16] = {
    float2(0.17678, 0.00000), float2(-0.22577, 0.20683), float2(0.03456, -0.39377), float2(0.28457, 0.37117),
    float2(-0.52222, -0.09237), float2(0.49470, -0.31468), float2(-0.16547, 0.61553), float2(-0.31556, -0.60759),
    float2(0.68464, 0.25003), float2(-0.71226, 0.29401), float2(0.34335, -0.73373), float2(0.25373, 0.80893),
    float2(-0.76475, -0.44319), float2(0.89713, -0.19723), float2(-0.54751, 0.77877), float2(-0.12649, -0.97609) };

// The sun's visibility at a point (1 = lit), this frame: x visibility, y caster distance (m), z N.L, w cascade (-1 sky).
// The cascade that holds the point; the casters searched around the point in the map: their distance from it along the
// sun sets the penumbra as a real sun's does (0.53 deg: 0.93 cm per metre of distance: sharp where a shadow starts,
// soft far from its caster); then the map compared over a disk of that size (bilinear taps). Every tap has its depth on
// the receiver's plane (no stripes on walls seen at an angle, no shadow lifted off the ground); the point is lifted off
// its surface by a texel or two. Rotated per pixel and frame; PS_SunTemporal averages the frames.
// 1.26: a kept map (1-3) was drawn with the sun of then: a caster t m away along the sun now throws its shadow SD[k].xy x t
// m off from where that map has it, so the taps read the map that far off (the shadows follow the sun every frame; a
// map drawn again changes nothing). The caster search is centred halfway along that shift for casters up to SP4.z m and
// wider by half of it (so the casters a newer sun brings in are found); their distance is from the taps within the
// usual search disk around the point if any (the wider disk reaches deeper into a caster: too far), else from all.
// Each tap's distance is from the receiver's plane under it.
// ray: the view ray (t1) at the pixel; gN: the surface's normal from the screen derivatives, nG: the G-buffer's.
// 1.54: the city's sun shadow from the rain map (sunshadow.inc): per texel of that map (its 512 m, 0.5 m texels: the
// city's height from above, whatever was ever drawn, behind the camera too) the height under which a point is in the
// shadow of something 10..250 m towards the sun (nearer casters: our cascades have them)
Texture2D<float> tCity : register(t72);
float PS_CityShadow(PSIn i) : SV_Target0
{
    float2 uv = i.pos.xy / 1024;
    float2 xz = float2(RN0.x + (uv.x - 0.5) * 2 * RN0.z, RN0.y - (uv.y - 0.5) * 2 * RN0.z);
    float3 L = -SL.xyz;   // towards the sun
    float hl = length(L.xz);
    if (L.y <= 0.02 || hl < 1e-3 || RN1.y < 0.5) return -1e4;
    float2 d = L.xz / hl;
    float tanE = L.y / hl, hs = -1e4;
    [loop] for (int k = 0; k < 64; ++k) {
        float s = 10 + k * 1.5 + k * k * 0.035;   // 10 .. ~245 m
        float2 p = xz + d * s;
        float2 u = float2((p.x - RN0.x) / RN0.z * 0.5 + 0.5, 0.5 - (p.y - RN0.y) / RN0.z * 0.5);
        if (any(u < 0) || any(u > 1)) break;
        hs = max(hs, RN0.w - tRain.SampleLevel(sPoint, u, 0) * RN1.x - s * tanE);
    }
    return hs;
}
// how lit a point is by the city's shadow (1: not in it)
float CityLit(float3 world)
{
    if (SD[0].z < 0.5) return 1;
    float2 u = float2((world.x - RN0.x) / RN0.z * 0.5 + 0.5, 0.5 - (world.z - RN0.y) / RN0.z * 0.5);
    if (any(u < 0.01) || any(u > 0.99)) return 1;
    return saturate((world.y + 0.3 - tCity.SampleLevel(sLin, u, 0)) / 1.2);
}
// 1.53 / 1.61: a wider cascade's far caster this one lost (each is drawn from what the game draws for its shadows now: a
// building beyond them missing, a lit wedge in its big shadow, ragged where the cascades meet): a caster over minM metres
// away along the sun there (not this surface's own) -> its shadow, 5 taps where it falls now. x: visibility, y: metres,
// z: the cascade (-1: none)
float3 SsFarCheck(int k, float3 world, float3 n, float lift, float2x2 rot, float minM, float2 grad)
{
    [loop] for (int k2 = k + 1; k2 < (int)SP2.w; ++k2) {
        float3 c2 = SsCoord(k2, world + n * SC[k2].y * lift);
        if (max(abs(c2.x), abs(c2.y)) > 0.95) continue;
        float2 uv2 = c2.xy * float2(0.5, -0.5) + 0.5;
        // 1.62: a low sun: a coarse texel of flat ground spans metres along the sun (its own depth over a texel: the
        // receiver's slope x the texel): that is not a caster (the ground shaded itself: a mess of patches in the morning)
        float slopeM = SC[k2].y * (2 + 1.5 * length(grad));
        float bm = (c2.z - SsDepth(k2, uv2)) / SC[k2].z;          // metres to a caster along the sun
        if (bm < max(max(minM, SC[k2].y * 4), slopeM * 2)) continue;
        float2 sh2 = SD[k2].xy * bm;
        float r2 = SC[k2].y * 1.5, acc2 = 0;
        [unroll] for (int t = 0; t < 5; ++t) {
            float2 o = (t == 0 ? float2(0, 0) : mul(rot, kVogel16[t * 3]) * r2) + sh2;
            float zt = c2.z + (dot(grad, o - sh2) - max(0.5, slopeM)) * SC[k2].z;   // (the receiver's plane under the tap)
            acc2 += SsCmp(k2, uv2 + float2(o.x, -o.y) * (0.5 * SC[k2].w), zt);
        }
        if (acc2 < 4.5) return float3(acc2 / 5, bm, k2);
    }
    return float3(1, 0, -1);
}
float4 SunMaskAt(float4 ray, float depth, float3 gN, float3 nG, float noise, float3 camPos)
{
    float dd = depth - ray.w;
    if (abs(dd) < 1e-9) return float4(1, 0, 0, -1);
    float3 rel = ray.xyz / dd;
    if (length(rel) > 3000.0) return float4(1, 0, 0, -1);
    float3 world = rel + camPos;
    float3 n = dot(gN, nG) > 0.5 ? gN : nG;    // the surface's own normal; at edges (no good derivative) the G-buffer's
    float ndl = dot(n, -SL.xyz);
    // the cascade: the finest that holds the point with the search around it; in its outer 15% now and then the next one
    int k = 4;
    float band = 0;
    // 1.63 (test, ShadowOnly): one map alone wherever it reaches, nothing from the others (no blend, no far casters)
    int only = (int)(SD[1].z + 0.5) - 1;
    if (only >= 0) {
        if (only >= (int)SP2.w) return float4(1, 0, ndl, 4);
        float3 co = SsCoord(only, world);
        if (max(abs(co.x), abs(co.y)) > 1 - (SP.z * 0.5 + 3 * SC[only].y) * SC[only].w) return float4(1, 0, ndl, 4);
    }
    [loop] for (int q = 0; q < 4; ++q) {
        if (q >= (int)SP2.w || only >= 0) break;
        float3 c = SsCoord(q, world);
        float e = max(abs(c.x), abs(c.y));
        float lim = 1 - (SP.z * 0.5 + 3 * SC[q].y) * SC[q].w;
        if (e < lim) {
            float b = saturate((e - lim * 0.85) / (lim * 0.15));
            if (frac(noise * 7.31 + 0.37) < b) { band = 1; continue; }   // the next one this frame (the smoothing blends them)
            k = q;
            break;
        }
    }
    if (only >= 0) k = only;
    if (k > 3) {   // past our maps: the game's map (3: still blended with ours)
        float v = ((L2.z & F_SHADOW) && SP4.w < 0.5) ? SunVisibility(world + n * 0.15 - SL.xyz * 0.3) : 1;   // (our sun: the game's map points elsewhere)
        return float4(v, 0, ndl, band > 0 ? 3 : 4);
    }
    if (ndl <= 0) return float4(0, 0, ndl, k);   // facing away from the sun: its own shadow
    float tex = SC[k].y;
    float lift = 1.5 + 2.0 * (1 - ndl);
    // the receiver's plane: metres of depth (along the sun) per metre along the map's right and up
    float2 grad = clamp(float2(dot(n, SR.xyz), dot(n, SU.xyz)) / max(ndl, 0.05), -8, 8);
    float sn, cs;
    sincos(noise * 6.2831853, sn, cs);
    float2x2 rot = float2x2(cs, -sn, sn, cs);
    // 1) the casters around: 16 taps over the widest penumbra searched (in the next cascade when it spans many texels)
    float searchM = max(SP.z * 0.5, tex * 2);
    int ks = (searchM > 24 * tex && k + 1 < (int)SP2.w && only < 0) ? k + 1 : k;
    float3 c0 = SsCoord(ks, world + n * SC[ks].y * lift);
    float2 uvS = c0.xy * float2(0.5, -0.5) + 0.5;
    float biasS = SC[ks].y * 1.5;
    float2 shS = SD[ks].xy * (SP4.z * 0.5);                        // (the sun's move since the map: see above)
    float searchR = searchM + SD[ks].w * SP4.z * 0.5;
    float bSum = 0, bN = 0, lSum = 0, lN = 0;
    [unroll] for (int s = 0; s < 16; ++s) {
        float2 o = mul(rot, kVogel16[s]) * searchR + shS;          // metres along the map's right, up
        float zp = c0.z + dot(grad, o) * SC[ks].z;                  // the receiver's plane there
        float st = SsDepth(ks, uvS + float2(o.x, -o.y) * (0.5 * SC[ks].w));
        if (st < zp - biasS * SC[ks].z) {
            bSum += zp - st; bN += 1;
            if (dot(o, o) <= searchM * searchM) { lSum += zp - st; lN += 1; }
        }
    }
    if (bN < 0.5 && only >= 0) return float4(1, 0, ndl, k);
    if (bN < 0.5) {                                                // nothing between it and the sun in this cascade:
        // 1.53: but a wider kept one may hold a far caster this one lost (this one is drawn from what the game draws for
        // its shadows now; a building beyond them was missing: a lit square cut out of its big shadow). A caster over
        // 2 m away along the sun there (not this surface's own: no acne) -> its shadow, 5 taps where it falls now
        float3 fc = SsFarCheck(k, world, n, lift, rot, 2.0, grad);
        if (fc.z >= 0) return float4(fc.x, fc.y, ndl, fc.z);
        float cl = CityLit(world);   // (1.54: buildings our maps lost: out of the game's shadow view, behind the camera)
        return float4(cl, cl < 1 ? 30 : 0, ndl, k);
    }
    float blockerM = (lN > 0.5 ? lSum / lN : bSum / bN) / SC[ks].z;   // how far the casters are, along the sun
    float penM = max(blockerM * SP.x, SP.y);                       // the penumbra's full width
    float rf = min(penM * 0.5, searchM);
    int kf = (rf > 24 * tex && k + 1 < (int)SP2.w && only < 0) ? k + 1 : k;
    float2 shF = SD[kf].xy * blockerM;                             // where those casters' shadow is now
    if (bN > 15.5 && length(shF - shS) + rf < searchR * 0.9) return float4(0, blockerM, ndl, k);   // covered all around: full shadow
    // 2) the map compared over the penumbra's disk (in the next cascade when that disk spans many texels)
    float texF = SC[kf].y;
    float3 cf = SsCoord(kf, world + n * texF * lift);
    float2 uvF = cf.xy * float2(0.5, -0.5) + 0.5;
    float r = max(rf, texF * 0.75);
    float acc = 0;
    [unroll] for (int t = 0; t < 16; ++t) {
        float2 o = mul(rot, kVogel16[t]) * r + shF;
        float zt = cf.z + (dot(grad, o) - texF) * SC[kf].z;
        acc += SsCmp(kf, uvF + float2(o.x, -o.y) * (0.5 * SC[kf].w), zt);
    }
    float v = acc / 16;
    // 1.61: casters here (a post, a tree) and still a far building this cascade lost: the wider ones asked as well
    // (1.53 asked them only where this one had nothing at all around)
    if (v > 0.02 && only < 0) {
        float3 fc = SsFarCheck(k, world, n, lift, rot, max(4.0, blockerM * 2), grad);
        if (fc.z >= 0 && fc.x < v) return float4(fc.x, fc.y, ndl, fc.z);
    }
    return float4(v, blockerM, ndl, k);
}

// 1.25: the mask is made at half the frame's resolution (SP3: the mask's size, the frame's). Its texel p stands for the
// frame's pixel p x 2 (its top left one; at full resolution p itself): that pixel's depth and normal, and the view ray
// there (the ray is affine on screen: moved from the texel's centre along its derivatives).
int SsScale() { return SP3.z > SP3.x * 1.5 ? 2 : 1; }
int2 SsPix(PSIn i, out float4 ray)
{
    int sc = SsScale();
    int2 fp = min(int2(i.pos.xy) * sc, int2(SP3.zw) - 1);
    float o = 0.5 / sc - 0.5;   // that pixel's centre from the texel's, in texels
    ray = i.t1 + (ddx(i.t1) + ddy(i.t1)) * o;
    return fp;
}
float3 GeoNRay(float4 ray, float depth)
{
    float dd = depth - ray.w;
    dd = abs(dd) < 1e-9 ? 1e-9 : dd;
    float3 r = ray.xyz / dd;
    float3 g = cross(ddy(r), ddx(r));
    float l = length(g);
    if (!(l > 1e-20) || abs(depth - ray.w) < 1e-9) return 0;
    g /= l;
    return dot(g, r) > 0 ? -g : g;
}

float4 PS_SunMask(PSIn i) : SV_Target0
{
    float3 camPos = 0;
    [loop] for (int cq = 0; cq < 1 + (int)(P2.w * 0); ++cq) camPos = G[L1.w].xyz;
    float4 ray;
    int2 fp = SsPix(i, ray);
    float2 uv = (fp + 0.5) / SP3.zw;
    float depth = tDepth.SampleLevel(sPoint, uv, 0).x;
    float3 gN = GeoNRay(ray, depth);
    float3 nG = normalize(tNormal.SampleLevel(sPoint, uv, 0).xyz * 2 - 1);
    return SunMaskAt(ray, depth, gN, nG, Ign(i.pos.xy), camPos);
}

// The mask smoothed over frames: the last frame's value found where this point was (its distance must agree), kept
// within this frame's values around the texel (a moving shadow leaves no trail), averaged with this frame's.
// x visibility, y frames + 64 x (cascade + 1) (0: sky), z N.L, w distance. At the mask's resolution.
float4 PS_SunTemporal(PSIn i) : SV_Target0
{
    float3 camPos = 0;
    [loop] for (int cq = 0; cq < 1 + (int)(P2.w * 0); ++cq) camPos = G[L1.w].xyz;
    int2 p = int2(i.pos.xy);
    float4 r = tSunRaw.Load(int3(p, 0));
    float4 ray;
    int2 fp = SsPix(i, ray);
    float depth = tDepth.SampleLevel(sPoint, (fp + 0.5) / SP3.zw, 0).x;
    float dd = depth - ray.w;
    if (abs(dd) < 1e-9 || r.w < 0) return float4(1, 0, 0, 6e4);
    float3 rel = ray.xyz / dd;
    float dist = length(rel);
    float mn = r.x, mx = r.x;
    int2 pmax = int2(SP3.xy) - 1;
    [unroll] for (int k = 0; k < 8; ++k) {
        int2 o = int2(k < 3 ? k - 1 : k == 3 ? -1 : k == 4 ? 1 : k - 6, k < 3 ? -1 : k < 5 ? 0 : 1);
        float v = tSunRaw.Load(int3(clamp(p + o, 0, pmax), 0)).x;
        mn = min(mn, v);
        mx = max(mx, v);
    }
    float v = r.x, frames = 1;
    float2 uv;
    float pd;
    if (SP2.z > 0.5 && Reproject(HistPoint(rel + camPos, (fp + 0.5) / SP3.zw), dd, uv, pd)) {   // (1.59: the player's car by its motion)
        // the texel whose pixel is nearest (a texel's pixel is its top left one at half resolution)
        float4 h = tSunHist.SampleLevel(sPoint, uv + (0.5 - 0.5 / SsScale()) / SP3.xy, 0);
        if (h.y >= 64 && abs(h.w - pd) < pd * 0.02 + 0.08) {
            frames = min(fmod(h.y, 64) + 1, 32);
            v = lerp(clamp(h.x, mn, mx), r.x, max(1 / frames, 1 - SP.w));
        }
    }
    return float4(v, frames + 64 * (r.w + 1), r.z, dist);
}

// 1.25: the mask at the frame's resolution, from the smoothed one at half: each pixel averages the mask texels next to
// it (they stand for the pixels 2p) that lie on its own surface: their point (the view ray at their pixel, their
// distance) within a few cm of this pixel's plane (and 0.4% of the distance), their N.L near this pixel's (a corner
// between a lit and a shaded face is not blended). Made here as at full resolution, with a noise fixed in time (no
// smoothing over frames for it): a pixel none of them shares a surface with (something thinner than two pixels: a post,
// a wire, a fence bar far off), and one between texels that differ a lot (a sharp shadow edge: kept to the pixel).
// x visibility, y cascade (-1 sky).
float IgnStill(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }
float2 PS_SunUp(PSIn i) : SV_Target0
{
    float3 camPos = 0;
    [loop] for (int cq = 0; cq < 1 + (int)(P2.w * 0); ++cq) camPos = G[L1.w].xyz;
    int2 q = int2(i.pos.xy);
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float3 gN = GeoN(i, depth);
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return float2(1, -1);
    float3 rel = i.t1.xyz / dd;
    float dist = length(rel);
    if (dist > 3000.0) return float2(1, -1);
    float3 nG = normalize(tNormal.SampleLevel(sPoint, i.t0.xy, 0).xyz * 2 - 1);
    float3 n = dot(gN, nG) > 0.5 ? gN : nG;
    float ndl = dot(n, -SL.xyz);
    int sc = SsScale();
    int2 p0 = q / sc, odd = q - p0 * sc;
    int2 pmax = int2(SP3.xy) - 1;
    float tol = 0.02 + dist * 0.004, sg = dd > 0 ? 1 : -1;
    float acc = 0, wsum = 0, casc = 9, vmin = 1, vmax = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        int2 o = int2(k & 1, k >> 1);
        if (o.x > odd.x || o.y > odd.y) continue;   // (an even pixel is a texel's own: only the odd ones lie between)
        int2 p = min(p0 + o, pmax);
        float4 h = tSunHist.Load(int3(p, 0));
        if (h.y < 64) continue;                      // sky
        float2 dp = float2(p * sc - q);
        float3 rs = i.t1.xyz + X.xyz * dp.x + Y.xyz * dp.y;
        float3 ps = normalize(rs) * (sg * h.w);      // its point (camera-relative)
        float w = saturate(2 - abs(dot(ps - rel, n)) / tol) * saturate(1 - abs(h.z - ndl) * 3);
        if (w <= 0) continue;
        acc += h.x * w;
        wsum += w;
        casc = min(casc, floor(h.y / 64) - 1);
        vmin = min(vmin, h.x);
        vmax = max(vmax, h.x);
    }
    if (wsum > 0.05 && vmax - vmin < SP4.x) return float2(acc / wsum, casc);
    float4 r = SunMaskAt(i.t1, depth, gN, nG, IgnStill(i.pos.xy), camPos);
    return float2(r.x, r.w + SP4.y * 10);   // (SP4.y: the test harness counts these)
}


// ---- relief (1.11): the small shapes of a surface from its normal map. Seen from the surface's own plane (its
// geometric normal g), a texel's normal tilts by the slope of the little height field the normal map was made from:
// along a direction d the height changes by -n.d / n.g per metre. Walking from the pixel along the surface (on screen:
// the G-buffer normal at each step) and adding those slopes gives the height along the way: towards the sun, a bump
// that rises above the sun's ray shades the pixel (pebbles, the edge of a slab, mortar between bricks); in four
// directions, how much the surroundings rise around it darkens the sky light (cavities). Only near: once a pixel
// covers more than ~1 cm the detail is below it (it fades). P17: strength, depth scale, reach (m), -.
float RelSlope(float3 nq, float3 g, float3 d) { return clamp(-dot(nq, d) / max(dot(nq, g), 0.25), -3, 3); }
bool RelNormal(PSIn i, float3 q, float4 X, float4 Y, float dd, out float3 nq)
{
    nq = 0;
    float2 dp; float4 rayAt;
    if (!Project(q, i.t1, X, Y, dd, dp, rayAt)) return false;
    float2 pix = i.pos.xy + dp;
    float sd = SceneDist(pix, rayAt);
    float lq = length(q);
    if (sd < 0 || abs(sd - lq) > 0.03 + lq * 0.004) return false;   // another surface there (an edge): stop
    nq = normalize(tNormal.SampleLevel(sPoint, pix / Size.xy, 0).xyz * 2 - 1);
    return true;
}
// x: the sun's visibility, y: the sky light's (1 = open)
float2 Relief(PSIn i, float3 rel, float3 n, float3 g, float3 toSun, float dd, float4 X, float4 Y, float noise, float str)
{
    float2 r = 1;
    if (str <= 0) return r;
    float pxM = length(X.xyz / dd);                 // metres per pixel, about
    float fade = saturate(1.6 - pxM / 0.01);        // full below 6 mm a pixel, gone at 16 mm
    if (fade <= 0 || dot(n, g) > 0.9995) return r;  // no detail
    float ds = max(pxM * 1.3, 0.002);
    float reach = P17.z, depth = P17.y;
    // the sun: the height towards it against the ray's rise
    float ng = dot(toSun, g);
    float3 sd = toSun - g * ng;
    float sl = length(sd);
    if (ng > 0.02 && sl > 1e-3) {
        sd /= sl;
        float tanE = ng / sl, h = 0, prev = RelSlope(n, g, sd), occ = 0;
        [loop] for (int k = 1; k <= 12; ++k) {
            float s = (k - 1 + noise) * ds + ds;
            if (s > reach) break;
            float3 nq;
            if (!RelNormal(i, rel + sd * s, X, Y, dd, nq)) break;
            float sl2 = RelSlope(nq, g, sd);
            h += 0.5 * (sl2 + prev) * ds * depth;
            prev = sl2;
            occ = max(occ, (h - s * tanE) / (0.003 + s * 0.12));   // soft: a wide edge (1.12)
        }
        r.x = 1 - min(saturate(occ) * fade * str, 0.85);   // never black: the sky lights a little shadow too
    }
    // the sky: how far the surroundings rise (4 directions, turned per pixel)
    float3 t = normalize(abs(g.y) < 0.9 ? cross(g, float3(0, 1, 0)) : cross(g, float3(1, 0, 0)));
    float3 b = cross(g, t);
    float sn, cs;
    sincos(noise * 1.5708, sn, cs);
    float occA = 0;
    [unroll] for (int a = 0; a < 4; ++a) {
        float3 d = a == 0 ? t * cs + b * sn : a == 1 ? -t * cs - b * sn : a == 2 ? b * cs - t * sn : -b * cs + t * sn;
        float h = 0, prev = RelSlope(n, g, d), hor = 0;
        [loop] for (int k = 1; k <= 5; ++k) {
            float s = k * ds * 1.5;
            if (s > reach * 0.5) break;
            float3 nq;
            if (!RelNormal(i, rel + d * s, X, Y, dd, nq)) break;
            float sl2 = RelSlope(nq, g, d);
            h += 0.5 * (sl2 + prev) * ds * 1.5 * depth;
            prev = sl2;
            hor = max(hor, h / s);
        }
        occA += saturate(hor * 1.5);
    }
    r.y = 1 - min(occA / 4 * 0.7 * fade * str, 0.75);
    return r;
}
PSOut Out(PSOut o, float4 dest, float3 K, float3 S)
{
#ifdef BLEND_OUT
    o.c = float4(S, 1);
    o.k = float4(K, 1);
#else
    o.c = float4(dest.rgb * K + S, dest.a);
#endif
    return o;
}

// 1.74: the car's simulated rain drops (carrain.inc)
Texture2D<uint> tDropAtlas : register(t73);
Texture2D<float4> tDropS : register(t74);
Texture2D<float4> tDropV : register(t75);
Texture2D<float> tDropTrail : register(t76);
Texture2D<float> tCarMap : register(t78);   // 1.80: the car's shape (carrain.inc; view 37 checks it)
float2 CrAtlas(int pl, float2 p)   // an unfolding's place (car-local m) -> atlas texel (carrain.inc's DropAtlas)
{
    if (pl == 0) return float2((p.x + 1.2) / 0.004, (p.y + 2.8) / 0.004);
    float2 o = pl == 1 ? float2(600, 0) : pl == 2 ? float2(600, 750) : pl == 3 ? float2(0, 1400) : float2(600, 1500);
    float umin = pl <= 2 ? -2.8 : -1.2;
    return o + float2((p.x - umin) / 0.004, (2.0 - p.y) / 0.004);
}
// 1.75: a bead per cell (some cells empty), alive part of its own cycle (real time): a drop lands, grows for ~10 s,
// stays, goes (taken by a run, dried); 40 .. 90 s a cycle. 1 at its centre .. 0 at its edge; o: from its centre in
// radii. stretch > 1 (vertical surfaces, v up): a teardrop, round and heavy below, drawn out and thinner above.
// 1.81: the car's shape (carrain.inc's map, 1 cm, t78): per face (0 top, 1 right, 2 left, 3 front, 4 back) the depth
// along that face's view at a place on it (1: no body there); a car-local point's place on a face, how far out it is
float2 FaceP(int pl, float3 l) { return pl == 0 ? l.xz : pl <= 2 ? l.zy : l.xy; }
float FaceOut(int pl, float3 l) { return pl == 0 ? l.y : pl == 1 ? l.x : pl == 2 ? -l.x : pl == 3 ? l.z : -l.z; }
float FaceSurf(int pl, float d) { return (pl == 0 ? 2.0 : pl <= 2 ? 1.3 : 3.0) - d * (pl == 0 ? 2.5 : pl <= 2 ? 2.6 : 6.0); }
float CarMapD(int pl, float2 p)
{
    float2 lo = pl == 0 ? float2(-1.2, -2.8) : pl <= 2 ? float2(-2.8, -1.0) : float2(-1.2, -1.0);
    float2 hi = pl == 0 ? float2(1.2, 2.8) : pl <= 2 ? float2(2.8, 2.0) : float2(1.2, 2.0);
    if (any(p < lo) || any(p > hi)) return 1;
    return tCarMap.Load(int3(clamp(int2(CrAtlas(pl, p) * 0.4), int2(0, 0), int2(799, 899)), 0));
}
int FaceOf(float3 nl)   // a car-local normal's face (the one it is nearest; -1: facing down)
{
    float3 a = abs(nl);
    if (nl.y > 0 && a.y >= a.x && a.y >= a.z) return 0;
    if (a.x >= a.z && a.x > a.y) return nl.x > 0 ? 1 : 2;
    if (a.z > a.y) return nl.z > 0 ? 3 : 4;
    return -1;
}
// how deep inside the car's shape (its hull seen from the five sides) a car-local point is (< 0: out); pl: the face
// it is least under (the surface it would cross going out)
float HullIn(float3 l, out int pl)
{
    float m = 9;
    pl = 0;
    [unroll] for (int k = 0; k < 5; ++k) {
        float d = CarMapD(k, FaceP(k, l));
        float u = d >= 0.99999 ? -1 : FaceSurf(k, d) - FaceOut(k, l);
        if (u < m) { m = u; pl = k; }
    }
    return m;
}
// a face's normal (car-local) from its depth over +-2 cm (one-sided where the body ends; carrain.inc's Surface)
float3 FaceNormal(int pl, float2 p)
{
    float3 U = pl <= 2 && pl > 0 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 V = pl == 0 ? float3(0, 0, 1) : float3(0, 1, 0);
    float3 N = pl == 0 ? float3(0, 1, 0) : pl == 1 ? float3(1, 0, 0) : pl == 2 ? float3(-1, 0, 0) : pl == 3 ? float3(0, 0, 1) : float3(0, 0, -1);
    float rg = pl == 0 ? 2.5 : pl <= 2 ? 2.6 : 6.0;
    float d0 = CarMapD(pl, p), dl = CarMapD(pl, p - float2(0.02, 0)), dr = CarMapD(pl, p + float2(0.02, 0));
    float du = CarMapD(pl, p - float2(0, 0.02)), dn = CarMapD(pl, p + float2(0, 0.02));
    bool hl = dl < 0.99999, hr = dr < 0.99999, hu = du < 0.99999, hn = dn < 0.99999;
    float gx = hl && hr ? (dr - dl) * 0.5 : hr ? dr - d0 : hl ? d0 - dl : 0;
    float gy = hu && hn ? (dn - du) * 0.5 : hn ? dn - d0 : hu ? d0 - du : 0;
    return normalize(N + U * (gx * rg / 0.02) + V * (gy * rg / 0.02));
}
float Bead(float2 uv, float cell, float dens, float salt, float stretch, float time, out float2 o)
{
    float2 c = uv / cell, id = floor(c), f = frac(c) - 0.5;
    float h = frac(sin(dot(id + salt, float2(127.1, 311.7))) * 43758.5453);
    float h2 = frac(h * 17.13 + salt), h3 = frac(h * 31.71), h4 = frac(h * 53.37);
    float ph = frac(time / (40 + 50 * h4) + h2);
    float life = smoothstep(0, 0.2, ph) * (1 - smoothstep(0.9, 0.93, ph));
    float r = (0.13 + 0.3 * h3) * (0.45 + 0.55 * life);
    float2 d = f - (float2(h2, frac(h2 * 7.7)) - 0.5) * max(1 - 2.2 * r * stretch, 0);
    if (stretch > 1.01) {
        float up = saturate(d.y / (r * stretch));
        d.x /= 1 - 0.45 * up;
        d.y = d.y > 0 ? d.y / stretch : d.y;
    }
    o = d / max(r, 1e-4);
    return h < dens && life > 0.02 ? saturate(1 - length(o)) : -1;   // (-1: no bead in this cell now)
}
float3 PanoSky(float3 d, float gloss);
float2 RainRipple(float2 p, float t, float rate);   // 1.83 (below)
bool RfTop(float2 xz, out float top);
PSOut PS_Sun(PSIn i)
{
    PSOut o;
    // the camera position, read here and now: d3dcompiler_47 otherwise re-reads G[L1.w] later with a wrong index register
    float3 camPos = 0;
    [loop] for (int cq = 0; cq < 1 + (int)(P2.w * 0); ++cq) camPos = G[L1.w].xyz;
    float4 X = ddx(i.t1), Y = ddy(i.t1);   // the view ray is affine on screen, so any derivative is exact
#ifdef BLEND_OUT
    float4 dest = 0;
#else
    float4 dest = tDest.SampleLevel(sPoint, i.t0.xy, 0);   // by uv, so captures can render smaller
#endif
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float3 gN = GeoN(i, depth);
#ifdef WRITE_DEPTH
    o.d = depth;
#endif
    int mode = L2.w;
    if (mode == 1 && i.t0.x < 0.5) return Out(o, dest, 1, 0);   // split screen: left half untouched

    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return Out(o, dest, 1, P10.y > 0.5 && !(L2.z & F_AIRLATER) ? tGlow.SampleLevel(sPoint, i.t0.xy, 0).rgb : 0);   // the sky: lamp glow only
    float3 rel = i.t1.xyz / dd;
    float3 world = rel + camPos;
    float dist = length(rel);
    float viewDepth = L2.x >= 0 ? dot(G[L2.x].xyz, rel) : dist;
    if (dist > 3000.0) {
        // far scenery (mountains past the map): only the air; the sky itself (no depth) stays as it is
        float3 K = 1, S = 0;
        if (dist < 60000.0 && mode != 1 && !(L2.z & F_AIRLATER)) {
            float hT, sh;
            float3 insc = AirLight(rel, dist, 0.5, false, -1, camPos, hT, sh);
            K = hT;
            S = insc * (1 - hT);
        }
        if (P10.y > 0.5 && !(L2.z & F_AIRLATER)) S += tGlow.SampleLevel(sPoint, i.t0.xy, 0).rgb;
        return Out(o, dest, K, S);
    }

    float4 N = tNormal.SampleLevel(sPoint, i.t0.xy, 0);
    bool useHalf = P5.w > 0.5;
    float noise = Ign(i.pos.xy);
    float halfAo = 1, halfShafts = -1, halfContact = 1;
    float3 bounce = 0;
    if (useHalf) {
        float4 acc = HalfFetch(tHalf, i.pos.xy, dist);
        halfAo = acc.x; halfShafts = acc.y; halfContact = acc.z;
        if (P7.w > 0.5) bounce = HalfFetch(tGI, i.pos.xy, dist).rgb;
    }
    float4 D = tDiffuse.SampleLevel(sPoint, i.t0.xy, 0);
    float4 Sp = (L2.z & F_SPEC) ? tSpec.SampleLevel(sPoint, i.t0.xy, 0) : 0;
    float addA = (L2.z & F_ADD) ? tAdd.SampleLevel(sPoint, i.t0.xy, 0).w : 0;

    // ---- the game's sun light without shadow (same maths as the game's shader)
    float3 n = normalize(N.xyz * 2 - 1);
    float power = SpecAA(exp2(N.w * -11.0 + 11.0001), n, DepthStep(i, depth));
    float3 albedo = D.rgb * D.rgb;
    float3 oneMinus = 1 - Sp.rgb;
    float3 Ld = G[L0.y].xyz;                                  // KeyLightDirection (light -> scene)
    float3 Lc = G[L0.x].xyz;
    float ndlRaw = dot(n, -Ld);
    float ndl = saturate(ndlRaw);
    float3 V = normalize(i.t1.xyz);
    float ndvSat = saturate(dot(n, V));
    float3 lam = albedo * ndl * oneMinus * 0.318310;
    float a1 = 1 - pow(1 - 0.5 * ndvSat, 5);
    float a2 = 1 - pow(1 - 0.5 * ndlRaw, 5);
    float3 ash = albedo * oneMinus * a1 * a2 * 0.387508 * ndl;
    // the diffuse model: variants with both pick it per material, others have only one
    float flag;
    if ((L2.z & F_LAMBERT) && (L2.z & F_ASHIKH)) flag = addA > 0.992157 ? 1 : 0;
    else flag = (L2.z & F_ASHIKH) ? 1 : 0;
    float3 diff = lam + flag * (ash - lam);
    float3 specT = 0;
    if (L2.z & F_SPEC) {
        float3 H = normalize(V - Ld);
        float ndh = saturate(dot(n, H));
        float vdh = dot(V, H);
        float sp = exp2(log2(min(ndh + 0.0001, 1)) * power) * (power + 1) * 0.039789;
        sp /= saturate(max(ndvSat, ndlRaw) * vdh) + 0.0013;
        float f = saturate(1 - vdh);
        float f5 = f * f * f * f * f;
        float3 Fr = Sp.rgb + oneMinus * Sp.w * f5;
        specT = Fr * sp * ndl;
    }
    float3 U = Lc * (diff + specT);
    // 1.29 / 1.30: our sun (SunPath): the light we add has its direction and colour, times sw (it rises and sets); the
    // game's key light kept x gw (the moon at night; 0 by day: it points elsewhere)
    bool ourSun = P19.w > 0.5;
    float sw = ourSun ? P18.w : 1, gw = ourSun ? P20.x : 0;
    float3 LdN = ourSun ? P18.xyz : Ld, LcN = ourSun ? P19.rgb : Lc;
    float ndlN = ndl;
    float3 UN = U;
    [branch] if (ourSun) {
        float ndlRawN = dot(n, -LdN);
        ndlN = saturate(ndlRawN);
        float3 lamN = albedo * ndlN * oneMinus * 0.318310;
        float a2N = 1 - pow(1 - 0.5 * ndlRawN, 5);
        float3 ashN = albedo * oneMinus * a1 * a2N * 0.387508 * ndlN;
        float3 specN = 0;
        if (L2.z & F_SPEC) {
            float3 H = normalize(V - LdN);
            float ndh = saturate(dot(n, H));
            float vdh = dot(V, H);
            float sp = exp2(log2(min(ndh + 0.0001, 1)) * power) * (power + 1) * 0.039789;
            sp /= saturate(max(ndvSat, ndlRawN) * vdh) + 0.0013;
            float f = saturate(1 - vdh);
            float f5 = f * f * f * f * f;
            specN = (Sp.rgb + oneMinus * Sp.w * f5) * sp * ndlN;
        }
        UN = LcN * (lamN + flag * (ashN - lamN) + specN);
    }

    // common factors: cloud shadows and the game's alpha term
    float F = saturate(1 - (1 - D.w) * 0.8);
    if (L2.z & F_CLOUD) {
        float4 m = O[L2.y];
        F *= tCloud.SampleLevel(sWrap, world.xz * m.z + m.w, 0).y * m.x + m.y;
    }
    float Fc = F * CloudShadow(world, -LdN, sWrap);   // our clouds' shadows (sky.inc): on our sun only

    // ---- shadow map: the game's filter and ours
    float blockerM = 0, penM = 0, cascDbg = -1, usedCoarse = 0;
    // our own maps' mask (sunshadow.inc): z = cascade, 0..3 ours (3 also its blend with the game's), 4 = past them
    float2 ssU = P10.z > 0.5 ? tSunMask.Load(int3(i.pos.xy, 0)) : float2(1, -1);
    // 1.69: cars' bodies (OnCar: their boxes) get their own care: smooth curved paint shows what flat walls hide. The sun
    // shadow there from 5 taps 2 px apart (same cascade): our maps' texels (a mirror's shadow in steps on the door) smoothed
    float carK = OnCar(world);
    [branch] if (carK > 0.5 && P10.z > 0.5) {
        float sm = ssU.x, nm = 1;
        [unroll] for (int q = 0; q < 4; ++q) {
            int2 o = q == 0 ? int2(2, 0) : q == 1 ? int2(-2, 0) : q == 2 ? int2(0, 2) : int2(0, -2);
            float2 t = tSunMask.Load(int3(int2(i.pos.xy) + o, 0));
            if (abs(t.y - ssU.y) < 0.5) { sm += t.x; nm += 1; }
        }
        ssU.x = sm / nm;
    }
    float4 ssM = float4(ssU.x, 0, ssU.y, 0);
    bool ssOurs = ssM.z >= 0 && (ssM.z < 3.5 || SP4.w > 0.5);   // (our sun: past our maps no shadow, not the game's map)
    if (ssOurs) {
        cascDbg = ssM.z;
        if (mode == 16 || mode == 17) {   // (the views of the caster distance and the penumbra: this frame's mask texel)
            float4 ssR = tSunRaw.Load(int3(int2(i.pos.xy) * int2(SP3.xy) / int2(SP3.zw), 0));
            blockerM = ssR.y; penM = blockerM > 0 ? max(blockerM * SP.x, SP.y) : 0;
        }
    }
    // without a shadow map the game uses a constant shadow term
    float gOrig = 1;
    if (!(L2.z & F_SHADOW) && L0.w >= 0) gOrig = G[L0.w].y * G[L0.w].y;
    float gNew = gOrig;
    if (L2.z & F_SHADOW) {
        int w = L1.z;
        float3 ls = world.y * G[w + 1].xyz + world.x * G[w].xyz + world.z * G[w + 2].xyz + G[w + 3].xyz;
        float4 so0 = G[L1.y], so1 = G[L1.y + 1], so2 = G[L1.y + 2];
        float2 cA = ls.xy, cB = ls.xy * so0.xy + so1.xy, cC = ls.xy * so0.zw + so2.xy;
        float3 sc;
        float casc;
        float scaleXY;
        if (max(abs(cA.x), abs(cA.y)) < 0.99) { sc = float3(cA * float2(0.5, -0.166667) + float2(0.5, 0.166667), ls.z); casc = 0; scaleXY = 1; }
        else if (max(abs(cB.x), abs(cB.y)) < 0.99) { sc = float3(cB * float2(0.5, -0.166667) + float2(0.5, 0.5), ls.z + so1.z); casc = 1; scaleXY = so0.x; }
        else { bool inC = max(abs(cC.x), abs(cC.y)) < 0.99; sc = float3(cC * float2(0.5, -0.166667) + float2(0.5, 0.833333), inC ? ls.z + so2.z : 0); casc = 2; scaleXY = so0.z; }

        // the game's own taps
        float r;
        if (L1.x >= 0) {
            float rad = saturate(1 - 0.025 * viewDepth);
            float2 off = G[L1.x].zw * rad;
            r = 0.2 * (CmpTap(sc.xy - off, sc.z) + CmpTap(sc.xy + off, sc.z) +
                       CmpTap(float2(sc.x + off.x, sc.y - off.y), sc.z) +
                       CmpTap(float2(sc.x - off.x, sc.y + off.y), sc.z) + CmpTap(sc.xy, sc.z));
        } else {
            r = CmpTap(sc.xy, sc.z);
        }
        float vfx = (L2.z & F_VFX) ? tVfx.SampleLevel(sLin, sc.xy, 0).x : 1;
        if (L2.z & F_VFX) r = saturate(vfx * r);
        float fade = saturate(-viewDepth * G[L0.w].w + G[L0.z].w);
        fade *= fade;
        float k = G[L0.w].y;
        float x = fade * (r - k) + k;
        gOrig = x * x;

      if (ssOurs) gNew = saturate(ssM.x);   // our maps: the sun disk's visible share (not squared like the game's)
      else if (P10.x > 0.5) {
        // ours: penumbra grows with the distance to the blocker (sun of P0.w x the real size)
        float2 texel = 1.0 / Size.zw;
        float yLo = casc / 3.0 + texel.y, yHi = (casc + 1) / 3.0 - texel.y;
        float mx = length(float3(G[w].x, G[w + 1].x, G[w + 2].x));       // light-space x per metre
        float mz = length(float3(G[w].z, G[w + 1].z, G[w + 2].z));       // light-space z per metre
        float texPerM = mx * scaleXY * 0.5 * Size.z;
        float sn, cs;
        sincos(noise * 6.2831853, sn, cs);
        float2x2 rot = float2x2(cs, -sn, sn, cs);
        // everything in metres: P1.x / P1.y = min / max softness in cm; sun size P0.w x the real sun
        float minSoftM = P1.x * 0.01, maxSoftM = P1.y * 0.01;
        float searchTex = clamp(maxSoftM * texPerM, 2, 32);
        // Distance to the caster: searched in this cascade (fine detail, keeps small objects sharp).
        // In the nearest cascade tall casters do not fit the depth range and get clamped to its
        // near plane ("pancaked"); only for those samples the wider cascade tells the real distance.
        float bSum = 0, bCnt = 0, flat = 0;
        [unroll] for (int b = 0; b < 8; ++b) {
            float2 uv = sc.xy + mul(rot, kPoisson[b * 2]) * searchTex * texel;
            uv.y = clamp(uv.y, yLo, yHi);
            float st = tShadow.SampleLevel(sPoint, uv, 0);
            if (IsBlocker(st, sc.z)) {
                bool pancaked = (L2.z & F_CMPGREAT) ? st > 0.9995 : st < 0.0005;
                if (pancaked) flat += 1; else { bSum += st; bCnt += 1; }
            }
        }
        float3 bc = sc;
        if (flat > 0 && casc < 0.5 && max(abs(cB.x), abs(cB.y)) < 0.99) {
            bc = float3(cB * float2(0.5, -0.166667) + float2(0.5, 0.5), ls.z + so1.z);
            float bTexPerM = mx * so0.x * 0.5 * Size.z;
            float bSearch = clamp(searchTex / texPerM * bTexPerM, 1, 32);   // same radius in metres
            float bBias = 0.15 * mz;                                          // must not find the receiver
            float cSum = 0, cCnt = 0;
            [unroll] for (int c = 0; c < 8; ++c) {
                float2 uv = bc.xy + mul(rot, kPoisson[c * 2 + 1]) * bSearch * texel;
                uv.y = clamp(uv.y, 1.0 / 3.0 + texel.y, 2.0 / 3.0 - texel.y);
                float st = tShadow.SampleLevel(sPoint, uv, 0);
                if (IsBlocker(st, (L2.z & F_CMPGREAT) ? bc.z + bBias : bc.z - bBias)) { cSum += st; cCnt += 1; }
            }
            if (cCnt > 0) { bSum = cSum; bCnt = cCnt; usedCoarse = 1; }
            else bc = sc;
        }
        // The distance to the caster also from the widest cascade, where every caster fits (a far crane is cut off
        // the near cascades): the larger of the two, so the softness does not depend on which cascade the receiver
        // falls in (no seams along long shadows, no sharpening when the camera comes closer).
        float coarseM = 0;
        if (max(abs(cC.x), abs(cC.y)) < 0.99) {
            float3 cc = float3(cC * float2(0.5, -0.166667) + float2(0.5, 0.833333), ls.z + so2.z);
            float cTexPerM = mx * so0.z * 0.5 * Size.z;
            float cSearch = clamp(maxSoftM * 2 * cTexPerM, 1.5, 16);
            float cBias = (0.3 + 1.5 / max(cTexPerM, 1e-3)) * mz;       // past the receiver's own texels
            float cSum = 0, cCnt = 0;
            [unroll] for (int c = 0; c < 8; ++c) {
                float2 uv = cc.xy + mul(rot, kPoisson[c * 2 + 1]) * cSearch * texel;
                uv.y = clamp(uv.y, 2.0 / 3.0 + texel.y, 1.0 - texel.y);
                float st = tShadow.SampleLevel(sPoint, uv, 0);
                if (IsBlocker(st, (L2.z & F_CMPGREAT) ? cc.z + cBias : cc.z - cBias)) { cSum += st; cCnt += 1; }
            }
            if (cCnt > 0) coarseM = abs(cc.z - cSum / cCnt) / max(mz, 1e-6);
        }
        float rn = 1;
        if (bCnt > 0 || coarseM > 0) {
            blockerM = max(bCnt > 0 ? abs(bc.z - bSum / bCnt) / max(mz, 1e-6) : 0, coarseM);
            // sharp until the caster is P2.z metres away, then the penumbra grows like a real sun's (0.53 deg) x P0.w
            penM = clamp(minSoftM + max(blockerM - SharpM, 0) * 0.00925 * P0.w, minSoftM, maxSoftM);
            float blur = clamp(penM * texPerM, 0.5, 32);
            float acc = 0;
            [unroll] for (int t = 0; t < 16; ++t) {
                float2 uv = sc.xy + mul(rot, kPoisson[t]) * blur * texel;
                uv.y = clamp(uv.y, yLo, yHi);
                acc += CmpTap(uv, sc.z);
            }
            rn = acc / 16;
        }
        if (L2.z & F_VFX) rn = saturate(vfx * rn);
        cascDbg = casc;
        float xn = fade * (rn - k) + k;
        gNew = xn * xn;
      } else gNew = gOrig;   // the game's own shadow
    } else if (ssOurs) gNew = saturate(ssM.x) * gOrig;   // a variant without the game's map: ours times its constant

    // (no ?: here: HLSL evaluates both sides of it, so the full-resolution march would run anyway)
    float contact = halfContact, ao = halfAo;
    // the surface's small shapes (1.12: stronger on walls, none on paint / metal, none where the plane is not known: at an
    // object's outline the depth's slope is not the surface's - it drew false shadows along edges)
    float relStr = P17.x * lerp(1, P17.w, saturate(1.4 - abs(gN.y) * 2)) * (1 - saturate((Lum(Sp.rgb) - 0.06) * 20)) *
                   saturate((dot(gN, n) - 0.75) * 8);
    if (relStr > 0 && length(X.xyz / dd) < 0.016) relStr *= 1 - OnCar(world);   // (1.50: none on cars; 1.52: only where Relief works: near)
    float2 relief = Relief(i, rel, n, gN, -LdN, dd, X, Y, noise, relStr);
    [branch] if (!useHalf) {
        if (ndlN > 0 && gNew > 0.01) contact = ScreenContact(i, rel, n, LdN, dd, dist, noise, X, Y);
        ao = ScreenAO(i, rel, n, dd, dist, noise, X, Y);
    }

    float3 sunOrig = U * gOrig * F;
    contact *= relief.x;
    ao *= relief.y;
    // 1.69: on cars the screen-space AO and contact shadows a third as strong: the depth of curved panels, thin mirrors and
    // trims made blotches in steps (half resolution); the game's own car shading keeps their shape
    ao = lerp(ao, 1 - (1 - ao) * 0.3, carK);
    contact = lerp(contact, 1 - (1 - contact) * 0.3, carK);
    // the sun, modern (P16.x) or the game's model
    float pAlpha = PbrAlpha(power);
    float3 pF0 = PbrF0(Sp);
    float3 Un = UN;
    if (P16.x > 0.5) Un = LcN * (albedo * (1 - pF0) * 0.318310 * ndlN * PbrDiffK(n, V, -LdN, pAlpha) + PbrSpecK(n, V, -LdN, pF0, pAlpha) * ndlN);
    float3 sunNew = Un * gNew * contact * Fc * P2.y;
    if (ourSun) sunNew = sunNew * sw + (P20.y > 0.5 ? U * gNew * Fc * P2.y : sunOrig) * gw;   // (the moon: with our maps, or its own shadow)
    float3 ambient = dest.rgb - sunOrig;
    // Natural shadows: where the sun is blocked, the fill light is darker and bluer (sky light only).
    float shadowAmt = (1 - saturate(gNew * contact)) * saturate(ndlN * 6) + (1 - saturate(ndlN * 6));
    shadowAmt = saturate(shadowAmt) * saturate(dot(LcN, float3(0.3, 0.6, 0.1)) * 4) * sw;   // no effect at night
    float3 fill = lerp(1.0, P3.y, shadowAmt) * lerp(1.0, float3(0.86, 0.96, 1.16), P3.z * shadowAmt * (1 - 0.6 * carK));   // (1.69: less blue on paint)
    // light colour: warmer / cooler sun, and a tint of the fill light (luminance kept)
    float3 warm = float3(1 + 0.18 * P3.w, 1 + 0.02 * P3.w, 1 - 0.30 * P3.w);
    warm /= dot(warm, float3(0.2126, 0.7152, 0.0722));
    float3 amb = float3(1 + 0.12 * P4.x, 1, 1 - 0.18 * P4.x);
    amb /= dot(amb, float3(0.2126, 0.7152, 0.0722));
    // bounce light from the sunlit surfaces around (diffuse only, like the game's sun term)
    // bounce light and lamp light (strengths applied in the half-res pass)
    float3 gi = albedo * oneMinus * bounce;

    // result = ambient * K + add, then the air on top: result * hazeT + inscatter * (1 - hazeT)
    // 1.66 (AOMultiBounce): light bounced around inside the occluded spot comes back to it: bright surfaces lose less of
    // their ambient (Jimenez et al. 2016: a cubic in ao per albedo channel, fitted to ground truth)
    float3 aoK = ao;
    if (P25.y > 0.5) {
        float3 ka = 2.0404 * albedo - 0.3324, kb = -4.7951 * albedo + 0.6417, kc = 2.7552 * albedo + 0.6903;
        aoK = max(ao, ((ao * ka + kb) * ao + kc) * ao);
    }
    float3 K = P2.x * aoK * fill * amb;
    // at night the baked light and the sky light are dimmed (the lamps light the scene instead)
    K *= lerp(P9.z, 1, saturate((Lum(G[L0.x].xyz) - 0.5) / 2));
    // lamps at full resolution with highlights (the game's own V and gloss)
    float3 lamps = 0;
    if (P10.y > 0.5) {   // made by PS_Lamps at the shown resolution (or half); the glow here only without the late air
        lamps = LampLightAt(i.t0.xy, dist);
        // the strongest lamp's highlight, at full size
        if (L2.z & F_SPEC) {
            float4 bb = tLampBest.Load(int3(min(int2(i.t0.xy * P10.zw), int2(P10.zw) - 1), 0));
            if (bb.x > 0.5) lamps += LampSpecOne((int)(bb.x - 0.5), world, n, V, Sp, power, oneMinus) * bb.y * P8.x;
        }
        if (!(L2.z & F_AIRLATER)) lamps += GlowAt(i.t0.xy, dist);
    }
    else if (P8.x > 0 && LampN.x > 0 && dist < P7.z) lamps = LampShade(world, n, V, albedo, oneMinus, Sp, power, (L2.z & F_SPEC) != 0, i.t0.xy,
                                                                     P9.x > 0 && dist < 120, P9.w > 0, i, rel, dd, X, Y, noise) * P8.x;
    // the sky reflected (modern lighting): the game's sky-light cube in the mirror direction, weighted by the split-sum
    // share for this roughness; shadowed by the occlusion (screen AO, sky occlusion, the texture's own). Not on paint /
    // metal / glass (screen reflections and the car's own reflections do those).
    float3 envS = 0;
    if (P16.x > 0.5 && P16.z > 0 && (L2.z & F_IRR)) {
        float3 R = reflect(-V, n);
        float3 c = tGameIrr.SampleLevel(sLin, R, 0).rgb;
        float metal = saturate((Lum(Sp.rgb) - 0.06) * 20);
        envS = 4 * c * c * D.w * PbrEnv(pF0, sqrt(pAlpha), ndvSat) * ao * P2.x * P16.z * (1 - metal);
        envS *= 1 - 0.9 * UnderPlayer(world) * saturate((n.y - 0.6) * 4);   // (1.73: no sky under the player's car)
        envS *= lerp(P9.z, 1, saturate((Lum(G[L0.x].xyz) - 0.5) / 2));   // at night as dim as the rest of the sky light
    }
    float3 add = sunNew * warm + gi + lamps + envS;
    float hazeT = 1, shafts = 1;
    float3 insc = 0;
    if (!(L2.z & F_AIRLATER)) insc = AirLight(rel, dist, noise, (L2.z & F_SHADOW) != 0, halfShafts, camPos, hazeT, shafts);

    bool dbg = true;
    float3 dv = 0;
    if (mode == 2) dv = contact.xxx;
    else if (mode == 3) dv = ao.xxx;
    else if (mode == 4) dv = (gNew * contact).xxx;
    else if (mode == 5) dv = gOrig.xxx;
    else if (mode == 7) dv = sunOrig * 4;                                  // the game's sun light (x4)
    else if (mode == 8) dv = lerp(dest.rgb, float3(1, 0, 1), 0.5);       // where this pass runs
    else if (mode == 9) dv = ambient;              // test harness only
    else if (mode == 10) dv = (L2.z & F_ADD) ? pow(tAdd.SampleLevel(sPoint, i.t0.xy, 0).rgb, 2) : 0;  // additive buffer
    else if (mode == 11) dv = D.w.xxx;            // diffuse alpha
    else if (mode == 12) dv = ndl.xxx;            // N.L to the sun
    else if (mode == 13) dv = dest.rgb;           // the game's image before us
    else if (mode == 14) dv = albedo;             // albedo
    else if (mode == 15) dv = F.xxx;              // cloud x alpha factor
    else if (mode == 16) dv = usedCoarse > 0 ? float3(saturate(blockerM / 10), 0.1, 0.1) : saturate(blockerM / 10).xxx;  // white = 10 m, red = from the wider cascade
    else if (mode == 17) dv = saturate(penM / (ssOurs ? 0.5 : max(P1.y * 0.01, 0.01))).xxx;  // penumbra width (ours: white = 50 cm)
    else if (mode == 19) dv = (1 - hazeT).xxx;          // haze amount
    else if (mode == 20) dv = shafts.xxx;               // light shafts: air that sees the sun
    else if (mode == 21) dv = gi * 4;                   // bounce light (x4)
    else if (mode == 23) dv = lamps * 4;                // lamp light alone (x4)
    else if (mode == 22) dv = bounce;                    // bounce source: green = previous frame, red = sun only
    else if (mode == 18) dv = cascDbg < 0 ? 0 : cascDbg < 0.5 ? float3(1, 0.3, 0.3) : cascDbg < 1.5 ? float3(0.3, 1, 0.3) : cascDbg < 2.5 ? float3(0.3, 0.3, 1) : float3(1, 0.9, 0.2);
    else if (mode == 33) dv = P10.z > 0.5 ? tSunRaw.Load(int3(int2(i.pos.xy) * int2(SP3.xy) / int2(SP3.zw), 0)).xxx : 0;   // our shadow, this frame alone
    else if (mode == 34) dv = envS * 4;                                  // the sky reflected (modern lighting), x4
    else if (mode == 35) dv = sqrt(pAlpha).xxx;                          // roughness (white = rough)
    else if (mode == 36) dv = relief.y * float3(1, relief.x, relief.x);  // relief: grey = cavities, red = small sun shadows
    else if (mode == 6) dv = (min(ambient.r, min(ambient.g, ambient.b)) < -0.02) ? float3(1, 0, 0) : max(ambient, 0);
    else if (mode == 25) dv = N.w.xxx;                                   // gloss in the game's G-buffer
    else if (mode == 26) dv = (L2.z & F_SPEC) ? tSpec.SampleLevel(sPoint, i.t0.xy, 0).rgb : 0;   // specular colour
    else if (mode == 27) {                                               // rain: blue = wet, white = puddles, red = dry under a roof
        float pd, cv, pg, pw;
        float w = Wetness(rel, WetUp(n, gN), i.t1, X, Y, dd, i.pos.xy, tDiffuse.SampleLevel(sLin, i.t0.xy, 0).rgb, world, pd, cv, pg, pw);
        float upF = saturate((WetUp(n, gN).y - 0.7) * 5);
        dv = lerp(dest.rgb * 0.3, float3(0.15, 0.35, 1), saturate(w)) + pd * 0.6;
        if (upF > 0.5 && w <= 0 && P12.y > 0 && tDiffuse.SampleLevel(sLin, i.t0.xy, 0).g <= max(tDiffuse.SampleLevel(sLin, i.t0.xy, 0).r, tDiffuse.SampleLevel(sLin, i.t0.xy, 0).b)) dv = float3(0.8, 0.15, 0.1);
    }
    else dbg = false;
    if (dbg) return Out(o, dest, 0, mode == 8 || mode == 13 ? dv : dv * DbgScale());   // (8, 13: the game's own image)
    // 1.74 (CarRainDrops): rain on the player's car (the wet-ground amount, out in the open). The drops that form, grow,
    // hold and run are simulated (carrain.inc: t73 the atlas of the car's five unfoldings, t74 / t75 the drops, t76 their
    // drying trails); here a pixel finds the nearest drop round it (2 x 2 atlas texels) and takes its exact shape: a dome,
    // a runlet thinning behind a moving one. Between them fine static beads (procedural, fixed on the body), wiped where a
    // drop ran, blown off the top / front at speed. A drop mirrors the sky round its edge (Fresnel), the sun as a
    // pin-point, and shows the paint darker through its middle; a trail leaves the paint wetter.
    float3 dropDbg = 0;   // (view 37)
    // 1.80 (CarRainDrops): rain on the player's car. The drops are simulated (carrain.inc: on the car's own shape, baked
    // from its draws; rain lands as small drops, they gather the fine droplets round them and merge when they touch, the
    // big ones break loose and run down the slopes, sweeping up what they meet, leaving a wet track). Here a pixel takes
    // the nearest drop of the atlas round it (2 x 2 texels) and shades it as a water cap (contact angle ~80 deg): its
    // mirror (Fresnel: the sky on its upper side, the ground below; the overcast sky's own soft highlight), the sun's
    // glint, a lens (the light it takes in gathers on its far side), its rim dark (the steep water bends the light away),
    // its shadow on the paint (its footprint moved away from the sun) with the focused spot inside. A drop smaller than
    // the pixel is spread over the pixels round it, fainter, as its average (neither flickering nor gone far away). The
    // drops stay when the rain stops (they dry slowly) and under a roof. 1.81: on the body's outer surface only, and on
    // the glass (the cabin seen through it: where the view ray leaves the car's baked shape).
    float3 Ppl, ax, az;
    bool poseOk = PlayerPose(Ppl, ax, az);
    [branch] if (P27.w > 0 && (P28.x > 0.5 || P12.y > 0) && poseOk && InPlayerBody(world, Ppl, ax, az)) {
        float rk = P27.w * saturate(P12.y) * RainOpenAt(world);   // (the fine droplets' sheen: rain now, out in the open)
        {
            float3 ay = normalize(cross(az, ax));
            if (ay.y < 0) ay = -ay;
            float3 lw = world - Ppl;   // (1.78: this frame's pose: the drops ride with the car)
            float3 lp = float3(dot(lw, ax), dot(lw, ay), dot(lw, az));
            float3 toC = normalize(-rel);
            // 1.81: the surface the drops sit on (with the car's shape baked): this pixel's own where it is the body's
            // outer surface (the shape meets it within 4 cm); where the pixel is the cabin seen through the glass (under
            // the shape's surface: the glass is not in the game's G-buffer; 1.80 put the drops in the cabin), the glass:
            // where the view ray goes out of the shape (bisected between the cabin point, inside it, and the car's box);
            // none where the pixel stands out of the shape (the wheels)
            float3 nS = n;
            float pxs = length(X.xyz / dd) / max(abs(dot(n, toC)), 0.25);
            int pl = FaceOf(float3(dot(n, ax), dot(n, ay), dot(n, az)));
            bool glass = false, onCar = pl >= 0;
            float3 lS = lp;
            [branch] if (P29.z > 0.5 && pl >= 0) {
                float off = FaceSurf(pl, CarMapD(pl, FaceP(pl, lp))) - FaceOut(pl, lp);   // > 0: under the shape's surface
                if (off > 0.04) {
                    float3 rl = float3(dot(rel, ax), dot(rel, ay), dot(rel, az));
                    float len = length(rl);
                    float3 rd = -rl / max(len, 1e-4);   // from the pixel towards the camera (car-local)
                    float3 bx = ((rd > 0 ? float3(1.25, 1.95, 2.85) : float3(-1.25, -0.2, -2.85)) - lp) / rd;   // (out of the car's box)
                    bx = abs(rd) < 1e-5 ? 1e9 : bx;
                    float t0 = 0, t1 = min(min(min(bx.x, bx.y), bx.z), len);
                    int hp = pl;
                    if (HullIn(lp + rd * t1, hp) < 0) {
                        [unroll] for (int it = 0; it < 8; ++it) { float tm = (t0 + t1) * 0.5; if (HullIn(lp + rd * tm, hp) > 0) t0 = tm; else t1 = tm; }
                        HullIn(lp + rd * t0, hp);   // (the face it crosses there)
                        lS = lp + rd * t1;
                        pl = hp;
                        float3 ng = FaceNormal(pl, FaceP(pl, lS));
                        nS = normalize(ax * ng.x + ay * ng.y + az * ng.z);
                        pxs = length(X.xyz / dd) * saturate(1 - t1 / max(len, 1e-3)) / max(abs(dot(nS, toC)), 0.25);
                        glass = true;
                    } else onCar = false;
                } else if (off < -0.04) onCar = false;
            }
            float2 pp = FaceP(max(pl, 0), lS);
            float3 Uw = pl == 1 || pl == 2 ? az : ax, Vw = pl == 0 ? az : ay;
            float far = smoothstep(0.003, 0.009, pxs);
            // the light along the surface: the sun where it shines (shadow, caustic, lens), else the sky above
            float cosT = saturate(dot(nS, -LdN));
            float sunK = saturate(gNew * Fc * cosT * 3);
            float3 ltgR = -LdN - nS * dot(-LdN, nS);
            float2 l2 = float2(dot(ltgR, Uw), dot(ltgR, Vw));
            float sOff = min(0.8 * length(l2) / max(cosT, 0.2), 2.5);
            float2 sdir = l2 / max(length(l2), 1e-4);
            float3 upT = ay - nS * dot(ay, nS);
            float3 Lt = normalize(lerp(upT, ltgR, sunK) + 1e-5);
            float trail = 0, bestD = 9, bestR = 1, bestF = 1, shSun = 0, causS = 0, rimL = 0;
            bool bestMove = false;
            float2 bestO = 0;
            [branch] if (onCar && P28.x > 0.5) {
                float2 tc = CrAtlas(pl, pp);
                trail = tDropTrail.SampleLevel(sLin, tc / float2(2000, 2250), 0);
                int2 b0 = int2(floor(tc - 0.5));
                [unroll] for (int k = 0; k < 4; ++k) {
                    uint id = tDropAtlas.Load(int3(clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), int2(1999, 2249)), 0));
                    if (id == 0) continue;
                    uint idx = id - 1;
                    int3 sq = int3(idx % 128, idx / 128, 0);
                    float4 S = tDropS.Load(sq), Vd = tDropV.Load(sq);
                    if (S.z <= 0 || abs(S.w - pl) > 0.5) continue;
                    float fade = saturate((S.w - pl) * 40);   // (1.81c: its age: in over 0.25 s)
                    float sp = length(Vd.xy);
                    float2 tl = sp > 1e-3 ? -Vd.xy / sp * min(sp * 0.1, 0.025) : 0;   // (carrain.inc's runlet)
                    float t = saturate(dot(pp - S.xy, tl) / max(dot(tl, tl), 1e-12));
                    float rr = S.z * (1 - 0.4 * t);
                    float2 o = (pp - S.xy - tl * t) / rr;               // from its axis, in radii
                    float d = length(o);
                    float fwk = pxs / rr, Rk = sqrt(1 + 0.25 * fwk * fwk);   // the pixel in its radii; how far it spreads
                    if (d / Rk < bestD) { bestD = d / Rk; bestO = o; bestR = rr; bestMove = sp > 1e-3; bestF = fade; }
                    float res = glass ? 0 : saturate(1.6 - fwk) * fade;   // (its shadow, focused spot, rim line: resolved, on paint)
                    float lenS = length(o + sdir * sOff);
                    shSun = max(shSun, (1 - smoothstep(0.8, 1.05, lenS)) * smoothstep(0.9, 1.1, d) * res);
                    causS = max(causS, (1 - smoothstep(0.08, 0.32, lenS)) * (d > 0.95 ? 1.0 : 0.6) * res);
                    rimL = max(rimL, smoothstep(0.85, 1.0, d) * (1 - smoothstep(1.0, 1.25, d)) * res);
                }
            }
            // the track a run leaves: tiny droplets (only there)
            float2 oR = 0;
            float dR = onCar && trail > 0.08 ? Bead(pp, 0.005, saturate(trail * 1.3) * 0.55, 5.1, 1.0, P29.x, oR) : -1;
            float fw = pxs / bestR, Rs = sqrt(1 + 0.25 * fw * fw);
            float cov = 0, x = 0, s = saturate(1.6 - fw);
            float2 dir2 = 0;
            if (bestD < 1) { cov = saturate((1 - bestD) * Rs / (0.5 * fw + 0.08)) / (Rs * Rs) * bestF; x = min(bestD * Rs, 0.999); dir2 = bestO / max(length(bestO), 1e-4); }
            else if (dR > 0) { cov = smoothstep(0, 0.15, dR) * (1 - far); x = 1 - dR; dir2 = oR / max(length(oR), 1e-4); s = saturate(1.6 - pxs / 0.0012); }
            cov = saturate(cov * P33.w);   // (1.88: CarDropsVisible: a far drop's share of its pixel x this)
            dropDbg = float3(bestD < 1 ? (bestMove ? 0.1 : 1.0) : 0, bestD < 1 && bestMove ? 1.0 : 0, dR > 0 ? 0.8 : 0) * saturate(cov * 4) + float3(0.9, 0.8, 0) * trail * 0.5;
            // (view 37) 1.80: the car's baked shape: green where the drops sit on this surface, blue on the glass in front
            // of it, red: none (out of the shape: a wheel)
            if (mode == 37 && P29.z > 0.5) dropDbg += !onCar ? float3(0.4, 0, 0) : glass ? float3(0, 0.1, 0.45) : float3(0, 0.3, 0);
            // far away (a pixel over ~4 mm of paint): the fine droplets' average (a little darker, the sky's sheen)
            if (onCar) {
                float ndv0 = saturate(dot(nS, toC));
                float avgCov = 0.15 * rk * far;
                add += PanoSky(reflect(-toC, nS), 0.8) * (0.05 + 0.3 * pow(1 - ndv0, 3)) * avgCov;
                K *= 1 - 0.2 * avgCov;
            }
            K *= 1 - 0.3 * rimL;                                     // where its rim meets the paint
            add -= sunNew * saturate(shSun - causS) * 0.7;           // the sun in its shadow
            add += sunNew * causS * 1.4;                             // focused: brighter than the direct sun
            K *= 1 - 0.08 * saturate(trail) * (glass ? 0 : 1);      // its track, wet
            if (cov > 0.001) {
                // the cap at this point (the pixel smaller than the drop): tilted by asin(x sin 80 deg) at x radii
                float st = x * 0.985, tilt = min(st / sqrt(1 - st * st), 5.0);
                float3 tw = Uw * dir2.x + Vw * dir2.y;
                float3 nd = normalize(nS + tw * tilt);
                float ndv = saturate(dot(nd, toC));
                float fr = 0.02 + 0.98 * pow(1 - ndv, 5);
                float3 rv = reflect(-toC, nd);
                float3 env = lerp(max(ambient, 0) * 0.25, PanoSky(rv, 0.97), saturate(rv.y * 4 + 0.5));
                float3 hv = normalize(toC - LdN);
                float ndh = saturate(dot(nd, hv)), dg = ndh * ndh * (0.0016 - 1) + 1;
                float fs = 0.02 + 0.98 * pow(1 - saturate(dot(hv, toC)), 5);
                float sunLit = gNew * Fc * P2.y;
                float3 sunS = LcN * min(0.0016 / (3.14159 * dg * dg) * fs / (4 * max(ndv, 0.1)), 40) * saturate(dot(nd, -LdN)) * sunLit;
                // the overcast sky's own highlight (its bright part above): a soft spot on the drop's upper side
                float3 hz = normalize(toC + float3(0, 1, 0));
                float nz = saturate(dot(nd, hz)), dz = nz * nz * (0.06 - 1) + 1;
                float3 skyS = PanoSky(float3(0, 1, 0), 0.6) * min(0.06 / (3.14159 * dz * dz), 5) * fr * 0.6;
                float edge = smoothstep(0.55, 0.95, x);
                // the lens: the light it takes in gathers on its side away from the light, mid-radius
                float toL = dot(normalize(tw + 1e-5), Lt);
                float lensB = saturate(-toL) * saturate(x * (1.2 - x) * 3);
                float3 eAdd = env * fr + sunS + skyS + (sunNew * 0.9 + max(ambient, 0) * 0.35 * (1 - sunK)) * lensB - sunNew * 0.5 * edge;
                float eK = 1 - (0.08 + 0.62 * edge + 0.15 * saturate(toL) * (1 - edge)) * (1 - fr);
                // its average (the pixel bigger than it): ~10% of the sky mirrored (more at a grazing view), the sun's
                // glint spread over it (a mirror ball's: a quarter of its Fresnel), the paint through it darker
                float fA = 0.02 + 0.98 * pow(1 - saturate(dot(hv, toC)), 5);
                float3 aAdd = PanoSky(reflect(-toC, nS), 0.6) * (0.08 + 0.2 * pow(1 - saturate(dot(nS, toC)), 3)) +
                              PanoSky(float3(0, 1, 0), 0.6) * 0.03 + LcN * fA * 0.25 * sunLit * step(0.17, dot(hv, nS));
                add += lerp(aAdd, eAdd, s) * cov;
                K *= 1 - cov * (1 - lerp(0.75, eK, s));
            }
        }
    }
    if (mode == 37) return Out(o, dest, 0, dest.rgb * 0.12 + dropDbg);   // 1.76: the car's rain drops (debug)
#ifdef BLEND_OUT
    // dest * K * hazeT + ... = (ambient + sunOrig) * K * hazeT + ...
    return Out(o, dest, K * hazeT, (add - sunOrig * K) * hazeT + insc * (1 - hazeT));
#else
    o.c = float4((max(ambient, 0) * K + add) * hazeT + insc * (1 - hazeT), dest.a);
    return o;
#endif
}

// Sky light with occlusion: how much of the sky a point sees. 4 directions along the surface (rotated per pixel), 6
// steps each out to P11.y metres (denser near the point): the highest surface seen above the tangent plane in each
// direction (its sine) is the horizon; what is above it is sky. Far samples count less (they may be in front of the
// point rather than over it). 1 = open sky, 0 = closed in.
float SkyVis(PSIn i, float3 rel, float3 n, float dd, float dist, float noise, float4 X, float4 Y)
{
    if (P11.x <= 0 || dist > 400) return 1;
    float R = P11.y * (1 + dist * 0.01);                     // a little wider far away (fewer pixels per metre)
    float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tx = normalize(cross(up, n)), ty = cross(n, tx);
    float3 start = rel + n * 0.05;
    float vis = 0;
    [unroll] for (int a = 0; a < 4; ++a) {
        float ang = (a + noise) * 1.5707963;
        float3 dir = tx * cos(ang) + ty * sin(ang);
        float hs = 0;                                          // the highest horizon so far (sine)
        RayProj rp = ProjSetup(start, dir, i.t1, X, Y, dd);
        bool alive = true;
        [unroll] for (int s = 0; s < 6; ++s) {   // (1.65: read together)
            float f = (s + frac(noise * 7.13 + a * 0.37)) / 6;
            float2 dp; float4 rayAt;
            alive = alive && ProjAt(rp, R * f * f + 0.1, i.t1, X, Y, dp, rayAt);
            float2 uv = (i.pos.xy + dp) / Size.xy;
            alive = alive && all(uv >= 0) && all(uv <= 1);
            float z = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
            float3 v = rayAt.xyz / z - rel;                     // the surface seen there, from the point
            float lv = length(v);
            float sh = dot(v, n) / max(lv, 1e-4);
            float fall = saturate(1 - lv * lv / (R * R * 1.5));
            if (alive && abs(z) >= 1e-9) hs = max(hs, sh * fall);   // (the sky there: nothing)
        }
        // 1.66 (SkyOcclusionCosine): the sky's share above a horizon of sine h, weighted by cos (as light falls on the
        // surface: GTAO's integral) is 1 - h^2; the plain angle share 1 - h darkened low horizons as much as high walls
        vis += P25.x > 0.5 ? 1 - hs * hs : 1 - hs;
    }
    vis *= 0.25;
    vis = vis * vis * (3 - 2 * vis);                            // a little more contrast: open stays open
    return lerp(1, vis, saturate(P11.x));
}

// The half-res AO / shafts / contact target smoothed: the occlusion over 5x5 (1-4-6-4-1, depth-aware; 1.49: was a 4x4
// box half a pixel off), the rest as it is. 1.49: then over frames: the last result where this surface was (reprojected,
// its distance alike), held within this frame's neighbourhood (mean +- 1.5 spread), 85%: the noise of its 8 samples,
// turned anew each frame, settles (it shimmered)
Texture2D<float4> tAoHist : register(t71);
float4 PS_ABlur(PSIn i) : SV_Target0
{
    int2 p = int2(i.pos.xy);
    int2 hmax = int2(Size.xy) - 1;
    float4 c = tHalf.Load(int3(p, 0));
    float acc = 0, ws = 0, m1 = 0, m2 = 0, nn = 0;
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) {
            float4 h = tHalf.Load(int3(clamp(p + int2(x, y), 0, hmax), 0));
            float same = saturate(1 - abs(h.w - c.w) / (c.w * 0.03 + 0.1));
            float g = (abs(x) == 2 ? 1 : abs(x) == 1 ? 4 : 6) * (abs(y) == 2 ? 1 : abs(y) == 1 ? 4 : 6);
            acc += h.x * (same * g + 1e-4);
            ws += same * g + 1e-4;
            if (same > 0.5 && abs(x) <= 1 && abs(y) <= 1) { m1 += h.x; m2 += h.x * h.x; nn += 1; }
        }
    float r = acc / ws;
    if (P24.w > 0.5 && c.w < 5e4) {
        float dd = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x - i.t1.w;
        float2 puv; float pd;
        if (abs(dd) > 1e-9 && Reproject(HistPoint(i.t1.xyz / dd + G[L1.w].xyz, i.t0.xy), dd, puv, pd)) {   // (1.59)
            float hw = tAoHist.Load(int3(clamp(int2(puv * Size.xy), 0, hmax), 0)).w;
            if (abs(hw - pd) < pd * 0.02 + 0.1) {
                float mu = nn > 0 ? m1 / nn : r, sg = nn > 0 ? sqrt(max(m2 / nn - mu * mu, 0)) : 0;
                float h = tAoHist.SampleLevel(sLin, puv, 0).x;
                h = h == h ? h : r;
                r = lerp(r, clamp(h, min(r, mu) - 1.5 * sg - 0.03, max(r, mu) + 1.5 * sg + 0.03), 0.85);
            }
        }
    }
    return float4(r, c.yzw);
}

// ---- half resolution: AO, light shafts, contact shadows and bounce light (the expensive screen-space parts)
struct HalfOut { float4 a : SV_Target0; float4 gi : SV_Target1; };

HalfOut PS_Half(PSIn i)
{
    HalfOut o;
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    o.a = float4(1, 1, 1, 6e4);
    o.gi = float4(0, 0, 0, 6e4);
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return o;
    float3 rel = i.t1.xyz / dd;
    float dist = length(rel);
    if (dist > 3000.0) return o;
    float3 n = normalize(tNormal.SampleLevel(sPoint, i.t0.xy, 0).xyz * 2 - 1);
    float3 Ld = SunLd();
    float noise = Ign(i.pos.xy);
    o.a = float4(ScreenAO(i, rel, n, dd, dist, noise, X, Y) * SkyVis(i, rel, n, dd, dist, noise, X, Y),
                 ((L2.z & F_SHADOW) && P5.x > 0 && P4.y > 0) ? MarchShafts(rel, dist, noise) : 1,
                 HalfContact(i, rel, n, Ld, dd, dist, noise, X, Y), dist);
    float3 b = 0;
    float2 src = 0;
    if (P6.x > 0 && dist < 400) b = ScreenGI(i, rel, n, dd, X, Y, src) * P6.x;

    if (L2.w == 22) b = float3(src.y, src.x, 0);
    o.gi = float4(b, dist);
    return o;
}

// 4x4 depth-aware average of the bounce light (covers the whole 4x4 sample pattern), then blended with
// the previous frames' result where the same surface was visible (reprojected).
float4 PS_GIBlur(PSIn i) : SV_Target0
{
    int2 p = int2(i.pos.xy);
    int2 hmax = int2(Size.xy) - 1;
    float dc = tGIRaw.Load(int3(p, 0)).w;
    float3 acc = 0, mn = 1e9, mx = 0;
    float ws = 0;
    [unroll] for (int y = -2; y < 2; ++y)
        [unroll] for (int x = -2; x < 2; ++x) {
            float4 h = tGIRaw.Load(int3(clamp(p + int2(x, y), 0, hmax), 0));
            float w = saturate(1 - abs(h.w - dc) / (dc * 0.03 + 0.1)) + 1e-4;
            acc += h.rgb * w;
            ws += w;
            if (w > 0.5 || (x == 0 && y == 0)) { mn = min(mn, h.rgb); mx = max(mx, h.rgb); }   // this surface's samples
        }
    float3 c = acc / ws;
    if (P6.w > 0.5 && dc < 5e4) {
        float dd = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x - i.t1.w;
        float2 puv; float pd;
        if (abs(dd) > 1e-9 && Reproject(HistPoint(i.t1.xyz / dd + G[L1.w].xyz, i.t0.xy), dd, puv, pd)) {   // (1.59)
            float4 h = tGIHist.SampleLevel(sPoint, puv, 0);
            // 1.65: the history held within this frame's samples of the surface (min .. max of the 4x4: the whole pattern,
            // so its noise passes): bounce light that changed (a car drove off, a lamp went out) no longer lingers
            if (abs(h.w - pd) < pd * 0.03 + 0.15) c = lerp(c, clamp(h.rgb, mn, mx), P7.y);
        }
    }
    return float4(c, dc);
}

// The finished lit scene at half resolution (before the haze), the light source of the next frame's bounce.
// Our own bounce / lamp light is taken out again, so it does not feed itself frame after frame.
struct SnapOut { float4 a : SV_Target0; float4 full : SV_Target1; };   // [1]: everything, the reflections' source
SnapOut PS_Snap(PSIn i)
{
    SnapOut so;
    float dd = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x - i.t1.w;
    float dist = abs(dd) < 1e-9 ? 6e4 : min(length(i.t1.xyz / dd), 6e4);
    float3 c = tDest.SampleLevel(sLin, i.t0.xy, 0).rgb;
    float3 cf = P24.z > 0.5 ? tEarly.SampleLevel(sLin, i.t0.xy, 0).rgb : c;   // (1.47: the reflections' source: no headlight halos)
    so.full = float4(cf == cf ? clamp(cf, 0, 6e4) : 0, dist);
    if (P7.w > 0.5 && dist < 400) {
        float4 D = tDiffuse.SampleLevel(sLin, i.t0.xy, 0);
        float3 sp = (L2.z & F_SPEC) ? tSpec.SampleLevel(sLin, i.t0.xy, 0).rgb : 0;
        c -= D.rgb * D.rgb * (1 - sp) * tGI.Load(int3(int2(i.pos.xy), 0)).rgb;
    }
    if (P10.y > 0.5) c -= tLampLight.SampleLevel(sLin, i.t0.xy, 0).rgb;   // this frame's lamp light, made once
    else if (P8.x > 0 && LampN.x > 0 && dist < P7.z && abs(dd) > 1e-9) {
        float4 D = tDiffuse.SampleLevel(sLin, i.t0.xy, 0);
        float3 sp = (L2.z & F_SPEC) ? tSpec.SampleLevel(sLin, i.t0.xy, 0).rgb : 0;
        float3 n = normalize(tNormal.SampleLevel(sPoint, i.t0.xy, 0).xyz * 2 - 1);
        float3 world = i.t1.xyz / dd + tCamPrev.Load(int3(3, 0, 0)).xyz;   // this frame's camera record
        c -= LampShade(world, n, 0, D.rgb * D.rgb, 1 - sp, 0, 1, false, i.t0.xy, false, P9.w > 0, i, world - tCamPrev.Load(int3(3, 0, 0)).xyz, dd, 0, 0, 0.5) * P8.x;
    }
    c = c == c ? clamp(c, 0, 6e4) : 0;
    so.a = float4(c, dist);
    return so;
}

// ---- reflections (screen space), half resolution: the reflected ray marched through the depth buffer (24 steps,
// denser near the point, then 4 halvings); a hit takes the scene there, a ray into the sky takes the sky. Glossy
// surfaces only; wet ground (facing up) gets glossier. Rough surfaces read a blurred level of the scene.
// 1.39: the panorama of our sky as seen: each texel's direction found on this frame's screen (camera record t28):
// where the screen shows sky there (depth: none), its colour (the half-size snapshot) eased in; else kept
float2 PanoUv(float3 d) { return float2(atan2(d.x, d.z) / 6.2831853 + 0.5, saturate((90 - degrees(asin(clamp(d.y, -1, 1)))) / 100)); }
// 1.41: kept premultiplied (rgb x a): what is not seen again fades (a: ~8 s half), the guess below takes its place
// 1.67 (ReflSkyLevel): the seen panorama stored divided by the sky's level now and read times it (it follows the time
// of day), or as seen
float PanoLevel() { return P26.w > 0.5 ? P23.z : 1; }
float4 PS_SkyPano(PSIn i) : SV_Target0
{
    float2 uv = i.pos.xy / float2(1024, 256);
    float az = (uv.x - 0.5) * 6.2831853, el = radians(90 - uv.y * 100);
    float3 d = float3(cos(el) * sin(az), sin(el), cos(el) * cos(az));
    float4 old = tSkyPano.Load(int3(int2(i.pos.xy), 0)) * P21.z;
    float4 A = tCamPrev.Load(int3(0, 0, 0)), B = tCamPrev.Load(int3(1, 0, 0)), C = tCamPrev.Load(int3(2, 0, 0));
    float3 c12 = cross(C.xyz, -d);
    float det = dot(B.xyz, c12);
    if (abs(det) < 1e-12) return old;
    float s = dot(B.xyz, cross(C.xyz, -A.xyz)) / det;
    float2 su = float2(dot(-A.xyz, c12), dot(B.xyz, cross(-A.xyz, -d))) / det;
    if (s >= 0 || any(su < 0.003) || any(su > 0.997)) return old;
    // the sky there? (1.39c: by the distance it gives: the ray rebuilt from the record is not bit-exact, so the passes'
    // own test, depth - w == 0 within 1e-9, never held here)
    float4 t1 = A + B * su.x + C * su.y;
    float z = tDepth.SampleLevel(sPoint, su, 0).x - t1.w;
    // the sky anywhere; what is around (1.40: buildings, not the bright sky behind them) beyond 25 m and over 2 deg up
    // (1.41: under that it took the far road and its puddles: lighter puddles in the puddles, moving with the camera)
    float far = abs(z) > 1e-12 ? length(t1.xyz / z) : 1e9;
    if (far < 15000 && (far < 25 || d.y < 0.035)) return old;
    float3 c = tRefl.SampleLevel(sLin, su, 0).rgb;
    c = c == c ? c : 0;
    return lerp(old, float4(c / max(PanoLevel(), 1e-3), 1), 0.35);   // (1.45: kept divided by the sky's level: it follows the time of day)
}
// 1.41: the sky guessed where the panorama has nothing of late: the haze's sky light (as AirLight: the sun's by its
// height, the city's glow at night), brighter at the horizon: it follows the time of day where the sky is not seen
float3 SkyGuess(float3 d)
{
    float lum = Lum(SunLc()), lumS = P19.w > 0.5 ? max(lum, P20.z) : lum;
    float3 sky = lerp(float3(1, 1, 1), float3(0.55, 0.72, 1.0), P5.y) * lumS * 0.12 * P14.z;
    float night = 1 - saturate((lumS - 0.5) / 2);
    float3 city = float3(1.0, 0.72, 0.45) * 0.035 * P14.y * night;
    return (sky + city) * lerp(1.15, 0.6, saturate(d.y));
}
// the sky for a reflected ray (d: world); rough surfaces: a blurred level. Where nothing was seen of late: 1.46 the sky
// probe (our sky all around, this second: the sky above the camera never looks at), else the seen ones' mean / a guess
float3 PanoSky(float3 d, float gloss)
{
    d.y = max(d.y, 0.02);
    d = normalize(d);
    float2 uv = PanoUv(d);
    float4 c = tSkyPano.SampleLevel(sLin, uv, (1 - gloss) * 6);
    float L = PanoLevel();
    int src = (int)(P26.y + 0.5);   // 1.67 (ReflSkySource): 0 auto, 1 the seen sky only, 2 the probe only (matched), 3 as drawn
    float4 seen = tSkyPano.SampleLevel(sLin, float2(0.5, 0.4), 9);
    float3 fill;
    if (P24.y > 0.5 && src != 1) {
        fill = tSkyProbe.SampleLevel(sLin, d, (1 - gloss) * 6).rgb;   // (1.52: 64^2: 7 levels)
        // 1.62 / 1.67: as the sky the screen shows (the probe is the sky's own draw, before our dusk / night darkening and
        // the haze): per colour channel, the seen panorama's mean over the probe's near the horizon; a black probe (night):
        // the seen sky's mean. ReflSkyMatch: how much.
        float3 pm = (tSkyProbe.SampleLevel(sLin, float3(1, 0.15, 0), 6).rgb + tSkyProbe.SampleLevel(sLin, float3(-1, 0.15, 0), 6).rgb
                   + tSkyProbe.SampleLevel(sLin, float3(0, 0.15, 1), 6).rgb + tSkyProbe.SampleLevel(sLin, float3(0, 0.15, -1), 6).rgb) * 0.25;
        // (1.67b: from 0.5 % of the panorama seen: at dawn / dusk little of it is (it was 5 %: the probe then went in as
        // drawn: bright orange clouds over a dark sky, the wet ground lit by them); never brighter than the seen sky)
        // (1.94: by how much of the panorama was seen of late (~6 s half): nothing seen, the probe as it is, not an old match)
        if (src != 3 && seen.a > 0.005) {
            float3 sAvg = seen.rgb / seen.a * L;
            float3 matched = Lum(pm) > 1e-4 ? fill * clamp(sAvg / max(pm, 1e-5), 0.02, 1.0) : sAvg;
            fill = lerp(fill, matched, saturate(P26.z) * saturate(seen.a * 20));
        }
    }
    else {
        float4 f = tSkyPano.SampleLevel(sLin, float2(uv.x, 0.4), 7);
        float3 guess = SkyGuess(d) * 0.5;   // (never seen: 1.41's guess was far brighter than the game's dark dawn sky: white)
        fill = f.a > 1e-3 ? lerp(guess, f.rgb / f.a * L, saturate(f.a * 4)) : guess;
    }
    // (1.94: the seen sky's own detail only while fresh (seen within ~3 s), else the probe: our sky of now all round)
    float3 r = src >= 2 ? fill : (c.a > 1e-3 ? lerp(fill, c.rgb / c.a * L, saturate((c.a - 0.25) * 2)) : fill);
    return r * P26.x;
}

// 1.70: the cars as boxes for the reflections (their bodies: 1.9 x 4.6 x 1.2 m over the road): a floor's ray that
// leaves the screen or reaches the sky but passes through one sees that car's dark side / underside, not the sky (a car
// seen from above had no image in the puddle around it: its underside is never on the screen). Distance along the ray,
// or 1e9.
float RayCars(float3 o, float3 d, out float w)
{
    // 1.72: the player's car only (traffic looked fine and its box stood out), and as its underside: a flat rectangle
    // 0.35 m over the floor the ray starts from (the car's own place may be its body's middle), not a box (a box mirrored
    // in a puddle showed as a dark cube). 1.76: its corners rounded (0.5 m), its edge soft: w = how much it covers
    w = 0;
    float3 Pp, ax, az;
    if (!PlayerPose(Pp, ax, az) || d.y <= 1e-3) return 1e9;
    float t = 0.35 / d.y;
    float3 l = o + d * t - Pp;
    float2 q = abs(float2(dot(l, ax), dot(l, az))) - float2(0.4, 1.7);
    float sd = length(max(q, 0)) + min(max(q.x, q.y), 0) - 0.5;
    w = 1 - smoothstep(-0.25, 0.3, sd);
    return w > 0 ? t : 1e9;
}

float4 SsrRay(PSIn i, inout float gl, inout float th)
{
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float3 gN = GeoN(i, depth);   // (derivatives: before any return)
    // 1.45: how fast the normal map turns from pixel to pixel (derivatives: here): bumpy wet stones scatter a sharp
    // reflection into shimmering dots; there it gets that much rougher (anti-shimmer), flat floors and puddles stay sharp
    float3 nRaw = normalize(tNormal.SampleLevel(sPoint, i.t0.xy, 0).xyz * 2 - 1);
    float nVar = (dot(ddx(nRaw), ddx(nRaw)) + dot(ddy(nRaw), ddy(nRaw))) * (1 - DepthStep(i, depth));   // (1.96h: not an edge's)
    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return float4(0, 0, 0, 6e4);
    float3 rel = i.t1.xyz / dd;
    float dist = length(rel);
    if (dist > P12.z) return float4(0, 0, 0, dist);
    float4 N = tNormal.SampleLevel(sPoint, i.t0.xy, 0);
    float3 n = normalize(N.xyz * 2 - 1);
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float puddle, covered, pGloss, pWater;
    float3 alb = tDiffuse.SampleLevel(sLin, i.t0.xy, 0).rgb;
    float wet = Wetness(rel, WetUp(n, gN), i.t1, X, Y, dd, i.pos.xy, alb, rel + tCamPrev.Load(int3(3, 0, 0)).xyz, puddle, covered, pGloss, pWater);
    // 1.70: nothing on a car is wet road (a tyre's top under its arch shone with the sky like a puddle); its paint has its own
    { float onCarW = OnCar(rel + tCamPrev.Load(int3(3, 0, 0)).xyz); wet *= 1 - onCarW; puddle *= 1 - onCarW; }
    float wet01 = saturate(max(wet, puddle)), wetVis = smoothstep(0, 0.8, wet01);   // (1.40: as wet as WetGround says; puddles on their own)
    // wet asphalt is rough (it reflects a blurred sky: no cloud shapes in it); puddles are mirrors
    float glossDry = lerp(N.w, min(N.w, 0.45), covered);
    float gloss = lerp(glossDry, lerp(P23.y, pGloss, puddle), wet01);   // (1.44: WetSharp, was 0.62; 1.47: the puddles' own)
    float paint = (L2.z & F_SPEC) ? PaintOf(tSpec.SampleLevel(sPoint, i.t0.xy, 0)) : 0;
    gloss = lerp(gloss, 0.93, saturate(paint));   // a clear coat: sharp reflections
    float dryOk = (L2.z & F_SPEC) ? saturate((Lum(tSpec.SampleLevel(sPoint, i.t0.xy, 0).rgb) - 0.08) * 15) : 0;   // (1.42: stricter: stones shone dry)
    // a dry floor's own gloss path goes as the floor gets wet (else both add up: puddle-shaped sky patches)
    float dryVis = SsrVis(glossDry, n.y, 0) * dryOk * (1 - wetVis);
    if (wetVis <= 0 && dryVis <= 0 && paint <= 0) return float4(0, 0, 0, dist);
    // water fills the bumps: a wet floor reflects as nearly flat, a puddle as flat (the asphalt's normal map would
    // scatter the sky into blotches)
    float flatK = wetVis * lerp(1 - P22.y, 1.0, saturate(puddle * pWater * 3));   // (1.48: standing water only: damp keeps the bumps)
    n = normalize(lerp(n, float3(0, 1, 0), flatK));
    // 1.83: rain on standing water (in the open): its rings
    if (P33.y > 0.01 && P31.w > 0 && puddle * pWater > 0.02) {
        float3 wp = rel + tCamPrev.Load(int3(3, 0, 0)).xyz;
        float top;
        if (!RfTop(wp.xz, top) || wp.y > top - 0.3) {
            float2 rs = RainRipple(wp.xz, P29.x, P33.y) * P31.w * saturate(puddle * pWater * 2);
            n = normalize(n + float3(rs.x, 0, rs.y));
        }
    }
    { float rough = 1 - gloss; gloss = 1 - sqrt(rough * rough + min(P22.x * 0.5 * nVar * (1 - flatK) * (1 - flatK), 0.8)); }
    float3 V = rel / dist;
    float3 Rv = reflect(V, n);
    float jit = P21.w > 0.5 ? Ign(i.pos.xy) : 0.5;   // (1.43: SsrJitter)
    float maxT = (P20.w > 0 ? P20.w : 40) + dist;   // (1.39: SsrRay: up walls and towers; it was 40 m past the point)
    float3 start = rel + n * (0.03 + dist * 0.003);
    float prevT = 0, tHit = maxT, tHide = 1e9;   // (1.52: tHide: where it first went behind something, unseen thickness)
    bool hit = false, lastSky = false;
    float2 uvr = -1;
    // (1.65: 7 groups of 4 steps, each group's depth read together, then looked at in order; the first hit refined)
    RayProj rp = ProjSetup(start, Rv, i.t1, X, Y, dd);
    bool done = false;
    float ha = 0, hb = 0;
    [loop] for (int g = 0; g < 7; ++g) {
        [unroll] for (int k = 0; k < 4; ++k) {
            float f = (g * 4 + k + jit) / 28.0 + 0.5 / 28.0;
            float t = maxT * f * f;
            float2 dp; float4 rayAt;
            bool ok = ProjAt(rp, t, i.t1, X, Y, dp, rayAt);
            float2 uv = (i.pos.xy + dp) / Size.xy;
            ok = ok && all(uv >= 0) && all(uv <= 1);
            float z = tDepth.SampleLevel(sPoint, uv, 0).x - rayAt.w;
            float diff = length(start + Rv * t) - length(rayAt.xyz / z);
            if (!done) {
                if (!ok) done = true;
                else {
                    uvr = uv;
                    if (abs(z) < 1e-9) { lastSky = true; prevT = t; }
                    else {
                        lastSky = false;
                        if (diff > 0 && diff < 0.3 + t * 0.08 + (t - prevT) * 1.2) { hit = true; ha = prevT; hb = t; done = true; }   // (1.40: + the step: long rays stepped through facades)
                        else { if (diff > 0) tHide = min(tHide, t); prevT = t; }
                    }
                }
            }
        }
        if (done) break;
    }
    if (hit) {
        float a = ha, b = hb;
        [unroll] for (int r = 0; r < 4; ++r) {
            float m = (a + b) * 0.5;
            float2 dpm; float4 ra;
            ProjAt(rp, m, i.t1, X, Y, dpm, ra);
            float2 uvm = (i.pos.xy + dpm) / Size.xy;
            float zm = tDepth.SampleLevel(sPoint, uvm, 0).x - ra.w;
            float dm = abs(zm) < 1e-9 ? -1 : length(start + Rv * m) - length(ra.xyz / zm);
            if (dm > 0) { b = m; uvr = uvm; } else a = m;
        }
        tHit = b;
    }
    // 1.39: off the screen or into the sky: our sky as seen (the panorama), not nothing / the screen's edge
    bool pano = P21.x > 0.5;
    float3 sky = pano ? PanoSky(Rv, gloss) : 0;
    // 1.52: a ray that went behind something within its first metres and found nothing is under it (a car's body over
    // the road, its underside not on the screen): dark there, not the sky (white halos under cars)
    // 1.59: only from floors (a car's lights, glass, body: their rays pass behind the car's own nearer parts, then leave
    // the screen: they went black)
    float3 wpos = rel + tCamPrev.Load(int3(3, 0, 0)).xyz;
    sky *= lerp(1, saturate(tHide / 5 - 0.2), saturate((gN.y - 0.6) * 4));   // (1.62: under cars again: the road there lay in their box)
    float wCar = 0, tCar = 1e9;   // (1.70: the player's car's underside on the way: dark; 1.76: soft, rounded)
    if (gN.y > 0.6 && OnCar(wpos) < 0.5) tCar = RayCars(wpos + n * 0.03, Rv, wCar);
    if (!hit && tCar < 25) sky *= lerp(1, 0.08, wCar);
    sky *= 1 - 0.9 * UnderPlayer(wpos) * saturate((gN.y - 0.6) * 4);   // (1.73)
    gl = gloss; th = hit ? tHit : 1000;   // (1.46: on their own target, for the smoothing and the history)
    if (!hit && (!lastSky || pano)) return float4(sky, dist);
    float2 e = min(uvr, 1 - uvr);
    float conf = saturate(min(e.x, e.y) * 12);                  // fade out near the screen edges (to the sky)
    conf *= saturate(tHit / 0.3 - 0.2);   // 1.60: not inside its own lamp housing (a tail light's LEDs: white / black dots)
    float mip = (1 - gloss) * 7 * saturate(tHit / 10 + 0.25);
    // sharp ones from the full-size frame, blurred ones from the half-size copy's levels
    float3 col = mip < 1 ? lerp(tScene.SampleLevel(sLin, uvr, 0).rgb, tRefl.SampleLevel(sLin, uvr, 0).rgb, mip)
                         : tRefl.SampleLevel(sLin, uvr, mip - 1).rgb;
    return float4(lerp(sky, col, conf) * (tCar < tHit ? lerp(1, 0.08, wCar) : 1), dist);   // (1.70b: a far bright hit past the car's underside: the car)
}
// 1.46: + the gloss and the ray's length, on a second target
struct SsrRayOut { float4 c : SV_Target0; float2 a : SV_Target1; };
SsrRayOut PS_SSR(PSIn i)
{
    SsrRayOut o;
    float gl = 0, th = 1000;
    o.c = SsrRay(i, gl, th);
    o.c.rgb = o.c.rgb == o.c.rgb ? min(o.c.rgb, 6e4) : 0;   // (1.51: the sharp source is not clamped: lamps' LEDs past FP16 -> inf -> NaN dots)
    o.a = float2(gl, th);
    return o;
}

// The reflections smoothed (3x3, 1-2-1, only from the same surface: similar distance; mirrors (gloss over 0.7) not
// averaged, only freed of lone bright pixels: fireflies), half resolution; then over frames.
// 1.46: over frames as a game's temporal anti-aliasing, for the reflections alone: the rays are jittered anew each frame
// and the last frames' result, found where this reflection was a frame ago, takes ~90%: steady and still sharp (not a
// blur of neighbours). Where it was: a mirror's picture moves with the reflected point (its mirror image: the view ray
// carried on past the surface by the ray's length), a rough one with the surface, a car moving with the camera with the
// camera; the old value used only from the same surface (its point on this one's plane) and held within this frame's
// neighbourhood (clipped toward its mean, within its spread x SsrClip: no ghosts, no trails).
// (1.42 .. 1.45: the gloss rode on w, x 1e5: past FP16's 65504: there was no history at all and no depth-aware weights)
float3 Med3(float3 a, float3 b, float3 c) { return max(min(a, b), min(max(a, b), c)); }
float3 HistCR(float2 uv)   // Catmull-Rom from 5 bilinear taps (the corners left out): resampled each frame, not blurred
{
    float2 sp = uv * Size.xy, t1 = floor(sp - 0.5) + 0.5, f = sp - t1;
    float2 w0 = f * (-0.5 + f * (1 - 0.5 * f)), w1 = 1 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2, t12 = (t1 + w2 / w12) / Size.xy, t0 = (t1 - 1) / Size.xy, t3 = (t1 + 2) / Size.xy;
    float3 r = tSsrHist.SampleLevel(sLin, float2(t12.x, t0.y), 0).rgb * (w12.x * w0.y)
             + tSsrHist.SampleLevel(sLin, float2(t0.x, t12.y), 0).rgb * (w0.x * w12.y)
             + tSsrHist.SampleLevel(sLin, t12, 0).rgb * (w12.x * w12.y)
             + tSsrHist.SampleLevel(sLin, float2(t3.x, t12.y), 0).rgb * (w3.x * w12.y)
             + tSsrHist.SampleLevel(sLin, float2(t12.x, t3.y), 0).rgb * (w12.x * w3.y);
    return max(r / (w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y), 0);
}
// toward the mean until inside the box mean +- ext
float3 ClipTo(float3 h, float3 mu, float3 ext)
{
    float3 d = h - mu, u = abs(d) / max(ext, 1e-5);
    float m = max(u.x, max(u.y, u.z));
    return m > 1 ? mu + d / m : h;
}
// the last frame's texel at uv: of this surface? Its point (its distance along the last frame's camera ray) on this
// surface's plane (pn; none: near the point); camSpace: both in their camera's space (a car moving with the camera)
bool HistFits(float2 uv, float sgn, float3 p, float3 pn, float tol, bool camSpace)
{
    if (any(uv <= 0) || any(uv >= 1)) return false;
    float hw = tSsrHist.Load(int3(clamp(int2(uv * Size.xy), 0, int2(Size.xy) - 1), 0)).w;
    if (hw > 5e4) return false;
    float4 A = tCamPrev.Load(int3(0, 0, 0)), B = tCamPrev.Load(int3(1, 0, 0)), C = tCamPrev.Load(int3(2, 0, 0));
    float3 q = normalize((A + B * uv.x + C * uv.y).xyz) * (sgn > 0 ? hw : -hw);
    if (!camSpace) q += tCamPrev.Load(int3(3, 0, 0)).xyz;
    return dot(pn, pn) > 0.25 ? abs(dot(q - p, pn)) < tol : length(q - p) < tol * 4;
}
float4 PS_SSRBlur(PSIn i) : SV_Target0
{
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float3 gN = GeoN(i, depth);   // (derivatives: before any branch)
    int2 p = int2(i.pos.xy);
    int2 hmax = int2(Size.xy) - 1;
    float4 c = tSsr.Load(int3(p, 0));
    float2 ax = tSsrAux.Load(int3(p, 0));   // gloss, the ray's length
    // (1.44: 3x3 around it, 1-2-1 weights, for the rough only: the 4x4 box, half a pixel off, made blocks)
    float cd = c.w, sharp = P22.w < 0.5 ? 1 : P22.w > 1.5 ? 0 : saturate((ax.x - 0.7) * 5);   // (1.43: SsrSmooth)
    float4 hs[9];
    float wt[9];
    float3 m1 = 0, m2 = 0;
    float nn = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            int k = (y + 1) * 3 + x + 1;
            hs[k] = tSsr.Load(int3(clamp(p + int2(x, y), 0, hmax), 0));
            float same = saturate(1 - abs(hs[k].w - cd) / (cd * 0.02 + 0.05));
            wt[k] = (same + 1e-4) * (x == 0 ? 2 : 1) * (y == 0 ? 2 : 1);
            if (same > 0.5) { m1 += hs[k].rgb; m2 += hs[k].rgb * hs[k].rgb; nn += 1; }
        }
    float3 mu = nn > 0 ? m1 / nn : c.rgb, sg = nn > 0 ? sqrt(max(m2 / nn - mu * mu, 0)) : 0;
    float3 hi = P22.z > 0 ? mu + P22.z * sg + 0.02 * mu : 1e9;   // (1.43: SsrClamp)
    float3 acc = 0;
    float ws = 0;
    [unroll] for (int k = 0; k < 9; ++k) { acc += min(hs[k].rgb, hi) * wt[k]; ws += wt[k]; }
    float3 blur = acc / ws, own = min(c.rgb, hi);
    // 1.60: a lone bright texel (a LED caught by one jittered ray) held to its neighbours' brightest x 2
    { float3 nb = 0; [unroll] for (int q = 0; q < 9; ++q) if (q != 4) nb = max(nb, hs[q].rgb); own = min(own, nb * 2 + 0.02); }
    // 1.70 (in place of 1.69's floor): a texel far from the median of its 3x3 (dark or bright: a ray that hit a wrong spot:
    // black dots on paint, lamp glass, puddles) takes that median (median of the rows' medians: edges kept)
    {
        float3 r0 = Med3(hs[0].rgb, hs[1].rgb, hs[2].rgb), r1 = Med3(hs[3].rgb, hs[4].rgb, hs[5].rgb), r2 = Med3(hs[6].rgb, hs[7].rgb, hs[8].rgb);
        float3 md = Med3(r0, r1, r2);
        float lo = Lum(own), lm = Lum(md);
        if (lo < lm * 0.5 || lo > lm * 2 + 0.05) own = md;
    }
    float3 r = lerp(blur, own, sharp);
    float dd = depth - i.t1.w;
    if (P12.w > 0.5 && cd < 5e4 && abs(dd) > 1e-9) {
        float3 rel = i.t1.xyz / dd, world = rel + tCamCur.Load(int3(3, 0, 0)).xyz;
        // 1.57: car paint: its plane from the G-buffer's smooth normal and a wider tolerance (the depth's normal is a flat
        // facet of the body: the history held on some facets and not on the next: a mosaic of squares), no mirror image
        bool pnt = (L2.z & F_SPEC) && PaintOf(tSpec.SampleLevel(sPoint, i.t0.xy, 0)) > 0.3;
        float3 pn = pnt ? normalize(tNormal.SampleLevel(sPoint, i.t0.xy, 0).xyz * 2 - 1) : gN;
        float tol = (0.05 + cd * 0.008) * (pnt ? 3 : 1);
        float2 uvS, uvV; float pdS, pdV;
        // 1.59: on the player's car its point where the car was (else the door's history from half a metre back at
        // speed: trails); no mirror-image candidate there
        bool player = OnPlayer(world);
        float3 wp = player ? HistPoint(world, i.t0.xy) : world;   // (1.60: not on its wheels / low non-paint parts)
        bool okS = Reproject(wp, dd, uvS, pdS) && HistFits(uvS, dd, wp, pn, tol, false);
        bool okV = !pnt && !player && Reproject(world + rel / max(cd, 1e-4) * ax.y, dd, uvV, pdV) && HistFits(uvV, dd, world, pn, tol, false);
        bool useV = okV && (ax.x > 0.45 || !okS);
        float2 uvH = useV ? uvV : uvS;
        bool ok = useV || okS;
        if (!ok && !player) { uvH = i.pos.xy / Size.xy; ok = HistFits(uvH, dd, rel, pn, tol, true); }
        if (ok) {
            float3 hc = HistCR(uvH);
            // 1.59: a moving car's paint: the world it mirrors runs over it: less of the last frames and held closer
            // to this frame's (no streaks of the old picture)
            float run = player ? saturate(length(world - wp) / 0.25) : 0;
            float3 h = ClipTo(hc == hc ? hc : mu, mu, sg * P24.x * (1 - 0.5 * run) + 0.01 * mu + 0.001);
            r = lerp(r, h, min(0.9 * P23.x, 0.97) * (1 - 0.45 * run));   // (1.43: SsrHistory)
        } else r = lerp(blur, own, sharp * 0.5);   // just uncovered: nothing to settle on: softer for these few frames
    }
    return float4(r == r ? min(r, 6e4) : 0, c.w);
}

// The reflections onto the frame (dual-source blend: result = c + dest * k): the game's Fresnel (its spec colour and
// factor), a water film's on wet ground, which also gets darker (wet materials absorb more).
struct SsrOut { float4 c : SV_Target0; float4 k : SV_Target1; };
SsrOut PS_SSRApply(PSIn i)
{
    SsrOut o;
    int dbgV = L2.w;
    bool dbg = dbgV >= 28 && dbgV <= 32;   // the reflection debug views: only what they show
    o.c = 0; o.k = dbg ? 0 : 1;
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float3 gN = GeoN(i, depth);   // (derivatives: before any return)
    float dd = depth - i.t1.w;
    if (abs(dd) < 1e-9) return o;
    float3 rel = i.t1.xyz / dd;
    float dist = length(rel);
    if (dist > P12.z) return o;
    float4 N = tNormal.SampleLevel(sPoint, i.t0.xy, 0);
    float3 n = normalize(N.xyz * 2 - 1);
    float4 Sp = (L2.z & F_SPEC) ? tSpec.SampleLevel(sPoint, i.t0.xy, 0) : 0;
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float puddle, covered, pGloss, pWater;
    float3 alb = tDiffuse.SampleLevel(sLin, i.t0.xy, 0).rgb;
    float wet = Wetness(rel, WetUp(n, gN), i.t1, X, Y, dd, i.pos.xy, alb, rel + tCamPrev.Load(int3(3, 0, 0)).xyz, puddle, covered, pGloss, pWater);
    // 1.70: nothing on a car is wet road (a tyre's top under its arch shone with the sky like a puddle); its paint has its own
    { float onCarW = OnCar(rel + tCamPrev.Load(int3(3, 0, 0)).xyz); wet *= 1 - onCarW; puddle *= 1 - onCarW; }
    float wet01 = saturate(max(wet, puddle)), wetVis = smoothstep(0, 0.8, wet01);   // (1.40: as wet as WetGround says; puddles on their own)
    // wet asphalt is rough (it reflects a blurred sky: no cloud shapes in it); puddles are mirrors
    float glossDry = lerp(N.w, min(N.w, 0.45), covered);
    float gloss = lerp(glossDry, lerp(P23.y, pGloss, puddle), wet01);   // (1.44: WetSharp, was 0.62; 1.47: the puddles' own)
    n = normalize(lerp(n, float3(0, 1, 0), wetVis * lerp(1 - P22.y, 1.0, saturate(puddle * pWater * 3))));   // as PS_SSR
    // 1.83: rain on standing water (in the open): its rings
    if (P33.y > 0.01 && P31.w > 0 && puddle * pWater > 0.02) {
        float3 wp = rel + tCamPrev.Load(int3(3, 0, 0)).xyz;
        float top;
        if (!RfTop(wp.xz, top) || wp.y > top - 0.3) {
            float2 rs = RainRipple(wp.xz, P29.x, P33.y) * P31.w * saturate(puddle * pWater * 2);
            n = normalize(n + float3(rs.x, 0, rs.y));
        }
    }
    float3 V = rel / dist;
    float ndv = saturate(dot(n, -V));
    // 1.96g: the 1-pixel white rims (a sign's frame, a bin, a car's vent edges): a pixel on the edge of a thing in front
    // of what lies behind it sees its surface edge-on, its normal unsure, and the grazing Fresnel made it a bright line
    // (no AA to soften it). Such a pixel: off its neighbours' plane (the distances' second difference near the first:
    // a step, not a slope; a wet road far off is a slope) with one neighbour well behind. It reflects as face on.
    float edge = 0;
    if (P25.w > 0.5) {
        float2 du = ddx(i.t0.xy), dv = ddy(i.t0.xy);
        float4 zs = float4(tDepth.SampleLevel(sPoint, i.t0.xy - du, 0).x, tDepth.SampleLevel(sPoint, i.t0.xy + du, 0).x,
                           tDepth.SampleLevel(sPoint, i.t0.xy - dv, 0).x, tDepth.SampleLevel(sPoint, i.t0.xy + dv, 0).x);
        float4 tw = float4(i.t1.w - X.w, i.t1.w + X.w, i.t1.w - Y.w, i.t1.w + Y.w);
        float4 dn;
        dn.x = length((i.t1.xyz - X.xyz) / max(abs(zs.x - tw.x), 1e-9));
        dn.y = length((i.t1.xyz + X.xyz) / max(abs(zs.y - tw.y), 1e-9));
        dn.z = length((i.t1.xyz - Y.xyz) / max(abs(zs.z - tw.z), 1e-9));
        dn.w = length((i.t1.xyz + Y.xyz) / max(abs(zs.w - tw.w), 1e-9));
        dn = min(dn, 1e5);
        float tolE = dist * 0.03 + 0.05;
        float2 c1 = abs(dn.xz - dn.yw), c2 = abs(dn.xz + dn.yw - 2 * dist);
        float2 step2 = saturate((c2 / max(c1, 1e-4) - 0.35) * 4);                     // a step (1), a slope (0)
        float2 behind = saturate((max(dn.xz, dn.yw) - dist) / tolE - 1);              // a neighbour well behind
        edge = max(step2.x * behind.x, step2.y * behind.y);
    }
    float f5 = pow(1 - ndv, 5) * (1 - edge);
    float3 F = Sp.rgb + (1 - Sp.rgb) * Sp.w * f5;
    // (1.42: at a glancing angle up to its gloss, not 1: rough wet asphalt glared; 1.47: puddles water's, 1, blurred or not:
    // the big matte ones reflect as much, only softer)
    F = lerp(F, 0.02 + lerp(max(gloss, 0.02) - 0.02, 0.98, puddle * pWater) * f5, wet01);   // (1.48: standing water's only)
    // car paint: a clear coat over the paint (Fresnel from 4%: faint face on, strong at a glancing angle)
    float paint = PaintOf(Sp);
    float f5p = pow(1 - max(ndv, 0.12), 5) * (1 - edge);   // 1.60: the silhouette's last pixels (the normal there grazes): no white rim
    F = lerp(F, (0.04 + 0.96 * f5p) * min(paint, 1.5), saturate(paint));
    gloss = lerp(gloss, 0.93, saturate(paint));
    // the reflections' target may be half, the screen's or full size: its 4 nearest texels, depth-aware
    float2 hp = i.pos.xy * P13.xy - 0.5;
    int2 b0 = int2(floor(hp));
    float2 fq = hp - b0;
    int2 hmax = int2(P13.zw) - 1;
    float4 r = 0;
    float rw = 0, best = 1e9;
    // 1.71: a 4x4 window (the 4 nearest by place, the ring around them a little), each by how well its distance fits this
    // pixel's (sharp: another surface counts for almost nothing): on a thin thing (a pole, a window frame) none of the 4
    // nearest may be its own: they took the background's reflection (a 1-pixel outline); the ring may have its own
    [unroll] for (int k = 0; k < 16; ++k) {
        int2 o2 = int2(k & 3, k >> 2) - 1;
        float4 h = tSsr.Load(int3(clamp(b0 + o2, 0, hmax), 0));
        bool inner = all(o2 >= 0) && all(o2 <= 1);
        float wb = inner ? (o2.x ? fq.x : 1 - fq.x) * (o2.y ? fq.y : 1 - fq.y) + 0.05 : 0.02;
        float df = abs(h.w - dist) / (dist * 0.02 + 0.05);
        float wd = exp2(-df * 3);
        best = min(best, df);
        r += h * (wb * wd + 1e-7);
        rw += wb * wd + 1e-7;
    }
    r /= rw;
    float match = saturate(2 - best);   // 1.60: none of its texels is this surface (an edge: a white outline round cars)
    // the game gives nearly everything a high gloss: dry surfaces reflect only with a real specular colour
    // (paint, metal, glass), not concrete or brick (no clouds in a dry wall)
    float dryOk = saturate((Lum(Sp.rgb) - 0.08) * 15);   // (as PS_SSR)
    // wet floors: as wet as they are (the rain map's gradient, no gloss threshold); dry: by gloss and a real specular
    float dryVis = SsrVis(glossDry, n.y, 0) * dryOk * (1 - wetVis);   // as PS_SSR
    // 1.62: a car's parts that are not paint: bumpy ones (a lamp's reflector, grilles) reflect nothing sharp (each pixel's
    // ray went elsewhere: white / black dots), coloured ones (lamp glass) tint what they reflect
    if (paint < 0.3 && OnCar(rel + tCamPrev.Load(int3(3, 0, 0)).xyz)) {
        float3 nR = normalize(N.xyz * 2 - 1);
        float nV = dot(ddx(nR), ddx(nR)) + dot(ddy(nR), ddy(nR));
        dryVis *= saturate(1 - nV * 25);
        float mx = max(alb.r, max(alb.g, alb.b)), mn = min(alb.r, min(alb.g, alb.b));
        float sat = mx > 0.02 ? (mx - mn) / mx : 0;
        F *= lerp(1, alb / max(mx, 1e-3), saturate(sat * 2 - 0.4));
    }
    float vis = max(max(wetVis, dryVis), saturate(paint)) * saturate(1 - dist / P12.z * 1.2 + 0.2) * match;
    o.c = float4(r.rgb * F * vis * P12.x, 0);
    o.k = float4((1 - 0.35 * max(wet, puddle * (1 - pWater)) - 0.1 * puddle).xxx, 1);   // wet surfaces are darker (puddles a little more; 1.48: damp patches as wet ground)
    if (dbg) {
        o.k = 0;
        if (dbgV == 28) o.c = float4(r.rgb * F * vis * P12.x * 2, 0);                              // what is added (x2)
        else if (dbgV == 29) o.c = float4(float3(wetVis, dryVis, saturate(paint)) * vis, 0);   // by source
        else if (dbgV == 30) o.c = float4(r.rgb, 0);                                              // what the ray found
        else if (dbgV == 31) o.c = float4(wet01.xxx * 0.8 + float3(0, 0, 1) * puddle, 0);         // wetness, puddles
        else o.c = float4(n * 0.5 + 0.5, 0);                                                      // the normal used
        o.c.rgb *= DbgScale();
    }
    return o;
}

// The lamps' light scattered by the air towards the camera along a view ray (0..T, homogeneous air): per lamp the
// closed form of the integral of 1 / d^2 along the ray, (atan((T - tc) / h) - atan(-tc / h)) / h, h = the lamp's
// distance to the ray, tc = where along the ray it is nearest. A soft halo around every lamp, also against the sky.
float3 LampGlow(float3 Vd, float T, float2 uv, float3 camPos)
{
    float3 sum = 0;
    float day = saturate((Lum(G[L0.x].xyz) - 0.5) / 2);
    float mapScale = lerp(1, P8.w, day);
    int2 tile = clamp(int2(uv * float2(LampN.yz)), 0, LampN.yz - 1);
    uint4 ta = tTileA.Load(int3(tile, 0)), tb = tTileB.Load(int3(tile, 0));
    [loop] for (int w = 0; w < 8; ++w) {
      uint4 tv = w < 4 ? ta : tb;   // (1.96: picked by selects, not an array: scratch memory on AMD)
      int wc = w & 3;
      uint m = wc == 0 ? tv.x : wc == 1 ? tv.y : wc == 2 ? tv.z : tv.w;
      [loop] while (m != 0) {
        uint b = m & (~m + 1);
        m ^= b;
        int k = w * 32 + (int)(asuint((float)b) >> 23) - 127;
        float4 LP = LampP(k), LC = LampC(k);
        if (LampX(k).z > 0.5) continue;   // a car's light: no glow in the air
        float3 Lr = LP.xyz - camPos;
        float tc = dot(Lr, Vd);
        float h = sqrt(max(dot(Lr, Lr) - tc * tc, 0.09));
        float I = (atan((T - tc) / h) - atan(-tc / h)) / h;
        float f = saturate(1 - h * h / (P8.z * P8.z));   // within the lamp's light radius (as its light on surfaces)
        sum += LC.rgb * I * f * f * (((uint)LP.w & 1) ? mapScale : 1);
      }
    }
    return sum * P14.x * 0.00025;
}

// ---- the player's headlights (the game's projected spot light, its constants copied: headlight.inc)
cbuffer HeadlightCB : register(b8)
{
    float4 HlAtt;    // attenuation: 1 / (x d^2 + y d + z)
    float4 HlCol;    // colour
    float4 HlPos;    // position, camera-relative
    float4 HlM[4];   // projection rows: clip = P.x M0 + P.y M1 + P.z M2 + M3
};
Texture2D<float4> tHlTex : register(t43);       // the beam pattern
Texture2D<float> tHlShadow : register(t44);     // the headlights' shadow map
SamplerComparisonState sHlCmp : register(s14);

// The beams in the air along a view ray (camera-relative direction Vd, 0..T m): 20 jittered samples inside the
// spot's frustum, each lit by the pattern, the game's attenuation and its shadow map (a car ahead cuts the beam).
float3 HeadlightBeam(float3 Vd, float T, float noise)
{
    // 1.65: the ray clipped to the spot's frustum first (in clip space every bound is linear along the ray): the samples
    // all fall inside the beam (less noise for the same count) and a ray that misses it costs nothing
    float4 c1 = Vd.x * HlM[0] + Vd.y * HlM[1] + Vd.z * HlM[2], c0 = HlM[3];   // clip(t) = c0 + c1 t
    float ta = 0, tb = T;
    float a[7] = { c0.w, c0.w - c0.x, c0.w + c0.x, c0.w - c0.y, c0.w + c0.y, c0.w - c0.z, c0.w + c0.z };
    float b[7] = { c1.w, c1.w - c1.x, c1.w + c1.x, c1.w - c1.y, c1.w + c1.y, c1.w - c1.z, c1.w + c1.z };
    [unroll] for (int k = 0; k < 7; ++k) {   // a + b t > 0
        if (abs(b[k]) < 1e-9) { if (a[k] <= 0) tb = -1; }
        else if (b[k] > 0) ta = max(ta, -a[k] / b[k]);
        else tb = min(tb, -a[k] / b[k]);
    }
    if (tb <= ta) return 0;
    float3 sum = 0;
    [loop] for (int s = 0; s < 20; ++s) {
        float t = ta + (s + noise) / 20 * (tb - ta);
        float3 P = Vd * t;
        float4 c = c0 + c1 * t;
        if (c.w <= 0 || any(abs(c.xyz) >= c.w)) continue;
        float3 q = c.xyz / c.w;
        float2 uv = q.xy * 0.5 + 0.5;
        float3 pat = tHlTex.SampleLevel(sLin, uv, 0).rgb;
        float sh = P15.y > 0.5 ? tHlShadow.SampleCmpLevelZero(sHlCmp, float2(uv.x, 1 - uv.y), q.z) : 1;
        float3 d = HlPos.xyz - P;
        float d2 = dot(d, d);
        float att = saturate(1 / (HlAtt.x * d2 + HlAtt.y * sqrt(d2 + 0.001) + HlAtt.z));
        att /= 1 + d2 / (P15.z * P15.z);   // in the air the beam spreads and fades with distance (the game's is almost flat)
        sum += pat * sh * att;
    }
    return sum * HlCol.rgb * ((tb - ta) / 20) * P15.x * 0.0005;
}

// ---- the lamps' final buffer (lamp-fix pass: one texel per lamp entry, into the buffer t33 is read from): the CPU's
// entries as they are, except the traffic cars' lights (x.w = 1 + which light): put on their car as drawn THIS frame
// (the matrix nearest to where the CPU expects the car, within 4 m), dark if that car is not drawn.
Buffer<float4> tLampsRaw : register(t45);
Buffer<float4> tCarMats : register(t46);   // per car draw: rows x, y, z axes, position
float4 PS_LampFix(PSIn i) : SV_Target0
{
    uint t = (uint)i.pos.x, k = t >> 2, c = t & 3;
    float4 raw = tLampsRaw.Load(t);
    float4 X = tLampsRaw.Load(k * 4 + 2);
    if (X.w < 0.5) return raw;
    float4 want = tLampsRaw.Load(k * 4 + 3);   // xyz: where the car should be, w: its heading
    int n = LampN.w, bi = -1;
    float best = 3.5 * 3.5;
    [loop] for (int j = 0; j < n; ++j) {
        float3 T = tCarMats.Load(j * 4 + 3).xyz - want.xyz;
        float d = dot(T, T);
        float3 z = tCarMats.Load(j * 4 + 2).xyz;
        if (d < best && dot(normalize(z.xz), float2(sin(want.w), cos(want.w))) > 0.4) { best = d; bi = j; }
    }
    // not drawn this frame (out of view): where the CPU expects it, upright on its heading
    float sh, ch;
    sincos(want.w, sh, ch);
    float3 ax = float3(ch, 0, -sh), ay = float3(0, 1, 0), az = float3(sh, 0, ch), T = want.xyz;
    if (bi >= 0) { ax = tCarMats.Load(bi * 4).xyz; ay = tCarMats.Load(bi * 4 + 1).xyz; az = tCarMats.Load(bi * 4 + 2).xyz; T = tCarMats.Load(bi * 4 + 3).xyz; }
    int li = (int)X.w - 1;
    bool head = li < 2;
    float3 lo = tLampsRaw.Load(k * 4).xyz;   // the light on the car (local), from its bounds
    if (c == 0) return float4(T + ax * lo.x + ay * lo.y + az * lo.z, raw.w);
    if (c == 3) return float4(normalize(az * (head ? 1 : -1) - ay * (head ? 0.12 : 0.05)), X.z - 1);   // the cone from the upload
    return raw;
}

// Lamp light alone, at the shown resolution (the game may render wider: SSAA): the sun pass and the snapshot read it.
// The camera from this frame's record (t28): G[L1.w] read by a dynamic index is what d3dcompiler_47 gets wrong.
struct LampsOut { float4 l : SV_Target0; float4 g : SV_Target1; float4 b : SV_Target2; };   // light / glow / strongest lamp
LampsOut PS_Lamps(PSIn i)
{
    LampsOut lo;
    lo.l = 0; lo.g = 0; lo.b = 0;
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float dd = depth - i.t1.w;
    if (P8.x <= 0 || LampN.x <= 0) return lo;
    bool sky = abs(dd) < 1e-9;
    float3 camPos = tCamPrev.Load(int3(3, 0, 0)).xyz;
    // the glow in the air along the view ray (to the surface, or 300 m into the sky)
    float sg = sky ? -1 : sign(dd);   // forward = t1 x the sign of the ray scale (as PS_Tiles)
    float3 Vd = normalize(i.t1.xyz) * sg;
    float dist = sky ? 6e4 : length(i.t1.xyz / dd);
    float3 glow = P14.x > 0 ? LampGlow(Vd, min(dist, 300), i.t0.xy, camPos) * P8.x : 0;
    if (P15.x > 0) glow += HeadlightBeam(Vd, min(dist, 70), Ign(i.pos.xy));
    glow = glow == glow ? glow : 0;
    lo.g = float4(glow, dist);
    lo.l = float4(0, 0, 0, dist);
    if (sky || dist >= P7.z) return lo;
    float3 rel = i.t1.xyz / dd;
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float3 world = rel + camPos;
    float4 N = tNormal.SampleLevel(sPoint, i.t0.xy, 0);
    float4 D = tDiffuse.SampleLevel(sPoint, i.t0.xy, 0);
    bool spec = (L2.z & F_SPEC) != 0;
    float4 Sp = spec ? tSpec.SampleLevel(sPoint, i.t0.xy, 0) : 0;
    float3 n = normalize(N.xyz * 2 - 1);
    float power = SpecAA(exp2(N.w * -11.0 + 11.0001), n, DepthStep(i, depth));
    float3 V = normalize(i.t1.xyz);
    float3 c = LampShade(world, n, V, D.rgb * D.rgb, 1 - Sp.rgb, Sp, power, spec, i.t0.xy, P9.x > 0 && dist < 120, P9.w > 0,
                         i, rel, dd, X, Y, Ign(i.pos.xy));
    // the strongest lamp's highlight is drawn by the sun pass at full size (sharp on glossy paint)
    if (gBestK >= 0 && spec) {
        c -= gBestSpec * gBestZf * gBestVis;
        lo.b = float4(gBestK + 1, gBestVis * gBestZf, 0, dist);
    }
    c *= P8.x;
    lo.l = float4(c == c ? c : 0, dist);
    return lo;
}

// Lamp finder: one pixel per 32x32 tile of the snapshot (t29, this frame's): the bright, self-lit spots
// (far above the frame's average) become one light at their centre, intensity = radiance x their area.
// This frame's camera record is at t28.
struct EmitOut { float4 p : SV_Target0; float4 c : SV_Target1; };
EmitOut PS_Emit(PSIn i)
{
    EmitOut o;
    o.p = 0;
    o.c = 0;
    uint w, h;
    tRadPrev.GetDimensions(w, h);
    float4 A = tCamPrev.Load(int3(0, 0, 0)), B = tCamPrev.Load(int3(1, 0, 0)), C = tCamPrev.Load(int3(2, 0, 0));
    float3 cam = tCamPrev.Load(int3(3, 0, 0)).xyz;
    float avg = Lum(tRadPrev.SampleLevel(sLin, float2(0.5, 0.5), 16).rgb);
    float thr = max(avg * P8.y, 1e-5);
    float4 mid = A + 0.5 * B + 0.5 * C;
    float pixAng = length(B.xyz) / (w * length(mid.xyz));       // angle of one snapshot pixel
    int2 base = int2(i.pos.xy) * 32;
    float3 ps = 0, I = 0;
    float ws = 0, mx = 0, ny = 0, nn = 0;
    [loop] for (int y = 0; y < 16; ++y)
        [loop] for (int x = 0; x < 16; ++x) {
            int2 q = base + int2(x, y) * 2;
            if (q.x >= (int)w || q.y >= (int)h) continue;
            float4 c = tRadPrev.Load(int3(q, 0));
            float l = Lum(c.rgb);
            if (l <= thr || c.w > 300) continue;
            float2 uv = (q + 0.5) / float2(w, h);
            float4 t1 = A + B * uv.x + C * uv.y;
            float dd = tDepth.SampleLevel(sPoint, uv, 0).x - t1.w;
            if (abs(dd) < 1e-9) continue;
            float3 rel = t1.xyz / dd;
            float d = length(rel);
            float3 e = c.rgb * (1 - thr / l) * (d * d * pixAng * pixAng * 4);   // 2x2 pixels per sample
            float le = Lum(e);
            I += e;
            ps += (rel + cam) * le;
            ws += le;
            ny += tNormal.SampleLevel(sPoint, uv, 0).y * 2 - 1;   // up-facing: a glint on the road, not a lamp face
            nn += 1;
            mx = max(mx, l);
        }
    if (ws > 0) {
        o.p = float4(ps / ws, 2 + ny / max(nn, 1));   // w: 2 + the bright pixels' mean normal y (valid if > 0.5)
        o.c = float4(I, mx / thr);
    }
    return o;
}

// ---- lamp editor (view 24): drawn after the snapshot, so the lamp finder never sees it.
// Lamps as dots (green = map, yellow = found), rings around the targets of Shift+J / Shift+K, the
// selected lamp's three arrows (x red, y green, z blue), the mouse cursor and the screen centre.
// Blended: result = dest * alpha + rgb (the rest of the picture dimmed).
float SegDist(float2 p, float2 a, float2 b)
{
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
    return length(p - a - ab * t);
}

// screen offset (in shown pixels, x units) of a point relative to the camera, from this pixel
bool ScreenOffset(PSIn i, float4 X, float4 Y, float sgn, float3 lr, out float2 dp)
{
    float4 ra;
    bool ok = Project(lr, i.t1, X, Y, sgn, dp, ra);
    dp.y *= LampScr.x;
    return ok;
}

float4 PS_Editor(PSIn i) : SV_Target0
{
    float4 X = ddx(i.t1), Y = ddy(i.t1);
    float dd = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x - i.t1.w;
    bool surf = abs(dd) > 1e-9;
    float sgn = surf ? sign(dd) : -1;
    float dist = surf ? length(i.t1.xyz / dd) : 1e9;
    // The camera position comes from this frame's camera record (t28), not from G[L1.w]: d3dcompiler_47
    // read that index here before loading it (the loop below then used camera = 0, all dots off to the side).
    float3 cam = tCamPrev.Load(int3(3, 0, 0)).xyz;
    float px = Size.x / 2560;                                  // sizes below in pixels of a 2560 wide picture
    float3 col = 0;
    bool hit = false;
    float2 dp;
    int cnt = min(LampN.x, 256);
    [loop] for (int k = 0; k < cnt; ++k) {
        float4 LP = LampP(k), LC = LampC(k);
        float3 lr = LP.xyz - cam;
        float t = length(lr);
        if (!ScreenOffset(i, X, Y, sgn, lr, dp)) continue;
        float rd = clamp(400 / t, 4, 12) * px, e = length(dp);
        float shade = t > dist + 0.5 ? 0.35 : 1;          // behind a surface: dimmer
        float3 lc = LC.rgb / max(max(LC.r, LC.g), max(LC.b, 1e-6));   // its own colour
        bool map = ((uint)LP.w & 1) != 0;
        if (e < rd && (map || e > rd * 0.5)) { col = (map ? lc * 0.9 : float3(0.9, 0.7, 0.1)) * shade; hit = true; }   // map: filled, found: a ring
        else if (ZoneN.y >= 0 && (((uint)LP.w >> (1 + ZoneN.y)) & 1) && e < rd + 3 * px) { col = float3(1, 1, 1); hit = true; }   // cut by the selected zone
        else if (e < rd + 1.5 * px) { col = 0; hit = true; }   // dark edge, readable on bright ground
    }
    [unroll] for (int g = 0; g < 2; ++g) {
        if (LampTgt[g].w < 0.5) continue;
        float3 lr = LampTgt[g].xyz - cam;
        if (!ScreenOffset(i, X, Y, sgn, lr, dp)) continue;
        float r = clamp(400 / length(lr), 4, 12) * px, e = length(dp);
        if (e > r * 1.5 && e < r * 2.1) { col = g == 0 ? float3(1, 0.1, 0.1) : float3(0.9, 0.9, 0.9); hit = true; }
    }
    if (LampSel.w > 0) {
        float3 sr = LampSel.xyz - cam;
        float2 p0;
        if (ScreenOffset(i, X, Y, sgn, sr, p0)) {
            [unroll] for (int a = 0; a < 3; ++a) {
                float3 e = a == 0 ? float3(1, 0, 0) : a == 1 ? float3(0, 1, 0) : float3(0, 0, 1);
                if (!ScreenOffset(i, X, Y, sgn, sr + e * LampSel.w, dp)) continue;
                bool on = LampUI.z == a + 1 || (LampUI.z == 0 && LampUI.w == a + 1);
                float w = (on ? 2.5 : 1.5) * px;
                if (SegDist(0, p0, dp) < w || length(dp) < w * 2.5) {
                    col = (a == 0 ? float3(0.8, 0.1, 0.1) : a == 1 ? float3(0.1, 0.7, 0.1) : float3(0.15, 0.3, 0.9)) * (on ? 1.4 : 1);
                    hit = true;
                }
            }
            if (length(p0) < 4 * px) { col = float3(0.9, 0.9, 0.9); hit = true; }
            // the game object it is tied to: a cyan line from the lamp to it
            float2 po;
            if (LampObj.w > 0.5 && ScreenOffset(i, X, Y, sgn, LampObj.xyz - cam, po) && SegDist(0, p0, po) < 1.2 * px) {
                col = float3(0.1, 0.8, 0.9); hit = true;
            }
        }
    }
    // the picked / connected game object: a cyan square at its position and a cyan post 5 m up from it
    {
        float2 po, pt;
        if (LampObj.w > 0.5 && ScreenOffset(i, X, Y, sgn, LampObj.xyz - cam, po) &&
            ScreenOffset(i, X, Y, sgn, LampObj.xyz + float3(0, 5, 0) - cam, pt)) {
            float2 q = abs(po);
            if ((max(q.x, q.y) < 7 * px && max(q.x, q.y) > 4.5 * px) || SegDist(0, po, pt) < 1.5 * px) {
                col = LampObj.w < 1.5 ? float3(0.1, 0.9, 1.0) : LampObj.w < 2.5 ? float3(1.0, 0.45, 0.05) : float3(0.5, 0.5, 0.5);
                hit = true;
            }
        }
    }
    // light zones: their edges (the selected one brighter) and a square at the centre
    {
        int zn = min(ZoneN.x, 16);
        [loop] for (int z = 0; z < zn; ++z) {
            float sn, cs;
            sincos(ZoneC[z].w, sn, cs);
            float3 ax = float3(cs, 0, -sn) * ZoneH[z].x, ay = float3(0, ZoneH[z].y, 0), az = float3(sn, 0, cs) * ZoneH[z].z;
            float3 c0 = ZoneC[z].xyz - cam;
            float2 q[8];
            bool ok = true;
            [unroll] for (int k = 0; k < 8; ++k) {
                float3 v = c0 + ax * ((k & 1) ? 1 : -1) + ay * ((k & 2) ? 1 : -1) + az * ((k & 4) ? 1 : -1);
                ok = ScreenOffset(i, X, Y, sgn, v, q[k]) && ok;
            }
            float2 pc;
            bool sel = z == ZoneN.y;
            if (ScreenOffset(i, X, Y, sgn, c0, pc)) {
                float2 m = abs(pc);
                if (max(m.x, m.y) < 6 * px && max(m.x, m.y) > 3.5 * px) { col = sel ? float3(1, 0.85, 0.1) : float3(0.3, 0.5, 1); hit = true; }
            }
            if (!ok) continue;
            float e = 1e9;
            [unroll] for (int a = 0; a < 8; ++a) {
                if (!(a & 1)) e = min(e, SegDist(0, q[a], q[a | 1]));
                if (!(a & 2)) e = min(e, SegDist(0, q[a], q[a | 2]));
                if (!(a & 4)) e = min(e, SegDist(0, q[a], q[a | 4]));
            }
            if (e < (sel ? 1.6 : 1.0) * px) { col = sel ? float3(1, 0.85, 0.1) : float3(0.25, 0.45, 0.9); hit = true; }
            if (!sel) continue;
            // the selected zone's handles: squares on its sides (stretch) and a ring around it (turn)
            int act = LampUI.z > 0 ? (int)LampUI.z : (int)LampUI.w;
            float3 axs[3] = { ax, ay, az };
            [unroll] for (int c = 4; c <= 9; ++c) {
                float3 fv = c0 + axs[(c - 4) / 2] * (((c - 4) & 1) ? 1 : -1);
                float2 fq;
                if (!ScreenOffset(i, X, Y, sgn, fv, fq)) continue;
                float2 m = abs(fq);
                if (max(m.x, m.y) < (act == c ? 7 : 5) * px) { col = act == c ? float3(1, 1, 1) : float3(0.9, 0.6, 0.1); hit = true; }
            }
            float R = length(c0) * 0.12 * 1.5 + 0.075;
            float2 pr;
            bool okp = false;
            [loop] for (int k2 = 0; k2 <= 48; ++k2) {
                float a2 = k2 * 6.2831853 / 48;
                float2 qq;
                bool okq = ScreenOffset(i, X, Y, sgn, c0 + float3(sin(a2), 0, cos(a2)) * R, qq);
                if (okq && okp && SegDist(0, pr, qq) < (act == 10 ? 2.2 : 1.3) * px) { col = act == 10 ? float3(1, 1, 1) : float3(0.8, 0.3, 0.9); hit = true; }
                okp = okq; pr = qq;
            }
        }
    }
    // the pointer: the screen centre, or where the mouse has dragged it while an arrow is held
    float2 c = abs(i.pos.xy - LampUI.xy * Size.xy) * float2(1, LampScr.x);
    if (!hit && ((c.x < 1 * px && c.y < 12 * px) || (c.y < 1 * px && c.x < 12 * px))) { col = float3(1, 0.15, 0.1); hit = true; }
    return hit ? float4(col, 0) : float4(0, 0, 0, 0.7);
}

// ---- the air, drawn after the transparents (car glass, particles), right before post-processing.
// Replays the sun pass's own vertex shader and draw, so the view ray is the same.
// Blended: result = dest * alpha + rgb (no copy of the frame).
float4 PS_Air(PSIn i) : SV_Target0
{
    if (L2.w == 1 && i.t0.x < 0.5) return float4(0, 0, 0, 1);   // split screen: left half untouched
    float depth = tDepth.SampleLevel(sPoint, i.t0.xy, 0).x;
    float dd = depth - i.t1.w;
    float3 skyGlow = P10.y > 0.5 ? tGlow.SampleLevel(sPoint, i.t0.xy, 0).rgb : 0;
    if (abs(dd) < 1e-9) return float4(skyGlow, 1);
    float3 rel = i.t1.xyz / dd;
    float dist = length(rel);
    if (dist > 60000.0) return float4(skyGlow, 1);              // the sky
    float shIn = -1;
    bool near = dist <= 3000.0;
    if (near && P5.w > 0.5) shIn = HalfFetch(tHalf, i.pos.xy, dist).y;
    float hT, sh;
    float3 camPos = 0;
    [loop] for (int cq = 0; cq < 1 + (int)(P2.w * 0); ++cq) camPos = G[L1.w].xyz;
    float3 insc = AirLight(rel, dist, Ign(i.pos.xy), near && (L2.z & F_SHADOW), near ? shIn : -1, camPos, hT, sh);
    if (L2.w == 19) return float4((1 - hT).xxx * DbgScale(), 0);
    if (L2.w == 20) return float4(sh.xxx * DbgScale(), 0);
    float3 glow = P10.y > 0.5 ? GlowAt(i.t0.xy, dist) : 0;
    return float4(insc * (1 - hT) + glow, hT);
}

// ---- on-screen text (drawn in Present)
cbuffer OverlayCB : register(b13) { float4 Rect; }  // NDC left, top, width, height
Texture2D<float4> tText : register(t0);
SamplerState sText : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VS_Overlay(uint id : SV_VertexID)
{
    VSOut o;
    float2 c = float2(id & 1, id >> 1);
    o.pos = float4(Rect.x + c.x * Rect.z, Rect.y - c.y * Rect.w, 0, 1);
    o.uv = c;
    return o;
}

// ---- the look: colour grading of the finished picture (grade.inc), drawn over it with the copy at t0 (with mips)
cbuffer GradeCB : register(b12)
{
    float4 Gr0;   // exposure (EV), contrast, saturation, vibrance
    float4 Gr1;   // shadows, highlights, temperature (warm +), tint (magenta +)
    float4 Gr2;   // clarity (local contrast), dehaze, sharpen, vignette
    float4 Gr3;   // split toning, mip of the local average, texel size
    float4 Gr4;   // x = split screen (left half untouched), y = frame (grain), z = bloom, w = grain
    float4 Gr5;   // film curve, red saturation, highlight desaturation, -
};

float3 ToLin(float3 c) { return pow(max(c, 0), 2.2); }
float3 ToDisp(float3 c) { return pow(saturate(c), 1 / 2.2); }

// 1.68 (AntiAliasing): FXAA 3.11 (Lottes, NVIDIA), quality preset with 8 search steps, on the game's finished picture
// (display-encoded, as FXAA wants): the edge's direction from the 3x3 lumas, its ends searched along it, the pixel moved
// across it by how far it is from the nearer end; plus a sub-pixel blend (Gr5.w - 1: AASoftness) for detail inside
// textures. One pass, ~0.3 ms at 1440p against the game's own (twice the pixels rendered).
float FxLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float FxL(float2 uv) { return FxLuma(tText.SampleLevel(sText, uv, 0).rgb); }
float3 Fxaa(float2 uv, float2 rcpF, float subpix)
{
    float3 rgbM = tText.SampleLevel(sText, uv, 0).rgb;
    float lM = FxLuma(rgbM);
    float lN = FxL(uv + float2(0, -rcpF.y)), lS = FxL(uv + float2(0, rcpF.y)), lW = FxL(uv + float2(-rcpF.x, 0)), lE = FxL(uv + float2(rcpF.x, 0));
    float mx = max(max(lN, lS), max(lW, max(lE, lM))), mn = min(min(lN, lS), min(lW, min(lE, lM)));
    float range = mx - mn;
    if (range < max(0.0312, mx * 0.125)) return rgbM;
    float lNW = FxL(uv - rcpF), lSE = FxL(uv + rcpF), lNE = FxL(uv + float2(rcpF.x, -rcpF.y)), lSW = FxL(uv + float2(-rcpF.x, rcpF.y));
    float lNS = lN + lS, lWE = lW + lE;
    float subpixNSWE = lNS + lWE;
    float edgeHorz1 = -2 * lM + lNS, edgeVert1 = -2 * lM + lWE;
    float lNESE = lNE + lSE, lNWNE = lNW + lNE;
    float edgeHorz2 = -2 * lE + lNESE, edgeVert2 = -2 * lN + lNWNE;
    float lNWSW = lNW + lSW, lSWSE = lSW + lSE;
    float edgeHorz4 = abs(edgeHorz1) * 2 + abs(edgeHorz2), edgeVert4 = abs(edgeVert1) * 2 + abs(edgeVert2);
    float edgeHorz3 = -2 * lW + lNWSW, edgeVert3 = -2 * lS + lSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4, edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lNWSW + lNESE;
    float lengthSign = rcpF.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2 + subpixNWSWNESE;
    if (!horzSpan) { lN = lW; lS = lE; }
    else lengthSign = rcpF.y;
    float subpixB = subpixA / 12 - lM;
    float gradientN = lN - lM, gradientS = lS - lM;
    float lNN = lN + lM, lSS = lS + lM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) / range);
    float2 posB = uv;
    float2 offNP = horzSpan ? float2(rcpF.x, 0) : float2(0, rcpF.y);
    if (!horzSpan) posB.x += lengthSign * 0.5; else posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP, posP = posB + offNP;
    float subpixD = -2 * subpixC + 3, subpixE = subpixC * subpixC;
    if (!pairN) lNN = lSS;
    float gradientScaled = gradient * 0.25;
    float lMM = lM - lNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lMLTZero = lMM < 0;
    float lEndN = FxL(posN) - lNN * 0.5, lEndP = FxL(posP) - lNN * 0.5;
    bool doneN = abs(lEndN) >= gradientScaled, doneP = abs(lEndP) >= gradientScaled;
    static const float kStep[7] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0 };
    [loop] for (int k = 0; k < 7 && !(doneN && doneP); ++k) {
        if (!doneN) { posN -= offNP * kStep[k]; lEndN = FxL(posN) - lNN * 0.5; doneN = abs(lEndN) >= gradientScaled; }
        if (!doneP) { posP += offNP * kStep[k]; lEndP = FxL(posP) - lNN * 0.5; doneP = abs(lEndP) >= gradientScaled; }
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y, dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lEndN < 0) != lMLTZero, goodSpanP = (lEndP < 0) != lMLTZero;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float pixelOffset = dst * (-1 / (dstP + dstN)) + 0.5;
    float subpixH = subpixF * subpixF * subpix;
    float off = max(goodSpan ? pixelOffset : 0, subpixH);
    float2 posM = uv;
    if (!horzSpan) posM.x += off * lengthSign; else posM.y += off * lengthSign;
    return tText.SampleLevel(sText, posM, 0).rgb;
}

float4 PS_Grade(VSOut i) : SV_Target
{
    float2 uv = i.uv;
    float4 src = tText.SampleLevel(sText, uv, 0);
    if (Gr4.x > 0.5 && uv.x < 0.5) return src;
    float3 c = Gr5.w > 0.5 ? Fxaa(uv, Gr3.zw, Gr5.w - 1) : src.rgb;   // 1.68: our anti-aliasing first
    // sharpening: AMD FidelityFX CAS (contrast adaptive, 3x3): strong where the neighbourhood is flat, little on hard
    // edges (no halos)
    if (Gr2.z > 0) {
        float2 t = Gr3.zw;
        float3 a = tText.SampleLevel(sText, uv + float2(-t.x, -t.y), 0).rgb, b = tText.SampleLevel(sText, uv + float2(0, -t.y), 0).rgb;
        float3 cc = tText.SampleLevel(sText, uv + float2(t.x, -t.y), 0).rgb, d = tText.SampleLevel(sText, uv + float2(-t.x, 0), 0).rgb;
        float3 f = tText.SampleLevel(sText, uv + float2(t.x, 0), 0).rgb, g = tText.SampleLevel(sText, uv + float2(-t.x, t.y), 0).rgb;
        float3 h = tText.SampleLevel(sText, uv + float2(0, t.y), 0).rgb, k = tText.SampleLevel(sText, uv + float2(t.x, t.y), 0).rgb;
        float3 mn = min(min(min(d, c), min(f, b)), h), mx = max(max(max(d, c), max(f, b)), h);
        mn += min(mn, min(min(a, cc), min(g, k)));
        mx += max(mx, max(max(a, cc), max(g, k)));
        float3 amp = sqrt(saturate(min(mn, 2 - mx) / max(mx, 1e-4)));
        float3 w = amp * (-1 / lerp(8.0, 4.5, saturate(Gr2.z)));
        c = saturate((c + (b + d + f + h) * w) / (1 + 4 * w));
    }
    float3 lin = ToLin(c);
    float3 loc = ToLin(tText.SampleLevel(sText, uv, Gr3.y).rgb);          // the local average (a blurred level)
    float3 all = ToLin(tText.SampleLevel(sText, float2(0.5, 0.5), 16).rgb); // the whole picture's average (the last level)
    // haze removal: take out part of the even veil (a share of the picture's average, less in its dark parts)
    // (never more than 60% of a pixel: dark places keep their detail instead of going black)
    float3 veil = all * 0.3 * Gr2.y;
    lin = max(lin - veil, lin * 0.4) / max(1 - veil, 0.2);
    loc = max(loc - veil, loc * 0.4) / max(1 - veil, 0.2);
    // exposure
    float ex = exp2(Gr0.x);
    lin *= ex; loc *= ex;
    float L = max(Lum(lin), 1e-5);
    float l = log2(L), la = log2(max(Lum(loc), 1e-5));
    // local contrast (clarity), in stops, limited
    float l2 = l + Gr2.x * clamp(l - la, -1.5, 1.5);
    // contrast around middle grey
    l2 = -2.47 + (l2 + 2.47) * Gr0.y;
    float L2 = exp2(l2);
    // shadows / highlights, weighted by where the pixel sits on the display scale
    float d = pow(saturate(L2), 1 / 2.2);
    float ws = 1 - smoothstep(0.0, 0.45, d), wh = smoothstep(0.55, 1.0, d);
    L2 *= exp2(Gr1.x * ws + Gr1.y * wh);
    // soft shoulder: no hard clip into white
    float knee = 0.8;
    if (L2 > knee) L2 = knee + (1 - knee) * (1 - exp(-(L2 - knee) / (1 - knee)));
    // film curve: the game's picture taken back towards scene light, then a photographic curve (soft toe, long
    // shoulder): bright parts roll off gently instead of flattening into one colour
    if (Gr5.x > 0) {
        float Ls = L2 / max(1 - min(L2, 0.97) * 0.85, 0.05);
        float W2 = 6.0 * 6.0;
        float Lf = Ls * (1 + Ls / W2) / (1 + Ls);
        Lf = pow(Lf, 0.93) * 1.04;                       // a lifted toe, mid grey kept about
        L2 = lerp(L2, Lf, Gr5.x);
    }
    lin *= L2 / L;
    // white balance (luminance kept)
    float3 wb = float3(1 + 0.12 * Gr1.z, 1 - 0.06 * Gr1.w, 1 - 0.12 * Gr1.z) * float3(1 + 0.03 * Gr1.w, 1, 1 + 0.03 * Gr1.w);
    lin *= wb / Lum(wb);
    // saturation and vibrance (vibrance lifts the dull colours more than the vivid ones)
    float Ln = Lum(lin);
    float mxc = max(lin.r, max(lin.g, lin.b)), mnc = min(lin.r, min(lin.g, lin.b));
    float sat = (mxc - mnc) / max(mxc, 1e-5);
    lin = Ln + (lin - Ln) * max(Gr0.z * (1 + Gr0.w * (1 - sat)), 0);
    lin = max(lin, 0);
    // reds (and red-oranges) on their own: real paint and lights are less neon than the game's
    {
        float red = saturate((lin.r - max(lin.g, lin.b)) / max(lin.r, 1e-5)) * saturate(1 - (lin.g - lin.b) / max(lin.r, 1e-5) * 1.5);
        float Lr = Lum(lin);
        float k = lerp(1, Gr5.y, red);
        lin = (Lr + (lin - Lr) * k) * lerp(1, 0.55 + 0.45 * Gr5.y, red);   // calmer and deeper (crimson, not pink)
        Lr = Lum(lin);
        // highlights go towards white as they near it (film), not into a saturated colour
        float dl = pow(saturate(Lr), 1 / 2.2);
        lin = lerp(lin, Lr, smoothstep(0.6, 1.0, dl) * Gr5.z);
    }
    // split toning: cool shadows, warm highlights (luminance kept)
    if (Gr3.x > 0) {
        float dl = pow(saturate(Lum(lin)), 1 / 2.2);
        float3 tone = lerp(float3(0.90, 0.98, 1.12), float3(1.08, 1.0, 0.88), smoothstep(0.15, 0.75, dl));
        lin *= lerp(1, tone / Lum(tone), Gr3.x);
    }
    // bloom: a soft glow (the copy's blurred levels, wide to wider): the bright parts glow, and the whole picture
    // diffuses a little towards it, like a mist filter on a camera
    if (Gr4.z > 0) {
        float3 g = 0, gb = 0;
        float wsum = 0;
        [unroll] for (int b = 0; b < 4; ++b) {
            float3 m = ToLin(tText.SampleLevel(sText, uv, Gr3.y - 3 + b).rgb) * ex;
            float w = 1 + b;
            g += m * w; wsum += w;
            gb += m * smoothstep(0.08, 0.5, Lum(m)) * w;
        }
        g /= wsum; gb /= wsum;
        lin = lerp(lin, g, Gr4.z * 0.12) + gb * Gr4.z * 0.8;
    }
    // film grain, strongest in the middle tones
    if (Gr4.w > 0) {
        float2 q = i.pos.xy + Gr4.y * float2(37.0, 17.0);
        float nz = frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715)))) - 0.5;
        float dl = pow(saturate(Lum(lin)), 1 / 2.2);
        lin *= 1 + nz * Gr4.w * 0.4 * (1 - abs(dl * 2 - 1) * 0.6);
    }
    // vignette
    float2 v = (uv - 0.5) * float2(1.0, 0.75);
    lin *= 1 - Gr2.w * smoothstep(0.1, 0.55, dot(v, v) * 2.0);
    // 1.65: the 8-bit picture dithered: triangular noise of +-1 step (two uniform ones added) before it is rounded, so
    // smooth gradients (the sky, haze, a lamp's falloff after the curves) show no bands; below what the eye sees
    float tp = IgnStill(i.pos.xy) + IgnStill(i.pos.xy + float2(37.17, 11.83)) - 1;
    return float4(ToDisp(lin) + tp / 255.0, src.a);
}

float4 PS_Overlay(VSOut i) : SV_Target
{
    float4 t = tText.Sample(sText, i.uv);
    return float4(t.rgb, t.a);
}

// 1.81: the rain drops on the player's car's glass. The game draws its glass after its lighting (PS_Sun saw the cabin
// through it: the drops there lay under the glass drawn over them), so each of its glass draws is drawn once more with
// this (carrain.inc), blended over (dest x (1 - a) + c): at a glass pixel the drop there (PS_Sun's atlas lookup; the
// glass's own normal from its triangle), shaded as a water cap on glass: the sky mirrored by its Fresnel (the ground
// below the horizon), the sky's soft highlight, the sun's glint; what is behind it darker through it, its rim dark.
Texture2D<float4> tCamNow : register(t79);   // this frame's view ray record (PS_Cam: A, B, C, the camera)
float4 PS_CarGlass(float4 pos : SV_Position) : SV_Target0
{
    if (P27.w <= 0 || P28.x < 0.5) discard;
    float2 uv = pos.xy / Size.xy;
    float4 t1 = tCamNow.Load(int3(0, 0, 0)) + tCamNow.Load(int3(1, 0, 0)) * uv.x + tCamNow.Load(int3(2, 0, 0)) * uv.y;
    float dd = pos.z - t1.w;
    dd = abs(dd) < 1e-9 ? 1e-9 : dd;
    float3 rel = t1.xyz / dd, world = tCamNow.Load(int3(3, 0, 0)).xyz + rel;
    float3 wx = ddx(world), wy = ddy(world);
    float3 n = normalize(cross(wy, wx) + 1e-12);
    float3 toC = normalize(-rel);
    if (dot(n, toC) < 0) n = -n;
    float pxs = max(length(wx), length(wy));
    float3 Ppl, ax, az;
    if (!PlayerPose(Ppl, ax, az) || !InPlayerBody(world, Ppl, ax, az)) discard;
    float3 ay = normalize(cross(az, ax));
    if (ay.y < 0) ay = -ay;
    float3 lw = world - Ppl, lp = float3(dot(lw, ax), dot(lw, ay), dot(lw, az));
    int pl = FaceOf(float3(dot(n, ax), dot(n, ay), dot(n, az)));
    if (pl < 0) discard;
    float2 pp = FaceP(pl, lp);
    float2 tc = CrAtlas(pl, pp);
    int2 b0 = int2(floor(tc - 0.5));
    float bestD = 9, bestR = 1, bestF = 1;
    float2 bestO = 0;
    [unroll] for (int k = 0; k < 4; ++k) {
        uint id = tDropAtlas.Load(int3(clamp(b0 + int2(k & 1, k >> 1), int2(0, 0), int2(1999, 2249)), 0));
        if (id == 0) continue;
        uint idx = id - 1;
        int3 sq = int3(idx % 128, idx / 128, 0);
        float4 S = tDropS.Load(sq), Vd = tDropV.Load(sq);
        if (S.z <= 0 || abs(S.w - pl) > 0.5) continue;
        float sp = length(Vd.xy);
        float2 tl = sp > 1e-3 ? -Vd.xy / sp * min(sp * 0.1, 0.025) : 0;   // (carrain.inc's runlet)
        float t = saturate(dot(pp - S.xy, tl) / max(dot(tl, tl), 1e-12));
        float rr = S.z * (1 - 0.4 * t);
        float2 o = (pp - S.xy - tl * t) / rr;
        float fwk = pxs / rr, Rk = sqrt(1 + 0.25 * fwk * fwk);
        if (length(o) / Rk < bestD) { bestD = length(o) / Rk; bestO = o; bestR = rr; bestF = saturate((S.w - pl) * 40); }
    }
    if (bestD >= 1) discard;
    float fw = pxs / bestR, Rs = sqrt(1 + 0.25 * fw * fw);
    float cov = saturate(saturate((1 - bestD) * Rs / (0.5 * fw + 0.08)) / (Rs * Rs) * bestF * P33.w), x = min(bestD * Rs, 0.999), s = saturate(1.6 - fw);
    float2 dir2 = bestO / max(length(bestO), 1e-4);
    float3 Uw = pl == 1 || pl == 2 ? az : ax, Vw = pl == 0 ? az : ay;
    float st = x * 0.985, tilt = min(st / sqrt(1 - st * st), 5.0);
    float3 nd = normalize(n + (Uw * dir2.x + Vw * dir2.y) * tilt);
    float ndv = saturate(dot(nd, toC));
    float fr = 0.02 + 0.98 * pow(1 - ndv, 5);
    float3 rv = reflect(-toC, nd);
    float3 env = lerp(PanoSky(float3(rv.x, 0.02, rv.z), 0.5) * 0.15, PanoSky(rv, 0.97), saturate(rv.y * 4 + 0.5));
    float3 Ld = SunLd(), Lc = SunLc();
    float3 hv = normalize(toC - Ld);
    float ndh = saturate(dot(nd, hv)), dg = ndh * ndh * (0.0016 - 1) + 1;
    float fs = 0.02 + 0.98 * pow(1 - saturate(dot(hv, toC)), 5);
    float3 sunS = Lc * min(0.0016 / (3.14159 * dg * dg) * fs / (4 * max(ndv, 0.1)), 40) * saturate(dot(nd, -Ld)) * P2.y;
    float3 hz = normalize(toC + float3(0, 1, 0));
    float nz = saturate(dot(nd, hz)), dz = nz * nz * (0.06 - 1) + 1;
    float3 skyS = PanoSky(float3(0, 1, 0), 0.6) * min(0.06 / (3.14159 * dz * dz), 5) * fr * 0.6;
    float edge = smoothstep(0.55, 0.95, x);
    float3 eC = env * fr + sunS + skyS;
    float eA = (0.12 + 0.6 * edge) * (1 - fr);
    // its average (the pixel bigger than it): ~10% of the sky mirrored, the sun's glint spread over it, darker through it
    float fA = 0.02 + 0.98 * pow(1 - saturate(dot(hv, toC)), 5);
    float3 aC = PanoSky(reflect(-toC, n), 0.6) * (0.08 + 0.2 * pow(1 - saturate(dot(n, toC)), 3)) + Lc * fA * 0.25 * P2.y * step(0.17, dot(hv, n));
    return float4(lerp(aC, eC, s) * cov, cov * lerp(0.25, eA, s));
}

// ---------------------------------------------------------------- 1.83: rain falling (rainfall.inc)
// Streaks round the camera: two lattices of drops (near: 14 x 10 x 14 m, far: 48 x 24 x 48 m), each fixed in the
// world, falling at its own speed (5.5 .. 9 m/s), drifting with the wind, wrapped round the camera. A streak is the
// drop's path over the eye's ~1/60 s as the camera sees it (its fall and drift less the camera's own motion: slanted
// when driving), a drop's width or a pixel's (then fainter: the same light spread over the pixel). Sheltered drops (the
// rain map: something above) are left out; a streak behind the scene is hidden (softly). Its light: the sky it bends
// to the eye and the light round it (the scene of the last frame, blurred: lamps, headlights, signs make it glint).
// Splashes where drops land: a disk of 25 m round the camera, on the rain map's top surface (the open ground), a crown
// for ~0.14 s each. P30: rain x streaks, their length x, drop size x, splashes x; P31: the wind (m/s), ripples x;
// P32: the camera's velocity (m/s); P33: -, rain now (ripples).
Texture2D<float4> tRainBg : register(t80);   // the scene, last frame (mips): the light round a drop
struct RainV { float4 pos : SV_Position; float4 a : TEXCOORD0; float4 b : TEXCOORD1; float4 c : TEXCOORD2; };   // (1.88b: c: the headlights' light)
uint RfHash(uint x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return x; }
float RfRnd(uint i, uint s) { return (RfHash(i * 0x9E3779B9U ^ RfHash(s * 0x85EBCA6BU + 0x165667B1U)) >> 8) / 16777216.0; }
// a world point on this frame's screen (the view ray record: A + B u + C v = s rel): clip space with w = its depth
float4 RfClip(float3 world, out float dist)
{
    float4 A = tCamNow.Load(int3(0, 0, 0)), B = tCamNow.Load(int3(1, 0, 0)), C = tCamNow.Load(int3(2, 0, 0));
    float3 rel = world - tCamNow.Load(int3(3, 0, 0)).xyz;
    dist = length(rel);
    float3 c12 = cross(C.xyz, -rel), b = -A.xyz;
    float det = dot(B.xyz, c12);
    if (abs(det) < 1e-12) return float4(0, 0, -1, 1);
    float2 uv = float2(dot(b, c12), dot(B.xyz, cross(b, -rel))) / det;
    // (1.86: the game's record holds its rays pointing back (rel = t1 / (depth - t1.w), that negative: worldedit.inc builds
    // it so, the flare reads it so); 1.83 .. 1.85 took them as forward: what was in front counted as behind the eye (left
    // out), what was behind drawn mirrored through it: splashes in the sky, the lens's drops going up, the rain in front
    // missing. Its forward from the record's own w: the test harness's runs forward, t1.w < 0)
    float4 t1c = A + 0.5 * B + 0.5 * C;
    float w = dot(rel, (t1c.w > 0.5 ? -1 : 1) * normalize(t1c.xyz));
    return float4((uv.x * 2 - 1) * w, (1 - uv.y * 2) * w, 0.5 * w, w);
}
// the rain map's top surface (the highest thing seen from above) at a place; false: not in the map
bool RfTop(float2 xz, out float top)
{
    top = -1e4;
    if (RN1.y < 0.5) return false;
    float2 uv = float2((xz.x - RN0.x) / RN0.z * 0.5 + 0.5, 0.5 - (xz.y - RN0.y) / RN0.z * 0.5);
    if (any(uv < 0.01) || any(uv > 0.99)) return false;
    top = RN0.w - tRain.SampleLevel(sPoint, uv, 0) * RN1.x;
    return true;
}
// a pixel's angle (radians), from the view ray record
float RfPixAngle()
{
    float4 A = tCamNow.Load(int3(0, 0, 0)), B = tCamNow.Load(int3(1, 0, 0)), C = tCamNow.Load(int3(2, 0, 0));
    return length(C.xyz) / max(length((A + 0.5 * B + 0.5 * C).xyz), 1e-6) / Size.y;
}
// the scene's distance at a pixel (for hiding what is behind it)
float RfSceneDistUv(float2 uv);
float RfSceneDist(float2 pix) { return RfSceneDistUv(pix / Size.xy); }
float RfSceneDistUv(float2 uv)
{
    float4 t1 = tCamNow.Load(int3(0, 0, 0)) + tCamNow.Load(int3(1, 0, 0)) * uv.x + tCamNow.Load(int3(2, 0, 0)) * uv.y;
    float dd = tDepth.SampleLevel(sPoint, uv, 0).x - t1.w;
    return abs(dd) < 1e-9 ? 1e6 : length(t1.xyz / dd);
}

RainV VS_Rain(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    RainV o;
    o.pos = float4(0, 0, -1, 1); o.a = 0; o.b = 0; o.c = 0;
    float R = P30.x;
    if (RfRnd(inst, 4) >= R) return o;   // (as many fall as the rain's strength)
    uint far = inst & 1;
    float3 box = far ? float3(48, 24, 48) : float3(14, 10, 14);
    float3 cam = tCamNow.Load(int3(3, 0, 0)).xyz;
    float t = P29.x;
    float vf = lerp(5.5, 9.0, RfRnd(inst, 5)) * P32.w;   // its fall (small .. big drops; 1.84: x RainSpeed)
    float3 vel = float3(P31.x, -vf, P31.z) * (0.85 + 0.3 * RfRnd(inst, 6));
    float3 p0 = float3(RfRnd(inst, 1), RfRnd(inst, 2), RfRnd(inst, 3)) * box + vel * t;
    float3 lo = cam - box * float3(0.5, 0.4, 0.5);
    float3 p = lo + (p0 - lo) - box * floor((p0 - lo) / box);   // wrapped into the box round the camera
    float top;
    if (RfTop(p.xz, top) && p.y < top - 0.3) return o;          // sheltered: a roof, a bridge, a tree above it
    float3 vrel = vel - P32.xyz * 0.5;                             // as the camera sees it go (its own motion halved: at speed
    float3 tail = -vrel * (0.0167 * P30.y);                         // the full one made a star field)
    float tl = length(tail);
    if (tl > 1.2) tail *= 1.2 / tl;
    float d0, d1;
    float4 c0 = RfClip(p, d0), c1 = RfClip(p + tail, d1);
    if (c0.w < 0.35 || c1.w < 0.35) return o;
    // width: the drop's (~1.5 mm) or a pixel's at that distance (then fainter)
    float pa = RfPixAngle();
    float wd = 0.0015 * P30.z, wp = max(wd, d0 * pa * 2.0);   // (2 px at least: 1 px ones came out dotted)
    float2 n0 = c0.xy / c0.w, n1 = c1.xy / c1.w;
    float2 sp = (n1 - n0) * Size.xy * 0.5;                         // the streak on screen, in pixels
    float sl = length(sp);
    float2 dir = sl > 1e-3 ? sp / sl : float2(0, -1), perp = float2(-dir.y, dir.x);
    float hw = wp / (d0 * pa) * 0.5;                               // half its width, in pixels
    bool head = (vid & 1) == 0;
    float side = (vid & 2) ? 1 : -1;
    float4 c = head ? c0 : c1;
    float2 offPix = perp * side * hw + dir * (head ? -hw : hw);
    o.pos = c + float4(offPix / (Size.xy * 0.5) * c.w, 0, 0);
    float near = smoothstep(0.35, 1.2, d0);                        // (right at the eye: none)
    float alpha = (far ? 0.9 : 1.8) * (wd / wp) * near * saturate(1.5 / max(sl / max(hw * 2, 1), 1) + 0.3) * P33.x;   // (1.88: x RainBright)
    o.a = float4(head ? 0 : 1, side, alpha, head ? d0 : d1);
    // 1.88b: the game's own headlights (its projected spot: HeadlightCB b8, its pattern t43; captured by headlight.inc
    // only when the game draws them): the light at the drop, the pattern x the game's attenuation x its colour, spread
    // with distance as in the air (1.88's own two cones were too bright, white, lit with the lights off too)
    float3 beam = 0;
    if (P31.y > 0) {
        float3 hP = p - cam;
        float4 hc = hP.x * HlM[0] + hP.y * HlM[1] + hP.z * HlM[2] + HlM[3];
        if (hc.w > 0 && all(abs(hc.xyz) < hc.w)) {
            float2 huv = hc.xy / hc.w * 0.5 + 0.5;
            float3 hd = HlPos.xyz - hP;
            float d2 = dot(hd, hd);
            float att = saturate(1 / (HlAtt.x * d2 + HlAtt.y * sqrt(d2 + 0.001) + HlAtt.z)) / (1 + d2 / 400);
            beam = tHlTex.SampleLevel(sLin, huv, 0).rgb * att * HlCol.rgb * P31.y;
        }
    }
    o.c = float4(beam, 0);
    o.b = float4(far, RfRnd(inst, 7), 0, 0);
    return o;
}
float4 PS_Rain(RainV i) : SV_Target0
{
    float across = 1 - abs(i.a.y);
    float along = saturate(min(i.a.x, 1 - i.a.x) * 6 + 0.15);
    float a = i.a.z * across * along * 1.6;
    a *= saturate((RfSceneDist(i.pos.xy) - i.a.w) / 0.4);           // behind the scene: hidden
    if (a < 0.002) discard;
    float2 uv = i.pos.xy / Size.xy;
    float3 bg = tRainBg.SampleLevel(sLin, uv, 3).rgb;
    float3 sky = PanoSky(float3(0, 1, 0), 0.4);
    float3 c = (sky * 0.5 + bg * 1.2) * (0.7 + 0.6 * i.b.y);
    c += i.c.rgb * (0.2 + 0.6 * i.b.y);   // (1.88b: the headlights' light it scatters to the eye; some drops glint more)
    return float4(c * a * 0.8, a * 0.15);   // blended: dest x (1 - a) + c: it brightens (the light it bends), hides little
}

RainV VS_Splash(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    RainV o;
    o.pos = float4(0, 0, -1, 1); o.a = 0; o.b = 0; o.c = 0;
    float R = P33.y * P30.w;
    if (RfRnd(inst, 11) >= R) return o;
    float per = lerp(0.35, 0.75, RfRnd(inst, 12));
    float ph = P29.x / per + RfRnd(inst, 13);
    float cyc = floor(ph), age = frac(ph) * per;
    if (age > 0.14) return o;
    uint seed = inst * 7919U + (uint)cyc * 104729U;
    float3 cam = tCamNow.Load(int3(3, 0, 0)).xyz;
    float ang = RfRnd(seed, 1) * 6.2832, rad = 0.6 + 24.4 * RfRnd(seed, 2) * RfRnd(seed, 2);   // (more near)
    float3 p = cam + float3(cos(ang) * rad, 0, sin(ang) * rad);
    float top;
    if (!RfTop(p.xz, top)) return o;
    p.y = top;
    if (top > cam.y + 2.0) return o;   // (1.84: a roof above the eye: not seen from here)
    // 1.85: its height from the surface seen there: down the column from 0.3 m over the rain map's top to 0.75 m under
    // it, where the view ray goes from passing over the surface (the scene farther than the point) to under it (nearer):
    // the height there, interpolated, then checked (the map's top stood ~0.3-0.5 m over the ground: 1.83's splashes
    // hung in the air, 1.84's were checked and dropped)
    float prevF = 0, prevY = 0, yHit = 0, d = 1;
    bool havePrev = false, found = false;
    [unroll] for (int s = 0; s < 8; ++s) {
        float y = top + 0.3 - s * 0.15, dq;
        float4 cq = RfClip(float3(p.x, y, p.z), dq);
        float2 qu = float2(cq.x / max(cq.w, 1e-4) * 0.5 + 0.5, 0.5 - cq.y / max(cq.w, 1e-4) * 0.5);
        if (cq.w < 0.5 || any(qu < 0) || any(qu > 1)) { havePrev = false; continue; }
        float fv = RfSceneDistUv(qu) - dq;
        if (!found && havePrev && prevF > 0 && fv <= 0) { yHit = lerp(prevY, y, prevF / (prevF - fv)); found = true; }
        prevF = fv; prevY = y; havePrev = true;
    }
    if (!found) return o;
    p.y = yHit;
    float4 cp = RfClip(p, d);
    if (cp.w < 0.5) return o;
    {
        float2 su = float2(cp.x / cp.w * 0.5 + 0.5, 0.5 - cp.y / cp.w * 0.5);
        if (any(su < 0) || any(su > 1) || abs(RfSceneDistUv(su) - d) > 0.15 + 0.03 * d) return o;   // (an edge in front: not here)
    }
    float k = age / 0.14;
    float size = (0.04 + 0.06 * RfRnd(seed, 3)) * (0.5 + 0.5 * sqrt(k));   // (1.85: a little bigger)
    float pa = RfPixAngle();
    float pix = size / (d * pa);                                    // its size on screen, pixels
    if (pix < 0.7) return o;
    float2 corner = float2((vid & 1) ? 1 : -1, (vid & 2) ? 1 : 0);  // across -1..1, up 0..1
    o.pos = cp + float4(float2(corner.x, corner.y * 1.4) * pix / (Size.xy * 0.5) * cp.w, 0, 0);
    o.a = float4(corner.x, corner.y * 1.4, saturate(pix / 3), d);
    o.b = float4(k, RfRnd(seed, 4) * 6.2832, 0, 0);
    return o;
}
float4 PS_Splash(RainV i) : SV_Target0
{
    // droplets thrown up and out (6, each its own angle, speed, size: on an arc, fading), a ring spreading at its foot
    float2 q = i.a.xy;   // across -1..1, up 0..1.4 (in its size)
    float k = i.b.x, a = 0;
    [unroll] for (int j = 0; j < 6; ++j) {
        float h1 = frac(sin(i.b.y * 7.13 + j * 12.9898) * 43758.5453), h2 = frac(h1 * 37.7), h3 = frac(h1 * 91.3);
        float2 c = float2(cos(i.b.y + j * 1.047 + h1) * (0.35 + 0.6 * h2) * k, (0.5 + 0.9 * h3) * 4 * k * (1 - k) * 0.8);
        float r = 0.06 + 0.05 * h2;
        a += exp(-dot(q - c, q - c) / (r * r)) * (1 - k * 0.7);
    }
    float ring = exp(-pow((abs(q.x) - k * 0.9) / 0.07, 2)) * exp(-pow(q.y / 0.05, 2)) * (1 - k);
    a = saturate(a + ring * 0.6) * i.a.z;
    a *= saturate((RfSceneDist(i.pos.xy) - i.a.w + 0.25) / 0.3);   // behind the scene: hidden
    if (a < 0.003) discard;
    float2 uv = i.pos.xy / Size.xy;
    float3 bg = tRainBg.SampleLevel(sLin, uv, 2).rgb;
    float3 sky = PanoSky(float3(0, 1, 0), 0.4);
    return float4((sky * 0.5 + bg * 0.8) * a, a * 0.2);
}

// 1.83: rain on standing water: rings from the drops landing (cells of 14 cm, a drop each at its own time, 0.6 .. 1.2 s a
// cycle; fewer cells in lighter rain), the ring's slope (a ridge running out at 25 cm/s, fading); 3 x 3 cells round the
// point. Returns the surface's slope (x, z)
float2 RainRipple(float2 p, float t, float rate)
{
    float2 s = 0;
    float2 id = floor(p / 0.14);
    [unroll] for (int k = 0; k < 9; ++k) {
        float2 cid = id + float2(k % 3, k / 3) - 1;
        uint ci = (uint)((int)cid.x * 73856093) ^ (uint)((int)cid.y * 19349663);
        float h = RfRnd(ci, 21), h2 = RfRnd(ci, 22), h3 = RfRnd(ci, 23);
        float per = 0.6 + 0.6 * h2;
        float age = frac(t / per + h3) * per;
        float2 ctr = (cid + 0.15 + 0.7 * float2(h2, RfRnd(ci, 24))) * 0.14;
        float2 dv = p - ctr;
        float r = length(dv);
        float x = (r - age * 0.25) / 0.012;
        float amp = step(h, rate) * exp(-age * 3.5) * 0.35;
        s += dv / max(r, 1e-4) * (-2 * x * exp(-x * x)) * amp;
    }
    return s;
}

// ---------------------------------------------------------------- 1.84: rain on the camera's lens (rainfall.inc: LensTick)
// The drops are simulated on the CPU (a few hundred: they land by the rain's flux into the lens, slide down by gravity
// once big enough, lag behind the camera's acceleration, are blown out to the edges by the air at speed, merge, dry),
// sent as LD[] (b10): per drop its head (uv), radius (screen heights), the aspect; its trail (screen heights), its
// strength. Each is a small lens: the scene through it bent (inverted, smaller; blurred: out of focus), its rim dark, a
// highlight of the sky above; a running one draws a thinning trail.
cbuffer LensCB : register(b10) { float4 LD[800]; };
RainV VS_Lens(uint vid : SV_VertexID, uint inst : SV_InstanceID)
{
    RainV o;
    o.c = 0;
    float4 d0 = LD[inst * 2], d1 = LD[inst * 2 + 1];
    float asp = d0.w, r = d0.z;
    float2 hs = float2(d0.x * asp, d0.y), ts = hs + d1.xy;
    float2 mn = min(hs, ts) - r * 1.4, mx = max(hs, ts) + r * 1.4;
    float2 c = float2((vid & 1) ? mx.x : mn.x, (vid & 2) ? mx.y : mn.y);
    o.pos = float4(c.x / asp * 2 - 1, 1 - c.y * 2, 0, 1);
    o.a = float4(c, r, d1.z);
    o.b = float4(hs, ts);
    return o;
}
float4 PS_Lens(RainV i) : SV_Target0
{
    // (1.86: a running drop as on glass: its round head, a thin trail behind it thinning to nothing; 1.84 drew one tapered
    // capsule: its soft edges made two "ears" at the head)
    float2 p = i.a.xy, a = i.b.xy, b = i.b.zw;
    float r = i.a.z;
    float2 o = (p - a) / r;
    float dh = length(o);
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-9));
    float wt = r * 0.32 * (1 - t);                       // the trail's half width
    float dt = length(p - a - ab * t) / max(wt, 1e-5);
    float covH = saturate((1 - dh) / 0.2);
    float covT = dot(ab, ab) > r * r * 0.04 && t > 0.05 ? saturate((1 - dt) / 0.35) * 0.55 * (1 - t * 0.5) : 0;
    float cov = max(covH, covT);
    if (cov <= 0.001) discard;
    float2 oo = covH >= covT ? o : (p - a - ab * t) / max(wt, 1e-5) * 0.6;   // (the trail: a shallow film)
    float d = min(length(oo), 0.999);
    float z = sqrt(1 - d * d);
    float2 uv = i.pos.xy / Size.xy;
    float2 bend = -oo * (covH >= covT ? r : wt) * 1.8 * float2(Size.y / Size.x, 1);
    float3 seen = tRainBg.SampleLevel(sLin, saturate(uv + bend), 1.5).rgb;
    float rim = smoothstep(0.78, 1.0, d);
    float3 sky = PanoSky(float3(0, 1, 0), 0.5);
    float hl = pow(saturate(dot(normalize(float3(oo.x, oo.y, z + 1e-3)), normalize(float3(-0.4, -0.6, 0.7)))), 40);
    float glow = saturate(oo.y * 1.4) * (1 - rim) * 0.12;
    float3 c = seen * (1.04 + glow - 0.45 * rim) + sky * hl * 0.8;
    float k = cov * i.a.w;
    return float4(c * k, k);
}
