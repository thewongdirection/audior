# audior

A command-line audio recorder for Windows 10 and 11.

`audior` records the microphone — and optionally the system's audio output —
to a WAV or MP3 file. It is written in strict C against the Windows SDK and
uses **WASAPI** for audio capture and **Media Foundation** for MP3 encoding.
It has no third-party dependencies; only the Windows SDK and the C runtime are
required.

**Purpose:** this project demonstrates the use of WASAPI and other libraries
available by default in Windows for the purpose of audio stream interaction and
audio encoding.

It ships in **two behaviourally-identical editions**: this C reference build
(`audior.exe`) and a PowerShell port in [`audior_ps/`](audior_ps/) whose audio
engine is embedded C# compiled at runtime — no build step, same features.

---

## Capabilities at a glance

| Area | What it does |
|------|--------------|
| **Capture** | Microphone via WASAPI; optionally system audio via WASAPI loopback. |
| **Formats** | MP3 (default, 128 kbps via Media Foundation) or uncompressed WAV. Extension auto-corrected to match. |
| **Internal format** | Everything normalised to 48 kHz / 16-bit / stereo (any device rate/depth/layout handled via resampling). |
| **Two sources** | `--merge` mixes them into one file at a chosen split (mic % configurable, default 65/35, never clips); `--separate` writes two files; prompts if unspecified. |
| **Mic gain** | `--amplify 0–300` (0 = unity, 100 = 2×, 300 = 4×); applied to the mic only. |
| **Devices** | `--list-devices` (marks `[DEFAULT]`); select by ID, index, or name substring. |
| **Streaming to disk** | Capture → ring buffer → drain loop, so memory is bounded for any length; a live in-place progress line shows seconds/bytes written. |
| **Stopping** | Press `[Enter]`, **or** run a second instance with `--stop` (shared named event; works across windows and across the two editions). |
| **Dependencies** | None beyond the in-box Windows runtime. |

---

## How it works

Recording runs in the **foreground**: you start it, it captures audio while
streaming it to disk, and it stops when you press **Enter** — or when another
instance is run with `--stop`. Captured audio is buffered in memory and
continuously flushed to the output file by a separate writer loop, so memory
use stays bounded regardless of recording length. Each flush prints how much
audio has been written so far (seconds and bytes).

```
  WASAPI capture thread(s) ──▶ ring buffer(s) ──▶ writer loop ──▶ WAV / MP3 file
                                                       ▲
                            [Enter], or 'audior --stop', stops the session
```

---

## Features

- **Microphone recording** to WAV or MP3.
- **System audio capture** (WASAPI loopback) with `--include-output`.
- **Merge or separate** — when capturing both sources, mix them into one file
  (with a configurable microphone share via `--merge [percent]`, default 65/35)
  or write two independent files. If you don't specify, audior asks.
- **Device enumeration** — list every capture and playback device, with the
  system default for each marked `[DEFAULT]`.
- **Device selection** by ID, list index, or name substring.
- **Microphone amplification** — boost the microphone stream by 0–300 %
  (`--amplify`; 0 = unity, 100 = 2×, 300 = 4×); system audio is always captured
  at unity gain.
- **Two ways to stop** — press `Enter`, or run a second instance with `--stop`
  (signalled through a named Windows event).
- **Continuous streaming to disk** — recordings are not held entirely in
  memory; data is written as it is captured.
- **No external libraries** — Windows SDK and C runtime only.

---

## Quick start

```bat
:: Build (from a Developer Command Prompt for VS 2022)
build.bat

:: List available devices
audior --list-devices

:: Record the default microphone to an MP3 file (mp3 is the default format)
audior --output meeting.mp3

:: Record to WAV instead
audior --output meeting.wav --format wav

:: Record microphone + system audio, merged, with the mic boosted by 50%
audior --output session.mp3 --include-output --merge --amplify 50
```

Press **Enter** to stop any recording.

- Full command reference: [USAGE.md](USAGE.md)
- Build instructions and troubleshooting: [BUILDING.md](BUILDING.md)

---

## Audio format

All audio is normalised to a canonical internal format before it is written:

| Property    | Value                |
|-------------|----------------------|
| Sample rate | 48 000 Hz            |
| Channels    | 2 (stereo)           |
| Sample type | 16-bit signed PCM    |
| MP3 bitrate | 128 kbps             |
| Default     | MP3 (use `--format wav` for WAV) |

Device formats that differ (mono, other sample rates, float samples, etc.) are
converted on the fly, which also lets the microphone and loopback streams be
mixed reliably.

---

## Project structure

```
audior/
├── audior.c           Entry point: argument parsing, help, dispatch
├── recorder.c/.h      Session orchestration and the record/stop loop
├── capture.c/.h       WASAPI capture (microphone and loopback)
├── writer.c/.h        WAV and MP3 (Media Foundation) writers
├── device.c/.h        Device enumeration and selection
├── audio_convert.c/.h Format conversion + linear-interpolation resampling
├── ring_buffer.c/.h   Thread-safe circular buffer (capture → writer)
├── common.h           Shared types, constants, helper macros
├── guids.c            Explicit WASAPI GUID / property-key definitions
├── build.bat          MSVC build script
├── README.md          This file
├── USAGE.md           Command-line reference
└── BUILDING.md        Build instructions
```

---

## Requirements

| Requirement      | Detail                                            |
|------------------|---------------------------------------------------|
| Operating system | Windows 10 or Windows 11                          |
| Compiler         | MSVC (Visual Studio 2022, any edition)            |
| Windows SDK      | 10.0.19041.0 or later                             |

See [BUILDING.md](BUILDING.md) for details.

---

## Known limitations

- **Loopback captures the device mix.** If nothing is playing through the
  selected playback device, the system-audio stream is silence.
- **Resampling uses linear interpolation.** Adequate for speech and general
  recording; it is not a high-quality (band-limited) resampler.
- **Merging uses a mic/system weighting** (default 65 / 35, set with
  `--merge [percent]`). The two weights always sum to 1.0, so the merged signal
  stays within range at any split. Use `--separate` if you need to balance the
  two tracks independently afterwards.
- **Amplification can clip.** `--amplify` (0–300) is a linear gain applied
  before clamping; boosting an already-loud microphone signal will clip its
  peaks, and higher settings make this more likely.
- **MP3 output is fixed at 128 kbps stereo.** The bitrate is set in
  `writer.c` (`MP3_BYTES_PER_SEC`) at compile time.
- **One recording per process.** Each invocation records one session.
