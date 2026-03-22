@echo off
setlocal enabledelayedexpansion

:: Source MSVC environment
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1

:: Use VS cmake (avoid STM32CubeCLT cmake on PATH)
set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

cd /d "%~dp0"

echo === CONFIGURE ===
"%CMAKE_EXE%" -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
if !errorlevel! neq 0 (
    echo CONFIGURE FAILED
    exit /b 1
)

echo === BUILD ===
"%CMAKE_EXE%" --build build
if !errorlevel! neq 0 (
    echo BUILD FAILED
    exit /b 1
)

echo === TESTS ===
if exist build\crosspad_core_tests.exe (
    build\crosspad_core_tests.exe --reporter compact
) else (
    echo ERROR: crosspad_core_tests.exe not found
    exit /b 1
)
