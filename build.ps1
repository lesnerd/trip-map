# Configures and builds with Ninja inside an MSVC x64 environment.
#   .\build.ps1                  # release build
#   .\build.ps1 -Config debug    # debug build (syncs after every kernel)
#   .\build.ps1 -Run --demo      # build and open the demo map
param(
    [ValidateSet('release', 'debug')][string]$Config = 'release',
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$AppArgs
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

if (-not $env:CUDA_PATH) { $env:CUDA_PATH = [Environment]::GetEnvironmentVariable('CUDA_PATH', 'User') }
if (-not $env:CUDA_PATH) { throw 'CUDA_PATH is not set; point it at your CUDA 12.x toolkit.' }

$ninjaDir = Join-Path $env:USERPROFILE 'tools\ninja'
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = 'C:\Program Files\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio' |
        Where-Object { Test-Path $_ } |
        ForEach-Object { Get-ChildItem $_ -Recurse -Filter vcvarsall.bat -ErrorAction SilentlyContinue } |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $vcvars) { throw 'vcvarsall.bat not found; install the MSVC C++ build tools.' }
    Write-Host "Using MSVC environment from $($vcvars.FullName)"
    cmd /c "`"$($vcvars.FullName)`" x64 >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

# vcvarsall puts Visual Studio's bundled (often older) CMake first on PATH; prefer a standalone install.
$standaloneCMake = Join-Path $env:ProgramFiles 'CMake\bin'
if (Test-Path (Join-Path $standaloneCMake 'cmake.exe')) { $env:PATH = "$standaloneCMake;$env:PATH" }
$cmakeVersion = [version]((cmake --version | Select-Object -First 1) -replace '[^0-9.]', '')
if ($cmakeVersion -lt [version]'3.24') {
    throw "CMake $cmakeVersion found at $((Get-Command cmake).Source); 3.24+ is required (winget install Kitware.CMake)."
}

cmake --preset $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Config
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if ($Run) { & "build\$Config\tripmap.exe" @AppArgs }
