TEMPLATE = lib
TARGET = LibJit
CONFIG += shared c++2a
CONFIG -= qt
LIBS += -ljit
INCLUDEPATH += $$PWD/..

# Avoid build errors with no files:
SOURCES += \
        LibJitContext.cpp \
        LibJitRuntime.cpp \
        LibJitTypeTranslation.cpp \
        LibjitModuleCompiler.cpp \
        LibjitOpcodeDispatcher.cpp

HEADERS += \
    LibJitContext.hpp \
    LibJitImportResolver.hpp \
    LibJitRuntime.hpp \
    LibJitTypeTranslation.hpp \
    LibjitModuleCompiler.hpp \
    LibjitOpcodeDispatcher.hpp
