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
