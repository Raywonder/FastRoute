<#
.SYNOPSIS
    Builds FastRoute and packages it into a distributable zip.

.DESCRIPTION
    Collects the built binaries and everything in docs\ into
    dist\FastRoute-<version>-<arch>.zip.

    The zip is flat: the exe sits at the top so it runs straight out of the
    extracted folder, with the documentation beside it. Nothing is installed
    and nothing is written outside the repository.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Version
    Version string used in the zip name. Defaults to whatever src\version.h
    says, so a zip is named for the build it contains without being told.

.PARAMETER SkipBuild
    Package whatever is already built instead of building first.

.EXAMPLE
    .\dist.ps1
    .\dist.ps1 -Version 1.0 -Configuration Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Version = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root  = $PSScriptRoot

# The version lives in src\version.h and nowhere else, so a release cannot be
# labelled with one number and built from another.
if (-not $Version) {
    $vh = Join-Path $PSScriptRoot 'src\version.h'
    $m = Select-String -Path $vh -Pattern '#define FASTROUTE_VERSION\s+"([^"]+)"'
    if (-not $m) { throw "No FASTROUTE_VERSION in $vh." }
    $Version = $m.Matches[0].Groups[1].Value
}
$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'
$arch  = 'x64'

function Say([string]$m) { Write-Host $m }

# --- build ----------------------------------------------------------------
if (-not $SkipBuild) {
    Say "Building $Configuration..."
    if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
        & cmake -S $root -B $build
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)." }
    }
    & cmake --build $build --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)." }
}

# --- gather ---------------------------------------------------------------
$binDir = Join-Path $build $Configuration
$exe    = Join-Path $binDir 'FastRoute.exe'
if (-not (Test-Path $exe)) {
    throw "No FastRoute.exe in $binDir. Build first, or drop -SkipBuild."
}

# A staging folder, so the zip contains exactly what is meant to be in it and
# nothing left over from a previous run.
$stage = Join-Path $dist "stage-$Configuration"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$payload = @()

# Binaries. The CRT is linked statically, so the exe is the whole program; any
# DLL that does appear beside it is picked up too rather than silently left out.
Copy-Item $exe -Destination $stage
$payload += 'FastRoute.exe'
foreach ($dll in Get-ChildItem -Path $binDir -Filter *.dll -File -ErrorAction SilentlyContinue) {
    Copy-Item $dll.FullName -Destination $stage
    $payload += $dll.Name
}

# Documentation: everything under docs\, keeping its structure. The repository
# README is deliberately not included - it documents how FastRoute is built,
# which is not what someone unpacking a zip is holding.
$docsSrc = Join-Path $root 'docs'
$docsDst = Join-Path $stage 'docs'
if (Test-Path $docsSrc) {
    $docFiles = Get-ChildItem -Path $docsSrc -File -Recurse
    if ($docFiles.Count -gt 0) {
        New-Item -ItemType Directory -Force -Path $docsDst | Out-Null
        Copy-Item -Path (Join-Path $docsSrc '*') -Destination $docsDst -Recurse -Force
        foreach ($f in $docFiles) {
            $payload += 'docs\' + $f.FullName.Substring($docsSrc.Length + 1)
        }
    } else {
        Say "docs\ is empty, so nothing from it is in this build."
    }
} else {
    Say "No docs\ folder, so nothing from it is in this build."
}

# --- zip ------------------------------------------------------------------
$name = "FastRoute-$Version-$arch"
if ($Configuration -ne 'Release') { $name += "-$Configuration" }
$zip = Join-Path $dist "$name.zip"

if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -Recurse -Force $stage

# --- report ---------------------------------------------------------------
$size = [math]::Round((Get-Item $zip).Length / 1MB, 2)
Say ""
Say "$zip  ($size MB)"
foreach ($p in $payload) { Say "  $p" }
