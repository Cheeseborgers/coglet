#include <stdio.h>

#include "compiler_driver.h"
#include "semantic_info.h"
#include "target_info.h"
#include "target_layout.h"

static int expect_param_kind(
    CompileResult *result,
    Node *function,
    int parameter_index,
    TypeKind expected_kind,
    const char *label
) {
    if (parameter_index < 0 || parameter_index >= function->as.func_decl.params.count) {
        fprintf(stderr, "%s: parameter index out of range\n", label);
        return 0;
    }

    Node *parameter = function->as.func_decl.params.items[parameter_index];
    SemDeclInfo *info = semantic_get_decl_info(&result->sem, parameter);

    if (!info || !info->type) {
        fprintf(stderr, "%s: missing semantic declaration type\n", label);
        return 0;
    }

    if (info->type->kind != expected_kind) {
        fprintf(
            stderr,
            "%s: expected TypeKind %d, got %d\n",
            label,
            (int)expected_kind,
            (int)info->type->kind
        );
        return 0;
    }

    return 1;
}


static int verify_target_layout_aliases(const TargetInfo *target, uint64_t expected_size, const char *label) {
    Type usize_type = {0};
    usize_type.kind = expected_size == 8 ? TYPE_U64 : TYPE_U32;
    TargetLayout layout;
    char message[160];
    if (!target_layout_of_type(target, &usize_type, &layout, message, sizeof(message))) {
        fprintf(stderr, "%s: layout query failed: %s\n", label, message);
        return 0;
    }
    if (layout.size != expected_size || layout.align == 0) {
        fprintf(stderr, "%s: expected usize layout size %llu, got %llu/%llu\n", label,
            (unsigned long long)expected_size, (unsigned long long)layout.size,
            (unsigned long long)layout.align);
        return 0;
    }
    return 1;
}

static int run_target_case(
    const char *source_path,
    const TargetInfo *target,
    TypeKind expected_long,
    TypeKind expected_size,
    TypeKind expected_char,
    TypeKind expected_usize,
    TypeKind expected_isize,
    const char *label
) {
    CompileResult result;
    CompileStatus status = compile_parse_and_check_for_target(
        source_path,
        target,
        &result
    );

    if (status != COMPILE_STATUS_OK) {
        fprintf(stderr, "%s: compilation failed with status %d\n", label, status);
        compile_result_destroy(&result);
        return 0;
    }

    if (result.program->type != NODE_PROGRAM ||
        result.program->as.program.statements.count != 1) {
        fprintf(stderr, "%s: unexpected fixture AST\n", label);
        compile_result_destroy(&result);
        return 0;
    }

    Node *function = result.program->as.program.statements.items[0];
    if (function->type != NODE_FUNC_DECL) {
        fprintf(stderr, "%s: fixture declaration is not a function\n", label);
        compile_result_destroy(&result);
        return 0;
    }

    int ok =
        expect_param_kind(&result, function, 0, expected_long, label) &&
        expect_param_kind(&result, function, 1, expected_size, label) &&
        expect_param_kind(&result, function, 2, expected_char, label) &&
        expect_param_kind(&result, function, 3, expected_usize, label) &&
        expect_param_kind(&result, function, 4, expected_isize, label);

    if (result.sem.target.c_long_bits != target->c_long_bits ||
        result.sem.target.c_size_bits != target->c_size_bits ||
        result.sem.target.c_char_is_signed != target->c_char_is_signed) {
        fprintf(stderr, "%s: semantic context did not preserve target description\n", label);
        ok = 0;
    }

    compile_result_destroy(&result);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <abi-scalar-fixture.cog>\n", argv[0]);
        return 2;
    }

    TargetInfo target_a = target_info_host();
    target_a.pointer_bits = 64;
    target_a.c_char_bits = 8;
    target_a.c_char_is_signed = 1;
    target_a.c_bool_bits = 8;
    target_a.c_short_bits = 16;
    target_a.c_int_bits = 32;
    target_a.c_long_bits = 32;
    target_a.c_long_long_bits = 64;
    target_a.c_size_bits = 64;
    target_a.c_float_format = TARGET_FLOAT_FORMAT_IEEE_BINARY32;
    target_a.c_double_format = TARGET_FLOAT_FORMAT_IEEE_BINARY64;

    TargetInfo target_b = target_a;
    target_b.pointer_bits = 32;
    target_b.pointer_align_bytes = 4;
    target_b.c_char_is_signed = 0;
    target_b.c_long_bits = 64;
    target_b.c_size_bits = 32;

    if (!verify_target_layout_aliases(&target_a, 8, "synthetic-llp64-like"))
        return 1;

    if (!run_target_case(
            argv[1],
            &target_a,
            TYPE_S32,
            TYPE_U64,
            TYPE_S8,
            TYPE_U64,
            TYPE_S64,
            "synthetic-llp64-like")) {
        return 1;
    }

    if (!verify_target_layout_aliases(&target_b, 4, "synthetic-wide-long"))
        return 1;

    if (!run_target_case(
            argv[1],
            &target_b,
            TYPE_S64,
            TYPE_U32,
            TYPE_U8,
            TYPE_U32,
            TYPE_S32,
            "synthetic-wide-long")) {
        return 1;
    }

    TargetInfo invalid = target_a;
    invalid.c_short_bits = 64;
    invalid.c_int_bits = 32;

    char message[160];
    if (target_info_validate(&invalid, message, sizeof(message))) {
        fprintf(stderr, "invalid target unexpectedly validated\n");
        return 1;
    }

    printf("target-info verification passed\n");
    return 0;
}
