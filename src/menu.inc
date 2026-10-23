// ---------------------------------------------------------------- menu (overlay text)
// Lines of coloured pieces, drawn with GDI into the overlay texture. Parameters are shown in groups; only the group
// of the selected one is open. U opens a list of the views. Load figures come last, in grey.

struct MenuGroup { const char* name; int items[40]; int n; };
static const MenuGroup kGroups[] = {
    { "SUN & MOON", { 135, 138, 139, 140, 141, 142, 136, 9, 148, 14, 143, 137, 144, 153, 154, 155, 146, 147, 145 }, 19 },
    { "SHADOWS", { 113, 114, 3, 43, 11, 118, 4, 5, 119, 116, 115, 185, 186, 187, 133, 134, 117, 120, 12, 13, 0, 1, 2, 10 }, 23 },
    { "AMBIENT & OCCLUSION", { 8, 38, 62, 63, 15, 6, 7 }, 7 },
    { "AIR", { 16, 149, 150, 151, 152, 17, 70, 20, 18, 19, 68, 69 }, 12 },
    { "CAR LIGHTS", { 71, 74, 72, 73, 75, 77, 78, 79, 76, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90 }, 20 },
    { "SKY", { 91, 103, 93, 100, 101, 102, 99, 94, 95, 96, 104, 105, 92, 97, 98 }, 15 },
    { "MATERIALS (LIGHTING MODEL)", { 129, 130, 132, 131, 125, 128, 126, 127, 121, 122, 123, 124 }, 12 },
    { "REFLECTIONS", { 64, 165, 166, 180, 109, 65, 175, 167, 112, 184, 182, 183, 177, 181, 178, 179, 110, 111, 66, 67 }, 20 },
    { "NOISE / SPARKLE (find what makes it)", { 173, 168, 170, 176, 171, 169, 172, 174 }, 8 },
    { "LOOK (COLOUR)", { 45, 61, 46, 47, 106, 48, 49, 50, 51, 107, 108, 52, 53, 54, 55, 56, 57, 58, 59, 60 }, 20 },
    { "BOUNCE LIGHT", { 21, 22, 24, 25 }, 4 },
    { "LAMPS", { 26, 164, 35, 44, 39, 40, 41, 42, 36, 37, 28, 29, 30, 33, 34, 27, 31, 32 }, 18 },
    { "LAMP GROUPS (the map's lamps by kind)", { 156, 157, 160, 158, 159, 161, 162, 163 }, 8 },
    { "SYSTEM", { 23 }, 1 },
};
static const int kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);
static int g_order[256], g_orderGroup[256], g_orderCount;   // menu position -> parameter, its group

static void MenuInit()
{
    g_orderCount = 0;
    for (int gI = 0; gI < kGroupCount; ++gI)
        for (int k = 0; k < kGroups[gI].n && g_orderCount < 256; ++k) { g_order[g_orderCount] = kGroups[gI].items[k]; g_orderGroup[g_orderCount++] = gI; }
}
static Param& SelParam() { return g_p[g_order[g_sel % g_orderCount]]; }

// parameters that are a choice, not a number
static const char* ChoiceText(int idx, float v)
{
    int c = (int)(v + 0.5f);
    switch (idx) {
    case P_FAST: case P_LAMP_AUTO: case P_AUTO_LAMPS: case P_LAMP_SHADOW_MAPS: case P_LAMP_SHADOW_CUT: case P_SOFT_SUN: case P_LOOK: case P_SKY: case P_SS: return c ? "ON" : "OFF";
    case P_POM: return c ? "ON" : "OFF";
    case P_SS_HALF: return c ? "half (edges kept)" : "full";
    case P_SS_SKIP: return c ? "ON (only past ours)" : "OFF";
    case P_SUN_PATH: return !c ? "the game's (a loop)" : g_p[P_SS].v > 0.5f ? "ours (by the clock)" : "ours: needs SHADOWS > our shadow maps ON";
    case 187: return c <= 0 ? "OFF (all maps, as usual)" : c == 1 ? "only map 1 (red, near)" : c == 2 ? "only map 2 (green)" : c == 3 ? "only map 3 (blue)" : "only map 4 (yellow, far)";
    case P_SUN_DEBUG: case P_SKY_NIGHT: case P_SSR_SKY: case P_LG + 7: case P_NOISE + 3: case P_SSR_EARLY: case 186: return c ? "ON" : "OFF";
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
    { 25, "surfaces" }, { 26, "surfaces" }, { 27, "surfaces" }, { 34, "surfaces" }, { 35, "surfaces" }, { 36, "surfaces" },
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
                      C_MSG = RGB(255, 190, 110), C_OBJ = RGB(240, 160, 90);

static void BuildMenu()
{
    g_mlN = 0;
    // ---- title
    MNew();
    MSegAdd(C_TITLE, "NewLook 1.64   ");
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
        // ---- parameters, in groups (the selected one's group open)
        int selGroup = editSel ? -1 : g_orderGroup[g_sel % g_orderCount];
        for (int gI = 0; gI < kGroupCount; ++gI) {
            bool open = gI == selGroup;
            MLINE(C_GROUP, "%s %s", open ? "[-]" : "[+]", kGroups[gI].name);
            if (!open) continue;
            for (int k = 0; k < kGroups[gI].n; ++k) {
                int idx = kGroups[gI].items[k];
                bool sel = g_order[g_sel % g_orderCount] == idx;
                const Param& P = g_p[idx];
                MNew();
                MSegAdd(sel ? C_SEL : C_ITEM, "  %s %-34s", sel ? ">" : " ", P.label);
                const char* ch = ChoiceText(idx, P.v);
                if (ch) MSegAdd(!strcmp(ch, "OFF") ? C_OFF : !strcmp(ch, "ON") ? C_ON : (sel ? C_SEL : C_ITEM), "%s", ch);
                else MSegAdd(sel ? C_SEL : C_ITEM, "%.2f", P.v);
                if (P.v != P.def) MSegAdd(C_DIM, "   (default %g)", P.def);
                if (g_bench[idx].valid) {
                    const BenchRes& r = g_bench[idx];
                    double c = r.on - r.off;
                    if (fabs(c) < r.noise) MSegAdd(C_DIM, "   bench: under the noise (%.2f ms)", r.noise);
                    else MSegAdd(c > 1.0 ? C_OFF : C_DIM, "   bench %+.2f ms%s", c, r.bound ? "" : " (or more)");
                }
            }
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
    else MLINE(C_HINT, "keys: I/O select   Shift+I/O group   K/L change   J default   H save all   P hide   Shift+P reload   Shift+U capture   B bench");
        MLINE(C_HINT, "bench: B the selected setting   Ctrl+B every effect%s   Ctrl+Shift+B store a viewpoint (DroneCam's free camera)",
              g_bptN ? "" : " (here: no viewpoints stored)");
        if (g_bptN) MLINE(C_DIM, "      %d bench viewpoint(s) stored: Ctrl+B runs them all (~12 min each, hands off)", g_bptN);
    if (g_view == 24) {
        MLINE(C_HINT, "editor: LMB select / drag   MMB or Shift+K add lamp   Shift+J delete   Shift+L learn found lamps");
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
        MLINE(C_DIM, "%s", l2);
    }
    MLINE(C_DIM, "lamps: %d found, %d in map, %d lit now   templates %d, auto %d added / %d tied   zones %d", g_lampCount, g_mapCount, g_lampUse,
          g_tplCount, g_autoAdded, g_autoTied, g_zoneCount);
    if (g_benchMod.valid) MLINE(C_DIM, "bench: the whole mod costs %+.2f ms of GPU frame (%.2f -> %.2f ms)", g_benchMod.on - g_benchMod.off, g_benchMod.off, g_benchMod.on);
    MLINE(C_DIM, "lamp shadow maps: %u faces drawn, %u objects drawn last frame (of %u)", g_lsFacesMade, g_lsLastDraws, g_lsLastIdx);
    if (g_compileError[0]) MLINE(C_OFF, "HLSL: %.120s", g_compileError);
    else if (!g_sunCount && g_lastReject[0]) MLINE(C_OFF, "skipped: %s", g_lastReject);
}
