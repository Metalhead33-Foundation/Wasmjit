TEMPLATE = subdirs

# Order matters
SUBDIRS = Io WasmBase WasmStub LibJit Asmjit Sljit Test

WasmBase.depends = Io
WasmStub.depends = WasmBase
LibJit.depends = WasmBase
Asmjit.depends = WasmBase
Sljit.depends = WasmBase
Test.depends = Io WasmBase WasmStub LibJit
