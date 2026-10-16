TEMPLATE = lib
TARGET = WasmBase
CONFIG += shared c++2a
CONFIG -= qt
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
        WasmException.cpp \
        WasmGcHeap.cpp \
        WasmMemory.cpp \
        WasmModule.cpp \
        WasmModuleInstance.cpp \
        WasmOpcodeDispatcher.cpp \
        WasmRegistryImportResolver.cpp \
        WasmSection.cpp \
        WasmStore.cpp \
        WasmTable.cpp \
        WasmTrap.cpp \
        WasmType.cpp \
        WasmTypeRegistry.cpp \
        wasm_simd_scalar.cpp \
        wasm_simd_vector.cpp

HEADERS += \
    WasmContext.hpp \
    WasmException.hpp \
    WasmGcHeader.hpp \
    WasmGcHeap.hpp \
    WasmCanonicalType.hpp \
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
    WasmStore.hpp \
    WasmTable.hpp \
    WasmTrap.hpp \
    WasmType.hpp \
    WasmTypeIdentity.hpp \
    WasmTypeRegistry.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp \
    wasm_simd_scalar.hpp \
    wasm_simd_vector.hpp
