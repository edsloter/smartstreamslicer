param(
  [switch]$Static,
  [switch]$Smoke,
  [string]$Ragel,
  [string]$BoostRoot,
  [string]$BuildDir,
  [int]$Jobs = 0,
  [switch]$Help
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

if (-not $BuildDir) {
  $BuildDir = if ($Static) { 'build' } else { 'build_shared' }
}

$RepoRoot = $PSScriptRoot
Set-Location $RepoRoot

if ($Help) {
  Write-Host @"
SmartStreamSlicer all-in-one build script (Windows / MinGW-w64).

  .\build.ps1 [-Static] [-Smoke] [-Ragel <path>] [-BoostRoot <dir>]
              [-BuildDir <dir>] [-Jobs <n>]

  -Static     Build a fully self-contained static binary (no MinGW runtime
              DLLs needed). Default is a shared build; hs.dll and
              hs_runtime.dll are staged next to the executable automatically.
  -Smoke      Run the test suite (tests/smoke.ps1) against the new binary.
  -Ragel      Path to ragel if it is not on PATH.
  -BoostRoot  Use an existing Boost install instead of downloading one. It is
              verified to be exactly Boost 1.84 (boost/version.hpp check).
  -BuildDir   Output directory (default: 'build' for -Static,
              'build_shared' for a shared build). It is removed and
              recreated, so each run is a clean build.
  -Jobs       Number of parallel jobs (default: all cores).

The script ensures prerequisites (git submodules, CMake 3.18+, MinGW g++, ragel,
Boost 1.84 headers) are present - downloading and hash-verifying what is
missing - then drives the CMake build, which remains the build backend.
"@
  exit 0
}

function Find-Command([string]$Name) {
  Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
}

Write-Host "[*] SmartStreamSlicer all-in-one build (Windows)"

$git = Find-Command git
if (-not $git) {
  throw 'git is required to fetch the vendored submodules (fastcdc, vectorscan).'
}

$cmake = Find-Command cmake
if (-not $cmake) {
  throw 'cmake not found. Install CMake 3.18+ and add it to PATH.'
}
$cmakeVerLine = (cmake --version | Select-Object -First 1)
if ($cmakeVerLine -notmatch 'cmake version ([0-9]+)\.([0-9]+)') {
  throw "unexpected cmake output: $cmakeVerLine"
}
$cMajor = [int]$Matches[1]; $cMinor = [int]$Matches[2]
if ($cMajor -lt 3 -or ($cMajor -eq 3 -and $cMinor -lt 18)) {
  throw "cmake $cMajor.$cMinor is too old; need 3.18+."
}
Write-Host "[ok] cmake $cMajor.$cMinor"

$gxx = Find-Command g++
$gcc = Find-Command gcc
if (-not $gxx -or -not $gcc) {
  throw 'g++/gcc not found. Install MinGW-w64 GCC (e.g. winget install mingw-w64.ucrt.GCC or MSYS2 mingw-w64-ucrt-x86_64-gcc) and put its bin on PATH.'
}
Write-Host "[ok] gxx: $($gxx.Source)"
Write-Host "[ok] gcc: $($gcc.Source)"

Write-Host "[*] initializing submodules (shallow, non-recursive)..."
& git submodule update --init --depth 1 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) {
  throw 'git submodule update failed.'
}
& git -C (Join-Path $RepoRoot 'thirdparty\vectorscan') submodule update --init --depth 1 2>&1 | Out-Host
if ($LASTEXITCODE -ne 0) {
  throw 'vectorscan/simde submodule init failed.'
}

$ragelPath = $null
if ($Ragel) {
  $ragelPath = (Resolve-Path $Ragel -ErrorAction Stop).Path
} else {
  $cmd = Find-Command ragel
  if ($cmd) { $ragelPath = $cmd.Source }
}
if (-not $ragelPath) {
  Write-Host '[..] ragel not found; attempting winget install...'
  & winget install --id PolarGoose.Ragel -e --accept-package-agreements --accept-source-agreements 2>&1 | Out-Host
}
if (-not $ragelPath) {
  $cmd = Find-Command ragel
  if ($cmd) { $ragelPath = $cmd.Source }
}
if (-not $ragelPath) {
  $exe = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter Ragel.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if ($exe) { $ragelPath = $exe.FullName }
}
if (-not $ragelPath) {
  throw 'ragel not found and install failed. Use -Ragel <path> or install it (e.g. pacman -S mingw-w64-ucrt-x86_64-ragel).'
}
& $ragelPath -v 2>&1 | Out-Null
if ($LASTEXITCODE -ne 0) {
  throw "ragel at '$ragelPath' failed to run."
}
Write-Host "[ok] ragel: $ragelPath"

$StaticFlag = if ($Static) { 'ON' } else { 'OFF' }
$cacheDir = if ($env:SSS_CACHE) { $env:SSS_CACHE } else { Join-Path $env:LOCALAPPDATA 'SmartStreamSlicer\cache' }
$boostTgz = Join-Path $cacheDir 'boost_1_84_0.tar.gz'
$boostSha256 = 'A5800F405508F5DF8114558CA9855D2640A2DE8F0445F051FA1C7C3383045724'
$boostUrl = 'https://archives.boost.io/release/1.84.0/source/boost_1_84_0.tar.gz'
$boostArgs = @()

if ($BoostRoot) {
  $br = (Resolve-Path $BoostRoot -ErrorAction Stop).Path
  $vh = Join-Path $br 'boost\version.hpp'
  if (-not (Test-Path $vh)) {
    throw "BOOST_ROOT '$br' has no boost/version.hpp."
  }
  if ((Get-Content $vh -Raw) -notmatch 'BOOST_VERSION\s+108400') {
    throw "BOOST_ROOT '$br' is not Boost 1.84 (expected BOOST_VERSION 108400)."
  }
  Write-Host "[ok] boost: using $br"
  $boostArgs = @("-DBOOST_ROOT=$br")
} else {
  New-Item -ItemType Directory -Path $cacheDir -Force | Out-Null
  if (Test-Path $boostTgz) {
    $h = (Get-FileHash $boostTgz -Algorithm SHA256).Hash
    if ($h -ne $boostSha256) {
      Write-Host "[..] cached boost tarball SHA256 mismatch ($h); re-downloading..."
      Remove-Item $boostTgz -Force
    } else {
      Write-Host "[ok] boost: cached tarball verified (SHA256 $boostSha256)"
    }
  }
  if (-not (Test-Path $boostTgz)) {
    Write-Host '[..] downloading boost 1.84.0 headers...'
    curl.exe -fSL -o $boostTgz $boostUrl
    if ($LASTEXITCODE -ne 0) {
      throw 'boost download failed.'
    }
    $h = (Get-FileHash $boostTgz -Algorithm SHA256).Hash
    if ($h -ne $boostSha256) {
      throw "downloaded boost tarball SHA256 mismatch ($h)."
    }
  }
}

if (Test-Path $BuildDir) {
  Remove-Item $BuildDir -Recurse -Force
}
if (-not $BoostRoot) {
  New-Item -ItemType Directory -Path (Join-Path $BuildDir '_deps') -Force | Out-Null
  Copy-Item $boostTgz (Join-Path $BuildDir '_deps\boost_1_84_0.tar.gz')
}

$cmakeArgs = @()
$cmakeArgs += '-S'; $cmakeArgs += $RepoRoot
$cmakeArgs += '-B'; $cmakeArgs += $BuildDir
$cmakeArgs += '-G'; $cmakeArgs += 'MinGW Makefiles'
$cmakeArgs += '-DCMAKE_BUILD_TYPE=Release'
$cmakeArgs += "-DSSS_STATIC=$StaticFlag"
$cmakeArgs += "-DCMAKE_C_COMPILER=$($gcc.Source)"
$cmakeArgs += "-DCMAKE_CXX_COMPILER=$($gxx.Source)"
$cmakeArgs += "-DRAGEL=$ragelPath"
$cmakeArgs += $boostArgs

Write-Host "[*] configuring (SSS_STATIC=$StaticFlag) via cmake..."
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) {
  throw 'cmake configure failed.'
}

Write-Host '[*] building...'
if ($Jobs -gt 0) {
  & cmake --build $BuildDir --parallel $Jobs
} else {
  & cmake --build $BuildDir
}
if ($LASTEXITCODE -ne 0) {
  throw 'cmake build failed.'
}

$exe = Join-Path $BuildDir 'sss.exe'
if (-not (Test-Path $exe)) {
  throw "expected binary not found: $exe"
}
Write-Host '[*] binary:'
& $exe --version

if ($Smoke) {
  $wd = Join-Path $env:TEMP ("sss_build_smoke_" + [guid]::NewGuid().ToString('N'))
  Write-Host '[*] running smoke tests...'
  & (Join-Path $RepoRoot 'tests\smoke.ps1') $exe $wd
  if ($LASTEXITCODE -ne 0) {
    throw 'smoke tests failed.'
  }
}

Write-Host "OK: $exe"