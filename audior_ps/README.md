# audior.ps1 — PowerShell edition

A PowerShell port of the C `audior` recorder, with the **same functionality**.
It records the microphone (and optionally system audio output via WASAPI
loopback) to WAV or MP3.

There is nothing to compile: the audio engine (WASAPI capture + Media
Foundation MP3 encoding) is written in C# and compiled at run time by
`Add-Type`. The script depends only on Windows (10/11) and PowerShell — no
external modules and no build step.

---

## Quick start

```powershell
# List devices (system default marked [DEFAULT])
.\audior.ps1 --list-devices

# Record the default microphone to MP3 (mp3 is the default format)
.\audior.ps1 --output meeting.mp3

# Record to WAV
.\audior.ps1 --output talk.wav --format wav

# Microphone + system audio, merged (65/35 mic/system), with 50% mic boost
.\audior.ps1 --output session.mp3 --include-output --merge --amplify 50
```

Recording runs in the foreground. Stop it in either of two ways: press
**[Enter]** in its window, or run `.\audior.ps1 --stop` from another window.
While recording, a single status line updates in place showing how much audio
has been written to disk (seconds and bytes).

Because both editions signal through the same named event, `--stop` from either
the PowerShell or the C version will stop a recording started by the other.

If PowerShell's execution policy blocks the script, run it as:

```powershell
powershell -ExecutionPolicy Bypass -File .\audior.ps1 --output meeting.mp3
```

---

## Options

| Option | Argument | Description |
|--------|----------|-------------|
| `-o`, `--output` | `<file>` | Output file path. **Required to record.** Extension is adjusted to match the format. |
| `-f`, `--format` | `wav` \| `mp3` | Output format. Default: `mp3`. |
| `-i`, `--include-output` | — | Also capture system audio output (WASAPI loopback). |
| `--merge` | `[percent]` | Mix mic + system into one file. Optional percent = mic's share (0–100); default 65 (65/35). |
| `--separate` | — | Write microphone and system audio to two files (`_mic` / `_system`). |
| `-m`, `--mic` | `<id\|index\|name>` | Microphone to record from. Default: system default. |
| `-a`, `--amplify` | `<0-300>` | Amplify the microphone stream. 0 = unity (default), 100 = 2×, 300 = 4×. |
| `-d`, `--output-device` | `<id\|index\|name>` | Playback device to capture for loopback. Default: system default. |
| `-l`, `--list-devices` | — | List all capture and playback devices, then exit. |
| `-s`, `--stop` | — | Stop a recording started by another instance (signals it to finalise and exit). |
| `-h`, `--help` | — | Show help and exit. |

When `--include-output` is given without `--merge` or `--separate`, the script
asks interactively which to use. The CLI flags mirror the C `audior` tool
exactly.

---

## Audio format

All audio is normalised to **48 kHz / 16-bit / stereo** internally (the same
canonical format as the C version), so the microphone and loopback streams can
be mixed reliably. WAV is written at that format; MP3 is encoded at 128 kbps by
the built-in Media Foundation MP3 encoder.

---

## How it works

| Concern | Mechanism |
|---------|-----------|
| Capture | WASAPI (`IMMDeviceEnumerator`, `IAudioClient`, `IAudioCaptureClient`) |
| Loopback | `IAudioClient` initialised with `AUDCLNT_STREAMFLAGS_LOOPBACK` on a render device |
| MP3 encode | Media Foundation `IMFSinkWriter` (MP3 file sink) |
| WAV write | Direct RIFF/WAVE file writing |
| Format convert | Per-stream linear-interpolation resampler + channel fold |
| Buffering | Thread-safe ring buffer per stream; a drain loop flushes to disk continuously |
| Mixing | Fixed 65/35 (microphone/system) weighted sum with clamping |
| Stop | `[Enter]`, or a second instance's `--stop` via the named event `Local\AudiorStopEvent` |
| Stop | A background `Console.ReadLine()` thread signals a stop event on [Enter] |

The COM/Media Foundation work runs on a dedicated **MTA** thread, so it behaves
correctly regardless of the PowerShell host's apartment state.

---

## Known limitations

Same as the C edition:

- Loopback captures the device mix; if nothing is playing, system audio is silence.
- Resampling is linear interpolation (fine for general recording, not a
  band-limited resampler).
- Merge uses a mic/system weighting (default 65/35, set with `--merge [percent]`);
  weights always sum to 1.0 so it never clips. Use `--separate` for full
  post-mix control.
- Amplification (0–300) is a linear gain applied before clamping, so boosting a
  loud source can clip.
- MP3 output is fixed at 128 kbps stereo.

---

## Relationship to the C version

This is a behavioural twin of the C `audior` in the parent folder. The CLI,
defaults (MP3, no amplification, 48 kHz/16-bit/stereo), device selection,
merge/separate prompt, 65/35 mixing, and in-place progress reporting all match.
