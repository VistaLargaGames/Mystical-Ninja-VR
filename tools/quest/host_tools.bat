@echo off
rem Builds the host file_to_c tool that the Quest cross build runs (shader/asset embedding).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0\..\..
cl /nologo /O2 /EHsc /std:c++17 lib\rt64\src\tools\file_to_c\file_to_c.cpp /Fe:out\build\x64-Release\file_to_c.exe /Fo:out\build\x64-Release\file_to_c.obj
