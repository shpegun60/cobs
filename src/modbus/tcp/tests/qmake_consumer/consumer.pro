# Author: shpegun60; SPDX-License-Identifier: MIT
TEMPLATE = app
TARGET = modbus_tcp_consumer
CONFIG += console c++20
CONFIG -= app_bundle
QT -= gui
DESTDIR = bin
include(../../tcp.pri)
SOURCES += main.cpp
