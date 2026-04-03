TEMPLATE = lib
TARGET = WasmBase
CONFIG += shared c++2a
CONFIG -= qt
include(WasmBase.pri)

# Avoid build errors with no files:
SOURCES += \
        WasmException.cpp \
        WasmModule.cpp \
        WasmModuleInstance.cpp \
        WasmOpcodeDispatcher.cpp \
        WasmRegistryImportResolver.cpp \
        WasmSection.cpp \
        WasmType.cpp

HEADERS += \
    WasmContext.hpp \
    WasmException.hpp \
    WasmImport.hpp \
    WasmModule.hpp \
    WasmModuleInstance.hpp \
    WasmOpcode.hpp \
    WasmOpcodeDispatcher.hpp \
    WasmRegistryImportResolver.hpp \
    WasmRuntime.hpp \
    WasmSection.hpp \
    WasmType.hpp \
    WasmVMContext.hpp \
    WasmValue.hpp
