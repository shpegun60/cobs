# Author: shpegun60
# SPDX-License-Identifier: MIT

# The PC side of the hardware comparison against QtSerialBus: Qt's
# QModbusRtuSerialClient or this repository's RtuClient against the board's
# reference server, and Qt's QModbusRtuSerialServer for the board's client.

TEMPLATE = app
TARGET = qmodbus_bench
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui
QT += serialbus

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc

QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow

include($$PWD/../../qt.pri)
include($$PWD/../../../../modbus/rtu/rtu.pri)

INCLUDEPATH += $$PWD/../../../../modbus/rtu/tests

SOURCES += $$PWD/main.cpp
