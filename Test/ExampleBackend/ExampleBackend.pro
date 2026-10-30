TEMPLATE = lib
CONFIG += shared plugin c++2a
CONFIG -= qt
TARGET = wasmjit-backend-example
# docs/BACKEND_PLUGIN_ABI.md §7: a plugin links the one shared core. Embedding a
# copy would give the process two stores; the loader also probes for this.
contains(CONFIG, staticlib)|contains(CONFIG, static)|!contains(CONFIG, shared): error("the example backend must link the shared libWasmBase (docs/BACKEND_PLUGIN_ABI.md §7).")

INCLUDEPATH += $$PWD/../..
include(../../WasmBase/WasmBase.pri)

LIBS += -L$$OUT_PWD/../../WasmBase -lWasmBase
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Euphemy\'

SOURCES += ExampleBackend.cpp
