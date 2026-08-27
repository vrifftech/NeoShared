[CmdletBinding(PositionalBinding=$false)]
param(
    [string]$BuildDir = '',
    [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')][string]$BuildType = 'Release',
    [ValidateSet('ON','OFF')][string]$MinimalRelease = 'OFF',
    [int]$Parallel = 0,
    [string]$Target = '',
    [string]$Generator = '',
    [string]$Platform = '',
    [switch]$Clean,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$ExtraCMakeArgs
)

$ErrorActionPreference = 'Stop'
$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RootDir 'build'
} elseif (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir = Join-Path $RootDir $BuildDir
}
$BuildDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDir)

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    $RootPath = [System.IO.Path]::GetFullPath($RootDir).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $BuildPath = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd([System.IO.Path]::DirectorySeparatorChar)
    $Prefix = $RootPath + [System.IO.Path]::DirectorySeparatorChar
    if (-not $BuildPath.StartsWith($Prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to clean a build directory outside the repository: $BuildPath"
    }
    Remove-Item -Recurse -Force -LiteralPath $BuildPath
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$cmakeArgs = @()
if (-not [string]::IsNullOrWhiteSpace($Generator)) {
    $cmakeArgs += @('-G', $Generator)
    if (-not [string]::IsNullOrWhiteSpace($Platform)) {
        $cmakeArgs += @('-A', $Platform)
    }
}
$cmakeArgs += @(
    '-S', $RootDir,
    '-B', $BuildDir,
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DNEO_MINIMAL_RELEASE=$MinimalRelease"
)
if ($ExtraCMakeArgs) { $cmakeArgs += $ExtraCMakeArgs }

& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$buildArgs = @('--build', $BuildDir, '--config', $BuildType)
if ($Parallel -gt 0) { $buildArgs += @('--parallel', [string]$Parallel) }
if (-not [string]::IsNullOrWhiteSpace($Target)) { $buildArgs += @('--target', $Target) }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
