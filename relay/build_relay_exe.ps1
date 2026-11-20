# Builds QuestVisor-Relay.exe (one file, no Python needed on the target PC) from relay/simple_forwarder.py.
#
#   powershell -ExecutionPolicy Bypass -File relay\build_relay_exe.ps1 [-WorkDir <dir>] [-Python <python.exe>]
#
# Needs Python 3.9+ on the build PC. A private virtual environment with PyInstaller (and Pillow, for the .ico)
# is created in the work folder; nothing is installed into the system Python.
# Output: <WorkDir>\dist\QuestVisor-Relay.exe   (default WorkDir: build\relay in this repository)
param(
    [string]$WorkDir = "",
    [string]$Python = "python",
    [string]$PyInstallerVersion = "6.22.3"
)
$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
$repo = Split-Path -Parent $tools
if (-not $WorkDir) { $WorkDir = Join-Path $repo "build\relay" }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$WorkDir = (Resolve-Path $WorkDir).Path

$src = Join-Path $tools "simple_forwarder.py"
$version = (Select-String -Path $src -Pattern '^__version__ = "([^"]+)"').Matches[0].Groups[1].Value
Write-Host "QuestVisor Relay $version -> $WorkDir"

# 1. build environment
$venv = Join-Path $WorkDir "venv"
$py = Join-Path $venv "Scripts\python.exe"
if (-not (Test-Path $py)) {
    & $Python -m venv $venv
    if ($LASTEXITCODE -ne 0) { throw "could not create a virtual environment with '$Python'" }
}
& $py -m pip install --disable-pip-version-check -q "pyinstaller==$PyInstallerVersion" pillow
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

# 2. icon: the app's launcher artwork as a multi-size .ico (optional)
$ico = Join-Path $WorkDir "questvisor.ico"
$png = Join-Path $repo "store-assets\icon_512.png"
$iconArgs = @()
if (Test-Path $png) {
    & $py -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2], sizes=[(16,16),(24,24),(32,32),(48,48),(64,64),(128,128),(256,256)])" $png $ico
    if ($LASTEXITCODE -eq 0) { $iconArgs = @("--icon", $ico) }
}

# 3. Windows version resource
$parts = ($version.Split(".") + @("0", "0", "0", "0"))[0..3] -join ", "
$verFile = Join-Path $WorkDir "version_info.txt"
@"
VSVersionInfo(
  ffi=FixedFileInfo(filevers=($parts), prodvers=($parts), mask=0x3f, flags=0x0, OS=0x40004,
                    fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040904B0', [
      StringStruct('FileDescription', 'QuestVisor Relay - DJI Goggles 3 video to Meta Quest'),
      StringStruct('ProductName', 'QuestVisor'),
      StringStruct('FileVersion', '$version'),
      StringStruct('ProductVersion', '$version'),
      StringStruct('OriginalFilename', 'QuestVisor-Relay.exe'),
      StringStruct('InternalName', 'QuestVisor-Relay')])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
"@ | Set-Content -Encoding utf8 $verFile

# 4. build. simple_forwarder.py uses only the standard library and loads no data files (the fake-IDR
#    assets are inside the Quest app, not the relay), so there is nothing else to bundle.
& $py -m PyInstaller --noconfirm --clean --onefile --console --name QuestVisor-Relay `
    --distpath (Join-Path $WorkDir "dist") --workpath (Join-Path $WorkDir "work") --specpath $WorkDir `
    --version-file $verFile @iconArgs $src
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

$exe = Join-Path $WorkDir "dist\QuestVisor-Relay.exe"
& $exe --version
Get-Item $exe | Format-Table Name, Length, LastWriteTime
