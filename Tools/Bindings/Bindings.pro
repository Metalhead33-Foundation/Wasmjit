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
    BindingModel.cpp \
    BindingSchema.cpp \
    Emit.cpp \
    EmitShared.cpp \
    EmitRust.cpp \
    EmitWeb.cpp \
    EmitManaged.cpp \
    EmitDynamic.cpp \
    EmitCpp.cpp \
    frontend/cpp/CppFrontend.cpp \
    frontend/wit/WitFrontend.cpp

HEADERS += \
    BindingModel.hpp \
    BindingSchema.hpp \
    EmitInternal.hpp \
    EmitShared.hpp \
    frontend/cpp/CppFrontend.hpp \
    frontend/wit/WitFrontend.hpp

LIBS += -L$$OUT_PWD/../../WasmBase -lWasmBase
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../WasmBase\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Elvavena\'
QMAKE_LFLAGS += -Wl,-rpath,\'\$$ORIGIN/../../extern/MhLib/Euphemy\'
