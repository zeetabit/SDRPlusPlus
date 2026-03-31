#include "cw.h"
#include <string.h>
#include <utils/flog.h>

namespace cw {
    Decoder::Decoder() {
        // Zero out batch
        memset(batch, 0, sizeof(batch));
    }

    void Decoder::process(uint8_t* symbols, int count) {
        for (int i = 0; i < count; i++);
    }

    void Decoder::flushMessage() {
        if (!msg.empty()) {
            // Send out message
            onMessage(addr, msgType, msg);

            // Reset state
            msg.clear();
            currChar = 0;
            currOffset = 0;
        }
    }

    void printbin(uint32_t cw) {
        for (int i = 31; i >= 0; i--) {
            printf("%c", ((cw >> i) & 1) ? '1':'0');
        }
    }

    void bitswapChar(char in, char& out) {
        out = 0;
        for (int i = 0; i < 7; i++) {
            out |= ((in >> (6-i)) & 1) << i;
        }
    }

    void Decoder::decodeBatch() {
        for (int i = 0; i < CW_BATCH_CODEWORD_COUNT; i++) {
            // Get codeword
            Codeword cw = batch[i];

            // Get codeword type
            CodewordType type = (CodewordType)((cw >> 31) & 1);
//            if (type == CODEWORD_TYPE_ADDRESS && (cw >> 11) == CW_IDLE_CODEWORD_DATA) {
//                type = CODEWORD_TYPE_IDLE;
//            }
//
//            // Decode codeword
//            if (type == CODEWORD_TYPE_IDLE) {
//                // If a non-empty message is available, send it out and clear
//                flushMessage();
//            }
//            else if (type == CODEWORD_TYPE_ADDRESS) {
//                // If a non-empty message is available, send it out and clear
//                flushMessage();
//
//                // Decode message type
//                msgType = MESSAGE_TYPE_ALPHANUMERIC;
//                // msgType = (MessageType)((cw >> 11) & 0b11);
//
//                // Decode address and append lower 8 bits from position
//                addr = ((cw >> 13) & 0b111111111111111111) << 3;
//                addr |= (i >> 1);
//            }
//            else if (type == CODEWORD_TYPE_MESSAGE) {
//                // Extract the 20 data bits
//                uint32_t data = (cw >> 11) & 0b11111111111111111111;
//            }
        }
    }
}