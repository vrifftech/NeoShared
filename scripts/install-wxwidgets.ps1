[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot,

    [string]$Triplet = 'x64-windows-static',

    [string]$LogDirectory,

    [switch]$CleanAfterBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        $Path = Join-Path (Get-Location) $Path
    }

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Copy-VcpkgFailureLogs {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )

    if (-not (Test-Path -LiteralPath $SourceRoot)) {
        Write-Warning "No wxWidgets build tree was created at: $SourceRoot"
        return
    }

    $CopiedRoot = Join-Path $DestinationRoot 'buildtrees-wxwidgets'
    New-Item -ItemType Directory -Force -Path $CopiedRoot | Out-Null
    Copy-Item -Path (Join-Path $SourceRoot '*') -Destination $CopiedRoot -Recurse -Force -ErrorAction SilentlyContinue

    $Logs = Get-ChildItem -LiteralPath $SourceRoot -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -in @('.log', '.txt') } |
        Sort-Object LastWriteTime -Descending

    if (-not $Logs) {
        Write-Warning "No vcpkg wxWidgets log files were found under: $SourceRoot"
        return
    }

    Write-Host ''
    Write-Host '========== wxWidgets vcpkg failure logs =========='
    foreach ($Log in ($Logs | Select-Object -First 12)) {
        Write-Host ''
        Write-Host "----- $($Log.FullName) -----"
        Get-Content -LiteralPath $Log.FullName -Tail 160 -ErrorAction SilentlyContinue
    }
    Write-Host '=================================================='
}

$NeoSharedRoot = Split-Path -Parent $PSScriptRoot
$OverlayPorts = Join-Path $NeoSharedRoot 'vcpkg-ports'
$WxPort = Join-Path $OverlayPorts 'wxwidgets\vcpkg.json'

if (-not (Test-Path -LiteralPath $WxPort)) {
    throw "The pinned wxWidgets overlay was not found at: $WxPort"
}

$PortMetadata = Get-Content -Raw -LiteralPath $WxPort | ConvertFrom-Json
if ($PortMetadata.name -ne 'wxwidgets' -or $PortMetadata.version -ne '3.3.3') {
    throw 'The NeoShared wxWidgets overlay must declare wxwidgets 3.3.3.'
}

$VcpkgRoot = Resolve-FullPath $VcpkgRoot
$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $VcpkgExe)) {
    throw "vcpkg.exe was not found at: $VcpkgExe"
}

if ([string]::IsNullOrWhiteSpace($LogDirectory)) {
    if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_WORKSPACE)) {
        $LogDirectory = Join-Path $env:GITHUB_WORKSPACE '.ci-logs\vcpkg-wxwidgets'
    }
    else {
        $LogDirectory = Join-Path $NeoSharedRoot '.ci-logs\vcpkg-wxwidgets'
    }
}
$LogDirectory = Resolve-FullPath $LogDirectory
New-Item -ItemType Directory -Force -Path $LogDirectory | Out-Null

$InvocationLog = Join-Path $LogDirectory 'vcpkg-install.log'
$BuildTree = Join-Path $VcpkgRoot 'buildtrees\wxwidgets'

Write-Host "NeoShared root: $NeoSharedRoot"
Write-Host "vcpkg root: $VcpkgRoot"
Write-Host "wxWidgets overlay: $OverlayPorts"
Write-Host "Triplet: $Triplet"
Write-Host "Failure-log directory: $LogDirectory"

& $VcpkgExe version
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg could not report its version (exit code $LASTEXITCODE)."
}

if (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git')) {
    $VcpkgCommit = (& git -C $VcpkgRoot rev-parse HEAD 2>$null | Out-String).Trim()
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($VcpkgCommit)) {
        Write-Host "vcpkg scripts commit: $VcpkgCommit"
    }
}

$Arguments = @(
    'install',
    "wxwidgets:$Triplet",
    "--overlay-ports=$OverlayPorts"
)

Write-Host "Running: $VcpkgExe $($Arguments -join ' ')"
& $VcpkgExe @Arguments 2>&1 | Tee-Object -FilePath $InvocationLog
$InstallExitCode = $LASTEXITCODE

if ($InstallExitCode -ne 0) {
    Copy-VcpkgFailureLogs -SourceRoot $BuildTree -DestinationRoot $LogDirectory
    throw "vcpkg failed to install wxWidgets 3.3.3 for $Triplet (exit code $InstallExitCode). Complete logs: $LogDirectory"
}

$Installed = (& $VcpkgExe list "wxwidgets:$Triplet" 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw 'vcpkg could not verify the installed wxWidgets package.'
}

$EscapedTriplet = [regex]::Escape($Triplet)
if ($Installed -notmatch "(?m)^wxwidgets:$EscapedTriplet\s+3\.3\.3(?:#\d+)?\b") {
    throw "wxWidgets 3.3.3 was not installed for $Triplet. vcpkg reported:`n$Installed"
}

if ($CleanAfterBuild) {
    Remove-Item -LiteralPath $BuildTree -Recurse -Force -ErrorAction SilentlyContinue

    $PackageDirectoryName = 'wxwidgets_' + ($Triplet -replace '[^A-Za-z0-9_.-]', '_')
    $PackageDirectory = Join-Path (Join-Path $VcpkgRoot 'packages') $PackageDirectoryName
    Remove-Item -LiteralPath $PackageDirectory -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "Verified wxWidgets 3.3.3 for $Triplet from $OverlayPorts"
