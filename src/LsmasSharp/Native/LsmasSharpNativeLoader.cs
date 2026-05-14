using System.Reflection;
using System.Runtime.InteropServices;

namespace LsmasSharp.Native;

public static class LsmasSharpNativeLoader
{
    private static int _installed;
    private static string? _nativeDir;

    public static void Install(string nativeDirectory)
    {
        if (string.IsNullOrWhiteSpace(nativeDirectory))
            throw new ArgumentException("nativeDirectory is null/empty.", nameof(nativeDirectory));

        nativeDirectory = Path.GetFullPath(nativeDirectory);
        _nativeDir = nativeDirectory;

        if (Interlocked.Exchange(ref _installed, 1) != 0)
            return;

        NativeLibrary.SetDllImportResolver(typeof(LsmasSharpNativeLoader).Assembly, Resolve);
    }

    private static nint Resolve(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        _ = assembly;
        _ = searchPath;

        if (string.IsNullOrEmpty(_nativeDir))
            return nint.Zero;

        // Allow default resolution for non-lsmas libraries.
        if (!string.Equals(libraryName, LsmasSharpNativeMethods.LibraryName, StringComparison.OrdinalIgnoreCase))
            return nint.Zero;

        TryPreloadDependencies(_nativeDir);

        var candidates = GetPlatformLibraryCandidates(libraryName);
        foreach (var candidate in candidates)
        {
            var fullPath = Path.Combine(_nativeDir, candidate);
            if (!File.Exists(fullPath))
                continue;
            if (NativeLibrary.TryLoad(fullPath, out var handle))
                return handle;
        }

        return nint.Zero;
    }

    private static void TryPreloadDependencies(string nativeDir)
    {
        foreach (var dep in GetFfmpegDependencyFileNames())
            TryLoadIfExists(nativeDir, dep);
    }

    private static void TryLoadIfExists(string nativeDir, string fileName)
    {
        var fullPath = Path.Combine(nativeDir, fileName);
        if (!File.Exists(fullPath))
            return;
        NativeLibrary.TryLoad(fullPath, out _);
    }

    private static IEnumerable<string> GetPlatformLibraryCandidates(string baseName)
    {
        if (OperatingSystem.IsWindows())
            return [baseName + ".dll", baseName];

        if (OperatingSystem.IsMacOS())
            return ["lib" + baseName + ".dylib", baseName, "lib" + baseName];

        // Linux/others
        return ["lib" + baseName + ".so", baseName, "lib" + baseName];
    }

    private static IEnumerable<string> GetFfmpegDependencyFileNames()
    {
        // Try-load a superset of common FFmpeg shared library names across versions.
        // This is a best-effort helper; missing files are ignored.
        if (OperatingSystem.IsWindows())
            return [
                // 4.x era
                "avcodec-58.dll", "avformat-58.dll", "avutil-56.dll", "swscale-5.dll", "swresample-3.dll",
                // 6.x era
                "avcodec-60.dll", "avformat-60.dll", "avutil-58.dll", "swscale-7.dll", "swresample-4.dll",
                // 7.x/8.x era
                "avcodec-61.dll", "avformat-61.dll", "avutil-59.dll", "swscale-8.dll", "swresample-5.dll",
                "avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swscale-9.dll", "swresample-6.dll",
                // Unversioned (some custom builds)
                "avcodec.dll", "avformat.dll", "avutil.dll", "swscale.dll", "swresample.dll"
            ];

        if (OperatingSystem.IsMacOS())
            return [
                "libavcodec.58.dylib", "libavformat.58.dylib", "libavutil.56.dylib", "libswscale.5.dylib", "libswresample.3.dylib",
                "libavcodec.60.dylib", "libavformat.60.dylib", "libavutil.58.dylib", "libswscale.7.dylib", "libswresample.4.dylib",
                "libavcodec.62.dylib", "libavformat.62.dylib", "libavutil.60.dylib", "libswscale.9.dylib", "libswresample.6.dylib",
                "libavcodec.dylib", "libavformat.dylib", "libavutil.dylib", "libswscale.dylib", "libswresample.dylib"
            ];

        return [
            "libavcodec.so.58", "libavformat.so.58", "libavutil.so.56", "libswscale.so.5", "libswresample.so.3",
            "libavcodec.so.60", "libavformat.so.60", "libavutil.so.58", "libswscale.so.7", "libswresample.so.4",
            "libavcodec.so.62", "libavformat.so.62", "libavutil.so.60", "libswscale.so.9", "libswresample.so.6",
            "libavcodec.so", "libavformat.so", "libavutil.so", "libswscale.so", "libswresample.so"
        ];
    }

    // L-SMASH (LibavSMASHSource) is intentionally not preloaded in the LWLibav-only phase.
}
