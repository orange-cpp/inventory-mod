# Drives a test session of the DLL in left4dead2.exe.  Only the game's own window is captured (PrintWindow), and
# input is posted to that window, so the real mouse and keyboard are left alone.
#
#   . .\tools\game_test.ps1
#   Start-Game          # launches windowed with -insecure and waits for the menu
#   Inject-Mod          # loads out\Release\inventory_mod.dll
#   Press-Key 0x49      # WM_KEYDOWN / WM_KEYUP of a virtual key ('I')
#   Click-At 548 177    # left click at client coordinates
#   Shot name           # %TEMP%\name.png
#   Console "status"    # types a line into the game's console and runs it
#   Stop-Game
#
# A dedicated server (srcds.exe) with the mod, and the game connected to it:
#   Start-Server         # srcds.exe -console -game left4dead2 -port 27020 -insecure +map c1m1_hotel
#   Inject-Server        # copies the DLL (and the settings) to out\Server and loads it into that srcds.exe
#   Start-Game -Connect  # the game, connecting to it (then Inject-Mod)
#   Server-Log / Unload-Server / Reload-Server / Stop-Server

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GameWin {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public struct RECT { public int L, T, R, B; }
}
"@

$script:Root = Split-Path -Parent $PSScriptRoot
$script:L4D = "C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2"
$script:L4DS = "C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2 Dedicated Server"
if (-not (Test-Path variable:script:ServerPort)) { $script:ServerPort = 27020 }
if (-not (Test-Path variable:script:ServerPid)) { $script:ServerPid = 0 }

function Game-Window { (Get-Process left4dead2 -ErrorAction Stop).MainWindowHandle }

function Start-Game([int]$WaitSeconds = 40, [switch]$Connect) {
    $arguments = @("-insecure", "-novid", "-sw", "-w", "1280", "-h", "720", "-console")
    if ($Connect) { $arguments += @("+connect", "$(Server-Address):$script:ServerPort") }
    Start-Process -FilePath "$script:L4D\left4dead2.exe" -WorkingDirectory $script:L4D -ArgumentList $arguments | Out-Null
    Start-Sleep -Seconds $WaitSeconds
}

function Stop-Game { Stop-Process -Name left4dead2 -Force -ErrorAction SilentlyContinue }

function Inject-Mod {
    & "$script:Root\out\Release\injector.exe" "$script:Root\out\Release\inventory_mod.dll"
    Start-Sleep -Seconds 4
}

function Mod-Log { Get-Content "$script:Root\out\Release\inventory_mod.log" }

# The game reads keys by scan code (lParam bits 16-23), so the real one is sent: with scan code 1 the game sees Escape.
function Press-Key([int]$vk, [int]$HoldMs = 60) {
    $h = Game-Window
    $l = 1 -bor ([int][GameWin]::MapVirtualKey([uint32]$vk, 0) -shl 16)
    [GameWin]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr]$l) | Out-Null
    Start-Sleep -Milliseconds $HoldMs
    [GameWin]::PostMessage($h, 0x101, [IntPtr]$vk, [IntPtr]($l -bor [int]0xC0000000)) | Out-Null
    Start-Sleep -Milliseconds 300
}

# Opens the console, types a line, runs it, closes the console.
function Console([string]$line) {
    $h = Game-Window
    Press-Key 0xC0
    Start-Sleep -Milliseconds 600
    foreach ($c in $line.ToCharArray()) {
        [GameWin]::PostMessage($h, 0x102, [IntPtr][int]$c, [IntPtr]1) | Out-Null
        Start-Sleep -Milliseconds 20
    }
    Press-Key 0x0D
    Press-Key 0xC0
}

function Move-To([int]$x, [int]$y) {
    $h = Game-Window
    [GameWin]::PostMessage($h, 0x200, [IntPtr]0, [IntPtr](($y -shl 16) -bor $x)) | Out-Null
    Start-Sleep -Milliseconds 150
}

function Click-At([int]$x, [int]$y) {
    $h = Game-Window
    $l = [IntPtr](($y -shl 16) -bor $x)
    Move-To $x $y
    [GameWin]::PostMessage($h, 0x201, [IntPtr]1, $l) | Out-Null
    Start-Sleep -Milliseconds 120
    [GameWin]::PostMessage($h, 0x202, [IntPtr]0, $l) | Out-Null
    Start-Sleep -Milliseconds 300
}

function Shot([string]$name) {
    $h = Game-Window
    $r = New-Object GameWin+RECT
    [GameWin]::GetClientRect($h, [ref]$r) | Out-Null
    $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [GameWin]::PrintWindow($h, $hdc, 3) | Out-Null
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $path = Join-Path $env:TEMP "$name.png"
    $bmp.Save($path)
    $bmp.Dispose()
    $path
}

# Asks the injected mod to unload itself (WM_APP + 0x4A), so the DLL can be rebuilt without restarting the game.
function Unload-Mod {
    $h = Game-Window
    [GameWin]::PostMessage($h, 0x804A, [IntPtr]0, [IntPtr]0) | Out-Null
    Start-Sleep -Seconds 3
}

# Unload, rebuild the DLL, load it again.
function Reload-Mod {
    Unload-Mod
    $build = Join-Path $script:Root 'build.bat'
    $log = Join-Path $script:Root 'build_log.txt'
    cmd /c "`"$build`" Release inventory_mod" > $log 2>&1
    if ($LASTEXITCODE -ne 0) { "build failed:"; Get-Content $log -Tail 30; return }
    Inject-Mod
    Mod-Log | Select-Object -Last 6
}

# ---- a dedicated server with the mod -------------------------------------------------------------------------------

# srcds binds to the machine's LAN address, not 127.0.0.1.
function Server-Address {
    (Get-NetIPConfiguration | Where-Object { $_.IPv4DefaultGateway -ne $null } | Select-Object -First 1).IPv4Address.IPAddress
}

# +sv_lan 1: a LAN server does not ask Steam to validate each player's ticket.  Without it the server drops the
# player a few seconds after they join ("STEAMAUTH: Client ... received failure code 1", "No Steam logon").
# Port 27020: the game itself keeps 27015 open, and a server started next to it on 27015 moved to 27016, where the game
# never got an answer.
function Start-Server([string]$Map = "c1m1_hotel", [int]$Port = 27020, [int]$WaitSeconds = 45) {
    $script:ServerPort = $Port
    $p = Start-Process -FilePath "$script:L4DS\srcds.exe" -WorkingDirectory $script:L4DS -PassThru `
        -ArgumentList "-console", "-game", "left4dead2", "-port", "$Port", "-insecure", "+sv_lan", "1", "+map", $Map
    $script:ServerPid = $p.Id
    Start-Sleep -Seconds $WaitSeconds
    "srcds.exe pid $($p.Id), $(Server-Address):$Port, $Map"
}

# The srcds.exe started on that port (found again in a new PowerShell session, where $script:ServerPid is gone).
function Server-Pid([int]$Port = $script:ServerPort) {
    if ($script:ServerPid -ne 0 -and (Get-Process -Id $script:ServerPid -ErrorAction SilentlyContinue)) { return $script:ServerPid }
    $all = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'srcds.exe' })
    $p = $all | Where-Object { $_.CommandLine -match "-port\s+$Port\b" } | Select-Object -First 1
    if ($p) { $script:ServerPid = [int]$p.ProcessId; return $script:ServerPid }
    # CommandLine can be empty (no access) or lack -port (a server started elsewhere, on the default port): fall back to
    # the only srcds.exe running.
    if ($all.Count -eq 1) {
        $script:ServerPid = [int]$all[0].ProcessId
        Write-Host "Server-Pid: no srcds.exe command line matched -port $Port; using the only srcds.exe, pid $script:ServerPid"
    } elseif ($all.Count -gt 1) {
        Write-Host "Server-Pid: several srcds.exe are running (pids $(($all | ForEach-Object ProcessId) -join ', ')) and none matched -port $Port"
    }
    $script:ServerPid
}

function Stop-Server {
    $id = Server-Pid
    if ($id -ne 0) { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    $script:ServerPid = 0
}

function Server-Log { Get-Content "$script:Root\out\Server\inventory_mod_server.log" }

# The server gets its own copy, so the game and the server never lock the same file.
function Inject-Server {
    $dir = "$script:Root\out\Server"
    $serverPid = Server-Pid
    if ($serverPid -eq 0) { Write-Host "Inject-Server: no srcds.exe found (Start-Server first, or pass the right port)"; return }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item "$script:Root\out\Release\inventory_mod.dll" $dir -Force
    # the mod's addons (the ammo packs), mounted by the mod from the addons folder next to the DLL (a copy in use is
    # left as it is: it stays mounted)
    if (Test-Path "$script:Root\out\Release\addons") {
        New-Item -ItemType Directory -Force "$dir\addons" | Out-Null
        Copy-Item "$script:Root\out\Release\addons\*.vpk" "$dir\addons" -Force -ErrorAction SilentlyContinue
    }
    if (-not (Test-Path "$dir\inventory_mod_settings.toml") -and (Test-Path "$script:Root\out\Release\inventory_mod_settings.toml")) {
        Copy-Item "$script:Root\out\Release\inventory_mod_settings.toml" $dir
    }
    # the server's rules (slots, weight limit, item weights): edit out\Server\inventory_mod.toml, then Reload-Server
    if (-not (Test-Path "$dir\inventory_mod.toml")) {
        Copy-Item "$script:Root\inventory_mod.example.toml" "$dir\inventory_mod.toml"
    }
    & "$script:Root\out\Release\injector.exe" "$dir\inventory_mod.dll" $serverPid
    Start-Sleep -Seconds 4
    Server-Log | Select-Object -Last 3
}

# The server copy unloads when this file appears next to it.
function Unload-Server {
    New-Item -ItemType File -Force "$script:Root\out\Server\inventory_mod_server.unload" | Out-Null
    Start-Sleep -Seconds 2
}

# A rebuilt DLL into the server (Reload-Mod does the game).
function Reload-Server {
    Unload-Server
    Inject-Server
}
