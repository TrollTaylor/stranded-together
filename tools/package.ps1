<#
.SYNOPSIS
  Builds the Stranded Together release zip: the files a player extracts into
  the DEATH STRANDING DIRECTOR'S CUT game folder (next to ds.exe).

.DESCRIPTION
  Stages dist\StrandedTogether-<version>\ and zips it to
  dist\StrandedTogether-<version>.zip. The zip's root is the game folder:

    StrandedTogether.exe                      the launcher (Host / Join / Solo test / Debug)
    winhttp.dll                               the mod (a winhttp proxy ds.exe loads from its folder)
    stranded_together\package.json           version, build id, SHA-256 of every file, source commit
    stranded_together\README.txt             install, play, debug, uninstall
    stranded_together\THIRD_PARTY_NOTICES.txt  MinHook and Dear ImGui licences

  The launcher checks winhttp.dll (and itself) against package.json; see
  docs/packaging.md. Both binaries always come from the same build folder, so
  their build ids match. The zip is also what the launcher's updater installs
  (a GitHub release asset named exactly StrandedTogether-<version>.zip): after
  zipping, the script runs the launcher's own check of it
  (st_launcher_update_tests --verify-zip) when that program is built.
  tools/release.ps1 calls this script for a release (docs/releasing.md).

.EXAMPLE
  # Configure (first time), build and package into dist\
  .\tools\package.ps1

.EXAMPLE
  # Package what an existing build folder already holds
  .\tools\package.ps1 -BuildDir build -NoBuild
#>
param(
    # The CMake build folder (default: <repo>\build-package, configured on first use).
    [string]$BuildDir = "",
    [string]$Config = "Release",
    # Package the binaries already in -BuildDir instead of building them.
    [switch]$NoBuild,
    # Skip the launcher's unit tests before packaging (the zip check still runs).
    [switch]$SkipTests,
    # Where the staged folder and the zip go (default: <repo>\dist).
    [string]$OutputRoot = ""
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2
$root = Split-Path -Parent $PSScriptRoot

function Resolve-Under([string]$Path, [string]$Default) {
    if (-not $Path) { $Path = $Default }
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $root $Path }
    return [IO.Path]::GetFullPath($Path)
}

function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant() }

# The same test the launcher applies (core_install LooksLikeOurDll): a PE image with the
# proxy's GLOBALROOT forwarders that names the stranded_together folder.
function Test-OurDll([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $false }
    $latin = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    if (-not $latin.Contains('\\.\GLOBALROOT\SystemRoot\System32\winhttp.dll.')) { return $false }
    $wide = [Text.Encoding]::GetEncoding(28591).GetString([Text.Encoding]::Unicode.GetBytes('stranded_together'))
    return $latin.Contains('stranded_together') -or $latin.Contains($wide)
}

function Write-Utf8Lf([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding($false)))
}

$BuildDir = Resolve-Under $BuildDir "build-package"
$OutputRoot = Resolve-Under $OutputRoot "dist"

# ---- Build -----------------------------------------------------------------------
if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $BuildDir "CMakeCache.txt"))) {
        Write-Host "configuring $BuildDir"
        & cmake -S $root -B $BuildDir -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    }
    Write-Host "building st_mod and st_launcher ($Config)"
    & cmake --build $BuildDir --config $Config --target st_mod st_launcher st_launcher_core_tests st_launcher_update_tests
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
$bin = Join-Path $BuildDir $Config
$dll = Join-Path $bin "winhttp.dll"
$exe = Join-Path $bin "StrandedTogether.exe"
foreach ($f in @($dll, $exe)) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "missing $f (build it, or drop -NoBuild)" }
}
if (-not (Test-OurDll $dll)) { throw "$dll is not the Stranded Together winhttp.dll proxy" }
if ($NoBuild) {
    # -NoBuild packages whatever the folder holds: say so when a binary is older than its sources.
    foreach ($pair in @(@($dll, @("native\mod", "native\net", "native\include")), @($exe, @("native\launcher", "native\net", "native\include")))) {
        $built = (Get-Item -LiteralPath $pair[0]).LastWriteTimeUtc
        $newer = @(foreach ($d in $pair[1]) {
                Get-ChildItem -LiteralPath (Join-Path $root $d) -Recurse -File -Include *.cpp, *.hpp, *.h, *.inc -ErrorAction SilentlyContinue |
                    Where-Object { $_.LastWriteTimeUtc -gt $built }
            })
        if ($newer.Count -gt 0) {
            Write-Warning "$($pair[0]) is older than $($newer.Count) source file(s) (e.g. $($newer[0].FullName.Substring($root.Length + 1))); rebuild or drop -NoBuild"
        }
    }
}
if (-not $SkipTests) {
    foreach ($t in @("st_launcher_core_tests", "st_launcher_update_tests")) {
        $tests = Join-Path $bin "$t.exe"
        if (Test-Path $tests) {
            & $tests | Select-Object -Last 1 | Write-Host
            if ($LASTEXITCODE -ne 0) { throw "$t failed; fix it or pass -SkipTests" }
        } else {
            Write-Warning "$t.exe not built; tests skipped"
        }
    }
}

# ---- Version and provenance --------------------------------------------------------
$versionHeader = Join-Path $BuildDir "generated\launcher\st_launcher_version.h"
if (-not (Test-Path $versionHeader)) { throw "missing $versionHeader (configure the build folder first)" }
$header = Get-Content -Raw -LiteralPath $versionHeader
$buildId = [regex]::Match($header, '#define ST_VERSION_STRING "([^"]+)"').Groups[1].Value
$major = [regex]::Match($header, '#define ST_VERSION_MAJOR (\d+)').Groups[1].Value
$minor = [regex]::Match($header, '#define ST_VERSION_MINOR (\d+)').Groups[1].Value
$patch = [regex]::Match($header, '#define ST_VERSION_PATCH (\d+)').Groups[1].Value
if (-not $buildId -or -not $major) { throw "cannot read the version from $versionHeader" }
$version = "$major.$minor.$patch"

$commit = $null
$dirty = $null
try {
    $out = @(& git -C $root rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $out.Count -eq 1 -and $out[0] -match '^[0-9a-f]{40}$') {
        $commit = $out[0]
        $dirty = (@(& git -C $root status --porcelain 2>$null).Count -gt 0)
    }
} catch { $commit = $null }
$installHpp = Get-Content -Raw -LiteralPath (Join-Path $root "native\launcher\core\core_install.hpp")
$gameExeSha = [regex]::Match($installHpp, 'kExeSha256\[\] = "([0-9A-F]{64})"').Groups[1].Value
$steamApiSha = [regex]::Match($installHpp, 'kSteamApiSha256\[\] = "([0-9A-F]{64})"').Groups[1].Value

# ---- Stage -----------------------------------------------------------------------------
$name = "StrandedTogether-$version"
New-Item -ItemType Directory -Force $OutputRoot | Out-Null
if ($OutputRoot -ieq (Join-Path $root "dist")) {
    # dist\ is build output: keep it out of git without touching the repository's .gitignore.
    $ignore = Join-Path $OutputRoot ".gitignore"
    if (-not (Test-Path $ignore)) { Write-Utf8Lf $ignore "# Release packages made by tools/package.ps1`n*`n" }
}
$stage = Join-Path $OutputRoot $name
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$modDir = Join-Path $stage "stranded_together"
New-Item -ItemType Directory -Force $modDir | Out-Null
Copy-Item -LiteralPath $exe -Destination (Join-Path $stage "StrandedTogether.exe")
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage "winhttp.dll")

$readme = (Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot "launcher\package-readme.txt")).Replace("@VERSION@", $version).Replace("@BUILD_ID@", $buildId)
[IO.File]::WriteAllText((Join-Path $modDir "README.txt"), ($readme -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))
$notices = "Third-party software in Stranded Together $version`r`n" +
    "=================================================`r`n`r`n" +
    "winhttp.dll contains MinHook (third_party/minhook) and Dear ImGui (third_party/imgui, the in-game debug menu); StrandedTogether.exe contains Dear ImGui.`r`n`r`n" +
    "-------------------------------------------------------------------------------`r`n" +
    "MinHook (BSD 2-Clause License)`r`n" +
    "-------------------------------------------------------------------------------`r`n" +
    ((Get-Content -Raw -LiteralPath (Join-Path $root "third_party\minhook\LICENSE.txt")).TrimStart([char]0xFEFF).Trim()) + "`r`n`r`n" +
    "-------------------------------------------------------------------------------`r`n" +
    "Dear ImGui (MIT License)`r`n" +
    "-------------------------------------------------------------------------------`r`n" +
    ((Get-Content -Raw -LiteralPath (Join-Path $root "third_party\imgui\LICENSE.txt")).TrimStart([char]0xFEFF).Trim()) + "`r`n"
[IO.File]::WriteAllText((Join-Path $modDir "THIRD_PARTY_NOTICES.txt"), ($notices -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding($false)))

$files = @()
foreach ($rel in @("winhttp.dll", "StrandedTogether.exe", "stranded_together/README.txt", "stranded_together/THIRD_PARTY_NOTICES.txt")) {
    $p = Join-Path $stage ($rel -replace '/', '\')
    $files += [ordered]@{ path = $rel; sha256 = (Get-Sha256 $p); size = (Get-Item -LiteralPath $p).Length }
}
$package = [ordered]@{
    product       = "stranded-together-package"
    v             = 1
    version       = $version
    build_id      = $buildId
    built_utc     = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
    config        = $Config
    source_commit = $commit
    source_dirty  = $dirty
    game          = [ordered]@{ app_id = 1850570; ds_exe_sha256 = $gameExeSha; steam_api64_sha256 = $steamApiSha }
    files         = $files
}
Write-Utf8Lf (Join-Path $modDir "package.json") (($package | ConvertTo-Json -Depth 6) + "`n")

# ---- Zip (forward slashes in entry names, whatever the PowerShell version) ------------------
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipPath = Join-Path $OutputRoot "$name.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$entries = @("StrandedTogether.exe", "winhttp.dll", "stranded_together/package.json", "stranded_together/README.txt", "stranded_together/THIRD_PARTY_NOTICES.txt")
# Round 30: the side seats' pose cache (game animation data the mod learns at runtime, %LOCALAPPDATA%\StrandedTogether\mule_ride_lib.bin)
# is never shipped: not staged, not an entry
$poseCache = @(Get-ChildItem -LiteralPath $stage -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*mule_ride_lib*' })
if ($poseCache.Count -gt 0) { throw "the stage holds the side seats' pose cache ($($poseCache[0].FullName)): it is never shipped" }
if (@($entries | Where-Object { $_ -like '*mule_ride_lib*' }).Count -gt 0) { throw "the zip would hold the side seats' pose cache: it is never shipped" }
$zip = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($e in $entries) {
        $src = Join-Path $stage ($e -replace '/', '\')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $src, $e, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $zip.Dispose() }

# ---- Verify the zip: every entry is there and every file matches package.json ------------------
$check = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $names = @($check.Entries | ForEach-Object { $_.FullName })
    foreach ($e in $entries) { if ($names -notcontains $e) { throw "zip is missing $e" } }
    if ($names.Count -ne $entries.Count) { throw "zip has unexpected entries: $($names -join ', ')" }
    if (@($names | Where-Object { $_ -like '*mule_ride_lib*' }).Count -gt 0) { throw "zip holds the side seats' pose cache: it is never shipped" }
    $sha = [Security.Cryptography.SHA256]::Create()
    foreach ($row in $files) {
        $s = $check.GetEntry($row.path).Open()
        try { $h = ([BitConverter]::ToString($sha.ComputeHash($s)) -replace '-', '') } finally { $s.Dispose() }
        if ($h -ne $row.sha256) { throw "zip entry $($row.path) does not match package.json" }
    }
} finally { $check.Dispose() }
# The launcher's own check (what Update now verifies before installing).
$verifier = Join-Path $bin "st_launcher_update_tests.exe"
if (Test-Path -LiteralPath $verifier) {
    $out = @(& $verifier --verify-zip $zipPath $version)
    $out | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "the launcher's updater would refuse $zipPath" }
} else {
    Write-Warning "st_launcher_update_tests.exe not built: the launcher's zip check was skipped"
}

$zipItem = Get-Item -LiteralPath $zipPath
Write-Host ""
Write-Host "Stranded Together $version (build $buildId)"
Write-Host "  staged : $stage"
Write-Host "  zip    : $zipPath ($([math]::Round($zipItem.Length / 1KB)) KB, $($entries.Count) files)"
Write-Host "  dll    : $((Get-Sha256 $dll))"
Write-Host "  exe    : $((Get-Sha256 $exe))"
if ($commit) { Write-Host "  source : $commit$(if ($dirty) { ' (working tree differs)' })" } else { Write-Host "  source : no git commit (source_commit null)" }
Write-Host "Install for testing: .\tools\deploy.ps1 -Package"
exit 0
