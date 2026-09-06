# Author: shpegun60
# SPDX-License-Identifier: MIT

TEMPLATE = app
CONFIG += console c++20
CONFIG -= app_bundle qt
TARGET = modbus_rtu_consumer

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj

include($$PWD/../../rtu.pri)

SOURCES += $$PWD/main.cpp
