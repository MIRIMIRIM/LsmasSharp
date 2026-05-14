namespace LsmasSharp;

public readonly record struct TimeBase(int Num, int Den)
{
    public bool IsValid => Den != 0;

    public bool TryToTicks(long pts, out long ticks)
    {
        ticks = 0;
        if (Den == 0)
            return false;
        if (pts == long.MinValue)
            return false;

        try
        {
            Int128 numer = (Int128)pts * Num * TimeSpan.TicksPerSecond;
            Int128 den = Den;
            if (den < 0)
            {
                den = -den;
                numer = -numer;
            }

            var q = numer / den;
            if (q < long.MinValue || q > long.MaxValue)
                return false;

            ticks = (long)q;
            return true;
        }
        catch (OverflowException)
        {
            return false;
        }
    }

    public bool TryToTimeSpan(long pts, out TimeSpan timeSpan)
    {
        timeSpan = default;
        if (!TryToTicks(pts, out var ticks))
            return false;
        timeSpan = new TimeSpan(ticks);
        return true;
    }
}
