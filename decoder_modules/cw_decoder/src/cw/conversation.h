#pragma once
#include <string>
#include <cctype>
#include "vocabulary.h"

namespace cw {

    // Tracks the position within a CW QSO to provide context-aware
    // correction hints. CW conversations follow a predictable structure:
    //   CQ CQ CQ DE <callsign> <callsign> K
    //   <callsign> DE <callsign> UR RST 599 599 QTH <location> ...
    //   TNX FER QSO 73 SK
    class ConversationTracker {
    public:
        enum State {
            IDLE,           // No conversation detected
            CQ_CALL,        // CQ sequence in progress
            EXCHANGE,       // Callsign/info exchange
            RST_EXCHANGE,   // RST report exchange
            CLOSING,        // End of QSO (73, SK, TU)
        };

        // Feed a decoded word with its confidence and the current channel WPM.
        // WPM is used to detect operator changes — a sudden WPM shift with
        // high-confidence CQ means a different station, not the same conversation.
        void feedWord(const std::string& word, float confidence = 0.5f, float wpm = 0.0f) {
            wordCount++;

            // Track WPM for operator consistency check
            if (wpm > 0 && lockedWpm > 0) {
                wpmDrift = fabsf(wpm - lockedWpm) / lockedWpm;
            }
            if (wpm > 0 && (lockedWpm <= 0 || state == IDLE)) {
                lockedWpm = wpm;
            }

            // High-confidence "CQ" in an unexpected state means we missed the
            // end of the previous conversation — a new station is calling.
            // Only reset if WPM is consistent (within 30%) — otherwise it's
            // likely cross-channel pollution from a different operator.
            if (word == "CQ" && confidence > 0.8f && state != IDLE && state != CQ_CALL && state != CLOSING) {
                bool wpmConsistent = (lockedWpm <= 0 || wpm <= 0 || wpmDrift < 0.3f);
                if (wpmConsistent) {
                    state = CQ_CALL;
                    wordCount = 1;
                    if (wpm > 0) lockedWpm = wpm;
                    return;
                }
                // WPM mismatch — ignore this CQ, it's from a different operator
            }

            switch (state) {
                case IDLE:
                    if (word == "CQ") state = CQ_CALL;
                    else if (word == "DE") state = EXCHANGE;
                    break;

                case CQ_CALL:
                    if (word == "DE") state = EXCHANGE;
                    else if (word == "CQ") {} // stay
                    else if (word == "TEST") {} // contest CQ TEST
                    else if (vocabulary::isCallsign(word)) state = EXCHANGE;
                    else if (word == "73" || word == "SK") state = CLOSING;
                    break;

                case EXCHANGE:
                    if (word == "RST" || word == "5NN") state = RST_EXCHANGE;
                    else if (vocabulary::isRSTReport(word)) state = RST_EXCHANGE;
                    else if (word == "73" || word == "SK" || word == "TU") state = CLOSING;
                    break;

                case RST_EXCHANGE:
                    if (word == "73" || word == "SK" || word == "TU") state = CLOSING;
                    break;

                case CLOSING:
                    if (word == "CQ") { state = CQ_CALL; wordCount = 1; }
                    break;
            }
        }

        // Returns true if the given word is expected/likely in the current state.
        // Used by the corrector to boost matching score for context-appropriate words.
        bool isExpectedWord(const std::string& word) const {
            switch (state) {
                case IDLE:
                    return word == "CQ" || word == "DE";

                case CQ_CALL:
                    return word == "CQ" || word == "DE" || word == "TEST"
                        || word == "K" || vocabulary::isCallsign(word);

                case EXCHANGE:
                    return word == "DE" || word == "RST" || word == "UR"
                        || word == "K" || word == "BK" || word == "QTH"
                        || word == "5NN" || vocabulary::isCallsign(word)
                        || vocabulary::isRSTReport(word)
                        || vocabulary::isQCode(word);

                case RST_EXCHANGE:
                    return vocabulary::isRSTReport(word) || word == "5NN"
                        || word == "QTH" || word == "BK" || word == "K"
                        || word == "TNX" || word == "FER"
                        || vocabulary::isQCode(word)
                        || vocabulary::isCallsign(word)
                        || word == "599" || word == "579";

                case CLOSING:
                    return word == "73" || word == "88" || word == "SK"
                        || word == "TU" || word == "K" || word == "CQ";
            }
            return false;
        }

        State getState() const { return state; }
        int getWordCount() const { return wordCount; }
        float getLockedWpm() const { return lockedWpm; }

        void reset() {
            state = IDLE;
            wordCount = 0;
            lockedWpm = 0;
            wpmDrift = 0;
        }

    private:
        State state = IDLE;
        int wordCount = 0;
        float lockedWpm = 0;    // WPM of the current conversation operator
        float wpmDrift = 0;     // fractional WPM change from locked value
    };

}
