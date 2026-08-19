#include <stdio.h>
#include <string.h>

#include "compiler_driver.h"

static int fail(CompileResult *result, const char *message) {
    fprintf(stderr, "constant-value check failed: %s\n", message);
    compile_result_destroy(result);
    return 1;
}

static int name_is(StringView name, const char *text) {
    size_t length = strlen(text);
    return name.length == length && memcmp(name.data, text, length) == 0;
}

static int integer_is(const ConstValue *value, uint64_t magnitude, int negative) {
    return value->kind == CONST_VALUE_INT &&
           value->as.integer.magnitude == magnitude &&
           value->as.integer.is_negative == negative;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    TargetConfig target_config = target_config_host();

    CompileResult result;
    CompileStatus status = compile_parse_and_check(argv[1], &target_config, &result);

    if (status != COMPILE_STATUS_OK)
        return fail(&result, "fixture did not pass semantic analysis");

    int cached_expression_count = 0;
    int contextual_constant_count = 0;
    int saw_int_to_float = 0;
    int saw_case_base = 0;

    for (SemExprInfo *info = result.sem.expr_infos; info; info = info->next) {
        if (!info->has_constant_value)
            continue;

        cached_expression_count++;

        ConstValue value;
        if (!semantic_get_constant_value(&result.sem, info->node, &value))
            return fail(&result, "cached expression constant was not retrievable");

        Type *effective = semantic_get_effective_expr_type(&result.sem, info->node);
        if (effective && value.type != effective)
            return fail(&result, "retrieved expression constant did not use effective type");

        if (info->contextual_type)
            contextual_constant_count++;

        if (info->contextual_conversion ==
                SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE) {
            if (value.kind != CONST_VALUE_FLOAT ||
                value.type->kind != TYPE_F32 ||
                value.as.floating != 3.0) {
                return fail(&result, "integer-to-f32 constant was not normalized");
            }
            saw_int_to_float = 1;
        }

        if (info->node->type == NODE_IDENT &&
            name_is(info->node->as.ident, "BASE") &&
            info->contextual_type &&
            info->contextual_type->kind == TYPE_S32) {
            if (!integer_is(&value, 42, 0) || value.type->kind != TYPE_S32)
                return fail(&result, "switch case constant did not normalize to s32");
            saw_case_base = 1;
        }
    }

    int constant_decl_count = 0;
    int enum_member_count = 0;
    int saw_base = 0;
    int saw_wide = 0;
    int saw_none = 0;
    int saw_zero = 0;
    int saw_seven = 0;
    int saw_eight = 0;

    for (SemDeclInfo *info = result.sem.decl_infos; info; info = info->next) {
        Node *node = info->node;
        if (!node)
            continue;

        if (node->type == NODE_CONST_DECL) {
            constant_decl_count++;

            ConstValue value;
            if (!semantic_get_constant_value(&result.sem, node, &value))
                return fail(&result, "constant declaration was not retrievable");

            if (value.type != info->type)
                return fail(&result, "constant declaration value type did not match declaration type");

            StringView name = node->as.const_decl.name;
            if (name_is(name, "BASE")) {
                if (!integer_is(&value, 42, 0))
                    return fail(&result, "BASE did not evaluate to 42");
                saw_base = 1;
            } else if (name_is(name, "WIDE")) {
                if (!integer_is(&value, 7, 0) || value.type->kind != TYPE_S64)
                    return fail(&result, "WIDE did not retain its declared s64 type");
                saw_wide = 1;
            } else if (name_is(name, "NONE")) {
                if (value.kind != CONST_VALUE_NULL || value.type->kind != TYPE_POINTER)
                    return fail(&result, "typed null constant was not normalized to its pointer type");
                saw_none = 1;
            }
        }

        if (node->type == NODE_ENUM_MEMBER) {
            enum_member_count++;

            ConstValue value;
            if (!semantic_get_constant_value(&result.sem, node, &value))
                return fail(&result, "enum member was not retrievable as a compile-time value");

            if (value.kind != CONST_VALUE_INT || value.type != info->type)
                return fail(&result, "enum member constant has invalid kind/type");

            StringView name = node->as.enum_member.name;
            if (name_is(name, "Zero")) {
                if (!integer_is(&value, 0, 0))
                    return fail(&result, "implicit enum value Zero was not 0");
                saw_zero = 1;
            } else if (name_is(name, "Seven")) {
                if (!integer_is(&value, 7, 0))
                    return fail(&result, "explicit enum value Seven was not 7");
                saw_seven = 1;
            } else if (name_is(name, "Eight")) {
                if (!integer_is(&value, 8, 0))
                    return fail(&result, "implicit enum value Eight was not 8");
                saw_eight = 1;
            }
        }
    }

    if (cached_expression_count < 10)
        return fail(&result, "too few constant expressions were cached");
    if (contextual_constant_count < 4)
        return fail(&result, "contextual constant normalization was not exercised");
    if (constant_decl_count != 6)
        return fail(&result, "fixture constant declaration count changed");
    if (enum_member_count != 3)
        return fail(&result, "fixture enum member count changed");

    if (!saw_int_to_float || !saw_case_base ||
        !saw_base || !saw_wide || !saw_none ||
        !saw_zero || !saw_seven || !saw_eight) {
        return fail(&result, "expected constant-value coverage was incomplete");
    }

    printf(
        "constant values cached: expressions=%d contextual=%d declarations=%d enum-members=%d\n",
        cached_expression_count,
        contextual_constant_count,
        constant_decl_count,
        enum_member_count
    );

    compile_result_destroy(&result);
    return 0;
}
