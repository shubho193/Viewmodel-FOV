@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo Could not find vswhere.exe. Is Visual Studio installed?
	exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
	echo Could not find the Visual Studio C++ tools.
	exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist obj mkdir obj

cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /DWIN32 /D_CRT_SECURE_NO_WARNINGS /Isdk /Fo:obj\ /LD src\main.cpp src\legal.cpp sdk\setup.cpp sdk\plugin_sdk_import.gen.cpp /Fe:vmfov_native.dll /link d3d11.lib dxgi.lib user32.lib
set "RESULT=%ERRORLEVEL%"

rmdir /s /q obj
del /q vmfov_native.lib vmfov_native.exp 2>nul

exit /b %RESULT%
