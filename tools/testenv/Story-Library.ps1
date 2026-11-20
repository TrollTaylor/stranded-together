<#
.SYNOPSIS
  The story library (docs/dev-tools.md "Story tools"): single saves at known story points, kept in
  testenv-saves\story\ (git-ignored), packed into one seed folder (testenv-saves\story-pack) that hidden runs
  (-SeedSaves story-pack) and the launcher's Local test (Debug > Test saves > Choose folder) load from.

.EXAMPLE
  pwsh -File tools\testenv\Story-Library.ps1 -List
  pwsh -File tools\testenv\Story-Library.ps1 -Add -Source "$env:LOCALAPPDATA\KojimaProductions\DeathStrandingDC\<account>\manualsave3" -Label ep2-order05
  pwsh -File tools\testenv\Story-Library.ps1 -Add -FromRun build-testenv\runs\<run> -Label ep3-start
  pwsh -File tools\testenv\Story-Library.ps1 -Add -FromSandbox local-host -Label ep3-start
  pwsh -File tools\testenv\Story-Library.ps1 -Pack
  pwsh -File tools\testenv\Start-TestRun.ps1 -Scenario story-points -SeedSaves story-pack
  pwsh -File tools\testenv\Story-Library.ps1 -Probe build-testenv\runs\<that run>

.NOTES
  Every source is only read (opened read-only, closed at once): your own saves, a hidden run's
  saves-scratch-changed\ (what the game wrote into the run's scratch copy), a Local test sandbox in the game folder.
  The tool writes only below testenv-saves\story and testenv-saves\story-pack. story.json never holds a path, an
  account folder or an id: the source is named by its kind and slot ("own saves: manualsave3").
  Entry NN (00..09) is manualsave(10+NN) in the pack; the mod's story_load / the Dev tools page's Story section load it.
#>
[CmdletBinding(DefaultParameterSetName = 'List')]
param(
    [Parameter(ParameterSetName = 'List')][switch]$List,
    [Parameter(ParameterSetName = 'Add', Mandatory)][switch]$Add,
    [Parameter(ParameterSetName = 'Add')][string]$Source,
    [Parameter(ParameterSetName = 'Add')][string]$FromRun,
    [Parameter(ParameterSetName = 'Add')][string]$FromSandbox,
    [Parameter(ParameterSetName = 'Add', Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,39}$')][string]$Label,
    [Parameter(ParameterSetName = 'Add')][string]$GameDir,
    [Parameter(ParameterSetName = 'Pack', Mandatory)][switch]$Pack,
    [Parameter(ParameterSetName = 'Probe', Mandatory)][string]$Probe
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$seeds = Join-Path $repo 'testenv-saves'
$lib = Join-Path $seeds 'story'
$packDir = Join-Path $seeds 'story-pack'
$firstSlot = 10
$maxEntries = 10

function Assert-Below([string]$path, [string]$root) {
    $full = [IO.Path]::GetFullPath($path)
    $r = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
    if (-not $full.StartsWith($r, [StringComparison]::OrdinalIgnoreCase)) { throw "refusing to write outside $root : $full" }
}

function Read-ReadOnly([string]$file) {
    if (Test-Path ($file + 'temp')) { throw "the game is writing this slot ($([IO.Path]::GetFileName($file))temp exists): try again" }
    $fs = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try {
        $ms = New-Object IO.MemoryStream
        $fs.CopyTo($ms)
        return $ms.ToArray()
    } finally { $fs.Dispose() }
}

function Get-Size([byte[]]$b, [int]$at) {
    $n = 0; $i = 0
    while ($i -lt 8 -and $b[$at + $i] -ge 0x30 -and $b[$at + $i] -le 0x39) { $n = $n * 10 + ($b[$at + $i] - 0x30); $i++ }
    return $n
}

function UrlDecode([string]$s) {
    $d = [Uri]::UnescapeDataString(($s -replace '\+', ' '))
    return (($d -split "[`r`n]+") | Where-Object { $_ }) -join ' / '
}

# A checkpoint.dat: three decimal sizes, the [Slot] ini, the PNG, the data (u32 magic 0xC0391E1D or 0xC0391E55).
function Get-SlotInfo([byte[]]$b) {
    if ($b.Length -lt 64) { throw 'not a checkpoint.dat (too small)' }
    $ini = Get-Size $b 0; $png = Get-Size $b 8; $data = Get-Size $b 16
    if ($ini -lt 6 -or 24 + $ini + $png + $data -ne $b.Length) { throw 'not a checkpoint.dat (the size fields do not add up)' }
    $magic = [BitConverter]::ToUInt32($b, 24 + $ini + $png)
    if (('{0:X8}' -f $magic) -notin 'C0391E1D', 'C0391E55') { throw ('not a checkpoint.dat (data magic 0x{0:X8})' -f $magic) }
    $text = [Text.Encoding]::UTF8.GetString($b, 24, $ini)
    if (-not $text.StartsWith('[Slot]')) { throw 'not a checkpoint.dat (no [Slot] header)' }
    $kv = @{}
    foreach ($line in ($text -split "`r?`n")) { if ($line -match '^\s*(\w+)\s*=\s*(.*)$') { $kv[$Matches[1]] = $Matches[2].Trim() } }
    [pscustomobject]@{
        title      = UrlDecode $kv['Title']
        detail     = UrlDecode $kv['Detail']
        user_param = $kv['UserParam']
    }
}

# Where a source came from, without its path: "own saves: manualsave3", "run 20261120-...: autosave4", ...
function Describe-Source([string]$file) {
    $full = [IO.Path]::GetFullPath($file)
    $slot = Split-Path (Split-Path $full -Parent) -Leaf
    $own = @((Join-Path $env:LOCALAPPDATA 'KojimaProductions'), (Join-Path $env:USERPROFILE 'AppData\Local\KojimaProductions'))
    foreach ($o in $own) { if ($full.StartsWith([IO.Path]::GetFullPath($o) + '\', [StringComparison]::OrdinalIgnoreCase)) { return "own saves: $slot" } }
    if ($full -match '\\runs\\([0-9]{8}-[0-9]{6}-[A-Za-z0-9-]+)\\') { return "hidden run $($Matches[1]): $slot" }
    if ($full -match '\\stranded_together\\sandbox\\([A-Za-z0-9_-]+)\\') { return "Local test sandbox $($Matches[1]): $slot" }
    if ($full -match '\\testenv-saves\\([A-Za-z0-9_.-]+)\\') { return "testenv-saves $($Matches[1]): $slot" }
    return $slot
}

function Newest-Checkpoint([string]$dir) {
    $f = Get-ChildItem $dir -Recurse -Filter checkpoint.dat -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if (-not $f) { throw "no checkpoint.dat below $dir" }
    return $f.FullName
}

function Get-Entries {
    if (-not (Test-Path $lib)) { return @() }
    Get-ChildItem $lib -Directory | Where-Object { $_.Name -match '^(\d\d)-' -and (Test-Path (Join-Path $_.FullName 'story.json')) } | Sort-Object Name | ForEach-Object {
        $j = Get-Content (Join-Path $_.FullName 'story.json') -Raw | ConvertFrom-Json
        $j | Add-Member -NotePropertyName dir -NotePropertyValue $_.FullName -Force
        $j
    }
}

function Save-Entry($e) {
    $copy = $e.PSObject.Copy()
    $copy.PSObject.Properties.Remove('dir')
    $json = $copy | ConvertTo-Json -Depth 4
    Assert-Below $e.dir $lib
    [IO.File]::WriteAllText((Join-Path $e.dir 'story.json'), ($json -replace "`r?`n", "`r`n") + "`r`n")
}

switch ($PSCmdlet.ParameterSetName) {
    'Add' {
        $given = @($Source, $FromRun, $FromSandbox | Where-Object { $_ }).Count
        if ($given -ne 1) { throw 'give exactly one of -Source, -FromRun, -FromSandbox' }
        if ($Source) {
            $file = if (Test-Path $Source -PathType Container) { Join-Path $Source 'checkpoint.dat' } else { $Source }
        } elseif ($FromRun) {
            $file = Newest-Checkpoint (Join-Path (Resolve-Path $FromRun).Path 'saves-scratch-changed')
        } else {
            if ($FromSandbox -notmatch '^[A-Za-z0-9_-]{1,40}$') { throw "bad sandbox name: $FromSandbox" }
            if (-not $GameDir) { $GameDir = 'C:\Program Files (x86)\Steam\steamapps\common\DEATH STRANDING DIRECTORS CUT' }
            $file = Newest-Checkpoint (Join-Path $GameDir "stranded_together\sandbox\$FromSandbox\saves\DeathStrandingDC")
        }
        if (-not (Test-Path $file -PathType Leaf)) { throw "not found: $file" }
        $bytes = Read-ReadOnly $file
        $info = Get-SlotInfo $bytes
        $entries = @(Get-Entries)
        if ($entries | Where-Object { $_.label -eq $Label }) { throw "an entry named $Label exists already" }
        $used = @($entries | ForEach-Object { [int]$_.entry })
        $nn = 0..($maxEntries - 1) | Where-Object { $used -notcontains $_ } | Select-Object -First 1
        if ($null -eq $nn) { throw "the library is full ($maxEntries entries: manualsave$firstSlot..$($firstSlot + $maxEntries - 1))" }
        $dir = Join-Path $lib ('{0:D2}-{1}' -f $nn, $Label)
        Assert-Below $dir $lib
        New-Item -ItemType Directory -Force $dir | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $dir 'checkpoint.dat'), $bytes)
        $sha = (Get-FileHash -Algorithm SHA256 (Join-Path $dir 'checkpoint.dat')).Hash.ToLowerInvariant()
        $e = [pscustomobject]@{
            entry      = $nn
            slot       = "manualsave$($firstSlot + $nn)"
            label      = $Label
            title      = $info.title
            detail     = $info.detail
            user_param = $info.user_param
            source     = Describe-Source $file
            sha256     = $sha
            bytes      = $bytes.Length
            added      = (Get-Date -Format 's')
            episode    = $null
            story      = $null
            probed_in  = $null
            dir        = $dir
        }
        Save-Entry $e
        Write-Host ("added {0:D2}-{1} (pack slot manualsave{2}): {3} | {4} [{5}]" -f $nn, $Label, ($firstSlot + $nn), $info.title, $info.detail, $e.source)
        Write-Host 'run -Pack to rebuild testenv-saves\story-pack'
    }
    'Pack' {
        $entries = @(Get-Entries)
        if (-not $entries.Count) { throw 'the library is empty (-Add first)' }
        Assert-Below $packDir $seeds
        if (Test-Path $packDir) { Remove-Item -Recurse -Force -LiteralPath $packDir }
        $index = @("Story pack: the story library's entries as manual slots (Story-Library.ps1 -Pack, $(Get-Date -Format 's')).",
                   'Seed it with Start-TestRun.ps1 -SeedSaves story-pack, or pick this folder as the Local test test save.', '')
        foreach ($e in $entries) {
            $slotDir = Join-Path $packDir "DeathStrandingDC\$($e.slot)"
            New-Item -ItemType Directory -Force $slotDir | Out-Null
            $src = Join-Path $e.dir 'checkpoint.dat'
            if ((Get-FileHash -Algorithm SHA256 $src).Hash.ToLowerInvariant() -ne $e.sha256) { throw "$($e.label): checkpoint.dat differs from its sha256 in story.json" }
            Copy-Item -LiteralPath $src -Destination (Join-Path $slotDir 'checkpoint.dat')
            $ep = if ($null -ne $e.episode) { if ($e.episode -eq 0) { 'Prologue' } else { "Episode $($e.episode)" } } else { 'episode not probed' }
            $index += ('{0}  {1,-24} {2,-18} {3}' -f $e.slot, $e.label, $ep, $e.title)
        }
        [IO.File]::WriteAllText((Join-Path $packDir 'STORY-PACK.txt'), (($index -join "`r`n") + "`r`n"))
        $index | Select-Object -Skip 3 | ForEach-Object { Write-Host $_ }
        Write-Host "packed $($entries.Count) entr$(if ($entries.Count -eq 1) { 'y' } else { 'ies' }) into $packDir"
    }
    'Probe' {
        # A story-points run's log: "[story] after the load of manualsave<N>: <summary>" per entry.
        $runDir = (Resolve-Path $Probe).Path
        $log = Join-Path $runDir 'game\stranded_together.log'
        if (-not (Test-Path $log)) { throw "no game log in $runDir" }
        $found = @{}
        foreach ($m in (Select-String -Path $log -Pattern '\[story\] after the load of (manualsave\d+): ((Prologue|Episode (\d+)|episode unknown).*)$')) {
            $g = $m.Matches[0].Groups
            $ep = if ($g[3].Value -eq 'Prologue') { 0 } elseif ($g[4].Success) { [int]$g[4].Value } else { $null }
            $found[$g[1].Value] = @{ episode = $ep; story = $g[2].Value }
        }
        $runName = Split-Path $runDir -Leaf
        foreach ($e in @(Get-Entries)) {
            if ($found.ContainsKey($e.slot)) {
                $e.episode = $found[$e.slot].episode
                $e.story = $found[$e.slot].story
                $e.probed_in = "hidden run $runName"
                Save-Entry $e
                Write-Host ("{0} {1}: {2}" -f $e.slot, $e.label, $e.story)
            } else {
                Write-Host ("{0} {1}: not loaded in that run" -f $e.slot, $e.label)
            }
        }
    }
    default {
        $entries = @(Get-Entries)
        if (-not $entries.Count) { Write-Host "the story library is empty ($lib)"; break }
        foreach ($e in $entries) {
            $ep = if ($null -ne $e.episode) { if ($e.episode -eq 0) { 'Prologue' } else { "Episode $($e.episode)" } } else { '(not probed)' }
            Write-Host ('{0:D2} {1,-14} {2,-24} {3,-14} {4}' -f [int]$e.entry, $e.slot, $e.label, $ep, $e.title)
            if ($e.story) { Write-Host ('   {0}' -f $e.story) }
            Write-Host ('   from {0}' -f $e.source)
        }
        Write-Host ("pack: {0}" -f $(if (Test-Path $packDir) { $packDir } else { '(none yet: -Pack)' }))
    }
}
