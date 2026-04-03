TEMPLATE = app
CONFIG += console c++2a
CONFIG -= qt

TARGET = WasmJit
# This tells the loader where to find the .so files at runtime
# $ORIGIN represents the directory containing the WasmJit executable
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../WasmStub\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../LibJit\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Euphemy\'

# Link to our local libraries
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
LIBS += -L$$OUT_PWD/../WasmStub -lWasmStub
LIBS += -L$$OUT_PWD/../LibJit -lLibJit
LIBS += -ljit
include(../WasmBase/WasmBase.pri)

INCLUDEPATH += $$PWD/..

SOURCES += main.cpp
HEADERS += 
