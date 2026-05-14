namespace LsmasSharp.Native;

public readonly record struct LsmasSharpAudioInfo(
    int StreamIndex,
    int SampleRate,
    int Channels,
    ulong ChannelLayout,
    int SampleFormat,
    int BitsPerSample,
    int BytesPerSample,
    int BlockAlign,
    long DecodedSamples,
    long DelaySamples,
    long TotalSamples
);

