# NewLook: what is left (1.81c, 2026-10-24)

Rule: features first, polish later. (check) = done, not yet seen in game.

## Fixes to come back to
- **Black dots in strong reflections** (car tail lights / lamp glass, sun on wet ground): not gone after 1.69 (dark
  floor) and 1.70 (3x3 median). Next idea: no ray-marched reflections on lamp glass and other small bumpy car parts
  (the sky probe only), or a confidence from how far the ray's hit is from its neighbours'.
- Shadows (skipped for now, user 1.71): far buildings' shadows ragged where cascades meet; broken spots on cars;
  phantom sharp shadows in the mountains; buildings' shadows popping when the camera turns; far shadows of moving
  things in steps; the shadow pass's cost.
- Squares on a car's door standing on grass (cause not found; probably grass shadows at a coarse map texel).
- 1.98a ReflectionSharp (full-size detail of reduced reflections): flickers, gains ~0.3 ms at most; default off.
  Needs the per-frame rotation of the sample and full-size history to be steady; low value (the reflections' rays are
  ~1.4 ms at 70 %).
- Procedural grass (1.97, grass.inc), left as it is at 1.97p (user, 2026-10-26):
  - A puddle on paving next to a lawn still shows through the blades. 1.97p stopped lawns and blades reflecting
    anything; not confirmed to be enough. Next ideas: cut the half-size reflection's spread at lawn pixels, or let no
    puddle within ~0.5 m of the lawn map's edge.
  - The car shows through the blades. 1.97m's lawn stencil did not fix it; cause unknown. Ask for screenshots with
    Grass 0 / 1.
  - Shadows on the blades may still jump in soft squares (the sun shadow's half-size smoothing?) if 1.97p's map
    history did not cure it.
  - Cost: ~3.3 ms at range 100 / density 5. Stage 4 optimisation still to do.
  - Optional: a pressed trail left behind the car.
- Lamps: lights not always going out when a post is knocked down; lamp shadow faces redrawn one by one; night cost.
- Parallax: a zigzag seam along some slabs; more wall shaders to rewrite.
- Some ripple left in the AO.

## Just done (check)
- 1.74 .. 1.81c: rain drops on the player's car (simulated on its baked shape, merging, running; on its glass too;
  RAIN ON THE CAR controls). 1.81c: flicker at seams, the read-back's flushes; the log line has CPU / long frames.
- 1.72: the player's car in puddles as a flat underside (no sky under it), not a box; traffic left as it was.
- 1.72: puddles' rims: a band of soaked asphalt round them.
- 1.72: the storm preset denser (cover 0.95, cloud shadows 1.0); re-pick "storm" in SKY to get it.

## Settled by the user (1.71)
- The three AO options (cosine sky occlusion, multi-bounce, bitmask) stay as options.
- Trails on traffic: not seen any more. Puddles / walls wet above the camera: gone. The sky (far clouds, cloud
  shadows, the sun in a storm): fine.
- FXAA: too heavy for the user; left off. The car paint layers (1.70): did not take on the body; taken out.

## New features (the list of 2026-10-15, with the user's answers)
1. Anti-aliasing (TAA): at the very end (FXAA tried in 1.68: too heavy).
2. Time of day / day speed from the mod's menu: with the weather, later. (Our sun / moon by the clock are done; the
   game's own time fields: MODLOG "Rollback to 1.54".)
3. Draw distance / LOD (cinematic mode, WorldDistance patches): part of the optimisation.
4. Depth of field / bokeh: not wanted.
5. Proper motion blur (per-object velocity): after the optimisation.
6. Shadows past 270 m from the city's height map: done (1.54), more in the shadows rework.
7. Volumetric light in tunnels / under lamps (light in the air with shadows, not a halo): to look at (the lamps'
   glow in the air exists, without shadows).
8. Lens flares / dirt on the "lens": the game has its own; a dynamic one undecided.
**Weather** (the next big block; plan of 2026-10-24, after the car's drops 1.74 .. 1.81c):
1. (1.82, check) The weather's state (weather.inc): rain 0..1 (by hand, or by itself: clear -> clouds -> rain -> clearing, minutes
   per phase); from it, smoothly: the sky (cover, rain clouds, cloud shadows: overcast while it rains), the wet ground
   soaking in a minute or two and drying over ~10 min, the puddles filling and draining slower, the car's drops (rain
   on / off by it), the haze thicker in rain. Its settings move in the menu as it goes; off: all as set by hand.
2. (1.83, check) Rain falling: streaks round the camera (instanced, stretched by their fall and the camera's motion, lit by the sky,
   the sun and the lamps), none under roofs / bridges / in tunnels (the rain map), thinning with distance into a haze.
3. (1.83, check: the ground; not on cars yet) Splashes on the ground and the cars (short bursts where rain hits, by the rain map); ripples in puddles.
4. Fog (morning / heavy rain): denser low air with the haze, its own setting.
5. The wind in the trees' leaves (asked 2026-10-24): the game's trees have no wind of their own (no such constants).
   1.86: the tool: Shift+X on a tree logs "wind pick" (its VS's outputs, a pass-through GS built for them and checked)
   and "wind census" (the frame's cut-out (VS, PS) pairs). Next, with the user's log: pick the leaves' VS / PS, insert
   that GS for their draws (GSSetShader round the game's draw), displace each vertex in world space (from SV_Position
   back through the camera record, forward again): gusts from weather.inc's wind, leaf flutter by a world-place hash;
   trunks (not cut-out) stay.
   Findings (Shift+X x3, 2026-10-24): the leaves = VS with 3 outputs (SV_Position o0, TEXCOORD0 o1.xyz, TEXCOORD1
   o2.xyz) + PS "DiffuseTextureSampler | diffuseTintCol", cut-out, 137-193 draws a frame, DrawIndexed (not instanced),
   its world matrix in cb1 at 0 (gObjectCB): the tree's own base is known per draw. The pass-through GS for it compiles
   and matches. (Pointers change per session: know it by the PS's names + cut-out + the VS's 3-output signature or its
   DXBC checksum.) Other cut-outs: "materialDiffus" 4-10 draws (grass / bushes?), 8-output families (fences, signs?).
   Plan 1.87: (1) know the leaves' pair at shader creation; (2) at their draws set our GS (built once from the VS's
   signature), bind cb1 (its world: the base) and our LightCB to the GS; (3) the GS: world from SV_Position through the
   camera record (mind its sign: RfClip), sway = wind x (height over the base / 8 m)^2 + flutter (hash of the place,
   gusts in time), back to clip (depth = t1.w + s); (4) WindLeaves / WindFlutter settings; (5) a harness test with a
   fake tree; (6) also the "materialDiffus" family if the user confirms it is grass.
6. (1.89, check) Weather kinds (presets) + their walk by the hour + WeatherChange; less sun through a thick deck.
   Next (asked 2026-10-25): its own menu tab: the kinds' values editable and saved (an ini section per kind, as the
   lamp presets), a "next weather" key, the walk's pace tied to the game's clock (a day's length), the kinds' own
   cloud looks (type, height, speed, rain bellies by kind, not only by cloudiness), more rain variants (showers that
   come and go under cumulus, light rain without full cover), dawn / dusk variants (golden clear, pink haze, grey
   mist), fog proper (stage 4) for the fog kinds, lightning for the storm.

## Waiting for a decision
- The city's height map from the game's own collision files (far buildings always cast).
- Cascade 1 drawn every frame (moving things' shadows steady, costs more).

## World editing
- Collision for new objects (Blender: TRK_COL, PolygonSoupList).
- Not yet tried in game: copies (Ctrl+D) and worldtools/bake.bat.

## Optimisation (1.96 started it; the priority now)
- 1.98b plan (user, 2026-10-26; target -40..50 % of NewLook: day ~16 -> 8-10 ms, night ~25 -> 12-15 ms at 5120x1440):
  1. (1.98b, check) the pass profile: Ctrl+Shift+B, by day and at night; the user sends scripts\NewLook_profile.txt.
  2. The lighting split: the smooth terms at half size, a lean full-size pass.
  3. The lamps at night: their diffuse at half size, shadows for the main lamps of a tile only.
  4. The sky and clouds spread over frames (1.98b: cheaper at night, check the look at dusk).
  5. What several passes compute (wetness, puddles, the lawns) once a frame.
  Also: does the game's own sun light draw (PR_GAMESUN) still need to run under ours?
  1.98c profile (2560x1440, reliable): day 7.2 ms = reflections 1.85, relief 1.12, bounce 0.75, sky occl. 0.45, sky 0.4
  (+0.3 untimed), grading 0.38, the rest base; night 11.4 = lamps 5.2 (full size: LampResolution 0), reflections 1.8,
  our sky 1.2 untimed (unknown: 1.98d profile), bounce 0.8, relief 0.6. The game's sun light is 0.2: not worth it.
  Next by size: the lamps (half size if the user agrees, cheaper screen shadows), the night sky's 1.2, reflections,
  relief's sky part, the half-res pass (sky occlusion / bounce: fewer samples a frame with the history), grading.
  1.98e: lamps at half size split (the strongest lamp of a pixel at full size). If the user still sees a loss: the
  2nd strongest at full size too; or the half pass's other screen shadows fewer. If it holds: make it the default and
  measure the fallback's share (thin things: trees, fences).
  1.98e profile: the split holds (user: "hardly drops"). Next by size, night 10.2 / day 6.5 ms: the sun pass (relief
  1.44 at night, bounce fetch 0.81 (?), the split 1.38), reflections 1.7, our sky shader in the game's draw (+1.0 at
  night, +0.8 by day; stars 0.3), grading 0.4, tiles 0.5 (1.98f). Before a release: remove PerfTest (266) and SkyTest (260).
- AMD (a Nexus player (AMD), RX 9060 XT, 1080p, the game's AA off: 120 -> ~15 FPS): cause not known. Wanted: his
  scripts\NewLook.log (the "timing:", "cpu:", "gpu:" lines) and NewLook_bench_report.txt after B. 1.96 took the
  arrays indexed at run time out of LampShade / PS_Tiles / LampGlow (AMD: scratch memory); next suspects: PS_Sun's
  size (9150 slots, ~85 temps: variants per feature set instead of one uber-shader), the leaves' GS, PS_Half (3464).
- PS_Half and its blur skipped when AO, sky occlusion, contact shadows, shafts and bounce are all 0 (PS_Sun's
  P5.w = 0 makes it march AO / contact itself at full size: needs a "nothing" mode there first).
- The overlay (the menu) costs ~1.4 ms of GPU while shown (1.95 log): draw its text once into a texture.
- Quick bench (1.96b: Ctrl+B, by the game's own clock): run it again; check that the day scene really is the game's
  day (the lamps off), the costs against the 1.96 report (inflated), the time it takes. Why the references stayed
  up for several windows after the street lamps came back (their shadow maps refilled? LsPickFace skips slots unused
  > 500 ms). Ultra tried only with > 700 MB address space free: is that the right line.
- Over 60 FPS (MW2012 FPS Unlock, Krashgamer, MIT: tried next to ours, not in our code): eases stepped per frame, not
  by time, run faster: proplink.inc:272 (a knocked lamp's fade, 0.25), sky.inc:404/426 (the sun through our clouds,
  0.15), skyrefl.inc:55 (the reflections' sky tint, 0.05): 1 - (1 - a)^(dt * 60).
- Sun path (1.96c): the dawn turn's mark is missed after each night (the turns looked for by day only); anchor the
  line at the game's sun coming on (aOn known) instead? (the switches are learnt from our phase: not both at once).
- The log's vectored handler logs PropScan's handled faults (proplink.inc, the game freeing memory as it is read):
  leave those out (an address inside PropScan) so real faults stand out.
- Bench 1.96c (RTX 3080, 2560x1440, day): the game 4.6 ms, NewLook 12.9 ms. SKY 2.1-3.6, REFL 2.1, LAMPS 1.8 (night),
  WET 0.9, MAT 0.8. SUN & MOON off and SHADOWS off cost MORE (-1.0, -0.8): what runs instead (the game's light
  back in our pass? its cascades?). COLOUR ~0.4 ms for a grade at 1440p: its copy + full pass, look.
- Optimisation order (1.96d bench): street lamps at night 6.1 ms (30% of NewLook there), sky & clouds 3.4 at night,
  reflections 1.7, wet 1.4, AO 1.1. Negative costs (a section off makes it slower): SUN & MOON -2.2, SHADOWS -1.3,
  AIR -1.2: find what takes over. Ultra = High: which Ultra settings do nothing. Windows 1.4 s: day noise 0.9 ms.
- Sky (1.96f): read the overlay's clouds / cloud shadows / sky at night, SkyFast 1 vs 0. Next if still dear: the
  "up" ambient sample every other step, the march every other frame per 2x2 (history), the probe less often.
- The moon (1.96b): drawn only while the game's moon quad is in front of the camera (its corners give the place):
  our moon in view while the game's is behind is not drawn.
- The menu (1.96: simple / advanced, sections, quality): not yet seen in game.

## Later
- PS_SunMask: does it read every cascade per tap (0.9 ms; needs a harness).
- Modern lighting with real roughness data (with the materials work).
- 1.98h: traffic: with the frame map, off / on (by day and at night) to see which part of the game's frame grows; at night our lamps (car headlights among them?). Our sun-shadow casters' CPU (SsAfterDraw 1.7-2.4 ms a frame) if the frame ever gets CPU-bound. The sky's other views: what they are (the log's "sky: the game's views").
- 1.98k: the far mountain stays hazy / bright under a storm sky (find its draw; our haze colour?). Storm clouds: sharp contour steps, worse with SkyFast 2.

## Our own eye adaptation (replace the game's; planned 2026-10-26, after 1.98)
The game's meters the whole frame's mean and pulls it to one mid-grey: a storm is brought back to a clear day's
brightness; looking at dark wet ground brightens everything, the sky in view darkens it. EyeAdapt (< 1) only damps its
answer and reads its value on the CPU (2 frames late; odd reads up to 7x off, 1.98ab's median hides them).
- Exposure from the light, not the frame: sun + sky illuminance we know (time of day, SunDeck, ovUp), as a camera's
  "sunny 16": a storm stays darker than a clear day.
- The frame's metering only as a small correction (about +/- 1 EV): centre-weighted, sky and extremes left out, the
  histogram's middle (as UE's); looking down or turning barely changes it. Night: more weight on the metering (lamps).
- Eye-like speeds: slower to dark, faster to light; no feedback (the game's own value never used).
- All on the GPU: a small pass at half res writes the 1x1 texture the game's tone mapping reads (no CPU read-back),
  the game's own value put back after (as 1.98aa's restore).
- Settings: mode (the game's / ours), how much darker a storm, the metering's range, the adaptation speed. 1.98w's
  storm darkening becomes part of it.
- First check: who else reads the 1x1 (bloom? the HUD? the odd 11.5 reads), the game's light units by time of day.
- About 2 versions: diagnostics + the exposure behind a switch; then tuning by day / storm / night shots.
