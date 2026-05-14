param(
    [string]$OutputDir = "build-deps\\ffmpeg-lsmas-win64-local-mingw-static",
    [string]$FfmpegRepo = "https://github.com/AkarinVS/FFmpeg",
    [string]$FfmpegRef = "lsmas",
    [string]$LsMashRepo = "",
    [string]$LsMashRef = "",
    [int]$Jobs = 0,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Convert-ToWslPath([string]$path) {
    $full = (Resolve-Path $path).Path
    $drive = $full.Substring(0, 1).ToLowerInvariant()
    $rest = ($full.Substring(2) -replace '\\', '/')
    return "/mnt/$drive$rest"
}

if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
}

$OutputDirFull = Resolve-RepoPath $OutputDir
New-Item -ItemType Directory -Force -Path $OutputDirFull | Out-Null
$PrefixWsl = Convert-ToWslPath $OutputDirFull

Write-Host "WSL build (no sudo; local-extracted mingw-w64; static libs) for FFmpeg (ref: $FfmpegRef)" -ForegroundColor Cyan
Write-Host "  Repo:   $FfmpegRepo" -ForegroundColor Gray
if (-not [string]::IsNullOrWhiteSpace($LsMashRepo)) {
    Write-Host "  L-SMASH: $LsMashRepo (ref: $LsMashRef)" -ForegroundColor Gray
}
Write-Host "  Output: $OutputDirFull" -ForegroundColor Gray
Write-Host "  Prefix: $PrefixWsl" -ForegroundColor Gray
Write-Host ""

if ($Clean.IsPresent) { $cleanFlag = "1" } else { $cleanFlag = "0" }

$BuildScriptWin = Join-Path $PSScriptRoot "build_ffmpeg_lsmas_win64_local_mingw_static.sh"
$BuildScriptWsl = Convert-ToWslPath $BuildScriptWin

$wslbash = "$env:WINDIR\\System32\\bash.exe"
if (-not (Test-Path $wslbash)) { throw "WSL bash.exe not found at: $wslbash" }

$lsmashRepoArg = $LsMashRepo
$lsmashRefArg = $LsMashRef
if ([string]::IsNullOrWhiteSpace($lsmashRepoArg)) { $lsmashRepoArg = "" }
if ([string]::IsNullOrWhiteSpace($lsmashRefArg)) { $lsmashRefArg = "" }

$cmd = "bash '$BuildScriptWsl' '$PrefixWsl' '$Jobs' '$cleanFlag' '$FfmpegRepo' '$FfmpegRef' '$lsmashRepoArg' '$lsmashRefArg'"
$prevEap = $ErrorActionPreference
try {
    # Windows PowerShell treats native stderr as non-terminating errors; don't let that abort the build.
    $ErrorActionPreference = "Continue"
    & $wslbash -lc "$cmd"
} finally {
    $ErrorActionPreference = $prevEap
}
if ($LASTEXITCODE -ne 0) {
    throw "WSL FFmpeg(local mingw static) build failed with exit code $LASTEXITCODE."
}

Write-Host ""
Write-Host "Done. Commit pinned at: $OutputDirFull\\lsmas-commit.txt" -ForegroundColor Green
