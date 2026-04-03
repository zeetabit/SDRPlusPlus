#include <catch.hpp>
#include <cw/channel_manager.h>
#include <cmath>

// Generate IQ with a tone at given frequency
static void generateTone(dsp::complex_t* buf, int count, float freq,
                         float sampleRate, float amplitude, float& phase) {
    float omega = 2.0f * M_PI * freq / sampleRate;
    for (int i = 0; i < count; i++) {
        buf[i].re = amplitude * cosf(phase);
        buf[i].im = amplitude * sinf(phase);
        phase += omega;
        if (phase > M_PI) { phase -= 2.0f * M_PI; }
    }
}

// ============================================================
// Channel Management Tests
// ============================================================

TEST_CASE("ChannelManager add and remove channels", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);  // no waterfall binding

    mgr.addChannel(700.0f, true);
    REQUIRE(mgr.entries.size() == 1);
    REQUIRE(mgr.entries[0].pinned == true);
    REQUIRE(mgr.entries[0].channel->toneFreq == Approx(700.0f));

    mgr.addChannel(800.0f, false);
    REQUIRE(mgr.entries.size() == 2);

    mgr.removeChannel(0);
    REQUIRE(mgr.entries.size() == 1);
    REQUIRE(mgr.entries[0].channel->toneFreq == Approx(800.0f));
}

TEST_CASE("ChannelManager respects max channel limit", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    mgr.maxChannels = 10;
    for (int i = 0; i < mgr.maxChannels + 5; i++) {
        mgr.addChannel(600.0f + i * 50.0f, false);
    }
    REQUIRE((int)mgr.entries.size() == mgr.maxChannels);
}

TEST_CASE("ChannelManager findChannelNear", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    mgr.addChannel(700.0f, true);
    mgr.addChannel(900.0f, true);

    REQUIRE(mgr.findChannelNear(710.0f, 30.0f) == 0);
    REQUIRE(mgr.findChannelNear(895.0f, 30.0f) == 1);
    REQUIRE(mgr.findChannelNear(800.0f, 30.0f) == -1);  // too far from both
}

TEST_CASE("ChannelManager auto-detect creates channels from tones", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);
    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    float phase = 0;
    dsp::complex_t buf[1024];
    // Feed tone and update channels repeatedly so stable tone confirms
    for (int i = 0; i < 15; i++) {
        generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
        mgr.process(buf, 1024);
        mgr.updateChannels();
    }

    bool found = false;
    for (auto& e : mgr.entries) {
        if (fabsf(e.channel->toneFreq - 700.0f) < 30.0f) { found = true; }
    }
    REQUIRE(found);
}

TEST_CASE("ChannelManager idle channels are removed", "[cw][manager]") {
    srand(12345);  // deterministic noise

    cw::ChannelManager mgr;
    mgr.init(8000.0f);
    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    // Create auto-detected channel via tone (needs multiple frames for confirmation)
    float phase = 0;
    dsp::complex_t buf[1024];
    for (int i = 0; i < 15; i++) {
        generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
        mgr.process(buf, 1024);
        mgr.updateChannels();
    }
    REQUIRE(!mgr.entries.empty());

    // Feed silence + update each frame
    for (int i = 0; i < CW_IDLE_TIMEOUT_ACTIVE + 80; i++) {
        for (int j = 0; j < 1024; j++) {
            buf[j].re = 0.01f * ((float)rand() / RAND_MAX - 0.5f);
            buf[j].im = 0.01f * ((float)rand() / RAND_MAX - 0.5f);
        }
        mgr.process(buf, 1024);
        mgr.updateChannels();
    }

    // Auto channels should have been removed (pinned would stay)
    bool hasAutoChannel = false;
    for (auto& e : mgr.entries) {
        if (!e.pinned) { hasAutoChannel = true; }
    }
    REQUIRE_FALSE(hasAutoChannel);
}

TEST_CASE("ChannelManager pinned channels survive idle timeout", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    mgr.addChannel(700.0f, true);  // pinned
    REQUIRE(mgr.entries.size() == 1);

    // Feed silence with auto-detect off — just verify pinned channel survives
    mgr.autoDetect = false;
    dsp::complex_t buf[1024];
    for (int i = 0; i < CW_IDLE_TIMEOUT + 10; i++) {
        for (int j = 0; j < 1024; j++) {
            buf[j].re = 0.0001f * ((float)rand() / RAND_MAX - 0.5f);
            buf[j].im = 0.0001f * ((float)rand() / RAND_MAX - 0.5f);
        }
        mgr.process(buf, 1024);
    }

    // Pinned channel should still be there
    REQUIRE(mgr.entries.size() == 1);
    REQUIRE(mgr.entries[0].pinned == true);
}

TEST_CASE("ChannelManager deduplicates close auto channels", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    // Manually create two auto channels within CW_TONE_MATCH_HZ (40 Hz)
    mgr.addChannel(700.0f, false);
    mgr.addChannel(730.0f, false);  // 30 Hz away, within 40 Hz match radius
    REQUIRE(mgr.entries.size() == 2);

    // Set different SNR so dedup knows which to keep
    mgr.entries[0].channel->snr = 10.0f;
    mgr.entries[1].channel->snr = 5.0f;

    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    // updateChannels should merge them — feed dummy data to trigger scan
    float phase = 0;
    dsp::complex_t buf[1024];
    generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
    mgr.process(buf, 1024);
    mgr.updateChannels();

    // Should have merged to 1 channel
    REQUIRE(mgr.entries.size() == 1);
}

TEST_CASE("ChannelManager dedup preserves pinned channels", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    // Two pinned channels at close frequencies
    mgr.addChannel(700.0f, true);
    mgr.addChannel(750.0f, true);
    REQUIRE(mgr.entries.size() == 2);

    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    float phase = 0;
    dsp::complex_t buf[1024];
    generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
    mgr.process(buf, 1024);
    mgr.updateChannels();

    // Pinned channels should NOT be deduped
    REQUIRE(mgr.entries.size() == 2);
}

TEST_CASE("ChannelManager stable tone confirmation requires multiple frames", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);
    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    float phase = 0;
    dsp::complex_t buf[1024];

    // Feed just 1 frame — not enough to confirm
    generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
    mgr.process(buf, 1024);
    mgr.updateChannels();

    // Should have a stable tone but no channel yet
    REQUIRE(mgr.entries.empty());
    REQUIRE(!mgr.stableTones.empty());
    REQUIRE_FALSE(mgr.stableTones[0].confirmed);

    // Feed enough frames to confirm (CW_TONE_CONFIRM_FRAMES = 3)
    for (int i = 0; i < 5; i++) {
        generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
        mgr.process(buf, 1024);
        mgr.updateChannels();
    }

    // Now channel should exist
    REQUIRE(!mgr.entries.empty());
}

TEST_CASE("ChannelManager stable tone dedup merges close tones", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);
    mgr.autoDetect = true;
    mgr.scanThreshold = 6.0f;

    // Feed two close tones alternately
    float phase1 = 0, phase2 = 0;
    dsp::complex_t buf1[1024], buf2[1024], combined[1024];

    for (int i = 0; i < 15; i++) {
        generateTone(buf1, 1024, 700.0f, 8000.0f, 1.0f, phase1);
        generateTone(buf2, 1024, 740.0f, 8000.0f, 0.5f, phase2);
        for (int j = 0; j < 1024; j++) {
            combined[j].re = buf1[j].re + buf2[j].re;
            combined[j].im = buf1[j].im + buf2[j].im;
        }
        mgr.process(combined, 1024);
        mgr.updateChannels();
    }

    // Should result in at most 1 channel (merged by scanner + stable tone dedup)
    int channelsNear700 = 0;
    for (auto& e : mgr.entries) {
        if (fabsf(e.channel->toneFreq - 700.0f) < 80.0f) { channelsNear700++; }
    }
    REQUIRE(channelsNear700 <= 1);
}

TEST_CASE("ChannelManager rebuildPinnedList", "[cw][manager]") {
    cw::ChannelManager mgr;
    mgr.init(8000.0f);

    mgr.addChannel(700.0f, true);
    mgr.addChannel(800.0f, false);
    mgr.addChannel(900.0f, true);

    mgr.rebuildPinnedList();
    REQUIRE(mgr.pinnedTones.size() == 2);
    REQUIRE(mgr.pinnedTones[0] == Approx(700.0f));
    REQUIRE(mgr.pinnedTones[1] == Approx(900.0f));
}
