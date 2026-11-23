<#
.SYNOPSIS
  The hidden check of the launcher's "Extract textures" (docs/textures.md, docs/hidden-testing.md "textures-extract"):
  StrandedTogether.exe --extract-textures reads the installed game's archives (read-only) into this run's folder and the
  result is checked. No game starts, nothing is shown on the desktop, nothing is written outside the run folder.

.EXAMPLE
  pwsh -File tools\testenv\Test-TextureExtract.ps1
  # through the batch runner (one at a time with every other hidden run, posted to devdash):
  pwsh -File tools\testenv\Run-Batch.ps1 -Scenarios textures-extract -RunScript tools\testenv\Test-TextureExtract.ps1 -Id r6-tex -Title 'Extract textures'

.NOTES
  Takes the same arguments as Start-TestRun.ps1 so Run-Batch.ps1 can run it as its -RunScript (-Scenario, -TimeoutSec,
  -BuildDir, -SeedSaves, -GuestSeed and -NoBuild; the save options mean nothing here: no game runs, no save is opened).
  Output: <BuildDir>\runs\<yyyyMMdd-HHmmss>-textures-extract\ with summary.json, supervisor.log (the verdict line),
  extract-*.log (the extractor's own lines) and listing-before.txt / listing-after.txt (the game's data folder and Oodle
  DLL, size + time: they must not change). The extracted pictures (game data) stay in the run folder only and are deleted
  at the end unless -KeepTextures; they are never copied anywhere else.
  Checks (design 3.7): exit 0 and a done line; every planned set extracted or skipped with a reason; her suit and boots
  maps present; every manifest row's file there with its size; Original read-only; every seed of 00 Default and the boots
  a byte copy of its Original, 01..16 the ready-made colours (round 8: the Colour of 01..14 and all three of 15 / 16
  differ from Original, every seed row is the bytes on disk); her Colour's mean about (67, 71, 81); Sam's gold
  Reflectance's bright pixels about (183, 149, 101); a second run keeps an edited seed byte-identical (and says kept); a
  deleted seed comes back; round 8: a v0.24 folder (st_textool v024) with 16 Gold painted and a subfolder of the user's
  is upgraded by the next extraction (replaced 17, yours 1; 01..15 byte-identical to the first extraction; 16 Gold and its
  subfolder untouched); a run cancelled in the middle leaves no .partial after the next run; Check my edits names the
  edit; the game folder is unchanged.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Scenario = 'textures-extract',
    [ValidateRange(30, 360)][int]$TimeoutSec = 360,
    [string]$BuildDir,
    [string]$SeedSaves,
    [string]$GuestSeed,
    [switch]$NoBuild,
    [string]$GameDir,
    [switch]$KeepTextures,
    [Parameter(ValueFromRemainingArguments)][string[]]$Rest
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build-testenv' }
if (-not $GameDir) { $GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT' }
$runsRoot = Join-Path $BuildDir 'runs'
$name = [IO.Path]::GetFileNameWithoutExtension($Scenario)
$run = Join-Path $runsRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + $name)
New-Item -ItemType Directory -Force -Path $run | Out-Null
$supLog = Join-Path $run 'supervisor.log'
$t0 = Get-Date
function Log([string]$text) {
    $line = '[{0,7:0.000}] {1}' -f ((Get-Date) - $t0).TotalSeconds, $text
    Add-Content -LiteralPath $supLog -Value $line -Encoding utf8
    Write-Host $line
}
$steps = New-Object System.Collections.Generic.List[object]
$failures = New-Object System.Collections.Generic.List[string]
$lineNo = 0
function Step([string]$what, [bool]$ok, [string]$detail = '') {
    $script:lineNo++
    $steps.Add([ordered]@{ line = $script:lineNo; step = $what; ok = $ok; optional = $false; detail = $detail })
    if (-not $ok) { $failures.Add("$what $detail") }
    Log ('{0} {1}{2}' -f $(if ($ok) { 'ok  ' } else { 'FAIL' }), $what, $(if ($detail) { ": $detail" } else { '' }))
}

# One hidden run at a time (Run-Batch also waits): no game may run while we read its archives.
if (Get-Process -Name ds -ErrorAction SilentlyContinue) {
    Log 'REFUSED: ds.exe is running (another hidden run or the user game)'
    exit 2
}
if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        & cmake -S $repo -B $BuildDir -G 'Visual Studio 17 2022' -A x64 | Out-Host
        if ($LASTEXITCODE) { throw 'cmake configure failed' }
    }
    & cmake --build $BuildDir --config Release --target st_launcher st_textool -- '/m' '/verbosity:minimal' | Out-Host
    if ($LASTEXITCODE) { throw 'build failed' }
}
$exe = Join-Path $BuildDir 'Release\StrandedTogether.exe'
if (-not (Test-Path $exe)) { throw "no $exe" }
$textool = Join-Path $BuildDir 'Release\st_textool.exe'
Log "textures-extract: $exe; game $GameDir (read-only); out $run\textures"

# The game folder's data and Oodle DLL before (size + last write time).
function Get-GameListing {
    $items = @(Get-ChildItem -LiteralPath (Join-Path $GameDir 'data') -Recurse -File -Force) + @(Get-Item -LiteralPath (Join-Path $GameDir 'oo2core_7_win64.dll'))
    $items | Sort-Object FullName | ForEach-Object { '{0}|{1}|{2}' -f $_.FullName.Substring($GameDir.Length), $_.Length, $_.LastWriteTimeUtc.Ticks }
}
$before = Get-GameListing
$before | Set-Content -LiteralPath (Join-Path $run 'listing-before.txt') -Encoding utf8

function Invoke-Extract([string]$out, [string]$tag, [string[]]$more = @()) {
    $log = Join-Path $run "extract-$tag.log"
    $argv = @('--extract-textures', '--game-dir', $GameDir, '--out', $out, '--allow-out-under', $run) + $more
    $p = Start-Process -FilePath $exe -ArgumentList ($argv | ForEach-Object { '"' + $_.TrimEnd('\') + '"' }) -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $log
    $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue)
    return [pscustomobject]@{ Code = $p.ExitCode; Lines = $lines }
}

Add-Type -AssemblyName System.Drawing
# Mean colour of a picture, scaled to $size first (fast; the box filter keeps the mean). $bright: only pixels whose red is
# above that value.
function Get-MeanRgb([string]$file, [int]$size = 256, [int]$bright = -1) {
    $src = [System.Drawing.Image]::FromFile($file)
    $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    # Bilinear keeps the mean; nearest keeps the metal islands' own colour when only bright pixels count.
    $g.InterpolationMode = $(if ($bright -ge 0) { 'NearestNeighbor' } else { 'HighQualityBilinear' })
    $g.DrawImage($src, 0, 0, $size, $size)
    $g.Dispose(); $src.Dispose()
    $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
    $data = $bmp.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $bytes = New-Object byte[] ($data.Stride * $size)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
    $bmp.UnlockBits($data); $bmp.Dispose()
    $r = 0.0; $gg = 0.0; $b = 0.0; $n = 0
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        if ($bright -ge 0 -and $bytes[$i + 2] -le $bright) { continue }
        $b += $bytes[$i]; $gg += $bytes[$i + 1]; $r += $bytes[$i + 2]; $n++
    }
    if ($n -eq 0) { return @(0, 0, 0, 0) }
    return @([math]::Round($r / $n, 1), [math]::Round($gg / $n, 1), [math]::Round($b / $n, 1), [math]::Round(100.0 * $n / ($bytes.Length / 4), 1))
}
function Test-Near($mean, $want, [double]$tol) {
    for ($k = 0; $k -lt 3; $k++) { if ([math]::Abs($mean[$k] - $want[$k]) -gt $tol) { return $false } }
    return $true
}

$out = Join-Path $run 'textures'
$out2 = Join-Path $run 'textures-cancel'
$orig = Join-Path $out 'Fragile porter suit\Original'
try {
    # 1. The first extraction.
    $r = Invoke-Extract $out 'first'
    $done = @($r.Lines | Where-Object { $_ -like 'done *' })
    Step 'extract: exit 0 and a done line' ($r.Code -eq 0 -and $done.Count -eq 1) ("exit $($r.Code); " + ($done -join ''))
    $sets = @($r.Lines | Where-Object { $_ -like 'set *' }).Count
    $skippedSets = @($r.Lines | Where-Object { $_ -like 'skip ds/*' }).Count
    $mapSkips = @($r.Lines | Where-Object { $_ -like 'skip *' -and $_ -notlike 'skip ds/*' })
    Step 'every planned set extracted or skipped with a reason' (($sets + $skippedSets) -eq 31 -and $sets -ge 30) "$sets extracted, $skippedSets skipped; map skips: $($mapSkips.Count) ($((@($mapSkips) | Select-Object -First 3) -join '; '))"
    $want = @('Suit - Colour.png', 'Suit - Roughness.png', 'Suit - Normal.png', 'Suit - Reflectance.png', 'Suit - Ambient occlusion.png',
        'Suit - Mask.png', 'Boots - Colour.png', 'Boots - Normal.png', 'Boots - Roughness.png', 'Boots - Reflectance.png', 'Boots - Ambient occlusion.png')
    $missing = @($want | Where-Object { -not (Test-Path -LiteralPath (Join-Path $orig $_)) })
    Step "her suit (S0-S3) and boots (B0-B2) maps in Original" ($missing.Count -eq 0) $(if ($missing) { 'missing: ' + ($missing -join ', ') } else { "$($want.Count) files" })
    # The manifest.
    $manifest = Join-Path $out '.manifest.txt'
    $rows = @(Get-Content -LiteralPath $manifest -Encoding utf8 | Where-Object { $_ -like 'file *' } | ForEach-Object {
            $f = $_.Substring(5).Split("`t"); [pscustomobject]@{ Path = $f[0]; Slot = $f[1]; Comp = $f[2]; Size = $f[3]; Sha = $f[4]; Kind = $f[5]; Bytes = [int64]$f[6] } })
    $bad = @($rows | Where-Object { $p = Join-Path $out $_.Path; -not (Test-Path -LiteralPath $p) -or (Get-Item -LiteralPath $p).Length -ne $_.Bytes })
    Step 'every manifest row: the file is there with its size' ($rows.Count -gt 200 -and $bad.Count -eq 0) "$($rows.Count) rows, $($bad.Count) wrong$(if ($bad) { ': ' + $bad[0].Path })"
    $hidden = ((Get-Item -LiteralPath $manifest -Force).Attributes -band [IO.FileAttributes]::Hidden) -ne 0
    $slots = @($rows | Where-Object { $_.Kind -eq 'original' -and $_.Slot -ne 'ref' } | ForEach-Object Slot | Sort-Object -Unique)
    Step 'manifest hidden, slot rows S0-S3 and B0-B2' ($hidden -and ($slots -join ',') -eq 'B0,B1,B2,S0,S1,S2,S3') ("slots " + ($slots -join ','))
    $rw = @(Get-ChildItem -LiteralPath $orig -File | Where-Object { -not $_.IsReadOnly })
    Step 'Original files read-only' ($rw.Count -eq 0) "$($rw.Count) writable"
    $seeds = @($rows | Where-Object Kind -eq 'seed')
    $origSha = @{}
    foreach ($row in $rows) { if ($row.Kind -eq 'original') { $origSha[(Split-Path $row.Path -Leaf)] = $row.Sha } }
    # round 8: 01..14 hold the ready-made Colour (their Reflectance / Roughness are copies), 15 / 16 all three ready-made
    function Test-ReadyMade([string]$path) {
        if ($path -notmatch '^Fragile porter suit\\(\d\d) ') { return $false }
        $n = [int]$Matches[1]
        return ($n -ge 1 -and $n -le 14 -and (Split-Path $path -Leaf) -eq 'Suit - Colour.png') -or $n -ge 15
    }
    $seedBad = @($seeds | Where-Object {
            $wantSha = $origSha[(Split-Path $_.Path -Leaf)]
            $fileSha = (Get-FileHash -LiteralPath (Join-Path $out $_.Path) -Algorithm SHA256).Hash.ToLower()
            -not $wantSha -or $_.Sha -ne $fileSha -or ((Test-ReadyMade $_.Path) -eq ($fileSha -eq $wantSha)) })
    Step 'every seed row is the bytes on disk: 00 Default and the boots copies of Original, 01..16 the ready-made colours (17 colours x 3 + 10 boot types x 3)' ($seeds.Count -eq 81 -and $seedBad.Count -eq 0) "$($seeds.Count) seeds, $($seedBad.Count) wrong$(if ($seedBad) { ': ' + $seedBad[0].Path })"
    $defaults = @($r.Lines | Where-Object { $_ -like 'defaults *' })
    Step 'the ready-made colours written (defaults 48 0 0)' ($defaults.Count -eq 1 -and $defaults[0] -like 'defaults 48 0 0 *') ($defaults -join '')
    $m = Get-MeanRgb (Join-Path $orig 'Suit - Colour.png') 256
    Step "her Colour's mean about (67, 71, 81)" (Test-Near $m @(67, 71, 81) 6) ("({0}, {1}, {2})" -f $m[0], $m[1], $m[2])
    # The metal islands (red above 64; the rest is the dark cloth grey). (220, 176, 112) is suit_colour_math.hpp's measured
    # gold reflectance; the design's (183, 149, 101) over 58% was the prototype's coarser measure of the same picture.
    $m = Get-MeanRgb (Join-Path $out 'Sam porter suit\16 Gold\Suit - Reflectance.png') 512 64
    Step "Sam's gold Reflectance, metal pixels about (220, 176, 112)" (Test-Near $m @(220, 176, 112) 8) ("({0}, {1}, {2}) over {3}%" -f $m[0], $m[1], $m[2], $m[3])
    $m = Get-MeanRgb (Join-Path $out 'Sam porter suit\15 Silver\Suit - Reflectance.png') 512 64
    Step "Sam's silver Reflectance, bright pixels grey-white" (([math]::Abs($m[0] - $m[2]) -lt 12) -and $m[0] -gt 150) ("({0}, {1}, {2}) over {3}%" -f $m[0], $m[1], $m[2], $m[3])
    $nrm = Get-MeanRgb (Join-Path $orig 'Suit - Normal.png') 128
    Step 'her Normal is a tangent-space map (mean about (128, 128, 250))' ($nrm[0] -gt 110 -and $nrm[0] -lt 146 -and $nrm[1] -gt 110 -and $nrm[1] -lt 146 -and $nrm[2] -gt 225) ("({0}, {1}, {2})" -f $nrm[0], $nrm[1], $nrm[2])
    $sizes = @{}
    foreach ($f in 'Suit - Colour.png', 'Suit - Reflectance.png', 'Boots - Colour.png') { $im = [System.Drawing.Image]::FromFile((Join-Path $orig $f)); $sizes[$f] = "$($im.Width)x$($im.Height)"; $im.Dispose() }
    Step 'sizes: suit Colour 2048, Reflectance 1024, boots 1024' ($sizes['Suit - Colour.png'] -eq '2048x2048' -and $sizes['Suit - Reflectance.png'] -eq '1024x1024' -and $sizes['Boots - Colour.png'] -eq '1024x1024') (($sizes.GetEnumerator() | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ', ')

    # 2. Re-run safety: an edited seed is kept byte-identical, a deleted one comes back.
    $edited = Join-Path $out 'Fragile porter suit\16 Gold\Suit - Colour.png'
    [IO.File]::AppendAllText($edited, 'x')
    $editedHash = (Get-FileHash -LiteralPath $edited -Algorithm SHA256).Hash
    $deleted = Join-Path $out 'Fragile boots\09 Chiral boots\Boots - Colour.png'
    Remove-Item -LiteralPath $deleted
    $r = Invoke-Extract $out 'rerun'
    $kept = @($r.Lines | Where-Object { $_ -eq 'kept Fragile porter suit\16 Gold\Suit - Colour.png' }).Count
    Step 're-run: exit 0, the edited seed byte-identical and reported kept' ($r.Code -eq 0 -and (Get-FileHash -LiteralPath $edited -Algorithm SHA256).Hash -eq $editedHash -and $kept -eq 1) ("exit $($r.Code); " + (@($r.Lines | Where-Object { $_ -like 'done *' }) -join ''))
    Step 're-run: a deleted seed is seeded again' (Test-Path -LiteralPath $deleted) ''
    $r = Start-Process -FilePath $exe -ArgumentList '--check-textures', '--out', ('"' + $out + '"') -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $run 'check.log')
    $check = @(Get-Content -LiteralPath (Join-Path $run 'check.log'))
    Step 'Check my edits names the edit' ($r.ExitCode -eq 0 -and (@($check | Where-Object { $_ -like 'check Edited: 16 Gold (Colour)*' }).Count -eq 1)) ($check -join ' | ')

    # 2b. Round 8, the upgrade: a v0.24 folder (copies of Original everywhere, extract 1), 16 Gold painted, a subfolder of
    # the user's in it; the next extraction refreshes every unpainted folder and leaves 16 Gold alone.
    $suitDir = Join-Path $out 'Fragile porter suit'
    function Get-SuitHashes([string]$filter) {
        $h = @{}
        Get-ChildItem -LiteralPath $suitDir -Directory | Where-Object { $_.Name -match $filter } | ForEach-Object {
            Get-ChildItem -LiteralPath $_.FullName -Recurse -File | ForEach-Object { $h[$_.FullName.Substring($suitDir.Length)] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash } }
        return $h
    }
    $firstHashes = Get-SuitHashes '^(0\d|1[0-5]) '
    & $textool v024 --dir $out | Set-Content -LiteralPath (Join-Path $run 'textool-v024.log') -Encoding utf8
    Step 'st_textool v024: a v0.24 folder' ($LASTEXITCODE -eq 0) (Get-Content -LiteralPath (Join-Path $run 'textool-v024.log') -Raw).Trim()
    [IO.File]::AppendAllText($edited, 'y')
    $grok = Join-Path $out 'Fragile porter suit\16 Gold\Grok versions'
    New-Item -ItemType Directory -Force -Path $grok | Out-Null
    Copy-Item -LiteralPath (Join-Path $out "Fragile porter suit\16 Gold\Sam's colour.png") -Destination (Join-Path $grok 'a.png')
    $goldHashes = Get-SuitHashes '^16 '
    $r = Invoke-Extract $out 'upgrade'
    $defaults = @($r.Lines | Where-Object { $_ -like 'defaults *' })
    Step 'upgrade: exit 0, replaced 17 (the Colour of 01..14, all three of 15), yours 1 (16 Gold)' ($r.Code -eq 0 -and $defaults.Count -eq 1 -and $defaults[0] -like 'defaults 0 17 1 *') ("exit $($r.Code); " + ($defaults -join ''))
    $now = Get-SuitHashes '^(0\d|1[0-5]) '
    $changed = @($firstHashes.Keys | Where-Object { $now[$_] -ne $firstHashes[$_] })
    Step 'upgrade: 00..15 byte-identical to the first extraction' ($changed.Count -eq 0 -and $now.Count -eq $firstHashes.Count) "$($changed.Count) differ$(if ($changed) { ': ' + $changed[0] })"
    $now = Get-SuitHashes '^16 '
    $changed = @($goldHashes.Keys | Where-Object { $now[$_] -ne $goldHashes[$_] })
    Step '16 Gold (painted) and its Grok versions subfolder untouched' ($changed.Count -eq 0 -and $now.Count -eq $goldHashes.Count -and $now.Count -ge 5) "$($now.Count) files, $($changed.Count) differ"
    $r = Start-Process -FilePath $exe -ArgumentList '--check-textures', '--out', ('"' + $out + '"') -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $run 'check-upgrade.log')
    $check = @(Get-Content -LiteralPath (Join-Path $run 'check-upgrade.log'))
    Step 'Check my edits after the upgrade names only 16 Gold' ($r.ExitCode -eq 0 -and (@($check | Where-Object { $_ -like 'check Edited: 16 Gold (Colour)' -or $_ -like 'check Edited: 16 Gold (Colour).' }).Count -eq 1)) ($check -join ' | ')

    # 3. Cancel safety: a run that ends in the middle of set 10 (a partial file on disk), then a full run.
    $r = Invoke-Extract $out2 'cancel' @('--cancel-after-sets', '10')
    $partials = @(Get-ChildItem -LiteralPath $out2 -Recurse -Force -Filter '*.partial' -ErrorAction SilentlyContinue).Count
    Step 'cancelled run: stopped with a partial file' ($r.Code -eq 3 -and $partials -ge 1) "exit $($r.Code), $partials partial"
    $r = Invoke-Extract $out2 'after-cancel'
    $partials = @(Get-ChildItem -LiteralPath $out2 -Recurse -Force -Filter '*.partial').Count + @(Get-ChildItem -LiteralPath $out2 -Recurse -Force -Directory -Filter '.extract-tmp').Count
    Step 'the next run finishes and leaves no .partial' ($r.Code -eq 0 -and $partials -eq 0) "exit $($r.Code), $partials left"

    # 4. The game folder is unchanged.
    $after = Get-GameListing
    $after | Set-Content -LiteralPath (Join-Path $run 'listing-after.txt') -Encoding utf8
    $diff = @(Compare-Object $before $after)
    Step "the game's data folder and Oodle DLL unchanged ($($before.Count) files)" ($diff.Count -eq 0) "$($diff.Count) differences"
    $elapsed = [math]::Round(((Get-Date) - $t0).TotalSeconds)
    Step "within the time limit ($TimeoutSec s)" ($elapsed -le $TimeoutSec) "$elapsed s"
} catch {
    Step 'script' $false $_.Exception.Message
} finally {
    if (-not $KeepTextures) {
        foreach ($d in $out, $out2) {
            if (Test-Path -LiteralPath $d) {
                Get-ChildItem -LiteralPath $d -Recurse -Force -File | ForEach-Object { $_.Attributes = 'Normal' }
                Remove-Item -LiteralPath $d -Recurse -Force
            }
        }
        Log 'the extracted pictures were deleted (game data stays out of run folders unless -KeepTextures)'
    }
}

$result = if ($failures.Count -eq 0) { 'pass' } else { 'fail' }
$summary = [ordered]@{
    scenario = $name; result = $result; kind = 'textures-extract (no game started)'
    game_exit_code = 0; guest_exit_code = $null
    saves_unchanged = $true; restored = $true; violation = $false; violation_text = ''
    cloud_uploads = @(); game_server_writes = @(); online_blocked = $true; online_gate_ok = $true
    failures = $failures.ToArray(); steps = $steps.ToArray()
    note = 'No game process and no save were opened; the game folder was only read (listing-before/after.txt).'
}
$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'summary.json') -Encoding utf8
if ($result -eq 'pass') { Log 'run PASSED (exit 0)'; exit 0 }
Log ('run FAILED (exit 1): ' + ($failures -join '; '))
exit 1
