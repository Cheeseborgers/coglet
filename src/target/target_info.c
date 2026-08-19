#include "target/target_info.h"

#include <float.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#define TARGET_ALIGNOF(type) offsetof(struct { char pad; type value; }, value)

static TargetFloatFormat host_float_format(
    unsigned bits,
    int mantissa_bits,
    int min_exponent,
    int max_exponent
) {
    if (FLT_RADIX != 2)
        return TARGET_FLOAT_FORMAT_UNSUPPORTED;

    if (
        bits == 32 &&
        mantissa_bits == 24 &&
        min_exponent == -125 &&
        max_exponent == 128
    ) {
        return TARGET_FLOAT_FORMAT_IEEE_BINARY32;
    }

    if (
        bits == 64 &&
        mantissa_bits == 53 &&
        min_exponent == -1021 &&
        max_exponent == 1024
    ) {
        return TARGET_FLOAT_FORMAT_IEEE_BINARY64;
    }

    return TARGET_FLOAT_FORMAT_UNSUPPORTED;
}

static void target_info_init_common(TargetInfo *target)
{
    target->pointer_bits = 64;
    target->pointer_align_bytes = 8;
    target->scalar_align_8_bytes = 1;
    target->scalar_align_16_bytes = 2;
    target->scalar_align_32_bytes = 4;
    target->scalar_align_64_bytes = 8;

    target->c_char_bits = 8;
    target->c_char_is_signed = 1;
    target->c_bool_bits = 8;
    target->c_short_bits = 16;
    target->c_int_bits = 32;
    target->c_long_long_bits = 64;
    target->c_size_bits = 64;
    target->c_float_format = TARGET_FLOAT_FORMAT_IEEE_BINARY32;
    target->c_double_format = TARGET_FLOAT_FORMAT_IEEE_BINARY64;
}

int target_info_from_config(
    const TargetConfig *config,
    TargetInfo *out
)
{
    if (!config || !out)
        return 0;

    target_info_init_common(out);

    switch (config->arch) {
        case TARGET_ARCH_X86_64:
        case TARGET_ARCH_AARCH64:
            break;
        default:
            return 0;
    }

    switch (config->os) {
        case TARGET_OS_LINUX:
            out->c_long_bits = 64;
            break;

        case TARGET_OS_WINDOWS:
            out->c_long_bits = 32;
            break;

        default:
            return 0;
    }

    switch (config->abi) {
        case TARGET_ABI_SYSV:
        case TARGET_ABI_WINDOWS:
            break;
        default:
            return 0;
    }

    return target_info_validate(out, NULL, 0);
}

TargetInfo target_info_host(void) {
    TargetInfo target;

    target.pointer_bits = (unsigned)(sizeof(void *) * CHAR_BIT);
    target.pointer_align_bytes = (unsigned)TARGET_ALIGNOF(void *);
    target.scalar_align_8_bytes = (unsigned)TARGET_ALIGNOF(uint8_t);
    target.scalar_align_16_bytes = (unsigned)TARGET_ALIGNOF(uint16_t);
    target.scalar_align_32_bytes = (unsigned)TARGET_ALIGNOF(uint32_t);
    target.scalar_align_64_bytes = (unsigned)TARGET_ALIGNOF(uint64_t);

    target.c_char_bits = (unsigned)(sizeof(char) * CHAR_BIT);
    target.c_char_is_signed = CHAR_MIN < 0;

    target.c_bool_bits = (unsigned)(sizeof(_Bool) * CHAR_BIT);
    target.c_short_bits = (unsigned)(sizeof(short) * CHAR_BIT);
    target.c_int_bits = (unsigned)(sizeof(int) * CHAR_BIT);
    target.c_long_bits = (unsigned)(sizeof(long) * CHAR_BIT);
    target.c_long_long_bits = (unsigned)(sizeof(long long) * CHAR_BIT);
    target.c_size_bits = (unsigned)(sizeof(size_t) * CHAR_BIT);

    target.c_float_format = host_float_format(
        (unsigned)(sizeof(float) * CHAR_BIT),
        FLT_MANT_DIG,
        FLT_MIN_EXP,
        FLT_MAX_EXP
    );
    target.c_double_format = host_float_format(
        (unsigned)(sizeof(double) * CHAR_BIT),
        DBL_MANT_DIG,
        DBL_MIN_EXP,
        DBL_MAX_EXP
    );

    return target;
}

static int fail(
    char *message,
    size_t message_size,
    const char *text
) {
    if (message && message_size > 0)
        snprintf(message, message_size, "%s", text);
    return 0;
}

int target_info_validate(
    const TargetInfo *target,
    char *message,
    size_t message_size
) {
    if (!target)
        return fail(message, message_size, "target description is null");

    if (target->pointer_bits == 0 || target->pointer_bits % CHAR_BIT != 0)
        return fail(message, message_size, "pointer width must be a non-zero whole number of bytes");

    if (target->pointer_align_bytes == 0 ||
        target->scalar_align_8_bytes == 0 ||
        target->scalar_align_16_bytes == 0 ||
        target->scalar_align_32_bytes == 0 ||
        target->scalar_align_64_bytes == 0) {
        return fail(message, message_size, "layout alignments must be non-zero");
    }

    if (target->c_char_bits == 0)
        return fail(message, message_size, "C char width must be non-zero");

    if (target->c_bool_bits == 0)
        return fail(message, message_size, "C _Bool width must be non-zero");

    if (target->c_short_bits == 0 ||
        target->c_int_bits == 0 ||
        target->c_long_bits == 0 ||
        target->c_long_long_bits == 0 ||
        target->c_size_bits == 0) {
        return fail(message, message_size, "C integer widths must be non-zero");
    }

    if (target->c_short_bits > target->c_int_bits ||
        target->c_int_bits > target->c_long_bits ||
        target->c_long_bits > target->c_long_long_bits) {
        return fail(
            message,
            message_size,
            "C signed integer widths must be non-decreasing"
        );
    }

    if (message && message_size > 0)
        message[0] = '\0';

    return 1;
}


int target_info_equal(const TargetInfo *a, const TargetInfo *b) {
    if (!a || !b)
        return 0;

    return
        a->pointer_bits == b->pointer_bits &&
        a->pointer_align_bytes == b->pointer_align_bytes &&
        a->scalar_align_8_bytes == b->scalar_align_8_bytes &&
        a->scalar_align_16_bytes == b->scalar_align_16_bytes &&
        a->scalar_align_32_bytes == b->scalar_align_32_bytes &&
        a->scalar_align_64_bytes == b->scalar_align_64_bytes &&
        a->c_char_bits == b->c_char_bits &&
        a->c_char_is_signed == b->c_char_is_signed &&
        a->c_bool_bits == b->c_bool_bits &&
        a->c_short_bits == b->c_short_bits &&
        a->c_int_bits == b->c_int_bits &&
        a->c_long_bits == b->c_long_bits &&
        a->c_long_long_bits == b->c_long_long_bits &&
        a->c_size_bits == b->c_size_bits &&
        a->c_float_format == b->c_float_format &&
        a->c_double_format == b->c_double_format;
}
