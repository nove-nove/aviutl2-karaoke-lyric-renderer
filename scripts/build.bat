@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
pushd "%SCRIPT_DIR%.."

set "BUILD_VERSION=%~1"
if "%BUILD_VERSION%"=="" if defined KLR_BUILD_VERSION set "BUILD_VERSION=%KLR_BUILD_VERSION%"
if "%BUILD_VERSION%"=="" set "BUILD_VERSION=dev"

where cl >nul 2>nul
if errorlevel 1 (
    echo cl.exe was not found.
    echo Run this script from a Visual Studio Developer PowerShell or Native Tools Command Prompt.
    popd
    exit /b 1
)

if not exist build (
    mkdir build
)

echo Building plugin version %BUILD_VERSION%...
cl /nologo /utf-8 /std:c++17 /EHsc /LD /I vendor\aviutl2_sdk /D "KLR_PLUGIN_VERSION=\"%BUILD_VERSION%\"" /Fo"build\\" /Fd"build\\AviUtl2KaraokeLyricRenderer.pdb" src\KaraokeLyricRenderer.cpp src\KaraokeLyricParsing.cpp src\KaraokeLyricLayout.cpp /link gdi32.lib user32.lib /OUT:build\AviUtl2KaraokeLyricRenderer.auf2 /IMPLIB:build\AviUtl2KaraokeLyricRenderer.lib /PDB:build\AviUtl2KaraokeLyricRenderer.pdb
set "BUILD_RESULT=%ERRORLEVEL%"

popd
exit /b %BUILD_RESULT%
