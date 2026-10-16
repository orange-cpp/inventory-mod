# Drives a test session of the DLL in left4dead2.exe.  Only the game's own window is captured (PrintWindow), and
# input is posted to that window, so the real mouse and keyboard are left alone.
#
#   . .\tools\game_test.ps1
#   Start-Game          # launches windowed with -insecure and waits for the menu
#   Inject-Mod          # loads out\Release\inventory_mod.dll
#   Press-Key 0x49      # WM_KEYDOWN / WM_KEYUP of a virtual key ('I')
#   Click-At 548 177    # left click at client coordinates
#   Shot name           # %TEMP%\name.png
#   Stop-Game

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GameWin {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  public struct RECT { public int L, T, R, B; }
}
"@

$script:Root = Split-Path -Parent $PSScriptRoot
$script:L4D = "C:\Program Files (x86)\Steam\steamapps\common\Left 4 Dead 2"

function Game-Window { (Get-Process left4dead2 -ErrorAction Stop).MainWindowHandle }

function Start-Game([int]$WaitSeconds = 40) {
    Start-Process -FilePath "$script:L4D\left4dead2.exe" -WorkingDirectory $script:L4D `
        -ArgumentList "-insecure", "-novid", "-sw", "-w", "1280", "-h", "720", "-console" | Out-Null
    Start-Sleep -Seconds $WaitSeconds
}

function Stop-Game { Stop-Process -Name left4dead2 -Force -ErrorAction SilentlyContinue }

function Inject-Mod {
    & "$script:Root\out\Release\injector.exe" "$script:Root\out\Release\inventory_mod.dll"
    Start-Sleep -Seconds 4
}

function Mod-Log { Get-Content "$script:Root\out\Release\inventory_mod.log" }

function Press-Key([int]$vk) {
    $h = Game-Window
    [GameWin]::PostMessage($h, 0x100, [IntPtr]$vk, [IntPtr]0x00010001) | Out-Null
    Start-Sleep -Milliseconds 60
    [GameWin]::PostMessage($h, 0x101, [IntPtr]$vk, [IntPtr]([int]0xC0010001)) | Out-Null
    Start-Sleep -Milliseconds 300
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
