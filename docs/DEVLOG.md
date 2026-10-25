# NFSLight — mod log

Game: Need for Speed: Most Wanted (2012), Steam 1262560, NFS13.exe 1.5.0.0 (x86, LAA), Criterion Chameleon.
`um scan`: no anti-cheat, ASI loader present (HD Effects' dinput8.dll), route = proxy DLL / native hooks.
Loader: Ultimate ASI Loader (dinput8.dll from NFSMW2012 HD Effects). Our plugin: `scripts\NFSLight.asi` + `.hlsl` + `.ini`.

## Renderer facts (verified)
- D3D11 API, device at feature level **10.x** → shaders must be SM 4.0 (ps_5_0 creation fails with E_INVALIDARG 80070057);
  texture size limit 8192 → shadow atlas N×3N caps N at 2730.
- The game creates 4 devices at start (3 probes); the 4th `D3D11CreateDeviceAndSwapChain` is the real one.
- The immediate context's vtable lives inside the object (`ctx+4`); draws do NOT go through it →
  hook Draw/DrawIndexed/DrawInstanced/DrawIndexedInstanced **in code** (prologue `8B FF 55 8B EC`), PSSetShader via vtable works.
- Sun pass draws with DrawIndexed, once per frame on the main view (RT 5120x2880 at SSAA 1x2, R32G32B32A32 typeless),
  more draws at small sizes (reflections) — skipped by size.
- Sun-pass shaders: 24 in SHADERS1.BNDL (21 usable; 3 single-cascade reflection variants, 1 thermal, 1 grey debug).
  Identified by reflection names; texture and sampler share names (match by type).
- View ray: rel = TEXCOORD1.xyz / (depth - TEXCOORD1.w); depth - w is NEGATIVE in game (projection must keep sign).
- Shadow map: R24G8 (view R24_UNORM_X8), comparison LESS, linear filter; cascades stacked vertically,
  ShadowMap_ConstantsArray[4].zw = texel size, ScaleOffset[0..2] = cascade B/C transforms.
- Lighting replica (sun term) GPU-verified exact against the game's own shaders on 18 variants (ltest/gpu.cpp).

## Gotchas hit
1. Comment swallowed `cb.L2[...]` assignments → flags/view always 0 → only AmbientScale worked. Harness built its own cb, missed it.
2. Shadow blur in atlas texels → same blur everywhere (cascade texel 0.7–4 cm) → use metres.
3. Blocker search in the coarse cascade → small objects lost shadows; now fine cascade + coarse only for pancaked (depth ≈ 0) samples.
4. Screen-space contact shadows ghost behind the car (no data behind it on screen) → off by default.
5. ini ShowOverlay=0 persisted by H after P → overlay "missing".
6. d3dcompiler_47 internal error X8000 ("relative index temp register ... uninitialized") on the SV_Depth variant once
   the half-res branch was added; fixed by compiling that branch out of the depth variant (#ifndef WRITE_DEPTH).
7. Frame trace: the first ~600 draws of a frame are depth-only shadow-map draws (no RTV) → skip them in traces.
8. Haze in the sun pass misses transparents (car glass) drawn later → needs a pass right before post-processing.
   Done in 0.18: the air pass runs when the first MotionBlurPixe / HDRToneMapping pixel shader is bound,
   replaying the sun pass's saved VS/IA/draw. User confirmed: haze now covers car glass.
9. 0.18 had two full-frame copies of a 5120x2880 RGBA32F target (236 MB each, ~470 MB of traffic per copy).
   0.19 removes both: the sun pass writes `result = dest*K + S` with dual-source blending (SRC1_COLOR), the air pass
   `dest*hazeT + inscatter` with SRC_ALPHA. Harness: blended output == copy-path output.
   Copy kept only for views 6/8/9/13 (they show the game's own image) and captures.
10. 2026-10-12 crash on 0.18: AV at NFS13.exe+151c29 (reads [null+0x28], an object table at 0x10EDB68 indexed by a
    short), 0.4 s after toggling the mod off. Game-side code, not D3D. Cause unknown; 0.19 logs the thread
    (render or not), stack return addresses and free address space to tell next time.

11. Street lamps are mostly baked: shader names show `LightmappedLights{Red,Green,Blue,Alpha}ChannelColour` and
    `LightmapLightsTextureSampler`; dynamic lights are `deferred_lightPositions/Colours` + `lightAccumulationBufferSampler`,
    headlights `gPlayerHeadlight`. So "bounce from every light" = bounce of the finished image, not of a light list.

## Bounce light (0.20)
- Source: the previous frame's finished scene (snapshot at half res in the air pass, before the haze; mips give its
  average for a soft limit on glints), found by reprojection. Fallback per sample: sun-only radiance computed now.
- Reprojection: PS_Cam records the affine view ray t1 = A + B*u + C*v (4x2 target) + camera position each frame;
  `Reproject` solves A + Bu + Cv = s*rel. Harness: a point reprojected with its own frame's record lands on its own
  pixel (error 0). Distance check (3% + 0.15 m) rejects disocclusions.
- History: blurred bounce of the last frame (ping-pong) blended in with weight BounceSmoothing (0.75), sample pattern
  shifted every frame. Reflectance capped at 0.8 (`min(albedo*strength, 0.8)`) so the frame-to-frame feedback settles.
- Measured on 0.19 (user's PC, 5120x2880): mod 3.5-8 ms GPU; half-res 1.5-4 ms (BounceRadius 8 is the costly part),
  sun 1-2.6 ms, air 0.8-1.7 ms. With the mod off: game sun pass ~0.7 ms, frame ~17-22 ms.

## 0.21: no feedback cap, lamps
- 0.20's cap `min(albedo*strength, 0.8)` made BounceLight 4 ~3x weaker on bright paint (user: "the effect is gone").
  0.21 takes our own bounce/lamp light out of the snapshot instead (PS_Snap subtracts albedo*(1-spec)*GI, G-buffer
  diffuse/spec SRVs kept from the sun pass), so no feedback and no cap.
- The additive G-buffer (AdditiveSampler) holds environment reflections, not emissive: lamps cannot be read from it.
- Lamp finder: PS_Emit, one pixel per 32x32 tile of the snapshot: pixels brighter than LampThreshold x average
  (top mip) within 300 m -> centroid (world, via this frame's camera record + depth) and intensity = radiance x area.
  Staging read-back ring of 4, Map DO_NOT_WAIT >= 2 frames later. CPU clusters (1.5 m), matches to a world list
  (2 m, EMA), drops lamps not re-found 4 times while on screen, or unseen 8 s. Used after 3 sightings, 32 best
  (intensity / distance^2) go to cbuffer b6; PS_Half adds them (wrapped Lambert, smooth range) to the bounce target.
- Harness 'e': a 4x4 bright spot -> exactly one light at the spot's world position.
- Views: 21 bounce + lamps x4, 22 bounce source (green = last frame, red = sun-only fallback), 23 lamps alone x4.

## 0.22: lamp map
- User: found lamps "float" (screen-space detection). Fix: a persistent world lamp map `scripts\NFSLight_lamps.txt`
  (x y z r g b per line). LampSource 0 found / 1 map + found (found within 2.5 m of a map lamp skipped) / 2 map only.
- Editing: Shift+K add at the screen centre (snaps to a found lamp within 3 m, else ManualLampPower warm white 30 cm
  off the surface), Shift+J delete the map lamp within 4 m of the aim, Shift+L learn found lamps seen >= 6 times
  (merge within 2 m). LampAutoLearn adds lamps seen >= 60 times (no merging, so map lamps do not drift).
- Aim point = second row of the camera record (PS_Cam reads depth at uv 0.5).
- Map lamps fade by day: key light luminance ~6 by day here (cb0[2] = 7.7 5.8 3.5); fade from 0.5 to 2.5, level LampMapByDay.
- View 24 = editor: map lamps green dots, found yellow, red cross at the centre (copy path).

## 0.23: editor targets + free camera (DroneCam)
- Targets picked by direction from the screen centre (within 3 deg, < 250 m), not by distance to the surface under
  the cross (lamps hang in the air, the cross hits the wall behind). Red ring = map lamp Shift+J deletes, white ring =
  found lamp Shift+K adds; LampCB.tgt[2]. Rings drawn through walls.
- Free camera lives in DroneCam (it owns the camera matrix, Cam_Post): FreeCamKey = VK_OEM_7 ('), WASD, R/F or
  PgUp/PgDn, arrows / mouse (cursor re-centred), Shift x4, Ctrl x0.25; car controls suspended (crash-scene switch).
  Yaw sign follows the game's handedness (det of R,U,F).

## 0.25
- Gotcha 12: the late pass (air / editor, at the first post-processing shader) replayed with the game's constant
  buffer *objects* saved at the sun pass; the game maps new contents into them later in the frame, so the camera /
  light seen there could be another pass's. Symptom: editor markers not where the lamps are. Fix: CopyResource the
  VS cb0-3 and PS cb0-1 into our own DEFAULT buffers at the sun pass (KeepCopy) and bind those late.
- Editor pointer = screen centre (free camera turns with the mouse all the time); while an arrow is held the mouse
  moves a pointer of its own and DroneCam stops turning (export `NFSLight_MouseCaptured`).
- DroneCam: in free camera the game's idle scene (~60 s without input, `gameplay camera hidden`) is overridden by
  setting the active bit obj[-0x4C] |= 8 (FreeCamKeepActive=1). Unverified whether the renderer follows it.

## 0.27
- Gotcha 13 (d3dcompiler_47 bug): in PS_Editor `G[L1.w]` (camera) inside a [loop] compiled to `cb0[r0.w]` with r0.w
  still holding another value (dist + 0.5); `mov r0.w, cb5[1].w` came only later. Camera read as 0 -> every lamp
  dot clustered far off the map. Found with harness test 'd' (lamp at a known point -> known pixel).
  Fix: take the camera from this frame's camera record texture (t28). Checked the disassembly of the other shaders:
  their cb0 indices are loaded before use. Lesson: when a shader result looks "camera at origin", read the asm.
- Editor targets now picked in screen space like the click (24 px).

## 0.29: lamp shading
- User: next to a lamp the car looks like plasticine (no gloss). Causes: lamp light was diffuse only, half res
  (normals lost in the depth-only upsample), with 15% wrap. Now LampShade in the full-res sun pass with the game's
  own BRDF (diffuse x (1-spec) / pi + normalised Blinn-Phong with Fresnel, gloss from N.w), no wrap, 1/(r^2+0.25).
- The game's V = normalize(t1.xyz) points to the camera (t1 = (depth - w) * rel, depth - w < 0).
- PS_Snap takes the lamps' diffuse light out of the snapshot (no feedback into bounce / the lamp finder).
- Groups (0.28): NFSLight_lamp_presets.txt (name hue sat power), map lamp's 7th number = group (-1 custom).

## 0.30: lamps follow their posts (image based)
- Probe pass (PS_Probe, 64x1, b7 list): for up to 64 on-screen map lamps within 150 m, brightest snapshot texel in a
  5x5 around the projected lamp, visibility (scene distance there >= lamp distance - 1 m), frame average, day factor.
  CPU (read back like the lamp finder): at night and visible: base = brightest seen; dark (< 30 % of base and below
  LampThreshold x average) for 6 checks -> out (fades over a few frames); glowing again -> back. Not saved.
- Works for lamps sitting on a bulb (found ones, Shift+K on a ring). Hand-placed lamps with no glow never go out.
- Alternative not done: the game's breakable-prop state in memory (needs RE with the user knocking a post down).

## 0.31: object research tools (image-based lamp breaking dropped at the user's request; 0.30 reverted)
- Game names found in the exe: PropInstanceList / PropObject resource types, ResetProps, "prop impact".
- objtools.inc: Shift+O in the lamp editor, then LMB on an object:
  1 pick = every main-view geometry draw before the lighting (RT of screen size + DSV, g_frameSun == 0) replayed into
    the pixel under the cross (scissor clone of its RS state, depth EQUAL, no writes) inside an occlusion query;
    the last one with samples is the visible object. Logs it and how widely each VS cb pointer is shared.
  2 next frame: draws of the same mesh (kind/counts/IB/offset/VS) copy VS cb0-7 (+ instance VB slice) to staging,
    query again; nearest float triplet (rows or 4x4 columns, camera position excluded) = object position.
  3 worker thread scans committed memory for that triplet (tol 0.01), 4 watches 1 KB around 48 hits for 3 min
    (first change of each dword, bursts, summary); once a second a check frame logs the mesh draws' positions.
- Synthetic test (ltest/objtest.cpp): 3 quads, near one wins, position found in cb1 +48, scan finds it. Gotcha: the
  default rasterizer culls the test quads (CULL_BACK) -> nothing drawn; the test needs CULL_NONE.
- Lamp map lines may carry an object position (columns 8-10); the editor draws a cyan line lamp -> object.
- Checklist for the user: OBJECT_RESEARCH.md.

## 0.32: lamps tied to breakable props (from the research log, kept as _NFSLight/research_objects_2026-10-12.log)
- Render side: lamp posts are one mesh (DrawIndexed 1731 indices) drawn per instance; the world matrix is in VS cb1
  (rows, translation at +48). Pointers of VS/IB change between sessions; positions are stable.
- Memory (two posts, same layout, both in one heap block 0x2B9Bxxxx = the prop instance pool):
  A-48/-32/-16 axes (rows, w=1), A+0 position, A+12 = 1, A+16..+44 velocity/spin (0 standing, e.g. -10.7 -2.3 9.9
  when hit; +28 = 447.04 and +44 = 1490.13 appear on hit, same for both), A+140 flags 0x01010400 standing ->
  0x01010C00 hit -> 0x00000C00 later, A+156 radius 2.51 (grows when moving), A+172 0x00FF0003 -> 0x00FF0103.
  Never changing copies (0x0E43..., 0x3E6E5D.., 0x40A1AD..) = placement data; 0x2BF0/0x2BF1.. = a render list
  re-sorted every frame (not state).
- proplink.inc: worker thread finds the standing instance by exact position + that pattern (known blocks first, full
  private memory at most every 15 s); every frame PropAlive/PropHit; KnockedLamp 0 = flicker + out, 1 = the light
  falls with the post (lamp kept in the post's local axes). GONE until found standing again.
- proptest.cpp: fake instance -> LIT, knocked -> out / falls (position follows the tipped axes), gone, standing at a
  new address -> found again in the learnt block.
- Object tools: tying a lamp skips the research scan; up to 128 draws of the mesh compared (32 was too few).

## 0.33: lamps on every post (templates)
- Shift+I on a lamp tied to a post (picked this session): template = mesh (index count, start, base vertex, stride),
  VS cb slot / offset of its matrix (cb1 +48, rows before it), all lamps tied to that post in its local axes.
  NFSLight_lamp_templates.txt.
- Every 8th frame the draws of template meshes copy that cb (max 96), read back >= 3 frames later (DO_NOT_WAIT):
  every upright post (row 1 y >= 0.995) with no lamps tied gets the template's lamps, tied; an untied hand lamp within
  2 m of a bulb is tied instead of adding one. autotest.cpp: rotated post, repeat, lying post, hand lamp - all right.
- Gotcha: `near` is a Windows macro (windef.h) - not a usable variable name.

## 0.34-0.35
- 0.34: templates take the post's axes from the pick-time constants (g_obM) - a second kind of post was never "found
  standing" because the memory pattern used the exact flags of the first kind; standing = bit 0x800 clear + 0x400 set.
- 0.35 light zones (NFSLight_zones.txt: centre, half sizes, yaw deg): light from outside a zone does not get in,
  from inside not out. 16 nearest zones in LampCB; per lamp a bit mask of the zones it is in (LampCol.w, CPU);
  per pixel ZoneIn (1 m soft edge), factor = prod(1 - |pixel - lamp|). Shift+B adds one at the aim, LMB on its
  centre square selects, arrows move, I/O/K/L size and turn, Del deletes.
- Gotcha 13 again: in PS_Sun the far-air branch read G[L1.w].y with an index register holding V.x. Even with camPos
  read at the top, the compiler re-read it lazily there. Fix: read it in a 1-iteration [loop] at the top.
  tools/idxcheck.py now checks every dynamic cb0 read in the disassembly (the index must come from cb5 or iadd).

## 0.36-0.38
- 0.36 zone handles (side squares stretch one side, ring turns, arrows move), soft edge per zone; new zones at the aim
  only if it is nearer than 60 m (the cross on the sky put them far away).
- 0.37 zone channels: zone channel 0 = all lamps, N = only lamps with channel N; per lamp an "acts on it" mask in
  LampPos.w (bit 0 = map lamp, bits 1.. = zones). Lamp file: 11 numbers when a channel or object is set.
- 0.38 menu (menu.inc): coloured GDI pieces per line, groups as an accordion (only the selected one open), choices shown
  as words / ON-OFF, U = view list (I/O choose, view changes at once), blocks: title, groups, selected lamp/zone,
  keys, load in grey. menutest.cpp renders the menu to PNG offline. New lamps take the colour/group of the last lamp
  edited by hand (ini key NewLamp).

## 0.39
- Auto lamps stayed "searching": the strict standing pattern (flags of the first post kind) rejected other kinds, and
  the request queue (64) overflowed. Now pools are learnt by AllocationBase (one heap ~11 MB, not 11 sub-regions);
  inside a pool a "loose" match = exact position + w 1 + zero velocity + orthonormal rows (render copies / placement
  data have pointers at +16); loose props are hit when moved > 0.3 m or tipped (row 1 y < 0.97). Queue 256, dedup.
- WaitQuery waited 2000 tries, not time: a busy GPU made picks fail ("nothing under the cross"). Now up to 250 ms.
- Undo/redo (Ctrl+Z / Ctrl+Y, 24 snapshots of lamps + zones, K/L repeats coalesced), Ctrl+C / Ctrl+V lamp settings
  (Ctrl+Shift+V: every lamp within 30 m). LampReach max 300 m.
- Gotcha again: a comment added by sed swallowed the rest of a line (`... // note g_map[i].propNext = 0; }`).

## 0.40 (2026-10-13)
- Templates made a new lamp for a knocked post: it slides away upright for a moment, and the auto placer saw a
  "new standing post without lamps" (it only matched an existing tie within 5 cm). Now a post gets lamps only if
  no tied post is within 2.5 m (horizontal, 3 m vertical) and it was seen at bit-identical position twice >= 1 s
  apart. autotest covers sliding, at rest 2 m away, moved between sightings.
- User reset: lamps and templates emptied (backups `NFSLight_lamps_before_reset_*.txt.bak`,
  `NFSLight_lamp_templates_before_reset_*.txt.bak`), templates being remade.

## 0.41 (2026-10-13): 256 lamps, screen tiles
- LampReach is now the draw distance (20..1000 m, default 200); new LampRadius (param 35, 5..150, default 40) is the
  light sphere of a lamp (shader P8.z); P7.z = reach + radius is the pixel distance limit (was a fixed 400 m).
- Lamps moved from LampCB (32) to a dynamic Buffer<float4> at t33 (256 lamps, [2k] pos, [2k+1] colour). CPU sends
  the 256 best by Lum/d^2 among those whose light sphere can be in view (cone from the camera record + 20 deg margin).
- PS_Tiles: one pixel per ~32x32 screen tile, two R32G32B32A32_UINT targets = 256-bit lamp mask (t31, t32), made in
  the sun pass right after PS_Cam with this frame's camera (t28 = current record) and rays; sphere vs the 4 side planes
  of the tile pyramid. Forward = t1 x sign(depth - w) (sky: -1), so it works with either sign convention.
  LampShade walks the set bits (lowest bit via asuint((float)b) >> 23, SM4 has no firstbitlow).
- gpu.cpp modes t / T (positive / game sign): 0 pixel-lamp pairs missing from their tile, shading with tiles equal to
  all-lamps-everywhere to the bit. Late pass saves / restores SRVs 0..33 now.

## 0.42 (2026-10-13): screen-space lamp shadows
- LampVis: 12-step march (quadratic spacing, IGN-jittered) from the pixel to the lamp through the depth buffer; occluded
  when a sample is 0.04..thickness behind the surface, or steps from behind (within thickness + half the step) to in
  front. Only the 4 strongest lamps of a pixel (by luminance of their contribution) get it, pixels < 120 m.
  Params 36 LampShadows (strength), 37 LampShadowThickness (m); cb P9. PS_Snap subtracts unshadowed lamp light.
- gpu.cpp mode s (wall, box, lamp): 82% of the expected shadow dark, misses only at its outer 1-2 px, 0 false.
  Limits: off-screen casters cast nothing; thin objects in front of far walls can give a halo (thickness).

## 0.43-0.44 (2026-10-13): night ambient, shadow research, lamp shadow maps
- NightAmbient (param 38, P9.z): K *= lerp(NightAmbient, 1, day) (day from the key light as for map lamps).
- Research (Shift+T, shadowrecon.inc; CreateVertexShader hooked at device vtable 12 and reflected): the sun shadow
  map is one atlas 2730x8190 (HD Effects ShadowRes 2730), D24 (f44), 3 cascades as viewports at y 0 / 2730 / 5460,
  depth only, every frame. Widest cascade: ~500-1500 draws, no car shaders; near ones 23-321 (some cars). The VS view is
  ViewProjectionModified (gGlobalCB, offset 0/16/32/48/112/128 by variant): rows x, y, depth (world, column vector),
  row 3 = (z scale, z bias, w scale, w bias): sun cascades (0.001, 0, 0, 1) = ortho; main camera (1.0005, -0.2, 1, 0)
  = perspective, near 0.2. world (gObjectCB) is row-vector, translation in row 3. Other targets: 2048^2 depth-only
  (623 draws, a 90-degree view straight along +y from the camera), 1024^2 cube-ish envmaps, G-buffer 5120x1440 (= the back buffer: the user has a 32:9 monitor, NOT SSAA).
- Lamp shadow maps (lampshadow.inc): 32 slots x 5 faces (+x -x +z -z -y) of 256^2 D32 in one array (t34). In the widest
  cascade every draw with a known non-car VS is replayed once more into this frame's face (one face a frame, never-
  drawn first, then oldest) with our constants (zeros + the face view at the VS's offset), own CULL_NONE rasterizer,
  viewport, then the game's cb / DSV / viewport / RS restored. Faces 1.08 x 90 degrees, near 0.1, far = light radius.
  Slots go to the strongest map lamps sent (score order), kept per lamp, redrawn when it moves.
- Shader: LsFace (same axes table), linear depth d = n*a/(a - z); soft (4 strongest lamps): 8-tap blocker search,
  penumbra = LampSize * (dr - db) / db, 12-tap filter; the other lamps 4 taps. Lamp buffer is 3 entries a lamp now.
- gpu.cpp mode m: game-style VS + imitated widest cascade through the real LsAfterDraw; a box not in the depth buffer
  shades the wall: 100% of the inside dark, 0 outside, soft edge.

## 0.45-0.49 (2026-10-13): optimisation with the bench
- Bench (bench.inc): B = selected setting, Ctrl+B = list; on/off/on/off, 30 settle + 90 measured frames each, raw
  GPU frame / passes / CPU frame; results next to the settings and as a sorted table in the log (bench:).
- Night 1 (0.47, 5120x1440; correction 0.61: a 5120x1440 32:9 screen, not SSAA): whole mod +16 ms GPU frame (10 -> 26). Lamp light +7.4, lamp maps +6.1
  (the replays: objects a frame 50 -> 250 = +3.5 ms, ~14 us a draw), light radius 5 -> 40 m +5.0, contact +1.1.
- 0.48: SoftSunShadows switch (P10.x); lamp map faces drawn once (redrawn only if the lamp moves or the camera gets
  2x closer than when drawn: the cascade's casters are around the camera); zones skipped for lamps and points
  outside every zone; budget default 100. Night 2: whole mod +9.2 ms; soft sun +0.85, maps +0.86, lamp light +3.2
  (sun pass +3.5), radius +3.9, contact +1.35, bounce +1.3.
- Gotcha 14: HLSL ?: evaluates BOTH sides. `useHalf ? halfContact : ScreenContact(...)` ran the full-resolution
  march every pixel anyway (contact +1.25 ms in the sun pass, AO too). Real [branch] ifs now.
- Gotcha 15: snap is nested in the air pass's timer: "mod passes" counted it twice. Fixed (air = with snap).
- 0.49: PS_Lamps: lamp light alone into an RGBA16F target at the shown size (backbuffer, or half with LampResolution)
  right after the half-res passes (t28 = this frame's camera); the sun pass and the snapshot read it at t35 (P10.y).
  Harness: identical to the in-pass result (max difference 0).

## 0.50-0.53 (2026-10-13): the look, sky occlusion
- 0.50: lamp light texture read back depth-aware (4 texels, weight by surface distance in its alpha): half size
  then differs from full by 0.00006 mean on a plain material (gpu m: random albedo cannot be compared between sizes).
- Post chain (exe shaders): exposure = 1x1 HDRToneMapping texture (w), downsample to RGBM /128; final pass =
  SamplerSource (+ distort, DOF) + SamplerSplatter (screen dirt) -> 3D colour cube (ColourCubeScalesOffsets); then HUD.
  No real tone curve: clip at 1 then the LUT.
- 0.51-0.52 grade.inc: after the final pass (found by "SamplerSplatter" in its resources) the target is copied (with
  mips) and PS_Grade (b12, VS_Overlay full screen) draws over it. Exposure, contrast around mid grey, shadows /
  highlights, soft shoulder, clarity (vs a blurred mip), dehaze (veil = share of the average, max 60% of a pixel),
  WB, saturation / vibrance, split toning, CAS-like sharpen, vignette, bloom (mist: lerp to blurred mips + glow of
  the bright ones), grain. Presets (LookPreset): overcast photo (matched to the user's DLSS-5 reference shots:
  sat 0.17, rgb 0.339/0.344/0.317 vs refs 0.34/0.35/0.31), clear and punchy, warm film. gradetest.cpp runs
  GradePass on a screenshot. Bloom / grain first versions were too weak (threshold 0.45 linear; grain 0.12).
- 0.53: SkyVis in PS_Half: 4 tangent directions x 6 steps out to SkyOcclusionRadius, horizon sine with distance
  fall-off, into the AO channel; PS_ABlur smooths that channel 4x4 depth-aware into hrt[5], which the sun pass reads.
  gpu k: open wall 1.0, wall next to a 1 m step 0.77-0.81, step face 1.0.

## 0.54 (2026-10-13): screen-space reflections, wet ground
- Late pass: PS_Snap writes 2 targets: [0] the snapshot minus our own light (bounce source), [1] the whole scene
  (hrt[6], mips) = the reflection source. PS_SSR (half, into hrt[5], reused after the sun pass read the smoothed AO):
  24 quadratic steps out to min(ReflectionDistance, 40 m + dist), hit = 0..0.3+0.08t behind, 4 halvings; a ray that
  ends on sky takes the sky; mip by roughness and ray length; screen-edge fade. PS_SSRApply (full, dual-source
  blend before the air): the game's Fresnel (Sp.rgb + (1-Sp.rgb) Sp.w f5), a water film's on wet up-facing
  ground (gloss -> 0.9, x0.65 darker). Params 64 Reflections, 65 WetGround, 66 ReflectionDistance; cb P12.
- gpu w: floor + wall with the screen position as the "scene": every reflection hit within 5% of the true mirror
  point (mean 1.1%).

## 0.55-0.59 (2026-10-14): reflections fixed up, atmosphere
- 0.55: menu g_order[64] overflowed at 67 settings (skipped items, dead groups) -> 256. SSR: wet ground flattens the
  normal for the ray (bump maps scattered it: noise), 4x4 depth-aware smoothing, 28 jittered steps.
- 0.56: temporal SSR (history ping-pong, reprojected with the previous camera record at t28 + this frame's at t40),
  walls reflect only at glass gloss (facades shimmered), sharp reflections from the full-size scene (t39).
  gpu w ... x: frame-to-frame flicker 0.31 -> 0.026.
- 0.57: variance clipping (mean +- 1.25 sigma), history weight by the neighbourhood's noise (0.3 smooth car paint ..
  0.9 rough wet asphalt): no trails on the moving car; distance check 1.5%. 0.58: ReflectionScale (% of the screen).
- 0.59: LampGlow in PS_Lamps: closed-form in-scattering of each tile lamp along the view ray (atan form), within the
  light radius; also on sky pixels (the lamp texture's alpha = 6e4 there; the sun pass adds it to sky and far
  scenery). AirLight: HazeBrightness factor, warm city glow at night (NightHazeGlow). Params 68-70, cb P14.

## 0.61 (2026-10-14): lamp light hybrid
- Correction: the back buffer is 5120x1440 (32:9 monitor); "the screen's" sizes = full size, half = 2560x720 (1/4).
- PS_Lamps writes a 3rd target: the pixel's strongest lamp (index + 1, shadow x zone factor) and leaves its highlight
  out; the sun pass redraws that one highlight at full size (LampSpecOne). Same-size harness: identical to in-pass.
- Bench night 3 (screen-size lamps, reflections 0.2 at 70%): whole mod +15.9 ms; lamps pass 7.0 (half: -5.0),
  radius 5->40 +4.8, reach 20->100 +3.1, reflections +2.3 (+1.0 for 30->70%), sky occlusion +1.2, look +1.0.

## 0.62-0.63 (2026-10-14): headlight beams
- Light shaders found by reflection (NameShader): kind 1 = batches of 4 lights (deferred_lightPositions/Colours/
  Directions arrays in cb1, positions camera-relative), kind 2 = projected spot (deferred_lightProjectionMatrix).
  Recon at night in traffic: NO batch draws (traffic lights are sprites/emissive, not lights); the player's headlights
  = 1 spot draw in the main view (and 1 in the 1024^2 road reflection view), in the sun pass's run.
- Spot cb1 (112 B): attenuation (0, 0.25, 10) -> 1 / (x d^2 + y d + z); colour 11.5; position camera-relative
  (~(-2.5, -0.97, 7.65) at the chase camera); rows M0..M3: clip = P.x M0 + P.y M1 + P.z M2 + M3 (P camera-relative,
  from inverseViewProj in cb0); inside if |xyz| < w; pattern t2 at (u, v), shadow t15 sample_c at (u, 1 - v), z.
- headlight.inc: after that draw on the main target: CopyResource of cb1 into our CB (b8), keep t2 / t15 / s15.
  PS_Lamps: 20 jittered samples along the view ray (to 70 m) inside the spot's frustum, pattern x attenuation x
  shadow, into the glow target (added by the late air pass, over transparents). gpu m: inside the cone 0.13, outside 0.

## 0.64-0.65 (2026-10-14): headlights tuned, soft headlight shadows
- Beam: x0.0005 (was 20x too bright), its own fall-off in the air 1 / (1 + d^2 / L^2), L = HeadlightBeamLength (20 m).
- GameHeadlights: the game's spot draw gets its blend state with SrcBlend ONE -> BLEND_FACTOR (factor = setting) in
  BeforeDraw, restored in AfterDraw (hlblendtest: 0.5 + 1.0 x 0.3 = 0.800, state restored).
- Soft headlight shadows: the game's spot shader with shadowMap0 comes in 2 variants (A: position from gGlobalCB
  inverseViewProj and TEXCOORD0.xyz; B: from the view ray TEXCOORD1). A copy of each (embedded source in
  headlight.inc, compiled at first use) with a 16-tap Poisson PCF (radius HeadlightShadowSoftness texels, b9) is bound
  in PSSetShader instead (g_lastPS stays the game's). hltest: copy == game to the bit (both variants, soft off);
  a straight shadow edge's transition 28 -> 44-47 columns at 3 texels.

## 0.66-0.67 (2026-10-14): traffic lights from the image
- Lamp buffer 4 entries a lamp: + x (slot / -2 = car light, own radius, car flag) and d (spot direction, cone cos).
  LampWindow: own radius window x spot cone; tiles use each lamp's radius; no glow for car lights.
- Car lights = lamp-finder clusters at road level (camera y -3 .. +0.5), not within 3 m of a map lamp, red (tail) or
  near white (head: cone 63 deg towards the camera, flat). Map-only lamp source only.
- 0.66 in game: they blinked all over the wet road: the finder caught glints / reflections (and our own light's
  highlights, a feedback loop). 0.67: PS_Emit writes the bright pixels' mean normal y (o.p.w = 2 + ny); clusters
  facing up (ny > 0.6) are glints; tracked round to round (same colour class, within 3 m, position lerp 0.6), used
  from 3 rounds seen, faded in over 4 and out over 4 misses. cartest: a moving red light used, a glint never, a
  one-round white spot not.

## 0.68-0.69 (2026-10-14): traffic lights tied to the cars
- Image-based car lights dropped (unreliable: glints, flicker). Recon of car draws (VS reflection carBits: 1 wheel,
  2 damage, 4 bones): traffic = VS with g_damageConstants + gvDamageVolumeParam (no bones); every part of a car has
  the same gObjectCB.world (rows x, y, z axes, row 3 position on the road). The player's car: bones VS. Its
  headlight spot (camera-relative) in the car's axes: (0.04, 0.73, 2.21): z forward, y up.
- carlights.inc: per traffic draw on the main target CopySubresourceRegion of the 64 world bytes from the VS cb
  into a staging buffer (ring of 4, 256 draws); read 2+ frames later without waiting, equal matrices merged,
  cars followed (nearest within 4 m of the predicted place), speed smoothed, lights placed at position + speed x
  frames since read. 2 headlights (+-0.65, 0.65, 2.1, cone cos 0.55 forward, slightly down) and 2 tail lights
  (+-0.6, 0.85, -2.2, red, wide backwards) as lamps (map-lamp day fade, own reach, no glow, no shadow maps).
  cbcopy: CB -> staging region copy works. carfollow: 3 parts -> 1 car, headlight error 3 cm -> 0 at 0.5 m/frame.

## Traffic lights on the car, same frame (0.70)
- User: lights jitter and lag behind the cars; parked cars glow too.
- The world matrices of this frame's traffic draws also go to a GPU buffer (t46). Lamp-fix pass (PS_LampFix, 1024x1
  RT over the lamp buffer, before the tile pass): each car light (X.w = light index + 1) finds the matrix nearest to
  the CPU's predicted car place and is rebuilt from it (position + spot direction/cone); no matrix -> colour 0.
  The CPU only chooses the cars (stage times in QPC seconds, speed per second, every ready capture read).
- Parked cars: lights only after the car was seen faster than 1.5 m/s. lampfixtest: turned car hit to 1 mm.

## Truck lights from mesh bounds, stricter "driving" test (0.71)
- User (0.70 test): much better; trucks need other light places; parked cars sometimes lit, and knocked ones light up.
- Mesh bounds: CreateInputLayout hooked (device vtable 11) -> POSITION format/offset/slot per layout. Each traffic
  draw's mesh (IB + start/count/base, VB + offset/stride) is read back once: index range to staging, then the used
  vertex range; decoded (float3/float4/half4/snorm16x4) to local bounds; 3 new meshes a frame, never waits.
  Logged as `car mesh N: fmt .. -> x .. y .. z ..` (first 200). A car's bounds = union of its parts (all known),
  kept if sane (L 2.8-18, W 1.2-3.4, H 0.8-5 m, bottom near 0). Lights: heads (cx +- (W/2-0.3), min(0.47H,1.15),
  front-0.2), tails (cx +- (W/2-0.25), min(0.6H,1.2), back+0.15); else the 0.70 car defaults.
  The local place goes in the upload's P.xyz (the fix pass turns it into world); D.w = heading.
- Following: pairs (car, track) within 3 m and heading dot > 0.7, closest pairs first (greedy-per-draw let a
  passing car's track jump onto a parked car and keep its "moved" flag). GPU match: 2.5 m and heading cos > 0.7.
- Lit only after 2.5 s seen driving: speed > 2 m/s, velocity along its own forward axis (cos > 0.94), speed change
  < 6 m/s2; a sideways slide / hard stop before that resets it. Stays lit once lit.
- Tests: lampfixtest (truck light on a turned car exact; wrong heading -> dark), meshtest (half4 at offset 8 of a
  24-byte vertex, 16-bit indices, start 6 base 4 -> exact bounds and light places). Not yet seen in game:
  the real traffic vertex formats (check the `car mesh` log lines), trailers (own matrix: would get headlights).

## Timing (0.19)
- GPU: D3D11 timestamp queries per pass (game sun draw, copy, half-res, GI blur, sun, air, overlay) + whole frame,
  read 5 frames later with DONOTFLUSH (never stalls). CPU: QPC between Presents, and time spent in our hooks.
- Overlay line 3-4; log line `timing:` every 10 s.

## Tools
- In-game capture: Shift+U → `scripts\NFSLight_capture\*.png` (1280x720 debug buffers), read with an image viewer.
- `um win shot/drive` available for screenshots/input — ask the user first (game has online modes).

## Traffic lights back, front/rear settings, lamp editor air (0.72)
- User (0.71): traffic lights gone; wants separate headlight / tail light settings; lamp editor view darker.
- Cause 1: the "driving" test compared the per-read speed with the previous read (acc < 6 m/s2 over ~30 ms):
  frame-time jitter in the stage timestamps made that fail nearly always -> drive reset -> never lit.
  Now: speed vs its own 0.5 s average, jolt = |diff| > 3 m/s; lit after 2 s forward (cos > 0.9), reset only by a
  jolt or a sideways slide before it is lit.
- 0.71 log: 1 mesh read in 4 min, fmt 13 (R16G16B16A16_SNORM, stride 44) with tiny bounds -> traffic positions are
  packed (scale elsewhere); the sanity check falls back to the car defaults. 0.72 logs why meshes fail
  (`car meshes:` every 1800 frames) and dumps the traffic VS disassembly to scripts\NFSLight_carvs.txt (3 shaders)
  to find the unpacking.
- Lamp editor (view 24) did not draw the air pass (drawAir list) -> no haze / glow -> darker. Added 24.
- Params 77-84: TrafficHeadBrightness/Warmth/Saturation/Cone, TrafficTailBrightness/Saturation/Reach/Cone
  (76 TrafficLightReach = headlights). Cone cosine goes in the upload's X.z as 1 + cos (still > 0.5 = car light).

## Far traffic, faster switch-on, mesh bounds working (0.73)
- User (0.72): parked cars fine; traffic lights come on ~2.5 s late and only near; far cars never lit.
- Delay: a new track started with v = 0 and ramped (0.35/read) -> the jolt test (speed vs 0.5 s average) fired for
  ~1 s. Now the 2nd read sets v directly and the average to it; lit after 0.6 s driving.
- Range: cut at 100 m -> 300 m; car-light score x3 (tails ranked as their car's heads) against the 256 places.
  kCarDraws 256 -> 1024 (a car is 10-20 draws: 256 held ~15 cars), cars 64 -> 128.
- 0.72 log: every mesh failed on "vertex range 0 + 65536": strip-cut indices 0xffff. Skipped now.
  NFSLight_carvs.txt (3 traffic VS): POSITION is R16G16B16A16_SNORM x 10 (`mul r0.xyz, v0.xyzx, l(10,10,10,0)`),
  world rows at cb1[24..27]. Decoder scales snorm by 10. meshtest: snorm x10 + strip cuts -> exact bounds.

## Lights out of view, parked guard, per-model light places (0.74)
- User (0.73): trucks fine; lights vanish when looking away and appear 0.5-1 s after a car enters the frame
  (only cars drawn are known); some models' lights a bit off; a hit parked car lights up for good.
- 0.73 log: meshes now read (115 done, no failures), most lit cars have bounds.
- Out of view: tracks kept 4 s after the last draw; lights placed at the predicted pose (speed for up to 1.5 s,
  then held; dropped after 3.5 s). Fix pass: no matrix -> axes from the heading (D.w) at the CPU place.
  lampfixtest: car not drawn -> lit at the expected place.
- Switch-on: 0.25 s of steady forward driving (0.6 before). Parked guard: standing > 1 s before ever being lit
  -> needs 4 s of driving (a knocked car rolls shorter).
- Per-model corrections: params 85-90 (AimedCarHeadOut/Up/Forward, AimedCarTailOut/Up/Back). While one is selected
  in the shown menu, the lit car nearest the screen centre (within 150 m) blinks and is edited; its model is known
  by bounds L/W/H within 12 cm; saved at once to scripts\NFSLight_carlights.txt. meshtest: offsets applied exactly.

## Lit cars remembered for a minute (0.75)
- User (0.74): light places tuned for all models (4 in NFSLight_carlights.txt, backup _NFSLight\*_2026-10-14.txt.bak);
  cars standing long at crossings lose their lights. No parked cars in traffic: a car that drove lately stays lit.
- Cause: a car out of view (houses, behind the camera) > 4 s was dropped; seen again standing -> new track ->
  "parked" -> dark.
- Now: lit tracks kept 60 s after the last draw (unlit 4 s; when full, the longest unseen makes room).
  Matching prediction capped at 1.5 s. An unmatched car takes a lit track out of view (> 0.3 s) within 15 m on the
  same heading (it drove on and stopped): keeps its lights, speed restarts. Out-of-view lights: moving 3.5 s,
  standing 20 s. A car first seen standing needs 2.5 s of gentle driving (was 4).
- cartrack test (30 reads/s, jitter): A lit at 0.3 s and kept after 10 s out of view, reappearing standing 25 m on;
  B knocked from rest never lit; C first seen standing, drives off at 1.5 m/s2 -> lit at 5.9 s.

## Ghost lights, crash, parked truck (0.76)
- User (0.75, lamp-editor shot): lights left hanging where a car was; a crashed car's lights off it; a truck lit.
- Lights of a car not drawn for > 6 frames whose place is on screen (> 4 m away) -> dark (it is not there);
  the out-of-view keep now only works out of view.
- GPU match 3.5 m, heading cos > 0.4 (a car spun in a crash; the CPU's heading is frames old).
- Track takeover out of view: only ahead along its heading (-2..40 m), within 1.8 m sideways (its lane), same
  size (L/W within 0.3 m when both known). The 0.75 rule (15 m any direction) let a parked truck take a lit track.
- lampfixtest, cartrack still pass.

## Sky research (0.77)
- User: the sky is plain, no clouds. 0.76 Shift+T: the lighting run (5120x1440 f1, 48 draws) starts with a pixel
  shader reading BaseCloudSampler..4 and BaseCloudColour: the game has a cloud sky shader, drawn first.
- skyrecon.inc: sky-like PS (cloud/sky/moon/star/atmos in names) disassembled at creation -> scripts\NFSLight_sky.txt;
  Shift+T: main-view draws listed by PS (`sky: draw ...`, blend state), first draw of each sky PS dumps every used
  constant (name = values from the bound buffers) and its textures (size/format/cube).

## Our sky (0.78)
- 0.77 sky research (NFSLight_sky.txt + Shift+T at night): main sky PS 784FFAB8, Draw(1080) dome, first in the
  lighting run. cb1 gObjectCB: BaseCloudColour, CloudParamsPS, CloudParamsPS2, KeyLightDirAndXZLength, SunColour.
  sky = pow(sat(dot(v, KeyLight)), SunColour.w) * SunColour.rgb + lerp(GradB, GradA by v4.w)(v4.z) * BaseCloudColour.w,
  then 4 layers: lerp(sky, tex.rgb * BaseCloudColour.rgb, sat((tex.a - CloudParamsPS[i]) * CloudParamsPS2.y)).
  Night values: colour 0.0187/0.0175/0.0167 w 0.25, thresholds 0.455 1 1 1 (3 layers off), sharp 0.956,
  key light (-0.70 0.38 0.60), SunColour 0.0068/0.0097/0.0121 pow 32. Cloud textures 256x64 (fmt 9), gradients 1D.
  Variants with SecondaryCloudSampler (512x512 BC1) render the reflection cube (not replaced).
- sky.inc: PSSetShader swaps the main sky PS (BaseCloudSampler4, no SecondaryCloudSampler, GradientTextureA at t5,
  5 inputs) for ours (same inputs, + b11 SkyCB): the game's sky, its layers with thresholds - 0.6 x SkyGameClouds,
  coloured + city light; our cloud layer: 5-octave value-noise fbm on d.xz / (y + 0.06), wind, self-shadow toward the
  key light; day = sat((Lum(SunColour) - 0.03) / 0.3) (guess: day values not seen yet); by day sun-lit, at night city
  glow from below; city glow at the horizon; stars (0.4% of cells, twinkle) high up at night.
  The game's sky constants logged every 30 s (`sky: game cloud colour ...`) to calibrate day/night.
- Params 91-98 (menu SKY): Sky, SkyGameClouds, SkyClouds, SkyCloudBrightness, SkyCloudScale, SkyCloudSpeed,
  SkyCityGlow, SkyStars. skytest: offline renders (_NFSLight\skytest_night/day.png, day constants guessed).

## Volumetric clouds, cloud shadows (0.79)
- User (0.78 shots, day/sunset/night): good, but clouds flat; at full cover the day should go overcast.
- 0.78 never logged the game's sky constants: the game binds cb1 AFTER PSSetShader -> logged now at the draw
  (AfterDraw, every 30 s).
- Clouds now in the world: slab 1200..2300 m; cover = tiling 2D fbm texture (256^2, 6 octaves, at t8) over
  5000 m x SkyCloudScale, wind 9/3.5 m/s x SkyCloudSpeed; density = cover x height gate (flat bottoms, tall where
  thick) eroded by a tiling 3D billowy noise (32^3 inverted Worley + value noise, t9, 900 m). 20 steps over
  <= 4500 m of the slab, white-noise jitter per frame; light: 2 samples toward the key light (100/400 m), Beer +
  0.55 x exp(-0.2 od) multiple scattering, phase 0.55 + 1.8 pow(cos, 8); ambient sky above, city below at night.
  Fade with distance exp(-t0/40 km) and at the horizon line. Sampler: our linear-wrap at s7.
- Sun pass: F *= CloudShadow(world, -Ld) (cover where the ray to the light crosses 1650 m; NFSLight.hlsl, t47 + b11
  bound by SkyBindForSun). Param 99 SkyCloudShadows (0.85): at cover 1 the direct light is 15% -> overcast.
- skytest: _NFSLight\skytest_volume_day.png. Not yet measured: the GPU cost at 5120x1440 (bench B on Sky).

## Clouds after Horizon Zero Dawn (Nubis), quarter-res cloud pass (0.80)
- User (0.79 shot): clouds smeared into vertical columns; wants real clouds (cumulus, rain clouds, cirrus) like
  Minecraft shader packs / known solutions.
- Cause of the columns: the sky's TEXCOORD0 is a dome POINT, not a view direction (0.79 normalised it): elevation
  barely changed down the screen. Now d = normalize(v1 - camera) when |v1| > 50, else v1. The sky VS is dumped once
  into NFSLight_sky.txt (VsDumpCode, all VS bytecode kept at creation) to confirm.
- Model (Schneider, "The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn", SIGGRAPH 2015; Nubis 2017):
  weather map 512^2 (r coverage, g type, b cirrus, a fine; tiling Perlin fbm) over 22 km; layer 1500..5000 m;
  height profiles stratus / cumulus / cumulonimbus by type; shape 64^3 (Perlin-Worley remapped by Worley fbm,
  spread x3 around 0.55) at 2.4 km; remap(shape*profile, 1-cov, 1, 0, 1)*cov; detail 32^3 Worley fbm at 520 m
  erodes (wispy low, billowy high); density x5. Light: 5 samples toward the key light (40 j^2 m), Beer + 0.6 Beer/4
  multiple scattering, powder (not toward the sun), two-lobe HG (0.75 / -0.25), ambient sky / city by height;
  rain absorbs more and darkens the base. Cirrus 2D at 8 km, stretched fbm. Shared HLSL kCloudCommon is prepended
  to NFSLight.hlsl at compile (sun pass CloudShadow: 4 cheap density samples along the ray to the light).
- Cost: full-res march = 26 ms for a whole 5120x1440 sky (skytest2). Now a cloud pass: the game's sky draw is
  replayed (BeforeDraw now gets the DrawCall) into a quarter-size RGBA16F target with the "clouds" entry, then the
  sky draw composites it (5 bilinear taps). Whole-screen sky 3.0 ms; in game the sky is a part of the frame.
- Params 100-104: SkyCloudType, SkyCirrus, SkyRainClouds, SkyWeather (custom/clear/fair/cloudy/overcast/storm/
  cirrus: sets cover, type, cirrus, rain, shadows), SkyQuality (steps). Previews: _NFSLight\sky_presets_080.png
  (fair, cloudy, overcast, storm, cirrus; the test view's top is distorted).

## Cloud shadows really applied, less cotton wool, exact view direction (0.81)
- User (0.80 shots): much better but cotton-wool look; no cloud shadows, the sun still lights the city.
- Shadows: CloudShadow multiplied F, which scales BOTH sunOrig (the game's sun, subtracted from dest to get the
  rest of the light) and sunNew: the light just moved from sun to "ambient". Now only sunNew gets it (Fc).
- Sky VS (dumped): o1.xyz = POSITION = the dome vertex in dome space; dome world at gObjectCB cb1[11..14] (+176).
  The view direction is now R*v + (dome translation - camera): the dome matrix is read (ReadVsConst, a small sync
  copy) every 30 frames with the camera at that time; logged once (`sky: dome world ...`).
- Look: multiple scattering in 3 octaves (a=b=c=0.5, Hillaire 2016) instead of one Beer + a floor; ambient
  occluded by the cloud 280 m above (0.6..1); clouds taller where cover is dense, small and low where thin
  (profile on h / (0.35 + 0.65 cov)); detail at 330 m, pushed by the shape noise (swirl, 140 m, less higher up),
  erosion 0.38; sky under heavy cover goes grey. skytest2 whole-screen 3.2 ms.

## No stall, clouds fixed in the world, checkerboard march, cloud-shadow map (0.82)
- User (0.81): clouds better, but a hitch about once a second; clouds move with the camera, light jitters.
- 0.81 log: dome world = uniform scale 400, translation = the camera; read while g_cam was still 0 -> the kept
  "offset" was the whole camera position: directions nearly constant (clouds glued to the view), re-read every 30
  frames (a sync GPU read: the hitch, and a jump). Removed: d = normalize(v1) (exact for a camera-centred sphere).
- The sky VS constants cb1[7..14] (viewProjection rows + dome world, row 3 = camera) are copied on the GPU each
  frame into a Buffer<float4> (2, ping-pong): the march origin is the exact camera now; last frame's VP reprojects.
- Checkerboard (Nubis-style): a per-pixel branch gained nothing (warps of 32 wait for the marching pixel). Now the
  march renders at 1/8 size (W/8 x H/8) for this frame's pixel of each quarter-size 2x2 (the dome point shifted by
  ddx/ddy: fractional viewports are not available at feature level 10), then a resolve pass at 1/4: own pixel from
  the march, the others from last frame's quarter picture reprojected (bilinear), else the march stretched.
- Cloud-shadow map: 256^2 R16F, 25 m texels around the camera (snapped), transmittance at the camera's height,
  made by replaying the dome draw; the sun pass samples it where its ray to the light meets that height.
- skytest2 with a real pinhole camera (its VS must output un-normalised directions: per-vertex normalising a
  full-screen triangle bends them, which looked like a reprojection bug): whole 5120x1440 sky 2.2 ms (quality
  0.5), 1.7 (0), 3.0 (1); base sky shader ~0.8. Preview: _NFSLight\sky_presets_082.png (fair, cloudy, storm).

## Cloud history: accumulate, clamp, sharp resampling (0.83)
- User (0.82 shot): performance good, but blurrier clouds that flicker strongly in the distance.
- Causes: history replaced (not accumulated) with a fixed jitter per pixel -> neighbours disagree near the horizon
  (long steps) and flicker as they refresh in turn; bilinear history resampling every frame + 5-tap composite blur.
- Now: the march start jitter changes each visit (golden-ratio sequence on g_frame/4); resolve mixes the own new
  march 35% into the history, others keep the history; history clamped to this frame's march 3x3 min/max (+25%
  pad); history and the full-size composite resampled with Catmull-Rom (9 bilinear taps). skytest3 cost unchanged.

## ReShade-safe hooking, release folder (0.84)
- User: on another PC with ReShade (DLSS5 via ReShade) the mod stopped working; asked for a separate,
  ReShade-compatible package.
- This PC: dinput8.dll = Ultimate ASI Loader (loads scripts\*.asi), version.dll = DroneCam. ReShade proxies take
  dxgi.dll or d3d11.dll in the game folder; InstallHooks used LoadLibrary("d3d11.dll") by name -> with a proxy
  d3d11.dll it detoured ReShade's exports (inline hooks into its code). Now the system d3d11/dxgi are loaded by full
  path (GetSystemDirectory -> SysWOW64 for x86) and the real device/context under the proxy are hooked; the log
  lists the proxy/loader DLLs found in the game folder and the hooked module paths.
- Not verified with ReShade here (the other PC's log is needed if it still fails).
- Release: _NFSLight\release\NFSLight_0.84\ (+ .zip): dinput8.dll (Ultimate ASI Loader, MIT), scripts\ NFSLight.asi,
  .hlsl, .ini (current settings), lamp map / zones / presets / templates, carlights; README_RU.txt.

## Clouds: detail fades with distance, half-size option, skip what the city hides (0.85)
- User (0.83): clouds still flicker far away, too low-res (pixelated at distance), and 0.83 felt heavier.
- Far sparkle: the 330 m detail noise (32^3, no mips) sampled at 10-30 km -> a pixel spans hundreds of metres of it.
  Now the erosion weight CLD = sat(1.6 - t0/9000) (full to ~5 km, none past ~14 km; also skips the detail fetch).
- Param 105 SkyCloudRes: 1 = the cloud picture at half size (march at a quarter, checkerboard), 0 = a quarter (0.84).
  skytest3 whole 5120x1440 sky: half 3.5 ms (q 0.5), quarter 1.9 ms (was 2.3 before the detail fade).
- Hidden pixels: the march reads the game's depth (g_air.depth, t15, bound after our RT replaces the game's DSV) at
  5 points of its footprint; all < 0.99999 (the sky VS writes z = w -> 1) -> no march (transparent). Not testable in
  skytest (no depth); if the clouds vanish in game this assumption is wrong.

## 80% / 100% clouds, full backup, installer (0.86)
- User (0.85): half-size clouds much better; wants 80% and 100% too. ReShade + DLSS5 on the other PC: still not
  working (its NFSLight.log is needed). Asked for a full backup and an installer in the F: backup folder.
- SkyCloudRes 0..3: 25 / 50 / 80 / 100% (cloud picture W*s, march W*s/2 with the 2x2 checkerboard).
  skytest3 whole 5120x1440 sky: 80% 6.9 ms, 100% 9.6 ms (in game less: partial sky, hidden pixels skipped).
- Full backup: <backup folder> (game_folder: _NFSLight, _DroneCam, scripts,
  dinput8.dll, version.dll, DroneCam.ini; test_tools; game_saves). NFSLight_v071.asi.bak never existed.
- Installer: <backup folder> [game folder] [/settings]: finds the
  game (F:/C: Steam paths or asks), refuses while NFS13.exe runs (System32 tasklist/find: Git's find shadows it),
  saves replaced files to _backup_before_NFSLight_<date>_<n>, keeps existing settings unless /settings. Tested on
  a fake game folder.

## Start behind ReShade's hooks (0.87)
- User installed DLSS5 (ReShade 6.8 dxgi.dll + dlss5-feed.addon32 + d3d9.dll) here: no menu, nothing.
- 0.86 log: "cannot hook D3D11CreateDevice / ...AndSwapChain / CreateDXGIFactory(1) (unexpected start)": ReShade
  loads first (dxgi.dll in the game folder) and hooks the system exports itself; Detour only accepted the
  hot-patch start (8B FF 55 8B EC) -> no device, no hooks at all.
- Detour now chains: E9 rel32 / FF 25 [m32] / push+ret starts get a trampoline to the existing hook (ours runs
  first, ReShade's next, then the original); unknown starts are logged with their bytes. The device we then see
  is ReShade's wrapper (its vtables patched; inline hooks fall back to vtable patches on unknown prologues).

## Photo look (DLSS-like) preset, CAS (0.88)
- User: works with ReShade/DLSS5 now; asked to get closer to a DLSS5 shot (pair: 32 ours, 33 DLSS5).
- Measured (no HUD region): DLSS5 shot vs ours: mean lum 0.208 vs 0.249, p10 0.038 vs 0.028, p90 0.475 vs 0.567,
  saturation 0.173 vs 0.220, car red (0.505 0.249 0.231) vs (0.610 0.196 0.181).
- PS_Grade: AMD FidelityFX CAS (3x3) replaces the 4-tap sharpen; film curve (Gr5.x: back towards scene light, then
  extended Reinhard W=6 + lifted toe); red saturation (Gr5.y: also darkens reds, crimson not pink); highlights to
  white (Gr5.z). Params 106-108 LookFilmCurve / LookRedSaturation / LookHighlightDesat; GradeCB 96 bytes.
- Preset 4 "photo (DLSS-like)" tuned with gradetest on the user's (already graded) shot to the DLSS5 one:
  mean 0.215 / p50 0.155 / p90 0.481 / p99 0.608, car (0.522 0.250 0.238). Comparison: _NFSLight\grade_photo_preset_088.png.
- Left for later: reflections on car paint / glass, sharper wet-road reflections (the rest of the gap).

## Custom keeps your values; rain wetness by sky / material / puddles (0.89)
- User (0.88): photo preset not quite it, but keeps the new controls; going to a preset and back to custom lost the
  own values. Reflections: wet everywhere facing up (the driver's seat, roads in tunnels, grass); wants car-paint
  reflections next.
- PresetMemory (Look preset 61 -> 18 params; SkyWeather 103 -> 5): leaving custom saves the values, coming back
  restores them; changing a member under a preset switches to custom; saved in the ini (LookCustom, SkyCustom).
- Wetness(): up-facing x open to the sky (a vertical ray 0.35..35 m marched through the depth in the picture; a
  surface just in front of it = a roof / ceiling -> dry; off-picture = open) x material from the albedo (green ~0,
  colourful porous 0.45, grey 1) ; puddles: value noise over x/z (3.2 m + 0.9 m) on grey ones -> gloss 0.97, flat.
  Used by PS_SSR and PS_SSRApply (albedo now bound at t19 in both; it was null in the SSR passes). Wet darkening
  0.35 + 0.1 in puddles.
- Debug views 25 gloss (G-buffer N.w), 26 specular colour, 27 rain (blue wet, white puddles, red dry under a roof);
  captures 19_gloss, 20_specular, 21_rain_wetness (Shift+U). Needed to see whether car paint is in the G-buffer.

## Rain map from above (0.90)
- User (0.89 shots, tunnel + gas station): a hard dry patch only under a lamp (the tunnel roof did not count), dry
  patches under moving cars, a dry band from the screen edge that moves with the view: 0.89's "open to the sky" was a
  march up in the picture (anything off screen = open; anything seen above = roof).
- rain.inc: while the game draws its widest sun cascade (every building / bridge / tunnel / post, no cars), each draw
  is replayed with our orthographic top-down view (the game's packed VS view: rows x, y, depth, then z/w scale+bias;
  w = 1) into a 1024^2 D32 map, 256 m around the camera (25 cm texels), 700 draws a frame, then swapped (double
  buffer), rebuilt continuously. The game's PS stays bound (cut-outs keep holes). Shader RainOpen: 3x3 samples,
  open where height > top - 0.35 m. b13 RainCB (+ t51).
- Cabins: InCabin() tests the player's car (bones VS world matrix, read back via a 4-stage ring) and the 15 nearest
  traffic cars (carlights tracks): |x| < 0.75, -1.3 < z < 0.8, 0.25 < y < 1.2 m = dry. The road under a car stays wet.
- Not verified in game yet (no offline test possible: needs the game's caster draws).

## Rain map kept and moved, dry floors under roofs, ceilings (0.91)
- User (0.90 rain views): the gas-station canopy in the dry zone only from some angles; not far enough in the
  tunnel; the tunnel ceiling looks wet; the tunnel road still shiny inside the red (dry) zone.
- The widest cascade only has casters whose shadows reach the present view: a rebuilt map loses what is behind
  you. Now one map is never cleared (depth LESS keeps the highest of all ever drawn), 512 m (0.5 m texels), 900
  draws a frame in turn; when the camera is 96 m off (or 120 m in height) it is shifted onto the new centre by a
  pass (SV_Depth from the old map at a texel offset, depth + top change / range; depth cannot be copied at an
  offset). raintest: shift values / edges right.
- SsrVis: ceilings (n.y < -0.5) need gloss 0.85 (as walls), not the floor's 0.55.
- Floors under a roof (covered): gloss limited to 0.45 (also the game's own sheen) when wetness is on.

## Rain map kept and moved, dry floors under roofs, ceilings (0.91) + car paint reflections (0.92)
- 0.91 (user, 0.90 rain views: canopy dry only from some angles, tunnel not far enough, ceiling wet, tunnel road
  shiny inside the dry zone): the widest cascade only has casters whose shadows reach the present view, so the map
  is now never cleared (depth LESS keeps the highest ever drawn), 512 m at 0.5 m, 900 draws a frame in turn, shifted
  by a pass when the camera is 96 m off (SV_Depth from the old map at a texel offset; depth cannot be copied at an
  offset; raintest checks it). SsrVis: ceilings need 0.85 (as walls). Covered floors: gloss <= 0.45 (no game sheen).
- Capture (user, tunnel): G-buffer gloss N.w ~0.9+ on nearly everything (concrete, asphalt); the specular colour is
  near black on roads/walls and bright on cars; car gloss ~0.7 (sides missed the 0.78 wall threshold).
- 0.92: PaintOf(Sp) = CarPaintReflections (param 109, P11.z) x sat((lum(spec) - 0.1) x 5): gloss -> 0.93, always
  reflected, Fresnel of a clear coat (0.04 + 0.96 f5). Menu REFLECTIONS.

## Soft dry edges, no sky in dry walls, the game's wetness found (0.93)
- User (0.92): better; hard wet/dry edge (0.5 m stair steps), clouds reflected where nothing is wet, wants the
  game's own (vanilla) wetness off: it is everywhere.
- RainOpen: 13 samples (centre + rings 0.9 m / 2 m, turned by a hash of the place), soft height test
  sat((y - top + 0.6) / 0.5), smoothstep: ~2 m soft edges, rain blowing a little under roofs.
- Dry surfaces reflect only with a real specular colour: dryOk = sat((lum(spec) - 0.04) x 25) (the game's gloss is
  ~0.9 on concrete and brick too): no clouds in dry walls; paint / wet as before.
- Research: WetNote (skyrecon.inc) disassembles every PS with a constant named wet/rain/puddle/driveable/damp into
  scripts\NFSLight_wet.txt (40 max), to find the game's road wetness switch.

## The game's wetness as a setting; wetness fades under roofs (0.94)
- User (0.93): still clouds in dry places, edge still too hard (wants a gradient inward), the game's own puddles /
  wetness still everywhere: make it a setting (off / strength; later tied to rain).
- NFSLight_wet.txt: 31 shaders with WetScaleParams (gGlobalCB +16: 0.055, 0.35, 5, 0 = planar reflection offset /
  mip, not an amount). Road PS: puddleMapSampler (t10 there; t5 in others) sampled at v5.xy: alpha x v5.z = wet
  amount: albedo darkened (lerp to rgb), normal flattened, gloss raised, planarReflectionSampler x the same amount.
- gamewet.inc: shaders with a puddleMapSampler noted at creation (slot by reflection). GameWetness (param 110,
  menu REFLECTIONS): 1 = the game's; 0 = a 1x1 (1,1,1,0) map bound for the draw; between: a copy with alpha x k
  (RGBA8 + mips, a pass, cached per map and value). Swapped in BeforeDraw, put back in AfterDraw. puddletest: copy
  alpha 200 -> 100, rgb kept, cached.
- RainOpen: 19 samples (0.9 / 2 / 3.5 m rings): wetness fades ~3.5 m inward from a roof's edge.
- The "clouds in dry places" in the user's shot are most likely the game's planar reflection through its puddle
  map (GameWetness 0 should remove them).

## The game's gloss before its lighting; reflections follow the wetness (0.95)
- User (0.94, gloss view): the cloud-like blotches are in the G-buffer gloss itself (the asphalt's gloss texture):
  the game's own lighting reflects the sky in them (also with our reflections at 0.05 and its wetness at 0).
  The wet/dry edge still looks cut: SsrVis is a threshold on the gloss.
- PS_WetGloss (NFSLight.hlsl) drawn with the sky dome right after the clouds (SkyBeforeDraw -> WetGlossPass), into
  the normal target (our RTV on the game's normal texture), alpha only, blend dest.a x src.a: dry floors / walls
  keep DryGloss (param 111, 0.55), wet floors (rain map, cabins) keep 1, bright specular (paint, metal) keeps 1.
  Place: P = cam + t d, d = normalize(dome point), t solved from depth = (A.z + t B.z) / (A.w + t B.w) with the sky
  VS's viewProjection rows (tDomeVP, t52); facing from ddx/ddy of P. The game's PS SRVs 0..15 saved / restored
  around it (the normal texture bound as a target unbinds it as a texture). Needs our sky on (it rides its draw).
- Reflections follow the wetness: smoothstep(0, 0.8, wet / WetGround) instead of sat(wet x 4).
- RainCB gets RN2 (wetness, dry gloss); cars at RC (after it), 816 bytes.

## Gloss fix really on (0.96)
- User (0.95): nothing changed, DryGloss no effect. Log: "gloss fix: no target view on the normal buffer": the
  normal texture is typeless, CreateRenderTargetView(r, 0) fails. Now the RTV takes the SRV's format (logged:
  "gloss fix: on the normal buffer (format N, WxH)" or the failure with hr / formats / bind flags).

## Gloss fix: on (0.96) and replacing the game's blotchy gloss (0.97)
- 0.95: "gloss fix: no target view on the normal buffer": the normal texture is typeless; 0.96 makes the RTV with
  the SRV's format (format 28, 1920x1080 in game).
- User (0.96 gloss view): the blotches stay on the open street: 0.96 kept the game's gloss where wet (open sky).
- 0.97: two passes over the normal target's alpha with the sky dome: x a (blend dest x src.a), then + b (blend add):
  wet floors: a = 0, b = an even gloss 0.82 (0.96 in our puddles); dry: a = DryGloss; paint / metal: a = 1, b = 0;
  in between by the wetness w = smoothstep(0, 0.8, open x (1 - cabin) x up x material x WetGround) (rain map
  gradient, albedo: grass ~0). glosstest (512x256 plane, 0.60/0.95 checker): wet -> 0.82..0.96, dry (DryGloss 0.5)
  -> 0.30..0.47, sky untouched.

## The "clouds" were our puddles; reflections follow the wetness (0.98)
- User (0.97): the cloud-like reflections only come with WetGround (ours); the wetness gradient (rain view) does not
  reach that effect.
- The "clouds": our puddle noise (3.2 m + 0.9 m patches, big white blobs in the rain view): mirror gloss 0.97 in
  them, 0.8 around -> sharp sky (with our clouds) in blotches. And the wet edge: visibility went through SsrVis's
  gloss threshold, so a partly wet floor was either nothing or nearly full.
- Now: wet visibility = smoothstep(0, 0.8, wet / WetGround) directly (SsrVis only for dry surfaces x dryOk); wet
  asphalt gloss 0.62 (a blurred sky), puddles 0.95 scaled by Puddles (param 112, 0.3; P11.w, RainCB RN2.z); the
  gloss pass's wet target 0.7 .. 0.95 (puddles). glosstest: wet 0.70..0.78, dry 0.30..0.47.

## Reflection debug views; wet floors reflect as nearly flat (0.99)
- User (0.98): still the "clouds"; asked for a proper debug tool and, if needed, a rewrite of WetGround.
- Views 28-32 (U list, "reflections"), drawn by PS_SSRApply with k = 0 (only what they show): 28 what our
  reflections add (x2); 29 by source (red wet, green dry gloss x dryOk, blue paint); 30 what the ray found (raw);
  31 wetness (white) / puddles (blue); 32 the normal used.
- Suspect: the wet path kept ~45% of the asphalt's normal map: its large-scale bumps scatter the reflected sky into
  blotches. Now the wet normal is up by wetVis x 0.9 (puddles 1.0), the same in PS_SSR and the Fresnel of the apply.

## 1.00 (2026-10-15): wet-ground blotches, debug views by exposure
- Debug view 29 (source) showed the "clouds" as red + green = orange patches on paving, not asphalt. Causes found:
  (1) the dry-gloss path (SsrVis(gloss) x dryOk) used the WET gloss, so in puddles (gloss 0.95) it fired as well:
  the sharp sky in puddle-shaped patches; (2) Wetness() took `up` from the G-buffer normal = normal-mapped (tiles /
  cobbles tilt it per texel); (3) mat varied with the albedo's colour (reddish slabs drier).
- Fixes: dryVis = SsrVis(glossDry) x dryOk x (1 - wetVis) (glossDry = before the wet lerp); GeoN(): the normal from
  the depth (cross(ddy(rel), ddx(rel)), computed before any return), up from max(n.y, geo.y); mat = 1 - green x 0.95
  only (green: smoothstep 0.08..0.25); puddles only on dark asphalt (Lum(albedo) < 0.16..0.3); Puddles default 0
  (user ini set 0, backup NFSLight_ini_before_100.bak). GlossFix gets the same material rule.
- Debug views glowed at night: the game multiplies the HDR image by the w of the 1x1 HDRToneMapping texture (exe
  shaders: downsample = sum4 x w / 128 -> RGBM; bloom divides by it). ExpoGrab (BeforeDraw): when a shader named
  with HDRToneMapping draws, its 1x1 SRV (slots 0..3) is copied into g_expoTex (t56); PS_Sun (all views but 8 and
  13), PS_Air 19/20 and PS_SSRApply 28..32 multiply by 1 / w. Logged every 600 frames while a view is on.

## Plan (2026-10-15, not started): our own sun shadows instead of the game's
User: shadows poor and glitchy vs modern games; wants them replaced: near the caster sharp, far from it soft (sun far).
1. Own shadow atlas: replay the game's caster draws (its 3 cascades: widest = statics, near = cars too) into our own
   4 stable cascades (texel-snapped: no shimmer; blend band between cascades), sun direction smooth every frame
   (the game steps it: jbe at VA 0x8A8FA7).
2. Physical penumbra (PCSS): width = (receiver - blocker distance) x sun angular diameter (0.53 deg, ~0.93 cm per m),
   in metres; blocker search radius from the max penumbra; rotated Vogel disk + temporal accumulation.
3. Bias: normal offset + receiver-plane slope bias (no acne / peter-panning).
4. Past the last cascade: far building shadows from a top-down height map (like the rain map).
5. Contact shadows for the last cm..m, with a thickness test (the old car ghosting).
Waiting for the user's screenshots of the current glitches.

## 1.01 (2026-10-15): our own sun shadow maps (sunshadow.inc)
- User (screens): shadows ripple (stripes on facades = acne), grainy dithered penumbras, sharpness tied to the camera
  (the game's cascades are fitted to the view), blotchy wide penumbra on a wall, diagonal cut lines.
- Research: SHADERS1.BNDL scanned (cbscan.py: disassembly, cbuffer layouts): every shader has its own packed gGlobalCB
  (ViewProjectionModified at 0 / 16 / 112 / 128...); the shadow pass is the first thing in a frame (before the main
  camera is uploaded): our cascades are placed from the camera record read back 1-2 frames late + its speed.
- Design: 4 square cascades (half 10/30/90/270 m by default = ShadowDistance / 27, 9, 3, 1) around the camera (+0.3 x
  half ahead), basis right = cross(up, L), up = cross(L, right); centre snapped to the texel grid; depth +-2000 m along
  the sun, depth clip off (casters beyond: flattened). Sun = KeyLightDirection read back from the sun pass's cb0.
  Atlas 2x2 (Texture2D, 2N x 2N D32): shader model 4.0 cannot SampleCmp a Texture2DArray (compile error X4532);
  the GS picks the quarter with SV_ViewportArrayIndex (4 viewports).
- Drawing: after each game draw into its shadow atlas (cascade = round(TopLeftY / Height)): cascade 2 (widest) non-car
  draws and cascades 0/1 car draws are replayed once: VS cbuffer at vpSlot -> an immutable buffer with an identity
  packed view (rows x, y, depth = world; (1, 0, 0, 1)), GS emits each triangle into every cascade its bbox touches.
  Game PS bound (cut-outs) -> a GS generated from the VS's output signature (captured in VsExamine), checked by
  reflection (same registers / masks in and out), else drawn solid without the PS.
- Mask (sun pass, after the camera record): PS_SunMask (full res, RGBA16F: vis, caster distance m, penumbra m,
  cascade): cascade = finest holding point + search reach (stochastic 15% band -> next; past the last -> the game's
  1-tap map, z = 3 in the band, 4 outside); geometric normal (GeoN, else G-buffer at edges); lift by 1.5-3.5 texels;
  receiver-plane depth per tap (grad = (n.R, n.U) / N.L, clamped 8); 16 Vogel taps blocker search (radius =
  CasterReach x 0.00925 x SunAngle / 2; in the next cascade above 24 texels), penumbra = caster distance x 0.00925 x
  SunAngle (min ShadowSharpestCm), 16 bilinear SampleCmp taps over its disk. PS_SunTemporal: reprojected history (its
  distance must agree), clamped to the 3x3 of this frame, alpha max(1/n, 1 - smoothing). PS_Sun: gNew = mask (linear,
  not squared) where z <= 3, else the old path.
- Harness ltest/shadowtest.cpp (FL10.0 hardware, includes nfslight.cpp): game-style caster VS through SsAfterDraw,
  CPU ray-cast G-buffer, 16 frames. Atlas depth vs expected: tower top +2 cm, ground +1 cm, box +0.5 cm, 180 m +9 cm.
  Lit ground / lit wall / front faces at N.L 0.15 and 0.05: 1.000 (no acne). Penumbra 10-90% (sun x1): tower edge
  (8.5 m) 8.0 cm vs 7.6 expected (0.69 x 7.8 / sin 45), box edge (2 m) 1.8 vs 1.8; sun x4: 34 vs 31, 9.6 vs 7.2.
- Params 113-119: SunShadowMaps, SunAngle (1 = real sun), ShadowResolution (2048/3072/4096 per cascade; default 3072
  = 144 MB), ShadowDistance (270), ShadowSmoothing (0.8), ShadowSharpestCm (0.3), ShadowCasterReach (80 m). View 33 =
  our shadow, one frame; 16/17/18 show ours. Log: counts every 900 frames, CPU ms, the game's widest cascade size.

## 1.02 (2026-10-15): casters from all of the game's cascades
- User (1.01 in game): "very cool", but at some camera angles a building's shadow on the street is gone; heavy lag.
- Log 1.01: the game's cascades 31 / 288 / 1017 draws a frame; we took cascade 2 (non-car) + 0/1 (cars) and skipped
  ~300 ("not ours"). The game's widest cascade is 450 x 450 m (half 225) fitted to the FAR part of the view: casters
  near the camera are at some angles only in cascades 0/1. Every game shadow draw had a PS (all through the generated
  pass-through GS, 0 dropped). Our draws: CPU 0.6 ms, mask 0.5-0.9 ms GPU; GPU frame 50-63 ms (mod passes 17-21, lamps
  7-9 of it).
- Now every draw of every cascade, a draw already seen this frame (hash of VS, VB0/1 + offsets, IB + offset, draw
  args, the object constant buffer at wSlot) skipped. Only while the object buffers look per-object (last frame:
  distinct >= 30% of keyed draws), else everything is drawn (logged on change). Harness: the 4 casters drawn again
  "in cascade 1": 4 skipped, results unchanged.
- New timing "shadow pass (game + ours)": GPU from the first draw into the game's shadow atlas to the first draw after.

## 1.03 (2026-10-15): our shadow maps depth-only unless the game's PS cuts out
- User (1.02): shadows fine at every angle now; FPS 10.9 with our maps vs 50.5 without (4096 x 4 chosen in the menu).
- Log 1.02: shadow pass (game + ours) 78 ms vs 13 ms without ours; 54 / 369 / 1844 game draws, all ~2250 ours, every one
  through the pass-through GS with the game's PS; only 13 object constant buffers a frame (the game shares them: the
  "drawn once" check switched itself off as designed).
- Cause: every caster drawn with the game's pixel shader into 4 x 4096^2: full pixel shading, no early depth.
- Fix: PsDiscards (HkCreatePS): the SHDR/SHEX chunk walked by instruction (opcode & 0x7ff, length bits 24..30, custom
  data 0x35 length in the next token), opcode 0x0d = discard. Checked against the disassembly on all 680 SHADERS1
  pixel shaders: 680 agree, 144 discard. In SsAfterDraw a PS that does not discard is dropped: depth only through the
  position-only GS. Log line now has "depth only N".

## 1.04 (2026-10-15): no geometry shader; near cascades every frame, far ones kept
- 1.03 in game: depth only 3088 of 3699 draws, still shadow pass 60-79 ms vs 10-13 without ours, CPU 50+ ms (waiting
  on the GPU): the geometry shader itself (3700 draws through a GS) is the cost, not pixel shading.
- Now plain replays with the VS view constants replaced per cascade (zeros + our view packed as the game packs it,
  remade when the cascade moves; one buffer per view offset per cascade). Membership: car or game cascade 0 -> ours
  0, 1; game 1 -> 0, 1, 2 (it has the near casters the widest misses); game 2 -> 2, 3. Far 2, 3: still things only,
  never cleared but when placed anew (camera moved a quarter of the cascade, sun 0.35 deg, or 3600 frames; one a
  frame); between, ShadowFarBudget (param 120, 400) draws a frame by an index cursor (like the rain map). Near 0, 1:
  cleared and drawn every frame. Cascades: ShadowDistance / 20, 6, 3, 1 (13.5 / 45 / 90 / 270): cars cast to 45 m.
- Textures: near 2N x N (0, 1 side by side, t57), far N x N each (t61, t62); SsDepth / SsCmp pick by cascade.
- Harness: same depths (+1..8 cm), penumbra 8.0 / 1.8 cm, no acne.

## 1.05 (2026-10-15): far cascades no longer rebuilt every frame; built over frames, swapped
- 1.04 in game: shadow pass 75.6 ms vs 12.4 without; log: far placed anew 1085 times in 900 frames, far draws 3077 a
  frame. Bug: the far centre is the camera + 0.3 x half ahead, but "moved" compared the camera itself with it: always
  0.3 > 0.25 of the half -> a full rebuild (all ~2800 far draws) every frame.
- Fix: compared with where it would be placed now. And no full rebuild in one frame any more: each far cascade has a
  spare texture; a new placement is built there by the same budgeted cursor (ShadowFarBudget a frame) while the old one
  is used, then swapped (rows and sizes copied into the shader constants at the swap). Not building: the budget goes
  into the one in use (casters seen from new directions are added). Memory: 6 x N^2 x 4 B (216 MB at 3072).
- Harness: 4 static frames -> each far placed once (2), depths and penumbras unchanged.

## 1.06 (2026-10-15): fewer draws into our cascades
- 1.05 in game: far placed anew 8-10 times / 900 frames (fixed); shadow pass 30.2 ms vs 12.8 without ours (FPS 24.6 vs
  44.4); ours near 890, far 701 draws a frame (game 64 / 387 / 1805).
- The game's middle cascade (big buildings) went into our nearest (13.5 m half) too: now only while the game's nearest is
  smaller than 2.5 x ours (its size read back every 300 frames for all three cascades and logged when it changes).
- Far: ShadowFarBudget 400 -> 200 while building; a quarter of it while a cascade is only kept up.

## 1.07 (2026-10-15): far cascades rebuilt continuously (moving things left trails)
- User (1.06): shadows of moving things (a construction lift going up and down, crane parts) smear into a trail that
  builds up for ~10 steps and then vanishes. Cause: the far cascades added draws into the one in use (rain-map style:
  whatever was drawn stays) - right for buildings, wrong for anything that moves without a car shader.
- Now each far cascade is always being built anew in its spare texture (ShadowFarBudget draws a frame by the cursor)
  and swapped in when every draw went in (~10 frames): every object once, where it was. Placed at the camera (+0.3 x
  half ahead) each time; the sun direction kept until it moved 0.35 deg. Far moving things step every ~10 frames;
  within 45 m (near cascades) every frame as before. Harness: far depths right after swaps.

## 1.08 (2026-10-15): modern lighting model (GGX / PBR)
- The game's sun pass (SHADERS1 disassembly): normalised Blinn-Phong, power = 2^(11 (1 - normal.a)), Fresnel
  F = spec + (1 - spec) x spec.a x (1 - v.h)^5 with a specular colour near black on roads / walls / stone (no highlight,
  no grazing sheen); ambient = 4 x albedo^2 x (1 - spec) x cubeIrradiance(n)^2 x albedo.a (+ the additive buffer x
  HDRConstants.y / tonemap.w). Some variants have a second cube (cubeIrradianceSampler2).
- New (Lighting = 1, param 121): GGX D, Smith height-correlated V, Schlick F with F0 = max(spec, 0.04 x
  SurfaceReflectance); alpha = sqrt(2 / (power + 2)) x Roughness (peak of the old lobe kept: paint 8.4 vs 8.3); Burley
  diffuse; lamps (LampShade, LampSpecOne) the same. Sky reflections: 4 x cubeIrradiance(R)^2 x albedo.a x split-sum
  (Karis' fit) x AO x ambient scale (x night dimming), not on paint / metal / glass. The cube SRV (by name) copied to
  t63 in the sun pass (flag 512). LightCB gets P16 (model, reflectance, sky reflections, roughness).
- Menu group MATERIALS (LIGHTING MODEL): 121-124. Views 34 (sky reflected x4), 35 (roughness).

## 1.09 (2026-10-15): texture replacement, material picker (texreplace.inc)
- Device vtable 5 (CreateTexture2D) and 7 (CreateShaderResourceView) hooked. A texture with initial data (2D, one
  slice, not a cube, shader-readable, known format) gets a 64-bit hash of its top level rows (+ size, format); a file
  scripts\NFSLight_textures\replace\<16 hex>*.dds is created instead (DDS: DXT1/3/5, ATI1/2, 32-bit RGBA / BGRA masks,
  DX10 header; one 2D texture; levels as in the file; sRGB from the game's format); views of it get its format and all
  its levels.
- Shift+X: the next frame every G-buffer draw (one of its targets = the G-buffer normal texture) is replayed with a
  pixel shader writing its number into an R32F target (depth LESS_EQUAL, no write); the centre texel read back; that
  draw's textures (slots 0..15, named by reflection of its pixel shader) saved from the GPU to
  NFSLight_textures\picked\<hash>_<pick>_<sampler>.dds and logged ("texture pick N: slot ...").
- Harness ltest/textest.cpp: BC1 sRGB 256x128 x9 levels: hash stable, saved and read back byte-equal; a 512x256 RGBA8
  replacement created instead (fmt 29 = RGBA8 sRGB), the game's BC1-sRGB 9-level view made as fmt 29, 1 level.
  The picker is not tested outside the game.

## 1.10 (2026-10-15): textures filled after creation
- 1.09 in game: the picker works (pick 3: a tiled ground material: MaskTexture 256 BC1, Tiling1..3 1024 BC1, Decal 1024,
  Tiling1..3Normal 2048 BC3, AOMap 512 BC3, LightMap 16), but "made with data" counted 2: the game makes its textures
  empty (formats 70 / 76: BC1 / BC3 typeless) and fills them later.
- Now tracked from creation (also staging ones); the fingerprint taken when the top level arrives: UpdateSubresource of
  level 0 (no box), or a write Map / Unmap of level 0 (staging / dynamic) then CopyResource / CopySubresourceRegion
  (0 -> 0, no box) into the texture. UpdateSubresource, Map, Unmap, CopyResource, CopySubresourceRegion and
  PSSetShaderResources hooked in code (vtable fallback). A replacement of the same size and kind is written in place
  (every level; the game's later uploads to it dropped); another size / format: an immutable typeless texture of ours,
  its view (sRGB as each game view asks) put in place of the game's view in PSSetShaderResources (views noted at
  creation -> their texture). Counters per path in the 10 s "textures:" line.
- Harness ltest/textest2.cpp: update path -> hash, in-place replacement, every level ours; staging -> copy path -> hash,
  a 512x256 RGBA8 replacement bound instead (fmt 27 texture, fmt 29 view for the game's BC1-sRGB view).

## 1.11 (2026-10-15): relief from normal maps (small sun shadows, cavities)
- Picked textures (Shift+X on grass / ground, 1.10): Tiling1..3Texture BC1 1024 (colour; all three the same here),
  Tiling1..3Normal BC3 2048: RGB a plain tangent normal map, ALPHA = roughness-like (white rough, pebbles darker: it
  feeds the G-buffer alpha), Mask BC1 256 (RGB = the three layers' weights), Decal BC1 1024 (G, B: dirt / worn patches),
  AOMap BC3 512 (grey), LightMap 16 (night light, empty here).
- Relief() in PS_Sun: the surface's plane = GeoN (depth derivatives; the G-buffer normal at edges). Along a direction d
  the height changes by -n.d / n.g per metre (clamped +-3); walking on screen (Project + the scene distance must match:
  an edge stops it) and summing gives the height: towards the sun (12 steps of ~1.3 px, ReliefReachCm 12) a rise above
  the sun's ray shades (soft); 4 rotated directions x 5 steps: the rise around darkens the sky light. Fades out once a
  pixel covers > 6..16 mm. contact *= sun part, ao *= sky part. Params 125-127 (Relief, ReliefDepth, ReliefReachCm),
  LightCB P17; view 36.
- Harness ltest/reltest.cpp: ground whose normal map holds ridges 1 cm peak-to-trough every 20 cm (slopes 9 deg):
  sun 20 deg up -> no sun shadow (right: the slopes are lower than the sun), troughs 0.94; sun 6 deg -> shadow bands
  behind each ridge (mean sun 0.935), picture relief.png.

## 1.12 (2026-10-15): relief tuned (user: walls want 2.1-2.5, cars 1.0-1.1; edges too sharp, "charcoal")
- Strength = Relief x lerp(1, ReliefWalls (param 128, 2.3), verticality) x (no paint / metal: spec luminance > 0.06)
  x (only where GeoN agrees with the G-buffer normal, dot > 0.75: the red outlines along cars and pillars in view 36
  were the depth slope across an outline taken for the surface).
- Softer sun part: (h - s tanE) / (3 mm + 0.12 s); never below 0.15 (sun) / 0.25 (sky): micro shadows are sky-lit.
- Harness reltest: ridges under a 6 deg sun 0.967 mean (was 0.935, softer edge), troughs unchanged.

## 1.13 (2026-10-15): stutter fixed, debug views at a fixed exposure, the picker saves the shader
- User: a short freeze twice a second, even with NFSLight off (Y). Cause: 1.10's Map / Unmap hook hashed every write map
  of a texture's top level at Unmap; the game rewrites a dynamic texture twice a second ("by map" +20 per 10 s) and the
  hash read it back from mapped GPU memory. Now only staging textures (the copy-in path: 3 in a session), and a sampled
  fingerprint (64 rows, 4 KB each) for them.
- User: view 36 (and others) too bright, brightness creeping after switching. Cause: the game's eye adaptation chases
  the debug picture while DbgScale divides by its exposure: a loop. Now in a debug view (not 0, 1, 24) the game's 1x1
  exposure texture is set to 1 (UpdateSubresource before the tone-mapping draw, every frame): the picture shows its
  values; the game adapts back after.
- Shift+X also writes the picked draw's pixel shader disassembly (picked\NNN_shader_<fnv64 of its code>.txt): material
  shaders to rewrite with parallax.
- Harness textest / textest2 pass (staging path with the sampled hash).

## 1.14 (2026-10-15): parallax on roads / pavements (matpom.inc)
- Picks (Shift+X, 1.13): pavement and road -> pixel shader 55f2f243af1bf610 (ColourMap 2048x4096 BC1, DetailMap
  diffuse 512 + normal 512, SpecColour (G), Normal 1024x2048 BC3 (alpha = gloss), RoadDepth (G: white surface, dark
  cracks / joints / holes; times the puddle map = where puddles stand), puddle map, planar reflection, a 3D Fresnel LUT,
  Tonemap); red paving -> the 3-layer tiled shader f0aaf6ee2b3884dc; a building -> ec3734a655430424 (Diffuse,
  Specular, AO, LightMap 128 RGBA8, Normal BC1, Reflection).
- The road shader rewritten in HLSL from its disassembly (inputs: uv TEXCOORD0 / detail uv TEXCOORD6 packed in v1,
  v2 = the geometric normal, v3 = tangent + handedness, v4 = clip position (planar reflection), v5 = puddle uv + wetness,
  v6 = eye - point, v7 = baked light rgb, .w -> albedo alpha) + parallax occlusion mapping: cotangent frame from the
  derivatives of -v6 and the uvs -> texture units per metre; up to 24 steps (fewer face on) + a linear refinement;
  depth = (1 - RoadDepth.g) x ParallaxDepthCm; every texture read with SampleGrad at the shifted uv (detail uv by its own
  gradients); fades out towards ParallaxFadeM. Swapped in at PSSetShader (b10 constants) when the game binds its shader;
  used only if its input signature matches the game's register for register.
- Harness ltest/pomtest.cpp (the game's bytecode taken from SHADERS1.BNDL by its fingerprint): parallax off, ours vs
  the game's on random inputs: outputs 0-2 identical, 3 within 0.000035; on: changes, no NaN.
- Params 129-131 (Parallax, ParallaxDepthCm 3, ParallaxFadeM 30) in MATERIALS.

## 1.15 (2026-10-15): parallax: no grooves at the texture's repeats
- User: parallax looks great, but straight grooves cross the slabs in a regular grid. The pavement's RoadDepth (512^2)
  has a black frame: columns / rows 0 and last fully black (mean 0.10-0.14), the next ones half (0.42-0.54); the 1024x
  2048 road one has none. Each repeat of the texture became a groove.
- In the march, h is pulled to 1 (the surface) within 3 texels of the texture's edge (frac(uv), GetDimensions); the
  game's own reads (puddles) unchanged. Harness pomtest: off = the game's (unchanged), on: no NaN.

## 1.16 (2026-10-15): parallax on buildings, heights made from normal maps (matheight.inc)
- The building shader ec3734a655430424 rewritten from its disassembly (Diffuse x v3 x material colour, Specular x/y,
  AO and LightMap on uv2, Normal BC1 with T = v4 / B = v5 / N = v2, reflection cube at gloss x 5.5, the 3D LUT) +
  parallax by a height map at t10 (s10 wrap, b10 constants: on / off per draw).
- Heights: for each normal map drawn with it (by its texture hash): replace\<hash>_height.dds, else
  height_auto\<hash>.dds from an earlier run, else made: the normal map read back at Present (largest level <= 512,
  BC1 / BC3 / RGBA8 decoded on the CPU), Frankot-Chellappa on a worker thread (radix-2 FFT, p = -nx/nz, q = -ny/nz,
  H = -i (wu P + wv Q) / (wu^2 + wv^2), 2nd..98th percentile -> 0..1), two channels (R: green = +slope along v, G: -v),
  R8G8 with box mips, saved as DDS. The shader takes R when the game's bitangent v5 points the way v grows on the
  surface (cotangent frame), else G. One normal map at a time; parallax off for a draw until its map is ready.
- Param 132 ParallaxWallsCm (1.5). Harness ltest/bldtest.cpp: staggered bricks -> normal map -> heights: correlation
  0.998 (the right channel), -0.497 (the other); shader off = the game's (0, 0, 1e-6, 3e-4).

## 1.17 (2026-10-16): world editor, phase 1: hide / move / turn / scale the game's own objects (worldedit.inc)
- Plan for the whole decor work: DECOR_PLAN.md. This step: visual only (collision stays where it was).
- Object identity: the world matrix of the draw ("world" in the vertex shader's constants: VsInfo wSlot / wOff, now
  with its type and a previous-frame world beside it, logged as "world editor: object constants ..."). Key = position
  rounded to 1 cm + a tag of the axes (1/256), FNV-64. Identity at the origin = the baked city: not editable.
- Reading it: the game refills shared dynamic constant buffers with Map(WRITE_DISCARD) per draw. While the editor is
  open or an edit exists, a buffer seen bound at a world slot is held (AddRef) and its Maps are redirected to a CPU copy
  of ours, memcpy'd into the driver's memory at Unmap (UpdateSubresource copied too). Orphans (only our reference left)
  dropped every 10 s. Off: no cost but a pointer test.
- Hidden: the draw is skipped before BeforeDraw (every pass: picture, the game's shadow map, our shadow replays,
  reflections). Moved: drawn with a pooled constant buffer of ours (the game's data with world and previous world
  changed: scaled, turned about the vertical through its origin, moved), also at pixel-shader slots holding the same
  buffer; the game's put back after AfterDraw (so our shadow replays draw it moved too).
- Editor (Shift+E): the cross at the centre; LMB = pick (one frame: every G-buffer draw with a key writes its number
  into an R32F ID target, depth LESS_EQUAL, the centre read back 2 frames later); the selected object's G-buffer draws
  replayed into a mask (R seen, G x-ray, B hidden ones while shown) -> outline over the picture at Present, with the
  gizmo (arrows X/Y/Z + ring, dragged like the lamp editor; NFSLight_MouseCaptured covers it). Del hide/show, +/- scale,
  [ ] turn 15 deg (Shift 1), Backspace reset, Ins show hidden, Home edits on/off, Ctrl+Z/Y. NFSLight_world.txt saved at
  every change ("obj x y z tag hide | move dx dy dz turn deg scale s").
- The camera record is read back while the editor is open (lamps condition), g_lampMsg 128 -> 256.
- Harness ltest/wetest.cpp (a shared dynamic CB like the game's, through the hooked Map / Unmap / DrawIndexed /
  CreateVertexShader): 20 checks pass (keys, identity left out, picture unchanged with copies on, hide, move + turn
  45 deg, pick at the centre, mask, buffer put back, save / load, undo / redo, edits off, game-moved object = new key,
  copies off with no edits).
- Not verified yet: the cost in game (per-draw state reads at ~5600 draws a frame, logged as "world editor (per
  frame ...) CPU x ms"), which props have a world matrix of their own, the matrix layout in game (logged).

## 1.17b / 1.17c (2026-10-16): matrices with a used w column; effects off while editing
- In game ~4400 of ~4900 object matrices failed the (w = 0 0 0 1) test: the game keeps other data in the w column.
  Now rows 0-2 = axes and row 3 = position whatever w holds (w left as it is). User: picking works, buildings too.
- Shift+E switches NFSLight's effects off (restored on leaving; Y turns them on). The camera then comes from the main
  pass's ViewProjectionModified (copied like the object constants; rows x, y, depth + z/w scale and bias): eye where
  w = 0, the view rays from depth where w = 1. G-buffer recognised without our sun pass: 3+ targets of full size.

## 1.18 (2026-10-16): pieces of one model (the developers merged several props into one mesh)
- MMB picks one piece. The model's index / vertex ranges are read back once (3 a frame, never waiting), split into
  islands (union-find on indices + vertices at the same place: equal position bytes), each with a box (the model's own
  units: float3/4, half4, snorm16x4 x 10 as the car shaders) and runs of triangles in a row.
- Part edit: key = the model's key + its box (mm); applies to every piece of the model (all its draws) whose box centre
  is in the part's box (+2 % +1 cm). A model with part edits is drawn as groups of runs, each through the whole draw
  path (BeforeDraw / AfterDraw: our shadow replays too) with its own matrix: the model's edit, then the part's about
  the part's centre. Saved as "obj x y z tag part minx miny minz maxx maxy maxz ...".
- Harness wetest: + two-piece mesh: MMB pick -> the piece, hide one piece, move + turn one piece, mask of the piece only,
  gizmo at its centre, save / load, undo: all pass (29 checks).

## 1.18b (2026-10-16): pieces of triangle-strip models; the piece pick stays on its object
- In game every prop mesh was a triangle strip (topology 5, R16 indices, R32G32B32 positions): 1.18 only split lists,
  so MMB fell back to the whole model. Now strips too: the triangle at index k uses k..k+2, stitch (degenerate) and
  cut triangles left out; a run of a piece starts on the same parity since the last cut (one index earlier if needed:
  that triangle is a stitch). Harness: a 2-piece stitched strip, the second piece starting on an odd index, back faces
  culled: moved and hidden pieces draw right (31 checks pass). (Harness note: the mesh read-backs need the GPU to
  finish frames: an event query per frame stands in for Present.)
- While a model's pieces are being read, the pick is repeated only for that model: if the cross is on another object
  then, it stops ("the cross left the object") instead of selecting whatever is under the cross. A model that cannot
  be split says so at once.

## 1.19 (2026-10-16): several selected; the editor on Ctrl+J
- Ctrl+LMB / Ctrl+MMB add an object / a piece to the selection or take it out (like a file manager); plain LMB / MMB
  select one. The set (up to 64) is outlined; the gizmo sits at its centre (pivots averaged). Move: the same offset for
  all; ring and [ ]: every one turned by the same angle, its pivot carried round the centre; +/-: scaled about the
  centre; Del hides all (or shows all if the last picked was hidden); Backspace resets all.
- Undo by action: every step of one action (all objects) has one group number; Ctrl+Z / Ctrl+Y take a whole group.
- The editor toggles with Ctrl+J (was Shift+E). J alone still resets the menu's parameter (the Ctrl+J press is taken
  first, so J's own handler sees no new press).
- Harness: + two selected turned 180 deg about their centre swap places, both hidden, one undo per action (39 checks).

## 1.20 (2026-10-16): a bench that sees GPU load under a frame cap; viewpoints through DroneCam (BENCH_PLAN.md)
- Found (ltest/busytest.cpp, RTX 3080, 32-bit): the kernel's per-process GPU running time (D3DKMTQueryStatistics
  PROCESS_NODE, node 0 = 3D) = PDH GPU Engine utilisation = Task Manager, but it under-reports timestamped work
  (24-60 % at full load, depends on the shader). Kept as a relative column only.
- The old bench compared the GPU frame span: in a CPU-limited / capped game the draws arrive over the whole frame,
  the GPU idles in between, the span stays ~ the frame (harness: 16.60 vs 16.60 ms for a 0.4 ms effect).
- New (bench.inc rewritten): vsync off while benching (Present sync 0); GPU-bound = the CPU waits in Present (median
  of the last 16 frames > 1 ms); if not, a padding pass (fixed GPU work: N draws of a sin/cos loop over 1024^2, own
  timestamp bracket PR_PAD) grows until it is, + 15 %. GPU frame per frame = span - the padding's own time. Windows
  on/off/off/on/on/off (20 settle + 60 measured frames), medians; noise = spread between windows; a window whose game
  draw count strays > 4 % from the effect's first is redone (2x). Registry of 41 effects; CSV per run
  (NFSLight_bench_<date>.csv); the cost (or "under the noise") next to the setting in the menu.
- Harness ltest/bench2test.cpp: a CPU-limited 60 FPS "game" (4 bursts of draws per frame) with a known effect:
  old measure -0.00 ms; new bench +0.60 / +0.59 / +0.31 ms against the effect's own timestamps 0.43 / 0.37 / 0.27
  (noise 0.08 / 0.01 / 0.16): PASS within 0.25 ms.
- Viewpoints: Ctrl+Shift+B stores DroneCam's free-camera pose (NFSLight_bench_points.txt); Ctrl+B runs every effect
  at every viewpoint, DroneCam holding the camera (input ignored), 150 frames to stream in at each.
- DroneCam (version.dll): exports DroneCam_GetFreeCam / DroneCam_SetFreeCam (queued, done on the game's camera update)
  / DroneCam_HoldFreeCam. Backups: _DroneCam/dronecam_v22.cpp.bak, version_v22_installed.dll.bak.
- Not yet: per-hook CPU timers, VRAM / address space per effect, repeat mode for our passes, the HTML report.

## 1.21 (2026-10-16): cached sun shadows; the rain map no longer rebuilt all the time
- Bench (2 viewpoints, 5120x1440, RTX 3080): the whole mod +38..44 ms of GPU over the game's ~9 ms; our sun maps
  +20..22 ms (of them the far cascades at ShadowFarBudget 400: +13 ms); "wet ground" +7 ms (6 of them the rain map:
  900 replays a frame all the time, with the game's pixel shaders); lamps +5..10; our sky +3.6..4.4; reflections
  +1.3..2.2; relief +0.1..1.3; AO, contact, haze, shafts, parallax, puddles, PBR: under the noise.
- Sun shadows (sunshadow.inc): static caching. Cascade 0 (13.5 m) every frame. 1, 2, 3 keep their still part: drawn
  again only when the camera has gone a quarter of the cascade, the sun has moved 0.35 deg, or every 90 / 240 / 480
  frames; into a spare map, a share of the draws a frame (the build spread over ~6 / 12 / 20 frames, at most
  ShadowFarBudget a frame), then swapped. Cascade 1 also has a cars map, cleared and drawn every frame (the cars of the
  game's two near cascades); the shader takes the nearer of kept / cars (t64 / t65, NFSLight.hlsl SsDepth / SsCmp).
  Cascade 0 is now its own N x N map (was half of a 2N x N atlas). Packs: 5 views (0..3 + 4 = cascade 1 in use).
- Found: on the user's NVIDIA driver, CopyResource from one depth texture (R32_TYPELESS, DSV + SRV) into another
  gives zeros (into a staging texture it works): the "copy kept + draw cars on it" plan replaced by two maps.
- Rain map (rain.inc): after it is made or moved, 900 draws a frame only until every draw went in once; then 40 a
  frame. Draws whose pixel shader cuts nothing out go in depth only.
- Labels: LampResolution = "at half resolution (faster)" (1 = half: the bench showed it saves 2.9..5.7 ms);
  ShadowFarBudget = the most objects a frame while a kept map is redrawn.
- Harness shadowtest: depths in all cascades within 1-22 cm, penumbra 8.0 / 1.8 cm (sun x1), no acne: the same as 1.20
  (the 1.20 sources built side by side: identical numbers). wetest all passed.
- To check in game: the bench again (expect the shadow pass and wet ground far lower), shadows while driving (a kept
  map redrawn every ~11 m at 45 m half size: does anything pop?), knocked props near the camera.

## 1.22 (2026-10-16): kept shadow maps that know their objects
- Bench of 1.21: the whole mod 44 -> 24 ms (point 1), 38 -> 20 (point 2); our sun maps 20-22 -> 5-7 ms; wet ground
  7 -> 1.3-2.3 ms. But (user): moving things in the city updated "once in five years", shadows came in late when the
  camera turned, the shadows should follow the sun. Cause: 1.21 drew the kept maps again only on a camera move, a sun
  step or every 3-16 s, from the game's view-fitted cascades (what came into view waited for the next rebuild).
- Now every shadow draw is an instance: its mesh (index / vertex buffer, range) + its world matrix from our copy of
  the game's constants (WeInstKey, worldedit.inc; the copies are on whenever our sun maps are: g_cbsCopy, separate
  from the editor's per-draw work g_cbsOn). An instance seen in two frames running is still: drawn once into each
  kept map it reaches (a set of instance keys per map), the frame it is first still. Not still (new, or moving: a new
  matrix every frame): drawn every frame into a moving-things map (cascades 1 and 2, t65 / t66; the cars there too),
  never into the kept maps. Instances first seen and not seen again the next frame = moving things: the kept maps
  built before that are drawn again (at most every 90 / 90 / 240 frames) so the old image of something that started
  moving goes. Kept maps are also drawn again when the camera has gone a quarter of the cascade, the sun has moved
  0.1 / 0.2 / 0.35 deg (cascade 1 / 2 / 3: steps too small to see), or every 1800 frames: into the spare map, the
  still instances as they come, swapped when a frame added nothing (or after 45 frames). At most ShadowFarBudget
  kept-map draws a frame.
- Harness ltest/shadowtest2.cpp (each caster with its own world matrix through the hooked UpdateSubresource): still
  box in kept 1; a box moving 0.3 m a frame not in kept 1 but in the moving-things map at its place of this frame
  (not where it was); a box appearing at frame 10 in kept 1 by frame 15 with no rebuild; the mover in kept 1 once
  it stands (and gone from the moving map); the box that left at frames 41-60: old image gone after the rebuild, in
  kept 1 and 2 where it stands: all 12 checks pass. shadowtest: depths, penumbra 8.0 / 1.8 cm, no acne as before.

## 1.23 / 1.24 (2026-10-16): day / night switch; caster culling by mesh radius
- User: shadow trails and pop-in still seen (deferred: TODO); asks for 45-50 FPS from ~30. Log at night (1.22): GPU
  frame 50 ms; shadow pass (game + ours) 10.6-12.9 ms (ours: near 251 draws, far ~35 a frame), lamps 6.3, snap 4.2
  (half-res copy of the scene + removing our lamp light from it + reflections 1.4 + the lamp finder), half-res 4.3,
  mask 1.4, look 1.0. Game cascades: 18 / 100 / 450 m across.
- 1.23: the key light's colour is read back with its direction (SsNote / SsReadBack): day = saturate((lum - 0.5) / 2)
  as in the shaders. NIGHT (day < 0.05 for 60 frames, leaves at > 0.15): our sun maps not drawn (the game's moon
  shadows; SsMaskPass falls back), the constant-buffer copies off, kept maps invalidated on leaving the night. FULL
  DAY (> 0.97 for 60 frames, leaves at < 0.9): the lamps off (g_lampUse 0: lamp pass, the snapshot's lamp removal,
  glow, lamp shadow maps; the lamp finder too) unless "Map lamps by day" / the editor / view 24. Logged:
  "day / night: key light ...".
- 1.24: caster culling. Each mesh's radius around its origin read back once (indices, then the vertices used;
  float3/4, half4, snorm16x4 x10; 4 a frame, never waiting); a placed instance (world matrix: its position and largest
  axis) whose projection across the sun misses a cascade's square by more than its radius x 1.05 + 0.5 m is not drawn
  into it (exact for a shadow map: unbounded along the sun). The game's middle cascade (100 m) had all its ~200 draws
  drawn into our 27 m cascade 0 every frame. Stats line: "out of reach N; meshes known M".
- Harness shadowtest2 + a box 160 m off: radii known by frame 20, culled from the cascades it cannot reach, the
  still boxes still drawn; all 15 checks pass. shadowtest unchanged (depths, penumbra 8.0 / 1.8 cm, no acne).

## 1.24b (2026-10-16): the day / night state no longer sticks in "night"
- User: by day the sun light was gone with the mod (screenshots), night fine; FPS a little better. Log: day detected
  (key light 6.38, FULL DAY), F8 (HD Effects time) to night: key light 0.01 -> NIGHT; then never back: the key light was
  read back only in our shadow pass (SsFrameStart), which does not run at night. Now SsNote (the sun pass, every frame,
  also with SunShadowMaps off) reads back too.
- Same log, by day before it stuck: FPS 43.1, GPU frame 22.1 ms, mod 9.6 ms (lamps 0, shadow pass small, culled 1143
  draws a frame). At night: FPS 28-29, GPU 33 ms: lamps 4.1, snap 3.0-3.3, half-res 2.6, look 1.1, reflections 1.3.
- The missing sun may also be our clouds: with "cloud shadows (overcast at full cover)" 1.0 a cloud over the sun takes
  the direct light away (the user had asked for that); to check with "Sky: ours" off.

## 1.25 (2026-10-17): the sun shadow mask at half resolution
- The game's device is feature level 10.x (shader model 4.0): no compute shaders; everything stays pixel passes.
- Log of 1.24b by day (mod toggled on / off, so rough): mask 0.93-1.33 ms, shadow pass (game + ours) 1.7-3.5, half-res
  2.8-4.0, sun 2.1-3.5, snap 1.3-2.2, reflections 1.0-1.7. At night: lamps 9-14.7 ms, half-res 2.1, snap 2.3-2.8.
- PS_SunMask and PS_SunTemporal now run at half the frame's resolution: texel p stands for the frame's pixel 2p (its
  depth and normal; the view ray moved there along its derivatives: SsPix). New PS_SunUp at the frame's resolution:
  averages the texels around that lie on the pixel's own surface (their point within 2 cm + 0.4% of the distance of
  its plane; N.L within 0.33: no blending across a lit / shaded corner). Pixels none of them shares a surface with
  (thinner than 2 px) and pixels between texels differing by more than 0.35 (sharp edges; SP4.x) get the full
  computation there (SunMaskAt, noise fixed in time). The sun pass reads one R16G16 target (visibility, cascade)
  instead of two RGBA16F ones. Layouts: raw = vis, caster distance, N.L, cascade; smoothed = vis, frames + 64 x
  (cascade + 1), N.L, distance (view 17 recomputes the penumbra from the distance).
- Mask targets at 5120 x 1440: 177 MB -> 74 MB.
- ShadowMaskHalf (index 133, menu SUN & SHADOWS, bench registry): 0 = the mask at full resolution (PS_SunUp copies).
- Harness ltest/shadowtest3.cpp (b37; the scene has a 1.5 cm wire and a 2 cm post): 5120 x 1440, sun x1: full mask
  2.07 + smoothing 0.36 ms -> 0.38 + 0.06 + frame-size pass 0.34 ms (0.78 total, was ~2.4). 1024 x 512: mask x N.L
  differs by > 0.1 on 0.010% of the pixels, > 0.25 on 0.004%; full computation in PS_SunUp on 0.25%; no acne; the
  penumbra widths identical; the wire's and post's lit faces 1.0. The wire's shadow on the ground (sub-pixel) is dashed
  at full resolution and a little sparser at half. Without the full-computation branch the frame-size pass is about
  as fast (0.25 vs 0.29 ms): no reason to split it. shadowtest2 (kept maps): all passed.
- Plan corrections: reflections already skip pixels with no gloss, wetness or paint (PS_SSR returns early): no cheap
  saving there. Relief already fades by pixel size (gone at 16 mm a pixel, ~40 m): limiting it more was among the
  options the user rejected as visible.
- CPU: with world edits applied (119) the editor's per-draw work runs all the time; its stat "CPU 2.1-2.6 ms" times 1
  call in 8 and divides by 600 frames where ~1000 passed: ~1.6 ms a frame on the render thread. Look at it once the
  bench shows whether frames are CPU-bound.

## 1.26 (2026-10-17): kept shadows follow the sun; the game's own shadow maps optional
- User (1.25): left ShadowMaskHalf at full (no felt difference: ~1 ms of ~30); asked whether shadows can be baked.
  Answer: not as a whole-city bake (moving sun; ~13 GB a sun position at 5 cm in a 32-bit process; cars; the soft
  filtering is per pixel and frame anyway); the kept maps already are the runtime cache. Log 1.25 (18:52): the game's
  own cascades 27 / 281 / 1042 draws a frame against our ~110; shadow pass 2.7-4.1 ms, mask 1.45 ms (full).
- User: building shadows move in jumps every few seconds, at sunset they jerk. Cause: a kept map holds the sun of when
  it was drawn and is drawn again only every 0.1 / 0.2 / 0.35 deg; at a low sun a 0.1 deg step moves a 15 m building's
  shadow tip by ~1.3 m (h x step / sin^2 elevation).
- Fix (no extra taps): SD[k] (b7) = per cascade, (its map's sun - the sun now) along that map's right and up; the mask
  reads the map off by SD x the caster distance it found (the shadow of a caster t m away moves by that). The caster
  search is centred halfway along the shift for casters up to 200 m (kSsShiftReach, SP4.z) and wider by half of it; the
  distance used is from the taps within the usual disk around the point if any (the wider disk reaches deeper into a
  roof: too far), each tap's distance from the receiver's plane under it. Cascade 0 (drawn every frame): SD 0.
- Harness ltest/shadowtest4.cpp (b38; a 15 m building, its shadow tip in kept 1 or 2; modes: sun A, sun B, A then B
  corrected / not, moving every frame corrected / not):
  - kept 1, 25 deg, +0.3 deg: corrected = the new sun's tip exactly from the first frame, no change at the swap;
    uncorrected stays put, then jumps 0.42 m at the swap.
  - kept 2, 25 deg, +0.3: corrected overshoots 0.17 m until the swap (the old and new maps rasterise the roof edge
    differently by < 1 map texel); uncorrected jumps 0.35 m.
  - 8 deg (sunset), sun moving 0.02 deg a frame: corrected tip moves 0.17-0.43 m a frame (geometry: 0.27) with its error
    steady through 4 rebuilds (kept 1) / within +-0.2 m with one held frame at a swap (kept 2); uncorrected holds then
    jumps 1.7 m (kept 1) / 5.7 m (kept 2).
- Side effect of the per-tap receiver plane: the box edge penumbra in shadowtest3 (sun x4) 9.3 -> 7.2 cm (the real-sun
  rule says 5.1); everything else in shadowtest3 unchanged; no acne.
- The game's sun motion is now logged every 10 s while it moves ("sun motion: N deg in K steps, largest ..."): if the
  game's sun itself steps, the shading steps too and the sun direction would need smoothing.
- SkipGameShadows (index 134, OFF by default, menu SUN & SHADOWS, bench registry): the game's own shadow map is read on
  screen only past our widest cascade's inner part (its outer 15% blends to it). A game shadow draw whose caster's whole
  light-space disk lies inside 80% of that (SsInsideWidest, both placements while one is drawn again; radius known) is
  drawn into ours only: the original draw is not made. Our replay (SsAfterDraw) now runs before the game's draw (hooks
  and WeSplitDraw: SsBeforeGameDraw) and returns whether to leave it out. Stats line: "the game's own draws left out".
  While it is on, every 3600 frames 30 frames look for other draws that bind the game's shadow map (outside its sun
  pass): logged "the game's sun shadow map: read by N draws a frame ..." (those would lose sun shadows near the camera).
- shadowtest2 + SkipGameShadows: the still box and a box 160 m off left out, the ground (800 m) and a box 400 m off not;
  all 17 checks pass.

## 1.27 (2026-10-17): our shadow replays recorded and drawn by map; kept maps redrawn by the sun's height
- User (1.26): better; at x5 day speed still some jerks; FPS still low; asks about modern-game levels, DLSS / FSR /
  frame generation. Log (x5 day): FPS 24, GPU 38-40 ms, shadow pass (game + ours) 11-13 ms with ours 166 near + 247-282
  far replays a frame and the kept maps drawn again 383-470 times per 900 frames; the game's sun moves ~1.1 deg/s
  smoothly ("sun motion": 230-261 steps per 10 s, largest 0.03-0.12 deg). At night the game draws no sun shadows
  (shadow pass 0.2-0.3 ms). Also found: ReShade (dxgi.dll) with the dlss5-feed32 add-on (a 64-bit host, "DLSS 5"
  NeuralUplift, upscaling off) and Lumenite_Kernel motion vectors: their GPU time is not in our profile.
- ltest/rtswitch.cpp (FL 10.0, the user's RTX 3080): every change of the bound depth target costs ~9 us of GPU time
  (pairs game -> ours -> game ~19-22 us), the same for D32 / D16, with or without SRV binding, array slices; a viewport
  change or binding the same target again costs nothing; grouped draws 0.02-0.08 us. A constant buffer copied on the GPU
  between draws costs ~4-5 us; Map(WRITE_DISCARD) per draw costs nothing. So our replays (2 switches each) were most of
  our shadow pass: ~25 us each in the game.
- Recorded replays (SsRecord / SsFlush): at the game's shadow draw the state is kept (IA, VS + the constant buffers its
  reflection says it reads: VsInfo::cbMask / srvMask from VsExamine, PS + SRVs / samplers / CBs 0-3 for cut-outs); the
  constants come from our CPU copies of the game's buffers (WeCbNow: 1 = copy, 2 = immutable or never written in two
  frames: the buffer itself) into DYNAMIC buffers of ours (pool by size). Drawn at the pass's end (the first draw not
  into the game's shadow map; also before a new frame's maps, the mask pass, Present), map after map (9 at most), the
  game's state saved and put back. Drawn at once as before: a geometry shader, a dynamic vertex / index buffer,
  constants not known yet. Stats line: "recorded R, drawn at once I, flushes F".
- Kept maps drawn again when the sun moved 2.9 deg x sin(height) (x1.25 / x1.5 for 2 / 3), within 0.1-1.5 / 0.2-2 /
  0.35-3 deg. shadowtest4 with no redraw: kept 1 at 25 deg: 0.5 deg exact, 1 deg 7 cm, 2 deg 15 cm off (uncorrected
  0.7 / 1.4 / 2.7 m); at 8 deg: 0.5 deg 0.39 m (uncorrected 6.3 m), 1 deg 1.1 m, 2 deg lost; kept 2 at 25 deg: 0.5 exact,
  1 deg 0.18 m, 2 deg 0.18 m; at 8 deg: 0.5 deg 0.56 m, 1 deg 1.39 m. A 0.3 m post's shadow kept at 2 deg (25 deg up).
- Harness: shadowtest2 (SsFlush after each frame's draws): all 17 pass, 741 replays recorded, 1 at once; shadowtest4
  (300 replays + the game's draws a frame): our pass 4.77 -> 1.93 ms (the rest: fill of the 800 m ground in the near
  map), tips identical to 1.26; shadowtest3 unchanged.
- Next: the rain map (RainAfterDraw: 40 draws a frame, 900 while filling) and lamp shadow maps (LsAfterDraw: up to
  LampShadowBudget 100 replays a frame) switch targets the same way: the same recorder for them.

## 1.28 (2026-10-18): slow movers, the sun renewed at every redraw
- User (1.27, screenshots): a turning crane leaves a smeared trail on a facade that goes at a redraw; building shadows
  still jump every 20-30 s; asks whether to replace the game's sun. Log 1.27 (x1): the game's sun moves smoothly
  (0.04 deg/s, a step every frame, largest 0.006 deg): no need to replace it. Kept maps drawn again 21-24 times per 900
  frames, nearly all for "a moving thing seen" (the 90 / 240-frame rule; the traffic's keys change every frame), and
  those redraws kept the map's old sun (only a 1.5-3 deg step renewed it: every 35-70 s, where what SD leaves showed).
  Recorded replays 88-98 a frame, at once 29-32 (= the cut-outs: their small pixel-shader constant buffers, < 64 bytes,
  were not copied). Shadow pass 2.1-2.7 ms (1.26 at x5: 11-13). Copies OK in game ("no copy yet 0").
- (1) Every kept-map redraw takes the sun of now (bSun = g_ssSun). (2) Cars' new keys no longer count as moving things
  (SsInstOf watch flag). (3) Slow movers: the key's rotation tag is 1/256 per axis entry (~0.2 deg), so a crane turning
  slowly gets a new key every few frames, each still for 2+ frames: kept every time = the trail. SsMoverCheck: when an
  instance of a mesh first becomes still, if the mesh's previous still instance lies within 1.5 x its radius + 1 m and
  was not drawn for 2 frames (the same object, moved), that spot (mesh, place, radius) becomes a mover (g_ssMover, 64 at
  most, logged "a slow mover"): its instances there are moving things (moving maps 1 / 2 every frame; none past 90 m)
  and the kept maps are drawn again. (4) CbShadow copies buffers from 16 bytes (was 64): cut-outs can be recorded.
  (5) Log: kept maps drawn again for: a start / the camera / the sun / a moving thing / age; slow movers N.
- Harness shadowtest2 + a 10 m jib turning 3 deg every 5 frames from frame 70, standing from 135: found as a slow mover,
  in the moving-things map at its angle now, no trail in kept 1 (its 0 and 18 deg images gone), none after it stands;
  no false mover among the other boxes; all 21 checks pass. (Harness gotcha: at frame 100 the D3D runtime gave the
  immediate context another method table, so the harness's vtable hook of UpdateSubresource fell off and the copies froze:
  every object read the jib's matrix. The mod hooks the function itself (inline), the game log shows copies fine; the
  harness now feeds the copy itself when its vtable hook is gone.) shadowtest4 unchanged.

## 1.29 (2026-10-18): our sun's path (an arc) and a soft sunrise / sunset
- User: the sun moves strangely, "like a figure eight", not rising on one side and setting on the other; in the morning
  there is no sunlight, then it comes on at once: wants a setting for that.
- Logs: the game's key light (hourly keyframes, LightRig) rises and sets on the same side: towards the light at azimuth
  -72 at sunrise, -55 at 19 deg up, -36 near noon, then back (-47 at 32 deg up after noon): a loop. The sky's glow has
  its own direction (KeyLightDir of the sky draw), ~15 deg lower than the light's (16:42: sky 34.8 deg, light 50 deg).
  The key light switches between the moon (~0.4) and the sun (~6) at once.
- SunPath (P_SUN_PATH 135, default on; sunshadow.inc SunPathStep, from each readback of the game's key light): ours
  keeps the game's height and puts it on an arc: theta from sin(theta) sin(M) = sin(height), before / after noon by the
  height's trend; towards the sun = cos(theta) E + sin(theta) (sin M up + cos M H). Noon (M, azimuth A) and the side it
  rises on are learned from the game each day (NFSLight_sun.txt; defaults 50 deg at -40, rising on the - side); the noon
  in use moves to a newly learned one at 3 deg/s, theta at 4x the game's motion + 2 deg/s (no jumps). Our maps follow
  ours (g_ssSun = ours while w > 0); the sun pass adds our light (P18 / P19: direction, share w, colour) and the game's
  key light for the rest (U x gOrig x F x (1 - w)), the game's term still removed exactly; the air, bounce light,
  half-res contact shadows, relief, cloud shadows use ours (SunLd / SunLc when w >= 0.5); the sky's glow and clouds:
  our side at the game's sky height (SK10); the cloud-shadow map: our light's direction. Past our maps no shadow with
  our sun (SP4.w; the game's map points elsewhere).
- SunFadeDeg (136, default 3): after the moon->sun switch our light comes in over that many degrees of the sun's travel
  (x1 time: ~75 s), and after the sun->moon switch it goes out over as many (going on along its arc with the day's last
  colour); a switch above 15 deg up (an F8 time jump) or the game's start: at once; time standing: 1.5 s. Our maps stay
  on while our sun shines (night = the game's night and w 0).
- Log every 10 s: "sun path: the game's .. up, azimuth ..; ours .. up, azimuth .. (theta, rising / setting, share)";
  "the game's noon learned".
- Harness ltest/suntest.cpp (b40): every shader compiles (main + sky); a simulated game sun (height 50 sin, a loop of
  azimuths -72 -> -36 -> -80, the moon at once below 4 deg): ours rises at -127, noon 50 deg at -40, sets at +48; height
  = the game's within 0.04 deg; azimuth step a frame <= 0.07 deg; light in over 1.7 s (3 deg at the sim's speed). With a
  wrong prior noon (game 38 deg at -20): day 1 no jump (<= 0.14 deg a frame) but up to 4 deg off in height around noon,
  day 2 exact (learned). shadowtest2 / 3 unchanged.

## 1.30 (2026-10-18): the time of day from the game's sun's azimuth; moon shadows ours; the corona follows
- User (1.29, screenshots): still a loop; our shadows froze; the game's shadows came back at times and in the evening; a
  "hole" of light in a building; asks to move the sun's halo and flare too.
- Log of 1.29 (17:19-17:26): the game's key light is not a sun's path: it comes on at once ~31 deg up at azimuth -7
  (towards it), wavers to 28 deg / -2.6, climbs to 50 deg at -38 (noon), goes down to 28 deg at -72 and goes off at once
  (the moon). It never reaches the horizon; its azimuth goes one way (-5 -> -72). 1.29 took the dawn dip for noon
  (learned M = 31.2): our theta stuck at 90 while the game's sun climbed (shadows froze), azimuths far off; at night our
  maps were off (NIGHT): the game's moon shadows (blocky) from dusk on; the "hole": kept maps shifted by SD for a sun
  that had turned tens of degrees in a moment.
- SunPath 1.30: time of day p = where the game's azimuth is between its day's first and last (learned when a whole day
  went by: began and ended low, noon between; NFSLight_sun.txt "azOn azOff M A"; defaults -5 / -72 / 50 / -38); p only
  goes on within a day (the dawn waver), at most 4x the game's motion + 1% of a day a second (at once after a jump, the
  night, the start). Ours: theta = 180 p on the arc (rises at A + 90 on the day's start side, noon at A, sets opposite):
  from the horizon when the game's sun comes on to the horizon when it goes off. Its light: the game's key colour x
  smoothstep(0, SunRiseDeg (new name, default 8), its height). The game's key light: taken out by day (it points
  elsewhere), kept at night (the moon), its share g 1 <-> 0 over 1.5 s at the switches (P20.x). Our maps: ours while
  g < 0.5, else the game's (the moon); a turn of over 3 deg at once: the kept maps drawn anew.
- MoonShadows (137, default on): our maps stay on at night for the moon (NIGHT no longer); the sun pass's moon term
  uses them (P20.y). Off: the game's moon shadows as before.
- The game's sun corona: shaders with SunCoronaDirection (gObjectCB: the fog / sky scattering shaders and some
  materials; their VS puts the sun on screen with it) found by reflection at creation (CoronaNote, PS and VS); by day
  their constant buffer is swapped at the draw for a copy with our direction (its sign against the game's key light
  learned and logged once: "sun corona: ..."). The sky's glow and cloud light: ours by day (full direction).
  The lens flare (rings, ghosts) not found by name in the shader dumps: if it is image-based (from the bright sun) it
  follows; else to find with a frame trace.
- Harness ltest/suntest.cpp (b40) with a game sun like the log (on at 31 deg / -6.7, waver, 50 at noon, off at 28 /
  -72, the moon elsewhere): day 2: ours 0.0 deg up when the game's comes on and goes off (light 0), at the game's noon
  50.0 at -39.3, rises at +51 and sets at -129, azimuth step <= 0.15 deg a frame, the game's key light out / in over
  1.5 s; learned -6.7 / -72 / 50 / -39.4. Shaders (main + sky) compile; shadowtest2 / 3 unchanged.


## 1.31 (2026-10-18): the game's day tracked as a phase all day and night; our moon; the sun's flare follows ours
- User (1.30, Shift+U on the flare): at dawn ours first follows the game's sun, then jumps to where it should rise; it
  sets too early, then "the game's sun" again (shadows in strange places); the horizon very dark in the evening:
  "dawn the game's, morning ours, evening the game's".
- Log of 1.30 (17:44-17:54, 64 lines "sun path" every 10 s): one game day 144.5 s (the user's day speed). The game's key
  light swings to and fro on one line: azimuth = c + r cos(2 pi phase) within 0.65 deg rms over 4 days, day and night
  (c -37.65, r 35.05: -2.6 at its dawn turn = phase 0, -72.7 at its dusk turn = 0.5); height 28 at both turns, by day
  28 + 22 sin^2 (going down in azimuth), at night 28 + 12 sin^2 (coming back). The sun comes on at phase ~0.92 (still
  coming back, before the dawn turn) and goes off at ~0.535 (just after the dusk turn); the moon's light 0.22 after
  dusk, 0.01 at midnight, 0.27-0.48 before dawn. 1.30 read the time of day from the azimuth by day only: it stood still
  at the turns (at dawn 20 s with no key light at all; at dusk ours set 10-20 s before the game's went off), and at night
  our maps took the game's moon (its line back).
- SunPath 1.31 (sunshadow.inc): the phase is a straight line in the game's time (the time stops while the game's light
  stands still: paused), fitted by weighted least squares to the last ~1.3 days of phase samples (one per 1/200 day)
  where the azimuth moves well (two phases fit an azimuth: the one nearer the line) and to the turns' marks (the
  azimuth 2.5 deg back from its furthest: a known phase). Over a whole day the azimuth's own unevenness no longer bends
  the line: the phase is off by a constant at most, and the switches are learned in the same phase. A new day speed:
  the latest 3% of a day off the line by > 1.5%: the older samples dropped; a turn's mark far from the line: the line
  anew from it. The phase shown follows the line, never back, at most 3x ahead, and never further past the next turn
  than the azimuth came back from it (+0.4 deg): the line cannot run away at an old speed where the azimuth tells
  nothing. Start / jump: the branch from the way the azimuth goes (by day / night until it moved 0.2 deg).
- Learned (NFSLight_sun.txt, format "v2 c r on off M day k"; the 1.30 file is not read): the swing from the turns'
  azimuths (where the azimuth turns back by 2.5 deg, whatever the phase; the first turn corrects its side at once), the
  sun's on / off phases (the first switch taken whole, later halfway; far ones only when seen at the same place 3 times),
  the noon height, the day's length. Learned values are eased in (0.2% of a day / 0.2 deg a second: no jump).
- Ours: the sun rises at the horizon when the game's comes on, sets when it goes off (arc: rises at c + 90 on the side
  the game's day starts, noon at c, height M). MoonShadows (on) is now our moon: the same arc through the night (rises
  as the sun sets, sets as it rises), the game's moon colour, faded in / out over SunRiseDeg like the sun; the game's key
  light is out all the time (P20.x 0) and our maps follow ours always (at least 1 deg up). Off: the game's moon with its
  own shadows at night (1.30).
- Steps of the game's light: by the cross product (acos of a float dot cannot tell steps under ~0.02 deg: the game's
  light moves 0.017 deg a frame at x5, so they read as "still").
- The sun's flare (the frame trace with it on): after the bloom, at half size, glare quads (dump VS 1766 / 1803: screen
  quads lit by (k^3 x 4) GenericVector4_B, k from -dot(GenericVector4_A, the view ray / normal): A the sun's direction in
  view space) times the sun's visibility (PS 2445 / 3094 sample a 32x32 target at GenericVector4_D, filled before by PS
  2493 / VS 1813: a point at the sun in view space through `projection`, 7x7 depth samples). These shaders known by
  their DXBC checksums at creation (FlareNote); for their draws (by day, ours on) the VS is ours: the same maths, the
  game's light (key light or corona, matched in view or world space; else as it was) replaced by ours in view space from
  this frame's camera record (t0), the glare faded with ours (FlareCB at b5). Logged once: the game's A against its key
  light / corona in view space (to confirm the convention in game).
- Harness ltest/suntest2.cpp (b41): our flare VSs against the game's (dump DXBC): signatures equal by reflection, outputs
  by stream output equal (max 1.3e-7) for: ours = the game's; ours elsewhere (= the game's VS fed ours, faded); the game's
  light its corona; something else (unchanged). SunPath against a game like the log, 2400 s: day speed 150 s, 4x slower
  for 900 s, a time jump of +0.3, a 15 s pause at the dusk turn; truth not the defaults. Mode 0: phase within 0.0054 of a
  day (settled), 0.0001 at the slow speed, through the pause 0.0005; ours at the horizon at all 15 switches; largest
  step 0.074 deg a frame; learned swing / switches exact. Mode 1 (the user's values, azimuth 1 deg off a sine, the moon
  1.2 deg off the sun's line): 0.0093; worst while a new speed is found 0.077 (the change at the dusk turn); all 16
  switches at the horizon. shadowtest2 unchanged.

## 1.32 (2026-10-18): our own sun by a clock
- User (1.31): the sun still switches on at once where the game's starts going one way; asks for our own motion to
  control: when it rises and sets, its angle, and a clock to set the evening / night / dawn by.
- Found: the user's ini has SunShadowMaps=0 (since before 1.31): SunPath needs our maps (g_sunActive), so in that session
  ours was off entirely (no "sun path: ... deg up" lines) and the game's sun was seen. The menu now says so on SunPath.
- The clock (hours): 06:00 when the game's sun comes on (its learned phase), + ClockShift (138). Ours by it: the sun from
  SunriseHour (139, 06:00) to SunsetHour (140, 20:45: the game's goes off at ~20:46) on its arc, noon height SunNoonDeg
  (141, 50), the arc turned SunTurnDeg (142) from across the game's line; the moon (MoonShadows) through the night on
  the same arc, its light MoonLight (143..144: 0.25, bluish). SunColour (143) on: the game's noon sun's colour (learned at
  its phase 0.15-0.35) through the air of our sun's height (Kasten-Young air mass, extinction 0.10 / 0.18 / 0.36 a unit
  of air mass past the noon's: orange and dim low); off: the game's key colour now. The menu shows the clock (now HH:MM,
  our sun's height) on ClockShift and the hours as HH:MM; the log line every 10 s starts with the time.
- Harness suntest2 (sunset set to the model's switch): mode 0 all pass; mode 1 two sunsets with ours 2.5 / 4 deg up at
  the game's switch (the hours are fixed now; the line's phase is off the game's by a constant in an uneven game).

## 1.33 (2026-10-18): the sky follows the sun (not the moon); a dimmer moon; the night's brightness; the paths shown
- User (1.32, a screenshot at 23:00, sunset set 21:45): dark, the moon should shine, but first a sun-like warm light,
  then it fades; the moon even at 0.05 too strong; the night should be much darker; split the sun and the shadows in
  the menu, clearer items; a debug showing our sun's path and the game's, the sun as a dot.
- Cause: the sky's glow and cloud light (SK10), the corona and the flare took our moon's direction at night; the
  game's sky still in its twilight (its sun colour orange and bright) drew a sun at the moon. Now they take g_sunSky:
  our sun all the time, at night on its circle under the horizon (the glow where it set, gone deeper into the night);
  the flare's light is the sun's (0 at night).
- MoonLight: a tenth of before. NightEV (146, -1 EV): added to the LOOK exposure by the night's share g_nightF (0 with
  our sun at the horizon, 1 from 8 deg under it; with ours off: 1 - the game's day).
- SunPathDebug (145): drawn at Present (also with the menu hidden) with the overlay's VS: on the screen the paths as dots
  (ours over the day: yellow the sun, pale blue the moon; the game's light seen over its day, 128 bins of its phase: cyan
  the sun, magenta the moon; now: big dots), the horizon grey; a sky map bottom right (the rim the horizon, the middle
  overhead, up where the camera looks). The camera from this frame's camera record.
- Menu: SUN & MOON (sun / moon on, the clock, sunrise, sunset, noon height, turn, fade, strength, warmth, colour, night
  light, moon brightness, night EV, debug) and SHADOWS; shorter labels; times as HH:MM.
- Harness suntest2: mode 0 all pass; the debug picture drawn offscreen (sundebug.png) looked at.

## 1.34 (2026-10-18): the sky darkens with our night; the game's disc in the debug
- User (1.33, screenshots 21:21-23:55, sunset 21:45): the sky and our clouds stay sunset-lit until ~00:00 ("night
  only at 12"); the game's moon disc lower than its path in the debug.
- Our sky draws with the game's palette (its gradient, SunColour, cloud colour), which keeps its twilight ~3 h of our
  clock past its sun going off (its night lum 0.01 only from phase ~0.69 = our ~00:30). SkyNight (147, on): with our
  sun under the horizon (g_nightF, SK9.y) the gradient goes to its luminance x (0.55, 0.65, 1) x 0.22, the game's cloud
  colour likewise (x 0.25), no sun glow, the clouds' sun light x 0.1; night = max (the city's glow, the stars).
- The game draws its sun / moon disc where its sky's light points (its corona's direction, ~15 deg under its key
  light): the debug now shows that path too (the same colours, darker) and the disc now.
- Harness suntest2: mode 0 all pass, the sky's and the debug's shaders compile.

## 1.35 (2026-10-18): the game's sun sprite gone, our discs; the sun dimmer when low; the dawn's haze
- User (1.34; sunset moved to 00:00): flares / glow of the old sun still there; the haze black for the first ~30 game
  minutes of dawn; our sun's colour should suit the set times (with sunset at 00:00 it blinds in the evening).
- Log: our flare VSs drew 26-27 thousand times (the swap works). What stayed: the game's sun / moon sprite (an effect's
  quad, PS 2623 / 2757 of the dumps, reading the sun's visibility 32x32 target at t1) and its bloom. Now not drawn while
  ours shine (known by its PS and that target, captured at the visibility draw); our sky draws our sun's disc (0.4 deg,
  warm when low, SK9.zw) and the moon's (SK11) under the clouds.
- Our sun's strength by its height like the game's (~sin(height)^1.3, SunLowDim 148), the air's extinction only as a
  tint: at 18 deg ~0.3 of noon (1.32-1.34: ~0.8).
- The haze's sky light (AirLight): with ours, max(its light, the noon sun's by our sun's height from 4 deg under the
  horizon, P20.z): low at dawn it had almost none and darkened the distance to black.
- Harness suntest2: all pass; shaders compile.

## 1.36 (2026-10-18): the haze by the clock; the moon hangs; the trace names the VS
- User (1.35): the haze is the issue: set when it comes and goes (strong at dawn, a little by day, none at night: it
  only shows as a fault in the distance); move the game's moon texture to ours (ours is "only a flashlight"); our moon
  should not cross the sky like the sun: hang somewhere, move a little over the night.
- Haze x (HazeAmount): HazeNight (152, 0) at night, rising to HazeDawn (149, 2) over the hour before sunrise, thinning to
  HazeDay (151, 0.5) over HazeDawnHours (150, 2.5 h), back to HazeNight over the hour after sunset (g_hazeMul into P4.y;
  1 with our sun off). In AIR; the dawn line shows the multiplier now.
- The moon: MoonHeightDeg (153, 35), MoonTurnDeg (154, 0: over the sun's noon), drifting MoonDriftDeg (155, 20) over the
  night the sun's way; its light in over the night's first 8%, out over its last.
- The game's moon: not the effect sprite of 1.35 (it stays at the game's place at night), not one of the screen-quad
  VSs with gOffsetXYZ (1783-2018 of the dumps: screen passes). The frame trace (Shift+U) now writes each draw's VS
  checksum (its first 4 bytes, as the dumps' DXBC, VsSumOf from the kept VS code): a trace at night with the moon in view
  will name its draw.
- Harness suntest2: all pass (the moon at 35 deg at the game's midnight; at the switches its light low).

## 1.37 (2026-10-18): the game's moon moved to ours
- The night trace (23:02, Shift+U with the moon in view; 1.36 writes the VS): just after the sky dome a textured quad of
  an effect, VS 1859 of the dumps (POSITION x worldViewProj; colour through the exposure) with PS 2561 / 3030
  (DiffuseSampler, gVfxConstants): the moon, placed in the world where the game's moon is.
- For these draws (by their DXBC checksums; our sun / moon on) the VS is ours (VS_Moon): the same, but where its middle
  (worldViewProj row 3) is on the screen in the direction of the game's moon (its sky light or key light, within 6 deg),
  the whole quad moved on the screen to our moon's place (size and look kept; from this frame's camera record); our
  moon not up: out of sight. Our sky's moon disc (1.35) no more.
- Harness suntest2: our moon VS's signatures are 1859's; by stream output: not the moon: as the game's (4.8e-7); the
  moon: the game's output moved to ours (4.8e-7); all else passes.

## 1.38 (2026-10-18): the moon matched by its corners
- User (1.37, screenshot with the debug): the moon's texture stayed at the game's disc place. Log: the moon VS was swapped
  (logged under the wrong name) but never matched: the effect's mesh is in the sky's world (its origin, worldViewProj
  row 3, is not the moon). Now each corner within 12 deg of the game's moon (its sky light or key light) moves by the
  game's moon's screen place to ours (the same shift for all: the quad kept).
- Harness: the mesh's origin mid-screen, its corners at the moon: moved to ours (4.8e-7); all pass.

## 1.39 (2026-10-19): lamp groups edited in the menu; a lamp budget; reflections reach up, our sky in them
- User: lamp shadow maps cost without a visible gain; lamps very heavy with many of them (windows); toggles per kind and
  an in-game editor (brightness, colour...); reflections cut off vertically, our sky not in them.
- Log (night): lamps 6.2 ms, air (with snapshot, reflections) 4.1, half-res 3.5 (5120x1440); 3713 map lamps. Each pixel
  walks every lamp of its tile (light, shadows, glow in the air); every lamp had the global radius.
- Lamp groups (NFSLight_lamp_presets.txt, now "name hue sat power on radius reach glow"; old lines read with on 1, 0, 0,
  glow 1): a group off is not sent; its own light radius (LampX.y: fewer tiles, less work a pixel) and reach from the
  camera; no glow in the air (LampX.z). Menu LAMP GROUPS: Group (name, lamp count) and its values (mirrored params
  156-163, written back and saved a second after a change; Shift+P reloads). LampsMax (164): lamps a frame at most.
- Reflections: the ray goes SsrRay (165, 150 m) past the point (was 40 m: walls and towers cut). SsrSky (166): our sky
  as seen in a panorama (1024 x 256 R16G16B16A16F, 2 in turn, mips; PS_SkyPano: each texel's direction found on this
  frame's screen, sky there: its colour eased in, else kept): rays off the screen or into the sky take it (rough: a
  blurred level; never seen: the seen ones' mean), hits fade to it at the screen's edges.
- Harnesses: shaders compile; suntest2, shadowtest2 pass.

## 1.39b / 1.39c (2026-10-19): the sky panorama fixed
- User (1.39): neither our sky nor the game's in the reflections. 1.39b: the panorama shown in the debug (a strip at the
  top) and its smallest level read back every 10 s ("sky panorama: N% seen"): 0% seen, all checks. PS_SkyPano worked on
  made-up data (ltest/panotest.cpp: the colour and place right), so the sky test failed in game: the passes find the sky
  by depth - t1.w == 0 within 1e-9 (bit-exact with the game's own ray); the ray rebuilt from the camera record is not
  bit-exact. 1.39c: the sky where the depth gives a distance over 15 km (or none).

## 1.40 (2026-10-19): menu groups by Shift+I/O; wetness by WetGround, roads first, no wet trees, puddles; rays through facades; near shadows whole
- User (1.39c: the sky is in the reflections now): too many settings to walk (jump by group); Wet ground wets everything
  (trees too), reflections ignore buildings / too bright (buildings white or grey in them), Puddles does nothing; near the
  camera (our red cascade) shadows missing, whole building shadows gone at times.
- Menu: Shift+I / Shift+O: the first item of the group before / after (not in the lamp editor).
- Wetness: 1.39 divided it by WetGround again (wet01 = wet / WetGround: how wet the reflections looked did not follow the
  setting), and took "up" as the larger of the geometric and the normal map's normal (leaves face up in their normal
  maps). Now: wet = WetGround x up (geometric normal where known) x open x (roads 1, other floors WetOther (167, 0.3),
  grass 0.05); puddles on their own (Puddles; were x WetGround and only on asphalt darker than 0.16-0.3: now 0.3-0.5).
- Rays: the hit test's thickness + 1.2 x the step (long rays, 150-400 m, stepped through facades and took the sky
  behind). The panorama keeps all beyond 25 m (what is around, not only the sky): rays off the screen and hits fading at
  its edges take buildings there, not the bright sky.
- Shadows: our nearest (27 m across) got only the game's nearest cascade's casters (18 m) and its middle's; a building
  only in the game's widest (450 m) casting into ours was missing. Now every caster whose shadow reaches our nearest
  (SsReaches), once a frame (SsInst.nearF). The game's widest is drawn every frame (1900-2300 draws a frame in the log).
- Harnesses: suntest2, shadowtest2, panotest pass; shaders compile.

## 1.41 (2026-10-19): the reflections' sky follows the time of day; no far road in it; smoother puddles
- User (1.40: white buildings gone, settings work; near shadows still not right: later): the sky in puddles never
  changes with the time of day; in puddles lighter "puddles far away" or clouds, moving with the camera (parallax);
  puddle edges glitchy.
- The panorama only learns where the sky is on the screen: looking down, its old (day) sky stayed. Now kept premultiplied
  and fading (~8 s half, P21.z from the frame time); the gap filled by SkyGuess (the haze's sky light as AirLight: the
  sun's by its height, the city's glow at night; brighter at the horizon).
- 1.40's "all beyond 25 m" took the far road (and its puddles) near the horizon: now buildings only over 2 deg up.
- Puddles: no colourfulness gate, a soft brightness gate 0.25-0.65 (the texels' detail made grainy edges).
- Harness: shaders compile; suntest2 passes; panotest: the panorama gets the sky right (premultiplied, a 0.35 a frame).

## 1.42 (2026-10-19): puddles stay put; mirrors sharp; fewer glints
- User (1.41): in the debug view (wetness white, puddles blue) the puddles ride with the camera; reflections in puddles
  soapy and grainy at full size; too much shine in many places.
- The apply pass (PS_SSRApply) had no camera record at t28 (the lamp finder unbinds it; the blur leaves the last frame's
  or none): world = rel + 0: the wetness, the puddles and the roofs' dry patches were laid out around the camera. The
  ray pass had the right one (the two disagreed). Now this frame's record is bound for it.
- The gloss rides on the reflections' w (dist + round(gloss x 15) x 1e5; SsrDist / SsrGloss): the smoothing leaves
  mirrors (over 0.75, puddles 0.95) unaveraged (only fireflies held to the neighbourhood's mean + 2 spread), the rest
  averaged from samples held the same way; history fetched by point (the gloss on w), less of it on mirrors.
- A wet film's Fresnel at a glancing angle goes up to its gloss, not 1 (rough wet asphalt was a mirror far off). Dry
  reflections only from a specular over 0.08 (full at 0.15; was 0.04 / 0.08): stones shone dry.
- Harness: shaders compile; suntest2, panotest pass.

## 1.43 (2026-10-19): settings to find the sparkle; highlights' anti-shimmer
- User (1.42: puddles fine now): still noise / sparkling dots in the reflections (worse, it feels); asks for settings
  or debug to find what makes it.
- Menu group NOISE / SPARKLE: SpecAA (173, 1: the highlights' lobe widened by the normal's spread from pixel to pixel,
  in the sun pass and the lamps: stones' normal maps made lone bright pixels flicker), SsrSmooth (168: none / by gloss /
  always 4x4), SsrClamp (169: bright dots cut at mean + N x spread, 0 off), SsrHistory (170: x the frames' blend),
  SsrJitter (171: rays jittered or not), WetBumps (172: the stones' relief kept in wet reflections, was 0.1 fixed),
  WetLightGloss (174: the gloss wet ground gets for the game's own lights, was 0.7; only used while DryGloss < 1).
  Constants P21.w, P22, P23; rain RN2.w.
- Harnesses: shaders compile; suntest2, shadowtest2 pass.

## 1.44 (2026-10-19): reflections sharper, no jitter by default
- User (1.43): only turning the rays' jitter off helped; the reflections are mush next to the sharp scene, as if 4x4.
- SsrJitter off by default (the per-pixel jitter's noise did not settle over frames). The smoothing: 3x3 around the
  pixel (1-2-1), for gloss under ~0.7-0.9 only (the 4x4 box from -2..1, half a pixel off, made blocks). Wet ground's
  reflections' gloss WetSharp (175, 0.8; was 0.62 fixed: a high blurred level of the scene).

## 1.45 (2026-10-19): anti-shimmer for bumpy reflections; the reflections' sky keeps its level
- User (1.44: nicer, but grain and heavy shimmer; the sky not seen is forgotten and goes white).
- PS_SSR: the normal map's spread from pixel to pixel (ddx / ddy of the G-buffer normal, before any return) x what the
  wet flattening leaves of it raises the roughness (SpecAA, now for reflections too): wet cobbles' sharp reflections
  scattered into shimmering dots; flat floors and puddles stay sharp.
- The panorama is kept divided by the sky's level at writing (P23.z: 0.02 + g_skyLum with ours on) and multiplied back
  when read: unseen sky follows the time of day in brightness; it fades over ~60 s (was 8); never seen: the seen ones'
  mean, else the guess at half (1.41's guess was far brighter than the game's dark dawn sky: white reflections).

## 1.46 (2026-10-19): the reflections' history works (FP16 overflow since 1.42); temporal AA; the sky probe
- User (1.45): still shimmer; asks whether anti-aliasing (general or the reflections', not 3x3 / 4x4) can solve it; the
  player can never look up, so the sky is never seen and never reflected right: it needs loading automatically.
- Cause found: 1.42 put the gloss on the raw reflections' w (dist + gloss step x 1e5) in an R16G16B16A16_FLOAT target:
  past FP16's 65504 for any gloss over 1/30. Since 1.42 there was no history at all (the distance test never held), no
  depth-aware smoothing and no gloss-based sharpness: why only turning the jitter off helped (1.44).
- PS_SSR writes a second target g_ssrAux (R16G16_FLOAT, t69): the gloss and the ray's length to its hit (1000: the
  sky); w is the plain distance again.
- PS_SSRBlur: temporal anti-aliasing for the reflections alone. The rays jittered anew each frame (SsrJitter on again,
  default and the user's ini); the last result taken at ~90% (x SsrHistory) from where the reflection was a frame ago:
  for gloss over 0.45 the reflected point's mirror image (surface + view direction x the ray's length), else the
  surface; a third try at the same pixel in camera space (the player's car moves with the camera). The history texel is
  accepted only if its point (its distance along the last camera record's ray) lies on this pixel's plane (geometric
  normal; tolerance 5 cm + 0.8% of the distance); resampled Catmull-Rom (5 bilinear taps) and clipped toward this
  frame's 3x3 mean within SsrClip (176, new, 1.25) x its spread. Just uncovered: half the 3x3 blur for those frames.
- Sky probe (sky.inc): our sky in a 128^2 cube with mips (t68, for PanoSky where nothing was seen of late). The game's
  dome draw replayed with a face's rows: its VS is world = v0 x cb1[11..13] + cb1[14], clip = world x cb1[7..10].xyww
  (from NFSLight_sky.txt); our copy of the VS constants (240 bytes copied from the game's on the GPU, then rows 7..14
  copied in from a per-face buffer: x / y = the face's right / up, z = w = its axis, no translation; world rows = the
  axes at 0). Per face: the cloud PS marched at once (SK5 = 1/128, SK7.yz = 0.5: no 2x2 shift, SK9.x = 0: no depth
  test), then the sky PS over it (SK9.z = 0: no sun disc), blended in at 25% (blend factor; 100% for the first fill
  of all six or when older than 120 frames); one face a frame, GenerateMips. Shown under the panorama strip in the
  sun debug view (SunPathDebug).
- Harnesses: probetest (new, b43: a replica of the dome VS; the cube's brightest texel 0.2 deg from the key light,
  100% covered, the screen's sky bit-identical with the probe on / off, a face a frame); taatest (new, b44: a mirror
  floor reflecting a striped wall: history error 0.015 against 0.150 without; by the mirror image 0.015, by the
  surface's place 0.077; a still view's noise to ~30% in 24 frames, rough too); panotest, suntest2 pass. (The D3D debug
  layer is not installed here: the copy into a constant buffer checked by the result only.)

## 1.47 (2026-10-19): the reflections' source before the game's effects; big matte puddles
- User (1.46 shots, night, wet: likes the reflections now): cars are white / grey in reflections; wants a kind of puddles
  that are big but matte (a blurred reflection), as the game's own.
- Shots zoomed: under an oncoming car the puddle shows a flat pale teal block with the headlights, not the dark car. The
  air pass (and the snapshot, the reflections' source) runs right before the post-processing, so the scene then has
  every transparent: the halos the game draws over cars' headlights (traffic headlights are emissive + sprites, no
  lights: 0.62 recon) wash the car's front pale, and the reflections showed that.
- SsrEarly (180, on): after our sun pass, at the first draw on the scene target that blends and writes no depth (not the
  game's light shaders: LightKindOf), the scene is copied (CopyResource, a full-size texture like it); the snapshot's
  reflection output (g_hrt[6], PS_Snap's SV_Target1, via t70 + P24.z) and the sharp source (t39) read the copy; the
  bounce / lamp finder's output (g_hrt[4]) stays the live scene (the finder needs the sprites). Emissive signs, lamps
  and windows are in the G-buffer's add target: in the copy. Logged: "reflections' source: the scene copied before draw
  N after the sun pass (ps, its name, vs checksum, blend)", when that shader changes or hourly. The frame trace
  (Shift+U) lines now also give "blend src*100+dest" and "dw" (depth written), and mark the draws after the copy.
- Big matte puddles: PuddleBig (177, 1), PuddleBigSize (178, 0.5), PuddleBigGloss (179, 0.6) in RainCB RN3 (after RC:
  832 bytes). BigPuddle(world): value noise 12 m / 4 m / 1.2 m, cut at lerp(0.72, 0.4, size). Wetness() puddle = max(the
  small mirrors (Puddles), the big) on road-dark floors, out pGloss (0.95 where the small ones win, else PuddleBigGloss);
  PS_SSR / PS_SSRApply gloss = lerp(WetSharp, pGloss, puddle); GlossFix (the game's lights' gloss) the same (it still
  had the 1.40 grey x asph puddle mask). Puddles' Fresnel water's (0.98 at a glancing angle) blurred or not.
- Harness early147 (new, b45): no copy without blending / on another target / with depth written / for a game light,
  a copy before the effect's draw holding the scene before it, once a frame; on 128 x 128 m of dark road (Puddles 1)
  big matte puddles 19% (patches ~5-15 m, gloss 0.62), small mirrors 22% (puddles147.png). taatest, probetest,
  panotest, suntest2 pass.

## 1.48 (2026-10-19): the reflections' copy works with the game's typeless scene; damp patches
- Log (1.47 in game): "reflections' source before the effects: no copy (5120x1440 format 1)": the scene texture is
  R32G32B32A32_TYPELESS; a view with no description fails on a typeless texture, so 1.47's SsrEarly never ran (the cars
  were still pale). The copy's view now takes the scene RTV's format (g_air.rtvFmt, as g_sceneSrv). The copy is
  R32G32B32A32: 118 MB at 5120x1440, ~0.3 ms.
- User (1.47 shot, big puddle on cobbles): the big ones change their gloss but still reflect almost as a mirror; wants
  just matte wetness in places. 1.47 gave every puddle water's Fresnel (0.98) and full flatness.
- PuddleBigWater (181, 0.2; RN3.w): Wetness() out pWater (the small mirrors 1, the big ones this, blended by the
  small ones' share). Flatness (PS_SSR flatK, PS_SSRApply's normal) and water's Fresnel only x puddle x pWater;
  otherwise the gloss's Fresnel (0.02 + (gloss - 0.02) f5) and the bumps kept. Darkening 0.35 x max(wet, puddle x
  (1 - pWater)) + 0.1 x puddle: a damp patch is as dark as fully wet ground (the small mirrors as before).
- Harness early147: the scene typeless R32G32B32A32 with a typed RTV (as the game's); the big ones' water 0.25 for 0.2
  (the small mirrors' share inside them). suntest2, probetest, taatest, panotest pass.

## 1.49 (2026-10-19): the small puddles' settings; AO over frames
- User (1.48: cars reflect right now, puddles fine): the same settings for the ordinary puddles; AO is very noisy.
- PuddleSize / PuddleGloss / PuddleWater (182..184; 0.5, 0.95, 1 = as before) in RainCB RN4 (848 bytes): the small
  ones' threshold lerp(0.7, 0.42, size) (+0.14), their gloss and water in Wetness's pGloss / pWater and GlossFix.
- AO: 8 samples at half size, turned anew each frame (IGN), smoothed only with sky occlusion on, by a 4x4 box half a
  pixel off: per-frame noise. PS_ABlur now always (AO or sky occlusion on): 5x5 1-4-6-4-1 depth-aware, then the last
  result reprojected (this frame's camera G[L1.w], last frame's record t28; its distance within 2% + 0.1 m), clamped
  to the 3x3 neighbourhood (mean +- 1.5 spread, with the smoothed value), 85%. Into g_hrt[9 + cur] (kHalfRT 11), the
  last one at t71, P24.w.
- Harness aotest (new, b46): floor pattern + noise 0.25: raw 0.124, smoothed 0.042, 24 still frames 0.016, moving
  (0.3 m + 0.5 deg a frame) 0.023. early147 (small puddles 21.9% as before), taatest, panotest pass.

## 1.50 (2026-10-19): no relief on cars
- User (1.49: all fine now): sharp relief shadows on cars (dark lines round the door handle, seams; facets).
- Relief() integrates the G-buffer normal against the surface's plane from the depth (GeoN): on a car's low-poly body
  the smooth normals over flat facets read as bumps. The paint test (spec luminance > 0.06) missed this paint.
  OnCar(world): inside a box (|x| < 1.45, z -4.3..4.4, y 0.06..3.3 m) of any car in RainCB's list (RC: the player's and
  the 15 nearest traffic cars, bound in the sun pass by RainBind) -> relStr 0.
- Shaders compile (panotest); shadowtest2 passes.

## 1.51 (2026-10-19): no NaN dots from bright lamps in reflections; the new passes timed
- User (1.50): glitches on a tail light's white LEDs (coloured / black dots); FPS lower than before (day 45-50 -> 35,
  night 25-30 -> 25) after the AO or another fix.
- The sharp reflection source (tScene, the scene itself, R32 float) is not clamped: LED pixels past FP16's 65504 went
  inf into the R16G16B16A16 reflection target, and the 1.46 history (Catmull-Rom, clip: inf - inf) made NaN that stays.
  PS_SSR clamps its output to 6e4 (NaN -> 0); the blur guards the history fetch (NaN -> the mean) and its output; the
  AO history fetch too.
- The log's passes were as before (half-res ~4, sun ~2.8, snap ~3, reflections ~2.4, air ~4.7 ms); the 1.47 scene copy
  (in a game draw) and the 1.46 sky probe (in the sky draw) were not timed: new slots "reflections' copy" and "sky
  probe" (in mod GPU). The timing text buffer 768.
- Harnesses: taatest, aotest, probetest, panotest pass.

## 1.52 (2026-10-19): the sky probe cheaper; the copy after the game's deferred lights; no sky under cars
- Log (1.51): sky probe 0.6-1.3 ms (the cloud march at 128^2 at full quality), the copy 0.37 ms; the copy came before
  draw 1 after the sun pass: additive "deferred_depthBufferSamp, deferred_normalBufferSam, ..." shaders (the game's
  other deferred lights, light volumes) not known to LightKindOf. User: white halos on the road under cars.
- Probe 64^2, a face every other frame, the clouds at the lowest quality (SK4.w 0); PanoSky mips x6.
- EarlyCopyCheck skips shaders whose names have "deferred_".
- PS_SSR: tHide = where the ray first went behind a surface; a miss then returns the sky x saturate(tHide / 5 - 0.2):
  a ray from the road going under a car's body (its underside not on screen) found nothing and took the (now bright,
  1.46 probe) sky.
- Relief's OnCar only where Relief can work (under 16 mm a pixel): 16 cars' boxes per pixel everywhere cost ~0.8 ms.
- Harnesses: probetest (every other frame), taatest, early147, panotest pass.

## 1.53 (2026-10-19): a lit square in far buildings' shadows; shadows drawn again more often as the sun moves
- User (1.52: 42 FPS by day now): a square from a shadow cascade cut out of a far building's strong shadow; shadows
  move jerkily with the time of day (stretched, then jump to their place).
- Square: our nearest (and kept maps after a redraw) hold only what the game draws for its shadows now; a far building
  outside that is missing there. SunMaskAt: where its cascade finds no caster, the wider kept cascades (2, 3) are asked:
  a caster over 2 m away along the sun at the point (0.5 m bias: not the surface's own) -> 5 taps there (shifted by SD
  x that distance) -> its shadow. Lit pixels pay one or two depth taps.
- Jerk (log: kept maps drawn again for the sun 0-1 times in 900 frames: steps of 0.75..1.5 deg at our sun's ~0.05
  deg/s, SD's shift between them stretched, the redraw jumped): ShadowSunStep (185, 0.3 x the step) in SHADOWS.
- Harnesses: shaders compile; shadowtest2/3/4 run (no new failures).

## 1.54 (2026-10-19): buildings out of the game's shadow view still cast (the city's height map)
- User (1.53): a little better, the jerk and the square still there; buildings behind the camera cast no shadow.
- Our maps (and the kept ones after a redraw) take only the casters the game draws for its own shadows, fitted to its
  view: buildings out of it cast nothing. New: the city's sun shadow from the rain map (the city's height from above,
  512 m, kept: whatever the widest cascade ever drew, behind the camera too): PS_CityShadow (every other frame, 1024^2
  R32F, t72) stores per texel the height under which a point is shaded by something 10..245 m towards the sun (64 steps
  of the map); SunMaskAt, where no cascade (nor the kept 2, 3) finds a caster: CityLit(world) = saturate((y + 0.3 - h) /
  1.2). Casters nearer than 10 m are left to our maps (tree canopies near would shade as solid blobs). ShadowCityMap
  (186, on) in SHADOWS; SD[0].z its flag.
- Harness citytest (new, b47): a 30 m block 40 m east, the sun from the east at 20 deg: shadow height 15.3 (15.4 exact),
  24.6 at 25 m (24.5), low beside it and past its reach. Shaders compile; shadowtest2/3 run.

## 1.55 (2026-10-19): the game's time of day from the menu; the city's shadow without a saw-tooth edge
- User (1.54): ShadowSunStep 0.1 much better (wants a bit faster); the city map places buildings right but with big
  visual bugs (saw-tooth edges); far buildings' shadows still go missing at times; wants the day's speed and a time of
  day to scrub in the mod (not the ini and F8).
- The game's time (found in the dump): the environment update 0x8B3F10 (this = env) reads [env + 0x2A84] (seconds,
  0..86400; x 1/3600 = 0.000277778 at 0x8B47B7 -> the hour index). Its caller 0x8B5D70 gets ecx = world + 0x18B80,
  called from 0x476DFD with ecx = game + 0x2BC400; the game object: the global [0x10EC5F0] (main loop 0x427EC2..).
  So time = [0x10EC5F0] + 0x2D7A04 (float). gametime.inc: each frame (Present) the game's own advance since our last
  look x TimeSpeed (187; jumps over 900 s are the game's own, kept); TimeOfDay (188, 0..24 h, K/L 15 min) changed in
  the menu -> the time set, else it shows the time now. Read / written under SEH; logged once ("game time: found").
- PS_CityShadow: 80 coarse steps (to ~345 m), then 16 between the best step and the one before (its near edge: the
  coarse steps of 1.5..7 m quantised the shadow's height -> the saw-tooth).
- ShadowSunStep down to 0.02.
- Harnesses: citytest (15.5 / 24.6), panotest, shadowtest2 pass. Not checked in game: the time pointer.

## 1.56 (2026-10-19): the game's clock drives our sun; the day's speed through the game's own field; no crane trails
- User (1.55): after a time change the sky / our sun out of step; a crash while scrubbing; the speed works now and then;
  the city's shadow a mess near moving cranes; saw-teeth at some angles still.
- The game's time advance found (0x8B4C00): time += dt x [env + 0x2A90] (its speed, game s a second), + [env + 0x2AA8]
  (a jump, then 0), wrapped at 86400 (or held between [+0x2A8C], [+0x2A88] when they differ); [+0x2D08] += dt, then
  +0x2D04 = the time (what the environment shows). The setter 0x892960 (a message, 0x8A9824) writes the time, the
  limits, the speed and +0x2D04. 1.55 rewrote the time each frame (x TimeSpeed): fought the game's own advance.
- gametime.inc: TimeSpeed scales the speed field (the game's own last value x TimeSpeed); TimeOfDay writes +0x2A84 and
  +0x2D04; only while the world is drawn (g_air.frame) and the object looks right (time 0..86400, speed 0..100000).
- SunPathStep: its phase = the game's time / 86400 + TimeSunOffset (189), learnt once where its azimuth line is sure
  (a tenth of a day of samples, 600 frames averaged), then kept: jumps and speed changes at once (the azimuth line
  lagged after each: the sky's night out of step).
- The crash (Event log 13:33, NFS13+0x151F8E: a movaps from an array in 0x551F**): the game's own function, the same
  one as the 0.19-era crash (NFS13+0x151C29); not known to come from the time writes.
- Rain map: shadow draws of moving things and slow movers' spots (g_ssRainSkip, from the shadow instance test) are not
  drawn into it: cranes left trails there (whatever was ever drawn stays), the city's shadow a mess.
- Harnesses: suntest2, panotest, citytest, shadowtest2 pass; raintest's edge lines read as before (not touched).

## Rollback to 1.54 (2026-10-19)
- User (1.56 shot: puddles broken into white speckles, lighting and sky broken, time sometimes spinning endlessly):
  back to the version before the time control; not needed for now.
- Installed NFSLight_v154.asi.bak + the 1.54 HLSL; sources restored from the v154 / v155 backups (nfslight.cpp, NFSLight.hlsl,
  menu.inc from *_v154; sunshadow.inc, rain.inc from *_v155 = their 1.54 state); gametime.inc removed. Rebuilt: the same
  size as the 1.54 ASI. 1.55 / 1.56 kept as *_v156.*.bak (and *_v155 for 1.55).
- For later (from 1.56): the game's time = [0x10EC5F0] + 0x2D7A04 (env + 0x2A84), speed env + 0x2A90, jump + 0x2AA8,
  shown + 0x2D04; the learnt offset of our sun's day phase from the game's clock: 0.161 (TimeSunOffset in the ini).
  Unknown what broke the puddles / lighting in 1.56 (the speed field written? the rain-map skip?): to check before reuse.

## 1.57 (2026-10-19, from 1.54): car paint reflections without a mosaic of squares
- User (1.54 back, fine): sharp squares in the reflections on a car's body.
- PS_SSRBlur's history test used the depth's normal (GeoN): on a low-poly body a flat facet per triangle; the history
  held on some facets and not on the next (fresh pixels look different): a facet mosaic. On car paint (PaintOf > 0.3)
  the plane is the G-buffer's smooth normal, the tolerance x3, and no mirror-image candidate (a curved body).
- World editing (asked to think about): HAWAII has 169 TRK_UNIT*.BNDL (resource types 0x5 177, 0x2 113, 0x1 110,
  0x15 73, 0x51 60, 0x14 60, 0x20f 15, 0x205 11, 0x7 9, 0x214 9, 0x217 4, 0x81 3; no debug names), 8 ARENA, 7 DISTRICT,
  GLOBALRESOURCES (97 MB), WORLDLIST, PVS, TRIGGERS, TRAFFIC.

## World editing, step 1 (2026-10-19): bundles rewritten, props added to a track unit (test)
- burnout.wiki Resource Types (Most Wanted): 0x50 InstanceList, 0x60 PolygonSoupList, 0x204 DynamicInstanceList,
  0x205 WorldObject, 0x20F / 0x214 Groundcover, 0x213 LightInstanceList, 0x215 / 0x216 Compound, 0x217 PropObject
  (PS3 symbols: version, Model*, damaged Model*, physical definition handle, impact effect / sound handles, reset
  distance^2, remove instead of reset), 0x218 PropInstanceList (not documented: decoded here).
- PropInstanceList: header [2, 16, count, 0]; 0x60 a prop: 4x4 matrix (w 1.0 per row), +0x40 PropObject* (import),
  +0x48 id, +0x4C 1.0, +0x50 offset of an optional "InstanceAdditiveAndAO" block; then those blocks; the import
  table (16-aligned) per prop. 169 units, 12290 props, 112 kinds; all re-serialised byte-identical.
- Bundle 2 here: header, entries (72 B), block 0 and 1 packed, each resource zlib level 9 (the original bytes again).
  _NFSLight/worldtools: bndw.py (writer, byte-identical round trip), props.py, addprops.py, restore.bat.
- Test installed: TRK_UNIT157 (the player's start, camera ~1730 33 2311) + 3 props: a copy of inst 218 (kind
  04ff5d30, 111 in the city) at 1731.9 31.6 2314.4, of 82 (c3da99c2, 941) at 1739.4 31.6 2313.4, of 80 (e8507222,
  379) at 1739.0 31.6 2316.4; ids 23acd2..4 (past every id). Original in worldtools/orig. Not yet seen in game.
- 2026-10-19 (user): the 3 copies are there (a bench and a planter seen; the world editor picks "PIECE of the object
  at 1739.43 31.61 2313.41" = our copy), no crash, physical. Added props in TRK_UNIT bundles work.

## 1.58 (2026-10-19): world editor copies (Ctrl+D) and baking props into the track units
- worldedit.inc: a copy is an edit (copy, cn, src; key WeCopyKey(src, cn)) placed from the source as the game puts it;
  drawn after the source's draw (also when the source is hidden) by WeCopiesDraw: the same draw with the copy's
  matrix through the whole draw path (BeforeDraw, our shadow replays, AfterDraw), picked and selected by its own key.
  Ctrl+D copies the selection (whole objects) 1 m along x and selects the copies; Del deletes a copy; undo removes it.
  Saved as "copy x y z tag n move .. turn .. scale ..". Harness wecopy (b48): saved / loaded back, tidy deletes.
- worldtools/bake.py (bake.bat): props found by place (2 cm) + axes' tag (NFSLight's WeTag); moves (matrix edited as
  WeApplyEdit), hides (removed: props.remove, offsets moved back), copies (props.add, new ids) written to the units
  (originals in orig/), baked lines out of NFSLight_world.txt. Dry run on the user's file: 1 of their old edits is a
  prop (TRK_UNIT157 prop 84, matched by its tag); the statics they edited are InstanceList entries (TRK_UNIT80 / 157
  / 161: header [16, n, n2, 3], 0x60 an entry, its matrix at +0x20): not baked (collision elsewhere). A copy line
  dry-run: prop 218 + (2, 0, 0) m, turned 90 -> added. props.py add / remove / move tested on TRK_UNIT11.

## World editing, Blender (2026-10-19)
- User installed DGIorio's add-ons (import 2.7.1, export 3.6) + the game library in Blender 3.6. "Game library not
  found": the add-on looks for <scripts>/addons/NeedForSpeedMostWanted2012 (NFSMWLibraryGet). Import works (headless
  test: TRK_UNIT157 6380 objects, 16 s) with Resource type InstanceList (the default is GraphicsSpec).
- The library's TRKs is empty: the exporter copies TRKs/<unit> and otherwise writes empty Prop / Light / Dynamic /
  Compound / Groundcover lists and navigation mesh (its TODO: props not exported). prepare_unit.py unpacks the game's
  unit there (bundle_packer_unpacker.unpack_bundle_mw, pure Python). Round trip (headless import + export, unchanged):
  every original resource kept (1603), +3 SamplerStates; InstanceList, 113 Models / Renderables, 25 Materials, 1 Texture
  rewritten. Installed in HAWAII for the user to look at (the props-test version kept as
  worldtools/TRK_UNIT157_props_test.BNDL; orig/ has the original). install_unit.py checks no resource is lost.


## 1.59 (2026-10-19): no 25 s stall at the switch-on (a shader cache); no trails on the player's car; no black reflections on car lights / glass
- User (1.57/1.58): the game stands ~15 s when the mod is switched on; black reflections still (a tail light, the rear
  window at times); trails on the car at speed; far shadows (wants the whole city's shadows at once).
- Log: "resources: creating" 20:23:27.0 -> "NFSLight.hlsl compiled" 20:23:52.7: the 28 entries of the 150 KB source
  compiled one after another at OPTIMIZATION_LEVEL3 on the render thread.
- g_compile = CachedCompile (d3dcompiler_47's D3DCompile behind it): the blob kept as scripts\NFSLight_cache\<FNV-64 of
  source, entry, target, defines, flags>.cso (read back via D3DCreateBlob, checked "DXBC"; written .tmp + MoveFileEx;
  emptied past 600 files). Every g_compile user (sky, headlight, puddles, ...) gets it. CompileShaders: the 28 on up to
  8 threads (ShCompileAll); at the game's first CreatePixelShader (LoadCompiler) a below-normal thread fills the cache
  (ShPrewarm), CompileShaders waits for it. Harness (taatest rebuilt): cold 7.8 s on threads, cached 16 ms.
- Trails: every history pass (reflections, AO, sun mask, bounce) found the last frame by the point's place in the
  world: on the moving car a door point lay on the same door plane a frame ago (0.5 m further back at 100 km/h), the
  distance / plane tests passed: the history slid along the body. rain.inc: the player's matrix (read back 2+ frames
  late) carried on to now at its last speed per frame, and where it was a frame ago (RP[3] after RN4 in RainCB, 896 B;
  RN4.w: RC[0] is the player's); HLSL OnPlayer / PlayerPrev / HistPoint: the point in the car's frame, put where the
  car was. PS_SunTemporal, PS_ABlur, PS_GIBlur reproject HistPoint(world); PS_SSRBlur its okS from it, no mirror-image
  or camera-space candidates on the player's car, and on its paint less history and a tighter clip with its speed
  (x (1 - 0.45 run), clip x (1 - 0.5 run), run = its motion / 0.25 m a frame). (The relief's OnCar box no longer lags.)
- Black: PS_SSR's 1.52 under-car darkening (a miss after passing behind something: x saturate(tHide / 5 - 0.2)) hit
  car lights / glass too (their rays pass behind the car's own nearer parts, then leave the screen): now only on
  floors (geometric normal y > 0.6 .. 0.85) and never inside RC's car boxes.
- Harnesses: taatest, aotest, citytest, shadowtest2, probetest pass.

## 1.60 (2026-10-19, HLSL only): wheels, a white outline round cars, a tail light's dots
- User (1.59): spinning wheels get brighter; a one-pixel white outline round cars in the reflections; the tail light's
  white / black dots still there.
- Wheels: 1.59's car motion put a spinning wheel's history on another spoke (AO / shadow / reflection history of the
  wrong part). HistPoint(world, uv): on the player's car, under 0.8 m above its place and not paint: no history.
- Outline: PS_SSRApply's 4 half-size texels: where none is this surface (best depth mismatch > 1..2 of its tolerance)
  the reflection fades (match); car paint's Fresnel from max(N.V, 0.12) (the silhouette's grazing normal took the
  full sky).
- Tail light: PS_SSR hits under 0.3 m (inside the lamp's own housing: its LEDs) fade to the sky; PS_SSRBlur holds a
  texel to 2 x its 8 neighbours' brightest (a LED caught by one jittered ray).
- Harnesses: taatest, aotest, shadowtest2 pass.

## 1.61 (2026-10-19, HLSL only): far buildings' shadows where the cascades meet
- User (1.60): the tail light's dots and the outline still there; shadows: a building's shadow comes in ragged where the
  near cascade gives way to the middle one (debug view: cascade colours).
- SunMaskAt asked the wider cascades for a far caster (1.53) only where its own cascade found no caster at all around:
  with a post / tree / the building's near part around, the far building (missing from that cascade: drawn from what
  the game draws for its shadows) was ignored; in the dithered band between cascades the points took either: ragged.
  SsFarCheck (k + 1 up, a caster over max(minM, 4 texels) away): now also for every lit result (v > 0.02, minM
  max(4, 2 x this caster's distance)), the darker taken. The 1.53 path: k + 1 up (was from 2), minM 2.
- Harnesses: shadowtest2, citytest pass; shadowtest3 / 4 as before.

## 1.62 (2026-10-20, HLSL only): tail lights, under cars, dawn reflections, low-sun cascades
- User (1.61, debug views): "only what they add" shows white streaks added in the tail lights (mod off: clean red);
  in the morning reflections white too early, no dark / reflection under the car; with a low sun the cascades view a
  mess of yellow / blue patches in the near one; far shadows a bit better.
- Tail lights: PS_SSRApply, on a car's box and not paint: the dry reflection x saturate(1 - normal variance x 25) (the
  reflector's bumps: each pixel's ray elsewhere: dots), F tinted by a saturated albedo (lamp glass).
- Under the car: 1.59 left out the under-car darkening inside RC's boxes: the road under the car lies in its box. Now
  on every floor again (geometric normal y; car lights / glass have low y).
- Dawn: the sky probe (our sky) scaled to the seen panorama: its mean near the horizon (level 9) / the probe's mean of 4
  horizon directions (level 6), 0.2 .. 1.2.
- Cascades: 1.61's far check on every lit point read a coarse cascade's own ground as a caster with a low sun (a texel
  of flat ground spans metres along the sun): SsFarCheck needs a caster past 2 x texel x (2 + 1.5 |plane gradient|),
  its taps compared on the receiver's plane less that.
- Harnesses: taatest, shadowtest2, probetest, citytest pass; panotest as before. The white outline: not found yet.

## 1.63 (2026-10-20): ShadowOnly (test): one of our sun maps alone
- User: wants a mode with one map only, everything at once over its whole reach, to find where the shadow trouble is.
- ShadowOnly (187, SHADOWS, 0 off, 1..4 = map 1 red .. 4 yellow): SD[1].z; SunMaskAt takes that map wherever it reaches
  (past it: lit), no dithered band, no next map for the search / penumbra, no far-caster check, no city map. Each map
  still holds what the game drew for its shadows (its cascade's casters): the view shows which map lacks a building.
- Harnesses: shadowtest2, taatest pass (shaders compile).

## Release 1.63 packaged (2026-10-24)
- Version labels 1.63 (cpp header, log line, menu title was still "1.24b"); rebuilt, installed. addprops.py: the
  HAWAII path from its own folder (no absolute path).
- TODO.md: one list (open now / waiting for a decision / world editing / the older polish list); the dawn reflections
  and under-car fix (1.62) confirmed by the user, taken out.
- _NFSLight/publish/: github/NFSLight (README EN + RU, LICENSE MIT, CHANGELOG from the headings, TODO, docs/DEVLOG =
  this log without the backup paths, src without backups, worldtools scripts only, .gitignore), release/NFSLight_1.63
  (+ zip, forward-slash paths: dinput8.dll = Ultimate ASI Loader + its note, scripts: asi, hlsl, ini, lamps, zones,
  presets, templates, carlights; README EN/RU), nexus/ (BBCode EN / RU, summary), DEMO_PLAN.md (RU: what to shoot).
  um publish check: PASS on both (against an empty folder: against the game folder our own installed files read as
  "game files").

## 1.64 (2026-10-24): renamed NFSLight -> NewLook
- User: rename NFSLight to NewLook everywhere. Sources (strings, comments, file names: NewLook.asi / NewLook.hlsl /
  NewLook.ini [NewLook] / NewLook_*.txt / NewLook_cache / NewLook.log), worldtools (bake.py reads
  scripts/NewLook_world.txt), docs, the repo, the release. The dev folder _NFSLight and its backups keep the old name.
- MigrateOldNames (DllMain, before the ini is read): scripts\NFSLight* -> NewLook* (files and folders; not .hlsl,
  .log, _cache; never over an existing NewLook one), the ini's [NFSLight] -> [NewLook]. An old NFSLight.asi beside it
  (both would hook the game): renamed NFSLight.asi.old, this start does nothing (logged), the next is NewLook's.
  Harness migtest: all pass. Export NewLook_MouseCaptured + the old NFSLight_MouseCaptured.
- DroneCam (version.dll, rebuilt): looks for NewLook.asi / NewLook_MouseCaptured, then the old names (its backups:
  _DroneCam/dronecam_before_newlook.cpp.bak, version_before_newlook.dll.bak).
- Installed when the game closed: NewLook.asi + .hlsl, version.dll; scripts\NFSLight.asi / .hlsl / _cache removed
  (the asi kept as _NFSLight/NFSLight_v163.asi.bak); the data files are renamed by the mod at its first start.

## 1.65 (2026-10-24): full code review: faster lamp shadows, batched rain / lamp replays, per-draw caches, fixes
- User: review all of the code (bugs, weak spots, speed without losing quality), look at modern techniques, apply what
  fits. Every file read; findings and their status in REVIEW_1.65.md. Backups: src/*_v164.*.bak, NewLook_v164.asi.bak,
  NFSLight_ini_before_165.bak.
- Measured first (the log of 1.64 at night: GPU-bound at ~50 fps, the mod 11.7-12.7 ms of GPU, of it lamps 4.7-5.2 ms).
  New harness ltest/lampperf: PS_Lamps / PS_Half / PS_SSR at 2560x1440 on this PC's GPU (a street, 34 lamps, the
  user's settings), GPU timestamps, outputs summed to compare versions.
- HLSL: the screen-space marches (lamp shadows, contact shadows, sky occlusion, reflections) were latency-bound: each
  depth read waited for the one before, each step re-solved Project's 3x3. Now RayProj (the solve set up once per
  ray: linear along it) and the reads issued together (unrolled; reflections in groups of 4; the first hit decides as
  before). PS_Lamps 6.3-6.9 -> 5.1-5.2 ms, bit for bit the same; PS_Half a little faster, the same; PS_SSR the same
  speed (1 pixel in 921600 differs by rounding). LampShade's zone loop to the zones there are (it ran 16 sincos a
  pixel). Headlight beams: the ray clipped to the spot's frustum first (all 20 samples in the beam: less noise).
  The bounce light's history clamped to this frame's samples of the surface (no lingering). PS_Grade: triangular
  dither of +-1 step before the 8-bit write (no banding in skies / haze).
- GPU: the rain map's replays (40 a frame, 900 a frame while it refills after a 96 m move) and the lamp faces' were
  drawn inside the game's shadow pass one by one, two depth-target switches each (~9 us each): now recorded and drawn
  with the sun replays after the pass (SsFlush targets 9 / 10); the constant-buffer copies kept at night for them.
  Harness rectest: the rain map and the lamp faces bit for bit as before.
- CPU: Immediate() by pointer; the draw's VS info and depth target asked once a game draw (DrawScope / DrawVs /
  DrawDsv) instead of 3-5 times; world matrix keys memoised by the matrix (WeMemoOf; 12 floorf + 64-bit multiplies
  per draw before), the shadow instance hash on 32-bit lanes; found lamps against the 4000-lamp map by a 4 m grid
  (~800k distances a frame before); the menu text redrawn only when it changed; SaveIni one section write (initest:
  188 values back); the building normal maps read back without waiting (a stall per new material); the prop memory
  scan backs off to 2 min when it finds nothing.
- Fixes: ResizeBuffers hooked (the overlay's back-buffer view made a resolution change / full-screen switch fail);
  MakeRT frees a half-made target; the reflection targets do not retry a failed size every frame; the world editor's
  per-frame stats divided by the real frame count (they were x2 at 50 fps).
- Harnesses after the change: raintest, wetest, wecopy, cbcopy, aotest, taatest, citytest, gradetest, shadowtest2,
  rectest, initest: all pass. Installed (the game closed): scripts\NewLook.asi + NewLook.hlsl.
- Not done (waiting for a decision, they change the look): GTAO's cosine-weighted sky occlusion and multi-bounce AO;
  a visibility-bitmask AO / bounce light as an option. Not measured in game yet: the CPU savings (the stats lines:
  "world editor", "sun shadows ... rain map + lamp face draws", "timing").

## 1.66 (2026-10-24): modern AO options to compare (PLAN_1.66.md; all off by default)
- User: make the GTAO / bitmask ideas options to compare. Backups: src/*_v165.*.bak, NewLook_v165.asi.bak.
- AMBIENT & OCCLUSION group, new: SkyOcclusionCosine (188: SkyVis 1 - h^2, cosine-weighted as GTAO), AOMultiBounce
  (189: PS_Sun's ambient AO per albedo channel, Jimenez et al. 2016 fit), AOBitmask (190: ScreenAO as a visibility
  bitmask, Therrien et al. 2023: 2 slices through the normal x 2 sides x 3 steps, 32 cosine-weighted sectors, each
  sample covering its front to its back (thickness 0.3-0.5 m): light passes behind thin posts and rails). LightCB P25.
- lampperf mode 1: options off = 1.65's output bit for bit; on: PS_Half 0.59 -> 0.63 ms at 2560x1440. Installed.
- To compare in game: the bitmask AO is lighter than the old one at the same AOStrength (no 1.6x boost: it is the
  cosine-weighted share itself); raise AOStrength to taste.

## 1.66b (2026-10-24, HLSL only): reflected sky where it was never seen matches the sky on screen
- User (debug sky map screenshots): the probe (bottom) a blue day sky under an orange dusk, lit under a night sky,
  black at night, against the seen panorama (top). The probe is the sky's own draw, before our dusk / night darkening
  and haze; PanoSky scaled it by brightness only, clamped 0.2 .. 1.2. Now per colour channel, 0.02 .. 1.5, and a black
  probe gives the seen sky's mean. Backup src/NewLook_v166.hlsl.bak. Installed (Shift+P reloads it in game).

## 1.66c (2026-10-24): reflected-sky debug sliders (the user tunes; 1.66b alone did not help: the seen panorama was
## nearly black / unseen at dawn and the puddles showed a white sky)
- REFLECTIONS: ReflSkyBright (191, x), ReflSkySource (192: 0 auto, 1 the seen sky only, 2 the probe only matched, 3 the
  probe as drawn), ReflSkyMatch (193: how much the probe takes the seen sky's colour), ReflSkyLevel (194: the seen
  panorama stored by the sky's level now and read times it, or as seen: suspected of the white at dawn when the
  level rises). LightCB P26. Backups *_v166.*.bak, NewLook_v166.asi.bak. Installed (asi + hlsl together).

## 1.67b (2026-10-24, HLSL): reflected sky at dawn / dusk
- The probe was matched to the seen sky only with 5 % of the panorama seen; at dawn / dusk less is: the probe went in
  as drawn (bright orange clouds over a dark sky; the wet ground glowed). Now from 0.5 % (the seen mean is divided by
  its coverage anyway), and the match never brightens (ratio up to 1, was 1.5). Backup src/NewLook_v166c.hlsl.bak.

## 1.68 (2026-10-24): our anti-aliasing (FXAA 3.11)
- User: a light anti-aliasing of our own (the game's renders twice the pixels: 2560x2880 for 2560x1440).
- LOOK (COLOUR): AntiAliasing (195, off by default), AASoftness (196, sub-pixel blend 0..1, 0.5): FXAA 3.11 quality
  (8 search steps) on the game's finished picture in PS_Grade, before CAS sharpening and the grading (needs LOOK on);
  GradeCB Gr5.w = 1 + softness. Compiled; cost not measured in game (FXAA: ~0.2-0.4 ms at 1440p is typical).
  Backups *_v167.*.bak, NewLook_v167.asi.bak. Installed.

## 1.69 (2026-10-24, HLSL): cars shaded with care; dark dots in reflections
- User (screenshots: blotches in steps on the white 911, a mirror's shadow in texels, blue patches; black dots in
  reflections). On car bodies (OnCar): our sun shadow from 5 mask taps 2 px apart (same cascade); screen-space AO and
  contact shadows a third as strong (half-res blotches on curved panels); the blue shadow tint 40% as strong.
  SSRBlur: a lone dark texel raised to half its neighbours' darkest (as the bright cap of 1.60).
  Backup src/NewLook_v168.hlsl.bak. Installed (Shift+P).

## 1.70 (2026-10-24): cars in reflections, dark dots by a median, car paint layers
- User: black dots in strong reflections again (a new approach asked); a tyre's top shining; the car missing from the
  puddles seen from above; better paint (clear coat, metallic, soft highlights).
- Reflections: nothing in a car's box is wet road (SsrRay and SSRApply: wet / puddle x (1 - OnCar)): the tyre's top
  under its arch no longer reflects the sky like a puddle. RayCars: the cars as boxes (1.9 x 4.6 x 1.2 m) for a floor's
  ray that leaves the screen / meets the sky: through a car's box within 25 m it sees its dark body (sky x 0.08).
  SSRBlur: the 1.69 dark floor replaced by a median test: a texel under half / over twice (+0.05) its 3x3's median of
  row medians takes that median (dark and bright dots alike, edges kept).
- Paint (modern lighting, cars' boxes with a bright specular): the colour with a softer highlight (PaintSoft 198,
  0.4), a clear coat over it (PaintClearCoat 197, 0.6: GGX F0 4 %, roughness 0.045), metallic flakes on the player's
  car (PaintFlakes 199, 0.4: 3 mm cells on its body, tilted mirrors). MATERIALS group. LightCB P27.
- Backups *_v169.*.bak, NewLook_v169.asi.bak. All 28 shaders compile. Installed.

## 1.70b (2026-10-24, HLSL): paint found by a paint-like specular; a bigger car box in reflections
- User: the paint layers showed only on rims and tail lights; the car still missing from the puddles. Paint: specular
  luminance 0.02 .. 0.3 (the body's; chrome above, rubber below; it was over 0.1: rims, lamps). RayCars box x +-1.1,
  y -0.7 .. 1.4 (the car's place may be its body's middle), z +-2.5; a hit found farther than a car's box on the
  way is darkened too (a bright far texel seen past the car's underside). Backup src/NewLook_v170.hlsl.bak. Installed.

## 1.71 (2026-10-24): reflections' 1-pixel outlines; the paint layers taken out
- User: the paint layers did not show on the body: removed (HLSL block and the menu entries; params 197-199 kept,
  unused). The outline round thin things (poles, window frames) in reflections: SSRApply took the 4 nearest half-size
  texels by place, weighted 1/(diff): on a thin thing none was its own, the background's reflection came in. Now a 4x4
  window, the ring weak by place, every texel by exp2(-3 x distance misfit): its own texel wins where there is one.
  Backup src/NewLook_v170b.hlsl.bak. Installed.

## 1.72 (2026-10-24): the player's car in puddles as a plane; puddle rims; a denser storm; TODO refreshed
- User: the box made a dark cube in the puddle: RayCars is now the player's car only (RN4.w), a flat 1.8 x 4.4 m
  underside 0.35 m over the floor the ray starts from. Traffic: nothing (it looked fine and the boxes stood out).
- Puddle rims: Wetness and GlossFix get a band of soaked asphalt round the small puddles (the noise just under the
  puddle threshold): wet, dark, glossy, not a mirror.
- Storm preset: cover 0.85 -> 0.95, cloud shadows 0.95 -> 1.0 (re-pick it to apply).
- Black dots in strong reflections remain (TODO). TODO.md rewritten for 1.72 with the user's answers and the feature
  list of 2026-10-15 (weather next). Backups src/NewLook_v171.hlsl.bak, src/sky_v171.inc.bak. Installed.

## 1.73 (2026-10-24): no sky glow under the player's car; rain drops and runs on it
- User (screenshot: a pale blue glow on the wet road under the car). Not our ray only: the game's own lighting lit the
  wet gloss there with its sky, and our modern-lighting sky term too. UnderPlayer(world): the player's footprint + 0.3 m,
  soft edges, floor height only: GlossFix's wetness x (1 - 0.85 k), SsrRay's sky x (1 - 0.9 k), PS_Sun's envS
  x (1 - 0.9 k) on floors.
- CarRainDrops (200, 0.7, REFLECTIONS; P27.w): on the player's car, with the wet-ground amount, out in the open: round
  beads on its tops (1.6 cm cells on its body), runs on its sides (0.9 x 7 cm cells sliding down with the frame count);
  each a small dome: its own sun glint (GGX, rough 0.03), the paint under it 15 % darker.
- Backups src/NewLook_v172.hlsl.bak, newlook_v172.cpp.bak, menu_v172.inc.bak. All 28 shaders compile.

## 1.73b (2026-10-24, HLSL): car rain drops that look like water
- User (a reference shot: dense beads, each with a bright sky rim): ours were only dark spots (the glint came from the
  sun alone). Now: beads of two sizes (1.0 / 1.7 cm cells, random size and place) on every side of the body, runs on
  the sides; each a dome mirroring the sky (the place's ambient over its albedo, brighter upwards) by Fresnel: a
  bright rim, a sun pin-point, the paint darker through its middle. Backup src/NewLook_v173.hlsl.bak. Installed.

## 1.74 (2026-10-24): rain drops on the player's car simulated (carrain.inc)
- User: 1.73b better but far from the goal: the runs a fence sliding and jumping back, jerky (the frame count), drops
  should behave physically and run against the motion when driving (as NFS Heat / Forza Horizon).
- carrain.inc: 8192 drops in a 128 x 64 RGBA32F ping-pong state (place on one of five car-local unfoldings: top x z,
  sides z y, front / back x y; radius; velocity; last place). Once a frame at Present (real dt): rain spawns them
  (0.4 .. 1.6 mm) and grows them (tops most); pushed by gravity in the car's frame (its tilt), inertia (- its
  acceleration, from the matrix read-backs, smoothed) and the air (k |w| w / r, w = - its velocity); static friction
  9.81 (1.7 mm / r)^2 (big drops run, beads hold), sliding friction, viscous damping; water lost along the way; die
  small, off the body, or by chance (1/240 s). Drawn into an atlas of the unfoldings (2000 x 2250, 4 mm, R16_UINT index,
  the nearest by SV_Depth) and their paths into a drying trail map (R8, tau 10 s). PS_Sun (t73 .. t76): the nearest
  drop of the 2 x 2 texels round a pixel, its exact dome / runlet (thinner behind); fine static beads between (the
  procedural cells), wiped by the trails, blown off at speed (P28: per side, from the speed over 8 m/s); Fresnel sky
  rim, sun pin-point, darker middle; trails make the paint wetter. No procedural runs any more (the fence).
- Harness crtest (no game): standing 10 s: 6134 drops, side runs; 25 m/s: top drops blown back (mean -1.45 m/s), top /
  sides cleared in 3 s; standing again: back to ~5900. All 28 NewLook.hlsl shaders compile.
- Backups src/NewLook_v173b.hlsl.bak, newlook_v173.cpp.bak, NewLook_v173.asi.bak.

## 1.75 (2026-10-24): car rain drops: water caps that mirror the sky, teardrops with life cycles, denser runs
- User: the drops did not look real, stuck on vertical sides, little motion or volume. How games do it (Lagarde's
  "Water drop" series, PBR practice): a drop is a smooth water cap with its own normal; every reflection / highlight by
  that normal (its top mirrors the bright sky, its bottom the dark ground: the volume), Fresnel rim, the paint darker
  through it, a caustic on its far side from the light; a dense procedural layer with life cycles plus a flowing layer.
- Shading: the sky by PanoSky (the reflections' matched sky, clouds) along the drop normal, the ground dark below; the
  sun as water's GGX highlight (roughness 0.04); a caustic; the paint darker towards the rim. Beads: three sizes (0.7 /
  1.1 / 1.9 cm cells), teardrops on vertical surfaces (round below, drawn out above), each with its own 40 .. 90 s life
  (lands, grows ~10 s, stays, goes; real time: LightCB P29.x).
- Simulation: 16384 drops, spawned on the body's own extent (the car's place is its floor: y 0 .. 1.45, z +-2.4, x +-1)
  by area; runs from 1.4 mm on vertical sides; runlets up to 2.5 cm. crtest: 12148 on the body standing, ~25-50 running.
- Backups src/carrain_v174.inc.bak, NewLook_v174.hlsl.bak, newlook_v174.cpp.bak. All shaders compile.

## 1.76 (2026-10-24): car rain drops readable from afar, a debug view; the under-car area rounded and soft
- User: drops invisible from a distance (want shadows or so), a debug view, the dark rectangle under the car in the
  puddles with rounded corners and soft edges.
- Beads: each casts a shadow on the paint (a dark crescent just outside its rim away from the light, a thin dark line
  where it meets the paint); the paint under drops darker (0.2 .. 0.65 to the rim). Far away (a pixel over ~4 mm of
  paint, foreshortening counted) the single beads give way to their average: the paint 8 % darker, the sky's sheen of
  their rims.
- View 37 (U list, surfaces): "car rain": red simulated drops held, green running, blue fine beads, yellow wet trails.
- UnderPlayer and RayCars: a rounded rectangle (corners 0.5 / 0.55 m, half 0.9 x 2.2 m) with soft edges (RayCars
  returns its cover w); the puddle's darkening lerps by it.
- Backups src/NewLook_v175.hlsl.bak, newlook_v175.cpp.bak, menu_v175.inc.bak. All shaders compile.

## 1.77 (2026-10-24): the player's car followed by place (it was lost now and then); drops on the body only
- User: now and then the car lost its drops, its shadows and the rest (and showed dark seams: the car's own treatment
  off); drops showed off the body. Cause: the player's matrix was the frame's FIRST car draw with bones (not a wheel);
  a traffic car near the camera drawn first was taken for it. Now up to 32 such draws a frame are copied (2 KB staging
  per frame) and RainBind picks the player's: the nearest to where it was (within 8 m), or, lost for 60 frames, the
  nearest to the camera. A log line every 900 frames (candidates a frame, the pick's distance, rejects).
- Drops only inside the body's extent (|x| < 1.05, |z| < 2.45, 0.05 < y < 1.5 m); OnPlayer's box is wider.
- Backups src/rain_v176.inc.bak, NewLook_v176.hlsl.bak, newlook_v176.cpp.bak, menu_v176.inc.bak.

## 1.78 (2026-10-24): the player's car by its own part's mesh, its matrix of the same frame on the GPU
- User: 1.77 worse: the car hardly found; when found, the drops slid as if fixed to the road. Causes: (1) the car's
  parts (doors, bumpers) have their own matrices (other origins): 1.77's "nearest to where it was" jumped between parts;
  (2) the pose was the read-back's, 2-4 frames old, carried on at its speed: drops slid at speed.
- Each car draw (bones, not a wheel) gets a mesh key (index / vertex buffers, offsets, range). RainBind picks the
  player's: its part (the same key) near where the car should be (its last place carried on: within 2.5 m), else the
  nearest there; lost a second: its key anywhere, else the frame's first car draw (as before 1.77); the key kept.
  Every frame the draw with that key has its matrix copied on the GPU (DEFAULT buffer, t77; LightCB P29.y = there): the
  drops, UnderPlayer and RayCars take that pose (PlayerPose), so they ride with the car exactly. The drops' gate is the
  body's extent by that pose (InPlayerBody). The log line now says whether the GPU matrix was there.
- Backups src/*_v177.*.bak, NewLook_v177.asi.bak.

## 1.79 (2026-10-24, HLSL): rain beads as lenses in the sun (DriveClub-like)
- User: the drops ride with the car now (1.78 works); asked for DriveClub-like drops: the shadow not right.
- A bead in the sun: its shadow is its own footprint moved away from the sun by its height x tan of the sun's angle to
  the surface (longer when low; up to 2.5 radii), taking the sun's light (0.7) there; inside it the light the bead
  focuses: a caustic spot (1.4 x the sun); a thin dark line where its rim meets the paint (also in the shade).
- The bead itself: its mirror at the true Fresnel (a bright arc only where its rim faces the sky; the rims had gone
  bright all round), the sun's glint; the paint through it a touch darker, its rim dark (0.1 .. 0.7), the sun thinned
  at its rim.
- Backup src/NewLook_v178.hlsl.bak. Installed (Shift+P).

## 1.80 (2026-10-24): the car's rain drops by their physics: the car's shape, real merging, running, rain by flux
- User (1.79): the shadow better, but the drops flat, on the sides too, not merging or running as in rain, too many;
  asked for the most real behaviour and physics.
- The car's shape: while a bake is on (every 45 frames), the player's car's draws in the main view (bones, not a
  wheel; parts within 3.5 m of its own part) are recorded (SsRecord, targets 11 .. 15) and drawn again by SsFlush into
  a shape map (D32 800 x 900, the atlas' layout at 1 cm) from above, the right, the left, the front and the back
  (orthographic, by the player's part's matrix of that frame from our copy of its constants); taken (double-buffered)
  when >= 4 draws went in. CarRainTick flushes at Present. The sim reads depth there: the surface's normal (+-2 cm),
  whether the body is there; drops run down slopes, go over an edge onto the face they now face (dot with its normal
  0.08 above the old face's, and the surface there within 3 cm), drip off where the body ends.
- Merging (1.80's first try lost 85% of the drops: each decided alone): a grid of 16 mm cells, 4 layers (each the
  smallest index above the layer before: depth peeling with points); a "target" pass writes into which drop each one
  flows (the biggest bigger one it touches or swept through; only drops stored in the grid), then the sim: a drop
  dies only if its target flows on nowhere, and that target gathers exactly those (water, momentum, centre).
- Physics: hold ~ 9.81 (1.4 mm / r)^2 x the paint's own unevenness (value noise 0.5 .. 1.5: they meander, pulled to
  where it holds less) x (1 - 0.5 wet track: pulled onto it); sliding: kinetic 0.55, the rim's drag 60 (1.5 mm / r)^2
  /s integrated exactly (steady 2 cm/s .. 1 m/s); air drag 6e-5 |v| v / r along the surface (0.3 in the wake, up over
  a face it hits); gains water sweeping an unrun path, leaves some on its track; resting in the rain grows in chunks
  by the rain landing there (up to 2.8 mm); dries with no rain (the sim runs 20 min after); integer hash RNG (the sin
  hash biased spawns). No random deaths.
- Rain lands by its flux: the rain past the car (7 m/s down minus the car's velocity) through each face's normal per
  unit of its unfolding, splashes / spray low on the sides (0.3 + 0.05 x speed m/s); rejection sampling per face;
  none where a 48 mm square already holds 2 x CarRainDrops drops (the rain joins them instead).
- Shading (PS_Sun): a cap of contact angle ~80 deg; the overcast sky's soft highlight on its upper side; a drop smaller
  than the pixel spread over the pixels round it, fainter, as its average (sky ~10%, the sun's glint as a mirror
  ball's: a quarter of its Fresnel; no more fading to nothing past 3-9 mm pixels); drops stay after the rain / under a
  roof. View 37 also checks the shape map: green where it meets the surface (3 cm), red not, purple none (P29.z, t78).
- crtest (analytic car shape: the copy into a depth target does not take, read from a plain texture): first runners
  ~14 s into heavy rain, ~130 running on the top and ~110 on the sides at 30 s (fastest 1.2 m/s), ~5400 drops; 25 m/s
  for 5 s: ~1400 moving, blown back, the front takes rain; no rain 20 s: 1% of the water dried. A tick 0.28 ms.
- MSVC: the HLSL raw string over 16 KB (C2026): split into three.
- Not verified in the game yet: the bake (the log line "the car's shape baked"; view 37's green / red), that the
  main view's car shaders take our view matrix as the shadow ones do.
- Backups src/*_v179.*.bak (+ rain, sunshadow), src/carrain_v180a.inc.bak, NewLook_v180a.hlsl.bak;
  NewLook_v178.asi.bak, NewLook_v179_installed.hlsl.bak. Installed.

## 1.81 (2026-10-24): the player's car kept; drops on the glass, not in the cabin; the drops' controls
- User (1.80): the drops work very well now (view 37: the baked shape green on the body); the wet tracks last too long;
  the car often "lost" (standing too: found, then not); no drops on the glass, some in the cabin seen through it; asked
  for controls (how much, how readily they run, more).
- The car lost: the log showed 32.0 car draws a frame (the staging's cap: the player's part could be left out) and the
  pick took the nearest part when its own was not there (a door, a bumper: own origins: the drops' box, the shape, all
  off); and CarRainTick cleared every drop after 30 frames without a read-back. Now 64 candidates (with their index
  counts); its part kept while it is drawn (20 frames), the same part trusted up to 6 m off; a new part only then: the
  biggest mesh within 2.5 m (the body). The drops pause through a loss (shown by the pose of now), cleared only after 5 s
  lost, a jump (>= 120 m/s: a reset, a teleport), or the rain long gone. The log: the part's changes, frames without
  its GPU matrix; the drops' clears and paused frames.
- Glass / cabin: the glass is not in the G-buffer (the cabin is), but it is in the baked shape. PS_Sun: drops only
  where the pixel is the body's outer surface (the shape's face meets it within 4 cm); the cabin (under the shape's
  surface by > 4 cm): the view ray bisected (8 steps) between the cabin point (inside the hull from the five views) and
  the car's box: the drops of the face crossed there, with that face's normal from the map (+-2 cm) and the pixel size
  at that distance; no shadows / tracks on the glass; out of the shape (wheels): none. View 37: green on the body, blue
  on the glass, red none.
- Controls (RAIN ON THE CAR group, 200 .. 207): CarRainDrops (on, how thick), CarRainLanding (x rain landing and
  the density cap), CarRainSlide (the paint's hold / x), CarRainSpeed (the running drag / x), CarRainTrail (the track's
  life, 3 s; was 10), CarRainSize (spawn size, growth cap, chunks x), CarRainWind (the air's drag x), CarRainDry (x).
- crtest as 1.80 (defaults); a tick 0.25 ms. All shaders compile (lampperf).
- Backups src/*_v180.*.bak, NewLook_v180.asi.bak, NewLook_v180_installed.hlsl.bak. Installed.

## 1.81b (2026-10-24): the player's car read back through a 16-copy ring; the drops drawn over its glass
- User (1.81): the car still lost now and then (the drops froze: 1.81's pause; the car's look glitched: dark seams,
  banded paint: the player car's own treatment off); the drops on the glass there but not seen.
- The log: "paused 400 .. 1300 frames (the car not seen)" every 30 s, and no "player car:" line in those stretches:
  the matrix read-back failed for hundreds of frames. It was a ring of 4 staging copies read without waiting from 2
  frames on: with the GPU 3-4 frames behind (a GPU-bound 60 FPS), each copy was written over before it was ready:
  never read, as the scene's load went. Now 16 copies.
- The glass: the game draws it after its lighting, over PS_Sun's drops. Each of its glass draws (a car draw with bones
  in the main view, blended, within 3.5 m of the player's part of this frame: our copy of its constants) is drawn once
  more with PS_CarGlass (the 29th shader; kShN), blended dest x (1 - a) + c (colour only): the world place from the
  glass's own depth and this frame's view ray record (t79), the glass's normal from its triangle, PS_Sun's atlas lookup
  and the cap's shading (sky mirror, the sky's highlight, the sun's glint, darker through its rim; the average when
  smaller than the pixel). Our b5 / b13, t67 / 68 (the sky), t73 .. t79, s11 .. 13 bound for it, the game's put back.
  The log line "car rain: ..." now also says how many glass draws were drawn again.
- All 29 shaders compile; crtest as before. Backups NewLook_v181a.asi.bak, NewLook_v181a_installed.hlsl.bak. Installed.

## 1.81c (2026-10-24): drops flickering at seams; the read-back without mid-frame flushes; CPU / long frames logged
- User (1.81b): the car is no longer lost. Drops flicker in places (very often), at edges / seams (a window's sill, a
  door's gap); big drops in FPS when turning the camera with the mouse.
- Flicker: drops landing on an edge, a seam or a panel's gap went at once (off the body, over to another face) and
  landed there again and again. Now no spawns where the shape jumps within 1 cm (> 1.2 cm, or the body ends), and a
  drop's age rides in S.w (its plane + age / 10, up to 4 s): it fades in over 0.25 s (PS_Sun, PS_CarGlass; its shadow,
  focused spot and rim line too), so one living a frame or two is not seen.
- The FPS drops: the log's 10 s timing lines in the slow stretch had every pass (lamps, half-res, snap, reflections,
  air) 1.5-2x slower at once (the GPU shared / clocked down: other windows over the game?), and one with 39 ms frames
  on 16 ms of GPU (a CPU stall). Ours that could stall: 1.81b mapped every read-back copy 2+ frames old without waiting
  (2-3 failed Maps a frame, each making the driver flush mid-frame). Now an event query after each frame's copies
  (End in RainBind), asked without a flush; only the newest done copy mapped, older ones dropped.
- The log ("car rain: ...", every 1800 frames) now has the CPU most in a frame of the glass passes, the tick and the
  read-back, and the frames over 40 ms (the longest): to tell whether a drop in FPS is ours.
- Note: the Python edits (newline='') turned the edited sources' CRLF into LF (no other change).
- Backups src/*_v181b.*.bak, NewLook_v181b.asi.bak, NewLook_v181b_installed.hlsl.bak. Installed.

## 1.81d (2026-10-24): the hitch finder; the drops' shaders compiled on a worker
- User (1.81c): no flicker now; the drops in FPS when turning the camera happen with our effects off too; asked to
  find what it is, or make a tool for it.
- The log of 1.81c: with our effects off (GPU 7-8 ms a frame) 77 frames over 40 ms in 30 s, the longest 321 ms: the
  CPU side (the GPU idle). Once, "the tick 663 ms": our drops' 9 shaders compiled on the render thread when the rain
  first came; now on a worker (CarRainCompile; no drops for that half second).
- hitch.inc: per frame (Present to Present) the time inside the game's draws (the driver), ours round them, the PS
  binds (with our air pass), the making of textures (the driver / ours), views, shaders (the driver / ours), Map /
  Unmap / UpdateSubresource (the driver / ours), Present (the driver / ours before it); the rest = the game's own and
  calls not hooked. A frame over 50 ms (and 2.5x the average) logged as "hitch: ..." (2 a second at most); every 1800
  frames "hitches (the last N frames): ...". TSC stamps (__rdtsc) measured against QPC; ~4 stamps a draw.
- Not verified in the game yet. Backups src/*_v181c.*.bak, NewLook_v181c.asi.bak. Installed.

## 1.82 (2026-10-24): the weather's state (stage 1 of the weather)
- weather.inc: rain now (0..1) and cloudiness (0..1), by hand (WeatherRain, WeatherClouds; rain closes the clouds in
  to >= 0.84) or by itself (WeatherAuto: phases clear, fair, cloudy, overcast, drizzle, rain, heavy rain, WeatherPhase
  minutes each x 0.6..1.4; the next one a neighbour (sometimes two away) taken Metropolis-wise by each phase's share
  0.15 / 0.2 / 0.2 / 0.16 / 0.12 / 0.1 / 0.05, off the ends it stays). Clouds move with tau 40 s; rain with tau 20 s
  and only under clouds over 0.7..0.84 (it comes as they close in, stops before they open).
- It drives (every frame, the menu showing it): WetGround = the ground's wetness (soaks exponentially, 3 x faster per
  WeatherSoak minutes, faster in hard rain; dries linearly over WeatherDry minutes); the puddles (Puddles up to 0.85,
  PuddleWater, PuddleBigWater) by a level filling over WeatherPuddles minutes of rain, draining over 1.5 x WeatherDry;
  the haze x (1 + 1.2 rain) + 3 x WeatherFog; the sky's cover / type / cirrus / rain bellies / cloud shadows between
  sky.inc's presets (clear 0, fair 0.3, cloudy 0.55, overcast 0.78, storm 1). The car's drops land only while it rains,
  by how hard (carrain.inc: g_wxRain; with the weather off, as before by WetGround). The hand-set values of what it
  drives are kept when it is turned on and put back when off.
- Menu: WEATHER group (208 .. 216; Weather / WeatherAuto ON / OFF); a "weather: ..." line under the load figures, and
  the hitch finder's line.
- wxtest (sped-up time): rain 0.6 by hand: wet 0.88 at 2 min, puddles 0.55 at 4 min; no rain: dry at ~9 min, puddles
  gone at ~11; by itself 40 h: clear 13 / fair 17 / cloudy 18 / overcast 13 / drizzle 13 / rain 14 / heavy 12 % (before
  the shares' last trim), raining ~35-39 % of the time; off: every value back as set by hand.
- Backups src/*_v181d.*.bak, NewLook_v181d.asi.bak, NFSLight_ini_before_182.bak (the ini not changed by us). Installed.

## 1.82b / 1.83 (2026-10-24): the hitch finder's findings; rain falling, splashes, ripples, the wind
- The hitch finder's log (1.82, a session of ~10 min): (1) every 30 s exactly a ~90 ms frame: "ours round the draws
  70 ms, Map: the driver 66-71 ms (the longest one 65-70)": SkyAfterDraw's sky-constants log line read its copy back
  waiting for the GPU (3-4 frames behind): now read without waiting at a later sky draw (sky.inc). (2) Frames of
  50-150 ms when turning the camera: 40-140 ms of them "the rest" (the game itself, outside every D3D call we hook;
  our parts a few ms; the driver's Present 10-25 ms: the GPU behind): the game's own, as with our effects off (1.81c).
  (3) Once 526 ms in the PS binds (our air pass, its first frame: made once). Now also timed: GetData (the game asking
  its queries: a wait on the GPU would show there), Flush, CopyResource / CopySubresourceRegion.
- Rain falling (rainfall.inc, NewLook.hlsl VS_Rain / PS_Rain / VS_Splash / PS_Splash; kShN 33), drawn in the air pass:
  2 x 24000 streak slots (near 14 x 10 x 14 m, far 48 x 24 x 48 m lattices fixed in the world, wrapped round the
  camera; as many fall as the weather's rain x Rain), each falling 5.5 .. 9 m/s and drifting with the wind; a streak =
  its path over 1/60 s x RainLength as the camera sees it (the camera's own motion halved: the full one was a star
  field at 25 m/s; up to 1.2 m), 2 px wide at least (fainter: its light spread), sheltered ones left out (the rain
  map, one sample), hidden behind the scene (its depth, soft over 0.4 m); lit by the sky above and the light round it
  (the scene's mips, t80: lamps and headlights make it glint). Splashes: 6000 slots, each a burst of 6 droplets on arcs
  + a spreading ring for 0.14 s every 0.35 .. 0.75 s, at a new place each time in a 25 m disk round the camera (more
  near), on the rain map's top surface. Blended dest x (1 - a) + c. The vertex stage gets b5, b13, t51, t79 (saved and
  put back, with the VS / IA the air pass replays after).
- Ripples: RainRipple (cells of 14 cm, a drop each per 0.6 .. 1.2 s, fewer in lighter rain; a ridge running out at
  25 cm/s, fading), its slope added to the reflections' normal on standing water in the open (PS_SSR and its apply).
- The wind (weather.inc): Wind m/s (+ WindStorm x the weather's rain) towards WindDir, gusts wander its strength and
  direction; the rain's drift, the car's drops (the air past the car and the rain's flux: carrain.inc). The camera's
  velocity from its record (P32). LightCB P30 .. P33. Menu: RAIN & WIND (217 .. 225).
- raintest (1280x720, a street): streaks 0.04-0.05 ms, splashes 0.01 ms of GPU; images checked (rain at 1 / 0.4,
  camera 0 / 15 / 25 m/s, wind 3 / 8 m/s): slanted streaks, droplet splashes; 1 px streaks came out dotted: 2 px.
- Not yet: the wind in the trees' leaves (next: find the trees' draws, a vertex-displacement stage after their VS).
- Backups src/*_v182.*.bak, NewLook_v182.asi.bak, NewLook_v182_installed.hlsl.bak. Installed.

## 1.84 (2026-10-24): the player's car from its draws' constants; the dark patch's switch; splashes on the ground; rain speed; rain on the lens
- User (1.83): the car still lost now and then, rarely; then the rectangle round it shows (the dark patch under it in
  the puddles' reflections), sharp body facets, wet-road / rain shading on some parts, shadows and AO broken (its own
  treatment off: its pose off); asked for a switch for the patch in reflections. Splashes seen in the air at some
  angles, none on the ground. Asked for the drops' fall speed and rain on the screen, physical, moving with the camera.
- The log: "paused 300 .. 900 frames (the car not seen)"; its part changed up to 7 times a half minute, 1662 of 1800
  frames without its part drawn (its key mesh comes and goes: LODs); the read-back's prediction drifted while lost.
  Now the player's car is found in the same frame from its draws' constants (our CPU copies, WeCbNow): RainPlayerAfterDraw
  keeps each candidate draw's world matrix (up to 64), RainFrame (once a frame, before the sun pass's constants and
  wherever the wetness binds first) groups them by matrix (a car's parts on its root share one), takes near where it
  was (carried on at its speed; 3 m + 0.5 m a frame lost) the group with the most draws (then the most drawn, then the
  nearest); lost a second: the group with the most draws nearest the camera. Its root = g_playerMat (this frame's: no
  latency), uploaded to t77 (UpdateSubresource), g_playerKey = its most drawn member (the bake's capture). The old
  read-back stays as the fallback. The log line: "found from our constants' copies N times (its group: M draws)".
- UnderCarDark (227, REFLECTIONS): the sky hidden under the player's car (UnderPlayer) on / off.
- Splashes: only where the rain map's top is no higher than 2 m over the eye, and on the surface seen there (the
  scene's depth at the splash, from the vertex stage: t17 bound to it; within 0.3 m + 3 %).
- RainSpeed (226): the streaks' fall x (P32.w); the lens's rain too.
- Rain on the lens (rainfall.inc LensTick / LensDraw, NewLook.hlsl VS_Lens / PS_Lens, b10 LensCB; kShN 35): up to 400
  drops in screen heights, simulated on the CPU every frame: landing by the rain's flux into the view (the rain's
  fall + wind less the camera's velocity, into its forward) + 0.6 (eddies), 6 .. 23 px at 1440 (LensDropSize); gravity
  along the screen (the camera's tilt), the camera's acceleration (they lag), the air past a moving camera outwards
  from the centre ~ v^2 / r; held by their rim ~ 1 / r^2; viscous drag; a running one loses a little and draws a
  thinning trail; touching ones merge; dry with no rain. Each a small lens: the last frame's scene bent through it
  (inverted, smaller, blurred), its rim dark, a sky highlight. LensDrops (228), LensDropSize (229).
- raintest: splashes still on the ground with the depth test; the lens: standing 30 s ~50 drops (held), 20 m/s for 3 s
  ~118 (13 running, blown out); 1 px dots at first (sizes x3). "small" is a macro in the Windows headers (renamed).
- Built while the game ran (its timings then meaningless): installed by a wait-and-copy when it closes (pending184/).
  Backups src/*_v183.*.bak, NewLook_v183.asi.bak, NewLook_v183_installed.hlsl.bak.

## 1.85 (2026-10-24): splashes found on the ground; the lens drops lighter, growing, running, fewer in the middle
- User (1.84): the car not lost any more (driven with the debug view: fine). No splashes on the ground. The lens drops
  just sit, one now and then runs upwards; wanted fewer in the middle of the view.
- Splashes: the rain map's top stands ~0.3-0.5 m over the ground (1.83's splashes hung in the air; 1.84's check of
  the depth dropped them all). Now down the column from 0.3 m over the top to 0.75 m under it (8 steps) the view ray's
  crossing from passing over the surface to under it, the height interpolated, then checked against the depth there
  (an edge in front: none). raintest with the map's top 0.4 m high: splashes on the ground again. A little bigger.
- Lens: the rim dark only at the very edge (1.84 darkened from half the radius: grey discs), the light gathered low
  inside; the drops grow in the rain (by the flux into the lens), hold radius 0.009 (from 0.016): they run; a running
  one wanders a little; the air past a moving camera pushes them out to the sides and down (1.84 outwards every way:
  up above the middle); spawns in the middle 15 % as many (rising to all from 0.25 .. 0.75 of the way out).
- Backups src/*_v184.*.bak, NewLook_v184.asi.bak, NewLook_v184_installed.hlsl.bak. Installed.

## 1.86 (2026-10-24): the camera record's direction (rain in front, splashes, the lens); a lens switch; the wind's research tool
- User (1.85): no splashes; the lens drops all crawl up, an odd shape; asked for a switch for them, a small fix, and to
  focus on the tools the wind in the leaves needs.
- Cause of all three: the game's camera record holds its view rays pointing back (rel = t1 / (depth - t1.w), that
  negative; worldedit.inc builds it so, the flare reads forward as -(A + B/2 + C/2), up as +C). RfClip took A + B/2 +
  C/2 as forward: everything in front of the eye counted as behind (left out), what was behind was drawn mirrored
  through the eye: 1.83's "splashes in the sky", 1.85's white dots over the sky, the rain in front never drawn; the
  lens's gravity "down" was up. The test harness ran the forward convention, so it all looked right there. Now forward
  = sign x (A + B/2 + C/2), the sign from the record's own w (t1.w > 0.5: the game's, back; the harness's t1.w = -1);
  the lens on the CPU the same (a w of 0, from the world editor's record: the game's). raintest now runs the game's
  convention too (GAMECONV): streaks to the vanishing point when driving, splashes on the ground, lens drops falling.
- Lens: LensDropsOn (230, ON / OFF). A running drop drawn as its round head and a thin trail thinning behind it (1.84's
  tapered capsule made two "ears").
- The wind's research (Shift+X, the texture picker): it now also logs "wind pick N: ..." for the draw at the screen
  centre (draw kind, counts / instances, its PS and cut-out or not, its VS: car flags, world / view constants' slots,
  every output: semantic, register, mask, type), its VS disassembly into NewLook_sky.txt, and builds a pass-through
  geometry shader for exactly that VS's outputs (GsSourceFor: elements in register order, the compiler packing them as
  declared), compiles it (gs_4_0) and compares its input / output signatures with the VS's ("the wind can go in there"
  or what differs); and "wind census N: ..." the frame's G-buffer cut-out draws grouped by (VS, PS) with counts.
  gstest (offline): plain, packed (TEXCOORD0.xy + TEXCOORD1.zw in one register, TEXCOORD2.xyz + TEXCOORD3.w) and
  position-last signatures: all compile and match.
- Backups src/*_v185.*.bak, NewLook_v185.asi.bak, NewLook_v185_installed.hlsl.bak. Installed.

## 1.86b (2026-10-24): the wind in the leaves, preparation step 1: the leaves' draws known
- Shift+X x3 (user): the leaves = a cut-out PS "DiffuseTextureSampler | diffuseTintCol" with a VS of 3 outputs
  (SV_Position, TEXCOORD0.xyz, TEXCOORD1.xyz), world in cb1 at 0, 137-193 DrawIndexed a frame; the pass-through GS
  for it compiles and matches. wind.inc: each main-view draw checked (WindIsLeaves: that PS name, cut-out, 3 outputs,
  not a car); its VS noted ("wind: a leaves' VS found"), every 1800 frames "wind: the leaves: N VS, M draws a frame".
  Next (TODO, weather 5): the GS round these draws, the sway. Backups newlook_v186.cpp.bak, NewLook_v186.asi.bak. Installed.

## 1.87 (2026-10-24): the wind in the leaves
- wind.inc: a leaves' draw (cut-out PS with "diffuseTintCol", its VS with 3 outputs, main view) gets a geometry shader
  of ours before the game's draw (WindBeforeDraw in BeforeDraw; off again first thing in AfterDraw): built once per VS
  from its outputs (GsSourceFor, the position named "pos"), it reads the game's VP from the VS's cb0 (at vpOff) and the
  tree's world from cb1 (at wOff): each vertex's world place from its clip position (the packed VP inverted), moved
  (sway along the wind x (h over the base / 8 m)^2 in gusts, each tree its phase; flutter: quick small shakes, each leaf
  its phase), and only the difference added back in clip space (with no wind exactly the game's vertex). The game's own
  GS there: left alone. WindLeaves (231), WindFlutter (232) in RAIN & WIND (0: off). b2: wind, time, the two settings.
- The log: "wind: the leaves: N VS, M draws a frame, K with our GS"; "wind: the leaves' GS made for VS ...".
- windtest (offline): a VS laid out as the game's leaves' (VP in cb0 at 16, world in cb1 at 0, 3 outputs): the GS made.
- Not yet: the leaves' shadows (their shadow-pass draws) do not sway; the menu's double refresh (user) to look at.
- Backups src/*_v186b.*.bak, NewLook_v186b.asi.bak. Installed.

## 1.88 (2026-10-25): the rain and the car's drops easier to see; rain lit in the headlights
- User (1.87): the leaves move, fine (their shadows: not now). The rain hard to see at times, the car's drops only up
  close; wanted the rain seen in the headlights' cone as in reality, without looking overdone.
- Streaks: alpha x1.3 (near 1.8, far 0.9), lit a little more by the sky and the scene round them; RainBright (233) x.
- The car's drops: CarDropsVisible (234, 1.6): a drop's coverage of its pixel x this (far ones, mostly spread over
  their pixel, are seen; near ones unchanged once covering it), on the glass too.
- Headlights: in VS_Rain two spots at the player's car's nose (car-local +-0.7, 0.7, 2.2 m along +z, a little down,
  ~25 deg, ~45 m, 1 / (1 + d^2 / 120)) from its pose (t77, now bound to the vertex stage too); the PS adds warm light x
  RainHeadlights (235) x 6 / (1 + 30 x the sky's luminance): bright at night, faint by day. LightCB P31.y, P33.x / .w.
- Backups src/*_v187.*.bak, NewLook_v187.asi.bak, NewLook_v187_installed.hlsl.bak. Installed.

## 1.88b (2026-10-25): the rain in the headlights from the game's own lights; the cost checked
- User (1.88): the lit drops pure white, not like drops caught in a beam, and lit with the headlights off too.
- Cause: 1.88's two guessed cones from the car's pose, x6 on a dark sky, and no check that the lights are on.
- Now VS_Rain lights a drop by the game's own headlight spot (headlight.inc's copy: HeadlightCB b8, pattern t43, s13,
  bound to the vertex stage in RainfallDraw, restored after): inside its frustum, pattern x the game's attenuation x
  its colour x 1 / (1 + d^2 / 400), out in TEXCOORD2; PS_Rain adds it x (0.2 + 0.6 x the drop's own random): some
  drops glint more, tinted by the lamp. P31.y = RainHeadlights only while the game draws its spot (HlOnNow: captured
  this or last frame), else 0. The t77 pose binding of 1.88 removed from the rain (not needed now).
- raintest: RainBright (P33.x) was 0 there since 1.88 (no streaks drawn: fixed, P33.x = 1, P33.w = 1.6); HL=1 = the
  game's spot as logged in 0.62 (att 0 / 0.25 / 10, colour 11.5); HLSL=path overrides the source. Middle of the view
  (background 0.05): mean 0.0579 -> 0.0604 with the lights, brightest 0.350 -> 0.394 (1.88 was ~x12 that).
- Cost: rain at 1280x720 splashes 0.014 ms + streaks 0.035 ms (headlights +0.00); windtest now times the leaves' GS:
  300k triangles +0.09 ms (0.3 ns a triangle; the game's 41-50 leaves draws a frame: well under 0.1 ms).
  Game log (1.88): car rain CPU tick 0.17-0.28 ms, glass 0.04-0.11, read-back 0.04-0.26; frames GPU-bound (GPU
  frame 15.8-19.9 ms of 16.7-20.5); mod GPU 7.4-8.7 ms mostly the older passes (half-res 2.2-2.6, sun 2.0, snap
  1.4-2.0, reflections 1.2-1.6); our CPU round the game's draws 4.5-6.3 ms a frame (sun shadows 1.2, world editor 1.5).
- Backups src/*_v188.*.bak, NewLook_v188.asi.bak, NewLook_v188_installed.hlsl.bak. Installed.

## 1.89 (2026-10-25): less sun through a thick cloud deck; the weather's kinds (presets), its walk, the change's time
- User (1.88b, storm by hand): the sun shines through the overcast too strongly (a bright patch round it on the
  clouds); the world's own light fine. Asked to prepare weather presets: rain variants, dawns with and without haze,
  clouds flowing between states, with a pattern, and a way to switch the weather / pick one.
- Sky (sky.inc): CloudDeck() = saturate((cover - 0.75) / 0.2) x (0.6 + 0.4 x rain clouds); SkyOvercastSun (236, 0.3)
  in SK12.x (SkyCB 192 -> 208 bytes, both buffers). Under a deck: the sun's light in the clouds x lerp(1, it, deck),
  their phase flattened (x0.85 towards isotropic: no bright patch), extinction x (1 + 0.8 deck (1 - it)); the game's
  glow behind x the same, our sun's disc x its square. The world's cloud shadows (CloudShadowCalc) untouched.
- Weather (weather.inc): 12 kinds (clear, clear misty dawn, fair, hazy humid, cloudy, overcast, fog, drizzle, rain,
  downpour, storm, after the rain): clouds, rain, and x the hand-set: drop size (219), their fall (226), wind (222),
  gusts (224), haze, the dawn's haze (149); fog (max with WeatherFog), cirrus +. The weather now owns 219 / 222 /
  224 / 226 / 149 too (kept and put back as the others). WeatherKind (237): 0 by hand (rain / clouds as before), else
  a kind: picked by hand = there now; by itself it walks the kinds' ways (each kind's next list), weighted by share x
  the hour (WxTod on our clock, sunshadow.inc WxHourNow: misty dawn 4-9 h only, fog night / morning, storms 13-21 h,
  stays a fifth), its phase x the kind's minutes; the menu shows where it goes. WeatherChange (238, 2 min): clouds
  and the kind's values with a time constant of a third of it, the rain half of that (was 40 s / 20 s fixed).
- wxtest (our clock a day per 2 h, 40 h): clear 12, misty dawn 3 (morning only), fair 25, hazy 11, cloudy 18,
  overcast 9, fog 2 (mostly morning), drizzle 5, rain 8, downpour 2, storm 1, after 4 %; raining 16 % of the time.
  Storm picked from clear: clouds 0.56 / 0.90 / 1.00 at 30 / 90 / 210 s, rain 0.81 at 90 s, wind 1.8 -> 6.6 m/s,
  gusts 0.28 -> 0.80, drops x1.25; weather off: the hand-set values back. The sky's 4 shaders compile (wxtest).
- Backups src/*_v188b.*.bak, NewLook_v188b.asi.bak, NewLook_v188b_installed.hlsl.bak, NFSLight_ini_before_189.bak.
  The game was running: installed by a wait-and-copy when it closes (NewLook_v189_pending.*).

## 1.90 (2026-10-25): the rectangle round the car; the player's car picked by its mesh and its place in the view
- User (1.88b, screenshots: views wetness, reflection source, car rain): a sharp rectangle round / beside the car, in
  different projections, where wetness and reflections are cut out (UnderCarDark off did not hide it); the car's
  detection jumps to other cars after changing the car or in a race.
- Rectangle: OnCar (2.9 x 8.7 m round each car in RC, from 0.06 m over its place) kills wet and puddle in the
  reflection passes (and relief); the car's place (its root's origin) sits at the road or under it (view 37's red:
  the road inside InPlayerBody, > 0.05 m over the pose): the road round the car was its "body". Now OnCar from 0.3 m,
  OnPlayer (history) from 0.2 m, InCabin from 0.4 m (was 0.25).
- Pick (rain.inc RainFrame): 1.84 when locked took the group with the most draws within 3 m (a rival rubbing or
  beside, with more parts, won), when lost the car nearest the camera (a parked car at a jack spot). Now: each
  group's mesh (its biggest draw) and its place in this frame's view (the main view's VP from a car draw's own
  constants (WeCbNow at vpSlot), clip x, y, w ~ metres): locked: the group with the player's mesh (anywhere: a
  teleport), else the nearest (draws only a small tie-break); lost: the group nearest the player's place in the view
  (learnt while followed, quickly near it, slowly when far with no other car there; a chase camera's guess before);
  within 10 s of finding it again (or its mesh changing) another car steadily (20 frames) within 0.8 m of that place
  while the picked one is > 2 m off replaces it. Log: "player car: found again", "another car at its place".
- picktest (new): the player's car followed, a rival 2.5 m beside with 12 parts, rubbing at 1.2 m, a teleport, a new
  car at a jack spot with a parked car nearer the camera, a wrong pick while the camera was elsewhere: all car 0 (the
  last replaced after 20 frames); 1.89's rule took the rival from the first case on.
- Backups src/*_v189.*.bak, NewLook_v189.asi.bak, NewLook_v189_installed.hlsl.bak, NFSLight_ini_before_190.bak.
  The game was running: installed by a wait-and-copy when it closes.

## 1.91 (2026-10-25): the pick held on a car part (the rectangle all the time); the glow round the sun in a storm
- User (1.89 / 1.90 screenshots): SkyOvercastSun changes nothing, the sun still glows through a full storm deck; the
  rectangle by the car now there all the time.
- Rectangle: the 1.90 log "its group: 1 draws": the pick held by its mesh on a part with its own matrix (a door, a
  wheel), so the car's pose (t77: UnderPlayer, the boxes, the drops) sat off the car. Now only a group of 3+ draws
  holds by its mesh, the mesh is only taken from such a group, a part picked moves to the nearest group of 3+ draws
  within 2 m (not the one with the most: a rival rubbing its side is about as near), the place in the view is learnt
  only from such a group; locked, the draws' tie-break 0.6 m^2 a draw (was 0.3). Log: "a part's pick moved to its
  car's root N".
- Storm glow: from the haze's forward lobe round the sun (AirLight's mie), not from the sky draw (1.89 dimmed only
  that). Now the lobe x (our clouds' shadow map at the camera toward the sun)^2 x lerp(1, SkyOvercastSun^2, deck);
  the air pass binds the sky's constants and the cloud shadows (SkyBindForSun). AirLight takes the camera's
  position (was its height).
- picktest: rubbing now 1.95 m (1.2 m overlapped the cars; a first try by "the most draws within 2 m" took the rival
  there); new case: the pick stuck on the car's door (1 draw): car 0 again within 5 frames; all cases car 0.
  PS_Air / PS_Sun compile (fxc ps_4_0 with the clouds' common code).
- Backups src/*_v190.*.bak, NewLook_v190.asi.bak, NewLook_v190_installed.hlsl.bak, NFSLight_ini_before_191.bak.
  The game was running: installed by a wait-and-copy when it closes.

## 1.92 (2026-10-25): no lamps found on screen, no adding lamps by key; the game's sun glare under our clouds
- User (1.91 screenshots: storm, cover 0.95, SkyOvercastSun 0; a dawn): with the haze off the sun still glows through
  the clouds, SkyOvercastSun changes nothing, morning or day. Asked: take out the lamps found on screen and the key
  that adds them to the map (pressed by mistake: "added lamp 4012"), and remove the added ones.
- Lamps: the log shows 4010 lamps loaded at the start and two added this session (01:12:52, MapAddAtAim twice: 4011,
  4012, the same place, the last lamp's settings); the map had ~4000 lamps for days (NFSLight.log of 10-24: 3961 ..
  4020). The two removed from NewLook_lamps.txt (4010 left; before: NewLook_lamps_before_192.txt.bak). Code: the
  lamps found on screen no longer kept or used (LampsUpdate, MapLearn gone; the finder's pass stays: it brings the
  camera record and the aim point), lamps used = the map only (LampSource / LampAutoLearn out of the menu), Shift+K /
  the middle button (add at the aim) and Shift+L (learn the found ones) gone; Shift+J / Del delete and undo stay.
  Templates (AutoLamps, lamps on every post of a set-up kind) stay.
- Glare: the game's sun flare glare (VS 1766 / 1803, ours since 1.31) is drawn over the whole frame after the bloom;
  the game knows the sun as seen from the depth only (the sky there), so our clouds never hid it. Now its light x
  (our clouds' shadow map at the camera towards the sun, read in the VS, bilinear)^2 x lerp(1, SkyOvercastSun^2,
  deck) (FlareCB 96 bytes: FL4 the map's origin / texel, FL5.x the deck's factor; t50 bound to the VS and restored).
  The glare VSs compile (fxc vs_4_0). If a glow stays: the game's corona on its fog shaders (SunCoronaDirection) next.
- Backups src/*_v191.*.bak, NewLook_v191.asi.bak, NewLook_v191_installed.hlsl.bak, NFSLight_ini_before_192.bak.
  Installed (the game closed).

## 1.93 (2026-10-25): the game's sun corona (its air scattering) under our clouds
- User (1.92 screenshot: custom, cover 1.00, SkyOvercastSun 0, the whole sky and the hills washed orange round the
  sun): the same; the corona, as guessed.
- SHADERS1.BNDL scanned for "SunCorona" (scratchpad corona_scan.py: DXBC blobs in the zlib streams, D3DDisassemble):
  6 shaders. 5 PSs (the game's air scattering: gGlobalCB ScattCoeffs / ScatteringColor / ScatteringSettings (0.85,
  0.0033), some LayerFogCoeffs; one water / reflection one with KeyLightColour): the glow = ScatteringSettings.y /
  (1 + ScatteringSettings.x dot(view, SunCoronaDirection))^2 x ScatteringColor x the fog's amount, one of them with a
  16-tap radial blur towards the sun (shafts); the direction used only in that dot. 1 VS: the direction through
  ViewProjection to the screen (x / w) for the blur's centre.
- So the corona's direction (our sun since 1.30, CoronaCopy) is now shortened by f = (our clouds' shadow map at the
  camera towards the sun)^2 x lerp(1, SkyOvercastSun^2, deck), f >= 0.02: the peak (1 / 0.15^2 = 44x the side)
  flattens into the even haze (f 0: 0.0033 everywhere); the VS's x / w unchanged. The map's texel read back (sky.inc
  CsSunHere: 1x1 R16F staging ring of 3, DO_NOT_WAIT, smoothed 0.15 a frame; the sky off or no cloud shadows: 1).
- Backups src/*_v192.*.bak, NewLook_v192.asi.bak, NewLook_v192_installed.hlsl.bak, NFSLight_ini_before_193.bak.
  Installed (the game closed).

## 1.94 (2026-10-25): the game's own reflections in our sky's colour; the reflected sky from the probe, not old views
- User (1.93: the corona fixed, "отлично"; screenshot: a glass tower reflecting a sunset in a storm): the game's own
  reflections stay where ours do not reach; and the reflected sky (our pass) often old: the camera does not show the
  part of the sky needed.
- The game's reflections: SHADERS1 scanned for "SkyReflectionAverage": 76 PSs (G-buffer materials: windows
  (LitWindowColour), walls (MaterialDiffuse, lightmaps), decals; the forward glass with reflection_amount), all with a
  cube "ReflectionTextureSampler": reflection = cube.rgb x SkyReflectionAverageLight + cube.a x
  SkyReflectionAverageColour (the game's time of day, no weather), baked into the G-buffer's 4th target (sqrt) or the
  frame. New skyrefl.inc: the buffer (gGlobalCB, found by reflection: slot and both offsets) swapped at their draws for
  a copy with both colours = their own luminance x our sky's colour (the sky probe's 1x1 mips of its 6 faces read back
  every 15 frames, never waiting, smoothed) x a deck's darkening (1 - 0.55 overcast), by GameReflOurSky (239, 1) x
  our day (1 - the game's key light's share): the night as the game's. Copies cached per buffer (its contents' hash,
  our values' version). Log: "game's reflections (skyrefl.inc): N shaders, M draws tinted".
- The reflected sky (PanoSky): the seen panorama now fades with a ~6 s half (was ~60 s); its own detail only while
  fresh (weight (a - 0.25) x 2, was a x 1.5); the probe's colour matched to the seen sky by how much of it was seen of
  late (x seen share x 20): nothing seen lately, the probe (our sky now, all round) as drawn.
- All 28 PSs compile (fxc). Backups src/*_v193.*.bak, NewLook_v193.asi.bak, NewLook_v193_installed.hlsl.bak,
  NFSLight_ini_before_194.bak. Installed (the game closed).

## 1.95 (2026-10-25): our sun up in the game's night (the clock taken from the moon's azimuth)
- User (1.94: "стало лучше"; screenshot: the night, lamps lit, sharp sun shadows and sunlit walls): the sun shines at
  night.
- Log: 02:35 the game's day ended (21:28, its sun 0.81 -> the moon); 02:35-02:41 its light jumped 5..35 deg many times;
  each jump took the phase anew from the azimuth ("goes the night's way: its phase 1.000"), our clock 08:30..12:00,
  our sun up, under "key light 0.0x-0.4, the moon". The moon is off the sun's line (azimuth 56..114, the line about
  -37.6 +- 35): its azimuth clamps to the line's end = the dawn's turn = morning.
- Now (sunshadow.inc): not sure of the way and the game's light the moon (under 0.6): no phase from its azimuth: our
  night kept, else an hour after our sunset (SpPhaseAtHour: the clock's inverse). Sure, and 3 s of the game's moon
  with our sun over 5 deg (or its strong sun, over 2, with ours 5 deg under): the clock to an hour after our sunset
  (sunrise), once in 20 s ("sun path: the game's moon while ours is up (..): our clock to ..").
- suntest: new case (the night, our phase put at noon): our moon within 1 s; its 6 old checks (from 1.30) fail on
  1.94 as well: the harness is out of date (its moon fixed at azimuth 120, not on a line), to redo.
- Backups src/*_v194.*.bak; the game was running: installed by a wait-and-copy (NewLook_v195_pending.*).

## 1.96 (2026-10-25): the menu for players (simple / advanced, sections on / off, quality); the quick bench (B); AMD
- User (Zelef): the menu easier for players (simple settings and an advanced view with every one), each section
  switched off whole, quality presets, the items regrouped by meaning; extra checks that cost out of the code; a
  bench players can read (2-3 min, every effect, day and night). Nexus (a Nexus player (AMD)): RX 9060 XT at 1080p, 120 FPS
  (capped) without the mod, ~15 with it, the game's AA off. Zelef's RTX 3080: slow only with the game's AA on.
- New sections.inc: 14 sections (sun & moon, sky, weather, shadows, AO & bounce, air, reflections, wet roads,
  materials, street lamps, car lights, rain effects, wind, colour), each a switch (params 242-255, Sec*) with a
  "kill" list set to off while it is off; the player's own values kept aside (g_secKeep; the ini and the menu show
  them: UserVal / UserValue); SecEnforce every frame after WeatherTick (the weather drives some of them), after a key
  and LoadIni; a setting of a section that is off changed: the section on again. Rain effects off also zeroes the
  splashes and the puddles' rings (220, 221: they were left on).
- Quality (240): Low / Medium / High (= the defaults) / Ultra over 24 costly settings (shadow map size and reach,
  half-res mask, city height map, kept shadows, sky occlusion, bounce, reflection size / rays / early source,
  clouds, lamp half res / maps / screen shadows / budget / count, shafts, relief, parallax, leaves, lens drops);
  "custom" when they match none (its values kept: QualityCustom= in the ini).
- The menu (menu.inc): MenuAdvanced (241) 0 = simple: each section a line (ON / OFF) with its 2-3 main settings in
  plain words under the selected one; 1 = advanced: a heading per section, the open one's every setting under
  sub-headings, a window of 26 rows. A line under the menu says what the selected section is. A warning when the
  game's own AA is on (it draws 2x: 2560x2880 for 2560x1440, every effect costs that much more).
- Quick bench (bench.inc, B; Ctrl+B the selected setting, Shift+B the old run over every effect): scenes day 13:00,
  night 23:30 (the map's lamps lit), rain 14:00 (only with WEATHER on: a downpour forced, the ground soaked); per
  scene windows of ~1.4 s after 0.6 s (presets 1.5 s; a scene 3 s): a reference every third, the mod off, each
  section that is on switched off, by day each preset; vsync off; medians of the CPU frame, the GPU span, our passes,
  the wait in Present. A section's cost = the references round it - its window. Report scripts\
  NewLook_bench_report.txt (the GPU, its driver, the AA warning, a table per scene, the heaviest three, every
  window); the costs next to the sections in the menu; the log "quick bench:". Ultra skipped under 700 MB free
  address space.
- AMD (shaders): fxc showed dcl_indexableTemp (arrays indexed at run time: scratch memory on AMD) in PS_Sun, PS_Snap,
  PS_Lamps and PS_Tiles, all from LampShade (the 4 strongest lamps in arrays, the zones' values [16], the tile's
  words [8]), LampGlow (words [8]) and PS_Tiles (words [8], written by index). Rewritten: the 4 in vectors and named
  variables (selects), the zones as bit masks (a soft edge computed again), the words picked by selects; the 4 soft
  shadows in a loop instead of 4 copies. fxc: no indexable temps left; PS_Sun 11223 -> 9150 instruction slots,
  PS_Lamps 3526 -> 1469, PS_Snap 1185 -> 626 (more plain temps: what was in scratch now in registers).
- CPU per draw: Immediate() remembers the contexts (a wrapper's immediate one, the last deferred one) instead of
  GetType ~20 times a draw; the wind's "leaves?" kept per pixel shader (the name list was searched at every
  alpha-tested draw), nothing asked while the wind is off; the game's reflections' tint (skyrefl.inc): the constant
  buffer's copy's fill number (now unique among all buffers: g_cbsFill) instead of hashing all its bytes at every draw,
  the last shader's entry kept.
- The log: every 10 s "cpu:" (an average frame's parts: the game's draws, ours round them, binds, Map, copies,
  queries, Present (the driver: vsync / GPU queue; ours), the game's own); once "gpu:" (adapter, memory, vendor,
  driver version, quality, the game's picture size).
- Builds (C4018 in texreplace.inc as before); all PS variants compile (fxc). Not yet run in game. Backups
  src/*_v195.*.bak, src/NewLook_v196_before_shaders.hlsl.bak, NewLook_v195.asi.bak, NewLook_v195_installed.hlsl.bak,
  NFSLight_ini_before_196.bak, TODO_v195.md.bak. The game was running: installed by a wait-and-copy
  (NewLook_v196_pending.*) when it closed, 19:28.

## 1.96b (2026-10-25): the dawn clock, the moon's shape, sections all off = the game's look, the bench by the game's own clock
- User (Zelef), 1.96 in game: (1) at 04:30-05:00 by the mod's clock the sun began to rise and the clock went back to
  01:00; (2) the moon drawn out sideways when the camera turns away from it; (3) every section off and something still
  changes the picture ("PBR with colour grading"); (4) the bench's day only switched our sun on, the map's lamps stayed
  lit (the game's night): his report was measured at night; (5) keys: the quick bench on Ctrl+B, the selected setting
  on Shift+B, the run over every effect taken off (the same as the quick bench).
- (1) The 1.95 guard (the game's moon while our sun is up: our clock to an hour after sunset) fired at dawn: the log,
  19:48: ours 4.7 deg up at 04:26 while the game's key light was its moon at 0.50 and rising (0.035 per 10 s; ours
  0.45 deg per 10 s). Its threshold 5 -> 12 deg (both ways): the game's light passes 0.6 at ~6 deg of ours.
- (2) VS_Moon (sunshadow.inc kFlareMoon) moved the game's moon quad on the screen by (our moon's place - the game's):
  the quad kept the perspective stretch of the game's moon's place (near the sides of a 32:9 screen). Now each corner's
  view ray is turned on the sky by the rotation taking the game's moon to ours (Rodrigues, axis g x o) and projected
  again (DirUv): the disc's size and shape on the sky kept. fxc vs_4_0: compiles.
- (3) Settings in PS_Sun's look that no kill list covered (neutral values while the section is off): TIME + 9 -> 1,
  14 -> 0; SHADOW + 12 -> 1, 13 -> 0, 134 -> 0; AO + 8 -> 1, 15 -> 0, 38 -> 1; REFL + 173 -> 0; WET + 110 -> 1; CARL
  + 72 -> 1. And SecSunPass(): our sun pass (the game's sun light drawn again by PS_Sun) only while a section drawn in
  it is on (TIME, SKY, SHADOW, AO, AIR, REFL, WET, MAT, LAMPS, CARL, RAIN); all of them off: the game's own picture
  (COLOUR's grade, WEATHER, WIND are their own passes). The flare (FlareBefore) needs this frame's camera record:
  with the pass off it leaves the game's own.
- (4) New gametime.inc: the game steps its clock at NFS13.exe 0x8B4C28 (movss [esi+0x2A84], xmm0; its environment
  object, the time 0..86400, its midday 50000; HD Effects hooks 0x8B4C0A / 0x8B4C1A, not this): a 5-byte jump (+3
  NOPs) to a cave keeping esi and doing the same store (no flags touched: the je at 0x8B4C30 reads cmp 0x8B4C00's).
  Only when the 8 bytes are the expected ones (log "game time: its clock hooked" or why not). TodGet / TodSet.
  The quick bench rewritten on it: day = the game's 50000, night = 02:00 (7200), held every frame; rain = midday with
  a downpour; the time put back after (and on a stop). Clock not found: one scene "now" (the report says so: run it
  again by day / at night). P_CLOCK / P_LAMP_DAY no longer touched.
- The bench's windows: a reference before and after each (day: the mod off, each section on but WEATHER / RAIN; the
  street lamps only at night and last: their shadow maps fill again for seconds after; night: SKY, SHADOW, AIR, CAR
  LIGHTS, LAMPS; rain: RAIN, WET, REFL), presets at the end of the first scene; a scene settles 4 s, a reference after
  a window 1.0 s, a preset 1.5 s. A section's cost: max(the CPU frame's, the GPU span's) against the references round
  it (their mean; the one after far from the one before (6 %, 0.8 ms): the one before alone). The 1.96 report's costs
  were inflated (the game's own 60 FPS cap hides the CPU frame; the references stayed up after the lamps came back).
  The report: the game alone with its GPU work (and "its own frame cap" when that is what limits it), the noise, '.'
  for a section not measured in a scene.
- Keys: Ctrl+B the quick bench, Shift+B the selected setting (not in the lamp-zone view 24: there it adds a zone), B
  (with or without them) stops a run; plain B is the game's again. Ctrl+Shift+B (bench viewpoints) and the old run over
  every effect taken off the keys (the code is still there). The menu's key lines (simple, advanced) said so.
- The version shown: 1.96b (the log, the menu's title, the report).
- Builds (C4018 as before). Not yet run in game. Backups src/*_v196.*.bak (sections, bench, menu, sunshadow,
  newlook.cpp), NewLook_v196.asi.bak, NewLook_v196_installed.hlsl.bak, NFSLight_ini_before_196b.bak, TODO_v196.md.bak.
  The game was running: installed by a wait-and-copy (NewLook_v196b_pending.*) when it closed, 20:09.

## 1.96c (2026-10-25): our clock by the game's day and night; no moon samples; the bench puts our path back. FPS Unlock tried beside
- User (Zelef): the FPS cap off before testing: MW2012 FPS Unlock (Krashgamer, Nexus 124 / github.com/Krashgamer/
  NFSMW2012FpsUnlock, MIT) looked at, integrated "for now" or our own. Read its source (not copied): it replaces the
  game's frame pacer (NFS13.exe 0xAC82B0: a busy wait for the 60 Hz tick) with ticks owed by real time, render-only
  frames between; interpolates cars, camera, driver, lamps, props, debris, effects, readouts between the last two
  ticks; a render-worker barrier, bounds checks, a crash guard, DXGI latency 1, streaming in the tick's order (crashes
  in tunnels otherwise); borderless window. Its patch sites (0xAC82B0, 0x118371C, 0x10E6D68, 0x11ACDE4, 0xDA07A0,
  0xDA3710, 0xA5D620.., 0x476FD6, 0x47E1E0, 0x404D00, 0x8C33BC, 0xD97D80, 0xD9BB20, 0x46F987.., 0x41AEF0..,
  0x534B4D.., 0x791818, 0x7B53C0, 0x49BC60, 0x4A20A0, 0x5F92C5, 0x8E14C0, 0x5356FA, 0x53DB60, 0x4B1F3E, 0xA9A370,
  0x550252, 0x8A03BA, ...) do not meet ours (0x8B4C28). Chosen with the user: not in our code; its v1.0.0 asi beside
  ours (the user downloaded it from Nexus: the same 1,088,232-byte zip as the GitHub release): scripts\
  MW2012FpsUnlock.asi, .ini (VsyncInterval=0: the game shows no vsync setting; Borderless=1), its README and LICENSE;
  its dinput8.dll (9.7.4) and global.ini not taken (ours: the loader 1.0.0.0 from HD Effects, DontLoadFromDllMain=0).
  Nothing of it to take out at release. Our eases stepped per frame (faster over 60 FPS): TODO.
- User: "a rewind" of the clock at some moment. Log 20:46:34: "the game's moon while ours is up (05:32): our clock to
  an hour after sunset" (01:00): the user's ini has SunriseHour 3.75, SunsetHour 24: our sun up 2 h 15 before the
  game's comes on (our 06:00): the 1.95 guard (our sun's height, 12 deg in 1.96b) is wrong for any sunrise the player
  sets before the game's. Now by the game's day and night: SpWellIn(phase, day): 1.5 h of our clock inside them, from
  the learnt switches (aOn, aOff); the game's moon while our clock is well in its day: to an hour after its sun goes off
  (aOff + 1/24); its strong sun while ours is well in its night: an hour after it comes on. The not-sure branch at night
  the same (our phase kept unless well in its day; was: kept only with our moon up, else our sunset + 1 h).
- And 20:45:53 -> 20:46:03: our clock 03:17 -> 05:17 at once, unlogged; before it "lost the game's day (0.15 off its
  line)" every ~9 s through the night (20:42-20:45). The game's moon's azimuth is off the sun's line: its samples
  (within 0.06 of the line) bent the fit (over 3 s of span: its slope) and the shown phase went to the line at once
  (over 0.1 off). Now: samples, the lost check and the turns by day only (the night runs at the speed kept; the turns
  looked for anew by day: the dawn's mark is missed after a night, the samples set the line); the at-once move logged.
- The quick bench put the game's clock back but not our path (its readbacks of the bench's light: "jumped", then the
  night's way from a day phase). Now our SunPathState kept at its start and put back with the clock; the game's light
  taken as still 1.5 s after (read back late).
- "Mod off (Y): the sky and sun look pale, not the game's own": every per-draw change checked: all gated by g_enabled
  (or by g_sunActive / skyOn, which are); no shader is changed at its making; weather.inc only moves our params;
  HD Effects changes no colours. At the user's screenshot (20:54:54) our clock 09:28 (sunrise 03:45: our sun 36.5 deg)
  while the game's own light was its early morning (its phase 0.04, 28.5 deg, key light 2.28 against 6.4 at noon): Y
  shows the game's own early-morning sky; and its exposure, set for our brighter picture, takes seconds to follow
  (Y was pressed ~every 0.5 s). Asked the user for an A/B at one time (F8 midday: Y off, and without NewLook.asi).
- The log's "exception c0000005 at NewLook.asi+2c24f" (20:45:56, the game's AA turned on: 2560x2880): PropScan
  (proplink.inc, its thread reading the game's memory for lamp posts) met memory the game freed; its __try handles it
  (the log's vectored handler sees it first). Found with a map file of the same build (identical but for 4
  timestamp bytes): scratchpad mapbuild.bat (cl ... /link /MAP).
- The version shown: 1.96c. Builds (C4018 as before). Not yet run in game. Backups src/*_v196b.*.bak (sunshadow,
  bench, newlook.cpp, menu), NewLook_v196b.asi.bak, NewLook_v196b_installed.hlsl.bak, NFSLight_ini_before_196c.bak.
  Installed 21:05 (the game closed).

## 1.96d (2026-10-25): our clock is the game's own; the bench's report and night; the weather line's refresh
- User (Zelef), 1.96c with FPS Unlock, the game's AA off: ~200 FPS without the mod. (1) The clock still jumped (21:xx
  to 07:00); why move it at all: let it run as the game's, or is it for the bench and F8? (2) The bench's night had
  daylight. (3) The weather's values in the menu change very often. (4) The bench's way: one section off with all the
  rest on; would all off and one on be more right? COLOUR alone took 200 -> 185 FPS.
- (1) Since 1.96b we read the game's clock (gametime.inc, [esi+0x2A84], seconds of its day). Its sun comes on at
  ~06:00 of it and goes off at ~20:45 (from the azimuth fit: its midday scene 50000 s at phase 0.245, sun on at phase
  0.92, off 0.535: 05:58 and 20:44): our clock's 06:00 / 20:45 already. Now our clock is the game's: SunPathStep with
  the clock known (tod = eased && TodKnown()): out = SpPhaseAtHour(game s / 3600 + ClockShift); the azimuth's line,
  the restarts, the at-once moves and the guards only without it. F8 (HD Effects) and the bench move ours with it, at
  once. gametime.inc now included before sunshadow.inc. The log: "game time: its sun comes on / goes off at its
  hh:mm" (to check the 06:00 / 20:45). The log's jump (21:18:58, "the game's sun while our clock is in its night
  (20:17)") came after 8 F8 presses.
- (2) The night scene (the game's 02:00) got our clock from the not-sure branch: an hour after the game's sun goes
  off (21:45), and the user's sunset is 24:00: our sun up. Gone with (1): the night scene is 02:00 on ours.
- The report said "the game's clock: not found" while the log said "set": QbRestore clears g_qbTodSet before
  QbReport; now g_qbTodUsed for the report.
- (3) The menu's text was made anew every 30 frames while the weather ran (7x a second at 200 FPS) besides every 500
  ms: the frame-counted one taken out.
- (4) Answered: one off with the rest on is what switching it off saves (the shared work, the sun pass, its copies
  and half-res buffers, stays with the rest); one on alone would charge the shared work to whichever is alone.
  Offered an "alone" column as well. COLOUR: 200 -> 185 FPS is 0.4 ms (at 78 FPS the same 0.4 ms is ~2 FPS).
- Builds. Not yet run in game. Backups src/*_v196c.*.bak (sunshadow, bench, menu, newlook.cpp), NewLook_v196c.asi.bak,
  NewLook_v196c_installed.hlsl.bak, NFSLight_ini_before_196d.bak. Installed 21:22 (the game closed).

## 1.96e (2026-10-25): each section's share of NewLook's cost; its FPS from its own scene
- User (Zelef): no "alone" column; show each effect's share in percent. His 1.96d bench (RTX 3080, 2560x1440, the
  game's AA off, WEATHER off): the game 5.2 / 5.7 ms (day / night), NewLook 13.4 / 20.4 ms (75 / 49 FPS; he sees ~40 at
  night while driving, the menu open: GPU 24.8 ms there). Lamps 6.1 ms at night, sky 1.4 / 3.4, reflections 1.7, wet
  1.4, AO 1.1. Off costs more: SUN & MOON -2.2 (day), SHADOWS -1.3 and AIR -1.2 (night). Ultra = High (13.7).
  The day noise 0.87 ms (references 12.9..14.7).
- The menu's line showed the street lamps' night cost against the day's FPS ("76 -> 141 FPS off"): now each cost
  with its scene's frame (g_qbCostSc, g_qbAllMs) and "at night" said; its share of NewLook's whole cost in that scene
  (g_qbModMs: max of the CPU frame's and the GPU span's difference to the mod off). The report's table: "1.4 ( 17%)".
- The game's clock: "its sun goes off at its 23:36" twice and "comes on at 13:53" (the bench's jump): 23:36 is not
  the expected 20:45: F8's dusk? To see over a whole game day without F8.
- Builds. Backups src/*_v196d.*.bak (bench, menu, newlook.cpp), NewLook_v196d.asi.bak. The game was running:
  NewLook_v196e_pending.asi, a wait-and-copy installs it when it closes.

## 1.96f (2026-10-25): the sky timed in parts; a cheaper cloud march (SkyFast)
- User (Zelef): at night ~60 FPS with the sky off, a bit over 40 with it; he likes its look: no visible loss.
  His SkyCloudRes=2 (80%: the Ultra level, ~2.5x the march pixels of High's 50%), SkyQuality 0.5.
- The GPU profiler had the sky only as "sky probe": now also "clouds" (the march + resolve, PR_CLOUDS), "cloud shadows"
  (the 256^2 map, PR_CSHADOW) and "sky" (the sky draw itself: begun at the end of SkyBeforeDraw, ended in AfterDraw by
  g_skyDrawTimed); in the overlay's mod sum and the bench's kMod.
- New param 256 SkyFast (default 1, SKY section's advanced list; SK12.y): in clouds() the steps grow far off
  (x clamp(t/8000,1,4), the alpha by the step's length), 4 samples toward the light by day and 2 at night (were 5,
  the same 0..1 km spread), only the first with the fine detail (were 3), out at T < 2% (was 1%). 0: as before.
- fxc clean (clouds, main, resolve, shadowmap); builds. Backups src/*_v196e.*.bak (newlook.cpp, sky, bench, menu,
  sections), NewLook_v196e.asi.bak, NFSLight_ini_before_196f.bak. Installed 21:47 (the game closed). Not yet run.

## 1.96g (2026-10-25): SkyFast gentler (1.96f's look changed); no white 1-pixel rims on reflections' edges
- User (Zelef), 1.96f, dusk by the bridge: SkyFast 0: 52.6 FPS, GPU frame 18.31 ms; 1: 62.9 FPS, 15.32 ms (-3 ms).
  But the look changed: with 1 the bright veil of cloud by the sun was gone, the clouds darker and grainy. The
  overlay's GPU line ran past the menu (the sky's timings not seen): now wrapped (before "overlay" and "clouds").
- Why (read from the shots): the grown steps through the gaps skipped thin cloud (the jitter: grain); the light
  samples without the fine detail made the clouds denser to the sun: darker. SkyFast now 0..2: 1 keeps the gaps'
  steps as before and grows only inside a cloud, 4 light samples by day / 3 at night, all but the last with the
  detail; 2 is 1.96f's.
- User: white 1-pixel outlines with reflections on (a sign's frame and the coffee panel, a bin on the wet road, the
  918's engine-vent edges); no AA (too dear). The apply (PS_SSRApply) already takes the reflection depth-aware (1.71)
  and has the paint's ndv floor (1.60); the rim is the grazing Fresnel of an edge-on pixel. New SsrEdges (257, P25.w,
  REFLECTIONS adv): a pixel off its neighbours' plane (second difference of the 4 neighbours' distances near the first:
  a step, not a slope, so a far wet road keeps its Fresnel) with a neighbour well behind (3% + 5 cm): f5 and the
  paint's f5p x (1 - edge). The ground beside a thing (the thing nearer) is not touched (no dark outline).
- fxc clean (clouds, main; PS_SSRApply with the cloud common in front). Builds. Backups src/*_v196f.*.bak,
  NewLook_v196f.hlsl.bak, NewLook_v196f.asi.bak, NewLook_v196f_installed.hlsl.bak. The game was running:
  NewLook_v196g_pending.asi/.hlsl, a wait-and-copy installs them (and NFSLight_ini_before_196g.bak) when it closes.

## 1.96h (2026-10-25): the white 1-pixel rims' real cause: the anti-shimmer at things' edges
- User (Zelef), 1.96g: SkyFast 1 looks as before: kept. The rims stayed on bins, signs, traffic cars and the 918's
  vents; he found them gone with "Noise: anti-shimmer" (SpecAA, 173, P22.x) at 0 (and set it to 0).
- Why: SpecAA (1.43, the sun's and the lamps' highlights) and the SSR ray's roughness (1.45, nVar) widen the lobe by
  the normal's pixel-to-pixel spread (ddx/ddy of the G-buffer normal). Across a thing's silhouette that spread is the
  edge (another surface), not bumps: the edge pixels got rough, and a rough reflection there is the blurred bright sky.
- New DepthStep(i, depth) (the 4 neighbours' distances, the test of 1.96g's SsrEdges without its "behind" side): the
  spread x (1 - step) in SpecAA (PS_Sun, PS_Lamps) and SsrRay's nVar. The anti-shimmer works on bumpy stones again;
  ~4 depth reads more per pixel in those passes.
- fxc clean (PS_Sun, PS_Lamps, PS_SSR, PS_SSRApply; old warnings only). Builds. Backups src/*_v196g.*.bak,
  NewLook_v196g.hlsl.bak, NewLook_v196g.asi.bak, NewLook_v196g_installed.hlsl.bak. The game was running:
  NewLook_v196h_pending.asi/.hlsl, a wait-and-copy installs them when it closes.

