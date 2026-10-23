# NewLook (formerly NFSLight): what is left to fix (1.64, 2026-10-24)

One list: what is open after 1.59-1.63, what waits for a decision, and the older polish list (2026-10-15).
Rule: features first, polish later; items marked (check) are fixed but not yet seen in game.

## Open now
- Far buildings' shadows: some still go missing / come in ragged. Next step: the user's shots of one place with
  ShadowOnly 2 / 3 / 4 (is the building missing from one map, from all, or is our blending at fault).
- Low sun (morning): a mess of patches in the cascades (1.62 fix: the far-caster check ignores a coarse map's own
  ground). (check)
- A one-pixel white outline round cars. Not in the reflection views: probably the half-size AO / bounce taking the
  road's value at the car's edge. Needs: a close shot of a car's edge in "split: left = original", "AO mask" and
  "bounce + lamp light x4".
- White / black dots in the tail lights (1.62: no sharp reflection on bumpy non-paint car parts, lamp glass tints its
  reflection). (check)
- Spinning wheels brighter (1.60), trails on the player's car at speed (1.59), the switch-on stall (1.59). (check)
- Squares on a car's door standing on grass: cause not found (probably grass shadows at a coarse map texel).
- Trails on traffic cars: only the player's car has its own motion in the history passes (traffic cars use the
  world position, as before).

## Waiting for a decision
- The city's height map from the game's own files (the collision of every TRK_UNIT, built once, loaded whole): far
  buildings cast whatever the game has streamed in.
- Shadows of moving things jerk: draw cascade 1 every frame (costs more).
- The time of day / day speed in the mod, redone safely (1.55 / 1.56 rolled back: they broke puddles, light, the sky;
  the game's time fields are in MODLOG "Rollback to 1.54").
- The city map's finer edge and crane skip from 1.55 / 1.56 (kept in *_v156.bak).

## World editing
- Collision for new objects (Blender: the TRK_COL collection, PolygonSoupList).
- Not yet tried in game: world editor copies (Ctrl+D) and worldtools/bake.bat.

## Shadows (older)
- Far shadows of moving things step (rebuilt every ~10 frames): smoother updates.
- Phantom sharp shadows of odd shapes in the mountains: find which casters and leave them out.
- Shadows popping in from buildings when the camera turns.
- Optimisation: our shadow pass costs a lot over the game's (near cascades redraw ~600-900 draws a frame).

## Lamps
- Street-lamp lights do not always go out when a post is knocked down (the tie to the prop).
- A lamp with shadow maps now and then re-draws its faces one by one: keep the old faces until the new set is done.
- Night cost: "lamps" up to ~14 ms.

## Sky
- Far clouds repeat visibly (the noise tiles).
- Cloud shadows fly over the ground too fast; in "storm" the sun still breaks through.

## Wet ground
- Our puddles do not look like puddles yet (real shapes: low spots, edges, a darker rim).
- Walls look wet above the camera's height.

## Materials
- Modern lighting (GGX) shows little gain without real roughness data: comes back with the materials work.
- Parallax: a zigzag seam along some slabs; more wall shaders to rewrite (a shader census first).
- Relief costs ~1 FPS: a candidate for an "Ultra" level.

## AO
- Some ripple left in the AO (much better since 1.49).

## General / feature queue
- An optimisation pass over everything (lamps, half-res, reflections, shadows); draw distance / LOD.
- Later: proper motion blur (per-object velocity); TAA last of all.
- Not wanted: depth of field / bokeh.
