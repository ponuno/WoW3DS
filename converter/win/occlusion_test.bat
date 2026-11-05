@echo off
rem 6.53: how much of what the 3DS draws from one spot is actually seen (the rest hides behind buildings / the ground).
rem The spot and the view are those of the last /gputest in Stormwind (gputest.txt: "at X Y Z", "camera yaw Y pitch P deg").
rem Usage: occlusion_test.bat [path to map0.w3dp] [x y z yaw pitch]
set "PACK=%~dp0all\map0.w3dp"
if not "%~1"=="" set "PACK=%~1"
set "X=-8828.7" & set "Y=627.3" & set "Z=95.5" & set "YAW=70" & set "PITCH=10"
if not "%~6"=="" (set "X=%~2" & set "Y=%~3" & set "Z=%~4" & set "YAW=%~5" & set "PITCH=%~6")
set "OUT=%~dp0occ"
cd /d "%~dp0"
if not exist "%OUT%" mkdir "%OUT%"
rem common*.w3dp must lie next to map0.w3dp (the models are there)
w3docc.exe "%PACK%" "%OUT%" %X% %Y% %Z% %YAW% %PITCH% --dist 533
echo.
echo Send occ\occ_report.txt, occ\occ_view.png and occ\occ_hidden.png
pause
