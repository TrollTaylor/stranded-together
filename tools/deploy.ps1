<#
.SYNOPSIS
  Developer deploy: copies the built mod (and launcher) into the Death Stranding
  folder, installs a staged release package, writes a session.ini for a quick
  test, or removes the mod again.

.DESCRIPTION
  Never overwrites or deletes a winhttp.dll that is not Stranded Together's
  (another mod or loader): the same check the launcher makes (a PE image with
  the proxy's GLOBALROOT forwarders that names the stranded_together folder).

  A -Session file is persistent (no one_shot): the mod reads it on every game
  start until -Uninstall or the launcher's Debug > Clear session removes it.
  Sessions started from StrandedTogether.exe are one-shot.

.EXAMPLE
  # Install the newest package staged by tools\package.ps1 (exe + dll + stranded_together\)
  .\tools\deploy.ps1 -Package

.EXAMPLE
  # Install a release zip (or a staged folder)
  .\tools\deploy.ps1 -Package -PackagePath dist\StrandedTogether-0.1.0.zip

.EXAMPLE
  # Solo test: a clone of Sam replays your movement 2 s behind you
  .\tools\deploy.ps1 -Session echo

.EXAMPLE
  # Debug tools only (in-game debug menu, no partner, no network)
  .\tools\deploy.ps1 -Session solo

.EXAMPLE
  # One-PC network test against tools\fakepeer (run st_fakepeer --role guest afterwards)
  .\tools\deploy.ps1 -Session udp-host

.EXAMPLE
  # Remove the mod from the game folder again (logs are kept)
  .\tools\deploy.ps1 -Uninstall
#>
param(
    [string]$GameDir = "",
    [ValidateSet("none", "echo", "solo", "udp-host", "udp-guest")]
    [string]$Session = "none",
    [string]$Peer = "127.0.0.1",
    [string]$Config = "Release",
    # The build folder holding $Config\winhttp.dll (default: <repo>\build).
    [string]$BuildDir = "",
    # Install a release package (StrandedTogether.exe, winhttp.dll, stranded_together\*).
    [switch]$Package,
    # -Package: a staged folder or a zip (default: the newest dist\StrandedTogether-*\ folder).
    [string]$PackagePath = "",
    # Developer deploy: do not copy StrandedTogether.exe next to the DLL.
    [switch]$NoLauncher,
    [switch]$Uninstall
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Find-GameDir {
    $steam = (Get-ItemProperty "HKCU:\Software\Valve\Steam" -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { throw "Steam not found; pass -GameDir" }
    $libs = @($steam)
    $vdf = Join-Path $steam "steamapps\libraryfolders.vdf"
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"').Matches) { $libs += $m.Groups[1].Value -replace '\\\\', '\' }
    }
    foreach ($lib in $libs) {
        $dir = "DEATH STRANDING DIRECTORS CUT"
        $acf = Join-Path $lib "steamapps\appmanifest_1850570.acf"
        if (Test-Path $acf) {
            $m = Select-String -Path $acf -Pattern '"installdir"\s+"([^"]+)"' | Select-Object -First 1
            if ($m) { $dir = $m.Matches[0].Groups[1].Value }
        }
        $d = Join-Path $lib "steamapps\common\$dir"
        if (Test-Path (Join-Path $d "ds.exe")) { return $d }
    }
    throw "DEATH STRANDING DIRECTORS CUT not found in any Steam library; pass -GameDir"
}

# The launcher's check (core_install LooksLikeOurDll).
function Test-OurDll([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { return $false }
    $latin = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    if (-not $latin.Contains('\\.\GLOBALROOT\SystemRoot\System32\winhttp.dll.')) { return $false }
    $wide = [Text.Encoding]::GetEncoding(28591).GetString([Text.Encoding]::Unicode.GetBytes('stranded_together'))
    return $latin.Contains('stranded_together') -or $latin.Contains($wide)
}

function Assert-GameNotRunning([string]$Dir) {
    $running = @(Get-Process -Name ds -ErrorAction SilentlyContinue | Where-Object {
            try { $_.Path -and ((Split-Path -Parent $_.Path) -ieq $Dir) } catch { $true } })
    if ($running.Count -gt 0) { throw "DEATH STRANDING is running from $Dir; quit it first (winhttp.dll is in use)" }
}

# Copies $Source over $Dest unless $Dest is somebody else's winhttp.dll.
function Install-Dll([string]$Source, [string]$Dest) {
    if ((Test-Path -LiteralPath $Dest) -and -not (Test-OurDll $Dest)) {
        throw "$Dest exists and is not Stranded Together's (another mod or loader); refusing to overwrite it"
    }
    Copy-Item -LiteralPath $Source -Destination $Dest -Force
    $disabled = "$Dest.disabled"
    if ((Test-Path -LiteralPath $disabled) -and (Test-OurDll $disabled)) {
        Remove-Item -LiteralPath $disabled
        Write-Host "removed the old $disabled"
    }
    Write-Host "installed $Dest"
}

function Copy-Launcher([string]$Source, [string]$Dest) {
    try { Copy-Item -LiteralPath $Source -Destination $Dest -Force }
    catch { throw "could not replace $Dest (close StrandedTogether.exe first): $($_.Exception.Message)" }
    Write-Host "installed $Dest"
}

if (-not $GameDir) { $GameDir = Find-GameDir }
$GameDir = [IO.Path]::GetFullPath($GameDir)
if (-not (Test-Path (Join-Path $GameDir "ds.exe"))) { throw "no ds.exe in $GameDir" }
$modDir = Join-Path $GameDir "stranded_together"
$dll = Join-Path $GameDir "winhttp.dll"
$launcher = Join-Path $GameDir "StrandedTogether.exe"
$ini = Join-Path $modDir "session.ini"

if ($Uninstall) {
    Assert-GameNotRunning $GameDir
    foreach ($f in @($dll, "$dll.disabled")) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        if (Test-OurDll $f) { Remove-Item -LiteralPath $f; Write-Host "removed $f" }
        else { Write-Warning "$f is not Stranded Together's; left in place" }
    }
    if (Test-Path -LiteralPath $launcher) {
        try { Remove-Item -LiteralPath $launcher; Write-Host "removed $launcher" }
        catch { Write-Warning "could not remove $launcher (is it running?)" }
    }
    foreach ($f in @($ini, (Join-Path $modDir "package.json"))) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f } }
    Write-Host "mod removed (logs and settings kept in $modDir)"
    return
}

Assert-GameNotRunning $GameDir
New-Item -ItemType Directory -Force $modDir | Out-Null

if ($Package) {
    # ---- A release package: the zip's root is the game folder --------------------------------
    $src = $PackagePath
    $temp = $null
    if (-not $src) {
        $dist = Join-Path $root "dist"
        $src = Get-ChildItem -Directory -Path $dist -Filter "StrandedTogether-*" -ErrorAction SilentlyContinue |
            Where-Object { Test-Path (Join-Path $_.FullName "stranded_together\package.json") } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
        if (-not $src) { throw "no staged package in $dist; run .\tools\package.ps1 first or pass -PackagePath" }
    }
    if (-not [IO.Path]::IsPathRooted($src)) { $src = Join-Path (Get-Location) $src }
    if ((Test-Path -LiteralPath $src -PathType Leaf) -and $src -like "*.zip") {
        $temp = Join-Path ([IO.Path]::GetTempPath()) ("st-package-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
        Expand-Archive -LiteralPath $src -DestinationPath $temp
        $src = $temp
    }
    try {
        $pkgJson = Join-Path $src "stranded_together\package.json"
        foreach ($f in @((Join-Path $src "winhttp.dll"), (Join-Path $src "StrandedTogether.exe"), $pkgJson)) {
            if (-not (Test-Path -LiteralPath $f)) { throw "$src is not a Stranded Together package (missing $f)" }
        }
        $pkg = Get-Content -Raw -LiteralPath $pkgJson | ConvertFrom-Json
        if ($pkg.product -ne "stranded-together-package") { throw "$pkgJson is not a Stranded Together package.json" }
        foreach ($row in $pkg.files) {
            $f = Join-Path $src ($row.path -replace '/', '\')
            if (-not (Test-Path -LiteralPath $f)) { throw "package file missing: $($row.path)" }
            if ((Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash -ne $row.sha256.ToUpperInvariant()) { throw "package file changed since packaging: $($row.path)" }
        }
        Install-Dll (Join-Path $src "winhttp.dll") $dll
        Copy-Launcher (Join-Path $src "StrandedTogether.exe") $launcher
        Get-ChildItem -LiteralPath (Join-Path $src "stranded_together") -File | ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $modDir $_.Name) -Force
        }
        Write-Host "installed package $($pkg.version) (build $($pkg.build_id)) into $GameDir"
    } finally {
        if ($temp) { Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue }
    }
} else {
    # ---- Developer deploy from a build folder ---------------------------------------------------
    if (-not $BuildDir) { $BuildDir = Join-Path $root "build" }
    elseif (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
    $built = Join-Path $BuildDir "$Config\winhttp.dll"
    if (-not (Test-Path $built)) { throw "build first: cmake --build $BuildDir --config $Config --target st_mod st_launcher" }
    Install-Dll $built $dll
    $builtLauncher = Join-Path $BuildDir "$Config\StrandedTogether.exe"
    if (-not $NoLauncher -and (Test-Path $builtLauncher)) { Copy-Launcher $builtLauncher $launcher }
    # A package.json of an earlier release would make the launcher report a version mismatch:
    # without it the launcher shows "development build".
    $pkgJson = Join-Path $modDir "package.json"
    if (Test-Path -LiteralPath $pkgJson) { Remove-Item -LiteralPath $pkgJson; Write-Host "removed $pkgJson (development build)" }
}

switch ($Session) {
    "none" { }
    "echo" {
        Set-Content -Path $ini -Encoding ascii -Value @(
            "# written by tools/deploy.ps1 - solo test (persistent: no one_shot)",
            "role=echo", "echo_delay_s=2.0", "echo_offset_m=1.5", "verbose=1")
    }
    "solo" {
        Set-Content -Path $ini -Encoding ascii -Value @(
            "# written by tools/deploy.ps1 - debug tools only (persistent: no one_shot)",
            "role=solo", "transport=udp", "udp_peer=127.0.0.1", "udp_port_local=27555", "udp_port_peer=27556",
            "debug_menu=1", "debug_menu_key=F9", "verbose=1")
    }
    "udp-host" {
        Set-Content -Path $ini -Encoding ascii -Value @(
            "# written by tools/deploy.ps1 - pair with: st_fakepeer --role guest --local 27556 --remote 27555",
            "role=host", "transport=udp", "udp_peer=$Peer", "udp_port_local=27555", "udp_port_peer=27556", "verbose=1")
    }
    "udp-guest" {
        Set-Content -Path $ini -Encoding ascii -Value @(
            "# written by tools/deploy.ps1",
            "role=guest", "transport=udp", "udp_peer=$Peer", "udp_port_local=27556", "udp_port_peer=27555", "verbose=1")
    }
}
if ($Session -ne "none") { Write-Host "wrote $ini ($Session, persistent)" }
Write-Host "log: $(Join-Path $modDir 'stranded_together.log')"
