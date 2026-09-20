# Syncs WebRtcInterop from the repo into the grafted checkout and builds it.
param(
    [string]$Root = 'C:\tmp\webrtc-build',
    # Defaults to the WebRtcInterop beside this script's repository.
    [string]$Repo = (Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'WebRtcInterop')
)
$ErrorActionPreference = 'Stop'


$root = $Root
$dt   = Join-Path $root 'depot_tools'
$src  = Join-Path $root 'webrtc-checkout\src'
$repo = $Repo

$env:PATH = "$dt;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -property installationPath
if (-not $install) { Write-Host '!!! vswhere found no Visual Studio'; exit 1 }
$env:GYP_MSVS_OVERRIDE_PATH = $install

Write-Host "=== syncing shim source into the checkout ==="
$graft = Join-Path $src 'WebRtcInterop'
foreach ($sub in 'include', 'src', 'test') {
  $dst = Join-Path $graft $sub
  if (-not (Test-Path $dst)) { New-Item -ItemType Directory -Force $dst | Out-Null }
  Copy-Item (Join-Path $repo "$sub\*") $dst -Force -Recurse
}
Copy-Item (Join-Path $repo 'BUILD.gn') $graft -Force

# Copy-Item carries the source's LastWriteTime across, and the repo files are
# older than the last build output, so ninja would decide there is nothing to
# do and quietly keep the previous DLL. Stamp them instead.
$now = Get-Date
Get-ChildItem $graft -Recurse -File | ForEach-Object { $_.LastWriteTime = $now }
Get-ChildItem $graft -Recurse -File | Select-Object FullName, Length | Format-Table -AutoSize | Out-String | Write-Host

Set-Location $src
Write-Host "=== autoninja WebRtcInterop === $(Get-Date -Format HH:mm:ss)"
& autoninja -C out/Default WebRtcInterop 2>&1 | Select-Object -Last 40
Write-Host "exit=$LASTEXITCODE  $(Get-Date -Format HH:mm:ss)"
