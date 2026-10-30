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
# `Sljit/` is a provisional, executing backend: it links the shared core the
# same way LibJit does and refuses what it does not lower
# (docs/WRITING_A_BACKEND.md §9). `Asmjit/` remains a reserved, empty subproject
# that emits no code yet and installs nothing.
Asmjit.depends   = WasmBase
Sljit.depends    = WasmBase
# A loadable example backend, built in its own directory so `Test` proves the
# plugin ABI against something the frontend was not compiled with
# (docs/BACKEND_PLUGIN_ABI.md BP-3).
ExampleBackend.subdir = Test/ExampleBackend
ExampleBackend.depends = WasmBase
Test.depends     = Elvavena Euphemy WasmBase WasmStub LibJit Sljit ExampleBackend
