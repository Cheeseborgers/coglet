#ifndef COGLET_TARGET_INFO_H
#define COGLET_TARGET_INFO_H

#include <stddef.h>

#include "target_config.h"

typedef enum TargetFloatFormat {
    TARGET_FLOAT_FORMAT_UNSUPPORTED = 0,
    TARGET_FLOAT_FORMAT_IEEE_BINARY32,
    TARGET_FLOAT_FORMAT_IEEE_BINARY64,
} TargetFloatFormat;

/*
 * Backend-neutral ABI and layout facts for the selected compilation target.
 *
 * TargetConfig identifies the architecture, operating system, and ABI.
 * TargetInfo supplies the concrete scalar widths and alignments that those
 * choices imply. Semantic analysis should consume TargetInfo rather than
 * inspecting the host C compiler directly.
 *
 * Widths are expressed in bits deliberately: semantic analysis should not
 * depend on the byte size or C implementation used to build the compiler.
 * Backend-specific layout objects (for example LLVM DataLayout) do not belong
 * here.
 */
typedef struct TargetInfo {
    unsigned pointer_bits;
    unsigned pointer_align_bytes;

    /* Alignment facts are expressed in bytes for concrete object layout. */
    unsigned scalar_align_8_bytes;
    unsigned scalar_align_16_bytes;
    unsigned scalar_align_32_bytes;
    unsigned scalar_align_64_bytes;

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
 * Builds the frontend ABI/layout description for a supported compilation
 * target. Returns non-zero when the target is supported.
 */
int target_info_from_config(
    const TargetConfig *config,
    TargetInfo *out
);

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
