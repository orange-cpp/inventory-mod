@echo off
rem Starts a dedicated L4D2 server (srcds.exe, -insecure, LAN) and injects the mod from out\Release into it.
rem   start-server.bat [map]        (default c1m1_hotel; build.bat Release first)
rem Server rules: out\Server\inventory_mod.toml. Log: out\Server\inventory_mod_server.log.
rem Unload: create out\Server\inventory_mod_server.unload.
setlocal
set MAP=%1
if "%MAP%"=="" set MAP=c1m1_hotel
powershell -NoProfile -ExecutionPolicy Bypass -Command ". '%~dp0tools\game_test.ps1'; Start-Server -Map '%MAP%'; Inject-Server"
