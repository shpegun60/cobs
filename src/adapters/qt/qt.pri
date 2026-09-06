# Author: shpegun60
# SPDX-License-Identifier: MIT

# Qt integration: adapters/qt/SerialAdapter.h binds a QSerialPort to a
# cobs::Endpoint or a framed modbus::rtu::Endpoint, and adapters/qt/RtuClient.h
# is the QModbus-shaped master on top of it. Include this fragment next to
# cobs.pri and/or rtu.pri; it adds the SerialPort module and the src include
# path, nothing else.

isEmpty(ADAPTERS_QT_PRI_INCLUDED) {
    ADAPTERS_QT_PRI_INCLUDED = 1

    CONFIG += c++20
    QT += serialport

    ADAPTERS_QT_DIR = $$clean_path($$PWD)
    isEmpty(ADAPTERS_DELEGATE_DIR) {
        ADAPTERS_DELEGATE_DIR = $$clean_path($$ADAPTERS_QT_DIR/../../../libs/delegate)
    }

    INCLUDEPATH += \
        $$clean_path($$ADAPTERS_QT_DIR/../..) \
        $$ADAPTERS_DELEGATE_DIR

    DEPENDPATH += $$ADAPTERS_QT_DIR

    HEADERS += \
        $$ADAPTERS_QT_DIR/SerialAdapter.h \
        $$ADAPTERS_QT_DIR/RtuClient.h
}
