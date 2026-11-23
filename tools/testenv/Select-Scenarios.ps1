<#
.SYNOPSIS
  Lists the fewest hidden-test scenarios that cover what a change touched (docs/hidden-testing.md "Test economy").

.EXAMPLE
  pwsh -File tools\testenv\Select-Scenarios.ps1                              # the working tree (and untracked files) against HEAD
  pwsh -File tools\testenv\Select-Scenarios.ps1 -Base v0.24.0 -Head v0.25.0 -Release -Explain
  pwsh -File tools\testenv\Select-Scenarios.ps1 -Range v0.24.0..v0.25.0
  pwsh -File tools\testenv\Select-Scenarios.ps1 -Base HEAD~1 -Reuse          # leave out scenarios that already passed on this build
  pwsh -File tools\testenv\Select-Scenarios.ps1 -Check                       # ctest: every scenario has "# covers:", every file an area
  pwsh -File tools\testenv\Select-Scenarios.ps1 -StepsHash tools\testenv\scenarios\smoke.txt

.NOTES
  1. The changed files (git diff -U0) map to areas through tools\testenv\test-areas.txt: the first plain rule a path matches,
     plus every "~regex" rule whose regex matches one of the file's changed lines. Files in "no-run" areas (docs, unit tests,
     research tooling, the launcher window) need no hidden run.
  2. Every scenario names the areas it checks in its header: "# covers: <area> ..." and, when it is not an ordinary proof,
     "# select: probe" (research, calibration, long or optional: never chosen), "# select: baseline" (a vanilla / before run:
     only with -Baselines, for a fix's before / after), "# select: ctest" (the fake game's e2e) or "# select: release" (the
     launcher's Local test: chosen with -Release, and like any other scenario). Areas declared "implicit" are covered by any
     run, "implicit-host" by any two-game run.
  3. Weighted set cover: until every touched area is covered, take the scenario with the lowest cost per newly covered area
     (ties: the one that covers fewer areas, then the name). Cost = an estimate in seconds: 60 s for build, backup and restore
     plus the scenario's median game time in passing runs under build-testenv\runs (no such run: 60 % of its timeout);
     two-game runs count 1.5 times (both games share the machine). A scenario the change added or whose steps it changed
     costs half (it is the change's own proof); -IncludeChanged runs every such scenario.
  4. -Reuse: a scenario with a passing, clean run (pass, no violation, saves unchanged, restored, online gate ok, no cloud
     upload, no game-server write) on the same build costs nothing and is listed as reused instead of run. Same build = the
     run's run-stamp.txt (written by Start-TestRun.ps1: the SHA-256 of winhttp.dll, StrandedTogether.exe,
     st_testenv_supervisor.exe and st_textool.exe it ran) equals the binaries in build-testenv\<Config> now, the run's
     scenario.txt has the same steps (comments ignored) and the same seed. Build first so the binaries match the tree.
  5. -Release adds the release scenario (the launcher's Local test) whatever the diff touched.
  Output: the scenario names (pipeline), the estimated time and Run-Batch.ps1 command lines grouped by seed (host). -Explain
  shows why: each file's areas and rule, each area's scenario and its cheaper or other options. -AsObject returns one object.
#>
[CmdletBinding()]
param(
    [string]$Base,                 # diff from this revision (to -Head, or to the working tree with untracked files)
    [string]$Head,
    [string]$Range,                # one git diff argument instead: A..B or A...B
    [string]$DiffFile,             # tests: a unified diff (git diff -U0 format) instead of asking git
    [switch]$Release,
    [switch]$IncludeChanged,
    [switch]$Baselines,
    [switch]$Reuse,
    [switch]$Explain,
    [switch]$AsObject,
    [switch]$Check,
    [string[]]$CheckFiles,         # -Check: these paths instead of git ls-files
    [string]$StepsHash,            # print the steps hash of this scenario file (comments and blank lines ignored) and exit
    [string]$ScenarioDir,
    [string]$AreaMap,
    [string]$RunsDir,
    [string]$BuildDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')][string]$Config = 'Release',
    [string]$DefaultSeed = 'complete-100'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $ScenarioDir) { $ScenarioDir = Join-Path $PSScriptRoot 'scenarios' }
if (-not $AreaMap) { $AreaMap = Join-Path $PSScriptRoot 'test-areas.txt' }
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
if (-not $RunsDir) { $RunsDir = Join-Path $BuildDir 'runs' }
$StampFiles = @('winhttp.dll', 'StrandedTogether.exe', 'st_testenv_supervisor.exe', 'st_textool.exe')
$Selects = @('auto', 'probe', 'baseline', 'ctest', 'release')

function Get-StepsHashOfText([string]$text) {
    $steps = @($text -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -and -not $_.StartsWith('#') })
    $sha = [Security.Cryptography.SHA256]::Create()
    try { -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($steps -join "`n")) | ForEach-Object { $_.ToString('x2') }) }
    finally { $sha.Dispose() }
}
if ($StepsHash) { Get-StepsHashOfText ([IO.File]::ReadAllText((Resolve-Path $StepsHash).Path)); exit 0 }

# ---------------------------------------------------------------- the area map
function Read-AreaMap([string]$path) {
    $m = [ordered]@{ Areas = [ordered]@{}; Rules = [Collections.Generic.List[object]]::new(); Errors = [Collections.Generic.List[string]]::new() }
    $n = 0
    foreach ($raw in [IO.File]::ReadAllLines($path)) {
        $n++
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith('#')) { continue }
        if ($line -match '^area\s+(\S+)((?:\s+(?:implicit-host|implicit|no-run))*)\s*(?::\s*(.*))?$') {
            $name = $Matches[1]; $flagText = $Matches[2]; $desc = $Matches[3]
            if ($name -notmatch '^[a-z0-9][a-z0-9-]*$') { $m.Errors.Add("test-areas.txt line ${n}: bad area name '$name'"); continue }
            if ($m.Areas.Contains($name)) { $m.Errors.Add("test-areas.txt line ${n}: area '$name' declared twice"); continue }
            $flags = @($flagText -split '\s+' | Where-Object { $_ })
            $m.Areas[$name] = [pscustomobject]@{ Name = $name; Implicit = ($flags -contains 'implicit'); ImplicitHost = ($flags -contains 'implicit-host');
                                                 NoRun = ($flags -contains 'no-run'); Description = $desc }
            continue
        }
        if ($line -match '^area\b') { $m.Errors.Add("test-areas.txt line ${n}: bad area line: $line"); continue }
        $words = @($line -split '\s+')
        if ($words.Count -lt 2) { $m.Errors.Add("test-areas.txt line ${n}: a rule needs a glob and at least one area: $line"); continue }
        $rx = $null; $areas = @($words[1..($words.Count - 1)])
        if ($areas[0].StartsWith('~')) {
            try { $rx = [regex]::new($areas[0].Substring(1), 'IgnoreCase') }
            catch { $m.Errors.Add("test-areas.txt line ${n}: bad regex $($areas[0])"); continue }
            $areas = @($areas | Select-Object -Skip 1)
            if (-not $areas) { $m.Errors.Add("test-areas.txt line ${n}: a ~regex rule needs at least one area"); continue }
        }
        $m.Rules.Add([pscustomobject]@{ Glob = $words[0]; Regex = $rx; Areas = $areas; Line = $n; Text = $line })
    }
    foreach ($r in $m.Rules) {
        foreach ($a in $r.Areas) { if (-not $m.Areas.Contains($a)) { $m.Errors.Add("test-areas.txt line $($r.Line): undeclared area '$a'") } }
    }
    $m
}

function Get-FileAreas($map, [string]$path, [string[]]$changed) {
    $p = $path -replace '\\', '/'
    $plain = $null; $extra = @()
    foreach ($r in $map.Rules) {
        if (-not ($p -like $r.Glob)) { continue }
        if ($r.Regex) {
            # lines over 200 characters are usage lists and log formats that name every feature ("unknown op ... 12 looks ..."): skipped
            $hit = $changed | Where-Object { $_.Length -le 200 -and $r.Regex.IsMatch($_) } | Select-Object -First 1
            if ($null -ne $hit) { $extra += $r }
        } elseif (-not $plain) { $plain = $r }
    }
    $areas = [Collections.Generic.List[string]]::new()
    if ($plain) { foreach ($a in $plain.Areas) { if (-not $areas.Contains($a)) { $areas.Add($a) } } }
    foreach ($r in $extra) { foreach ($a in $r.Areas) { if (-not $areas.Contains($a)) { $areas.Add($a) } } }
    [pscustomobject]@{ Path = $p; Areas = @($areas); Plain = $plain; Extra = $extra; Unmapped = (-not $plain) }
}

# ---------------------------------------------------------------- scenarios
function Read-Scenarios([string]$dir) {
    $list = [Collections.Generic.List[object]]::new()
    foreach ($f in (Get-ChildItem -LiteralPath $dir -Filter '*.txt' -File | Sort-Object Name)) {
        $text = [IO.File]::ReadAllText($f.FullName)
        $lines = $text -split "`r?`n"
        $header = @(); foreach ($l in $lines) { if (-not $l.Trim() -or $l.TrimStart().StartsWith('#')) { $header += $l } else { break } }
        $headerText = $header -join "`n"
        $covLines = @($header | Where-Object { $_ -match '^\s*#\s*covers:' })
        $selLines = @($header | Where-Object { $_ -match '^\s*#\s*select:' })
        $covers = @(); if ($covLines.Count -ge 1 -and $covLines[0] -match '#\s*covers:\s*(.*)$') { $covers = @($Matches[1].Trim() -split '\s+' | Where-Object { $_ }) }
        $select = 'auto'; if ($selLines.Count -ge 1 -and $selLines[0] -match '#\s*select:\s*(\S+)') { $select = $Matches[1].ToLowerInvariant() }
        $role = 'solo'; $m = [regex]::Match($text, '(?m)^\s*role\s+(\S+)'); if ($m.Success) { $role = $m.Groups[1].Value }
        $timeout = 360; $m = [regex]::Match($text, '(?m)^\s*timeout\s+(\d+)'); if ($m.Success) { $timeout = [int]$m.Groups[1].Value }
        $seed = $null; $m = [regex]::Match($headerText, '-SeedSaves\s+([A-Za-z0-9._-]+)'); if ($m.Success) { $seed = $m.Groups[1].Value.TrimEnd('.') }
        $guestSeed = 'full'; $m = [regex]::Match($headerText, '-GuestSeed\s+([A-Za-z-]+)'); if ($m.Success) { $guestSeed = $m.Groups[1].Value }
        $list.Add([pscustomobject]@{
            Name = $f.BaseName; Path = $f.FullName; Covers = $covers; CoversLines = $covLines.Count; SelectLines = $selLines.Count
            Select = $select; Role = $role; Timeout = $timeout; Seed = $seed; GuestSeed = $guestSeed
            Sandbox = ($headerText -match '-SandboxSession'); LauncherIni = ($text -match '(?m)^\s*launcherini\s')
            StepsHash = (Get-StepsHashOfText $text)
        })
    }
    $list
}

function Get-EffectiveCovers($s, $map) {
    $set = [Collections.Generic.List[string]]::new()
    foreach ($a in $s.Covers) { if (-not $set.Contains($a)) { $set.Add($a) } }
    foreach ($a in $map.Areas.Values) {
        if (($a.Implicit -or ($a.ImplicitHost -and $s.Role -eq 'host')) -and -not $set.Contains($a.Name)) { $set.Add($a.Name) }
    }
    @($set)
}

# ---------------------------------------------------------------- -Check
if ($Check) {
    $map = Read-AreaMap $AreaMap
    $problems = [Collections.Generic.List[string]]::new()
    foreach ($e in $map.Errors) { $problems.Add($e) }
    $scen = Read-Scenarios $ScenarioDir
    foreach ($s in $scen) {
        if ($s.CoversLines -ne 1) { $problems.Add("scenario $($s.Name): $($s.CoversLines) '# covers:' lines in its header (needs exactly one)"); continue }
        if (-not $s.Covers) { $problems.Add("scenario $($s.Name): '# covers:' names no area") }
        foreach ($a in $s.Covers) { if (-not $map.Areas.Contains($a)) { $problems.Add("scenario $($s.Name): unknown area '$a' (declare it in test-areas.txt)") } }
        if ($s.SelectLines -gt 1) { $problems.Add("scenario $($s.Name): more than one '# select:' line") }
        if ($Selects -notcontains $s.Select) { $problems.Add("scenario $($s.Name): '# select: $($s.Select)' (one of $($Selects -join ', '))") }
    }
    $auto = @($scen | Where-Object { $_.Select -in 'auto', 'release' -and $_.CoversLines -eq 1 })
    foreach ($a in $map.Areas.Values) {
        if ($a.NoRun) { continue }
        $c = @($auto | Where-Object { (Get-EffectiveCovers $_ $map) -contains $a.Name })
        if (-not $c) { $problems.Add("area $($a.Name): no scenario that Select-Scenarios.ps1 may choose covers it") }
    }
    $rel = @($scen | Where-Object { $_.Select -eq 'release' })
    if ($rel.Count -ne 1) { $problems.Add("$($rel.Count) scenarios say '# select: release' (needs exactly one: the launcher's Local test)") }
    $files = $CheckFiles
    if (-not $PSBoundParameters.ContainsKey('CheckFiles')) {
        $files = @(& git -C $repo -c core.quotepath=off ls-files 2>$null)
        if ($LASTEXITCODE) { Write-Warning 'git ls-files failed: tracked files not checked'; $files = @() }
    }
    foreach ($f in $files) {
        $fa = Get-FileAreas $map $f @()
        if ($fa.Unmapped) { $problems.Add("file ${f}: no rule in test-areas.txt matches it") }
    }
    if ($problems.Count) {
        foreach ($p in $problems) { Write-Host "FAIL $p" }
        Write-Host ("check: {0} problem(s) ({1} scenarios, {2} areas, {3} files)" -f $problems.Count, $scen.Count, $map.Areas.Count, @($files).Count)
        exit 1
    }
    Write-Host ("check: ok ({0} scenarios, {1} areas, {2} rules, {3} files)" -f $scen.Count, $map.Areas.Count, $map.Rules.Count, @($files).Count)
    exit 0
}

# ---------------------------------------------------------------- the diff
function Read-UnifiedDiff([string[]]$lines) {
    $entries = [Collections.Generic.List[object]]::new()
    $cur = $null; $inHunk = $false
    foreach ($l in $lines) {
        if ($l -match '^diff --git a/(.*) b/(.*)$') {
            $cur = [pscustomobject]@{ Path = $Matches[2]; OldPath = $Matches[1]; Status = 'M'; Lines = [Collections.Generic.List[string]]::new() }
            $entries.Add($cur); $inHunk = $false; continue
        }
        if (-not $cur) { continue }
        if (-not $inHunk) {
            if ($l.StartsWith('new file mode')) { $cur.Status = 'A' }
            elseif ($l.StartsWith('deleted file mode')) { $cur.Status = 'D' }
            elseif ($l -match '^rename to (.*)$') { $cur.Path = $Matches[1]; $cur.Status = 'R' }
            elseif ($l.StartsWith('@@')) { $inHunk = $true }
            continue
        }
        if ($l.StartsWith('@@')) { continue }
        if ($l.StartsWith('+') -or $l.StartsWith('-')) { $cur.Lines.Add($l.Substring(1)) }
    }
    $entries
}

$diffLabel = ''
if ($DiffFile) {
    $diffLines = [IO.File]::ReadAllLines((Resolve-Path $DiffFile).Path)
    $diffLabel = "diff file $DiffFile"
    $untracked = @()
} else {
    if ($Head -and -not $Base) { throw '-Head needs -Base' }
    $gitArgs = @(if ($Range) { $Range } elseif ($Base -and $Head) { $Base, $Head } elseif ($Base) { $Base } else { 'HEAD' })
    $errFile = [IO.Path]::GetTempFileName()
    try {
        $diffLines = @(& git -C $repo -c core.quotepath=off -c core.safecrlf=false diff -U0 --no-color --no-ext-diff --find-renames @gitArgs 2>$errFile)
        $code = $LASTEXITCODE
        if ($code) { throw "git diff $($gitArgs -join ' ') failed (exit $code): $((Get-Content -LiteralPath $errFile -Raw))" }
    } finally { Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue }
    $diffLabel = 'git diff ' + ($gitArgs -join ' ')
    $untracked = @()
    if (-not $Range -and -not $Head) {
        $untracked = @(& git -C $repo -c core.quotepath=off ls-files --others --exclude-standard 2>$null)
        if ($untracked) { $diffLabel += " + $($untracked.Count) untracked file(s)" }
    }
}
$entries = [Collections.Generic.List[object]]::new()
foreach ($e in @(Read-UnifiedDiff $diffLines)) { $entries.Add($e) }
foreach ($u in $untracked) {
    $full = Join-Path $repo $u
    $content = [Collections.Generic.List[string]]::new()
    try { if ((Get-Item -LiteralPath $full).Length -lt 2MB) { foreach ($l in [IO.File]::ReadAllLines($full)) { $content.Add($l) } } } catch { }
    $entries.Add([pscustomobject]@{ Path = $u; OldPath = $u; Status = 'A'; Lines = $content })
}

$map = Read-AreaMap $AreaMap
foreach ($e in $map.Errors) { Write-Warning $e }
$scenarios = Read-Scenarios $ScenarioDir
$byName = @{}; foreach ($s in $scenarios) { $byName[$s.Name] = $s }

# files -> areas; changed scenarios
$fileInfo = [Collections.Generic.List[object]]::new()
$touched = [Collections.Generic.List[string]]::new()
$changedScen = [Collections.Generic.List[object]]::new()
foreach ($e in $entries) {
    $fa = Get-FileAreas $map $e.Path @($e.Lines)
    $fa | Add-Member Status $e.Status
    $fileInfo.Add($fa)
    if ($e.Status -ne 'D') {
        foreach ($a in $fa.Areas) { if ($map.Areas.Contains($a) -and -not $map.Areas[$a].NoRun -and -not $touched.Contains($a)) { $touched.Add($a) } }
    }
    if ($e.Status -ne 'D' -and $fa.Path -like 'tools/testenv/scenarios/*.txt') {
        $name = [IO.Path]::GetFileNameWithoutExtension($fa.Path)
        $steps = $e.Status -eq 'A' -or $e.Status -eq 'R' -or [bool]($e.Lines | Where-Object { $_.Trim() -and -not $_.Trim().StartsWith('#') } | Select-Object -First 1)
        $changedScen.Add([pscustomobject]@{ Name = $name; New = ($e.Status -eq 'A'); StepsChanged = $steps; Known = $byName.ContainsKey($name) })
    }
}
$unmapped = @($fileInfo | Where-Object { $_.Unmapped -and $_.Status -ne 'D' })
foreach ($u in $unmapped) { Write-Warning "unmapped: $($u.Path) (no rule in test-areas.txt; add one)" }

# ---------------------------------------------------------------- history: cost and reuse
$history = @{}
$current = $null
if ($Reuse) {
    $bin = Join-Path $BuildDir $Config
    $current = [ordered]@{}
    foreach ($f in $StampFiles) { $p = Join-Path $bin $f; if (Test-Path -LiteralPath $p) { $current[$f] = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant() } }
    if (-not $current.Contains('winhttp.dll')) { Write-Warning "-Reuse: no $bin\winhttp.dll: nothing can be reused (build first)"; $current = $null }
}
if (Test-Path -LiteralPath $RunsDir) {
    foreach ($d in (Get-ChildItem -LiteralPath $RunsDir -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) {  # newest first
        if ($d.Name -notmatch '^\d{8}-\d{6}-(.+)$') { continue }
        $name = $Matches[1]
        if (-not $byName.ContainsKey($name)) { continue }
        $sumPath = Join-Path $d.FullName 'summary.json'
        if (-not (Test-Path -LiteralPath $sumPath)) { continue }
        $raw = [IO.File]::ReadAllText($sumPath)
        if ($raw -notmatch '"result"\s*:\s*"pass"') { continue }
        $alive = 0.0
        foreach ($k in 'game_alive_s', 'guest_alive_s') {
            $m = [regex]::Match($raw, '"' + $k + '"\s*:\s*([0-9.]+)')
            if ($m.Success) { $alive = [math]::Max($alive, [double]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)) }
        }
        if (-not $history.ContainsKey($name)) { $history[$name] = [pscustomobject]@{ Alive = [Collections.Generic.List[double]]::new(); Reusable = $null } }
        if ($alive -gt 0) { $history[$name].Alive.Add($alive) }
        if (-not $current -or $history[$name].Reusable) { continue }
        # -Reuse: a clean pass on the same binaries, steps and seed
        $clean = ($raw -match '"violation"\s*:\s*false') -and ($raw -match '"saves_unchanged"\s*:\s*true') -and ($raw -match '"restored"\s*:\s*true') -and
                 ($raw -match '"online_gate_ok"\s*:\s*true') -and ($raw -match '"cloud_uploads"\s*:\s*\[\s*\]') -and ($raw -match '"game_server_writes"\s*:\s*\[\s*\]')
        if (-not $clean) { continue }
        $stampPath = Join-Path $d.FullName 'run-stamp.txt'
        $scenCopy = Join-Path $d.FullName 'scenario.txt'
        if (-not (Test-Path -LiteralPath $stampPath) -or -not (Test-Path -LiteralPath $scenCopy)) { continue }
        $stamp = @{}
        foreach ($l in [IO.File]::ReadAllLines($stampPath)) { if ($l -match '^([^=]+)=(.*)$') { $stamp[$Matches[1].Trim()] = $Matches[2].Trim().ToLowerInvariant() } }
        $same = $true
        foreach ($k in $current.Keys) { if ($stamp[$k] -ne $current[$k]) { $same = $false } }
        if (-not $same) { continue }
        $s = $byName[$name]
        if ((Get-StepsHashOfText ([IO.File]::ReadAllText($scenCopy))) -ne $s.StepsHash) { continue }
        $wantSeed = if ($s.Seed) { $s.Seed } else { $DefaultSeed }
        $m = [regex]::Match($raw, '"seed_saves"\s*:\s*"([^"]*)"')
        $runSeed = if ($m.Success -and $m.Groups[1].Value) { Split-Path -Leaf ($m.Groups[1].Value -replace '\\\\', '\') } else { '' }
        if ($runSeed -ne $wantSeed) { continue }
        if ($s.Role -eq 'host') {
            $m = [regex]::Match($raw, '"guest_seed"\s*:\s*"([^"]*)"')
            $runGuest = if ($m.Success -and $m.Groups[1].Value) { $m.Groups[1].Value } else { 'full' }
            if ($runGuest -ne $s.GuestSeed) { continue }
        }
        $history[$name].Reusable = $d.FullName
    }
}

$changedNames = @{}; foreach ($c in $changedScen) { if ($c.StepsChanged -and $c.Known) { $changedNames[$c.Name] = $c } }
foreach ($s in $scenarios) {
    $h = $history[$s.Name]
    $alive = $null; $src = 'timeout'
    if ($h -and $h.Alive.Count) {
        $sorted = @($h.Alive | Sort-Object); $alive = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]; $src = "median of $($sorted.Count) passing run(s)"
    } else { $alive = 0.6 * $s.Timeout }
    $cost = 60 + $alive
    if ($s.Role -eq 'host') { $cost *= 1.5 }
    $s | Add-Member -Force EstSeconds ([int][math]::Round($cost))
    $s | Add-Member -Force CostSource $src
    $s | Add-Member -Force ReusedRun $(if ($h) { $h.Reusable } else { $null })
    $w = [double]$cost
    if ($changedNames.ContainsKey($s.Name)) { $w *= 0.5 }
    if ($s.ReusedRun) { $w = 0 }
    $s | Add-Member -Force Weight $w
    $s | Add-Member -Force Effective (Get-EffectiveCovers $s $map)
}

# ---------------------------------------------------------------- selection
$selected = [Collections.Generic.List[object]]::new()
$why = [ordered]@{}            # area -> scenario name
$reasons = @{}                 # scenario name -> text
$covered = [Collections.Generic.HashSet[string]]::new()
function Add-Pick($s, [string]$reason) {
    if ($selected | Where-Object { $_.Name -eq $s.Name }) { return }
    $selected.Add($s); $reasons[$s.Name] = $reason
    foreach ($a in $s.Effective) { if ($touched.Contains($a) -and $covered.Add($a)) { $why[$a] = $s.Name } }
}
if ($Release) {
    foreach ($s in @($scenarios | Where-Object { $_.Select -eq 'release' })) { Add-Pick $s 'release gate (-Release): the launcher''s Local test' }
}
if ($IncludeChanged) {
    foreach ($c in $changedNames.Values) {
        $s = $byName[$c.Name]
        if ($s.Select -ne 'ctest') { Add-Pick $s $(if ($c.New) { 'new scenario (-IncludeChanged)' } else { 'its steps changed (-IncludeChanged)' }) }
    }
}
$candidates = @($scenarios | Where-Object { $_.Select -in 'auto', 'release' -and $_.Covers.Count })
while ($true) {
    $left = @($touched | Where-Object { -not $covered.Contains($_) })
    if (-not $left) { break }
    $best = $null; $bestKey = $null; $bestGain = @()
    foreach ($s in $candidates) {
        if ($selected | Where-Object { $_.Name -eq $s.Name }) { continue }
        $gain = @($s.Effective | Where-Object { $left -contains $_ })
        if (-not $gain) { continue }
        $key = @(($s.Weight / $gain.Count), $s.Covers.Count, $s.Name)
        if (-not $best -or $key[0] -lt $bestKey[0] -or ($key[0] -eq $bestKey[0] -and ($key[1] -lt $bestKey[1] -or ($key[1] -eq $bestKey[1] -and [string]::CompareOrdinal($key[2], $bestKey[2]) -lt 0)))) {
            $best = $s; $bestKey = $key; $bestGain = $gain
        }
    }
    if (-not $best) { break }
    $r = 'covers ' + ($bestGain -join ', ')
    if ($best.ReusedRun) { $r += ' (passed on this build)' } elseif ($changedNames.ContainsKey($best.Name)) { $r += ' (changed by this diff: preferred)' }
    Add-Pick $best $r
}
$uncovered = @($touched | Where-Object { -not $covered.Contains($_) })
if ($Baselines) {
    foreach ($a in @($touched)) {
        $b = @($scenarios | Where-Object { $_.Select -eq 'baseline' -and $_.Covers -contains $a } | Sort-Object Weight, Name | Select-Object -First 1)
        if ($b) { Add-Pick $b[0] "baseline for $a (-Baselines: the fix's before / vanilla comparison)" }
    }
}
$toRun = @($selected | Where-Object { -not $_.ReusedRun } | Sort-Object @{ Expression = { $_.Select -eq 'release' } }, EstSeconds, Name)
$reused = @($selected | Where-Object { $_.ReusedRun })
$estimate = ($toRun | Measure-Object EstSeconds -Sum).Sum
if (-not $estimate) { $estimate = 0 }

# Run-Batch.ps1 command lines, one per seed set
$commands = @()
foreach ($g in ($toRun | Group-Object { '{0}|{1}|{2}' -f $(if ($_.Seed) { $_.Seed } else { $DefaultSeed }), $(if ($_.Role -eq 'host') { $_.GuestSeed } else { 'full' }), $_.Sandbox })) {
    $first = $g.Group[0]
    $seed = if ($first.Seed) { $first.Seed } else { $DefaultSeed }
    $cmd = "pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios $(($g.Group | ForEach-Object Name) -join ',') -SeedSaves $seed"
    if ($first.Role -eq 'host' -and $first.GuestSeed -ne 'full') { $cmd += " -GuestSeed $($first.GuestSeed)" }
    if ($first.Sandbox) { $cmd += ' -- -SandboxSession' }
    $commands += $cmd
}

$result = [pscustomobject]@{
    Diff = $diffLabel; Files = @($fileInfo); Touched = @($touched); Uncovered = $uncovered; Unmapped = @($unmapped | ForEach-Object Path)
    ChangedScenarios = @($changedScen); Selected = @($selected | ForEach-Object Name); Run = @($toRun | ForEach-Object Name)
    Reused = @($reused | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Run = $_.ReusedRun } }); EstimatedSeconds = [int]$estimate
    Commands = $commands; Reasons = $reasons; AreaScenario = $why
}

if ($Explain) {
    Write-Host "Changes: $diffLabel ($($fileInfo.Count) file(s))"
    $noRun = @($fileInfo | Where-Object { -not $_.Unmapped -and -not @($_.Areas | Where-Object { -not $map.Areas[$_].NoRun }) })
    Write-Host "Files -> areas:"
    foreach ($f in ($fileInfo | Where-Object { $noRun -notcontains $_ })) {
        $txt = if ($f.Unmapped) { 'UNMAPPED (add a rule to test-areas.txt)' } else { ($f.Plain.Areas -join ' ') }
        foreach ($x in $f.Extra) { $txt += " + $($x.Areas -join ' ') (changed lines match ~$($x.Regex))" }
        if ($f.Status -eq 'D') { $txt += ' (deleted)' }
        Write-Host ("  {0,-55} {1}" -f $f.Path, $txt)
    }
    if ($noRun) {
        $groups = $noRun | Group-Object { ($_.Areas -join ' ') }
        Write-Host ("  no hidden run: {0} file(s) ({1})" -f $noRun.Count, (($groups | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', '))
    }
    if ($changedScen.Count) {
        Write-Host 'Changed scenarios:'
        foreach ($c in ($changedScen | Where-Object { $_.StepsChanged -or -not $_.Known })) {
            $state = if (-not $c.Known) { 'not in the scenario folder' } elseif ($c.New) { 'new' } else { 'steps changed' }
            Write-Host ("  {0,-40} {1}" -f $c.Name, $state)
        }
        $commentOnly = @($changedScen | Where-Object { $_.Known -and -not $_.StepsChanged })
        if ($commentOnly) { Write-Host ("  {0} scenario(s) with comment-only changes (not a reason to run)" -f $commentOnly.Count) }
    }
    Write-Host ("Touched areas ({0}): {1}" -f $touched.Count, ($touched -join ' '))
    Write-Host 'Selection:'
    $i = 0
    foreach ($s in $selected) {
        $i++
        $tag = if ($s.ReusedRun) { "REUSED $(Split-Path -Leaf $s.ReusedRun)" } else { "~$($s.EstSeconds) s ($($s.CostSource)$(if ($s.Role -eq 'host') { ', two games' }))" }
        Write-Host ("  {0,2}. {1,-36} {2}" -f $i, $s.Name, $tag)
        Write-Host ("      why: {0}; checks: {1}" -f $reasons[$s.Name], ($s.Covers -join ' '))
    }
    Write-Host 'Per area (chosen, then the next cheapest choices):'
    foreach ($a in $touched) {
        $alts = @($candidates | Where-Object { $_.Effective -contains $a -and $_.Name -ne $why[$a] } | Sort-Object Weight, Name | Select-Object -First 3 |
                  ForEach-Object { "$($_.Name) ~$($_.EstSeconds) s" })
        $chosen = if ($why.Contains($a)) { $why[$a] } else { 'NONE: no scenario covers it' }
        $imp = if ($map.Areas[$a].Implicit) { ' (implicit: any run)' } elseif ($map.Areas[$a].ImplicitHost) { ' (implicit: any two-game run)' } else { '' }
        Write-Host ("  {0,-14} {1}{2}{3}" -f $a, $chosen, $imp, $(if ($alts) { '; else ' + ($alts -join ', ') } else { '' }))
    }
}

if ($AsObject) { return $result }
foreach ($n in $result.Run) { Write-Output $n }
if (-not $Explain -and $reused) { foreach ($r in $result.Reused) { Write-Host "reused: $($r.Name) ($(Split-Path -Leaf $r.Run))" } }
if ($uncovered) { Write-Warning ("no scenario covers: {0}" -f ($uncovered -join ', ')) }
if ($toRun) {
    Write-Host ("{0} scenario(s), about {1} min of hidden runs{2}" -f $toRun.Count, [math]::Ceiling($estimate / 60), $(if ($reused) { "; $($reused.Count) reused" } else { '' }))
    foreach ($c in $commands) { Write-Host "  $c" }
} elseif (-not $touched.Count) {
    Write-Host 'no hidden run needed (only no-run areas touched: ctest covers the change)'
} else {
    Write-Host 'nothing to run'
}
