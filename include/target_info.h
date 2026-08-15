#ifndef COGLET_TARGET_INFO_H
#define COGLET_TARGET_INFO_H

#include <stddef.h>

typedef enum TargetFloatFormat {
    TARGET_FLOAT_FORMAT_UNSUPPORTED = 0,
    TARGET_FLOAT_FORMAT_IEEE_BINARY32,
    TARGET_FLOAT_FORMAT_IEEE_BINARY64,
} TargetFloatFormat;

/*
 * Backend-neutral target facts required by semantic analysis.
 *
 * Widths are expressed in bits deliberately: semantic analysis should not
 * depend on the byte size or C implementation used to build the compiler.
 * Backend-specific layout objects (for example LLVM DataLayout) do not belong
 * here.
 */
typedef struct TargetInfo {
    unsigned pointer_bits;

    unsigned c_char_bits;
    int c_char_is_signed;

    unsigned c_bool_bits;
    unsigned c_short_bits;
    unsigned c_int_bits;
    unsigned c_long_bits;
    unsigned c_long_long_bits;
    unsigned c_size_bits;

    TargetFloatFormat c_float_format;
    TargetFloatFormat c_double_format;
} TargetInfo;

/* Returns a description of the native C ABI used to build this compiler. */
TargetInfo target_info_host(void);

/*
 * Performs structural validation independent of Coglet's currently supported
 * scalar widths. Returns non-zero when the target description is well-formed.
 */
int target_info_validate(
    const TargetInfo *target,
    char *message,
    size_t message_size
);

/* Exact equality for the frontend target facts represented today. */
int target_info_equal(const TargetInfo *a, const TargetInfo *b);

#endif
