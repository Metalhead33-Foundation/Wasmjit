TEMPLATE = lib
TARGET = Sljit
CONFIG += shared c++2a
CONFIG -= qt
INCLUDEPATH += $$PWD/..

# Avoid build errors with no files:
SOURCES += 
HEADERS +=
