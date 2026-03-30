#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>

// Modern virtual interface for SDR source modules.
// New modules implement this instead of populating a SourceHandler struct.
//
// Usage:
//   class MySource : public ModuleManager::Instance, public ISource {
//       dsp::stream<dsp::complex_t> stream;
//       dsp::stream<dsp::complex_t>* getStream() override { return &stream; }
//       void onSelect() override { ... }
//       void onDeselect() override { ... }
//       void start() override { ... }
//       void stop() override { ... }
//       void tune(double freq) override { ... }
//       void drawMenu() override { ... }
//   };
//
// Register with: sigpath::sourceManager.registerSource("MySource", this);
// (uses the ISource* overload, not the SourceHandler* overload)
class ISource {
public:
    virtual ~ISource() = default;

    virtual dsp::stream<dsp::complex_t>* getStream() = 0;
    virtual void onSelect() = 0;
    virtual void onDeselect() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void tune(double freq) = 0;
    virtual void drawMenu() = 0;
};
