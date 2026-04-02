# SDR++ Module API V2

This document describes the V2 module API introduced to improve modularity, type safety, and inter-module communication. The V2 API is fully backward compatible -- all existing V1 modules continue to work without changes.

## Table of Contents

- [API Version & Compatibility](#api-version--compatibility)
- [Module Manifest (V2)](#module-manifest-v2)
- [Event Bus](#event-bus)
- [Service Registry](#service-registry)
- [Standard Service Interfaces](#standard-service-interfaces)
- [Migration Guide](#migration-guide)
- [Resource Auto-Detection](#resource-auto-detection)

---

## API Version & Compatibility

**Header:** `core/src/api_version.h`

The core defines a semantic version for the module API:

```cpp
#define SDRPP_API_VERSION_MAJOR 2
#define SDRPP_API_VERSION_MINOR 0
#define SDRPP_API_VERSION_PATCH 0
```

Compatibility rules:
- **Same major version** required (breaking changes bump major)
- **Module minor <= core minor** (new optional features bump minor)
- Checked at load time -- incompatible modules are rejected with an error log

```cpp
// Core checks this when loading a V2 module:
sdrppApiCompatible(module->apiVersion);  // true if compatible
```

---

## Module Manifest (V2)

**Header:** `core/src/module_manifest.h`

V2 modules export an extended info struct alongside the existing `_INFO_` symbol. The loader probes for `_INFO_V2_` first; if absent, falls back to V1 behavior.

### Declaring a V2 Module

```cpp
#include <module.h>
#include <module_manifest.h>

// V1 info (still required for backward compatibility)
SDRPP_MOD_INFO {
    "my_source", "My SDR Source", "Author", 1, 0, 0, 1
};

// V2 info (optional, enables new features)
SDRPP_MOD_INFO_V2 {
    "my_source", "My SDR Source", "Author", 1, 0, 0, 1,
    SDRPP_API_VERSION,       // API version this module was built against
    MOD_CAP_SOURCE,          // Capability flags
    0,                       // Dependency count
    nullptr                  // Dependencies array
};
```

### Capability Flags

```cpp
enum ModuleCapability {
    MOD_CAP_SOURCE   = (1 << 0),  // Hardware/network source
    MOD_CAP_SINK     = (1 << 1),  // Audio/network output
    MOD_CAP_DECODER  = (1 << 2),  // Demodulator/decoder
    MOD_CAP_MISC     = (1 << 3),  // Utility module
};
```

Flags can be combined: `MOD_CAP_DECODER | MOD_CAP_MISC`.

### Declaring Dependencies

```cpp
static const ModuleDependency deps[] = {
    { "radio", 0 },                                    // any version
    { "recorder", SDRPP_MAKE_API_VERSION(2, 0, 0) },   // minimum API version
};

SDRPP_MOD_INFO_V2 {
    "my_plugin", "My Plugin", "Author", 1, 0, 0, -1,
    SDRPP_API_VERSION,
    MOD_CAP_MISC,
    2, deps       // 2 dependencies
};
```

Dependencies are checked after all modules are loaded. If a required module is missing or has an incompatible API version, the dependent module will not be instantiated.

---

## Event Bus

**Header:** `core/src/utils/event_bus.h`
**Standard events:** `core/src/utils/events.h`

A global typed publish/subscribe system that decouples publishers from subscribers. Replaces the need to hold direct references to `SourceManager`, `SinkManager`, etc.

### Subscribing

```cpp
#include <utils/event_bus.h>
#include <utils/events.h>

// Subscribe returns an RAII handle -- destructor auto-unsubscribes
auto sub = EventBus::get().subscribe<events::FrequencyChanged>(
    [](const events::FrequencyChanged& e) {
        flog::info("Frequency: {0}", e.frequency);
    }
);

// Manual unsubscribe (optional, destructor does this too)
sub.unsubscribe();
```

### Publishing

```cpp
EventBus::get().publish(events::FrequencyChanged{145.5e6});
```

### Thread Safety

- `subscribe()` and `publish()` are mutex-protected and safe from any thread.
- Handlers are called synchronously on the publisher's thread.
- Keep handlers fast -- offload heavy work to a queue if needed.

### Standard Event Types

All defined in `core/src/utils/events.h` under the `events` namespace:

| Event | Fields | Emitted when |
|-------|--------|-------------|
| `SourceRegistered` | `name` | Source module registers with SourceManager |
| `SourceUnregistered` | `name` | Source module unregisters |
| `SourceSelected` | `name` | User selects a source |
| `SourceDeselected` | `name` | Previous source deselected |
| `SourceStarted` | -- | Source begins streaming |
| `SourceStopped` | -- | Source stops streaming |
| `FrequencyChanged` | `frequency` | Tuning frequency changes |
| `TuningOffsetChanged` | `offset` | Tuning offset changes |
| `VFOCreated` | `name`, `bandwidth`, `sampleRate` | New VFO created |
| `VFODeleted` | `name` | VFO removed |
| `VFOBandwidthChanged` | `name`, `bandwidth` | VFO bandwidth changes |
| `SinkProviderRegistered` | `name` | Audio sink provider registered |
| `SinkProviderUnregistered` | `name` | Audio sink provider removed |
| `StreamRegistered` | `name` | Audio stream registered |
| `StreamUnregistered` | `name` | Audio stream removed |
| `StreamSampleRateChanged` | `streamName`, `sampleRate` | Stream sample rate changes |
| `PlayStateChanged` | `playing` | Play/stop toggled |
| `ModuleInstanceCreated` | `instanceName`, `moduleName` | Module instance created |
| `ModuleInstanceDeleted` | `instanceName` | Module instance deleted |
| `InputSampleRateChanged` | `sampleRate` | DSP input sample rate changes |

### Defining Custom Events

```cpp
// In your module header:
struct MyCustomEvent {
    std::string channel;
    float signalStrength;
};

// Publish from your module:
EventBus::get().publish(MyCustomEvent{"CH1", -45.2f});
```

---

## Service Registry

**Header:** `core/src/utils/service_registry.h`
**Standard interfaces:** `core/src/utils/services.h`

A typed service locator that replaces `ModuleComManager`'s opaque `void*` command dispatch. Modules register service implementations by interface type and instance name.

### Providing a Service

```cpp
#include <utils/service_registry.h>
#include <utils/services.h>

// In your module constructor:
ServiceRegistry::get().provide<IRadioControl>(name, myAdapter);

// In your module destructor:
ServiceRegistry::get().remove<IRadioControl>(name);
```

### Querying a Service

```cpp
auto* radio = ServiceRegistry::get().query<IRadioControl>("Radio");
if (radio) {
    int mode = radio->getMode();
    radio->setBandwidth(12500.0);
}
```

### Listing Available Services

```cpp
auto names = ServiceRegistry::get().listNames<IRadioControl>();
for (auto& name : names) {
    flog::info("Radio instance: {0}", name);
}
```

### Checking Existence

```cpp
if (ServiceRegistry::get().exists<IRadioControl>("Radio")) {
    // safe to query
}
```

---

## Standard Service Interfaces

**Header:** `core/src/utils/services.h`

### IRadioControl

Controls a radio demodulator instance (replaces `RADIO_IFACE_CMD_*` codes):

```cpp
class IRadioControl {
public:
    virtual int getMode() = 0;
    virtual void setMode(int mode) = 0;
    virtual double getBandwidth() = 0;
    virtual void setBandwidth(double bw) = 0;
    virtual int getSquelchMode() = 0;
    virtual void setSquelchMode(int mode) = 0;
    virtual float getSquelchLevel() = 0;
    virtual void setSquelchLevel(float level) = 0;
    virtual float getCTCSSTone() = 0;
    virtual void setCTCSSTone(float tone) = 0;
    virtual bool getHighPass() = 0;
    virtual void setHighPass(bool enabled) = 0;
};
```

Mode values match the existing `RADIO_IFACE_MODE_*` enum (NFM=0, WFM=1, AM=2, DSB=3, USB=4, CW=5, LSB=6, RAW=7).

### IRecorderControl

Controls a recorder instance (replaces `RECORDER_IFACE_CMD_*` codes):

```cpp
class IRecorderControl {
public:
    virtual int getMode() = 0;
    virtual void setMode(int mode) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
};
```

### IDemodulatorControl

Generic demodulator start/stop (replaces `METEOR_DEMODULATOR_IFACE_CMD_*`):

```cpp
class IDemodulatorControl {
public:
    virtual void start() = 0;
    virtual void stop() = 0;
};
```

---

## Migration Guide

### Migrating a Module to V2

1. **Add V2 manifest** (keep V1 `SDRPP_MOD_INFO` for compatibility):

```cpp
#include <module_manifest.h>

SDRPP_MOD_INFO { "my_mod", "Desc", "Author", 1, 0, 0, 1 };

SDRPP_MOD_INFO_V2 {
    "my_mod", "Desc", "Author", 1, 0, 0, 1,
    SDRPP_API_VERSION, MOD_CAP_SOURCE, 0, nullptr
};
```

2. **Switch from `EventHandler<T>` to `EventBus`** (can be done incrementally):

```cpp
// Old:
EventHandler<double> onRetuneHandler;
onRetuneHandler.handler = [](double freq, void* ctx) { ... };
onRetuneHandler.ctx = this;
sigpath::sourceManager.onRetune.bindHandler(&onRetuneHandler);

// New:
auto sub = EventBus::get().subscribe<events::FrequencyChanged>(
    [this](const events::FrequencyChanged& e) { ... }
);
```

3. **Switch from `ModuleComManager` to `ServiceRegistry`**:

```cpp
// Old (caller side):
core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);

// New (caller side):
auto* radio = ServiceRegistry::get().query<IRadioControl>(vfoName);
if (radio) { mode = radio->getMode(); }
```

### Coexistence

During migration, both old and new systems run in parallel:
- Core emits events on both `Event<T>` and `EventBus`
- Radio module registers with both `ModuleComManager` and `ServiceRegistry`
- No module is forced to migrate -- V1 continues to work indefinitely

---

## Resource Auto-Detection

When `resourcesDirectory` from config.json doesn't point to a valid directory (common after building from source without `make install`), the application automatically searches these locations:

**Relative to executable:**
- `<exe_dir>/res`
- `<exe_dir>/../res`
- `<exe_dir>/../root/res`
- `<exe_dir>/../root_dev/res`

**Relative to current working directory:**
- `./res`
- `./root/res`
- `./root_dev/res`
- `../res`
- `../root/res`
- `../root_dev/res`

When found, the detected path is saved to config.json for future runs.

---

## Module Author Quick Reference

Summary of what changed and what module authors need to adjust.

### What's Different from Upstream SDR++

| Old Pattern | New Pattern | Action Required |
|-------------|-------------|-----------------|
| `gui::waterfall.getBandwidth()` | `ServiceRegistry::get().query<IRadioState>("core")->getBandwidth()` | Add `#include <utils/service_registry.h>` and `#include <utils/radio_state.h>` |
| `gui::waterfall.getCenterFrequency()` | `query<IRadioState>("core")->getCenterFrequency()` | Same includes |
| `gui::waterfall.selectedVFO` | `query<IRadioState>("core")->getSelectedVFO()` | Same includes |
| `gui::waterfall.vfos[name]->generalOffset` | `query<IRadioState>("core")->getVFOGeneralOffset(name)` | Same includes |
| `gui::waterfall.centerFrequencyLocked = true` | `query<IRadioStateControl>("core")->setCenterFrequencyLocked(true)` | Add `#include <utils/radio_control.h>` |
| `modComManager.callInterface(vfo, CMD, &in, &out)` | `query<IRadioControl>(vfo)->setMode(mode)` | Add `#include <utils/services.h>` |
| `modComManager.interfaceExists(name)` | `query<IRadioControl>(name) != nullptr` | Same includes |
| `class X : public Processor<A,B>` | `class X : public ScheduledProcessor<A,B>` | Add `#include <dsp/engine/scheduled_processor.h>` |
| `class X : public Sink<T>` | `class X : public ScheduledSink<T>` | Same include |
| `class X : public Operator<A,B,O>` | `class X : public ScheduledOperator<A,B,O>` | Same include |
| `SourceHandler` struct + `registerSource(name, &handler)` | Implement `ISource` + `registerSource(name, this)` | Add `#include <signal_path/isource.h>` |
| `add_subdirectory("decoder_modules/my_mod")` | `add_sdrpp_module("decoder_modules/my_mod" my_mod)` | Update CMakeLists.txt |

### V2.1: ModuleConfig and Scoped Configuration

V2.1 modules receive a `ModuleConfig*` in their constructor, providing instance-scoped config access:

```cpp
class MyModule : public ModuleManager::Instance {
public:
    MyModule(std::string name, ModuleConfig* cfg) {
        float gain = cfg->get<float>("gain", 1.0f);  // reads with default
        cfg->set("gain", gain);                        // writes to instance namespace
    }
};
```

**Manifest defaults**: Declare default config in the V2 manifest:

```cpp
SDRPP_MOD_INFO_V2 {
    "my_mod", "My Module", "Author", 1, 0, 0, -1,
    SDRPP_API_VERSION, MOD_CAP_DECODER, 0, nullptr,
    R"({"gain": 1.0, "enabled": true})",   // configDefaults JSON
    "my_mod_config.json"                     // configFileName
};
```

**V1 compatibility proxy**: V1 modules that don't export `_CONFIG_` automatically get a proxy `ConfigManager` at `<root>/<moduleName>_config.json`. This means `ModuleConfig` always has a valid backend -- V1 modules get config support without code changes.

**Macros**:
- `SDRPP_MOD_CONFIG(configVar)` -- exports the module's ConfigManager for V2.1
- `SDRPP_CREATE_INSTANCE_V2(ClassName)` -- exports both V1 and V2 constructors

### Module Lifecycle

**Auto-discovery**: Loaded modules without a config entry get auto-created with a display name derived from the module name (e.g., `radiosonde_decoder` -> `Radiosonde Decoder`). Auto-created entries are persisted to config.

**Enable/disable persistence**: Module enabled state is saved to `config["moduleInstances"][name]["enabled"]`. Disabled modules are loaded but not started on next launch.

**Removed modules**: Deleting a module in Module Manager marks it as `"removed": true` in config. Removed modules are not auto-recreated. Re-add via the Module Manager "+" button.

**Module Manager UI**: V1 (legacy) modules are shown with a yellow warning icon and tooltip in the Module Manager instance table. V2 modules have no indicator. Faulted modules get a red row highlight with error tooltip.

**VFO lifecycle**: Decoder modules should create VFO in `enable()` and delete in `disable()`:

```cpp
void enable() {
    enabled = true;
    vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, bw, sr, bw, bw, true);
    // start DSP chain...
}
void disable() {
    // stop DSP chain (consumers before producers)...
    if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
    enabled = false;
}
```

### DSP Block Lifecycle

`block::stop()` is `noexcept` -- exceptions in stop are caught to prevent app termination from destructors. DSP chains must be stopped in reverse data-flow order (consumers first, producers last) before deleting VFOs.

`scheduled_block::doStop()` uses a 2-second timeout on `workerDone.get()` with automatic retry to handle rare race conditions.

All output streams that a block writes to must be registered via `registerOutput()`. Unregistered outputs won't receive `stopWriter()` during shutdown, causing deadlocks. (Example: `BroadcastFM` registers both `out` and `rdsOut`.)

### Shutdown

The app publishes `events::ShutdownRequested{}` via `EventBus::get().publishWithTimeout()` with a 5-second timeout per handler. Modules can subscribe to perform cleanup:

```cpp
auto sub = EventBus::get().subscribe<events::ShutdownRequested>([this](const auto&) {
    saveState();
});
```

The `core::shuttingDown` flag is set before shutdown begins. Guards in `setInputSampleRate()` and `sourcemenu::onSourcesChanged()` prevent cascading source re-selection during teardown.

### Config System

Core config uses a component-key allowlist for cleanup. Module-specific keys stored in core config must be registered in `componentKeys` (in `core.cpp`) to survive cleanup. Module-owned config files (via `ConfigManager`) are not affected.

**Registered component keys** (in `core.cpp`):
- Source menu: `source`, `manualOffset`, `selectedOffset`, `iqCorrection`, `invertIQ`, `decimation`
- Display: `min`, `max`, `frequency`, `showMenu`, `menuWidth`, `fftHeight`, `centerTuning`, `fftSpeed`, `fftSmoothing`
- Zoom state: `bandwidth_slider`, `bandwidth_view`, `bandwidth_offset`
- Theme/UI: `theme`, `uiScale`
- Other: `bandColors`, `modulesDirectory`, `resourcesDirectory`

**Source selection persistence**: The source menu saves and restores the selected source, offset mode, decimation, and IQ correction settings. When a saved source is not available on restart (e.g., hardware disconnected), it falls back to the first available source and persists the fallback. Same pattern for offset mode and decimation.

**Zoom persistence**: View bandwidth, slider position, and view offset are saved on every zoom change and restored on startup via `ViewStateCoordinator::loadFromConfig()`.

The `conf.value("key", default)` pattern is used throughout for crash-safe config reads. Never use `conf["key"]` directly for reads -- it creates null entries on missing keys.

### Minimum Required for a New Module

V1 patterns still compile and work. To adopt V2 features:

1. **Required for V2.1**: Add `SDRPP_MOD_INFO_V2` + `SDRPP_MOD_CONFIG` + `SDRPP_CREATE_INSTANCE_V2`
2. **Recommended**: Use `ModuleConfig` for scoped config instead of manual JSON paths
3. **Recommended**: Use `ScheduledProcessor` for DSP blocks (thread pool)
4. **Recommended**: Proper VFO lifecycle in enable/disable
5. **Recommended**: Per-module tests in `<module>/tests/` with own CMakeLists.txt

### Reference Implementation

The CW decoder (`decoder_modules/cw_decoder/`) is a complete V2.1 reference:
- Multi-channel auto-detection with FFT-based tone scanning
- CFAR-style noise estimation, adaptive timing, Morse tree decoding
- Waterfall overlay via `onFFTRedraw` callback
- Per-module test suite (`tests/CMakeLists.txt` + Catch2)
- Clean separation: `main.cpp` (99 lines), channel_manager, menu, DSP, detector

### Includes Cheat Sheet

```cpp
// V2.1 module config
#include <module_config.h>
#include <module_manifest.h>

// Radio state (read-only)
#include <utils/service_registry.h>
#include <utils/radio_state.h>
auto* rs = ServiceRegistry::get().query<IRadioState>("core");

// Radio state mutations
#include <utils/radio_control.h>
auto* rc = ServiceRegistry::get().query<IRadioStateControl>("core");

// Inter-module communication
#include <utils/services.h>
auto* radio = ServiceRegistry::get().query<IRadioControl>(vfoName);

// DSP blocks on thread pool
#include <dsp/engine/scheduled_processor.h>

// Source module with ISource
#include <signal_path/isource.h>

// EventBus
#include <utils/event_bus.h>
#include <utils/events.h>
```
