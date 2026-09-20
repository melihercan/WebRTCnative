# Fetches the WebRTC source at the branch the CI workflow resolves to.
# Mirrors .github/workflows/WebRtcNativeWindowsDynamicLib.yml, except that the
# user's global git config is left alone -- the two settings gclient wants are
# passed through GIT_CONFIG_* instead.

param(
    # The branch-head to build, e.g. 8010 for M153. Resolve the current one with
    # .github/actions/resolve-webrtc-branch/resolve_webrtc_branch.py.
    [Parameter(Mandatory = $true)][string]$Branch,
    # Roughly 30 GB lands here, plus about 5 GB of build output later.
    [string]$Root = 'C:\tmp\webrtc-build'
)

$root     = $Root
$dt       = Join-Path $root 'depot_tools'
$checkout = Join-Path $root 'webrtc-checkout'
$branch   = $Branch

$env:PATH = "$dt;$env:PATH"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$env:GIT_CONFIG_COUNT = '2'
$env:GIT_CONFIG_KEY_0 = 'core.autocrlf'; $env:GIT_CONFIG_VALUE_0 = 'false'
$env:GIT_CONFIG_KEY_1 = 'core.filemode'; $env:GIT_CONFIG_VALUE_1 = 'false'

function Step($msg) { Write-Host "=== $msg === $(Get-Date -Format HH:mm:ss)" }

New-Item -ItemType Directory -Force -Path $checkout | Out-Null
Set-Location $checkout

if (-not (Test-Path (Join-Path $checkout 'src'))) {
  Step 'fetch --nohooks webrtc'
  & fetch --nohooks webrtc
  Write-Host "fetch exit=$LASTEXITCODE"
} else {
  Step 'src already present, skipping fetch'
}

Set-Location (Join-Path $checkout 'src')

Step "checkout branch-heads/$branch"
git fetch origin ('+refs/branch-heads/{0}:refs/remotes/branch-heads/{0}' -f $branch)
Write-Host "git fetch exit=$LASTEXITCODE"
git checkout -B "webrtc-$branch" "refs/remotes/branch-heads/$branch"
Write-Host "git checkout exit=$LASTEXITCODE"

Step 'gclient sync'
& gclient sync -D --force --reset
Write-Host "gclient sync exit=$LASTEXITCODE"

git log -1 --oneline
Step 'FETCH STAGE DONE'
