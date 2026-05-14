namespace LsmasSharp;

public sealed class AvSyncIndex
{
    private readonly long[] _frameStartSamples;

    public int SampleRate { get; }
    public int FrameCount => _frameStartSamples.Length;

    public AvSyncIndex(int sampleRate, long[] frameStartSamples)
    {
        if (sampleRate <= 0)
            throw new ArgumentOutOfRangeException(nameof(sampleRate));
        SampleRate = sampleRate;
        _frameStartSamples = frameStartSamples ?? throw new ArgumentNullException(nameof(frameStartSamples));
    }

    public long GetFrameStartSample(int frameIndex)
    {
        if ((uint)frameIndex >= (uint)_frameStartSamples.Length)
            throw new ArgumentOutOfRangeException(nameof(frameIndex));
        return _frameStartSamples[frameIndex];
    }

    public (long Start, int Length) GetFrameAudioRange(int frameIndex, long defaultFrameLengthSamples = 0)
    {
        if ((uint)frameIndex >= (uint)_frameStartSamples.Length)
            throw new ArgumentOutOfRangeException(nameof(frameIndex));

        var start = _frameStartSamples[frameIndex];
        long end;
        if (frameIndex + 1 < _frameStartSamples.Length)
        {
            end = _frameStartSamples[frameIndex + 1];
        }
        else
        {
            end = defaultFrameLengthSamples > 0 ? start + defaultFrameLengthSamples : start;
        }

        if (end < start)
            end = start;
        var length = end - start;
        if (length > int.MaxValue)
            length = int.MaxValue;
        return (start, (int)length);
    }

    public int FindFrameAtOrBeforeSample(long sampleIndex, bool clamp = true)
    {
        if (_frameStartSamples.Length == 0)
            return clamp ? 0 : -1;

        if (sampleIndex < _frameStartSamples[0])
            return clamp ? 0 : -1;

        var lastIndex = _frameStartSamples.Length - 1;
        if (sampleIndex >= _frameStartSamples[lastIndex])
            return lastIndex;

        var lo = 0;
        var hi = lastIndex;
        while (lo + 1 < hi)
        {
            var mid = lo + ((hi - lo) / 2);
            var v = _frameStartSamples[mid];
            if (v <= sampleIndex)
                lo = mid;
            else
                hi = mid;
        }
        return lo;
    }
}

public static class AvSync
{
    public static AvSyncIndex BuildIndex(TimeBase videoTimeBase, ReadOnlySpan<long> videoPtsList, int videoFpsNum, int videoFpsDen, int audioSampleRate)
    {
        if (!videoTimeBase.IsValid)
            throw new ArgumentException("Invalid time base (Den=0).", nameof(videoTimeBase));
        if (audioSampleRate <= 0)
            throw new ArgumentOutOfRangeException(nameof(audioSampleRate));

        var normalizedPts = TimecodesV2.BuildNormalizedPts(videoTimeBase, videoPtsList, videoFpsNum, videoFpsDen);
        var frameStartSamples = new long[normalizedPts.Length];
        for (var i = 0; i < normalizedPts.Length; i++)
        {
            frameStartSamples[i] = PtsToSamples(normalizedPts[i], videoTimeBase, audioSampleRate);
        }

        return new AvSyncIndex(audioSampleRate, frameStartSamples);
    }

    private static long PtsToSamples(long pts, TimeBase timeBase, int sampleRate)
    {
        Int128 numer = (Int128)pts * timeBase.Num * sampleRate;
        Int128 den = (Int128)timeBase.Den;
        if (den == 0)
            return 0;
        if (den < 0)
        {
            den = -den;
            numer = -numer;
        }

        Int128 q;
        if (numer >= 0)
            q = (numer + den / 2) / den;
        else
            q = -((-numer + den / 2) / den);

        if (q < long.MinValue)
            return long.MinValue;
        if (q > long.MaxValue)
            return long.MaxValue;
        return (long)q;
    }
}
