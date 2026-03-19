TEMPLATE = app
CONFIG += console c++2a
CONFIG -= app_bundle
CONFIG -= qt
LIBS += -ljit

SOURCES += \
        Base/WasmException.cpp \
        Base/WasmModule.cpp \
        Base/WasmModuleInstance.cpp \
        Base/WasmOpcodeDispatcher.cpp \
        Base/WasmSection.cpp \
        Base/WasmType.cpp \
        Io/EuphConstBufferDevice.cpp \
        Io/EuphFile.cpp \
        Io/EuphMemoryDevice.cpp \
        Io/EuphPlatformDependentFileBase.cpp \
        Io/EuphPmrMemoryDevice.cpp \
        LibJit/LibJitContext.cpp \
        LibJit/LibJitRuntime.cpp \
        LibJit/LibJitTypeTranslation.cpp \
        LibJit/LibjitModuleCompiler.cpp \
        main.cpp

HEADERS += \
    Base/WasmContext.hpp \
    Base/WasmException.hpp \
    Base/WasmImport.hpp \
    Base/WasmModule.hpp \
    Base/WasmModuleInstance.hpp \
    Base/WasmOpcode.hpp \
    Base/WasmOpcodeDispatcher.hpp \
    Base/WasmRuntime.hpp \
    Base/WasmSection.hpp \
    Base/WasmType.hpp \
    Base/WasmVMContext.hpp \
    Base/WasmValue.hpp \
    Io/ElvAllocatorBasic.hpp \
    Io/ElvContainerBasic.hpp \
    Io/ElvContinuousIterator.hpp \
    Io/ElvDataStream.hpp \
    Io/ElvEndianness.hpp \
    Io/ElvIoDevice.hpp \
    Io/ElvMathUtil.hpp \
    Io/ElvUtilGlobals.hpp \
    Io/EuphConstBufferDevice.hpp \
    Io/EuphFile.hpp \
    Io/EuphMemoryDevice.hpp \
    Io/EuphPlatformDependentFileBase.hpp \
    Io/EuphPmrMemoryDevice.hpp \
    LibJit/LibJitContext.hpp \
    LibJit/LibJitRuntime.hpp \
    LibJit/LibJitTypeTranslation.hpp \
    LibJit/LibjitModuleCompiler.hpp
