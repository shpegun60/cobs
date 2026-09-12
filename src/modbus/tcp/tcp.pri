# Author: shpegun60; SPDX-License-Identifier: MIT
isEmpty(MODBUS_TCP_PRI_INCLUDED) {
    MODBUS_TCP_PRI_INCLUDED = 1
    CONFIG += c++20
    MODBUS_ROOT = $$clean_path($$PWD/..)
    MODBUS_TCP_DIR = $$clean_path($$PWD)
    isEmpty(MODBUS_DELEGATE_DIR) {
        MODBUS_DELEGATE_DIR = $$clean_path($$MODBUS_ROOT/../../libs/delegate)
    }
    include($$MODBUS_ROOT/../wire/wire.pri)
    include($$MODBUS_ROOT/../crc/crc.pri)
    INCLUDEPATH += $$MODBUS_DELEGATE_DIR
    DEPENDPATH += $$MODBUS_DELEGATE_DIR
    HEADERS += \
        $$MODBUS_ROOT/Types.h \
        $$MODBUS_ROOT/Pdu.h \
        $$MODBUS_TCP_DIR/Format.h \
        $$MODBUS_TCP_DIR/Stats.h \
        $$MODBUS_TCP_DIR/Tcp.h \
        $$MODBUS_TCP_DIR/detail/Message.h \
        $$MODBUS_TCP_DIR/detail/Packet.h \
        $$MODBUS_TCP_DIR/detail/Receiver.h \
        $$MODBUS_TCP_DIR/detail/RxBlock.h
}
