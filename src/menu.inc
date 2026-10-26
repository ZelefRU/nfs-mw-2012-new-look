// ---------------------------------------------------------------- menu (overlay text)
// Lines of coloured pieces, drawn with GDI into the overlay texture. 1.96: two views (the first line, Menu): simple,
// the sections (sections.inc) with their switch and their main settings under the selected one; advanced, every
// setting by section (sub-headings in them), only the selected section open. U opens a list of the views. Load
// figures come last, in grey.

// a group of the menu: group 0 the menu and quality, then the sections
static int g_order[320], g_orderGroup[320], g_orderCount, g_groupCount;   // menu position -> parameter, its group
static bool g_menuAdv;   // the view the order was made for

static bool MenuAdv() { return g_p[P_MENU_ADV].v > 0.5f; }
// the rows of group g in the current view (sub-headings < 0 too); n: how many
static int GroupRows(int g, short* rows)
{
    int n = 0;
    if (g == 0) {
        rows[n++] = P_MENU_ADV; rows[n++] = P_QUALITY;
        if (MenuAdv()) rows[n++] = P_FAST;
        return n;
    }
    const SecDef& S = kSec[g - 1];
    rows[n++] = (short)(P_SEC0 + g - 1);
    if (MenuAdv()) for (int k = 0; k < S.na; ++k) rows[n++] = S.adv[k];
    else for (int k = 0; k < S.ns; ++k) rows[n++] = S.simple[k];
    return n;
}
static void MenuInit()
{
    int keep = g_orderCount ? g_order[g_sel % g_orderCount] : P_MENU_ADV;
    g_menuAdv = MenuAdv();
    g_orderCount = 0;
    g_groupCount = 1 + kSecN;
    for (int gI = 0; gI < g_groupCount; ++gI) {
        short rows[80];
        int n = GroupRows(gI, rows);
        for (int k = 0; k < n && g_orderCount < 320; ++k)
            if (rows[k] >= 0) { g_order[g_orderCount] = rows[k]; g_orderGroup[g_orderCount++] = gI; }
    }
    // the selection kept (in the simple view a setting it does not show: its section's switch)
    int to = -1, sec = SecKilling(keep) >= 0 ? SecKilling(keep) : -1;
    for (int k = 0; k < g_orderCount && to < 0; ++k) if (g_order[k] == keep) to = k;
    if (to < 0)
        for (int s = 0; s < kSecN && sec < 0; ++s)
            for (int k = 0; k < kSec[s].na && sec < 0; ++k) if (kSec[s].adv[k] == keep) sec = s;
    if (to < 0 && sec >= 0) for (int k = 0; k < g_orderCount && to < 0; ++k) if (g_order[k] == P_SEC0 + sec) to = k;
    g_sel = to < 0 ? 0 : to;
}
static Param& SelParam() { return g_p[g_order[g_sel % g_orderCount]]; }
static int SelIdx() { return g_order[g_sel % g_orderCount]; }
// the group (0: menu, 1 + s: section s) a parameter is shown in, -1 none
static int GroupOfParam(int p)
{
    if (p == P_MENU_ADV || p == P_QUALITY || p == P_FAST) return 0;
    if (SecOf(p) >= 0) return 1 + SecOf(p);
    for (int s = 0; s < kSecN; ++s) for (int k = 0; k < kSec[s].na; ++k) if (kSec[s].adv[k] == p) return 1 + s;
    return -1;
}

// the simple view's own names (plain words); others: the setting's label
// 1.96l: the settings added since the last release: a dot before them, easy to find (emptied at each release). 1.98:
// orange, and kept for the experimental ones (the grass, the weather section: its switch and settings)
static const int kNewParams[] = { 261, 262, 263, 264, 208, 209, 210, 211, 212, 213, 214, 215, 216, 237, 238 };
static const int kExpSection = 2;   // (WEATHER: an orange dot by its name)
static bool IsNewParam(int p)
{
    for (int k = 0; k < (int)(sizeof(kNewParams) / sizeof(kNewParams[0])); ++k) if (kNewParams[k] == p) return true;
    return false;
}

static const char* SimpleLabel(int p)
{
    switch (p) {
    case P_MENU_ADV: return "Menu";
    case P_QUALITY: return "Quality";
    case 138: return "Clock shift, h"; case 139: return "Sunrise at"; case 140: return "Sunset at";
    case 103: return "Sky (a preset)"; case 93: return "Cloud cover"; case 104: return "Cloud quality (costs)";
    case 237: return "Weather now"; case 211: return "Changes by itself"; case 238: return "A change takes, minutes";
    case 115: return "Shadow quality (costs)"; case 116: return "Shadow reach, m (costs)"; case 114: return "Shadow softness (sun size)";
    case 6: return "Ambient occlusion"; case 62: return "Sky occlusion (costs)"; case 21: return "Bounce light (costs)";
    case 16: return "Haze"; case 19: return "Light shafts (costs)";
    case 64: return "Reflections"; case 67: return "Reflection resolution, % (costs)"; case 265: return "Sharp detail at full size";
    case 65: return "Wet ground"; case 112: return "Small puddles (mirrors)"; case 177: return "Big puddles (matte)";
    case 129: return "Parallax on roads (costs)"; case 125: return "Relief (costs)"; case 121: return "Lighting model";
    case 26: return "Lamp brightness"; case 39: return "Lamp shadows on walls (costs)"; case 44: return "Lamp light at half resolution";
    case 71: return "Headlight beams in the air"; case 75: return "Traffic cars' lights";
    case 217: return "Falling rain"; case 230: return "Drops on the lens"; case 200: return "Drops on the car";
    case 222: return "Wind, m/s"; case 231: return "Leaves sway (costs)";
    case 61: return "Look (a preset)"; case 46: return "Exposure, EV"; case 47: return "Contrast";
    }
    return g_p[p].label;
}

// parameters that are a choice, not a number
static const char* ChoiceText(int idx, float v)
{
    int c = (int)(v + 0.5f);
    if (SecOf(idx) >= 0) {   // 1.96: a section's switch
        if (!c) return "OFF";
        if (idx == P_SEC0 + S_TIME && g_p[P_SS].v < 0.5f) return "ON (needs SHADOWS > our shadow maps)";
        return "ON";
    }
    switch (idx) {
    case P_QUALITY: return kQualNames[c < 0 ? 0 : c > 4 ? 4 : c];
    case P_MENU_ADV: return c ? "ADVANCED (every setting)" : "SIMPLE (the main settings; K/L: advanced)";
    case P_FAST: case P_LAMP_AUTO: case P_AUTO_LAMPS: case P_LAMP_SHADOW_MAPS: case P_LAMP_SHADOW_CUT: case P_SOFT_SUN: case P_LOOK: case P_SKY: case P_SS: return c ? "ON" : "OFF";
    case P_POM: return c ? "ON" : "OFF";
    case 208: case 211: case 227: case 230: return c ? "ON" : "OFF";   // 1.82: the weather, by itself; 1.84: the dark patch under the car
    case P_SS_HALF: return c ? "half (edges kept)" : "full";
    case P_SS_SKIP: return c ? "ON (only past ours)" : "OFF";
    case P_SUN_PATH: return !c ? "the game's (a loop)" : g_p[P_SS].v > 0.5f ? "ours (by the clock)" : "ours: needs SHADOWS > our shadow maps ON";
    case 187: return c <= 0 ? "OFF (all maps, as usual)" : c == 1 ? "only map 1 (red, near)" : c == 2 ? "only map 2 (green)" : c == 3 ? "only map 3 (blue)" : "only map 4 (yellow, far)";
    case P_SUN_DEBUG: case P_SKY_NIGHT: case P_SSR_SKY: case P_LG + 7: case P_NOISE + 3: case P_SSR_EARLY: case 186: case 188: case 189: case 190: case 195: return c ? "ON" : "OFF";
    case 192: return c <= 0 ? "auto (seen sky, the probe where unseen)" : c == 1 ? "seen sky only (its mean where unseen)" : c == 2 ? "probe only (matched)" : "probe only, as drawn (no matching)";
    case 194: return c ? "ON (stored by the sky's level now)" : "OFF (as seen)";
    case P_NOISE: return c <= 0 ? "none (raw: the noise as it is)" : c == 1 ? "by gloss (puddles sharp)" : "always 3x3 (soft)";
    case P_LG + 1: return c ? "ON" : "OFF (not drawn: faster)";
    case P_LG: { static char b[96]; if (c >= 0 && c < g_presetCount) sprintf(b, "%s   (%d lamps)", g_presets[c].name, c < 32 ? g_groupLamps[c] : 0); else sprintf(b, "-"); return b; }
    case P_LG + 5: { static char b[64]; if (v > 0) sprintf(b, "%.0f m", v); else sprintf(b, "the Lamps' (%.0f m)", g_p[P_LAMP_RADIUS].v); return b; }
    case P_LG + 6: { static char b[64]; if (v > 0) sprintf(b, "%.0f m", v); else sprintf(b, "the Lamps' (%.0f m)", g_p[P_LAMP_REACH].v); return b; }
    case P_HAZE_DAWN: { static char b[64]; sprintf(b, "x%.2f   (now x%.2f%s)", v, g_hazeMul, g_sunActive ? "" : ": our sun off"); return b; }
    case P_NIGHT_EV: { static char b[64]; sprintf(b, "%+.2f EV   (now %.0f%% night)%s", v, g_nightF * 100, g_p[P_LOOK].v > 0.5f ? "" : "  needs LOOK ON"); return b; }
    case P_SUN_COL: return c ? "ours (by its height: orange low)" : "the game's";
    case P_CLOCK: case P_SUNRISE: case P_SUNSET: {   // as a time of day; the clock: now, the sun
        static char b[96];
        float h = idx == P_CLOCK ? g_sp.hour : v;
        int hm = (int)(h * 60 + 0.5f) % 1440;
        if (idx == P_CLOCK) sprintf(b, "%+.2f h   now %02d:%02d, our %s %.0f deg up%s", v, hm / 60, hm % 60, g_sp.moon ? "moon" : "sun", g_sp.ourEl,
                                    g_sunActive ? "" : " (our sun off)");
        else sprintf(b, "%02d:%02d", hm / 60, hm % 60);
        return b;
    }
    case P_MOON_SHADOWS: return c ? "our moon (the sun's arc, our shadows)" : "the game's moon (its shadows)";
    case P_PBR: return c ? "modern (GGX, PBR)" : "the game's (Blinn-Phong)";
    case P_SS + 2: return c <= 0 ? "2048 x 4 (64 MB)" : c == 1 ? "3072 x 4 (144 MB)" : "4096 x 4 (256 MB)";
    case P_BOUNCE_ALL: return c ? "all lights" : "sun only";
    case P_LAMP_SRC: return c == 0 ? "found only" : c == 1 ? "map + found" : "map only";
    case P_LAMP_RES: return c ? "half of the screen" : "the screen's";
    case P_LOOK_PRESET: return c == 1 ? "overcast photo" : c == 2 ? "clear and punchy" : c == 3 ? "warm film" : c == 4 ? "photo (DLSS-like)" : "custom";
    case P_KNOCK: return c ? "falls with the post" : "goes out";
    case P_SKY + 14: return c <= 0 ? "25% of the screen" : c == 1 ? "50% of the screen" : c == 2 ? "80% of the screen" : "100% (full)";
    case P_SKY_VIEWS: return c <= 0 ? "the game's sky (fastest)" : c == 1 ? "ours, without our clouds" : "ours with clouds (simpler)";
    case 237: {   // 1.89: the weather's kind
        static char b[96];
        if (c <= 0) return "by hand (rain, clouds below)";
        int k = c - 1 < kWxPhaseN ? c - 1 : kWxPhaseN - 1;
        sprintf(b, "%s%s", kWxPhases[k].name, g_p[P_WX + 3].v > 0.5f ? "   (by itself)" : "");
        return b;
    }
    case P_SKY + 12: { static const char* w[] = { "custom", "clear", "fair (cumulus)", "cloudy", "overcast", "storm", "cirrus" }; return w[c < 0 ? 0 : c > 6 ? 6 : c]; }
    }
    return 0;
}

// the views, in groups, for the U list
struct ViewItem { int view; const char* group; };
static const ViewItem kViewList[] = {
    { 0, "picture" }, { 1, "picture" }, { 24, "picture" },
    { 4, "shadows" }, { 33, "shadows" }, { 5, "shadows" }, { 2, "shadows" }, { 16, "shadows" }, { 17, "shadows" }, { 18, "shadows" },
    { 3, "light" }, { 7, "light" }, { 21, "light" }, { 22, "light" }, { 23, "light" }, { 8, "light" }, { 6, "light" },
    { 19, "air" }, { 20, "air" },
    { 25, "surfaces" }, { 26, "surfaces" }, { 27, "surfaces" }, { 34, "surfaces" }, { 35, "surfaces" }, { 36, "surfaces" }, { 37, "surfaces" }, { 38, "surfaces" },
    { 28, "reflections" }, { 29, "reflections" }, { 30, "reflections" }, { 31, "reflections" }, { 32, "reflections" },
};
static const int kViewListCount = sizeof(kViewList) / sizeof(kViewList[0]);
static bool g_viewPicker;
static int ViewListPos() { for (int i = 0; i < kViewListCount; ++i) if (kViewList[i].view == g_view) return i; return 0; }

// ---- lines of coloured pieces
struct MSeg { char t[160]; COLORREF c; };
struct MLine { MSeg s[6]; int n; };
static MLine g_ml[90];
static int g_mlN;
static void MNew() { if (g_mlN < 90) { g_ml[g_mlN].n = 0; ++g_mlN; } }
static void MSegAdd(COLORREF c, const char* fmt, ...)
{
    if (!g_mlN) MNew();
    MLine& L = g_ml[g_mlN - 1];
    if (L.n >= 6) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(L.s[L.n].t, sizeof(L.s[0].t) - 1, fmt, ap);
    va_end(ap);
    L.s[L.n].t[sizeof(L.s[0].t) - 1] = 0;
    L.s[L.n].c = c;
    ++L.n;
}
#define MLINE(c, ...) do { MNew(); MSegAdd(c, __VA_ARGS__); } while (0)

static const COLORREF C_TITLE = RGB(245, 245, 245), C_ON = RGB(90, 225, 120), C_OFF = RGB(235, 95, 85), C_GROUP = RGB(110, 190, 255),
                      C_ITEM = RGB(205, 205, 205), C_SEL = RGB(255, 215, 90), C_HINT = RGB(135, 165, 195), C_DIM = RGB(125, 125, 125),
                      C_MSG = RGB(255, 190, 110), C_OBJ = RGB(240, 160, 90), C_EXP = RGB(255, 160, 40);

static void BuildMenu()
{
    g_mlN = 0;
    // ---- title
    MNew();
    MSegAdd(C_TITLE, "NewLook 1.98   ");
    MSegAdd(g_enabled ? C_ON : C_OFF, g_enabled ? "[ ON ]" : "[ OFF - the game's own picture ]");
    MSegAdd(C_DIM, "  Y      ");
    MSegAdd(C_TITLE, "view: ");
    MSegAdd(g_view ? C_SEL : C_ITEM, "%s", kViews[g_view]);
    MSegAdd(C_DIM, "  (U: list)");
    if (g_lampMsg[0]) MLINE(C_MSG, "%s", g_lampMsg);
    MNew();

    bool editSel = g_view == 24 && (g_lampSel >= 0 || g_zoneSel >= 0);   // I/O/K/L work on the selected lamp / zone
    if (g_viewPicker) {
        // ---- the view list
        MLINE(C_GROUP, "VIEWS   [I/O] choose (shown at once)   [U] close");
        const char* last = "";
        int pos = ViewListPos();
        for (int i = 0; i < kViewListCount; ++i) {
            if (strcmp(last, kViewList[i].group)) { MLINE(C_DIM, "  %s", kViewList[i].group); last = kViewList[i].group; }
            MLINE(i == pos ? C_SEL : C_ITEM, "    %s %s", i == pos ? ">" : " ", kViews[kViewList[i].view]);
        }
    } else {
        // ---- parameters, in groups (the selected one's group open). 1.96: group 0 the menu and quality (always shown),
        // then the sections: simple, the switch line with its main settings under it when selected; advanced, a heading
        // per section, its every setting (with sub-headings) when selected
        int selGroup = editSel ? -1 : g_orderGroup[g_sel % g_orderCount];
        int selP = editSel ? -1 : SelIdx();
        bool adv = MenuAdv();
        for (int gI = 0; gI < g_groupCount; ++gI) {
            short rows[80];
            int n = GroupRows(gI, rows);
            bool open = gI == selGroup;
            int s = gI - 1;
            bool secOn = s < 0 || g_p[P_SEC0 + s].v > 0.5f;
            if (s >= 0 && adv) {
                MNew();
                if (s == kExpSection) {
                    int pad = 40 - 2 - (int)strlen(kSec[s].name);
                    MSegAdd(C_GROUP, "%s %s ", open ? "[-]" : "[+]", kSec[s].name);
                    MSegAdd(C_EXP, "\x95");
                    MSegAdd(C_GROUP, "%*s", pad > 0 ? pad : 0, "");
                } else MSegAdd(C_GROUP, "%s %-40s", open ? "[-]" : "[+]", kSec[s].name);
                MSegAdd(secOn ? C_ON : C_OFF, "%s", secOn ? "ON" : "OFF");
                if (g_qbCost[s] > -1e8f) MSegAdd(g_qbCost[s] > 2 ? C_OFF : C_DIM, "   %s", QbCostText(s));
                if (!open) continue;
            }
            if (s == 0 && !adv) MNew();   // (a gap after the menu and quality)
            // a long section: the rows round the selection
            int selRow = 0;
            for (int k = 0; k < n; ++k) if (rows[k] == selP) selRow = k;
            int first = 0, last = n;
            if (open && n > 26) { first = selRow - 12 < 0 ? 0 : selRow - 12; last = first + 26 < n ? first + 26 : n; if (last - first < 26) first = last - 26 < 0 ? 0 : last - 26; }
            if (first > 0) MLINE(C_DIM, "        ... %d more above", first);
            for (int k = first; k < last; ++k) {
                int idx = rows[k];
                bool head = s >= 0 && k == 0 && !adv;   // the simple view's switch line: the section's name
                if (!head && !open && s >= 0) break;
                if (idx < 0) { MLINE(C_DIM, "      - %s -", kSubHeads[-1 - idx]); continue; }
                bool sel = idx == selP;
                const Param& P = g_p[idx];
                float uv = UserValue(idx);
                bool dim = !secOn && SecOf(idx) < 0;   // (a switched-off section's settings: grey, their own values)
                MNew();
                if (head) MSegAdd(sel ? C_SEL : C_GROUP, "%s %-40s", sel ? ">" : " ", kSec[s].name);
                else if (SecOf(idx) >= 0) MSegAdd(sel ? C_SEL : C_ITEM, "  %s %-34s", sel ? ">" : " ", "the whole section");
                else {
                    const char* ind = adv || s < 0 ? "  " : "      ";
                    if (IsNewParam(idx)) { MSegAdd(C_EXP, "%.*s\x95", (int)strlen(ind) - 1, ind); ind = ""; }   // (cp1251 / 1252: a bullet)
                    MSegAdd(sel ? C_SEL : dim ? C_DIM : C_ITEM, "%s%s %-34s", ind, sel ? ">" : " ", adv ? P.label : SimpleLabel(idx));
                }
                const char* ch = ChoiceText(idx, uv);
                if (ch) MSegAdd(dim ? C_DIM : !strcmp(ch, "OFF") ? C_OFF : !strncmp(ch, "ON", 2) ? C_ON : (sel ? C_SEL : C_ITEM), "%s", ch);
                else MSegAdd(dim ? C_DIM : sel ? C_SEL : C_ITEM, "%.2f", uv);
                if (head && g_qbCost[s] > -1e8f) MSegAdd(g_qbCost[s] > 2 ? C_OFF : C_DIM, "   %s", QbCostText(s));
                if (!head && uv != P.def && SecOf(idx) < 0 && idx != P_QUALITY && idx != P_MENU_ADV) MSegAdd(C_DIM, "   (default %g)", P.def);
                if (dim && SecKilling(idx) >= 0) MSegAdd(C_DIM, "   (the section is off)");
                if (adv && g_bench[idx].valid) {
                    const BenchRes& r = g_bench[idx];
                    double c = r.on - r.off;
                    if (fabs(c) < r.noise) MSegAdd(C_DIM, "   bench: under the noise (%.2f ms)", r.noise);
                    else MSegAdd(c > 1.0 ? C_OFF : C_DIM, "   bench %+.2f ms%s", c, r.bound ? "" : " (or more)");
                }
            }
            if (last < n && open) MLINE(C_DIM, "        ... %d more below", n - last);
        }
        // what the selected section is (one line)
        if (selGroup > 0) { MNew(); MLINE(C_HINT, "%s: %s", kSec[selGroup - 1].name, kSec[selGroup - 1].hint); }
        // 1.96: the game's own anti-aliasing draws the picture larger (2x: 2560x2880 for 2560x1440): every effect costs that much more
        if (g_bbW && g_bbH && g_rtW && (double)g_rtW * g_rtH > 1.3 * g_bbW * g_bbH) {
            double x = (double)g_rtW * g_rtH / ((double)g_bbW * g_bbH);
            MLINE(C_MSG, "The game's anti-aliasing is ON: it draws %ux%u for a %ux%u screen (x%.1f the pixels): every effect costs x%.1f.", g_rtW, g_rtH, g_bbW, g_bbH, x, x);
            MLINE(C_MSG, "    Turned off in the game's graphics options: much faster.");
        }
    }

    if (g_carEditOn) { MNew(); MLINE(C_OBJ, "%s", g_carEditMsg); }

    // ---- the selected lamp / zone (lamp editor)
    if (g_view == 24) {
        MNew();
        if (g_lampSel >= 0 && g_lampSel < g_mapCount) {
            const MapLamp& L = g_map[g_lampSel];
            MLINE(C_OBJ, "LAMP %d   at %.1f %.1f %.1f", g_lampSel + 1, L.p[0], L.p[1], L.p[2]);
            for (int q = 0; q < 5; ++q) {
                char v[96];
                if (q == 0) sprintf(v, "%s", L.group < 0 ? "custom (own colour)" : g_presets[L.group].name);
                else if (q == 1) sprintf(v, "%.0f%s", g_selHue, L.group >= 0 ? "   (set by the group)" : "");
                else if (q == 2) sprintf(v, "%.2f%s", g_selSat, L.group >= 0 ? "   (set by the group)" : "");
                else if (q == 3) sprintf(v, "%.2f%s", g_selPow, L.group >= 0 ? "   (set by the group)" : "");
                else if (L.channel) sprintf(v, "%d   (cut by zones of channel %d and zones for all)", L.channel, L.channel);
                else sprintf(v, "0   (cut only by zones for all lamps)");
                MLINE(q == g_lampProp ? C_SEL : C_ITEM, "  %s %-14s %s", q == g_lampProp ? ">" : " ", kLampProps[q], v);
            }
            if (L.hasObj) {
                static const char* st[] = { "-", "looking for it in the game's memory", "found, standing", "KNOCKED DOWN", "knocked down, gone" };
                MLINE(C_ITEM, "    tied to the object at %.1f %.1f %.1f: %s", L.obj[0], L.obj[1], L.obj[2], st[L.propState >= 0 && L.propState <= 4 ? L.propState : 0]);
            } else MLINE(C_DIM, "    not tied to an object");
        } else if (g_zoneSel >= 0 && g_zoneSel < g_zoneCount) {
            const Zone& Z = g_zone[g_zoneSel];
            MLINE(C_OBJ, "ZONE %d   at %.1f %.1f %.1f   (light from outside does not get in)", g_zoneSel + 1, Z.c[0], Z.c[1], Z.c[2]);
            float v[5] = { Z.h[0], Z.h[1], Z.h[2], Z.yaw, Z.soft };
            for (int q = 0; q < 5; ++q) MLINE(q == g_zoneProp ? C_SEL : C_ITEM, "  %s %-22s %.1f", q == g_zoneProp ? ">" : " ", kZoneProps[q], v[q]);
            if (Z.channel) MLINE(g_zoneProp == 5 ? C_SEL : C_ITEM, "  %s %-22s %d   (lamps with a white ring)", g_zoneProp == 5 ? ">" : " ", kZoneProps[5], Z.channel);
            else MLINE(g_zoneProp == 5 ? C_SEL : C_ITEM, "  %s %-22s all lamps", g_zoneProp == 5 ? ">" : " ", kZoneProps[5]);
        } else MLINE(C_DIM, "nothing selected - put the red cross on a lamp dot or a zone square and click");
        MLINE(C_ITEM, "in the cross: %s%s%s", g_tgtDel >= 0 ? "map lamp (red ring)  " : "", g_tgtAdd >= 0 ? "found lamp (white ring)" : "",
              g_tgtDel < 0 && g_tgtAdd < 0 ? (g_aim[3] > 0.5f ? "a surface" : "nothing") : "");
        if (g_obConnect || strcmp(g_obStatus, "off")) MLINE(g_obConnect ? C_SEL : C_ITEM, "object tools: %s%s", g_obConnect ? "CONNECT - click the object | " : "", g_obStatus);
    }

    // ---- the world editor (worldedit.inc)
    if (g_weOn) {
        MNew();
        if (g_weSetN > 1) MLINE(C_OBJ, "%d SELECTED (moved / turned / scaled / hidden together; the last picked:)", g_weSetN);
        if (g_weSel) {
            const WeEdit* e = WeFind(g_weSel);
            MLINE(C_OBJ, "%s at %.2f %.2f %.2f%s%s", g_weSelIsPart ? "PIECE of the object" : "OBJECT", g_weSelT[0], g_weSelT[1], g_weSelT[2], e && e->hidden ? "   HIDDEN" : "",
                  g_weSelSeen + 2 < g_frame ? "   (not drawn now)" : "");
            if (e && WeIsMoved(*e)) MLINE(C_ITEM, "  moved %.2f %.2f %.2f m   turned %.0f deg   scale %.2f", e->off[0], e->off[1], e->off[2], e->yaw, e->scale);
        } else MLINE(C_DIM, "nothing selected - put the red cross on an object and click (LMB)");
        MLINE(C_ITEM, "world edits: %d (%d hidden, %d moved)%s%s", g_weN, g_weHidden, g_weMoved, g_weApply ? "" : "   EDITS OFF (Home)",
              g_weShowHidden ? "   hidden ones SHOWN in red (Ins)" : "");
    }

    // ---- keys
    MNew();
    if (g_weOn) {
        MLINE(C_HINT, "world editor: LMB pick the object / drag the arrows and the ring   MMB pick one piece of it   Ctrl+LMB / Ctrl+MMB add / remove");
        MLINE(C_HINT, "              Del hide / show (a copy: delete)   Ctrl+D copy (props: worldtools\\bake.bat puts them in the game)");
        MLINE(C_HINT, "              +/- scale   [ ] turn 15 deg (Shift: 1)");
        MLINE(C_HINT, "              Backspace reset   Ins show hidden   Home edits on/off   Ctrl+Z undo  Ctrl+Y redo (%d / %d)   Ctrl+J close",
              g_weUndoN, g_weRedoN);
    }
    if (g_viewPicker) MLINE(C_HINT, "keys: I/O choose a view   U close");
    else if (editSel) MLINE(C_HINT, "keys: I/O pick a value   K/L change   arrows/squares/ring: hold LMB and move the mouse   Del delete");
    else if (!MenuAdv()) {
        MLINE(C_HINT, "keys: I/O select   K/L change   Shift+I/O next section   J default   H save   P hide menu   Y the mod on / off");
        MLINE(C_HINT, "      Ctrl+B benchmark: what costs how much here, day and night (2-3 min; park the car, hands off; B stops)");
    } else {
        MLINE(C_HINT, "keys: I/O select   Shift+I/O section   K/L change   J default   H save all   P hide   Shift+P reload   Shift+U capture   U views");
        MLINE(C_HINT, "bench: Ctrl+B quick (sections, 2-3 min)   Ctrl+Shift+B passes (3 min)   Shift+B the selected setting   B stops");
    }
    if (g_view == 24) {
        MLINE(C_HINT, "editor: LMB select / drag   Shift+J or Del delete");
        MLINE(C_HINT, "        Shift+O tie the lamp to an object   Shift+I post template   Shift+B zone   Shift+T shadow research");
        MLINE(C_HINT, "        Ctrl+Z undo  Ctrl+Y redo  (%d / %d)   Ctrl+C copy lamp   Ctrl+V paste   Ctrl+Shift+V paste to lamps within 30 m",
              g_undoN, g_redoN);
    }

    // ---- load (grey, last)
    MNew();
    char pt[512];
    ProfText(pt);
    char* nl = strchr(pt, '\n');
    if (nl) {
        *nl = 0;
        char* l2 = nl + 1;
        char* e = strchr(l2, '\n');
        if (e) *e = 0;
        MLINE(C_DIM, "%s", pt);
        if (MenuAdv()) MLINE(C_DIM, "%s", l2);
    }
    for (int q = 0; q < 3; ++q) if (g_qbSummary[q][0]) MLINE(g_qbPhase >= 0 ? C_MSG : q ? C_DIM : C_ITEM, "%s", g_qbSummary[q]);
    if (!MenuAdv()) { if (g_compileError[0]) MLINE(C_OFF, "HLSL: %.120s", g_compileError); return; }
    MLINE(C_DIM, "lamps: %d found, %d in map, %d lit now   templates %d, auto %d added / %d tied   zones %d", g_lampCount, g_mapCount, g_lampUse,
          g_tplCount, g_autoAdded, g_autoTied, g_zoneCount);
    if (g_benchMod.valid) MLINE(C_DIM, "bench: the whole mod costs %+.2f ms of GPU frame (%.2f -> %.2f ms)", g_benchMod.on - g_benchMod.off, g_benchMod.off, g_benchMod.on);
    MLINE(C_DIM, "lamp shadow maps: %u faces drawn, %u objects drawn last frame (of %u)", g_lsFacesMade, g_lsLastDraws, g_lsLastIdx);
    if (g_wxRain >= 0)   // 1.82
        MLINE(C_DIM, "weather: rain %.2f (to %.2f), clouds %.2f (to %.2f), wet ground %.2f, puddles %.2f%s%s%s", g_wxRain, g_wxRainTo, g_wxCloud, g_wxCloudTo,
              g_wxWet, g_wxPuddle, g_p[P_WX + 3].v > 0.5f || g_p[P_WX_KIND].v > 0.5f ? "   going to: " : "",
              g_p[P_WX + 3].v > 0.5f || g_p[P_WX_KIND].v > 0.5f ? kWxPhases[g_wxPhase].name : "",
              g_p[P_WX + 3].v > 0.5f && g_wxPhaseLeft > 0 ? " (by itself)" : "");
    MLINE(C_DIM, "hitches (frames over 50 ms) since the last log line: %u (%u with our effects off), the worst %.0f ms (%.0f of it the game's own / not hooked)",
          g_htHitches, g_htHitchesOff, g_htWorst, g_htWorstRest);   // 1.81d (hitch.inc; the log has each one's parts)
    if (g_compileError[0]) MLINE(C_OFF, "HLSL: %.120s", g_compileError);
    else if (!g_sunCount && g_lastReject[0]) MLINE(C_OFF, "skipped: %s", g_lastReject);
}
