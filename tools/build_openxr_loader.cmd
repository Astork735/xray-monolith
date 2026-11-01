@echo off
rem ===========================================================================
rem Fetch + build the OpenXR loader and vendor it into the repo.
rem
rem The OpenXR loader import library (sdk\libraries\x64\openxr_loader.lib) lives
rem under a git-ignored path (the repo ignores /**/x64/), so a fresh clone does
rem not contain it. Run this once before building the DX11/VR target.
rem
rem Requires: git, cmake, and VS2022. Run from the repository root.
rem ===========================================================================
setlocal

set "ROOT=%~dp0.."
set "BUILD=%TEMP%\openxr_loader_build"
set "SRC=%BUILD%\OpenXR-SDK"

if not exist "%ROOT%\src\engine-vs2022.sln" (
    echo [!] Run this from the repository root: tools\build_openxr_loader.cmd
    exit /b 1
)

echo [*] Cloning OpenXR-SDK...
if exist "%SRC%\.git" (
    git -C "%SRC%" pull --ff-only || goto :fail
) else (
    git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git "%SRC%" || goto :fail
)

echo [*] Copying OpenXR headers...
if not exist "%ROOT%\sdk\include\openxr" mkdir "%ROOT%\sdk\include\openxr"
copy /Y "%SRC%\include\openxr\*.h" "%ROOT%\sdk\include\openxr\" >nul || goto :fail

echo [*] Configuring + building the loader (x64 Release)...
cmake -S "%SRC%" -B "%BUILD%\b" -G "Visual Studio 17 2022" -A x64 ^
    -DBUILD_API_LAYERS=OFF -DBUILD_TESTS=OFF -DBUILD_LOADER=ON ^
    -DDYNAMIC_LOADER=ON -DBUILD_CONFORMANCE_TESTS=OFF || goto :fail
cmake --build "%BUILD%\b" --config Release --target openxr_loader || goto :fail

echo [*] Copying loader lib + dll...
if not exist "%ROOT%\sdk\libraries\x64" mkdir "%ROOT%\sdk\libraries\x64"
if not exist "%ROOT%\sdk\binaries" mkdir "%ROOT%\sdk\binaries"
copy /Y "%BUILD%\b\src\loader\Release\openxr_loader.lib" "%ROOT%\sdk\libraries\x64\" >nul || goto :fail
copy /Y "%BUILD%\b\src\loader\Release\openxr_loader.dll" "%ROOT%\sdk\binaries\" >nul || goto :fail

echo [OK] OpenXR loader vendored into sdk\.
exit /b 0

:fail
echo [!] Failed. Ensure git, cmake and VS2022 are installed and on PATH.
exit /b 1
