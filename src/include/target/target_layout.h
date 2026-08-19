#ifndef COGLET_TARGET_LAYOUT_H
#define COGLET_TARGET_LAYOUT_H

#include <stdint.h>

#include "target_info.h"
#include "types.h"

typedef struct TargetLayout {
    uint64_t size;
    uint64_t align;
} TargetLayout;

/*
 * Computes the concrete object layout for a semantic runtime type using the
 * frozen TargetInfo contract. This is the single frontend-visible authority
 * for size_of(T)/align_of(T); callers must not duplicate aggregate layout.
 */
int target_layout_of_type(
    const TargetInfo *target,
    const Type *type,
    TargetLayout *out,
    char *message,
    size_t message_size
);

#endif
