@echo off
rem PC test build (clang-cl from C:\Program Files\LLVM, Ninja). Used to check the VR camera hook on a desk.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d %~dp0\..\..
if not exist out\build\pc\build.ninja (
  cmake -S . -B out\build\pc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DGOEMON_PREBUILT_PATCHES=ON
)
cmake --build out\build\pc --target Goemon64Recompiled -j 6
