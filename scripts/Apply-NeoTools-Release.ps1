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
    Sort-Object @{ Expression = { if ($_.Name -eq 'neoshared') { 0 } else { 1 } } }, Name

foreach ($repo in $repos) {
    $repoPath = Join-Path $rootPath $repo.Name
    Invoke-NeoGit -C $repoPath switch main
    Invoke-NeoGit -C $repoPath pull --ff-only origin main
}

Invoke-NeoGit -C $rootPath apply --ignore-space-change $patchPath

$versionFiles = @{
    Neo2DA = 'src/core/Version.hpp'
    NeoBIF = 'src/core/Version.hpp'
    NeoDLG = 'src/core/Version.hpp'
    NeoERF = 'src/core/Version.hpp'
    NeoGFF = 'src/core/Version.hpp'
    NeoGUI = 'CMakeLists.txt'
    NeoJRL = 'src/core/Version.hpp'
    NeoMDL = 'CMakeLists.txt'
    NeoPatcher = 'CMakeLists.txt'
    NeoQST = 'CMakeLists.txt'
    NeoSSF = 'src/core/Version.hpp'
    NeoTLK = 'src/core/Version.hpp'
    NeoTPC = 'CMakeLists.txt'
    'NeoToolKit-Test' = 'CMakeLists.txt'
    NeoLIP = 'CMakeLists.txt'
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($name in $versionFiles.Keys) {
    $file = Join-Path (Join-Path $rootPath $name) $versionFiles[$name]
    $text = [IO.File]::ReadAllText($file)
    $pattern = if ($file.EndsWith('.hpp')) {
        '(?m)^\s*#define\s+\w+_VERSION_STRING\s+"(\d+\.\d+\.\d+)"'
    } else {
        '(?is)\bproject\s*\([^)]*?\bVERSION\s+(\d+\.\d+\.\d+)'
    }
    $match = [regex]::Match($text, $pattern).Groups[1]
    $version = [version]::Parse($match.Value)
    $next = '{0}.{1}.{2}' -f $version.Major, $version.Minor, ($version.Build + 1)
    $text = $text.Remove($match.Index, $match.Length).Insert($match.Index, $next)
    [IO.File]::WriteAllText($file, $text, $utf8)
    Write-Host "$name`: $version -> $next"
}

foreach ($repo in $repos) {
    $repoPath = Join-Path $rootPath $repo.Name
    $paths = @(@($repo.Group.File) + @($versionFiles[$repo.Name]) |
        Where-Object { $_ } | Sort-Object -Unique)
    Invoke-NeoGit -C $repoPath add -- @paths
    Invoke-NeoGit -C $repoPath commit --only -m 'new release method + version bump' -- @paths
    Invoke-NeoGit -C $repoPath push origin main
}
