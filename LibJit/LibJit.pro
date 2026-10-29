TEMPLATE = lib
TARGET = LibJit
# Explicit soname policy (docs/EMBEDDING_SURFACE.md §7.2/ES-4): the major is the
# soname, so a change to a frozen surface (API_ABI_STABILITY.md §2.2 S1-S9) bumps
# it. The value matches WASMJIT_VERSION_* in WasmVersion.hpp.
VERSION = 1.0.0
CONFIG += shared c++2a
CONFIG -= qt
LIBS += -ljit
INCLUDEPATH += $$PWD/..
include(../WasmBase/WasmBase.pri)

# ── Exactly one core image ──────────────────────────────────────────────────
# docs/BACKEND_PLUGIN_ABI.md §7 (blocker 2): the store, the type registry and the
# GC heap live in libWasmBase, so a backend must *depend on* that library, never
# embed a copy of it. Linking the shared core records `NEEDED libWasmBase.so.1`
# in this backend, which is what makes the guarantee a property of the dynamic
# linker rather than a convention:
#   * a static archive of the core is a configure-time error (below), and
#   * `--no-undefined` turns a core symbol that is *not* satisfied by the linked
#     core into a link error instead of a load-time surprise.
contains(CONFIG, staticlib)|contains(CONFIG, static)|!contains(CONFIG, shared): error("libLibJit must be linked against the shared libWasmBase: a static core lets a backend embed a second store and type registry (docs/BACKEND_PLUGIN_ABI.md §7).")
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
PRE_TARGETDEPS += $$OUT_PWD/../WasmBase/libWasmBase.so
QMAKE_LFLAGS += -Wl,--no-undefined


# Install the code generator next to the core (docs/EMBEDDING_SURFACE.md ES-5).
isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/lib
INSTALLS += target

# Avoid build errors with no files:
SOURCES += \
        LibJitContext.cpp \
        LibJitBackend.cpp \
        LibJitTypeTranslation.cpp \
        LibjitModuleCompiler.cpp \
        LibjitOpcodeDispatcher.cpp

HEADERS += \
    LibJitContext.hpp \
    LibJitImportResolver.hpp \
    LibJitTypeTranslation.hpp \
    LibjitModuleCompiler.hpp \
    LibjitOpcodeDispatcher.hpp
