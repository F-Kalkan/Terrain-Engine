<#
.SYNOPSIS
    Builds, tests and packages the terrain engine and TerrainBench.

.DESCRIPTION
    One entry point for everything CI, a release and a local build need:

        .\build.ps1                          # engine, CLI, DLL and app: build and test
        .\build.ps1 -Zip                     # + a portable, self-contained win-x64 zip in artifacts/
        .\build.ps1 -Zip -Version 1.2.0      # a release build, stamped with that version

    Steps, each stopping the build on failure: build TerrainEngine.sln (Release | x64); check the
    DLL needs no runtime installed; run the engine's test executable; build and test the .NET
    solution; and, with -Zip, publish TerrainBench self-contained and pack it.

    Needs Visual Studio 2022 with the C++ desktop workload (or its Build Tools) and the .NET 10 SDK.

.PARAMETER Version
    The version stamped into the DLL and the app. Defaults to the VersionPrefix in
    app/Directory.Build.props. A release passes the tag's version.

.PARAMETER Commit
    The commit stamped into the DLL. Defaults to the checked-out commit.
#>

[CmdletBinding()]
param(
    [string] $Version,
    [string] $Commit,
    [switch] $Zip,
    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$appSolution = Join-Path $root 'app/TerrainBench.sln'
$artifacts = Join-Path $root 'artifacts'
$publishDir = Join-Path $artifacts 'publish'
$engineOut = Join-Path $root 'x64/Release'

function Write-Step([string] $message) {
    Write-Host ''
    Write-Host "==> $message" -ForegroundColor Cyan
}

function Invoke-Checked([string] $what, [scriptblock] $action) {
    & $action
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE." }
}

function Find-VsTool([string] $pattern) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    return & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find $pattern | Select-Object -First 1
}

if (-not $Version) {
    $props = Get-Content (Join-Path $root 'app/Directory.Build.props') -Raw
    $Version = if ($props -match '<VersionPrefix>([^<]+)</VersionPrefix>') { $Matches[1] } else { '0.0.0' }
}
if (-not $Commit) {
    $Commit = (git -C $root rev-parse HEAD 2>$null)
    if (-not $Commit) { $Commit = 'unknown' }
}
if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') { throw "Expected a version like 1.0.0, got '$Version'." }
if ($Commit -notmatch '^[0-9A-Za-z._-]+$') { throw "Unexpected commit '$Commit'." }

Write-Host "TerrainBench $Version, commit $Commit" -ForegroundColor Green

$onPath = Get-Command 'MSBuild.exe' -ErrorAction SilentlyContinue
$msbuild = if ($onPath) { $onPath.Source } else { Find-VsTool 'MSBuild\**\Bin\MSBuild.exe' }
if (-not $msbuild) { throw 'MSBuild was not found. Install Visual Studio 2022 with the "Desktop development with C++" workload.' }

Write-Step 'Building the engine, the CLI and the DLL (Release | x64)'
Invoke-Checked 'The C++ build' {
    & $msbuild (Join-Path $root 'TerrainEngine.sln') /t:Rebuild /p:Configuration=Release /p:Platform=x64 `
        "/p:TeEngineVersion=$Version" "/p:TeEngineCommit=$Commit" /m /nologo /v:m
}

Write-Step 'Checking the DLL needs nothing installed'
$dumpbin = Find-VsTool 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe'
if ($dumpbin) {
    $dependencies = & $dumpbin /nologo /dependents (Join-Path $engineOut 'TerrainEngineApi.dll') |
        Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    $unexpected = @($dependencies | Where-Object { $_ -ne 'KERNEL32.dll' })
    if ($unexpected.Count -gt 0) { throw "TerrainEngineApi.dll depends on $($unexpected -join ', '); it must link its runtime statically (/MT)." }
    Write-Host "    depends only on: $($dependencies -join ', ')"
}
else {
    Write-Host '    dumpbin not found; skipped.'
}

if (-not $SkipTests) {
    Write-Step "Running the engine's test executable"
    Push-Location $root
    try {
        Invoke-Checked 'The engine tests' { & (Join-Path $engineOut 'TerrainEngine.exe') }
    }
    finally {
        Pop-Location
    }
}

$versionArgs = @(
    "-p:Version=$Version",
    "-p:AssemblyVersion=$Version.0",
    "-p:FileVersion=$Version.0",
    "-p:InformationalVersion=$Version"
)

Write-Step 'Building the app'
Invoke-Checked 'restore' { dotnet restore $appSolution }
Invoke-Checked 'The app build' { dotnet build $appSolution -c Release --no-restore @versionArgs }

if (-not $SkipTests) {
    Write-Step "Running the app's tests"
    Invoke-Checked 'The app tests' { dotnet test $appSolution -c Release --no-build }
}

if ($Zip) {
    Write-Step 'Publishing TerrainBench, self-contained win-x64'
    if (Test-Path $publishDir) { Remove-Item $publishDir -Recurse -Force }
    Invoke-Checked 'publish' {
        dotnet publish (Join-Path $root 'app/src/TerrainBench/TerrainBench.csproj') -c Release -r win-x64 --self-contained true `
            -p:PublishSingleFile=false -p:DebugType=none @versionArgs -o $publishDir
    }

    foreach ($required in 'TerrainBench.exe', 'TerrainEngineApi.dll', 'Samples/N36W112.hgt') {
        if (-not (Test-Path (Join-Path $publishDir $required))) { throw "The published app is missing $required." }
    }

    Write-Step 'Packing the portable archive'
    New-Item -ItemType Directory -Force $artifacts | Out-Null
    $zipPath = Join-Path $artifacts "TerrainBench-$Version-win-x64-portable.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path (Join-Path $publishDir '*') -DestinationPath $zipPath
    $sizeMb = [math]::Round((Get-Item $zipPath).Length / 1MB, 1)
    Write-Host "    $zipPath ($sizeMb MB)"
}

Write-Host ''
Write-Host 'Done.' -ForegroundColor Green
