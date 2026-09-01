# Author: shpegun60
# SPDX-License-Identifier: MIT

TEMPLATE = app
TARGET = cobs_pri_consumer

CONFIG += console c++20
CONFIG -= app_bundle qt

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj

include(../../cobs.pri)

SOURCES += main.cpp
