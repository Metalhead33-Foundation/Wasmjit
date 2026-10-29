TEMPLATE = lib
CONFIG += plugin c++2a
CONFIG -= qt
TARGET = wasmjit-backend-example

INCLUDEPATH += $$PWD/../..
include(../../WasmBase/WasmBase.pri)

LIBS += -L$$OUT_PWD/../../WasmBase -lWasmBase
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Euphemy\'

SOURCES += ExampleBackend.cpp
