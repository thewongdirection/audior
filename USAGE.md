# audior — Command Reference

## Synopsis

```
audior [options]
```

Running `audior` with **no arguments** prints the built-in help.

A recording session runs in the foreground and continues until you press
**Enter**, or until another instance is run with `--stop`. The output file(s)
are finalised on stop.

---

## Options

| Option | Argument | Description |
|--------|----------|-------------|
| `-o`, `--output` | `<file>` | Output file path. **Required to record.** The extension is adjusted to match the format (e.g. `meeting` → `meeting.mp3`). |
| `-f`, `--format` | `wav` \| `mp3` | Output format. Default: `mp3`. |
| `-i`, `--include-output` | — | Also capture system audio output via WASAPI loopback. |
| `--merge` | `[percent]` | Mix mic + system into one file. Optional percent = mic's share (0–100); default 65. |
| `--separate` | — | Write microphone and system audio to two files. |
| `-m`, `--mic` | `<id\|index\|name>` | Microphone to record from. Default: system default capture device. |
| `-a`, `--amplify` | `<0-300>` | Amplify the microphone stream. Default: `0` (unity); `100`=2×, `300`=4×. |
| `-d`, `--output-device` | `<id\|index\|name>` | Playback device to capture for loopback. Default: system default render device. |
| `-l`, `--list-devices` | — | List all capture and playback devices, then exit. |
| `-s`, `--stop` | — | Stop a recording started by another instance (signals it to finalise and exit). |
| `-h`, `--help` | — | Show help and exit. |

---

## Commands in detail

### Recording (default action)

Any invocation with `--output` and without `--list-devices`/`--help` starts a
recording. The microphone is always captured. Add `--include-output` to also
capture system audio.

```
audior --output notes.wav
```

While recording, audior streams audio to disk in the background. Each time
buffered data is flushed it refreshes a single status line in place (it does
not scroll), reporting the running total of audio written:

```
Recording to notes.wav
Press [Enter] to stop, or run --stop from another window.
  written to disk: 1.74 s, 334076 bytes (last flush 48000 bytes)
```

Because audio is streamed to disk during the session, memory use stays bounded
no matter how long you record.

### Stopping a recording

A recording can be stopped in **two ways**:

1. **Press `Enter`** in the window where it is running.
2. **Run a second instance with `--stop`** from any other window:

   ```
   audior --stop
   ```

   This signals the active recording to finalise its file(s) and exit. If no
   recording is active, it prints `No active recording to stop.` and exits with
   code 1.

Both methods finalise the output identically. Stop signalling uses a named
Windows event (`Local\AudiorStopEvent`), so `--stop` works across windows and
even between the C and PowerShell editions.

### `--format <wav|mp3>`

- `mp3` — MPEG-1 Audio Layer III at 128 kbps, encoded by the built-in Media
  Foundation MP3 encoder. **The default.**
- `wav` — uncompressed PCM, 48 kHz / 16-bit / stereo.

The output extension is corrected automatically to match the format. For
example, `--output clip.wav` with the default MP3 format produces `clip.mp3`;
`--output clip --format wav` produces `clip.wav`.

### `--amplify <0-300>`

Applies a linear gain to the **microphone** stream only. System audio captured
via loopback is always recorded at unity gain.

| Value | Effect | Linear gain |
|-------|--------|-------------|
| `0`   | No amplification (default) | ×1.0 |
| `100` | +100 % louder (2×) | ×2.0 |
| `200` | +200 % louder (3×) | ×3.0 |
| `300` | +300 % louder (4×) | ×4.0 |

Values outside `0–300` are clamped (with a warning). Because the gain is applied
before the signal is clamped to 16-bit range, amplifying an already-loud source
can clip its peaks — increase gradually and check the result.

### `--include-output` and merge vs. separate

`--include-output` enables a second capture stream from a playback device
(loopback). You then choose how the two streams are stored:

- `--merge [percent]` — both streams are mixed into the single `--output` file.
  The optional `percent` (0–100) is the **microphone's** share of the mix; the
  system audio gets the remainder. It defaults to **65** (i.e. 65 % mic / 35 %
  system). The two weights always sum to 1.0, so the merged result stays within
  range without clipping at any split. Example: `--merge 80` → 80 % mic / 20 %
  system.
- `--separate` — two files are written, derived from `--output` by inserting
  a suffix before the extension:
  - `<name>_mic.<ext>` — microphone
  - `<name>_system.<ext>` — system audio

If you specify `--include-output` **without** `--merge` or `--separate`, audior
asks interactively:

```
System audio output will be included.
Save as (M)erged single file or (S)eparate files? [M/s]:
```

Pressing Enter (or `M`) merges; `S` keeps them separate.

### `--mic` and `--output-device` — device selection

Both accept the same three forms, tried in order:

1. **List index** — the number shown by `--list-devices` (e.g. `1`).
2. **Device ID** — the full ID string, or a substring of it.
3. **Friendly name** — a case-insensitive substring (e.g. `Realtek`).

If omitted, the system default device for that role is used.

### `--list-devices`

Prints all active capture and playback endpoints and exits. The system default
for each category is marked `[DEFAULT]`:

```
=== Audio Devices ===

Microphones / Capture Devices:
  [0] Microphone (USB Audio)
      ID: {0.0.1.00000000}.{a21f2501-...}
  [1] Microphone Array (Realtek(R) Audio)  [DEFAULT]
      ID: {0.0.1.00000000}.{aefb719d-...}

Speakers / Playback Devices:
  [0] Speakers (Realtek(R) Audio)  [DEFAULT]
      ID: {0.0.0.00000000}.{d5966fbc-...}
```

---

## Examples

```bat
:: List devices
audior --list-devices

:: Default microphone to MP3 (mp3 is the default format)
audior --output meeting.mp3

:: Default microphone to WAV
audior --output talk.wav --format wav

:: Microphone, boosted by 50%, to MP3
audior --output lecture.mp3 --amplify 50

:: Microphone + system audio merged into one MP3
audior --output session.mp3 --include-output --merge

:: Microphone + system audio as two separate WAV files
::   -> call_mic.wav and call_system.wav
audior --output call.wav --format wav --include-output --separate

:: Record from a specific microphone (by list index), doubled volume
audior --output input1.mp3 --mic 1 --amplify 100

:: Record from a microphone selected by name
audior --output realtek.mp3 --mic "Realtek"

:: Loopback from a specific playback device, merged
audior --output stream.mp3 -i --merge -d "Speakers"
```

---

## Exit codes

| Code | Meaning |
|------|---------|
| `0`  | Success (help shown, devices listed, or recording finalised). |
| `1`  | Error — invalid arguments, device could not be opened, or a capture/write failure. The message is printed to stderr. |

---

## Notes

- **System audio is silent if nothing is playing.** Loopback captures whatever
  the playback device is currently mixing.
- **Merged output can clip** when both sources are loud at once. Use
  `--separate` if you want to balance levels afterwards.
- **MP3 requires Media Foundation**, which is present on all supported Windows
  10/11 systems.
