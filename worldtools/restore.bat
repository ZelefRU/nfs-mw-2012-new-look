@echo off
rem Puts every original track unit kept in worldtools\orig back into HAWAII (the game must be closed)
copy /y "%~dp0orig\*.BNDL" "%~dp0..\..\HAWAII\"
