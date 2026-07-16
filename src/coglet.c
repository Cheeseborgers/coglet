/*
 * Main cog compiler dummy test
 *
 */

#include <stdio.h>
#include <assert.h>

#include "parser.h"
#include "../include/ast.h"
#include "../include/diag.h"
#include "../include/semantic_anal.h"

#include "../tools/ast/ast_print.h"
#include "../include/utils/utils.h"

static int verify_mutation_info(SemanticContext *sem,Node *node) {

    SemExprInfo *info = semantic_get_expr_info(sem, node);

    if (!info) {
        fprintf(
            stderr,
            "semantic-info test failed at line %d: "
            "mutation has no SemExprInfo\n",
            node->line
        );

        return 0;
    }

    if (info->node != node) {
        fprintf(
            stderr,
            "semantic-info test failed at line %d: "
            "SemExprInfo points to the wrong AST node\n",
            node->line
        );

        return 0;
    }

    if (info->type != NULL) {
        fprintf(
            stderr,
            "semantic-info test failed at line %d: "
            "statement-only mutation has a value type\n",
            node->line
        );

        return 0;
    }

    if (info->symbol != NULL) {
        fprintf(
            stderr,
            "semantic-info test failed at line %d: "
            "mutation unexpectedly has a resolved symbol\n",
            node->line
        );

        return 0;
    }

    if (info->value_category != VALUE_CATEGORY_NONE) {
        fprintf(
            stderr,
            "semantic-info test failed at line %d: "
            "mutation category is %d instead of VALUE_CATEGORY_NONE\n",
            node->line,
            (int)info->value_category
        );

        return 0;
    }

    return 1;
}

static int verify_mutation_expression(
    SemanticContext *sem,
    Node *node,
    int *mutation_count
) {
    if (!node)
        return 1;

    switch (node->type) {
        case NODE_ASSIGN:
            (*mutation_count)++;

            if (!verify_mutation_info(sem, node))
                return 0;

            /*
             * Also verify that the target and RHS were checked as normal
             * value expressions.
             */
            if (!verify_mutation_expression(
                    sem,
                    node->as.assign.target,
                    mutation_count)) {
                return 0;
            }

            return verify_mutation_expression(
                sem,
                node->as.assign.value,
                mutation_count
            );

        case NODE_COMPOUND_ASSIGN:
            (*mutation_count)++;

            if (!verify_mutation_info(sem, node))
                return 0;

            if (!verify_mutation_expression(
                    sem,
                    node->as.compound_assign.target,
                    mutation_count)) {
                return 0;
            }

            return verify_mutation_expression(
                sem,
                node->as.compound_assign.value,
                mutation_count
            );

        case NODE_INC_DEC:
            (*mutation_count)++;

            if (!verify_mutation_info(sem, node))
                return 0;

            return verify_mutation_expression(
                sem,
                node->as.inc_dec.target,
                mutation_count
            );

        case NODE_BINARY:
            if (!verify_mutation_expression(
                    sem,
                    node->as.binary.left,
                    mutation_count)) {
                return 0;
            }

            return verify_mutation_expression(
                sem,
                node->as.binary.right,
                mutation_count
            );

        case NODE_UNARY:
            return verify_mutation_expression(
                sem,
                node->as.unary.operand,
                mutation_count
            );

        case NODE_CALL:
            if (!verify_mutation_expression(
                    sem,
                    node->as.call.callee,
                    mutation_count)) {
                return 0;
            }

            for (int i = 0;
                 i < node->as.call.arguments.count;
                 i++) {

                if (!verify_mutation_expression(
                        sem,
                        node->as.call.arguments.items[i],
                        mutation_count)) {
                    return 0;
                }
            }

            return 1;

        case NODE_FIELD:
            return verify_mutation_expression(
                sem,
                node->as.field.object,
                mutation_count
            );

        case NODE_INDEX:
            if (!verify_mutation_expression(
                    sem,
                    node->as.index.object,
                    mutation_count)) {
                return 0;
            }

            return verify_mutation_expression(
                sem,
                node->as.index.index,
                mutation_count
            );

        default:
            return 1;
    }
}

static int verify_mutations_in_ast(
    SemanticContext *sem,
    Node *node,
    int *mutation_count
) {
    if (!node)
        return 1;

    switch (node->type) {
        case NODE_PROGRAM:
            for (int i = 0;
                 i < node->as.program.statements.count;
                 i++) {

                if (!verify_mutations_in_ast(
                        sem,
                        node->as.program.statements.items[i],
                        mutation_count)) {
                    return 0;
                }
            }

            return 1;

        case NODE_FUNC_DECL:
            return verify_mutations_in_ast(
                sem,
                node->as.func_decl.body,
                mutation_count
            );

        case NODE_BLOCK:
            for (int i = 0;
                 i < node->as.block.statements.count;
                 i++) {

                if (!verify_mutations_in_ast(
                        sem,
                        node->as.block.statements.items[i],
                        mutation_count)) {
                    return 0;
                }
            }

            return 1;

        case NODE_EXPR_STMT:
            return verify_mutation_expression(
                sem,
                node->as.expr_stmt.expr,
                mutation_count
            );

        case NODE_VAR_DECL:
            return verify_mutation_expression(
                sem,
                node->as.var_decl.initializer,
                mutation_count
            );

        case NODE_RETURN:
            return verify_mutation_expression(
                sem,
                node->as.return_stmt.value,
                mutation_count
            );

        case NODE_IF:
            if (!verify_mutation_expression(
                    sem,
                    node->as.if_stmt.condition,
                    mutation_count)) {
                return 0;
            }

            if (!verify_mutations_in_ast(
                    sem,
                    node->as.if_stmt.then_branch,
                    mutation_count)) {
                return 0;
            }

            return verify_mutations_in_ast(
                sem,
                node->as.if_stmt.else_branch,
                mutation_count
            );

        case NODE_WHILE:
            if (!verify_mutation_expression(
                    sem,
                    node->as.while_stmt.condition,
                    mutation_count)) {
                return 0;
            }

            return verify_mutations_in_ast(
                sem,
                node->as.while_stmt.body,
                mutation_count
            );

        case NODE_FOR:
            if (!verify_mutation_expression(
                    sem,
                    node->as.for_stmt.condition,
                    mutation_count)) {
                return 0;
            }

            if (!verify_mutation_expression(
                    sem,
                    node->as.for_stmt.post,
                    mutation_count)) {
                return 0;
            }

            return verify_mutations_in_ast(
                sem,
                node->as.for_stmt.body,
                mutation_count
            );

        default:
            return 1;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    char *source = read_file(filename);

    if (!source) {
        fprintf(stderr, "error: could not read '%s'\n", filename);
        return 1;
    }

    Arena *arena   = arena_create(MB(2));
    Arena *scratch = arena_create(MB(1));

    if (!arena || !scratch) {
        fprintf(stderr, "error: failed to create compiler arenas\n");
        free(source);
        return 1;
    }

    Parser parser;
    parser_init(
        &parser,
        filename,
        source,
        arena,
        scratch
    );

    Node *ast = parse_program(&parser);

    int exit_code = 0;

    //ast_pretty_print(ast);

    if (parser.had_error) {
        for (int i = 0;
             i < parser.diagnostic_count;
             i++) {

            print_diagnostic(
                parser.lexer.filename,
                source,
                &parser.diagnostics[i]
            );
             }

        fprintf(
            stderr,
            "%d parser error%s generated.\n",
            parser.error_count,
            parser.error_count == 1 ? "" : "s"
        );

        exit_code = 1;
    } else {
        SemanticContext sem = {0};
        sem.arena = arena;

        semantic_check(ast, &sem);

        if (sem.had_error) {
            fprintf(
                stderr,
                "%d semantic error%s generated.\n",
                sem.error_count,
                sem.error_count == 1 ? "" : "s"
            );

            exit_code = 1;
        } else {
            int mutation_count = 0;

            if (!verify_mutations_in_ast(
                    &sem,
                    ast,
                    &mutation_count)) {

                fprintf(
                    stderr,
                    "semantic-info mutation test failed\n"
                );

                exit_code = 1;
                    } else {
                        printf(
                            "semantic-info mutation test passed: "
                            "%d mutation nodes verified\n",
                            mutation_count
                        );

                        ast_pretty_print(ast);
                    }
        }
    }

    arena_destroy(scratch);
    arena_destroy(arena);
    free(source);

    return exit_code;
}
