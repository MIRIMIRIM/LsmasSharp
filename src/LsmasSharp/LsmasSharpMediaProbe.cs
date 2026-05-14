using LsmasSharp.Native;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Runtime.InteropServices;

namespace LsmasSharp;

public static class LsmasSharpMediaProbe
{
    public sealed record Rational(
        [property: JsonPropertyName("num")] int Num,
        [property: JsonPropertyName("den")] int Den
    );

    public sealed record StreamInfo(
        [property: JsonPropertyName("index")] int Index,
        [property: JsonPropertyName("type")] string Type,
        [property: JsonPropertyName("codec")] string Codec,
        [property: JsonPropertyName("default")] bool Default,
        [property: JsonPropertyName("language")] string? Language,
        [property: JsonPropertyName("time_base")] Rational? TimeBase,
        [property: JsonPropertyName("start_time")] long? StartTime,
        [property: JsonPropertyName("duration")] long? Duration,
        [property: JsonPropertyName("width")] int? Width,
        [property: JsonPropertyName("height")] int? Height,
        [property: JsonPropertyName("sar")] Rational? Sar,
        [property: JsonPropertyName("avg_frame_rate")] Rational? AvgFrameRate,
        [property: JsonPropertyName("sample_rate")] int? SampleRate,
        [property: JsonPropertyName("channels")] int? Channels,
        [property: JsonPropertyName("channel_layout")] ulong? ChannelLayout
    );

    public sealed record MediaInfo([property: JsonPropertyName("streams")] IReadOnlyList<StreamInfo> Streams);

    public static MediaInfo Probe(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("path is null/empty.", nameof(path));

        var jsonPtr = LsmasSharpNativeMethods.lsmas_probe_streams_json_utf8(path, out var errPtr);
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
        if (jsonPtr == nint.Zero)
            throw new InvalidOperationException("lsmas_probe_streams_json_utf8 returned NULL without error.");

        try
        {
            var json = Marshal.PtrToStringUTF8(jsonPtr) ?? throw new InvalidOperationException("Probe JSON is NULL.");
            return JsonSerializer.Deserialize(json, LsmasSharpMediaProbeJsonContext.Default.MediaInfo)
                ?? throw new InvalidOperationException("Failed to parse probe JSON.");
        }
        finally
        {
            LsmasSharpNativeMethods.lsmas_free(jsonPtr);
        }
    }
}

[JsonSourceGenerationOptions(
    PropertyNameCaseInsensitive = true,
    NumberHandling = JsonNumberHandling.AllowReadingFromString
)]
[JsonSerializable(typeof(LsmasSharpMediaProbe.MediaInfo))]
internal sealed partial class LsmasSharpMediaProbeJsonContext : JsonSerializerContext
{
}
