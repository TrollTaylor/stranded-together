<#
.SYNOPSIS
  ctest st_testenv_select_tests: Select-Scenarios.ps1 on a fake scenario folder, area map, runs folder and diffs in %TEMP%
  (no game, no git writes), then its -Check on the real repo (every scenario has "# covers:", every tracked file an area).
#>
$ErrorActionPreference = 'Stop'
$sel = Join-Path $PSScriptRoot 'Select-Scenarios.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ("st-select-test-{0}" -f [guid]::NewGuid().ToString('N'))
$scen = Join-Path $root 'scenarios'
$map = Join-Path $root 'test-areas.txt'
$runs = Join-Path $root 'runs'
$build = Join-Path $root 'build'
$script:failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $script:failures++ }
}
function Same($a, $b) { (@($a) -join ',') -eq (@($b) -join ',') }
function New-Scenario([string]$name, [string]$covers, [string]$select = '', [string]$role = 'solo', [int]$timeout = 100, [string]$extraHeader = '', [string]$steps = 'wait 1000') {
    $lines = @("# $name (fixture)")
    if ($extraHeader) { $lines += "# $extraHeader" }
    if ($covers) { $lines += "# covers: $covers" }
    if ($select) { $lines += "# select: $select" }
    $lines += "role $role", "timeout $timeout", $steps, 'quit'
    Set-Content -LiteralPath (Join-Path $scen "$name.txt") -Value ($lines -join "`r`n") -Encoding ascii
}
# One entry per file: @{ Path; Status = 'A'|'M'|'D'; Added = @(lines); Removed = @(lines) }
function New-Diff([object[]]$files) {
    $out = @()
    foreach ($f in $files) {
        $out += "diff --git a/$($f.Path) b/$($f.Path)"
        if ($f.Status -eq 'A') { $out += 'new file mode 100644' }
        if ($f.Status -eq 'D') { $out += 'deleted file mode 100644' }
        $out += $(if ($f.Status -eq 'A') { '--- /dev/null' } else { "--- a/$($f.Path)" })
        $out += $(if ($f.Status -eq 'D') { '+++ /dev/null' } else { "+++ b/$($f.Path)" })
        $out += '@@ -1,1 +1,1 @@'
        foreach ($l in @($f.Removed)) { if ($null -ne $l) { $out += "-$l" } }
        foreach ($l in @($f.Added)) { if ($null -ne $l) { $out += "+$l" } }
    }
    $p = Join-Path $root ("diff-{0}.txt" -f [guid]::NewGuid().ToString('N'))
    Set-Content -LiteralPath $p -Value ($out -join "`n") -Encoding ascii
    $p
}
function Select-For([object[]]$files, [hashtable]$opts = @{}) {
    $d = New-Diff $files
    $a = @{ DiffFile = $d; ScenarioDir = $scen; AreaMap = $map; RunsDir = $runs; BuildDir = $build; AsObject = $true } + $opts
    & $sel @a 6>$null 3>$null
}
function M([string]$path, [string[]]$added = @('x'), [string[]]$removed = @()) { @{ Path = $path; Status = 'M'; Added = $added; Removed = $removed } }
function New-Run([string]$name, [string]$scenarioName, [double]$alive, [string]$stamp, [string]$result = 'pass', [string]$savesUnchanged = 'true') {
    $d = Join-Path $runs $name
    New-Item -ItemType Directory -Force $d | Out-Null
    Copy-Item -LiteralPath (Join-Path $scen "$scenarioName.txt") -Destination (Join-Path $d 'scenario.txt')
    $json = '{"mode":"game","result":"' + $result + '","violation":false,"game_alive_s":' + $alive.ToString([Globalization.CultureInfo]::InvariantCulture) +
            ',"restored":true,"saves_unchanged":' + $savesUnchanged + ',"seed_saves":"C:\\x\\testenv-saves\\complete-100","cloud_uploads":[],"game_server_writes":[],"online_gate_ok":true}'
    Set-Content -LiteralPath (Join-Path $d 'summary.json') -Value $json -Encoding ascii
    if ($stamp) { Set-Content -LiteralPath (Join-Path $d 'run-stamp.txt') -Value "winhttp.dll=$stamp" -Encoding ascii }
}

try {
    New-Item -ItemType Directory -Force $scen, $runs, (Join-Path $build 'Release') | Out-Null
    @(
        'area none no-run : docs and the like'
        'area boot implicit : every run'
        'area link implicit-host : every two-game run'
        'area alpha : a'
        'area beta : b'
        'area gamma : c'
        'area launcher : the Local test'
        'docs/*                      none'
        'tools/testenv/scenarios/*   none'
        'src/alpha.cpp               alpha'
        'src/beta.cpp                beta'
        'src/big.cpp                 boot'
        'src/big.cpp ~gamma_thing    gamma'
        'src/link.cpp                link'
        'src/launcher.cpp            launcher'
        'src/*                       boot'
    ) | Set-Content -LiteralPath $map -Encoding ascii
    New-Scenario 's-smoke' 'boot'
    New-Scenario 's-alpha-cheap' 'alpha'
    New-Scenario 's-alpha-dear' 'alpha' -timeout 250                       # 60 + 150 = 210 s
    New-Scenario 's-beta' 'beta' -extraHeader 'pwsh -File Start-TestRun.ps1 -Scenario s-beta -SeedSaves story-pack.'
    New-Scenario 's-alphabeta' 'alpha beta' -timeout 150                   # 150 s for two areas: cheaper than two runs of 120 s
    New-Scenario 's-gamma' 'gamma'
    New-Scenario 's-gamma-base' 'gamma' -select 'baseline'
    New-Scenario 's-probe' 'alpha' -select 'probe' -timeout 10
    New-Scenario 's-ctest' 'boot' -select 'ctest' -timeout 10
    New-Scenario 's-two' 'link' -role 'host' -extraHeader '-SeedSaves complete-100 -GuestSeed profile-only'
    New-Scenario 's-local' 'launcher' -select 'release' -role 'host' -timeout 200

    # ---- areas and the cover
    $r = Select-For @((M 'docs/readme.md'))
    Check ($r.Run.Count -eq 0 -and $r.Touched.Count -eq 0) 'a docs-only change needs no hidden run'
    $r = Select-For @((M 'src/alpha.cpp'))
    Check (Same $r.Run 's-alpha-cheap') 'one area: its cheapest scenario (not the dearer one, never the cheaper probe)'
    $r = Select-For @((M 'src/alpha.cpp'), (M 'src/beta.cpp'))
    Check (Same $r.Run 's-alphabeta') 'two areas: the one scenario covering both beats two cheaper runs'
    $r = Select-For @((M 'src/big.cpp' @('int x = gamma_thing();')))
    Check ((Same $r.Touched 'boot,gamma') -and (Same $r.Run 's-gamma')) 'a ~regex rule adds its area when a changed line matches; the implicit boot rides along'
    $r = Select-For @((M 'src/big.cpp' @('int x = 1;')))
    Check ((Same $r.Touched 'boot') -and $r.Run.Count -eq 1 -and $r.Run[0] -ne 's-ctest') 'no matching line: only the plain rule''s implicit area, one run (never the ctest scenario)'
    $r = Select-For @((M 'src/big.cpp' @(('x' * 210) + ' gamma_thing')))
    Check (Same $r.Touched 'boot') 'a changed line over 200 characters (a usage list) does not trigger a ~regex rule'
    $r = Select-For @((M 'src/big.cpp' @() @('old gamma_thing();')))
    Check ($r.Touched -contains 'gamma') 'a removed line matches too'
    $r = Select-For @((M 'src/link.cpp'))
    Check (Same $r.Run 's-two') 'an implicit-host area: the cheapest two-game scenario'
    $r = Select-For @(@{ Path = 'src/alpha.cpp'; Status = 'D'; Added = @(); Removed = @('gone') })
    Check ($r.Run.Count -eq 0) 'a deleted file touches nothing'
    $r = Select-For @((M 'src/new.cpp'), (M 'lib/other.cpp'))
    Check ((Same $r.Unmapped 'lib/other.cpp') -and (Same $r.Touched 'boot')) 'a file no plain rule matches is reported unmapped'

    # ---- release, baselines
    $r = Select-For @((M 'docs/readme.md')) @{ Release = $true }
    Check (Same $r.Run 's-local') '-Release: the Local test even when nothing else is needed'
    $r = Select-For @((M 'src/launcher.cpp'), (M 'src/link.cpp')) @{ Release = $true }
    Check (Same $r.Run 's-local') '-Release: the Local test covers launcher and link, nothing more'
    $r = Select-For @((M 'src/alpha.cpp')) @{ Release = $true }
    Check (Same $r.Run 's-alpha-cheap,s-local') '-Release: the Local test runs last'
    $r = Select-For @((M 'src/big.cpp' @('gamma_thing')))
    Check ($r.Selected -notcontains 's-gamma-base') 'a baseline is never chosen by default'
    $r = Select-For @((M 'src/big.cpp' @('gamma_thing'))) @{ Baselines = $true }
    Check (Same $r.Run 's-gamma,s-gamma-base') '-Baselines adds the touched area''s baseline'

    # ---- changed scenarios
    $r = Select-For @((M 'src/alpha.cpp'), @{ Path = 'tools/testenv/scenarios/s-alpha-dear.txt'; Status = 'A'; Added = @('role solo', 'wait 1') })
    Check (Same $r.Run 's-alpha-dear') 'a scenario the change added costs half: preferred for its area'
    $r = Select-For @((M 'src/alpha.cpp'), (M 'tools/testenv/scenarios/s-alpha-dear.txt' @('# better words') @('# old words')))
    Check (Same $r.Run 's-alpha-cheap') 'a comments-only scenario change is not preferred'
    $r = Select-For @((M 'tools/testenv/scenarios/s-alpha-dear.txt' @('# covers: alpha'))) @{ IncludeChanged = $true }
    Check ($r.Run.Count -eq 0) '-IncludeChanged ignores a comments-only change'
    $r = Select-For @((M 'tools/testenv/scenarios/s-alpha-dear.txt' @('wait 2000') @('wait 1000'))) @{ IncludeChanged = $true }
    Check (Same $r.Run 's-alpha-dear') '-IncludeChanged runs a scenario whose steps changed'

    # ---- seeds -> Run-Batch commands
    $r = Select-For @((M 'src/beta.cpp'), (M 'src/link.cpp'), (M 'src/big.cpp' @('gamma_thing')))
    $listed = @($r.Commands | ForEach-Object { if ($_ -match '-Scenarios (\S+)') { $Matches[1] -split ',' } })
    Check (($r.Commands.Count -eq 3) -and (Same ($listed | Sort-Object) ($r.Run | Sort-Object))) 'one command per seed set, listing every scenario to run once'
    $r = Select-For @((M 'src/beta.cpp'))
    Check ((Same $r.Run 's-beta') -and ($r.Commands[0] -match '-SeedSaves story-pack$')) 'a scenario''s header seed (-SeedSaves story-pack.) goes on its command, trailing dot dropped'
    $r = Select-For @((M 'src/link.cpp'))
    Check ($r.Commands[0] -match '-SeedSaves complete-100 -GuestSeed profile-only$') 'a two-game scenario''s guest seed goes on its command'
    $r = Select-For @((M 'src/alpha.cpp'))
    Check ($r.Commands[0] -match '-SeedSaves complete-100$') 'no seed in the header: the default complete-100'

    # ---- history: cost and reuse
    New-Run '20261101-100000-s-alpha-dear' 's-alpha-dear' 20 ''
    $r = Select-For @((M 'src/alpha.cpp'))
    Check (Same $r.Run 's-alpha-dear') 'a passing run''s game time replaces the timeout estimate (80 s < 120 s)'
    New-Run '20261101-110000-s-alpha-cheap' 's-alpha-cheap' 500 '' 'fail'
    $r = Select-For @((M 'src/alpha.cpp'))
    Check (Same $r.Run 's-alpha-dear') 'a failed run does not count for the cost'
    Remove-Item -LiteralPath $runs -Recurse -Force; New-Item -ItemType Directory $runs | Out-Null
    Set-Content -LiteralPath (Join-Path $build 'Release\winhttp.dll') -Value 'fake mod build 1' -Encoding ascii
    $sha = (Get-FileHash -LiteralPath (Join-Path $build 'Release\winhttp.dll') -Algorithm SHA256).Hash.ToLowerInvariant()
    New-Run '20261101-120000-s-alpha-cheap' 's-alpha-cheap' 40 $sha
    $r = Select-For @((M 'src/alpha.cpp'))
    Check (Same $r.Run 's-alpha-cheap') 'without -Reuse a passing run is not reused'
    $r = Select-For @((M 'src/alpha.cpp')) @{ Reuse = $true }
    Check (($r.Run.Count -eq 0) -and ((Same ($r.Reused | ForEach-Object Name) 's-alpha-cheap'))) '-Reuse: a clean pass on the same build, steps and seed is reused, not run'
    Set-Content -LiteralPath (Join-Path $build 'Release\winhttp.dll') -Value 'fake mod build 2' -Encoding ascii
    $r = Select-For @((M 'src/alpha.cpp')) @{ Reuse = $true }
    Check ((Same $r.Run 's-alpha-cheap') -and -not $r.Reused) '-Reuse: another build is run again'
    Set-Content -LiteralPath (Join-Path $build 'Release\winhttp.dll') -Value 'fake mod build 1' -Encoding ascii
    New-Scenario 's-alpha-cheap' 'alpha' -steps 'wait 2000'
    $r = Select-For @((M 'src/alpha.cpp')) @{ Reuse = $true }
    Check (-not $r.Reused) '-Reuse: changed steps are run again'
    New-Scenario 's-alpha-cheap' 'alpha' -extraHeader 'a new comment line'
    $r = Select-For @((M 'src/alpha.cpp')) @{ Reuse = $true }
    Check ($r.Reused.Count -eq 1) '-Reuse: a comment change keeps the run reusable'
    New-Run '20261101-130000-s-alpha-cheap' 's-alpha-cheap' 40 $sha 'pass' 'false'
    Remove-Item -LiteralPath (Join-Path $runs '20261101-120000-s-alpha-cheap') -Recurse -Force
    $r = Select-For @((M 'src/alpha.cpp')) @{ Reuse = $true }
    Check (-not $r.Reused) '-Reuse: a pass whose saves changed is not clean: run again'

    # ---- steps hash
    $h1 = & $sel -StepsHash (Join-Path $scen 's-alpha-cheap.txt')
    New-Scenario 's-alpha-cheap' 'alpha beta' -extraHeader 'other words'
    $h2 = & $sel -StepsHash (Join-Path $scen 's-alpha-cheap.txt')
    New-Scenario 's-alpha-cheap' 'alpha' -steps 'wait 3000'
    $h3 = & $sel -StepsHash (Join-Path $scen 's-alpha-cheap.txt')
    Check (($h1 -eq $h2) -and ($h1 -ne $h3) -and ($h1 -match '^[0-9a-f]{64}$')) '-StepsHash ignores comments, sees steps'
    New-Scenario 's-alpha-cheap' 'alpha'

    # ---- -Check
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @('docs/a.md', 'src/x.cpp') 6>$null
    Check ($LASTEXITCODE -eq 0) '-Check passes on the fixture'
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @('lib/unmapped.cpp') 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for a file no rule matches'
    New-Scenario 's-bad' 'alpha nosuch'
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @() 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for an undeclared area in "# covers:"'
    New-Scenario 's-bad' ''
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @() 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for a scenario without "# covers:"'
    New-Scenario 's-bad' 'beta' -select 'sometimes'
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @() 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for an unknown "# select:"'
    Remove-Item -LiteralPath (Join-Path $scen 's-bad.txt')
    Remove-Item -LiteralPath (Join-Path $scen 's-gamma.txt')
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @() 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for an area only a baseline covers'
    New-Scenario 's-gamma' 'gamma'
    New-Scenario 's-local2' 'launcher' -select 'release' -role 'host'
    & $sel -Check -ScenarioDir $scen -AreaMap $map -CheckFiles @() 6>$null
    Check ($LASTEXITCODE -eq 1) '-Check fails for two release scenarios'
    Remove-Item -LiteralPath (Join-Path $scen 's-local2.txt')

    # ---- the real repo: every scenario has its covers line, every tracked file an area (git ls-files; skipped without git)
    if (Get-Command git -ErrorAction SilentlyContinue) {
        $out = & $sel -Check 6>&1 | Out-String
        $code = $LASTEXITCODE
        if ($code -ne 0) { Write-Host $out }
        Check ($code -eq 0) 'the repo: every scenario names its areas, every area has a scenario, every tracked file an area'
        # git's own diff paths (read-only): a range, and the working tree with its untracked files
        $r = & $sel -Range 'HEAD..HEAD' -RunsDir $runs -AsObject 6>$null 3>$null
        Check (($r.Files.Count -eq 0) -and ($r.Run.Count -eq 0)) 'the repo: an empty range selects nothing'
        $r = & $sel -RunsDir $runs -AsObject 6>$null 3>$null
        Check ($null -ne $r -and $r.Diff -like 'git diff HEAD*') 'the repo: the working tree against HEAD (untracked files included) runs'
    } else {
        $out = & $sel -Check -CheckFiles @() 6>&1 | Out-String
        Check ($LASTEXITCODE -eq 0) 'the repo''s scenarios and area map (git not found: tracked files not checked)'
    }
} finally {
    if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($script:failures) { Write-Host "$($script:failures) check(s) failed"; exit 1 }
Write-Host 'all checks passed'
exit 0
