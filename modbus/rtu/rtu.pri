# Author: shpegun60
# SPDX-License-Identifier: MIT

isEmpty(MODBUS_RTU_PRI_INCLUDED) {
    MODBUS_RTU_PRI_INCLUDED = 1

    CONFIG += c++20

    MODBUS_ROOT = $$clean_path($$PWD/..)
    MODBUS_RTU_DIR = $$clean_path($$PWD)
    isEmpty(MODBUS_DELEGATE_DIR) {
        MODBUS_DELEGATE_DIR = $$clean_path($$MODBUS_ROOT/../libs/delegate)
    }

    include($$MODBUS_ROOT/../crc/crc.pri)

    INCLUDEPATH += $$MODBUS_DELEGATE_DIR

    DEPENDPATH += $$MODBUS_DELEGATE_DIR

    HEADERS += \
        $$MODBUS_ROOT/../wire/Scalar.h \
        $$MODBUS_ROOT/../wire/Read.h \
        $$MODBUS_ROOT/Types.h \
        $$MODBUS_ROOT/Pdu.h \
        $$MODBUS_ROOT/detail/BlockPool.h \
        $$MODBUS_RTU_DIR/Crc.h \
        $$MODBUS_RTU_DIR/RtuLimits.h \
        $$MODBUS_RTU_DIR/Format.h \
        $$MODBUS_RTU_DIR/Stats.h \
        $$MODBUS_RTU_DIR/Storage.h \
        $$MODBUS_RTU_DIR/Rtu.h \
        $$MODBUS_RTU_DIR/detail/Message.h \
        $$MODBUS_RTU_DIR/detail/Packet.h \
        $$MODBUS_RTU_DIR/detail/Receiver.h
}
