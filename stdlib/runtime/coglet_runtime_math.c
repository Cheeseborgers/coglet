#include <math.h>

/*
 * Coglet runtime ABI, v0.
 *
 * These symbols are implementation details used by runtime-facing standard
 * modules. User code should import std.io/std.math/std.mem rather than declaring them directly.
 * The ABI intentionally uses only ISO C scalar/pointer types so the same source
 * compiles for Linux and Windows on both x86-64 and AArch64 native toolchains.
 */

/* Floating-point math runtime component. */

float coglet_rt_math_sqrt_f32(float value)
{
    return sqrtf(value);
}

double coglet_rt_math_sqrt_f64(double value)
{
    return sqrt(value);
}

float coglet_rt_math_sin_f32(float value)
{
    return sinf(value);
}

double coglet_rt_math_sin_f64(double value)
{
    return sin(value);
}

float coglet_rt_math_cos_f32(float value)
{
    return cosf(value);
}

double coglet_rt_math_cos_f64(double value)
{
    return cos(value);
}

float coglet_rt_math_tan_f32(float value)
{
    return tanf(value);
}

double coglet_rt_math_tan_f64(double value)
{
    return tan(value);
}

float coglet_rt_math_asin_f32(float value)
{
    return asinf(value);
}

double coglet_rt_math_asin_f64(double value)
{
    return asin(value);
}

float coglet_rt_math_acos_f32(float value)
{
    return acosf(value);
}

double coglet_rt_math_acos_f64(double value)
{
    return acos(value);
}

float coglet_rt_math_atan_f32(float value)
{
    return atanf(value);
}

double coglet_rt_math_atan_f64(double value)
{
    return atan(value);
}

float coglet_rt_math_atan2_f32(float y, float x)
{
    return atan2f(y, x);
}

double coglet_rt_math_atan2_f64(double y, double x)
{
    return atan2(y, x);
}

float coglet_rt_math_floor_f32(float value)
{
    return floorf(value);
}

double coglet_rt_math_floor_f64(double value)
{
    return floor(value);
}

float coglet_rt_math_ceil_f32(float value)
{
    return ceilf(value);
}

double coglet_rt_math_ceil_f64(double value)
{
    return ceil(value);
}

float coglet_rt_math_round_f32(float value)
{
    return roundf(value);
}

double coglet_rt_math_round_f64(double value)
{
    return round(value);
}

float coglet_rt_math_trunc_f32(float value)
{
    return truncf(value);
}

double coglet_rt_math_trunc_f64(double value)
{
    return trunc(value);
}

float coglet_rt_math_fmod_f32(float x, float y)
{
    return fmodf(x, y);
}

double coglet_rt_math_fmod_f64(double x, double y)
{
    return fmod(x, y);
}
