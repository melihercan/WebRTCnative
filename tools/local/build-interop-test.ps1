# Compiles one of the C test harnesses with the checkout's own clang-cl and
# drops the exe next to WebRtcInterop.dll, which it loads at run time.
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Root = 'C:\tmp\webrtc-build',
    [string]$Repo = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'WebRtcInterop')
)

$ErrorActionPreference = 'Stop'
$src  = Join-Path $Root 'webrtc-checkout\src'
$out  = Join-Path $src 'out\Default'
$clang = Join-Path $src 'third_party\llvm-build\Release+Asserts\bin\clang-cl.exe'
$repo = $Repo

# clang-cl needs the Windows SDK headers and libs, which VsDevCmd exports.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -property installationPath
$devcmd = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $devcmd)) { Write-Host "!!! no VsDevCmd at $devcmd"; exit 1 }

cmd /c "`"$devcmd`" -arch=amd64 -no_logo && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

Set-Location $out
$source = Join-Path $repo "test\$Name.c"
if (-not (Test-Path $source)) { Write-Host "!!! no such test: $source"; exit 1 }

Write-Host "=== clang-cl $Name.c ==="
& $clang /nologo /W4 /I (Join-Path $repo 'include') $source "/Fe:$Name.exe" "/Fo:$Name.obj" 2>&1 |
  Select-Object -Last 25
Write-Host "compile exit=$LASTEXITCODE"
