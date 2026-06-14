TEMPLATE = app
CONFIG += console c++2a
CONFIG -= qt

TARGET = WasmJit

WASM_BUILD_OUTPUT_DIR = $$OUT_PWD/wasm_test_modules
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
include(../WasmBase/WasmBase.pri)

INCLUDEPATH += $$PWD/..

# Compile every AssemblyScript test module found under Test/wasm into its own .wasm output
# using a single shared node_modules install under Test/, not one per module.
AS_SOURCES = $$files($$PWD/wasm/*.ts)

as_compiler.name = AssemblyScript ${QMAKE_FILE_IN}
as_compiler.input = AS_SOURCES
as_compiler.output = $$WASM_BUILD_OUTPUT_DIR/${QMAKE_FILE_BASE}.wasm
as_compiler.commands = $$QMAKE_MKDIR $$WASM_BUILD_OUTPUT_DIR && cd $$PWD && npx asc $$PWD/wasm/${QMAKE_FILE_BASE}.ts -o $$WASM_BUILD_OUTPUT_DIR/${QMAKE_FILE_BASE}.wasm --optimize --runtime stub --noAssert
as_compiler.dependency_type = TYPE_C
as_compiler.CONFIG += no_link target_predeps

QMAKE_EXTRA_COMPILERS += as_compiler

SOURCES += main.cpp
HEADERS += 

DISTFILES += \
    package-lock.json \
    package.json
