TEMPLATE = lib
TARGET = WasmBase
# Explicit soname policy (docs/EMBEDDING_SURFACE.md §7.2/ES-4): the major is the
# soname, so a change to a frozen surface (API_ABI_STABILITY.md §2.2 S1-S9) bumps
# it. The value matches WASMJIT_VERSION_* in WasmVersion.hpp.
VERSION = 1.0.0
CONFIG += shared c++2a
CONFIG -= qt
# docs/BACKEND_PLUGIN_ABI.md §7 (blocker 2): a static archive of the core would
# let a backend or plugin embed a second store/type registry. Only the shared
# library is a supported configuration.
contains(CONFIG, staticlib)|contains(CONFIG, static)|!contains(CONFIG, shared): error("libWasmBase must be built shared: a static core lets a backend embed a second store and type registry (docs/BACKEND_PLUGIN_ABI.md §7).")

# docs/EMBEDDING_SURFACE.md ES-4: the library hides its symbols by default; the
# public tier is marked WASMJIT_API (WasmApi.hpp). Hiding inline instantiations
# is what removes most of the old 23k-symbol table.
CONFIG += hide_symbols
DEFINES += WASMJIT_BUILD
QMAKE_CXXFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
# libffi backs the component runtime's canonical ABI closures. It is a host
# ABI library, not a wasm code generator; the runtime itself is backend-neutral.
CONFIG += link_pkgconfig
PKGCONFIG += libffi
# nlohmann_json is header-only and used by the diagnostics JSON formatter
# (`WasmBase/WasmDiagnostics.cpp`). It is a host utility library, not a wasm
# code generator, so the dependency is backend-neutral.
PKGCONFIG += nlohmann_json
include(WasmBase.pri)

# Highway (`libhwy`, per pkg-config: -I/usr/include -DHWY_SHARED_DEFINE): the
# vectorised SIMD helper bodies in wasm_simd_vector.* are compiled once per CPU
# target through Highway's re-inclusion machinery. The per-target ISA flags come
# from the pragmas Highway pushes around each target's namespace, *not* from
# global -m flags, so the rest of the library stays baseline code.
LIBS += -lhwy

# Dynamic backend loading (docs/BACKEND_PLUGIN_ABI.md BP-3). glibc >= 2.34 keeps
# dlopen in libc; the explicit -ldl keeps older sysroots working.
LIBS += -ldl
DEFINES += HWY_SHARED_DEFINE

# Avoid build errors with no files:
SOURCES += \
        WasmArtifact.cpp \
        WasmAgent.cpp \
        WasmBackendRegistry.cpp \
        WasmComponent.cpp \
        WasmComponentRuntime.cpp \
        WasmComponentValidation.cpp \
        WasmContinuation.cpp \
        WasmDiagnostics.cpp \
        WasmException.cpp \
        WasmExn.cpp \
        WasmGcHeap.cpp \
        WasmLegacyEh.cpp \
        WasmMemory.cpp \
        WasmModule.cpp \
        WasmModuleInstance.cpp \
        WasmNullInstantiator.cpp \
        WasmOpcodeDispatcher.cpp \
        WasmRegistryImportResolver.cpp \
        WasmRuntime.cpp \
        WasmSection.cpp \
        WasmStack.cpp \
        WasmStore.cpp \
        WasmString.cpp \
        WasmTable.cpp \
        WasmTrap.cpp \
        WasmType.cpp \
        WasmTypeStore.cpp \
        WasmTypeRegistry.cpp \
        WasmValidator.cpp \
        WasmVersion.cpp \
        wasm_atomic.cpp \
        wasm_simd_scalar.cpp \
        wasm_simd_vector.cpp

HEADERS += \
    WasmAgent.hpp \
    WasmBackendAbi.hpp \
    WasmBackendRegistry.hpp \
    WasmArtifact.hpp \
    WasmComponent.hpp \
    WasmComponentModel.hpp \
    WasmComponentRuntime.hpp \
    WasmComponentSection.hpp \
    WasmComponentValidation.hpp \
    WasmConcurrency.hpp \
    WasmContinuation.hpp \
    WasmDeterminism.hpp \
    WasmDiagnostics.hpp \
    WasmException.hpp \
    WasmExn.hpp \
    WasmGcHeader.hpp \
    WasmGcHeap.hpp \
    WasmCanonicalType.hpp \
    WasmHostBridge.hpp \
    WasmImport.hpp \
    WasmJit.hpp \
    WasmMemory.hpp \
    WasmModule.hpp \
    WasmModuleInstance.hpp \
    WasmNativeImport.hpp \
    WasmNullInstantiator.hpp \
    WasmOpcode.hpp \
    WasmOpcodeDispatcher.hpp \
    WasmProfile.hpp \
    WasmRegistryImportResolver.hpp \
    WasmRuntime.hpp \
    WasmSafety.hpp \
    WasmSection.hpp \
    WasmStack.hpp \
    WasmStore.hpp \
    WasmString.hpp \
    WasmTable.hpp \
    WasmTrap.hpp \
    WasmTracing.hpp \
    WasmTypeStore.hpp \
    WasmType.hpp \
    WasmTypeIdentity.hpp \
    WasmTypeRegistry.hpp \
    WasmLegacyEh.hpp \
    WasmParseOptions.hpp \
    WasmValidator.hpp \
    WasmValidation.hpp \
    WasmVersion.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp \
    wasmjit_backend.h \
    wasm_atomic.hpp \
    wasm_simd_scalar.hpp \
    wasm_simd_vector.hpp \
    wasm_simd_ops.inc \
    WasmComponentParser.inc

# ── Install (docs/EMBEDDING_SURFACE.md ES-5) ────────────────────────────────
# Default prefix; override with `qmake PREFIX=/opt/wasmjit`.
isEmpty(PREFIX): PREFIX = /usr/local

# The public set is "all headers minus the internal tier" (EMBEDDING_SURFACE
# §6.1.1), so the tier lists stay the single source of truth.
WASMJIT_INTERNAL_HEADERS = \
    $$PWD/WasmSection.hpp \
    $$PWD/WasmLegacyEh.hpp \
    $$PWD/WasmTypeStore.hpp \
    $$PWD/WasmCanonicalType.hpp \
    $$PWD/WasmTypeRegistry.hpp \
    $$PWD/WasmGcHeap.hpp \
    $$PWD/WasmValidator.hpp \
    $$PWD/WasmValidation.hpp \
    $$PWD/WasmDeterminism.hpp \
    $$PWD/WasmComponent.hpp \
    $$PWD/WasmComponentAbi.hpp \
    $$PWD/WasmComponentAsync.hpp \
    $$PWD/WasmComponentModel.hpp \
    $$PWD/WasmComponentRuntime.hpp \
    $$PWD/WasmComponentSection.hpp \
    $$PWD/WasmComponentValidation.hpp \
    $$PWD/WasmComponentParser.inc

WASMJIT_PUBLIC_HEADERS = $$files($$PWD/*.hpp) $$files($$PWD/*.h) $$files($$PWD/*.inc)
WASMJIT_PUBLIC_HEADERS -= $$WASMJIT_INTERNAL_HEADERS

headers.files = $$WASMJIT_PUBLIC_HEADERS
headers.path  = $$PREFIX/include/wasmjit
# The public headers include the internal ones, so the closure is installed
# under an explicitly private subdirectory rather than being part of the API
# surface (EMBEDDING_SURFACE §6.1.1).
internal_headers.files = $$WASMJIT_INTERNAL_HEADERS
internal_headers.path  = $$PREFIX/include/wasmjit/internal
target.path   = $$PREFIX/lib

# The vendored headers the public SDK includes (EMBEDDING_SURFACE §6.2 accepts
# them for the first freeze, so an install without them is not usable).
elv_io.files   = $$files($$PWD/../extern/MhLib/Elvavena/Io/*.hpp)
elv_io.path    = $$PREFIX/include/Elvavena/Io
elv_util.files = $$files($$PWD/../extern/MhLib/Elvavena/Util/*.hpp)
elv_util.path  = $$PREFIX/include/Elvavena/Util
euph_asset.files = $$files($$PWD/../extern/MhLib/Euphemy/Asset/*.hpp)
euph_asset.path  = $$PREFIX/include/Euphemy/Asset
euph_config.files = $$files($$PWD/../extern/MhLib/Euphemy/Config/*.hpp)
euph_config.path  = $$PREFIX/include/Euphemy/Config
euph_io.files  = $$files($$PWD/../extern/MhLib/Euphemy/Io/*.hpp)
euph_io.path   = $$PREFIX/include/Euphemy/Io
euph_audio.files = $$files($$PWD/../extern/MhLib/Euphemy/Media/Audio/*.hpp)
euph_audio.path  = $$PREFIX/include/Euphemy/Media/Audio
euph_image.files = $$files($$PWD/../extern/MhLib/Euphemy/Media/Image/*.hpp)
euph_image.path  = $$PREFIX/include/Euphemy/Media/Image
euph_memory.files = $$files($$PWD/../extern/MhLib/Euphemy/Memory/*.hpp)
euph_memory.path  = $$PREFIX/include/Euphemy/Memory

# pkg-config: the file qmake generates is named after TARGET, so it is produced
# from a template instead and installed under the package name.
WASMJIT_PREFIX = $$PREFIX
wasmjit_pc.input  = $$PWD/wasmjit.pc.in
wasmjit_pc.output = $$OUT_PWD/wasmjit.pc
QMAKE_SUBSTITUTES += wasmjit_pc

pkgconfig.files = $$OUT_PWD/wasmjit.pc
pkgconfig.path  = $$PREFIX/lib/pkgconfig

INSTALLS += headers internal_headers elv_io elv_util euph_asset euph_config euph_io \
            euph_audio euph_image euph_memory target pkgconfig
