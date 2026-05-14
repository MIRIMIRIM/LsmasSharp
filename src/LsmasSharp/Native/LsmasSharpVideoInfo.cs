namespace LsmasSharp.Native;

public readonly record struct LsmasSharpVideoInfo(
    int Width,
    int Height,
    int NumFrames,
    int FpsNum,
    int FpsDen
);

