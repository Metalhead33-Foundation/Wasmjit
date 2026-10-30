TEMPLATE = lib
TARGET = Asmjit
# Explicit soname policy (docs/EMBEDDING_SURFACE.md §7.2/ES-4): the major is the
# soname, so a change to a frozen surface (API_ABI_STABILITY.md §2.2 S1-S9) bumps
# it. The value matches WASMJIT_VERSION_* in WasmVersion.hpp.
VERSION = 1.0.0
CONFIG += shared c++2a
CONFIG -= qt
CONFIG += link_pkgconfig
PKGCONFIG += asmjit
INCLUDEPATH += $$PWD/..
include(../WasmBase/WasmBase.pri)

# AsmJit is external and remains confined to this backend.
SOURCES += AsmjitContext.cpp
HEADERS += AsmjitContext.hpp
