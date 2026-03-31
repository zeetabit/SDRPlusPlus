#include <catch.hpp>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <dsp/engine/scheduled_processor.h>
#include <dsp/engine/thread_pool.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <cmath>

// A trivial ScheduledProcessor that doubles every float sample.
class TestDoubler : public dsp::ScheduledProcessor<float, float> {
    using base_type = dsp::ScheduledProcessor<float, float>;
public:
    int run() {
        int count = _in->read();
        if (count < 0) { return -1; }
        for (int i = 0; i < count; i++) {
            out.writeBuf[i] = _in->readBuf[i] * 2.0f;
        }
        _in->flush();
        if (!out.swap(count)) { return -1; }
        return count;
    }
};

// A trivial ScheduledSink that sums all float samples.
class TestSummer : public dsp::ScheduledSink<float> {
public:
    float total = 0;
    int samplesReceived = 0;
    std::mutex mtx;
    std::condition_variable cv;

    int run() {
        int count = _in->read();
        if (count < 0) { return -1; }
        for (int i = 0; i < count; i++) {
            total += _in->readBuf[i];
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            samplesReceived += count;
        }
        cv.notify_all();
        _in->flush();
        return count;
    }

    bool waitFor(int expected, int timeoutMs = 2000) {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [&]() { return samplesReceived >= expected; });
    }
};

TEST_CASE("ScheduledProcessor end-to-end data flow", "[integration][dsp]") {
    dsp::stream<float> input;
    dsp::stream<float> output;

    TestDoubler doubler;
    doubler.init(&input);

    TestSummer sink;
    sink.init(&doubler.out);

    doubler.start();
    sink.start();

    // Push 100 samples: [1.0, 2.0, 3.0, ...]
    const int N = 100;
    for (int i = 0; i < N; i++) {
        input.writeBuf[i] = (float)(i + 1);
    }
    input.swap(N);

    REQUIRE(sink.waitFor(N));

    // Stop processing
    sink.stop();
    doubler.stop();

    REQUIRE(sink.samplesReceived == N);
    // Input: 1+2+...+100 = 5050, doubled = 10100
    REQUIRE(sink.total == Approx(10100.0f));
}

TEST_CASE("ScheduledProcessor chain of two blocks", "[integration][dsp]") {
    dsp::stream<float> input;

    TestDoubler d1, d2;
    TestSummer sink;

    d1.init(&input);
    d2.init(&d1.out);
    sink.init(&d2.out);

    d1.start();
    d2.start();
    sink.start();

    const int N = 50;
    for (int i = 0; i < N; i++) {
        input.writeBuf[i] = 1.0f;
    }
    input.swap(N);

    REQUIRE(sink.waitFor(N));

    sink.stop();
    d2.stop();
    d1.stop();

    REQUIRE(sink.samplesReceived == N);
    // 1.0 * 2 * 2 = 4.0 per sample, 50 samples = 200.0
    REQUIRE(sink.total == Approx(200.0f));
}

TEST_CASE("ScheduledProcessor tempStop/tempStart preserves data", "[integration][dsp]") {
    dsp::stream<float> input;

    TestDoubler doubler;
    TestSummer sink;

    doubler.init(&input);
    sink.init(&doubler.out);

    doubler.start();
    sink.start();

    // First batch
    const int N = 10;
    for (int i = 0; i < N; i++) { input.writeBuf[i] = 1.0f; }
    input.swap(N);

    REQUIRE(sink.waitFor(N));

    // Pause and resume
    doubler.tempStop();
    doubler.tempStart();

    // Second batch
    for (int i = 0; i < N; i++) { input.writeBuf[i] = 3.0f; }
    input.swap(N);

    REQUIRE(sink.waitFor(N * 2));

    sink.stop();
    doubler.stop();

    REQUIRE(sink.samplesReceived == N * 2);
    // Batch 1: 10 * 2.0 = 20.0, Batch 2: 10 * 6.0 = 60.0
    REQUIRE(sink.total == Approx(80.0f));
}

// --- ScheduledOperator tests (will compile once ScheduledOperator exists) ---

// A trivial 2-input block that adds floats from two streams.
class TestAdder : public dsp::ScheduledOperator<float, float, float> {
    using base_type = dsp::ScheduledOperator<float, float, float>;
public:
    int run() {
        int a_count = _a->read();
        if (a_count < 0) { return -1; }
        int b_count = _b->read();
        if (b_count < 0) { return -1; }
        int count = std::min(a_count, b_count);
        for (int i = 0; i < count; i++) {
            out.writeBuf[i] = _a->readBuf[i] + _b->readBuf[i];
        }
        _a->flush();
        _b->flush();
        if (!out.swap(count)) { return -1; }
        return count;
    }
};

TEST_CASE("ScheduledOperator two-input data flow", "[integration][dsp]") {
    dsp::stream<float> inputA;
    dsp::stream<float> inputB;

    TestAdder adder;
    TestSummer sink;

    adder.init(&inputA, &inputB);
    sink.init(&adder.out);

    adder.start();
    sink.start();

    const int N = 20;
    for (int i = 0; i < N; i++) {
        inputA.writeBuf[i] = 3.0f;
        inputB.writeBuf[i] = 7.0f;
    }
    inputA.swap(N);
    inputB.swap(N);

    REQUIRE(sink.waitFor(N));

    sink.stop();
    adder.stop();

    REQUIRE(sink.samplesReceived == N);
    // 3.0 + 7.0 = 10.0 per sample, 20 samples = 200.0
    REQUIRE(sink.total == Approx(200.0f));
}

TEST_CASE("ScheduledOperator setInputs dynamic rewiring", "[integration][dsp]") {
    dsp::stream<float> a1, b1, a2, b2;

    TestAdder adder;
    TestSummer sink;

    adder.init(&a1, &b1);
    sink.init(&adder.out);

    adder.start();
    sink.start();

    const int N = 10;
    for (int i = 0; i < N; i++) { a1.writeBuf[i] = 1.0f; b1.writeBuf[i] = 2.0f; }
    a1.swap(N);
    b1.swap(N);

    REQUIRE(sink.waitFor(N));

    // Rewire to different streams
    adder.setInputs(&a2, &b2);

    for (int i = 0; i < N; i++) { a2.writeBuf[i] = 10.0f; b2.writeBuf[i] = 20.0f; }
    a2.swap(N);
    b2.swap(N);

    REQUIRE(sink.waitFor(N * 2));

    sink.stop();
    adder.stop();

    REQUIRE(sink.samplesReceived == N * 2);
    // Batch 1: 10 * 3.0 = 30.0, Batch 2: 10 * 30.0 = 300.0
    REQUIRE(sink.total == Approx(330.0f));
}

TEST_CASE("ScheduledProcessor deadlock detection via waitFor timeout", "[integration][dsp]") {
    TestSummer sink;

    SECTION("waitFor returns false when no data arrives") {
        // Sink is not connected to any pipeline — no data will ever come
        REQUIRE_FALSE(sink.waitFor(1, 100)); // 100ms timeout
        REQUIRE(sink.samplesReceived == 0);
    }

    SECTION("waitFor returns false when pipeline stalls mid-stream") {
        dsp::stream<float> input;
        TestDoubler doubler;

        doubler.init(&input);
        sink.init(&doubler.out);
        doubler.start();
        sink.start();

        // Send 10 samples
        const int N = 10;
        for (int i = 0; i < N; i++) { input.writeBuf[i] = 1.0f; }
        input.swap(N);
        REQUIRE(sink.waitFor(N));

        // Now wait for 20 more that will never arrive
        REQUIRE_FALSE(sink.waitFor(N + 20, 100));
        REQUIRE(sink.samplesReceived == N);

        sink.stop();
        doubler.stop();
    }
}

TEST_CASE("ScheduledProcessor handles stream shutdown cleanly", "[integration][dsp]") {
    dsp::stream<float> input;
    TestDoubler doubler;
    TestSummer sink;

    doubler.init(&input);
    sink.init(&doubler.out);
    doubler.start();
    sink.start();

    // Send some data first
    const int N = 5;
    for (int i = 0; i < N; i++) { input.writeBuf[i] = 2.0f; }
    input.swap(N);
    REQUIRE(sink.waitFor(N));

    // Stop the pipeline — this should not deadlock
    sink.stop();
    doubler.stop();

    REQUIRE(sink.samplesReceived == N);
    REQUIRE(sink.total == Approx(20.0f)); // 5 * 2.0 * 2.0 = 20.0
}
