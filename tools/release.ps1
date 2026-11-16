<#
.SYNOPSIS
  Makes a Stranded Together release: bumps the version, builds and tests a fresh
  Release build, packages the zip, commits, tags, pushes and publishes it on
  GitHub, where every launcher's updater finds it. See docs/releasing.md.

.DESCRIPTION
  Steps (each printed as "== <step>"):
    preflight  git repository with commits, on -Branch (main), nothing changed or
               untracked; -Remote/-Branch fetched and HEAD not behind it; gh signed in
               (gh auth status); cmake and ctest on PATH; the new tag exists neither
               locally, on the remote nor as a GitHub release.
    version    the new version from CMakeLists.txt's project(stranded_together VERSION x.y.z):
               patch + 1 by default, -Minor, -Major, or -Version x.y.z (not lower than the
               current one; equal = release the current version without a bump).
    bump       that one line of CMakeLists.txt is rewritten (nothing else in the file).
    commit     git commit -m "Release vX.Y.Z" (CMakeLists.txt only). The commit comes before
               the build so the binaries' build id (X.Y.Z+<hash>) and package.json's
               source_commit name the release commit itself. Skipped with -DryRun.
    build      a fresh -BuildDir (default build-release, deleted first): cmake configure
               (-G -Generator -A x64) and cmake --build --config Release (every target).
    test       ctest -C Release --output-on-failure: every test must pass.
    package    tools/package.ps1 -BuildDir <it> -NoBuild -SkipTests -OutputRoot -OutputRoot
               (it zips, checks every file against package.json and runs the launcher's own
               zip check); then package.json's version (and, without -DryRun, its
               source_commit = HEAD and source_dirty = false) are checked here too.
    notes      -Notes, or the commit subjects since the last v* tag, plus how to install
               and the zip's SHA-256: <OutputRoot>\release-notes-X.Y.Z.md.
    tag        git tag -a vX.Y.Z.                                      Skipped with -DryRun.
    push       git push --atomic <Remote> <Branch> vX.Y.Z.             Skipped with -DryRun.
    release    gh release create vX.Y.Z <zip> --verify-tag --title --notes-file
               (--prerelease with -Prerelease: the launchers then ignore it), then
               gh release view checks that exactly one StrandedTogether-X.Y.Z.zip of the
               local size is attached.                                  Skipped with -DryRun.

  A failure before the push undoes the release commit and the tag (nothing was pushed;
  CMakeLists.txt is back as it was). -DryRun runs everything except commit, tag, push
  and release, then puts CMakeLists.txt back: the working tree and HEAD end as they
  started (checked), and the zip and notes stay in -OutputRoot for a look.

  Works in PowerShell 7 and Windows PowerShell 5.1.

.EXAMPLE
  .\tools\release.ps1 -DryRun                 # 0.1.0 -> 0.1.1, everything but commit/tag/push/release
.EXAMPLE
  .\tools\release.ps1                         # patch release
.EXAMPLE
  .\tools\release.ps1 -Minor -Notes "Partner animations; cargo on the partner's back."
.EXAMPLE
  .\tools\release.ps1 -Version 1.0.0
#>
[CmdletBinding()]
param(
    [switch]$Minor,
    [switch]$Major,
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')][string]$Version,
    # The release notes' "Changes" text instead of the commit subjects since the last tag.
    [string]$Notes,
    [switch]$DryRun,
    # Publish as a GitHub prerelease (launchers do not offer prereleases).
    [switch]$Prerelease,
    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repo = 'TrollTaylor/stranded-together',
    [string]$Remote = 'origin',
    [string]$Branch = 'main',
    [string]$BuildDir = 'build-release',
    [string]$OutputRoot = 'dist',
    [string]$Generator = 'Visual Studio 17 2022'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$root = Split-Path -Parent $PSScriptRoot
$cmakeLists = Join-Path $root 'CMakeLists.txt'
$projectLine = '(?m)^(project\(stranded_together\s+VERSION\s+)([0-9]+)\.([0-9]+)\.([0-9]+)(?=[\s)])'

function Resolve-Under([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path)) { $Path = Join-Path $root $Path }
    return [IO.Path]::GetFullPath($Path)
}
$BuildDir = Resolve-Under $BuildDir
$OutputRoot = Resolve-Under $OutputRoot

# ---- Helpers ----------------------------------------------------------------------------
function Invoke-Tool([string]$Exe, [string[]]$ToolArgs, [switch]$AllowFail, [switch]$Echo) {
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'   # stderr lines are output, not errors (Windows PowerShell 5.1)
    try {
        $lines = @(& $Exe @ToolArgs 2>&1 | ForEach-Object {
                $l = "$_"
                if ($Echo) { Write-Host "   | $l" }
                $l
            })
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $eap }
    if ($null -eq $code) { $code = 0 }
    if ($code -ne 0 -and -not $AllowFail) {
        $tail = ($lines | Select-Object -Last 15) -join "`n"
        throw "$Exe $($ToolArgs -join ' ') failed (exit $code)`n$tail"
    }
    return [pscustomobject]@{ Code = $code; Lines = $lines; Text = (($lines -join "`n").Trim()) }
}
function Invoke-Git([string[]]$GitArgs, [switch]$AllowFail) { Invoke-Tool 'git' (@('-C', $root) + $GitArgs) -AllowFail:$AllowFail }

$script:step = ''
function Step([string]$Name) {
    $script:step = $Name
    Write-Host ""
    Write-Host "== $Name"
}
function Note([string]$Text) { Write-Host "   $Text" }

function Get-ProjectVersion {
    $text = [IO.File]::ReadAllText($cmakeLists)
    $m = [regex]::Matches($text, $projectLine)
    if ($m.Count -ne 1) { throw "CMakeLists.txt must have exactly one 'project(stranded_together VERSION x.y.z ...)' line (found $($m.Count))." }
    return [version]("{0}.{1}.{2}" -f $m[0].Groups[2].Value, $m[0].Groups[3].Value, $m[0].Groups[4].Value)
}
function Format-Version([version]$V) { return "$($V.Major).$($V.Minor).$($V.Build)" }

# ---- State for the undo -----------------------------------------------------------------
$state = @{
    OriginalCMake = $null   # the bytes before the bump
    Bumped        = $false
    Committed     = $false
    Tagged        = $false
    Pushed        = $false
    Released      = $false
    HeadBefore    = $null
    Tag           = $null
}
$failed = $null
try {
    # ---- preflight ------------------------------------------------------------------------
    Step 'preflight'
    if (([int][bool]$Minor + [int][bool]$Major + [int][bool]$Version) -gt 1) { throw 'Use only one of -Minor, -Major and -Version.' }
    foreach ($tool in @('git', 'gh', 'cmake', 'ctest')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is not on PATH." }
    }
    $top = (Invoke-Git @('rev-parse', '--show-toplevel')).Text
    if ([IO.Path]::GetFullPath($top) -ne [IO.Path]::GetFullPath($root)) { throw "$root is not the top of a git repository ($top is)." }
    if ((Invoke-Git @('rev-parse', '--verify', '--quiet', 'HEAD') -AllowFail).Code -ne 0) { throw 'The repository has no commits yet.' }
    $state.HeadBefore = (Invoke-Git @('rev-parse', 'HEAD')).Text
    $current = (Invoke-Git @('symbolic-ref', '--quiet', '--short', 'HEAD') -AllowFail).Text
    if ($current -ne $Branch) { throw "Releases are made from $Branch; this checkout is on '$current'." }
    $dirty = @((Invoke-Git @('status', '--porcelain', '--untracked-files=all')).Lines | Where-Object { $_ })
    if ($dirty.Count) { throw "The working tree is not clean ($($dirty.Count) change(s), e.g. '$($dirty[0])'). Commit or stash first." }
    if ((Invoke-Git @('remote', 'get-url', $Remote) -AllowFail).Code -ne 0) { throw "There is no git remote '$Remote'." }
    Note "fetching $Remote"
    $null = Invoke-Git @('fetch', '--quiet', '--tags', $Remote)
    $remoteRef = "refs/remotes/$Remote/$Branch"
    if ((Invoke-Git @('rev-parse', '--verify', '--quiet', $remoteRef) -AllowFail).Code -eq 0) {
        $behind = [int](Invoke-Git @('rev-list', '--count', "HEAD..$remoteRef")).Text
        if ($behind -gt 0) { throw "$Branch is $behind commit(s) behind $Remote/${Branch}: pull first." }
        $ahead = [int](Invoke-Git @('rev-list', '--count', "$remoteRef..HEAD")).Text
        Note "$Branch is $ahead commit(s) ahead of $Remote/$Branch"
    } else {
        Note "$Remote has no $Branch yet: the push creates it"
    }
    $auth = Invoke-Tool 'gh' @('auth', 'status') -AllowFail
    if ($auth.Code -ne 0) { throw 'gh is not signed in (gh auth status failed): run gh auth login.' }
    Note 'gh is signed in'

    # ---- version ----------------------------------------------------------------------------
    Step 'version'
    $old = Get-ProjectVersion
    if ($Version) { $new = [version]$Version }
    elseif ($Major) { $new = [version]::new($old.Major + 1, 0, 0) }
    elseif ($Minor) { $new = [version]::new($old.Major, $old.Minor + 1, 0) }
    else { $new = [version]::new($old.Major, $old.Minor, $old.Build + 1) }
    if ($new -lt $old) { throw "-Version $(Format-Version $new) is lower than the current $(Format-Version $old)." }
    $ver = Format-Version $new
    $tag = "v$ver"
    $state.Tag = $tag
    if ((Invoke-Git @('rev-parse', '--verify', '--quiet', "refs/tags/$tag") -AllowFail).Code -eq 0) { throw "The tag $tag exists already." }
    if ((Invoke-Git @('ls-remote', '--tags', $Remote, "refs/tags/$tag")).Text) { throw "The tag $tag exists on $Remote already." }
    $view = Invoke-Tool 'gh' @('release', 'view', $tag, '--repo', $Repo, '--json', 'tagName') -AllowFail
    if ($view.Code -eq 0) { throw "GitHub already has a release $tag in $Repo." }
    Note "$(Format-Version $old) -> $ver ($tag)$(if ($DryRun) { ', dry run' })"

    # ---- bump -------------------------------------------------------------------------------
    Step 'bump'
    if ($new -eq $old) {
        Note "CMakeLists.txt is at $ver already: no bump"
    } else {
        $state.OriginalCMake = [IO.File]::ReadAllBytes($cmakeLists)
        $text = [Text.Encoding]::UTF8.GetString($state.OriginalCMake)
        $bom = $state.OriginalCMake.Length -ge 3 -and $state.OriginalCMake[0] -eq 0xEF -and $state.OriginalCMake[1] -eq 0xBB -and $state.OriginalCMake[2] -eq 0xBF
        if ($bom) { $text = $text.Substring(1) }
        $bumped = [regex]::Replace($text, $projectLine, { param($m) $m.Groups[1].Value + $ver })
        [IO.File]::WriteAllText($cmakeLists, $bumped, (New-Object Text.UTF8Encoding($bom)))
        $state.Bumped = $true
        if ((Get-ProjectVersion) -ne $new) { throw 'The bump did not take.' }
        $changed = @((Invoke-Git @('diff', '--numstat', '--', 'CMakeLists.txt')).Lines | Where-Object { $_ })
        if ($changed.Count -ne 1 -or $changed[0] -notmatch '^1\s+1\s') { throw "The bump changed more than the project line: $($changed -join '; ')" }
        Note "CMakeLists.txt: project(stranded_together VERSION $ver ...)"
    }

    # ---- commit -----------------------------------------------------------------------------
    Step 'commit'
    if ($DryRun) {
        Note "dry run: would run git commit -m `"Release $tag`" (CMakeLists.txt)"
    } elseif (-not $state.Bumped) {
        Note 'nothing to commit: the tag goes on HEAD'
    } else {
        $null = Invoke-Git @('add', '--', 'CMakeLists.txt')
        $null = Invoke-Git @('commit', '--quiet', '-m', "Release $tag", '--', 'CMakeLists.txt')
        $state.Committed = $true
        Note "committed $((Invoke-Git @('rev-parse', '--short', 'HEAD')).Text) Release $tag"
    }
    $releaseCommit = (Invoke-Git @('rev-parse', 'HEAD')).Text

    # ---- build ------------------------------------------------------------------------------
    Step 'build'
    if (Test-Path -LiteralPath $BuildDir) {
        $inside = $BuildDir.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)
        $isBuild = Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')
        $empty = @(Get-ChildItem -LiteralPath $BuildDir -Force).Count -eq 0
        if (-not $inside -or -not ($isBuild -or $empty)) { throw "$BuildDir is not a CMake build folder inside the repository: not deleting it." }
        Note "deleting $BuildDir"
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }
    $null = Invoke-Tool 'cmake' @('-S', $root, '-B', $BuildDir, '-G', $Generator, '-A', 'x64') -Echo
    $null = Invoke-Tool 'cmake' @('--build', $BuildDir, '--config', 'Release', '--parallel') -Echo

    # ---- test -------------------------------------------------------------------------------
    Step 'test'
    $ctest = Invoke-Tool 'ctest' @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure') -Echo
    $summary = @($ctest.Lines | Where-Object { $_ -match 'tests passed' }) | Select-Object -Last 1
    Note $summary

    # ---- package ------------------------------------------------------------------------------
    Step 'package'
    $ps = (Get-Process -Id $PID).Path
    $null = Invoke-Tool $ps @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'package.ps1'),
        '-BuildDir', $BuildDir, '-NoBuild', '-SkipTests', '-OutputRoot', $OutputRoot) -Echo
    $zipName = "StrandedTogether-$ver.zip"
    $zip = Join-Path $OutputRoot $zipName
    if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw "tools/package.ps1 did not write $zip." }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $archive.GetEntry('stranded_together/package.json')
        if (-not $entry) { throw "$zipName has no stranded_together/package.json." }
        $reader = New-Object IO.StreamReader($entry.Open())
        try { $pkg = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
    } finally { $archive.Dispose() }
    if ([string]$pkg.version -ne $ver) { throw "package.json says version $($pkg.version), not $ver." }
    if (-not $DryRun) {
        if ([string]$pkg.source_commit -ne $releaseCommit) { throw "package.json's source_commit $($pkg.source_commit) is not the release commit $releaseCommit." }
        if ($pkg.source_dirty -ne $false) { throw "package.json says the source tree was not clean (source_dirty $($pkg.source_dirty))." }
    }
    $zipSha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    $zipSize = (Get-Item -LiteralPath $zip).Length
    Note "$zipName  $zipSize bytes  SHA-256 $zipSha  build $($pkg.build_id)"

    # ---- notes ------------------------------------------------------------------------------
    Step 'notes'
    $lastTag = (Invoke-Git @('describe', '--tags', '--abbrev=0', '--match', 'v[0-9]*', 'HEAD') -AllowFail)
    $range = if ($lastTag.Code -eq 0 -and $lastTag.Text) { "$($lastTag.Text)..HEAD" } else { 'HEAD' }
    if ($Notes) {
        $changes = $Notes.Trim()
    } else {
        $subjects = @((Invoke-Git @('log', '--no-merges', '--format=%s (%h)', $range)).Lines | Where-Object { $_ -and $_ -notmatch '^Release v[0-9]+\.[0-9]+\.[0-9]+ \(' })
        $changes = if ($subjects.Count) { ($subjects | ForEach-Object { "- $_" }) -join "`n" } else { '- Maintenance release.' }
    }
    $since = if ($lastTag.Code -eq 0 -and $lastTag.Text) { " since $($lastTag.Text)" } else { '' }
    $body = @(
        "## Changes$since", '', $changes, '',
        '## Install or update', '',
        "- **Update:** open StrandedTogether.exe; it offers **Update now** (or About > Check for updates).",
        "- **New install:** download ``$zipName`` below and extract everything into the DEATH STRANDING DIRECTOR'S CUT folder (the one with ``ds.exe``), then run ``StrandedTogether.exe``.",
        '- Both players need the same version.', '',
        "``$zipName`` SHA-256: ``$zipSha``"
    ) -join "`n"
    $notesPath = Join-Path $OutputRoot "release-notes-$ver.md"
    [IO.File]::WriteAllText($notesPath, $body + "`n", (New-Object Text.UTF8Encoding($false)))
    Note "$notesPath ($range)"
    $body -split "`n" | ForEach-Object { Write-Host "   > $_" }

    $title = "Stranded Together $ver"
    $ghCreate = @('release', 'create', $tag, $zip, '--repo', $Repo, '--verify-tag', '--title', $title, '--notes-file', $notesPath)
    if ($Prerelease) { $ghCreate += '--prerelease' }
    if ($DryRun) {
        Step 'tag / push / release (dry run: skipped)'
        Note "would run: git tag -a $tag -m `"$title`""
        Note "would run: git push --atomic $Remote refs/heads/${Branch} refs/tags/$tag"
        Note "would run: gh $($ghCreate -join ' ')"
    } else {
        # ---- tag ------------------------------------------------------------------------------
        Step 'tag'
        $null = Invoke-Git @('tag', '-a', $tag, '-m', $title, $releaseCommit)
        $state.Tagged = $true
        Note "$tag -> $($releaseCommit.Substring(0, 8))"

        # ---- push -----------------------------------------------------------------------------
        Step 'push'
        $null = Invoke-Tool 'git' @('-C', $root, 'push', '--atomic', $Remote, "refs/heads/${Branch}:refs/heads/${Branch}", "refs/tags/${tag}:refs/tags/${tag}") -Echo
        $state.Pushed = $true

        # ---- release --------------------------------------------------------------------------
        Step 'release'
        $null = Invoke-Tool 'gh' $ghCreate -Echo
        $state.Released = $true
        $view = Invoke-Tool 'gh' @('release', 'view', $tag, '--repo', $Repo, '--json', 'tagName,isDraft,isPrerelease,assets,url')
        $rel = $view.Text | ConvertFrom-Json
        $assets = @($rel.assets)
        $mine = @($assets | Where-Object { [string]$_.name -ceq $zipName })
        $others = @($assets | Where-Object { [string]$_.name -like 'StrandedTogether-*.zip' -and [string]$_.name -cne $zipName })
        if ($mine.Count -ne 1) { throw "The release $tag does not carry $zipName." }
        if ([int64]$mine[0].size -ne $zipSize) { throw "$zipName on GitHub is $($mine[0].size) bytes, the local file $zipSize." }
        if ($others.Count) { throw "The release $tag also carries $($others[0].name), which looks like a release zip." }
        if ($rel.isDraft) { throw "The release $tag is still a draft: launchers do not see it." }
        Note "$($rel.url) ($(if ($rel.isPrerelease) { 'prerelease: launchers do not offer it' } else { 'launchers offer it now' }))"
    }
} catch {
    $failed = $_
} finally {
    # Undo what was not pushed: the tag, the release commit, the bump.
    if (-not $state.Pushed) {
        if ($state.Tagged) { $null = Invoke-Git @('tag', '-d', $state.Tag) -AllowFail }
        if ($state.Committed) {
            $null = Invoke-Git @('reset', '--quiet', '--soft', 'HEAD~1') -AllowFail
            $null = Invoke-Git @('restore', '--staged', '--', 'CMakeLists.txt') -AllowFail
        }
        if ($state.Bumped -and $state.OriginalCMake) { [IO.File]::WriteAllBytes($cmakeLists, $state.OriginalCMake) }
        if ($state.HeadBefore) {
            $head = (Invoke-Git @('rev-parse', 'HEAD') -AllowFail).Text
            $left = @((Invoke-Git @('status', '--porcelain', '--untracked-files=all') -AllowFail).Lines | Where-Object { $_ })
            if ($head -ne $state.HeadBefore -or $left.Count) {
                Write-Warning "The checkout did not end where it started (HEAD $head, $($left.Count) change(s)); look at git status."
                if (-not $failed -and $DryRun) { $failed = 'the dry run left the checkout changed' }
            } elseif ($state.Committed) {
                Write-Host "   (the release commit was undone and CMakeLists.txt put back: the checkout is as it was)"
            } elseif ($state.Bumped) {
                Write-Host "   (CMakeLists.txt was put back: the checkout is as it was)"
            }
        }
    }
}

Write-Host ""
if ($failed) {
    $msg = if ($failed -is [Management.Automation.ErrorRecord]) { $failed.Exception.Message } else { [string]$failed }
    Write-Host "Release FAILED at '$script:step': $msg" -ForegroundColor Red
    if ($state.Pushed -and -not $state.Released) {
        Write-Host "The commit and the tag $($state.Tag) are pushed, but the GitHub release is not made. Finish it with:"
        Write-Host "  gh $($ghCreate -join ' ')"
    } elseif ($state.Released) {
        Write-Host "The release $($state.Tag) is published; check it on GitHub."
    } else {
        Write-Host 'Nothing was pushed or published.'
    }
    exit 1
}
if ($DryRun) {
    Write-Host "Dry run of $tag passed: built, tested and packaged; nothing committed, tagged, pushed or published."
    Write-Host "  zip   $zip"
    Write-Host "  notes $notesPath"
} else {
    Write-Host "Released $tag."
    Write-Host "  zip   $zip"
}
exit 0
