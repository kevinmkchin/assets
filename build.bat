@echo off
:: Builds build\assets.exe. Run it from an x64 Visual Studio developer prompt (cl on PATH).
setlocal
cd /D "%~dp0"
if not exist build mkdir build
cl -nologo -O2 -W4 -WX -MT -Z7 assets.cpp -Fobuild\ -Febuild\assets.exe /link -incremental:no /PDBALTPATH:assets.pdb || exit /b 1
