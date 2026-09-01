QT += widgets

CONFIG += c++20

INCLUDEPATH += libs/spsc/src

# Compile and expose the real COBS core in the host project. The separate
# qmake consumer under cobs/tests/qmake_consumer also instantiates the complete
# Endpoint API over both built-in storage strategies.
include(cobs/cobs.pri)

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    mainwindow.cpp

HEADERS += \
    mainwindow.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
