using System.Runtime.InteropServices;

namespace LsmasSharp.Native;

public readonly record struct LsmasSharpVideoFramePlane(nint Data, int Stride, int Width, int Height);

public sealed class LsmasSharpVideoFrame : IDisposable
{
    private nint _handle;
    private LsmasSharpVideoFrameProperties? _properties;

    internal LsmasSharpVideoFrame(nint handle)
    {
        _handle = handle;
    }

    public nint AvFrame
    {
        get
        {
            EnsureNotDisposed();
            return LsmasSharpNativeMethods.lsmas_video_frame_get_avframe(_handle);
        }
    }

    public string? PixelFormatName
    {
        get
        {
            EnsureNotDisposed();
            var ptr = LsmasSharpNativeMethods.lsmas_video_frame_get_pix_fmt_name(_handle);
            return ptr == nint.Zero ? null : Marshal.PtrToStringUTF8(ptr);
        }
    }

    public LsmasSharpVideoFrameProperties Properties
    {
        get
        {
            EnsureNotDisposed();
            if (_properties is { } props)
                return props;

            var ret = LsmasSharpNativeMethods.lsmas_video_frame_get_props(_handle, out var native, out var errPtr);
            LsmasSharpVideoSource.ThrowIfNativeError(errPtr);
            if (ret != 0)
                throw new InvalidOperationException($"lsmas_video_frame_get_props failed: {ret}.");

            props = new LsmasSharpVideoFrameProperties(native);
            _properties = props;
            return props;
        }
    }

    public LsmasSharpVideoFramePlane GetPlane(int planeIndex)
    {
        EnsureNotDisposed();
        var ret = LsmasSharpNativeMethods.lsmas_video_frame_get_plane(
            _handle,
            planeIndex,
            out var data,
            out var stride,
            out var width,
            out var height,
            out var errPtr);
        LsmasSharpVideoSource.ThrowIfNativeError(errPtr);
        if (ret != 0)
            throw new InvalidOperationException($"lsmas_video_frame_get_plane failed: {ret}.");
        return new LsmasSharpVideoFramePlane(data, stride, width, height);
    }

    public void Dispose()
    {
        var h = _handle;
        if (h == nint.Zero)
            return;
        _handle = nint.Zero;
        LsmasSharpNativeMethods.lsmas_video_release_frame(h);
    }

    private void EnsureNotDisposed()
    {
        if (_handle == nint.Zero)
            throw new ObjectDisposedException(nameof(LsmasSharpVideoFrame));
    }
}
