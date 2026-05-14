namespace LsmasSharp;

public static class TimecodesV2
{
    public static bool TryBuildNormalizedTicksStrict(TimeBase timeBase, ReadOnlySpan<long> ptsList, out long[] ticks, out string? error, bool normalizeToZero = true)
    {
        ticks = [];
        error = null;

        if (!timeBase.IsValid)
        {
            error = "Invalid time base (Den=0).";
            return false;
        }
        if (ptsList.IsEmpty)
        {
            ticks = [];
            return true;
        }

        long? firstKnown = null;
        for (var i = 0; i < ptsList.Length; i++)
        {
            if (ptsList[i] != long.MinValue)
            {
                firstKnown = ptsList[i];
                break;
            }
        }
        if (firstKnown is null)
        {
            error = "PTS list has no known timestamps.";
            return false;
        }

        for (var i = 0; i < ptsList.Length; i++)
        {
            if (ptsList[i] == long.MinValue)
            {
                error = $"PTS missing at frame {i}.";
                return false;
            }
        }

        for (var i = 1; i < ptsList.Length; i++)
        {
            if (ptsList[i] <= ptsList[i - 1])
            {
                error = $"PTS is not strictly increasing at frame {i} (prev={ptsList[i - 1]}, cur={ptsList[i]}).";
                return false;
            }
        }

        var basePts = normalizeToZero ? firstKnown.Value : 0;
        ticks = new long[ptsList.Length];
        for (var i = 0; i < ptsList.Length; i++)
        {
            var diffPts = (Int128)ptsList[i] - basePts;
            var numer = diffPts * timeBase.Num * TimeSpan.TicksPerSecond;
            var den = (Int128)timeBase.Den;
            ticks[i] = (long)DivRoundNearest(numer, den);
        }
        return true;
    }

    public static long[] BuildNormalizedPts(TimeBase timeBase, ReadOnlySpan<long> ptsList, int fpsNum = 0, int fpsDen = 1)
    {
        if (!timeBase.IsValid)
            throw new ArgumentException("Invalid time base (Den=0).", nameof(timeBase));
        if (ptsList.IsEmpty)
            return [];

        var (filled, basePts) = FillMonotonicPts(timeBase, ptsList, fpsNum, fpsDen);

        var normalized = new long[filled.Length];
        for (var i = 0; i < filled.Length; i++)
            normalized[i] = filled[i] - basePts;
        return normalized;
    }

    public static long[] BuildNormalizedTicks(TimeBase timeBase, ReadOnlySpan<long> ptsList, int fpsNum = 0, int fpsDen = 1)
    {
        if (!timeBase.IsValid)
            throw new ArgumentException("Invalid time base (Den=0).", nameof(timeBase));
        if (ptsList.IsEmpty)
            return [];

        var (filled, basePts) = FillMonotonicPts(timeBase, ptsList, fpsNum, fpsDen);

        var ticks = new long[filled.Length];
        for (var i = 0; i < filled.Length; i++)
        {
            var diffPts = (Int128)filled[i] - basePts;
            var numer = diffPts * timeBase.Num * TimeSpan.TicksPerSecond;
            var den = (Int128)timeBase.Den;
            ticks[i] = (long)DivRoundNearest(numer, den);
        }
        return ticks;
    }

    public static string[] BuildLines(TimeBase timeBase, ReadOnlySpan<long> ptsList, int fpsNum = 0, int fpsDen = 1, int fracDigits = 6)
    {
        if (!timeBase.IsValid)
            throw new ArgumentException("Invalid time base (Den=0).", nameof(timeBase));
        if (ptsList.IsEmpty)
            return ["# timecode format v2"];
        if (fracDigits < 0 || fracDigits > 9)
            throw new ArgumentOutOfRangeException(nameof(fracDigits));

        var (filled, basePts) = FillMonotonicPts(timeBase, ptsList, fpsNum, fpsDen);

        var lines = new string[filled.Length + 1];
        lines[0] = "# timecode format v2";
        for (var i = 0; i < filled.Length; i++)
        {
            var diffPts = (Int128)filled[i] - basePts;
            var msNumer = diffPts * timeBase.Num * 1000;
            var msDen = (Int128)timeBase.Den;
            lines[i + 1] = FormatRationalDecimal(msNumer, msDen, fracDigits);
        }

        return lines;
    }

    public static void Write(string path, TimeBase timeBase, ReadOnlySpan<long> ptsList, int fpsNum = 0, int fpsDen = 1, int fracDigits = 6)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("Path is null/empty.", nameof(path));

        var fullPath = Path.GetFullPath(path);
        var dir = Path.GetDirectoryName(fullPath);
        if (!string.IsNullOrEmpty(dir))
            Directory.CreateDirectory(dir);

        File.WriteAllLines(fullPath, BuildLines(timeBase, ptsList, fpsNum, fpsDen, fracDigits));
    }

    private static (long[] FilledPts, long BasePts) FillMonotonicPts(TimeBase timeBase, ReadOnlySpan<long> ptsList, int fpsNum, int fpsDen)
    {
        long? firstKnown = null;
        for (var i = 0; i < ptsList.Length; i++)
        {
            var p = ptsList[i];
            if (p != long.MinValue)
            {
                firstKnown = p;
                break;
            }
        }
        var basePts = firstKnown ?? 0;

        var defaultDelta = GuessDefaultDeltaPts(ptsList, timeBase, fpsNum, fpsDen);

        var filled = new long[ptsList.Length];
        long prev = basePts;
        for (var i = 0; i < ptsList.Length; i++)
        {
            var cur = ptsList[i];
            if (cur == long.MinValue)
                cur = prev + defaultDelta;
            if (i > 0 && cur <= prev)
                cur = prev + 1;
            filled[i] = cur;
            prev = cur;
        }

        return (filled, basePts);
    }

    private static long GuessDefaultDeltaPts(ReadOnlySpan<long> ptsList, TimeBase timeBase, int fpsNum, int fpsDen)
    {
        var deltas = new List<long>(Math.Min(ptsList.Length, 4096));
        for (var i = 1; i < ptsList.Length; i++)
        {
            var a = ptsList[i - 1];
            var b = ptsList[i];
            if (a == long.MinValue || b == long.MinValue)
                continue;
            var delta = b - a;
            if (delta > 0)
                deltas.Add(delta);
        }

        if (deltas.Count > 0)
        {
            deltas.Sort();
            return deltas[deltas.Count / 2];
        }

        if (fpsNum > 0 && fpsDen > 0)
        {
            Int128 numer = (Int128)timeBase.Den * fpsDen;
            Int128 denom = (Int128)timeBase.Num * fpsNum;
            if (denom > 0)
            {
                var q = (numer + denom / 2) / denom;
                if (q > 0 && q <= long.MaxValue)
                    return (long)q;
            }
        }

        return 1;
    }

    private static Int128 DivRoundNearest(Int128 numer, Int128 den)
    {
        if (den == 0)
            return 0;
        if (den < 0)
        {
            den = -den;
            numer = -numer;
        }

        if (numer >= 0)
            return (numer + den / 2) / den;
        return -((-numer + den / 2) / den);
    }

    private static Int128 Pow10(int digits)
    {
        var v = (Int128)1;
        for (var i = 0; i < digits; i++)
            v *= 10;
        return v;
    }

    private static string FormatRationalDecimal(Int128 numer, Int128 den, int fracDigits)
    {
        if (den == 0)
            return "0";
        if (numer == 0)
            return "0";

        var sign = "";
        if (numer < 0)
        {
            sign = "-";
            numer = -numer;
        }
        if (den < 0)
            den = -den;

        var ip = numer / den;
        var rem = numer % den;
        if (fracDigits <= 0 || rem == 0)
            return sign + ip.ToString();

        var pow10 = Pow10(fracDigits);
        var frac = (rem * pow10 + den / 2) / den;
        if (frac >= pow10)
        {
            ip += 1;
            frac -= pow10;
        }

        var fracStr = frac.ToString().PadLeft(fracDigits, '0');
        return $"{sign}{ip}.{fracStr}";
    }
}
