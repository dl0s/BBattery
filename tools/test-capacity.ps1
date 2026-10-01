param([string]$Compiler = '')
$ErrorActionPreference = 'Stop'
if (-not $Compiler) {
    $compilerCommand = Get-Command x86_64-w64-mingw32-g++.exe -CommandType Application -ErrorAction SilentlyContinue
    if (-not $compilerCommand) { $compilerCommand = Get-Command g++.exe -CommandType Application -ErrorAction SilentlyContinue }
    if (-not $compilerCommand) { throw 'A host C++ compiler is required for the capacity tests' }
    $Compiler = $compilerCommand.Source
}
$savedPath = $env:PATH
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    New-Item -ItemType Directory -Force -Path build | Out-Null
    $env:PATH = (Split-Path -Parent $Compiler) + ';' + $env:PATH
    $mingwRuntime = 'C:/cygwin64/usr/x86_64-w64-mingw32/sys-root/mingw/bin'
    if (Test-Path -LiteralPath $mingwRuntime -PathType Container) { $env:PATH = $mingwRuntime + ';' + $env:PATH }
    & $Compiler -std=c++98 -Wall -Wextra -Werror tests/capacity_test.cpp -o build/capacity-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Capacity test compilation failed' }
    & .\build\capacity-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Capacity calculation tests failed' }
    python -B tests/capacity_history_test.py
    if ($LASTEXITCODE -ne 0) { throw 'Capacity history tests failed' }
} finally {
    $env:PATH = $savedPath
    Pop-Location
}
