namespace LsmasSharp.Native;

internal sealed class SharedLsmasSharpHandle
{
    public nint Handle { get; private set; }

    public SharedLsmasSharpHandle(nint handle)
    {
        Handle = handle;
    }

    public void EnsureNotDisposed()
    {
        if (Handle == nint.Zero)
            throw new ObjectDisposedException(nameof(SharedLsmasSharpHandle));
    }

    public void DisposeViaAvClose()
    {
        if (Handle == nint.Zero)
            return;
        LsmasSharpNativeMethods.lsmas_av_close(Handle);
        Handle = nint.Zero;
    }
}
