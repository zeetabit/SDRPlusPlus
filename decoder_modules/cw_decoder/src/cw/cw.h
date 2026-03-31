#pragma once
#include <string>
#include <stdint.h>
#include <utils/new_event.h>

#define CW_SYNC_DIST            4
#define CW_BATCH_CODEWORD_COUNT 16

namespace cw {
    enum CodewordType {
        CODEWORD_TYPE_IDLE      = -1,
        CODEWORD_TYPE_ADDRESS   = 0,
        CODEWORD_TYPE_MESSAGE   = 1
    };

    enum MessageType {
        MESSAGE_TYPE_NUMERIC        = 0b00,
        MESSAGE_TYPE_ALPHANUMERIC   = 0b11
    };

    using Codeword = uint32_t;
    using Address = uint32_t;

    class Decoder {
    public:
        Decoder();

        void process(uint8_t* symbols, int count);

        NewEvent<Address, MessageType, const std::string&> onMessage;

    private:
        void flushMessage();
        void decodeBatch();

        uint32_t syncSR = 0;
        bool synced = false;
        int batchOffset = 0;

        Codeword batch[CW_BATCH_CODEWORD_COUNT];

        Address addr;
        MessageType msgType;
        std::string msg;

        char currChar = 0;
        int currOffset = 0;
    };
}