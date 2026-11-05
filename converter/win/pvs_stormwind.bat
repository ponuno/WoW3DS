@echo off
rem 6.54: the precomputed visibility (PVS) of Stormwind: map0.pvs next to map0.w3dp. The 3DS then skips the doodads and the
rem ground hidden behind buildings. Copy all\map0.pvs to sdmc:/3ds/w3dproto/ next to map0.w3dp.
rem It renders the town from thousands of points (all processor cores): 6.54e about twice as fast as before (nearest first, hidden
rem buildings / ground skipped whole) - the progress is shown.
rem 6.54e: the far ground's height taken at its real distance (the far ground / shells are skipped from inside the walls now).
rem 6.56d: file version 4 - interior WMO groups with portals (Stormwind's streets / canal walls) seen from afar are in it
rem (clients from 8zg; older ones refuse v4: use --interior-near for them).
rem 6.54d: also the WMO groups and whether anything past the draw distance can be seen (the far ground skipped). Make it again with
rem this w3docc: an older map0.pvs is refused by the client (perf_events: "not a PVS of this version").
rem At the end it checks itself: random cameras in the area, "MISSING" = something seen that the PVS would hide (should be 0 or a few).
set "PACK=%~dp0all\map0.w3dp"
if not "%~1"=="" set "PACK=%~1"
cd /d "%~dp0"
w3docc.exe --pvs "%PACK%" --around -8700 700 550
echo.
echo Send all\map0.pvs.txt (the report). Copy all\map0.pvs to the SD card next to map0.w3dp.
pause
