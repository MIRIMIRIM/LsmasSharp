param(
    [ValidateSet("AkarinVS", "HOE")]
    [string]$Variant = "AkarinVS",
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$ArtifactsDir = "artifacts",
    [int]$Jobs = 0,
    [switch]$SkipFfmpeg,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

if ($Jobs -le 0) {
    $Jobs = [Environment]::ProcessorCount
}

$ffmpegRepo = "https://github.com/AkarinVS/FFmpeg"
$ffmpegRef = "lsmas"
$lsmasWorksRepo = "https://github.com/AkarinVS/L-SMASH-Works.git"
$lsmasWorksRef = "master"
$lsmasWorksDir = "build-deps\L-SMASH-Works-AkarinVS"
$ffmpegPrefix = "build-deps\ffmpeg-lsmas-win64-local-mingw-static"

if ($Variant -eq "HOE") {
    $ffmpegRepo = "https://github.com/HomeOfAviSynthPlusEvolution/FFmpeg"
    $ffmpegRef = "custom-patches-for-lsmashsource"
    $lsmasWorksRepo = "https://github.com/HomeOfAviSynthPlusEvolution/L-SMASH-Works.git"
    $lsmasWorksRef = "master"
    $lsmasWorksDir = "build-deps\L-SMASH-Works-HOE"
    $ffmpegPrefix = "build-deps\ffmpeg-hoe-win64-local-mingw-static"
}

$nativeOut = Join-Path $ArtifactsDir "native\win-x64"

Write-Host "Building LsmasSharp win64 ($Variant, $Configuration)" -ForegroundColor Cyan
Write-Host "  Repo root:       $RepoRoot" -ForegroundColor Gray
Write-Host "  FFmpeg prefix:   $(Resolve-RepoPath $ffmpegPrefix)" -ForegroundColor Gray
Write-Host "  L-SMASH-Works:   $(Resolve-RepoPath $lsmasWorksDir)" -ForegroundColor Gray
Write-Host "  Native output:   $(Resolve-RepoPath $nativeOut)" -ForegroundColor Gray
Write-Host ""

if (-not $SkipFfmpeg.IsPresent) {
    $ffmpegArgs = @{
        OutputDir = $ffmpegPrefix
        FfmpegRepo = $ffmpegRepo
        FfmpegRef = $ffmpegRef
        Jobs = $Jobs
    }

    if ($Clean.IsPresent) {
        $ffmpegArgs.Clean = $true
    }

    & (Join-Path $PSScriptRoot "build_ffmpeg_lsmas_win64_local_mingw_static_wsl.ps1") @ffmpegArgs
}

$nativeArgs = @{
    LsmasWorksDir = $lsmasWorksDir
    LsmasWorksRepo = $lsmasWorksRepo
    LsmasWorksRef = $lsmasWorksRef
    FfmpegPrefix = $ffmpegPrefix
    OutDir = $nativeOut
}

if ($Configuration -eq "Release") {
    $nativeArgs.Release = $true
}

& (Join-Path $PSScriptRoot "build_lsmasnative_win64_zig.ps1") @nativeArgs

dotnet build (Join-Path $RepoRoot "LsmasSharp.slnx") -c $Configuration

Write-Host ""
Write-Host "Done. Native binaries are in: $(Resolve-RepoPath $nativeOut)" -ForegroundColor Green
