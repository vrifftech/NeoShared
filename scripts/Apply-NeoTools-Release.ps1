[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Patch,
    [string]$NeoToolsRoot = (Get-Location).Path
)

$ErrorActionPreference = 'Stop'
$patchPath = (Resolve-Path -LiteralPath $Patch).Path
$rootPath = (Resolve-Path -LiteralPath $NeoToolsRoot).Path
$env:GIT_AUTHOR_NAME = 'vrifftech'
$env:GIT_AUTHOR_EMAIL = '74007411+vrifftech@users.noreply.github.com'
$env:GIT_COMMITTER_NAME = $env:GIT_AUTHOR_NAME
$env:GIT_COMMITTER_EMAIL = $env:GIT_AUTHOR_EMAIL

function Invoke-NeoGit {
    & git.exe @args
    if ($LASTEXITCODE -ne 0) { throw "Git failed: $args" }
}

$repos = Get-Content -LiteralPath $patchPath -Encoding UTF8 |
    ForEach-Object {
        if ($_ -match '^\+\+\+ b/([^/]+)/(.+)$') {
            [pscustomobject]@{ Repo = $Matches[1]; File = $Matches[2] }
        }
    } |
    Group-Object Repo |
    Sort-Object @{ Expression = {
        if ($_.Name -eq 'neoshared') { 0 } elseif ($_.Name -eq 'NeoToolKit-Test') { 2 } else { 1 }
    } }, Name

foreach ($repo in $repos) {
    $repoPath = Join-Path $rootPath $repo.Name
    Invoke-NeoGit -C $repoPath switch main
    Invoke-NeoGit -C $repoPath pull --ff-only origin main
}

Invoke-NeoGit -C $rootPath apply --ignore-space-change $patchPath

foreach ($repo in $repos) {
    $repoPath = Join-Path $rootPath $repo.Name
    $paths = @($repo.Group.File | Sort-Object -Unique)
    Invoke-NeoGit -C $repoPath add -- @paths
    Invoke-NeoGit -C $repoPath commit --only -m 'new release method + version bump' -- @paths
    Invoke-NeoGit -C $repoPath push origin main
}
