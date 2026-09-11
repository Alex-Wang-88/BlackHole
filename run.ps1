$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakePath = if($null -ne $cmakeCommand) { $cmakeCommand.Source } else { $null }

if($null -eq $cmakePath)
{
    $cmakeCandidates = @(
        'C:\Program Files\CMake\bin\cmake.exe',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')

    foreach($candidate in $cmakeCandidates)
    {
        if(Test-Path -LiteralPath $candidate)
        {
            $cmakePath = $candidate
            break
        }
    }
}

if($null -eq $cmakePath)
{
    throw 'CMake was not found. Install CMake or Visual Studio 2022 with C++ tools.'
}

$buildDir = Join-Path $projectDir 'build'
$cmakeArguments = @()

$streamlineSdkRoot = $env:BLACKHOLE_STREAMLINE_SDK_ROOT
if([string]::IsNullOrWhiteSpace($streamlineSdkRoot))
{
    $siblingStreamlineRoot = Join-Path (Split-Path -Parent $projectDir) 'streamline-src'
    if(Test-Path -LiteralPath (Join-Path $siblingStreamlineRoot 'include\sl.h'))
    {
        $streamlineSdkRoot = $siblingStreamlineRoot
    }
}
if(-not [string]::IsNullOrWhiteSpace($streamlineSdkRoot))
{
    $cmakeArguments += "-DBLACKHOLE_STREAMLINE_SDK_ROOT=$streamlineSdkRoot"
}

$streamlineRuntimeDir = $env:BLACKHOLE_STREAMLINE_RUNTIME_DIR
if([string]::IsNullOrWhiteSpace($streamlineRuntimeDir))
{
    $localRuntimeDir = Join-Path $projectDir 'third_party\streamline\bin'
    if(Test-Path -LiteralPath (Join-Path $localRuntimeDir 'sl.interposer.dll'))
    {
        $streamlineRuntimeDir = $localRuntimeDir
    }
}
if(-not [string]::IsNullOrWhiteSpace($streamlineRuntimeDir))
{
    $cmakeArguments += "-DBLACKHOLE_STREAMLINE_RUNTIME_DIR=$streamlineRuntimeDir"
}

& $cmakePath -S $projectDir -B $buildDir -G 'Visual Studio 17 2022' -A x64 @cmakeArguments
& $cmakePath --build $buildDir --config Release --parallel
& (Join-Path $buildDir 'Release\BlackHole.exe')
