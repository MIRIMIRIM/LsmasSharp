using System.Globalization;
using System.Text;

namespace LsmasSharp;

public static class LwiIndexReader
{
    public static bool TryReadHeader(string lwiPath, out LwiIndexHeader header)
    {
        header = default;
        if (string.IsNullOrWhiteSpace(lwiPath))
            return false;
        if (!File.Exists(lwiPath))
            return false;

        using var fs = new FileStream(lwiPath, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var sr = new StreamReader(fs, Encoding.UTF8, detectEncodingFromByteOrderMarks: true, bufferSize: 4096, leaveOpen: true);

        var line1 = sr.ReadLine();
        var line2 = sr.ReadLine();
        if (line1 is null || line2 is null)
            return false;

        if (!TryParseLwindexVersion(line1, out var lwindexVersion))
            return false;
        if (!TryParseIndexFileVersion(line2, out var indexFileVersion))
            return false;

        header = new LwiIndexHeader(lwindexVersion, indexFileVersion);
        return true;
    }

    private static bool TryParseLwindexVersion(string line, out uint version)
    {
        version = default;
        // Example:
        // <LsmasSharpHWorksIndexVersion=1.2.3.4>
        const string prefix = "<LsmasSharpHWorksIndexVersion=";
        const string suffix = ">";
        if (!line.StartsWith(prefix, StringComparison.Ordinal) || !line.EndsWith(suffix, StringComparison.Ordinal))
            return false;

        var inner = line[prefix.Length..^suffix.Length];
        var parts = inner.Split('.', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (parts.Length != 4)
            return false;

        if (!byte.TryParse(parts[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out var a)) return false;
        if (!byte.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out var b)) return false;
        if (!byte.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture, out var c)) return false;
        if (!byte.TryParse(parts[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out var d)) return false;

        version = (uint)(a << 24 | b << 16 | c << 8 | d);
        return true;
    }

    private static bool TryParseIndexFileVersion(string line, out int version)
    {
        version = default;
        // Example:
        // <LibavReaderIndexFile=16>
        const string prefix = "<LibavReaderIndexFile=";
        const string suffix = ">";
        if (!line.StartsWith(prefix, StringComparison.Ordinal) || !line.EndsWith(suffix, StringComparison.Ordinal))
            return false;

        var inner = line[prefix.Length..^suffix.Length];
        return int.TryParse(inner, NumberStyles.Integer, CultureInfo.InvariantCulture, out version);
    }
}

