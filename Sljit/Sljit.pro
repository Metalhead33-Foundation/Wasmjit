TEMPLATE = lib
TARGET = Sljit
# Explicit soname policy (docs/EMBEDDING_SURFACE.md §7.2/ES-4): the major is the
# soname, so a change to a frozen surface (API_ABI_STABILITY.md §2.2 S1-S9) bumps
# it. The value matches WASMJIT_VERSION_* in WasmVersion.hpp.
VERSION = 1.0.0
CONFIG += shared c++2a
CONFIG -= qt
# `BackendSession::invoke` marshals a call through libffi, the same dependency
# the core already uses for the component canonical ABI (WasmBase.pro).
CONFIG += link_pkgconfig
PKGCONFIG += libffi
INCLUDEPATH += $$PWD/.. $$PWD/../extern/sljit/sljit_src
include(../WasmBase/WasmBase.pri)

# ── The vendored SLJIT sources ─────────────────────────────────────────────
# sljit is a submodule under extern/, compiled *into* this library, so there is
# no external `-lsljit` and no second copy of the emitter in the process.
# `sljitLir.c` includes `sljitUtils.c`, the allocator and (selected by CPU) the
# one native emitter; compiling any of those again duplicates their symbols.
#
# W^X: the default allocator asks the host for read-write-execute pages. The WX
# allocator writes a block while it is read-write and flips it to read-execute
# once SLJIT has finished generating, which is the handoff
# docs/BACKEND_WORK_ORDER.md §5.5 requires. The cost is one mapping per
# generated block instead of a pooled code arena, which is acceptable for a
# provisional backend (docs/WRITING_A_BACKEND.md §9).
DEFINES += SLJIT_WX_EXECUTABLE_ALLOCATOR=1

SOURCES += \
        $$PWD/../extern/sljit/sljit_src/sljitLir.c \
        SljitBackend.cpp \
        SljitBackendSession.cpp \
        SljitModuleCompiler.cpp \
        SljitOpcodeDispatcher.cpp

HEADERS += \
    SljitBackend.hpp \
    SljitBackendSession.hpp \
    SljitModuleCompiler.hpp \
    SljitOpcodeDispatcher.hpp

# ── Exactly one core image ──────────────────────────────────────────────────
# docs/BACKEND_PLUGIN_ABI.md §7 (blocker 2): the store, the type registry and the
# GC heap live in libWasmBase, so a backend must *depend on* that library, never
# embed a copy of it. Linking the shared core records `NEEDED libWasmBase.so.1`
# in this backend, which is what makes the guarantee a property of the dynamic
# linker rather than a convention:
#   * a static archive of the core is a configure-time error (below), and
#   * `--no-undefined` turns a core symbol that is *not* satisfied by the linked
#     core into a link error instead of a load-time surprise.
contains(CONFIG, staticlib)|contains(CONFIG, static)|!contains(CONFIG, shared): error("libSljit must be linked against the shared libWasmBase: a static core lets a backend embed a second store and type registry (docs/BACKEND_PLUGIN_ABI.md §7).")
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
PRE_TARGETDEPS += $$OUT_PWD/../WasmBase/libWasmBase.so
QMAKE_LFLAGS += -Wl,--no-undefined

# Install the code generator next to the core (docs/EMBEDDING_SURFACE.md ES-5).
isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/lib
INSTALLS += target
