#include "semantic_info_verifier.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Semantic-info verification deliberately lives in the test tool rather than
 * semantic analysis. It walks the parsed AST in deterministic source order and
 * checks that every successfully checked expression has one coherent side-table
 * entry.
 */

typedef void (*ExpressionVisitor)(void *context, Node *expression);

typedef struct ExpressionWalker {
    SemanticContext *sem;
    ExpressionVisitor visit;
    void *context;
} ExpressionWalker;

typedef struct ExpressionList {
    Node **items;
    int count;
    int capacity;
} ExpressionList;

typedef struct Verifier {
    SemanticContext *sem;
    FILE *diagnostics;
    ExpressionList expressions;
    int mutation_count;
    int table_entry_count;
    int error_count;
} Verifier;

typedef struct DumpContext {
    SemanticContext *sem;
    FILE *output;
    int count;
} DumpContext;

// TODO: move these type printers out
static const char *value_category_name(ValueCategory category)
{
    switch (category) {
        case VALUE_CATEGORY_NONE:   return "none";
        case VALUE_CATEGORY_RVALUE: return "rvalue";
        case VALUE_CATEGORY_LVALUE: return "lvalue";
    }

    return "<invalid>";
}

static const char *node_type_name(NodeType type)
{
    switch (type) {
        case NODE_NUMBER:            return "number";
        case NODE_IDENT:             return "ident";
        case NODE_STRING:            return "string";
        case NODE_CHAR:              return "char";
        case NODE_BOOL:              return "bool";
        case NODE_CAST:              return "cast";
        case NODE_UNARY:             return "unary";
        case NODE_BINARY:            return "binary";
        case NODE_INC_DEC:           return "inc_dec";
        case NODE_BLOCK:             return "block";
        case NODE_ASSIGN:            return "assign";
        case NODE_COMPOUND_ASSIGN:   return "compound_assign";
        case NODE_EXPR_STMT:         return "expr_stmt";
        case NODE_CALL:              return "call";
        case NODE_FIELD:             return "field";
        case NODE_INDEX:             return "index";
        case NODE_PROGRAM:           return "program";
        case NODE_VAR_DECL:          return "var_decl";
        case NODE_FUNC_DECL:         return "func_decl";
        case NODE_FUNC_PARAM_DECL:   return "param_decl";
        case NODE_STRUCT_DECL:       return "struct_decl";
        case NODE_STRUCT_FIELD_DECL: return "struct_field_decl";
        case NODE_ENUM_DECL:         return "enum_decl";
        case NODE_ENUM_MEMBER:       return "enum_member";
        case NODE_STRUCT_INIT:       return "struct_init";
        case NODE_FIELD_INIT:        return "field_init";
        case NODE_CONST_DECL:        return "const_decl";
        case NODE_ARRAY_LITERAL:     return "array_literal";
        case NODE_IF:                return "if";
        case NODE_SWITCH:            return "switch";
        case NODE_SWITCH_CASE:       return "switch_case";
        case NODE_RETURN:            return "return";
        case NODE_WHILE:             return "while";
        case NODE_FOR:               return "for";
        case NODE_BREAK:             return "break";
        case NODE_CONTINUE:          return "continue";
        case NODE_ERROR:             return "error";
    }

    return "<unknown-node>";
}

static const char *type_kind_name(TypeKind kind)
{
    switch (kind) {
        case TYPE_VOID:     return "void";
        case TYPE_BOOL:     return "bool";
        case TYPE_I8:       return "i8";
        case TYPE_I16:      return "i16";
        case TYPE_I32:      return "i32";
        case TYPE_I64:      return "i64";
        case TYPE_U8:       return "u8";
        case TYPE_U16:      return "u16";
        case TYPE_U32:      return "u32";
        case TYPE_U64:      return "u64";
        case TYPE_F32:      return "f32";
        case TYPE_F64:      return "f64";
        case TYPE_POINTER:  return "pointer";
        case TYPE_ARRAY:    return "array";
        case TYPE_STRUCT:   return "struct";
        case TYPE_ENUM:     return "enum";
        case TYPE_FUNCTION: return "function";
        case TYPE_NAMED:    return "named";
        case TYPE_UNTYPED_INT: return "untyped-int";
        case TYPE_UNTYPED_FLOAT: return "untyped-float";
    }

    return "<unknown-type>";
}

static int node_is_expression(NodeType type)
{
    switch (type) {
        case NODE_NUMBER:
        case NODE_IDENT:
        case NODE_STRING:
        case NODE_CHAR:
        case NODE_BOOL:
        case NODE_CAST:
        case NODE_UNARY:
        case NODE_BINARY:
        case NODE_INC_DEC:
        case NODE_ASSIGN:
        case NODE_COMPOUND_ASSIGN:
        case NODE_CALL:
        case NODE_FIELD:
        case NODE_INDEX:
        case NODE_STRUCT_INIT:
        case NODE_ARRAY_LITERAL:
            return 1;

        default:
            return 0;
    }
}

static int node_is_mutation(NodeType type)
{
    return type == NODE_ASSIGN ||
           type == NODE_COMPOUND_ASSIGN ||
           type == NODE_INC_DEC;
}

static int node_can_be_lvalue(NodeType type)
{
    return type == NODE_IDENT ||
           type == NODE_FIELD ||
           type == NODE_INDEX;
}

static void expression_list_destroy(ExpressionList *list)
{
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int expression_list_push(ExpressionList *list, Node *expression)
{
    if (list->count == list->capacity) {
        int new_capacity = list->capacity > 0
            ? list->capacity * 2
            : 64;

        Node **new_items = realloc(
            list->items,
            sizeof(*new_items) * (size_t)new_capacity
        );

        if (!new_items)
            return 0;

        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->count++] = expression;
    return 1;
}

static int expression_list_contains(
    const ExpressionList *list,
    const Node *expression
) {
    for (int i = 0; i < list->count; i++) {
        if (list->items[i] == expression)
            return 1;
    }

    return 0;
}

static void verifier_error(Verifier *verifier,const Node *node,const char *format, ...) {

    FILE *output = verifier->diagnostics
        ? verifier->diagnostics
        : stderr;

    fprintf(
        output,
        "semantic-info verification failed"
    );

    if (node) {
        fprintf(
            output,
            " at line %d (%s)",
            node->line,
            node_type_name(node->type)
        );
    }

    fputs(": ", output);

    va_list args;
    va_start(args, format);
    vfprintf(output, format, args);
    va_end(args);

    fputc('\n', output);
    verifier->error_count++;
}

static int field_uses_enum_type_qualifier(SemanticContext *sem, Node *field) {
    if (!field ||
        field->type != NODE_FIELD ||
        !field->as.field.object ||
        field->as.field.object->type != NODE_IDENT) {
        return 0;
    }

    SemExprInfo *info = semantic_get_expr_info(sem, field);

    return info &&
           info->symbol &&
           info->symbol->kind == SYMBOL_TYPE &&
           info->type &&
           info->type->kind == TYPE_ENUM;
}

static void walk_expression(ExpressionWalker *walker, Node *expression);
static void walk_node(ExpressionWalker *walker, Node *node);

static void walk_expression_list(
    ExpressionWalker *walker,
    NodeList *list
) {
    if (!list)
        return;

    for (int i = 0; i < list->count; i++)
        walk_expression(walker, list->items[i]);
}

static void walk_node_list(
    ExpressionWalker *walker,
    NodeList *list
) {
    if (!list)
        return;

    for (int i = 0; i < list->count; i++)
        walk_node(walker, list->items[i]);
}

static void walk_expression(ExpressionWalker *walker, Node *expression)
{
    if (!expression)
        return;

    walker->visit(walker->context, expression);

    switch (expression->type) {
        case NODE_CAST:
            walk_expression(
                walker,
                expression->as.cast_expr.expression
            );
            break;

        case NODE_UNARY:
            walk_expression(
                walker,
                expression->as.unary.operand
            );
            break;

        case NODE_BINARY:
            walk_expression(
                walker,
                expression->as.binary.left
            );
            walk_expression(
                walker,
                expression->as.binary.right
            );
            break;

        case NODE_INC_DEC:
            walk_expression(
                walker,
                expression->as.inc_dec.target
            );
            break;

        case NODE_ASSIGN:
            walk_expression(
                walker,
                expression->as.assign.target
            );
            walk_expression(
                walker,
                expression->as.assign.value
            );
            break;

        case NODE_COMPOUND_ASSIGN:
            walk_expression(
                walker,
                expression->as.compound_assign.target
            );
            walk_expression(
                walker,
                expression->as.compound_assign.value
            );
            break;

        case NODE_CALL:
            walk_expression(
                walker,
                expression->as.call.callee
            );
            walk_expression_list(
                walker,
                &expression->as.call.arguments
            );
            break;

        case NODE_FIELD:
            /*
             * In Color.Red, Color is a type qualifier rather than a value
             * expression. Semantic analysis intentionally records the field
             * expression but not the qualifier identifier.
             */
            if (!field_uses_enum_type_qualifier(
                    walker->sem,
                    expression)) {
                walk_expression(
                    walker,
                    expression->as.field.object
                );
            }
            break;

        case NODE_INDEX:
            walk_expression(
                walker,
                expression->as.index.object
            );
            walk_expression(
                walker,
                expression->as.index.index
            );
            break;

        case NODE_STRUCT_INIT:
            for (int i = 0;
                 i < expression->as.struct_init.fields.count;
                 i++) {
                Node *field_init =
                    expression->as.struct_init.fields.items[i];

                if (field_init && field_init->type == NODE_FIELD_INIT) {
                    walk_expression(
                        walker,
                        field_init->as.field_init.value
                    );
                }
            }
            break;

        case NODE_ARRAY_LITERAL:
            walk_expression_list(
                walker,
                &expression->as.array_literal.elements
            );
            break;

        case NODE_NUMBER:
        case NODE_IDENT:
        case NODE_STRING:
        case NODE_CHAR:
        case NODE_BOOL:
        default:
            break;
    }
}

static void walk_node(ExpressionWalker *walker, Node *node)
{
    if (!node)
        return;

    switch (node->type) {
        case NODE_PROGRAM:
            walk_node_list(
                walker,
                &node->as.program.statements
            );
            break;

        case NODE_BLOCK:
            walk_node_list(
                walker,
                &node->as.block.statements
            );
            break;

        case NODE_VAR_DECL:
            walk_expression(
                walker,
                node->as.var_decl.initializer
            );
            break;

        case NODE_FUNC_PARAM_DECL:
            walk_expression(
                walker,
                node->as.param_decl.default_value
            );
            break;

        case NODE_FUNC_DECL:
            walk_node_list(
                walker,
                &node->as.func_decl.params
            );
            walk_node(
                walker,
                node->as.func_decl.body
            );
            break;

        case NODE_STRUCT_DECL:
        case NODE_STRUCT_FIELD_DECL:
            break;

        case NODE_ENUM_DECL:
            walk_node_list(
                walker,
                &node->as.enum_decl.members
            );
            break;

        case NODE_ENUM_MEMBER:
            walk_expression(
                walker,
                node->as.enum_member.value
            );
            break;

        case NODE_CONST_DECL:
            walk_expression(
                walker,
                node->as.const_decl.value
            );
            break;

        case NODE_EXPR_STMT:
            walk_expression(
                walker,
                node->as.expr_stmt.expr
            );
            break;

        case NODE_IF:
            walk_expression(
                walker,
                node->as.if_stmt.condition
            );
            walk_node(
                walker,
                node->as.if_stmt.then_branch
            );
            walk_node(
                walker,
                node->as.if_stmt.else_branch
            );
            break;

        case NODE_SWITCH:
            walk_expression(
                walker,
                node->as.switch_stmt.expression
            );
            walk_node_list(
                walker,
                &node->as.switch_stmt.cases
            );
            break;

        case NODE_SWITCH_CASE:
            if (!node->as.switch_case.is_default) {
                walk_expression(
                    walker,
                    node->as.switch_case.value
                );
            }
            walk_node(
                walker,
                node->as.switch_case.body
            );
            break;

        case NODE_RETURN:
            walk_expression(
                walker,
                node->as.return_stmt.value
            );
            break;

        case NODE_WHILE:
            walk_expression(
                walker,
                node->as.while_stmt.condition
            );
            walk_node(
                walker,
                node->as.while_stmt.body
            );
            break;

        case NODE_FOR:
            walk_expression(
                walker,
                node->as.for_stmt.condition
            );
            walk_expression(
                walker,
                node->as.for_stmt.post
            );
            walk_node(
                walker,
                node->as.for_stmt.body
            );
            break;

        case NODE_NUMBER:
        case NODE_IDENT:
        case NODE_STRING:
        case NODE_CHAR:
        case NODE_BOOL:
        case NODE_CAST:
        case NODE_UNARY:
        case NODE_BINARY:
        case NODE_INC_DEC:
        case NODE_ASSIGN:
        case NODE_COMPOUND_ASSIGN:
        case NODE_CALL:
        case NODE_FIELD:
        case NODE_INDEX:
        case NODE_STRUCT_INIT:
        case NODE_ARRAY_LITERAL:
            walk_expression(walker, node);
            break;

        case NODE_FIELD_INIT:
            walk_expression(
                walker,
                node->as.field_init.value
            );
            break;

        case NODE_BREAK:
        case NODE_CONTINUE:
        case NODE_ERROR:
            break;
    }
}

static void collect_expression(void *context, Node *expression)
{
    Verifier *verifier = context;

    if (!node_is_expression(expression->type)) {
        verifier_error(
            verifier,
            expression,
            "AST walker classified a non-expression node as an expression"
        );
        return;
    }

    if (expression_list_contains(
            &verifier->expressions,
            expression)) {
        verifier_error(
            verifier,
            expression,
            "AST contains the same expression node more than once"
        );
        return;
    }

    if (!expression_list_push(
            &verifier->expressions,
            expression)) {
        verifier_error(
            verifier,
            expression,
            "out of memory while collecting expression nodes"
        );
        return;
    }

    if (node_is_mutation(expression->type))
        verifier->mutation_count++;
}

static int verify_mutation_info(
    Verifier *verifier,
    Node *expression,
    SemExprInfo *info
) {
    int valid = 1;

    if (info->type) {
        verifier_error(
            verifier,
            expression,
            "statement-only mutation has type %s",
            type_kind_name(info->type->kind)
        );
        valid = 0;
    }

    if (info->symbol) {
        verifier_error(
            verifier,
            expression,
            "statement-only mutation unexpectedly has a resolved symbol"
        );
        valid = 0;
    }

    if (info->value_category != VALUE_CATEGORY_NONE) {
        verifier_error(
            verifier,
            expression,
            "statement-only mutation has category %s instead of none",
            value_category_name(info->value_category)
        );
        valid = 0;
    }

    return valid;
}

static int verify_value_info(
    Verifier *verifier,
    Node *expression,
    SemExprInfo *info
) {
    int valid = 1;

    if (!info->type) {
        verifier_error(
            verifier,
            expression,
            "value expression has no type"
        );
        return 0;
    }

    if (info->value_category == VALUE_CATEGORY_NONE) {
        int is_void_call =
            expression->type == NODE_CALL &&
            info->type->kind == TYPE_VOID;

        if (!is_void_call) {
            verifier_error(
                verifier,
                expression,
                "typed expression has category none"
            );
            valid = 0;
        }
    } else if (info->value_category != VALUE_CATEGORY_RVALUE &&
               info->value_category != VALUE_CATEGORY_LVALUE) {
        verifier_error(
            verifier,
            expression,
            "expression has invalid value category %d",
            (int)info->value_category
        );
        valid = 0;
    }

    if (info->value_category == VALUE_CATEGORY_LVALUE &&
        !node_can_be_lvalue(expression->type)) {
        verifier_error(
            verifier,
            expression,
            "node kind cannot produce an lvalue"
        );
        valid = 0;
    }

    if (expression->type == NODE_IDENT) {
        if (!info->symbol) {
            verifier_error(
                verifier,
                expression,
                "resolved identifier has no symbol"
            );
            valid = 0;
        } else if (info->symbol->type != info->type) {
            verifier_error(
                verifier,
                expression,
                "identifier type does not match its symbol type"
            );
            valid = 0;
        }
    } else if (info->symbol) {
        int is_enum_member =
            expression->type == NODE_FIELD &&
            info->symbol->kind == SYMBOL_TYPE &&
            info->type->kind == TYPE_ENUM;

        if (!is_enum_member) {
            verifier_error(
                verifier,
                expression,
                "only identifiers and enum member expressions may carry symbols"
            );
            valid = 0;
        }
    }

    return valid;
}

static void verify_expression_info(
    Verifier *verifier,
    Node *expression
) {
    SemExprInfo *info = semantic_get_expr_info(
        verifier->sem,
        expression
    );

    if (!info) {
        verifier_error(
            verifier,
            expression,
            "successfully checked expression has no SemExprInfo"
        );
        return;
    }

    if (info->node != expression) {
        verifier_error(
            verifier,
            expression,
            "SemExprInfo points to a different AST node"
        );
        return;
    }

    if (node_is_mutation(expression->type)) {
        verify_mutation_info(
            verifier,
            expression,
            info
        );
        return;
    }

    verify_value_info(
        verifier,
        expression,
        info
    );
}

static void verify_table_entries(Verifier *verifier)
{
    for (SemExprInfo *info = verifier->sem->expr_infos;
         info;
         info = info->next) {
        verifier->table_entry_count++;

        if (!info->node) {
            verifier_error(
                verifier,
                NULL,
                "semantic-info table contains an entry with a null node"
            );
            continue;
        }

        if (!node_is_expression(info->node->type)) {
            verifier_error(
                verifier,
                info->node,
                "semantic-info entry belongs to a non-expression node"
            );
        }

        if (!expression_list_contains(
                &verifier->expressions,
                info->node)) {
            verifier_error(
                verifier,
                info->node,
                "semantic-info entry does not belong to the program AST"
            );
        }

        for (SemExprInfo *other = info->next;
             other;
             other = other->next) {
            if (other->node == info->node) {
                verifier_error(
                    verifier,
                    info->node,
                    "semantic-info table contains duplicate entries for one AST node"
                );
                break;
            }
        }
    }
}

static void dump_expression(void *context, Node *expression)
{
    DumpContext *dump = context;
    SemExprInfo *info = semantic_get_expr_info(
        dump->sem,
        expression
    );

    const char *type_name     = "<missing>";
    const char *category_name = "<missing>";
    const char *symbol_name   = "<missing>";

    if (info) {
        type_name = info->type
            ? type_kind_name(info->type->kind)
            : "<none>";
        category_name = value_category_name(
            info->value_category
        );
        symbol_name = info->symbol ? "yes" : "no";
    }

    fprintf(
        dump->output,
        "  %4d  line %-4d node=%-18s type=%-18s category=%-6s symbol=%s\n",
        dump->count,
        expression->line,
        node_type_name(expression->type),
        type_name,
        category_name,
        symbol_name
    );

    dump->count++;
}

void semantic_info_dump_program(
    SemanticContext *sem,
    Node *program,
    FILE *output
) {
    if (!output)
        output = stderr;

    fputs("\nSemantic-info expressions in source order:\n", output);

    if (!sem || !program) {
        fputs("  <unavailable>\n", output);
        return;
    }

    DumpContext dump;

    dump.sem = sem;
    dump.output = output;
    dump.count = 1;

    ExpressionWalker walker;
    walker.sem = sem;
    walker.visit = dump_expression;
    walker.context = &dump;

    walk_node(&walker, program);

    fprintf(
        output,
        "  %d expression%s listed\n",
        dump.count - 1,
        dump.count == 2 ? "" : "s"
    );
}

int semantic_info_verify_program(
    SemanticContext *sem,
    Node *program,
    FILE *diagnostics,
    SemanticInfoVerification *verification
) {
    if (verification)
        memset(verification, 0, sizeof(*verification));

    if (!sem || !program) {
        if (diagnostics) {
            fputs(
                "semantic-info verification failed: "
                "missing semantic context or program AST\n",
                diagnostics
            );
        }

        if (verification)
            verification->error_count = 1;

        return 0;
    }

    Verifier verifier = {0};

    verifier.sem = sem;
    verifier.diagnostics = diagnostics
        ? diagnostics
        : stderr;

    ExpressionWalker walker;
    walker.sem = sem;
    walker.visit = collect_expression;
    walker.context = &verifier;

    walk_node(&walker, program);

    for (int i = 0; i < verifier.expressions.count; i++) {
        verify_expression_info(
            &verifier,
            verifier.expressions.items[i]
        );
    }

    verify_table_entries(&verifier);

    if (verifier.table_entry_count != verifier.expressions.count) {
        verifier_error(
            &verifier,
            NULL,
            "table has %d entries but AST has %d checked expressions",
            verifier.table_entry_count,
            verifier.expressions.count
        );
    }

    if (verification) {
        verification->expression_count = verifier.expressions.count;
        verification->mutation_count = verifier.mutation_count;
        verification->table_entry_count = verifier.table_entry_count;
        verification->error_count = verifier.error_count;
    }

    int succeeded = verifier.error_count == 0;
    expression_list_destroy(&verifier.expressions);

    return succeeded;
}
