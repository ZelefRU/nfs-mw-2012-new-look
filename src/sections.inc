// ---------------------------------------------------------------- 1.96: sections, quality presets
// Each part of the mod is a section with its own switch (a parameter: Sec<Name> in the ini). Off: the settings that
// switch its effects off (its "kill" list) are set to their off values and the player's own values are kept aside:
// the menu shows them, the ini saves them, turning the section on puts them back. Enforced every frame (SecEnforce):
// the weather drives some of them (the haze, the wet ground) and would bring them back.
// The quality presets set the settings that cost (resolutions, reach, budgets, the costly extras); High = the
// defaults. Changing one of them by hand: Custom (its values kept: going back to Custom brings them back).
static const int P_QUALITY = 240, P_MENU_ADV = 241, P_SEC0 = 242;
enum { S_TIME, S_SKY, S_WX, S_SHADOW, S_AO, S_AIR, S_REFL, S_WET, S_MAT, S_LAMPS, S_CARL, S_RAIN, S_WIND, S_LOOK, kSecN };
// rows of a menu group: >= 0 a parameter, < 0 a sub-heading (kSubHeads[-1 - row])
static const char* kSubHeads[] = {
    "clock", "the sun", "night", "debug", "look", "quality (what it costs)", "by hand (Weather now: by hand)", "the ground",
    "our shadow maps", "softness", "the game's shadow maps (when ours are off)", "contact shadows", "test", "ambient light",
    "ambient occlusion (near)", "sky occlusion (large)", "bounce light", "by the time of day", "the reflected sky",
    "noise / sparkle (find what makes it)", "puddles: small mirrors", "puddles: big, matte", "lighting model", "relief",
    "parallax", "shadows", "posts", "lamp groups (the map's lamps by kind)", "the player's car", "traffic",
    "the aimed car's lights moved (fix a car model)", "falling", "on the lens", "on the car", "light", "colour", "detail",
    "lens", "anti-aliasing (ours: FXAA, heavy)" };
enum { H_CLOCK = -1, H_SUN = -2, H_NIGHT = -3, H_DEBUG = -4, H_LOOK = -5, H_QUAL = -6, H_BYHAND = -7, H_GROUND = -8, H_MAPS = -9,
       H_SOFT = -10, H_GAMEMAPS = -11, H_CONTACT = -12, H_TEST = -13, H_AMB = -14, H_AO = -15, H_SKYOCC = -16, H_BOUNCE = -17,
       H_BYTIME = -18, H_RSKY = -19, H_NOISE = -20, H_PSMALL = -21, H_PBIG = -22, H_MODEL = -23, H_RELIEF = -24, H_PARALLAX = -25,
       H_SHADOWS = -26, H_POSTS = -27, H_LGROUPS = -28, H_PLAYER = -29, H_TRAFFIC = -30, H_AIMED = -31, H_FALL = -32, H_LENS = -33,
       H_CAR = -34, H_LIGHT = -35, H_COLOUR = -36, H_DETAIL = -37, H_LENSFX = -38, H_AA = -39 };

struct SecDef {
    const char* name;            // in both views
    const char* hint;            // one line under the menu: what it is, what off saves
    short kill[6]; float killV[6]; short nk;
    short simple[4]; short ns;   // the simple view: under its switch
    short adv[64]; short na;     // the advanced view: every setting of it, with sub-headings (the switch first, added)
};
static const SecDef kSec[kSecN] = {
    { "SUN & MOON (time of day)", "our sun and moon cross the sky by a clock (shadows turn with them); off: the game's own sun loop",
      { 135, 9, 14 }, { 0, 1, 0 }, 3, { 138, 139, 140 }, 3,
      { H_CLOCK, 138, 139, 140, H_SUN, 141, 142, 136, 9, 148, 14, 143, H_NIGHT, 137, 144, 153, 154, 155, 146, 147, H_DEBUG, 145 }, 22 },
    { "SKY & CLOUDS", "our sky: moving clouds with their shadows, the city's glow, stars; off: the game's sky (saves the cloud pass)",
      { 91 }, { 0 }, 1, { 103, 93, 104 }, 3,
      { 103, 93, 100, 101, 102, 99, 236, H_LOOK, 94, 95, 96, 97, 98, 92, H_QUAL, 104, 105, 256 }, 18 },
    { "WEATHER", "weather that changes: rain, fog, clouds, roads getting wet and drying; off: as set by hand",
      { 208 }, { 0 }, 1, { 237, 211, 238 }, 3,
      { 237, 211, 238, 212, H_BYHAND, 209, 210, 216, H_GROUND, 213, 214, 215 }, 12 },
    { "SHADOWS", "our sun shadows: sharp near the caster, soft far, a long reach; off: the game's own (one of the costliest)",
      { 113, 43, 0, 12, 13, 134 }, { 0, 0, 0, 1, 0, 0 }, 6, { 115, 116, 114 }, 3,
      { H_MAPS, 113, 115, 116, 133, 186, 120, 185, 117, 134, H_SOFT, 114, 118, 119, H_GAMEMAPS, 43, 3, 11, 4, 5, H_LOOK, 12, 13,
        H_CONTACT, 0, 1, 2, 10, H_TEST, 187 }, 30 },
    { "AMBIENT OCCLUSION & BOUNCE LIGHT", "shade in corners and under things, light bounced off bright walls; off: flatter light",
      { 6, 62, 21, 8, 15, 38 }, { 0, 0, 0, 1, 0, 1 }, 6, { 6, 62, 21 }, 3,
      { H_AMB, 8, 15, 38, H_AO, 6, 7, 189, 190, H_SKYOCC, 62, 63, 188, H_BOUNCE, 21, 22, 24, 25 }, 18 },
    { "AIR (haze, light shafts, sun glow)", "haze in the distance, shafts of sunlight, the glow round the sun; off: clear air",
      { 16, 18, 19, 69 }, { 0, 0, 0, 0 }, 4, { 16, 19 }, 2,
      { 16, 17, 70, 20, H_BYTIME, 149, 150, 151, 152, 18, 19, 69 }, 12 },
    { "REFLECTIONS", "real reflections (screen space) on wet roads, puddles, glossy floors, car paint; off: the game's own",
      { 64, 239, 173 }, { 0, 0, 0 }, 3, { 64, 67 }, 2,
      { 64, 67, 66, 165, 166, 180, 227, 109, 239, 257, H_RSKY, 191, 193, 194, 192, H_NOISE, 173, 168, 170, 176, 171, 169, 172, 174 }, 24 },
    { "WET ROADS & PUDDLES", "roads that stay wet after rain, puddles that mirror the street; off: dry (as the game's own wetness)",
      { 65, 167, 112, 177, 110 }, { 0, 0, 0, 0, 1 }, 5, { 65, 112, 177 }, 3,
      { 65, 167, 175, 110, H_PSMALL, 112, 182, 183, 184, H_PBIG, 177, 178, 179, 181 }, 14 },
    { "MATERIALS (relief, parallax, lighting)", "depth on roads and walls, small shadows in their bumps, the light model; off: flat as the game's",
      { 121, 125, 129, 132 }, { 0, 0, 0, 0 }, 4, { 129, 125, 121 }, 3,
      { H_MODEL, 121, 122, 123, 124, 111, H_RELIEF, 125, 126, 127, 128, H_PARALLAX, 129, 130, 131, 132 }, 16 },
    { "STREET LAMPS (night)", "the city's lamps light the streets at night, with shadows; off: the game's own baked light",
      { 26, 68 }, { 0, 0 }, 2, { 26, 39, 44 }, 3,
      { 26, 35, 28, 164, 44, 30, 68, H_SHADOWS, 36, 37, 39, 40, 41, 42, H_POSTS, 34, 33, 32, H_LGROUPS, 156, 157, 160, 158, 159, 161, 162, 163 }, 27 },
    { "CAR LIGHTS", "headlight beams in the air, soft headlight shadows, the traffic's lights on the road; off: the game's",
      { 71, 73, 75, 72 }, { 0, 0, 0, 1 }, 4, { 71, 75 }, 2,
      { H_PLAYER, 72, 73, 71, 74, H_TRAFFIC, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, H_AIMED, 85, 86, 87, 88, 89, 90 }, 23 },
    { "RAIN EFFECTS", "falling rain, splashes, drops on the lens and on the player's car (with WEATHER: when it rains)",
      { 217, 220, 221, 230, 200 }, { 0, 0, 0, 0, 0 }, 5, { 217, 230, 200 }, 3,
      { H_FALL, 217, 233, 235, 226, 218, 219, 220, 221, H_LENS, 230, 228, 229, H_CAR, 200, 234, 201, 202, 203, 204, 205, 206, 207 }, 23 },
    { "WIND", "the wind: trees' leaves sway, the rain slants; off: still leaves (saves their geometry shader)",
      { 231, 232 }, { 0, 0 }, 2, { 222, 231 }, 2,
      { 222, 223, 224, 225, 231, 232 }, 6 },
    { "COLOUR (look)", "colour grading: exposure, contrast, a film look, bloom, sharpening; off: the game's colours",
      { 45 }, { 0 }, 1, { 61, 46, 47 }, 3,
      { 61, H_LIGHT, 46, 47, 106, 48, 49, H_COLOUR, 50, 51, 107, 108, 52, 53, 58, H_DETAIL, 54, 55, 56, H_LENSFX, 57, 59, 60, H_AA, 195, 196 }, 26 },
};
static bool g_qbForceRain, g_qbHold;   // (bench.inc: the quick bench's rain (weather.inc), the sun path's guard held (sunshadow.inc))
static float g_secKeep[kSecN][6];   // the player's values of a section's kill list while it is off
static bool g_secOff[kSecN];        // applied: off

static int SecOf(int p)   // the section whose switch p is (-1: none)
{
    return p >= P_SEC0 && p < P_SEC0 + kSecN ? p - P_SEC0 : -1;
}
// the player's own value of a parameter (kept aside while its section is off)
static float* UserVal(int p)
{
    for (int s = 0; s < kSecN; ++s)
        if (g_secOff[s])
            for (int k = 0; k < kSec[s].nk; ++k) if (kSec[s].kill[k] == p) return &g_secKeep[s][k];
    return &g_p[p].v;
}
static float UserValue(int p) { return *UserVal(p); }
// the section of a parameter's kill list (-1: none)
static int SecKilling(int p)
{
    for (int s = 0; s < kSecN; ++s)
        for (int k = 0; k < kSec[s].nk; ++k) if (kSec[s].kill[k] == p) return s;
    return -1;
}
// every frame (and after the keys, the ini): each section as its switch says
static void SecEnforce()
{
    for (int s = 0; s < kSecN; ++s) {
        const SecDef& S = kSec[s];
        bool on = g_p[P_SEC0 + s].v > 0.5f;
        if (!on) {
            for (int k = 0; k < S.nk; ++k) {
                float& v = g_p[S.kill[k]].v;
                if (!g_secOff[s] || v != S.killV[k]) { g_secKeep[s][k] = v; v = S.killV[k]; }   // (driven meanwhile: the new one kept)
            }
            if (!g_secOff[s]) Log("section %s: off", S.name);
            g_secOff[s] = true;
        } else if (g_secOff[s]) {
            for (int k = 0; k < S.nk; ++k) g_p[S.kill[k]].v = g_secKeep[s][k];
            g_secOff[s] = false;
            Log("section %s: on", S.name);
        }
    }
}
// 1.96b: our sun pass (the game's sun light drawn again by ours) only while a section drawn in it is on: all of them
// off, the game's own (the picture as the game's, not ours with everything at zero)
static bool SecSunPass()
{
    static const int k[] = { S_TIME, S_SKY, S_SHADOW, S_AO, S_AIR, S_REFL, S_WET, S_MAT, S_LAMPS, S_CARL, S_RAIN };
    for (int i = 0; i < (int)(sizeof(k) / sizeof(k[0])); ++i) if (g_p[P_SEC0 + k[i]].v > 0.5f) return true;
    return false;
}
// the ini read again: the values there are the player's (nothing kept aside)
static void SecReset() { for (int s = 0; s < kSecN; ++s) g_secOff[s] = false; }

// ---- quality presets: 1 low, 2 medium, 3 high (the defaults), 4 ultra; 0 custom
struct QualRow { short p; float v[4]; };
static const QualRow kQual[] = {
    { 115, { 0, 0, 1, 2 } },             // shadow map resolution: 2048 2048 3072 4096
    { 116, { 150, 210, 270, 450 } },     // shadow maps' reach, m
    { 133, { 1, 1, 1, 0 } },             // their mask at half resolution
    { 186, { 0, 1, 1, 1 } },             // buildings out of view cast (the city's height map)
    { 120, { 100, 150, 200, 400 } },     // kept shadows: objects a frame while redrawn
    { 62, { 0, 0.8f, 0.8f, 0.8f } },     // sky occlusion
    { 21, { 0, 0, 1, 1 } },              // bounce light
    { 67, { 40, 55, 70, 100 } },         // reflections' resolution, %
    { 165, { 60, 100, 150, 250 } },      // their rays, m
    { 180, { 0, 1, 1, 1 } },             // their source before the game's effects (a copy of the frame)
    { 104, { 0.2f, 0.3f, 0.5f, 0.8f } }, // cloud quality
    { 105, { 0, 0, 1, 2 } },             // cloud resolution: 25 25 50 80 %
    { 44, { 1, 1, 1, 0 } },              // lamp light at half resolution
    { 39, { 0, 1, 1, 1 } },              // lamp shadow maps
    { 36, { 0, 0, 1, 1 } },              // lamp shadows (screen space)
    { 41, { 50, 75, 100, 200 } },        // lamp shadow objects a frame
    { 164, { 64, 128, 256, 256 } },      // lamps a frame
    { 19, { 0, 1, 1, 1 } },              // light shafts
    { 125, { 0, 1, 1, 1 } },             // relief
    { 129, { 0, 1, 1, 1 } },             // parallax on roads
    { 132, { 0, 0, 1.5f, 1.5f } },       // parallax on buildings, cm
    { 231, { 0, 1, 1, 1 } },             // leaves sway
    { 232, { 0, 0, 1, 1 } },             // leaves flutter
    { 230, { 0, 1, 1, 1 } },             // drops on the lens
};
static const int kQualN = sizeof(kQual) / sizeof(kQual[0]);
static const char* kQualNames[5] = { "custom", "Low", "Medium", "High", "Ultra" };
static float g_qualCustom[kQualN];
static bool g_qualCustomHas;
// the preset the player's values are (0: none of them)
static int QualMatch()
{
    for (int q = 1; q <= 4; ++q) {
        bool all = true;
        for (int i = 0; i < kQualN && all; ++i) all = fabsf(UserValue(kQual[i].p) - kQual[i].v[q - 1]) < 1e-3f;
        if (all) return q;
    }
    return 0;
}
static bool IsQualMember(int p) { for (int i = 0; i < kQualN; ++i) if (kQual[i].p == p) return true; return false; }
static void QualApply(int q)
{
    if (q < 1 || q > 4) {   // custom: the player's own values back
        if (g_qualCustomHas) for (int i = 0; i < kQualN; ++i) *UserVal(kQual[i].p) = g_qualCustom[i];
        return;
    }
    for (int i = 0; i < kQualN; ++i) *UserVal(kQual[i].p) = kQual[i].v[q - 1];
    Log("quality: %s", kQualNames[q]);
}
// after a key changed parameter idx (before: its value then): the preset's choice and its custom memory
static const char* QualChanged(int idx, float before)
{
    if (idx == P_QUALITY) {
        int now = (int)(g_p[idx].v + 0.5f), was = (int)(before + 0.5f);
        if (now == was) return 0;
        if (was == 0) { for (int i = 0; i < kQualN; ++i) g_qualCustom[i] = UserValue(kQual[i].p); g_qualCustomHas = true; }
        QualApply(now);
        return now ? 0 : g_qualCustomHas ? "quality: custom (your settings back)" : "quality: custom";
    }
    if (IsQualMember(idx)) {
        int m = QualMatch();
        if ((int)(g_p[P_QUALITY].v + 0.5f) != m) { g_p[P_QUALITY].v = (float)m; return m ? 0 : "changed by hand: quality custom"; }
    }
    return 0;
}
// the ini: custom's values (QualityCustom=...), the preset shown by what the values are
static void QualLoad()
{
    char b[512];
    GetPrivateProfileStringA("NewLook", "QualityCustom", "", b, sizeof(b), g_iniPath);
    char* p = b;
    int i = 0;
    for (; i < kQualN; ++i) { char* e; float v = strtof(p, &e); if (e == p) break; g_qualCustom[i] = v; p = e; }
    g_qualCustomHas = i == kQualN;
    g_p[P_QUALITY].v = (float)QualMatch();
}
static void QualSaveLine(char* out)   // QualityCustom's value
{
    if (g_p[P_QUALITY].v < 0.5f) { for (int i = 0; i < kQualN; ++i) g_qualCustom[i] = UserValue(kQual[i].p); g_qualCustomHas = true; }
    *out = 0;
    if (!g_qualCustomHas) return;
    for (int i = 0; i < kQualN; ++i) sprintf(out + strlen(out), "%s%.3f", i ? " " : "", g_qualCustom[i]);
}
