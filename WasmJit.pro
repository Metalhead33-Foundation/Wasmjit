TEMPLATE = app
CONFIG += console c++2a
CONFIG -= app_bundle
CONFIG -= qt
LIBS += -ljit

SOURCES += \
        Base/WasmModule.cpp \
        Base/WasmSection.cpp \
        Io/EuphFile.cpp \
        Io/EuphPlatformDependentFileBase.cpp \
        LibJit/LibJitContext.cpp \
        LibJit/LibJitRuntime.cpp \
        main.cpp

HEADERS += \
    Base/WasmContext.hpp \
    Base/WasmModule.hpp \
    Base/WasmRuntime.hpp \
    Base/WasmSection.hpp \
    Base/WasmType.hpp \
    Io/ElvAllocatorBasic.hpp \
    Io/ElvContainerBasic.hpp \
    Io/ElvContinuousIterator.hpp \
    Io/ElvDataStream.hpp \
    Io/ElvEndianness.hpp \
    Io/ElvIoDevice.hpp \
    Io/ElvLEB128.hpp \
    Io/ElvMathUtil.hpp \
    Io/ElvUtilGlobals.hpp \
    Io/EuphFile.hpp \
    Io/EuphPlatformDependentFileBase.hpp \
    LibJit/LibJitContext.hpp \
    LibJit/LibJitRuntime.hpp
