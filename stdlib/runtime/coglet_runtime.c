#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(COGLET_RUNTIME_MATH)
#include <math.h>
#endif

/*
 * Coglet runtime ABI, v0.
 *
 * These symbols are implementation details used by runtime-facing standard
 * modules. User code should import std.io/std.math/std.mem rather than declaring them directly.
 * The ABI intentionally uses only ISO C scalar/pointer types so the same source
 * compiles for Linux and Windows on both x86-64 and AArch64 native toolchains.
 */

void coglet_rt_io_write(const uint8_t *data, uint64_t length)
{
    if (length == 0)
        return;

    (void)fwrite(data, 1, (size_t)length, stdout);
}

void coglet_rt_io_newline(void)
{
    fputc('\n', stdout);
}

void coglet_rt_io_flush(void)
{
    fflush(stdout);
}

void coglet_rt_io_print_bool(_Bool value)
{
    fputs(value ? "true" : "false", stdout);
}

void coglet_rt_io_print_s8(int8_t value)
{
    fprintf(stdout, "%" PRId8, value);
}

void coglet_rt_io_print_s16(int16_t value)
{
    fprintf(stdout, "%" PRId16, value);
}

void coglet_rt_io_print_s32(int32_t value)
{
    fprintf(stdout, "%" PRId32, value);
}

void coglet_rt_io_print_s64(int64_t value)
{
    fprintf(stdout, "%" PRId64, value);
}

void coglet_rt_io_print_u8(uint8_t value)
{
    fprintf(stdout, "%" PRIu8, value);
}

void coglet_rt_io_print_u16(uint16_t value)
{
    fprintf(stdout, "%" PRIu16, value);
}

void coglet_rt_io_print_u32(uint32_t value)
{
    fprintf(stdout, "%" PRIu32, value);
}

void coglet_rt_io_print_u64(uint64_t value)
{
    fprintf(stdout, "%" PRIu64, value);
}

void coglet_rt_io_print_f32(float value)
{
    fprintf(stdout, "%.9g", (double)value);
}

void coglet_rt_io_print_f64(double value)
{
    fprintf(stdout, "%.17g", value);
}

#if defined(COGLET_RUNTIME_MATH)

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

#endif

/*
 * Explicit aligned heap storage used by std.mem.
 *
 * The allocator stores the original malloc pointer and allocation size in a
 * private header immediately before the aligned user pointer. This keeps the
 * runtime portable across the initial Linux/Windows x86-64/AArch64 matrix and
 * avoids platform-specific aligned-free/realloc pairing rules.
 */
typedef struct CogletRuntimeAllocationHeader {
    void *base;
    size_t size;
} CogletRuntimeAllocationHeader;

static void coglet_rt_mem_fail(void)
{
    fputs("Coglet runtime: out of memory\n", stderr);
    abort();
}

static size_t coglet_rt_mem_checked_size(uint64_t value)
{
    if (value > (uint64_t)SIZE_MAX)
        coglet_rt_mem_fail();
    return (size_t)value;
}

static size_t coglet_rt_mem_checked_alignment(uint64_t value)
{
    if (value == 0 || (value & (value - 1)) != 0 || value > (uint64_t)SIZE_MAX)
        coglet_rt_mem_fail();

    size_t alignment = (size_t)value;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    return alignment;
}

static void *coglet_rt_mem_allocate_impl(size_t size, size_t alignment)
{
    if (size == 0)
        return NULL;

    size_t header_size = sizeof(CogletRuntimeAllocationHeader);
    if (size > SIZE_MAX - header_size ||
        size + header_size > SIZE_MAX - (alignment - 1)) {
        coglet_rt_mem_fail();
    }

    size_t total = size + header_size + alignment - 1;
    unsigned char *base = (unsigned char *)malloc(total);
    if (!base)
        coglet_rt_mem_fail();

    uintptr_t first = (uintptr_t)(base + header_size);
    uintptr_t aligned = (first + (uintptr_t)(alignment - 1)) &
                        ~(uintptr_t)(alignment - 1);
    CogletRuntimeAllocationHeader *header =
        (CogletRuntimeAllocationHeader *)(aligned - header_size);
    header->base = base;
    header->size = size;
    return (void *)aligned;
}

void *coglet_rt_mem_alloc(uint64_t size, uint64_t alignment)
{
    return coglet_rt_mem_allocate_impl(
        coglet_rt_mem_checked_size(size),
        coglet_rt_mem_checked_alignment(alignment)
    );
}

void coglet_rt_mem_free(void *pointer)
{
    if (!pointer)
        return;

    CogletRuntimeAllocationHeader *header =
        (CogletRuntimeAllocationHeader *)((unsigned char *)pointer -
                                           sizeof(CogletRuntimeAllocationHeader));
    free(header->base);
}

void *coglet_rt_mem_resize(void *pointer, uint64_t size, uint64_t alignment)
{
    size_t new_size = coglet_rt_mem_checked_size(size);
    size_t checked_alignment = coglet_rt_mem_checked_alignment(alignment);

    if (!pointer)
        return coglet_rt_mem_allocate_impl(new_size, checked_alignment);

    if (new_size == 0) {
        coglet_rt_mem_free(pointer);
        return NULL;
    }

    CogletRuntimeAllocationHeader *old_header =
        (CogletRuntimeAllocationHeader *)((unsigned char *)pointer -
                                           sizeof(CogletRuntimeAllocationHeader));
    size_t old_size = old_header->size;
    void *resized = coglet_rt_mem_allocate_impl(new_size, checked_alignment);
    memcpy(resized, pointer, old_size < new_size ? old_size : new_size);
    coglet_rt_mem_free(pointer);
    return resized;
}
