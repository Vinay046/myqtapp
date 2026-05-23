QT       += widgets
TARGET    = myqtapp
SOURCES  += myQtApp.cpp
RESOURCES += fonts.qrc
CONFIG   += c++11

# linux/input.h comes from linux-libc-headers in every Yocto sysroot.
# No extra LIBS or INCLUDEPATH is needed – the kernel headers directory
# is already on the cross-compiler's default search path.