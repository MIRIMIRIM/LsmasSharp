using LsmasSharp.Native;
using System.Runtime.InteropServices;

namespace LsmasSharp;

public static class LsmasSharpRuntimeVersions
{
    public const int SupportedApiVersionMajor = 1;
    public const int SupportedApiVersionMinor = 0;
    public const int SupportedApiVersionPatch = 0;
    public const int SupportedApiVersion = (SupportedApiVersionMajor << 16) | (SupportedApiVersionMinor << 8) | SupportedApiVersionPatch;
    public const string SupportedApiVersionString = "1.0.0";

    public static int GetNativeApiVersion() => LsmasSharpNativeMethods.lsmas_get_api_version();

    public static string GetVersionsJson()
    {
        var jsonPtr = LsmasSharpNativeMethods.lsmas_get_versions_json_utf8(out var errPtr);
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
            throw new InvalidOperationException("lsmas_get_versions_json_utf8 returned NULL without error.");

        try
        {
            return Marshal.PtrToStringUTF8(jsonPtr) ?? throw new InvalidOperationException("Versions JSON is NULL.");
        }
        finally
        {
            LsmasSharpNativeMethods.lsmas_free(jsonPtr);
        }
    }
}
