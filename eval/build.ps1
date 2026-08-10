# Build the evaluation shared library with Android NDK (arm64-v8a).
param(
  [string]$NdkRoot = "$env:LOCALAPPDATA\Android\Sdk\ndk\27.2.12479018",
  [string]$Api = "21",
  [string]$Opt = "O1"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $Root "build"
$Corpus = Join-Path $Root "corpus"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$Prebuilt = Join-Path $NdkRoot "toolchains\llvm\prebuilt\windows-x86_64\bin"
$Clang = Join-Path $Prebuilt "aarch64-linux-android$Api-clang.cmd"
$Nm = Join-Path $Prebuilt "llvm-nm.exe"
$Readelf = Join-Path $Prebuilt "llvm-readelf.exe"

foreach ($tool in @($Clang, $Nm, $Readelf)) {
  if (-not (Test-Path $tool)) {
    throw "Missing NDK tool: $tool`nSet -NdkRoot to a valid NDK install."
  }
}

$So = Join-Path $OutDir "libxdec_eval.so"
$Map = Join-Path $OutDir "symbols.txt"

# Every source*.c in the corpus, linked into the one library the run
# decompiles. Discovered rather than listed so that adding a corpus file is one
# edit and not two, and sorted so the layout of the .so does not depend on the
# order the filesystem happens to return.
$Sources = Get-ChildItem -Path $Corpus -Filter "source*.c" | Sort-Object Name
if ($Sources.Count -eq 0) {
  throw "No source*.c in $Corpus"
}

Write-Host "NDK: $NdkRoot"
Write-Host "Building $So from $($Sources.Count) source file(s):"
$Sources | ForEach-Object { Write-Host "  $($_.Name)" }

& $Clang `
  -shared -fPIC `
  "-$Opt" -g `
  -fstack-protector-strong `
  -Wall -Wextra `
  -o $So `
  ($Sources | ForEach-Object { $_.FullName })

if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $Nm -g --defined-only $So |
  Where-Object { $_ -match '\beval_' } |
  Set-Content -Encoding utf8 $Map

Write-Host "Symbols ($((Get-Content $Map).Count) eval_*):"
Get-Content $Map | ForEach-Object { Write-Host "  $_" }

$Info = Join-Path $OutDir "readelf.txt"
& $Readelf -h -s $So 2>&1 | Set-Content -Encoding utf8 $Info
Write-Host "Wrote $So"
Write-Host "Wrote $Map"
