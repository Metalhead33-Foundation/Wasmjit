TEMPLATE = app
CONFIG += console c++2a
CONFIG -= qt

TARGET = WasmJit

# Link to our local libraries
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
LIBS += -L$$OUT_PWD/../WasmStub -lWasmStub
LIBS += -L$$OUT_PWD/../LibJit -lLibJit
LIBS += -ljit
include(../WasmBase/WasmBase.pri)

INCLUDEPATH += $$PWD/..

SOURCES += main.cpp
HEADERS += 
