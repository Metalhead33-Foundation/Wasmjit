#!/bin/sh
# ES-5 end-to-end check: install into a scratch prefix, then build and run a
# consumer that includes only the installed umbrella header.
#
#   ./Test/install-check.sh <build-dir> [prefix-root]
#
# `libElvavena`/`libEuphemy` are copied into the prefix by hand: the MhLib
# submodules do not install themselves in this tree, and §6.2 accepts them as
# part of the public SDK for the first freeze.
set -eu

build=$(cd "${1:?usage: install-check.sh <build-dir> [prefix-root]}" && pwd)
root=${2:-"$PWD/build/install-check-prefix"}
prefix=/usr/local
source_dir=$(cd "$(dirname "$0")/.." && pwd)

rm -rf "$root"
mkdir -p "$root"

cd "$build"
env MAKE=/usr/bin/make /usr/bin/make install QMAKE=/usr/bin/qmake6 \
    INSTALL_ROOT="$root" PREFIX="$prefix" > "$root/install.log" 2>&1

installed="$root$prefix"
test -f "$installed/include/wasmjit/WasmJit.hpp"
test -f "$installed/include/Elvavena/Io/ElvIoDevice.hpp"
test -f "$installed/lib/pkgconfig/wasmjit.pc"

# ── Exactly one core image (docs/BACKEND_PLUGIN_ABI.md §7) ─────────────────
# A static archive of the core would let a backend or plugin embed a second
# store / type registry; the install must not ship one.
if ls "$installed"/lib/libWasmBase.a >/dev/null 2>&1; then
    echo "install-check: FAILED: a static libWasmBase.a was installed" >&2
    exit 1
fi
# Every shipped backend must *depend on* the shared core, so the dynamic linker
# guarantees a single copy in the process.
if command -v readelf >/dev/null 2>&1; then
    for lib in "$installed"/lib/libLibJit.so.1; do
        if [ -e "$lib" ] && ! readelf -d "$lib" | grep -q 'NEEDED.*libWasmBase\.so'; then
            echo "install-check: FAILED: $(basename "$lib") does not record a" \
                 "dependency on the shared libWasmBase" >&2
            exit 1
        fi
    done
fi

cp -a "$build"/extern/MhLib/Elvavena/libElvavena.so* "$installed/lib/"
cp -a "$build"/extern/MhLib/Euphemy/libEuphemy.so* "$installed/lib/"

PKG_CONFIG_PATH="$installed/lib/pkgconfig" \
PKG_CONFIG_SYSROOT_DIR="$root" \
    c++ -std=c++2a "$source_dir/Test/consumer/Consumer.cpp" \
    -o "$root/consumer" \
    $(PKG_CONFIG_PATH="$installed/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$root" \
        pkg-config --cflags --libs wasmjit) \
    -Wl,-rpath,"$installed/lib"

"$root/consumer"
echo "install-check: OK (out-of-tree consumer linked through wasmjit.pc and ran)"
