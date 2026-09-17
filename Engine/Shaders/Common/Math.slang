implementing Common;

// Shared math constants. Keep target-neutral; Slang folds these at compile time.
public static const float kPI = radians(180.0);

// Float range limits for encoding-aware clamping.
public static const float kFloat16Max = 65504.0;
public static const float kFloat16Min = -65504.0;
public static const float kFloat32Max = 3.402823466e+38;
public static const float kFloat32Min = -3.402823466e+38;
