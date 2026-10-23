"""prepare_unit.py TRK_UNIT157 [blender version, default 3.6]
Unpacks the game's track unit (as it is now in HAWAII) into the Blender add-on's library (NFSMW_Library_PC/TRKs/<unit>):
the exporter copies that folder and replaces only the models and the collision, so the unit's props, lights, plants,
navigation stay (with an empty TRKs folder it writes them empty)."""
import sys, os, shutil
unit = sys.argv[1].upper().replace('.BNDL', '')
ver = sys.argv[2] if len(sys.argv) > 2 else '3.6'
addons = os.path.join(os.environ['APPDATA'], 'Blender Foundation', 'Blender', ver, 'scripts', 'addons')
sys.path.insert(0, addons)
from bundle_packer_unpacker import unpack_bundle_mw
game = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
src = os.path.join(game, 'HAWAII', unit + '.BNDL')
lib = os.path.join(addons, 'NeedForSpeedMostWanted2012', 'NFSMW_Library_PC', 'TRKs', unit)
if os.path.isdir(lib):
    shutil.rmtree(lib)
print('unpacking', src, '->', lib)
r = unpack_bundle_mw(src, lib, 'IDs_' + unit + '.BNDL')
print('done' if r != 1 else 'FAILED', sorted(os.listdir(lib)) if os.path.isdir(lib) else '')
