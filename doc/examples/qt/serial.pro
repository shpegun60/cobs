# Author: shpegun60; SPDX-License-Identifier: MIT
TEMPLATE = app
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui
DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Werror
include($$PWD/../../../src/adapters/qt/qt.pri)
include($$PWD/../../../src/cobs/cobs.pri)
include($$PWD/../../../src/modbus/rtu/rtu.pri)
equals(MODE, rtu) {
    TARGET = qt_rtu
    DEFINES += EXAMPLE_RTU=1
} else {
    TARGET = qt_cobs
}
HEADERS += $$PWD/LoopPort.h
SOURCES += $$PWD/serial.cpp
