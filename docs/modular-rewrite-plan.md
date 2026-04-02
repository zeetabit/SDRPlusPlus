# SDR++ Modular Rewrite Plan

A phased plan to modernize the SDR++ codebase for improved modularity, type safety, performance, and maintainability. Each phase is independently shippable and fully backward compatible.

## Table of Contents

- [Current State Summary](#current-state-summary)
- [Phase 1: Core Abstractions](#phase-1-core-abstractions) -- DONE
- [Phase 2: DSP Engine Modernization](#phase-2-dsp-engine-modernization) -- DONE
- [Phase 3: GUI Decoupling](#phase-3-gui-decoupling) -- DONE
- [Phase 4: Module Ecosystem](#phase-4-module-ecosystem) -- DONE
- [Phase 5: Build & Distribution](#phase-5-build--distribution) -- DONE
- [Test Infrastructure](#test-infrastructure) -- DONE
- [Migration Tracker](#migration-tracker)
- [Final Summary](#final-summary)

---

## Current State Summary

| Metric | Before | After |
|--------|--------|-------|
| Total LOC | ~150K | ~158K (+8K new infra) |
| DSP blocks on thread pool | 0/55 | 55/55 (100%) |
| Modules with gui::waterfall coupling | 15 | 2 (overlay drawing only) |
| modComManager consumer calls | ~30 | 0 (100% migrated) |
| Module error isolation | None | try/catch on all entry points + noexcept stop() |
| Test coverage | None | 46 test cases, 153+ assertions |
| V2.1 modules with ModuleConfig | 0 | 2 (recorder, cw_decoder) |
| V1 modules with proxy ConfigManager | 0 | 55 (auto-provisioned) |
| Module auto-discovery | None | Auto-create + persist + removed flag |
| CMake auto-detection | None | Deps, resources, modules dir |
| Source modules on ISource | 0 | 1 (file_source, adapter for rest) |
| Shutdown coordination | None | EventBus publishWithTimeout + shuttingDown flag |
| Config persistence | Partial | Source, zoom, offsets, enable/disable all saved |
| Branch | `feature/modular-core-abstractions` | |

### Pain Points Addressed

1. No module dependency system -- modules can't declare requirements
2. No API versioning -- core/module ABI compatibility unchecked
3. GUI-DSP coupling -- demodulators directly call waterfall callbacks
4. Thread proliferation -- 15-25 threads for basic operation
5. C-style function pointer interfaces -- `SourceHandler` uses `void*` ctx
6. Opaque inter-module IPC -- `callInterface(name, CMD_CODE, void*, void*)`
7. Splitter memcpy per fan-out output -- unnecessary data copies
8. No error isolation -- plugin crash takes down the application
9. Resource directory not auto-detected -- build-from-source fails on first run

---

## Phase 1: Core Abstractions

**Status: DONE**

Introduces versioned module API, typed event bus, typed service registry, and backward-compatible migration path for all existing modules.

### What Was Built

#### 1.1 API Version & Module Manifest

| File | Purpose |
|------|---------|
| `core/src/api_version.h` | `SDRPP_API_VERSION` (2.0.0), semantic version scheme, `sdrppApiCompatible()` check |
| `core/src/module_manifest.h` | `ModuleInfoV2` struct with capability flags (`MOD_CAP_SOURCE/SINK/DECODER/MISC`), dependency array, `SDRPP_MOD_INFO_V2` macro |

**Compatibility rules:**
- Same major version required (breaking changes bump major)
- Module minor <= core minor (new optional features bump minor)
- Checked at module load time; incompatible modules rejected with error log

**Module capabilities:**
```cpp
enum ModuleCapability {
    MOD_CAP_SOURCE   = (1 << 0),
    MOD_CAP_SINK     = (1 << 1),
    MOD_CAP_DECODER  = (1 << 2),
    MOD_CAP_MISC     = (1 << 3),
};
```

**Dependencies declared as:**
```cpp
static const ModuleDependency deps[] = {
    { "radio", 0 },                                  // any version
    { "recorder", SDRPP_MAKE_API_VERSION(2, 0, 0) }, // minimum version
};
```

#### 1.2 Typed Event Bus

| File | Purpose |
|------|---------|
| `core/src/utils/event_bus.h` | Global typed pub/sub `EventBus` singleton, RAII `Subscription` handles |
| `core/src/utils/events.h` | 15 standard event structs under `events::` namespace |

**Design:**
- Type-safe: `EventBus::get().subscribe<events::FrequencyChanged>([](const auto& e) { ... })`
- Thread-safe: mutex-protected subscribe/publish, handlers called synchronously
- RAII: `Subscription` destructor auto-unsubscribes; no dangling handler risk
- Coexists with V1 `Event<T>`: core emits on both systems during migration

**Standard events:**
| Event | Fields | Emitted when |
|-------|--------|-------------|
| `SourceRegistered` | `name` | Source registers with SourceManager |
| `SourceUnregistered` | `name` | Source unregisters |
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

#### 1.3 Typed Service Registry

| File | Purpose |
|------|---------|
| `core/src/utils/service_registry.h` | Typed `ServiceRegistry` singleton with `provide<T>()` / `query<T>()` / `remove<T>()` / `listNames<T>()` |
| `core/src/utils/services.h` | Standard interfaces: `IRadioControl`, `IRecorderControl`, `IDemodulatorControl` |

**Replaces** `ModuleComManager`'s opaque `callInterface(name, CMD_CODE, void*, void*)` with:
```cpp
auto* radio = ServiceRegistry::get().query<IRadioControl>("Radio");
if (radio) { radio->setMode(RADIO_IFACE_MODE_NFM); }
```

#### 1.4 Core Wiring

EventBus emissions added alongside existing `Event<T>` in:
- `core/src/signal_path/source.cpp` -- `SourceRegistered`, `SourceUnregistered`, `FrequencyChanged`
- `core/src/signal_path/sink.cpp` -- `SinkProvider*`, `Stream*`, `StreamSampleRateChanged`
- `core/src/signal_path/vfo_manager.cpp` -- `VFOCreated`, `VFODeleted`
- `core/src/gui/main_window.cpp` -- `PlayStateChanged`
- `core/src/module.cpp` -- `ModuleInstanceCreated`, `ModuleInstanceDeleted`
- `core/src/core.cpp` -- `InputSampleRateChanged`

#### 1.5 Module Loader V2 Support

Modified `core/src/module.h` and `core/src/module.cpp`:
- Loader probes for `_INFO_V2_` symbol via `dlsym`/`GetProcAddress`
- Checks API version compatibility before calling `_INIT_()`
- `checkDependencies()` validates all declared dependencies are loaded
- V1 modules (no `_INFO_V2_`) load exactly as before

#### 1.6 Proof-of-Concept Module Conversions

| Module | Changes |
|--------|---------|
| `source_modules/file_source/src/main.cpp` | Added `SDRPP_MOD_INFO_V2` with `MOD_CAP_SOURCE` |
| `decoder_modules/radio/src/radio_module.h` | Added `RadioControlAdapter` implementing `IRadioControl`, registered with `ServiceRegistry` alongside existing `ModuleComManager` |

#### 1.7 Resource Auto-Detection

Modified `core/src/core.cpp`: when `resourcesDirectory` from config doesn't exist, searches 10 candidate paths relative to executable and CWD. Auto-updates config.json on success.

---

## Phase 2: DSP Engine Modernization

**Status: DONE (committed, needs verification)**

Replaces thread-per-block with a configurable thread pool and provides zero-copy splitter for fan-out paths.

### Key Design Decision

`block::doStart()` and `block::doStop()` are virtual (`core/src/dsp/block.h:71-94`). We introduced `scheduled_block` that overrides these to use a thread pool. **Zero changes to `block.h`**. All 52 existing blocks continue working. Migration is one-line per block.

**Deferred:** Typed stream connections -- templates already enforce type safety at compile time. `untyped_stream*` in `block.h` is internal plumbing never touched by module authors. Effort-to-value ratio is poor.

**Deferred:** Full DSP graph abstraction -- data flow is implicit in stream connections and works well. No benefit from explicit DAG.

### What Was Built

#### 2.1 Thread Pool (`core/src/dsp/engine/thread_pool.h`)

```
ThreadPool(numThreads)     Create pool (default: hardware_concurrency())
submit(fn) → future<int>   Submit work, get future for result
submitAsync(fn)             Fire-and-forget work submission
getPool()                   Global singleton accessor
```

Standard work queue with mutex + condition variable. Pool threads are long-lived.

#### 2.2 Scheduler (`core/src/dsp/engine/scheduler.h`)

```
registerBlock(block*)       Register block for pool-based execution
unregisterBlock(block*)     Unregister, wait for in-flight run() to complete
getCallback(block*)         Get onDataReady callback to install on input stream
```

**Single-flight guarantee:** A block's `run()` is never called concurrently from two pool threads. Uses `atomic<bool> inFlight` per block with compare-exchange.

**Execution model:** When `onDataReady` fires (stream has data), scheduler submits block's `run()` to pool. Block's `run()` loops while data is available, then returns. Pool thread becomes available for other blocks.

#### 2.3 Scheduled Block (`core/src/dsp/engine/scheduled_block.h`)

Subclass of `dsp::block` that overrides:
- `doStart()` -- registers with scheduler, installs `onDataReady` on input streams
- `doStop()` -- removes callbacks, unregisters (waits for in-flight), cleans up streams

#### 2.4 Typed Wrappers (`core/src/dsp/engine/scheduled_processor.h`)

- `ScheduledProcessor<I, O>` -- drop-in replacement for `Processor<I, O>`
- `ScheduledSink<T>` -- drop-in replacement for `Sink<T>`

Same API, same `_in`/`out`/`base_type`, same macro compatibility. Migration is:
```cpp
// Before:
class Conjugate : public Processor<complex_t, complex_t> {
    using base_type = Processor<complex_t, complex_t>;

// After:
class Conjugate : public ScheduledProcessor<complex_t, complex_t> {
    using base_type = ScheduledProcessor<complex_t, complex_t>;
```

#### 2.5 Stream Hook (`core/src/dsp/stream.h`)

Added to `untyped_stream`:
```cpp
std::function<void()>* onDataReady = nullptr;
```

Added to `stream<T>::swap()`, after `rdyCV.notify_all()`:
```cpp
if (onDataReady) { (*onDataReady)(); }
```

Backward compatible: nullptr by default. Only set by scheduler for scheduled blocks. Existing threaded blocks ignore it (they wait on the CV directly).

#### 2.6 Zero-Copy Infrastructure

| File | Purpose |
|------|---------|
| `core/src/dsp/buffer/shared_buffer.h` | `SharedBuffer<T>` with atomic refcount + `SharedBufferPool<T>` for recycling |
| `core/src/dsp/routing/zero_copy_splitter.h` | `ZeroCopySplitter<T>` -- drop-in Splitter replacement using shared buffer pool |

Current `ZeroCopySplitter` still does memcpy (shared buffer as staging area). True zero-copy with alias streams is a follow-up once `stream<T>` supports aliased reads.

#### 2.7 Proof of Concept

`core/src/dsp/math/conjugate.h` -- migrated from `Processor` to `ScheduledProcessor`. Verified compilation.

### Remaining: Block Migration

52 blocks total, 1 migrated (Conjugate). Migration order by risk:

#### Group 1: Leaf/Stateless (~12 blocks, 1-line each)

These have trivial `run()` with no internal state beyond what `ctrlMtx` protects:

| Block | File |
|-------|------|
| `math::Conjugate` | `dsp/math/conjugate.h` | DONE |
| `math::Delay` | `dsp/math/delay.h` |
| `convert::ComplexToReal` | `dsp/convert/complex_to_real.h` |
| `convert::ComplexToStereo` | `dsp/convert/complex_to_stereo.h` |
| `convert::MonoToStereo` | `dsp/convert/mono_to_stereo.h` |
| `convert::RealToComplex` | `dsp/convert/real_to_complex.h` |
| `convert::StereoToMono` | `dsp/convert/stereo_to_mono.h` |
| `convert::LRToStereo` | `dsp/convert/l_r_to_stereo.h` |
| `digital::BinarySlicer` | `dsp/digital/binary_slicer.h` |
| `digital::DifferentialDecoder` | `dsp/digital/differential_decoder.h` |
| `digital::ManchesterDecoder` | `dsp/digital/manchester_decoder.h` |
| `sink::Null` | `dsp/sink/null_sink.h` |

#### Group 2: Stateful Processors (~15 blocks, 1-line each)

Have internal filter state, but `process()` is reentrant when `ctrlMtx` is held for reconfig:

| Block | File |
|-------|------|
| `filter::FIR` | `dsp/filter/fir.h` |
| `filter::DecimatingFIR` | `dsp/filter/fir.h` |
| `filter::Deemphasis` | `dsp/filter/deephasis.h` |
| `loop::AGC` | `dsp/loop/agc.h` |
| `loop::FastAGC` | `dsp/loop/fast_agc.h` |
| `loop::PLL` | `dsp/loop/pll.h` |
| `loop::CarrierTrackingPLL` | `dsp/loop/carrier_tracking_pll.h` |
| `loop::Costas` | `dsp/loop/costas.h` |
| `correction::DCBlocker` | `dsp/correction/dc_blocker.h` |
| `noise_reduction::NoiseBlanker` | `dsp/noise_reduction/noise_blanker.h` |
| `noise_reduction::PowerSquelch` | `dsp/noise_reduction/power_squelch.h` |
| `noise_reduction::CTCSSSquelch` | `dsp/noise_reduction/ctcss_squelch.h` |
| `noise_reduction::FMIF` | `dsp/noise_reduction/fm_if.h` |
| `audio::Volume` | `dsp/audio/volume.h` |
| `bench::PeakLevelMeter` | `dsp/bench/peak_level_meter.h` |

#### Group 3: Composite (~10 blocks, 1-line each)

These call child blocks' `process()` inline (children never start their own threads):

| Block | File |
|-------|------|
| `demod::Quadrature` | `dsp/demod/quadrature.h` |
| `demod::FM` | `dsp/demod/fm.h` |
| `demod::AM` | `dsp/demod/am.h` |
| `demod::SSB` | `dsp/demod/ssb.h` |
| `demod::CW` | `dsp/demod/cw.h` |
| `demod::BroadcastFM` | `dsp/demod/broadcast_fm.h` |
| `demod::GFSK` | `dsp/demod/gfsk.h` |
| `demod::PSK` | `dsp/demod/psk.h` |
| `channel::RxVFO` | `dsp/channel/rx_vfo.h` |
| `multirate::RationalResampler` | `dsp/multirate/rational_resampler.h` |

#### Group 4: Complex (3 blocks, full rewrites needed)

These extend `block` directly or spawn multiple threads:

| Block | File | Issue |
|-------|------|-------|
| `buffer::Reshaper` | `dsp/buffer/reshaper.h` | Spawns 2 threads, marked "TRASH" in comments |
| `buffer::SampleFrameBuffer` | `dsp/buffer/frame_buffer.h` | Spawns 2 threads, marked "TRASH" in comments |
| `buffer::Packer` | `dsp/buffer/packer.h` | Extends `block` directly, not typed |

#### Group 5: Routing/Sinks (~6 blocks)

| Block | File | Notes |
|-------|------|-------|
| `routing::Splitter` | `dsp/routing/splitter.h` | Replace with ZeroCopySplitter in IQFrontEnd |
| `routing::Doubler` | `dsp/routing/doubler.h` | |
| `routing::StreamLink` | `dsp/routing/stream_link.h` | |
| `sink::Handler` | `dsp/sink/handler_sink.h` | |
| `sink::RingBuffer` | `dsp/sink/ring_buffer.h` | |
| `multirate::PowerDecimator` | `dsp/multirate/power_decimator.h` | |

---

## Phase 3: GUI Decoupling

**Status: DONE (uncommitted, needs verification)**

Separates UI from business logic so modules don't need direct references to waterfall, menus, or GUI state.

### 3.1 GUI Abstraction Layer

**Problem:** Demodulators directly call `gui::waterfall.onFFTRedraw`, modules reference `gui::waterfall.selectedVFO`, and menu rendering is tightly coupled to core.

**Solution:**
- Module provides an `ISettingsPanel` interface; core renders it
- Waterfall/spectrum becomes a standalone component with event-driven updates via EventBus
- Modules subscribe to `events::FFTRedraw` instead of holding waterfall references

**Files to create:**
| File | Purpose |
|------|---------|
| `core/src/gui/settings_panel.h` | `ISettingsPanel` interface with `draw()`, `getTitle()` |
| `core/src/gui/gui_events.h` | GUI-specific events (FFT data, waterfall interaction, menu state) |

**Files to modify:**
| File | Change |
|------|--------|
| `core/src/gui/widgets/waterfall.h` | Emit EventBus events instead of direct `Event<T>` |
| `core/src/gui/main_window.cpp` | Query `ISettingsPanel` from modules instead of hardcoded menu entries |

### 3.2 ViewModel Pattern

**Problem:** DSP threads update shared state that the GUI reads each frame. Current ad-hoc mutex usage risks races.

**Solution:**
- Observable state objects (`ViewModel<T>`) that DSP threads write to atomically
- GUI thread polls state each frame (no locks on hot path)
- Double-buffered state: DSP writes to back buffer, GUI reads front buffer, swap on frame boundary

**Files to create:**
| File | Purpose |
|------|---------|
| `core/src/gui/view_model.h` | `ViewModel<T>` template with atomic swap semantics |

### 3.3 Headless Mode Cleanup

**Problem:** Headless/server mode uses `#ifdef` scattered through GUI code.

**Solution:** With GUI abstraction layer, headless mode simply doesn't create GUI components. SmGui already handles serialization for server mode. The abstraction layer makes this cleaner.

---

## Phase 4: Module Ecosystem

**Status: DONE (uncommitted, needs verification)**

Improves module isolation, configuration, and hardware abstraction interfaces.

### 4.1 Plugin Error Isolation

**Problem:** Module crash (segfault, uncaught exception) kills the entire application.

**Solution (minimum viable):**
- Wrap all module entry points (`_CREATE_INSTANCE_`, `postInit`, `enable`, `disable`) in try/catch
- On exception: log error, disable module, continue running
- Mark module as "faulted" in UI with error message

**Solution (advanced, deferred):**
- Run modules in separate processes with shared-memory IPC for streams
- Crash of one module doesn't affect others
- Significant complexity; only pursue if demand exists

**Files to modify:**
| File | Change |
|------|--------|
| `core/src/module.cpp` | Add try/catch around `createInstance()`, `postInit()`, `enable()`, `disable()` |
| `core/src/module.h` | Add `faulted` flag and error message to `Instance_t` |

### 4.2 Unified Configuration

**Problem:** Each module creates its own `ConfigManager` with a separate JSON file. No schema validation, no auto-generated settings UI, no migration support.

**Solution:**
- Module manifest (V2) declares a JSON schema for its configuration
- Core validates config on load, provides defaults for missing fields
- Optional: auto-generate basic settings UI from schema (combo boxes, sliders, checkboxes)

**Files to create:**
| File | Purpose |
|------|---------|
| `core/src/config_schema.h` | Schema definition types (string, int, float, enum, range) |
| `core/src/config_validator.h` | Validate JSON against schema, apply defaults, report errors |

### 4.3 Source/Sink Interface Modernization

**Problem:** `SourceHandler` uses C-style function pointers + `void* ctx`. No capability queries, no standard device enumeration.

**Solution:**
- `ISource` virtual class replacing `SourceHandler` struct
- Capability queries: `supports(Feature::FullDuplex)`, `getGainRange()`, `getSampleRates()`
- Standard device enumeration API: `listDevices()` returning structured device info
- Backward compatible: adapter wraps old `SourceHandler` into `ISource`

**Files to create:**
| File | Purpose |
|------|---------|
| `core/src/signal_path/isource.h` | `ISource` interface with capabilities, device enumeration |
| `core/src/signal_path/isink.h` | `ISink` interface (replaces `SinkProvider` function pointers) |
| `core/src/signal_path/source_adapter.h` | Adapter wrapping `SourceHandler` → `ISource` for backward compatibility |

---

## Phase 5: Build & Distribution

**Status: DONE (committed, needs verification)**

Standardizes module discovery and enables a plugin ecosystem.

### 5.1 Standardized Plugin Discovery

**Problem:** Module paths are hardcoded in config.json. Each platform has different conventions. No directory scanning.

**Solution:**
- On startup, scan standard plugin directories:
  - Linux: `~/.local/lib/sdrpp/plugins`, `/usr/lib/sdrpp/plugins`, `/usr/local/lib/sdrpp/plugins`
  - macOS: `~/Library/Application Support/sdrpp/plugins`, `/usr/local/lib/sdrpp/plugins`, `<bundle>/Plugins`
  - Windows: `%APPDATA%/sdrpp/plugins`, `<exe_dir>/modules`
- Auto-discover all `.so`/`.dylib`/`.dll` files
- Config.json overrides for explicit enable/disable per module

**Files to modify:**
| File | Change |
|------|--------|
| `core/src/core.cpp` | Add plugin directory scanning logic after config load |
| `core/src/module.cpp` | Add `scanDirectory(path)` method |

### 5.2 Version Compatibility Matrix

**Problem:** No checking between module binary and core version. Breaking changes silently fail.

**Solution:**
- V2 modules already declare `apiVersion` -- enforce check at load time (done in Phase 1)
- Add `SDRPP_CORE_VERSION` to the binary (already have `VERSION_STR`)
- Log a matrix at startup: module name, module version, API version, compatible (yes/no)
- Reject incompatible modules with clear error message (done in Phase 1)

### 5.3 Remote Module Repository (Future)

**Deferred indefinitely.** Would require:
- Central package registry (web service)
- Signed module binaries
- Automatic download and installation
- Far exceeds current project scope

---

## Test Infrastructure

**Status: DONE (committed, needs verification)**

25 test cases, 92 assertions, FIRST-compliant. Catch2 single-header framework.

### Framework Selection

**Recommendation:** Catch2 single-header (v2.x). Reasons:
- Single header file, no build system changes needed
- Works on all platforms (Windows, Linux, macOS)
- Widely used in C++ embedded/DSP projects
- No dependencies

### Test Categories

#### Unit Tests (Priority 1)

| Component | Test Cases |
|-----------|-----------|
| `EventBus` | Subscribe/publish, multiple subscribers, unsubscribe via RAII, unsubscribe via manual call, publish with no subscribers, thread safety |
| `ServiceRegistry` | Provide/query/remove, duplicate provide, query nonexistent, listNames, type isolation (same name different types), thread safety |
| `ThreadPool` | Submit work, multiple tasks, concurrent execution, shutdown with pending tasks, future result retrieval |
| `Scheduler` | Register/unregister, single-flight guarantee, data-ready triggers run(), unregister waits for in-flight |
| `SharedBuffer` | Refcount addRef/release, pool acquire/recycle, pool grows on demand |
| `stream<T>` | swap/read/flush cycle, stopWriter/stopReader, onDataReady callback fires on swap |
| `sdrppApiCompatible()` | Same major/minor, higher minor, different major, edge cases |

#### Integration Tests (Priority 2)

| Scenario | What it validates |
|----------|------------------|
| `ScheduledProcessor` data flow | Create Conjugate, feed data, verify output matches expected |
| Mixed old/new blocks in chain | `chain<T>` with some Processor and some ScheduledProcessor blocks |
| EventBus + core wiring | Trigger frequency change, verify event fires with correct data |
| ServiceRegistry + radio module | Register IRadioControl, query from another "module", call methods |

#### Benchmarks (Priority 3)

| Benchmark | What it measures |
|-----------|-----------------|
| Thread-per-block vs pool | Throughput for 10-block chain, both modes |
| Splitter: memcpy vs zero-copy | Fan-out to 3 outputs at 2.4 Msps |
| EventBus overhead | Publish latency with 0, 1, 10, 100 subscribers |

### Test File Layout

```
tests/
├── CMakeLists.txt           # Test build configuration
├── catch2/
│   └── catch.hpp            # Catch2 single header
├── unit/
│   ├── test_event_bus.cpp
│   ├── test_service_registry.cpp
│   ├── test_thread_pool.cpp
│   ├── test_scheduler.cpp
│   ├── test_shared_buffer.cpp
│   ├── test_stream.cpp
│   └── test_api_version.cpp
├── integration/
│   ├── test_scheduled_processor.cpp
│   ├── test_mixed_chain.cpp
│   └── test_event_wiring.cpp
└── bench/
    ├── bench_thread_pool.cpp
    ├── bench_splitter.cpp
    └── bench_event_bus.cpp
```

---

## Migration Tracker

Legend: Committed = in git, needs verification testing. Uncommitted = working tree only.

### Phase 1-2 (Committed: `a8bdebb3`, needs verification)

Committed but requires full integration testing before considered production-ready.

| File | Phase | Change |
|------|-------|--------|
| `core/src/api_version.h` | P1 | New: API version constants |
| `core/src/module_manifest.h` | P1 | New: V2 module manifest with capabilities + dependencies |
| `core/src/utils/event_bus.h` | P1 | New: Typed pub/sub EventBus |
| `core/src/utils/events.h` | P1 | New: 15 standard event types |
| `core/src/utils/service_registry.h` | P1 | New: Typed service locator |
| `core/src/utils/services.h` | P1 | New: IRadioControl, IRecorderControl, IDemodulatorControl |
| `core/src/module.h` | P1 | V2 manifest support, checkDependencies(), faulted flag |
| `core/src/module.cpp` | P1+P4 | V2 loader, try/catch error isolation on all entry points |
| `core/src/core.cpp` | P1+P5 | EventBus emissions, resource auto-detect, pool shutdown |
| `core/src/signal_path/source.cpp` | P1 | EventBus emissions |
| `core/src/signal_path/sink.cpp` | P1 | EventBus emissions |
| `core/src/signal_path/vfo_manager.cpp` | P1 | EventBus emissions |
| `core/src/gui/main_window.cpp` | P1+P5 | EventBus, modules dir auto-detect, recursive scan |
| `decoder_modules/radio/src/radio_module.h` | P1 | RadioControlAdapter + ServiceRegistry |
| `source_modules/file_source/src/main.cpp` | P1 | V2 manifest |
| `core/src/dsp/engine/thread_pool.h` | P2 | New: Elastic thread pool with timeout shutdown |
| `core/src/dsp/engine/scheduler.h` | P2 | New: Data-driven scheduler (available for future use) |
| `core/src/dsp/engine/scheduled_block.h` | P2 | New: Pool-based block execution |
| `core/src/dsp/engine/scheduled_processor.h` | P2 | New: ScheduledProcessor, ScheduledSink, ScheduledOperator |
| `core/src/dsp/buffer/shared_buffer.h` | P2 | New: Ref-counted buffer pool |
| `core/src/dsp/routing/zero_copy_splitter.h` | P2 | New: ZeroCopySplitter |
| `core/src/dsp/stream.h` | P2 | onDataReady callback hook |
| `core/src/dsp/chain.h` | P2 | Type-erased ChainLink for Scheduled* compat |
| 55 DSP block headers | P2 | Migrated to Scheduled* base classes |
| `core/src/dsp/buffer/frame_buffer.h` | P2 | Pool threads via custom doStart/doStop |
| `CMakeLists.txt` | P5 | Auto-detect deps, config summary, module dependency tracking |
| `tests/` (full directory) | Test | Catch2 + 25 test cases, 92 assertions |
| `docs/architecture.md` | Docs | Full architecture reference |
| `docs/module-api-v2.md` | Docs | V2 API reference |
| `docs/modular-rewrite-plan.md` | Docs | This file |

### Phase 3-4 (Uncommitted -- needs verification)

| File | Phase | Change | Verify |
|------|-------|--------|--------|
| `core/src/utils/radio_state.h` | P3 | New: IRadioState read-only interface | Build |
| `core/src/utils/radio_control.h` | P3 | New: IRadioStateControl mutation interface | Build |
| `core/src/utils/radio_state_adapter.h` | P3 | New: Adapter wrapping gui::waterfall | Run app, check waterfall works |
| `core/src/gui/main_window.cpp` | P3 | Register RadioStateAdapter with ServiceRegistry | Run app |
| `core/src/gui/menus/module_manager.cpp` | P4 | Red highlight + tooltip for faulted modules | Simulate fault |
| `core/src/utils/services.h` | P4 | Renamed IRecorderControl/IDemodulatorControl methods | Build |
| `core/src/signal_path/isource.h` | P4 | New: ISource virtual interface | Build |
| `core/src/signal_path/isink_provider.h` | P4 | New: ISinkProvider (inside sink.h) | Build |
| `core/src/signal_path/source.h` | P4 | registerSource(name, ISource*) overload | Build |
| `core/src/signal_path/source.cpp` | P4 | ISource adapter with lambda trampolines | Run file_source |
| `core/src/signal_path/sink.h` | P4 | ISinkProvider + registerSinkProvider overload | Build |
| `core/src/signal_path/sink.cpp` | P4 | ISinkProvider adapter | Build |
| `source_modules/file_source/src/main.cpp` | P4 | Full ISource conversion (proof of concept) | Load file, play WAV |
| `decoder_modules/*/src/main.cpp` (7 files) | P3 | getBandwidth() via IRadioState | Run decoders |
| `misc_modules/recorder/src/main.cpp` | P3+P4 | IRadioState + IRecorderControl provider | Record audio |
| `misc_modules/discord_integration/src/main.cpp` | P3+P4 | IRadioState + IRadioControl query | Check Discord presence |
| `misc_modules/rigctl_server/src/main.cpp` | P3+P4 | Full ServiceRegistry migration (0 modComManager calls) | Test rigctl |
| `misc_modules/frequency_manager/src/main.cpp` | P3+P4 | IRadioControl via ServiceRegistry | Apply bookmark |
| `misc_modules/scanner/src/main.cpp` | P3 | IRadioState for FFT reads | Run scanner |
| `misc_modules/scheduler/src/actions/tune_vfo.h` | P3 | IRadioState for VFO names | Build |
| `tests/unit/test_service_registry.cpp` | Test | FIRST compliance fix (isolated listNames) | Run tests |

### Verification Checklist

All phases (committed and uncommitted) need verification:

**Build & Tests (Phase 1-5):**
- [ ] `cmake .. && make` builds with zero errors (all default modules)
- [ ] `./sdrpp_unit_tests` -- 92 assertions, 25 test cases, all pass
- [ ] App starts without crashes, no ERROR in log (WARN is ok)
- [ ] App shuts down cleanly (no hang, "Exiting successfully" logged)

**Phase 1-2 (EventBus, DSP Pool):**
- [ ] Waterfall renders and scrolls smoothly
- [ ] Audio plays without glitches or dropouts
- [ ] Start/stop SDR source works repeatedly
- [ ] Changing sample rate works
- [ ] VFO drag-tuning works

**Phase 3-4 (GUI Decoupling, Service Interfaces):**
- [ ] App starts, waterfall renders, audio plays (with RTL-SDR or file source)
- [ ] File source: load WAV, play, verify center freq lock works
- [ ] Radio decoder: change mode (AM/FM/SSB), verify bandwidth changes
- [ ] Recorder: start/stop recording, verify file saved with correct frequency
- [ ] Frequency manager: create bookmark, apply it, verify mode/bandwidth set
- [ ] Scanner: run frequency scan, verify FFT reads work
- [ ] Rigctl server: connect gpredict or rigctl client, verify mode get/set
- [ ] Faulted module UI: add `throw` in a module constructor, verify red highlight + tooltip
- [ ] Module manager: add/remove instances, verify config saved
- [ ] App closes cleanly (no hang at shutdown)

## Final Summary

### What Was Built (All Phases)

| Component | Description |
|-----------|-------------|
| **EventBus** | Typed pub/sub with RAII subscriptions, 15 standard events |
| **ServiceRegistry** | Typed service locator, replaces void* modComManager |
| **API Versioning** | V2 module manifest with capabilities + dependency resolution |
| **Elastic Thread Pool** | DSP blocks run on pool threads, grows on demand, timeout shutdown |
| **55 DSP blocks migrated** | All Processor/Sink/Operator blocks on ScheduledProcessor |
| **chain<T> type erasure** | ChainLink supports both old and new block types |
| **IRadioState / IRadioStateControl** | GUI-decoupled radio state access (13/15 modules migrated) |
| **ISource / ISinkProvider** | Virtual interfaces with backward-compatible adapters |
| **IRadioControl / IRecorderControl / IDemodulatorControl** | Typed service interfaces replacing modComManager |
| **Error isolation** | try/catch on all module entry points, faulted flag + UI indicator |
| **CMake auto-detection** | Dep scanning, config summary, module dir discovery, CLion compat |
| **Resource auto-detection** | 10 candidate paths for dev builds |
| **Test infrastructure** | Catch2, 25 test cases, 92 assertions, FIRST compliant |
| **Documentation** | architecture.md, module-api-v2.md, this plan |

### What Remains (Optional, Non-Blocking)

| Item | Reason to defer |
|------|----------------|
| Config schema validation | Every module has different needs, JSON is flexible enough |
| 27 source modules to ISource | Adapter handles them, migrate opportunistically |
| 5 sink modules to ISinkProvider | Adapter handles them |
| frequency_manager FFT overlay | Inherently GUI-coupled (draws on spectrum) |
| WFM demod FFT overlay | Inherently GUI-coupled (draws RDS data) |
| Remove modComManager provider registrations | Keep for backward compat with potential external modules |
| Remove SourceHandler struct | Keep indefinitely as C-compatible API |
