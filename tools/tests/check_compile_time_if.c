#include <stdio.h>
#include <string.h>

#include "compiler_driver.h"
#include "target/target_config.h"

static int message_contains(const DiagnosticList *diagnostics, const char *text)
{
    for (const Diagnostic *diagnostic = diagnostics->first;
         diagnostic;
         diagnostic = diagnostic->next) {
        if (diagnostic->message && strstr(diagnostic->message, text))
            return 1;
    }
    return 0;
}

static Node *first_function(CompileResult *result)
{
    if (!result->program || result->program->type != NODE_PROGRAM)
        return NULL;

    for (int i = 0; i < result->program->as.program.statements.count; ++i) {
        Node *node = result->program->as.program.statements.items[i];
        if (node && node->type == NODE_FUNC_DECL)
            return node;
    }
    return NULL;
}

static Node *first_compile_if(Node *node)
{
    if (!node)
        return NULL;
    if (node->type == NODE_IF && node->as.if_stmt.is_compile_time)
        return node;
    if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.statements.count; ++i) {
            Node *found = first_compile_if(node->as.block.statements.items[i]);
            if (found)
                return found;
        }
    }
    return NULL;
}

static int check_valid_fixture(const char *path)
{
    const TargetConfig configs[] = {
        { TARGET_ARCH_X86_64, TARGET_OS_LINUX, TARGET_ABI_SYSV },
        { TARGET_ARCH_X86_64, TARGET_OS_WINDOWS, TARGET_ABI_WINDOWS },
        { TARGET_ARCH_AARCH64, TARGET_OS_LINUX, TARGET_ABI_SYSV },
        { TARGET_ARCH_AARCH64, TARGET_OS_WINDOWS, TARGET_ABI_WINDOWS },
    };

    const int expected_root_selection[] = { 1, 1, 2, 2 };
    const int expected_nested_selection[] = { 1, 2, 1, 2 };

    for (int i = 0; i < 4; ++i) {
        CompileResult result;
        CompileStatus status = compile_parse_and_check(path, &configs[i], &result);
        if (status != COMPILE_STATUS_OK) {
            fprintf(stderr, "valid compile-time-if fixture failed for target %d\n", i);
            compile_result_destroy(&result);
            return 0;
        }

        Node *function = first_function(&result);
        Node *root = function ? first_compile_if(function->as.func_decl.body) : NULL;
        if (!root || root->as.if_stmt.compile_time_selection != expected_root_selection[i]) {
            fprintf(stderr, "unexpected root compile-time selection for target %d\n", i);
            compile_result_destroy(&result);
            return 0;
        }

        Node *nested = expected_root_selection[i] == 1
            ? root->as.if_stmt.then_branch
            : root->as.if_stmt.else_branch;
        if (nested && nested->type == NODE_BLOCK &&
            nested->as.block.statements.count > 0)
            nested = nested->as.block.statements.items[0];

        if (expected_root_selection[i] == 2) {
            if (!nested || nested->type != NODE_IF ||
                !nested->as.if_stmt.is_compile_time ||
                nested->as.if_stmt.compile_time_selection != 1) {
                fprintf(stderr, "unexpected #elif selection for target %d\n", i);
                compile_result_destroy(&result);
                return 0;
            }
            nested = nested->as.if_stmt.then_branch;
            if (nested && nested->type == NODE_BLOCK &&
                nested->as.block.statements.count > 0)
                nested = nested->as.block.statements.items[0];
        }

        if (!nested || nested->type != NODE_IF ||
            !nested->as.if_stmt.is_compile_time ||
            nested->as.if_stmt.compile_time_selection != expected_nested_selection[i]) {
            fprintf(stderr, "unexpected nested compile-time selection for target %d\n", i);
            compile_result_destroy(&result);
            return 0;
        }

        compile_result_destroy(&result);

    }

    return 1;
}

static int check_inactive_branch(const char *path)
{
    TargetConfig x86 = { TARGET_ARCH_X86_64, TARGET_OS_LINUX, TARGET_ABI_SYSV };
    TargetConfig aarch64 = { TARGET_ARCH_AARCH64, TARGET_OS_LINUX, TARGET_ABI_SYSV };
    CompileResult result;

    if (compile_parse_and_check(path, &x86, &result) != COMPILE_STATUS_OK) {
        fprintf(stderr, "inactive compile-time branch was checked for x86_64\n");
        compile_result_destroy(&result);
        return 0;
    }
    compile_result_destroy(&result);

    if (compile_parse_and_check(path, &aarch64, &result) == COMPILE_STATUS_OK ||
        !message_contains(&result.sem.diagnostics, "undefined identifier")) {
        fprintf(stderr, "selected invalid compile-time branch did not produce the expected semantic diagnostic\n");
        compile_result_destroy(&result);
        return 0;
    }
    compile_result_destroy(&result);
    return 1;
}

static int check_condition_diagnostic(const char *path, const TargetConfig *target, const char *message)
{
    CompileResult result;
    if (compile_parse_and_check(path, target, &result) == COMPILE_STATUS_OK ||
        !message_contains(&result.sem.diagnostics, message)) {
        fprintf(stderr, "missing expected compile-time #if diagnostic: %s\n", message);
        compile_result_destroy(&result);
        return 0;
    }
    compile_result_destroy(&result);
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <valid> <inactive> <nonconstant> <nonbool>\n", argv[0]);
        return 2;
    }

    TargetConfig native = target_config_native();

    if (!check_valid_fixture(argv[1])) { fprintf(stderr, "valid fixture failed\n"); return 1; }
    if (!check_inactive_branch(argv[2])) { fprintf(stderr, "inactive fixture failed\n"); return 1; }
    if (!check_condition_diagnostic(argv[3], &native,
            "compile-time #if condition must be a constant expression")) { fprintf(stderr, "nonconstant diagnostic failed\n"); return 1; }
    if (!check_condition_diagnostic(argv[4], &native,
            "compile-time #if condition must evaluate to a boolean")) { fprintf(stderr, "nonbool diagnostic failed\n"); return 1; }

    return 0;
}
