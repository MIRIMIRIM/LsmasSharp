namespace LsmasSharp;

public readonly record struct Rational32(int Num, int Den);

public sealed record MasteringDisplayMetadata(
    bool HasPrimaries,
    bool HasLuminance,
    Rational32 PrimaryRX,
    Rational32 PrimaryRY,
    Rational32 PrimaryGX,
    Rational32 PrimaryGY,
    Rational32 PrimaryBX,
    Rational32 PrimaryBY,
    Rational32 WhiteX,
    Rational32 WhiteY,
    Rational32 MaxLuminance,
    Rational32 MinLuminance
);

public sealed record ContentLightMetadata(int MaxCll, int MaxFall);

public sealed record DoviConfigurationRecord(
    int VersionMajor,
    int VersionMinor,
    int Profile,
    int Level,
    bool RpuPresent,
    bool ElPresent,
    bool BlPresent,
    int BlSignalCompatibilityId
);

