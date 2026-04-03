# WasmBase/WasmBase.pri
INCLUDEPATH += $$PWD/..  # Allows #include <WasmBase/Header.hpp>

# Automatically pull in dependencies
include(../extern/MhLib/Elvavena/Elvavena.pri)
include(../extern/MhLib/Euphemy/Euphemy.pri)
