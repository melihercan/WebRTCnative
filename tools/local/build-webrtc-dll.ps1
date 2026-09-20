# Waits for the fetch stage, applies the five shared-library patch edits, then
# builds webrtc.dll. Mirrors .github/workflows/WebRtcNativeWindowsDynamicLib.yml.

param([string]$Root = 'C:\tmp\webrtc-build')

$root = $Root
$dt   = Join-Path $root 'depot_tools'
$src  = Join-Path $root 'webrtc-checkout\src'

$env:PATH = "$dt;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$env:GIT_CONFIG_COUNT = '2'
$env:GIT_CONFIG_KEY_0 = 'core.autocrlf'; $env:GIT_CONFIG_VALUE_0 = 'false'
$env:GIT_CONFIG_KEY_1 = 'core.filemode'; $env:GIT_CONFIG_VALUE_1 = 'false'

function Step($msg) { Write-Host "=== $msg === $(Get-Date -Format HH:mm:ss)" }
function Fail($msg) { Write-Host "!!! FAILED: $msg"; Step 'BUILD STAGE DONE'; exit 1 }

# ---------------------------------------------------------------- wait -----
Step 'waiting for fetch stage'
$deadline = (Get-Date).AddHours(4)
while (-not (Select-String -Path "$root\fetch.log" -Pattern 'FETCH STAGE DONE' -Quiet -ErrorAction SilentlyContinue)) {
  if ((Get-Date) -gt $deadline) { Fail 'timed out waiting for the fetch stage' }
  Start-Sleep -Seconds 30
}
if (-not (Test-Path $src)) { Fail "no checkout at $src" }
Set-Location $src

# --------------------------------------------------------- sync check -----
# Never build on a half-synced tree: it fails much later and much less
# legibly. chromium.googlesource.com rate-limits (HTTP 429) when gclient runs
# its default wide parallelism, so retry narrow rather than giving up.
Step 'verify gclient sync'
# sync-ok.marker records a sync that already succeeded on an earlier run of this
# script, so a resume does not repeat ~20 minutes of dependency verification.
$syncOk = (Test-Path "$root\sync-ok.marker") -or
          (Select-String -Path "$root\fetch.log" -Pattern 'gclient sync exit=0' -SimpleMatch -Quiet -ErrorAction SilentlyContinue)
if (Test-Path "$root\sync-ok.marker") { Write-Host "sync previously confirmed: $(Get-Content "$root\sync-ok.marker")" }
$attempt = 0
while (-not $syncOk -and $attempt -lt 3) {
  $attempt++
  Step "sync incomplete - retry $attempt of 3 with -j4 (gentler on the rate limiter)"
  Set-Location (Split-Path $src -Parent)
  & gclient sync -D --force --reset -j4
  $code = $LASTEXITCODE
  Write-Host "gclient sync retry $attempt exit=$code"
  Set-Location $src
  if ($code -eq 0) { $syncOk = $true }
  elseif ($attempt -lt 3) { Write-Host 'backing off 120s'; Start-Sleep -Seconds 120 }
}
if (-not $syncOk) { Fail 'gclient sync never completed; the checkout is incomplete' }
Write-Host 'checkout is complete'

# ------------------------------------------------------- visual studio -----
Step 'locate Visual Studio'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -property installationPath
$version = & $vswhere -latest -products * -property installationVersion
if (-not $install) { Fail 'vswhere found no Visual Studio' }
$year = switch ([int]$version.Split('.')[0]) { 18 { '2026' } 17 { '2022' } 16 { '2019' } default { '' } }
Write-Host "Visual Studio $version at $install (Chromium year: '$year')"
$env:GYP_MSVS_OVERRIDE_PATH = $install
if ($year) { Set-Item -Path "env:vs${year}_install" -Value $install }

# -------------------------------------------------------------- patch -----
Step 'reset tree before patching'
git checkout -- BUILD.gn webrtc.gni rtc_tools/BUILD.gn
Write-Host "reset exit=$LASTEXITCODE"

Step 'assert patch anchors'
$anchors = @(
  @{ File = 'BUILD.gn';           Text = 'rtc_static_library' },
  @{ File = 'BUILD.gn';           Text = 'complete_static_lib' },
  @{ File = 'webrtc.gni';         Text = '!build_with_chromium && is_component_build' },
  @{ File = 'webrtc.gni';         Text = 'if (build_with_chromium && defined(deps))' },
  @{ File = 'rtc_tools\BUILD.gn'; Text = ':frame_analyzer' }
)
foreach ($a in $anchors) {
  $hit = Select-String -Path $a.File -Pattern $a.Text -SimpleMatch -Quiet
  Write-Host ("  {0,-58} in {1}: {2}" -f $a.Text, $a.File, $hit)
  if (-not $hit) { Fail "anchor missing: '$($a.Text)' in $($a.File)" }
}

Step 'apply the five edits'
(Get-Content BUILD.gn).replace('rtc_static_library', 'rtc_shared_library') | Set-Content BUILD.gn
(Get-Content BUILD.gn) -notmatch 'complete_static_lib' | Set-Content BUILD.gn
(Get-Content webrtc.gni).replace('!build_with_chromium && is_component_build', 'false') | Set-Content webrtc.gni
(Get-Content rtc_tools\BUILD.gn) -notmatch ':frame_analyzer' | Set-Content rtc_tools\BUILD.gn
(Get-Content webrtc.gni).replace('if (build_with_chromium && defined(deps))', 'if (defined(deps))') | Set-Content webrtc.gni

# Edit 5 is the one under test, so prove it actually landed.
$remaining = (Select-String -Path webrtc.gni -Pattern 'if (build_with_chromium && defined(deps))' -SimpleMatch).Count
$applied   = (Select-String -Path webrtc.gni -Pattern 'if (defined(deps))' -SimpleMatch).Count
Write-Host "edit 5: $remaining chromium-gated remaps left, $applied ungated (expect 0 and 5)"
if ($remaining -ne 0) { Fail 'edit 5 did not apply' }

# ---------------------------------------------------------------- gen -----
Step 'gn gen'
New-Item -ItemType Directory -Force -Path out/Default | Out-Null
$gnArgs = @(
  'is_debug = false',
  'target_os = "win"',
  'target_cpu = "x64"',
  'is_component_build = true',
  'rtc_enable_symbol_export = true',
  'rtc_include_tests = false',
  'rtc_build_tools = false',
  'rtc_build_examples = false'
)
Set-Content -Path out/Default/args.gn -Value $gnArgs -Encoding utf8
Get-Content out/Default/args.gn
gn gen out/Default
if ($LASTEXITCODE -ne 0) { Fail "gn gen exit=$LASTEXITCODE" }

# -------------------------------------------------------------- build -----
Step 'autoninja webrtc'
autoninja -C out/Default webrtc
$ninjaExit = $LASTEXITCODE
Write-Host "autoninja exit=$ninjaExit"
if ($ninjaExit -ne 0) { Fail "autoninja exit=$ninjaExit" }

# ------------------------------------------------------------- verify -----
Step 'verify output'
$dll = 'out/Default/webrtc.dll'
if (-not (Test-Path $dll)) { Fail 'webrtc.dll was not produced' }
Write-Host ("webrtc.dll = {0:N1} MB" -f ((Get-Item $dll).Length / 1MB))

$dlls = Get-ChildItem out/Default -Filter *.dll -File
Write-Host "$($dlls.Count) DLLs in out/Default:"
$dlls | Sort-Object Length -Descending | Select-Object -First 15 |
  ForEach-Object { Write-Host ("  {0,-45} {1,8:N1} MB" -f $_.Name, ($_.Length / 1MB)) }

$dumpbin = Get-ChildItem "$install\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
           Select-Object -First 1
if ($dumpbin) {
  $exports = & $dumpbin.FullName /exports $dll 2>$null | Select-String -Pattern '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s'
  Write-Host "exported symbols: $($exports.Count)"
} else {
  Write-Host 'dumpbin not found; skipping export check'
}

Step 'BUILD STAGE DONE'
