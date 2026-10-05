param([switch]$Package,[string]$SdkRoot='', [string]$JavaHome='')
$ErrorActionPreference='Stop'
$stage='sdkPreflight'
function Stage([string]$value) { $script:stage=$value; Write-Output "@@Q10_STAGE:$value" }
function BuildError([string]$code,[string]$message) { Write-Output "@@Q10_ERROR:$code"; throw $message }
function RequireFile([string]$path) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { BuildError 'SDK_FILE_MISSING' "Missing dependency: $path" } }
function Sha256([string]$path) {
    $stream=[IO.File]::OpenRead($path); $hash=[Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($hash.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
    finally { $hash.Dispose(); $stream.Dispose() }
}
function UniqueDirectory([string]$pattern,[string]$required,[string]$label) {
    $matches=@(Get-ChildItem -Path $pattern -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName $required) -PathType Leaf })
    if ($matches.Count -ne 1) { BuildError 'SDK_LAYOUT_AMBIGUOUS' "$label needs one matching directory under $SdkRoot; found $($matches.Count)" }
    return $matches[0].FullName
}
$before=@{Host=$env:QNX_HOST;Target=$env:QNX_TARGET;Path=$env:PATH}
Push-Location $PSScriptRoot
try {
    Stage 'sdkPreflight'
    if (-not $SdkRoot) { $SdkRoot=$env:INTROOP_SDK_ROOT }
    if (-not $SdkRoot) { $SdkRoot=[Environment]::GetEnvironmentVariable('INTROOP_SDK_ROOT','User') }
    if (-not $SdkRoot -or -not (Test-Path -LiteralPath $SdkRoot -PathType Container)) { BuildError 'SDK_DIRECTORY_MISSING' 'Pass -SdkRoot or set INTROOP_SDK_ROOT to the BB10 SDK root.' }
    $SdkRoot=(Resolve-Path -LiteralPath $SdkRoot).Path
    # Resolve host Python before QNX_HOST/bin, which contains an incompatible old Python.
    $hostPython=(Get-Command python.exe -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source
    & $hostPython -c 'import sys; sys.exit(0 if sys.version_info >= (3,8) else 1)'
    if ($LASTEXITCODE -ne 0) { BuildError 'HOST_PYTHON_INVALID' 'Host validation requires Python >=3.8, separately from device Python 3.2.' }
    $hostRoot=UniqueDirectory (Join-Path $SdkRoot 'host_*/win32/x86') 'usr/bin/qcc.exe' 'QNX_HOST'
    $targetRoot=UniqueDirectory (Join-Path $SdkRoot 'target_*/qnx6') 'usr/include/qt4/QtCore/QObject' 'QNX_TARGET'
    $env:QNX_HOST=$hostRoot; $env:QNX_TARGET=$targetRoot
    $qcc=Join-Path $hostRoot 'usr/bin/qcc.exe'; $moc=Join-Path $hostRoot 'usr/bin/moc.exe'
    $readelf=Join-Path $hostRoot 'usr/bin/ntoarm-readelf.exe'; $packager=Join-Path $hostRoot 'usr/bin/blackberry-nativepackager.bat'
    $qt=Join-Path $targetRoot 'usr/include/qt4'; $libs=Join-Path $targetRoot 'armle-v7/usr/lib/qt4/lib'
    foreach ($dependency in @($qcc,$moc,$readelf,$packager,"$qt/QtCore/QObject","$libs/libQtCore.so","$targetRoot/armle-v7/lib/libcpp.so.4","$targetRoot/usr/include/bb/cascades/Application","$targetRoot/usr/include/sqlite3.h")) { RequireFile $dependency }
    if ($Package) {
        if (-not $JavaHome) { $JavaHome=$env:BBATTERY_JAVA_HOME }
        if (-not $JavaHome) {
            $javaCandidates=@(Get-ChildItem -Path 'C:/bbndk/features/com.qnx.tools.jre.*/jre' -Directory -ErrorAction SilentlyContinue | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/java.exe') })
            if ($javaCandidates.Count -eq 1) { $JavaHome=$javaCandidates[0].FullName }
            elseif ($env:JAVA_HOME) { $JavaHome=$env:JAVA_HOME }
            else { BuildError 'JAVA_RUNTIME_MISSING' 'Pass -JavaHome or set BBATTERY_JAVA_HOME to the BB10 packager Java runtime.' }
        }
        RequireFile (Join-Path $JavaHome 'bin/java.exe')
        $env:PATH=(Join-Path $JavaHome 'bin')+';'+$env:PATH
    }
    $env:PATH=(Join-Path $hostRoot 'usr/bin')+';'+$env:PATH
    New-Item -ItemType Directory -Force build | Out-Null
    Write-Output "SDK=$SdkRoot; QNX_HOST=$hostRoot; QNX_TARGET=$targetRoot; Java=$JavaHome"
    & "$PSScriptRoot/tools/prepare-assets.ps1"
    & $hostPython -B tools/qml_preview.py --sdk $SdkRoot --all
    if ($LASTEXITCODE -ne 0) { BuildError 'QML_VALIDATION_FAILED' 'Native SDK QML loading failed; inspect build/qml-preview evidence.' }
    $flags=@('-V4.6.3,gcc_ntoarmv7le_cpp','-O2','-g','-Wall','-Wextra','-Wno-psabi','-D_FILE_OFFSET_BITS=64',"-I$qt","-I$qt/QtCore","-I$qt/QtGui",'-Isrc',"-L$libs","-Wl,-rpath-link,$libs",'-Wl,-rpath,/usr/lib/qt4/lib')
    Stage 'compile'
    foreach ($unit in @('measurement','store')) {
        & $qcc @flags -c "src/$unit.cpp" -o "build/$unit.o"
        if ($LASTEXITCODE -ne 0) { BuildError 'COMPILE_FAILED' "Compiler failed for $unit; exit=$LASTEXITCODE" }
    }
    & $qcc @flags -o build/batteryd src/collector.cpp build/measurement.o build/store.o -lbb -lbbsystem -lQtCore -lbps -lpps -lsqlite3 -lm
    if ($LASTEXITCODE -ne 0) { BuildError 'COLLECTOR_LINK_FAILED' "Collector linker exit=$LASTEXITCODE" }
    & $moc src/backend.h -o build/moc_backend.cpp
    if ($LASTEXITCODE -ne 0) { BuildError 'MOC_FAILED' "Qt4 moc exit=$LASTEXITCODE" }
    & $qcc @flags -o build/bbattery src/main.cpp src/backend.cpp build/moc_backend.cpp build/measurement.o build/store.o -lbbcascades -lbbsystem -lbbdata -lbb -lQtCore -lQtGui -lQtDeclarative -lpps -lsqlite3 -lm
    if ($LASTEXITCODE -ne 0) { BuildError 'GUI_LINK_FAILED' "Native GUI linker exit=$LASTEXITCODE" }
    Stage 'elfValidation'
    $abi=@{}
    foreach ($binary in @('bbattery','batteryd')) {
        $header=(& $readelf -h "build/$binary") -join "`n"
        if ($LASTEXITCODE -ne 0 -or $header -notmatch 'ELF32' -or $header -notmatch 'little endian' -or $header -notmatch 'ARM') { BuildError 'ELF_ARCH_INVALID' "Unexpected ELF architecture: $binary" }
        $dynamic=(& $readelf -d "build/$binary") -join "`n"
        if ($LASTEXITCODE -ne 0 -or $dynamic -match 'libstdc\+\+' -or $dynamic -notmatch 'libcpp\.so\.4') { BuildError 'RUNTIME_ABI_INVALID' "Expected BB10 libcpp.so.4 runtime: $binary" }
        $abi[$binary]=$dynamic
    }
    $context=@{sdk=$SdkRoot;host=$hostRoot;target=$targetRoot;java=$JavaHome;hostPython=$hostPython;compiler=(Sha256 $qcc);variant='gcc_ntoarmv7le_cpp';applicationVersion='0.1.0.12';firmwareBaseline='1.1.6-M';abi=$abi}
    [IO.File]::WriteAllText((Join-Path $PSScriptRoot 'build/environment.json'),($context | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
    if ($Package) {
        Stage 'package'
        & $packager -package -devMode build/BBattery.bar bar-descriptor.xml
        if ($LASTEXITCODE -ne 0) { BuildError 'PACKAGE_FAILED' "BAR packager exit=$LASTEXITCODE" }
        Stage 'barValidation'
        & $hostPython -B tools/audit_bar.py build/BBattery.bar
        if ($LASTEXITCODE -ne 0) { BuildError 'BAR_VALIDATION_FAILED' "Resource/ELF audit exit=$LASTEXITCODE" }
        Write-Output "BAR SHA256: $(Sha256 'build/BBattery.bar')"
    }
} catch {
    Write-Output "Build stopped at $stage`: $($_.Exception.Message)"
    throw
} finally {
    $env:PATH=$before.Path; $env:QNX_HOST=$before.Host; $env:QNX_TARGET=$before.Target
    Pop-Location
}
