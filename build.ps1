param([switch]$Package,[string]$SdkRoot='C:/Users/dove1/Documents/BBarmin/sdk')
$ErrorActionPreference='Stop'
$before = @{ Host=$env:QNX_HOST; Target=$env:QNX_TARGET; Path=$env:PATH }
Push-Location $PSScriptRoot
try {
    $env:QNX_HOST = Join-Path $SdkRoot 'host_10_3_1_12/win32/x86'
    $env:QNX_TARGET = Join-Path $SdkRoot 'target_10_3_1_995/qnx6'
    $env:PATH = 'C:/bbndk/features/com.qnx.tools.jre.win32.x86_64_1.7.0.51/jre/bin;' + (Join-Path $env:QNX_HOST 'usr/bin') + ';' + $env:PATH
    New-Item -ItemType Directory -Force build | Out-Null
    & "$PSScriptRoot/tools/prepare-assets.ps1"
    $qcc = Join-Path $env:QNX_HOST 'usr/bin/qcc.exe'
    $moc = Join-Path $env:QNX_HOST 'usr/bin/moc.exe'
    $qt = Join-Path $env:QNX_TARGET 'usr/include/qt4'
    $libs = Join-Path $env:QNX_TARGET 'armle-v7/usr/lib/qt4/lib'
    $flags = @('-V4.6.3,gcc_ntoarmv7le_cpp','-O2','-g','-Wall','-Wextra','-Wno-psabi', '-D_FILE_OFFSET_BITS=64',
        "-I$qt","-I$qt/QtCore","-I$qt/QtGui",'-Isrc',"-L$libs","-Wl,-rpath-link,$libs",'-Wl,-rpath,/usr/lib/qt4/lib')
    foreach ($unit in @('measurement','store')) {
        & $qcc @flags -c "src/$unit.cpp" -o "build/$unit.o"
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $unit" }
    }
    & $qcc @flags -o build/batteryd src/collector.cpp build/measurement.o build/store.o -lQtCore -lbps -lpps -lsqlite3 -lm
    if ($LASTEXITCODE -ne 0) { throw 'Collector build failed' }
    & $moc src/backend.h -o build/moc_backend.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Backend moc failed' }
    & $qcc @flags -o build/bbattery src/main.cpp src/backend.cpp build/moc_backend.cpp build/measurement.o build/store.o -lbbcascades -lbbsystem -lbbdata -lbb -lQtCore -lQtGui -lQtDeclarative -lpps -lsqlite3 -lm
    if ($LASTEXITCODE -ne 0) { throw 'Native UI build failed' }
    $readelf = Join-Path $env:QNX_HOST 'usr/bin/ntoarm-readelf.exe'
    foreach ($binary in @('bbattery','batteryd')) {
        $dynamic = (& $readelf -d "build/$binary") -join "`n"
        if ($LASTEXITCODE -ne 0 -or $dynamic -match 'libstdc\+\+' -or $dynamic -notmatch 'libcpp\.so\.4') { throw "Unexpected runtime ABI: $binary" }
    }
    if ($Package) {
        & (Join-Path $env:QNX_HOST 'usr/bin/blackberry-nativepackager.bat') -package -devMode build/BBattery.bar bar-descriptor.xml
        if ($LASTEXITCODE -ne 0) { throw 'BAR packaging failed' }
        Get-FileHash build/BBattery.bar -Algorithm SHA256
    }
} finally {
    $env:PATH = $before.Path
    $env:QNX_HOST = $before.Host
    $env:QNX_TARGET = $before.Target
    Pop-Location
}
