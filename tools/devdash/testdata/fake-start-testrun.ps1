# Stand-in for Start-TestRun.ps1 in tools/devdash/test_devdash.py: makes a run folder, starts no game.
param([string]$Scenario, [int]$TimeoutSec, [string]$BuildDir, [string]$SeedSaves, [string]$GuestSeed, [switch]$NoBuild,
      [Parameter(ValueFromRemainingArguments)][string[]]$Rest)
$name = [IO.Path]::GetFileNameWithoutExtension($Scenario)
$run = Join-Path $BuildDir ("runs\{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $name)
New-Item -ItemType Directory -Force $run | Out-Null
$ok = $name -notmatch 'fail'
$res = if ($ok) { 'pass' } else { 'fail' }
$final = if ($ok) { 'PASSED (exit 0)' } else { 'FAILED (exit 1)' }
"[   0.000] st_testenv_supervisor (pid $PID) started 2026-11-21T10:00:00.000`n[   1.000] run $final. Run folder: $run" | Set-Content (Join-Path $run 'supervisor.log')
@{ mode = 'game'; result = $res; game_exit_code = 0; saves_unchanged = $true; restored = $true; violation = $false
   failures = $(if ($ok) { 0 } else { 1 }); cloud_uploads = @()
   steps = @(@{ line = 3; optional = $false; ok = $ok; step = 'wait 1'; detail = "nobuild=$NoBuild seed=$SeedSaves rest=$($Rest -join ' ')" }) } |
    ConvertTo-Json -Depth 4 | Set-Content (Join-Path $run 'summary.json')
Start-Sleep -Milliseconds 1100
if ($ok) { exit 0 } else { exit 1 }
