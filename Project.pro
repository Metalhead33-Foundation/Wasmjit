TEMPLATE = subdirs

# Define the subprojects with aliases
Elvavena.subdir = extern/MhLib/Elvavena
Euphemy.subdir  = extern/MhLib/Euphemy

# Now add those aliases (plus your other folders) to SUBDIRS
SUBDIRS = Elvavena Euphemy WasmBase WasmStub LibJit Asmjit Sljit ExampleBackend Test

# Dependencies now work because 'Elvavena' is a recognized target
WasmBase.depends = Elvavena Euphemy
WasmStub.depends = WasmBase
LibJit.depends   = WasmBase
# AsmJit currently builds native code-generation probes but is not a Wasm
# backend and is not installed. SLJIT remains an empty placeholder; neither
# target is registered as an executing backend.
Asmjit.depends   = WasmBase
Sljit.depends    = WasmBase
# A loadable example backend, built in its own directory so `Test` proves the
# plugin ABI against something the frontend was not compiled with
# (docs/BACKEND_PLUGIN_ABI.md BP-3).
ExampleBackend.subdir = Test/ExampleBackend
ExampleBackend.depends = WasmBase
Test.depends     = Elvavena Euphemy WasmBase WasmStub LibJit Asmjit ExampleBackend
