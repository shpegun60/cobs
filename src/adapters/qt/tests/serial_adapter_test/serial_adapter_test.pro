# Author: shpegun60
# SPDX-License-Identifier: MIT

# adapters/qt/SerialAdapter.h on a QIODevice stand-in for QSerialPort: no COM
# port, no hardware, an event loop and the real endpoints.

TEMPLATE = app
TARGET = serial_adapter_test
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc

QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow

include($$PWD/../../qt.pri)
include($$PWD/../../../../modbus/rtu/rtu.pri)
include($$PWD/../../../../cobs/cobs.pri)

INCLUDEPATH += $$PWD/../../../../modbus/rtu/tests

SOURCES += $$PWD/serial_adapter_test.cpp
