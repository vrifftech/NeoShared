[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$VcpkgRoot,

    [string]$Triplet = 'x64-windows-static',

    [switch]$CleanAfterBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$NeoSharedRoot = Split-Path -Parent $PSScriptRoot
$OverlayPorts = Join-Path $NeoSharedRoot 'vcpkg-ports'
$WxPort = Join-Path $OverlayPorts 'wxwidgets\vcpkg.json'

if (-not (Test-Path -LiteralPath $WxPort)) {
    throw "The pinned wxWidgets overlay was not found at: $WxPort"
}

$PortMetadata = Get-Content -Raw -LiteralPath $WxPort | ConvertFrom-Json
if ($PortMetadata.name -ne 'wxwidgets' -or $PortMetadata.version -ne '3.3.3') {
    throw "The neoshared wxWidgets overlay must declare wxwidgets 3.3.3."
}

if (-not [System.IO.Path]::IsPathRooted($VcpkgRoot)) {
    $VcpkgRoot = Join-Path (Get-Location) $VcpkgRoot
}
$VcpkgRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VcpkgRoot)

$VcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
if (-not (Test-Path -LiteralPath $VcpkgExe)) {
    throw "vcpkg.exe was not found at: $VcpkgExe"
}

$Arguments = @(
    'install',
    "wxwidgets:$Triplet",
    "--overlay-ports=$OverlayPorts"
)
if ($CleanAfterBuild) {
    $Arguments += '--clean-after-build'
}

& $VcpkgExe @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg failed to install wxWidgets 3.3.3 for $Triplet (exit code $LASTEXITCODE)."
}

$Installed = (& $VcpkgExe list "wxwidgets:$Triplet" | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg could not verify the installed wxWidgets package."
}

$EscapedTriplet = [regex]::Escape($Triplet)
if ($Installed -notmatch "(?m)^wxwidgets:$EscapedTriplet\s+3\.3\.3(?:#\d+)?\b") {
    throw "wxWidgets 3.3.3 was not installed for $Triplet. vcpkg reported:`n$Installed"
}

Write-Host "Verified wxWidgets 3.3.3 for $Triplet from $OverlayPorts"
