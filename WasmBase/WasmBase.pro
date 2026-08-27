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
        WasmType.cpp

HEADERS += \
    WasmContext.hpp \
    WasmException.hpp \
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
    WasmType.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp
