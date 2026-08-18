#include "target_layout.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int fail(char *message, size_t message_size, const char *format, ...) {
    if (message && message_size) {
        va_list args;
        va_start(args, format);
        vsnprintf(message, message_size, format, args);
        va_end(args);
    }
    return 0;
}

static int round_up(uint64_t value, uint64_t align, uint64_t *out) {
    if (!align || value > UINT64_MAX - (align - 1)) return 0;
    *out = ((value + align - 1) / align) * align;
    return 1;
}

static unsigned scalar_align(const TargetInfo *target, unsigned bits) {
    switch (bits) {
        case 8: return target->scalar_align_8_bytes;
        case 16: return target->scalar_align_16_bytes;
        case 32: return target->scalar_align_32_bytes;
        case 64: return target->scalar_align_64_bytes;
    }
    return 0;
}

static int layout_impl(const TargetInfo *target, const Type *type, TargetLayout *out,
                       const Type **stack, size_t depth, char *message, size_t message_size) {
    if (!type) return fail(message, message_size, "layout type is null");
    if (depth >= 256) return fail(message, message_size, "recursive type layout is too deep");
    for (size_t i = 0; i < depth; ++i)
        if (stack[i] == type)
            return fail(message, message_size, "recursive by-value type has no finite layout");

    switch (type->kind) {
        case TYPE_BOOL: case TYPE_S8: case TYPE_U8:
            out->size = 1; out->align = scalar_align(target, 8); return 1;
        case TYPE_S16: case TYPE_U16:
            out->size = 2; out->align = scalar_align(target, 16); return 1;
        case TYPE_S32: case TYPE_U32: case TYPE_F32:
            out->size = 4; out->align = scalar_align(target, 32); return 1;
        case TYPE_S64: case TYPE_U64: case TYPE_F64:
            out->size = 8; out->align = scalar_align(target, 64); return 1;
        case TYPE_POINTER: case TYPE_OPAQUE_POINTER:
            out->size = target->pointer_bits / 8; out->align = target->pointer_align_bytes; return 1;
        case TYPE_FUNCTION:
            if (type->function_abi != FUNCTION_ABI_C)
                return fail(message, message_size, "ordinary Coglet function has no object layout");
            out->size = target->pointer_bits / 8; out->align = target->pointer_align_bytes; return 1;
        case TYPE_ARRAY: {
            if (type->array_size < 0) return fail(message, message_size, "unsized array has no object layout");
            TargetLayout element;
            stack[depth] = type;
            if (!layout_impl(target, type->element, &element, stack, depth + 1, message, message_size)) return 0;
            if ((uint64_t)type->array_size && element.size > UINT64_MAX / (uint64_t)type->array_size)
                return fail(message, message_size, "array layout size overflows");
            out->size = element.size * (uint64_t)type->array_size;
            out->align = element.align;
            return 1;
        }
        case TYPE_SLICE: {
            TargetLayout pointer = { target->pointer_bits / 8, target->pointer_align_bytes };
            TargetLayout length = { target->pointer_bits / 8, target->scalar_align_32_bytes };
            /* usize uses pointer width; its alignment follows the matching integer width. */
            length.align = scalar_align(target, target->pointer_bits);
            uint64_t offset;
            if (!round_up(pointer.size, length.align, &offset) || offset > UINT64_MAX - length.size)
                return fail(message, message_size, "slice layout size overflows");
            uint64_t size = offset + length.size;
            uint64_t align = pointer.align > length.align ? pointer.align : length.align;
            if (!round_up(size, align, &out->size)) return fail(message, message_size, "slice layout size overflows");
            out->align = align;
            return 1;
        }
        case TYPE_ENUM:
            if (!type->enum_backing_type) return fail(message, message_size, "enum backing type is unavailable");
            return layout_impl(target, type->enum_backing_type, out, stack, depth, message, message_size);
        case TYPE_STRUCT: {
            if (type->struct_is_incomplete) return fail(message, message_size, "incomplete struct has no object layout");
            stack[depth] = type;
            uint64_t size = 0, max_align = 1;
            if (type->struct_is_union) {
                for (int i = 0; i < type->field_count; ++i) {
                    TargetLayout field;
                    if (!layout_impl(target, type->fields[i].type, &field, stack, depth + 1, message, message_size)) return 0;
                    if (field.size > size) size = field.size;
                    if (field.align > max_align) max_align = field.align;
                }
            } else {
                for (int i = 0; i < type->field_count; ++i) {
                    TargetLayout field;
                    if (!layout_impl(target, type->fields[i].type, &field, stack, depth + 1, message, message_size)) return 0;
                    uint64_t offset;
                    uint64_t field_align = type->struct_repr_c_packed ? 1 : field.align;
                    if (!round_up(size, field_align, &offset) || offset > UINT64_MAX - field.size)
                        return fail(message, message_size, "struct layout size overflows");
                    size = offset + field.size;
                    if (field_align > max_align) max_align = field_align;
                }
            }
            if (type->struct_repr_c_packed) max_align = 1;
            if (type->struct_repr_c_align > 0 && (uint64_t)type->struct_repr_c_align > max_align)
                max_align = (uint64_t)type->struct_repr_c_align;
            if (!round_up(size, max_align, &out->size)) return fail(message, message_size, "struct layout size overflows");
            out->align = max_align;
            return 1;
        }
        default:
            return fail(message, message_size, "type has no concrete object layout");
    }
}

int target_layout_of_type(const TargetInfo *target, const Type *type, TargetLayout *out,
                          char *message, size_t message_size) {
    if (!target || !out) return fail(message, message_size, "layout target or output is null");
    char target_message[128];
    if (!target_info_validate(target, target_message, sizeof(target_message)))
        return fail(message, message_size, "invalid target layout: %s", target_message);
    const Type *stack[256];
    memset(stack, 0, sizeof(stack));
    return layout_impl(target, type, out, stack, 0, message, message_size);
}
