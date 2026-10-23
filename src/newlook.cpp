// NewLook 1.64 - better sun shadows, AO, bounce light and air for Need for Speed: Most Wanted (2012).
//
// An ASI plugin (loaded by the ASI loader of HD Effects, dinput8.dll), separate from DroneCam.
// It hooks D3D11 through the vtables of a throw-away device: CreatePixelShader recognises the
// game's deferred sun-pass shaders (by their reflection names), and right after every sun-pass
// draw we copy the render target and draw once more with our own pixel shader (NewLook.hlsl),
// which takes the game's sun light out again and puts it back with better shadows.
//
// Timing: the overlay shows FPS and the GPU time of every pass (D3D11 timestamp queries).
//
// Keys: Y on/off, U view (normal / split / masks), I/O select, K/L change, J reset,
//       H save, P overlay on/off, Shift+P reload NewLook.hlsl and NewLook.ini.

#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

// ---------------------------------------------------------------- log

static char g_dir[MAX_PATH], g_logPath[MAX_PATH], g_iniPath[MAX_PATH], g_hlslPath[MAX_PATH];
static CRITICAL_SECTION g_logLock;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_logLock);
    FILE* f = fopen(g_logPath, "a");
    if (f) {
        fprintf(f, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
    LeaveCriticalSection(&g_logLock);
}

// ---------------------------------------------------------------- settings

struct Param { const char* key; const char* label; float def, mn, mx, step, v; };
static Param g_p[] = {
    { "ContactStrength", "Contact shadows (screen space)", 0.0f, 0.0f, 1.0f, 0.1f, 0 },
    { "ContactLength", "Contact length, m", 0.8f, 0.1f, 4.0f, 0.1f, 0 },
    { "ContactThickness", "Contact thickness, m", 0.5f, 0.05f, 3.0f, 0.05f, 0 },
    { "SunSize", "Sun size (x real sun)", 3.0f, 0.0f, 40.0f, 0.5f, 0 },
    { "MinSoftCm", "Shadow softness near, cm", 1.0f, 0.0f, 20.0f, 0.5f, 0 },
    { "MaxSoftCm", "Shadow softness far, cm", 40.0f, 5.0f, 300.0f, 5.0f, 0 },
    { "AOStrength", "Ambient occlusion", 0.6f, 0.0f, 1.5f, 0.05f, 0 },
    { "AORadius", "AO radius, m", 1.0f, 0.1f, 4.0f, 0.1f, 0 },
    { "AmbientScale", "Ambient light", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "SunScale", "Sun strength", 1.0f, 0.0f, 3.0f, 0.05f, 0 },
    { "ContactBias", "Contact start offset, m", 0.03f, 0.0f, 0.5f, 0.01f, 0 },
    { "SharpCm", "Sharp shadow up to, cm", 45.0f, 0.0f, 300.0f, 5.0f, 0 },
    { "ShadowDarkness", "Shadow darkness (1 = game)", 0.85f, 0.3f, 1.5f, 0.05f, 0 },
    { "SkyTint", "Sky-blue tint in shadow", 0.5f, 0.0f, 2.0f, 0.1f, 0 },
    { "SunWarmth", "Sun warmth (- cool + warm)", 0.3f, -1.0f, 1.0f, 0.05f, 0 },
    { "AmbientTint", "Ambient tint (- cool + warm)", 0.0f, -1.0f, 1.0f, 0.05f, 0 },
    { "HazeAmount", "Haze", 1.0f, 0.0f, 6.0f, 0.1f, 0 },
    { "HazeHeight", "Haze height, m", 60.0f, 5.0f, 500.0f, 5.0f, 0 },
    { "SunGlow", "Glow around the sun", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "Shafts", "Light shafts", 1.0f, 0.0f, 1.0f, 0.1f, 0 },
    { "HazeBlue", "Haze colour (0 grey, 1 sky blue)", 0.6f, 0.0f, 1.0f, 0.05f, 0 },
    { "BounceLight", "Bounce light", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "BounceRadius", "Bounce light reach, m", 2.5f, 0.5f, 8.0f, 0.25f, 0 },
    { "FastPath", "Fast path (no frame copies)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "BounceAllLights", "Bounce light source", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "BounceSmoothing", "Bounce smoothing over frames", 0.75f, 0.0f, 0.95f, 0.05f, 0 },
    { "LampLight", "Lamp light", 1.0f, 0.0f, 10.0f, 0.1f, 0 },
    { "LampThreshold", "Lamp detection threshold", 6.0f, 2.0f, 40.0f, 1.0f, 0 },
    { "LampReach", "Lamps drawn up to, m", 200.0f, 20.0f, 1000.0f, 10.0f, 0 },
    { "LampSource", "Lamps used", 1.0f, 0.0f, 2.0f, 1.0f, 0 },
    { "LampMapByDay", "Map lamps by day", 0.0f, 0.0f, 1.0f, 0.1f, 0 },
    { "LampAutoLearn", "Learn steady lamps into the map", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "ManualLampPower", "First hand-placed lamp power", 20.0f, 1.0f, 200.0f, 1.0f, 0 },
    { "KnockedLamp", "Lamp of a knocked-down post", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "AutoLamps", "Lamps on every post (templates)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LampRadius", "Lamp light radius, m", 40.0f, 5.0f, 150.0f, 1.0f, 0 },
    { "LampShadows", "Lamp shadows (screen space)", 1.0f, 0.0f, 1.0f, 0.1f, 0 },
    { "LampShadowThickness", "Lamp shadow caster thickness, m", 1.5f, 0.2f, 6.0f, 0.1f, 0 },
    { "NightAmbient", "Night ambient (baked light, sky)", 0.75f, 0.1f, 1.0f, 0.05f, 0 },
    { "LampShadowMaps", "Lamp shadow maps (walls, buildings)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LampSize", "Lamp size (shadow softness), m", 0.3f, 0.02f, 2.0f, 0.02f, 0 },
    { "LampShadowBudget", "Lamp shadow objects a frame", 100.0f, 25.0f, 2000.0f, 25.0f, 0 },
    { "LampShadowCutouts", "Lamp shadow cut-outs (leaves, fences)", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SoftSunShadows", "Soft sun shadows (ours; off = the game's)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LampResolution", "Lamp light at half resolution (faster)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "Look", "Colour grading", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LookExposure", "Exposure, EV", 0.0f, -2.0f, 2.0f, 0.05f, 0 },
    { "LookContrast", "Contrast", 1.1f, 0.5f, 2.0f, 0.05f, 0 },
    { "LookShadows", "Shadows (- deeper, + lifted)", 0.05f, -1.0f, 1.0f, 0.05f, 0 },
    { "LookHighlights", "Highlights (- recovered)", -0.15f, -1.0f, 1.0f, 0.05f, 0 },
    { "LookSaturation", "Saturation", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "LookVibrance", "Vibrance (dull colours)", 0.25f, -1.0f, 1.0f, 0.05f, 0 },
    { "LookTemperature", "Temperature (- cool + warm)", 0.05f, -1.0f, 1.0f, 0.05f, 0 },
    { "LookTint", "Tint (- green + magenta)", 0.0f, -1.0f, 1.0f, 0.05f, 0 },
    { "LookClarity", "Clarity (local contrast)", 0.35f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookDehaze", "Dehaze", 0.4f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookSharpen", "Sharpen", 0.3f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookVignette", "Vignette", 0.2f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookSplitTone", "Split toning (cool shadows, warm lights)", 0.25f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookBloom", "Bloom (glow of bright parts)", 0.2f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookGrain", "Film grain", 0.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookPreset", "Look preset", 0.0f, 0.0f, 4.0f, 1.0f, 0 },
    { "SkyOcclusion", "Sky light occlusion (large-scale AO)", 0.8f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyOcclusionRadius", "Sky occlusion radius, m", 6.0f, 1.0f, 20.0f, 0.5f, 0 },
    { "Reflections", "Reflections (screen space)", 0.8f, 0.0f, 1.5f, 0.05f, 0 },
    { "WetGround", "Wet ground (roads after rain)", 0.3f, 0.0f, 1.0f, 0.05f, 0 },
    { "ReflectionDistance", "Reflections up to, m", 150.0f, 30.0f, 400.0f, 10.0f, 0 },
    { "ReflectionScale", "Reflection resolution, % of the screen", 70.0f, 30.0f, 100.0f, 5.0f, 0 },
    { "LampGlow", "Lamp glow in the air", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "NightHazeGlow", "City glow in the night haze", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "HazeBrightness", "Haze brightness", 1.0f, 0.3f, 3.0f, 0.05f, 0 },
    { "HeadlightBeams", "Headlight beams in the air", 0.5f, 0.0f, 3.0f, 0.05f, 0 },
    { "GameHeadlights", "The game's headlight light on the road", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "HeadlightShadowSoftness", "Headlight shadow softness (0 = the game's)", 3.0f, 0.0f, 12.0f, 0.5f, 0 },
    { "HeadlightBeamLength", "Headlight beam length in the air, m", 20.0f, 5.0f, 80.0f, 1.0f, 0 },
    { "TrafficLights", "Traffic cars' lights (all)", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "TrafficLightReach", "Traffic headlights: reach, m", 12.0f, 4.0f, 40.0f, 1.0f, 0 },
    { "TrafficHeadBrightness", "Traffic headlights: brightness", 1.0f, 0.0f, 4.0f, 0.05f, 0 },
    { "TrafficHeadWarmth", "Traffic headlights: warmth (0 xenon, 1 halogen)", 0.5f, 0.0f, 1.0f, 0.05f, 0 },
    { "TrafficHeadSaturation", "Traffic headlights: saturation", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "TrafficHeadCone", "Traffic headlights: cone half angle, deg", 57.0f, 15.0f, 85.0f, 1.0f, 0 },
    { "TrafficTailBrightness", "Traffic tail lights: brightness", 1.0f, 0.0f, 4.0f, 0.05f, 0 },
    { "TrafficTailSaturation", "Traffic tail lights: saturation", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "TrafficTailReach", "Traffic tail lights: reach, m", 8.0f, 2.0f, 30.0f, 1.0f, 0 },
    { "TrafficTailCone", "Traffic tail lights: cone half angle, deg", 81.0f, 20.0f, 90.0f, 1.0f, 0 },
    { "AimedCarHeadOut", "Aimed car: headlights apart (+) / together", 0.0f, -1.0f, 1.0f, 0.02f, 0 },
    { "AimedCarHeadUp", "Aimed car: headlights up / down", 0.0f, -1.0f, 1.0f, 0.02f, 0 },
    { "AimedCarHeadForward", "Aimed car: headlights forward / back", 0.0f, -1.5f, 1.5f, 0.02f, 0 },
    { "AimedCarTailOut", "Aimed car: tail lights apart (+) / together", 0.0f, -1.0f, 1.0f, 0.02f, 0 },
    { "AimedCarTailUp", "Aimed car: tail lights up / down", 0.0f, -1.0f, 1.0f, 0.02f, 0 },
    { "AimedCarTailBack", "Aimed car: tail lights back (+) / forward", 0.0f, -1.5f, 1.5f, 0.02f, 0 },
    { "Sky", "Sky: ours (clouds, city glow, stars)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SkyGameClouds", "Sky: more of the game's clouds", 0.4f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyClouds", "Sky: cloud cover", 0.45f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyCloudBrightness", "Sky: cloud brightness", 1.0f, 0.0f, 4.0f, 0.05f, 0 },
    { "SkyCloudScale", "Sky: cloud size (smaller +)", 1.0f, 0.3f, 3.0f, 0.05f, 0 },
    { "SkyCloudSpeed", "Sky: wind", 1.0f, 0.0f, 5.0f, 0.1f, 0 },
    { "SkyCityGlow", "Sky: city glow at night", 1.0f, 0.0f, 4.0f, 0.1f, 0 },
    { "SkyStars", "Sky: stars", 0.5f, 0.0f, 2.0f, 0.05f, 0 },
    { "SkyCloudShadows", "Sky: cloud shadows (overcast at full cover)", 0.85f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyCloudType", "Sky: cloud type (flat > heaped > towering)", 0.5f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyCirrus", "Sky: high feathery clouds", 0.2f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyRainClouds", "Sky: rain clouds (dark bellies)", 0.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "SkyWeather", "Sky: weather", 0.0f, 0.0f, 6.0f, 1.0f, 0 },
    { "SkyQuality", "Sky: cloud quality (steps)", 0.5f, 0.0f, 1.0f, 0.1f, 0 },
    { "SkyCloudRes", "Sky: cloud resolution", 1.0f, 0.0f, 3.0f, 1.0f, 0 },
    { "LookFilmCurve", "Film curve (soft highlights, photo tone)", 0.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "LookRedSaturation", "Red saturation", 1.0f, 0.0f, 1.5f, 0.05f, 0 },
    { "LookHighlightDesat", "Highlights towards white", 0.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "CarPaintReflections", "Car paint reflections (clear coat)", 1.0f, 0.0f, 2.0f, 0.05f, 0 },
    { "GameWetness", "The game's own wet roads / puddles", 1.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "DryGloss", "Dry surfaces: the game's gloss kept (less = matte)", 0.55f, 0.0f, 1.0f, 0.05f, 0 },
    { "Puddles", "Puddles, small mirrors (on wet asphalt)", 0.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "SunShadowMaps", "Sun shadows: our own maps (sharp near the caster, soft far)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SunAngle", "Sun size for shadows (1 = the real sun, 0.53 deg)", 1.0f, 0.0f, 8.0f, 0.25f, 0 },
    { "ShadowResolution", "Shadow map resolution", 1.0f, 0.0f, 2.0f, 1.0f, 0 },
    { "ShadowDistance", "Shadow maps reach, m", 270.0f, 90.0f, 810.0f, 30.0f, 0 },
    { "ShadowSmoothing", "Shadow smoothing over frames", 0.8f, 0.0f, 0.95f, 0.05f, 0 },
    { "ShadowSharpestCm", "Sharpest shadow edge, cm", 0.3f, 0.0f, 5.0f, 0.1f, 0 },
    { "ShadowCasterReach", "Softening counted for casters up to, m", 80.0f, 10.0f, 300.0f, 10.0f, 0 },
    { "ShadowFarBudget", "Kept shadows: most objects a frame while redrawn", 200.0f, 50.0f, 4000.0f, 50.0f, 0 },
    { "Lighting", "Lighting model", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SurfaceReflectance", "Surfaces' own reflectance (1 = 4%, real stone)", 1.0f, 0.0f, 3.0f, 0.1f, 0 },
    { "SkyReflections", "Sky reflected by rough surfaces", 1.0f, 0.0f, 3.0f, 0.1f, 0 },
    { "Roughness", "Surface roughness (x the game's)", 1.0f, 0.3f, 1.5f, 0.05f, 0 },
    { "Relief", "Relief: small shadows and cavities from normal maps", 1.0f, 0.0f, 2.0f, 0.1f, 0 },
    { "ReliefDepth", "Relief depth (x)", 1.0f, 0.2f, 5.0f, 0.1f, 0 },
    { "ReliefReachCm", "Relief reach, cm", 12.0f, 2.0f, 40.0f, 1.0f, 0 },
    { "ReliefWalls", "Relief on walls (x, brick, stone)", 2.3f, 0.0f, 5.0f, 0.1f, 0 },
    { "Parallax", "Parallax: depth on roads and pavements", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "ParallaxDepthCm", "Parallax depth, cm", 3.0f, 0.5f, 15.0f, 0.5f, 0 },
    { "ParallaxFadeM", "Parallax up to, m", 30.0f, 5.0f, 100.0f, 5.0f, 0 },
    { "ParallaxWallsCm", "Parallax on buildings, cm (heights from normal maps)", 1.5f, 0.0f, 10.0f, 0.25f, 0 },
    { "ShadowMaskHalf", "Shadow mask at half resolution (faster; edges kept)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SkipGameShadows", "The game's own shadow maps only past ours (faster)", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SunPath", "Sun and moon", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SunRiseDeg", "Sun / moon: light fades in over its first (deg up)", 8.0f, 0.0f, 20.0f, 1.0f, 0 },
    { "MoonShadows", "Night light", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "ClockShift", "Clock: shift, h (06:00 = the game's sun comes on)", 0.0f, -12.0f, 12.0f, 0.25f, 0 },
    { "SunriseHour", "Sunrise at", 6.0f, 0.0f, 12.0f, 0.25f, 0 },
    { "SunsetHour", "Sunset at", 20.75f, 12.0f, 24.0f, 0.25f, 0 },
    { "SunNoonDeg", "Sun: height at noon, deg", 50.0f, 10.0f, 89.0f, 1.0f, 0 },
    { "SunTurnDeg", "Sun: path turned around the sky, deg", 0.0f, -180.0f, 180.0f, 5.0f, 0 },
    { "SunColour", "Sun: colour", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "MoonLight", "Moon: brightness", 0.25f, 0.0f, 2.0f, 0.05f, 0 },
    { "SunPathDebug", "Debug: the sun's paths on the screen and a sky map", 0.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "NightEV", "Night: brightness of the picture, EV (with LOOK on)", -1.0f, -4.0f, 1.0f, 0.25f, 0 },
    { "SkyNight", "Night: the sky darkens with our sun (not the game's)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "SunLowDim", "Sun: dimmer when low (strength ~ sin(height)^this)", 1.3f, 0.0f, 3.0f, 0.1f, 0 },
    { "HazeDawn", "Haze at sunrise (x the haze)", 2.0f, 0.0f, 5.0f, 0.1f, 0 },
    { "HazeDawnHours", "Haze: the dawn's thins over, h", 2.5f, 0.25f, 8.0f, 0.25f, 0 },
    { "HazeDay", "Haze by day (x the haze)", 0.5f, 0.0f, 3.0f, 0.05f, 0 },
    { "HazeNight", "Haze at night (x the haze)", 0.0f, 0.0f, 3.0f, 0.05f, 0 },
    { "MoonHeightDeg", "Moon: height, deg", 35.0f, 5.0f, 85.0f, 1.0f, 0 },
    { "MoonTurnDeg", "Moon: direction, deg (0 = over the sun's noon)", 0.0f, -180.0f, 180.0f, 5.0f, 0 },
    { "MoonDriftDeg", "Moon: moves over the night, deg", 20.0f, 0.0f, 120.0f, 1.0f, 0 },
    { "LampGroup", "Group", 0.0f, 0.0f, 31.0f, 1.0f, 0 },
    { "LampGroupOn", "  its lamps", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LampGroupHue", "  colour: hue (0 red, 30 orange, 60 yellow, 210 blue)", 30.0f, 0.0f, 360.0f, 5.0f, 0 },
    { "LampGroupSat", "  colour: saturation (0 white)", 0.5f, 0.0f, 1.0f, 0.05f, 0 },
    { "LampGroupPower", "  brightness", 1.0f, 0.0f, 40.0f, 0.05f, 0 },
    { "LampGroupRadius", "  light reaches, m (0 = the Lamps' radius)", 0.0f, 0.0f, 60.0f, 1.0f, 0 },
    { "LampGroupReach", "  lit up to, m from the camera (0 = the Lamps')", 0.0f, 0.0f, 1500.0f, 25.0f, 0 },
    { "LampGroupGlow", "  glow in the air (haze)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "LampsMax", "Lamps a frame, at most (fewer: faster)", 256.0f, 16.0f, 256.0f, 16.0f, 0 },
    { "SsrRay", "Reflections: ray length, m (up walls and towers)", 150.0f, 20.0f, 500.0f, 10.0f, 0 },
    { "SsrSky", "Reflections: our sky where the screen has none", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "WetOther", "Wet ground: pavements, roofs, tiles (x the roads')", 0.3f, 0.0f, 1.0f, 0.05f, 0 },
    { "SsrSmooth", "Noise: reflections smoothed", 1.0f, 0.0f, 2.0f, 1.0f, 0 },
    { "SsrClamp", "Noise: reflections' bright dots cut (x spread; 0 = off)", 2.0f, 0.0f, 6.0f, 0.5f, 0 },
    { "SsrHistory", "Noise: reflections blended over frames (x)", 1.0f, 0.0f, 1.5f, 0.1f, 0 },
    { "SsrJitter", "Noise: reflection rays jittered (1.46: on: the history smooths them)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "WetBumps", "Noise: wet stones' relief kept in reflections", 0.1f, 0.0f, 1.0f, 0.05f, 0 },
    { "SpecAA", "Noise: anti-shimmer (highlights and bumpy reflections)", 1.0f, 0.0f, 4.0f, 0.25f, 0 },
    { "WetLightGloss", "Noise: wet gloss for the game's own lights", 0.7f, 0.0f, 0.95f, 0.05f, 0 },
    { "WetSharp", "Wet ground: reflections' sharpness (1 = a mirror)", 0.8f, 0.4f, 1.0f, 0.05f, 0 },
    { "SsrClip", "Noise: reflections' history kept (x this frame's spread; more: steadier, less: no ghosts)", 1.25f, 0.25f, 4.0f, 0.25f, 0 },
    { "PuddleBig", "Puddles, big and matte (as the game's own): how much", 1.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "PuddleBigSize", "  their size (more: wider)", 0.5f, 0.0f, 1.0f, 0.05f, 0 },
    { "PuddleBigGloss", "  their sharpness (0.95 = a mirror)", 0.6f, 0.3f, 0.95f, 0.05f, 0 },
    { "SsrEarly", "Reflections: of the scene before the game's effects (no headlight halos over cars)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "PuddleBigWater", "  water in them (0 = just damp: darker, a faint sheen; 1 = standing water)", 0.2f, 0.0f, 1.0f, 0.05f, 0 },
    { "PuddleSize", "  their size (more: wider)", 0.5f, 0.0f, 1.0f, 0.05f, 0 },
    { "PuddleGloss", "  their sharpness (0.95 = a mirror)", 0.95f, 0.3f, 0.95f, 0.05f, 0 },
    { "PuddleWater", "  water in them (0 = just damp: darker, a faint sheen; 1 = standing water)", 1.0f, 0.0f, 1.0f, 0.05f, 0 },
    { "ShadowSunStep", "Shadows: far maps drawn again as the sun moves (x the step; less: smoother, costs more)", 0.3f, 0.1f, 1.0f, 0.05f, 0 },
    { "ShadowCityMap", "Shadows: buildings out of the game's view still cast (the city's height map)", 1.0f, 0.0f, 1.0f, 1.0f, 0 },
    { "ShadowOnly", "Shadows (test): only one of our maps everywhere it reaches (1 red near .. 4 yellow far), no help from the others", 0.0f, 0.0f, 4.0f, 1.0f, 0 },
};
enum { P_BOUNCE = 21, P_BOUNCE_R = 22, P_FAST = 23, P_BOUNCE_ALL = 24, P_BOUNCE_HIST = 25, P_LAMP = 26, P_LAMP_THR = 27,
       P_LAMP_REACH = 28, P_LAMP_SRC = 29, P_LAMP_DAY = 30, P_LAMP_AUTO = 31, P_LAMP_POWER = 32, P_KNOCK = 33, P_AUTO_LAMPS = 34, P_LAMP_RADIUS = 35, P_LAMP_SHADOW = 36, P_LAMP_SHADOW_T = 37, P_NIGHT_AMB = 38, P_LAMP_SHADOW_MAPS = 39, P_LAMP_SIZE = 40, P_LAMP_SHADOW_BUDGET = 41, P_LAMP_SHADOW_CUT = 42, P_SOFT_SUN = 43, P_LAMP_RES = 44, P_LOOK = 45, P_LOOK_PRESET = 61, P_SKY_OCC = 62, P_SSR = 64, P_GLOW = 68, P_HL_BEAM = 71, P_HL_GAME = 72, P_HL_SOFT = 73, P_HL_LEN = 74, P_TRAFFIC = 75, P_TR_HEAD = 77, P_TR_TAIL = 81, P_TR_FIX = 85, P_SKY = 91, P_LOOK_X = 106, P_SS = 113, P_PBR = 121, P_REL = 125, P_POM = 129, P_SS_HALF = 133, P_SS_SKIP = 134, P_SUN_PATH = 135, P_SUN_FADE = 136, P_MOON_SHADOWS = 137,
       P_CLOCK = 138, P_SUNRISE = 139, P_SUNSET = 140, P_SUN_NOON = 141, P_SUN_TURN = 142, P_SUN_COL = 143, P_MOON_LIGHT = 144,
       P_SUN_DEBUG = 145, P_NIGHT_EV = 146, P_SKY_NIGHT = 147, P_SUN_LOW = 148,
       P_HAZE_DAWN = 149, P_HAZE_DAWN_H = 150, P_HAZE_DAY = 151, P_HAZE_NIGHT = 152, P_MOON_EL = 153, P_MOON_AZ = 154, P_MOON_DRIFT = 155,
       P_LG = 156, P_LAMP_MAX = 164, P_SSR_RAY = 165, P_SSR_SKY = 166, P_WET_OTHER = 167,
       P_NOISE = 168,     // 1.43: 168 smoothing, 169 dots cut, 170 history, 171 jitter, 172 bumps, 173 spec AA, 174 wet light gloss; 175 wet sharpness; 1.46: 176 history clip
       P_PUDDLE_BIG = 177, P_SSR_EARLY = 180 };   // 1.47: 177 big matte puddles, 178 their size, 179 their gloss; 180 the reflections' source before the effects; 1.48: 181 their water; 1.49: 182..184 the small ones' size, gloss, water
static void LookApplyPreset(int k);
static const int kParams = sizeof(g_p) / sizeof(g_p[0]);
static const char* kViews[] = { "normal", "split: left = original", "contact shadow mask", "AO mask",
                                "new shadow", "original shadow", "check: red = estimate error",
                                "game sun light x4", "coverage (magenta = processed)", "", "", "", "", "", "", "",
                                "distance to caster (white = 10 m)", "penumbra width (ours: white = 50 cm)", "shadow cascades (red near, green, blue, yellow far)",
                                "haze amount", "light shafts (white = air in sun)", "bounce + lamp light x4",
                                "bounce source: green = last frame, red = sun only", "lamp light alone x4",
                                "lamp editor: click a lamp, drag its arrows",
                                "gloss (the game's G-buffer)", "specular colour (G-buffer)",
                                "rain: blue wet, white puddles, red dry under a roof",
                                "reflections: only what they add (x2)", "reflections: source (red wet, green dry gloss, blue paint)",
                                "reflections: what the ray found", "wetness (white) and puddles (blue)", "reflections: the normal used",
                                "sun shadow: this frame alone (before smoothing)",
                                "modern lighting: the sky reflected (x4)", "modern lighting: roughness (white = rough)",
                                "relief: grey = cavities, red = small sun shadows" };
static const int kViewCount = 37;
// views that show the game's own image need the copy of the frame
static bool ViewNeedsCopy(int v) { return v == 6 || v == 8 || v == 9 || v == 13; }
static volatile LONG g_wantDump;  // log the next main sun pass in detail

static bool g_enabled = true, g_overlay = true, g_halfRes = true;
static int g_view = 0, g_sel = 0;

// the colour / group of the last lamp set up by hand: new lamps start with it
struct NewLamp { int valid, group; float c[3]; };
static NewLamp g_newLamp;
struct LampClip { int valid, group, channel; float c[3]; };   // Ctrl+C / Ctrl+V in the lamp editor
static LampClip g_clip;

static void LoadIni()
{
    char b[64];
    GetPrivateProfileStringA("NewLook", "NewLamp", "", b, sizeof(b), g_iniPath);
    g_newLamp.valid = sscanf(b, "%f %f %f %d", &g_newLamp.c[0], &g_newLamp.c[1], &g_newLamp.c[2], &g_newLamp.group) == 4;
    g_enabled = GetPrivateProfileIntA("NewLook", "Enabled", 1, g_iniPath) != 0;
    g_overlay = GetPrivateProfileIntA("NewLook", "ShowOverlay", 1, g_iniPath) != 0;
    g_halfRes = GetPrivateProfileIntA("NewLook", "HalfResEffects", 1, g_iniPath) != 0;
    g_view = GetPrivateProfileIntA("NewLook", "View", 0, g_iniPath);
    if (g_view < 0 || g_view >= kViewCount) g_view = 0;
    for (int i = 0; i < kParams; ++i) {
        char def[32];
        sprintf(def, "%g", g_p[i].def);
        GetPrivateProfileStringA("NewLook", g_p[i].key, def, b, sizeof(b), g_iniPath);
        float v = (float)atof(b);
        g_p[i].v = v < g_p[i].mn ? g_p[i].mn : v > g_p[i].mx ? g_p[i].mx : v;
    }
}

// "custom" in a preset choice (Look preset, sky weather) keeps what you set: going to a preset saves your values,
// coming back to custom brings them back; changing one of them under a preset turns the choice to custom.
struct PresetMemory { int preset; int members[24]; int n; float saved[24]; bool has; const char* key; };
static PresetMemory g_presetMem[2] = {
    { 61, { 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 106, 107, 108 }, 18, {}, false, "LookCustom" },
    { 103, { 93, 100, 101, 102, 99 }, 5, {}, false, "SkyCustom" },
};
static void PresetMemSave(PresetMemory& m) { for (int i = 0; i < m.n; ++i) m.saved[i] = g_p[m.members[i]].v; m.has = true; }
static void PresetMemLoad() {
    for (int s = 0; s < 2; ++s) {
        PresetMemory& m = g_presetMem[s];
        char b[512];
        GetPrivateProfileStringA("NewLook", m.key, "", b, sizeof(b), g_iniPath);
        char* p = b;
        int i = 0;
        for (; i < m.n; ++i) { char* e; float v = strtof(p, &e); if (e == p) break; m.saved[i] = v; p = e; }
        m.has = i == m.n;
    }
}
// called after a parameter changed by a key; before: its value before the change
static const char* PresetMemChanged(int idx, float before)   // a message, or 0
{
    const char* msg = 0;
    for (int s = 0; s < 2; ++s) {
        PresetMemory& m = g_presetMem[s];
        if (idx == m.preset) {
            int now = (int)(g_p[idx].v + 0.5f), was = (int)(before + 0.5f);
            if (now == was) continue;
            if (was == 0) PresetMemSave(m);   // leaving custom: keep it
            if (now == 0 && m.has) { for (int i = 0; i < m.n; ++i) g_p[m.members[i]].v = m.saved[i]; msg = "custom: your settings back"; }
            continue;
        }
        for (int i = 0; i < m.n; ++i)
            if (m.members[i] == idx && g_p[m.preset].v > 0.5f) { g_p[m.preset].v = 0; PresetMemSave(m); msg = "changed by hand: custom"; }
    }
    return msg;
}

static void SaveIni()
{
    char b[64];
    WritePrivateProfileStringA("NewLook", "Enabled", g_enabled ? "1" : "0", g_iniPath);
    WritePrivateProfileStringA("NewLook", "ShowOverlay", g_overlay ? "1" : "0", g_iniPath);
    sprintf(b, "%d", g_view);
    WritePrivateProfileStringA("NewLook", "View", b, g_iniPath);
    for (int i = 0; i < kParams; ++i) {
        sprintf(b, "%.3f", g_p[i].v);
        WritePrivateProfileStringA("NewLook", g_p[i].key, b, g_iniPath);
    }
    for (int s = 0; s < 2; ++s) {
        PresetMemory& m = g_presetMem[s];
        if (g_p[m.preset].v < 0.5f) PresetMemSave(m);   // custom now: these are its values
        if (!m.has) continue;
        char lb[512] = "";
        for (int i = 0; i < m.n; ++i) { char t[24]; sprintf(t, "%s%.3f", i ? " " : "", m.saved[i]); strcat(lb, t); }
        WritePrivateProfileStringA("NewLook", m.key, lb, g_iniPath);
    }
    if (g_newLamp.valid) {
        sprintf(b, "%.4f %.4f %.4f %d", g_newLamp.c[0], g_newLamp.c[1], g_newLamp.c[2], g_newLamp.group);
        WritePrivateProfileStringA("NewLook", "NewLamp", b, g_iniPath);
    }
    Log("settings saved");
}

// ---------------------------------------------------------------- d3dcompiler

typedef HRESULT(WINAPI* D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI* D3DReflectFn)(LPCVOID, SIZE_T, REFIID, void**);
static D3DCompileFn g_compile;
static D3DReflectFn g_reflect;

// 1.59: compiled shaders kept on disk (scripts\NewLook_cache\<key>.cso, the key: the source, entry, target, defines,
// flags): the first switch-on compiled the 28 shaders of the 150 KB source one after another (~25 s, the game stood
// still); now they come from the cache, are compiled in the background from the game's start, several at once
typedef HRESULT(WINAPI* D3DCreateBlobFn)(SIZE_T, ID3DBlob**);
static D3DCompileFn g_compileReal;
static D3DCreateBlobFn g_createBlob;
static char g_cacheDir[MAX_PATH];
static volatile LONG g_cacheHits, g_cacheMisses;
static unsigned long long CacheFnv(unsigned long long h, const void* p, size_t n)
{
    const unsigned char* b = (const unsigned char*)p;
    for (size_t k = 0; k < n; ++k) h = (h ^ b[k]) * 1099511628211ull;
    return h;
}
static HRESULT WINAPI CachedCompile(LPCVOID src, SIZE_T n, LPCSTR name, const D3D_SHADER_MACRO* mac, ID3DInclude* inc,
                                    LPCSTR entry, LPCSTR target, UINT f1, UINT f2, ID3DBlob** code, ID3DBlob** err)
{
    if (inc || !code || !g_createBlob || !g_cacheDir[0] || !entry || !target)
        return g_compileReal(src, n, name, mac, inc, entry, target, f1, f2, code, err);
    unsigned long long h = 14695981039346656037ull;
    h = CacheFnv(h, src, n);
    h = CacheFnv(h, entry, strlen(entry) + 1);
    h = CacheFnv(h, target, strlen(target) + 1);
    for (const D3D_SHADER_MACRO* m = mac; m && m->Name; ++m) {
        h = CacheFnv(h, m->Name, strlen(m->Name) + 1);
        if (m->Definition) h = CacheFnv(h, m->Definition, strlen(m->Definition) + 1);
    }
    UINT fl[2] = { f1, f2 };
    h = CacheFnv(h, fl, sizeof(fl));
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH - 1, "%s%016llx.cso", g_cacheDir, h);
    path[MAX_PATH - 1] = 0;
    if (FILE* f = fopen(path, "rb")) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        ID3DBlob* b = 0;
        bool ok = sz > 32 && SUCCEEDED(g_createBlob(sz, &b)) && fread(b->GetBufferPointer(), 1, sz, f) == (size_t)sz
                  && !memcmp(b->GetBufferPointer(), "DXBC", 4);
        fclose(f);
        if (ok) { *code = b; if (err) *err = 0; InterlockedIncrement(&g_cacheHits); return S_OK; }
        if (b) b->Release();
    }
    InterlockedIncrement(&g_cacheMisses);
    HRESULT hr = g_compileReal(src, n, name, mac, inc, entry, target, f1, f2, code, err);
    if (SUCCEEDED(hr) && *code) {
        char tmp[MAX_PATH];
        _snprintf(tmp, MAX_PATH - 1, "%s.%lu.tmp", path, GetCurrentThreadId());
        tmp[MAX_PATH - 1] = 0;
        if (FILE* f = fopen(tmp, "wb")) {
            bool ok = fwrite((*code)->GetBufferPointer(), 1, (*code)->GetBufferSize(), f) == (*code)->GetBufferSize();
            ok = fclose(f) == 0 && ok;
            if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(tmp);
        }
    }
    return hr;
}
// the cache emptied when it grows past 600 files (old sources' shaders pile up there)
static void CacheTidy()
{
    char pat[MAX_PATH];
    sprintf(pat, "%s*.cso", g_cacheDir);
    WIN32_FIND_DATAA fd;
    int n = 0;
    HANDLE fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do ++n; while (FindNextFileA(fh, &fd));
    FindClose(fh);
    if (n <= 600) return;
    fh = FindFirstFileA(pat, &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do { char p[MAX_PATH]; sprintf(p, "%s%s", g_cacheDir, fd.cFileName); DeleteFileA(p); } while (FindNextFileA(fh, &fd));
    FindClose(fh);
    Log("shader cache: %d files, emptied", n);
}

static void StartShaderPrewarm();
static bool LoadCompiler()
{
    HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
    if (!m) { Log("d3dcompiler_47.dll not found"); return false; }
    g_compileReal = (D3DCompileFn)GetProcAddress(m, "D3DCompile");
    g_reflect = (D3DReflectFn)GetProcAddress(m, "D3DReflect");
    g_createBlob = (D3DCreateBlobFn)GetProcAddress(m, "D3DCreateBlob");
    if (!g_compileReal || !g_reflect) return false;
    sprintf(g_cacheDir, "%sNewLook_cache\\", g_dir);
    if (!CreateDirectoryA(g_cacheDir, 0) && GetLastError() != ERROR_ALREADY_EXISTS) g_cacheDir[0] = 0;
    if (g_cacheDir[0]) CacheTidy();
    g_compile = CachedCompile;
    StartShaderPrewarm();
    return true;
}

// ---------------------------------------------------------------- sun-pass shaders

struct SunInfo {
    ID3D11PixelShader* ps;
    int tDepth, tNormal, tDiffuse, tSpec, tAdd, tShadow, tCloud, tVfx, sCmp, tIrr;
    bool lambert, ashikhmin;  // which diffuse models the variant contains
    int oKeyCol, oKeyDir, oC, oC2, oArr4, oScale, oW2L, oView, oCamDir, oMask;
    bool writesDepth;
};
static SunInfo g_sun[128];
static volatile LONG g_sunCount;
static char g_lastReject[160];

static int BindSlot(ID3D11ShaderReflection* r, const char* name, D3D_SHADER_INPUT_TYPE type)
{
    // the game names a texture and its sampler the same, so match name and type
    D3D11_SHADER_DESC sd;
    if (FAILED(r->GetDesc(&sd))) return -1;
    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd;
        if (SUCCEEDED(r->GetResourceBindingDesc(i, &bd)) && bd.Type == type && strcmp(bd.Name, name) == 0) return (int)bd.BindPoint;
    }
    return -1;
}

static int VarIndex(ID3D11ShaderReflection* r, const char* cb, const char* name, int extra = 0)
{
    ID3D11ShaderReflectionConstantBuffer* c = r->GetConstantBufferByName(cb);
    if (!c) return -1;
    ID3D11ShaderReflectionVariable* v = c->GetVariableByName(name);
    D3D11_SHADER_VARIABLE_DESC vd;
    if (!v || FAILED(v->GetDesc(&vd))) return -1;
    if (vd.StartOffset % 16) return -2;
    return (int)(vd.StartOffset / 16) + extra;
}

static bool HasBytes(const void* p, SIZE_T n, const char* s)
{
    size_t l = strlen(s);
    const char* b = (const char*)p;
    for (SIZE_T i = 0; i + l <= n; ++i)
        if (b[i] == s[0] && memcmp(b + i, s, l) == 0) return true;
    return false;
}

static void ExamineShader(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    if (!HasBytes(code, size, "deferred_normalBufferSampler") || !HasBytes(code, size, "KeyLightDirection")) return;
    if (HasBytes(code, size, "Thermal")) return;          // thermal-vision variant
    if (HasBytes(code, size, "LightRigColours")) return;  // front-end / garage light rig
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    SunInfo s;
    memset(&s, 0, sizeof(s));
    s.ps = ps;
    s.tDepth = BindSlot(r, "deferred_depthBufferSampler", D3D_SIT_TEXTURE);
    s.tNormal = BindSlot(r, "deferred_normalBufferSampler", D3D_SIT_TEXTURE);
    s.tDiffuse = BindSlot(r, "DiffuseSampler", D3D_SIT_TEXTURE);
    s.tSpec = BindSlot(r, "SpecularSampler", D3D_SIT_TEXTURE);
    s.tAdd = BindSlot(r, "AdditiveSampler", D3D_SIT_TEXTURE);
    s.tShadow = BindSlot(r, "shadowMap0", D3D_SIT_TEXTURE);
    s.tCloud = BindSlot(r, "KeylightProjectedShadowMaskSampler", D3D_SIT_TEXTURE);
    s.tVfx = BindSlot(r, "shadowMapVfx", D3D_SIT_TEXTURE);
    s.sCmp = BindSlot(r, "shadowMapSamplerHighDetail", D3D_SIT_SAMPLER);
    s.tIrr = BindSlot(r, "cubeIrradianceSampler", D3D_SIT_TEXTURE);   // the sky-light cube (modern lighting reflects it)
    // the two diffuse models leave their constants (1/pi and 28/(23 pi)) in the bytecode
    for (SIZE_T k = 0; k + 4 <= size; k += 4) {
        float f;
        memcpy(&f, (const char*)code + k, 4);
        if (fabsf(f - 0.3183099f) < 2e-6f) s.lambert = true;
        if (fabsf(f - 0.3875081f) < 2e-6f) s.ashikhmin = true;
    }
    int cbG = BindSlot(r, "gGlobalCB", D3D_SIT_CBUFFER);
    int cbO = BindSlot(r, "gObjectCB", D3D_SIT_CBUFFER);
    s.oKeyCol = VarIndex(r, "gGlobalCB", "KeyLightColour");
    s.oKeyDir = VarIndex(r, "gGlobalCB", "KeyLightDirection");
    s.oC = VarIndex(r, "gGlobalCB", "ShadowMap_Constants");
    s.oC2 = VarIndex(r, "gGlobalCB", "ShadowMap_Constants2");
    s.oArr4 = VarIndex(r, "gGlobalCB", "ShadowMap_ConstantsArray", 4);
    s.oScale = VarIndex(r, "gGlobalCB", "ShadowMap_ScaleOffset");
    s.oW2L = VarIndex(r, "gGlobalCB", "ShadowMap_WorldToLight0");
    s.oView = VarIndex(r, "gGlobalCB", "ViewPosition");
    s.oCamDir = VarIndex(r, "gGlobalCB", "deferred_cameraDirection");
    s.oMask = cbO == 1 ? VarIndex(r, "gObjectCB", "KeylightProjectedShadowMaskSettings") : -1;
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT i = 0; i < sd.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC pd;
        if (SUCCEEDED(r->GetOutputParameterDesc(i, &pd)) && pd.SystemValueType == D3D_NAME_DEPTH) s.writesDepth = true;
    }
    r->Release();

    const char* why = 0;
    if (cbG != 0) why = "gGlobalCB not in b0";
    else if (s.tDepth < 0 || s.tNormal < 0 || s.tDiffuse < 0) why = "missing G-buffer inputs";
    else if (s.oKeyCol < 0 || s.oKeyDir < 0 || s.oView < 0) why = "missing light / camera constants";
    else if (s.tShadow >= 0 && (s.sCmp < 0 || s.oC < 0 || s.oC2 < 0 || s.oScale < 0 || s.oW2L < 0 || s.oCamDir < 0))
        why = "single-cascade variant (reflections), left alone";
    if (!why && !s.lambert && !s.ashikhmin) why = "unknown diffuse model (debug variant), left alone";
    if (why) {
        _snprintf(g_lastReject, sizeof(g_lastReject) - 1, "%s", why);
        Log("shader %p looks like a sun pass but is skipped: %s", ps, why);
        return;
    }
    if (s.oArr4 < -1) s.oArr4 = -1;
    LONG n = g_sunCount;
    if (n >= 128) return;
    g_sun[n] = s;
    InterlockedIncrement(&g_sunCount);
    Log("sun-pass shader %p #%ld: shadow %s (%s), spec %d, additive %d, cloud mask %d, depth out %d", ps, n + 1,
        s.tShadow >= 0 ? "yes" : "no", s.oArr4 >= 0 ? "5 taps" : "1 tap", s.tSpec >= 0, s.tAdd >= 0, s.tCloud >= 0 && s.oMask >= 0,
        s.writesDepth);
}

// Short description of every pixel shader (first bound textures / constants), for the frame trace.
struct PsName { ID3D11PixelShader* ps; char name[96]; };
static ID3D11PixelShader* volatile g_postPS[16];
static ID3D11PixelShader* volatile g_tonePS[8];   // the game's shaders that read the 1x1 HDRToneMapping texture (exposure)
static ID3D11Texture2D *g_expoTex, *g_expoStage;   // our copy of it (t56: the debug views divide by its w), a readback
static ID3D11ShaderResourceView* g_expoSrv;
static UINT g_expoFrame;
static PsName g_psNames[4096];
static volatile LONG g_psNameCount;

static void NoteFinalPS(ID3D11PixelShader* ps, const char* names);
static bool HlReady();
static int g_carN;   // traffic cars followed (carlights.inc)
static void CarsRead(ID3D11DeviceContext* ctx);
static int CarLights(int c, float (*pos)[4], float (*col)[4], float (*dir)[4], float (*lo)[4]);   // lo w: the cone's cosine
static int CarMatCount();
static ID3D11PixelShader* HlSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);
static void HlBind(ID3D11DeviceContext* ctx, bool on);
static ID3D11ShaderResourceView* g_hlShadow;
struct LightPS { ID3D11PixelShader* ps; int kind, var, shadow; };   // var: 0 = position from gGlobalCB, 1 = from the view ray
static LightPS g_lightPS[64];
static volatile LONG g_lightPSN;
static int LightKindOf(ID3D11PixelShader* ps)
{
    LONG n = g_lightPSN;
    for (LONG i = 0; i < n; ++i) if (g_lightPS[i].ps == ps) return g_lightPS[i].kind;
    return 0;
}
static void SkyNote(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name);
static void WetNote(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name);
static void PuddleNote(const void* code, SIZE_T size, ID3D11PixelShader* ps);
static void SkyNoteGame(const void* code, SIZE_T size, ID3D11PixelShader* ps, const char* name);
static ID3D11PixelShader* SkySubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);
static void SkyBindForSun(ID3D11DeviceContext* ctx);
static void RainBind(ID3D11DeviceContext* ctx);   // rain.inc
static void SkyAfterDraw(ID3D11DeviceContext* ctx);
static void SkyApplyWeather(int k);
struct DrawCall;
static void SkyBeforeDraw(ID3D11DeviceContext* ctx, const DrawCall& d);
static bool g_skyPending;
static bool g_skyDrawLog;
static void NameShader(const void* code, SIZE_T size, ID3D11PixelShader* ps)
{
    LONG n = g_psNameCount;
    if (n >= 4096 || !g_reflect) return;
    ID3D11ShaderReflection* r = 0;
    if (FAILED(g_reflect(code, size, __uuidof(ID3D11ShaderReflection), (void**)&r)) || !r) return;
    char buf[96] = "";
    D3D11_SHADER_DESC sd;
    r->GetDesc(&sd);
    for (UINT i = 0; i < sd.BoundResources && strlen(buf) < 70; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bd;
        if (SUCCEEDED(r->GetResourceBindingDesc(i, &bd)) && bd.Type == D3D_SIT_TEXTURE) {
            strncat(buf, bd.Name, 24);
            strcat(buf, ",");
        }
    }
    if (sd.ConstantBuffers) {
        ID3D11ShaderReflectionConstantBuffer* c = r->GetConstantBufferByIndex(0);
        D3D11_SHADER_BUFFER_DESC cd;
        if (c && SUCCEEDED(c->GetDesc(&cd)) && cd.Variables) {
            D3D11_SHADER_VARIABLE_DESC vd;
            if (SUCCEEDED(c->GetVariableByIndex(0)->GetDesc(&vd)) && strlen(buf) < 80) { strcat(buf, "|"); strncat(buf, vd.Name, 14); }
        }
    }
    {   // the game's light shaders
        int kind = 0;
        for (UINT c = 0; c < sd.ConstantBuffers && !kind; ++c) {
            ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByIndex(c);
            D3D11_SHADER_BUFFER_DESC cbd;
            if (!cb || FAILED(cb->GetDesc(&cbd))) continue;
            for (UINT v = 0; v < cbd.Variables; ++v) {
                D3D11_SHADER_VARIABLE_DESC vd;
                if (FAILED(cb->GetVariableByIndex(v)->GetDesc(&vd))) continue;
                if (!strcmp(vd.Name, "deferred_lightPositions")) kind = 1;
                else if (!strcmp(vd.Name, "deferred_lightProjectionMatrix")) kind = 2;
            }
        }
        if (kind) {
            int var = 1, shadow = 0;
            D3D11_SHADER_INPUT_BIND_DESC ib;
            if (SUCCEEDED(r->GetResourceBindingDescByName("gGlobalCB", &ib))) var = 0;
            if (SUCCEEDED(r->GetResourceBindingDescByName("shadowMap0", &ib))) shadow = 1;
            LONG k = InterlockedIncrement(&g_lightPSN) - 1;
            if (k < 64) { g_lightPS[k].ps = ps; g_lightPS[k].kind = kind; g_lightPS[k].var = var; g_lightPS[k].shadow = shadow; } else g_lightPSN = 64;
        }
    }
    r->Release();
    g_psNames[n].ps = ps;
    memcpy(g_psNames[n].name, buf, sizeof(buf));
    SkyNote(code, size, ps, buf);   // sky research (skyrecon.inc)
    WetNote(code, size, ps, buf);   // the game's wetness (skyrecon.inc)
    PuddleNote(code, size, ps);     // its puddle maps (gamewet.inc)
    SkyNoteGame(code, size, ps, buf);   // the sky we replace (sky.inc)
    // the first post-processing passes (motion blur, else the HDR downsample): the air goes in before them
    if (strstr(buf, "HDRToneMapping"))
        for (int k = 0; k < 8; ++k) if (!g_tonePS[k]) { g_tonePS[k] = ps; break; }
    if (strstr(buf, "MotionBlurPixe") || strstr(buf, "HDRToneMapping")) {
        for (int k = 0; k < 16; ++k) if (!g_postPS[k]) { g_postPS[k] = ps; Log("post-processing shader %p: %s", ps, buf); break; }
    }
    NoteFinalPS(ps, buf);
    InterlockedIncrement(&g_psNameCount);
}

static const char* PsNameOf(ID3D11PixelShader* ps)
{
    LONG n = g_psNameCount;
    for (LONG i = 0; i < n; ++i) if (g_psNames[i].ps == ps) return g_psNames[i].name;
    return "?";
}

static SunInfo* FindSun(ID3D11PixelShader* ps)
{
    if (!ps) return 0;
    LONG n = g_sunCount;
    for (LONG i = 0; i < n; ++i)
        if (g_sun[i].ps == ps) return &g_sun[i];
    return 0;
}

// ---------------------------------------------------------------- hooks

typedef HRESULT(STDMETHODCALLTYPE* CreatePSFn)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
typedef void(STDMETHODCALLTYPE* PSSetShaderFn)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
typedef void(STDMETHODCALLTYPE* DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void(STDMETHODCALLTYPE* DrawIndexedFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void(STDMETHODCALLTYPE* DrawIndexedInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* DrawInstancedFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
static CreatePSFn oCreatePS;
static PSSetShaderFn oPSSetShader;
static DrawFn oDraw;
static DrawIndexedFn oDrawIndexed;
static DrawIndexedInstancedFn oDrawIndexedInstanced;
static DrawInstancedFn oDrawInstanced;
static PresentFn oPresent;

// sun-pass shader currently bound, per context (the game may record on deferred contexts)
struct CtxState { ID3D11DeviceContext* ctx; SunInfo* sun; };
static CtxState g_ctxState[16];
static ID3D11PixelShader* g_lastPS;   // last pixel shader on the immediate context (frame trace)
static CRITICAL_SECTION g_ctxLock;
static volatile LONG g_sunBindsImm, g_sunBindsDef, g_sunDrawsImm, g_sunDrawsDef, g_psCalls;
static volatile LONG g_drawCnt[8];   // Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawAuto, IndexedIndirect, InstancedIndirect, ExecuteCommandList
static volatile LONG g_traceLeft = 40;  // calls to log right after the first sun-shader binds
static void Trace(const char* what, ID3D11DeviceContext* ctx, const void* p)
{
    if (g_traceLeft <= 0) return;
    InterlockedDecrement(&g_traceLeft);
    Log("trace: %s ctx %p %p", what, ctx, p);
}
static volatile LONG g_tracing;

static SunInfo** CurSlot(ID3D11DeviceContext* ctx)
{
    for (int i = 0; i < 16; ++i) if (g_ctxState[i].ctx == ctx) return &g_ctxState[i].sun;
    EnterCriticalSection(&g_ctxLock);
    SunInfo** r = 0;
    for (int i = 0; i < 16 && !r; ++i) if (g_ctxState[i].ctx == ctx) r = &g_ctxState[i].sun;
    for (int i = 0; i < 16 && !r; ++i) if (!g_ctxState[i].ctx) { g_ctxState[i].ctx = ctx; r = &g_ctxState[i].sun; }
    LeaveCriticalSection(&g_ctxLock);
    static SunInfo* dummy;
    return r ? r : &dummy;
}

// our resources
static ID3D11Device* g_dev;
static ID3D11PixelShader* g_psSun[4];     // [0] copy, [1] copy + SV_Depth, [2] blended, [3] blended + SV_Depth
static ID3D11PixelShader* g_psHalf;       // half res: AO, shafts, contact, bounce light
static ID3D11PixelShader* g_psGIBlur;     // half res: bounce light blur + history
static ID3D11PixelShader* g_psSnap;       // half res copy of the finished scene (next frame's bounce source)
static ID3D11PixelShader* g_psCam;        // 4x2 record of the view ray (reprojection)
static ID3D11PixelShader* g_psEmit;       // lamp finder (one pixel per 32x32 tile)
static ID3D11PixelShader* g_psEditor;     // lamp editor overlay (view 24)
static ID3D11PixelShader* g_psAir;        // haze / glow / shafts after the transparents
static ID3D11VertexShader* g_vsText;
static ID3D11PixelShader* g_psText;
static ID3D11Buffer* g_cb;
static ID3D11Buffer* g_cbText;
static ID3D11SamplerState *g_sPoint, *g_sWrap, *g_sText;
static ID3D11BlendState* g_blendAlpha;
static ID3D11BlendState* g_blendDual;     // result = src0 + dest * src1 (sun pass without a copy)
static ID3D11BlendState* g_blendAir;      // result = src.rgb + dest * src.a
static ID3D11RasterizerState* g_rsNoCull;
static ID3D11Texture2D* g_copy;
static ID3D11ShaderResourceView* g_copySrv;
static D3D11_TEXTURE2D_DESC g_copyDesc;
// half-res targets: [0] ao / shafts / contact / distance, [1] bounce light, [2] [3] bounce light blurred
// (this frame / last frame, swapped every frame), [4] the finished scene of the last frame (with mips: average).
// Camera records (4x2): this frame / last frame.
struct HalfRT { ID3D11Texture2D* t; ID3D11RenderTargetView* rtv; ID3D11ShaderResourceView* srv; };
static const int kHalfRT = 11;   // 1.49: [9] [10]: the AO over frames, this frame / last frame (swapped)   // [7] [8]: the smoothed reflections, this frame / last frame (swapped)   // [5]: the AO target smoothed (sky occlusion), then the reflections; [6] the whole scene (mips)
static HalfRT g_hrt[kHalfRT], g_camRec[2];
static HalfRT g_lampLT, g_lampGT, g_lampBT;          // lamp light at the shown resolution (t35), the lamps' glow in the air (t41)
static UINT g_lampLW, g_lampLH;
static unsigned g_lampLFrame;              // frame + 1 it was made
static ID3D11PixelShader* g_psLamps;
static ID3D11PixelShader* g_psGrade;
static ID3D11PixelShader* g_psABlur;
static ID3D11PixelShader* g_psSSR;
static ID3D11PixelShader* g_psCity;                // 1.54: the city's sun shadow from the rain map (sunshadow.inc)
static ID3D11PixelShader* g_psPano;                // 1.39: our sky as seen, into a panorama (the reflections' sky)
static ID3D11Texture2D* g_panoTex[2];
static ID3D11RenderTargetView* g_panoRtv[2];
static ID3D11ShaderResourceView* g_panoSrv[2];
static bool g_panoCleared[2];
static int g_panoLast;
static ID3D11Texture2D* g_panoStage;   // (its smallest level read back now and then: how much is seen, its mean colour)
static unsigned g_panoStageFrame;
static ID3D11ShaderResourceView* g_probeSrv;   // 1.46: the sky probe (sky.inc): our sky all around, the reflections' (t68)
static ID3D11Texture2D* g_earlyTex;            // 1.47: the scene before the game's effects (the reflections' source, t70)
static ID3D11ShaderResourceView* g_earlySrv;
static unsigned g_earlyArm, g_earlyFrame;      // frame + 1: our sun pass ran / the copy was made
static unsigned g_probeFrame;                  // frame + 1 it was last drawn into
static float HalfToFloat(unsigned short h)
{
    unsigned e = (h >> 10) & 31, f = h & 1023;
    float v = e == 0 ? f / 1024.0f / 16384.0f : e == 31 ? 65504.0f : (1 + f / 1024.0f) * powf(2.0f, (float)e - 15);
    return h & 0x8000 ? -v : v;
}
static ID3D11PixelShader* g_psSSRApply;
static ID3D11PixelShader* g_psSSRBlur;
static ID3D11PixelShader *g_psSunMask, *g_psSunTemporal, *g_psSunUp;   // our sun shadows' mask (sunshadow.inc)
static const int kLampsUp = 256;          // lamps sent a frame (shader buffer t33)
static ID3D11Buffer* g_lampBuf;
static ID3D11ShaderResourceView* g_lampSrv;
static HalfRT g_tileRT[2];                // which lamps light which screen tile (256 bits: t31, t32)
static UINT g_tileW, g_tileH, g_tileFor[2];
static unsigned g_tileFrame;              // frame + 1 the tiles were made
static ID3D11PixelShader* g_psTiles;
static UINT g_halfW, g_halfH;
static unsigned g_giFrame[2], g_camFrame[2], g_radFrame, g_ssrFrame[2], g_aoFrame[2];
static HalfRT g_ssrRT[3];                  // reflections: raw, smoothed this frame / last frame (swapped)
static HalfRT g_ssrAux;                    // 1.46: the raw ones' gloss and ray length (R16G16F; the gloss on w overflowed FP16)
static UINT g_ssrW, g_ssrH;
   // frame each one was written (+1, 0 = never)

static bool MakeRT(HalfRT& r, UINT w, UINT h, DXGI_FORMAT f, bool mips)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = mips ? 0 : 1; d.ArraySize = 1; d.Format = f;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    return SUCCEEDED(g_dev->CreateTexture2D(&d, 0, &r.t)) && SUCCEEDED(g_dev->CreateRenderTargetView(r.t, 0, &r.rtv)) &&
           SUCCEEDED(g_dev->CreateShaderResourceView(r.t, 0, &r.srv));
}

static void FreeRT(HalfRT& r)
{
    if (r.srv) r.srv->Release();
    if (r.rtv) r.rtv->Release();
    if (r.t) r.t->Release();
    memset(&r, 0, sizeof(r));
}

static bool EnsureLampLT(UINT w, UINT h)
{
    if (g_lampBT.srv && g_lampLW == w && g_lampLH == h) return true;
    static bool failed;
    if (failed) return false;
    FreeRT(g_lampLT);
    FreeRT(g_lampGT);
    FreeRT(g_lampBT);
    if (!MakeRT(g_lampLT, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false) || !MakeRT(g_lampGT, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false) ||
        !MakeRT(g_lampBT, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) {
        Log("lamp light target failed"); failed = true; return false;
    }
    g_lampLW = w; g_lampLH = h;
    Log("lamp light target %ux%u", w, h);
    return true;
}

static bool EnsureTiles(UINT rtW, UINT rtH)
{
    if (g_tileRT[1].srv && g_tileFor[0] == rtW && g_tileFor[1] == rtH) return true;
    static bool failed;
    if (failed) return false;
    for (int k = 0; k < 2; ++k) FreeRT(g_tileRT[k]);
    UINT tw = (rtW + 31) / 32, th = (rtH + 31) / 32;
    for (int k = 0; k < 2; ++k)
        if (!MakeRT(g_tileRT[k], tw, th, DXGI_FORMAT_R32G32B32A32_UINT, false)) { Log("lamp tiles failed"); failed = true; return false; }
    g_tileW = tw; g_tileH = th; g_tileFor[0] = rtW; g_tileFor[1] = rtH;
    Log("lamp tiles %ux%u", tw, th);
    return true;
}

static bool EnsureSsrRT(UINT w, UINT h)
{
    if (g_ssrRT[2].srv && g_ssrAux.srv && g_ssrW == w && g_ssrH == h) return true;
    for (int k = 0; k < 3; ++k) FreeRT(g_ssrRT[k]);
    FreeRT(g_ssrAux);
    g_ssrFrame[0] = g_ssrFrame[1] = 0;
    for (int k = 0; k < 3; ++k)
        if (!MakeRT(g_ssrRT[k], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false)) { Log("reflection targets failed"); g_ssrW = 0; return false; }
    if (!MakeRT(g_ssrAux, w, h, DXGI_FORMAT_R16G16_FLOAT, false)) { Log("reflection targets failed"); g_ssrW = 0; return false; }
    g_ssrW = w; g_ssrH = h;
    Log("reflection targets %ux%u", w, h);
    return true;
}

static bool EnsureHalf(UINT w, UINT h)
{
    if (g_hrt[kHalfRT - 1].srv && g_halfW == w && g_halfH == h) return true;
    static bool failed;
    if (failed) return false;
    for (int k = 0; k < kHalfRT; ++k) FreeRT(g_hrt[k]);
    for (int k = 0; k < 2; ++k) FreeRT(g_camRec[k]);
    g_giFrame[0] = g_giFrame[1] = g_camFrame[0] = g_camFrame[1] = g_radFrame = 0;
    for (int k = 0; k < kHalfRT; ++k)
        if (!MakeRT(g_hrt[k], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, k == 4 || k == 6)) { Log("half-res target %d failed", k); failed = true; return false; }
    for (int k = 0; k < 2; ++k)
        if (!MakeRT(g_camRec[k], 4, 2, DXGI_FORMAT_R32G32B32A32_FLOAT, false)) { Log("camera record failed"); failed = true; return false; }
    g_halfW = w; g_halfH = h;
    Log("half-res targets %ux%u", w, h);
    return true;
}

// ---------------------------------------------------------------- lamps
// The lamp finder (PS_Emit) leaves one candidate per 32x32 tile of the snapshot: world position and
// intensity. Read back a few frames later (never waiting), clustered and kept in a world-space list,
// so lamps keep lighting when they leave the screen. The 32 that matter most near the camera go to the shaders.

struct LampCB { int n[4]; float tgt[2][4], sel[4], ui[4], scr[4], obj[4], zc[16][4], zh[16][4]; int zn[4]; };
struct Lamp { float p[3], c[3]; DWORD seen; int hits, misses; };
static const int kMaxLamps = 256, kStages = 4;
static Lamp g_lamps[kMaxLamps];
static int g_lampCount, g_lampUse;
static ID3D11Buffer* g_cbLamp;
static HalfRT g_emit[2];                  // tile candidates: position, intensity
static ID3D11Texture2D* g_stage[kStages][3];   // read-back copies: position, intensity, camera record
static unsigned g_stageFrame[kStages];    // frame + 1 of a pending copy, 0 = free
static int g_stageNext;
static UINT g_tilesW, g_tilesH;
ID3D11Resource* g_sunShadowTex;           // the sun shadow atlas (seen in the sun pass; identity only, not referenced)
static float g_cam[4][4];                 // last camera record read back: A, B, C, position
static float g_aim[4];                    // world point at the screen centre (w = 1 if valid)
static int g_tgtDel = -1, g_tgtAdd = -1;  // lamp editor: map lamp / found lamp in the cross (by direction)
static bool g_camKnown;
static bool g_weOn, g_cbsOn, g_cbsCopy;   // world editor open (per-draw edits live); the game's constant buffers copied (worldedit.inc)
static int g_weDrag;                      // world editor: the handle held

static float Lum3(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

static bool EnsureEmit()
{
    UINT tw = (g_halfW + 31) / 32, th = (g_halfH + 31) / 32;
    if (g_emit[1].srv && tw == g_tilesW && th == g_tilesH) return true;
    static bool failed;
    if (failed || !tw || !th) return false;
    for (int k = 0; k < 2; ++k) FreeRT(g_emit[k]);
    for (int k = 0; k < kStages; ++k) for (int j = 0; j < 3; ++j) if (g_stage[k][j]) { g_stage[k][j]->Release(); g_stage[k][j] = 0; }
    for (int k = 0; k < 2; ++k)
        if (!MakeRT(g_emit[k], tw, th, DXGI_FORMAT_R32G32B32A32_FLOAT, false)) { failed = true; Log("lamp finder target failed"); return false; }
    D3D11_TEXTURE2D_DESC d = {};
    d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int k = 0; k < kStages; ++k)
        for (int j = 0; j < 3; ++j) {
            d.Width = j < 2 ? tw : 4; d.Height = j < 2 ? th : 2;
            if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_stage[k][j]))) { failed = true; Log("lamp read-back failed"); return false; }
        }
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = (sizeof(LampCB) + 15) & ~15;
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (!g_cbLamp && FAILED(g_dev->CreateBuffer(&bd, 0, &g_cbLamp))) { failed = true; Log("lamp constants failed"); return false; }
    g_tilesW = tw; g_tilesH = th;
    memset(g_stageFrame, 0, sizeof(g_stageFrame));
    Log("lamp finder: %ux%u tiles", tw, th);
    return true;
}

// Where is a world point on the screen of the camera record (u, v = 0..1)? False if behind the camera.
static bool ProjectUV(const float* p, float* uo, float* vo)
{
    float rel[3] = { p[0] - g_cam[3][0], p[1] - g_cam[3][1], p[2] - g_cam[3][2] };
    const float *c0 = g_cam[1], *c1 = g_cam[2];
    float c2[3] = { -rel[0], -rel[1], -rel[2] }, b[3] = { -g_cam[0][0], -g_cam[0][1], -g_cam[0][2] };
    #define CROSS(o, a, b_) { o[0] = a[1] * b_[2] - a[2] * b_[1]; o[1] = a[2] * b_[0] - a[0] * b_[2]; o[2] = a[0] * b_[1] - a[1] * b_[0]; }
    #define DOT(a, b_) (a[0] * b_[0] + a[1] * b_[1] + a[2] * b_[2])
    float c12[3], bc2[3], c1b[3];
    CROSS(c12, c1, c2); CROSS(bc2, b, c2); CROSS(c1b, c1, b);
    float det = DOT(c0, c12);
    if (fabsf(det) < 1e-12f) return false;
    float u = DOT(b, c12) / det, v = DOT(c0, bc2) / det, sc = DOT(c0, c1b) / det;
    #undef CROSS
    #undef DOT
    *uo = u; *vo = v;
    return sc < 0;   // in game the ray scale (depth - w) is negative
}

static bool OnScreen(const float* p)
{
    float u, v;
    return ProjectUV(p, &u, &v) && u > 0.03f && u < 0.97f && v > 0.03f && v < 0.97f;
}

// The view ray through screen point (u, v) of the camera record, normalised.
static void CamRay(float u, float v, float* d)
{
    float l = 0;
    for (int j = 0; j < 3; ++j) { d[j] = -(g_cam[0][j] + g_cam[1][j] * u + g_cam[2][j] * v); l += d[j] * d[j]; }
    l = sqrtf(l) + 1e-9f;
    for (int j = 0; j < 3; ++j) d[j] /= l;
}

// the cars' lights of the last lamp-finder round
struct CarLight { float p[3], c[3]; int tail, age, misses; };
static CarLight g_carLight[64];
static int g_carLightN;
static int MapNearest(const float* p, float maxD);

static void LampsUpdate(const float* pos, UINT posPitch, const float* col, UINT colPitch)
{
    DWORD now = GetTickCount();
    // 1) this round's candidates, clustered (a lamp on a tile border shows up twice)
    struct Cl { float p[3], c[3], w, ny; };
    static Cl cl[1024];
    int nc = 0;
    for (UINT y = 0; y < g_tilesH; ++y)
        for (UINT x = 0; x < g_tilesW; ++x) {
            const float* pp = (const float*)((const char*)pos + y * posPitch) + x * 4;
            const float* cc = (const float*)((const char*)col + y * colPitch) + x * 4;
            float l = Lum3(cc);
            if (pp[3] < 0.5f || !(l > 0) || !(l < 1e30f)) continue;
            int k = 0;
            for (; k < nc; ++k) {
                float dx = cl[k].p[0] / cl[k].w - pp[0], dy = cl[k].p[1] / cl[k].w - pp[1], dz = cl[k].p[2] / cl[k].w - pp[2];
                if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f) break;
            }
            if (k == nc) { if (nc == 1024) continue; memset(&cl[nc++], 0, sizeof(Cl)); }
            for (int j = 0; j < 3; ++j) { cl[k].p[j] += pp[j] * l; cl[k].c[j] += cc[j]; }
            cl[k].w += l;
            cl[k].ny += (pp[3] - 2) * l;   // the bright pixels' normal y (the finder's w = 2 + it)
        }
    // 2) matched to the kept lamps
    static bool matched[kMaxLamps];
    memset(matched, 0, sizeof(matched));
    for (int k = 0; k < nc; ++k) {
        float cp[3] = { cl[k].p[0] / cl[k].w, cl[k].p[1] / cl[k].w, cl[k].p[2] / cl[k].w };
        int best = -1;
        float bd = 2.0f * 2.0f;
        for (int i = 0; i < g_lampCount; ++i) {
            float dx = g_lamps[i].p[0] - cp[0], dy = g_lamps[i].p[1] - cp[1], dz = g_lamps[i].p[2] - cp[2];
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bd) { bd = d2; best = i; }
        }
        if (best >= 0) {
            Lamp& L = g_lamps[best];
            for (int j = 0; j < 3; ++j) { L.p[j] += (cp[j] - L.p[j]) * 0.25f; L.c[j] += (cl[k].c[j] - L.c[j]) * 0.3f; }
            L.seen = now; L.misses = 0; ++L.hits;
            matched[best] = true;
        } else {
            int slot = g_lampCount < kMaxLamps ? g_lampCount++ : -1;
            if (slot < 0) {   // full: replace the longest unseen
                slot = 0;
                for (int i = 1; i < g_lampCount; ++i) if ((LONG)(g_lamps[slot].seen - g_lamps[i].seen) > 0) slot = i;
            }
            Lamp& L = g_lamps[slot];
            memcpy(L.p, cp, sizeof(cp)); memcpy(L.c, cl[k].c, sizeof(L.c));
            L.seen = now; L.hits = 1; L.misses = 0;
            matched[slot] = true;
        }
    }
    // 3) not found again: gone (moved, switched off) if it should have been seen, else kept a while
    for (int i = 0; i < g_lampCount; ++i) {
        if (matched[i]) continue;
        bool drop = now - g_lamps[i].seen > 8000;
        if (!drop && g_camKnown && OnScreen(g_lamps[i].p) && ++g_lamps[i].misses >= 4) drop = true;
        if (drop) { g_lamps[i] = g_lamps[--g_lampCount]; matched[i] = matched[g_lampCount]; --i; }
    }
}

static bool MapLearn(const Lamp& L, bool merge);

// Read back the oldest pending lamp-finder result if the GPU has finished it (never waits).
static void LampsReadBack(ID3D11DeviceContext* ctx, unsigned frame)
{
    for (int k = 0; k < kStages; ++k) {
        if (!g_stageFrame[k] || frame + 1 - g_stageFrame[k] < 2) continue;
        D3D11_MAPPED_SUBRESOURCE m[3];
        int got = 0;
        for (; got < 3; ++got)
            if (ctx->Map(g_stage[k][got], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m[got]) != S_OK) break;
        if (got == 3) {
            for (int r = 0; r < 4; ++r) memcpy(g_cam[r], (const float*)m[2].pData + r * 4, 16);
            memcpy(g_aim, (const float*)((const char*)m[2].pData + m[2].RowPitch), 16);
            g_camKnown = true;
            LampsUpdate((const float*)m[0].pData, m[0].RowPitch, (const float*)m[1].pData, m[1].RowPitch);
            if (g_p[P_LAMP_AUTO].v > 0.5f)   // steady for a long while: a fixed lamp, not a car
                for (int i = 0; i < g_lampCount; ++i) if (g_lamps[i].hits >= 60 && g_lamps[i].misses == 0) MapLearn(g_lamps[i], false);
            g_stageFrame[k] = 0;
        }
        for (int j = 0; j < got; ++j) ctx->Unmap(g_stage[k][j], 0);
    }
}

// ---- lamp map: lamps kept for good (NewLook_lamps.txt), placed by hand or learnt from the found ones
// ---- lamp groups: every map lamp is "custom" (its own colour / power, editable) or belongs to a group with a
// fixed colour and power (NewLook_lamp_presets.txt: name hue saturation power; edit the file, Shift+P reloads).
struct Preset { char name[32]; float hue, sat, power, on, radius, reach, glow; };   // (1.39: on, own light radius / reach (0: the Lamps'), glow in the air)
static Preset g_presets[32];
static int g_presetCount;
static char g_presetPath[MAX_PATH];
static const Preset kDefaultPresets[] = {
    { "sodium", 28, 0.80f, 20, 1, 0, 0, 1 }, { "warm_white", 35, 0.40f, 18, 1, 0, 0, 1 }, { "neutral", 40, 0.12f, 18, 1, 0, 0, 1 },
    { "cold_led", 215, 0.22f, 22, 1, 0, 0, 1 }, { "window", 35, 0.45f, 3, 1, 8, 150, 0 }, { "neon_red", 0, 1, 6, 1, 0, 0, 1 },
    { "neon_orange", 22, 1, 6, 1, 0, 0, 1 }, { "neon_green", 125, 1, 6, 1, 0, 0, 1 }, { "neon_cyan", 185, 1, 6, 1, 0, 0, 1 },
    { "neon_blue", 225, 1, 6, 1, 0, 0, 1 }, { "neon_magenta", 300, 1, 6, 1, 0, 0, 1 } };
static bool g_presetReload = true;   // (the menu's group values: taken from the groups again)

static float Lum3(const float* c);
static void Hsv2Rgb(float h, float s, float* rgb)
{
    h = fmodf(h, 360.0f); if (h < 0) h += 360.0f;
    float c = s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2.0f) - 1)), m = 1 - c;
    float r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; } else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; } else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
    rgb[0] = r + m; rgb[1] = g + m; rgb[2] = b + m;
}

// colour of a hue / saturation at a given power (luminance)
static void ColourOf(float hue, float sat, float power, float* c)
{
    float rgb[3];
    Hsv2Rgb(hue, sat, rgb);
    float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2] + 1e-6f;
    for (int j = 0; j < 3; ++j) c[j] = rgb[j] / l * power;
}

static void PresetsSave()
{
    FILE* f = fopen(g_presetPath, "w");
    if (!f) return;
    fprintf(f, "# NewLook lamp groups: name hue(0-360) saturation(0-1) power [on(1/0) radius(m, 0 = the Lamps') reach(m, 0 = the\n"
               "# Lamps') glow(1/0)]. The order is the group number in NewLook_lamps.txt (7th number, -1 = custom), so add new\n"
               "# groups at the end. Edited in the menu (LAMP GROUPS) or here (Shift+P reloads).\n");
    for (int i = 0; i < g_presetCount; ++i)
        fprintf(f, "%s %.0f %.2f %.2f %.0f %.1f %.0f %.0f\n", g_presets[i].name, g_presets[i].hue, g_presets[i].sat, g_presets[i].power,
                g_presets[i].on, g_presets[i].radius, g_presets[i].reach, g_presets[i].glow);
    fclose(f);
}
static void PresetsLoad()
{
    g_presetCount = 0;
    FILE* f = fopen(g_presetPath, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && g_presetCount < 32) {
            Preset P;
            if (line[0] == '#') continue;
            P.on = 1; P.radius = 0; P.reach = 0; P.glow = 1;
            if (sscanf(line, "%31s %f %f %f %f %f %f %f", P.name, &P.hue, &P.sat, &P.power, &P.on, &P.radius, &P.reach, &P.glow) >= 4) g_presets[g_presetCount++] = P;
        }
        fclose(f);
    }
    if (!g_presetCount) {   // first run: the defaults, written out for editing
        g_presetCount = sizeof(kDefaultPresets) / sizeof(kDefaultPresets[0]);
        memcpy(g_presets, kDefaultPresets, sizeof(kDefaultPresets));
        f = fopen(g_presetPath, "w");
        if (f) {
            fclose(f);
        }
        PresetsSave();
    }
    g_presetReload = true;
    Log("lamp groups: %d", g_presetCount);
}

// c = its own colour (used when custom), group -1 = custom, obj = position of the game object it is tied to
struct MapLamp {
    float p[3], c[3]; int group; float obj[3]; int hasObj;
    int channel;   // light zones: 0 = cut only by zones for all lamps, N = also by the zones of channel N
    // not saved: the tied prop's instance in the game's memory and what it does (proplink.inc)
    unsigned char* propAddr; int propState, propLoose; float propLocal[3], propFade, propM[16]; DWORD propNext, propFlicker;
};
static void MapLampClear(MapLamp& L) { memset(&L, 0, sizeof(L)); L.group = -1; L.propFade = 1; }

static void LampColour(const MapLamp& L, float* c)
{
    if (L.group >= 0 && L.group < g_presetCount) ColourOf(g_presets[L.group].hue, g_presets[L.group].sat, g_presets[L.group].power, c);
    else memcpy(c, L.c, 12);
}
static const int kMaxMap = 8192;
static MapLamp g_map[kMaxMap];
static int g_mapCount;
static char g_mapPath[MAX_PATH], g_lampMsg[256];
static bool g_mapDirty;

static void MapLoad()
{
    g_mapCount = 0;
    FILE* f = fopen(g_mapPath, "r");
    if (!f) { Log("lamp map: none yet (%s)", g_mapPath); return; }
    char line[256];
    while (fgets(line, sizeof(line), f) && g_mapCount < kMaxMap) {
        MapLamp L;
        if (line[0] == '#') continue;
        MapLampClear(L);
        int got = sscanf(line, "%f %f %f %f %f %f %d %f %f %f %d", &L.p[0], &L.p[1], &L.p[2], &L.c[0], &L.c[1], &L.c[2], &L.group,
                         &L.obj[0], &L.obj[1], &L.obj[2], &L.channel);
        if (got >= 10 && (L.obj[0] != 0 || L.obj[1] != 0 || L.obj[2] != 0)) L.hasObj = 1;
        if (got < 10) L.obj[0] = L.obj[1] = L.obj[2] = 0;
        if (got == 11) L.channel = L.channel < 0 ? 0 : L.channel > 15 ? 15 : L.channel;
        if (got >= 6) g_map[g_mapCount++] = L;
    }
    fclose(f);
    Log("lamp map: %d lamps loaded", g_mapCount);
}

static void MapSave()
{
    FILE* f = fopen(g_mapPath, "w");
    if (!f) { Log("lamp map: cannot write %s", g_mapPath); return; }
    fprintf(f, "# NewLook lamp map: x y z (world, m)  r g b (own colour x power)  group (-1 = custom, else a line of\n"
               "# NewLook_lamp_presets.txt counting from 0)  [object x y z (0 0 0 = none)  zone channel].\n"
               "# Edit freely; lines starting with # are ignored.\n");
    for (int i = 0; i < g_mapCount; ++i)
    {
        if (g_map[i].hasObj || g_map[i].channel)
            fprintf(f, "%.3f %.3f %.3f %.4f %.4f %.4f %d %.4f %.4f %.4f %d\n", g_map[i].p[0], g_map[i].p[1], g_map[i].p[2], g_map[i].c[0],
                    g_map[i].c[1], g_map[i].c[2], g_map[i].group, g_map[i].hasObj ? g_map[i].obj[0] : 0, g_map[i].hasObj ? g_map[i].obj[1] : 0,
                    g_map[i].hasObj ? g_map[i].obj[2] : 0, g_map[i].channel);
        else
            fprintf(f, "%.3f %.3f %.3f %.4f %.4f %.4f %d\n", g_map[i].p[0], g_map[i].p[1], g_map[i].p[2], g_map[i].c[0], g_map[i].c[1],
                    g_map[i].c[2], g_map[i].group);
    }
    fclose(f);
    g_mapDirty = false;
    Log("lamp map: %d lamps saved", g_mapCount);
}

static float Dist2(const float* a, const float* b)
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static int MapNearest(const float* p, float maxD)
{
    int best = -1;
    float bd = maxD * maxD;
    for (int i = 0; i < g_mapCount; ++i) { float d = Dist2(g_map[i].p, p); if (d < bd) { bd = d; best = i; } }
    return best;
}

// a found lamp into the map (merged with a map lamp within 2 m)
static bool MapLearn(const Lamp& L, bool merge)
{
    int k = MapNearest(L.p, 2.0f);
    if (k >= 0) {
        if (!merge) return false;
        for (int j = 0; j < 3; ++j) { g_map[k].p[j] = (g_map[k].p[j] + L.p[j]) * 0.5f; g_map[k].c[j] = (g_map[k].c[j] + L.c[j]) * 0.5f; }
        return false;
    }
    if (g_mapCount >= kMaxMap) return false;
    MapLampClear(g_map[g_mapCount]);
    memcpy(g_map[g_mapCount].p, L.p, 12);
    memcpy(g_map[g_mapCount].c, L.c, 12);
    g_map[g_mapCount].group = -1;
    g_map[g_mapCount].hasObj = 0;
    ++g_mapCount;
    g_mapDirty = true;
    return true;
}

// Shift+K: a lamp at the aim point (snapped to a found lamp within 3 m, if any)
static void SelLoad();
static int g_lampSel = -1, g_dragAxis;

// ---- light zones: boxes around tunnels, garages, underpasses. Light from outside a zone does not get in and light
// from inside does not get out (soft 1 m edge). NewLook_zones.txt: centre x y z, half sizes x y z, turn (degrees).
// soft: width of the edge where the light fades (m); channel: 0 = cuts every lamp, N = only the lamps of channel N
struct Zone { float c[3], h[3], yaw, soft; int channel; };
static Zone g_zone[512];
static int g_zoneCount, g_zoneSel = -1, g_zoneProp;
static char g_zonePath[MAX_PATH];
static bool g_zoneDirty;
static int g_zoneUp[16], g_zoneUpN;   // the zones in the shaders' constants this frame

static void ZoneSave()
{
    FILE* f = fopen(g_zonePath, "w");
    if (!f) return;
    fprintf(f, "# NewLook light zones: centre x y z, half size x y z (m), turn about the vertical (degrees), soft edge (m),\n"
               "# channel (0 = cuts every lamp, N = only the lamps of channel N)\n");
    for (int i = 0; i < g_zoneCount; ++i)
        fprintf(f, "%.3f %.3f %.3f %.3f %.3f %.3f %.2f %.2f %d\n", g_zone[i].c[0], g_zone[i].c[1], g_zone[i].c[2], g_zone[i].h[0], g_zone[i].h[1],
                g_zone[i].h[2], g_zone[i].yaw, g_zone[i].soft, g_zone[i].channel);
    fclose(f);
    g_zoneDirty = false;
}

static void ZoneLoad()
{
    g_zoneCount = 0;
    FILE* f = fopen(g_zonePath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f) && g_zoneCount < 512) {
        Zone z;
        z.soft = 1;
        z.channel = 0;
        if (line[0] != '#' && sscanf(line, "%f %f %f %f %f %f %f %f %d", &z.c[0], &z.c[1], &z.c[2], &z.h[0], &z.h[1], &z.h[2], &z.yaw, &z.soft,
                                     &z.channel) >= 7)
            g_zone[g_zoneCount++] = z;
    }
    fclose(f);
    Log("light zones: %d", g_zoneCount);
}

static bool ZoneInside(const Zone& z, const float* p)
{
    float d[3] = { p[0] - z.c[0], p[1] - z.c[1], p[2] - z.c[2] };
    float a = z.yaw * 0.0174533f, sn = sinf(a), cs = cosf(a);
    float lx = d[0] * cs - d[2] * sn, lz = d[0] * sn + d[2] * cs;
    return fabsf(lx) <= z.h[0] && fabsf(d[1]) <= z.h[1] && fabsf(lz) <= z.h[2];
}

// ---- undo / redo for the lamp editor: the whole lamp map and zones before every edit (24 steps)
struct UndoSnap { MapLamp* lamps; int lampN; Zone* zones; int zoneN; char what[48]; };
static UndoSnap g_undo[24], g_redo[24];
static int g_undoN, g_redoN;
static DWORD g_undoLast;
static char g_undoLastWhat[48];
static void MapSave();
static void ZoneSave();
static void SnapFree(UndoSnap& u) { free(u.lamps); free(u.zones); memset(&u, 0, sizeof(u)); }
static void SnapTake(UndoSnap& u, const char* what);
static void SnapPut(UndoSnap& u);

// before an edit; edits of the same kind within 0.7 s (holding K/L) make one step
static void UndoPush(const char* what)
{
    DWORD now = GetTickCount();
    if (!strcmp(what, g_undoLastWhat) && now - g_undoLast < 700) { g_undoLast = now; return; }
    g_undoLast = now;
    strncpy(g_undoLastWhat, what, 47);
    if (g_undoN == 24) { SnapFree(g_undo[0]); memmove(g_undo, g_undo + 1, 23 * sizeof(UndoSnap)); --g_undoN; }
    SnapTake(g_undo[g_undoN++], what);
    for (int i = 0; i < g_redoN; ++i) SnapFree(g_redo[i]);
    g_redoN = 0;
}

static const char* kZoneProps[] = { "width (half), m", "height (half), m", "length (half), m", "turn, degrees", "soft edge, m", "cuts lamps of channel" };

static void SnapTake(UndoSnap& u, const char* what)
{
    memset(&u, 0, sizeof(u));
    u.lamps = (MapLamp*)malloc((g_mapCount + 1) * sizeof(MapLamp));
    u.zones = (Zone*)malloc((g_zoneCount + 1) * sizeof(Zone));
    if (u.lamps) memcpy(u.lamps, g_map, g_mapCount * sizeof(MapLamp));
    if (u.zones) memcpy(u.zones, g_zone, g_zoneCount * sizeof(Zone));
    u.lampN = g_mapCount; u.zoneN = g_zoneCount;
    strncpy(u.what, what, 47);
}

static void SnapPut(UndoSnap& u)
{
    if (!u.lamps || !u.zones) return;
    memcpy(g_map, u.lamps, u.lampN * sizeof(MapLamp));
    memcpy(g_zone, u.zones, u.zoneN * sizeof(Zone));
    g_mapCount = u.lampN; g_zoneCount = u.zoneN;
    // propState 1 / 0 = PS_SEARCH / PS_NONE (declared below)
    for (int i = 0; i < g_mapCount; ++i) { g_map[i].propAddr = 0; g_map[i].propState = g_map[i].hasObj ? 1 : 0; g_map[i].propNext = 0; }
    g_lampSel = g_zoneSel = -1;
    g_dragAxis = 0;
    MapSave();
    ZoneSave();
}

static void UndoStep(bool redo)
{
    UndoSnap* from = redo ? g_redo : g_undo;
    UndoSnap* to = redo ? g_undo : g_redo;
    int& nf = redo ? g_redoN : g_undoN;
    int& nt = redo ? g_undoN : g_redoN;
    if (!nf) { strcpy(g_lampMsg, redo ? "nothing to redo" : "nothing to undo"); return; }
    UndoSnap& u = from[--nf];
    if (nt == 24) { SnapFree(to[0]); memmove(to, to + 1, 23 * sizeof(UndoSnap)); --nt; }
    SnapTake(to[nt++], u.what);   // the current state, for the other way
    SnapPut(u);
    sprintf(g_lampMsg, "%s: %s  (%d more)", redo ? "redone" : "undone", u.what, nf);
    SnapFree(u);
    g_undoLastWhat[0] = 0;
}

static bool g_obConnect;          // lamp editor: the next click picks a game object (Shift+O)
static void PropRequest(const float* T);
static void PropTick();
static float PropLight(MapLamp& L, float* pos);
enum { PS_NONE, PS_SEARCH, PS_LIT, PS_KNOCKED, PS_GONE };   // a tied lamp's prop: GONE = was hit, its instance is gone
static float g_obT[3];            // the picked object's world position (object tools)
static float g_obM[16];           // its axes (rows) and position from its draw's constants
static bool g_obHaveT;
static void ObPickRequest();
static void MapAddAtAim()
{
    UndoPush("add lamp");
    if (!g_camKnown || (g_aim[3] < 0.5f && g_tgtAdd < 0)) { strcpy(g_lampMsg, "add: nothing in the cross"); return; }
    int best = g_tgtAdd >= 0 && g_tgtAdd < g_lampCount ? g_tgtAdd : -1;
    Lamp L;
    memset(&L, 0, sizeof(L));
    if (best >= 0) L = g_lamps[best];
    else {
        // 30 cm in front of the surface, warm white
        float v[3] = { g_aim[0] - g_cam[3][0], g_aim[1] - g_cam[3][1], g_aim[2] - g_cam[3][2] };
        float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6f;
        float pw = g_p[P_LAMP_POWER].v;
        for (int j = 0; j < 3; ++j) L.p[j] = g_aim[j] - v[j] / l * 0.3f;
        L.c[0] = pw; L.c[1] = pw * 0.8f; L.c[2] = pw * 0.55f;
    }
    if (g_mapCount >= kMaxMap) { strcpy(g_lampMsg, "add: map is full"); return; }
    MapLampClear(g_map[g_mapCount]);
    g_map[g_mapCount].p[0] = L.p[0]; g_map[g_mapCount].p[1] = L.p[1]; g_map[g_mapCount].p[2] = L.p[2];
    memcpy(g_map[g_mapCount].c, L.c, 12);
    g_map[g_mapCount].group = -1;
    g_map[g_mapCount].hasObj = 0;
    if (g_newLamp.valid) {   // the settings of the last lamp set up by hand
        memcpy(g_map[g_mapCount].c, g_newLamp.c, 12);
        g_map[g_mapCount].group = g_newLamp.group < g_presetCount ? g_newLamp.group : -1;
    }
    ++g_mapCount;
    MapSave();
    sprintf(g_lampMsg, "added lamp %d (%s), selected", g_mapCount, g_newLamp.valid ? "settings of the last lamp" : best >= 0 ? "found colour" : "by hand");
    g_lampSel = g_mapCount - 1;
    SelLoad();
    Log("lamp map: %s", g_lampMsg);
}

// Shift+J: the map lamp nearest to the aim point (within 4 m) goes away
static void MapDeleteAtAim()
{
    UndoPush("delete lamp");
    int k = g_lampSel >= 0 && g_lampSel < g_mapCount ? g_lampSel : g_tgtDel >= 0 && g_tgtDel < g_mapCount ? g_tgtDel : -1;
    if (k < 0) { strcpy(g_lampMsg, "delete: no map lamp in the cross (red ring)"); return; }
    g_map[k] = g_map[--g_mapCount];
    g_tgtDel = -1;
    g_lampSel = -1;
    MapSave();
    sprintf(g_lampMsg, "deleted a lamp, %d left", g_mapCount);
    Log("lamp map: %s", g_lampMsg);
}

// Shift+L: every steady found lamp into the map
static void MapLearnAll()
{
    UndoPush("learn found lamps");
    int added = 0;
    for (int i = 0; i < g_lampCount; ++i) if (g_lamps[i].hits >= 6 && MapLearn(g_lamps[i], true)) ++added;
    MapSave();
    sprintf(g_lampMsg, "learnt %d new lamps, map has %d", added, g_mapCount);
    Log("lamp map: %s", g_lampMsg);
}

static UINT g_rtW, g_rtH, g_bbW, g_bbH;
// ---- lamp editor (view 24): click a map lamp to select it, drag one of its three arrows to move it
// along that axis (like Blender), I/O/K/L change its colour and power while it is selected.
static int g_hoverAxis, g_lampProp;
static float g_dragP0[3], g_dragS0, g_mouseU = 0.5f, g_mouseV = 0.5f;
static bool g_lmbDown;
static HWND g_hwnd;
static float g_ptrX, g_ptrY;   // pointer in client pixels while dragging
static float g_clientW, g_clientH;

// DroneCam's free camera asks this: while 1 the mouse belongs to the lamp editor (no camera turning).
extern "C" __declspec(dllexport) int __cdecl NewLook_MouseCaptured() { return g_dragAxis != 0 || g_weDrag != 0; }
extern "C" __declspec(dllexport) int __cdecl NFSLight_MouseCaptured() { return g_dragAxis != 0 || g_weDrag != 0; }   // (the name before 1.64: older DroneCam)
static float g_selHue, g_selSat, g_selPow;
static int g_selPreset = -1;   // group of the selected lamp, -1 = custom
static const char* kLampProps[] = { "group", "hue", "saturation", "power", "zone channel" };

static float GizmoLen(const float* p) { return sqrtf(Dist2(p, g_cam[3])) * 0.12f + 0.05f; }

// what the arrows move: the selected zone's centre or the selected lamp
static float* SelPos()
{
    if (g_zoneSel >= 0 && g_zoneSel < g_zoneCount) return g_zone[g_zoneSel].c;
    if (g_lampSel >= 0 && g_lampSel < g_mapCount) return g_map[g_lampSel].p;
    return 0;
}

// Shift+B: a zone at the aim point, turned like the camera, 10 x 8 x 30 m
static void ZoneAddAtAim()
{
    UndoPush("add zone");
    if (!g_camKnown || g_zoneCount >= 512) return;
    float d[3];
    CamRay(0.5f, 0.5f, d);
    Zone z;
    // at the surface under the cross if that is near; else 15 m ahead (the cross may be on the sky)
    if (g_aim[3] > 0.5f && Dist2(g_aim, g_cam[3]) < 60.0f * 60.0f) { z.c[0] = g_aim[0]; z.c[1] = g_aim[1] + 3; z.c[2] = g_aim[2]; }
    else for (int j = 0; j < 3; ++j) z.c[j] = g_cam[3][j] + d[j] * 15;
    z.h[0] = 5; z.h[1] = 4; z.h[2] = 15;
    z.soft = 2;
    z.channel = 0;
    z.yaw = atan2f(d[0], d[2]) * 57.29578f;
    g_zone[g_zoneCount++] = z;
    g_zoneSel = g_zoneCount - 1;
    g_lampSel = -1;
    ZoneSave();
    sprintf(g_lampMsg, "zone %d added: arrows move, side squares stretch, the ring turns it, [I/O] [K/L] values, [Del] delete", g_zoneCount);
}

static void SelLoad()
{
    if (g_lampSel < 0) return;
    g_selPreset = g_map[g_lampSel].group;
    float own[3];
    LampColour(g_map[g_lampSel], own);   // a group lamp shows its group's values
    const float* c = own;
    g_selPow = Lum3(c);
    float mx = c[0] > c[1] ? c[0] : c[1]; if (c[2] > mx) mx = c[2];
    float mn = c[0] < c[1] ? c[0] : c[1]; if (c[2] < mn) mn = c[2];
    g_selSat = mx > 0 ? (mx - mn) / mx : 0;
    float d = mx - mn, h = 0;
    if (d > 1e-6f) {
        if (mx == c[0]) h = 60 * fmodf((c[1] - c[2]) / d, 6.0f);
        else if (mx == c[1]) h = 60 * ((c[2] - c[0]) / d + 2);
        else h = 60 * ((c[0] - c[1]) / d + 4);
    }
    g_selHue = h < 0 ? h + 360 : h;
}

static void SelApply()
{
    if (g_lampSel < 0 || g_map[g_lampSel].group >= 0) return;   // only custom lamps have their own colour
    ColourOf(g_selHue, g_selSat, g_selPow, g_map[g_lampSel].c);
    g_mapDirty = true;
}

// where along an axis line (through p0, direction e) the mouse ray passes closest
static float AxisParam(const float* p0, const float* e, const float* d)
{
    float w[3] = { g_cam[3][0] - p0[0], g_cam[3][1] - p0[1], g_cam[3][2] - p0[2] };
    float b = d[0] * e[0] + d[1] * e[1] + d[2] * e[2];
    float we = w[0] * e[0] + w[1] * e[1] + w[2] * e[2], wd = w[0] * d[0] + w[1] * d[1] + w[2] * d[2];
    float den = 1 - b * b;
    return den > 1e-4f ? (we - b * wd) / den : 0;
}

static float SegDist2D(float px, float py, float ax, float ay, float bx, float by)
{
    float abx = bx - ax, aby = by - ay, l2 = abx * abx + aby * aby;
    float t = l2 > 1e-6f ? ((px - ax) * abx + (py - ay) * aby) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float dx = px - ax - abx * t, dy = py - ay - aby * t;
    return sqrtf(dx * dx + dy * dy);
}

// The selected zone's handles. Codes: 1-3 move arrows, 4-9 its sides (-x +x -y +y -z +z, its own axes), 10 the turn ring.
static void ZoneAxes(const Zone& z, float e[3][3])
{
    float a = z.yaw * 0.0174533f, sn = sinf(a), cs = cosf(a);
    float ex[3] = { cs, 0, -sn }, ey[3] = { 0, 1, 0 }, ez[3] = { sn, 0, cs };
    memcpy(e[0], ex, 12); memcpy(e[1], ey, 12); memcpy(e[2], ez, 12);
}
static void ZoneFace(const Zone& z, int code, float* p)
{
    float e[3][3];
    ZoneAxes(z, e);
    int ax = (code - 4) / 2;
    float sg = ((code - 4) & 1) ? 1.0f : -1.0f;
    for (int j = 0; j < 3; ++j) p[j] = z.c[j] + e[ax][j] * z.h[ax] * sg;
}
static float ZoneRing(const Zone& z) { return GizmoLen(z.c) * 1.5f; }
static Zone g_dragZone;
static float g_dragAng0;

// where the view ray through the pointer meets the level plane at height y (false if it does not)
static bool RayLevel(const float* d, float y, float* p)
{
    if (fabsf(d[1]) < 1e-4f) return false;
    float t = (y - g_cam[3][1]) / d[1];
    if (t <= 0) return false;
    for (int j = 0; j < 3; ++j) p[j] = g_cam[3][j] + d[j] * t;
    return true;
}

static void EditorUpdate(bool active)
{
    if (g_view != 24 || !g_camKnown) {
        if (g_dragAxis) { MapSave(); if (g_zoneDirty) ZoneSave(); }
        g_lampSel = -1; g_zoneSel = -1; g_dragAxis = g_hoverAxis = 0;
        return;
    }
    if (g_lampSel >= g_mapCount) { g_lampSel = -1; g_dragAxis = 0; }
    if (g_zoneSel >= g_zoneCount) { g_zoneSel = -1; g_dragAxis = 0; }
    POINT ms;
    RECT rc;
    if (!g_hwnd || !GetCursorPos(&ms) || !GetClientRect(g_hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    float cw = (float)rc.right, ch = (float)rc.bottom;
    POINT centre = { rc.right / 2, rc.bottom / 2 };
    ClientToScreen(g_hwnd, &centre);
    struct { float x, y; } cp = { cw * 0.5f, ch * 0.5f };   // the pointer: the screen centre (the red cross)
    if (g_dragAxis) {
        if (active) { g_ptrX += ms.x - centre.x; g_ptrY += ms.y - centre.y; SetCursorPos(centre.x, centre.y); }
        cp.x = g_ptrX; cp.y = g_ptrY;
    }
    g_mouseU = cp.x / cw; g_mouseV = cp.y / ch;
    g_clientW = cw; g_clientH = ch;
    float d[3];
    CamRay(g_mouseU, g_mouseV, d);
    bool lmb = active && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    static const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    // the arrow under the cursor
    g_hoverAxis = 0;
    if (SelPos() && !g_dragAxis) {
        const float* P = SelPos();
        float len = GizmoLen(P), u0, v0, best = 12;
        if (ProjectUV(P, &u0, &v0))
            for (int a = 0; a < 3; ++a) {
                float q[3] = { P[0] + axes[a][0] * len, P[1] + axes[a][1] * len, P[2] + axes[a][2] * len }, u1, v1;
                if (!ProjectUV(q, &u1, &v1)) continue;
                float dd = SegDist2D(cp.x, cp.y, u0 * cw, v0 * ch, u1 * cw, v1 * ch);
                if (dd < best) { best = dd; g_hoverAxis = a + 1; }
            }
        if (g_zoneSel >= 0 && g_zoneSel < g_zoneCount) {
            const Zone& Z = g_zone[g_zoneSel];
            for (int c = 4; c <= 9; ++c) {   // side squares
                float fp[3], u, v;
                ZoneFace(Z, c, fp);
                if (!ProjectUV(fp, &u, &v)) continue;
                float dx = u * cw - cp.x, dy = v * ch - cp.y, dd = sqrtf(dx * dx + dy * dy);
                if (dd < best) { best = dd; g_hoverAxis = c; }
            }
            float R = ZoneRing(Z), pu = 0, pv = 0;   // the ring
            bool havePrev = false;
            for (int k = 0; k <= 48; ++k) {
                float a = k * 6.2831853f / 48, rp[3] = { Z.c[0] + sinf(a) * R, Z.c[1], Z.c[2] + cosf(a) * R }, u, v;
                bool ok = ProjectUV(rp, &u, &v);
                if (ok && havePrev) {
                    float dd = SegDist2D(cp.x, cp.y, pu * cw, pv * ch, u * cw, v * ch);
                    if (dd < best) { best = dd; g_hoverAxis = 10; }
                }
                havePrev = ok; pu = u; pv = v;
            }
        }
    }
    static bool mmbDown;
    bool mmb = active && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
    if (mmb && !mmbDown) MapAddAtAim();   // middle button = Shift+K
    mmbDown = mmb;
    if (lmb && !g_lmbDown && g_obConnect) {   // connect mode: the click picks the game object under the cross
        ObPickRequest();
    } else if (lmb && !g_lmbDown) {   // click: grab an arrow, else pick the map lamp under the cursor
        if (g_hoverAxis) {
            UndoPush(g_zoneSel >= 0 ? "change zone" : "move lamp");
            g_undoLastWhat[0] = 0;   // every drag is its own step
            g_dragAxis = g_hoverAxis;
            g_ptrX = cp.x; g_ptrY = cp.y;
            SetCursorPos(centre.x, centre.y);
            memcpy(g_dragP0, SelPos(), 12);
            if (g_dragAxis <= 3) g_dragS0 = AxisParam(g_dragP0, axes[g_dragAxis - 1], d);
            else if (g_zoneSel >= 0) {
                g_dragZone = g_zone[g_zoneSel];
                if (g_dragAxis <= 9) {
                    float e[3][3], fp[3];
                    ZoneAxes(g_dragZone, e);
                    ZoneFace(g_dragZone, g_dragAxis, fp);
                    g_dragS0 = AxisParam(fp, e[(g_dragAxis - 4) / 2], d);
                } else {
                    float hp[3];
                    g_dragAng0 = RayLevel(d, g_dragZone.c[1], hp) ? atan2f(hp[0] - g_dragZone.c[0], hp[2] - g_dragZone.c[2]) : 0;
                }
            }
        } else {
            int best = -1, bestZone = -1;
            float bd = 24;
            for (int i = 0; i < g_mapCount; ++i) {
                float u, v;
                if (!ProjectUV(g_map[i].p, &u, &v)) continue;
                float dx = u * cw - cp.x, dy = v * ch - cp.y, dd = sqrtf(dx * dx + dy * dy);
                if (dd < bd) { bd = dd; best = i; }
            }
            for (int z = 0; z < g_zoneUpN; ++z) {   // zone centres (the squares) shown now
                float u, v;
                if (!ProjectUV(g_zone[g_zoneUp[z]].c, &u, &v)) continue;
                float dx = u * cw - cp.x, dy = v * ch - cp.y, dd = sqrtf(dx * dx + dy * dy);
                if (dd < bd) { bd = dd; bestZone = g_zoneUp[z]; best = -1; }
            }
            if (g_lampSel >= 0 && g_mapDirty) MapSave();
            if (g_zoneDirty) ZoneSave();
            g_lampSel = best;
            g_zoneSel = bestZone;
            SelLoad();
            if (best >= 0) sprintf(g_lampMsg, "selected lamp %d", best + 1);
            else if (bestZone >= 0) sprintf(g_lampMsg, "selected zone %d", bestZone + 1);
            else strcpy(g_lampMsg, "nothing selected");
        }
    }
    if (lmb && g_dragAxis && SelPos() && g_dragAxis <= 3) {
        const float* e = axes[g_dragAxis - 1];
        float sNow = AxisParam(g_dragP0, e, d);
        float* P = SelPos();
        for (int j = 0; j < 3; ++j) P[j] = g_dragP0[j] + e[j] * (sNow - g_dragS0);
    }
    if (lmb && g_dragAxis >= 4 && g_dragAxis <= 9 && g_zoneSel >= 0) {   // a side: stretch the zone that way only
        Zone& Z = g_zone[g_zoneSel];
        float e[3][3], fp[3];
        ZoneAxes(g_dragZone, e);
        ZoneFace(g_dragZone, g_dragAxis, fp);
        int ax = (g_dragAxis - 4) / 2;
        float sg = ((g_dragAxis - 4) & 1) ? 1.0f : -1.0f;
        float ds = AxisParam(fp, e[ax], d) - g_dragS0;
        if (g_dragZone.h[ax] + sg * ds * 0.5f < 0.25f) ds = (0.25f - g_dragZone.h[ax]) * 2 * sg;
        Z.h[ax] = g_dragZone.h[ax] + sg * ds * 0.5f;
        for (int j = 0; j < 3; ++j) Z.c[j] = g_dragZone.c[j] + e[ax][j] * ds * 0.5f;
        g_zoneDirty = true;
    }
    if (lmb && g_dragAxis == 10 && g_zoneSel >= 0) {   // the ring: turn it about the vertical
        float hp[3];
        if (RayLevel(d, g_dragZone.c[1], hp)) {
            float a = atan2f(hp[0] - g_dragZone.c[0], hp[2] - g_dragZone.c[2]);
            g_zone[g_zoneSel].yaw = fmodf(g_dragZone.yaw + (a - g_dragAng0) * 57.29578f + 720, 360);
            g_zoneDirty = true;
        }
    }
    if (!lmb && g_lmbDown && g_dragAxis) {
        g_dragAxis = 0;
        if (g_zoneSel >= 0) { ZoneSave(); sprintf(g_lampMsg, "zone %d changed", g_zoneSel + 1); }
        else { MapSave(); sprintf(g_lampMsg, "moved lamp %d", g_lampSel + 1); }
    }
    g_lmbDown = lmb;
}

#define DOT3(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
static void LsAssign(const int* lampIdx, const float (*pos)[4], int n, float farM);
static int LsSlotOf(int lamp);
static ID3D11ShaderResourceView* g_lsSrv;
static ID3D11Buffer* g_lampFixBuf;         // the lamps' final buffer (render target + t33)
static ID3D11RenderTargetView* g_lampFixRtv;
static ID3D11ShaderResourceView* g_lampFixSrv;
static ID3D11ShaderResourceView* g_lampUseSrv;   // what t33 shows this frame: the fixed buffer, or the CPU's
static ID3D11PixelShader* g_psLampFix;
static ID3D11PixelShader *g_psWetGloss, *g_psWetGlossAdd;   // the game's gloss before its lighting (gamewet.inc): x a, + b
static ID3D11ShaderResourceView* g_carMatSrv;
static int g_carMatN;
static unsigned g_carMatFrame;
// The lamps that matter most near the camera and in view (their light sphere), into the shaders' lamp buffer. Returns how many.
// Sources: the map (steady), the found ones (live), or both (found ones next to a map lamp are skipped).
// 1.39: the lamp group editor (LAMP GROUPS): LampGroup picks one, the rest show and set its values (saved to the file)
static int g_groupLamps[32];
static void LampGroupSync()
{
    static int lastSel = -1;
    static float last[7];
    static DWORD dirtyAt, countAt;
    if (!g_presetCount) return;
    int sel = (int)(g_p[P_LG].v + 0.5f);
    sel = sel < 0 ? 0 : sel >= g_presetCount ? g_presetCount - 1 : sel;
    g_p[P_LG].v = (float)sel;
    Preset& P = g_presets[sel];
    float* fields[7] = { &P.on, &P.hue, &P.sat, &P.power, &P.radius, &P.reach, &P.glow };
    if (sel != lastSel || g_presetReload) {
        for (int k = 0; k < 7; ++k) { g_p[P_LG + 1 + k].v = *fields[k]; last[k] = *fields[k]; }
        lastSel = sel; g_presetReload = false;
    } else {
        for (int k = 0; k < 7; ++k)
            if (g_p[P_LG + 1 + k].v != last[k]) { *fields[k] = last[k] = g_p[P_LG + 1 + k].v; dirtyAt = GetTickCount() | 1; }
    }
    DWORD now = GetTickCount();
    if (dirtyAt && now - dirtyAt > 1000) { PresetsSave(); dirtyAt = 0; Log("lamp groups: %s changed and saved", P.name); }
    if (now - countAt > 2000) {   // (how many map lamps each group has: shown in the menu)
        countAt = now;
        memset(g_groupLamps, 0, sizeof(g_groupLamps));
        for (int i = 0; i < g_mapCount; ++i) if (g_map[i].group >= 0 && g_map[i].group < 32) ++g_groupLamps[g_map[i].group];
    }
}

static int LampsUpload(ID3D11DeviceContext* ctx)
{
    if (!g_cbLamp) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (sizeof(LampCB) + 15) & ~15;
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateBuffer(&bd, 0, &g_cbLamp))) return 0;
    }
    if (!g_lampBuf) {
        D3D11_BUFFER_DESC lb = {};
        lb.ByteWidth = kLampsUp * 4 * 16;
        lb.Usage = D3D11_USAGE_DYNAMIC; lb.BindFlags = D3D11_BIND_SHADER_RESOURCE; lb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Buffer.FirstElement = 0; sv.Buffer.NumElements = kLampsUp * 4;
        if (FAILED(g_dev->CreateBuffer(&lb, 0, &g_lampBuf)) || FAILED(g_dev->CreateShaderResourceView(g_lampBuf, &sv, &g_lampSrv))) {
            if (g_lampBuf) g_lampBuf->Release();
            g_lampBuf = 0; g_lampSrv = 0;
            Log("lamp buffer failed");
            return 0;
        }
    }
    LampCB lc;
    memset(&lc, 0, sizeof(lc));
    static float upPos[kLampsUp][4], upCol[kLampsUp][4];
    int n = 0;
    static int upChan[kLampsUp];   // zone channel of every lamp sent
    static int upMap[kLampsUp];    // its map index (-1: a found lamp, -2: a car's light)
    static float upX[kLampsUp][4], upD[kLampsUp][4], upLo[kLampsUp][3];   // upLo: a car light's place on its car (fix pass)   // radius / car flag, a spot's direction and cone
    static float upScore[kLampsUp];
    int src = (int)(g_p[P_LAMP_SRC].v + 0.5f);
    int cap = (int)(g_p[P_LAMP_MAX].v + 0.5f);   // (1.39: the most lamps a frame)
    cap = cap < 8 ? 8 : cap > kLampsUp ? kLampsUp : cap;
    LampGroupSync();
    PropTick();
    if (g_camKnown) {
        float radius = g_p[P_LAMP_RADIUS].v;
        float reach = g_p[P_LAMP_REACH].v + radius;
        static float score[kLampsUp];
        // the view (from the camera record, a few frames old: a wide margin; the tiles are cut exactly on the GPU)
        float fwd[3], cr[3], cosView = 1;
        CamRay(0.5f, 0.5f, fwd);
        for (int c = 0; c < 4; ++c) { CamRay((float)(c & 1), (float)(c >> 1), cr); float d = DOT3(fwd, cr); if (d < cosView) cosView = d; }
        float viewAng = acosf(cosView < -1 ? -1 : cosView) + 0.35f;   // + 20 degrees
        int total = (src >= 1 ? g_mapCount : 0) + (src <= 1 ? g_lampCount : 0);
        for (int i = 0; i < total; ++i) {
            bool fromMap = src >= 1 && i < g_mapCount;
            const float* pp;
            float col[3];
            float mpos[3];
            const Preset* G = fromMap && g_map[i].group >= 0 && g_map[i].group < g_presetCount ? &g_presets[g_map[i].group] : 0;
            if (G && G->on < 0.5f) continue;   // (1.39: its group off)
            if (fromMap) {
                float k = PropLight(g_map[i], mpos);   // a knocked-down post's lamp goes out (or falls with it)
                if (k < 0.01f) continue;
                pp = mpos;
                LampColour(g_map[i], col);
                for (int j = 0; j < 3; ++j) col[j] *= k;
            }
            else {
                const Lamp& L = g_lamps[src >= 1 ? i - g_mapCount : i];
                if (L.hits < 3) continue;   // seen a few times: not a passing glint
                if (src == 1 && MapNearest(L.p, 2.5f) >= 0) continue;
                pp = L.p;
                float fade = L.hits < 8 ? L.hits / 8.0f : 1.0f;
                for (int j = 0; j < 3; ++j) col[j] = L.c[j] * fade;
            }
            float gRad = G && G->radius > 0 ? G->radius : radius, gReach = G && G->reach > 0 ? G->reach + gRad : reach;   // (its group's own)
            float d2 = Dist2(pp, g_cam[3]);
            if (d2 > gReach * gReach) continue;
            float dist = sqrtf(d2);
            if (dist > gRad + 2) {   // its light sphere is out of view: not sent
                float rel[3] = { (pp[0] - g_cam[3][0]) / dist, (pp[1] - g_cam[3][1]) / dist, (pp[2] - g_cam[3][2]) / dist };
                float c = DOT3(fwd, rel);
                float ang = acosf(c > 1 ? 1 : c < -1 ? -1 : c);
                if (ang > viewAng + asinf(gRad / dist)) continue;
            }
            float sc = Lum3(col) / (d2 + 4);
            int slot = n < cap ? n++ : -1;
            if (slot < 0) {
                slot = 0;
                for (int k = 1; k < cap; ++k) if (score[k] < score[slot]) slot = k;
                if (score[slot] >= sc) continue;
            }
            score[slot] = sc;
            for (int j = 0; j < 3; ++j) { upPos[slot][j] = pp[j]; upCol[slot][j] = col[j]; }
            upCol[slot][3] = 0;   // its zone mask, below
            upChan[slot] = fromMap ? g_map[i].channel : 0;
            upMap[slot] = fromMap ? i : -1;
            upScore[slot] = sc;
            upPos[slot][3] = fromMap ? 1.0f : 0.0f;
            memset(upX[slot], 0, 16); memset(upD[slot], 0, 16);
            if (G) { upX[slot][1] = G->radius; upX[slot][2] = G->glow < 0.5f ? 1.0f : 0.0f; }   // (its own light radius; no glow in the air)
        }
        // the traffic cars' lights (tied to the cars: carlights.inc): spots, small reach, no glow, no shadow maps
        if (g_p[P_TRAFFIC].v > 0) {
            CarsRead(ctx);
            float tr = g_p[P_TRAFFIC + 1].v;
            for (int c = 0; c < g_carN; ++c) {
                float cp[4][4], cc[4][4], cd[4][4], cl[4][4];
                int nl = CarLights(c, cp, cc, cd, cl);
                for (int q = 0; q < nl; ++q) {
                    float d2 = Dist2(cp[q], g_cam[3]);
                    if (d2 > 300.0f * 300.0f) continue;
                    float sc = Lum3(q < 2 ? cc[q] : cc[0]) * 3 / (d2 + 4);   // tail lights rank as their car's headlights
                    int slot = n < cap ? n++ : -1;
                    if (slot < 0) {
                        slot = 0;
                        for (int k = 1; k < cap; ++k) if (score[k] < score[slot]) slot = k;
                        if (score[slot] >= sc) continue;
                    }
                    score[slot] = sc;
                    memcpy(upPos[slot], cp[q], 16); memcpy(upCol[slot], cc[q], 16);
                    upChan[slot] = 0; upMap[slot] = -2; upScore[slot] = sc;
                    // car light q (fix pass); z = 1 + the cone's cosine (> 0.5: a car's light)
                    upX[slot][0] = -2; upX[slot][1] = q < 2 ? tr : g_p[P_TR_TAIL + 2].v; upX[slot][2] = 1 + cl[q][3]; upX[slot][3] = (float)(q + 1);
                    memcpy(upD[slot], cd[q], 16);
                    memcpy(upLo[slot], cl[q], 12);
                }
            }
        }
    }
    lc.n[0] = n;
    lc.n[1] = (int)g_tileW; lc.n[2] = (int)g_tileH;
    lc.n[3] = CarMatCount();   // this frame's car matrices (t46)
    // light zones: the 16 nearest to the camera, and which of them every lamp is in
    g_zoneUpN = 0;
    lc.zn[1] = -1;
    if (g_camKnown) {
        float zd[16];
        for (int z = 0; z < g_zoneCount; ++z) {
            const Zone& Z = g_zone[z];
            float r = sqrtf(Z.h[0] * Z.h[0] + Z.h[1] * Z.h[1] + Z.h[2] * Z.h[2]);
            float d = sqrtf(Dist2(Z.c, g_cam[3])) - r;
            if (d > g_p[P_LAMP_REACH].v + g_p[P_LAMP_RADIUS].v + 30) continue;
            int slot = g_zoneUpN < 16 ? g_zoneUpN++ : -1;
            if (slot < 0) { slot = 0; for (int k = 1; k < 16; ++k) if (zd[k] > zd[slot]) slot = k; if (zd[slot] <= d) continue; }
            zd[slot] = d;
            g_zoneUp[slot] = z;
        }
        for (int k = 0; k < g_zoneUpN; ++k) {
            const Zone& Z = g_zone[g_zoneUp[k]];
            memcpy(lc.zc[k], Z.c, 12); lc.zc[k][3] = Z.yaw * 0.0174533f;
            memcpy(lc.zh[k], Z.h, 12); lc.zh[k][3] = Z.soft;
            if (g_zoneUp[k] == g_zoneSel) lc.zn[1] = k;
            for (int l = 0; l < n; ++l) {
                if (ZoneInside(Z, upPos[l])) upCol[l][3] += (float)(1 << k);
                if (Z.channel == 0 || Z.channel == upChan[l]) upPos[l][3] += (float)(2 << k);   // this zone acts on it
            }
        }
    }
    lc.zn[0] = g_zoneUpN;
    // lamp editor targets: the lamps nearest to the pointer on screen (within 24 pixels), as the click picks them
    g_tgtDel = g_tgtAdd = -1;
    if (g_camKnown && g_clientW > 0) {
        float bestDel = 24, bestAdd = 24;
        for (int i = 0; i < g_mapCount + g_lampCount; ++i) {
            bool isMap = i < g_mapCount;
            const float* pp = isMap ? g_map[i].p : g_lamps[i - g_mapCount].p;
            if (!isMap && (g_lamps[i - g_mapCount].hits < 3 || MapNearest(pp, 2.0f) >= 0)) continue;   // already in the map
            float u, v;
            if (!ProjectUV(pp, &u, &v)) continue;
            float dx = (u - g_mouseU) * g_clientW, dy = (v - g_mouseV) * g_clientH, d = sqrtf(dx * dx + dy * dy);
            if (isMap && d < bestDel) { bestDel = d; g_tgtDel = i; }
            if (!isMap && d < bestAdd) { bestAdd = d; g_tgtAdd = i - g_mapCount; }
        }
        if (g_tgtDel >= 0) { memcpy(lc.tgt[0], g_map[g_tgtDel].p, 12); lc.tgt[0][3] = 1; }
        if (g_tgtAdd >= 0) { memcpy(lc.tgt[1], g_lamps[g_tgtAdd].p, 12); lc.tgt[1][3] = 1; }
    }
    if (g_zoneSel >= 0 && g_zoneSel < g_zoneCount) { memcpy(lc.sel, g_zone[g_zoneSel].c, 12); lc.sel[3] = GizmoLen(g_zone[g_zoneSel].c); }
    if (g_lampSel >= 0 && g_lampSel < g_mapCount) {
        memcpy(lc.sel, g_map[g_lampSel].p, 12); lc.sel[3] = GizmoLen(g_map[g_lampSel].p);
        const MapLamp& S = g_map[g_lampSel];
        if (S.hasObj) {   // its prop: cyan = found standing, orange = knocked down, grey = still looking for it
            memcpy(lc.obj, S.propState == PS_KNOCKED ? S.propM + 12 : S.obj, 12);
            lc.obj[3] = S.propState == PS_LIT ? 1.0f : S.propState == PS_KNOCKED || S.propState == PS_GONE ? 2.0f : 3.0f;
        }
    }
    if (lc.obj[3] < 0.5f && g_obHaveT) { memcpy(lc.obj, g_obT, 12); lc.obj[3] = 1; }   // the last picked object
    lc.ui[0] = g_mouseU; lc.ui[1] = g_mouseV; lc.ui[2] = (float)g_dragAxis; lc.ui[3] = (float)g_hoverAxis;
    lc.scr[0] = g_bbW && g_bbH && g_rtW && g_rtH ? ((float)g_bbH / g_rtH) / ((float)g_bbW / g_rtW) : 1.0f;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_cbLamp, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &lc, sizeof(lc)); ctx->Unmap(g_cbLamp, 0); }
    if (n && SUCCEEDED(ctx->Map(g_lampBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float (*o)[4] = (float (*)[4])m.pData;
        // shadow maps: the strongest map lamps get one
        static int order[kLampsUp], ordMap[kLampsUp];
        static float ordPos[kLampsUp][4];
        for (int k = 0; k < n; ++k) order[k] = k;
        std::sort(order, order + n, [](int x, int y) { return upScore[x] > upScore[y]; });
        for (int k = 0; k < n; ++k) { ordMap[k] = upMap[order[k]]; memcpy(ordPos[k], upPos[order[k]], 16); }
        bool maps = g_p[P_LAMP_SHADOW_MAPS].v > 0.5f;
        if (maps) LsAssign(ordMap, ordPos, n, g_p[P_LAMP_RADIUS].v);
        for (int k = 0; k < n; ++k) {
            memcpy(o[4 * k], upPos[k], 16); memcpy(o[4 * k + 1], upCol[k], 16);
            memcpy(o[4 * k + 2], upX[k], 16); memcpy(o[4 * k + 3], upD[k], 16);
            if (upMap[k] != -2) o[4 * k + 2][0] = maps ? (float)LsSlotOf(upMap[k]) : -1.0f;
            else memcpy(o[4 * k], upLo[k], 12);   // the fix pass makes it the world position
        }
        ctx->Unmap(g_lampBuf, 0);
    }
    return n;
}
static char g_compileError[256];

// stats
static int g_frameSun, g_lastFrameSun, g_skipSmall;
static DXGI_FORMAT g_rtFmt;
static unsigned g_frame;

struct LightCB {
    int L0[4], L1[4], L2[4];
    float size[4], P0[4], P1[4], P2[4], P3[4], P4[4], P5[4], P6[4], P7[4], P8[4], P9[4], P10[4], P11[4], P12[4], P13[4], P14[4], P15[4], P16[4], P17[4];
    float P18[4], P19[4];   // our sun (SunPath, sunshadow.inc): its direction (light -> scene) and light; its colour and on
    float P20[4];           // the game's key light's share (the moon at night); it with our maps (MoonShadows)
    float P21[4];           // 1.39: x = our sky's panorama for the reflections (t67)
    float P22[4], P23[4];   // 1.43: noise settings (spec AA, wet bumps, dots cut, smoothing; history)
    float P24[4];           // 1.46: the reflections' history clip; the sky probe ready (t68)
};

static const char* CloudCommonSrc();
// the game runs D3D11 at feature level 10.x: shader model 4.0
struct ShEntry { const char* entry; const char* target; bool depth, blend; };
static const ShEntry kShList[28] = {
    { "PS_Sun", "ps_4_0", false, false }, { "PS_Sun", "ps_4_0", true, false },
    { "PS_Sun", "ps_4_0", false, true },  { "PS_Sun", "ps_4_0", true, true },
    { "PS_Half", "ps_4_0", false, false }, { "PS_GIBlur", "ps_4_0", false, false },
    { "PS_Air", "ps_4_0", false, false },  { "VS_Overlay", "vs_4_0", false, false },
    { "PS_Overlay", "ps_4_0", false, false }, { "PS_Snap", "ps_4_0", false, false },
    { "PS_Cam", "ps_4_0", false, false }, { "PS_Emit", "ps_4_0", false, false },
    { "PS_Editor", "ps_4_0", false, false }, { "PS_Tiles", "ps_4_0", false, false },
    { "PS_Lamps", "ps_4_0", false, false }, { "PS_Grade", "ps_4_0", false, false },
    { "PS_ABlur", "ps_4_0", false, false }, { "PS_SSR", "ps_4_0", false, false },
    { "PS_SSRApply", "ps_4_0", false, false }, { "PS_SSRBlur", "ps_4_0", false, false },
    { "PS_LampFix", "ps_4_0", false, false }, { "PS_WetGlossMul", "ps_4_0", false, false },
    { "PS_WetGlossAdd", "ps_4_0", false, false }, { "PS_SunMask", "ps_4_0", false, false },
    { "PS_SunTemporal", "ps_4_0", false, false }, { "PS_SunUp", "ps_4_0", false, false },
    { "PS_SkyPano", "ps_4_0", false, false }, { "PS_CityShadow", "ps_4_0", false, false } };
// 1.59: the 28 compiled on several threads (each its own entry; d3dcompiler_47 is thread-safe), through the cache
struct ShJob { const char* src; long n; ID3DBlob* code[28]; ID3DBlob* err[28]; HRESULT hr[28]; volatile LONG next; };
static DWORD WINAPI ShWorker(LPVOID p)
{
    ShJob* j = (ShJob*)p;
    for (;;) {
        LONG k = InterlockedIncrement(&j->next) - 1;
        if (k >= 28) return 0;
        D3D_SHADER_MACRO mac[3] = {};
        int nm = 0;
        if (kShList[k].depth) { mac[nm].Name = "WRITE_DEPTH"; mac[nm++].Definition = "1"; }
        if (kShList[k].blend) { mac[nm].Name = "BLEND_OUT"; mac[nm++].Definition = "1"; }
        j->hr[k] = g_compile(j->src, j->n, "NewLook.hlsl", mac, 0, kShList[k].entry, kShList[k].target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &j->code[k], &j->err[k]);
    }
}
// the source (our clouds' common part + NewLook.hlsl) compiled into j's blobs; false: no file
static bool ShCompileAll(ShJob* j)
{
    memset(j, 0, sizeof(*j));
    FILE* f = fopen(g_hlslPath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    const char* common = CloudCommonSrc();   // our clouds (sky.inc), used by the sun pass's cloud shadows
    size_t nc = strlen(common);
    char* src = (char*)malloc(nc + n + 1);
    memcpy(src, common, nc);
    fread(src + nc, 1, n, f);
    src[nc + n] = 0;
    fclose(f);
    j->src = src; j->n = n + (long)nc;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int nt = (int)si.dwNumberOfProcessors - 1;
    nt = nt < 1 ? 1 : nt > 8 ? 8 : nt;
    HANDLE th[8];
    int made = 0;
    for (int t = 0; t < nt; ++t) if ((th[made] = CreateThread(0, 1 << 20, ShWorker, j, 0, 0)) != 0) ++made;
    if (!made) ShWorker(j);
    WaitForMultipleObjects(made, th, TRUE, INFINITE);
    for (int t = 0; t < made; ++t) CloseHandle(th[t]);
    free(src);
    j->src = 0;
    return true;
}
static void ShRelease(ShJob* j)
{
    for (int k = 0; k < 28; ++k) { if (j->code[k]) j->code[k]->Release(); if (j->err[k]) j->err[k]->Release(); j->code[k] = j->err[k] = 0; }
}
// at the game's start (the compiler loaded with its first shaders): the cache filled in the background, at a low
// priority, so the switch-on finds it ready
static HANDLE g_prewarm;
static DWORD WINAPI ShPrewarm(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    DWORD t0 = GetTickCount();
    LONG m0 = g_cacheMisses;
    static ShJob j;
    if (ShCompileAll(&j)) {
        int bad = 0;
        for (int k = 0; k < 28; ++k) bad += FAILED(j.hr[k]);
        Log("shader cache: ready in the background in %lu ms (%ld compiled, %d failed)", GetTickCount() - t0, g_cacheMisses - m0, bad);
        ShRelease(&j);
    }
    return 0;
}
static void StartShaderPrewarm()
{
    if (!g_cacheDir[0] || g_prewarm) return;
    g_prewarm = CreateThread(0, 1 << 20, ShPrewarm, 0, 0, 0);
}

static bool CompileShaders(ID3D11Device* dev)
{
    DWORD t0 = GetTickCount();
    if (g_prewarm) {   // (still at it: its results are the cache's)
        WaitForSingleObject(g_prewarm, INFINITE);
        CloseHandle(g_prewarm);
        g_prewarm = 0;
    }
    LONG h0 = g_cacheHits, m0 = g_cacheMisses;
    static ShJob j;
    if (!ShCompileAll(&j)) { sprintf(g_compileError, "NewLook.hlsl not found"); Log("%s", g_compileError); return false; }
    ID3D11DeviceChild* made[28] = {};
    bool ok = true;
    for (int k = 0; k < 28 && ok; ++k) {
        HRESULT hr = j.hr[k];
        if (FAILED(hr) || !j.code[k]) {
            _snprintf(g_compileError, sizeof(g_compileError) - 1, "%s", j.err[k] ? (const char*)j.err[k]->GetBufferPointer() : "compile failed");
            Log("shader compile error (%s%s%s): %s", kShList[k].entry, kShList[k].depth ? " depth" : "", kShList[k].blend ? " blend" : "", g_compileError);
            ok = false;
        } else {
            if (kShList[k].target[0] == 'v')
                hr = dev->CreateVertexShader(j.code[k]->GetBufferPointer(), j.code[k]->GetBufferSize(), 0, (ID3D11VertexShader**)&made[k]);
            else
                hr = dev->CreatePixelShader(j.code[k]->GetBufferPointer(), j.code[k]->GetBufferSize(), 0, (ID3D11PixelShader**)&made[k]);
            if (FAILED(hr)) { sprintf(g_compileError, "create shader %s failed %08lx", kShList[k].entry, hr); Log("%s", g_compileError); ok = false; }
        }
    }
    ShRelease(&j);
    Log("NewLook.hlsl: %lu ms (%ld from the cache, %ld compiled)", GetTickCount() - t0, g_cacheHits - h0, g_cacheMisses - m0);
    if (!ok) {
        for (int k = 0; k < 28; ++k) if (made[k]) made[k]->Release();
        return false;
    }
    ID3D11DeviceChild** dst[28] = { (ID3D11DeviceChild**)&g_psSun[0], (ID3D11DeviceChild**)&g_psSun[1], (ID3D11DeviceChild**)&g_psSun[2],
                                   (ID3D11DeviceChild**)&g_psSun[3], (ID3D11DeviceChild**)&g_psHalf, (ID3D11DeviceChild**)&g_psGIBlur,
                                   (ID3D11DeviceChild**)&g_psAir, (ID3D11DeviceChild**)&g_vsText, (ID3D11DeviceChild**)&g_psText,
                                   (ID3D11DeviceChild**)&g_psSnap, (ID3D11DeviceChild**)&g_psCam,
                                   (ID3D11DeviceChild**)&g_psEmit, (ID3D11DeviceChild**)&g_psEditor, (ID3D11DeviceChild**)&g_psTiles, (ID3D11DeviceChild**)&g_psLamps, (ID3D11DeviceChild**)&g_psGrade, (ID3D11DeviceChild**)&g_psABlur, (ID3D11DeviceChild**)&g_psSSR,
                                   (ID3D11DeviceChild**)&g_psSSRApply, (ID3D11DeviceChild**)&g_psSSRBlur, (ID3D11DeviceChild**)&g_psLampFix,
                                   (ID3D11DeviceChild**)&g_psWetGloss, (ID3D11DeviceChild**)&g_psWetGlossAdd,
                                   (ID3D11DeviceChild**)&g_psSunMask, (ID3D11DeviceChild**)&g_psSunTemporal, (ID3D11DeviceChild**)&g_psSunUp,
                                   (ID3D11DeviceChild**)&g_psPano, (ID3D11DeviceChild**)&g_psCity };
    for (int k = 0; k < 28; ++k) { if (*dst[k]) (*dst[k])->Release(); *dst[k] = made[k]; }
    g_compileError[0] = 0;
    Log("NewLook.hlsl compiled");
    return true;
}

static bool EnsureResources(ID3D11DeviceContext* ctx)
{
    static bool done;
    if (done) return g_psSun[0] != 0;
    if (!g_compile) {
        static bool said;
        if (!said) { said = true; Log("resources: waiting for the shader compiler"); }
        return false;
    }
    done = true;
    Log("resources: creating (device %p)", g_dev);
    if (!g_dev) ctx->GetDevice(&g_dev);
    if (!g_dev) return false;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(LightCB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, 0, &g_cb);
    bd.ByteWidth = 16;
    g_dev->CreateBuffer(&bd, 0, &g_cbText);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_sPoint);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_dev->CreateSamplerState(&sd, &g_sText);
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    g_dev->CreateSamplerState(&sd, &g_sWrap);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bl, &g_blendAlpha);
    // colour = src0 + dest * src1, alpha kept
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    if (FAILED(g_dev->CreateBlendState(&bl, &g_blendDual))) Log("dual-source blend state failed: frame copies stay on");
    // colour = src.rgb + dest * src.a, alpha kept
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    g_dev->CreateBlendState(&bl, &g_blendAir);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_rsNoCull);
    return CompileShaders(g_dev);
}

static bool EnsureCopy(ID3D11Texture2D* rt, ID3D11RenderTargetView* rtv)
{
    D3D11_TEXTURE2D_DESC d;
    rt->GetDesc(&d);
    if (d.SampleDesc.Count != 1) return false;
    if (g_copy && g_copyDesc.Width == d.Width && g_copyDesc.Height == d.Height && g_copyDesc.Format == d.Format) return true;
    if (g_copySrv) { g_copySrv->Release(); g_copySrv = 0; }
    if (g_copy) { g_copy->Release(); g_copy = 0; }
    D3D11_TEXTURE2D_DESC c = d;
    c.MipLevels = 1;
    c.ArraySize = 1;
    c.Usage = D3D11_USAGE_DEFAULT;
    c.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    c.CPUAccessFlags = 0;
    c.MiscFlags = 0;
    if (d.MipLevels != 1 || d.ArraySize != 1) return false;
    if (FAILED(g_dev->CreateTexture2D(&c, 0, &g_copy))) { Log("copy texture create failed (fmt %d)", d.Format); return false; }
    D3D11_RENDER_TARGET_VIEW_DESC rv;
    rtv->GetDesc(&rv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = rv.Format;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(g_copy, &sv, &g_copySrv))) { Log("copy SRV create failed (fmt %d)", rv.Format); return false; }
    g_copyDesc = c;
    Log("lighting buffer %ux%u format %d (view %d)", d.Width, d.Height, d.Format, rv.Format);
    return true;
}

static bool Immediate(ID3D11DeviceContext* ctx);
static volatile LONG g_dumpsLeft = 3;

// Detailed log of one sun pass: variant, inputs and the game's constants (read back from the GPU).
static void DumpPass(ID3D11DeviceContext* ctx, SunInfo* s)
{
    int idx = (int)(s - g_sun) + 1;
    Log("dump: variant #%d ps %p: depth t%d normal t%d diffuse t%d spec t%d add t%d shadow t%d cloud t%d vfx t%d cmp s%d lambert %d ashikhmin %d",
        idx, s->ps, s->tDepth, s->tNormal, s->tDiffuse, s->tSpec, s->tAdd, s->tShadow, s->tCloud, s->tVfx, s->sCmp, s->lambert, s->ashikhmin);
    if (s->tShadow >= 0) {
        ID3D11ShaderResourceView* v = 0;
        ctx->PSGetShaderResources(s->tShadow, 1, &v);
        if (v) {
            D3D11_SHADER_RESOURCE_VIEW_DESC vd; v->GetDesc(&vd);
            ID3D11Resource* r = 0; v->GetResource(&r);
            ID3D11Texture2D* t = 0; if (r) r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t);
            D3D11_TEXTURE2D_DESC td = {}; if (t) t->GetDesc(&td);
            Log("dump: shadow map %ux%u tex fmt %d view fmt %d", td.Width, td.Height, td.Format, vd.Format);
            if (t) t->Release(); if (r) r->Release(); v->Release();
        } else Log("dump: shadow map slot is EMPTY");
        ID3D11SamplerState* sm = 0;
        if (s->sCmp >= 0) ctx->PSGetSamplers(s->sCmp, 1, &sm);
        if (sm) { D3D11_SAMPLER_DESC sd; sm->GetDesc(&sd); Log("dump: cmp sampler filter %d func %d", sd.Filter, sd.ComparisonFunc); sm->Release(); }
        else Log("dump: comparison sampler EMPTY");
    }
    for (int b = 0; b < 2; ++b) {
        ID3D11Buffer* cb = 0;
        ctx->PSGetConstantBuffers(b, 1, &cb);
        if (!cb) { Log("dump: cb%d empty", b); continue; }
        D3D11_BUFFER_DESC bd; cb->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = bd; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ID3D11Buffer* st = 0;
        if (SUCCEEDED(g_dev->CreateBuffer(&sd, 0, &st))) {
            ctx->CopyResource(st, cb);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                const float* f = (const float*)m.pData;
                int n4 = bd.ByteWidth / 16;
                for (int i = 0; i < n4 && i < 40; ++i)
                    Log("dump: cb%d[%2d] = %10.4f %10.4f %10.4f %10.4f", b, i, f[i * 4], f[i * 4 + 1], f[i * 4 + 2], f[i * 4 + 3]);
                ctx->Unmap(st, 0);
            }
            st->Release();
        }
        cb->Release();
    }
    {
        ID3D11RenderTargetView* rv[8] = {};
        ID3D11DepthStencilView* dv = 0;
        ctx->OMGetRenderTargets(8, rv, &dv);
        int nrt = 0;
        for (int k = 0; k < 8; ++k) if (rv[k]) { ++nrt; rv[k]->Release(); }
        ID3D11BlendState* bs = 0; FLOAT bf[4]; UINT bm;
        ctx->OMGetBlendState(&bs, bf, &bm);
        D3D11_BLEND_DESC bd = {};
        if (bs) { bs->GetDesc(&bd); bs->Release(); }
        Log("dump: %d render targets, depth %d, blend %d (src %d dest %d op %d, alpha src %d dest %d)", nrt, dv != 0,
            bd.RenderTarget[0].BlendEnable, bd.RenderTarget[0].SrcBlend, bd.RenderTarget[0].DestBlend, bd.RenderTarget[0].BlendOp,
            bd.RenderTarget[0].SrcBlendAlpha, bd.RenderTarget[0].DestBlendAlpha);
        if (dv) dv->Release();
    }
    Log("dump: offsets keycol %d keydir %d C %d C2 %d arr4 %d scale %d w2l %d view %d camdir %d mask %d",
        s->oKeyCol, s->oKeyDir, s->oC, s->oC2, s->oArr4, s->oScale, s->oW2L, s->oView, s->oCamDir, s->oMask);
}

// Our extra draw right after a sun-pass draw.
// The game's draw call, to repeat it with our shader.
enum DrawKind { DK_DRAW, DK_INDEXED, DK_INSTANCED, DK_INDEXED_INSTANCED };
struct DrawCall { DrawKind kind; UINT a, b, c, e; INT base; };
// world editor (worldedit.inc): before a draw (draw it, skip it, or draw it with our matrix), after it; the constant-buffer copies
enum { WE_DRAW, WE_SKIP, WE_SUBST, WE_SPLIT };
static int WeBefore(ID3D11DeviceContext* ctx, const DrawCall& d);
static void WeSplitDraw(ID3D11DeviceContext* ctx, const DrawCall& d);   // a model with edited parts, drawn piece group by group
static void WeEnd(ID3D11DeviceContext* ctx, const DrawCall& d, int we);
static unsigned long long g_weCopyFrom;   // 1.58: the key of the draw now going on, kept for its copies (worldedit.inc)
static void WeCopiesDraw(ID3D11DeviceContext* ctx, const DrawCall& d);
static void CbsOnMap(ID3D11Resource* r, D3D11_MAP type, D3D11_MAPPED_SUBRESOURCE* m);
static void CbsOnUnmap(ID3D11Resource* r);
static void CbsOnUpdate(ID3D11Resource* r, const D3D11_BOX* box, const void* data);
struct VsInfo;
static unsigned long long WeInstKey(ID3D11DeviceContext* ctx, VsInfo* e, const DrawCall& d, unsigned long long* mesh, float* T, bool* placed, float* scale);
static int WeCbNow(ID3D11Buffer* b, const unsigned char** data, UINT* size);   // worldedit.inc
static void CoronaNote(const void* code, SIZE_T size, void* sh);               // sunshadow.inc
static void FlareNote(const void* code, SIZE_T size, void* sh, bool vs);       // sunshadow.inc
static void Replay(ID3D11DeviceContext* ctx, const DrawCall& d);
static volatile LONG g_captureReq;
static void Capture(ID3D11DeviceContext* ctx, SunInfo* s, const DrawCall& dc, struct LightCB* cb, ID3D11RenderTargetView* rtv,
                    ID3D11DepthStencilView* dsv);

static void SunPassLocked(ID3D11DeviceContext* ctx, SunInfo* s, const DrawCall& dc);
static void SsNote(ID3D11DeviceContext* ctx, ID3D11Texture2D* camRec, int oKeyDir, int oKeyCol);           // sunshadow.inc
static bool g_ssFullDay;
static float g_sunSky[3] = { 0, -1, 0 }, g_sunSkyW, g_nightF;
static float g_sunDisc[2], g_moonDisc[4], g_skyLum;
static float g_hazeMul = 1;                           // 1.36: the haze by the clock (x HazeAmount)   // 1.35: our sun's disc (strength, warmth), the moon's (towards, strength); the sky's light for the haze   // 1.33: the sun for the sky (at night under the horizon), its light; the night's share (0..1)
static float g_sunOurs[3] = { 0, -1, 0 }, g_sunCol[3], g_sunW;   // our sun (SunPath, sunshadow.inc): light -> scene, colour, its light
static float g_sunG = 1;                                          // the game's key light's share (1 at night: the moon)
static bool g_sunMoonMaps;                                        // it gets our maps (MoonShadows, our maps on at night)
static bool g_sunActive;
static bool SsMaskPass(ID3D11DeviceContext* ctx, const DrawCall& dc, ID3D11ShaderResourceView* prevCam);
static void SsUnbind(ID3D11DeviceContext* ctx);
static CRITICAL_SECTION g_passLock;
static void SunPass(ID3D11DeviceContext* ctx, SunInfo* s, const DrawCall& dc)
{
    // deferred contexts may record on several threads; our copy texture and constants are shared
    EnterCriticalSection(&g_passLock);
    SunPassLocked(ctx, s, dc);
    LeaveCriticalSection(&g_passLock);
}

// ---------------------------------------------------------------- capture (Shift+U)
// Renders our shader in several debug modes into a small float target and saves PNGs next to
// the plugin (NewLook_capture\), so the buffers can be looked at outside the game.

static unsigned Crc32(const unsigned char* p, size_t n, unsigned c = 0xffffffff)
{
    static unsigned t[256];
    if (!t[1]) for (unsigned i = 0; i < 256; ++i) { unsigned x = i; for (int k = 0; k < 8; ++k) x = x & 1 ? 0xedb88320 ^ (x >> 1) : x >> 1; t[i] = x; }
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void PngChunk(FILE* f, const char* type, const unsigned char* d, unsigned n)
{
    unsigned char hdr[8] = { (unsigned char)(n >> 24), (unsigned char)(n >> 16), (unsigned char)(n >> 8), (unsigned char)n,
                             (unsigned char)type[0], (unsigned char)type[1], (unsigned char)type[2], (unsigned char)type[3] };
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    unsigned c = Crc32(hdr + 4, 4);
    if (n) c = Crc32(d, n, c);
    c ^= 0xffffffff;
    unsigned char cc[4] = { (unsigned char)(c >> 24), (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
    fwrite(cc, 1, 4, f);
}

static void WritePng(const char* path, const unsigned char* rgb, int w, int h)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    unsigned char ih[13] = { (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w,
                             (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h, 8, 2, 0, 0, 0 };
    PngChunk(f, "IHDR", ih, 13);
    // zlib stream made of stored (uncompressed) blocks
    size_t row = (size_t)w * 3 + 1, raw = (size_t)h * row;
    unsigned char* r = (unsigned char*)malloc(raw);
    for (int y = 0; y < h; ++y) { r[y * row] = 0; memcpy(r + y * row + 1, rgb + (size_t)y * w * 3, (size_t)w * 3); }
    size_t blocks = (raw + 65534) / 65535;
    unsigned char* z = (unsigned char*)malloc(2 + raw + blocks * 5 + 4);
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < raw; ++i) { a = (a + r[i]) % 65521; b = (b + a) % 65521; }
    for (size_t p = 0; p < raw; p += 65535) {
        unsigned len = (unsigned)(raw - p < 65535 ? raw - p : 65535);
        z[o++] = p + len >= raw ? 1 : 0;
        z[o++] = (unsigned char)(len & 255); z[o++] = (unsigned char)(len >> 8);
        z[o++] = (unsigned char)(~len & 255); z[o++] = (unsigned char)((~len >> 8) & 255);
        memcpy(z + o, r + p, len); o += len;
    }
    unsigned ad = (b << 16) | a;
    z[o++] = (unsigned char)(ad >> 24); z[o++] = (unsigned char)(ad >> 16); z[o++] = (unsigned char)(ad >> 8); z[o++] = (unsigned char)ad;
    PngChunk(f, "IDAT", z, (unsigned)o);
    PngChunk(f, "IEND", 0, 0);
    fclose(f);
    free(r);
    free(z);
}

static void Capture(ID3D11DeviceContext* ctx, SunInfo* s, const DrawCall& dc, LightCB* cb, ID3D11RenderTargetView* rtv,
                    ID3D11DepthStencilView* dsv)
{
    const int W = 1280, H = 720;
    char dir[MAX_PATH];
    sprintf(dir, "%sNewLook_capture", g_dir);
    CreateDirectoryA(dir, 0);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = 0, *st = 0;
    ID3D11RenderTargetView* trtv = 0;
    if (FAILED(g_dev->CreateTexture2D(&td, 0, &tex)) || FAILED(g_dev->CreateRenderTargetView(tex, 0, &trtv))) { Log("capture: target failed"); return; }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&td, 0, &st))) { Log("capture: staging failed"); trtv->Release(); tex->Release(); return; }
    D3D11_VIEWPORT savedVp[16];
    UINT nVp = 16;
    ctx->RSGetViewports(&nVp, savedVp);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    struct Shot { int mode; const char* name; bool hdr; } shots[] = {
        { 13, "01_game_before", true }, { 0, "02_with_mod", true }, { 7, "03_game_sun_light", true },
        { 10, "04_additive_buffer", true }, { 9, "05_everything_but_sun", true }, { 5, "06_shadow_game", false },
        { 4, "07_shadow_new_with_contact", false }, { 2, "08_contact_mask", false }, { 3, "09_ao_mask", false },
        { 11, "10_diffuse_alpha", false }, { 12, "11_n_dot_l", false }, { 14, "12_albedo", false }, { 15, "13_cloud_alpha_factor", false },
        { 16, "14_caster_distance", false }, { 17, "15_penumbra_width", false }, { 18, "16_cascades", false },
        { 19, "17_haze_amount", false }, { 20, "18_light_shafts", false },
        { 25, "19_gloss", false }, { 26, "20_specular", false }, { 27, "21_rain_wetness", false } };
    LightCB c = *cb;
    c.P5[2] = 0; c.P5[3] = 0;   // compute everything in place at the capture size
    c.size[0] = (float)W;
    c.size[1] = (float)H;
    float expo = 0;
    unsigned char* rgb = (unsigned char*)malloc(W * H * 3);
    for (int k = 0; k < (int)(sizeof(shots) / sizeof(shots[0])); ++k) {
        c.L2[3] = shots[k].mode;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &c, sizeof(c)); ctx->Unmap(g_cb, 0); }
        float clear[4] = { 0, 0, 0, 0 };
        ctx->ClearRenderTargetView(trtv, clear);
        ctx->OMSetRenderTargets(1, &trtv, 0);
        oPSSetShader(ctx, g_psSun[0], 0, 0);
        Replay(ctx, dc);
        ctx->CopyResource(st, tex);
        if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) continue;
        if (shots[k].hdr && expo == 0) {  // one exposure for all colour shots, from the game's image
            double sum = 0;
            int n = 0;
            for (int y = 0; y < H; y += 4)
                for (int x = 0; x < W; x += 4) {
                    const float* px = (const float*)((char*)m.pData + y * m.RowPitch) + x * 4;
                    float l = 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
                    if (l > 0 && l < 1e6f) { sum += log(l + 1e-4); ++n; }
                }
            expo = n ? (float)(0.18 / exp(sum / n)) : 1.0f;
            Log("capture: exposure %.3f", expo);
        }
        double mn = 1e30, mx = -1e30, avg = 0;
        for (int y = 0; y < H; ++y) {
            const float* px = (const float*)((char*)m.pData + y * m.RowPitch);
            for (int x = 0; x < W; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    float v = px[x * 4 + ch];
                    if (!(v == v)) v = 0;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    avg += v;
                    float o = shots[k].hdr ? v * expo / (1 + v * expo) : v;
                    o = o < 0 ? 0 : o > 1 ? 1 : o;
                    rgb[(y * W + x) * 3 + ch] = (unsigned char)(powf(o, shots[k].hdr ? 1 / 2.2f : 1.0f) * 255 + 0.5f);
                }
        }
        ctx->Unmap(st, 0);
        char path[MAX_PATH];
        sprintf(path, "%s\\%s.png", dir, shots[k].name);
        WritePng(path, rgb, W, H);
        Log("capture: %s min %.4f max %.4f mean %.4f", shots[k].name, mn, mx, avg / (W * H * 3));
    }
    free(rgb);
    // back to the game's target and our normal constants
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, cb, sizeof(*cb)); ctx->Unmap(g_cb, 0); }
    ctx->OMSetRenderTargets(1, &rtv, dsv);
    ctx->RSSetViewports(nVp, savedVp);
    trtv->Release();
    tex->Release();
    st->Release();
    Log("capture saved to %s", dir);
}

// ---------------------------------------------------------------- timing
// GPU time of every pass from D3D11 timestamp queries (read back 4 frames later, never waiting),
// CPU frame time from QueryPerformanceCounter. Shown in the overlay, logged with the stats.

// (snap runs inside air: the mod's total counts air only)
enum { PR_GAMESUN, PR_COPY, PR_HALF, PR_BLUR, PR_LAMPS, PR_SUN, PR_SNAP, PR_SSR, PR_AIR, PR_LOOK, PR_OVERLAY, PR_SSMASK, PR_SHPASS, PR_PAD, PR_EARLY, PR_PROBE, PR_COUNT };
static const char* kProfNames[PR_COUNT] = { "game sun", "copy", "half-res", "blur", "lamps", "sun", "snap", "reflections", "air (with snap, refl.)", "look", "overlay", "sun shadow mask", "shadow pass (game + ours)", "bench padding", "reflections' copy", "sky probe" };
struct ProfFrame { ID3D11Query* dis; ID3D11Query* t[PR_COUNT][2]; ID3D11Query* f[2]; bool used[PR_COUNT]; bool live; };
static const int kProfLag = 5;
static ProfFrame g_pf[kProfLag];
static int g_pfIdx;
static bool g_profOk, g_profFailed, g_pfStarted;
static double g_gpuMs[PR_COUNT], g_gpuFrameMs, g_cpuFrameMs, g_cpuModMs, g_cpuModAcc;
static unsigned g_profLost;
static LARGE_INTEGER g_qpf, g_lastPresentQpc;

static bool ProfInit()
{
    if (g_profOk || g_profFailed || !g_dev) return g_profOk;
    QueryPerformanceFrequency(&g_qpf);
    D3D11_QUERY_DESC dj = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, ts = { D3D11_QUERY_TIMESTAMP, 0 };
    for (int f = 0; f < kProfLag; ++f) {
        bool ok = SUCCEEDED(g_dev->CreateQuery(&dj, &g_pf[f].dis));
        for (int k = 0; k < PR_COUNT && ok; ++k)
            ok = SUCCEEDED(g_dev->CreateQuery(&ts, &g_pf[f].t[k][0])) && SUCCEEDED(g_dev->CreateQuery(&ts, &g_pf[f].t[k][1]));
        ok = ok && SUCCEEDED(g_dev->CreateQuery(&ts, &g_pf[f].f[0])) && SUCCEEDED(g_dev->CreateQuery(&ts, &g_pf[f].f[1]));
        if (!ok) { g_profFailed = true; Log("timing: queries not available"); return false; }
    }
    g_profOk = true;
    Log("timing: GPU timestamp queries ready");
    return true;
}

// marks on the immediate context only
static void ProfMark(ID3D11DeviceContext* ctx, int id, int end)
{
    if (!g_pfStarted || !Immediate(ctx)) return;
    ProfFrame& f = g_pf[g_pfIdx];
    ctx->End(f.t[id][end]);
    if (end) f.used[id] = true;
}

static double Smooth(double old, double v) { return old <= 0 ? v : old + (v - old) * 0.08; }
static void BenchSample(double frameMs, const double* pass);
static void BenchCpu(double ms, double modMs);

// End of a frame (before Present): close it, read the oldest one, open the next (after Present).
static void ProfFrameEnd(ID3D11DeviceContext* ctx)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_lastPresentQpc.QuadPart && g_qpf.QuadPart) {
        double raw = (now.QuadPart - g_lastPresentQpc.QuadPart) * 1000.0 / g_qpf.QuadPart;
        g_cpuFrameMs = Smooth(g_cpuFrameMs, raw);
        BenchCpu(raw, g_cpuModAcc);
    }
    g_lastPresentQpc = now;
    g_cpuModMs = Smooth(g_cpuModMs, g_cpuModAcc);
    g_cpuModAcc = 0;
    if (!g_pfStarted) return;
    ProfFrame& f = g_pf[g_pfIdx];
    ctx->End(f.f[1]);
    ctx->End(f.dis);
    f.live = true;
}

static void ProfFrameBegin(ID3D11DeviceContext* ctx)
{
    if (!ProfInit()) return;
    g_pfIdx = (g_pfIdx + 1) % kProfLag;
    ProfFrame& f = g_pf[g_pfIdx];
    if (f.live) {   // the oldest frame: read it if the GPU is done with it
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx->GetData(f.dis, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency) {
            UINT64 a = 0, b = 0;
            double ms = 1000.0 / (double)dj.Frequency, rawFrame = -1, raw[PR_COUNT] = {};
            if (ctx->GetData(f.f[0], &a, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx->GetData(f.f[1], &b, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b >= a) {
                rawFrame = (b - a) * ms;
                g_gpuFrameMs = Smooth(g_gpuFrameMs, rawFrame);
            }
            for (int k = 0; k < PR_COUNT; ++k) {
                double v = 0;
                if (f.used[k] && ctx->GetData(f.t[k][0], &a, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                    ctx->GetData(f.t[k][1], &b, 8, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && b >= a)
                    v = (b - a) * ms;
                g_gpuMs[k] = g_gpuMs[k] + (v - g_gpuMs[k]) * 0.08;
                raw[k] = v;
            }
            BenchSample(rawFrame, raw);
        } else ++g_profLost;
    }
    f.live = false;
    for (int k = 0; k < PR_COUNT; ++k) f.used[k] = false;
    ctx->Begin(f.dis);
    ctx->End(f.f[0]);
    g_pfStarted = true;
}

struct CpuTimer {
    LARGE_INTEGER t0;
    CpuTimer() { QueryPerformanceCounter(&t0); }
    ~CpuTimer() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); if (g_qpf.QuadPart) g_cpuModAcc += (t1.QuadPart - t0.QuadPart) * 1000.0 / g_qpf.QuadPart; }
};

static int ProfText(char* p)
{
    char* s = p;
    double fps = g_cpuFrameMs > 0 ? 1000.0 / g_cpuFrameMs : 0;
    double mod = g_gpuMs[PR_COPY] + g_gpuMs[PR_HALF] + g_gpuMs[PR_BLUR] + g_gpuMs[PR_LAMPS] + g_gpuMs[PR_SUN] + g_gpuMs[PR_AIR] + g_gpuMs[PR_LOOK] + g_gpuMs[PR_EARLY] + g_gpuMs[PR_PROBE];
    p += sprintf(p, "FPS %.1f (%.2f ms)   GPU frame %.2f ms   mod GPU %.2f ms   mod CPU %.2f ms\n", fps, g_cpuFrameMs, g_gpuFrameMs, mod, g_cpuModMs);
    p += sprintf(p, "GPU ms:");
    for (int k = 0; k < PR_COUNT; ++k) p += sprintf(p, " %s %.2f%s", kProfNames[k], g_gpuMs[k], k == PR_GAMESUN ? " |" : "");
    p += sprintf(p, "\n");
    return (int)(p - s);
}

// What the late air pass needs from the main sun pass of this frame.
struct AirState {
    unsigned frame;
    ID3D11VertexShader* vs;
    ID3D11InputLayout* il;
    ID3D11Buffer* vb[2];
    UINT stride[2], offset[2];
    ID3D11Buffer* ib;
    DXGI_FORMAT ibFmt;
    UINT ibOff;
    D3D11_PRIMITIVE_TOPOLOGY top;
    ID3D11Buffer* vscb[4];
    ID3D11Buffer* pscb[2];
    ID3D11ShaderResourceView* depth;
    ID3D11ShaderResourceView* shadow;
    ID3D11ShaderResourceView* diffuse;
    ID3D11ShaderResourceView* spec;
    ID3D11ShaderResourceView* normal;
    ID3D11SamplerState* cmp;
    ID3D11Texture2D* scene;
    DXGI_FORMAT rtvFmt;
    DrawCall dc;
    LightCB cb;
};
static AirState g_air;
static bool g_airWorks = true;     // false: no post-processing pass found, the air stays in the sun pass
static unsigned g_airDoneFrame, g_airMisses;
static ID3D11RenderTargetView* g_sceneRtv;
static ID3D11ShaderResourceView* g_sceneSrv;
static ID3D11Texture2D* g_sceneRtvTex;

static void ReleaseAir()
{
    IUnknown* u[] = { g_air.vs, g_air.il, g_air.vb[0], g_air.vb[1], g_air.ib, g_air.vscb[0], g_air.vscb[1], g_air.vscb[2],
                      g_air.vscb[3], g_air.pscb[0], g_air.pscb[1], g_air.depth, g_air.shadow, g_air.cmp, g_air.scene,
                      g_air.diffuse, g_air.spec, g_air.normal };
    for (int i = 0; i < (int)(sizeof(u) / sizeof(u[0])); ++i) if (u[i]) u[i]->Release();
    memset(&g_air, 0, sizeof(g_air));
}

// A copy of one of the game's constant buffers as it is now (the game maps new contents into them later in
// the frame, and the late passes must see the sun pass's camera and light).
static ID3D11Buffer* g_cbCopy[6];
static UINT g_cbCopySize[6];
static void KeepCopy(ID3D11DeviceContext* ctx, int slot, ID3D11Buffer*& b)
{
    if (!b) return;
    D3D11_BUFFER_DESC d;
    b->GetDesc(&d);
    if (!g_cbCopy[slot] || g_cbCopySize[slot] != d.ByteWidth) {
        if (g_cbCopy[slot]) { g_cbCopy[slot]->Release(); g_cbCopy[slot] = 0; }
        D3D11_BUFFER_DESC n = {};
        n.ByteWidth = d.ByteWidth; n.Usage = D3D11_USAGE_DEFAULT; n.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&n, 0, &g_cbCopy[slot]))) { g_cbCopy[slot] = 0; return; }
        g_cbCopySize[slot] = d.ByteWidth;
    }
    ctx->CopyResource(g_cbCopy[slot], b);
    b->Release();
    b = g_cbCopy[slot];
    b->AddRef();
}

static void SaveAir(ID3D11DeviceContext* ctx, const DrawCall& dc, const LightCB& cb, ID3D11ShaderResourceView* depth,
                    ID3D11ShaderResourceView* diffuse, ID3D11ShaderResourceView* spec, ID3D11ShaderResourceView* normal,
                    ID3D11ShaderResourceView* shadow, ID3D11SamplerState* cmp, ID3D11Texture2D* scene, DXGI_FORMAT rtvFmt)
{
    ReleaseAir();
    g_air.frame = g_frame;
    ctx->VSGetShader(&g_air.vs, 0, 0);
    ctx->IAGetInputLayout(&g_air.il);
    ctx->IAGetVertexBuffers(0, 2, g_air.vb, g_air.stride, g_air.offset);
    ctx->IAGetIndexBuffer(&g_air.ib, &g_air.ibFmt, &g_air.ibOff);
    ctx->IAGetPrimitiveTopology(&g_air.top);
    ctx->VSGetConstantBuffers(0, 4, g_air.vscb);
    ctx->PSGetConstantBuffers(0, 2, g_air.pscb);
    for (int k = 0; k < 4; ++k) KeepCopy(ctx, k, g_air.vscb[k]);
    for (int k = 0; k < 2; ++k) KeepCopy(ctx, 4 + k, g_air.pscb[k]);
    g_air.depth = depth; if (depth) depth->AddRef();
    g_air.shadow = shadow; if (shadow) shadow->AddRef();
    g_air.diffuse = diffuse; if (diffuse) diffuse->AddRef();
    g_air.spec = spec; if (spec) spec->AddRef();
    g_air.normal = normal; if (normal) normal->AddRef();
    g_air.cmp = cmp; if (cmp) cmp->AddRef();
    g_air.scene = scene; if (scene) scene->AddRef();
    g_air.rtvFmt = rtvFmt;
    g_air.dc = dc;
    g_air.cb = cb;
}

// The air pass: haze, glow and light shafts blended onto the finished 3D image.
static void AirPass(ID3D11DeviceContext* ctx)
{
    if (!g_air.scene || !g_psAir || !g_air.vs || !g_blendAir) return;
    CpuTimer cpu;
    D3D11_TEXTURE2D_DESC sd;
    g_air.scene->GetDesc(&sd);
    if (g_sceneRtvTex != g_air.scene) {
        if (g_sceneRtv) { g_sceneRtv->Release(); g_sceneRtv = 0; }
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = g_air.rtvFmt;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(g_air.scene, &rd, &g_sceneRtv))) { Log("air: scene RTV failed"); return; }
        if (g_sceneSrv) { g_sceneSrv->Release(); g_sceneSrv = 0; }
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = g_air.rtvFmt;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(g_air.scene, &sv, &g_sceneSrv))) { g_sceneSrv = 0; Log("air: scene SRV failed, bounce light from the sun only"); }
        g_sceneRtvTex = g_air.scene;
    }
    // save
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0;
    ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    ID3D11Buffer* sVb[2]; UINT sSt[2], sOf[2]; ctx->IAGetVertexBuffers(0, 2, sVb, sSt, sOf);
    ID3D11Buffer* sIb = 0; DXGI_FORMAT sIbF; UINT sIbO; ctx->IAGetIndexBuffer(&sIb, &sIbF, &sIbO);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11Buffer* sVcb[4]; ctx->VSGetConstantBuffers(0, 4, sVcb);
    ID3D11Buffer* sPcb[7]; ctx->PSGetConstantBuffers(0, 7, sPcb);
    ID3D11ShaderResourceView* sSrv[42]; ctx->PSGetShaderResources(0, 42, sSrv);
    ID3D11SamplerState* sSmp[4]; ctx->PSGetSamplers(10, 4, sSmp);
    ID3D11RenderTargetView* sRtv[8] = {}; ID3D11DepthStencilView* sDsv = 0; ctx->OMGetRenderTargets(8, sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16; ctx->RSGetViewports(&nVp, sVp);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);

    // draw
    ProfMark(ctx, PR_AIR, 0);
    ID3D11ShaderResourceView* noSrv[42] = {};
    ctx->PSSetShaderResources(0, 42, noSrv);          // the scene may be bound as an input right now
    ctx->PSSetShaderResources(56, 1, &g_expoSrv);     // the game's exposure (the debug views)
    LightCB cb = g_air.cb;
    cb.L2[3] = g_view;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
    ID3D11ShaderResourceView* in[10] = { 0, g_air.depth, 0, 0, 0, 0, g_air.shadow, 0, 0, cb.P5[3] > 0.5f ? g_hrt[0].srv : 0 };
    ctx->PSSetShaderResources(16, 10, in);
    ID3D11SamplerState* smp[4] = { g_air.cmp, g_sPoint, g_sWrap, g_sText };
    ctx->PSSetSamplers(10, 4, smp);
    ctx->PSSetConstantBuffers(0, 2, g_air.pscb);
    ctx->PSSetConstantBuffers(5, 1, &g_cb);
    ctx->VSSetShader(g_air.vs, 0, 0);
    ctx->VSSetConstantBuffers(0, 4, g_air.vscb);
    ctx->IASetInputLayout(g_air.il);
    ctx->IASetVertexBuffers(0, 2, g_air.vb, g_air.stride, g_air.offset);
    ctx->IASetIndexBuffer(g_air.ib, g_air.ibFmt, g_air.ibOff);
    ctx->IASetPrimitiveTopology(g_air.top);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->RSSetState(g_rsNoCull);
    // the finished scene at half resolution, light source of the next frame's bounce light
    int cur = g_frame & 1;
    bool lamps = ((g_p[P_LAMP].v > 0 && (!g_ssFullDay || g_p[P_LAMP_DAY].v > 0)) || g_view == 24 || g_weOn) && g_psEmit &&
                 g_camFrame[cur] == g_frame + 1;   // the lamp finder (the camera is read back with it); not in full day
    bool ssrOn = g_p[P_SSR].v > 0 && g_psSSR && g_psSSRApply && g_blendDual && g_air.normal;
    bool ssrDone = false;
    if (g_sceneSrv && g_psSnap && g_hrt[4].rtv && ((g_p[P_BOUNCE].v > 0 && g_p[P_BOUNCE_ALL].v > 0.5f) || lamps || ssrOn) &&
        sd.Width == g_halfW * 2 && sd.Height == g_halfH * 2) {
        ProfMark(ctx, PR_SNAP, 0);
        ctx->OMSetBlendState(0, 0, 0xffffffff);
        ID3D11RenderTargetView* snapT[2] = { g_hrt[4].rtv, g_hrt[6].rtv };
        ctx->OMSetRenderTargets(2, snapT, 0);
        ctx->PSSetShaderResources(16, 1, &g_sceneSrv);
        // 1.47: the reflections' source (the snapshot's [1], the sharp one) from the scene before the game's effects
        bool early = ssrOn && g_p[P_SSR_EARLY].v >= 0.5f && g_earlySrv && g_earlyFrame == g_frame + 1;
        cb.P24[2] = early ? 1.0f : 0.0f;
        ctx->PSSetShaderResources(70, 1, early ? &g_earlySrv : noSrv);
        // our own bounce / lamp light of this frame, to take it out of the snapshot
        bool gi = cb.P7[3] > 0.5f && g_giFrame[cur] == g_frame + 1;
        bool lampsOn = g_lampUse > 0 && g_cbLamp && g_tileFrame == g_frame + 1;
        ID3D11ShaderResourceView* gin[3] = { g_air.normal, gi || lampsOn ? g_air.diffuse : 0, gi || lampsOn ? g_air.spec : 0 };
        ctx->PSSetShaderResources(18, 3, gin);
        ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);
        if (lampsOn) {
            ctx->PSSetConstantBuffers(6, 1, &g_cbLamp);
            bool lt = g_lampLFrame == g_frame + 1;
            ID3D11ShaderResourceView* li[5] = { g_tileRT[0].srv, g_tileRT[1].srv, g_lampUseSrv ? g_lampUseSrv : g_lampSrv, g_lsSrv, lt ? g_lampLT.srv : 0 };
            ctx->PSSetShaderResources(31, 5, li);
            cb.P10[1] = lt ? 1.0f : 0.0f;
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
        }
        if (!lampsOn) { cb.P8[0] = 0; if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); } }
        ctx->PSSetShaderResources(26, 1, gi ? &g_hrt[2 + cur].srv : noSrv);
        if (!gi) { cb.P7[3] = 0; if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); } }
        D3D11_VIEWPORT hv = { 0, 0, (float)g_halfW, (float)g_halfH, 0, 1 };
        ctx->RSSetViewports(1, &hv);
        oPSSetShader(ctx, g_psSnap, 0, 0);
        Replay(ctx, g_air.dc);
        ctx->OMSetRenderTargets(1, &g_sceneRtv, 0);   // unbinds the snapshot as a target
        ctx->PSSetShaderResources(16, 10, in);         // the air pass's own inputs again
        ctx->PSSetShaderResources(26, 1, noSrv);
        ctx->PSSetShaderResources(70, 1, noSrv);
        ctx->GenerateMips(g_hrt[4].srv);
        ctx->GenerateMips(g_hrt[6].srv);
        g_radFrame = g_frame + 1;
        // reflections, half resolution (the source: the whole scene just copied, with its blurred levels)
        // the reflections' size: a share of the screen's (the game may render wider for SSAA: the screen's at most)
        UINT bw = g_bbW && g_bbW < sd.Width ? g_bbW : sd.Width, bh = g_bbH && g_bbH < sd.Height ? g_bbH : sd.Height;
        float ssrS = g_p[P_SSR + 3].v * 0.01f;
        UINT sw = (UINT)(bw * ssrS + 0.5f), sh = (UINT)(bh * ssrS + 0.5f);
        if (sw < 64) sw = 64;
        if (sh < 36) sh = 36;
        if (ssrOn && !EnsureSsrRT(sw, sh)) ssrOn = false;
        if (ssrOn) RainBind(ctx);   // the rain map for the wetness (rain.inc)
        if (ssrOn) {
            ProfMark(ctx, PR_SSR, 0);
            cb.P13[0] = (float)sw / sd.Width; cb.P13[1] = (float)sh / sd.Height; cb.P13[2] = (float)sw; cb.P13[3] = (float)sh;
            LightCB sc = cb;
            sc.size[0] = (float)sw; sc.size[1] = (float)sh;
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &sc, sizeof(sc)); ctx->Unmap(g_cb, 0); }
            ctx->OMSetRenderTargets(1, &g_ssrRT[0].rtv, 0);
            D3D11_VIEWPORT hv2 = { 0, 0, (float)sw, (float)sh, 0, 1 };
            ctx->RSSetViewports(1, &hv2);
            ID3D11ShaderResourceView* gb[4] = { g_air.depth, g_air.normal, g_air.diffuse, g_air.spec };   // albedo: wetness by material
            ctx->PSSetShaderResources(17, 4, gb);
            ctx->PSSetShaderResources(37, 1, &g_hrt[6].srv);
            ctx->PSSetShaderResources(39, 1, early ? &g_earlySrv : &g_sceneSrv);
            // 1.39: our sky as seen into the panorama (1024 x 256: azimuth, 90 .. -10 deg up; mips: blurred, the unseen filled)
            bool pano = g_p[P_SSR_SKY].v > 0.5f && g_psPano && g_camFrame[cur] == g_frame + 1;
            if (pano && !g_panoTex[1]) {
                D3D11_TEXTURE2D_DESC pd = {};
                pd.Width = 1024; pd.Height = 256; pd.MipLevels = 8; pd.ArraySize = 1; pd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; pd.SampleDesc.Count = 1;
                pd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; pd.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                for (int k = 0; k < 2; ++k)
                    if (FAILED(g_dev->CreateTexture2D(&pd, 0, &g_panoTex[k])) || FAILED(g_dev->CreateRenderTargetView(g_panoTex[k], 0, &g_panoRtv[k])) ||
                        FAILED(g_dev->CreateShaderResourceView(g_panoTex[k], 0, &g_panoSrv[k]))) { Log("sky panorama failed"); pano = false; break; }
                if (pano) Log("sky panorama: 1024 x 256 for the reflections' sky");
            }
            if (pano && g_panoSrv[1]) {
                int pc = g_frame & 1, pl = pc ^ 1;
                float zero[4] = { 0, 0, 0, 0 };
                for (int k = 0; k < 2; ++k) if (!g_panoCleared[k]) { ctx->ClearRenderTargetView(g_panoRtv[k], zero); g_panoCleared[k] = true; }
                ctx->OMSetRenderTargets(1, &g_panoRtv[pc], 0);
                D3D11_VIEWPORT pv = { 0, 0, 1024, 256, 0, 1 };
                ctx->RSSetViewports(1, &pv);
                ctx->PSSetShaderResources(67, 1, &g_panoSrv[pl]);
                ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);
                oPSSetShader(ctx, g_psPano, 0, 0);
                Replay(ctx, g_air.dc);
                ctx->PSSetShaderResources(67, 1, noSrv);
                ctx->OMSetRenderTargets(1, &g_ssrRT[0].rtv, 0);
                ctx->RSSetViewports(1, &hv2);
                ctx->GenerateMips(g_panoSrv[pc]);
                ctx->PSSetShaderResources(67, 1, &g_panoSrv[pc]);
                g_panoLast = pc;
                if (!g_panoStage) {
                    D3D11_TEXTURE2D_DESC sd2 = {};
                    sd2.Width = 8; sd2.Height = 2; sd2.MipLevels = 1; sd2.ArraySize = 1; sd2.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; sd2.SampleDesc.Count = 1;
                    sd2.Usage = D3D11_USAGE_STAGING; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    g_dev->CreateTexture2D(&sd2, 0, &g_panoStage);
                }
                if (g_panoStage) {
                    D3D11_MAPPED_SUBRESOURCE pm;
                    if (g_panoStageFrame && g_frame - g_panoStageFrame > 3 && ctx->Map(g_panoStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &pm) == S_OK) {
                        float s[4] = {};
                        for (int y = 0; y < 2; ++y) for (int x = 0; x < 8; ++x) {
                            const unsigned short* h = (const unsigned short*)((const char*)pm.pData + y * pm.RowPitch) + x * 4;
                            for (int j = 0; j < 4; ++j) s[j] += HalfToFloat(h[j]);
                        }
                        ctx->Unmap(g_panoStage, 0);
                        Log("sky panorama: %.0f%% seen, its mean %.3f %.3f %.3f", s[3] / 16 * 100, s[3] > 0 ? s[0] / s[3] : 0.0f, s[3] > 0 ? s[1] / s[3] : 0.0f, s[3] > 0 ? s[2] / s[3] : 0.0f);
                        g_panoStageFrame = 0;
                    }
                    static DWORD nextPano;
                    if (!g_panoStageFrame && (LONG)(GetTickCount() - nextPano) >= 0) {
                        nextPano = GetTickCount() + 10000;
                        ctx->CopySubresourceRegion(g_panoStage, 0, 0, 0, 0, g_panoTex[pc], 7, 0);
                        g_panoStageFrame = g_frame;
                    }
                }
            }
            sc.P21[0] = pano && g_panoSrv[1] ? 1.0f : 0.0f;
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &sc, sizeof(sc)); ctx->Unmap(g_cb, 0); }
            {   // 1.46: + the gloss and the ray's length (the smoothing, the history); the sky probe (sky.inc)
                ID3D11RenderTargetView* srt[2] = { g_ssrRT[0].rtv, g_ssrAux.rtv };
                ctx->OMSetRenderTargets(2, srt, 0);
                ctx->PSSetShaderResources(68, 1, sc.P24[1] > 0.5f ? &g_probeSrv : noSrv);
            }
            oPSSetShader(ctx, g_psSSR, 0, 0);
            Replay(ctx, g_air.dc);
            ctx->PSSetShaderResources(39, 1, noSrv);
            ctx->PSSetShaderResources(67, 2, noSrv);
            if (g_psSSRBlur) {   // smoothed, and over frames, into hrt[7 + cur] (hrt[7 + last]: last frame's)
                int last = cur ^ 1;
                bool hist = g_ssrFrame[last] == g_frame && g_camFrame[last] == g_frame;
                sc.P12[3] = hist ? 1.0f : 0.0f;
                if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &sc, sizeof(sc)); ctx->Unmap(g_cb, 0); }
                ctx->OMSetRenderTargets(1, &g_ssrRT[1 + cur].rtv, 0);
                ID3D11ShaderResourceView* bi[5] = { g_ssrRT[0].srv, 0, hist ? g_ssrRT[1 + last].srv : 0, 0, g_camRec[cur].srv };
                ctx->PSSetShaderResources(36, 5, bi);
                ctx->PSSetShaderResources(28, 1, hist ? &g_camRec[last].srv : noSrv);
                ctx->PSSetShaderResources(69, 1, &g_ssrAux.srv);   // 1.46
                oPSSetShader(ctx, g_psSSRBlur, 0, 0);
                Replay(ctx, g_air.dc);
                ctx->PSSetShaderResources(36, 5, noSrv);
                ctx->PSSetShaderResources(28, 1, noSrv);
                ctx->PSSetShaderResources(69, 1, noSrv);
                g_ssrFrame[cur] = g_frame + 1;
            }
            ctx->OMSetRenderTargets(1, &g_sceneRtv, 0);
            ctx->PSSetShaderResources(37, 1, noSrv);
            ctx->PSSetShaderResources(16, 10, in);
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
            ssrDone = true;
            ProfMark(ctx, PR_SSR, 1);
        }
        // lamp finder on the snapshot, copied for the CPU (read back a few frames later)
        if (lamps && EnsureEmit() && !g_stageFrame[g_stageNext]) {
            ID3D11RenderTargetView* et[2] = { g_emit[0].rtv, g_emit[1].rtv };
            ctx->OMSetRenderTargets(2, et, 0);
            D3D11_VIEWPORT ev = { 0, 0, (float)g_tilesW, (float)g_tilesH, 0, 1 };
            ctx->RSSetViewports(1, &ev);
            ID3D11ShaderResourceView* ein[2] = { g_camRec[cur].srv, g_hrt[4].srv };

            ctx->PSSetShaderResources(28, 2, ein);
            oPSSetShader(ctx, g_psEmit, 0, 0);
            Replay(ctx, g_air.dc);
            ctx->OMSetRenderTargets(1, &g_sceneRtv, 0);
            ctx->PSSetShaderResources(28, 2, noSrv);
            ctx->CopyResource(g_stage[g_stageNext][0], g_emit[0].t);
            ctx->CopyResource(g_stage[g_stageNext][1], g_emit[1].t);
            ctx->CopyResource(g_stage[g_stageNext][2], g_camRec[cur].t);
            g_stageFrame[g_stageNext] = g_frame + 1;
            g_stageNext = (g_stageNext + 1) % kStages;
        }
        ProfMark(ctx, PR_SNAP, 1);
    }
    if (ssrDone) {   // the reflections onto the frame, before the air (they get hazed with everything else)
        ctx->OMSetRenderTargets(1, &g_sceneRtv, 0);
        D3D11_VIEWPORT fv = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
        ctx->RSSetViewports(1, &fv);
        ctx->OMSetBlendState(g_blendDual, 0, 0xffffffff);
        ID3D11ShaderResourceView* gb[4] = { g_air.depth, g_air.normal, g_air.diffuse, g_air.spec };   // albedo: wetness by material
        ctx->PSSetShaderResources(17, 4, gb);
        ctx->PSSetShaderResources(36, 1, g_psSSRBlur ? &g_ssrRT[1 + cur].srv : &g_ssrRT[0].srv);
        // 1.42: this frame's camera record (t28: the lamp finder and the blur left none / the last frame's): without it the
        // wetness, the puddles and the roofs' dry patches were placed around the camera, riding with it
        ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);
        oPSSetShader(ctx, g_psSSRApply, 0, 0);
        Replay(ctx, g_air.dc);
        ctx->PSSetShaderResources(36, 1, noSrv);
        ctx->PSSetShaderResources(28, 1, noSrv);
        ctx->PSSetShaderResources(16, 10, in);
    }
    bool drawAir = g_view == 0 || g_view == 1 || g_view == 19 || g_view == 20 || g_view == 24;
    ctx->OMSetRenderTargets(1, &g_sceneRtv, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(g_blendAir, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->RSSetState(g_rsNoCull);
    if (drawAir) {
        bool glowOk = cb.P10[1] > 0.5f && g_lampLFrame == g_frame + 1 && g_lampGT.srv;
        if (!glowOk && cb.P10[1] > 0.5f) {
            cb.P10[1] = 0;
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
        }
        ctx->PSSetShaderResources(41, 1, glowOk ? &g_lampGT.srv : noSrv);
        oPSSetShader(ctx, g_psAir, 0, 0);
        Replay(ctx, g_air.dc);
    }
    if (g_view == 24 && g_psEditor && g_cbLamp && g_camFrame[cur] == g_frame + 1) {
        ctx->PSSetConstantBuffers(6, 1, &g_cbLamp);
        ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);   // this frame's camera
        ctx->PSSetShaderResources(33, 1, g_lampUseSrv ? &g_lampUseSrv : &g_lampSrv);
        oPSSetShader(ctx, g_psEditor, 0, 0);
        Replay(ctx, g_air.dc);
    }
    ProfMark(ctx, PR_AIR, 1);

    // restore
    ID3D11RenderTargetView* none = 0;
    ctx->OMSetRenderTargets(1, &none, 0);
    ctx->VSSetShader(sVs, 0, 0); oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl); ctx->IASetVertexBuffers(0, 2, sVb, sSt, sOf); ctx->IASetIndexBuffer(sIb, sIbF, sIbO);
    ctx->IASetPrimitiveTopology(sTop);
    ctx->VSSetConstantBuffers(0, 4, sVcb); ctx->PSSetConstantBuffers(0, 7, sPcb);
    ctx->PSSetShaderResources(0, 42, sSrv); ctx->PSSetSamplers(10, 4, sSmp);
    ctx->OMSetRenderTargets(8, sRtv, sDsv); ctx->RSSetViewports(nVp, sVp);
    ctx->OMSetBlendState(sBl, sBf, sBm); ctx->OMSetDepthStencilState(sDs, sRef); ctx->RSSetState(sRs);
    IUnknown* rel[] = { sVs, sPs, sIl, sVb[0], sVb[1], sIb, sVcb[0], sVcb[1], sVcb[2], sVcb[3], sPcb[0], sPcb[1], sPcb[2], sPcb[3],
                        sPcb[4], sPcb[5], sPcb[6], sSmp[0], sSmp[1], sSmp[2], sSmp[3], sDsv, sBl, sDs, sRs };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
    for (int i = 0; i < 8; ++i) if (sRtv[i]) sRtv[i]->Release();
    for (int i = 0; i < 42; ++i) if (sSrv[i]) sSrv[i]->Release();
    g_airDoneFrame = g_frame;
}

static void SunPassLocked(ID3D11DeviceContext* ctx, SunInfo* s, const DrawCall& dc)
{
    if (!EnsureResources(ctx)) return;
    CpuTimer cpu;
    ID3D11RenderTargetView* rtvs[8] = {};
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);
    ID3D11RenderTargetView* rtv = rtvs[0];
    ID3D11Resource* res = 0;
    ID3D11Texture2D* rt = 0;
    if (rtv) rtv->GetResource(&res);
    if (res) res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt);
    bool go = false;
    if (rt) {
        D3D11_TEXTURE2D_DESC d;
        rt->GetDesc(&d);
        g_rtW = d.Width; g_rtH = d.Height; g_rtFmt = d.Format;
        // only the main view (reflections and such are rendered smaller)
        if (g_bbW && d.Width + 8 < g_bbW) ++g_skipSmall;
        else go = d.SampleDesc.Count == 1;
    }
    bool imm = Immediate(ctx);
    // Fast: our result is blended onto the game's (dual-source blending), no copy of the frame.
    // The copy is still made for the views that show the game's own image and for captures.
    bool fast = g_p[P_FAST].v > 0.5f && g_blendDual && g_psSun[2] && !ViewNeedsCopy(g_view);
    bool needCopy = !fast || (g_captureReq && imm);
    if (go && needCopy && !EnsureCopy(rt, rtv)) {
        if (fast) needCopy = false;   // only the capture is lost
        else go = false;
    }
    if (go) {
        if (needCopy) {
            ProfMark(ctx, PR_COPY, 0);
            ctx->CopyResource(g_copy, rt);
            ProfMark(ctx, PR_COPY, 1);
        }
        ++g_frameSun;
        if (g_wantDump && g_dumpsLeft > 0 && imm) { g_wantDump = 0; --g_dumpsLeft; DumpPass(ctx, s); }

        // save what we touch
        ID3D11ShaderResourceView* savedSrv[15], *savedLampSrv[5];
        ID3D11SamplerState* savedSmp[4];
        ID3D11Buffer* savedCb = 0;
        ID3D11BlendState* savedBlend = 0;
        FLOAT savedFactor[4];
        UINT savedMask = 0;
        ctx->PSGetShaderResources(16, 15, savedSrv);
        ctx->PSGetShaderResources(31, 5, savedLampSrv);
        ctx->PSGetSamplers(10, 4, savedSmp);
        ctx->PSGetConstantBuffers(5, 1, &savedCb);
        ctx->OMGetBlendState(&savedBlend, savedFactor, &savedMask);

        // the game's inputs at our fixed slots
        ID3D11ShaderResourceView* in[9] = { needCopy ? g_copySrv : 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        int slots[8] = { s->tDepth, s->tNormal, s->tDiffuse, s->tSpec, s->tAdd, s->tShadow, s->tCloud, s->tVfx };
        for (int k = 0; k < 8; ++k) if (slots[k] >= 0) ctx->PSGetShaderResources(slots[k], 1, &in[k + 1]);
        ID3D11SamplerState* cmp = 0;
        if (s->sCmp >= 0) ctx->PSGetSamplers(s->sCmp, 1, &cmp);
        bool cmpGreater = false;
        if (cmp) {
            D3D11_SAMPLER_DESC sdsc;
            cmp->GetDesc(&sdsc);
            cmpGreater = sdsc.ComparisonFunc == D3D11_COMPARISON_GREATER || sdsc.ComparisonFunc == D3D11_COMPARISON_GREATER_EQUAL;
        }
        float smW = 2048, smH = 6144;
        if (in[6]) {
            ID3D11Resource* sr = 0;
            in[6]->GetResource(&sr);
            g_sunShadowTex = sr;
            ID3D11Texture2D* st = 0;
            if (sr) { sr->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&st); sr->Release(); }
            if (st) { D3D11_TEXTURE2D_DESC sd; st->GetDesc(&sd); smW = (float)sd.Width; smH = (float)sd.Height; st->Release(); }
        }

        LightCB cb;
        memset(&cb, 0, sizeof(cb));
        cb.L0[0] = s->oKeyCol; cb.L0[1] = s->oKeyDir; cb.L0[2] = s->oC; cb.L0[3] = s->oC2;  // -1 = not in this variant
        cb.L1[0] = s->oArr4; cb.L1[1] = s->oScale < 0 ? 0 : s->oScale; cb.L1[2] = s->oW2L < 0 ? 0 : s->oW2L; cb.L1[3] = s->oView;
        int flags = 0;
        if (s->tShadow >= 0 && in[6] && cmp) flags |= 1;
        if (s->tSpec >= 0 && in[4]) flags |= 2;
        if (s->tAdd >= 0 && in[5]) flags |= 4;
        if (s->tCloud >= 0 && s->oMask >= 0 && in[7]) flags |= 8;
        if (cmpGreater) flags |= 16;
        if (s->lambert) flags |= 32;
        if (s->ashikhmin) flags |= 64;
        if (s->tVfx >= 0 && in[8]) flags |= 128;
        ID3D11ShaderResourceView* irr = 0;   // the game's sky-light cube at our t63 (modern lighting)
        if (s->tIrr >= 0) ctx->PSGetShaderResources(s->tIrr, 1, &irr);
        if (irr) { flags |= 512; ctx->PSSetShaderResources(63, 1, &irr); irr->Release(); }
        bool airLate = g_airWorks && imm;
        if (airLate) flags |= 256;
        cb.L2[0] = s->oCamDir;  // -1: no camera direction (no-shadow variant)
        cb.L2[1] = s->oMask < 0 ? 0 : s->oMask;
        cb.L2[2] = flags;
        cb.L2[3] = g_view;
        cb.size[0] = (float)g_rtW; cb.size[1] = (float)g_rtH; cb.size[2] = smW; cb.size[3] = smH;
        cb.P0[0] = g_p[0].v; cb.P0[1] = g_p[1].v; cb.P0[2] = g_p[2].v; cb.P0[3] = g_p[3].v;
        cb.P1[0] = g_p[4].v; cb.P1[1] = g_p[5].v; cb.P1[2] = g_p[6].v; cb.P1[3] = g_p[7].v;
        cb.P2[0] = g_p[8].v; cb.P2[1] = g_p[9].v; cb.P2[2] = g_p[10].v; cb.P2[3] = (float)(g_frame % 64);
        cb.P3[0] = g_p[11].v * 0.01f;
        cb.P3[1] = g_p[12].v;
        cb.P3[2] = g_p[13].v;
        cb.P3[3] = g_p[14].v;
        cb.P4[0] = g_p[15].v;
        cb.P4[1] = g_p[16].v * g_hazeMul;
        cb.P4[2] = g_p[17].v;
        cb.P4[3] = g_p[18].v;
        cb.P5[0] = g_p[19].v;
        cb.P5[1] = g_p[20].v;
        cb.P6[0] = g_p[P_BOUNCE].v;
        cb.P6[1] = g_p[P_BOUNCE_R].v;
        cb.P7[0] = g_p[P_BOUNCE_ALL].v;
        cb.P7[1] = g_p[P_BOUNCE_HIST].v;
        cb.P8[0] = g_p[P_LAMP].v;
        cb.P8[1] = g_p[P_LAMP_THR].v;
        cb.P8[2] = g_p[P_LAMP_RADIUS].v;
        cb.P7[2] = g_p[P_LAMP_REACH].v + g_p[P_LAMP_RADIUS].v;
        cb.P8[3] = g_p[P_LAMP_DAY].v;
        cb.P9[0] = g_p[P_LAMP_SHADOW].v;
        cb.P9[1] = g_p[P_LAMP_SHADOW_T].v;
        cb.P9[2] = g_p[P_NIGHT_AMB].v;
        cb.P10[0] = g_p[P_SOFT_SUN].v;
        cb.P11[0] = g_p[P_SKY_OCC].v; cb.P11[1] = g_p[P_SKY_OCC + 1].v; cb.P11[2] = g_p[109].v; cb.P11[3] = g_p[112].v;   // car paint reflections, puddles
        cb.P12[0] = g_p[P_SSR].v; cb.P12[1] = g_p[P_SSR + 1].v; cb.P12[2] = g_p[P_SSR + 2].v;
        cb.P14[0] = g_p[P_GLOW].v; cb.P14[1] = g_p[P_GLOW + 1].v; cb.P14[2] = g_p[P_GLOW + 2].v;
        cb.P16[0] = g_p[P_PBR].v; cb.P16[1] = g_p[P_PBR + 1].v; cb.P16[2] = g_p[P_PBR + 2].v; cb.P16[3] = g_p[P_PBR + 3].v;
        cb.P17[0] = g_p[P_REL].v; cb.P17[1] = g_p[P_REL + 1].v; cb.P17[2] = g_p[P_REL + 2].v * 0.01f; cb.P17[3] = g_p[P_REL + 3].v;
        for (int j = 0; j < 3; ++j) { cb.P18[j] = g_sunOurs[j]; cb.P19[j] = g_sunCol[j]; }   // our sun (SunPath)
        cb.P18[3] = g_sunW; cb.P19[3] = g_sunActive ? 1.0f : 0.0f;
        cb.P20[0] = g_sunG; cb.P20[1] = g_sunMoonMaps ? 1.0f : 0.0f; cb.P20[2] = g_skyLum; cb.P20[3] = g_p[P_SSR_RAY].v;
        cb.P21[1] = g_p[P_WET_OTHER].v;
        cb.P21[3] = g_p[P_NOISE + 3].v;
        cb.P22[0] = g_p[P_NOISE + 5].v; cb.P22[1] = g_p[P_NOISE + 4].v; cb.P22[2] = g_p[P_NOISE + 1].v; cb.P22[3] = g_p[P_NOISE].v;
        cb.P23[0] = g_p[P_NOISE + 2].v; cb.P23[1] = g_p[P_NOISE + 7].v;
        cb.P24[0] = g_p[P_NOISE + 8].v;   // 1.46: the history's clip; the sky probe drawn of late (sky.inc)
        cb.P24[1] = g_probeSrv && g_probeFrame && g_frame + 1 - g_probeFrame < 120 ? 1.0f : 0.0f;
        {   // 1.41: the sky panorama's fading this frame (~8 s half)
            static LARGE_INTEGER lastQ; LARGE_INTEGER q; QueryPerformanceCounter(&q);
            float dt = lastQ.QuadPart && g_qpf.QuadPart ? (float)(q.QuadPart - lastQ.QuadPart) / g_qpf.QuadPart : 0.016f;
            lastQ = q;
            dt = dt < 0 ? 0 : dt > 0.25f ? 0.25f : dt;
            cb.P21[2] = expf(-dt * 0.01155f);   // (1.45: ~60 s half: the sky kept by its level, below)
            cb.P23[2] = g_sunActive ? 0.02f + g_skyLum : 1.0f;   // 1.45: the sky's level now (the panorama kept divided by it)
        }
        cb.P9[3] = g_p[P_LAMP_SHADOW_MAPS].v > 0.5f && g_lsSrv ? g_p[P_LAMP_SIZE].v : 0.0f;
        D3D11_MAPPED_SUBRESOURCE m;
        ID3D11SamplerState* smp[4] = { cmp, g_sPoint, g_sWrap, g_sText };
        ID3D11ShaderResourceView* none[6] = {};
        ctx->PSSetShaderResources(16, 9, in);
        ctx->PSSetShaderResources(25, 6, none);
        ctx->PSSetSamplers(10, 4, smp);
        ctx->PSSetConstantBuffers(5, 1, &g_cb);
        SkyBindForSun(ctx);   // our clouds' cover: their shadows (sky.inc)
        RainBind(ctx);        // the rain map (rain.inc)
        ctx->PSSetShaderResources(56, 1, &g_expoSrv);   // the game's exposure (the debug views)
        ctx->OMSetBlendState(0, 0, 0xffffffff);

        // 1) half resolution: AO, light shafts, contact shadows and bounce light (the screen-space parts)
        bool half = g_halfRes && g_psHalf && g_psGIBlur && EnsureHalf(g_rtW / 2, g_rtH / 2);
        if (half && imm) LampsReadBack(ctx, g_frame);
        bool lampsByDay = g_p[P_LAMP_DAY].v > 0 || g_view == 24 || g_weOn;   // (in full day the lamps are off: sunshadow.inc)
        g_lampUse = half && imm && (g_p[P_LAMP].v > 0 || g_view == 24) && (!g_ssFullDay || lampsByDay) ? LampsUpload(ctx) : 0;
        bool bounce = half && g_p[P_BOUNCE].v > 0;
        cb.P7[3] = bounce ? 1.0f : 0.0f;
        ID3D11Buffer* savedCb6 = 0;
        ctx->PSGetConstantBuffers(6, 1, &savedCb6);
        if (g_cbLamp) ctx->PSSetConstantBuffers(6, 1, &g_cbLamp);
        if (!half) cb.P6[0] = 0;
        int cur = g_frame & 1, last = cur ^ 1;
        bool ssOk = false;
        if (half) {
            // what the previous frame left: its camera, its finished image and its bounce light
            bool prevCam = imm && g_psCam && g_camFrame[last] == g_frame;           // stored as frame + 1
            bool prevRad = prevCam && g_radFrame == g_frame;
            bool hist = prevCam && g_giFrame[last] == g_frame && g_p[P_BOUNCE_HIST].v > 0;
            LightCB hc = cb;
            hc.size[0] = (float)g_halfW; hc.size[1] = (float)g_halfH;
            hc.P5[3] = 0;
            hc.P6[2] = prevRad ? 1.0f : 0.0f;
            hc.P6[3] = hist ? 1.0f : 0.0f;
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &hc, sizeof(hc)); ctx->Unmap(g_cb, 0); }
            D3D11_VIEWPORT svp[16]; UINT nvp = 16;
            ctx->RSGetViewports(&nvp, svp);
            D3D11_VIEWPORT hv = { 0, 0, (float)g_halfW, (float)g_halfH, 0, 1 };
            ProfMark(ctx, PR_HALF, 0);
            if (imm && g_psCam) {   // this frame's camera, for the next frame (bounce, lamps)
                D3D11_VIEWPORT cv = { 0, 0, 4, 2, 0, 1 };
                ctx->OMSetRenderTargets(1, &g_camRec[cur].rtv, 0);
                ctx->RSSetViewports(1, &cv);
                oPSSetShader(ctx, g_psCam, 0, 0);
                Replay(ctx, dc);
                g_camFrame[cur] = g_frame + 1;
                SsNote(ctx, g_camRec[cur].t, s->oKeyDir, s->oKeyCol);   // our sun shadows' placement, day / night (sunshadow.inc)
                // the lamps' final buffer: the traffic cars' lights put on their cars as drawn this frame
                g_lampUseSrv = g_lampSrv;
                if (g_lampUse > 0 && g_psLampFix && g_lampSrv) {
                    if (!g_lampFixBuf) {
                        D3D11_BUFFER_DESC fb = {};
                        fb.ByteWidth = kLampsUp * 4 * 16; fb.Usage = D3D11_USAGE_DEFAULT; fb.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        D3D11_RENDER_TARGET_VIEW_DESC rv = {};
                        rv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; rv.ViewDimension = D3D11_RTV_DIMENSION_BUFFER;
                        rv.Buffer.FirstElement = 0; rv.Buffer.NumElements = kLampsUp * 4;
                        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
                        sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = kLampsUp * 4;
                        if (FAILED(g_dev->CreateBuffer(&fb, 0, &g_lampFixBuf)) || FAILED(g_dev->CreateRenderTargetView(g_lampFixBuf, &rv, &g_lampFixRtv)) ||
                            FAILED(g_dev->CreateShaderResourceView(g_lampFixBuf, &sv, &g_lampFixSrv))) {
                            Log("lamp fix buffer failed"); g_psLampFix = 0;
                        }
                    }
                    if (g_lampFixRtv && g_lampFixSrv) {
                        ctx->OMSetRenderTargets(1, &g_lampFixRtv, 0);
                        D3D11_VIEWPORT fv = { 0, 0, (float)(kLampsUp * 4), 1, 0, 1 };
                        ctx->RSSetViewports(1, &fv);
                        ID3D11ShaderResourceView* fi[2] = { g_lampSrv, g_carMatSrv };
                        ctx->PSSetShaderResources(45, 2, fi);
                        oPSSetShader(ctx, g_psLampFix, 0, 0);
                        Replay(ctx, dc);
                        ID3D11RenderTargetView* nr = 0;
                        ctx->OMSetRenderTargets(1, &nr, 0);
                        ID3D11ShaderResourceView* nz[2] = {};
                        ctx->PSSetShaderResources(45, 2, nz);
                        g_lampUseSrv = g_lampFixSrv;
                    }
                }
                // which lamps light which screen tile, with this frame's camera
                if (g_lampUse > 0 && g_psTiles && g_lampSrv && EnsureTiles(g_rtW, g_rtH)) {
                    ID3D11RenderTargetView* tr[2] = { g_tileRT[0].rtv, g_tileRT[1].rtv };
                    ctx->OMSetRenderTargets(2, tr, 0);
                    D3D11_VIEWPORT tv = { 0, 0, (float)g_tileW, (float)g_tileH, 0, 1 };
                    ctx->RSSetViewports(1, &tv);
                    ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);
                    ctx->PSSetShaderResources(33, 1, g_lampUseSrv ? &g_lampUseSrv : &g_lampSrv);
                    oPSSetShader(ctx, g_psTiles, 0, 0);
                    Replay(ctx, dc);
                    g_tileFrame = g_frame + 1;
                }
            }
            ssOk = SsMaskPass(ctx, dc, prevCam ? g_camRec[last].srv : 0);   // our sun shadows' mask (sunshadow.inc)
            ID3D11ShaderResourceView* prevIn[3] = { prevCam ? g_camRec[last].srv : 0, prevRad ? g_hrt[4].srv : 0,
                                                    hist ? g_hrt[2 + last].srv : 0 };
            ctx->PSSetShaderResources(28, 3, prevIn);
            ID3D11RenderTargetView* hr[2] = { g_hrt[0].rtv, g_hrt[1].rtv };
            ctx->OMSetRenderTargets(2, hr, 0);
            ctx->RSSetViewports(1, &hv);
            oPSSetShader(ctx, g_psHalf, 0, 0);
            Replay(ctx, dc);
            // the occlusion smoothed (sky occlusion is noisy at 24 samples); 1.49: always (AO's 8 samples too), and over frames
            bool aSmooth = g_psABlur && (g_p[P_SKY_OCC].v > 0 || g_p[6].v > 0);
            if (aSmooth) {
                bool aoHist = prevCam && g_aoFrame[last] == g_frame;   // last frame's, with its camera
                hc.P24[3] = aoHist ? 1.0f : 0.0f;
                if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &hc, sizeof(hc)); ctx->Unmap(g_cb, 0); }
                ctx->OMSetRenderTargets(1, &g_hrt[9 + cur].rtv, 0);
                ctx->PSSetShaderResources(25, 1, &g_hrt[0].srv);
                ctx->PSSetShaderResources(71, 1, aoHist ? &g_hrt[9 + last].srv : none);
                oPSSetShader(ctx, g_psABlur, 0, 0);
                Replay(ctx, dc);
                ctx->PSSetShaderResources(25, 1, none);
                ctx->PSSetShaderResources(71, 1, none);
                g_aoFrame[cur] = g_frame + 1;
            }
            ProfMark(ctx, PR_HALF, 1);
            if (bounce) {
                ProfMark(ctx, PR_BLUR, 0);
                ctx->OMSetRenderTargets(1, &g_hrt[2 + cur].rtv, 0);
                ctx->PSSetShaderResources(27, 1, &g_hrt[1].srv);
                oPSSetShader(ctx, g_psGIBlur, 0, 0);
                Replay(ctx, dc);
                ctx->PSSetShaderResources(27, 4, none);
                if (imm) g_giFrame[cur] = g_frame + 1;
                ProfMark(ctx, PR_BLUR, 1);
            }
            ctx->PSSetShaderResources(28, 3, none);
            // lamp light alone, at the shown resolution (a game rendering wider for SSAA gets it at half the cost)
            if (g_lampUse > 0 && g_psLamps && g_tileFrame == g_frame + 1 && g_camFrame[cur] == g_frame + 1) {
                UINT lw = g_bbW && g_bbW < g_rtW ? g_bbW : g_rtW, lh = g_bbH && g_bbH < g_rtH ? g_bbH : g_rtH;
                if (g_p[P_LAMP_RES].v > 0.5f) { lw = (lw + 1) / 2; lh = (lh + 1) / 2; }
                if (EnsureLampLT(lw, lh)) {
                    ProfMark(ctx, PR_LAMPS, 0);
                    LightCB lcb = cb;
                    lcb.size[0] = (float)lw; lcb.size[1] = (float)lh;
                    lcb.P9[3] = g_p[P_LAMP_SHADOW_MAPS].v > 0.5f && g_lsSrv ? g_p[P_LAMP_SIZE].v : 0.0f;
                    bool hl = HlReady();
                    lcb.P15[0] = hl ? g_p[P_HL_BEAM].v : 0.0f; lcb.P15[1] = hl && g_hlShadow ? 1.0f : 0.0f; lcb.P15[2] = g_p[P_HL_LEN].v;
                    HlBind(ctx, hl);
                    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &lcb, sizeof(lcb)); ctx->Unmap(g_cb, 0); }
                    ID3D11RenderTargetView* lt2[3] = { g_lampLT.rtv, g_lampGT.rtv, g_lampBT.rtv };
                    ctx->OMSetRenderTargets(3, lt2, 0);
                    D3D11_VIEWPORT lv = { 0, 0, (float)lw, (float)lh, 0, 1 };
                    ctx->RSSetViewports(1, &lv);
                    ctx->PSSetShaderResources(28, 1, &g_camRec[cur].srv);
                    ID3D11ShaderResourceView* li[4] = { g_tileRT[0].srv, g_tileRT[1].srv, g_lampUseSrv ? g_lampUseSrv : g_lampSrv, g_lsSrv };
                    ctx->PSSetShaderResources(31, 4, li);
                    oPSSetShader(ctx, g_psLamps, 0, 0);
                    Replay(ctx, dc);
                    ctx->PSSetShaderResources(28, 1, none);
                    if (hl) HlBind(ctx, false);
                    g_lampLFrame = g_frame + 1;
                    ProfMark(ctx, PR_LAMPS, 1);
                }
            }
            ctx->OMSetRenderTargets(8, rtvs, dsv);
            ctx->RSSetViewports(nvp, svp);
            ID3D11ShaderResourceView* hs[2] = { aSmooth ? g_hrt[9 + cur].srv : g_hrt[0].srv, bounce ? g_hrt[2 + cur].srv : 0 };
            ctx->PSSetShaderResources(25, 2, hs);
            cb.P5[3] = 1;
        }
        if (!bounce) cb.P6[0] = 0;
        cb.P10[2] = ssOk ? 1.0f : 0.0f;   // our sun shadows' mask bound (t58, t59)
        // 2) full resolution: lighting, shadows (and the air when it cannot be drawn later)
        if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
        if (fast) {
            ctx->OMSetRenderTargets(1, &rtv, dsv);   // dual-source blending works on target 0 alone
            ctx->OMSetBlendState(g_blendDual, 0, 0xffffffff);
        }
        ProfMark(ctx, PR_SUN, 0);
        if (g_lampUse > 0 && g_tileFrame == g_frame + 1) {
            bool lt = g_lampLFrame == g_frame + 1;
            ID3D11ShaderResourceView* li[5] = { g_tileRT[0].srv, g_tileRT[1].srv, g_lampUseSrv ? g_lampUseSrv : g_lampSrv, g_lsSrv, lt ? g_lampLT.srv : 0 };
            ctx->PSSetShaderResources(31, 5, li);
            cb.P10[1] = lt ? 1.0f : 0.0f;
            cb.P10[2] = (float)g_lampLW; cb.P10[3] = (float)g_lampLH;
            ID3D11ShaderResourceView* gbt[2] = { lt ? g_lampGT.srv : 0, lt ? g_lampBT.srv : 0 };
            ctx->PSSetShaderResources(41, 2, gbt);
            if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
        } else cb.P8[0] = 0, g_lampUse = 0;
        if (cb.P8[0] == 0 && SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(g_cb, 0); }
        oPSSetShader(ctx, g_psSun[(fast ? 2 : 0) + (s->writesDepth ? 1 : 0)], 0, 0);
        Replay(ctx, dc);
        ProfMark(ctx, PR_SUN, 1);
        ctx->PSSetShaderResources(25, 6, none);
        ctx->OMSetBlendState(0, 0, 0xffffffff);
        if (airLate) {
            D3D11_RENDER_TARGET_VIEW_DESC rv;
            rtv->GetDesc(&rv);
            SaveAir(ctx, dc, cb, in[1], in[3], in[4], in[2], in[6], cmp, rt, rv.Format);
        }
        if (g_captureReq && imm && needCopy) { g_captureReq = 0; Capture(ctx, s, dc, &cb, rtv, dsv); }

        // restore
        if (ssOk) SsUnbind(ctx);
        ctx->OMSetRenderTargets(8, rtvs, dsv);
        oPSSetShader(ctx, s->ps, 0, 0);
        ctx->PSSetShaderResources(16, 15, savedSrv);
        ctx->PSSetShaderResources(31, 5, savedLampSrv);
        for (int k = 0; k < 5; ++k) if (savedLampSrv[k]) savedLampSrv[k]->Release();
        ctx->PSSetSamplers(10, 4, savedSmp);
        ctx->PSSetConstantBuffers(5, 1, &savedCb);
        ctx->PSSetConstantBuffers(6, 1, &savedCb6);
        if (savedCb6) savedCb6->Release();
        ctx->OMSetBlendState(savedBlend, savedFactor, savedMask);
        for (int k = 0; k < 15; ++k) if (savedSrv[k]) savedSrv[k]->Release();
        for (int k = 1; k < 9; ++k) if (in[k]) in[k]->Release();
        for (int k = 0; k < 4; ++k) if (savedSmp[k]) savedSmp[k]->Release();
        if (savedCb) savedCb->Release();
        if (savedBlend) savedBlend->Release();
        if (cmp) cmp->Release();
    }
    if (rt) rt->Release();
    if (res) res->Release();
    for (int k = 0; k < 8; ++k) if (rtvs[k]) rtvs[k]->Release();
    if (dsv) dsv->Release();
}

// Does a pixel shader's code discard pixels (alpha-tested cut-outs)? Its SHDR / SHEX chunk walked instruction by
// instruction (opcode in the low 11 bits, length in bits 24..30; custom data: length in the next token).
static bool PsDiscards(const void* code, SIZE_T size)
{
    const unsigned char* b = (const unsigned char*)code;
    if (size < 32 || memcmp(b, "DXBC", 4)) return true;   // unknown: keep it
    unsigned nch = *(const unsigned*)(b + 28);
    for (unsigned c = 0; c < nch && 32 + c * 4 + 4 <= size; ++c) {
        unsigned off = *(const unsigned*)(b + 32 + c * 4);
        if (off + 8 > size) continue;
        if (memcmp(b + off, "SHDR", 4) && memcmp(b + off, "SHEX", 4)) continue;
        unsigned len = *(const unsigned*)(b + off + 4);
        const unsigned* t = (const unsigned*)(b + off + 8);
        unsigned n = len / 4;
        if (off + 8 + len > size || n < 2) return true;
        for (unsigned i = 2; i < n;) {
            unsigned tok = t[i], op = tok & 0x7ff, l = op == 0x35 ? (i + 1 < n ? t[i + 1] : 0) : (tok >> 24) & 0x7f;
            if (op == 0x0d) return true;   // discard
            if (!l) return true;           // not understood: keep it
            i += l;
        }
        return false;
    }
    return true;
}
static ID3D11PixelShader* volatile g_psCut[4096];   // the game's pixel shaders that discard (open addressing)
static bool PsIsCut(ID3D11PixelShader* ps)
{
    unsigned h = ((unsigned)(size_t)ps >> 4) * 2654435761u;
    for (int k = 0; k < 64; ++k) { ID3D11PixelShader* p = g_psCut[(h + k) & 4095]; if (p == ps) return true; if (!p) return false; }
    return true;
}

static void TxNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps);   // texreplace.inc
static void MatNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps);  // matpom.inc
static ID3D11PixelShader* MatSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);
static void BldNotePs(const void* code, SIZE_T size, ID3D11PixelShader* ps);  // matheight.inc
static ID3D11PixelShader* BldSubstitute(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps);
static HRESULT STDMETHODCALLTYPE HkCreatePS(ID3D11Device* dev, const void* code, SIZE_T size, ID3D11ClassLinkage* cl, ID3D11PixelShader** out)
{
    HRESULT hr = oCreatePS(dev, code, size, cl, out);
    static bool tried;
    if (!tried) { tried = true; if (!LoadCompiler()) Log("no shader compiler: NewLook stays off"); }
    if (SUCCEEDED(hr) && out && *out && code && g_reflect) { ExamineShader(code, size, *out); NameShader(code, size, *out); TxNotePs(code, size, *out); CoronaNote(code, size, *out); }
    if (SUCCEEDED(hr) && out && *out && code) FlareNote(code, size, *out, false);   // the game's sun flare (sunshadow.inc)
    if (SUCCEEDED(hr) && out && *out && code) { MatNotePs(code, size, *out); BldNotePs(code, size, *out); }
    if (SUCCEEDED(hr) && out && *out && code && PsDiscards(code, size)) {
        unsigned h = ((unsigned)(size_t)*out >> 4) * 2654435761u;
        for (int k = 0; k < 64; ++k) if (!InterlockedCompareExchangePointer((PVOID volatile*)&g_psCut[(h + k) & 4095], *out, 0)) break;
    }
    return hr;
}

static void STDMETHODCALLTYPE HkPSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n)
{
    SunInfo* s = FindSun(ps);
    if (ps && g_enabled && g_airWorks && g_air.frame == g_frame && g_airDoneFrame != g_frame && Immediate(ctx)) {
        for (int k = 0; k < 16 && g_postPS[k]; ++k)
            if (g_postPS[k] == ps) { EnterCriticalSection(&g_passLock); AirPass(ctx); LeaveCriticalSection(&g_passLock); break; }
    }
    *CurSlot(ctx) = s;
    if (Immediate(ctx)) g_lastPS = ps;
    InterlockedIncrement(&g_psCalls);
    if (s) InterlockedIncrement(ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? &g_sunBindsImm : &g_sunBindsDef);
    if (s && g_frame > 300) g_tracing = 1;
    if (g_tracing) Trace(s ? "PSSetShader SUN" : "PSSetShader", ctx, ps);
    ID3D11PixelShader* use = BldSubstitute(ctx, MatSubstitute(ctx, ps));   // game material shaders with parallax (matpom.inc, matheight.inc)
    oPSSetShader(ctx, SkySubstitute(ctx, g_lightPSN ? HlSubstitute(ctx, use) : use), ci, n);   // the headlights: ours with a soft shadow
}

static bool Immediate(ID3D11DeviceContext* ctx)
{
    return ctx->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

static void Replay(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    switch (d.kind) {
    case DK_DRAW: oDraw(ctx, d.a, d.b); break;
    case DK_INDEXED: oDrawIndexed(ctx, d.a, d.b, d.base); break;
    case DK_INSTANCED: oDrawInstanced(ctx, d.a, d.b, d.c, d.e); break;
    case DK_INDEXED_INSTANCED: oDrawIndexedInstanced(ctx, d.a, d.b, d.c, d.base, d.e); break;
    }
}

static volatile LONG g_frameTrace;      // 1 = log every draw of the next frame (set by Shift+U)
static const unsigned char* VsSumOf(void* vs);   // shadowrecon.inc
static void TraceDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    static int lines;
    static ID3D11PixelShader* lastPs;
    static UINT lastW;
    if (lines > 2500) return;
    ID3D11RenderTargetView* rtv = 0;
    ctx->OMGetRenderTargets(1, &rtv, 0);
    UINT w = 0, h = 0;
    int fmt = 0;
    if (rtv) {
        ID3D11Resource* r = 0;
        rtv->GetResource(&r);
        ID3D11Texture2D* t = 0;
        if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
        if (t) { D3D11_TEXTURE2D_DESC td; t->GetDesc(&td); w = td.Width; h = td.Height; fmt = td.Format; t->Release(); }
        rtv->Release();
    }
    if (!w || (g_lastPS == lastPs && w == lastW)) return;   // depth-only (shadow maps) and repeats
    lastPs = g_lastPS; lastW = w;
    ++lines;
    ID3D11VertexShader* vs = 0;   // (1.36: and the VS: the first bytes of its checksum, as the dumps' DXBC)
    ctx->VSGetShader(&vs, 0, 0);
    const unsigned char* sum = VsSumOf(vs);
    if (vs) vs->Release();
    int bl = 0, dw = 0;   // (1.47: blending on (src / dest), depth written)
    {
        ID3D11BlendState* bs = 0; float bf[4]; UINT bm;
        ctx->OMGetBlendState(&bs, bf, &bm);
        if (bs) { D3D11_BLEND_DESC bd; bs->GetDesc(&bd); bs->Release(); if (bd.RenderTarget[0].BlendEnable) bl = bd.RenderTarget[0].SrcBlend * 100 + bd.RenderTarget[0].DestBlend; }
        ID3D11DepthStencilState* ds = 0; UINT ref;
        ctx->OMGetDepthStencilState(&ds, &ref);
        dw = 1;
        if (ds) { D3D11_DEPTH_STENCIL_DESC dd; ds->GetDesc(&dd); ds->Release(); dw = dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL; }
    }
    Log("frame: draw k%d n%u rt %ux%u f%d ps %p %s | vs %02x%02x%02x%02x | blend %d dw %d%s", (int)d.kind, d.a, w, h, fmt, g_lastPS, PsNameOf(g_lastPS),
        sum ? sum[0] : 0, sum ? sum[1] : 0, sum ? sum[2] : 0, sum ? sum[3] : 0, bl, dw, g_earlyFrame == g_frame + 1 ? " (after the reflections' copy)" : "");
}

// Is the bound render target the main view (not a reflection or another small target)?
static bool MainTarget(ID3D11DeviceContext* ctx)
{
    ID3D11RenderTargetView* v = 0;
    ctx->OMGetRenderTargets(1, &v, 0);
    if (!v) return false;
    ID3D11Resource* r = 0;
    v->GetResource(&r);
    v->Release();
    ID3D11Texture2D* t = 0;
    if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
    bool main = false;
    if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); main = !(g_bbW && d.Width + 8 < g_bbW); t->Release(); }
    return main;
}

#include "objtools.inc"
#include "proplink.inc"
#include "autolamps.inc"
#include "shadowrecon.inc"
#include "skyrecon.inc"
#include "lampshadow.inc"
#include "grade.inc"
#include "headlight.inc"
#include "carlights.inc"
#include "sky.inc"
#include "rain.inc"
#include "gamewet.inc"
#include "sunshadow.inc"
#include "texreplace.inc"
#include "matpom.inc"
#include "matheight.inc"

// before a draw: start the timer of the game's own main sun-pass draw
// the game's exposure: a tone-mapping shader reads a 1x1 texture (w = the exposure); copied into ours once a frame
static void ExpoGrab(ID3D11DeviceContext* ctx)
{
    if (!g_lastPS || g_expoFrame == g_frame) return;
    bool tone = false;
    for (int k = 0; k < 8 && g_tonePS[k]; ++k) if (g_tonePS[k] == g_lastPS) { tone = true; break; }
    if (!tone) return;
    ID3D11ShaderResourceView* v[4] = {};
    ctx->PSGetShaderResources(0, 4, v);
    for (int k = 0; k < 4; ++k) {
        if (!v[k]) continue;
        ID3D11Resource* r = 0;
        v[k]->GetResource(&r);
        ID3D11Texture2D* t = 0;
        if (r && SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t))) {
            D3D11_TEXTURE2D_DESC td;
            t->GetDesc(&td);
            if (td.Width == 1 && td.Height == 1 && td.ArraySize == 1 && td.SampleDesc.Count == 1 && g_expoFrame != g_frame) {
                // a debug view: the game's exposure held at 1 (1.13): its eye adaptation would chase the debug picture (it
                // brightened and dimmed by itself), and the picture is shown as its values are
                if (g_view != 0 && g_view != 1 && g_view != 24) {
                    unsigned short h1[4] = { 0x3C00, 0x3C00, 0x3C00, 0x3C00 };
                    float f1[4] = { 1, 1, 1, 1 };
                    int fv = (int)td.Format;
                    const void* one = fv >= 9 && fv <= 14 ? (const void*)h1 : fv >= 1 && fv <= 4 ? (const void*)f1 : 0;
                    if (one) { if (oUpdSub) oUpdSub(ctx, t, 0, 0, one, 16, 16); else ctx->UpdateSubresource(t, 0, 0, one, 16, 16); }
                }
                if (g_expoTex) {   // a different format after a reset: make it again
                    D3D11_TEXTURE2D_DESC od; g_expoTex->GetDesc(&od);
                    if (od.Format != td.Format || od.MipLevels != td.MipLevels) {
                        g_expoSrv->Release(); g_expoTex->Release(); g_expoSrv = 0; g_expoTex = 0;
                        if (g_expoStage) { g_expoStage->Release(); g_expoStage = 0; }
                    }
                }
                if (!g_expoTex) {
                    D3D11_TEXTURE2D_DESC nd = td;
                    nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
                    D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                    v[k]->GetDesc(&sd);
                    if (SUCCEEDED(g_dev->CreateTexture2D(&nd, 0, &g_expoTex)) && FAILED(g_dev->CreateShaderResourceView(g_expoTex, &sd, &g_expoSrv))) {
                        g_expoTex->Release(); g_expoTex = 0;
                    }
                    nd.Usage = D3D11_USAGE_STAGING; nd.BindFlags = 0; nd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (g_expoTex) g_dev->CreateTexture2D(&nd, 0, &g_expoStage);
                    Log("exposure texture: format %d, slot %d", td.Format, k);
                }
                if (g_expoTex) {
                    ctx->CopyResource(g_expoTex, t);
                    g_expoFrame = g_frame;
                    // a readback now and then (for the log, a frame late: no stall)
                    if (g_expoStage && g_view != 0 && g_frame % 600 == 1 && (td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT)) {
                        D3D11_MAPPED_SUBRESOURCE m;
                        if (SUCCEEDED(ctx->Map(g_expoStage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
                            float w;
                            if (td.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) w = ((const float*)m.pData)[3];
                            else {
                                unsigned short h = ((const unsigned short*)m.pData)[3];
                                unsigned e = (h >> 10) & 31, f = h & 1023;
                                w = e == 0 ? f / 1024.0f / 16384.0f : (float)ldexp(1.0 + f / 1024.0, (int)e - 15);
                            }
                            ctx->Unmap(g_expoStage, 0);
                            Log("exposure: %.3f (debug views x %.3f)", w, w > 1e-4f ? 1 / w : 1.0f);
                        }
                        ctx->CopyResource(g_expoStage, g_expoTex);
                    }
                }
            }
            t->Release();
        }
        if (r) r->Release();
    }
    for (int k = 0; k < 4; ++k) if (v[k]) v[k]->Release();
}

// our sun shadow maps (sunshadow.inc): a shadow draw of the game's drawn into ours first; true: the game's own left out
static bool SsBeforeGameDraw(ID3D11DeviceContext* ctx, const DrawCall& d) { return g_enabled && Immediate(ctx) && SsAfterDraw(ctx, d); }

// 1.26: who else reads the game's sun shadow map (SkipGameShadows leaves it empty where ours are read): for 30 frames every
// 3600, each draw outside the game's sun pass looks at its pixel-shader textures; logged
static unsigned g_gsrFrame, g_gsrDraws, g_gsrN;
static ID3D11PixelShader* g_gsrPs[16];
static void GameShadowReaders(ID3D11DeviceContext* ctx)
{
    unsigned ph = g_frame % 3600;
    if (ph > 30 || !g_sunShadowTex) return;
    if (g_gsrFrame != g_frame) {
        g_gsrFrame = g_frame;
        if (ph == 0) { g_gsrDraws = 0; g_gsrN = 0; }
        if (ph == 30) Log("the game's sun shadow map: read by %.1f draws a frame outside its sun pass (%u pixel shaders)%s", g_gsrDraws / 30.0f, g_gsrN,
                          g_gsrDraws ? ": those lose their sun shadows near the camera with SkipGameShadows" : ": nothing else uses it");
    }
    if (ph == 30 || *CurSlot(ctx)) return;
    ID3D11ShaderResourceView* v[16] = {};
    ctx->PSGetShaderResources(0, 16, v);
    bool hit = false;
    for (int i = 0; i < 16; ++i) {
        if (!v[i]) continue;
        if (!hit) { ID3D11Resource* r = 0; v[i]->GetResource(&r); if (r) { hit = r == g_sunShadowTex; r->Release(); } }
        v[i]->Release();
    }
    if (!hit) return;
    ++g_gsrDraws;
    for (unsigned k = 0; k < g_gsrN; ++k) if (g_gsrPs[k] == g_lastPS) return;
    if (g_gsrN < 16) g_gsrPs[g_gsrN++] = g_lastPS;
}

// 1.47: the reflections' source taken before the game's effects: after our sun pass, at the first draw on the scene
// target that blends and writes no depth (car glass, light halos, sprites, particles; not the game's own lights) the
// scene is copied, and the reflections read the copy. The halos around headlights are drawn over the cars: oncoming
// cars reflected as pale blocks. (Lamps, signs and windows are emissive in the G-buffer: already in the copy.)
static void EarlyCopyCheck(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    static unsigned seen;   // draws looked at since the sun pass, this frame
    static unsigned seenFrame;
    if (seenFrame != g_frame) { seenFrame = g_frame; seen = 0; }
    if (!g_enabled || g_p[P_SSR_EARLY].v < 0.5f || g_p[P_SSR].v <= 0 || g_earlyFrame == g_frame + 1 || !g_air.scene || g_air.frame != g_frame) return;
    ++seen;
    if (LightKindOf(g_lastPS)) return;   // the game's own lights (deferred)
    if (strstr(PsNameOf(g_lastPS), "deferred_")) return;   // (1.52: its other deferred lights: light volumes reading the G-buffer, additive)
    ID3D11RenderTargetView* rtv = 0;
    ID3D11DepthStencilView* dsv = 0;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Resource* r = 0;
    if (rtv) { rtv->GetResource(&r); rtv->Release(); }
    bool scene = r && r == (ID3D11Resource*)g_air.scene;
    if (r) r->Release();
    if (!scene) { if (dsv) dsv->Release(); return; }
    ID3D11BlendState* bs = 0; float bf[4]; UINT bm;
    ctx->OMGetBlendState(&bs, bf, &bm);
    D3D11_BLEND_DESC bd = {};
    if (bs) { bs->GetDesc(&bd); bs->Release(); }
    bool write = false;
    if (dsv) {
        ID3D11DepthStencilState* ds = 0; UINT ref;
        ctx->OMGetDepthStencilState(&ds, &ref);
        write = true;   // (none bound: the default writes)
        if (ds) { D3D11_DEPTH_STENCIL_DESC dd; ds->GetDesc(&dd); ds->Release(); write = dd.DepthEnable && dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL; }
        dsv->Release();
    }
    if (!bd.RenderTarget[0].BlendEnable || write) return;
    D3D11_TEXTURE2D_DESC td;
    g_air.scene->GetDesc(&td);
    if (g_earlyTex) {
        D3D11_TEXTURE2D_DESC ed;
        g_earlyTex->GetDesc(&ed);
        if (ed.Width != td.Width || ed.Height != td.Height || ed.Format != td.Format || ed.MipLevels != td.MipLevels) {
            g_earlySrv->Release(); g_earlyTex->Release(); g_earlySrv = 0; g_earlyTex = 0;
        }
    }
    if (!g_earlyTex) {
        static bool failed;
        if (failed) return;
        D3D11_TEXTURE2D_DESC ed = td;
        ed.Usage = D3D11_USAGE_DEFAULT; ed.BindFlags = D3D11_BIND_SHADER_RESOURCE; ed.CPUAccessFlags = 0; ed.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};   // (1.48: the scene is typeless here, R32G32B32A32: the view as the scene's own)
        sv.Format = g_air.rtvFmt; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateTexture2D(&ed, 0, &g_earlyTex)) || FAILED(g_dev->CreateShaderResourceView(g_earlyTex, &sv, &g_earlySrv))) {
            if (g_earlyTex) g_earlyTex->Release();
            g_earlyTex = 0; failed = true;
            Log("reflections' source before the effects: no copy (%ux%u format %d, view %d)", td.Width, td.Height, (int)td.Format, (int)g_air.rtvFmt);
            return;
        }
        Log("reflections' source before the effects: a %ux%u copy of the scene (format %d, view %d)", td.Width, td.Height, (int)td.Format, (int)g_air.rtvFmt);
    }
    ProfMark(ctx, PR_EARLY, 0);
    ctx->CopyResource(g_earlyTex, g_air.scene);
    ProfMark(ctx, PR_EARLY, 1);
    g_earlyFrame = g_frame + 1;
    static unsigned lastLog;
    static ID3D11PixelShader* lastPs;
    if (g_lastPS != lastPs || g_frame - lastLog > 3600) {   // where it happens (its shader), now and then
        lastLog = g_frame; lastPs = g_lastPS;
        ID3D11VertexShader* vs = 0;
        ctx->VSGetShader(&vs, 0, 0);
        const unsigned char* sum = VsSumOf(vs);
        if (vs) vs->Release();
        Log("reflections' source: the scene copied before draw %u after the sun pass (k%d n%u, ps %p %s | vs %02x%02x%02x%02x, blend %d/%d)",
            seen, (int)d.kind, d.a, g_lastPS, PsNameOf(g_lastPS), sum ? sum[0] : 0, sum ? sum[1] : 0, sum ? sum[2] : 0, sum ? sum[3] : 0,
            (int)bd.RenderTarget[0].SrcBlend, (int)bd.RenderTarget[0].DestBlend);
    }
}

static bool BeforeDraw(ID3D11DeviceContext* ctx, const DrawCall& d)
{
    if (g_earlyArm == g_frame + 1 && g_earlyFrame != g_frame + 1 && Immediate(ctx)) EarlyCopyCheck(ctx, d);
    if (g_coronaN && Immediate(ctx)) CoronaBefore(ctx);   // the game's sun corona at our sun (sunshadow.inc)
    if (g_flareN && Immediate(ctx)) FlareBefore(ctx);     // its flare too
    HlScaleBegin(ctx);
    if (Immediate(ctx)) ExpoGrab(ctx);                            // the game's exposure, for the debug views
    if (g_skyPending && Immediate(ctx)) SkyBeforeDraw(ctx, d);   // our clouds, before the sky draw (sky.inc)
    if (Immediate(ctx)) GameWetBefore(ctx);                       // the game's puddle map, scaled or none (gamewet.inc)
    if (g_bldBound && Immediate(ctx)) BldBeforeDraw(ctx);          // a building's height map (matheight.inc)
    if (!g_pfStarted || !*CurSlot(ctx) || !Immediate(ctx) || !MainTarget(ctx)) return false;
    ProfMark(ctx, PR_GAMESUN, 0);
    return true;
}

static void AfterDraw(ID3D11DeviceContext* ctx, const DrawCall& d, bool timed)
{
    if (g_coronaPsSlot >= 0 || g_coronaVsSlot >= 0) CoronaAfter(ctx);
    if (g_flareOn) FlareAfter(ctx);
    if (Immediate(ctx)) GameWetAfter(ctx);
    HlScaleEnd(ctx);
    if (timed) ProfMark(ctx, PR_GAMESUN, 1);
    if (g_frameTrace == 2 && Immediate(ctx)) TraceDraw(ctx, d);
    if (g_recon == 2 && Immediate(ctx)) ReconDraw(ctx, d);
    if (g_enabled && g_lampUse > 0 && Immediate(ctx)) LsAfterDraw(ctx, d);
    if (g_enabled && Immediate(ctx)) { RainAfterDraw(ctx, d); RainPlayerAfterDraw(ctx); }   // the rain map, the player's car (rain.inc)
    if (g_pickReq == 2 && Immediate(ctx)) TxPickAfterDraw(ctx, d);   // the texture picker (texreplace.inc)
    if (g_enabled && g_p[P_SS_SKIP].v > 0.5f && Immediate(ctx)) GameShadowReaders(ctx);
    if (g_enabled && Immediate(ctx) && IsFinalPS(g_lastPS)) GradePass(ctx);
    if (g_enabled && Immediate(ctx) && g_lightPSN) HlAfterDraw(ctx);
    if (g_enabled && Immediate(ctx)) CarAfterDraw(ctx, d);
    if (g_skyDrawLog && Immediate(ctx)) SkyAfterDraw(ctx);
    if (g_obPhase == OB_QUERY || g_obPhase == OB_CB) ObAfterDraw(ctx, d);
    if (g_tplCount && g_p[P_AUTO_LAMPS].v > 0.5f) AutoAfterDraw(ctx, d);
    InterlockedIncrement(&g_drawCnt[d.kind]);
    if (g_tracing) { static const char* n[] = { "Draw", "DrawIndexed", "DrawInstanced", "DrawIndexedInstanced" }; Trace(n[d.kind], ctx, (void*)(size_t)d.a); }
    SunInfo* s = *CurSlot(ctx);
    if (!s) return;
    InterlockedIncrement(Immediate(ctx) ? &g_sunDrawsImm : &g_sunDrawsDef);
    if (g_enabled) { SunPass(ctx, s, d); if (Immediate(ctx)) g_earlyArm = g_frame + 1; }
}

static void STDMETHODCALLTYPE HkDraw(ID3D11DeviceContext* ctx, UINT count, UINT start)
{
    DrawCall d = { DK_DRAW, count, start, 0, 0, 0 };
    int we = g_cbsOn ? WeBefore(ctx, d) : WE_DRAW;   // the world editor: hidden / moved objects
    if (we == WE_SKIP) { if (g_weCopyFrom) WeCopiesDraw(ctx, d); return; }
    bool timed = BeforeDraw(ctx, d);
    if (!SsBeforeGameDraw(ctx, d) && !SunSpriteSkip(ctx)) oDraw(ctx, count, start);
    AfterDraw(ctx, d, timed);
    if (g_cbsOn) { unsigned long long cf = g_weCopyFrom; WeEnd(ctx, d, we); g_weCopyFrom = cf; if (cf) WeCopiesDraw(ctx, d); }
}

static void STDMETHODCALLTYPE HkDrawIndexed(ID3D11DeviceContext* ctx, UINT count, UINT start, INT base)
{
    DrawCall d = { DK_INDEXED, count, start, 0, 0, base };
    int we = g_cbsOn ? WeBefore(ctx, d) : WE_DRAW;   // the world editor: hidden / moved objects
    if (we == WE_SKIP) { if (g_weCopyFrom) WeCopiesDraw(ctx, d); return; }
    if (we == WE_SPLIT) { WeSplitDraw(ctx, d); return; }
    bool timed = BeforeDraw(ctx, d);
    if (!SsBeforeGameDraw(ctx, d) && !SunSpriteSkip(ctx)) oDrawIndexed(ctx, count, start, base);
    AfterDraw(ctx, d, timed);
    if (g_cbsOn) { unsigned long long cf = g_weCopyFrom; WeEnd(ctx, d, we); g_weCopyFrom = cf; if (cf) WeCopiesDraw(ctx, d); }
}

static void STDMETHODCALLTYPE HkDrawInstanced(ID3D11DeviceContext* ctx, UINT count, UINT inst, UINT start, UINT startInst)
{
    DrawCall d = { DK_INSTANCED, count, inst, start, startInst, 0 };
    int we = g_cbsOn ? WeBefore(ctx, d) : WE_DRAW;   // the world editor: hidden / moved objects
    if (we == WE_SKIP) { if (g_weCopyFrom) WeCopiesDraw(ctx, d); return; }
    bool timed = BeforeDraw(ctx, d);
    if (!SsBeforeGameDraw(ctx, d) && !SunSpriteSkip(ctx)) oDrawInstanced(ctx, count, inst, start, startInst);
    AfterDraw(ctx, d, timed);
    if (g_cbsOn) { unsigned long long cf = g_weCopyFrom; WeEnd(ctx, d, we); g_weCopyFrom = cf; if (cf) WeCopiesDraw(ctx, d); }
}

static void STDMETHODCALLTYPE HkDrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT count, UINT inst, UINT start, INT base, UINT startInst)
{
    DrawCall d = { DK_INDEXED_INSTANCED, count, inst, start, startInst, base };
    int we = g_cbsOn ? WeBefore(ctx, d) : WE_DRAW;   // the world editor: hidden / moved objects
    if (we == WE_SKIP) { if (g_weCopyFrom) WeCopiesDraw(ctx, d); return; }
    bool timed = BeforeDraw(ctx, d);
    if (!SsBeforeGameDraw(ctx, d) && !SunSpriteSkip(ctx)) oDrawIndexedInstanced(ctx, count, inst, start, base, startInst);
    AfterDraw(ctx, d, timed);
    if (g_cbsOn) { unsigned long long cf = g_weCopyFrom; WeEnd(ctx, d, we); g_weCopyFrom = cf; if (cf) WeCopiesDraw(ctx, d); }
}

typedef void(STDMETHODCALLTYPE* DrawAutoFn)(ID3D11DeviceContext*);
typedef void(STDMETHODCALLTYPE* DrawIndirectFn)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
typedef void(STDMETHODCALLTYPE* ExecListFn)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
static DrawAutoFn oDrawAuto;
static DrawIndirectFn oDrawIdxIndirect, oDrawInstIndirect;
static ExecListFn oExecList;
static void STDMETHODCALLTYPE HkDrawAuto(ID3D11DeviceContext* ctx) { InterlockedIncrement(&g_drawCnt[4]); if (g_tracing) Trace("DrawAuto", ctx, 0); oDrawAuto(ctx); }
static void STDMETHODCALLTYPE HkDrawIdxIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT o) { InterlockedIncrement(&g_drawCnt[5]); if (g_tracing) Trace("DrawIndexedInstancedIndirect", ctx, b); oDrawIdxIndirect(ctx, b, o); }
static void STDMETHODCALLTYPE HkDrawInstIndirect(ID3D11DeviceContext* ctx, ID3D11Buffer* b, UINT o) { InterlockedIncrement(&g_drawCnt[6]); if (g_tracing) Trace("DrawInstancedIndirect", ctx, b); oDrawInstIndirect(ctx, b, o); }
static void STDMETHODCALLTYPE HkExecList(ID3D11DeviceContext* ctx, ID3D11CommandList* l, BOOL r) { InterlockedIncrement(&g_drawCnt[7]); oExecList(ctx, l, r); }

// ---------------------------------------------------------------- overlay

static ID3D11Texture2D* g_textTex;
static ID3D11ShaderResourceView* g_textSrv;
static ID3D11RenderTargetView* g_bbRtv;
static ID3D11Texture2D* g_bbTex;
static const int kTextW = 980, kTextH = 960;
static bool g_textDirty = true;
static DWORD g_nextStats;

#include "worldedit.inc"
#include "bench.inc"
#include "menu.inc"

static void UpdateText(ID3D11DeviceContext* ctx)
{
    if (!g_textTex) {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = kTextW; d.Height = kTextH; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_dev->CreateTexture2D(&d, 0, &g_textTex))) return;
        g_dev->CreateShaderResourceView(g_textTex, 0, &g_textSrv);
    }
    BuildMenu();
    HDC dc = CreateCompatibleDC(0);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kTextW; bi.bmiHeader.biHeight = -kTextH; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    void* bits = 0;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HFONT font = CreateFontA(17, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
    HFONT bold = CreateFontA(17, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, FIXED_PITCH, "Consolas");
    HGDIOBJ oldFont = SelectObject(dc, font);
    memset(bits, 0, kTextW * kTextH * 4);
    SetBkMode(dc, TRANSPARENT);
    int y = 6, used = 0;
    for (int i = 0; i < g_mlN && y < kTextH - 18; ++i) {
        int x = 10;
        const MLine& L = g_ml[i];
        for (int k = 0; k < L.n; ++k) {
            bool strong = L.s[k].c == C_GROUP || L.s[k].c == C_OBJ || L.s[k].c == C_ON || L.s[k].c == C_OFF || (i == 0 && k == 0);
            SelectObject(dc, strong ? bold : font);
            SetTextColor(dc, L.s[k].c);
            int n = (int)strlen(L.s[k].t);
            TextOutA(dc, x, y, L.s[k].t, n);
            SIZE sz;
            GetTextExtentPoint32A(dc, L.s[k].t, n, &sz);
            x += sz.cx;
            if (x > used) used = x;
        }
        y += L.n ? 19 : 9;   // empty lines are short gaps between the blocks
    }
    GdiFlush();
    int panelW = used + 12 < kTextW ? used + 12 : kTextW, panelH = y + 6 < kTextH ? y + 6 : kTextH;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_textTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int yy = 0; yy < kTextH; ++yy) {
            unsigned* srcp = (unsigned*)bits + yy * kTextW;
            unsigned* dst = (unsigned*)((char*)m.pData + yy * m.RowPitch);
            for (int x = 0; x < kTextW; ++x) {
                if (x >= panelW || yy >= panelH) { dst[x] = 0; continue; }   // nothing outside the panel
                unsigned c = srcp[x] & 0xffffff;
                unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, bl = c & 255;
                unsigned mx = r > g ? r : g; if (bl > mx) mx = bl;
                unsigned a = 165 + mx * 90 / 255;   // coloured text on a dark translucent panel
                dst[x] = (a << 24) | c;
            }
        }
        ctx->Unmap(g_textTex, 0);
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(font);
    DeleteObject(bold);
    DeleteObject(bmp);
    DeleteDC(dc);
}

static void DrawOverlay(IDXGISwapChain* sc, ID3D11DeviceContext* ctx)
{
    ID3D11Texture2D* bb = 0;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) {
        static bool said; if (!said) { said = true; Log("overlay: GetBuffer failed"); }
        return;
    }
    static bool first = true;
    if (first) { first = false; Log("overlay: first draw, back buffer %p", bb); }
    if (bb != g_bbTex || !g_bbRtv) {
        if (g_bbRtv) { g_bbRtv->Release(); g_bbRtv = 0; }
        g_dev->CreateRenderTargetView(bb, 0, &g_bbRtv);
        g_bbTex = bb;
    }
    if (g_textDirty) { UpdateText(ctx); g_textDirty = false; }
    if (!g_bbRtv || !g_textSrv || !g_vsText || !g_psText) {
        static bool said;
        if (!said) { said = true; Log("overlay: missing rtv %p text %p vs %p ps %p", g_bbRtv, g_textSrv, g_vsText, g_psText); }
        bb->Release();
        return;
    }

    // save
    ID3D11RenderTargetView* sRtv = 0; ID3D11DepthStencilView* sDsv = 0;
    ctx->OMGetRenderTargets(1, &sRtv, &sDsv);
    D3D11_VIEWPORT sVp[16]; UINT nVp = 16;
    ctx->RSGetViewports(&nVp, sVp);
    ID3D11VertexShader* sVs = 0; ID3D11PixelShader* sPs = 0;
    ctx->VSGetShader(&sVs, 0, 0); ctx->PSGetShader(&sPs, 0, 0);
    ID3D11InputLayout* sIl = 0; ctx->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTop; ctx->IAGetPrimitiveTopology(&sTop);
    ID3D11BlendState* sBl = 0; FLOAT sBf[4]; UINT sBm; ctx->OMGetBlendState(&sBl, sBf, &sBm);
    ID3D11DepthStencilState* sDs = 0; UINT sRef; ctx->OMGetDepthStencilState(&sDs, &sRef);
    ID3D11RasterizerState* sRs = 0; ctx->RSGetState(&sRs);
    ID3D11ShaderResourceView* sSrv = 0; ctx->PSGetShaderResources(0, 1, &sSrv);
    ID3D11SamplerState* sSmp = 0; ctx->PSGetSamplers(0, 1, &sSmp);
    ID3D11Buffer* sCb = 0; ctx->VSGetConstantBuffers(13, 1, &sCb);

    D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
    float rect[4] = { -1.0f + 24.0f * 2 / bd.Width, 1.0f - 24.0f * 2 / bd.Height, kTextW * 2.0f / bd.Width, kTextH * 2.0f / bd.Height };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(g_cbText, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) { memcpy(m.pData, rect, 16); ctx->Unmap(g_cbText, 0); }
    D3D11_VIEWPORT vp = { 0, 0, (float)bd.Width, (float)bd.Height, 0, 1 };
    ctx->OMSetRenderTargets(1, &g_bbRtv, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(g_vsText, 0, 0);
    oPSSetShader(ctx, g_psText, 0, 0);
    ctx->IASetInputLayout(0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->OMSetBlendState(g_blendAlpha, 0, 0xffffffff);
    ctx->OMSetDepthStencilState(0, 0);
    ctx->RSSetState(g_rsNoCull);
    ctx->PSSetShaderResources(0, 1, &g_textSrv);
    ctx->PSSetSamplers(0, 1, &g_sText);
    ctx->VSSetConstantBuffers(13, 1, &g_cbText);
    if (g_enabled && g_p[P_SUN_DEBUG].v > 0.5f) { SunDbgDraw(ctx, (float)bd.Width / bd.Height); ctx->PSSetShaderResources(0, 1, &g_textSrv); }   // the sun's paths (sunshadow.inc)
    if (g_overlay) oDraw(ctx, 4, 0);

    // restore
    ctx->OMSetRenderTargets(1, &sRtv, sDsv);
    ctx->RSSetViewports(nVp, sVp);
    ctx->VSSetShader(sVs, 0, 0);
    oPSSetShader(ctx, sPs, 0, 0);
    ctx->IASetInputLayout(sIl);
    ctx->IASetPrimitiveTopology(sTop);
    ctx->OMSetBlendState(sBl, sBf, sBm);
    ctx->OMSetDepthStencilState(sDs, sRef);
    ctx->RSSetState(sRs);
    ctx->PSSetShaderResources(0, 1, &sSrv);
    ctx->PSSetSamplers(0, 1, &sSmp);
    ctx->VSSetConstantBuffers(13, 1, &sCb);
    ID3D11DeviceChild* rel[] = { sRtv, sDsv, sVs, sPs, sIl, sBl, sDs, sRs, sSrv, sSmp, sCb };
    for (int i = 0; i < (int)(sizeof(rel) / sizeof(rel[0])); ++i) if (rel[i]) rel[i]->Release();
    bb->Release();
}

// ---------------------------------------------------------------- keys

static bool g_down[256];
static DWORD g_repeatAt[256];

static bool Pressed(int vk, bool repeat)
{
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    DWORD now = GetTickCount();
    bool fire = false;
    if (d && !g_down[vk]) { fire = true; g_repeatAt[vk] = now + 350; }
    else if (d && repeat && (LONG)(now - g_repeatAt[vk]) >= 0) { fire = true; g_repeatAt[vk] = now + 60; }
    g_down[vk] = d;
    return fire;
}

static bool OurWindowActive()
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void Keys()
{
    if (!OurWindowActive()) return;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool changed = false;
    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000) && Pressed('Y', false)) { g_enabled = !g_enabled; changed = true; Log("NewLook %s", g_enabled ? "on" : "off"); }
    if (shift && g_view != 24 && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && Pressed('B', false)) { BptAdd(); changed = true; }   // a bench viewpoint (bench.inc)
    if (!shift && Pressed('B', false)) {   // bench: B the selected setting, Ctrl+B every effect (at every viewpoint), B again stops
        if (g_benchPhase >= 0) BenchStop();
        else if (GetAsyncKeyState(VK_CONTROL) & 0x8000) BenchStart(true, 0);
        else if (!g_viewPicker && g_view != 24) BenchStart(false, g_order[g_sel % g_orderCount]);
        changed = true;
    }
    if (Pressed('X', false) && shift && !g_pickReq) { g_pickReq = 1; strcpy(g_lampMsg, "texture pick: the material at the screen centre..."); changed = true; }
    if (Pressed('T', false) && shift) { g_recon = 1; strcpy(g_lampMsg, "shadow research: one frame logged (recon: lines)"); changed = true; }
    if (Pressed('U', false)) {
        if (shift) { g_captureReq = 1; g_frameTrace = 1; Log("capture requested"); }
        else { g_viewPicker = !g_viewPicker; changed = true; }
    }
    if (g_viewPicker) {   // the view list takes I/O (the view changes at once)
        bool up = Pressed('I', true), down = Pressed('O', true);
        if (up || down) {
            int pos = (ViewListPos() + (down ? 1 : kViewListCount - 1)) % kViewListCount;
            g_view = kViewList[pos].view;
            changed = true;
        }
    }
    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && !shift && Pressed('J', false)) { WeToggle(); changed = true; }   // the world editor (worldedit.inc); J below sees no new press
    if (g_weOn && g_view != 24 && WeKeys(shift, ctrl)) changed = true;
    if (g_view == 24 && ctrl) {
        if (Pressed('Z', false)) { UndoStep(false); changed = true; }
        if (Pressed('Y', false)) { UndoStep(true); changed = true; }
        if (Pressed('C', false)) {
            if (g_lampSel >= 0 && g_lampSel < g_mapCount) {
                const MapLamp& L = g_map[g_lampSel];
                memcpy(g_clip.c, L.c, 12); g_clip.group = L.group; g_clip.channel = L.channel; g_clip.valid = 1;
                sprintf(g_lampMsg, "copied the settings of lamp %d", g_lampSel + 1);
            } else strcpy(g_lampMsg, "copy: select a lamp first");
            changed = true;
        }
        if (Pressed('V', false)) {
            if (!g_clip.valid) strcpy(g_lampMsg, "paste: copy a lamp first (Ctrl+C)");
            else if (g_lampSel < 0 || g_lampSel >= g_mapCount) strcpy(g_lampMsg, "paste: select a lamp first");
            else {
                UndoPush("paste lamp settings");
                g_undoLastWhat[0] = 0;
                float centre[3];
                memcpy(centre, g_map[g_lampSel].p, 12);
                int n = 0;
                for (int i = 0; i < g_mapCount; ++i) {
                    if (shift ? Dist2(g_map[i].p, centre) > 30.0f * 30.0f : i != g_lampSel) continue;
                    memcpy(g_map[i].c, g_clip.c, 12); g_map[i].group = g_clip.group; g_map[i].channel = g_clip.channel;
                    ++n;
                }
                g_mapDirty = true;
                SelLoad();
                sprintf(g_lampMsg, "pasted the settings onto %d lamp(s)", n);
            }
            changed = true;
        }
    }
    bool lampEdit = g_view == 24 && g_lampSel >= 0;
    bool zoneEdit = g_view == 24 && g_zoneSel >= 0 && g_zoneSel < g_zoneCount;
    if (g_view == 24 && shift && Pressed('B', false)) { ZoneAddAtAim(); changed = true; }
    if (zoneEdit) {
        Zone& Z = g_zone[g_zoneSel];
        bool zi = Pressed('I', true), zo = Pressed('O', true), zk = Pressed('K', true), zl = Pressed('L', true);
        if (zi) g_zoneProp = (g_zoneProp + 5) % 6;
        if (zo) g_zoneProp = (g_zoneProp + 1) % 6;
        if (zk || zl) {
            UndoPush("zone values");
            float dir = zl ? 1.0f : -1.0f;
            if (g_zoneProp == 5) Z.channel = (Z.channel + (zl ? 1 : 15)) % 16;
            else if (g_zoneProp == 4) { Z.soft += dir * 0.5f; Z.soft = Z.soft < 0.25f ? 0.25f : Z.soft > 30 ? 30 : Z.soft; }
            else if (g_zoneProp < 3) { Z.h[g_zoneProp] += dir * 0.5f; if (Z.h[g_zoneProp] < 0.5f) Z.h[g_zoneProp] = 0.5f; }
            else Z.yaw = fmodf(Z.yaw + dir * 5 + 360, 360);
            g_zoneDirty = true;
        }
        if (Pressed(VK_DELETE, false)) {
            UndoPush("delete zone");
            g_zone[g_zoneSel] = g_zone[--g_zoneCount];
            g_zoneSel = -1;
            ZoneSave();
            strcpy(g_lampMsg, "zone deleted");
        }
        if (zi || zo || zk || zl) changed = true;
        lampEdit = false;   // I/O/K/L/Del were taken by the zone (a second Pressed() of the same key sees nothing)
    }
    if (Pressed('I', !shift)) {
        if (shift && g_view == 24) TplMake();   // the selected lamp's post becomes a template
        else if (shift && !lampEdit && g_orderCount) {   // 1.40: the group before (its first item)
            int gI = g_orderGroup[g_sel % g_orderCount], to = (gI + kGroupCount - 1) % kGroupCount;
            for (int k = 0; k < g_orderCount; ++k) if (g_orderGroup[k] == to) { g_sel = k; break; }
        }
        else if (!shift) { if (lampEdit) g_lampProp = (g_lampProp + 4) % 5; else g_sel = (g_sel + g_orderCount - 1) % g_orderCount; }
        changed = true;
    }
    if (Pressed('O', !shift)) {
        if (shift && g_view == 24) {
            g_obConnect = !g_obConnect;
            strcpy(g_lampMsg, g_obConnect ? "CONNECT: put the cross on the object (lamp post) and click LMB" : "connect cancelled");
        } else if (shift && !lampEdit && g_orderCount) {   // 1.40: the group after
            int gI = g_orderGroup[g_sel % g_orderCount], to = (gI + 1) % kGroupCount;
            for (int k = 0; k < g_orderCount; ++k) if (g_orderGroup[k] == to) { g_sel = k; break; }
        } else if (!shift) { if (lampEdit) g_lampProp = (g_lampProp + 1) % 5; else g_sel = (g_sel + 1) % g_orderCount; }
        changed = true;
    }
    bool k = Pressed('K', !shift), l = Pressed('L', !shift), j = Pressed('J', false);
    if (lampEdit && !shift && (k || l)) {   // the selected lamp's colour / power
        UndoPush("lamp values");
        float dir = l ? 1.0f : -1.0f;
        if (g_lampProp == 0) {   // custom (-1), then the groups
            int n = g_presetCount + 1;
            int gidx = (g_map[g_lampSel].group + 1 + (l ? 1 : n - 1)) % n - 1;
            g_map[g_lampSel].group = gidx;
            g_mapDirty = true;
            SelLoad();
            sprintf(g_lampMsg, "lamp %d: %s", g_lampSel + 1, gidx < 0 ? "custom" : g_presets[gidx].name);
        } else if (g_lampProp == 4) {   // zone channel 0..15
            int& ch = g_map[g_lampSel].channel;
            ch = (ch + (l ? 1 : 15)) % 16;
            g_mapDirty = true;
            sprintf(g_lampMsg, "lamp %d: zone channel %d", g_lampSel + 1, ch);
        } else if (g_map[g_lampSel].group >= 0) {
            strcpy(g_lampMsg, "a group sets this; set the group to custom to change it by hand");
        } else if (g_lampProp == 1) g_selHue = fmodf(g_selHue + dir * 5 + 360, 360);
        else if (g_lampProp == 2) { g_selSat += dir * 0.05f; g_selSat = g_selSat < 0 ? 0 : g_selSat > 1 ? 1 : g_selSat; }
        else g_selPow *= l ? 1.1f : 1 / 1.1f;
        if (g_lampProp != 4) SelApply();
        if (g_lampProp != 4) {   // remembered for the next new lamp
            LampColour(g_map[g_lampSel], g_newLamp.c);
            if (g_map[g_lampSel].group < 0) memcpy(g_newLamp.c, g_map[g_lampSel].c, 12);
            g_newLamp.group = g_map[g_lampSel].group;
            g_newLamp.valid = 1;
        }
        k = l = false;
        changed = true;
    }
    if (g_view == 24 && Pressed(VK_DELETE, false)) { MapDeleteAtAim(); changed = true; }
    if (shift) {   // lamp map
        if (k) { MapAddAtAim(); changed = true; }
        if (j) { MapDeleteAtAim(); changed = true; }
        if (l) { MapLearnAll(); changed = true; }
    } else {
        float before = SelParam().v;
        if (k) { Param& p = SelParam(); p.v -= p.step; if (p.v < p.mn) p.v = p.mn; changed = true; }
        if (l) { Param& p = SelParam(); p.v += p.step; if (p.v > p.mx) p.v = p.mx; changed = true; }
        if (j) { SelParam().v = SelParam().def; changed = true; }
        if (k || l || j) {
            int idx = (int)(&SelParam() - g_p);
            if (idx == P_LOOK_PRESET && g_p[idx].v > 0.5f) LookApplyPreset((int)(g_p[P_LOOK_PRESET].v + 0.5f));
            if (idx == P_SKY + 12 && g_p[idx].v > 0.5f) SkyApplyWeather((int)(g_p[P_SKY + 12].v + 0.5f));
            if (const char* pm = PresetMemChanged(idx, before)) strcpy(g_lampMsg, pm);
        }
    }
    if (Pressed('H', false)) { SaveIni(); MapSave(); ZoneSave(); strcpy(g_lampMsg, "saved: settings, lamp map, zones"); changed = true; }
    if (Pressed('P', false)) {
        if (shift) { LoadIni(); PresetsLoad(); SelLoad(); if (g_dev) CompileShaders(g_dev); }
        else g_overlay = !g_overlay;
        changed = true;
    }
    if (changed) {
        g_textDirty = true;
        Log("key: enabled %d overlay %d view %d param %s = %.2f", g_enabled, g_overlay, g_view, SelParam().key, SelParam().v);
    }
}

static DWORD g_renderTid, g_nextText;
static HRESULT STDMETHODCALLTYPE HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    g_renderTid = GetCurrentThreadId();
    if (g_airWorks && g_enabled && g_air.frame == g_frame && g_airDoneFrame != g_frame) {
        if (++g_airMisses > 30) { g_airWorks = false; Log("air: no post-processing pass found, the air stays in the sun pass"); }
    } else if (g_airDoneFrame == g_frame) g_airMisses = 0;
    ++g_frame;
    if (CarEditTick(g_order[g_sel % g_orderCount], g_overlay)) g_textDirty = true;
    if (g_recon) ReconFrameEnd();
    if (g_frameTrace == 2) { g_frameTrace = 0; Log("frame: end"); }
    else if (g_frameTrace == 1) { g_frameTrace = 2; Log("frame: start"); }
    g_lastFrameSun = g_frameSun;
    g_frameSun = 0;
    Keys();
    TxPickFrame();   // Shift+X: the material at the centre, its textures saved (texreplace.inc)
    BldTick();       // building height maps: read a normal map, integrate it on a worker (matheight.inc)
    if (g_txMsgDirty) { g_txMsgDirty = false; g_textDirty = true; }
    BenchTick();
    DXGI_SWAP_CHAIN_DESC d;
    if (SUCCEEDED(sc->GetDesc(&d))) { g_bbW = d.BufferDesc.Width; g_bbH = d.BufferDesc.Height; g_hwnd = d.OutputWindow; }
    EditorUpdate(OurWindowActive());
    WeUpdate(OurWindowActive());   // the world editor (worldedit.inc)
    if (g_ssRecN && g_dev) {         // our shadow replays still recorded (sunshadow.inc): drawn now
        ID3D11DeviceContext* ic = 0;
        g_dev->GetImmediateContext(&ic);
        if (ic) { SsFlush(ic); ic->Release(); }
    }
    WeFrame();
    DWORD now = GetTickCount();
    if ((LONG)(now - g_nextStats) >= 0) {
        g_nextStats = now + 10000;
        if (g_mapDirty) MapSave();   // lamps learnt automatically
        TxStats();
        if (g_zoneDirty) ZoneSave();
        g_wantDump = 1;
        g_textDirty = true;
        Log("stats: sun passes/frame %d, sun shaders %ld, buffer %ux%u fmt %d, back buffer %ux%u, small skipped %d | "
            "PSSetShader %ld, sun binds imm/def %ld/%ld, sun draws imm/def %ld/%ld",
            g_lastFrameSun, g_sunCount, g_rtW, g_rtH, (int)g_rtFmt, g_bbW, g_bbH, g_skipSmall, g_psCalls,
            g_sunBindsImm, g_sunBindsDef, g_sunDrawsImm, g_sunDrawsDef);
        Log("stats: draws %ld/%ld/%ld/%ld auto %ld indirect %ld/%ld exec-lists %ld | overlay %d enabled %d view %d dev %p compiler %p sunPS %p textPS %p text %p err '%s'",
            g_drawCnt[0], g_drawCnt[1], g_drawCnt[2], g_drawCnt[3], g_drawCnt[4], g_drawCnt[5], g_drawCnt[6], g_drawCnt[7],
            g_overlay, g_enabled, g_view, g_dev, g_compile, g_psSun[0], g_psText, g_textSrv, g_compileError);
        char pt[768];
        ProfText(pt);
        for (char* c = pt; *c; ++c) if (*c == '\n') *c = ' ';
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        GlobalMemoryStatusEx(&ms);
        Log("timing: %s| lost %u | free address space %u of %u MB | render thread %lu | fast %d bounce %.2f | lamps %d found %d used",
            pt, g_profLost, (unsigned)(ms.ullAvailVirtual >> 20), (unsigned)(ms.ullTotalVirtual >> 20), g_renderTid,
            g_p[P_FAST].v > 0.5f, g_p[P_BOUNCE].v, g_lampCount, g_lampUse);
    }
    if (g_overlay && (LONG)(now - g_nextText) >= 0) { g_nextText = now + 500; g_textDirty = true; }
    ID3D11DeviceContext* ctx = 0;
    if (g_dev) g_dev->GetImmediateContext(&ctx);
    if (ctx) {
        if (g_weOn && EnsureResources(ctx)) WeOverlay(sc, ctx);   // the selection's outline, the gizmo
        BenchPad(ctx);   // the bench's fixed GPU work when the frame is not GPU-bound (bench.inc)
        if ((g_overlay || (g_enabled && g_p[P_SUN_DEBUG].v > 0.5f)) && EnsureResources(ctx)) {   // (1.33: the sun debug too)
            ProfMark(ctx, PR_OVERLAY, 0);
            DrawOverlay(sc, ctx);
            ProfMark(ctx, PR_OVERLAY, 1);
        }
        if (g_obPhase != OB_IDLE) ObPresent(ctx);
        if (g_autoCopyN) AutoPresent(ctx);
        ProfFrameEnd(ctx);
    }
    LARGE_INTEGER pq0, pq1;
    QueryPerformanceCounter(&pq0);
    HRESULT hr = oPresent(sc, g_benchUncap ? 0 : sync, flags);   // (vsync off while benching)
    QueryPerformanceCounter(&pq1);
    if (g_qpf.QuadPart) g_benchWaitMs = (pq1.QuadPart - pq0.QuadPart) * 1000.0 / g_qpf.QuadPart;
    if (ctx) { ProfFrameBegin(ctx); ctx->Release(); }
    return hr;
}

// ---------------------------------------------------------------- install

static void Patch(void** vtbl, int idx, void* hook, void** orig)
{
    DWORD old;
    VirtualProtect(&vtbl[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *orig = vtbl[idx];
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void*), old, &old);
}

// The hooks go on the game's own device: the D3D11 / DXGI creation functions are detoured
// (hot-patch prologue "mov edi,edi; push ebp; mov ebp,esp"), and when the game's device,
// context and swap chain come out, their vtables get patched on the game's own thread.
typedef HRESULT(WINAPI* CreateDeviceFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                        ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateDeviceSCFn)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*,
                                          ID3D11DeviceContext**);
typedef HRESULT(WINAPI* CreateFactoryFn)(REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSwapChainFn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
static CreateDeviceFn oCreateDevice;
static CreateDeviceSCFn oCreateDeviceSC;
static CreateFactoryFn oCreateFactory, oCreateFactory1;
static CreateSwapChainFn oCreateSwapChain;
static volatile LONG g_devHooked, g_scHooked, g_factHooked;

static const char* ModuleOf(const void* p, char* buf, int n)
{
    HMODULE m = 0;
    buf[0] = 0;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)p, &m)) {
        char full[MAX_PATH];
        GetModuleFileNameA(m, full, MAX_PATH);
        const char* b = strrchr(full, 92);
        _snprintf(buf, n - 1, "%s+%lx", b ? b + 1 : full, (unsigned long)((const char*)p - (const char*)m));
        buf[n - 1] = 0;
    } else {
        _snprintf(buf, n - 1, "heap");
    }
    return buf;
}

// Puts a jump at the start of a function (the code itself, whoever calls it). Needs a prologue
// we can move: "mov edi,edi; push ebp; mov ebp,esp" or "push ebp; mov ebp,esp; ..." handled below.
static void* InlineHook(void* target, void* hook, const char* name)
{
    unsigned char* fn = (unsigned char*)target;
    char mod[160];
    Log("%s at %p (%s): %02x %02x %02x %02x %02x %02x %02x %02x", name, fn, ModuleOf(fn, mod, sizeof(mod)),
        fn[0], fn[1], fn[2], fn[3], fn[4], fn[5], fn[6], fn[7]);
    int len = 0;
    if (fn[0] == 0x8B && fn[1] == 0xFF && fn[2] == 0x55 && fn[3] == 0x8B && fn[4] == 0xEC) len = 5;      // mov edi,edi; push ebp; mov ebp,esp
    else if (fn[0] == 0x55 && fn[1] == 0x8B && fn[2] == 0xEC && fn[3] == 0x83 && fn[4] == 0xEC) len = 6; // push ebp; mov ebp,esp; sub esp,imm8
    else if (fn[0] == 0x55 && fn[1] == 0x8B && fn[2] == 0xEC && fn[3] >= 0x50 && fn[3] <= 0x57 && fn[4] >= 0x50 && fn[4] <= 0x57) len = 5; // push ebp; mov ebp,esp; push r; push r
    else if (fn[0] == 0xE9) {  // already a jump (someone else's hook): follow it is unsafe, hook the jump target
        void* t = fn + 5 + *(long*)(fn + 1);
        Log("%s starts with a jump to %p, hooking there", name, t);
        return InlineHook(t, hook, name);
    }
    if (!len) { Log("%s: unknown prologue, not hooked", name); return 0; }
    unsigned char* tr = (unsigned char*)VirtualAlloc(0, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    memcpy(tr, fn, len);
    tr[len] = 0xE9;
    *(long*)(tr + len + 1) = (long)((fn + len) - (tr + len + 5));
    DWORD old;
    VirtualProtect(fn, len, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9;
    *(long*)(fn + 1) = (long)((unsigned char*)hook - (fn + 5));
    for (int i = 5; i < len; ++i) fn[i] = 0x90;
    VirtualProtect(fn, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, len);
    Log("%s hooked in code", name);
    return tr;
}

static void HookDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
    if (!dev || !ctx || InterlockedExchange(&g_devHooked, 1)) return;
    void** dv = *(void***)dev;
    void** cv = *(void***)ctx;
    Patch(dv, 15, (void*)HkCreatePS, (void**)&oCreatePS);
    TxScan();   // texture replacements (texreplace.inc)
    Patch(dv, 5, (void*)HkCreateTex2D, (void**)&oCreateTex2D);
    Patch(dv, 7, (void*)HkCreateSRV, (void**)&oCreateSRV);
    Patch(dv, 12, (void*)HkCreateVS, (void**)&oCreateVS);
    Patch(dv, 11, (void*)HkCreateIL, (void**)&oCreateIL);
    Patch(cv, 9, (void*)HkPSSetShader, (void**)&oPSSetShader);
    // texture uploads (texreplace.inc): in the code where possible (the game may not call through this vtable)
    oUpdSub = (UpdSubFn)InlineHook(cv[48], (void*)HkUpdSub, "UpdateSubresource");
    if (!oUpdSub) Patch(cv, 48, (void*)HkUpdSub, (void**)&oUpdSub);
    oMap = (MapFn)InlineHook(cv[14], (void*)HkMap, "Map");
    if (!oMap) Patch(cv, 14, (void*)HkMap, (void**)&oMap);
    oUnmap = (UnmapFn)InlineHook(cv[15], (void*)HkUnmap, "Unmap");
    if (!oUnmap) Patch(cv, 15, (void*)HkUnmap, (void**)&oUnmap);
    oCopyRes = (CopyResFn)InlineHook(cv[47], (void*)HkCopyRes, "CopyResource");
    if (!oCopyRes) Patch(cv, 47, (void*)HkCopyRes, (void**)&oCopyRes);
    oCopySub = (CopySubFn)InlineHook(cv[46], (void*)HkCopySub, "CopySubresourceRegion");
    if (!oCopySub) Patch(cv, 46, (void*)HkCopySub, (void**)&oCopySub);
    oPSSetSRV = (PSSetSRVFn)InlineHook(cv[8], (void*)HkPSSetSRV, "PSSetShaderResources");
    if (!oPSSetSRV) Patch(cv, 8, (void*)HkPSSetSRV, (void**)&oPSSetSRV);
    {
        char m1[160], m2[160], m3[160];
        Log("vtable entries: PSSetShader %p (%s), Draw %p (%s), DrawIndexed %p (%s)", cv[9], ModuleOf(cv[9], m1, 160),
            cv[13], ModuleOf(cv[13], m2, 160), cv[12], ModuleOf(cv[12], m3, 160));
    }
    // draws: hook the code itself (the game may not call through this vtable); fall back to the vtable
    oDrawIndexed = (DrawIndexedFn)InlineHook(cv[12], (void*)HkDrawIndexed, "DrawIndexed");
    if (!oDrawIndexed) Patch(cv, 12, (void*)HkDrawIndexed, (void**)&oDrawIndexed);
    oDraw = (DrawFn)InlineHook(cv[13], (void*)HkDraw, "Draw");
    if (!oDraw) Patch(cv, 13, (void*)HkDraw, (void**)&oDraw);
    oDrawIndexedInstanced = (DrawIndexedInstancedFn)InlineHook(cv[20], (void*)HkDrawIndexedInstanced, "DrawIndexedInstanced");
    if (!oDrawIndexedInstanced) Patch(cv, 20, (void*)HkDrawIndexedInstanced, (void**)&oDrawIndexedInstanced);
    oDrawInstanced = (DrawInstancedFn)InlineHook(cv[21], (void*)HkDrawInstanced, "DrawInstanced");
    if (!oDrawInstanced) Patch(cv, 21, (void*)HkDrawInstanced, (void**)&oDrawInstanced);
    Patch(cv, 38, (void*)HkDrawAuto, (void**)&oDrawAuto);
    Patch(cv, 39, (void*)HkDrawIdxIndirect, (void**)&oDrawIdxIndirect);
    Patch(cv, 40, (void*)HkDrawInstIndirect, (void**)&oDrawInstIndirect);
    Patch(cv, 58, (void*)HkExecList, (void**)&oExecList);
    Log("context %p vtable %p, device %p vtable %p", ctx, cv, dev, dv);
    if (!g_dev) { g_dev = dev; g_dev->AddRef(); }
    Log("hooked the game's D3D11 device");
}

static void HookSwapChain(IDXGISwapChain* sc)
{
    if (!sc || InterlockedExchange(&g_scHooked, 1)) return;
    Patch(*(void***)sc, 8, (void*)HkPresent, (void**)&oPresent);
    Log("hooked the game's swap chain");
}

static HRESULT STDMETHODCALLTYPE HkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc)
{
    HRESULT hr = oCreateSwapChain(f, dev, d, sc);
    if (SUCCEEDED(hr) && sc) HookSwapChain(*sc);
    return hr;
}

static void HookFactory(void* f)
{
    if (!f || InterlockedExchange(&g_factHooked, 1)) return;
    Patch(*(void***)f, 10, (void*)HkCreateSwapChain, (void**)&oCreateSwapChain);
    Log("hooked the DXGI factory");
}

static HRESULT WINAPI HkCreateDevice(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT fl, const D3D_FEATURE_LEVEL* fls,
                                     UINT n, UINT sdk, ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx)
{
    HRESULT hr = oCreateDevice(a, t, m, fl, fls, n, sdk, dev, got, ctx);
    Log("game called D3D11CreateDevice: %08lx", hr);
    if (SUCCEEDED(hr) && dev && ctx) HookDevice(*dev, *ctx);
    return hr;
}

static HRESULT WINAPI HkCreateDeviceSC(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT fl, const D3D_FEATURE_LEVEL* fls,
                                       UINT n, UINT sdk, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc,
                                       ID3D11Device** dev, D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx)
{
    HRESULT hr = oCreateDeviceSC(a, t, m, fl, fls, n, sdk, sd, sc, dev, got, ctx);
    Log("game called D3D11CreateDeviceAndSwapChain: %08lx", hr);
    if (SUCCEEDED(hr)) {
        if (dev && ctx) HookDevice(*dev, *ctx);
        if (sc) HookSwapChain(*sc);
    }
    return hr;
}

static HRESULT WINAPI HkCreateFactory(REFIID id, void** f)
{
    HRESULT hr = oCreateFactory(id, f);
    if (SUCCEEDED(hr) && f) HookFactory(*f);
    return hr;
}

static HRESULT WINAPI HkCreateFactory1(REFIID id, void** f)
{
    HRESULT hr = oCreateFactory1(id, f);
    if (SUCCEEDED(hr) && f) HookFactory(*f);
    return hr;
}

static void* Detour(HMODULE mod, const char* name, void* hook)
{
    unsigned char* fn = (unsigned char*)GetProcAddress(mod, name);
    if (!fn) { Log("cannot hook %s (not found)", name); return 0; }
    static const unsigned char pro[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    unsigned char* tr = (unsigned char*)VirtualAlloc(0, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return 0;
    char m[160];
    if (!memcmp(fn, pro, 5)) {   // the hot-patch start: moved into the trampoline
        memcpy(tr, fn, 5);
        tr[5] = 0xE9;
        *(long*)(tr + 6) = (long)((fn + 5) - (tr + 10));
    } else if (fn[0] == 0xE9) {   // already hooked (ReShade and the like load first): ours goes in front, theirs runs next
        unsigned char* t = fn + 5 + *(long*)(fn + 1);
        Log("%s is hooked already (jump to %p, %s): chained", name, t, ModuleOf(t, m, sizeof(m)));
        tr[0] = 0xE9;
        *(long*)(tr + 1) = (long)(t - (tr + 5));
    } else if (fn[0] == 0xFF && fn[1] == 0x25) {   // jmp [address]
        void** at = *(void***)(fn + 2);
        Log("%s is hooked already (jump via %p to %p, %s): chained", name, at, *at, ModuleOf(*at, m, sizeof(m)));
        tr[0] = 0xFF; tr[1] = 0x25; *(void***)(tr + 2) = at;
    } else if (fn[0] == 0x68 && fn[5] == 0xC3) {   // push address; ret
        memcpy(tr, fn, 6);
        Log("%s is hooked already (push/ret to %p): chained", name, *(void**)(fn + 1));
    } else {
        Log("cannot hook %s (unexpected start %02x %02x %02x %02x %02x %02x)", name, fn[0], fn[1], fn[2], fn[3], fn[4], fn[5]);
        VirtualFree(tr, 0, MEM_RELEASE);
        return 0;
    }
    DWORD old;
    VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old);
    fn[0] = 0xE9;
    *(long*)(fn + 1) = (long)((unsigned char*)hook - (fn + 5));
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    return tr;
}

// Logs crashes (with module and offset) so a failing start can be traced.
static volatile LONG g_crashLogs;
static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashLogs) > 20) return EXCEPTION_CONTINUE_SEARCH;
    void* at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = 0;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m))
        GetModuleFileNameA(m, name, MAX_PATH);
    const char* base = strrchr(name, 92);
    Log("exception %08lx at %p (%s+%lx), thread %lu%s, data %p", code, at, base ? base + 1 : name,
        (unsigned long)((char*)at - (char*)m), GetCurrentThreadId(), GetCurrentThreadId() == g_renderTid ? " (render thread)" : "",
        e->ExceptionRecord->NumberParameters > 1 ? (void*)e->ExceptionRecord->ExceptionInformation[1] : 0);
    // return addresses on the stack (values right after a call instruction inside a module)
    char chain[640] = "";
    int found = 0;
    DWORD* sp = (DWORD*)e->ContextRecord->Esp;
    DWORD* top = (DWORD*)((NT_TIB*)NtCurrentTeb())->StackBase;
    for (DWORD* q = sp; q < top && q < sp + 2048 && found < 12; ++q) {
        const unsigned char* v = (const unsigned char*)*q;
        HMODULE vm = 0;
        if ((size_t)v < 0x10000 || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)v, &vm))
            continue;
        if ((size_t)(v - (const unsigned char*)vm) < 0x1000) continue;
        if (v[-5] != 0xE8 && v[-6] != 0xFF && v[-2] != 0xFF && v[-3] != 0xFF) continue;   // not after a call
        char mn[MAX_PATH];
        GetModuleFileNameA(vm, mn, MAX_PATH);
        const char* b2 = strrchr(mn, 92);
        size_t l = strlen(chain);
        _snprintf(chain + l, sizeof(chain) - l - 1, " %s+%lx", b2 ? b2 + 1 : mn, (unsigned long)(v - (const unsigned char*)vm));
        ++found;
    }
    Log("exception stack:%s", chain);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallHooks()
{
    // the system's d3d11 / dxgi, by full path: a ReShade (or other) proxy d3d11.dll / dxgi.dll in the game folder
    // wraps the device; hooking its exports would put our hooks into its code. The real device under it is hooked.
    char sys[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);   // a 32-bit process: redirected to SysWOW64
    static const char* proxies[] = { "d3d11.dll", "dxgi.dll", "d3d9.dll", "opengl32.dll", "dinput8.dll", "version.dll", "winmm.dll" };
    for (int k = 0; k < 7; ++k) {
        // the game folder is the parent of scripts\ (or the folder itself)
        char game[MAX_PATH];
        strcpy(game, g_dir);
        size_t gl = strlen(game);
        if (gl > 8 && !_stricmp(game + gl - 8, "scripts\\")) game[gl - 8] = 0;
        sprintf(path, "%s%s", game, proxies[k]);
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) Log("in the game folder: %s (a proxy / loader: ReShade or an ASI loader)", proxies[k]);
    }
    sprintf(path, "%s\\d3d11.dll", sys);
    HMODULE d3d = LoadLibraryA(path);
    sprintf(path, "%s\\dxgi.dll", sys);
    HMODULE dxgi = LoadLibraryA(path);
    {
        char m1[MAX_PATH] = "", m2[MAX_PATH] = "";
        if (d3d) GetModuleFileNameA(d3d, m1, MAX_PATH);
        if (dxgi) GetModuleFileNameA(dxgi, m2, MAX_PATH);
        Log("hooking the system's %s and %s", m1, m2);
    }
    if (d3d) {
        oCreateDevice = (CreateDeviceFn)Detour(d3d, "D3D11CreateDevice", (void*)HkCreateDevice);
        oCreateDeviceSC = (CreateDeviceSCFn)Detour(d3d, "D3D11CreateDeviceAndSwapChain", (void*)HkCreateDeviceSC);
    }
    if (dxgi) {
        oCreateFactory = (CreateFactoryFn)Detour(dxgi, "CreateDXGIFactory", (void*)HkCreateFactory);
        oCreateFactory1 = (CreateFactoryFn)Detour(dxgi, "CreateDXGIFactory1", (void*)HkCreateFactory1);
    }
    Log("waiting for the game to create its D3D11 device");
}

// 1.64: the mod was NFSLight before: its files in scripts\ (settings, lamp map, zones, world edits, cache ...) renamed
// NFSLight* -> NewLook* (not over a NewLook one already there), the ini's section [NFSLight] -> [NewLook]. An old
// NFSLight.asi still beside us would hook the same game twice: it is renamed .old and this start does nothing (the old
// one may already be running with those files); the next start is ours alone.
static bool MigrateOldNames()
{
    char p[MAX_PATH], q[MAX_PATH];
    sprintf(p, "%sNFSLight.asi", g_dir);
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
        sprintf(q, "%sNFSLight.asi.old", g_dir);
        DeleteFileA(q);
        MoveFileA(p, q);
        return false;
    }
    sprintf(p, "%sNFSLight*", g_dir);
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA(p, &fd);
    if (fh == INVALID_HANDLE_VALUE) return true;
    static char names[64][MAX_PATH];
    int n = 0;
    do if (n < 64 && strncmp(fd.cFileName, "NFSLight", 8) == 0) strcpy(names[n++], fd.cFileName); while (FindNextFileA(fh, &fd));
    FindClose(fh);
    for (int k = 0; k < n; ++k) {
        const char* rest = names[k] + 8;
        if (!_stricmp(rest, ".hlsl") || !_stricmp(rest, ".log") || !_stricmp(rest, "_cache") || !_stricmp(rest, ".asi.old")) continue;
        sprintf(p, "%s%s", g_dir, names[k]);
        sprintf(q, "%sNewLook%s", g_dir, rest);
        if (GetFileAttributesA(q) != INVALID_FILE_ATTRIBUTES || !MoveFileA(p, q)) continue;
        if (!_stricmp(rest, ".ini")) {   // its section
            FILE* f = fopen(q, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            char* b = (char*)malloc(sz + 1);
            size_t got = fread(b, 1, sz, f);
            fclose(f);
            b[got] = 0;
            char* sec = strstr(b, "[NFSLight]");
            if (sec && (f = fopen(q, "wb")) != 0) {
                fwrite(b, 1, sec - b, f);
                fputs("[NewLook]", f);
                fputs(sec + 10, f);
                fclose(f);
            }
            free(b);
        }
    }
    return true;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_ctxLock);
        InitializeCriticalSection(&g_passLock);
        GetModuleFileNameA(h, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\');
        if (s) s[1] = 0;
        sprintf(g_logPath, "%sNewLook.log", g_dir);
        if (!MigrateOldNames()) {   // an old NFSLight.asi was here (now .old): nothing this start
            DeleteFileA(g_logPath);
            Log("NewLook 1.64: an old NFSLight.asi was in scripts (renamed NFSLight.asi.old): restart the game");
            return TRUE;
        }
        sprintf(g_iniPath, "%sNewLook.ini", g_dir);
        sprintf(g_hlslPath, "%sNewLook.hlsl", g_dir);
        DeleteFileA(g_logPath);
        LoadIni();
        PresetMemLoad();
        MenuInit();
        sprintf(g_mapPath, "%sNewLook_lamps.txt", g_dir);
        sprintf(g_presetPath, "%sNewLook_lamp_presets.txt", g_dir);
        sprintf(g_tplPath, "%sNewLook_lamp_templates.txt", g_dir);
        sprintf(g_zonePath, "%sNewLook_zones.txt", g_dir);
        sprintf(g_wePath, "%sNewLook_world.txt", g_dir);
        sprintf(g_bptPath, "%sNewLook_bench_points.txt", g_dir);
        PresetsLoad();
        MapLoad();
        TplLoad();
        ZoneLoad();
        WeLoad();
        BptLoad();
        Log("NewLook 1.64 loaded");
        AddVectoredExceptionHandler(0, CrashLogger);
        InstallHooks();
    }
    return TRUE;
}
