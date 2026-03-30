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
