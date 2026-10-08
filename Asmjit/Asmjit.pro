TEMPLATE = lib
TARGET = Asmjit
CONFIG += shared c++2a
CONFIG -= qt
INCLUDEPATH += $$PWD/..
include(../WasmBase/WasmBase.pri)

# Avoid build errors with no files:
SOURCES += 
HEADERS +=
