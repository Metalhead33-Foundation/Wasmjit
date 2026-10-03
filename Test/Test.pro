TEMPLATE = app
CONFIG += console c++2a
CONFIG -= qt

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
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../extern/MhLib/Euphemy\'

# Link to our local libraries
LIBS += -L$$OUT_PWD/../WasmBase -lWasmBase
LIBS += -L$$OUT_PWD/../WasmStub -lWasmStub
LIBS += -L$$OUT_PWD/../LibJit -lLibJit
LIBS += -ljit
LIBS += -lCatch2 -lCatch2Main
include(../WasmBase/WasmBase.pri)

INCLUDEPATH += $$PWD/..

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

SOURCES += main.cpp \
    helper.cpp \
    WastScript.cpp \
    SpecSuite.cpp
HEADERS +=  \
    helper.hpp \
    WastScript.hpp

DISTFILES += \
    package-lock.json \
    package.json
