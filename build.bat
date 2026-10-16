@echo off
rem Configures and builds the x86 DLL (left4dead2.exe is 32-bit).
rem   build.bat [Debug|Release] [target]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
set TARGET=%2
set VS=C:\Program Files\Microsoft Visual Studio\18\Insiders
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set BUILD=%~dp0build\x86-%CONFIG%
"%CMAKE%" -S "%~dp0." -B "%BUILD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_CXX_COMPILER=cl || exit /b 2
if "%TARGET%"=="" (
    "%CMAKE%" --build "%BUILD%" || exit /b 3
) else (
    "%CMAKE%" --build "%BUILD%" --target %TARGET% || exit /b 3
)
endlocal
