# Building audior

## Requirements

| Tool          | Version                                          |
|---------------|--------------------------------------------------|
| Compiler      | MSVC `cl.exe` (Visual Studio 2022, any edition)  |
| Windows SDK   | 10.0.19041.0 or later                            |
| OS            | Windows 10 or Windows 11                          |

audior is compiled as **C** (the build uses `/TC`). MinGW, Clang-cl, and
cross-compilation are not supported.

---

## Build steps

1. Open a **Developer Command Prompt for VS 2022** (or any shell where
   `vcvars64.bat` has been run), so that `cl.exe` and the SDK headers/libraries
   are on the path.

2. Change to the project directory and run the build script:

   ```bat
   cd path\to\audior
   build.bat
   ```

3. On success you will see:

   ```
   Building audior...
   ...
   Build SUCCESS: audior.exe
   ```

   The executable `audior.exe` is produced in the project directory.

---

## What the build does

`build.bat` invokes the compiler directly:

```bat
cl.exe /nologo /W4 /TC /O2 ^
    audior.c recorder.c capture.c device.c writer.c audio_convert.c ring_buffer.c guids.c ^
    /link ^
    ole32.lib ^
    mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib ^
    /out:audior.exe
```

| Flag / library  | Purpose |
|-----------------|---------|
| `/W4`           | High warning level. |
| `/TC`           | Compile all sources as C. |
| `/O2`           | Optimise for speed. |
| `ole32.lib`     | COM (`CoCreateInstance`, `CoTaskMemFree`). |
| `mfplat.lib`    | Media Foundation platform (`MFStartup`, media types, samples). |
| `mfreadwrite.lib` | The MP3 sink writer (`MFCreateSinkWriterFromURL`). |
| `mfuuid.lib`    | Media Foundation GUIDs (`MFMediaType_Audio`, `MFAudioFormat_*`, `MF_MT_*`). |
| `mf.lib`        | Media Foundation runtime support. |

### About `guids.c`

The MMDevice/WASAPI headers declare the COM class and interface IDs
(`CLSID_MMDeviceEnumerator`, `IID_IMMDeviceEnumerator`, `IID_IAudioClient`,
`IID_IAudioCaptureClient`) and `PKEY_Device_FriendlyName`, but in a pure C
build no import library *defines* them. `guids.c` provides those definitions in
one translation unit. It must always be part of the build.

Media Foundation GUIDs are **not** in `guids.c` — they come from `mfuuid.lib`.

---

## Verifying the build

```bat
audior --help
audior --list-devices
```

A quick functional check (records ~2 seconds from the default mic to MP3, then
press Enter):

```bat
audior --output check.mp3
```

---

## Troubleshooting

### `'cl.exe' is not recognized`

You are not in a Developer Command Prompt. Launch **Developer Command Prompt
for VS 2022**, or run `vcvars64.bat` from your Visual Studio installation
first.

### `LNK2019: unresolved external symbol IID_IAudioClient` (or similar)

`guids.c` was left out of the build. Make sure it is listed among the source
files in `build.bat`.

### `LNK2019: unresolved external symbol MFCreate...`

A Media Foundation library is missing from the link line. Confirm
`mf.lib mfplat.lib mfreadwrite.lib mfuuid.lib` are all present, and that the
Windows SDK is installed.

### Device enumeration fails at runtime with `hr=0x800401F0`

`0x800401F0` is `CO_E_NOTINITIALIZED`. This indicates COM was not initialised
before a COM call. In normal use audior initialises COM itself; if you see this
after modifying the code, ensure `CoInitializeEx` runs before any device or
capture call on that thread.

### MP3 file fails to create

- Confirm the output path ends in `.mp3` (audior enforces this when the format
  is MP3).
- Confirm the destination directory exists and is writable.
- Media Foundation must be available — it is on all supported Windows 10/11
  editions, but may be absent on "N" editions without the Media Feature Pack.

### Recording produces a silent file

- **Microphone:** check the device is not muted and that microphone access is
  permitted under Settings → Privacy & Security → Microphone.
- **System audio (loopback):** audio must actually be playing through the
  selected playback device during the recording, otherwise silence is captured.
