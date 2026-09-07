# Author: shpegun60
# SPDX-License-Identifier: MIT
TEMPLATE = app
TARGET = server_trace_test
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui
DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
QMAKE_CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Werror
SOURCES += $$PWD/test_server_trace.cpp
HEADERS += $$PWD/ServerTrace.h
