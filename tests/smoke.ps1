# Smoke + determinism + cross-platform CI test for sss (Windows/PowerShell).
#
#   usage: .\smoke.ps1 <path-to-sss> [[-Workdir] <dir>]
#
# Generates the SAME deterministic dataset as tests/smoke.sh (byte-identical on
# Windows and POSIX) and runs the same checks; writes <workdir>\manifest.sha256
# (sorted part hashes) so CI can compare Windows and POSIX manifests.
# Exits 0 on success and prints SMOKE_OK.
param(
  [Parameter(Mandatory = $true)][string]$Binary,
  [string]$Workdir
)

$ErrorActionPreference = 'Stop'
if (-not $Workdir) {
  $Workdir = Join-Path $env:TEMP ("sss_smoke_" + [guid]::NewGuid().ToString('N'))
}
New-Item -ItemType Directory -Path $Workdir -Force | Out-Null
$S = (Resolve-Path $Binary).Path
$BIN = Split-Path $S -Parent
$R = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

# Shared-build support: put Vectorscan DLLs on PATH.
$libDirs = @($BIN, (Join-Path $BIN '..\thirdparty\vectorscan\bin'),
  (Join-Path $BIN '..\thirdparty\vectorscan\lib'),
  (Join-Path $BIN 'thirdparty\vectorscan\bin'),
  (Join-Path $BIN 'thirdparty\vectorscan\lib'))
foreach ($d in $libDirs) {
  if (Test-Path (Join-Path $d 'hs.dll')) {
    $env:PATH = $d + ';' + $env:PATH
  }
}

Write-Host "smoke: binary=$S workdir=$Workdir"

$FILLER = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!@#._-~:;'

function gen_file([string]$Path, [int]$Lines) {
  $sb = New-Object System.Text.StringBuilder
  for ($i = 0; $i -lt $Lines; $i++) {
    [void]$sb.Append(('SSSLIN{0:D6} {1}' -f $i, $FILLER))
    [void]$sb.Append([char]10)  # '\n', not CRLF
  }
  [System.IO.File]::WriteAllText($Path, $sb.ToString())
}

New-Item -ItemType Directory -Path (Join-Path $Workdir 'data\sub') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Workdir 'data\upper') -Force | Out-Null
# Deliberately created in a NON-sorted order, with names that would diverge
# if the encoder used filesystem enumeration order instead of sorting.
$d = Join-Path $Workdir 'data'
gen_file (Join-Path $d '0numeric.txt') 20
gen_file (Join-Path $d 'Zulu.txt') 5
gen_file (Join-Path $d '_underscore.txt') 3
gen_file (Join-Path $d 'big.bin') 72000
gen_file (Join-Path $d 'alpha1.txt') 4
gen_file (Join-Path $d 'small.txt') 1
[System.IO.File]::WriteAllText((Join-Path $d 'sub\Ced.bin'), "a`nb`nc`n")
gen_file (Join-Path $d 'sub\nested.txt') 8
gen_file (Join-Path $d 'upper\CASEFILE.TXT') 2
gen_file (Join-Path $d 'aLPha2.txt') 6
gen_file (Join-Path $d 'Alpha0.txt') 7
gen_file (Join-Path $d 'B_kiwi.bin') 9
gen_file (Join-Path $d 'b_banana.bin') 11

Set-Location $Workdir
New-Item -ItemType Directory -Path out1, out2 -Force | Out-Null
$A = @('-e', '-f', '-v', '0', '-j', '0', '--target', '1m', '--min', '750k', '--max', '1300k')

# 1: encode
& $S @A data (Join-Path $Workdir 'out1\part') *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "encode failed: $LASTEXITCODE" }

# 2: plan determinism via two --dry-run captures
& $S -e --dry-run -f -v 0 -j 0 --target 1m --min 750k --max 1300k data dryA *> planA.txt
& $S -e --dry-run -f -v 0 -j 0 --target 1m --min 750k --max 1300k data dryB *> planB.txt
if (Compare-Object (Get-Content planA.txt) (Get-Content planB.txt)) {
  Write-Error 'dry-run plan NOT deterministic'
}
Write-Host 'smoke: dry-run plan deterministic'

# 3: decode + verify, byte-compare restored tree
$rt1 = Join-Path $Workdir 'rt1'
if (Test-Path $rt1) { Remove-Item $rt1 -Recurse -Force }
& $S -d --verify -f -v 0 (Join-Path $Workdir 'out1\part.sss001') $rt1 *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "decode failed: $LASTEXITCODE" }
$srcRoot = Join-Path $Workdir 'data'
$mismatch = $null
foreach ($f in (Get-ChildItem $srcRoot -Recurse -File)) {
  $rel = $f.FullName.Substring($srcRoot.Length + 1)
  $dec = Join-Path $rt1 $rel
  if (-not (Test-Path $dec)) { $mismatch = $rel; break }
  $h1 = (Get-FileHash $f.FullName -Algorithm SHA256).Hash
  $h2 = (Get-FileHash $dec -Algorithm SHA256).Hash
  if ($h1 -ne $h2) { $mismatch = $rel; break }
}
if ($mismatch) { Write-Error "round-trip mismatch on: $mismatch" }
Write-Host 'smoke: round-trip byte-equal'

# 4: encode again -> same split manifest
& $S @A data (Join-Path $Workdir 'out2\part') *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "second encode failed: $LASTEXITCODE" }

function manifest([string]$Dir) {
  $lines = Get-ChildItem (Join-Path $Dir 'part.sss*') -File |
    ForEach-Object { "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)" } |
    Sort-Object
  $utf8 = New-Object System.Text.UTF8Encoding($false)
  [System.IO.File]::WriteAllText((Join-Path $Workdir ('manifest_' + (Split-Path $Dir -Leaf) + '.txt')),
    ($lines -join "`n") + "`n", $utf8)
}
manifest 'out1'
manifest 'out2'
if (Compare-Object (Get-Content (Join-Path $Workdir 'manifest_out1.txt')) (Get-Content (Join-Path $Workdir 'manifest_out2.txt'))) {
  Write-Error 'splits NOT deterministic across runs'
}
Write-Host 'smoke: splits deterministic across runs'

# 5: decoding many small files must not exhaust OS open-handle limits;
#    -1 regression: decode held every output open until the end (Windows CRT
#    defaults to 512 open FILE streams), so >~500 files failed with "cannot
#    create output". Kept bounded via LRU eviction (kMaxOpenOutputs = 400).
$many = Join-Path $Workdir 'many'
New-Item -ItemType Directory -Path $many -Force | Out-Null
$mbuf = New-Object byte[] 8192
$mrnd = [Random]::new(2026)
for ($i = 0; $i -lt 640; $i++) {
  $mrnd.NextBytes($mbuf)
  [System.IO.File]::WriteAllBytes((Join-Path $many ("t{0:D4}.bin" -f $i)), $mbuf)
}
& $S -e -f -v 0 -j 0 $many (Join-Path $Workdir 'manyout') *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "many-files encode failed: $LASTEXITCODE" }
$rm = Join-Path $Workdir 'rt-many'
if (Test-Path $rm) { Remove-Item $rm -Recurse -Force }
& $S -d -f -v 0 (Join-Path $Workdir 'manyout.sss001') $rm *> $null
if ($LASTEXITCODE -ne 0) { Write-Error "many-files decode failed: $LASTEXITCODE" }
if ((Get-ChildItem $rm -File).Count -ne 640) { Write-Error 'many-files: restored count mismatch' }
Write-Host 'smoke: many-files decode under handle budget'

# 6: corruption is detected, exit code 4
$p2 = (Get-ChildItem (Join-Path $Workdir 'out1') -Filter 'part.sss*' -File | Sort-Object Name)[1]
$bytes = [System.IO.File]::ReadAllBytes($p2.FullName)
$bytes[300] = $bytes[300] -bxor 0xFF
[System.IO.File]::WriteAllBytes($p2.FullName, $bytes)
$rc = Join-Path $Workdir 'rc'
if (Test-Path $rc) { Remove-Item $rc -Recurse -Force }
& $S -d --verify -f -v 0 (Join-Path $Workdir 'out1\part.sss001') $rc *> $null
if ($LASTEXITCODE -ne 4) { Write-Error "corruption: expected exit 4, got $LASTEXITCODE" }
Write-Host 'smoke: corruption -> exit 4'

# 7: cross-platform manifest for CI comparison (before corruption above)
$manifestDir = if ($env:SMOKE_MANIFEST_DIR) { $env:SMOKE_MANIFEST_DIR } else { $Workdir }
New-Item -ItemType Directory -Path $manifestDir -Force | Out-Null
Copy-Item (Join-Path $Workdir 'manifest_out1.txt') (Join-Path $manifestDir 'manifest.sha256')
Write-Host "manifest=$(Join-Path $manifestDir 'manifest.sha256')"
Write-Host 'SMOKE_OK'
exit 0