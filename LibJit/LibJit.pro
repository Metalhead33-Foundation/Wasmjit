TEMPLATE = lib
TARGET = LibJit
# Explicit soname policy (docs/EMBEDDING_SURFACE.md §7.2/ES-4): the major is the
# soname, so a change to a frozen surface (API_ABI_STABILITY.md §2.2 S1-S9) bumps
# it. The value matches WASMJIT_VERSION_* in WasmVersion.hpp.
VERSION = 1.0.0
CONFIG += shared c++2a
CONFIG -= qt
LIBS += -ljit
INCLUDEPATH += $$PWD/..
include(../WasmBase/WasmBase.pri)

# Avoid build errors with no files:
SOURCES += \
        LibJitContext.cpp \
        LibJitBackend.cpp \
        LibJitTypeTranslation.cpp \
        LibjitModuleCompiler.cpp \
        LibjitOpcodeDispatcher.cpp

HEADERS += \
    LibJitContext.hpp \
    LibJitImportResolver.hpp \
    LibJitTypeTranslation.hpp \
    LibjitModuleCompiler.hpp \
    LibjitOpcodeDispatcher.hpp
