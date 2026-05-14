using System.Runtime.InteropServices;
using System.Threading;

namespace LsmasSharp.Native;

public sealed class LsmasSharpAudioSource : IDisposable
{
    private nint _handle;
    private SharedLsmasSharpHandle? _shared;
    private bool _ownsHandle = true;
    private LsmasSharpAudioInfo? _cachedInfo;

    private LsmasSharpAudioSource(nint handle)
    {
        _handle = handle;
    }

    internal static LsmasSharpAudioSource WrapShared(SharedLsmasSharpHandle shared)
    {
        shared.EnsureNotDisposed();
        return new LsmasSharpAudioSource(shared.Handle)
        {
            _shared = shared,
            _ownsHandle = false,
        };
    }

    public static LsmasSharpAudioSource Open(string path, global::LsmasSharp.AudioSourceOptions? options = null)
        => Open(path, options, progress: null, cancellationToken: default);

    private sealed class ProgressState
    {
        public required global::LsmasSharp.IndexingProgressCallback Callback { get; init; }
        public CancellationToken CancellationToken { get; init; }
    }

    private static int OnProgress(nint userdata, nint messageUtf8, int percent)
    {
        var handle = GCHandle.FromIntPtr(userdata);
        if (handle.Target is not ProgressState state)
            return 0;

        if (state.CancellationToken.IsCancellationRequested)
            return 1;

        var msg = messageUtf8 != nint.Zero ? Marshal.PtrToStringUTF8(messageUtf8) : null;
        return state.Callback(msg, percent) ? 0 : 1;
    }

    public static LsmasSharpAudioSource Open(string path, global::LsmasSharp.AudioSourceOptions? options, global::LsmasSharp.IndexingProgressCallback? progress, CancellationToken cancellationToken = default)
    {
        options ??= new global::LsmasSharp.AudioSourceOptions();

        var nativeOpt = new LsmasSharpNativeMethods.AudioOpenOptions
        {
            stream_index = options.StreamIndex,
            threads = options.Threads,
            av_sync = options.AvSync ? 1 : 0,

            ff_loglevel = options.FfLogLevel,
            decoder = nint.Zero,

            cache_index = options.CacheIndex ? 1 : 0,
            cachefile = nint.Zero,
            cachedir = nint.Zero,

            channel_layout = options.ChannelLayout,
            sample_rate = options.SampleRate,
            sample_format = (int)options.SampleFormat,
        };

        var decoderUtf8 = StringToCoTaskMemUtf8OrNull(options.Decoder);
        var cacheFileUtf8 = StringToCoTaskMemUtf8OrNull(options.CacheFile);
        var cacheDirUtf8 = StringToCoTaskMemUtf8OrNull(options.CacheDir);

        try
        {
            nativeOpt.decoder = decoderUtf8;
            nativeOpt.cachefile = cacheFileUtf8;
            nativeOpt.cachedir = cacheDirUtf8;

            var optPtr = Marshal.AllocHGlobal(Marshal.SizeOf<LsmasSharpNativeMethods.AudioOpenOptions>());
            try
            {
                Marshal.StructureToPtr(nativeOpt, optPtr, false);

                nint handle;
                if (progress is null && !cancellationToken.CanBeCanceled)
                {
                    handle = LsmasSharpNativeMethods.lsmas_audio_open_utf8(path, optPtr, out var errPtr);
                    ThrowIfError(errPtr);
                }
                else
                {
                    var state = new ProgressState { Callback = progress ?? ((_, _) => true), CancellationToken = cancellationToken };
                    var gch = GCHandle.Alloc(state);
                    var cb = new LsmasSharpNativeMethods.ProgressCallback(OnProgress);
                    try
                    {
                        handle = LsmasSharpNativeMethods.lsmas_audio_open_with_progress_utf8(path, optPtr, cb, GCHandle.ToIntPtr(gch), out var errPtr);
                        ThrowIfError(errPtr);
                    }
                    finally
                    {
                        gch.Free();
                    }
                }
                if (handle == nint.Zero)
                    throw new InvalidOperationException("lsmas_audio_open_utf8 returned NULL without error.");

                return new LsmasSharpAudioSource(handle);
            }
            finally
            {
                Marshal.FreeHGlobal(optPtr);
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(decoderUtf8);
            Marshal.FreeCoTaskMem(cacheFileUtf8);
            Marshal.FreeCoTaskMem(cacheDirUtf8);
        }
    }

    public LsmasSharpAudioInfo GetInfo()
    {
        EnsureNotDisposed();
        if (_cachedInfo is { } cached)
            return cached;

        var ret = LsmasSharpNativeMethods.lsmas_audio_get_info(_handle, out var info, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_audio_get_info failed: {ret}.");

        var managed = new LsmasSharpAudioInfo(
            info.stream_index,
            info.sample_rate,
            info.channels,
            info.channel_layout,
            info.sample_format,
            info.bits_per_sample,
            info.bytes_per_sample,
            info.block_align,
            info.decoded_samples,
            info.delay_samples,
            info.total_samples
        );

        _cachedInfo = managed;
        return managed;
    }

    public long TimeToSample(TimeSpan time, MidpointRounding rounding = MidpointRounding.AwayFromZero)
    {
        var info = GetInfo();
        if (info.SampleRate <= 0)
            throw new InvalidOperationException($"Invalid sample rate: {info.SampleRate}.");

        try
        {
            Int128 numer = (Int128)time.Ticks * info.SampleRate;
            Int128 den = TimeSpan.TicksPerSecond;
            if (den <= 0)
                return 0;

            Int128 q = rounding switch
            {
                MidpointRounding.ToZero => numer / den,
                MidpointRounding.AwayFromZero => numer >= 0 ? (numer + den / 2) / den : -((-numer + den / 2) / den),
                MidpointRounding.ToNegativeInfinity => numer >= 0 ? numer / den : -((-numer + den - 1) / den),
                MidpointRounding.ToPositiveInfinity => numer >= 0 ? (numer + den - 1) / den : -( (-numer) / den ),
                _ => numer >= 0 ? (numer + den / 2) / den : -((-numer + den / 2) / den),
            };

            if (q < long.MinValue) return long.MinValue;
            if (q > long.MaxValue) return long.MaxValue;
            return (long)q;
        }
        catch (OverflowException)
        {
            return time.Ticks >= 0 ? long.MaxValue : long.MinValue;
        }
    }

    public TimeSpan SampleToTime(long sampleIndex)
    {
        var info = GetInfo();
        if (info.SampleRate <= 0)
            throw new InvalidOperationException($"Invalid sample rate: {info.SampleRate}.");

        Int128 numer = (Int128)sampleIndex * TimeSpan.TicksPerSecond;
        Int128 den = info.SampleRate;
        var ticks = (long)(numer / den);
        return new TimeSpan(ticks);
    }

    public long CopySamplesRaw(TimeSpan startTime, int sampleFrameCount, nint dst)
        => CopySamplesRaw(TimeToSample(startTime), sampleFrameCount, dst);

    public long CopySamplesRaw(TimeSpan startTime, int sampleFrameCount, byte[] buffer)
        => CopySamplesRaw(TimeToSample(startTime), sampleFrameCount, buffer);

    public long CopySamplesRaw(long startSampleFrame, int sampleFrameCount, nint dst)
    {
        EnsureNotDisposed();
        if (dst == nint.Zero)
            throw new ArgumentException("dst is NULL.", nameof(dst));
        if (sampleFrameCount < 0)
            throw new ArgumentOutOfRangeException(nameof(sampleFrameCount));
        if (sampleFrameCount == 0)
            return 0;

        var written = LsmasSharpNativeMethods.lsmas_audio_get_samples(_handle, dst, startSampleFrame, sampleFrameCount, out var errPtr);
        ThrowIfError(errPtr);
        return written;
    }

    public long CopySamplesRaw(long startSampleFrame, int sampleFrameCount, byte[] buffer)
    {
        if (buffer is null)
            throw new ArgumentNullException(nameof(buffer));
        if (sampleFrameCount < 0)
            throw new ArgumentOutOfRangeException(nameof(sampleFrameCount));
        if (sampleFrameCount == 0)
            return 0;

        var info = GetInfo();
        var requiredBytes = (long)sampleFrameCount * info.BlockAlign;
        if ((long)buffer.Length < requiredBytes)
            throw new ArgumentException($"Buffer too small: need {requiredBytes} bytes, got {buffer.Length}.", nameof(buffer));

        unsafe
        {
            fixed (byte* ptr = buffer)
            {
                return CopySamplesRaw(startSampleFrame, sampleFrameCount, (nint)ptr);
            }
        }
    }

    public long CopySamplesFloat32(long startSampleFrame, int sampleFrameCount, float[] interleavedBuffer)
    {
        if (interleavedBuffer is null)
            throw new ArgumentNullException(nameof(interleavedBuffer));
        if (sampleFrameCount < 0)
            throw new ArgumentOutOfRangeException(nameof(sampleFrameCount));
        if (sampleFrameCount == 0)
            return 0;

        var info = GetInfo();
        if (info.BytesPerSample != sizeof(float))
            throw new NotSupportedException($"CopySamplesFloat32 requires float32 output; bytes_per_sample={info.BytesPerSample}.");

        var required = (long)sampleFrameCount * info.Channels;
        if ((long)interleavedBuffer.Length < required)
            throw new ArgumentException($"Buffer too small: need {required} float samples, got {interleavedBuffer.Length}.", nameof(interleavedBuffer));

        unsafe
        {
            fixed (float* ptr = interleavedBuffer)
            {
                return CopySamplesRaw(startSampleFrame, sampleFrameCount, (nint)ptr);
            }
        }
    }

    public byte[] GetSamplesRaw(long startSampleFrame, int sampleFrameCount)
    {
        var info = GetInfo();
        var bytes = checked((int)((long)sampleFrameCount * info.BlockAlign));
        var buffer = new byte[bytes];
        CopySamplesRaw(startSampleFrame, sampleFrameCount, buffer);
        return buffer;
    }

    public float[] GetSamplesFloat32(long startSampleFrame, int sampleFrameCount)
    {
        var info = GetInfo();
        if (info.BytesPerSample != sizeof(float))
            throw new NotSupportedException($"GetSamplesFloat32 requires float32 output; bytes_per_sample={info.BytesPerSample}.");

        var count = checked(sampleFrameCount * info.Channels);
        var buffer = new float[count];
        CopySamplesFloat32(startSampleFrame, sampleFrameCount, buffer);
        return buffer;
    }

    public void Dispose()
    {
        if (_ownsHandle)
        {
            if (_handle == nint.Zero)
                return;
            LsmasSharpNativeMethods.lsmas_audio_close(_handle);
            _handle = nint.Zero;
        }
        GC.SuppressFinalize(this);
    }

    ~LsmasSharpAudioSource()
    {
        Dispose();
    }

    private void EnsureNotDisposed()
    {
        if (_ownsHandle)
        {
            if (_handle == nint.Zero)
                throw new ObjectDisposedException(nameof(LsmasSharpAudioSource));
            return;
        }

        _shared?.EnsureNotDisposed();
        _handle = _shared?.Handle ?? nint.Zero;
        if (_handle == nint.Zero)
            throw new ObjectDisposedException(nameof(LsmasSharpAudioSource));
    }

    private static void ThrowIfError(nint errPtr)
    {
        if (errPtr == nint.Zero)
            return;
        try
        {
            var msg = Marshal.PtrToStringUTF8(errPtr) ?? "Unknown native error.";
            throw new InvalidOperationException(msg);
        }
        finally
        {
            LsmasSharpNativeMethods.lsmas_free(errPtr);
        }
    }

    private static nint StringToCoTaskMemUtf8OrNull(string? value)
    {
        if (string.IsNullOrEmpty(value))
            return nint.Zero;
        return Marshal.StringToCoTaskMemUTF8(value);
    }
}
