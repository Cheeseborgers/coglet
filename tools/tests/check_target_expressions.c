#include <stdio.h>
#include <string.h>

#include "compiler_driver.h"
#include "target/target_config.h"

static int name_is(StringView name, const char *text) {
    size_t length = strlen(text);
    return name.length == length && memcmp(name.data, text, length) == 0;
}

static int integer_is(const ConstValue *value, uint64_t expected) {
    return value &&
           value->kind == CONST_VALUE_INT &&
           !value->as.integer.is_negative &&
           value->as.integer.magnitude == expected;
}

static int verify_target_expression(
    CompileResult *result,
    const char *object_name,
    const char *field_name,
    uint64_t expected
) {
    for (SemExprInfo *info = result->sem.expr_infos; info; info = info->next) {
        Node *node = info->node;
        if (!node || node->type != NODE_FIELD)
            continue;

        if (!node->as.field.object ||
            node->as.field.object->type != NODE_IDENT ||
            !name_is(node->as.field.object->as.ident, object_name) ||
            !name_is(node->as.field.name, field_name)) {
            continue;
        }

        ConstValue value;
        if (!semantic_get_constant_value(&result->sem, node, &value)) {
            fprintf(
                stderr,
                "target expression %s.%s was not retrievable as a constant\n",
                object_name,
                field_name
            );
            return 0;
        }

        if (!integer_is(&value, expected)) {
            fprintf(
                stderr,
                "target expression %s.%s: expected %llu, got %llu\n",
                object_name,
                field_name,
                (unsigned long long)expected,
                (unsigned long long)value.as.integer.magnitude
            );
            return 0;
        }

        return 1;
    }

    fprintf(
        stderr,
        "target expression %s.%s was not recorded\n",
        object_name,
        field_name
    );
    return 0;
}

static int run_target_case(
    const char *source_path,
    const TargetConfig *target,
    const char *label
) {
    CompileResult result;
    CompileStatus status = compile_parse_and_check(
        source_path,
        target,
        &result
    );

    if (status != COMPILE_STATUS_OK) {
        fprintf(
            stderr,
            "%s: fixture failed semantic analysis with status %d\n",
            label,
            status
        );
        compile_result_destroy(&result);
        return 0;
    }

    int ok =
        verify_target_expression(
            &result,
            "target",
            "arch",
            (uint64_t)target->arch
        ) &&
        verify_target_expression(
            &result,
            "target",
            "os",
            (uint64_t)target->os
        ) &&
        verify_target_expression(
            &result,
            "target",
            "abi",
            (uint64_t)target->abi
        ) &&
        verify_target_expression(
            &result,
            "arch",
            target->arch == TARGET_ARCH_X86_64 ? "x86_64" : "aarch64",
            (uint64_t)target->arch
        ) &&
        verify_target_expression(
            &result,
            "os",
            target->os == TARGET_OS_LINUX ? "linux" : "windows",
            (uint64_t)target->os
        ) &&
        verify_target_expression(
            &result,
            "abi",
            target->abi == TARGET_ABI_SYSV ? "sysv" : "windows",
            (uint64_t)target->abi
        );

    compile_result_destroy(&result);
    return ok;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <fixture.cog>\n", argv[0]);
        return 2;
    }

    TargetConfig targets[] = {
        {
            TARGET_ARCH_X86_64,
            TARGET_OS_LINUX,
            TARGET_ABI_SYSV
        },
        {
            TARGET_ARCH_AARCH64,
            TARGET_OS_LINUX,
            TARGET_ABI_SYSV
        },
        {
            TARGET_ARCH_X86_64,
            TARGET_OS_WINDOWS,
            TARGET_ABI_WINDOWS
        },
        {
            TARGET_ARCH_AARCH64,
            TARGET_OS_WINDOWS,
            TARGET_ABI_WINDOWS
        },
    };

    const char *labels[] = {
        "x86_64-linux",
        "aarch64-linux",
        "x86_64-windows",
        "aarch64-windows",
    };

    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        if (!run_target_case(argv[1], &targets[i], labels[i]))
            return 1;
    }

    printf("target compile-time expressions verified across %zu targets\n",
        sizeof(targets) / sizeof(targets[0]));
    return 0;
}
