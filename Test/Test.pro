TEMPLATE = app
CONFIG += console c++2a
CONFIG -= qt
# The spec-script reader and the diagnostics JSON assertions use nlohmann_json
# (header-only; the same dependency WasmBase declares).
CONFIG += link_pkgconfig
PKGCONFIG += nlohmann_json

TARGET = WasmJit

WASM_BUILD_OUTPUT_DIR = $$OUT_PWD/wasm_test_modules
WASM_TESTSUITE_BUILD_DIR = $$OUT_PWD/wasm_testsuite
DEFINES += WASM_TESTSUITE_DIR=\\\"$$WASM_TESTSUITE_BUILD_DIR/\\\"
DEFINES += WASM_TESTSUITE_SOURCE_DIR=\\\"$$PWD/\\\"

DEFINES += WASM_TEST_DIR=\\\"$$WASM_BUILD_OUTPUT_DIR/\\\"
# This tells the loader where to find the .so files at runtime
# $ORIGIN represents the directory containing the WasmJit executable
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../WasmStub\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../LibJit\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../Asmjit\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../Sljit\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Euphemy\'

# Link to our local libraries
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
LIBS += -L$$OUT_PWD/../WasmStub -lWasmStub
LIBS += -L$$OUT_PWD/../LibJit -lLibJit
LIBS += -L$$OUT_PWD/../Asmjit -lAsmjit
LIBS += -L$$OUT_PWD/../Sljit -lSljit
# AsmJit's install (equery f asmjit) ships headers, `libasmjit.so` and a CMake
# config, but no `asmjit.pc` — a pkg-config dependency would fail configure.
# The include directory and library live in the platform defaults, so link the
# library directly. The test TUs include `<asmjit/...>` themselves
# (TestBackend.cpp), hence the direct dependency on top of `-lAsmjit`.
LIBS += -lasmjit
LIBS += -ljit
LIBS += -lCatch2 -lCatch2Main
include(../WasmBase/WasmBase.pri)

INCLUDEPATH += $$PWD/..
INCLUDEPATH += $$PWD/../Tools/Bindings

# Compile every AssemblyScript test module found under Test/wasm into its own .wasm output
# using a single shared node_modules install under Test/, not one per module.
AS_SOURCES = $$files($$PWD/wasm/*.ts)

as_compiler.name = AssemblyScript ${QMAKE_FILE_IN}
as_compiler.input = AS_SOURCES
as_compiler.output = $$WASM_BUILD_OUTPUT_DIR/${QMAKE_FILE_BASE}.wasm
as_compiler.commands = $$QMAKE_MKDIR $$WASM_BUILD_OUTPUT_DIR && cd $$PWD && npx asc $$PWD/wasm/${QMAKE_FILE_BASE}.ts -o $$WASM_BUILD_OUTPUT_DIR/${QMAKE_FILE_BASE}.wasm --optimize --runtime stub --noAssert --initialMemory 1
as_compiler.dependency_type = TYPE_C
as_compiler.CONFIG += no_link target_predeps

QMAKE_EXTRA_COMPILERS += as_compiler

# ---------------------------------------------------------------------------
# Official WebAssembly spec test suite (checked out under extern/WasmTestsuite)
#
# Every *.wast script is converted with `wasm-tools json-from-wast` into
# <name>.json plus the <name>.<N>.wasm modules it references. All generated
# files land in the shadow build directory, never in the source tree.
#
# Requires `wasm-tools` on PATH at build time. Override the tool with:
#     qmake Test.pro WASM_TOOLS=/path/to/wasm-tools
# ---------------------------------------------------------------------------
isEmpty(WASM_TOOLS) {
    WASM_TOOLS = wasm-tools
}

# Upstream `type-subtyping.wast` uses a script construct that wasm-tools
# refuses to parse, so drop it: one unparsable file must not break the build.
WAST_SOURCES = $$files($$PWD/../extern/WasmTestsuite/*.wast)
WAST_SOURCES -= $$PWD/../extern/WasmTestsuite/type-subtyping.wast
# The threads proposal's atomic semantics test lives in a subdirectory, so the
# top-level glob does not see it. It is a *single-agent* script (one shared
# memory, no second thread), which matches this engine's execution model — see
# docs/proposals/THREADS.md TH-1/TD9. Listed explicitly rather than globbing the
# whole `proposals/` tree, whose other scripts are for features we do not ship.
WAST_SOURCES += $$PWD/../extern/WasmTestsuite/proposals/threads/atomic.wast

wast2json.name = wasm-tools json-from-wast ${QMAKE_FILE_IN}
wast2json.input = WAST_SOURCES
wast2json.output = $$WASM_TESTSUITE_BUILD_DIR/${QMAKE_FILE_BASE}.json
wast2json.commands = $$QMAKE_MKDIR $$WASM_TESTSUITE_BUILD_DIR && $$WASM_TOOLS json-from-wast ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT} --wasm-dir $$WASM_TESTSUITE_BUILD_DIR
wast2json.dependency_type = TYPE_C
wast2json.CONFIG += no_link target_predeps

QMAKE_EXTRA_COMPILERS += wast2json

# ---------------------------------------------------------------------------
# Multi-memory / imported-memory test modules are authored in WAT. They are
# parsed with the same `wasm-tools` used above and produced in the shadow build
# directory, never in the source tree.
# ---------------------------------------------------------------------------
WAT_SOURCES = $$files($$PWD/wasm_wat/*.wat)

wat2wasm.name = wasm-tools parse ${QMAKE_FILE_IN}
wat2wasm.input = WAT_SOURCES
wat2wasm.output = $$WASM_BUILD_OUTPUT_DIR/${QMAKE_FILE_BASE}.wasm
wat2wasm.commands = $$QMAKE_MKDIR $$WASM_BUILD_OUTPUT_DIR && $$WASM_TOOLS parse ${QMAKE_FILE_IN} -o ${QMAKE_FILE_OUT}
wat2wasm.dependency_type = TYPE_C
wat2wasm.CONFIG += no_link target_predeps

QMAKE_EXTRA_COMPILERS += wat2wasm

# ---------------------------------------------------------------------------
# CM-9a: the component-model reference corpus (extern/component-model/test)
#
# That corpus is authored for `wasmtime wast`, so `json-from-wast` cannot read it
# (core parser). Test/component_corpus.py extracts every `(component ...)` form,
# writes it through `wasm-tools parse` (which does understand components) and
# emits a verdict manifest; Test/ComponentCorpusTests.cpp asserts our validator
# against it (CM-9b). Everything lands under $$OUT_PWD/component-corpus.
# ---------------------------------------------------------------------------
COMPONENT_CORPUS_DIR = $$WASM_TESTSUITE_BUILD_DIR/component-corpus

component_corpus.target = $$COMPONENT_CORPUS_DIR/manifest.tsv
component_corpus.commands = $$QMAKE_MKDIR $$COMPONENT_CORPUS_DIR && \
    python3 $$PWD/component_corpus.py --corpus $$PWD/../extern/component-model/test \
        --tools $$WASM_TOOLS --out $$COMPONENT_CORPUS_DIR \
        --known-failures $$PWD/component_known_failures.txt
component_corpus.depends = $$PWD/component_corpus.py
PRE_TARGETDEPS += $$COMPONENT_CORPUS_DIR/manifest.tsv
QMAKE_EXTRA_TARGETS += component_corpus

SOURCES += main.cpp \
    helper.cpp \
    Diagnose.cpp \
    TestBackend.cpp \
    LibJitBackendTests.cpp \
    SljitBackendTests.cpp \
    WastScript.cpp \
    SpecSuite.cpp \
    ProposalDocsTests.cpp \
    ComponentCorpusTests.cpp \
    LayeringTests.cpp \
    HeaderDocTests.cpp \
    SurfaceTests.cpp \
    BackendAbiTests.cpp \
    RuntimeTests.cpp \
    ConcurrencyTests.cpp \
    PluginTests.cpp \
    HeaderTierTests.cpp \
    AsmjitBackendTests.cpp \
    AsmjitTypeTranslatorTests.cpp \
    AsmjitModuleCompilerTests.cpp \
    LegacyEhTests.cpp \
    MemoryProviderTests.cpp \
    ConfigurationTests.cpp \
    BranchHintTests.cpp \
    ../Tools/Bindings/BindingModel.cpp \
    ../Tools/Bindings/BindingSchema.cpp \
    ../Tools/Bindings/Emit.cpp \
    ../Tools/Bindings/EmitShared.cpp \
    ../Tools/Bindings/EmitRust.cpp \
    ../Tools/Bindings/EmitWeb.cpp \
    ../Tools/Bindings/EmitManaged.cpp \
    ../Tools/Bindings/EmitDynamic.cpp \
    ../Tools/Bindings/EmitCpp.cpp \
    ../Tools/Bindings/frontend/cpp/CppFrontend.cpp \
    ../Tools/Bindings/frontend/wit/WitFrontend.cpp \
    BindingSchemaTests.cpp \
    CallCacheTests.cpp
HEADERS +=  \
    helper.hpp \
    Diagnose.hpp \
    TestBackend.hpp \
    TestStore.hpp \
    TestFixtures.hpp \
    AgentFixture.hpp \
    WastScript.hpp

# One translation unit per embedder-tier header. Each file includes exactly one
# header, so a header that stops compiling standalone fails the build instead of
# an embedder's project (docs/EMBEDDING_SURFACE.md §6.1, `ES-3`).
SOURCES += $$files($$PWD/headers/*.cpp)

DISTFILES += \
    package-lock.json \
    package.json
