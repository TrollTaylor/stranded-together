<#
.SYNOPSIS
  ctest st_testenv_prune_tests: Prune-TestRuns.ps1 on a fake runs folder in %TEMP% (no game, nothing else touched).
#>
$ErrorActionPreference = 'Stop'
$prune = Join-Path $PSScriptRoot 'Prune-TestRuns.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ("st-prune-test-{0}" -f [guid]::NewGuid().ToString('N'))
$runs = Join-Path $root 'runs'
$outside = Join-Path $root 'outside'
$now = [datetime]'2026-11-22T12:00:00'
$script:failures = 0
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "ok   $what" } else { Write-Host "FAIL $what"; $script:failures++ }
}
function New-Run([string]$name, [int]$kb = 4, [hashtable]$files = @{}) {
    $d = Join-Path $runs $name
    New-Item -ItemType Directory -Force (Join-Path $d 'automation') | Out-Null
    $fs = [IO.File]::Create((Join-Path $d 'automation\shot.png')); $fs.SetLength($kb * 1KB); $fs.Dispose()
    foreach ($k in $files.Keys) { Set-Content -LiteralPath (Join-Path $d $k) -Value $files[$k] }
    $d
}
function Exists([string]$name) { Test-Path -LiteralPath (Join-Path $runs $name) }
function Reset-Runs {
    if (Test-Path $root) { Remove-Item -LiteralPath $root -Recurse -Force }
    New-Item -ItemType Directory -Force $runs, $outside | Out-Null
}

try {
    # ---- age, protections
    Reset-Runs
    New-Run '20261122-110000-recent' | Out-Null                      # 1 h old
    New-Run '20261121-130000-yesterday' | Out-Null                   # 23 h old: last 24 h
    New-Run '20261120-100000-two-days' | Out-Null                    # inside KeepDays 3
    New-Run '20261110-100000-old-plain' | Out-Null
    New-Run '20261110-110000-old-kept' -files @{ 'keep.txt' = 'mine' } | Out-Null
    New-Run '20261110-120000-old-unrestored' -files @{ 'restore.txt' = 'x' } | Out-Null
    New-Run '20261110-130000-old-restored' -files @{ 'restore.txt' = 'x'; 'restored.txt' = 'ok' } | Out-Null
    New-Run '20261110-140000-old-saves-incomplete' -files @{ 'saves-restore.txt' = 'x'; 'saves-restored.txt' = 'INCOMPLETE' } | Out-Null
    New-Run '20261110-150000-old-saves-ok' -files @{ 'saves-restore.txt' = 'x'; 'saves-restored.txt' = "ok`nunchanged" } | Out-Null
    $d = New-Run '20261110-160000-old-saves-added' -files @{ 'saves-restore.txt' = 'x'; 'saves-restored.txt' = "ok`nrestored" }
    New-Item -ItemType Directory -Force (Join-Path $d 'saves-added\DeathStrandingDC\1') | Out-Null
    Set-Content (Join-Path $d 'saves-added\DeathStrandingDC\1\checkpoint.dat') 'the only copy'
    $d = New-Run '20261110-170000-old-game-new' -files @{ 'restore.txt' = 'x'; 'restored.txt' = 'ok' }
    New-Item -ItemType Directory -Force (Join-Path $d 'game-new') | Out-Null
    Set-Content (Join-Path $d 'game-new\userfile.txt') 'moved out of the game folder'
    New-Run '20261110-180000-old-saves-restored' -files @{ 'saves-restore.txt' = 'x'; 'saves-restored.txt' = "ok`nrestored" } | Out-Null
    New-Item -ItemType Directory -Force (Join-Path $runs '20261110-190000-old-empty-game-new\game-new') | Out-Null
    New-Item -ItemType Directory (Join-Path $runs 'probe-1') | Out-Null
    New-Item -ItemType Directory (Join-Path $runs 'notes') | Out-Null
    Set-Content (Join-Path $runs 'readme.txt') 'not a run'
    Set-Content (Join-Path $outside 'precious.txt') 'outside the runs folder'
    New-Item -ItemType Junction -Path (Join-Path $runs '20261101-100000-junction') -Target $outside | Out-Null

    & $prune -IgnoreSupervisor -RunsDir $runs -Now $now -DryRun -KeepNewest 1 | Out-Null
    Check ((Exists '20261110-100000-old-plain') -and (Exists '20261110-130000-old-restored')) 'dry run deletes nothing'

    $env:ST_TESTENV_KEEP_ALL = '1'
    & $prune -IgnoreSupervisor -RunsDir $runs -Now $now -KeepNewest 1 | Out-Null
    Remove-Item Env:\ST_TESTENV_KEEP_ALL
    Check (Exists '20261110-100000-old-plain') 'ST_TESTENV_KEEP_ALL=1 deletes nothing'

    & $prune -IgnoreSupervisor -RunsDir $runs -Now $now -KeepNewest 1 | Out-Null
    Check (Exists '20261122-110000-recent') 'newest run kept'
    Check (Exists '20261121-130000-yesterday') 'a run from the last 24 h kept'
    Check (Exists '20261120-100000-two-days') 'a run inside KeepDays kept'
    Check (-not (Exists '20261110-100000-old-plain')) 'an old run deleted'
    Check (Exists '20261110-110000-old-kept') 'an old run with keep.txt kept'
    Check (Exists '20261110-120000-old-unrestored') 'an old run whose game-folder restore never finished kept'
    Check (-not (Exists '20261110-130000-old-restored')) 'an old, restored run deleted'
    Check (Exists '20261110-140000-old-saves-incomplete') 'an old run whose saves restore is INCOMPLETE kept'
    Check (-not (Exists '20261110-150000-old-saves-ok')) 'an old run whose saves were checked unchanged deleted'
    Check (Exists '20261110-160000-old-saves-added') 'an old run holding files moved out of the real saves (saves-added) kept'
    Check (Exists '20261110-170000-old-game-new') 'an old run holding files moved out of the game folder (game-new) kept'
    Check (Exists '20261110-180000-old-saves-restored') 'an old run whose saves had to be restored kept'
    Check (-not (Exists '20261110-190000-old-empty-game-new')) 'an old run with an empty game-new deleted'
    Check ((Exists 'probe-1') -and (Exists 'notes') -and (Exists 'readme.txt')) 'folders and files not named like a run kept'
    Check (Exists '20261101-100000-junction') 'a junction named like an old run kept'
    Check (Test-Path (Join-Path $outside 'precious.txt')) 'nothing outside the runs folder touched'
    $log = Join-Path $runs 'prune.log'
    Check ((Test-Path $log) -and ((Get-Content $log -Raw) -match 'deleted 20261110-100000-old-plain')) 'prune.log lists each deletion'

    # ---- size cap: oldest unprotected first, protected ones still count
    Reset-Runs
    New-Run '20261122-110000-newest' -kb 400 | Out-Null               # protected (newest), counts toward the cap
    New-Run '20261121-090000-c' -kb 400 | Out-Null                    # 27 h old, unprotected
    New-Run '20261120-090000-b' -kb 400 | Out-Null
    New-Run '20261120-080000-a' -kb 400 | Out-Null
    New-Run '20261119-080000-pinned' -kb 400 -files @{ 'keep.txt' = 'mine' } | Out-Null               # older, protected
    New-Run '20261119-070000-unrestored' -kb 400 -files @{ 'restore.txt' = 'x' } | Out-Null           # older, protected
    New-Run '20261122-020000-ten-hours' -kb 400 | Out-Null                                            # last 24 h
    New-Item -ItemType Junction -Path (Join-Path $runs '20261119-060000-link') -Target $outside | Out-Null  # counts 0
    $fs = [IO.File]::Create((Join-Path $outside 'big.bin')); $fs.SetLength(50MB); $fs.Dispose()
    # cap 2010 KB: 2800 KB (+ the marker files' bytes) in all, the link's 50 MB target not counted -> a (oldest unprotected)
    # then b go, about 2000 KB stay
    & $prune -IgnoreSupervisor -RunsDir $runs -Now $now -KeepNewest 1 -MaxGB (2010KB / 1GB) | Out-Null
    Check ((-not (Exists '20261120-080000-a')) -and (-not (Exists '20261120-090000-b'))) 'over the size cap: the oldest unprotected runs go first'
    Check ((Exists '20261121-090000-c') -and (Exists '20261122-110000-newest')) 'under the size cap again: the rest stay'
    Check ((Exists '20261119-080000-pinned') -and (Exists '20261119-070000-unrestored')) 'the size cap skips a pinned run and an unfinished restore'
    Check (Exists '20261122-020000-ten-hours') 'the size cap never takes a run from the last 24 h'
    Check ((Exists '20261119-060000-link') -and (Test-Path (Join-Path $outside 'big.bin'))) 'a link is not counted, followed or deleted'

    # ---- the 24 h rule alone: KeepDays 1, a 20 h old run outside KeepNewest
    Reset-Runs
    New-Run '20261122-110000-newest' | Out-Null
    New-Run '20261121-160000-twenty-hours' | Out-Null
    New-Run '20261121-080000-twenty-eight-hours' | Out-Null
    & $prune -IgnoreSupervisor -RunsDir $runs -Now $now -KeepNewest 1 -KeepDays 1 | Out-Null
    Check ((Exists '20261121-160000-twenty-hours') -and -not (Exists '20261121-080000-twenty-eight-hours')) 'KeepDays 1: 20 h kept (last 24 h), 28 h deleted'

    # ---- refuses a folder not named runs
    Set-Content (Join-Path $outside 'precious.txt') 'outside the runs folder'
    New-Item -ItemType Directory (Join-Path $outside '20261101-100000-looks-like-a-run') | Out-Null
    $threw = $false
    try { & $prune -RunsDir $outside -Now $now | Out-Null } catch { $threw = $true }
    Check $threw 'refuses a folder not named runs'
    Check ((Test-Path (Join-Path $outside 'precious.txt')) -and (Test-Path (Join-Path $outside '20261101-100000-looks-like-a-run'))) 'the refused folder is untouched'
} finally {
    Remove-Item Env:\ST_TESTENV_KEEP_ALL -ErrorAction SilentlyContinue
    if (Test-Path $root) {
        # the junction first, so removing the tree never follows it
        Get-ChildItem -LiteralPath $runs -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | ForEach-Object { $_.Delete() }
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
if ($script:failures) { Write-Host "$script:failures check(s) failed"; exit 1 }
Write-Host 'all prune checks passed'
exit 0
