@echo off
rem Builds the mod and the release for players (dist\inventory-mod and dist\inventory-mod-<date>.zip).
rem   make_dist.bat [-SkipBuild] [-SkipTests] [-SkipAddons] [-GameDir PATH]    see tools\make_dist.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\make_dist.ps1" %*
exit /b %ERRORLEVEL%
