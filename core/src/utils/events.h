#pragma once
#include <string>
#include <dsp/types.h>

// Standard event types for the EventBus.
// Modules publish/subscribe to these instead of holding direct references
// to SourceManager, SinkManager, etc.
//
// Naming convention: <Subject><Verb> in past tense for notifications,
// present tense for requests.

namespace events {

// --- Source events ---
struct SourceRegistered   { std::string name; };
struct SourceUnregistered { std::string name; };
struct SourceSelected     { std::string name; };
struct SourceDeselected   { std::string name; };
struct SourceStarted      {};
struct SourceStopped      {};

// --- Tuning events ---
struct FrequencyChanged   { double frequency; };
struct TuningOffsetChanged { double offset; };

// --- VFO events ---
struct VFOCreated {
    std::string name;
    double bandwidth;
    double sampleRate;
};
struct VFODeleted { std::string name; };
struct VFOBandwidthChanged {
    std::string name;
    double bandwidth;
};

// --- Sink events ---
struct SinkProviderRegistered   { std::string name; };
struct SinkProviderUnregistered { std::string name; };
struct StreamRegistered         { std::string name; };
struct StreamUnregistered       { std::string name; };
struct StreamSampleRateChanged {
    std::string streamName;
    float sampleRate;
};

// --- Playback state ---
struct PlayStateChanged { bool playing; };

// --- Module lifecycle ---
struct ModuleInstanceCreated { std::string instanceName; std::string moduleName; };
struct ModuleInstanceDeleted { std::string instanceName; };

// --- Input sample rate ---
struct InputSampleRateChanged { double sampleRate; };

// --- Waterfall / FFT ---
struct FFTRedraw {
    double min;
    double max;
    int dataWidth;
    float* data;
};

// --- Application lifecycle ---
// Published before shutdown begins. Handlers should save state, flush buffers,
// and release external resources. Handlers run synchronously on the main thread.
struct ShutdownRequested {};

} // namespace events
