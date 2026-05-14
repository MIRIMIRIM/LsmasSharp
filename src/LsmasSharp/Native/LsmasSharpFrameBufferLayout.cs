namespace LsmasSharp.Native;

public readonly record struct LsmasSharpFramePlaneLayout(
    int Width,
    int Height,
    int Stride,
    long Offset);

public sealed class LsmasSharpFrameBufferLayout
{
    private readonly LsmasSharpFramePlaneLayout[] _planes;

    internal LsmasSharpFrameBufferLayout(LsmasSharpNativeMethods.VideoFrameBufferLayout native)
    {
        OutputFormat = (LsmasSharpFrameOutputFormat)native.output_format;
        Width = native.width;
        Height = native.height;
        PixelFormat = native.pix_fmt;
        RequiredBytes = native.required_bytes;

        var planeCount = Math.Clamp(native.plane_count, 0, 4);
        _planes = new LsmasSharpFramePlaneLayout[planeCount];
        for (var i = 0; i < planeCount; i++)
        {
            _planes[i] = new LsmasSharpFramePlaneLayout(
                GetPlaneWidth(native, i),
                GetPlaneHeight(native, i),
                GetPlaneStride(native, i),
                GetPlaneOffset(native, i));
        }
    }

    public LsmasSharpFrameOutputFormat OutputFormat { get; }
    public int Width { get; }
    public int Height { get; }
    public int PixelFormat { get; }
    public int PlaneCount => _planes.Length;
    public long RequiredBytes { get; }
    public IReadOnlyList<LsmasSharpFramePlaneLayout> Planes => _planes;

    private static int GetPlaneWidth(LsmasSharpNativeMethods.VideoFrameBufferLayout layout, int index) => index switch
    {
        0 => layout.plane_width0,
        1 => layout.plane_width1,
        2 => layout.plane_width2,
        3 => layout.plane_width3,
        _ => throw new ArgumentOutOfRangeException(nameof(index)),
    };

    private static int GetPlaneHeight(LsmasSharpNativeMethods.VideoFrameBufferLayout layout, int index) => index switch
    {
        0 => layout.plane_height0,
        1 => layout.plane_height1,
        2 => layout.plane_height2,
        3 => layout.plane_height3,
        _ => throw new ArgumentOutOfRangeException(nameof(index)),
    };

    private static int GetPlaneStride(LsmasSharpNativeMethods.VideoFrameBufferLayout layout, int index) => index switch
    {
        0 => layout.plane_stride0,
        1 => layout.plane_stride1,
        2 => layout.plane_stride2,
        3 => layout.plane_stride3,
        _ => throw new ArgumentOutOfRangeException(nameof(index)),
    };

    private static long GetPlaneOffset(LsmasSharpNativeMethods.VideoFrameBufferLayout layout, int index) => index switch
    {
        0 => layout.plane_offset0,
        1 => layout.plane_offset1,
        2 => layout.plane_offset2,
        3 => layout.plane_offset3,
        _ => throw new ArgumentOutOfRangeException(nameof(index)),
    };
}
