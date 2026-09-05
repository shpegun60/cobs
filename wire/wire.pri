# Author: shpegun60
# SPDX-License-Identifier: MIT

# Reusable qmake fragment for the shared wire layer: scalar codec, stateless
# readers and the protocol-independent storage contract with its built-in
# Heap and Pool. Header-only; cobs.pri and rtu.pri include this fragment so
# both protocols see one copy of every shared header.

isEmpty(WIRE_PRI_INCLUDED) {
    WIRE_PRI_INCLUDED = 1

    CONFIG += c++20

    WIRE_DIR = $$clean_path($$PWD)
    INCLUDEPATH += $$clean_path($$WIRE_DIR/..)
    DEPENDPATH += $$clean_path($$WIRE_DIR/..)

    HEADERS += \
        $$WIRE_DIR/Scalar.h \
        $$WIRE_DIR/Read.h \
        $$WIRE_DIR/Storage.h \
        $$WIRE_DIR/detail/BlockPool.h
}
