#!/bin/zsh
# Build dxmt-SC6 for ARM64EC (CrossOver Preview's aarch64 DXMT), following
# upstream CI job build-clang-release-arm64ec-windows-cross. Installs nothing.
set -euo pipefail
root=${0:A:h:h}
# deps.lock pins the toolchains; this downloads any that are missing and checks their hashes.
$root/tools/fetch-deps.sh build
cd $root/dxmt
T=toolchains  # dxmt/toolchains links to the repository's toolchains/ folder
MINGW=llvm-mingw-20260908-ucrt-macos-universal
if [[ ! -x $T/llvm-darwin-arm64/bin/llvm-config ]]; then
  cmake -G Ninja -B $T/llvm-darwin-arm64-build -S $T/llvm-project/llvm \
    -DCMAKE_INSTALL_PREFIX="$PWD/$T/llvm-darwin-arm64" \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DLLVM_HOST_TRIPLE=arm64-apple-darwin \
    -DLLVM_ENABLE_ASSERTIONS=On -DLLVM_ENABLE_ZSTD=Off -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_FLAGS="-D_LIBCPP_KEEP_TRANSITIVE_INCLUDES_LLVM23" \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_BUILD_TOOLS=Off -DLLVM_INCLUDE_BENCHMARKS=Off \
    -DBUG_REPORT_URL="https://github.com/3Shain/dxmt" -DPACKAGE_VENDOR="DXMT" \
    -DLLVM_VERSION_PRINTER_SHOW_HOST_TARGET_INFO=Off
  cmake --build $T/llvm-darwin-arm64-build
  cmake --install $T/llvm-darwin-arm64-build
fi
export PATH="$PATH:$PWD/$T/$MINGW/bin"
rm -rf build-arm64ec install-arm64ec
meson setup --cross-file build-arm64ec.txt -Denable_nvapi=true -Denable_nvngx=true \
  -Dnative_llvm_path=$T/llvm-darwin-arm64 -Dwine_install_path=$T/wine-arm64ec \
  build-arm64ec --buildtype release --prefix "$PWD/install-arm64ec" --strip
meson compile -C build-arm64ec
meson install -C build-arm64ec
print "BUILD_OK"; ls install-arm64ec/*
