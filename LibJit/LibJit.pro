TEMPLATE = lib
TARGET = LibJit
CONFIG += shared c++2a
CONFIG -= qt
CONFIG += link_pkgconfig
PKGCONFIG += libffi
LIBS += -ljit
INCLUDEPATH += $$PWD/..
include(../WasmBase/WasmBase.pri)

# Avoid build errors with no files:
SOURCES += \
        ComponentRuntime.cpp \
        LibJitContext.cpp \
        LibJitRuntime.cpp \
        LibJitTypeTranslation.cpp \
        LibjitModuleCompiler.cpp \
        LibjitOpcodeDispatcher.cpp

HEADERS += \
    ComponentRuntime.hpp \
    LibJitContext.hpp \
    LibJitImportResolver.hpp \
    LibJitRuntime.hpp \
    LibJitTypeTranslation.hpp \
    LibjitModuleCompiler.hpp \
    LibjitOpcodeDispatcher.hpp
