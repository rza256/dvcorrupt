namespace DvCorrupt.Core;

/// <summary>Stateless, deterministic hashing so any frame can be re-rendered identically.</summary>
public static class Rng
{
    public static uint Mix(uint a)
    {
        a ^= a >> 16; a *= 0x7feb352dU;
        a ^= a >> 15; a *= 0x846ca68bU;
        a ^= a >> 16;
        return a;
    }

    public static uint Hash(int seed, long a, int b = 0, int c = 0)
    {
        uint h = Mix((uint)seed ^ 0x9E3779B9U);
        h = Mix(h ^ (uint)a);
        h = Mix(h ^ (uint)(a >> 32) ^ 0x85EBCA6BU);
        h = Mix(h ^ (uint)b);
        h = Mix(h ^ ((uint)c * 0xC2B2AE35U));
        return h;
    }

    /// <summary>Uniform float in [0,1).</summary>
    public static float F(int seed, long a, int b = 0, int c = 0)
        => (Hash(seed, a, b, c) >> 8) * (1f / 16777216f);

    /// <summary>Smooth 1D value noise in [0,1].</summary>
    public static float ValueNoise(int seed, double x, int salt)
    {
        long i = (long)Math.Floor(x);
        float t = (float)(x - i);
        t = t * t * (3 - 2 * t);
        float a = F(seed, i, salt, 77), b = F(seed, i + 1, salt, 77);
        return a + (b - a) * t;
    }
}
