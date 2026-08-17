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
typedef void (*DeclarationVisitor)(void *context, Node *declaration);

typedef struct ExpressionWalker {
    SemanticContext *sem;
    ExpressionVisitor visit;
    DeclarationVisitor visit_declaration;
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
    ExpressionList declarations;
    int mutation_count;
    int table_entry_count;
    int declaration_table_entry_count;
    int error_count;
} Verifier;

typedef struct DumpContext {
    SemanticContext *sem;
    FILE *output;
    int count;
} DumpContext;

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
        case NODE_NULL:              return "null";
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
        case NODE_MODULE_DECL:       return "module_decl";
        case NODE_IMPORT_DECL:       return "import_decl";
        case NODE_VAR_DECL:          return "var_decl";
        case NODE_VAR_DECL_GROUP:    return "var_decl_group";
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
        case TYPE_OPAQUE_POINTER: return "opaque-pointer";
        case TYPE_ARRAY:    return "array";
        case TYPE_STRUCT:   return "struct";
        case TYPE_ENUM:     return "enum";
        case TYPE_FUNCTION: return "function";
        case TYPE_NAMED:    return "named";
        case TYPE_NULL:     return "null";
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
        case NODE_NULL:
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

static int node_is_declaration(NodeType type)
{
    switch (type) {
        case NODE_VAR_DECL:
        case NODE_FUNC_DECL:
        case NODE_FUNC_PARAM_DECL:
        case NODE_STRUCT_DECL:
        case NODE_STRUCT_FIELD_DECL:
        case NODE_ENUM_DECL:
        case NODE_ENUM_MEMBER:
        case NODE_CONST_DECL:
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

static int node_can_be_lvalue(const Node *node) {
    if (!node)
        return 0;

    if (node->type == NODE_IDENT ||
        node->type == NODE_FIELD ||
        node->type == NODE_INDEX) {
        return 1;
    }

    return node->type == NODE_UNARY &&
           node->as.unary.op == TOK_STAR;
}

static void expression_list_destroy(ExpressionList *list) {

    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int expression_list_push(ExpressionList *list, Node *expression) {

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

static int expression_list_contains(const ExpressionList *list, const Node *expression) {

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

    fprintf(output, "semantic-info verification failed");

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

static int field_uses_declaration_qualifier(SemanticContext *sem, Node *field) {
    if (!field || field->type != NODE_FIELD)
        return 0;

    SemExprInfo *info = semantic_get_expr_info(sem, field);

    /*
     * Module-qualified declarations and enum members are represented with the
     * ordinary field AST shape, but their prefix is namespace/type syntax rather
     * than a runtime value expression. Semantic analysis therefore records the
     * resolved declaration on the complete field expression and intentionally
     * does not create SemExprInfo for intermediate namespace components.
     */
    return info && info->symbol;
}

static void walk_expression(ExpressionWalker *walker, Node *expression);
static void walk_node(ExpressionWalker *walker, Node *node);

static void walk_expression_list(ExpressionWalker *walker, NodeList *list) {

    if (!list) return;

    for (int i = 0; i < list->count; i++)
        walk_expression(walker, list->items[i]);
}

static void walk_node_list(ExpressionWalker *walker, NodeList *list) {

    if (!list) return;

    for (int i = 0; i < list->count; i++)
        walk_node(walker, list->items[i]);
}

static void walk_expression(ExpressionWalker *walker, Node *expression)
{
    if (!expression) return;

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
             * Namespace/type qualifiers are not runtime value expressions.
             * Semantic analysis records the resolved declaration on the full
             * field expression but not the qualifier path beneath it.
             */
            if (!field_uses_declaration_qualifier(
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
            for (int i = 0; i < expression->as.struct_init.fields.count; i++) {
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
        case NODE_NULL:
        default:
            break;
    }
}

static void walk_node(ExpressionWalker *walker, Node *node)
{
    if (!node)
        return;

    if (walker->visit_declaration && node_is_declaration(node->type))
        walker->visit_declaration(walker->context, node);

    switch (node->type) {
        case NODE_PROGRAM:
            walk_node_list(
                walker,
                &node->as.program.statements
            );
            break;

        case NODE_MODULE_DECL:
        case NODE_IMPORT_DECL:
            /* Compile-time namespace metadata has no semantic expression info. */
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

        case NODE_VAR_DECL_GROUP:
            walk_node_list(walker, &node->as.var_decl_group.declarations);
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
            walk_node_list(
                walker,
                &node->as.struct_decl.fields
            );
            break;

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
        case NODE_NULL:
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
        verifier_error(verifier,expression,
            "AST walker classified a non-expression node as an expression");

        return;
    }

    if (expression_list_contains(&verifier->expressions, expression)) {
        verifier_error(verifier, expression,
            "AST contains the same expression node more than once");

        return;
    }

    if (!expression_list_push(&verifier->expressions, expression)) {
        verifier_error(verifier, expression,
            "out of memory while collecting expression nodes");

        return;
    }

    if (node_is_mutation(expression->type))
        verifier->mutation_count++;
}

static void collect_declaration(void *context, Node *declaration)
{
    Verifier *verifier = context;

    if (!node_is_declaration(declaration->type)) {
        verifier_error(verifier, declaration,
            "AST walker classified a non-declaration node as a declaration");
        return;
    }

    if (expression_list_contains(&verifier->declarations, declaration)) {
        verifier_error(verifier, declaration,
            "AST contains the same declaration node more than once");
        return;
    }

    if (!expression_list_push(&verifier->declarations, declaration)) {
        verifier_error(verifier, declaration,
            "out of memory while collecting declaration nodes");
    }
}

static const char *value_access_name(ValueAccess access) {

    switch (access) {
        case VALUE_ACCESS_NONE:
            return "none";

        case VALUE_ACCESS_READONLY:
            return "readonly";

        case VALUE_ACCESS_WRITABLE:
            return "writable";
    }

    return "<invalid>";
}

static const char *context_conversion_name(SemContextConversionKind conversion) {
    switch (conversion) {
        case SEM_CONTEXT_CONVERSION_NONE:
            return "none";
        case SEM_CONTEXT_CONVERSION_INT_MATERIALIZE:
            return "int-materialize";
        case SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE:
            return "int-to-float";
        case SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE:
            return "float-materialize";
        case SEM_CONTEXT_CONVERSION_NULL_TO_POINTER:
            return "null-to-pointer";
        case SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION:
            return "pointer-qualification";
        case SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER:
            return "c-string-to-pointer";
    }

    return "<invalid>";
}

static int is_concrete_integer_type_kind(TypeKind kind) {
    switch (kind) {
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
            return 1;

        default:
            return 0;
    }
}

static int is_concrete_float_type_kind(TypeKind kind) {
    return kind == TYPE_F32 || kind == TYPE_F64;
}

static int is_raw_pointer_type_kind(TypeKind kind) {
    return kind == TYPE_POINTER || kind == TYPE_OPAQUE_POINTER;
}

static int verify_context_conversion_info(
    Verifier *verifier,
    Node *expression,
    SemExprInfo *info
) {
    SemContextConversionKind conversion = info->contextual_conversion;
    Type *target = info->contextual_type;

    if (conversion == SEM_CONTEXT_CONVERSION_NONE) {
        if (target) {
            verifier_error(verifier, expression,
                "expression has contextual type %s but no contextual conversion",
                type_kind_name(target->kind));
            return 0;
        }

        if (semantic_get_effective_expr_type(verifier->sem, expression) != info->type) {
            verifier_error(verifier, expression,
                "effective expression type differs without a contextual conversion");
            return 0;
        }

        return 1;
    }

    if (!target || !info->type) {
        verifier_error(verifier, expression,
            "contextual conversion %s is missing source or destination type",
            context_conversion_name(conversion));
        return 0;
    }

    if (semantic_get_effective_expr_type(verifier->sem, expression) != target) {
        verifier_error(verifier, expression,
            "effective expression type does not use the recorded contextual destination");
        return 0;
    }

    int valid = 1;

    switch (conversion) {
        case SEM_CONTEXT_CONVERSION_NONE:
            break;

        case SEM_CONTEXT_CONVERSION_INT_MATERIALIZE:
            if (info->type->kind != TYPE_UNTYPED_INT ||
                !is_concrete_integer_type_kind(target->kind)) {
                verifier_error(verifier, expression,
                    "int materialization does not map untyped-int to a concrete integer");
                valid = 0;
            }
            break;

        case SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE:
            if (info->type->kind != TYPE_UNTYPED_INT ||
                !is_concrete_float_type_kind(target->kind)) {
                verifier_error(verifier, expression,
                    "integer-to-float materialization has invalid source or destination");
                valid = 0;
            }
            break;

        case SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE:
            if (info->type->kind != TYPE_UNTYPED_FLOAT ||
                !is_concrete_float_type_kind(target->kind)) {
                verifier_error(verifier, expression,
                    "float materialization does not map untyped-float to a concrete float");
                valid = 0;
            }
            break;

        case SEM_CONTEXT_CONVERSION_NULL_TO_POINTER:
            if (info->type->kind != TYPE_NULL ||
                !(is_raw_pointer_type_kind(target->kind) ||
                  (target->kind == TYPE_FUNCTION &&
                   target->function_abi == FUNCTION_ABI_C))) {
                verifier_error(verifier, expression,
                    "null contextualization does not target a nullable raw pointer/cfn");
                valid = 0;
            }
            break;

        case SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION:
            if (!is_raw_pointer_type_kind(info->type->kind) ||
                info->type->kind != target->kind) {
                verifier_error(verifier, expression,
                    "pointer qualification conversion changes pointer family");
                valid = 0;
                break;
            }

            if (info->type->pointer_access == POINTER_ACCESS_READONLY &&
                target->pointer_access == POINTER_ACCESS_MUTABLE) {
                verifier_error(verifier, expression,
                    "pointer qualification conversion removes readonly access");
                valid = 0;
            }

            if (info->type->pointer_is_volatile &&
                !target->pointer_is_volatile) {
                verifier_error(verifier, expression,
                    "pointer qualification conversion removes volatile access");
                valid = 0;
            }
            break;

        case SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER:
            if (expression->type != NODE_STRING ||
                target->kind != TYPE_POINTER ||
                target->pointer_access != POINTER_ACCESS_READONLY) {
                verifier_error(verifier, expression,
                    "C-string contextualization does not target a readonly typed pointer");
                valid = 0;
            }
            break;
    }

    return valid;
}

static int pointer_access_to_value_access(PointerAccess pointer_access, ValueAccess *out) {

    if (!out) return 0;

    switch (pointer_access) {
        case POINTER_ACCESS_MUTABLE:
            *out = VALUE_ACCESS_WRITABLE;
            return 1;

        case POINTER_ACCESS_READONLY:
            *out = VALUE_ACCESS_READONLY;
            return 1;
    }

    return 0;
}

static int value_access_to_pointer_access(ValueAccess value_access,PointerAccess *out) {

    if (!out) return 0;

    switch (value_access) {
        case VALUE_ACCESS_WRITABLE:
            *out = POINTER_ACCESS_MUTABLE;
            return 1;

        case VALUE_ACCESS_READONLY:
            *out = POINTER_ACCESS_READONLY;
            return 1;

        case VALUE_ACCESS_NONE:
            return 0;
    }

    return 0;
}

static int verify_mutation_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    int valid = 1;

    if (info->type) {
        verifier_error(verifier, expression,
            "statement-only mutation has type %s",
            type_kind_name(info->type->kind));

        valid = 0;
    }

    if (info->symbol) {
        verifier_error(verifier, expression,
            "statement-only mutation unexpectedly has a resolved symbol");

        valid = 0;
    }

    if (info->value_category != VALUE_CATEGORY_NONE) {
        verifier_error(verifier, expression,
            "statement-only mutation has category %s instead of none",
            value_category_name(info->value_category));

        valid = 0;
    }

    if (info->value_access != VALUE_ACCESS_NONE) {
        verifier_error(verifier, expression,
            "statement-only mutation has access %s instead of none",
            value_access_name(info->value_access));

        valid = 0;
    }

    if (!verify_context_conversion_info(verifier, expression, info))
        valid = 0;

    return valid;
}

static int verify_pointer_unary_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    if (expression->type != NODE_UNARY)
        return 1;

    TokenType op =
        expression->as.unary.op;

    if (op != TOK_AND &&
        op != TOK_STAR) {
        return 1;
    }

    Node *operand =
        expression->as.unary.operand;

    SemExprInfo *operand_info =
        semantic_get_expr_info(verifier->sem, operand);

    if (!operand_info ||
        !operand_info->type) {
        verifier_error(verifier, expression,
            "pointer unary operand has no semantic type");

        return 0;
    }

    int valid = 1;

    /*
     * Address-of:
     *
     *     &writable_lvalue -> mutable pointer
     *     &readonly_lvalue -> readonly pointer
     */
    if (op == TOK_AND) {
        if (operand_info->value_category !=
            VALUE_CATEGORY_LVALUE) {
            verifier_error(verifier, expression,
                "address-of operand is not an lvalue");

            valid = 0;
        }

        if (!info->type ||
            info->type->kind != TYPE_POINTER ||
            info->type->element !=
                operand_info->type) {
            verifier_error(
                verifier,
                expression,
                "address-of result is not a pointer "
                "to the operand type"
            );

            valid = 0;
        }

        if (info->value_category !=
            VALUE_CATEGORY_RVALUE) {
            verifier_error(verifier, expression,
                "address-of result is not an rvalue");

            valid = 0;
        }

        PointerAccess expected_pointer_access;

        if (!value_access_to_pointer_access(
                operand_info->value_access,
                &expected_pointer_access
            )) {
            verifier_error(
                verifier,
                expression,
                "address-of operand has invalid storage access %s",
                value_access_name(
                    operand_info->value_access
                )
            );

            valid = 0;
        } else if (
            info->type &&
            info->type->kind == TYPE_POINTER &&
            info->type->pointer_access !=
                expected_pointer_access
        ) {
            verifier_error(verifier, expression,
                "address-of result does not preserve operand storage access");

            valid = 0;
        }

        if (info->type &&
            info->type->kind == TYPE_POINTER &&
            info->type->pointer_is_volatile != operand_info->value_is_volatile) {
            verifier_error(verifier, expression,
                "address-of result does not preserve operand volatile access");
            valid = 0;
        }

        return valid;
    }

    /*
     * Dereference:
     *
     *     *T*            -> writable lvalue
     *     *readonly T*   -> readonly lvalue
     */
    if (operand_info->type->kind !=
        TYPE_POINTER) {
        verifier_error(verifier, expression,
            "dereference operand is not a pointer");

        return 0;
    }

    if (info->type !=
        operand_info->type->element) {
        verifier_error(verifier, expression,
            "dereference result does not match the pointer element type");

        valid = 0;
    }

    if (info->value_category !=
        VALUE_CATEGORY_LVALUE) {
        verifier_error(verifier, expression,
            "dereference result is not an lvalue");

        valid = 0;
    }

    ValueAccess expected_value_access;

    if (!pointer_access_to_value_access(
            operand_info->type->pointer_access,
            &expected_value_access
        )) {
        verifier_error(verifier,expression,
            "dereference operand has invalid pointer access");

        valid = 0;
    } else if (
        info->value_access !=
            expected_value_access
    ) {
        verifier_error(
            verifier,
            expression,
            "dereference result has access %s instead of %s",
            value_access_name(info->value_access),
            value_access_name(expected_value_access));

        valid = 0;
    }

    if (info->value_is_volatile != operand_info->type->pointer_is_volatile) {
        verifier_error(verifier, expression,
            "dereference result does not preserve volatile pointer access");
        valid = 0;
    }

    return valid;
}

static int verify_field_access_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    if (expression->type != NODE_FIELD)
        return 1;

    /*
     * Color.Red is an enum member, not runtime storage.
     */
    if (field_uses_declaration_qualifier(verifier->sem, expression))
        return 1;

    Node *object =
        expression->as.field.object;

    SemExprInfo *object_info =
        semantic_get_expr_info(verifier->sem, object);

    if (!object_info || !object_info->type) {
        verifier_error(verifier, expression,
            "field object has no semantic information");

        return 0;
    }

    int valid = 1;

    if (object_info->value_category == VALUE_CATEGORY_LVALUE) {
        if (info->value_category != VALUE_CATEGORY_LVALUE) {
            verifier_error(verifier, expression,
                "field of an lvalue is not an lvalue");

            valid = 0;
        }

        if (info->value_access != object_info->value_access) {
            verifier_error(verifier, expression,
                "field access %s does not match object access %s",
                value_access_name(info->value_access),
                value_access_name(object_info->value_access));

            valid = 0;
        }

        if (info->value_is_volatile != object_info->value_is_volatile) {
            verifier_error(verifier, expression,
                "field access does not preserve volatile storage access");
            valid = 0;
        }

    } else {
        if (info->value_category != VALUE_CATEGORY_RVALUE) {
            verifier_error(verifier, expression,
                "field of an rvalue is not an rvalue");

            valid = 0;
        }

        if (info->value_access != VALUE_ACCESS_NONE) {
            verifier_error(verifier, expression,
                "rvalue field has storage access %s",
                value_access_name(info->value_access));

            valid = 0;
        }

        if (info->value_is_volatile) {
            verifier_error(verifier, expression,
                "rvalue field unexpectedly has volatile storage access");
            valid = 0;
        }
    }

    return valid;
}

static int verify_index_access_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    if (expression->type != NODE_INDEX)
        return 1;

    Node *object =
        expression->as.index.object;

    SemExprInfo *object_info =
        semantic_get_expr_info(verifier->sem, object);

    if (!object_info ||
        !object_info->type) {
        verifier_error(verifier, expression,
            "index object has no semantic information");

        return 0;
    }

    Type *object_type =
        object_info->type;

    if (object_type->kind != TYPE_POINTER &&
        object_type->kind != TYPE_ARRAY) {
        verifier_error(verifier, expression,
            "index object is not an array or pointer");

        return 0;
    }

    int valid = 1;

    if (info->type != object_type->element) {
        verifier_error(verifier, expression,
            "index result does not match the object element type");

        valid = 0;
    }

    if (object_type->kind == TYPE_POINTER) {
        ValueAccess expected_access;

        if (!pointer_access_to_value_access(object_type->pointer_access, &expected_access)) {
            verifier_error(verifier, expression,
                "indexed pointer has invalid access");

            return 0;
        }

        if (info->value_category !=
            VALUE_CATEGORY_LVALUE) {
            verifier_error(verifier, expression,
                "pointer index result is not an lvalue");

            valid = 0;
        }

        if (info->value_access !=
            expected_access) {
            verifier_error(verifier, expression,
                "pointer index has access %s instead of %s",
                value_access_name(info->value_access),
                value_access_name(expected_access));

            valid = 0;
        }

        if (info->value_is_volatile != object_type->pointer_is_volatile) {
            verifier_error(verifier, expression,
                "pointer index does not preserve volatile pointer access");
            valid = 0;
        }

        return valid;
    }

    /*
     * Fixed-array indexing inherits the array expression's
     * category and storage access.
     */
    if (object_info->value_category ==
        VALUE_CATEGORY_LVALUE) {
        if (info->value_category !=
            VALUE_CATEGORY_LVALUE) {
            verifier_error(verifier, expression,
                "array lvalue index is not an lvalue");

            valid = 0;
        }

        if (info->value_access !=
            object_info->value_access) {
            verifier_error(verifier, expression,
                "array index access does not match the array expression");

            valid = 0;
        }
    } else {
        if (info->value_category !=
            VALUE_CATEGORY_RVALUE) {
            verifier_error(verifier, expression,
                "temporary array index is not an rvalue");

            valid = 0;
        }

        if (info->value_access !=
            VALUE_ACCESS_NONE) {
            verifier_error(verifier, expression,
                "temporary array index has storage access");

            valid = 0;
        }
    }

    return valid;
}

static int verify_null_cast_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    if (expression->type != NODE_CAST) return 1;

    Node *source =
        expression->as.cast_expr.expression;

    if (!source || source->type != NODE_NULL)
        return 1;

    SemExprInfo *source_info =
        semantic_get_expr_info(verifier->sem, source);

    if (!source_info || !source_info->type) {
        verifier_error(verifier, expression,
            "null cast operand has no semantic type");

        return 0;
    }

    int valid = 1;

    /*
     * The literal itself retains its contextual pseudo-type. The cast
     * expression carries the concrete pointer type.
     */
    if (source_info->type != verifier->sem->type_null) {
        verifier_error(verifier, source,
            "null cast operand does not use the canonical null type");

        valid = 0;
    }

    if (source_info->value_category !=
        VALUE_CATEGORY_RVALUE) {
        verifier_error(verifier, source,
            "null cast operand is not an rvalue");

        valid = 0;
    }

    if (!info->type ||
        (info->type->kind != TYPE_POINTER &&
         info->type->kind != TYPE_OPAQUE_POINTER)) {
        verifier_error(verifier, expression,
            "null cast result is not a concrete pointer type");

        valid = 0;
    }

    if (info->value_category !=
        VALUE_CATEGORY_RVALUE) {
        verifier_error(verifier, expression,
            "null cast result is not an rvalue");

        valid = 0;
    }

    return valid;
}

static int value_access_is_valid(ValueAccess access) {

    switch (access) {
        case VALUE_ACCESS_NONE:
        case VALUE_ACCESS_READONLY:
        case VALUE_ACCESS_WRITABLE:
            return 1;
    }

    return 0;
}

static int verify_value_info(Verifier *verifier, Node *expression, SemExprInfo *info) {

    int valid = 1;

    if (!verify_context_conversion_info(verifier, expression, info))
        valid = 0;

    if (info->has_constant_value) {
        ConstValue value;
        Type *effective_type =
            semantic_get_effective_expr_type(verifier->sem, expression);

        if (!semantic_get_constant_value(
                verifier->sem,
                expression,
                &value)) {
            verifier_error(verifier, expression,
                "cached constant expression is not retrievable through semantic API");
            valid = 0;
        } else if (effective_type && value.type != effective_type) {
            verifier_error(verifier, expression,
                "retrieved constant type does not match effective expression type");
            valid = 0;
        }
    }

    if (!value_access_is_valid(info->value_access)) {
        verifier_error(verifier, expression,
            "expression has invalid value access %d",
            (int)info->value_access);

        valid = 0;
    }

    /*
    * Builtin identifiers are resolved call targets rather than
     * first-class function values.
    */
    if (expression->type == NODE_IDENT &&
        info->symbol &&
        info->symbol->kind == SYMBOL_BUILTIN) {

        if (info->type != NULL) {
            verifier_error(verifier, expression,
                "builtin callee identifier unexpectedly has a type");

            valid = 0;
        }

        if (info->value_category != VALUE_CATEGORY_NONE) {
            verifier_error(verifier, expression,
                "builtin callee identifier has a value category");

            valid = 0;
        }

        return valid;
    }

    if (!info->type) {
        verifier_error(verifier, expression,
            "value expression has no type");
        return 0;
    }

    if (info->value_category == VALUE_CATEGORY_NONE) {
        int is_void_call =
            expression->type == NODE_CALL &&
            info->type->kind == TYPE_VOID;

        if (!is_void_call) {
            verifier_error(verifier, expression,
                "typed expression has category none");
            valid = 0;
        }
    } else if (info->value_category != VALUE_CATEGORY_RVALUE &&
               info->value_category != VALUE_CATEGORY_LVALUE) {
        verifier_error(verifier,expression,
            "expression has invalid value category %d",
            (int)info->value_category);
        valid = 0;
    }

    /*
    *  *p may be lvalue
    *  -x never lvalue
    *  !x never lvalue
    *  &x never lvalue
    */
    if (info->value_category == VALUE_CATEGORY_LVALUE &&
        !node_can_be_lvalue(expression)) {
        verifier_error(verifier, expression,
            "node kind cannot produce an lvalue");
        valid = 0;
    }

    switch (info->value_category) {
        case VALUE_CATEGORY_NONE:
        case VALUE_CATEGORY_RVALUE:
            if (info->value_access != VALUE_ACCESS_NONE) {
                verifier_error(verifier, expression,
                    "%s expression has storage access %s",
                    value_category_name(info->value_category),
                    value_access_name(info->value_access));

                valid = 0;
            }

            break;

        case VALUE_CATEGORY_LVALUE:
            if (info->value_access !=
                    VALUE_ACCESS_READONLY &&
                info->value_access !=
                    VALUE_ACCESS_WRITABLE) {
                verifier_error(verifier, expression,
                    "lvalue has invalid storage access %s",
                    value_access_name(info->value_access));

                valid = 0;
            }

            break;
    }

    if (expression->type == NODE_IDENT &&
        info->symbol &&
        info->symbol->kind == SYMBOL_BUILTIN) {

        if (info->type != NULL) {
            verifier_error(verifier, expression,
                "builtin callee identifier unexpectedly has a type");

            valid = 0;
        }

        if (info->value_category != VALUE_CATEGORY_NONE) {
            verifier_error(verifier, expression,
                "builtin callee identifier has a value category");

            valid = 0;
        }

        if (info->value_access != VALUE_ACCESS_NONE) {
            verifier_error(verifier, expression,
                "builtin callee identifier has storage access %s",
                value_access_name(info->value_access));

            valid = 0;
        }

        return valid;
    }

    if (!verify_pointer_unary_info(verifier, expression, info)) {
        valid = 0;
    }

    if (!verify_field_access_info(verifier, expression, info)) {
        valid = 0;
    }

    if (!verify_index_access_info(verifier, expression, info)) {
        valid = 0;
    }

    if (!verify_null_cast_info(verifier, expression, info)) {
        valid = 0;
    }

    return valid;
}

static void verify_expression_info(Verifier *verifier, Node *expression) {

    SemExprInfo *info = semantic_get_expr_info(verifier->sem, expression);

    if (!info) {
        verifier_error(verifier, expression,
            "successfully checked expression has no SemExprInfo");
        return;
    }

    if (info->node != expression) {
        verifier_error(verifier, expression,
            "SemExprInfo points to a different AST node");
        return;
    }

    if (node_is_mutation(expression->type)) {
        verify_mutation_info(verifier, expression, info);
        return;
    }

    verify_value_info(verifier, expression, info);
}

static SemCScalarKind expected_c_scalar_kind(const Type *source_type)
{
    if (!source_type || source_type->kind != TYPE_NAMED ||
        source_type->named_module.length != 0)
        return SEM_C_SCALAR_NONE;

#define EXPECT_C_SCALAR(text, kind) \
    if (string_view_equals_cstr(source_type->named_name, text)) return kind

    EXPECT_C_SCALAR("c_char",      SEM_C_SCALAR_CHAR);
    EXPECT_C_SCALAR("c_schar",     SEM_C_SCALAR_SCHAR);
    EXPECT_C_SCALAR("c_uchar",     SEM_C_SCALAR_UCHAR);
    EXPECT_C_SCALAR("c_short",     SEM_C_SCALAR_SHORT);
    EXPECT_C_SCALAR("c_ushort",    SEM_C_SCALAR_USHORT);
    EXPECT_C_SCALAR("c_int",       SEM_C_SCALAR_INT);
    EXPECT_C_SCALAR("c_uint",      SEM_C_SCALAR_UINT);
    EXPECT_C_SCALAR("c_long",      SEM_C_SCALAR_LONG);
    EXPECT_C_SCALAR("c_ulong",     SEM_C_SCALAR_ULONG);
    EXPECT_C_SCALAR("c_longlong",  SEM_C_SCALAR_LONGLONG);
    EXPECT_C_SCALAR("c_ulonglong", SEM_C_SCALAR_ULONGLONG);
    EXPECT_C_SCALAR("c_size",      SEM_C_SCALAR_SIZE);
    EXPECT_C_SCALAR("c_bool",      SEM_C_SCALAR_BOOL);
    EXPECT_C_SCALAR("c_float",     SEM_C_SCALAR_FLOAT);
    EXPECT_C_SCALAR("c_double",    SEM_C_SCALAR_DOUBLE);

#undef EXPECT_C_SCALAR

    return SEM_C_SCALAR_NONE;
}

static int verify_abi_type(
    Verifier *verifier,
    Node *owner,
    const Type *source_type,
    Type *semantic_type,
    const SemAbiType *abi_type
) {
    if (!abi_type) {
        verifier_error(verifier, owner,
            "C ABI surface has no normalized SemAbiType");
        return 0;
    }

    int valid = 1;

    if (abi_type->semantic_type != semantic_type) {
        verifier_error(verifier, owner,
            "normalized ABI type does not reference the resolved semantic type");
        valid = 0;
    }

    SemCScalarKind c_scalar = expected_c_scalar_kind(source_type);
    if (c_scalar != SEM_C_SCALAR_NONE) {
        if (abi_type->kind != SEM_ABI_TYPE_C_SCALAR ||
            abi_type->c_scalar_kind != c_scalar) {
            verifier_error(verifier, owner,
                "normalized ABI type lost the native-C scalar spelling");
            valid = 0;
        }

        return valid;
    }

    switch (source_type->kind) {
        case TYPE_POINTER:
            if (abi_type->kind != SEM_ABI_TYPE_POINTER) {
                verifier_error(verifier, owner,
                    "normalized pointer ABI type has the wrong kind");
                return 0;
            }

            if (!semantic_type || semantic_type->kind != TYPE_POINTER) {
                verifier_error(verifier, owner,
                    "normalized pointer ABI type has a non-pointer semantic type");
                return 0;
            }

            if (!verify_abi_type(
                    verifier,
                    owner,
                    source_type->element,
                    semantic_type->element,
                    abi_type->element)) {
                valid = 0;
            }
            break;

        case TYPE_OPAQUE_POINTER:
            if (abi_type->kind != SEM_ABI_TYPE_OPAQUE_POINTER) {
                verifier_error(verifier, owner,
                    "normalized opaque-pointer ABI type has the wrong kind");
                valid = 0;
            }
            break;

        case TYPE_ARRAY:
            if (abi_type->kind != SEM_ABI_TYPE_ARRAY) {
                verifier_error(verifier, owner,
                    "normalized array ABI type has the wrong kind");
                return 0;
            }

            if (!semantic_type || semantic_type->kind != TYPE_ARRAY) {
                verifier_error(verifier, owner,
                    "normalized array ABI type has a non-array semantic type");
                return 0;
            }

            if (!verify_abi_type(
                    verifier,
                    owner,
                    source_type->element,
                    semantic_type->element,
                    abi_type->element)) {
                valid = 0;
            }
            break;

        case TYPE_FUNCTION:
            if (abi_type->kind != SEM_ABI_TYPE_FUNCTION) {
                verifier_error(verifier, owner,
                    "normalized function ABI type has the wrong kind");
                return 0;
            }

            if (!semantic_type || semantic_type->kind != TYPE_FUNCTION) {
                verifier_error(verifier, owner,
                    "normalized function ABI type has a non-function semantic type");
                return 0;
            }

            if (abi_type->parameter_count != semantic_type->parameter_count ||
                abi_type->parameter_count != source_type->parameter_count) {
                verifier_error(verifier, owner,
                    "normalized function ABI type has the wrong parameter count");
                valid = 0;
            } else {
                for (int i = 0; i < abi_type->parameter_count; i++) {
                    if (!verify_abi_type(
                            verifier,
                            owner,
                            source_type->parameters[i],
                            semantic_type->parameters[i],
                            abi_type->parameters[i])) {
                        valid = 0;
                    }
                }
            }

            if (!verify_abi_type(
                    verifier,
                    owner,
                    source_type->return_type,
                    semantic_type->return_type,
                    abi_type->return_type)) {
                valid = 0;
            }
            break;

        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_NAMED:
        case TYPE_STRUCT:
        case TYPE_ENUM:
            if (abi_type->kind != SEM_ABI_TYPE_SEMANTIC) {
                verifier_error(verifier, owner,
                    "normalized semantic ABI leaf has the wrong kind");
                valid = 0;
            }
            break;

        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
            verifier_error(verifier, owner,
                "non-concrete source type reached normalized ABI metadata");
            valid = 0;
            break;
    }

    return valid;
}

static int string_views_equal(StringView a, StringView b)
{
    return a.length == b.length &&
           (a.length == 0 || memcmp(a.data, b.data, a.length) == 0);
}

static void verify_function_abi_info(
    Verifier *verifier,
    Node *declaration,
    SemDeclInfo *info
) {
    Type *type = info->type;
    if (!type || type->kind != TYPE_FUNCTION)
        return;

    if (info->abi_kind != SEM_DECL_ABI_FUNCTION) {
        verifier_error(verifier, declaration,
            "function declaration has no normalized function ABI metadata");
        return;
    }

    SemFunctionAbiInfo *abi = &info->abi.function;
    FunctionAbi expected_abi =
        (declaration->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C ||
         declaration->as.func_decl.is_repr_c)
            ? FUNCTION_ABI_C
            : FUNCTION_ABI_COGLET;

    if (abi->abi != expected_abi || abi->abi != type->function_abi) {
        verifier_error(verifier, declaration,
            "normalized function ABI differs from the resolved function type");
    }

    SemFunctionLinkage expected_linkage =
        declaration->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C
            ? SEM_FUNCTION_LINKAGE_EXTERNAL
            : SEM_FUNCTION_LINKAGE_INTERNAL;

    if (abi->linkage != expected_linkage) {
        verifier_error(verifier, declaration,
            "normalized function linkage differs from the declaration contract");
    }

    if (abi->c_call_conv != type->function_call_conv ||
        abi->c_call_conv !=
            (expected_abi == FUNCTION_ABI_C
                ? declaration->as.func_decl.c_call_conv
                : C_CALL_DEFAULT)) {
        verifier_error(verifier, declaration,
            "normalized C calling convention differs from the resolved contract");
    }

    if (abi->is_variadic != type->function_is_variadic ||
        abi->is_variadic != declaration->as.func_decl.is_variadic) {
        verifier_error(verifier, declaration,
            "normalized variadic flag differs from the resolved contract");
    }


    if (expected_linkage == SEM_FUNCTION_LINKAGE_EXTERNAL) {
        StringView expected_symbol =
            declaration->as.func_decl.external_name.length > 0
                ? declaration->as.func_decl.external_name
                : declaration->as.func_decl.name;

        if (!string_views_equal(abi->external_symbol, expected_symbol)) {
            verifier_error(verifier, declaration,
                "normalized external symbol name differs from #extern(c)");
        }
    } else if (abi->external_symbol.length != 0) {
        verifier_error(verifier, declaration,
            "internal function unexpectedly carries an external linker symbol");
    }

    if (expected_abi == FUNCTION_ABI_C) {
        verify_abi_type(
            verifier,
            declaration,
            declaration->as.func_decl.return_type,
            type->return_type,
            abi->return_abi_type
        );
    } else if (abi->return_abi_type) {
        verifier_error(verifier, declaration,
            "ordinary Coglet function unexpectedly carries a C ABI return spelling");
    }

    for (int i = 0; i < declaration->as.func_decl.params.count; i++) {
        Node *param = declaration->as.func_decl.params.items[i];
        SemDeclInfo *param_info = semantic_get_decl_info(verifier->sem, param);

        if (!param_info)
            continue;

        if (expected_abi == FUNCTION_ABI_C ||
            (type->parameters[i]->kind == TYPE_FUNCTION &&
             type->parameters[i]->function_abi == FUNCTION_ABI_C)) {
            verify_abi_type(
                verifier,
                param,
                param->as.param_decl.var_type,
                type->parameters[i],
                param_info->abi_type
            );
        } else if (param_info->abi_type) {
            verifier_error(verifier, param,
                "ordinary Coglet parameter unexpectedly carries a C ABI spelling");
        }
    }
}

static void verify_aggregate_abi_info(
    Verifier *verifier,
    Node *declaration,
    SemDeclInfo *info
) {
    Type *type = info->type;
    if (!type || type->kind != TYPE_STRUCT)
        return;

    if (info->abi_kind != SEM_DECL_ABI_AGGREGATE) {
        verifier_error(verifier, declaration,
            "aggregate declaration has no normalized ABI metadata");
        return;
    }

    SemAggregateAbiInfo *abi = &info->abi.aggregate;
    SemAbiRepresentation expected_repr =
        declaration->as.struct_decl.is_repr_c
            ? SEM_ABI_REPR_C
            : SEM_ABI_REPR_COGLET;

    if (abi->representation != expected_repr ||
        (type->struct_is_repr_c ? SEM_ABI_REPR_C : SEM_ABI_REPR_COGLET) != expected_repr) {
        verifier_error(verifier, declaration,
            "normalized aggregate representation differs from semantic type");
    }

    SemAggregateKind expected_kind =
        declaration->as.struct_decl.is_union
            ? SEM_AGGREGATE_UNION
            : SEM_AGGREGATE_STRUCT;

    if (abi->aggregate_kind != expected_kind ||
        (type->struct_is_union ? SEM_AGGREGATE_UNION : SEM_AGGREGATE_STRUCT) != expected_kind) {
        verifier_error(verifier, declaration,
            "normalized aggregate kind differs from semantic type");
    }

    if (abi->is_incomplete != declaration->as.struct_decl.is_incomplete ||
        abi->is_incomplete != type->struct_is_incomplete ||
        abi->is_packed != declaration->as.struct_decl.repr_c_packed ||
        abi->is_packed != type->struct_repr_c_packed ||
        abi->explicit_alignment !=
            (unsigned)(declaration->as.struct_decl.repr_c_align > 0
                ? declaration->as.struct_decl.repr_c_align
                : 0) ||
        abi->explicit_alignment !=
            (unsigned)(type->struct_repr_c_align > 0
                ? type->struct_repr_c_align
                : 0)) {
        verifier_error(verifier, declaration,
            "normalized aggregate layout controls differ from semantic type");
    }

    for (int i = 0; i < declaration->as.struct_decl.fields.count; i++) {
        Node *field = declaration->as.struct_decl.fields.items[i];
        SemDeclInfo *field_info = semantic_get_decl_info(verifier->sem, field);

        if (!field_info)
            continue;

        if (expected_repr == SEM_ABI_REPR_C) {
            verify_abi_type(
                verifier,
                field,
                field->as.struct_field_decl.var_type,
                field_info->type,
                field_info->abi_type
            );
        } else if (field_info->abi_type) {
            verifier_error(verifier, field,
                "ordinary Coglet struct field unexpectedly carries a C ABI spelling");
        }
    }
}

static void verify_enum_abi_info(
    Verifier *verifier,
    Node *declaration,
    SemDeclInfo *info
) {
    Type *type = info->type;
    if (!type || type->kind != TYPE_ENUM)
        return;

    if (info->abi_kind != SEM_DECL_ABI_ENUM) {
        verifier_error(verifier, declaration,
            "enum declaration has no normalized ABI metadata");
        return;
    }

    SemAbiRepresentation expected_repr =
        declaration->as.enum_decl.is_repr_c
            ? SEM_ABI_REPR_C
            : SEM_ABI_REPR_COGLET;

    if (info->abi.enumeration.representation != expected_repr ||
        (type->enum_is_repr_c ? SEM_ABI_REPR_C : SEM_ABI_REPR_COGLET) != expected_repr) {
        verifier_error(verifier, declaration,
            "normalized enum representation differs from semantic type");
    }

    if (expected_repr == SEM_ABI_REPR_C) {
        verify_abi_type(
            verifier,
            declaration,
            declaration->as.enum_decl.backing_type,
            type->enum_backing_type,
            info->abi.enumeration.backing_abi_type
        );
    } else if (info->abi.enumeration.backing_abi_type) {
        verifier_error(verifier, declaration,
            "ordinary Coglet enum unexpectedly carries a C ABI backing spelling");
    }
}

static void verify_declaration_info(Verifier *verifier, Node *declaration) {

    SemDeclInfo *info =
        semantic_get_decl_info(verifier->sem, declaration);

    if (!info) {
        verifier_error(verifier, declaration,
            "successfully checked declaration has no SemDeclInfo");
        return;
    }

    if (info->node != declaration) {
        verifier_error(verifier, declaration,
            "SemDeclInfo points to a different AST node");
        return;
    }

    if (info->id == INVALID_SEM_DECL_ID) {
        verifier_error(verifier, declaration,
            "SemDeclInfo has the invalid declaration ID");
    }

    if (!info->type) {
        verifier_error(verifier, declaration,
            "SemDeclInfo has no resolved semantic type");
    }

    if (semantic_get_decl_info_by_id(verifier->sem, info->id) != info) {
        verifier_error(verifier, declaration,
            "declaration ID does not resolve back to the same SemDeclInfo");
    }

    if (info->is_exported != declaration->is_exported) {
        verifier_error(verifier, declaration,
            "declaration export visibility differs from SemDeclInfo");
    }

    int declaration_must_have_constant =
        declaration->type == NODE_CONST_DECL ||
        declaration->type == NODE_ENUM_MEMBER;

    if (declaration_must_have_constant) {
        ConstValue value;

        if (!info->has_constant_value) {
            verifier_error(verifier, declaration,
                "constant-like declaration has no cached compile-time value");
        } else if (!semantic_get_constant_value(
                       verifier->sem,
                       declaration,
                       &value)) {
            verifier_error(verifier, declaration,
                "constant-like declaration is not retrievable through semantic API");
        } else if (value.type != info->type) {
            verifier_error(verifier, declaration,
                "declaration constant type differs from SemDeclInfo type");
        }
    } else if (info->has_constant_value) {
        verifier_error(verifier, declaration,
            "non-constant declaration unexpectedly carries a compile-time value");
    }

    if (info->symbol) {
        if (info->symbol->declaration != declaration) {
            verifier_error(verifier, declaration,
                "declaration Symbol points to a different AST node");
        }

        if (info->symbol->declaration_id != info->id) {
            verifier_error(verifier, declaration,
                "declaration Symbol carries a different stable declaration ID");
        }

        if (info->symbol->type != info->type) {
            verifier_error(verifier, declaration,
                "declaration Symbol type differs from SemDeclInfo type");
        }
    }

    switch (declaration->type) {
        case NODE_VAR_DECL:
        case NODE_CONST_DECL:
            if (!info->symbol) {
                verifier_error(verifier, declaration,
                    "lexical declaration has no resolved Symbol");
            }

            if (info->abi_kind != SEM_DECL_ABI_NONE) {
                verifier_error(verifier, declaration,
                    "ordinary value declaration unexpectedly carries declaration ABI metadata");
            }

            if (declaration->type == NODE_VAR_DECL &&
                declaration->as.var_decl.var_type &&
                info->type && info->type->kind == TYPE_FUNCTION &&
                info->type->function_abi == FUNCTION_ABI_C) {
                verify_abi_type(
                    verifier,
                    declaration,
                    declaration->as.var_decl.var_type,
                    info->type,
                    info->abi_type
                );
            } else if (info->abi_type) {
                verifier_error(verifier, declaration,
                    "ordinary value declaration unexpectedly carries a direct ABI type spelling");
            }
            break;

        case NODE_FUNC_DECL:
            if (!info->symbol) {
                verifier_error(verifier, declaration,
                    "lexical declaration has no resolved Symbol");
            }

            if (info->abi_type) {
                verifier_error(verifier, declaration,
                    "function declaration unexpectedly carries a direct ABI type spelling");
            }

            verify_function_abi_info(verifier, declaration, info);
            break;

        case NODE_STRUCT_DECL:
            if (!info->symbol) {
                verifier_error(verifier, declaration,
                    "lexical declaration has no resolved Symbol");
            }

            if (info->abi_type) {
                verifier_error(verifier, declaration,
                    "aggregate declaration unexpectedly carries a direct ABI type spelling");
            }

            verify_aggregate_abi_info(verifier, declaration, info);
            break;

        case NODE_ENUM_DECL:
            if (!info->symbol) {
                verifier_error(verifier, declaration,
                    "lexical declaration has no resolved Symbol");
            }

            if (info->abi_type) {
                verifier_error(verifier, declaration,
                    "enum declaration unexpectedly carries a direct ABI type spelling");
            }

            verify_enum_abi_info(verifier, declaration, info);
            break;

        case NODE_STRUCT_FIELD_DECL:
        case NODE_ENUM_MEMBER:
            if (info->symbol) {
                verifier_error(verifier, declaration,
                    "aggregate member unexpectedly has a lexical Symbol");
            }

            if (info->abi_kind != SEM_DECL_ABI_NONE) {
                verifier_error(verifier, declaration,
                    "aggregate member unexpectedly carries declaration-level ABI metadata");
            }
            break;

        case NODE_FUNC_PARAM_DECL:
            /*
             * Parameters of body-less declarations (notably #extern(c)) have
             * semantic identity/type but intentionally no lexical Symbol.
             * Their optional abi_type is verified from the owning function.
             */
            if (info->abi_kind != SEM_DECL_ABI_NONE) {
                verifier_error(verifier, declaration,
                    "function parameter unexpectedly carries declaration-level ABI metadata");
            }
            break;

        default:
            verifier_error(verifier, declaration,
                "SemDeclInfo attached to unsupported declaration node kind");
            break;
    }
}

static void verify_declaration_table_entries(Verifier *verifier) {

    for (SemDeclInfo *info = verifier->sem->decl_infos; info; info = info->next) {
        verifier->declaration_table_entry_count++;

        if (!info->node) {
            verifier_error(verifier, NULL,
                "declaration semantic-info table contains an entry with a null node");
            continue;
        }

        if (!node_is_declaration(info->node->type)) {
            verifier_error(verifier, info->node,
                "declaration semantic-info entry belongs to a non-declaration node");
        }

        if (!expression_list_contains(&verifier->declarations, info->node)) {
            verifier_error(verifier, info->node,
                "declaration semantic-info entry does not belong to the program AST");
        }

        if (info->id == INVALID_SEM_DECL_ID ||
            info->id >= verifier->sem->next_declaration_id) {
            verifier_error(verifier, info->node,
                "declaration semantic-info entry has an out-of-range stable ID");
        }

        if (semantic_get_decl_info_by_id(verifier->sem, info->id) != info) {
            verifier_error(verifier, info->node,
                "stable declaration ID lookup does not return its table entry");
        }

        for (SemDeclInfo *other = info->next; other; other = other->next) {
            if (other->node == info->node) {
                verifier_error(verifier, info->node,
                    "declaration semantic-info table contains duplicate entries for one AST node");
                break;
            }

            if (other->id == info->id) {
                verifier_error(verifier, info->node,
                    "declaration semantic-info table contains duplicate stable IDs");
                break;
            }
        }
    }

    if ((SemDeclId)verifier->declaration_table_entry_count !=
        verifier->sem->next_declaration_id) {
        verifier_error(
            verifier,
            NULL,
            "declaration ID sequence has %zu IDs but table has %d entries",
            (size_t)verifier->sem->next_declaration_id,
            verifier->declaration_table_entry_count
        );
    }
}

static void verify_table_entries(Verifier *verifier) {

    for (SemExprInfo *info = verifier->sem->expr_infos; info; info = info->next) {

        verifier->table_entry_count++;

        if (info->symbol && info->symbol->declaration) {
            SemDeclInfo *decl_info = semantic_get_decl_info_by_id(
                verifier->sem,
                info->symbol->declaration_id
            );

            if (!decl_info || decl_info->symbol != info->symbol) {
                verifier_error(verifier, info->node,
                    "expression Symbol does not resolve through stable declaration identity");
            }
        }

        if (!info->node) {
            verifier_error(verifier, NULL,
                "semantic-info table contains an entry with a null node");

            continue;
        }

        if (!node_is_expression(info->node->type)) {
            verifier_error(verifier, info->node,
                "semantic-info entry belongs to a non-expression node");
        }

        if (!expression_list_contains(&verifier->expressions, info->node)) {
            verifier_error(verifier, info->node,
                "semantic-info entry does not belong to the program AST");
        }

        for (SemExprInfo *other = info->next;
             other;
             other = other->next) {
            if (other->node == info->node) {
                verifier_error(verifier, info->node,
                    "semantic-info table contains duplicate entries for one AST node");

                break;
            }
        }
    }
}

static void dump_expression(void *context, Node *expression)
{
    DumpContext *dump = context;
    SemExprInfo *info = semantic_get_expr_info(dump->sem, expression);

    const char *type_name     = "<missing>";
    const char *category_name = "<missing>";
    const char *access_name   = "<missing>";
    const char *symbol_name   = "<missing>";
    const char *context_name  = "<missing>";
    const char *context_type_name = "<missing>";

    if (info) {
        type_name = info->type
            ? type_kind_name(info->type->kind)
            : "<none>";
        category_name = value_category_name(
            info->value_category
        );
        symbol_name = info->symbol ? "yes" : "no";

        access_name = value_access_name(info->value_access);
        context_name = context_conversion_name(info->contextual_conversion);
        context_type_name = info->contextual_type
            ? type_kind_name(info->contextual_type->kind)
            : "<none>";
    }

    fprintf(
        dump->output,
        "  %4d  line %-4d node=%-18s "
        "type=%-18s category=%-6s "
        "access=%-8s convert=%-22s to=%-18s symbol=%s\n",
        dump->count,
        expression->line,
        node_type_name(expression->type),
        type_name,
        category_name,
        access_name,
        context_name,
        context_type_name,
        symbol_name);

    dump->count++;
}

void semantic_info_dump_program(SemanticContext *sem, Node *program, FILE *output) {

    if (!output)
        output = stderr;

    fputs("\nSemantic-info expressions in source order:\n", output);

    if (!sem || !program) {
        fputs("  <unavailable>\n", output);
        return;
    }

    DumpContext dump;

    dump.sem    = sem;
    dump.output = output;
    dump.count  = 1;

    ExpressionWalker walker;
    walker.sem = sem;
    walker.visit = dump_expression;
    walker.visit_declaration = NULL;
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
    SemanticContext *sem, Node *program, FILE *diagnostics, SemanticInfoVerification *verification) {

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
    walker.visit_declaration = collect_declaration;
    walker.context = &verifier;

    walk_node(&walker, program);

    for (int i = 0; i < verifier.expressions.count; i++) {
        verify_expression_info(
            &verifier,
            verifier.expressions.items[i]
        );
    }

    for (int i = 0; i < verifier.declarations.count; i++) {
        verify_declaration_info(
            &verifier,
            verifier.declarations.items[i]
        );
    }

    verify_table_entries(&verifier);
    verify_declaration_table_entries(&verifier);

    if (verifier.table_entry_count != verifier.expressions.count) {
        verifier_error(
            &verifier,
            NULL,
            "table has %d entries but AST has %d checked expressions",
            verifier.table_entry_count,
            verifier.expressions.count
        );
    }

    if (verifier.declaration_table_entry_count != verifier.declarations.count) {
        verifier_error(
            &verifier,
            NULL,
            "declaration table has %d entries but AST has %d checked declarations",
            verifier.declaration_table_entry_count,
            verifier.declarations.count
        );
    }

    if (verification) {
        verification->expression_count = verifier.expressions.count;
        verification->mutation_count = verifier.mutation_count;
        verification->table_entry_count = verifier.table_entry_count;
        verification->declaration_count = verifier.declarations.count;
        verification->declaration_table_entry_count =
            verifier.declaration_table_entry_count;
        verification->error_count = verifier.error_count;
    }

    int succeeded = verifier.error_count == 0;
    expression_list_destroy(&verifier.expressions);
    expression_list_destroy(&verifier.declarations);

    return succeeded;
}
