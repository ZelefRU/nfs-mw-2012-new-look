@echo off
rem Bakes the world editor's edits of props (moves, hides, Ctrl+D copies) into the game's track units.
rem  bake.bat          shows what would be done
rem  bake.bat --write  does it (the game must be closed); restore.bat undoes every baked unit
python "%~dp0bake.py" %*
pause
