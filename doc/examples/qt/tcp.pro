# Author: shpegun60; SPDX-License-Identifier: MIT
TEMPLATE = app
TARGET = qt_tcp
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui
QT += network
DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Werror
include($$PWD/../../../src/modbus/tcp/tcp.pri)
SOURCES += $$PWD/tcp.cpp
