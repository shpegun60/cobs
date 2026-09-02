# Author: shpegun60
# SPDX-License-Identifier: MIT

# Reusable qmake fragment for the COBS library.
#
# Normal consumers include only Cobs.h. The fragment owns the complete source
# list so Decoder.cpp and Encoder.cpp are compiled exactly once by a target,
# while template implementation headers remain visible to qmake/Qt Creator.

isEmpty(COBS_PRI_INCLUDED) {
    COBS_PRI_INCLUDED = 1

    CONFIG += c++20

    COBS_LIBRARY_DIR = $$clean_path($$PWD)
    isEmpty(COBS_DELEGATE_DIR) {
        COBS_DELEGATE_DIR = $$clean_path($$COBS_LIBRARY_DIR/../libs/delegate)
    }

    INCLUDEPATH += \
        $$COBS_LIBRARY_DIR \
        $$COBS_DELEGATE_DIR

    DEPENDPATH += \
        $$COBS_LIBRARY_DIR \
        $$COBS_DELEGATE_DIR

    HEADERS += \
        $$COBS_LIBRARY_DIR/../wire/Scalar.h \
        $$COBS_LIBRARY_DIR/Cobs.h \
        $$COBS_LIBRARY_DIR/Codec.h \
        $$COBS_LIBRARY_DIR/Format.h \
        $$COBS_LIBRARY_DIR/Stats.h \
        $$COBS_LIBRARY_DIR/Storage.h \
        $$COBS_LIBRARY_DIR/detail/BlockPool.h \
        $$COBS_LIBRARY_DIR/detail/Message.h \
        $$COBS_LIBRARY_DIR/detail/Packet.h \
        $$COBS_LIBRARY_DIR/detail/Receiver.h

    SOURCES += \
        $$COBS_LIBRARY_DIR/Decoder.cpp \
        $$COBS_LIBRARY_DIR/Encoder.cpp
}
