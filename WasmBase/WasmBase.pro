TEMPLATE = lib
TARGET = WasmBase
CONFIG += shared c++2a
CONFIG -= qt
# libffi backs the component runtime's canonical ABI closures. It is a host
# ABI library, not a wasm code generator; the runtime itself is backend-neutral.
CONFIG += link_pkgconfig
PKGCONFIG += libffi
include(WasmBase.pri)

# Highway (`libhwy`, per pkg-config: -I/usr/include -DHWY_SHARED_DEFINE): the
# vectorised SIMD helper bodies in wasm_simd_vector.* are compiled once per CPU
# target through Highway's re-inclusion machinery. The per-target ISA flags come
# from the pragmas Highway pushes around each target's namespace, *not* from
# global -m flags, so the rest of the library stays baseline code.
LIBS += -lhwy
DEFINES += HWY_SHARED_DEFINE

# Avoid build errors with no files:
SOURCES += \
        WasmArtifact.cpp \
        WasmComponent.cpp \
        WasmComponentRuntime.cpp \
        WasmComponentValidation.cpp \
        WasmException.cpp \
        WasmExn.cpp \
        WasmGcHeap.cpp \
        WasmMemory.cpp \
        WasmModule.cpp \
        WasmModuleInstance.cpp \
        WasmOpcodeDispatcher.cpp \
        WasmRegistryImportResolver.cpp \
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
        wasm_atomic.cpp \
        wasm_simd_scalar.cpp \
        wasm_simd_vector.cpp

HEADERS += \
    WasmArtifact.hpp \
    WasmComponent.hpp \
    WasmComponentModel.hpp \
    WasmComponentRuntime.hpp \
    WasmComponentSection.hpp \
    WasmComponentValidation.hpp \
    WasmContext.hpp \
    WasmException.hpp \
    WasmExn.hpp \
    WasmGcHeader.hpp \
    WasmGcHeap.hpp \
    WasmCanonicalType.hpp \
    WasmHostBridge.hpp \
    WasmImport.hpp \
    WasmMemory.hpp \
    WasmModule.hpp \
    WasmModuleInstance.hpp \
    WasmOpcode.hpp \
    WasmOpcodeDispatcher.hpp \
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
    WasmValidator.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp \
    wasm_atomic.hpp \
    wasm_simd_scalar.hpp \
    wasm_simd_vector.hpp \
    wasm_simd_ops.inc \
    WasmComponentParser.inc
