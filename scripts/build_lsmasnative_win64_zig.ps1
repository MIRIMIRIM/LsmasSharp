param(
    [string]$LsmasWorksDir = "build-deps\\L-SMASH-Works",
    [string]$LsmasWorksRepo = "https://github.com/AkarinVS/L-SMASH-Works.git",
    [string]$LsmasWorksRef = "master",
    [string]$LsmasWorksRefLabel = "",
    [string]$FfmpegPrefix = "build-deps\\ffmpeg-lsmas-win64-local-mingw-static",
    [string]$OutDir = "artifacts\\native\\win-x64",
    [switch]$Release
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

function Resolve-RepoPath([string]$Path, [switch]$MustExist) {
    $full = if ([System.IO.Path]::IsPathRooted($Path)) {
        [System.IO.Path]::GetFullPath($Path)
    } else {
        [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Path))
    }

    if ($MustExist.IsPresent -and -not (Test-Path -LiteralPath $full)) {
        throw "Path does not exist: $full"
    }

    return $full
}

$LsmasWorksDirFull = Resolve-RepoPath $LsmasWorksDir
$FfmpegPrefixFull = Resolve-RepoPath $FfmpegPrefix -MustExist
$OutDirFull = Resolve-RepoPath $OutDir
New-Item -ItemType Directory -Force -Path $OutDirFull | Out-Null

if ($Release.IsPresent) { $opt = "-Doptimize=ReleaseFast" } else { $opt = "-Doptimize=Debug" }

Write-Host "Building lsmasnative via zig" -ForegroundColor Cyan
Write-Host "  L-SMASH-WORKS:  $LsmasWorksDirFull" -ForegroundColor Gray
Write-Host "  LSW repo/ref:    $LsmasWorksRepo @ $LsmasWorksRef" -ForegroundColor Gray
Write-Host "  FFmpeg prefix: $FfmpegPrefixFull" -ForegroundColor Gray
Write-Host "  Output:        $OutDirFull" -ForegroundColor Gray
Write-Host ""

function Try-RunGit([string]$WorkingDir, [string[]]$GitArgs) {
    try {
        Push-Location $WorkingDir
        try {
            $out = (& git @GitArgs 2>$null)
            if ($LASTEXITCODE -ne 0) { return $null }
            return ($out | Out-String).Trim()
        } finally {
            Pop-Location
        }
    } catch {
        return $null
    }
}

function Invoke-GitChecked([string]$WorkingDir, [string[]]$GitArgs) {
    Push-Location $WorkingDir
    try {
        & git @GitArgs | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "git $($GitArgs -join ' ') failed with exit code $LASTEXITCODE."
        }
    } finally {
        Pop-Location
    }
}

function Fetch-GitRef([string]$WorkingDir, [string]$Ref) {
    $candidates = @($Ref, "refs/heads/$Ref", "refs/tags/$Ref") | Select-Object -Unique
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        Push-Location $WorkingDir
        try {
            & git -c http.version=HTTP/1.1 fetch --depth 1 origin $candidate | Out-Host
            if ($LASTEXITCODE -eq 0) { return }
        } finally {
            Pop-Location
        }
    }

    throw "Could not fetch ref '$Ref' from origin in $WorkingDir."
}

function Ensure-GitCheckout([string]$Dir, [string]$Repo, [string]$Ref) {
    if ([string]::IsNullOrWhiteSpace($Ref)) { $Ref = "master" }

    if (-not (Test-Path (Join-Path $Dir ".git"))) {
        if ((Test-Path $Dir) -and (Get-ChildItem -Force -LiteralPath $Dir -ErrorAction SilentlyContinue)) {
            throw "Dependency directory exists but is not a git checkout: $Dir"
        }

        $parent = Split-Path -Parent $Dir
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        Write-Host "Cloning L-SMASH-WORKS dependency..." -ForegroundColor Cyan
        git clone --no-checkout $Repo $Dir | Out-Host
        if ($LASTEXITCODE -ne 0) {
            throw "git clone L-SMASH-WORKS failed with exit code $LASTEXITCODE."
        }
    }

    Invoke-GitChecked $Dir @("remote", "set-url", "origin", $Repo)
    Fetch-GitRef $Dir $Ref
    Invoke-GitChecked $Dir @("checkout", "-f", "FETCH_HEAD")
    Invoke-GitChecked $Dir @("submodule", "update", "--init", "--recursive", "--depth", "1")
}

function Get-RelativePath([string]$FromDir, [string]$ToPath) {
    $fromFull = (Resolve-Path $FromDir).Path.TrimEnd('\') + '\'
    $toFull = (Resolve-Path $ToPath).Path
    $fromUri = [System.Uri]::new($fromFull)
    $toUri = [System.Uri]::new($toFull)
    $rel = $fromUri.MakeRelativeUri($toUri).ToString()
    return [System.Uri]::UnescapeDataString($rel).Replace('/', '\\')
}

Ensure-GitCheckout $LsmasWorksDirFull $LsmasWorksRepo $LsmasWorksRef

$lsmashworksGitUrl = Try-RunGit $LsmasWorksDirFull @("config", "--get", "remote.origin.url")
if ([string]::IsNullOrWhiteSpace($lsmashworksGitUrl)) { $lsmashworksGitUrl = $LsmasWorksRepo }
$lsmashworksGitHead = Try-RunGit $LsmasWorksDirFull @("rev-parse", "--short=12", "HEAD")
if ([string]::IsNullOrWhiteSpace($lsmashworksGitHead)) { $lsmashworksGitHead = "unknown" }
$lsmashworksGitBranch = $null
if (-not [string]::IsNullOrWhiteSpace($LsmasWorksRefLabel)) {
    $lsmashworksGitBranch = $LsmasWorksRefLabel
} else {
    $lsmashworksGitBranch = Try-RunGit $LsmasWorksDirFull @("rev-parse", "--abbrev-ref", "HEAD")
}
if ([string]::IsNullOrWhiteSpace($lsmashworksGitBranch)) { $lsmashworksGitBranch = "unknown" }
if ($lsmashworksGitBranch -eq "HEAD" -and -not [string]::IsNullOrWhiteSpace($LsmasWorksRef)) { $lsmashworksGitBranch = $LsmasWorksRef }

$lsmasnativeGitHead = Try-RunGit $RepoRoot @("rev-parse", "--short=12", "HEAD")
if ([string]::IsNullOrWhiteSpace($lsmasnativeGitHead)) { $lsmasnativeGitHead = "unknown" }

$ffmpegGitUrl = $null
$ffmpegGitHead = $null
$ffmpegGitBranch = $null
$ffmpegRemoteFile = Join-Path $FfmpegPrefixFull "lsmas-remote.txt"
if (Test-Path $ffmpegRemoteFile) { $ffmpegGitUrl = (Get-Content $ffmpegRemoteFile -Raw).Trim() }
$ffmpegHeadFile = Join-Path $FfmpegPrefixFull "lsmas-commit-short.txt"
if (Test-Path $ffmpegHeadFile) { $ffmpegGitHead = (Get-Content $ffmpegHeadFile -Raw).Trim() }
$ffmpegBranchFile = Join-Path $FfmpegPrefixFull "lsmas-branch.txt"
if (Test-Path $ffmpegBranchFile) { $ffmpegGitBranch = (Get-Content $ffmpegBranchFile -Raw).Trim() }
if ([string]::IsNullOrWhiteSpace($ffmpegGitUrl)) { $ffmpegGitUrl = "https://github.com/AkarinVS/FFmpeg" }
if ([string]::IsNullOrWhiteSpace($ffmpegGitHead)) {
    $ffmpegFullFile = Join-Path $FfmpegPrefixFull "lsmas-commit.txt"
    if (Test-Path $ffmpegFullFile) {
        $full = (Get-Content $ffmpegFullFile -Raw).Trim()
        if ($full.Length -ge 12) { $ffmpegGitHead = $full.Substring(0, 12) } else { $ffmpegGitHead = $full }
    }
}
if ([string]::IsNullOrWhiteSpace($ffmpegGitHead)) { $ffmpegGitHead = "unknown" }
if ([string]::IsNullOrWhiteSpace($ffmpegGitBranch)) { $ffmpegGitBranch = "lsmas" }

$dav1dVer = $null
$dav1dVerFile = Join-Path $FfmpegPrefixFull "dav1d-version.txt"
if (Test-Path $dav1dVerFile) { $dav1dVer = (Get-Content $dav1dVerFile -Raw).Trim() }
if ([string]::IsNullOrWhiteSpace($dav1dVer)) { $dav1dVer = "unknown" }

$zlibVer = $null
$zlibVerFile = Join-Path $FfmpegPrefixFull "zlib-version.txt"
if (Test-Path $zlibVerFile) { $zlibVer = (Get-Content $zlibVerFile -Raw).Trim() }
if ([string]::IsNullOrWhiteSpace($zlibVer)) { $zlibVer = "unknown" }

$NativeDir = Join-Path $RepoRoot "src\\LsmasNative"
$LsmasWorksDirForZig = Get-RelativePath $NativeDir $LsmasWorksDirFull
$FfmpegPrefixForZig = Get-RelativePath $NativeDir $FfmpegPrefixFull
Push-Location $NativeDir
try {
    $ver = $null
    try {
        $ver = (git describe --tags --always --dirty 2>$null)
    } catch {
        $ver = $null
    }
    if ([string]::IsNullOrWhiteSpace($ver)) { $ver = "0.0.0-dev" }

    $zigVer = $null
    try {
        $zigVer = (zig version 2>$null)
    } catch {
        $zigVer = $null
    }
    if ([string]::IsNullOrWhiteSpace($zigVer)) { $zigVer = "unknown" }

    zig build `
        -Dtarget=x86_64-windows-gnu `
        $opt `
        -Dffmpeg_static=true `
        -Dffmpeg_prefix="$FfmpegPrefixForZig" `
        -Dl_smash_works_dir="$LsmasWorksDirForZig" `
        -Dlsmasnative_version="$ver" `
        -Dlsmasnative_zig_version="$zigVer" `
        -Dlsmasnative_git_head="$lsmasnativeGitHead" `
        -Dlsmashworks_git_url="$lsmashworksGitUrl" `
        -Dlsmashworks_git_head="$lsmashworksGitHead" `
        -Dlsmashworks_git_branch="$lsmashworksGitBranch" `
        -Dffmpeg_git_url="$ffmpegGitUrl" `
        -Dffmpeg_git_head="$ffmpegGitHead" `
        -Dffmpeg_git_branch="$ffmpegGitBranch" `
        -Ddav1d_version="$dav1dVer" `
        -Dzlib_version="$zlibVer"
    if ($LASTEXITCODE -ne 0) {
        throw "zig build failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$nativeBin = Join-Path $RepoRoot "src\\LsmasNative\\zig-out\\bin"
Copy-Item -Force -Path (Join-Path $nativeBin "lsmasnative.dll") -Destination $OutDirFull
if (Test-Path (Join-Path $nativeBin "lsmasnative.pdb")) {
    Copy-Item -Force -Path (Join-Path $nativeBin "lsmasnative.pdb") -Destination $OutDirFull
}

Write-Host ""
Write-Host "Done. Native binaries in: $OutDirFull" -ForegroundColor Green
