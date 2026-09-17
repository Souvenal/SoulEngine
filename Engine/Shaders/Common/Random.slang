implementing Common;

// PCG random generator (32-bit PCG output function from "Hash Functions for
// GPU Rendering") shared by compute presampling and path-tracing sampling.
// One uint of state keeps payload-free transport viable: consumers carry the
// state in registers across a bounce loop.

/// Advance one 32-bit PCG state and return the post-step state.
uint pcgStep(uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

public struct RngState {
    public uint state;
};

/// Seed one generator from a pixel/thread index, a frame counter, and a
/// per-use stream seed. Two warm-up steps decorrelate linear index patterns.
public RngState initRng(uint2 index, uint frameIndex, uint seed) {
    uint state = index.x + index.y * 0x9e3779b9u + frameIndex * 0x85ebca6bu + seed * 0xc2b2ae35u;
    state = pcgStep(state);
    state = pcgStep(state);
    RngState result = { state };
    return result;
}

/// Uniform float in [0, 1).
public float nextFloat1(inout RngState rng) {
    rng.state = pcgStep(rng.state);
    return float(rng.state) * (1.0 / 4294967296.0);
}

public float2 nextFloat2(inout RngState rng) {
    return float2(nextFloat1(rng), nextFloat1(rng));
}

public float3 nextFloat3(inout RngState rng) {
    return float3(nextFloat1(rng), nextFloat1(rng), nextFloat1(rng));
}
