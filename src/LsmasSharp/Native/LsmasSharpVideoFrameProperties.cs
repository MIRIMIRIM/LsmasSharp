namespace LsmasSharp.Native;

public sealed class LsmasSharpVideoFrameProperties
{
    internal LsmasSharpVideoFrameProperties(LsmasSharpNativeMethods.VideoFrameProps native)
    {
        Width = native.width;
        Height = native.height;
        PixelFormat = native.pix_fmt;
        PlaneCount = native.plane_count;

        SarNum = native.sar_num;
        SarDen = native.sar_den;

        ColorRange = native.color_range;
        ColorSpace = native.colorspace;
        ColorPrimaries = native.color_primaries;
        ColorTransfer = native.color_trc;
        ChromaLocation = native.chroma_location;
        FieldOrder = native.field_order;

        InterlacedFrame = native.interlaced_frame;
        TopFieldFirst = native.top_field_first;
        RepeatPict = native.repeat_pict;

        CropLeft = native.crop_left;
        CropTop = native.crop_top;
        CropRight = native.crop_right;
        CropBottom = native.crop_bottom;

        DisplayRotationDegrees = native.display_rotation_degrees;
        DisplayHFlip = native.display_hflip != 0;
        DisplayVFlip = native.display_vflip != 0;

        HasMasteringDisplayMetadata = native.has_mastering_display_metadata != 0;
        HasContentLightMetadata = native.has_content_light_metadata != 0;
        HasDynamicHdrPlus = native.has_dynamic_hdr_plus != 0;
        HasDoviMetadata = native.has_dovi_metadata != 0;
        HasDoviRpu = native.has_dovi_rpu != 0;
        HasFilmGrainParams = native.has_film_grain_params != 0;
        HasDisplayMatrix = native.has_displaymatrix != 0;
    }

    public int Width { get; }
    public int Height { get; }
    public int PixelFormat { get; }
    public int PlaneCount { get; }

    public int SarNum { get; }
    public int SarDen { get; }

    public int ColorRange { get; }
    public int ColorSpace { get; }
    public int ColorPrimaries { get; }
    public int ColorTransfer { get; }
    public int ChromaLocation { get; }
    public int FieldOrder { get; }

    public int InterlacedFrame { get; }
    public int TopFieldFirst { get; }
    public int RepeatPict { get; }

    public int CropLeft { get; }
    public int CropTop { get; }
    public int CropRight { get; }
    public int CropBottom { get; }

    public int DisplayRotationDegrees { get; }
    public bool DisplayHFlip { get; }
    public bool DisplayVFlip { get; }

    public bool HasMasteringDisplayMetadata { get; }
    public bool HasContentLightMetadata { get; }
    public bool HasDynamicHdrPlus { get; }
    public bool HasDoviMetadata { get; }
    public bool HasDoviRpu { get; }
    public bool HasFilmGrainParams { get; }
    public bool HasDisplayMatrix { get; }
}
