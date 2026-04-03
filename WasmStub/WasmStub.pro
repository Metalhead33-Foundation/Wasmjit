TEMPLATE = lib
TARGET = WasmStub
CONFIG += shared c++2a
CONFIG -= qt
INCLUDEPATH += $$PWD/..

# Avoid build errors with no files:
SOURCES += StubOpcodeDispatcher.cpp
HEADERS += StubOpcodeDispatcher.hpp
