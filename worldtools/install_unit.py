"""install_unit.py path\to\exported\TRK_UNITxxx.BNDL
Puts an exported track unit into HAWAII (the game must be closed); the original kept in worldtools/orig first time."""
import sys, os, shutil, subprocess
sys.path.insert(0, os.path.dirname(__file__))
import bndw
src = sys.argv[1]
unit = os.path.basename(src)
game = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
dst = os.path.join(game, 'HAWAII', unit)
if 'NFS13.exe' in subprocess.run(['tasklist', '/FI', 'IMAGENAME eq NFS13.exe'], capture_output=True, text=True).stdout:
    print('the game is running: close it first'); sys.exit(1)
if not os.path.exists(dst):
    print('no such unit in HAWAII:', unit); sys.exit(1)
a, b = bndw.Bundle(dst), bndw.Bundle(src)
ida = {r.id & 0xffffffff for r in a.res}; idb = {r.id & 0xffffffff for r in b.res}
lost = ida - idb
print('%s: %d resources now, %d in the new one; %d of the old ones missing' % (unit, len(a.res), len(b.res), len(lost)))
if lost and '--force' not in sys.argv:
    print('some resources would be lost (props, lights, plants?): was prepare_unit run before the export? (--force to install anyway)'); sys.exit(1)
orig = os.path.join(os.path.dirname(__file__), 'orig')
os.makedirs(orig, exist_ok=True)
if not os.path.exists(os.path.join(orig, unit)):
    shutil.copy2(dst, os.path.join(orig, unit))
shutil.copy2(src, dst)
print('installed', unit, '(restore.bat puts the original back)')
