namespace LsmasSharp;

public sealed record VideoProperties(
    int SarNum,
    int SarDen,
    int ColorRange,
    int ColorSpace,
    int ColorPrimaries,
    int ColorTransfer,
    int ChromaLocation,
    int FieldOrder,
    int InterlacedFrame,
    int TopFieldFirst
);

