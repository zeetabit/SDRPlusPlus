#pragma once
#include <string>
#include <set>
#include <algorithm>
#include <cctype>

namespace cw {
namespace vocabulary {

    inline const std::set<std::string>& dictionary() {
        static const std::set<std::string> words = {
            // Common CW words
            "CQ", "DE", "K", "R", "BK", "SK", "AR", "BT", "KN",
            "RST", "UR", "QTH", "QSO", "QRM", "QRN", "QSB", "QSL",
            "QRZ", "QRP", "QRO", "QRT", "QRV", "QSY",
            "TNX", "TKS", "FER", "PSE", "HR", "HW", "FB", "OM",
            "ES", "GE", "GM", "GA", "GN",
            "TEST", "CW", "SSB", "FM",
            "ANT", "RIG", "WX", "PWR",
            "73", "88",
            "5NN", "599", "579", "569", "559", "549", "539",
            "DX", "DXCC",
            "NEWINGTON", "CT",
            "SOS", "PARIS",
            "HI",
        };
        return words;
    }

    inline bool isKnownWord(const std::string& word) {
        return dictionary().count(word) > 0;
    }

    // Callsign pattern: 1-2 letters, 1 digit, 1-4 letters
    // Covers: W1AW, K1ABC, VE3NEA, JA1XYZ, DL1ABC, etc.
    inline bool isCallsign(const std::string& s) {
        if (s.size() < 3 || s.size() > 7) return false;

        // Find the digit position (there should be exactly one digit cluster)
        int digitPos = -1;
        int digitCount = 0;
        for (int i = 0; i < (int)s.size(); i++) {
            if (std::isdigit(s[i])) {
                if (digitPos < 0) digitPos = i;
                digitCount++;
            }
        }
        if (digitCount != 1 || digitPos < 1 || digitPos > 2) return false;

        // Prefix: 1-2 letters before digit
        for (int i = 0; i < digitPos; i++) {
            if (!std::isalpha(s[i])) return false;
        }
        // Suffix: 1-4 letters after digit
        int suffixLen = (int)s.size() - digitPos - 1;
        if (suffixLen < 1 || suffixLen > 4) return false;
        for (int i = digitPos + 1; i < (int)s.size(); i++) {
            if (!std::isalpha(s[i])) return false;
        }
        return true;
    }

    // RST report: 3 digits, first 1-5, second 1-9, third 1-9
    inline bool isRSTReport(const std::string& s) {
        if (s.size() != 3) return false;
        if (!std::isdigit(s[0]) || !std::isdigit(s[1]) || !std::isdigit(s[2])) return false;
        if (s[0] < '1' || s[0] > '5') return false;
        if (s[1] < '1' || s[2] < '1') return false;
        return true;
    }

    // Q-code: Q followed by 2 uppercase letters
    inline bool isQCode(const std::string& s) {
        if (s.size() != 3) return false;
        if (s[0] != 'Q') return false;
        if (!std::isalpha(s[1]) || !std::isalpha(s[2])) return false;
        return true;
    }

}}
