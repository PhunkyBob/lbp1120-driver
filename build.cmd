@echo off
rem build.cmd - builds the Canon LASER SHOT LBP-1120 CAPT driver for Windows x64 and assembles
rem the unsigned driver package in pkg\. Nothing is installed or signed (install.ps1 does that).
rem
rem   bin\lbp1120rnd.dll (+ .pdb)  Unidrv v3 render plug-in   (src\render\)
rem   bin\lbp1120lm.dll  (+ .pdb)  "LBP1120 CAPT Monitor" language monitor (src\langmon\)
rem   pkg\                         the 2 DLLs + driver\lbp1120.gpd, .ini, lbp1120v3.inf, .cdf
rem   obj\                         intermediate files
rem
rem Toolchain: MSVC x64 (Visual Studio Build Tools) + Windows SDK 10.0.26100.
rem vcvarsall.bat is located with vswhere; set VCVARS=<full path of vcvarsall.bat> to override.
rem Already in an x64 developer prompt: the current environment is used as is.
setlocal
set "ROOT=%~dp0"

if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :haveenv
set "VSINSTALL="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
rem vcvarsall itself runs vswhere.exe from PATH: add the Installer folder when it exists.
if exist "%VSWHERE%" set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if defined VCVARS goto :checkvc
if not exist "%VSWHERE%" goto :fallback
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL goto :fallback
set "VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if exist "%VCVARS%" goto :checkvc
:fallback
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
:checkvc
if not exist "%VCVARS%" goto :novc
echo === vcvarsall: "%VCVARS%" x64 10.0.26100.0
call "%VCVARS%" x64 10.0.26100.0 >nul
if errorlevel 1 goto :novc
:haveenv

pushd "%ROOT%"
if exist obj rd /s /q obj
if exist bin rd /s /q bin
if exist pkg rd /s /q pkg
mkdir obj\render obj\langmon bin pkg
if errorlevel 1 goto :fail

rem Static CRT (/MT) for both DLLs: they are loaded into spoolsv.exe / PrintIsolationHost.exe,
rem so no VC++ redistributable is needed on the target machine.

echo === lbp1120rnd.dll (Unidrv render plug-in)
set CFLAGS_RND=/nologo /W4 /WX /EHsc /O2 /MT /Zi /GS /guard:cf /DUNICODE /D_UNICODE /DKERNEL_MODE /DUSERMODE_DRIVER /D_CRT_SECURE_NO_WARNINGS
rc /nologo /fo obj\render\lbp1120rnd.res src\render\lbp1120rnd.rc
if errorlevel 1 goto :fail
cl %CFLAGS_RND% /LD /Foobj\render\ /Fdobj\render\ /Febin\lbp1120rnd.dll src\render\lbp1120rnd.cpp src\render\capt_encode.cpp src\render\capt_page.cpp obj\render\lbp1120rnd.res ^
   /link /nologo /DLL /DEF:src\render\lbp1120rnd.def /PDB:bin\lbp1120rnd.pdb /IMPLIB:obj\render\lbp1120rnd.lib /DEBUG /OPT:REF /OPT:ICF /guard:cf /DYNAMICBASE /NXCOMPAT ^
   kernel32.lib user32.lib gdi32.lib winspool.lib ole32.lib uuid.lib
if errorlevel 1 goto :fail

echo === lbp1120lm.dll (language monitor)
set CFLAGS_LM=/nologo /W4 /WX /O2 /MT /GS /guard:cf /Zi /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE
cl %CFLAGS_LM% /Foobj\langmon\ /Fdobj\langmon\ src\langmon\lbp1120lm.c /LD /Fe:bin\lbp1120lm.dll ^
   /link /DEF:src\langmon\lbp1120lm.def /DLL /MACHINE:X64 /GUARD:CF /DYNAMICBASE /NXCOMPAT /DEBUG /OPT:REF /PDB:bin\lbp1120lm.pdb /IMPLIB:obj\langmon\lbp1120lm.lib ^
   kernel32.lib advapi32.lib
if errorlevel 1 goto :fail

echo === exports
dumpbin /nologo /exports bin\lbp1120rnd.dll > obj\render\exports.txt
if errorlevel 1 goto :fail
findstr /c:"DllGetClassObject" obj\render\exports.txt
if errorlevel 1 ( echo lbp1120rnd.dll does not export DllGetClassObject & goto :fail )
findstr /c:"DllCanUnloadNow" obj\render\exports.txt
if errorlevel 1 ( echo lbp1120rnd.dll does not export DllCanUnloadNow & goto :fail )
dumpbin /nologo /exports bin\lbp1120lm.dll > obj\langmon\exports.txt
if errorlevel 1 goto :fail
findstr /c:"InitializePrintMonitor2" obj\langmon\exports.txt
if errorlevel 1 ( echo lbp1120lm.dll does not export InitializePrintMonitor2 & goto :fail )
dumpbin /nologo /dependents bin\lbp1120lm.dll | findstr /i "\.dll"

echo === pkg\ (unsigned driver package, signed and installed by install.ps1)
for %%f in (bin\lbp1120rnd.dll bin\lbp1120lm.dll driver\lbp1120.gpd driver\lbp1120.ini driver\lbp1120v3.inf driver\lbp1120v3.cdf) do (
    copy /y %%f pkg\ >nul
    if errorlevel 1 ( echo copy %%f failed & goto :fail )
)
dir /b pkg

popd
echo BUILD OK
exit /b 0

:novc
echo vcvarsall.bat not found or "vcvarsall x64 10.0.26100.0" failed: "%VCVARS%"
echo Install Visual Studio Build Tools with the MSVC x64 toolset and Windows SDK 10.0.26100,
echo or set VCVARS to the full path of vcvarsall.bat.
echo BUILD FAILED
exit /b 1

:fail
popd
echo BUILD FAILED
exit /b 1
