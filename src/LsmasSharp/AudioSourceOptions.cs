namespace LsmasSharp;

public sealed record AudioSourceOptions
{
    public int StreamIndex { get; init; } = -1;
    public int Threads { get; init; } = 0;
    public bool AvSync { get; init; } = false;

    public int FfLogLevel { get; init; } = 0;
    public string Decoder { get; init; } = "";

    public bool CacheIndex { get; init; } = true;
    public string? CacheFile { get; init; } = null;
    public string? CacheDir { get; init; } = null;

    public ulong ChannelLayout { get; init; } = 0;
    public int SampleRate { get; init; } = 0;
    public AudioSampleFormat SampleFormat { get; init; } = AudioSampleFormat.Float32;
}

