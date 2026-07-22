#!/usr/bin/env python3
# Prep fixtures for the [realpileup] truthless benchmark (docs §52 real-data gate).
#
# Takes a wideband IQ WAV capture of a CW band and produces per-signal 8 kHz IQ
# fixtures the C++ harness (test_realpileup.cpp) decodes. The raw capture is not
# committed (large, user-specific); this script + the C++ harness are.
#
# Env:
#   CW_PILEUP_RAW   path to the wideband IQ WAV (int16 interleaved I/Q)
#   CW_PILEUP_DIR   output fixtures dir (default: ./pileup_fixtures)
#   CW_PILEUP_FS    capture sample rate Hz (default 2400000)
#   CW_PILEUP_CTR   capture centre RF Hz (default 6462577)
#
# Two-tier decimation: one heavy pass mixes the CW window to baseband and
# decimates 2.4 MHz -> 96 kHz; then each surveyed carrier is fine-shifted to a
# fixed pitch and decimated 96 kHz -> 8 kHz. Requires numpy only.
import numpy as np, struct, os, sys

RAW = os.environ.get("CW_PILEUP_RAW")
if not RAW:
    sys.exit("set CW_PILEUP_RAW to the wideband IQ WAV")
FS     = int(os.environ.get("CW_PILEUP_FS", "2400000"))
CENTER = int(os.environ.get("CW_PILEUP_CTR", "6462577"))
OUT    = os.environ.get("CW_PILEUP_DIR", os.path.join(os.path.dirname(__file__), "pileup_fixtures"))
os.makedirs(OUT, exist_ok=True)

# Carriers located by the survey (absolute RF Hz, dB above floor), strength order.
# Regenerate this list from a PSD sweep when the capture changes.
CARRIERS = [
    (7_005_200, 27.7), (7_025_500, 26.9), (7_023_500, 25.5), (7_016_000, 19.8),
    (7_014_000, 18.4), (7_030_000, 16.8), (7_012_000, 16.3), (7_032_200, 16.1),
    (7_029_000, 15.7), (7_038_500, 14.5), (7_019_000, 14.1), (7_031_000, 13.7),
    (7_025_800, 12.0), (7_028_000, 10.6),
]
PITCH = 600.0
FS1, FS2 = 96_000, 8_000
D1, D2 = FS // FS1, FS1 // FS2
REGION_CENTER = 7_020_000

raw = np.memmap(RAW, dtype=np.int16, mode="r", offset=44)
n_iq = raw.size // 2
print(f"IQ {n_iq:,} samples, tier1 /{D1} -> {FS1}Hz, tier2 /{D2} -> {FS2}Hz")

def design_lpf(cutoff, fs, ntaps):
    n = np.arange(ntaps) - (ntaps - 1) / 2
    h = np.sinc(2 * cutoff / fs * n) * np.hamming(ntaps)
    return (h / h.sum()).astype(np.complex64)

h1 = design_lpf(40_000, FS, 401)
shift1 = (REGION_CENTER - CENTER) / FS
CHUNK = D1 * 200_000
wide, tail, i = [], np.zeros(len(h1) - 1, dtype=np.complex64), 0
while i < n_iq:
    cnt = min(CHUNK, n_iq - i)
    a = np.asarray(raw[2*i:2*(i+cnt)], dtype=np.float32)
    c = (a[0::2] + 1j*a[1::2]).astype(np.complex64)
    c *= np.exp(-2j*np.pi*shift1*np.arange(i, i+cnt)).astype(np.complex64)
    c = np.concatenate([tail, c]); tail = c[-(len(h1)-1):].copy()
    y = np.convolve(c, h1, mode="valid")
    wide.append(y[(-i) % D1::D1]); i += cnt
wide = np.concatenate(wide).astype(np.complex64)
print(f"tier1: {wide.size:,} @ {FS1}Hz ({wide.size/FS1:.1f}s)")

h2 = design_lpf(3_400, FS1, 201)
def write_iq_wav(path, iq, fs):
    iq = iq / (np.max(np.abs(iq)) + 1e-9) * 0.9
    inter = np.empty(iq.size*2, dtype=np.int16)
    inter[0::2] = np.clip(iq.real*32767, -32768, 32767).astype(np.int16)
    inter[1::2] = np.clip(iq.imag*32767, -32768, 32767).astype(np.int16)
    data = inter.tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF"); f.write(struct.pack("<I", 36+len(data))); f.write(b"WAVE")
        f.write(b"fmt "); f.write(struct.pack("<IHHIIHH", 16, 1, 2, fs, fs*4, 4, 16))
        f.write(b"data"); f.write(struct.pack("<I", len(data))); f.write(data)

t2 = np.arange(wide.size)
with open(os.path.join(OUT, "manifest.txt"), "w") as mf:
    for k, (rf, snr) in enumerate(CARRIERS):
        shift2 = ((rf - REGION_CENTER) - PITCH) / FS1
        c = wide * np.exp(-2j*np.pi*shift2*t2).astype(np.complex64)
        c = np.convolve(c, h2, mode="valid")[::D2].astype(np.complex64)
        name = f"sig{k:02d}_{rf//1000}k_{snr:.0f}dB.wav"
        write_iq_wav(os.path.join(OUT, name), c, FS2)
        mf.write(f"{name}\t{rf}\t{snr:.1f}\t{PITCH:.0f}\t{c.size}\n")
        print(f"  {name}  {c.size/FS2:.1f}s")
print(f"wrote {len(CARRIERS)} fixtures + manifest to {OUT}")
