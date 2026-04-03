TEMPLATE = lib
TARGET = Io
CONFIG += shared c++2a
CONFIG -= qt
INCLUDEPATH += $$PWD/..

# Avoid build errors with no files:
SOURCES += \
	EuphConstBufferDevice.cpp \
	EuphFile.cpp \
	EuphMemoryDevice.cpp \
	EuphPlatformDependentFileBase.cpp \
	EuphPmrMemoryDevice.cpp

HEADERS += \
    ElvAllocatorBasic.hpp \
    ElvContainerBasic.hpp \
    ElvContinuousIterator.hpp \
    ElvDataStream.hpp \
    ElvEndianness.hpp \
    ElvIoDevice.hpp \
    ElvMathUtil.hpp \
    ElvStringhashMap.hpp \
    ElvUtilGlobals.hpp \
    EuphConstBufferDevice.hpp \
    EuphFile.hpp \
    EuphMemoryDevice.hpp \
    EuphPlatformDependentFileBase.hpp \
    EuphPmrMemoryDevice.hpp
