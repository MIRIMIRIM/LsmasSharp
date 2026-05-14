using LsmasSharp.Native;
using System.Runtime.InteropServices;
using System.Threading;

namespace LsmasSharp;

public sealed class SyncedAvSource : IDisposable
{
    public LsmasSharpVideoSource Video { get; }
    public LsmasSharpAudioSource Audio { get; }
    public AvSyncIndex SyncIndex { get; }

    private readonly int _blockAlign;
    private readonly long _defaultFrameLengthSamples;
    private readonly long[] _frameStartTicks;
    private readonly SharedLsmasSharpHandle? _shared;

    private SyncedAvSource(LsmasSharpVideoSource video, LsmasSharpAudioSource audio, AvSyncIndex syncIndex, int blockAlign, long defaultFrameLengthSamples, long[] frameStartTicks, SharedLsmasSharpHandle? shared)
    {
        Video = video;
        Audio = audio;
        SyncIndex = syncIndex;
        _blockAlign = blockAlign;
        _defaultFrameLengthSamples = defaultFrameLengthSamples;
        _frameStartTicks = frameStartTicks;
        _shared = shared;
    }

    private sealed class ProgressState
    {
        public required IndexingProgressCallback Callback { get; init; }
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

    public static SyncedAvSource Open(string path, VideoSourceOptions? videoOptions = null, AudioSourceOptions? audioOptions = null, bool strictTimeline = false, IndexingProgressCallback? progress = null, CancellationToken cancellationToken = default)
    {
        videoOptions ??= new VideoSourceOptions();
        audioOptions ??= new AudioSourceOptions();

        var vo = videoOptions;
        var ao = audioOptions with { AvSync = true };

        var vNative = new LsmasSharpNativeMethods.VideoOpenOptions
        {
            stream_index = vo.StreamIndex,
            threads = vo.Threads,
            seek_mode = (int)vo.SeekMode,
            seek_threshold = vo.SeekThreshold,

            fpsnum = vo.FpsNum,
            fpsden = vo.FpsDen,

            decoder = nint.Zero,
            prefer_hw = (int)vo.PreferHw,

            ff_loglevel = vo.FfLogLevel,

            cache_index = vo.CacheIndex ? 1 : 0,
            cachefile = nint.Zero,
            cachedir = nint.Zero,

            soft_reset = vo.SoftResetOnSeek ? 1 : 0,

            repeat = vo.ApplyRepeat ? 1 : 0,
            dominance = (int)vo.Dominance,
        };

        var aNative = new LsmasSharpNativeMethods.AudioOpenOptions
        {
            stream_index = ao.StreamIndex,
            threads = ao.Threads,
            av_sync = ao.AvSync ? 1 : 0,

            ff_loglevel = ao.FfLogLevel,
            decoder = nint.Zero,

            cache_index = ao.CacheIndex ? 1 : 0,
            cachefile = nint.Zero,
            cachedir = nint.Zero,

            channel_layout = ao.ChannelLayout,
            sample_rate = ao.SampleRate,
            sample_format = (int)ao.SampleFormat,
        };

        var vDecoderUtf8 = Marshal.StringToCoTaskMemUTF8(vo.Decoder);
        var vCacheFileUtf8 = vo.CacheFile is { Length: > 0 } ? Marshal.StringToCoTaskMemUTF8(vo.CacheFile) : nint.Zero;
        var vCacheDirUtf8 = vo.CacheDir is { Length: > 0 } ? Marshal.StringToCoTaskMemUTF8(vo.CacheDir) : nint.Zero;

        var aDecoderUtf8 = Marshal.StringToCoTaskMemUTF8(ao.Decoder);
        var aCacheFileUtf8 = ao.CacheFile is { Length: > 0 } ? Marshal.StringToCoTaskMemUTF8(ao.CacheFile) : nint.Zero;
        var aCacheDirUtf8 = ao.CacheDir is { Length: > 0 } ? Marshal.StringToCoTaskMemUTF8(ao.CacheDir) : nint.Zero;

        nint vOptPtr = nint.Zero;
        nint aOptPtr = nint.Zero;

        try
        {
            vNative.decoder = string.IsNullOrWhiteSpace(vo.Decoder) ? nint.Zero : vDecoderUtf8;
            vNative.cachefile = vCacheFileUtf8;
            vNative.cachedir = vCacheDirUtf8;

            aNative.decoder = string.IsNullOrWhiteSpace(ao.Decoder) ? nint.Zero : aDecoderUtf8;
            aNative.cachefile = aCacheFileUtf8 != nint.Zero ? aCacheFileUtf8 : vCacheFileUtf8;
            aNative.cachedir = aCacheDirUtf8 != nint.Zero ? aCacheDirUtf8 : vCacheDirUtf8;

            vOptPtr = Marshal.AllocHGlobal(Marshal.SizeOf<LsmasSharpNativeMethods.VideoOpenOptions>());
            aOptPtr = Marshal.AllocHGlobal(Marshal.SizeOf<LsmasSharpNativeMethods.AudioOpenOptions>());
            Marshal.StructureToPtr(vNative, vOptPtr, false);
            Marshal.StructureToPtr(aNative, aOptPtr, false);

            nint handle;
            if (progress is null && !cancellationToken.CanBeCanceled)
            {
                handle = LsmasSharpNativeMethods.lsmas_av_open_utf8(path, vOptPtr, aOptPtr, out var errPtr);
                if (errPtr != nint.Zero)
                {
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
            }
            else
            {
                var state = new ProgressState { Callback = progress ?? ((_, _) => true), CancellationToken = cancellationToken };
                var gch = GCHandle.Alloc(state);
                var cb = new LsmasSharpNativeMethods.ProgressCallback(OnProgress);
                try
                {
                    handle = LsmasSharpNativeMethods.lsmas_av_open_with_progress_utf8(path, vOptPtr, aOptPtr, cb, GCHandle.ToIntPtr(gch), out var errPtr);
                    if (errPtr != nint.Zero)
                    {
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
                }
                finally
                {
                    gch.Free();
                }
            }
            if (handle == nint.Zero)
                throw new InvalidOperationException("lsmas_av_open_utf8 returned NULL without error.");

            var shared = new SharedLsmasSharpHandle(handle);
            var video = LsmasSharpVideoSource.WrapShared(shared);
            var audio = LsmasSharpAudioSource.WrapShared(shared);

            var vi = video.GetInfo();
            var tb = video.GetTimeBase();
            var pts = video.GetPtsList();
            var ai = audio.GetInfo();

            var sync = AvSync.BuildIndex(tb, pts, vi.FpsNum, vi.FpsDen, ai.SampleRate);
            long[] ticks0;
            if (strictTimeline)
            {
                if (!TimecodesV2.TryBuildNormalizedTicksStrict(tb, pts, out ticks0, out var err))
                    throw new InvalidOperationException($"Strict timeline failed: {err}");
            }
            else
            {
                ticks0 = TimecodesV2.BuildNormalizedTicks(tb, pts, vi.FpsNum, vi.FpsDen);
            }

            var defaultLen = GuessDefaultFrameLengthSamples(sync);
            if (defaultLen <= 0)
                defaultLen = Math.Max(1, ai.SampleRate / 60);

            return new SyncedAvSource(video, audio, sync, ai.BlockAlign, defaultLen, ticks0, shared);
        }
        finally
        {
            if (vOptPtr != nint.Zero) Marshal.FreeHGlobal(vOptPtr);
            if (aOptPtr != nint.Zero) Marshal.FreeHGlobal(aOptPtr);

            if (vDecoderUtf8 != nint.Zero) Marshal.FreeCoTaskMem(vDecoderUtf8);
            if (vCacheFileUtf8 != nint.Zero) Marshal.FreeCoTaskMem(vCacheFileUtf8);
            if (vCacheDirUtf8 != nint.Zero) Marshal.FreeCoTaskMem(vCacheDirUtf8);

            if (aDecoderUtf8 != nint.Zero) Marshal.FreeCoTaskMem(aDecoderUtf8);
            if (aCacheFileUtf8 != nint.Zero && aCacheFileUtf8 != vCacheFileUtf8) Marshal.FreeCoTaskMem(aCacheFileUtf8);
            if (aCacheDirUtf8 != nint.Zero && aCacheDirUtf8 != vCacheDirUtf8) Marshal.FreeCoTaskMem(aCacheDirUtf8);
        }
    }

    public (long StartSample, int SampleFrames) GetAudioRangeForFrame(int frameIndex, long maxSamples = 0)
    {
        var (start, len) = SyncIndex.GetFrameAudioRange(frameIndex, _defaultFrameLengthSamples);
        if (maxSamples > 0 && len > maxSamples)
            len = (int)Math.Min(len, maxSamples);
        return (start, len);
    }

    public int FindFrameAtOrBeforeTime(TimeSpan time, bool clamp = true)
    {
        if (_frameStartTicks.Length == 0)
            return clamp ? 0 : -1;

        var t = time.Ticks;
        if (t < _frameStartTicks[0])
            return clamp ? 0 : -1;

        var lastIndex = _frameStartTicks.Length - 1;
        if (t >= _frameStartTicks[lastIndex])
            return lastIndex;

        var lo = 0;
        var hi = lastIndex;
        while (lo + 1 < hi)
        {
            var mid = lo + ((hi - lo) / 2);
            var v = _frameStartTicks[mid];
            if (v <= t)
                lo = mid;
            else
                hi = mid;
        }
        return lo;
    }

    public long TimeToSample(TimeSpan time) => Audio.TimeToSample(time);

    public TimeSpan GetFrameStartTime(int frameIndex)
    {
        if ((uint)frameIndex >= (uint)_frameStartTicks.Length)
            throw new ArgumentOutOfRangeException(nameof(frameIndex));
        return new TimeSpan(_frameStartTicks[frameIndex]);
    }

    public TimeSpan GetFrameDuration(int frameIndex, bool estimateLastFrame = true)
    {
        if ((uint)frameIndex >= (uint)_frameStartTicks.Length)
            throw new ArgumentOutOfRangeException(nameof(frameIndex));

        if (frameIndex + 1 < _frameStartTicks.Length)
            return new TimeSpan(Math.Max(0, _frameStartTicks[frameIndex + 1] - _frameStartTicks[frameIndex]));

        if (!estimateLastFrame || _frameStartTicks.Length < 2)
            return TimeSpan.Zero;

        var prev = _frameStartTicks[^1] - _frameStartTicks[^2];
        return new TimeSpan(Math.Max(0, prev));
    }

    public long GetFrameAudioBytesRequired(int frameIndex, long maxSamples = 0)
    {
        var (_, len) = GetAudioRangeForFrame(frameIndex, maxSamples);
        return (long)len * _blockAlign;
    }

    public int CopyFrameAudioRaw(int frameIndex, byte[] buffer, long maxSamples = 0)
    {
        if (buffer is null)
            throw new ArgumentNullException(nameof(buffer));
        var (start, len) = GetAudioRangeForFrame(frameIndex, maxSamples);
        var required = (long)len * _blockAlign;
        if ((long)buffer.Length < required)
            throw new ArgumentException($"Buffer too small: need {required} bytes, got {buffer.Length}.", nameof(buffer));
        _ = Audio.CopySamplesRaw(start, len, buffer);
        return len;
    }

    public void Dispose()
    {
        /* Child wrappers do not own the shared handle. */
        _shared?.DisposeViaAvClose();
        GC.SuppressFinalize(this);
    }

    private static long GuessDefaultFrameLengthSamples(AvSyncIndex index)
    {
        if (index.FrameCount < 2)
            return 0;

        var deltas = new List<long>(Math.Min(index.FrameCount, 4096));
        var prev = index.GetFrameStartSample(0);
        for (var i = 1; i < index.FrameCount; i++)
        {
            var cur = index.GetFrameStartSample(i);
            var d = cur - prev;
            if (d > 0)
                deltas.Add(d);
            prev = cur;
            if (deltas.Count >= 4096)
                break;
        }

        if (deltas.Count == 0)
            return 0;
        deltas.Sort();
        return deltas[deltas.Count / 2];
    }
}
