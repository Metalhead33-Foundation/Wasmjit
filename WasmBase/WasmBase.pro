TEMPLATE = lib
TARGET = WasmBase
CONFIG += shared c++2a
CONFIG -= qt
include(WasmBase.pri)

# Avoid build errors with no files:
SOURCES += \
        WasmException.cpp \
        WasmMemory.cpp \
        WasmModule.cpp \
        WasmModuleInstance.cpp \
        WasmOpcodeDispatcher.cpp \
        WasmRegistryImportResolver.cpp \
        WasmSection.cpp \
        WasmStore.cpp \
        WasmTable.cpp \
        WasmType.cpp \
        WasmTypeRegistry.cpp

HEADERS += \
    WasmContext.hpp \
    WasmException.hpp \
    WasmCanonicalType.hpp \
    WasmImport.hpp \
    WasmMemory.hpp \
    WasmModule.hpp \
    WasmModuleInstance.hpp \
    WasmOpcode.hpp \
    WasmOpcodeDispatcher.hpp \
    WasmRegistryImportResolver.hpp \
    WasmRuntime.hpp \
    WasmSection.hpp \
    WasmStore.hpp \
    WasmTable.hpp \
    WasmType.hpp \
    WasmTypeIdentity.hpp \
    WasmTypeRegistry.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp
