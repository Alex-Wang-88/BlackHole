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
& $cmakePath -S $projectDir -B $buildDir -G 'Visual Studio 17 2022' -A x64
& $cmakePath --build $buildDir --config Release --parallel
& (Join-Path $buildDir 'Release\BlackHole.exe')
