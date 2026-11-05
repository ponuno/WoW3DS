@echo off
rem 6.54: the PVS of the capitals (run pvs_stormwind.bat first to see how long one town takes).
rem map0: Stormwind, Ironforge, Undercity; map1: Orgrimmar, Thunder Bluff, Darnassus. Copy map0.pvs and map1.pvs next to their packs.
set "ALL=%~dp0all"
if not "%~1"=="" set "ALL=%~1"
cd /d "%~dp0"
w3docc.exe --pvs "%ALL%\map0.w3dp" --around -8700 700 550 --around -4840 -1000 300 --around 1630 240 250
w3docc.exe --pvs "%ALL%\map1.w3dp" --around 1600 -4400 400 --around -1250 100 300 --around 9900 2300 400
echo.
echo Send all\map0.pvs.txt and all\map1.pvs.txt. Copy all\map0.pvs and all\map1.pvs to the SD card next to the packs.
pause
