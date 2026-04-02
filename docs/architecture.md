# SDR++ Architecture Overview

A comprehensive technical reference for the SDR++ codebase -- a cross-platform Software Defined Radio application written in C++17.

## Table of Contents

- [Signal Flow](#signal-flow)
  - [Complete Data Flow Diagram](#complete-data-flow-diagram)
  - [Stream Architecture](#stream-architecture)
  - [IQ Frontend Pipeline](#iq-frontend-pipeline)
  - [VFO Channel Processing](#vfo-channel-processing)
  - [Audio Output Path](#audio-output-path)
  - [FFT / Display Path](#fft--display-path)
- [Threading Model](#threading-model)
- [Project at a Glance](#project-at-a-glance)
- [Directory Structure](#directory-structure)
- [Core Library](#core-library)
  - [DSP Engine](#dsp-engine)
  - [Signal Path](#signal-path)
  - [GUI Layer](#gui-layer)
  - [Configuration System](#configuration-system)
  - [Module System](#module-system)
  - [Server Mode](#server-mode)
- [Module Categories](#module-categories)
- [Build System](#build-system)
- [Key Constants](#key-constants)

---

## Signal Flow

### Complete Data Flow Diagram

The entire application is a streaming pipeline from RF hardware to audio output, with a parallel path for spectrum visualization:

```
                        ┌─────────────────────────────────────────┐
                        │           SDR HARDWARE / SOURCE         │
                        │  (RTL-SDR, Airspy, HackRF, file, ...)  │
                        └──────────────────┬──────────────────────┘
                                           │
                              dsp::stream<complex_t>
                                    (raw IQ samples)
                                           │
                        ┌──────────────────▼──────────────────────┐
                        │            IQ FRONTEND                  │
                        │                                         │
                        │  ┌─────────────────────────────────┐    │
                        │  │  SampleFrameBuffer              │    │
                        │  │  (variable → fixed block size)  │    │
                        │  └──────────────┬──────────────────┘    │
                        │                 │                       │
                        │  ┌──────────────▼──────────────────┐    │
                        │  │  Pre-processing Chain           │    │
                        │  │  ┌──────────────────────────┐   │    │
                        │  │  │ PowerDecimator (÷2,÷4..) │   │    │
                        │  │  ├──────────────────────────┤   │    │
                        │  │  │ Conjugate (IQ inversion) │   │    │
                        │  │  ├──────────────────────────┤   │    │
                        │  │  │ DC Blocker (50 Hz HPF)   │   │    │
                        │  │  └──────────────────────────┘   │    │
                        │  └──────────────┬──────────────────┘    │
                        │                 │                       │
                        │  ┌──────────────▼──────────────────┐    │
                        │  │          SPLITTER               │    │
                        │  │    (1 input → N outputs)        │    │
                        │  └──┬───────────────────────┬──────┘    │
                        │     │                       │           │
                        └─────┼───────────────────────┼───────────┘
                              │                       │
              ┌───────────────▼───────┐   ┌──────────▼──────────────────────┐
              │    FFT PATH           │   │    VFO CHANNELS                 │
              │                       │   │                                 │
              │  Reshaper             │   │  ┌────────────┐ ┌────────────┐  │
              │  (→ 65536 samples)    │   │  │ VFO "Radio"│ │ VFO "Scan" │  │
              │       │               │   │  │            │ │            │  │
              │  FFT (FFTW3)          │   │  │  RxVFO:    │ │  RxVFO:   │  │
              │       │               │   │  │  ├─ NCO    │ │  ├─ NCO   │  │
              │  ┌────▼────┐          │   │  │  ├─ Resamp │ │  ├─ Resamp│  │
              │  │Spectrum │          │   │  │  └─ FIR    │ │  └─ FIR   │  │
              │  │   +     │          │   │  └─────┬──────┘ └─────┬─────┘  │
              │  │Waterfall│          │   │        │              │        │
              │  │ display │          │   └────────┼──────────────┼────────┘
              │  └─────────┘          │            │              │
              └───────────────────────┘            │              │
                                      complex_t   │              │ complex_t
                                                   │              │
                        ┌──────────────────────────▼──┐   ┌──────▼──────────┐
                        │  RADIO DECODER MODULE       │   │ OTHER DECODER   │
                        │                             │   │ (pager, m17,    │
                        │  IF Chain (toggleable):     │   │  meteor, etc.)  │
                        │  ├─ Noise Blanker           │   └──────┬──────────┘
                        │  └─ FM IF Noise Reduction   │          │
                        │       │                     │          │
                        │  Demodulator:               │          │
                        │  ┌────────────────────────┐ │          │
                        │  │ WFM │ NFM │ AM │ SSB │ │ │          │
                        │  │ DSB │ CW  │RAW │     │ │ │          │
                        │  └──────────┬─────────────┘ │          │
                        │             │               │          │
                        │  AF Chain (toggleable):     │          │
                        │  ├─ CTCSS Squelch           │          │
                        │  ├─ Resampler (→ 48kHz)     │          │
                        │  ├─ High-pass (300 Hz)      │          │
                        │  └─ Deemphasis (50/75 μs)   │          │
                        └─────────────┬───────────────┘          │
                                      │                          │
                             stereo_t │                 stereo_t │
                                      │                          │
                        ┌─────────────▼──────────────────────────▼──┐
                        │              SINK MANAGER                 │
                        │                                           │
                        │  ┌─────────────────────────────────┐      │
                        │  │  Stream "Radio"                 │      │
                        │  │                                 │      │
                        │  │  Splitter ──┬──→ Recorder       │      │
                        │  │             │                   │      │
                        │  │             └──→ Volume Adjust  │      │
                        │  │                      │          │      │
                        │  │              Sink Provider      │      │
                        │  │            (audio / network)    │      │
                        │  └──────────────────┬──────────────┘      │
                        └─────────────────────┼─────────────────────┘
                                              │
                                              ▼
                        ┌─────────────────────────────────────────┐
                        │            AUDIO OUTPUT                 │
                        │  (RtAudio / PortAudio / Network / AAudio)│
                        └─────────────────────────────────────────┘
```

### Stream Architecture

Every arrow in the diagram above is a `dsp::stream<T>` -- a double-buffered, blocking communication channel:

```
┌─────────────────────────────────────────────────────────────┐
│  dsp::stream<T>                                             │
│                                                             │
│  ┌──────────────┐        swap()         ┌──────────────┐   │
│  │   writeBuf    │ ◄──────────────────► │   readBuf     │   │
│  │  (producer)   │     atomic exchange  │  (consumer)   │   │
│  │               │                      │               │   │
│  │  1M samples   │                      │  1M samples   │   │
│  │  VOLK-aligned │                      │  VOLK-aligned │   │
│  └──────────────┘                       └──────────────┘   │
│                                                             │
│  Producer:  write to writeBuf → swap(count)  [blocks]       │
│  Consumer:  read() → process readBuf → flush()              │
│  Shutdown:  stopWriter() / stopReader() → returns -1        │
└─────────────────────────────────────────────────────────────┘
```

**Buffer size:** 1,000,000 samples (`STREAM_BUFFER_SIZE`).
**Allocation:** VOLK-aligned (`volk_malloc`) for SIMD operations.
**Sync:** Dual mutex + condition variable pairs (one for swap readiness, one for data readiness).

### IQ Frontend Pipeline

The IQ Frontend transforms raw source samples into FFT display data and per-VFO filtered streams:

```
Source stream
     │
     ▼
SampleFrameBuffer ─── Converts variable-length hardware reads
     │                 into fixed-size blocks
     ▼
Pre-processing Chain (dsp::chain<complex_t>):
┌──────────────────────────────────────────┐
│  [PowerDecimator]  ÷2, ÷4, ÷8, ...     │  optional
│  [Conjugate]       swap I/Q              │  optional
│  [DCBlocker]       50 Hz highpass        │  optional
│                                          │
│  Blocks can be toggled at runtime.       │
│  Chain auto-rewires when toggled.        │
└──────────────────────────────────────────┘
     │
     ▼
Splitter ─── One input fans out to:
     ├──→ FFT reshaper + sink (display)
     ├──→ VFO "Radio" (RxVFO block)
     ├──→ VFO "Scan" (RxVFO block)
     └──→ ... more VFOs as needed
```

**Effective sample rate:** `source_rate / decimation_ratio`

### VFO Channel Processing

Each VFO extracts a narrow band from the wideband IQ stream:

```
Wideband IQ (e.g. 2.4 MHz @ center freq)
     │
     ▼
┌─────────────────────────────────────┐
│  RxVFO                              │
│                                     │
│  1. NCO (frequency shift)           │
│     Translates desired signal       │
│     to baseband (0 Hz)              │
│                                     │
│  2. Rational Resampler              │
│     Adjusts sample rate to          │
│     match demodulator needs         │
│                                     │
│  3. FIR Filter                      │
│     Bandwidth-limits the signal     │
│     (e.g. 12.5 kHz for NFM)        │
└──────────────────┬──────────────────┘
                   │
        dsp::stream<complex_t>
          (narrowband, resampled)
                   │
                   ▼
            Demodulator
```

**GUI coupling:** Each VFO has a paired `WaterfallVFO` that renders as a draggable overlay on the spectrum display. Dragging changes the VFO offset; resizing changes bandwidth.

### Audio Output Path

```
Demodulator → stereo_t stream
                  │
                  ▼
         SinkManager::Stream
         ┌────────────────────────────────────────┐
         │                                        │
         │  Splitter ──┬──→ bound stream          │
         │             │    (recorder, exporter)   │
         │             │                          │
         │             └──→ volumeInput           │
         │                     │                  │
         │              Volume Adjust             │
         │              (gain + mute)             │
         │                     │                  │
         │              sinkOut → Sink Provider   │
         │                                        │
         └────────────────────────────────────────┘
                                │
                                ▼
                     Audio device / Network
```

Sink providers are hot-swappable at runtime. The splitter allows multiple consumers (e.g., simultaneous playback and recording).

### FFT / Display Path

```
Splitter output (full-rate IQ)
     │
     ▼
Reshaper ─── Accumulates samples into FFT-sized blocks
     │       (default 65,536 samples)
     ▼
Handler Sink ─── Callback to MainWindow
     │
     ▼
┌──────────────────────────────────┐
│  MainWindow FFT Processing       │
│                                  │
│  1. Apply window function        │
│     (Blackman / Nuttall / Rect)  │
│                                  │
│  2. FFTW3 forward transform      │
│     (fftwf_execute)              │
│                                  │
│  3. Magnitude → dB conversion    │
│                                  │
│  4. Update spectrum + waterfall  │
│     display buffers              │
└──────────────────────────────────┘
```

**FFT rate:** Configurable (default 20 fps). The reshaper controls how often complete FFT blocks are delivered.

---

## Threading Model

DSP blocks run on an **elastic thread pool** (`dsp::engine::ThreadPool`), not dedicated threads. The pool grows automatically when all workers are busy, preventing deadlocks from blocking reads.

```
Main Thread
│  GLFW event loop + ImGui rendering
│  Menu drawing, waterfall rendering
│  Configuration changes, event dispatching
│
├── Auto-Save Thread(s)
│     Config writes every 1s if dirty (one per ConfigManager)
│
├── Source Thread
│     Hardware reads → stream.swap()
│
├── DSP Thread Pool (elastic, starts at hardware_concurrency)
│   │  All ScheduledProcessor/ScheduledSink blocks share this pool.
│   │  Pool grows when idle=0, workers block on stream::read().
│   ├── IQ Frontend blocks (decimator, DC blocker, splitter, FFT)
│   ├── VFO blocks (frequency xlator, resampler, filter)
│   ├── Demodulator blocks (FM, AM, SSB, CW, etc.)
│   ├── Post-processing blocks (resampler, HPF, squelch, AGC)
│   └── Sink handlers (audio, network, recorder)
│
└── Module-Specific Threads
    ├── Rigctl server socket
    ├── Scanner stepping
    └── Server TCP listener (if enabled)
```

**Thread pool benefits over thread-per-block:**
- Centralized lifecycle management and shutdown with timeout
- Thread reuse when blocks are stopped/restarted
- Observable: `size()` and `active()` for diagnostics

**Synchronization primitives:**
- `std::mutex` + `std::condition_variable` in streams (buffer swap coordination)
- `std::recursive_mutex` in blocks (allows nested `tempStop`/`tempStart`)
- `std::mutex` in ConfigManager (acquire/release pattern)
- `block::stop()` is `noexcept` -- exceptions caught to prevent `std::terminate` from destructors
- `scheduled_block::doStop()` uses 2-second timeout with retry for stuck workers

**Reconfiguration pattern** (`tempStop` / `tempStart`):
When changing a parameter that affects a running block (e.g., bandwidth, sample rate):
1. `block.tempStop()` -- increment stop depth, stop worker if depth goes 0 to 1
2. Modify parameters
3. `block.tempStart()` -- decrement stop depth, restart worker if depth goes 1 to 0

This nesting allows multiple callers to pause a block without conflicting.

**Shutdown sequence** (8 steps with logging):
1. Publish `ShutdownRequested` event with 5s timeout per handler
2. Stop IQ frontend
3. Disable proxy config auto-save
4. Delete all module instances (stop DSP chains consumers-first)
5. Call `_END_()` on all modules
6. Save and clear proxy configs
7. End backend (GLFW)
8. Shutdown thread pool and save core config

The `core::shuttingDown` flag prevents source cascade during teardown.

---

## Project at a Glance

| Aspect | Value |
|--------|-------|
| Language | C++17 |
| Lines of code | ~150K |
| Build system | CMake 3.13+ |
| GUI framework | ImGui (immediate mode) + GLFW3 + OpenGL |
| DSP architecture | Elastic thread pool, stream-based |
| Plugin system | Dynamic `.so`/`.dylib`/`.dll` loading |
| Configuration | JSON (nlohmann/json) |
| Platforms | Windows, Linux, macOS, Android |
| Core dependencies | FFTW3, GLFW3, Volk, zstd |

---

## Directory Structure

```
SDRPlusPlus/
├── core/                        # Core shared library (sdrpp_core)
│   ├── src/
│   │   ├── dsp/                 # DSP primitives (streams, blocks, processors)
│   │   │   ├── stream.h         # Dual-buffer inter-block communication
│   │   │   ├── block.h          # Thread-per-block base class
│   │   │   ├── processor.h      # Single-input/single-output transform
│   │   │   ├── chain.h          # Dynamic enable/disable processor chain
│   │   │   ├── types.h          # complex_t, stereo_t
│   │   │   ├── routing/         # Splitter, panner
│   │   │   ├── multirate/       # Decimators, resamplers
│   │   │   ├── demod/           # FM, AM, SSB, PSK demodulator blocks
│   │   │   ├── audio/           # Volume, deemphasis, stereo conversion
│   │   │   ├── filter/          # FIR, polyphase filters
│   │   │   ├── noise_reduction/ # Squelch, noise blanker, CTCSS
│   │   │   ├── correction/      # DC blocker, IQ correction
│   │   │   ├── buffer/          # Frame buffer, reshaper, packer
│   │   │   ├── sink/            # Null sink, handler sink
│   │   │   ├── bench/           # Peak level meter
│   │   │   └── math/            # Complex math, phase operations
│   │   ├── signal_path/         # High-level signal routing
│   │   │   ├── iq_frontend.h    # Source → decimation → FFT/VFO split
│   │   │   ├── source.h         # Source manager (hardware abstraction)
│   │   │   ├── sink.h           # Sink manager (audio output)
│   │   │   ├── vfo_manager.h    # Virtual frequency oscillator management
│   │   │   └── signal_path.h    # Global sigpath:: exports
│   │   ├── gui/                 # GUI components
│   │   │   ├── main_window.h    # Central render loop, FFT, play/stop
│   │   │   ├── gui.h            # Global gui:: exports (waterfall, menu)
│   │   │   ├── smgui.h          # Styled ImGui wrapper for server sync
│   │   │   ├── widgets/         # Waterfall, frequency select, SNR meter
│   │   │   ├── menus/           # Source, sink, display, theme menus
│   │   │   ├── dialogs/         # Credits, loading screen
│   │   │   ├── icons.h          # Icon loading and rendering
│   │   │   ├── style.h          # Theming and UI scale
│   │   │   └── tuner.h          # Frequency tuning logic
│   │   ├── utils/               # Utilities
│   │   │   ├── event.h          # V1 event system (function pointer + ctx)
│   │   │   ├── event_bus.h      # V2 typed pub/sub event bus
│   │   │   ├── events.h         # V2 standard event types
│   │   │   ├── service_registry.h # V2 typed service locator
│   │   │   ├── services.h       # V2 standard service interfaces
│   │   │   ├── flog.h           # Logging
│   │   │   └── optionlist.h     # Combo box option lists
│   │   ├── core.h / core.cpp    # Entry point, global managers
│   │   ├── module.h / module.cpp          # Module loader
│   │   ├── module_manifest.h              # V2 module manifest
│   │   ├── api_version.h                  # V2 API versioning
│   │   ├── module_com.h / module_com.cpp  # V1 inter-module IPC
│   │   ├── config.h / config.cpp          # JSON config manager
│   │   ├── server.h / server.cpp          # Headless server mode
│   │   └── json.hpp             # nlohmann/json (single header)
│   ├── backends/
│   │   ├── glfw/                # Desktop backend (GLFW + OpenGL)
│   │   └── android/             # Android backend (EGL + GLES3)
│   └── libcorrect/              # Bundled forward error correction
│
├── source_modules/              # 28 hardware/network/file sources
├── decoder_modules/             # 11 demodulator/decoder modules
├── sink_modules/                # 5 audio/network output modules
├── misc_modules/                # 9 utility modules
│
├── src/main.cpp                 # Thin entry point → sdrpp_main()
├── CMakeLists.txt               # Master build (70+ option flags)
├── sdrpp_module.cmake           # Shared module build template
├── root/res/                    # Runtime resources
│   ├── bandplans/               # Band allocation JSON files
│   ├── colormaps/               # Waterfall color palettes
│   ├── fonts/                   # UI fonts
│   ├── icons/                   # Toolbar/menu icons
│   └── themes/                  # ImGui theme files
└── docs/                        # Documentation
```

---

## Core Library

The `sdrpp_core` shared library (~49K LOC) provides everything modules need: DSP primitives, signal routing, GUI framework, configuration, and the module loader.

### DSP Engine

The DSP engine is a graph of processing blocks connected by typed streams.

#### Data Types (`dsp/types.h`)

```
complex_t { float re, im }     RF/IF signal (IQ samples)
stereo_t  { float l, r }       Audio signal (left/right)
```

Both types support arithmetic operators (`+`, `-`, `*`, `/`). `complex_t` also provides `phase()`, `amplitude()`, `conj()`, and fast approximations (`fastPhase()`, `fastAmplitude()`).

#### Stream (`dsp/stream.h`)

The fundamental inter-block communication channel. Double-buffered, thread-safe, blocking.

```
┌─────────────────────────────────────────────────┐
│  stream<T>                                      │
│                                                 │
│  writeBuf ──┐   swap()   ┌── readBuf            │
│  (producer) │ ←────────→ │ (consumer)           │
│             └────────────┘                      │
│                                                 │
│  Buffer size: STREAM_BUFFER_SIZE (1M samples)   │
│  Allocation: VOLK-aligned (SIMD-friendly)       │
│  Sync: mutex + condition_variable per direction │
└─────────────────────────────────────────────────┘
```

**Lifecycle:**
1. Producer writes samples into `writeBuf`
2. Producer calls `swap(count)` -- blocks until consumer is done with `readBuf`
3. Buffers are exchanged atomically
4. Consumer's `read()` unblocks, returns sample count
5. Consumer processes `readBuf`, calls `flush()` to release

**Shutdown:** `stopWriter()` / `stopReader()` break the blocking wait, causing `swap()` or `read()` to return `-1`.

#### Block (`dsp/block.h`)

Base class for all DSP processing nodes. Each block runs its own worker thread.

```
┌──────────────────────────────┐
│  block                       │
│                              │
│  inputs[]   ← registered     │
│  outputs[]  ← registered     │
│                              │
│  workerThread:               │
│    while (run() >= 0) {}     │
│                              │
│  ctrlMtx (recursive_mutex)   │
│  tempStop() / tempStart()    │
└──────────────────────────────┘
```

- `start()` spawns the worker thread
- `stop()` signals all I/O streams to stop, joins the thread
- `tempStop()` / `tempStart()` pause processing for reconfiguration (reference counted for nesting)
- `run()` is the pure virtual processing function, called in a loop

#### Processor (`dsp/processor.h`)

A block with exactly one typed input and one typed output:

```cpp
template <class I, class O>
class Processor : public block {
    stream<I>* _in;     // input
    stream<O>  out;     // output
};
```

Standard run loop pattern:
1. `count = _in->read()` -- block until input available
2. Process `_in->readBuf[0..count]` into `out.writeBuf`
3. `_in->flush()` -- release input buffer
4. `out.swap(count)` -- publish output

#### Chain (`dsp/chain.h`)

A sequence of same-type processors that can be individually enabled/disabled at runtime:

```
Input ──→ [Block A] ──→ [Block B] ──→ [Block C] ──→ Output
              on           off            on

Effective: Input ──→ [Block A] ──→ [Block C] ──→ Output
```

When a block is toggled, the chain:
1. Stops adjacent blocks
2. Rewires stream connections around the disabled block
3. Restarts affected blocks
4. Calls `onOutputChange()` callback if the chain output changes

Used for: post-processing chains (high-pass, deemphasis, squelch), IF processing chains.

---

### Signal Path

The signal path connects sources to sinks through the IQ frontend, VFOs, and demodulators.

#### IQ Frontend (`signal_path/iq_frontend.h`)

Central signal processing hub between the source and all consumers.

```
Source Stream (raw IQ)
     │
     ▼
┌─────────────────────────────┐
│  IQFrontEnd                 │
│                             │
│  inBuf (SampleFrameBuffer)  │
│     │                       │
│  preproc chain:             │
│  ├─ PowerDecimator          │
│  ├─ Conjugate (IQ invert)   │
│  └─ DCBlocker (50 Hz)       │
│     │                       │
│  Splitter ──┬──→ FFT path   │
│             │    (reshape    │
│             │     + sink)    │
│             │                │
│             ├──→ VFO "Radio" │
│             ├──→ VFO "Scan"  │
│             └──→ VFO ...     │
└─────────────────────────────┘
```

**Key parameters:**
- Sample rate, decimation ratio
- FFT size, rate, window function (Rectangular, Blackman, Nuttall)
- DC blocking on/off, IQ inversion on/off

**Effective sample rate:** `sourceSampleRate / decimationRatio`

#### Source Manager (`signal_path/source.h`)

Manages hardware/network sources. Only one source is active at a time.

Each source module registers a `SourceHandler` struct:

```
SourceHandler:
  stream*          → IQ output stream
  menuHandler()    → Draw ImGui settings UI
  selectHandler()  → Called when source is selected
  deselectHandler()→ Called when source is deselected
  startHandler()   → Start streaming
  stopHandler()    → Stop streaming
  tuneHandler()    → Set center frequency
  ctx              → Module instance pointer
```

**Tuning modes:**
- `NORMAL` -- tune to requested frequency
- `PANADAPTER` -- tune to fixed IF frequency

#### VFO Manager (`signal_path/vfo_manager.h`)

Creates virtual frequency oscillators for multi-channel reception. Each VFO:

```
VFO:
  dspVFO (RxVFO)           DSP block: frequency shift + resample + filter
  wtfVFO (WaterfallVFO)    GUI overlay on waterfall display
  output                    Filtered IQ stream for demodulator
```

**Parameters:** offset, bandwidth, sample rate, min/max bandwidth, snap interval, reference mode (LOWER/CENTER/UPPER), color.

#### Sink Manager (`signal_path/sink.h`)

Routes demodulated audio to output devices. Each audio stream flows through:

```
Demodulator output (stereo_t)
     │
     ▼
┌─────────────────────────────┐
│  SinkManager::Stream        │
│                             │
│  Splitter (multi-consumer)  │
│     │                       │
│  Volume control             │
│     │                       │
│  Sink (provider-specific)   │
│     │                       │
│  Audio device / network     │
└─────────────────────────────┘
```

Sink providers (audio_sink, network_sink, etc.) register a factory function. Streams bind to a provider and can switch providers at runtime.

#### Global Signal Path Exports

```cpp
namespace sigpath {
    IQFrontEnd    iqFrontEnd;
    VFOManager    vfoManager;
    SourceManager sourceManager;
    SinkManager   sinkManager;
};
```

---

### GUI Layer

ImGui-based immediate mode rendering with GLFW (desktop) or Android backend.

#### Backend (`backend.h`)

```
backend::init(resDir)     Initialize window, OpenGL context
backend::renderLoop()     Main loop: beginFrame → draw → render
backend::end()            Cleanup
```

#### Main Window (`gui/main_window.h`)

The central render coordinator:

```
┌─────────────────────────────────────────────────────┐
│  MainWindow                                         │
│                                                     │
│  ┌───────────────┬─────────────────────────────┐    │
│  │  Menu Panel   │  Spectrum + Waterfall        │    │
│  │  (300px)      │                              │    │
│  │               │  ┌──────────────────────┐    │    │
│  │  Source       │  │  FFT Spectrum (300px) │    │    │
│  │  Sinks       │  │  with VFO overlays    │    │    │
│  │  Band Plan   │  └──────────────────────┘    │    │
│  │  Display     │  ┌──────────────────────┐    │    │
│  │  Theme       │  │  Waterfall           │    │    │
│  │  Modules...  │  │  (scrolling)         │    │    │
│  │               │  └──────────────────────┘    │    │
│  └───────────────┴─────────────────────────────┘    │
│                                                     │
│  [Play/Stop] [Frequency] [Source Select] [Volume]   │
└─────────────────────────────────────────────────────┘
```

**FFT processing:**
- Size: 65,536 samples (default)
- Library: FFTW3 (float, in-place)
- Window: configurable (Blackman default)
- Rate: configurable (20 fps default)
- Display range: -70 dB to 0 dB (default)

#### Waterfall Widget (`gui/widgets/waterfall.h`)

Real-time spectrum and waterfall display:

- **WaterFall** -- renders the spectrum plot and scrolling waterfall
- **WaterfallVFO** -- interactive overlay per VFO (draggable, resizable)

VFO reference modes:
- `REF_LOWER` -- offset from lower bandwidth edge
- `REF_CENTER` -- offset from center (default)
- `REF_UPPER` -- offset from upper bandwidth edge

VFO events:
- `onUserChangedBandwidth` -- user dragged bandwidth edge
- `onUserChangedNotch` -- user adjusted notch filter

Waterfall events:
- `onFFTRedraw` -- new FFT data rendered (used by decoders for overlays)
- `onInputProcess` -- raw input events for custom interaction

#### SmGui (`gui/smgui.h`)

Wrapper around ImGui that records draw commands. In normal mode, it delegates directly to ImGui. In server mode, it serializes the draw list for remote UI synchronization (50+ command types: combo, button, slider, checkbox, text, tables, etc.).

#### GUI Global Exports

```cpp
namespace gui {
    ImGui::WaterFall   waterfall;
    FrequencySelect    freqSelect;
    Menu               menu;
    ThemeManager       themeManager;
    MainWindow         mainWindow;
};
```

---

### Configuration System

**Header:** `config.h`

JSON-based configuration with auto-save:

```
ConfigManager:
  path        → config file path
  conf        → nlohmann::json object
  mtx         → access mutex

  acquire()   → lock for read/write
  release(modified) → unlock, mark dirty if modified
  load(defaults) → load from file, merge defaults
  save()      → write to disk

  enableAutoSave()  → start background thread
  disableAutoSave() → stop background thread
```

**Auto-save thread:** Checks `changed` flag every 1 second, writes to disk if dirty. Graceful shutdown with condition variable.

**Per-module configs:** Each module creates its own `ConfigManager` with a separate JSON file (e.g., `rtl_sdr_source_config.json`).

**Global config** (`config.json`): Stores UI preferences, module instances, source/sink bindings, band plan colors, FFT settings, frequency, etc.

---

### Module System

Modules are shared libraries loaded at runtime via `dlopen` / `LoadLibrary`.

#### Module Lifecycle

```
loadModule(path)
  ├─ dlopen / LoadLibrary
  ├─ Resolve symbols: _INFO_, _INIT_, _CREATE_INSTANCE_, _DELETE_INSTANCE_, _END_
  ├─ (V2) Resolve _INFO_V2_, check API compatibility
  ├─ Call _INIT_()
  └─ Store in modules map

createInstance(name, moduleName)
  ├─ Check max instances
  ├─ (V2) Check dependencies
  ├─ Call _CREATE_INSTANCE_(name)
  ├─ Store in instances map
  └─ Emit onInstanceCreated

postInit(name)
  └─ Call instance->postInit()  (cross-module links established here)

enable/disable(name)
  └─ Call instance->enable() / disable()

deleteInstance(name)
  ├─ Emit onInstanceDelete
  ├─ Call _DELETE_INSTANCE_(instance)
  └─ Emit onInstanceDeleted

shutdown
  └─ Call _END_() for all modules
```

#### Module Interface

Every module must implement:

```cpp
class ModuleManager::Instance {
    virtual void postInit() = 0;
    virtual void enable() = 0;
    virtual void disable() = 0;
    virtual bool isEnabled() = 0;
};
```

And export these C symbols:

```
_INFO_            → ModuleInfo_t (name, description, author, version, maxInstances)
_INIT_()          → Module-level initialization
_CREATE_INSTANCE_ → Factory function returning Instance*
_DELETE_INSTANCE_  → Destructor
_END_()           → Module-level cleanup
```

#### V2 Extensions

V2 modules additionally export `_INFO_V2_`:

```
_INFO_V2_ → ModuleInfoV2 (extends ModuleInfo_t with:)
  apiVersion      → Compiled API version
  capabilities    → MOD_CAP_SOURCE | MOD_CAP_SINK | MOD_CAP_DECODER | MOD_CAP_MISC
  dependencyCount → Number of required modules
  dependencies[]  → { moduleName, minApiVersion } pairs
```

See [module-api-v2.md](module-api-v2.md) for the full V2 API reference.

#### Inter-Module Communication

**V1 (`ModuleComManager`):** Opaque command dispatch with integer codes and `void*` params.

```cpp
// Provider:
core::modComManager.registerInterface("radio", name, handler, this);
// Consumer:
core::modComManager.callInterface(name, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
```

**V2 (`ServiceRegistry`):** Typed service interfaces.

```cpp
// Provider:
ServiceRegistry::get().provide<IRadioControl>(name, adapter);
// Consumer:
auto* radio = ServiceRegistry::get().query<IRadioControl>(name);
if (radio) { mode = radio->getMode(); }
```

Both systems run in parallel during migration.

---

### Server Mode

SDR++ can run headless, serving IQ data and remote UI to network clients.

```
server::main()
  ├─ Listen on addr:port (TCP)
  ├─ Accept client connections
  ├─ Stream IQ samples to client
  ├─ Synchronize SmGui draw commands
  └─ Process remote commands (tune, start, stop, etc.)
```

SmGui records draw lists instead of rendering directly, enabling the server to serialize UI state over the network.

---

## Module Categories

### Source Modules (28)

Produce `dsp::stream<dsp::complex_t>` from hardware or network:

| Category | Modules |
|----------|---------|
| USB SDRs | `rtl_sdr`, `airspy`, `airspyhf`, `hackrf`, `bladerf`, `limesdr`, `plutosdr`, `sdrplay`, `usrp`, `rfnm`, `perseus`, `sddc`, `fobossdr`, `badgesdr`, `kcsdr` |
| Network | `rtl_tcp`, `spyserver`, `sdrpp_server`, `spectran_http`, `rfspace`, `hermes`, `network` |
| Specialty | `spectran`, `harogic`, `hydrasdr`, `dragonlabs`, `soapy` |
| File | `file_source` (WAV) |
| Audio | `audio_source` (soundcard loopback) |

### Decoder Modules (11)

Consume IQ from a VFO, produce `dsp::stream<dsp::stereo_t>`:

| Module | Modes |
|--------|-------|
| `radio` | NFM, WFM, AM, DSB, USB, LSB, CW, RAW |
| `m17_decoder` | M17 digital voice (Codec2) |
| `meteor_demodulator` | METEOR-M satellite QPSK |
| `pager_decoder` | POCSAG/FLEX paging |
| `atv_decoder` | Analog television |
| `dab_decoder` | DAB/DAB+ digital radio |
| `falcon9_decoder` | SpaceX telemetry |
| `kg_sstv_decoder` | KG-STV image mode |
| `ryfi_decoder` | RyFi data link |
| `vor_receiver` | VOR navigation beacon |
| `weather_sat_decoder` | NOAA HRPT |

#### Demodulator Interface

The `radio` module defines a demodulator plugin system within itself:

```cpp
class demod::Demodulator {
    virtual void init(name, config, input, bandwidth, audioSR) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void showMenu() = 0;
    virtual void setBandwidth(double) = 0;
    virtual dsp::stream<dsp::stereo_t>* getOutput() = 0;
    // + metadata: sample rates, bandwidth limits, feature flags
};
```

Built-in demodulators: WFM, NFM, AM, DSB, USB, LSB, CW, RAW.

**Audio post-processing chain** (toggleable per-block via `dsp::chain`):
- High-pass filter (300 Hz)
- Deemphasis (22/50/75 us or off)
- Squelch (power, SNR, CTCSS, DCS)
- CTCSS tone decoder
- Noise blanker
- FM IF noise reduction

### Sink Modules (5)

Consume `dsp::stream<dsp::stereo_t>`:

| Module | Backend |
|--------|---------|
| `audio_sink` | RtAudio (cross-platform) |
| `portaudio_sink` | PortAudio |
| `new_portaudio_sink` | PortAudio (rewrite) |
| `network_sink` | UDP/TCP streaming |
| `android_audio_sink` | AAudio (Android) |

### Misc Modules (9)

| Module | Purpose |
|--------|---------|
| `recorder` | Audio + baseband IQ recording to WAV |
| `frequency_manager` | Frequency/bookmark database |
| `scanner` | Automated frequency scanning |
| `rigctl_server` | Hamlib rigctld protocol (gpredict, etc.) |
| `rigctl_client` | Panadapter mode via rigctl |
| `iq_exporter` | Raw IQ sample export |
| `discord_integration` | Discord Rich Presence |
| `scheduler` | Timed recording scheduling |
| `demo_module` | Example/template module |

---

## Build System

### CMake Structure

```
CMakeLists.txt (top-level)
├── core/CMakeLists.txt          → builds sdrpp_core shared library
├── source_modules/*/CMakeLists.txt  → each includes sdrpp_module.cmake
├── decoder_modules/*/CMakeLists.txt
├── sink_modules/*/CMakeLists.txt
└── misc_modules/*/CMakeLists.txt
```

### Module Build Template (`sdrpp_module.cmake`)

Every module CMakeLists.txt includes this template:

```cmake
include(${SDRPP_MODULE_CMAKE})
# Result:
#   add_library(${PROJECT_NAME} SHARED ${SRC})
#   target_link_libraries(... sdrpp_core)
#   target_include_directories(... core/src/)
#   set_target_properties(... PREFIX "")  # no "lib" prefix
#   install(... lib/sdrpp/plugins)
```

### Compiler Flags

| Mode | Clang/GCC | MSVC |
|------|-----------|------|
| Debug | `-g -Og -std=c++17` | `/std:c++17 /EHsc` |
| Release | `-O3 -std=c++17` | `/O2 /Ob2 /std:c++17 /EHsc` |

Additional: `-Wall`, `-Wno-unused-command-line-argument`, `-Wpedantic` (when supported).

### Core Dependencies

| Library | Purpose | Required |
|---------|---------|----------|
| FFTW3 (float) | FFT for waterfall | Yes |
| GLFW3 | Window + input | Yes (desktop) |
| Volk | SIMD-optimized DSP | Yes |
| zstd | Compression | Yes |
| OpenGL | Rendering | Yes |
| libcorrect | Error correction | Yes (bundled or system) |

Module dependencies are module-specific (e.g., `librtlsdr` for `rtl_sdr_source`).

### Build Options (70+)

All modules are individually toggleable:

```cmake
option(OPT_BUILD_RTL_SDR_SOURCE "Build RTL-SDR Source" ON)
option(OPT_BUILD_RADIO "Build Radio Decoder" ON)
option(OPT_BUILD_AUDIO_SINK "Build Audio Sink" ON)
# ... etc
```

Backend selection:
```cmake
option(OPT_BACKEND_GLFW "Desktop backend" ON)
option(OPT_BACKEND_ANDROID "Android backend" OFF)
```

---

## Key Constants

| Constant | Value | Location | Purpose |
|----------|-------|----------|---------|
| `STREAM_BUFFER_SIZE` | 1,000,000 | `dsp/stream.h` | Samples per stream buffer |
| `WATERFALL_RESOLUTION` | 1,000,000 | `gui/widgets/waterfall.h` | FFT zoom scale factor |
| Default FFT size | 65,536 | `gui/main_window.h` | Spectrum analysis resolution |
| Default FFT rate | 20 fps | config | Waterfall refresh rate |
| FFT display range | -70 to 0 dB | `gui/main_window.h` | Spectrum Y-axis |
| Default bandwidth | 8 MHz | `gui/main_window.h` | Initial view bandwidth |
| Menu width | 300 px | `gui/main_window.h` | Left panel width |
| FFT height | 300 px | `gui/main_window.h` | Spectrum panel height |
| DC block corner | 50 Hz | `signal_path/iq_frontend.h` | DC blocker highpass frequency |
| VFO snap interval | 5,000 Hz | `gui/widgets/waterfall.h` | Default frequency snap step |
| Config auto-save | 1,000 ms | `config.cpp` | Save check interval |
| `SDRPP_API_VERSION` | 2.0.0 | `api_version.h` | Module API version (V2) |
