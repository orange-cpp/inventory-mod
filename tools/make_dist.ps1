# Builds the mod and the release for players: dist\inventory-mod (the folder) and dist\inventory-mod-<date>.zip.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_dist.ps1 [-SkipBuild] [-SkipTests]
#   (make_dist.bat in the repository's root runs it)
#
# 1. build.bat Release (the DLL, the injector, the tests); the game must not have this DLL loaded (Ctrl+End in the
#    game unloads it, or close the game): Windows does not let the linker replace a loaded DLL.
# 2. out\Release\inventory_tests.exe: the release stops when a test fails.
# 3. tools\make_rounds_addon.py (Python with Pillow, and the game installed: it reads the game's ammo pack model)
#    builds addons\inventory_mod_rounds.vpk, and tools\make_menu_addon.py addons\inventory_mod_menu.vpk (the
#    Options > Inventory Mod page), straight into the release folder.
# 4. The release folder is made anew: inventory_mod.dll, injector.exe, load-mod.bat and HOW-TO.txt (packaging\),
#    inventory_mod.example.toml, inventory_mod_screamer.jpg (assets\), addons\; then it is zipped.

param(
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root 'out\Release'
$Dist = Join-Path $Root 'dist'
$Folder = Join-Path $Dist 'inventory-mod'

function Step([string]$text) { Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string]$text) { Write-Host "release not made: $text" -ForegroundColor Red; exit 1 }

if (-not $SkipBuild) {
    Step 'build (build.bat Release)'
    $log = Join-Path $Root 'build_log.txt'
    # (the redirection is cmd's: PowerShell would stop at the first line vcvarsall writes to stderr)
    cmd /c "`"$Root\build.bat`" Release > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Get-Content $log | Select-String ' error |fatal error' | Select-Object -First 10 | ForEach-Object { Write-Host $_ }
        if (Select-String -Path $log -Pattern 'LNK1104' -Quiet) {
            Fail 'the DLL is in use: unload the mod in the game (Ctrl+End) or close the game, then run this again'
        }
        Fail "the build failed (all of it in $log)"
    }
}

if (-not $SkipTests) {
    Step 'tests (inventory_tests.exe)'
    & (Join-Path $Out 'inventory_tests.exe')
    if ($LASTEXITCODE -ne 0) { Fail 'a test failed' }
}

foreach ($file in 'inventory_mod.dll', 'injector.exe') {
    if (-not (Test-Path (Join-Path $Out $file))) { Fail "out\Release\$file is missing: build first" }
}

Step 'the release folder'
if (Test-Path $Folder) { Remove-Item $Folder -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $Folder 'addons') | Out-Null
Copy-Item (Join-Path $Out 'inventory_mod.dll'), (Join-Path $Out 'injector.exe') $Folder
Copy-Item (Join-Path $Root 'packaging\load-mod.bat'), (Join-Path $Root 'packaging\HOW-TO.txt') $Folder
Copy-Item (Join-Path $Root 'inventory_mod.example.toml') $Folder
Copy-Item (Join-Path $Root 'assets\inventory_mod_screamer.jpg') $Folder

Step 'the ammo packs (tools\make_rounds_addon.py)'
& python -I (Join-Path $Root 'tools\make_rounds_addon.py') --out (Join-Path $Folder 'addons\inventory_mod_rounds.vpk')
if ($LASTEXITCODE -ne 0) { Fail 'the ammo pack addon was not made (Python with Pillow, and the game installed?)' }

Step 'the menus (tools\make_menu_addon.py)'
& python -I (Join-Path $Root 'tools\make_menu_addon.py') --out (Join-Path $Folder 'addons\inventory_mod_menu.vpk')
if ($LASTEXITCODE -ne 0) { Fail 'the menu addon was not made (the game installed?)' }

Step 'the zip'
$zip = Join-Path $Dist ("inventory-mod-{0}.zip" -f (Get-Date -Format 'yyyy-MM-dd'))
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $Folder -DestinationPath $zip
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
$archive.Entries | ForEach-Object { '  {0,-45} {1,10:N0}' -f $_.FullName, $_.Length }
$archive.Dispose()
Write-Host ("release: {0} ({1:N0} KB)" -f $zip, ((Get-Item $zip).Length / 1KB)) -ForegroundColor Green
