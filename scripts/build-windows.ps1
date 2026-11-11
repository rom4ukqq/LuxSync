$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$dist = Join-Path (Get-Location) 'dist'
$output = Join-Path $dist 'windows'
New-Item -ItemType Directory -Force -Path $output | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -or $env:VSCMD_ARG_TGT_ARCH -ne 'x64') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Install the Visual Studio C++ build tools and Windows SDK.' }
    & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
}
$boring = Join-Path $dist 'deps/boringssl'
$revision = (Get-Content scripts/boringssl-revision.txt -Raw).Trim()
if (!(Test-Path (Join-Path $boring '.git')) -or (git -C $boring rev-parse HEAD) -ne $revision) {
    New-Item -ItemType Directory -Force -Path $boring | Out-Null
    git -C $boring init -q
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize BoringSSL checkout.' }
    git -C $boring fetch -q --depth 1 https://github.com/google/boringssl.git $revision
    if ($LASTEXITCODE -ne 0) { throw 'Could not fetch BoringSSL.' }
    git -C $boring checkout -q --detach FETCH_HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Could not check out BoringSSL.' }
}
$crypto = Join-Path $dist 'deps/win-crypto'
& cmake -S $boring -B $crypto -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF `
    -DOPENSSL_NO_ASM=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
if ($LASTEXITCODE -ne 0) { throw 'Could not configure BoringSSL.' }
& cmake --build $crypto --target crypto --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Could not build BoringSSL.' }
$cryptoLib = Join-Path $crypto 'crypto.lib'
$pake = Join-Path $crypto 'pake.obj'
& cl.exe /nologo /c /O1 /MT "/I$boring/include" "/Fo$pake" protocol/pake.c
if ($LASTEXITCODE -ne 0) { throw 'Could not compile the pairing adapter.' }
$res = Join-Path $crypto 'resources.res'
& rc.exe /nologo "/fo$res" windows/brightsync/resources.rc
if ($LASTEXITCODE -ne 0) { throw 'Could not compile the Windows icons and dialog.' }
$exe = Join-Path $output 'BrightSync.exe'
$obj = Join-Path $dist 'BrightSync.obj'
& cl.exe /nologo /std:c++17 /EHsc /W4 /O1 /GL /Gy /Gw /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 `
    "/Fo$obj" "/Fe$exe" windows/brightsync/main.cpp $pake $res $cryptoLib /link /LTCG /SUBSYSTEM:WINDOWS /MACHINE:X64 `
    dxva2.lib bcrypt.lib crypt32.lib ws2_32.lib shell32.lib ole32.lib advapi32.lib uuid.lib gdi32.lib user32.lib wtsapi32.lib
if ($LASTEXITCODE -ne 0) { throw 'BrightSync compilation failed.' }
Copy-Item LICENSE (Join-Path $output 'LICENSE.txt') -Force
Copy-Item (Join-Path $boring 'LICENSE') (Join-Path $output 'BoringSSL-LICENSE.txt') -Force
Compress-Archive -Path $exe, (Join-Path $output 'LICENSE.txt'), (Join-Path $output 'BoringSSL-LICENSE.txt') -DestinationPath (Join-Path $dist 'BrightSync-Windows-x64.zip') -Force

if ($env:BRIGHTSYNC_RUN_TESTS -eq '1') {
    $test = Join-Path $dist 'core-tests.exe'
    $testObj = Join-Path $dist 'core-tests.obj'
    & cl.exe /nologo /std:c++17 /EHsc /W4 "/Fo$testObj" "/Fe$test" tests/core.cpp
    if ($LASTEXITCODE -ne 0) { throw 'Core test compilation failed.' }
    & $test
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    $pairTest = Join-Path $dist 'pairing-tests.exe'
    $pairObj = Join-Path $dist 'pairing-tests.obj'
    & cl.exe /nologo /std:c++17 /EHsc /W4 /MT "/Fo$pairObj" "/Fe$pairTest" tests/pairing.cpp $pake $cryptoLib ws2_32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Pairing test compilation failed.' }
    & $pairTest
    if ($LASTEXITCODE -ne 0) { throw 'Pairing tests failed.' }
    & python tests/simulation.py windows $exe
    if ($LASTEXITCODE -ne 0) { throw 'Receiver simulation failed.' }
}
