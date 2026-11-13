#!/usr/bin/env sh
# Usage: ./build.sh <llvm-mingw bin dir> <ViGEmClient checkout>
# Builds VRTreadmill.exe and the SteamVR driver DLL.
set -e
T="$1"; V="$2"
[ -d "$T" ] && [ -d "$V/include" ] || { echo "usage: $0 <llvm-mingw/bin> <ViGEmClient dir>"; exit 1; }
CC="$T/x86_64-w64-mingw32-gcc"; CXX="$T/x86_64-w64-mingw32-g++"
TC_INC="$(dirname "$T")/generic-w64-mingw32/include"
mkdir -p build/shim
# ViGEmClient includes Windows headers with capital letters (fine on Windows, not on Linux)
ln -sf "$TC_INC/windows.h" build/shim/Windows.h
ln -sf "$TC_INC/setupapi.h" build/shim/SetupAPI.h

# Desktop app
"$CXX" -c -O2 -DVIGEM_USE_STATIC_LIB -Ibuild/shim -I"$V/include" -I"$V/src" "$V/src/ViGEmClient.cpp" -o build/vigem.o
"$T/x86_64-w64-mingw32-windres" src/app.rc -O coff -o build/app.res
"$CC" -c -O2 -municode -Ibuild/shim -I"$V/include" src/main.c -o build/main.o
"$CXX" -O2 -municode -mwindows -static build/main.o build/vigem.o build/app.res -o VRTreadmill.exe \
    -lcomctl32 -lshell32 -lsetupapi
echo "Built VRTreadmill.exe"

# SteamVR driver
mkdir -p driver/00vrtreadmill/bin/win64
"$CXX" -std=c++17 -O2 -shared -static -isystem driver/include driver/src/driver.cpp \
    -o driver/00vrtreadmill/bin/win64/driver_00vrtreadmill.dll -Wl,--exclude-all-symbols
echo "Built driver/00vrtreadmill/bin/win64/driver_00vrtreadmill.dll"
