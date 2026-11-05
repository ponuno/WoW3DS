@echo off
rem 6.57: the horizon - one coarse mesh per 133-yd block of every tile of the continents (buildings and large trees), drawn by
rem the 3DS from 160 yd on instead of the real ones (pad.txt hlod: the switch distance, 0 off) and past the draw distance up to the
rem horizon (Settings: Horizon). Writes all\map0.hlod and all\map1.hlod next to the packs (+ .hlod.txt reports); a few minutes.
rem Copy map0.hlod and map1.hlod to sdmc:/3ds/w3dproto/ next to map0.w3dp / map1.w3dp. Made again after every conversion.
set "ALL=%~dp0all"
if not "%~1"=="" set "ALL=%~1"
cd /d "%~dp0"
w3docc.exe --hlod "%ALL%\map0.w3dp"
w3docc.exe --hlod "%ALL%\map1.w3dp"
echo.
echo Send all\map0.hlod.txt. Copy all\map0.hlod and all\map1.hlod to the SD card next to the packs.
pause
