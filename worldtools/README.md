# World tools (NFS Most Wanted 2012 track units)

- `bndw.py`: Bundle 2 reader / writer (byte-identical round trip on TRK_UNIT11 / ARENA_527909, also recompressed).
- `props.py`: PropInstanceList (0x218): read, move, add (layout in its docstring).
- `addprops.py UNIT out src:dx:dy:dz ...`: copies of props; the original unit kept in `orig/` on first use.
- `restore.bat`: every unit in `orig/` back into HAWAII (game closed).

Resource types (burnout.wiki "Resource Types", Most Wanted): 0x50 InstanceList, 0x51 Model, 0x60 PolygonSoupList,
0x68 NavigationMesh, 0x204 DynamicInstanceList, 0x205 WorldObject, 0x206 ZoneHeader, 0x20F GroundcoverCollection,
0x213 LightInstanceList, 0x214 GroundcoverInstances, 0x215 CompoundObject, 0x216 CompoundInstanceList,
0x217 PropObject, 0x218 PropInstanceList. Each of the 169 TRK_UNITs has one 0x50, 0x60, 0x68, 0x204, 0x206, 0x213,
0x216, 0x218. 12290 props of 112 kinds.

## Baking the world editor's edits (1.58)
- In game (Ctrl+J): select props, Ctrl+D makes copies (1 m along x, then selected: move / turn / scale them); Del on a
  copy deletes it; moves and hides of props as before. Saved in scripts/NFSLight_world.txt ("copy" lines).
- `bake.bat`: shows what would go into the track units; `bake.bat --write` (the game closed) writes them: props moved,
  removed (hidden), copies added (new ids), the originals kept in orig/ (restore.bat), the baked lines taken out of
  NFSLight_world.txt (the old one kept as NFSLight_world_before_bake_<time>.txt).
- Not baked (stay as the mod's runtime edits): the city's static models (InstanceList 0x50: their collision is in the
  PolygonSoupList, it would not follow) and pieces of models.

## Blender (DGIorio's importer / exporter, Blender 3.6)
- Library: %APPDATA%/Blender Foundation/Blender/3.6/scripts/addons/NeedForSpeedMostWanted2012/NFSMW_Library_PC; its
  TRKs folder is empty as shipped: exporting a unit then writes its props / lights / plants / navigation lists EMPTY.
  `prepare_unit.bat TRK_UNITxxx` unpacks the game's unit there first (the exporter copies it, replaces only models and
  collision).
- Import: File > Import > NFS MW 2012, Resource type InstanceList (default GraphicsSpec = cars), PC, Is bundle.
  Collections: TRK_UNITxxx_list (InstanceList: statics, exported), TRK_COL_xxx (collision, hidden at import: kept as
  it was unless shown), _proplist / _dynlist / _cmplist (shown, NOT exported by the add-on: props via the game + bake).
  Blender axes: game (x, y, z) = Blender (x, -z, y)... i.e. Blender z = the game's up.
- Export: File > Export > NFS MW 2012, a folder, Pack bundle: <folder>/TRK_UNITxxx/TRK_UNITxxx.BNDL;
  `install_unit.bat <that file>` (refuses if resources would be lost).
- Round trip TRK_UNIT157 (2026-10-19): all 1603 resources kept, +3 SamplerStates; rewritten: InstanceList, 113 Models,
  113 Renderables, 25 Materials, 1 Texture.
