# Author: shpegun60
# SPDX-License-Identifier: MIT

isEmpty(CRC_PRI_INCLUDED) {
    CRC_PRI_INCLUDED = 1

    CONFIG += c++20

    CRC_DIR = $$clean_path($$PWD)
    INCLUDEPATH += $$clean_path($$CRC_DIR/..)
    DEPENDPATH += $$clean_path($$CRC_DIR/..)

    HEADERS += $$CRC_DIR/Crc.h
}
