namespace LsmasSharp;

public readonly record struct Rational(int Num, int Den)
{
    public static readonly Rational One = new(1, 1);

    public override string ToString() => $"{Num}/{Den}";
}

