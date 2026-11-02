#!/bin/zsh
set -euo pipefail
here=${0:A:h}
mkdir -p "$here/../../dll/build"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$here/input-control.c" -o "$here/../../dll/build/input-control.exe" -static-libgcc
