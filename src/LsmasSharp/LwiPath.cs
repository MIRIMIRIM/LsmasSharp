using System.Text;

namespace LsmasSharp;

public static class LwiPath
{
    private const int MaxFilenameBytes = 254;
    private const string Suffix = ".lwi";

    /// <summary>
    /// Replicates <c>common/lwindex.c:create_lwi_path</c> behavior for computing the index file path.
    /// </summary>
    public static string Create(string mediaPath, string? cacheDir)
    {
        if (string.IsNullOrEmpty(mediaPath))
            throw new ArgumentException("mediaPath is null/empty.", nameof(mediaPath));

        if (string.IsNullOrEmpty(cacheDir))
            return mediaPath + Suffix;

        var dir = cacheDir;

        string realPath;
        try
        {
            realPath = Path.GetFullPath(mediaPath);
        }
        catch
        {
            realPath = mediaPath;
        }

        var maxElemSize = MaxFilenameBytes - Encoding.ASCII.GetByteCount(Suffix);
        var trimmed = TrimUtf8FromFront(realPath, maxElemSize);

        var fileName = trimmed
            .Replace('/', '_')
            .Replace('\\', '_')
            .Replace(':', '_')
            + Suffix;

        return Path.Combine(dir, fileName);
    }

    /// <summary>
    /// Creates a variant index path by inserting a tag before <c>.lwi</c>.
    /// Useful for caching per (video/audio) stream selection.
    /// </summary>
    public static string CreateVariant(string mediaPath, string? cacheDir, string? variantTag)
    {
        var basePath = Create(mediaPath, cacheDir);
        if (string.IsNullOrWhiteSpace(variantTag))
            return basePath;

        var tag = SanitizeTag(variantTag);
        if (tag.Length == 0)
            return basePath;

        if (!basePath.EndsWith(Suffix, StringComparison.OrdinalIgnoreCase))
            return basePath + "." + tag + Suffix;

        return basePath[..^Suffix.Length] + "." + tag + Suffix;
    }

    public static string CreatePerStream(string mediaPath, string? cacheDir, int videoStreamIndex, int audioStreamIndex)
    {
        static string FormatIndex(int v) => v switch
        {
            -1 => "auto",
            _ => v.ToString()
        };

        var tag = $"v{FormatIndex(videoStreamIndex)}_a{FormatIndex(audioStreamIndex)}";
        return CreateVariant(mediaPath, cacheDir, tag);
    }

    private static string TrimUtf8FromFront(string value, int maxBytes)
    {
        if (maxBytes <= 0)
            return "";

        var bytes = Encoding.UTF8.GetBytes(value);
        if (bytes.Length <= maxBytes)
            return value;

        var offset = 0;
        var remaining = bytes.Length;
        while (remaining > maxBytes && offset < bytes.Length)
        {
            var first = bytes[offset];
            var seqLen = Utf8SequenceLength(first);
            if (seqLen <= 0)
                break;
            if (offset + seqLen > bytes.Length)
                break;
            offset += seqLen;
            remaining -= seqLen;
        }

        return Encoding.UTF8.GetString(bytes, offset, bytes.Length - offset);
    }

    private static int Utf8SequenceLength(byte first)
    {
        if ((first & 0b1000_0000) == 0)
            return 1;
        if ((first & 0b1110_0000) == 0b1100_0000)
            return 2;
        if ((first & 0b1111_0000) == 0b1110_0000)
            return 3;
        if ((first & 0b1111_1000) == 0b1111_0000)
            return 4;
        return -1;
    }

    private static string SanitizeTag(string value)
    {
        Span<char> buf = stackalloc char[Math.Min(value.Length, 64)];
        var n = 0;
        foreach (var ch in value)
        {
            if (n >= buf.Length)
                break;
            if (char.IsLetterOrDigit(ch) || ch is '_' or '-' or '.')
                buf[n++] = ch;
            else
                buf[n++] = '_';
        }
        return new string(buf[..n]).Trim('_');
    }
}
