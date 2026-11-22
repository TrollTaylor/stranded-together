<#
.SYNOPSIS
  Runs Death Stranding hidden on a private Windows desktop and drives it with a scenario
  (docs/hidden-testing.md). Nothing appears on your screen; audio is muted in the game.

.EXAMPLE
  pwsh -File tools\testenv\Start-TestRun.ps1 -Probe            # D3D12 on a private desktop, no game
  pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario smoke   # the real game, one smoke run

.EXAMPLE
  # Inline steps, no scenario file: straight into the world (quickstart), then the steps (docs/hidden-testing.md "-Steps")
  pwsh -File tools\testenv\Start-TestRun.ps1 -SeedSaves complete-100 -Steps 'goto lake-knot','screenshot lake'
  pwsh -File tools\testenv\Start-TestRun.ps1 -SeedSaves complete-100 -LauncherIni -Steps '@guest goto mknot','screenshot a'

.NOTES
  Refuses when ds.exe is already running. The game folder's winhttp.dll and
  stranded_together files are backed up to the run folder and restored afterwards.
  Your saves (%LOCALAPPDATA%\KojimaProductions) are hashed and backed up to the run
  folder first; the mod redirects every access to them to a scratch copy (the run is
  stopped unless that is confirmed), and afterwards they are compared by hash and
  restored from the backup if anything differs.
  Output: build-testenv\runs\<yyyyMMdd-HHmmss>-<scenario>\ (summary.json, supervisor.log,
  game\stranded_together.log, automation\*.png, results.txt, saves-*.txt).
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Scenario = 'smoke',
    [switch]$Probe,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
    [string]$GameDir,
    [ValidateRange(30, 360)][int]$TimeoutSec = 360,
    [string]$Saves,
    # Save the hidden game loads (scratch copy only): a folder holding DeathStrandingDC\<slots>,
    # a name under testenv-saves\, or 'own'. Default: the user's own saves. DS binds saves to the account that wrote
    # them ('Save data is corrupted' otherwise); with a seed the supervisor writes automation_foreign_saves=1 and the
    # mod skips that check in this hidden run only (docs/hidden-testing.md "Using another save").
    [string]$SeedSaves,
    # Experiments: this file is the games' settings.cfg (the graphics settings; redirected, never the real one).
    [string]$SettingsCfg,
    # Experiments: "Key" "Value" lines merged into the scratch profiles' game_settings.cfg (graphics menu options).
    [string]$GameSettings,
    # The launcher's "Play with test save" session.ini (sandbox_saves, no automation channel; scenario sandbox-alive).
    [switch]$SandboxSession,
    # Two-game runs: what the guest game's scratch saves hold. full (default): the user's saves (and -SeedSaves' slots),
    # as the host. profile-only: the guest's own profile\ and settings only, no save slot (the host-held save session's
    # guest: it gets the host's save over the link; docs/coop-session.md). The host's scratch is seeded as always.
    [ValidateSet('full', 'profile-only')][string]$GuestSeed = 'full',
    # Two-game runs on the launcher's own session.ini (the Local test's host.ini + guest.ini, written by
    # StrandedTogether.exe --write-session local-test from the game folder's launcher settings, read only). Only the save
    # location is swapped for the run's redirect. Automatic for a scenario with a 'launcherini' line.
    [switch]$LauncherIni,
    [switch]$KeepAudio,
    [switch]$NoBuild,
    # Bisecting a game that does not start (docs/hidden-testing.md "When the game exits at once"):
    [switch]$NoMod,          # no session/overlay/game hooks: winhttp.dll only redirects the saves; scenario 'alive'
    [switch]$NoUiLimits,     # job without UI restrictions (kill-on-close stays)
    [switch]$SteamEnv,       # the environment variables Steam sets for a game it launches
    [switch]$NoIsolation,    # automation channel without the overlay's isolation hooks
    [switch]$NoGameStdout,   # do not hand ds.exe a file as stdout (its own log -> game-stdout.txt)
    # Inline steps (no scenario file): scenario lines, one per element (an element may hold several lines). Start-TestRun.ps1
    # writes <run>\steps.txt: the header (role, timeout, graphics, launcherini), "start <Start>" (unless -NoStart), the steps,
    # then "quit". Every scenario line works, "goto <place>" too (tools\testenv\places.txt).
    [string[]]$Steps,
    [ValidateSet('solo', 'host')][string]$Role = 'solo',  # -Steps: one game, or two (role host; -LauncherIni: the Local test's)
    [string]$Start = 'continue',                          # -Steps: the start line's spec (continue | story <n> | load <type> <i>)
    [switch]$NoStart,                                     # -Steps: no start line (the title flow is the test)
    [string]$Name,                                        # -Steps: the run folder's name (default "steps")
    # A held session (docs/hidden-testing.md "Held sessions"; Start-TestSession.ps1 sets it): the supervisor runs on after the
    # scenario, taking steps from <run>\inbox (Send-TestStep.ps1) until Stop-TestSession.ps1, its time limit or 60 s without a
    # heartbeat. This script then returns once the session is ready; Stop-TestSession.ps1 checks the restore.
    [switch]$Hold
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
$bin = Join-Path $BuildDir $Config

function Find-GameDir {
    $candidates = @()
    try {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $candidates += (Join-Path $steam 'steamapps\common\DEATH STRANDING DIRECTORS CUT')
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path $vdf) {
                foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $candidates += (Join-Path ($m.Groups[1].Value -replace '\\\\', '\') 'steamapps\common\DEATH STRANDING DIRECTORS CUT')
                }
            }
        }
    } catch { }
    $candidates += 'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT'
    foreach ($c in $candidates) { if (Test-Path (Join-Path $c 'ds.exe')) { return (Resolve-Path $c).Path } }
    throw 'Death Stranding Director''s Cut (ds.exe) not found; pass -GameDir'
}

if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        & cmake -S $repo -B $BuildDir -G 'Visual Studio 17 2022' -A x64 | Out-Host
        if ($LASTEXITCODE) { throw 'cmake configure failed' }
    }
    & cmake --build $BuildDir --config $Config --target st_mod st_testenv_supervisor st_testenv_probe st_launcher st_textool -- '/m' '/verbosity:minimal' | Out-Host
    if ($LASTEXITCODE) { throw 'build failed' }
}
$sup = Join-Path $bin 'st_testenv_supervisor.exe'
if (-not (Test-Path $sup)) { throw "missing $sup (build first)" }

$name = if ($Probe) { 'probe' } elseif ($Steps) { if ($Name) { $Name } else { 'steps' } } else { [IO.Path]::GetFileNameWithoutExtension($Scenario) }
$run = Join-Path $BuildDir ("runs\{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $name)
New-Item -ItemType Directory -Force $run | Out-Null

if ($Probe) {
    & $sup --probe (Join-Path $bin 'st_testenv_probe.exe') --run-dir $run --seconds 8 2>&1 | Out-Host
    $code = $LASTEXITCODE
    Write-Host "probe exit $code; $run\probe.json, $run\probe-frame.png"
    exit $code
}

if (Get-Process -Name ds -ErrorAction SilentlyContinue) { throw 'ds.exe is running: close the game first (the hidden run refuses to share it)' }
if (-not $GameDir) { $GameDir = Find-GameDir }
# Interrupted runs (restore.txt without restored.txt = ok): never restored automatically (an old run's backup would put back
# files the user has changed since), but no new run starts over one: its backup would record the interrupted run's DLL and
# session.ini as the user's own (the supervisor also refuses while <mod>\automation or an automation=1 session.ini is left).
# Skipped while another supervisor runs (its own run is not restored yet; the supervisor refuses this run then anyway).
if (-not (Get-Process -Name st_testenv_supervisor -ErrorAction SilentlyContinue)) {
    $cut = (Get-Date).AddHours(-48)
    $unrestored = @()
    foreach ($d in (Get-ChildItem (Join-Path $BuildDir 'runs') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $cut -and $_.FullName -ne $run })) {
        $needs = ((Test-Path (Join-Path $d.FullName 'restore.txt')) -and -not ((Test-Path (Join-Path $d.FullName 'restored.txt')) -and (Get-Content (Join-Path $d.FullName 'restored.txt') -TotalCount 1) -eq 'ok')) -or
                 ((Test-Path (Join-Path $d.FullName 'saves-restore.txt')) -and -not ((Test-Path (Join-Path $d.FullName 'saves-restored.txt')) -and (Get-Content (Join-Path $d.FullName 'saves-restored.txt') -TotalCount 1) -eq 'ok'))
        if ($needs) { $unrestored += $d.FullName }
    }
    if ($unrestored) {
        Remove-Item -LiteralPath $run -Recurse -Force -ErrorAction SilentlyContinue  # created above, still empty
        throw ("refusing to start: {0} interrupted run(s) never restored. Check, then restore each:`n{1}" -f $unrestored.Count,
            (($unrestored | ForEach-Object { "  tools\testenv\Restore-TestRun.ps1 -RunDir `"$_`"" }) -join "`n"))
    }
}
if ($Steps) {
    # Inline steps: a scenario file in the run folder (the supervisor keeps a copy as scenario.txt too)
    $lines = @("# Start-TestRun.ps1 -Steps ($(Get-Date -Format s))")
    if ($LauncherIni) { $lines += 'role host', 'graphics low', 'launcherini local-test' }
    else {
        $lines += "role $Role"
        if ($Role -eq 'host') { $lines += 'graphics low' }
    }
    $lines += "timeout $TimeoutSec"
    if (-not $NoStart) { $lines += "start $Start" }
    # one step per element, per line, or separated by " ; " (pwsh -File passes one string: -Steps 'goto mknot ; screenshot a')
    foreach ($e in $Steps) { foreach ($l in ($e -split "`r?`n| ; ")) { if ($l.Trim()) { $lines += $l.Trim() } } }
    if ($lines[-1] -notmatch '^\s*(@\w+\s+)?quit\b' -and -not $Hold) { $lines += 'quit' }
    $scenarioFile = Join-Path $run 'steps.txt'
    Set-Content -LiteralPath $scenarioFile -Value ($lines -join "`r`n") -Encoding ascii
    Write-Host ("inline steps -> {0}:`n  {1}" -f $scenarioFile, ($lines -join "`n  "))
} else {
    $scenarioFile = if (Test-Path $Scenario) { (Resolve-Path $Scenario).Path } else { Join-Path $PSScriptRoot "scenarios\$Scenario.txt" }
}
if (-not (Test-Path $scenarioFile)) { throw "scenario not found: $Scenario" }
$mod = Join-Path $bin 'winhttp.dll'

$supArgs = @('--game-dir', $GameDir, '--mod', $mod, '--scenario', $scenarioFile, '--run-dir', $run, '--timeout-s', $TimeoutSec)
$places = Join-Path $PSScriptRoot 'places.txt'
if (Test-Path $places) { $supArgs += @('--places', $places) }  # the scenario's "goto <name>"
if ($Hold) { $supArgs += '--hold' }
if ($Saves) { $supArgs += @('--saves', $Saves) }
if ($SeedSaves -and $SeedSaves -ne 'own') {
    if (-not (Test-Path $SeedSaves)) { $SeedSaves = Join-Path $repo ('testenv-saves\' + $SeedSaves) }
    if (-not (Test-Path (Join-Path $SeedSaves 'DeathStrandingDC'))) { throw "seed saves not found: $SeedSaves (needs DeathStrandingDC\<slots>)" }
    $supArgs += @('--seed-saves', (Resolve-Path $SeedSaves).Path)
    Write-Host "seed saves: $SeedSaves (scratch copy only; your real saves stay untouched)"
}
if ($GameSettings) { $supArgs += @('--game-settings', (Resolve-Path $GameSettings).Path) }
if ($SettingsCfg) { $supArgs += @('--settings-cfg', (Resolve-Path $SettingsCfg).Path) }
if ($SandboxSession) { $supArgs += '--sandbox-session' }
if ($GuestSeed -ne 'full') { $supArgs += @('--guest-seed', $GuestSeed) }
if (-not $LauncherIni -and (Select-String -Path $scenarioFile -Pattern '^\s*launcherini\s' -Quiet)) { $LauncherIni = $true }
if ($LauncherIni) {
    # Exactly what the launcher's Local test button writes (MakeLocalTestInis + MakeSessionIniText), into the run folder.
    $launcher = Join-Path $bin 'StrandedTogether.exe'
    if (-not (Test-Path $launcher)) { throw "missing $launcher (build st_launcher first)" }
    $iniDir = Join-Path $run 'launcher-ini'
    & $launcher --write-session local-test --out $iniDir --game-dir $GameDir | Out-Host
    if (-not (Test-Path (Join-Path $iniDir 'host.ini')) -or -not (Test-Path (Join-Path $iniDir 'guest.ini'))) { throw "the launcher wrote no local-test ini into $iniDir" }
    Write-Host "launcher ini: $iniDir (host.ini, guest.ini as the Local test writes them)"
    $supArgs += @('--launcher-ini', $iniDir)
}
if ($KeepAudio) { $supArgs += '--keep-audio' }
if ($NoMod) { $supArgs += '--no-mod' }
if ($NoUiLimits) { $supArgs += '--no-ui-limits' }
if ($SteamEnv) { $supArgs += '--steam-env' }
if ($NoIsolation) { $supArgs += '--no-isolation' }
if ($NoGameStdout) { $supArgs += '--no-game-stdout' }
Write-Host "hidden run: $name -> $run"
if ($Hold) {
    # A held session: the supervisor runs on in the background (GUI subsystem: no window), this script returns once it takes steps
    $p = Start-Process -FilePath $sup -ArgumentList ($supArgs | ForEach-Object { if ("$_" -match '\s') { '"' + $_ + '"' } else { "$_" } }) -PassThru -WindowStyle Hidden
    @{ pid = $p.Id; run = $run; started = (Get-Date -Format s) } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'session.json') -Encoding ascii
    New-Item -ItemType Directory -Force (Join-Path $run 'inbox') | Out-Null
    Set-Content -LiteralPath (Join-Path $run 'inbox\alive') -Value (Get-Date -Format o) -Encoding ascii
    $deadline = (Get-Date).AddSeconds(360)
    while (-not (Test-Path (Join-Path $run 'session-ready.txt')) -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        Set-Content -LiteralPath (Join-Path $run 'inbox\alive') -Value (Get-Date -Format o) -Encoding ascii
    }
    if (Test-Path (Join-Path $run 'session-ready.txt')) {
        Write-Host "held session ready: $run (pid $($p.Id)). Send-TestStep.ps1 -Steps '...'; Stop-TestSession.ps1 ends it (60 s without a step or heartbeat ends it too)"
        Write-Output $run
        exit 0
    }
    $p.WaitForExit()
    Write-Warning "the held session ended before it was ready (supervisor exit $($p.ExitCode)): see $run\supervisor.log"
    $code = $p.ExitCode
} else {
    & $sup @supArgs 2>&1 | Out-Host
    $code = $LASTEXITCODE
}

# A supervisor that died mid-run leaves restore.txt without restored.txt (or saves-restore.txt
# without saves-restored.txt), and a restore that met a locked file says INCOMPLETE: restore
# again (idempotent; refuses while ds.exe runs; also checks and restores the saves).
$restoredTxt = Join-Path $run 'restored.txt'
$savesRestoredTxt = Join-Path $run 'saves-restored.txt'
if ((Test-Path (Join-Path $run 'restore.txt')) -or (Test-Path (Join-Path $run 'saves-restore.txt'))) {
    $state = if (Test-Path $restoredTxt) { (Get-Content $restoredTxt -TotalCount 1) } else { '' }
    $savesState = if (-not (Test-Path (Join-Path $run 'saves-restore.txt'))) { 'ok' } elseif (Test-Path $savesRestoredTxt) { (Get-Content $savesRestoredTxt -TotalCount 1) } else { '' }
    if ($state -ne 'ok' -or $savesState -ne 'ok') {
        Write-Warning ("game folder: {0}, saves: {1}; restoring again from the run folder" -f $(if ($state) { $state } else { 'no restore ran' }), $(if ($savesState) { $savesState } else { 'no check ran' }))
        for ($i = 0; $i -lt 3; $i++) {
            & $sup --restore $run 2>&1 | Out-Host
            if ($LASTEXITCODE -eq 0) { break }
            Start-Sleep -Seconds 2
        }
        if ($LASTEXITCODE -ne 0) { Write-Warning "restore still incomplete: run tools\testenv\Restore-TestRun.ps1 -RunDir `"$run`" once ds.exe is gone" }
    }
}

$summaryPath = Join-Path $run 'summary.json'
if (Test-Path $summaryPath) {
    $s = Get-Content $summaryPath -Raw | ConvertFrom-Json
    Write-Host ("result: {0}  failures: {1}  restored: {2}  violation: {3}  error.txt: {4}" -f $s.result, $s.failures, $s.restored, $s.violation, $s.error_txt)
    Write-Host ("game: exit code {0} after {1} s alive ({2} s CPU); windows on the private desktop: {3} ({4})" -f $s.game_exit_code, $s.game_alive_s, $s.game_cpu_s, $s.private_desktop_windows, $s.private_desktop_titles)
    foreach ($step in $s.steps) { Write-Host ("  [{0}] line {1}: {2} -- {3}" -f ($(if ($step.ok) { 'ok' } else { 'FAIL' })), $step.line, $step.step, $step.detail) }
    Write-Host ("saves: redirected {0} ({1}); real tree unchanged: {2}; restored/verified: {3}" -f $s.saves_redirected, $s.saves_evidence, $s.saves_unchanged, $s.saves_ok)
    if ($s.saves_refused) { Write-Warning ("the mod refused to run the game: {0}" -f $s.saves_refused) }
    if ($s.save_changes.Count) { Write-Warning ("REAL SAVES CHANGED (restored from saves-backup): {0}" -f ($s.save_changes -join ', ')) }
    if ($s.scratch_changes.Count) { Write-Host ("scratch copy changed by the game (expected, discarded): {0}" -f ($s.scratch_changes -join ', ')) }
    Write-Host ("Steam Cloud: AutoCloud pass logged: {0}; app lines: {1}; uploads: {2}" -f $s.cloud_autocloud_seen, $s.cloud_lines, $s.cloud_uploads.Count)
    if ($s.exit_trace) { Write-Host ("exit trace: {0}" -f $s.exit_trace.Trim()) }
    if ($s.game_stdout_tail) { Write-Host "game stdout (last lines, game-stdout.txt):"; Write-Host $s.game_stdout_tail }
}
$shots = Get-ChildItem (Join-Path $run 'automation') -Filter *.png -ErrorAction SilentlyContinue
if ($shots) { Write-Host ("screenshots: {0}" -f (($shots | ForEach-Object Name) -join ', ')) }
exit $code
