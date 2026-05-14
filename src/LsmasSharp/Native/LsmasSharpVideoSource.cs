using System.Runtime.InteropServices;
using System.Threading;

namespace LsmasSharp.Native;

public sealed class LsmasSharpVideoSource : IDisposable
{
    private nint _handle;
    private SharedLsmasSharpHandle? _shared;
    private bool _ownsHandle = true;
    private LsmasSharpVideoInfo? _cachedInfo;
    private global::LsmasSharp.TimeBase? _cachedTimeBase;
    private long[]? _cachedPtsList;

    private LsmasSharpVideoSource(nint handle)
    {
        _handle = handle;
    }

    internal static LsmasSharpVideoSource WrapShared(SharedLsmasSharpHandle shared)
    {
        shared.EnsureNotDisposed();
        return new LsmasSharpVideoSource(shared.Handle)
        {
            _shared = shared,
            _ownsHandle = false,
        };
    }

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

    public static LsmasSharpVideoSource Open(string path, global::LsmasSharp.VideoSourceOptions? options = null)
        => Open(path, options, progress: null, cancellationToken: default);

    public static LsmasSharpVideoSource Open(string path, global::LsmasSharp.VideoSourceOptions? options, global::LsmasSharp.IndexingProgressCallback? progress, CancellationToken cancellationToken = default)
    {
        options ??= new global::LsmasSharp.VideoSourceOptions();
        ThrowIfUnsupportedOptions(options);

        var nativeOpt = new LsmasSharpNativeMethods.VideoOpenOptions
        {
            stream_index = options.StreamIndex,
            threads = options.Threads,
            seek_mode = (int)options.SeekMode,
            seek_threshold = options.SeekThreshold,
            direct_rendering = options.DirectRendering ? 1 : 0,

            fpsnum = options.FpsNum,
            fpsden = options.FpsDen,

            variable_info = options.VariableInfo ? 1 : 0,
            output_format = nint.Zero,
            decoder = nint.Zero,
            prefer_hw = (int)options.PreferHw,

            ff_loglevel = options.FfLogLevel,

            cache_index = options.CacheIndex ? 1 : 0,
            cachefile = nint.Zero,
            cachedir = nint.Zero,

            soft_reset = options.SoftResetOnSeek ? 1 : 0,
            framelist = options.FrameList ? 1 : 0,

            repeat = options.ApplyRepeat ? 1 : 0,
            dominance = (int)options.Dominance,
        };

        var outputFormatUtf8 = StringToCoTaskMemUtf8OrNull(options.OutputFormat);
        var decoderUtf8 = StringToCoTaskMemUtf8OrNull(options.Decoder);
        var cacheFileUtf8 = StringToCoTaskMemUtf8OrNull(options.CacheFile);
        var cacheDirUtf8 = StringToCoTaskMemUtf8OrNull(options.CacheDir);

        try
        {
            nativeOpt.output_format = outputFormatUtf8;
            nativeOpt.decoder = decoderUtf8;
            nativeOpt.cachefile = cacheFileUtf8;
            nativeOpt.cachedir = cacheDirUtf8;

            var optPtr = Marshal.AllocHGlobal(Marshal.SizeOf<LsmasSharpNativeMethods.VideoOpenOptions>());
            try
            {
                Marshal.StructureToPtr(nativeOpt, optPtr, false);

                nint handle;
                if (progress is null && !cancellationToken.CanBeCanceled)
                {
                    handle = LsmasSharpNativeMethods.lsmas_video_open_utf8(path, optPtr, out var errPtr);
                    ThrowIfError(errPtr);
                }
                else
                {
                    var state = new ProgressState { Callback = progress ?? ((_, _) => true), CancellationToken = cancellationToken };
                    var gch = GCHandle.Alloc(state);
                    var cb = new LsmasSharpNativeMethods.ProgressCallback(OnProgress);
                    try
                    {
                        handle = LsmasSharpNativeMethods.lsmas_video_open_with_progress_utf8(path, optPtr, cb, GCHandle.ToIntPtr(gch), out var errPtr);
                        ThrowIfError(errPtr);
                    }
                    finally
                    {
                        gch.Free();
                    }
                }
                if (handle == nint.Zero)
                    throw new InvalidOperationException("lsmas_video_open_utf8 returned NULL without error.");

                return new LsmasSharpVideoSource(handle);
            }
            finally
            {
                Marshal.FreeHGlobal(optPtr);
            }
        }
        finally
        {
            Marshal.FreeCoTaskMem(outputFormatUtf8);
            Marshal.FreeCoTaskMem(decoderUtf8);
            Marshal.FreeCoTaskMem(cacheFileUtf8);
            Marshal.FreeCoTaskMem(cacheDirUtf8);
        }
    }

    private static void ThrowIfUnsupportedOptions(global::LsmasSharp.VideoSourceOptions options)
    {
        if (options.DirectRendering)
            throw new NotSupportedException($"{nameof(global::LsmasSharp.VideoSourceOptions)}.{nameof(options.DirectRendering)} is not supported in the independent provider (VS-only concept).");
        if (options.VariableInfo)
            throw new NotSupportedException($"{nameof(global::LsmasSharp.VideoSourceOptions)}.{nameof(options.VariableInfo)} is not supported in the independent provider (VS-only concept).");
        if (!string.IsNullOrWhiteSpace(options.OutputFormat))
            throw new NotSupportedException($"{nameof(global::LsmasSharp.VideoSourceOptions)}.{nameof(options.OutputFormat)} is not supported; use GetFrame/CopyFrame with {nameof(LsmasSharpFrameOutputFormat)} instead.");
        if (options.FrameList)
            throw new NotSupportedException($"{nameof(global::LsmasSharp.VideoSourceOptions)}.{nameof(options.FrameList)} is not supported; use GetSourcePictureTypes/GetSourceKeyframeFlags instead.");
    }

    public LsmasSharpVideoInfo GetInfo()
    {
        EnsureNotDisposed();
        if (_cachedInfo is { } cached)
            return cached;
        var ret = LsmasSharpNativeMethods.lsmas_video_get_info(_handle, out var info, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_get_info failed: {ret}.");
        var managed = new LsmasSharpVideoInfo(info.width, info.height, info.num_frames, info.fps_num, info.fps_den);
        _cachedInfo = managed;
        return managed;
    }

    public global::LsmasSharp.VideoProperties GetStreamProperties()
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_stream_props(_handle, out var props, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_get_stream_props failed: {ret}.");

        return new global::LsmasSharp.VideoProperties(
            props.sar_num,
            props.sar_den,
            props.color_range,
            props.colorspace,
            props.color_primaries,
            props.color_trc,
            props.chroma_location,
            props.field_order,
            props.interlaced_frame,
            props.top_field_first
        );
    }

    public bool TryGetMasteringDisplayMetadata(out global::LsmasSharp.MasteringDisplayMetadata metadata)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_stream_mastering_display_metadata(_handle, out var md, out var errPtr);
        ThrowIfError(errPtr);
        if (ret < 0)
            throw new InvalidOperationException($"lsmas_video_get_stream_mastering_display_metadata failed: {ret}.");
        if (ret != 0)
        {
            metadata = default!;
            return false;
        }

        metadata = new global::LsmasSharp.MasteringDisplayMetadata(
            md.has_primaries != 0,
            md.has_luminance != 0,
            new global::LsmasSharp.Rational32(md.primary_r_x.num, md.primary_r_x.den),
            new global::LsmasSharp.Rational32(md.primary_r_y.num, md.primary_r_y.den),
            new global::LsmasSharp.Rational32(md.primary_g_x.num, md.primary_g_x.den),
            new global::LsmasSharp.Rational32(md.primary_g_y.num, md.primary_g_y.den),
            new global::LsmasSharp.Rational32(md.primary_b_x.num, md.primary_b_x.den),
            new global::LsmasSharp.Rational32(md.primary_b_y.num, md.primary_b_y.den),
            new global::LsmasSharp.Rational32(md.white_x.num, md.white_x.den),
            new global::LsmasSharp.Rational32(md.white_y.num, md.white_y.den),
            new global::LsmasSharp.Rational32(md.max_luminance.num, md.max_luminance.den),
            new global::LsmasSharp.Rational32(md.min_luminance.num, md.min_luminance.den)
        );
        return true;
    }

    public bool TryGetContentLightMetadata(out global::LsmasSharp.ContentLightMetadata metadata)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_stream_content_light_metadata(_handle, out var cl, out var errPtr);
        ThrowIfError(errPtr);
        if (ret < 0)
            throw new InvalidOperationException($"lsmas_video_get_stream_content_light_metadata failed: {ret}.");
        if (ret != 0)
        {
            metadata = default!;
            return false;
        }

        metadata = new global::LsmasSharp.ContentLightMetadata(cl.max_cll, cl.max_fall);
        return true;
    }

    public bool TryGetDoviConfiguration(out global::LsmasSharp.DoviConfigurationRecord record)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_stream_dovi_conf(_handle, out var dc, out var errPtr);
        ThrowIfError(errPtr);
        if (ret < 0)
            throw new InvalidOperationException($"lsmas_video_get_stream_dovi_conf failed: {ret}.");
        if (ret != 0)
        {
            record = default!;
            return false;
        }

        record = new global::LsmasSharp.DoviConfigurationRecord(
            dc.dv_version_major,
            dc.dv_version_minor,
            dc.dv_profile,
            dc.dv_level,
            dc.rpu_present_flag != 0,
            dc.el_present_flag != 0,
            dc.bl_present_flag != 0,
            dc.dv_bl_signal_compatibility_id
        );
        return true;
    }

    public void Flush()
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_flush(_handle, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_flush failed: {ret}.");
    }

    public void SeekFrame(int frameIndex)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_seek_frame(_handle, frameIndex, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_seek_frame failed: {ret}.");
    }

    public global::LsmasSharp.VideoProperties GetFrameProperties(int frameIndex)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_frame_props(_handle, frameIndex, out var props, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_get_frame_props failed: {ret}.");

        return new global::LsmasSharp.VideoProperties(
            props.sar_num,
            props.sar_den,
            props.color_range,
            props.colorspace,
            props.color_primaries,
            props.color_trc,
            props.chroma_location,
            props.field_order,
            props.interlaced_frame,
            props.top_field_first
        );
    }

    public int GetSourceFrameCount()
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_get_source_frame_count(_handle, out var count, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_get_source_frame_count failed: {ret}.");
        return count;
    }

    public sbyte[] GetSourcePictureTypes()
    {
        EnsureNotDisposed();
        var needed = LsmasSharpNativeMethods.lsmas_video_get_source_pict_type_list(_handle, nint.Zero, 0, out var errPtr);
        ThrowIfError(errPtr);
        if (needed <= 0)
            return [];

        var types = new sbyte[needed];
        unsafe
        {
            fixed (sbyte* ptr = types)
            {
                var written = LsmasSharpNativeMethods.lsmas_video_get_source_pict_type_list(_handle, (nint)ptr, types.Length, out errPtr);
                ThrowIfError(errPtr);
                if (written != types.Length)
                    Array.Resize(ref types, Math.Max(0, written));
                return types;
            }
        }
    }

    public byte[] GetSourceKeyframeFlags()
    {
        EnsureNotDisposed();
        var needed = LsmasSharpNativeMethods.lsmas_video_get_source_keyframe_flags(_handle, nint.Zero, 0, out var errPtr);
        ThrowIfError(errPtr);
        if (needed <= 0)
            return [];

        var flags = new byte[needed];
        unsafe
        {
            fixed (byte* ptr = flags)
            {
                var written = LsmasSharpNativeMethods.lsmas_video_get_source_keyframe_flags(_handle, (nint)ptr, flags.Length, out errPtr);
                ThrowIfError(errPtr);
                if (written != flags.Length)
                    Array.Resize(ref flags, Math.Max(0, written));
                return flags;
            }
        }
    }

    public global::LsmasSharp.TimeBase GetTimeBase()
    {
        EnsureNotDisposed();
        if (_cachedTimeBase is { } cached)
            return cached;
        var ret = LsmasSharpNativeMethods.lsmas_video_get_time_base(_handle, out var num, out var den, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_get_time_base failed: {ret}.");
        if (den == 0)
            throw new InvalidOperationException("Invalid time base returned (den=0).");
        var tb = new global::LsmasSharp.TimeBase(num, den);
        _cachedTimeBase = tb;
        return tb;
    }

    public long GetFramePts(int frameIndex)
    {
        EnsureNotDisposed();
        var pts = LsmasSharpNativeMethods.lsmas_video_get_frame_pts(_handle, frameIndex, out var errPtr);
        ThrowIfError(errPtr);
        return pts;
    }

    public long[] GetPtsList()
    {
        EnsureNotDisposed();
        if (_cachedPtsList is { } cached)
            return cached;
        var needed = LsmasSharpNativeMethods.lsmas_video_get_pts_list(_handle, nint.Zero, 0, out var errPtr);
        ThrowIfError(errPtr);
        if (needed <= 0)
            return [];

        var pts = new long[needed];
        unsafe
        {
            fixed (long* ptr = pts)
            {
                var written = LsmasSharpNativeMethods.lsmas_video_get_pts_list(_handle, (nint)ptr, pts.Length, out errPtr);
                ThrowIfError(errPtr);
                if (written != pts.Length)
                    Array.Resize(ref pts, Math.Max(0, written));
                _cachedPtsList = pts;
                return pts;
            }
        }
    }

    public TimeSpan? GetFrameTimecode(int frameIndex)
    {
        var tb = GetTimeBase();
        var pts = GetFramePts(frameIndex);
        if (!tb.TryToTimeSpan(pts, out var ts))
            return null;
        return ts;
    }

    public TimeSpan? GetFrameDuration(int frameIndex, bool estimateIfMissing = true)
    {
        var info = GetInfo();
        if (frameIndex < 0 || frameIndex >= info.NumFrames)
            throw new ArgumentOutOfRangeException(nameof(frameIndex));

        var tb = GetTimeBase();
        var pts = GetFramePts(frameIndex);
        if (pts == long.MinValue)
            return null;

        long? deltaPts = null;
        if (frameIndex + 1 < info.NumFrames)
        {
            var next = GetFramePts(frameIndex + 1);
            if (next != long.MinValue)
            {
                var d = next - pts;
                if (d > 0)
                    deltaPts = d;
            }
        }
        else if (frameIndex > 0)
        {
            var prev = GetFramePts(frameIndex - 1);
            if (prev != long.MinValue)
            {
                var d = pts - prev;
                if (d > 0)
                    deltaPts = d;
            }
        }

        if (deltaPts is null && estimateIfMissing)
        {
            var list = GetPtsList();
            deltaPts = GuessDefaultDeltaPts(list, info, tb);
        }

        if (deltaPts is null)
            return null;

        if (!tb.TryToTimeSpan(deltaPts.Value, out var duration))
            return null;
        return duration;
    }

    public TimeSpan?[] GetTimecodesList()
    {
        var tb = GetTimeBase();
        var pts = GetPtsList();
        var arr = new TimeSpan?[pts.Length];
        for (var i = 0; i < pts.Length; i++)
        {
            if (!tb.TryToTimeSpan(pts[i], out var ts))
            {
                arr[i] = null;
                continue;
            }
            arr[i] = ts;
        }
        return arr;
    }

    public TimeSpan?[] GetDurationsList(bool estimateIfMissing = true)
    {
        var info = GetInfo();
        var tb = GetTimeBase();
        var pts = GetPtsList();
        var arr = new TimeSpan?[pts.Length];
        var defaultDeltaPts = estimateIfMissing ? GuessDefaultDeltaPts(pts, info, tb) : (long?)null;

        for (var i = 0; i < pts.Length; i++)
        {
            var cur = pts[i];
            if (cur == long.MinValue)
            {
                arr[i] = null;
                continue;
            }

            long? deltaPts = null;
            if (i + 1 < pts.Length)
            {
                var next = pts[i + 1];
                if (next != long.MinValue)
                {
                    var d = next - cur;
                    if (d > 0)
                        deltaPts = d;
                }
            }
            else if (i > 0)
            {
                var prev = pts[i - 1];
                if (prev != long.MinValue)
                {
                    var d = cur - prev;
                    if (d > 0)
                        deltaPts = d;
                }
            }

            if (deltaPts is null && estimateIfMissing)
                deltaPts = defaultDeltaPts;

            if (deltaPts is null || !tb.TryToTimeSpan(deltaPts.Value, out var dur))
                arr[i] = null;
            else
                arr[i] = dur;
        }

        return arr;
    }

    private static long? GuessDefaultDeltaPts(long[] ptsList, LsmasSharpVideoInfo info, global::LsmasSharp.TimeBase timeBase)
    {
        var deltas = new List<long>(Math.Min(ptsList.Length, 4096));
        for (var i = 1; i < ptsList.Length; i++)
        {
            var a = ptsList[i - 1];
            var b = ptsList[i];
            if (a == long.MinValue || b == long.MinValue)
                continue;
            var d = b - a;
            if (d > 0)
                deltas.Add(d);
        }

        if (deltas.Count > 0)
        {
            deltas.Sort();
            return deltas[deltas.Count / 2];
        }

        if (info.FpsNum > 0 && info.FpsDen > 0)
        {
            Int128 numer = (Int128)timeBase.Den * info.FpsDen;
            Int128 denom = (Int128)timeBase.Num * info.FpsNum;
            if (denom > 0)
            {
                var q = (numer + denom / 2) / denom;
                if (q > 0 && q <= long.MaxValue)
                    return (long)q;
            }
        }

        return null;
    }

    public long GetFrameBytesRequired(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, int dstStride = 0)
        => GetFrameLayout(frameIndex, outputFormat, dstStride).RequiredBytes;

    public LsmasSharpFrameBufferLayout GetFrameLayout(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, int dstStride = 0)
    {
        EnsureNotDisposed();
        var bytes = LsmasSharpNativeMethods.lsmas_video_get_frame(
            _handle,
            frameIndex,
            (int)outputFormat,
            nint.Zero,
            dstStride,
            out var nativeLayout,
            out var errPtr);
        ThrowIfError(errPtr);
        if (bytes < 0)
            throw new InvalidOperationException($"lsmas_video_get_frame failed: {bytes}.");

        var layout = new LsmasSharpFrameBufferLayout(nativeLayout);
        if (layout.RequiredBytes != bytes)
            throw new InvalidOperationException($"Native frame layout mismatch. bytes={bytes} layout={layout.RequiredBytes}.");
        return layout;
    }

    public LsmasSharpFrameBufferLayout CopyFrame(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, nint dst, int dstStride = 0)
    {
        EnsureNotDisposed();
        if (dst == nint.Zero)
            throw new ArgumentException("dst is NULL.", nameof(dst));

        var bytes = LsmasSharpNativeMethods.lsmas_video_get_frame(
            _handle,
            frameIndex,
            (int)outputFormat,
            dst,
            dstStride,
            out var nativeLayout,
            out var errPtr);
        ThrowIfError(errPtr);
        if (bytes <= 0)
            throw new InvalidOperationException($"lsmas_video_get_frame returned {bytes}.");

        var layout = new LsmasSharpFrameBufferLayout(nativeLayout);
        if (layout.RequiredBytes != bytes)
            throw new InvalidOperationException($"Native frame layout mismatch. bytes={bytes} layout={layout.RequiredBytes}.");
        return layout;
    }

    public LsmasSharpFrameBufferLayout CopyFrame(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, byte[] buffer, int dstStride = 0)
    {
        if (buffer is null)
            throw new ArgumentNullException(nameof(buffer));

        var layout = GetFrameLayout(frameIndex, outputFormat, dstStride);
        if (layout.RequiredBytes > buffer.LongLength)
            throw new ArgumentException($"Buffer is too small. required={layout.RequiredBytes} actual={buffer.LongLength}.", nameof(buffer));

        var effectiveStride = dstStride > 0 || outputFormat == LsmasSharpFrameOutputFormat.Native
            ? dstStride
            : layout.PlaneCount > 0 ? layout.Planes[0].Stride : 0;

        unsafe
        {
            fixed (byte* ptr = buffer)
            {
                return CopyFrame(frameIndex, outputFormat, (nint)ptr, effectiveStride);
            }
        }
    }

    public byte[] GetFrame(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, int dstStride = 0)
        => GetFrame(frameIndex, outputFormat, out _, dstStride);

    public byte[] GetFrame(int frameIndex, LsmasSharpFrameOutputFormat outputFormat, out LsmasSharpFrameBufferLayout layout, int dstStride = 0)
    {
        layout = GetFrameLayout(frameIndex, outputFormat, dstStride);
        if (layout.RequiredBytes > int.MaxValue)
            throw new InvalidOperationException("Frame too large for a single managed byte[].");

        var buffer = new byte[(int)layout.RequiredBytes];
        var effectiveStride = dstStride > 0 || outputFormat == LsmasSharpFrameOutputFormat.Native
            ? dstStride
            : layout.PlaneCount > 0 ? layout.Planes[0].Stride : 0;

        unsafe
        {
            fixed (byte* ptr = buffer)
            {
                layout = CopyFrame(frameIndex, outputFormat, (nint)ptr, effectiveStride);
            }
        }

        return buffer;
    }

    public LsmasSharpVideoFrame AcquireFrame(int frameIndex)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_acquire_avframe(_handle, frameIndex, out var frameHandle, out var errPtr);
        ThrowIfError(errPtr);
        if (ret != 0 || frameHandle == nint.Zero)
            throw new InvalidOperationException($"lsmas_video_acquire_avframe failed: {ret}.");
        return new LsmasSharpVideoFrame(frameHandle);
    }

    public void Dispose()
    {
        if (_ownsHandle)
        {
            if (_handle == nint.Zero)
                return;
            LsmasSharpNativeMethods.lsmas_video_close(_handle);
            _handle = nint.Zero;
        }
        GC.SuppressFinalize(this);
    }

    ~LsmasSharpVideoSource()
    {
        Dispose();
    }

    private void EnsureNotDisposed()
    {
        if (_ownsHandle)
        {
            if (_handle == nint.Zero)
                throw new ObjectDisposedException(nameof(LsmasSharpVideoSource));
            return;
        }

        _shared?.EnsureNotDisposed();
        _handle = _shared?.Handle ?? nint.Zero;
        if (_handle == nint.Zero)
            throw new ObjectDisposedException(nameof(LsmasSharpVideoSource));
    }

    internal static void ThrowIfNativeError(nint errPtr) => ThrowIfError(errPtr);

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
