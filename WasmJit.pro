TEMPLATE = app
CONFIG += console c++2a
CONFIG -= app_bundle
CONFIG -= qt
LIBS += -lbinaryen
LIBS += -lwabt
LIBS += -ljit

SOURCES += \
        LibJit/WasmLibjit.cpp \
        LibJit/WasmLibjitContext.cpp \
        main.cpp

HEADERS += \
    LibJit/WasmLibjit.hpp \
    LibJit/WasmLibjitContext.hpp \
    WasmJitBackend.hpp \
    WasmJitContext.hpp
