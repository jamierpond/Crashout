@echo off
rem Builds CrashReportingTests in an MSVC environment for %1 (x64 or arm64) and runs it.
setlocal
set arch=%1
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set vs=%%i
call "%vs%\VC\Auxiliary\Build\vcvarsall.bat" %arch% || exit /b 1
cd /d "%~dp0.."
cmake -G Ninja -B build-%arch% -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEACP_UNITY_BUILD=OFF || exit /b 1
cmake --build build-%arch% --target CrashReportingTests || exit /b 1
ctest --test-dir build-%arch% --output-on-failure -R "^(CrashReport|EventWaits)$"
