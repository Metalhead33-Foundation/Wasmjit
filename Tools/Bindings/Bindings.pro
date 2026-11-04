TEMPLATE = app
TARGET = wasmjit-bindings
CONFIG += console c++2a
CONFIG -= qt
# The schema is JSON; use the same header-only library as WasmBase and Test.
CONFIG += link_pkgconfig
PKGCONFIG += nlohmann_json

include(../../WasmBase/WasmBase.pri)
INCLUDEPATH += $$PWD

SOURCES += \
    main.cpp \
    BindingSchema.cpp \
    frontend/cpp/CppFrontend.cpp

HEADERS += \
    BindingSchema.hpp \
    frontend/cpp/CppFrontend.hpp

LIBS += -L$$OUT_PWD/../../WasmBase -lWasmBase
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Euphemy\'
