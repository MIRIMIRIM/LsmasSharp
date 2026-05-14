namespace LsmasSharp;

public sealed record VideoSourceOptions
{
    public int StreamIndex { get; init; } = -1;
    public int Threads { get; init; } = 0;
    public SeekMode SeekMode { get; init; } = SeekMode.Normal;
    public int SeekThreshold { get; init; } = 10;

    public bool DirectRendering { get; init; } = false;

    public int FpsNum { get; init; } = 0;
    public int FpsDen { get; init; } = 1;

    public bool VariableInfo { get; init; } = false;
    public string OutputFormat { get; init; } = "";

    public string Decoder { get; init; } = "";
    public HardwareDecoderPreference PreferHw { get; init; } = HardwareDecoderPreference.None;

    public int FfLogLevel { get; init; } = 0;

    public bool CacheIndex { get; init; } = true;
    public string? CacheFile { get; init; } = null;
    public string? CacheDir { get; init; } = null;

    public bool SoftResetOnSeek { get; init; } = true;

    public bool FrameList { get; init; } = false;

    public bool ApplyRepeat { get; init; } = true;
    public FieldDominance Dominance { get; init; } = FieldDominance.ObeySource;
}
