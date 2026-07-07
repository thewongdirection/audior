<#
    audior.ps1 - Windows command-line audio recorder (PowerShell edition)

    A faithful PowerShell port of the C "audior" tool.  It records the
    microphone (and optionally system audio output via WASAPI loopback) to
    WAV or MP3, with the same features:

      * WAV or MP3 output (MP3 via the built-in Media Foundation encoder)
      * optional system-audio loopback capture
      * merge (fixed 65/35 mic/system weighting) or separate files
      * device enumeration with the system default marked [DEFAULT]
      * device selection by index / id / name substring
      * microphone amplification (0-100 %)
      * foreground recording, stopped with [Enter]
      * continuous streaming to disk with an in-place progress line

    Design - two layers, and why:

      * A thin PowerShell CLI layer (bottom of the file) handles argument
        parsing, help, the device listing, the merge/separate prompt, and the
        --stop signal.  Keeping this in PowerShell makes the user-facing
        behaviour easy to read and tweak.

      * An embedded C# "engine" (compiled at run time with Add-Type) does the
        real-time audio work.  PowerShell/.NET cannot call WASAPI or Media
        Foundation directly, but C# can via COM interop - so the engine is a
        near-line-for-line port of the C modules (ring buffer, resampler,
        writers, capture, recorder).  This keeps the "no external modules"
        promise: only the in-box Windows runtime and the C# compiler bundled
        with .NET are used.

    The two editions are behavioural twins by design; the C version in the
    parent folder is the reference implementation.

    Usage:  see  .\audior.ps1 --help
#>

# ===================================================================
#  Embedded C#: WASAPI capture + Media Foundation encoding engine
# ===================================================================

# The C# engine is compiled on demand (only the record and list-devices paths
# need it); --help and --stop stay fast by skipping compilation.
function Initialize-Engine {
if (-not ([System.Management.Automation.PSTypeName]'Audior.Engine').Type) {
$cs = @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace Audior
{
    // ---------------- enums / constants ----------------
    enum EDataFlow { eRender = 0, eCapture = 1, eAll = 2 }
    enum ERole { eConsole = 0, eMultimedia = 1, eCommunications = 2 }

    public class DeviceInfo
    {
        public int Index;
        public string Name;
        public string Id;
        public bool IsDefault;
    }

    // ---------------- COM structures ----------------
    [StructLayout(LayoutKind.Sequential)]
    struct PROPERTYKEY { public Guid fmtid; public int pid; }

    [StructLayout(LayoutKind.Sequential)]
    struct PROPVARIANT { public ushort vt; public ushort r1, r2, r3; public IntPtr p; public IntPtr p2; }

    // ---------------- MMDevice / WASAPI interfaces ----------------
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(EDataFlow dataFlow, int stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice device);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, int dwClsCtx, IntPtr pActivationParams,
                                   [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);
        [PreserveSig] int OpenPropertyStore(int stgmAccess, out IPropertyStore properties);
        [PreserveSig] int GetId(out IntPtr ppstrId);
        [PreserveSig] int GetState(out int pdwState);
    }

    [ComImport, Guid("886d8eeb-8cf2-4446-8d02-cdba1dbdcf99"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore
    {
        [PreserveSig] int GetCount(out uint cProps);
        [PreserveSig] int GetAt(uint iProp, out PROPERTYKEY pkey);
        [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
        [PreserveSig] int SetValue(ref PROPERTYKEY key, ref PROPVARIANT propvar);
        [PreserveSig] int Commit();
    }

    [ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioClient
    {
        [PreserveSig] int Initialize(int ShareMode, int StreamFlags, long hnsBufferDuration,
                                     long hnsPeriodicity, IntPtr pFormat, IntPtr AudioSessionGuid);
        [PreserveSig] int GetBufferSize(out uint pNumBufferFrames);
        [PreserveSig] int GetStreamLatency(out long phnsLatency);
        [PreserveSig] int GetCurrentPadding(out uint pNumPaddingFrames);
        [PreserveSig] int IsFormatSupported(int ShareMode, IntPtr pFormat, IntPtr ppClosestMatch);
        [PreserveSig] int GetMixFormat(out IntPtr ppDeviceFormat);
        [PreserveSig] int GetDevicePeriod(out long phnsDefaultDevicePeriod, out long phnsMinimumDevicePeriod);
        [PreserveSig] int Start();
        [PreserveSig] int Stop();
        [PreserveSig] int Reset();
        [PreserveSig] int SetEventHandle(IntPtr eventHandle);
        [PreserveSig] int GetService(ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
    }

    [ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioCaptureClient
    {
        [PreserveSig] int GetBuffer(out IntPtr ppData, out uint pNumFramesToRead,
                                    out uint pdwFlags, IntPtr pu64DevicePosition, IntPtr pu64QPCPosition);
        [PreserveSig] int ReleaseBuffer(uint NumFramesRead);
        [PreserveSig] int GetNextPacketSize(out uint pNumFramesInNextPacket);
    }

    // ---------------- Media Foundation interfaces ----------------
    // A COM interface's methods are dispatched by their POSITION in the vtable,
    // not by name.  So for these hand-written interop interfaces we must declare
    // every method that precedes one we actually call, in exact order, even if
    // we stub them (the aNN() placeholders).  Getting a count wrong silently
    // calls the wrong native method - e.g. an early version had SetGUID one slot
    // too late and was really calling SetString, which stored the MP3 subtype as
    // a string and made AddStream fail.  The // NN markers are the 1-based method
    // index within the interface (IUnknown's 3 methods are implicit).
    //
    // IMFMediaType derives from IMFAttributes (30 methods); we only need its
    // SetUINT32 (#19) and SetGUID (#22), so we declare up to #22 and stop.
    [ComImport, Guid("44ae0fa8-ea31-4109-8d2e-4cae4997c555"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaType
    {
        [PreserveSig] int a01(); [PreserveSig] int a02(); [PreserveSig] int a03();
        [PreserveSig] int a04(); [PreserveSig] int a05(); [PreserveSig] int a06();
        [PreserveSig] int a07(); [PreserveSig] int a08(); [PreserveSig] int a09();
        [PreserveSig] int a10(); [PreserveSig] int a11(); [PreserveSig] int a12();
        [PreserveSig] int a13(); [PreserveSig] int a14(); [PreserveSig] int a15();
        [PreserveSig] int a16(); [PreserveSig] int a17(); [PreserveSig] int a18();
        [PreserveSig] int SetUINT32(ref Guid key, uint value);               // 19
        [PreserveSig] int a20(); [PreserveSig] int a21();                    // 20 SetUINT64, 21 SetDouble
        [PreserveSig] int SetGUID(ref Guid key, ref Guid value);             // 22
    }

    [ComImport, Guid("3137f1cd-fe5e-4805-a5d8-fb477448cb3d"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFSinkWriter
    {
        [PreserveSig] int AddStream(IntPtr pTargetMediaType, out uint pdwStreamIndex);            // 1
        [PreserveSig] int SetInputMediaType(uint dwStreamIndex, IntPtr pInputMediaType, IntPtr pEncodingParameters); // 2
        [PreserveSig] int BeginWriting();                                                        // 3
        [PreserveSig] int WriteSample(uint dwStreamIndex, IntPtr pSample);                        // 4
        [PreserveSig] int b05(); [PreserveSig] int b06(); [PreserveSig] int b07(); [PreserveSig] int b08(); // 5-8
        [PreserveSig] int DoFinalize();                                                          // 9 (Finalize)
    }

    [ComImport, Guid("c40a00f2-b93a-4d80-ae8c-5a1c634f58e4"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFSample
    {
        [PreserveSig] int a01(); [PreserveSig] int a02(); [PreserveSig] int a03();
        [PreserveSig] int a04(); [PreserveSig] int a05(); [PreserveSig] int a06();
        [PreserveSig] int a07(); [PreserveSig] int a08(); [PreserveSig] int a09();
        [PreserveSig] int a10(); [PreserveSig] int a11(); [PreserveSig] int a12();
        [PreserveSig] int a13(); [PreserveSig] int a14(); [PreserveSig] int a15();
        [PreserveSig] int a16(); [PreserveSig] int a17(); [PreserveSig] int a18();
        [PreserveSig] int a19(); [PreserveSig] int a20(); [PreserveSig] int a21();
        [PreserveSig] int a22(); [PreserveSig] int a23(); [PreserveSig] int a24();
        [PreserveSig] int a25(); [PreserveSig] int a26(); [PreserveSig] int a27();
        [PreserveSig] int a28(); [PreserveSig] int a29(); [PreserveSig] int a30();
        [PreserveSig] int GetSampleFlags(out uint f);          // 31
        [PreserveSig] int SetSampleFlags(uint f);              // 32
        [PreserveSig] int GetSampleTime(out long t);           // 33
        [PreserveSig] int SetSampleTime(long t);               // 34
        [PreserveSig] int GetSampleDuration(out long d);       // 35
        [PreserveSig] int SetSampleDuration(long d);           // 36
        [PreserveSig] int GetBufferCount(out uint c);          // 37
        [PreserveSig] int GetBufferByIndex(uint i, out IMFMediaBuffer b); // 38
        [PreserveSig] int ConvertToContiguousBuffer(out IMFMediaBuffer b); // 39
        [PreserveSig] int AddBuffer(IntPtr b);                 // 40
    }

    [ComImport, Guid("045fa593-8799-42b8-bc8d-8968c6453507"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaBuffer
    {
        [PreserveSig] int Lock(out IntPtr ppbBuffer, out uint pcbMaxLength, out uint pcbCurrentLength); // 1
        [PreserveSig] int Unlock();                          // 2
        [PreserveSig] int GetCurrentLength(out uint pcb);    // 3
        [PreserveSig] int SetCurrentLength(uint cb);         // 4
    }

    // ---------------- native helpers ----------------
    static class Native
    {
        public const int    CLSCTX_ALL = 23;
        public const int    AUDCLNT_SHAREMODE_SHARED = 0;
        public const int    AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000;
        public const int    AUDCLNT_BUFFERFLAGS_SILENT = 0x2;
        public const int    DEVICE_STATE_ACTIVE = 0x1;
        public const int    STGM_READ = 0x0;
        public const uint   MF_VERSION = 0x00020070;
        public const uint   MFSTARTUP_LITE = 1;   // MFSTARTUP_NOSOCKET: skip the network layer we never use

        public static Guid IID_IAudioClient        = new Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");
        public static Guid IID_IAudioCaptureClient = new Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317");

        public static PROPERTYKEY PKEY_Device_FriendlyName = MakeKey(
            new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"), 14);

        // Media Foundation attribute keys
        public static Guid MF_MT_MAJOR_TYPE              = new Guid("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");
        public static Guid MF_MT_SUBTYPE                 = new Guid("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");
        public static Guid MF_MT_AUDIO_NUM_CHANNELS      = new Guid("37e48bf5-645e-4c5b-89de-ada9e29b696a");
        public static Guid MF_MT_AUDIO_SAMPLES_PER_SECOND= new Guid("5faeeae7-0290-4c31-9e8a-c534f68d9dba");
        public static Guid MF_MT_AUDIO_AVG_BYTES_PER_SECOND = new Guid("1aab75c8-cfef-451c-ab95-ac034b8e1731");
        public static Guid MF_MT_AUDIO_BLOCK_ALIGNMENT   = new Guid("322de230-9eeb-43bd-ab7a-ff412251541d");
        public static Guid MF_MT_AUDIO_BITS_PER_SAMPLE   = new Guid("f2deb57f-40fa-4764-aa33-ed4f2d1ff669");
        public static Guid MFMediaType_Audio             = new Guid("73647561-0000-0010-8000-00aa00389b71");
        public static Guid MFAudioFormat_PCM             = new Guid("00000001-0000-0010-8000-00aa00389b71");
        public static Guid MFAudioFormat_MP3             = new Guid("00000055-0000-0010-8000-00aa00389b71");

        static PROPERTYKEY MakeKey(Guid g, int pid) { PROPERTYKEY k; k.fmtid = g; k.pid = pid; return k; }

        [DllImport("ole32.dll")] public static extern int PropVariantClear(ref PROPVARIANT pvar);
        [DllImport("mfplat.dll")] public static extern int MFStartup(uint Version, uint dwFlags);
        [DllImport("mfplat.dll")] public static extern int MFShutdown();
        [DllImport("mfplat.dll")] public static extern int MFCreateMediaType(out IMFMediaType ppMFType);
        [DllImport("mfplat.dll")] public static extern int MFCreateSample(out IMFSample ppIMFSample);
        [DllImport("mfplat.dll")] public static extern int MFCreateMemoryBuffer(int cbMaxLength, out IMFMediaBuffer ppBuffer);
        [DllImport("mfreadwrite.dll", CharSet = CharSet.Unicode)]
        public static extern int MFCreateSinkWriterFromURL(string pwszOutputURL, IntPtr pByteStream,
                                                           IntPtr pAttributes, out IntPtr ppSinkWriter);

        public static IMMDeviceEnumerator CreateEnumerator()
        {
            Type t = Type.GetTypeFromCLSID(new Guid("BCDE0395-E52F-467C-8E3D-C4579291692E"));
            return (IMMDeviceEnumerator)Activator.CreateInstance(t);
        }
    }

    // ---------------- canonical format ----------------
    // Every stream is normalised to 48 kHz / 16-bit / stereo before buffering,
    // so the ring buffer, mixer and writers are all format-agnostic.  48 kHz is
    // the usual Windows mix rate (so often no resampling is needed) and both WAV
    // and the MP3 encoder accept it directly.  Mirrors CANON_* in the C common.h.
    static class Fmt
    {
        public const int Rate = 48000;
        public const int Channels = 2;
        public const int Bytes = 4;          // 16-bit stereo => 4 bytes/frame
        public const int ByteRate = Rate * Bytes;
    }

    // ---------------- thread-safe ring buffer ----------------
    // Decouples real-time capture from slow disk/encoder writes: the capture
    // thread writes here and returns immediately, a drain loop reads and does
    // the blocking I/O.  On overflow it drops the newest data rather than ever
    // stalling capture (see the C ring_buffer.c for the full rationale).
    class RingBuffer
    {
        byte[] data; int cap, head, tail, used; readonly object sync = new object();
        public RingBuffer(int capacity) { data = new byte[capacity]; cap = capacity; }
        public int Write(byte[] src, int count)
        {
            lock (sync)
            {
                int space = cap - used; int n = Math.Min(count, space);
                int first = Math.Min(n, cap - head);
                Array.Copy(src, 0, data, head, first);
                if (n > first) Array.Copy(src, first, data, 0, n - first);
                head = (head + n) % cap; used += n; return n;
            }
        }
        public int Read(byte[] dst, int count)
        {
            lock (sync)
            {
                int n = Math.Min(count, used);
                int first = Math.Min(n, cap - tail);
                Array.Copy(data, tail, dst, 0, first);
                if (n > first) Array.Copy(data, 0, dst, first, n - first);
                tail = (tail + n) % cap; used -= n; return n;
            }
        }
        public int Used() { lock (sync) { return used; } }
    }

    // ---------------- output writers ----------------
    interface IAudioFileWriter { void Write(byte[] buf, int count); void Close(); }

    class WavFileWriter : IAudioFileWriter
    {
        FileStream fs; uint dataBytes;
        public WavFileWriter(string path)
        {
            fs = new FileStream(path, FileMode.Create, FileAccess.Write);
            WriteHeader(0);
        }
        void WriteHeader(uint dataSize)
        {
            BinaryWriter bw = new BinaryWriter(fs, System.Text.Encoding.ASCII, true);
            fs.Seek(0, SeekOrigin.Begin);
            bw.Write(new char[] { 'R', 'I', 'F', 'F' });
            bw.Write((uint)(36 + dataSize));
            bw.Write(new char[] { 'W', 'A', 'V', 'E' });
            bw.Write(new char[] { 'f', 'm', 't', ' ' });
            bw.Write((uint)16);
            bw.Write((ushort)1);
            bw.Write((ushort)Fmt.Channels);
            bw.Write((uint)Fmt.Rate);
            bw.Write((uint)Fmt.ByteRate);
            bw.Write((ushort)Fmt.Bytes);
            bw.Write((ushort)16);
            bw.Write(new char[] { 'd', 'a', 't', 'a' });
            bw.Write((uint)dataSize);
            bw.Flush();
        }
        public void Write(byte[] buf, int count) { fs.Write(buf, 0, count); dataBytes += (uint)count; }
        public void Close()
        {
            if (fs == null) return;
            WriteHeader(dataBytes);
            fs.Flush(); fs.Close(); fs = null;
        }
    }

    class Mp3FileWriter : IAudioFileWriter
    {
        IMFSinkWriter sink; uint streamIndex; long timeHns;
        public Mp3FileWriter(string path)
        {
            path = Path.GetFullPath(path);
            IntPtr pSink;
            int hr = Native.MFCreateSinkWriterFromURL(path, IntPtr.Zero, IntPtr.Zero, out pSink);
            if (hr < 0) throw new Exception("MFCreateSinkWriterFromURL failed 0x" + hr.ToString("X8"));
            sink = (IMFSinkWriter)Marshal.GetObjectForIUnknown(pSink);
            Marshal.Release(pSink);

            IMFMediaType outType; Native.MFCreateMediaType(out outType);
            outType.SetGUID(ref Native.MF_MT_MAJOR_TYPE, ref Native.MFMediaType_Audio);
            outType.SetGUID(ref Native.MF_MT_SUBTYPE, ref Native.MFAudioFormat_MP3);
            outType.SetUINT32(ref Native.MF_MT_AUDIO_NUM_CHANNELS, (uint)Fmt.Channels);
            outType.SetUINT32(ref Native.MF_MT_AUDIO_SAMPLES_PER_SECOND, (uint)Fmt.Rate);
            outType.SetUINT32(ref Native.MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
            outType.SetUINT32(ref Native.MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);

            // The MF methods take their media-type/sample arguments as raw
            // IntPtr rather than the interface type; we hand over the COM
            // pointer explicitly (GetComInterfaceForObject + Release).  This
            // sidesteps the CLR's automatic interface marshalling, which for
            // these hand-declared interfaces was brittle.
            IntPtr pOut = Marshal.GetComInterfaceForObject(outType, typeof(IMFMediaType));
            hr = sink.AddStream(pOut, out streamIndex);
            Marshal.Release(pOut);
            if (hr < 0) throw new Exception("AddStream failed 0x" + hr.ToString("X8"));

            IMFMediaType inType; Native.MFCreateMediaType(out inType);
            inType.SetGUID(ref Native.MF_MT_MAJOR_TYPE, ref Native.MFMediaType_Audio);
            inType.SetGUID(ref Native.MF_MT_SUBTYPE, ref Native.MFAudioFormat_PCM);
            inType.SetUINT32(ref Native.MF_MT_AUDIO_NUM_CHANNELS, (uint)Fmt.Channels);
            inType.SetUINT32(ref Native.MF_MT_AUDIO_SAMPLES_PER_SECOND, (uint)Fmt.Rate);
            inType.SetUINT32(ref Native.MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            inType.SetUINT32(ref Native.MF_MT_AUDIO_BLOCK_ALIGNMENT, (uint)Fmt.Bytes);
            inType.SetUINT32(ref Native.MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (uint)Fmt.ByteRate);

            IntPtr pIn = Marshal.GetComInterfaceForObject(inType, typeof(IMFMediaType));
            hr = sink.SetInputMediaType(streamIndex, pIn, IntPtr.Zero);
            Marshal.Release(pIn);
            if (hr < 0) throw new Exception("SetInputMediaType failed 0x" + hr.ToString("X8"));

            hr = sink.BeginWriting();
            if (hr < 0) throw new Exception("BeginWriting failed 0x" + hr.ToString("X8"));

            Marshal.ReleaseComObject(outType);
            Marshal.ReleaseComObject(inType);
        }
        public void Write(byte[] buf, int count)
        {
            if (count <= 0) return;
            IMFMediaBuffer mb; Native.MFCreateMemoryBuffer(count, out mb);
            IntPtr p; uint max, cur;
            mb.Lock(out p, out max, out cur);
            Marshal.Copy(buf, 0, p, count);
            mb.Unlock();
            mb.SetCurrentLength((uint)count);

            IMFSample s; Native.MFCreateSample(out s);
            IntPtr pBuf = Marshal.GetComInterfaceForObject(mb, typeof(IMFMediaBuffer));
            s.AddBuffer(pBuf);
            Marshal.Release(pBuf);
            long frames = count / Fmt.Bytes;
            long dur = frames * 10000000L / Fmt.Rate;
            s.SetSampleTime(timeHns);
            s.SetSampleDuration(dur);
            timeHns += dur;
            IntPtr pSample = Marshal.GetComInterfaceForObject(s, typeof(IMFSample));
            sink.WriteSample(streamIndex, pSample);
            Marshal.Release(pSample);

            Marshal.ReleaseComObject(s);
            Marshal.ReleaseComObject(mb);
        }
        public void Close()
        {
            if (sink == null) return;
            sink.DoFinalize();
            Marshal.ReleaseComObject(sink);
            sink = null;
        }
    }

    // ---------------- WASAPI capture stream ----------------
    // See the C capture.h for the loopback silence-injection rationale: an idle
    // render endpoint delivers no packets, so a loopback stream pads itself with
    // silence up to the wall-clock frame count to stay aligned with the mic (and
    // to avoid an empty/invalid system file in --separate mode).
    class CaptureStream
    {
        IAudioClient client; IAudioCaptureClient capture; IntPtr mixFmt;
        int srcChannels, srcContainer, srcBlockAlign; bool srcIsFloat; uint srcRate;
        double step, pos; float lastL, lastR; float gain; bool loopback;
        RingBuffer ring; ManualResetEvent stop; Thread thread;
        byte[] copyBuf = new byte[0]; byte[] outBuf = new byte[0];
        System.Diagnostics.Stopwatch clock; long producedFrames;   // silence-injection bookkeeping

        public void Init(IMMDevice dev, bool loopback, float gain, ManualResetEvent stop, RingBuffer ring)
        {
            this.gain = gain; this.stop = stop; this.ring = ring; this.loopback = loopback;
            object o; Guid iac = Native.IID_IAudioClient;
            Check(dev.Activate(ref iac, Native.CLSCTX_ALL, IntPtr.Zero, out o), "Activate");
            client = (IAudioClient)o;
            Check(client.GetMixFormat(out mixFmt), "GetMixFormat");

            ushort tag = (ushort)Marshal.ReadInt16(mixFmt, 0);
            srcChannels = (ushort)Marshal.ReadInt16(mixFmt, 2);
            srcRate = (uint)Marshal.ReadInt32(mixFmt, 4);
            srcBlockAlign = (ushort)Marshal.ReadInt16(mixFmt, 12);
            ushort cb = (ushort)Marshal.ReadInt16(mixFmt, 16);
            if (srcChannels < 1) srcChannels = 1;
            srcContainer = srcBlockAlign / srcChannels;
            srcIsFloat = (tag == 3);
            if (tag == 0xFFFE && cb >= 22) { int sub = Marshal.ReadInt32(mixFmt, 24); srcIsFloat = (sub == 3); }

            int flags = loopback ? Native.AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
            Check(client.Initialize(Native.AUDCLNT_SHAREMODE_SHARED, flags, 2000000L, 0L, mixFmt, IntPtr.Zero), "Initialize");
            Guid icc = Native.IID_IAudioCaptureClient; object o2;
            Check(client.GetService(ref icc, out o2), "GetService");
            capture = (IAudioCaptureClient)o2;

            step = (double)srcRate / Fmt.Rate; pos = 0; lastL = 0; lastR = 0;
        }

        public void Start()
        {
            Check(client.Start(), "Start");
            // Baseline for loopback silence injection: frame 0 == now.
            clock = System.Diagnostics.Stopwatch.StartNew();
            producedFrames = 0;
            thread = new Thread(new ThreadStart(Run));
            thread.IsBackground = true;
            thread.Start();
        }

        public void StopAndJoin()
        {
            if (thread != null) { thread.Join(); thread = null; }
            if (client != null) client.Stop();
        }

        public void Free()
        {
            if (thread != null) { thread.Join(); thread = null; }
            if (capture != null) { Marshal.ReleaseComObject(capture); capture = null; }
            if (client != null) { Marshal.ReleaseComObject(client); client = null; }
            if (mixFmt != IntPtr.Zero) { Marshal.FreeCoTaskMem(mixFmt); mixFmt = IntPtr.Zero; }
        }

        // Timer-driven poll (wake every 10 ms, drain whatever is ready) rather
        // than WASAPI event callbacks: the same loop works for mic and loopback
        // (loopback doesn't reliably raise the buffer event), and 10 ms is far
        // below the 200 ms device buffer so nothing is dropped.  Draining AFTER
        // the stop check guarantees one final drain, so audio buffered at stop
        // time is captured.  Mirrors CaptureThread in the C capture.c.
        void Run()
        {
            for (;;)
            {
                bool stopped = stop.WaitOne(10);
                Drain();
                if (stopped) break;
            }
        }

        void Drain()
        {
            uint packet;
            bool gotData = false;
            while (capture.GetNextPacketSize(out packet) == 0 && packet > 0)
            {
                IntPtr data; uint frames, dwFlags;
                int hr = capture.GetBuffer(out data, out frames, out dwFlags, IntPtr.Zero, IntPtr.Zero);
                if (hr < 0) break;
                if (frames == 0) { capture.ReleaseBuffer(0); break; }
                gotData = true;
                bool silent = (dwFlags & Native.AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                Process(data, (int)frames, silent);
                capture.ReleaseBuffer(frames);
            }
            // Idle loopback endpoint delivers nothing - fill the gap with silence.
            if (loopback && !gotData) InjectSilence();
        }

        // Pad the loopback stream with silence up to the wall-clock frame count,
        // keeping it continuous and aligned with the mic while nothing plays.
        void InjectSilence()
        {
            if (clock == null) return;
            long expected = (long)(clock.Elapsed.TotalSeconds * Fmt.Rate);
            if (producedFrames >= expected) return;
            long deficit = expected - producedFrames;
            int cap = outBuf.Length / Fmt.Bytes;
            if (cap <= 0) { outBuf = new byte[Fmt.Rate * Fmt.Bytes / 10]; cap = outBuf.Length / Fmt.Bytes; }
            Array.Clear(outBuf, 0, outBuf.Length);
            while (deficit > 0)
            {
                int chunk = (int)Math.Min(deficit, cap);
                ring.Write(outBuf, chunk * Fmt.Bytes);
                producedFrames += chunk;
                deficit        -= chunk;
            }
        }

        // Convert + resample one capture packet into canonical PCM and push it
        // into the ring.  `pos` is a fractional position in SOURCE frames within
        // this packet; index -1 is the previous packet's last frame (the carry,
        // in lastL/lastR).  We interpolate while both floor(pos) and floor(pos)+1
        // exist (pos < frames-1), then remember the last frame and rebase pos by
        // -frames so the fractional offset flows into the next packet seamlessly.
        // This is the C AudioConvert_Process logic; see audio_convert.c.
        void Process(IntPtr data, int frames, bool silent)
        {
            int srcBytes = frames * srcBlockAlign;
            if (copyBuf.Length < srcBytes) copyBuf = new byte[srcBytes];
            if (!silent && data != IntPtr.Zero) Marshal.Copy(data, copyBuf, 0, srcBytes);

            // Size the output scratch for the worst (upsampling) case so the
            // loop below can never overrun it.
            int maxOut = (int)(((double)frames + 1.0) / step) + 2;
            int needBytes = maxOut * Fmt.Bytes;
            if (outBuf.Length < needBytes) outBuf = new byte[needBytes];

            int outCount = 0;
            double limit = (double)frames - 1.0;
            while (pos < limit && outCount < maxOut)
            {
                int i0 = (int)Math.Floor(pos);
                double frac = pos - i0;
                float l0, r0, l1, r1;
                Fetch(i0, silent, out l0, out r0);
                Fetch(i0 + 1, silent, out l1, out r1);
                // Gain applied in the float domain, before the int16 clamp.
                WriteOut(outBuf, outCount * 4, (float)(l0 + frac * (l1 - l0)) * gain);
                WriteOut(outBuf, outCount * 4 + 2, (float)(r0 + frac * (r1 - r0)) * gain);
                outCount++;
                pos += step;
            }
            float cl, cr; Fetch(frames - 1, silent, out cl, out cr);
            lastL = cl; lastR = cr;   // carry for the next packet
            pos -= frames;            // rebase position into next-packet coords

            if (outCount > 0) { ring.Write(outBuf, outCount * 4); producedFrames += outCount; }
        }

        void Fetch(int idx, bool silent, out float l, out float r)
        {
            if (silent) { l = 0; r = 0; return; }
            if (idx < 0) { l = lastL; r = lastR; return; }
            l = Decode(idx, 0);
            r = (srcChannels >= 2) ? Decode(idx, 1) : l;
        }

        float Decode(int frameIdx, int ch)
        {
            int off = frameIdx * srcBlockAlign + ch * srcContainer;
            if (srcIsFloat) return BitConverter.ToSingle(copyBuf, off);
            switch (srcContainer)
            {
                case 1: return ((float)copyBuf[off] - 128f) / 128f;
                case 2: return (float)BitConverter.ToInt16(copyBuf, off) / 32768f;
                case 3:
                    {
                        int v = copyBuf[off] | (copyBuf[off + 1] << 8) | (copyBuf[off + 2] << 16);
                        if ((v & 0x800000) != 0) v |= unchecked((int)0xFF000000);
                        return (float)v / 8388608f;
                    }
                case 4: return (float)BitConverter.ToInt32(copyBuf, off) / 2147483648f;
                default: return 0f;
            }
        }

        static void WriteOut(byte[] buf, int off, float val)
        {
            float s = val * 32767f;
            if (s > 32767f) s = 32767f;
            if (s < -32768f) s = -32768f;
            short v = (short)(s >= 0f ? s + 0.5f : s - 0.5f);
            buf[off] = (byte)(v & 0xFF);
            buf[off + 1] = (byte)((v >> 8) & 0xFF);
        }

        static void Check(int hr, string what)
        {
            if (hr < 0) throw new Exception(what + " failed 0x" + hr.ToString("X8"));
        }
    }

    // ---------------- engine ----------------
    public static class Engine
    {
        const int RingCapacity = Fmt.ByteRate * 8;
        const int PumpChunk = 32768;

        // ---- device enumeration ----
        public static DeviceInfo[] ListDevices(int flow)
        {
            IMMDeviceEnumerator en = Native.CreateEnumerator();
            string defId = GetDefaultId(en, flow);
            IMMDeviceCollection coll;
            en.EnumAudioEndpoints((EDataFlow)flow, Native.DEVICE_STATE_ACTIVE, out coll);
            uint count; coll.GetCount(out count);
            List<DeviceInfo> list = new List<DeviceInfo>();
            for (uint i = 0; i < count; i++)
            {
                IMMDevice d; coll.Item(i, out d);
                string id = GetId(d); string name = GetName(d);
                DeviceInfo di = new DeviceInfo();
                di.Index = (int)i; di.Name = name; di.Id = id;
                di.IsDefault = (defId != null && id == defId);
                list.Add(di);
                Marshal.ReleaseComObject(d);
            }
            Marshal.ReleaseComObject(coll);
            Marshal.ReleaseComObject(en);
            return list.ToArray();
        }

        static string GetDefaultId(IMMDeviceEnumerator en, int flow)
        {
            IMMDevice d;
            if (en.GetDefaultAudioEndpoint((EDataFlow)flow, ERole.eConsole, out d) == 0)
            {
                string id = GetId(d); Marshal.ReleaseComObject(d); return id;
            }
            return null;
        }

        static string GetId(IMMDevice d)
        {
            IntPtr p; if (d.GetId(out p) != 0 || p == IntPtr.Zero) return null;
            string s = Marshal.PtrToStringUni(p);
            Marshal.FreeCoTaskMem(p);
            return s;
        }

        static string GetName(IMMDevice d)
        {
            IPropertyStore store;
            if (d.OpenPropertyStore(Native.STGM_READ, out store) != 0) return "(unknown)";
            PROPVARIANT pv;
            string name = "(unknown)";
            if (store.GetValue(ref Native.PKEY_Device_FriendlyName, out pv) == 0 && pv.p != IntPtr.Zero)
                name = Marshal.PtrToStringUni(pv.p);
            Native.PropVariantClear(ref pv);
            Marshal.ReleaseComObject(store);
            return name;
        }

        static IMMDevice GetDevice(int flow, string selector)
        {
            IMMDeviceEnumerator en = Native.CreateEnumerator();
            IMMDevice result = null;
            if (string.IsNullOrEmpty(selector))
            {
                en.GetDefaultAudioEndpoint((EDataFlow)flow, ERole.eConsole, out result);
            }
            else
            {
                bool byIndex = IsAllDigits(selector);
                int want = byIndex ? int.Parse(selector) : -1;
                IMMDeviceCollection coll;
                en.EnumAudioEndpoints((EDataFlow)flow, Native.DEVICE_STATE_ACTIVE, out coll);
                uint count; coll.GetCount(out count);
                for (uint i = 0; i < count; i++)
                {
                    IMMDevice d; coll.Item(i, out d);
                    bool match;
                    if (byIndex) match = ((int)i == want);
                    else
                    {
                        string id = GetId(d); string name = GetName(d);
                        // ID matched case-sensitively (like wcsstr); name case-insensitively.
                        match = (id != null && id.IndexOf(selector, StringComparison.Ordinal) >= 0)
                             || (name != null && name.IndexOf(selector, StringComparison.OrdinalIgnoreCase) >= 0);
                    }
                    if (match) { result = d; break; }
                    Marshal.ReleaseComObject(d);
                }
                Marshal.ReleaseComObject(coll);
            }
            Marshal.ReleaseComObject(en);
            return result;
        }

        static bool IsAllDigits(string s)
        {
            if (string.IsNullOrEmpty(s)) return false;
            foreach (char c in s) if (c < '0' || c > '9') return false;
            return true;
        }

        static string SuffixPath(string path, string suffix)
        {
            int dot = path.LastIndexOf('.');
            int slash = Math.Max(path.LastIndexOf('\\'), path.LastIndexOf('/'));
            if (dot > slash && dot >= 0) return path.Substring(0, dot) + suffix + path.Substring(dot);
            return path + suffix;
        }

        // ---- mixing / pumping ----
        // Weighted mix of mic (a) + system (b) in place into `a`.  micW + loopW
        // always equals 1.0 (set by the caller from the configured mic percent),
        // so a full-scale sum can't overflow => merged output never clips,
        // whatever split the user chose.  Samples are little-endian int16 packed
        // in the byte buffers.  Mirrors MixInto in the C recorder.c.
        static void MixInto(byte[] a, byte[] b, int n, float micW, float loopW)
        {
            for (int i = 0; i < n; i += 2)
            {
                short ma = (short)(a[i] | (a[i + 1] << 8));
                short lb = (short)(b[i] | (b[i + 1] << 8));
                float mixed = micW * ma + loopW * lb;
                if (mixed > 32767f) mixed = 32767f;
                if (mixed < -32768f) mixed = -32768f;
                short v = (short)(mixed >= 0f ? mixed + 0.5f : mixed - 0.5f);
                a[i] = (byte)(v & 0xFF);
                a[i + 1] = (byte)((v >> 8) & 0xFF);
            }
        }

        static long PumpStream(RingBuffer ring, IAudioFileWriter w, byte[] tmp)
        {
            long total = 0; int n;
            while ((n = ring.Read(tmp, tmp.Length)) > 0) { w.Write(tmp, n); total += n; }
            return total;
        }

        static long PumpMerged(RingBuffer micRing, RingBuffer loopRing, IAudioFileWriter w,
                               byte[] a, byte[] b, float micW, float loopW)
        {
            long total = 0;
            for (;;)
            {
                // Only mix the portion both streams have produced, so they stay
                // frame-aligned; the surplus waits in its ring.
                int avail = Math.Min(micRing.Used(), loopRing.Used());
                avail -= avail % Fmt.Bytes;
                if (avail <= 0) break;
                int n = Math.Min(avail, a.Length); n -= n % Fmt.Bytes;
                micRing.Read(a, n); loopRing.Read(b, n);
                MixInto(a, b, n, micW, loopW);
                w.Write(a, n); total += n;
            }
            return total;
        }

        // Accumulate flush totals and refresh the in-place progress line.
        // `disk` is every byte written this flush (both files in separate mode);
        // `timeline` is only the mic stream's bytes = the recording's duration.
        // Seconds come from timeline, bytes from disk (reporting disk-bytes as
        // seconds would double-count in separate mode).  The leading '\r' and
        // trailing spaces rewrite the same console line.  Mirrors C ReportFlush.
        static void ReportFlush(long disk, long timeline, ref long totalDisk, ref long totalTimeline)
        {
            if (disk <= 0) return;
            totalDisk += disk; totalTimeline += timeline;
            double secs = (double)totalTimeline / (double)Fmt.ByteRate;
            Console.Write("\r  written to disk: " + secs.ToString("F2") + " s, " +
                          totalDisk.ToString() + " bytes (last flush " + disk.ToString() + " bytes)        ");
            Console.Out.Flush();
        }

        static IAudioFileWriter OpenWriter(string path, int format)
        {
            if (format == 1) return new Mp3FileWriter(path);
            return new WavFileWriter(path);
        }

        // ---- recording session ----
        // Media Foundation and WASAPI prefer the MTA, but a PowerShell host is
        // usually STA.  Run the whole session on a dedicated MTA thread so the
        // tool works regardless of how it was launched.
        public static int Record(string outputPath, int format, bool includeOutput,
                                 int mixMode, string micSel, string renderSel,
                                 int amplifyPercent, int mergeMicPercent)
        {
            int rc = 1;
            Thread worker = new Thread(delegate ()
            {
                try
                {
                    rc = RecordImpl(outputPath, format, includeOutput, mixMode,
                                    micSel, renderSel, amplifyPercent, mergeMicPercent);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("Recording failed: " + ex.Message);
                    rc = 1;
                }
            });
            worker.SetApartmentState(ApartmentState.MTA);
            worker.Start();
            worker.Join();
            return rc;
        }

        static int RecordImpl(string outputPath, int format, bool includeOutput,
                              int mixMode, string micSel, string renderSel,
                              int amplifyPercent, int mergeMicPercent)
        {
            bool mfReady = false;
            IMMDevice micDev = null, loopDev = null;
            CaptureStream micCap = null, loopCap = null;
            IAudioFileWriter wA = null, wB = null;
            EventWaitHandle namedStop = null;

            try
            {
                // Media Foundation is only needed for MP3, so start it lazily.
                if (format == 1)
                {
                    int hr = Native.MFStartup(Native.MF_VERSION, Native.MFSTARTUP_LITE);
                    if (hr < 0) { Console.Error.WriteLine("MFStartup failed (0x" + hr.ToString("X8") + ")"); return 1; }
                    mfReady = true;
                }

                micDev = GetDevice(1, micSel);
                if (micDev == null) { Console.Error.WriteLine("Could not open microphone device."); return 1; }
                if (includeOutput)
                {
                    loopDev = GetDevice(0, renderSel);
                    if (loopDev == null) { Console.Error.WriteLine("Could not open playback device for loopback."); return 1; }
                }

                // Prompt for merge vs separate when loopback is on and no choice was
                // given (same text and default as the C version's PromptMixMode).
                if (includeOutput && mixMode == -1)
                {
                    Console.Write("\nSystem audio output will be included.\n");
                    Console.Write("Save as (M)erged single file or (S)eparate files? [M/s]: ");
                    string line = Console.In.ReadLine();
                    mixMode = (line != null && line.Length > 0 && (line[0] == 's' || line[0] == 'S')) ? 1 : 0;
                }

                RingBuffer micRing = new RingBuffer(RingCapacity);
                RingBuffer loopRing = includeOutput ? new RingBuffer(RingCapacity) : null;

                if (includeOutput && mixMode == 1)
                {
                    wA = OpenWriter(SuffixPath(outputPath, "_mic"), format);
                    wB = OpenWriter(SuffixPath(outputPath, "_system"), format);
                }
                else
                {
                    wA = OpenWriter(outputPath, format);
                }

                ManualResetEvent stop = new ManualResetEvent(false);

                // Named event lets a second 'audior.ps1 --stop' instance end this session.
                bool createdNew;
                namedStop = new EventWaitHandle(
                    false, EventResetMode.ManualReset, "Local\\AudiorStopEvent", out createdNew);
                namedStop.Reset();   // clear any stale signal from a prior --stop

                float micGain = 1.0f + (float)amplifyPercent / 100.0f;

                micCap = new CaptureStream();
                micCap.Init(micDev, false, micGain, stop, micRing);
                if (includeOutput)
                {
                    loopCap = new CaptureStream();
                    loopCap.Init(loopDev, true, 1.0f, stop, loopRing);
                }

                micCap.Start();
                if (loopCap != null) loopCap.Start();

                Thread enter = new Thread(delegate () { try { Console.In.ReadLine(); } catch { } stop.Set(); });
                enter.IsBackground = true;
                enter.Start();

                // Watch the named stop event (set by 'audior.ps1 --stop') too.
                Thread stopWatcher = new Thread(delegate () {
                    WaitHandle.WaitAny(new WaitHandle[] { namedStop, stop });
                    stop.Set();
                });
                stopWatcher.IsBackground = true;
                stopWatcher.Start();

                Console.Write("\nRecording to " + outputPath);
                if (includeOutput)
                    Console.Write(mixMode == 1 ? " (separate system audio)"
                                              : " (merged, mic " + mergeMicPercent + "%)");
                Console.Write("\nPress [Enter] to stop, or run --stop from another window.\n");

                // Merge weights derived once from the configured mic percentage;
                // loopW is the complement so they always sum to 1.0 (no clipping).
                float micW = (float)mergeMicPercent / 100.0f;
                float loopW = 1.0f - micW;

                long totalDisk = 0, totalTimeline = 0;
                byte[] tmp = new byte[PumpChunk];
                byte[] mixA = new byte[PumpChunk];
                byte[] mixB = new byte[PumpChunk];

                // Drain loop: wake every 250 ms (or instantly on stop) and flush
                // whatever the capture threads have buffered.  The multi-second
                // rings make this relaxed cadence safe while keeping progress
                // updates infrequent.  Mirrors the C recorder drain loop.
                for (;;)
                {
                    bool stopped = stop.WaitOne(250);
                    long disk, timeline;
                    if (includeOutput && mixMode == 0)
                    {
                        long m = PumpMerged(micRing, loopRing, wA, mixA, mixB, micW, loopW);
                        disk = m; timeline = m;
                    }
                    else
                    {
                        long mic = PumpStream(micRing, wA, tmp);
                        long sys = includeOutput ? PumpStream(loopRing, wB, tmp) : 0;
                        disk = mic + sys; timeline = mic;
                    }
                    ReportFlush(disk, timeline, ref totalDisk, ref totalTimeline);
                    if (stopped) break;
                }

                micCap.StopAndJoin();
                if (loopCap != null) loopCap.StopAndJoin();

                if (includeOutput && mixMode == 0)
                {
                    long m = PumpMerged(micRing, loopRing, wA, mixA, mixB, micW, loopW);
                    long tail = PumpStream(micRing, wA, tmp) + PumpStream(loopRing, wA, tmp);
                    ReportFlush(m + tail, m + tail, ref totalDisk, ref totalTimeline);
                }
                else
                {
                    long mic = PumpStream(micRing, wA, tmp);
                    long sys = includeOutput ? PumpStream(loopRing, wB, tmp) : 0;
                    ReportFlush(mic + sys, mic, ref totalDisk, ref totalTimeline);
                }

                if (totalDisk > 0) Console.Write("\n");
                Console.Write("Recording stopped. Finalising file(s)...\n");

                wA.Close(); wA = null;
                if (wB != null) { wB.Close(); wB = null; }
                Console.Write("Done.\n");
                return 0;
            }
            finally
            {
                if (micCap != null) micCap.Free();
                if (loopCap != null) loopCap.Free();
                if (wA != null) wA.Close();
                if (wB != null) wB.Close();
                if (namedStop != null) namedStop.Close();
                if (micDev != null) Marshal.ReleaseComObject(micDev);
                if (loopDev != null) Marshal.ReleaseComObject(loopDev);
                if (mfReady) Native.MFShutdown();
            }
        }
    }
}
'@
Add-Type -TypeDefinition $cs -Language CSharp | Out-Null
}
}

# ===================================================================
#  PowerShell CLI layer
# ===================================================================

function Show-Usage {
@'

AUDIOR  -  Windows Command-Line Audio Recorder
Records the microphone (and optionally system audio) to WAV or MP3.
Built on WASAPI + Media Foundation.  Windows 10/11.  No external libraries.

USAGE
  audior.ps1 [options]

RECORDING
  Recording runs in the foreground and continues until you press [Enter],
  or until another instance is run with --stop.
  Audio is streamed to disk continuously while recording.

OPTIONS
  -o, --output <file>        Output file path. Required to record.
                             The extension is adjusted to match the format
                             (e.g. 'meeting' becomes 'meeting.mp3').

  -f, --format <wav|mp3>     Output format. Default: mp3.
                             mp3 - MPEG Layer-3, 128 kbps (Media Foundation)
                             wav - uncompressed PCM, 48 kHz/16-bit/stereo

  -a, --amplify <0-300>      Amplify the microphone stream. Default: 0.
                             0   = no amplification (unity gain)
                             100 = +100% (2x), 300 = +300% (4x)
                             Applies to the microphone only; system audio
                             loopback is always captured at unity gain.

  -i, --include-output       Also capture system audio output (loopback).
                             By default only the microphone is recorded.
                             You will be asked whether to merge the two
                             streams or keep separate files (unless --merge
                             or --separate is given).

      --merge [percent]      Mix microphone + system audio into one file.
                             Optional percent (0-100) is the microphone's
                             share of the mix; the system gets the rest.
                             Default: 65 (i.e. 65% mic / 35% system).
      --separate             Write microphone and system audio to two files
                             ('<name>_mic' and '<name>_system').

  -m, --mic <id|index|name>  Microphone to record from. Default: system
                             default capture device. Accepts a device ID,
                             a list index, or part of the device name.

  -d, --output-device <id|index|name>
                             Playback device to capture for loopback.
                             Default: system default render device.

  -l, --list-devices         List all capture and playback devices, then
                             exit. The system default is marked [DEFAULT].

  -s, --stop                 Stop a recording started by another instance
                             (signals it to finalise its file and exit).

  -h, --help                 Show this help and exit.

EXAMPLES
  audior.ps1 --list-devices
  audior.ps1 --output meeting.mp3
  audior.ps1 --output talk.wav --format wav
  audior.ps1 --output session.mp3 --include-output --merge
  audior.ps1 --output lecture.mp3 --amplify 50
  audior.ps1 --output podcast.mp3 -i --merge 70   (70% mic / 30% system)
  audior.ps1 -o call.wav -f wav -i --separate --mic 1
  audior.ps1 --stop                 (from another window, ends the recording)

'@ | Write-Host
}

function Show-Collection([string]$title, [int]$flow) {
    Write-Host ""
    Write-Host "${title}:"
    $devs = @([Audior.Engine]::ListDevices($flow))
    if ($devs.Count -eq 0) { Write-Host "  (none)" }
    foreach ($d in $devs) {
        $mark = ''; if ($d.IsDefault) { $mark = '  [DEFAULT]' }
        $id = if ($d.Id) { $d.Id } else { '(unknown)' }
        Write-Host ("  [{0}] {1}{2}" -f $d.Index, $d.Name, $mark)
        Write-Host ("      ID: {0}" -f $id)
    }
}

function Show-Devices {
    Write-Host "=== Audio Devices ==="
    Show-Collection "Microphones / Capture Devices" 1
    Show-Collection "Speakers / Playback Devices" 0
    Write-Host ""
}

function Set-Extension([string]$path, [int]$format) {
    $ext = if ($format -eq 1) { '.mp3' } else { '.wav' }
    $dir = [System.IO.Path]::GetDirectoryName($path)
    $name = [System.IO.Path]::GetFileName($path)
    $dot = $name.LastIndexOf('.')
    if ($dot -ge 0) { $name = $name.Substring(0, $dot) }
    $name = $name + $ext
    if ([string]::IsNullOrEmpty($dir)) { return $name }
    return [System.IO.Path]::Combine($dir, $name)
}

# Write a plain line to stderr (matches the C tool's fprintf(stderr, ...)).
function Write-Err([string]$msg) { [Console]::Error.WriteLine($msg) }

# Parse a leading integer the way C's atoi() does (non-numeric -> 0).
function ConvertTo-Atoi([string]$s) {
    if ($s -match '^\s*([+-]?\d+)') { return [int]$Matches[1] }
    return 0
}

# ---- parse arguments (mirrors the C CLI exactly) ----
$cmd        = 'record'
$outputPath = ''
$format     = 1            # default mp3
$includeOut = $false
$mixMode    = -1           # -1 = prompt, 0 = merge, 1 = separate
$micSel     = ''
$renderSel  = ''
$amplify    = 0
$mergeMic   = 65           # microphone's % share of a merged mix (default 65/35)

if ($args.Count -eq 0) { Show-Usage; exit 0 }

for ($i = 0; $i -lt $args.Count; $i++) {
    $a = [string]$args[$i]
    $hasNext = ($i + 1) -lt $args.Count

    if ($a -ceq '-h' -or $a -ceq '--help') {
        Show-Usage; exit 0                                   # -h short-circuits (as in C)
    } elseif ($a -ceq '-l' -or $a -ceq '--list-devices') {
        $cmd = 'list'
    } elseif ($a -ceq '-s' -or $a -ceq '--stop') {
        $cmd = 'stop'
    } elseif ($a -ceq '-i' -or $a -ceq '--include-output') {
        $includeOut = $true
    } elseif ($a -ceq '--merge') {
        $mixMode = 0
        # Optional next token: the mic's % of the mix.  Only consumed when it is
        # all digits, so a following flag/end leaves the default (65) in place.
        if ($hasNext -and ([string]$args[$i + 1]) -match '^\d+$') {
            $i++; $v = [int]([string]$args[$i])
            if ($v -lt 0)   { Write-Err 'Warning: --merge percent clamped to 0.';   $v = 0 }
            if ($v -gt 100) { Write-Err 'Warning: --merge percent clamped to 100.'; $v = 100 }
            $mergeMic = $v
        }
    } elseif ($a -ceq '--separate') {
        $mixMode = 1
    } elseif (($a -ceq '-o' -or $a -ceq '--output') -and $hasNext) {
        $i++; $outputPath = [string]$args[$i]
    } elseif (($a -ceq '-f' -or $a -ceq '--format') -and $hasNext) {
        $i++; $v = [string]$args[$i]
        if ($v -ieq 'wav') { $format = 0 }
        elseif ($v -ieq 'mp3') { $format = 1 }
        else { Write-Err "Error: unknown format '$v' (use wav or mp3)."; exit 1 }
    } elseif (($a -ceq '-m' -or $a -ceq '--mic') -and $hasNext) {
        $i++; $micSel = [string]$args[$i]
    } elseif (($a -ceq '-d' -or $a -ceq '--output-device') -and $hasNext) {
        $i++; $renderSel = [string]$args[$i]
    } elseif (($a -ceq '-a' -or $a -ceq '--amplify') -and $hasNext) {
        $i++; $v = ConvertTo-Atoi ([string]$args[$i])
        if ($v -lt 0)   { Write-Err 'Warning: --amplify clamped to 0.';   $v = 0 }
        if ($v -gt 300) { Write-Err 'Warning: --amplify clamped to 300.'; $v = 300 }
        $amplify = $v
    } else {
        Write-Err "Error: unknown or incomplete argument '$a'.`nRun 'audior.ps1 --help' for usage."
        exit 1
    }
}

if ($cmd -eq 'list') {
    Initialize-Engine
    Show-Devices
    exit 0
}

if ($cmd -eq 'stop') {
    try {
        $h = [System.Threading.EventWaitHandle]::OpenExisting("Local\AudiorStopEvent")
        [void]$h.Set()
        $h.Close()
        Write-Host "Stop signal sent."
        exit 0
    } catch [System.Threading.WaitHandleCannotBeOpenedException] {
        Write-Err "No active recording to stop."
        exit 1
    }
}

# record
if ([string]::IsNullOrEmpty($outputPath)) {
    Write-Err "Error: --output <file> is required.`nRun 'audior.ps1 --help' for usage."
    exit 1
}
$outputPath = Set-Extension $outputPath $format
Initialize-Engine

# The merge/separate prompt (when loopback is on and no choice given) is handled
# inside the engine, after device resolution, exactly as the C version does.
$rc = [Audior.Engine]::Record($outputPath, $format, $includeOut, $mixMode, $micSel, $renderSel, $amplify, $mergeMic)
exit $rc
