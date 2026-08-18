#include "../include/ast.h"

#include <stdio.h>
#include <string.h>

#include "../include/utils/utils.h"

static Node *new_node(Arena *arena, NodeType type, SourceSpan span) {
    Node *node = arena_alloc(arena, sizeof(Node));
    node->is_exported = 0;
    node->span = span;
    node->line = (int)span.line;
    node->type = type;
    return node;
}

Node *ast_new_integer(Arena *arena, uint64_t value, SourceSpan span)
{
    Node *node = new_node(arena, NODE_NUMBER, span);

    node->as.number.kind = NUMBER_LITERAL_INTEGER;
    node->as.number.value.integer = value;

    return node;
}

Node *ast_new_float(Arena *arena, double value, SourceSpan span)
{
    Node *node = new_node(arena, NODE_NUMBER, span);

    node->as.number.kind = NUMBER_LITERAL_FLOAT;
    node->as.number.value.floating = value;

    return node;
}

Node *ast_new_ident(Arena *arena, const char *start, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_IDENT, span);
    node->as.ident.data  = start;
    node->as.ident.length = length;
    return node;
}

Node *ast_new_compound_assign(Arena *arena, TokenType op, Node *target, Node *value, SourceSpan span) {
    Node *node = new_node(arena, NODE_COMPOUND_ASSIGN, span);
    node->as.compound_assign.op  = op;
    node->as.compound_assign.target = target;
    node->as.compound_assign.value = value;
    return node;
}

Node *ast_new_string(Arena *arena, const char *start, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_STRING, span);
    node->as.string_literal.data   = start;
    node->as.string_literal.length = length;
    return node;
}

Node *ast_new_char(Arena *arena, const char *start, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_CHAR, span);
    node->as.char_literal.data   = start;
    node->as.char_literal.length = length;
    return node;
}

Node *ast_new_null(Arena *arena, SourceSpan span) {
    return new_node(arena, NODE_NULL, span);
}

Node *ast_new_bool(Arena *arena, int value, SourceSpan span) {
    Node *node = new_node(arena, NODE_BOOL, span);
    node->as.boolean.value = value;
    return node;
}

Node *ast_new_cast(Arena *arena, CastKind kind, Type *target_type, Node *expression, SourceSpan span) {
    Node *node = new_node(arena, NODE_CAST, span);
    node->as.cast_expr.kind = kind;
    node->as.cast_expr.target_type = target_type;
    node->as.cast_expr.expression  = expression;

    return node;
}

Node *ast_new_unary(Arena *arena, TokenType op, Node *operand, SourceSpan span) {
    Node *node = new_node(arena, NODE_UNARY, span);
    node->as.unary.op      = op;
    node->as.unary.operand = operand;
    return node;
}

Node *ast_new_inc_dec(Arena *arena, TokenType op, Node *target, int is_prefix, SourceSpan span) {
    Node *node = new_node(arena, NODE_INC_DEC, span);
    node->as.inc_dec.op = op;
    node->as.inc_dec.target = target;
    node->as.inc_dec.is_prefix = is_prefix;
    return node;
}

Node *ast_new_binary(Arena *arena, TokenType op, Node *left, Node *right, SourceSpan span) {
    Node *node = new_node(arena, NODE_BINARY, span);
    node->as.binary.op     = op;
    node->as.binary.left   = left;
    node->as.binary.right  = right;
    return node;
}

Node *ast_new_assign(Arena *arena,Node *target,Node *value,SourceSpan span) {
    Node *node = new_node(arena, NODE_ASSIGN, span);
    node->as.assign.target = target;
    node->as.assign.value  = value;
    return node;
}

Node *ast_new_if(Arena *arena, Node *cond, Node *then_b, Node *else_b, SourceSpan span) {
    Node *node = new_node(arena, NODE_IF, span);
    node->as.if_stmt.condition   = cond;
    node->as.if_stmt.then_branch = then_b;
    node->as.if_stmt.else_branch = else_b;
    return node;
}

Node *ast_new_if_expr(Arena *arena, Node *cond, Node *then_value, Node *else_value, SourceSpan span) {
    Node *node = new_node(arena, NODE_IF_EXPR, span);
    node->as.if_expr.condition = cond;
    node->as.if_expr.then_value = then_value;
    node->as.if_expr.else_value = else_value;
    return node;
}

Node *ast_new_expr_stmt(Arena *arena, Node *expr, SourceSpan span) {
    Node *node = new_node(arena, NODE_EXPR_STMT, span);
    node->as.expr_stmt.expr = expr;
    return node;
}

Node *ast_new_static_assert(Arena *arena, Node *condition, Node *message, SourceSpan span) {
    Node *node = new_node(arena, NODE_STATIC_ASSERT, span);
    node->as.static_assert_stmt.condition = condition;
    node->as.static_assert_stmt.message = message;
    return node;
}

Node *ast_new_block(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_BLOCK, span);
    node->as.block.statements.items    = NULL;
    node->as.block.statements.count    = 0;
    node->as.block.statements.capacity = 0;
    return node;
}

Node *ast_new_call(Arena *arena, Node *callee, SourceSpan span) {
    Node *node = new_node(arena, NODE_CALL, span);
    node->as.call.callee                  = callee;
    node->as.call.type_arguments.items    = NULL;
    node->as.call.type_arguments.count    = 0;
    node->as.call.type_arguments.capacity = 0;
    node->as.call.arguments.items         = NULL;
    node->as.call.arguments.count    = 0;
    node->as.call.arguments.capacity = 0;
    return node;
}

Node *ast_new_field(Arena *arena, Node *object, const char *name, int length, SourceSpan span ) {
    Node *node = new_node(arena, NODE_FIELD, span);
    node->as.field.object = object;
    node->as.field.name.data   = name;
    node->as.field.name.length = length;
    node->as.field.dotted_path = string_view_empty();

    StringView prefix = string_view_empty();
    if (object && object->type == NODE_IDENT) {
        prefix = object->as.ident;
    } else if (object && object->type == NODE_FIELD &&
               object->as.field.dotted_path.length != 0) {
        prefix = object->as.field.dotted_path;
    }

    if (prefix.length != 0 && length > 0) {
        size_t total = prefix.length + 1 + (size_t)length;
        char *path = arena_alloc(arena, total);
        memcpy(path, prefix.data, prefix.length);
        path[prefix.length] = '.';
        memcpy(path + prefix.length + 1, name, (size_t)length);
        node->as.field.dotted_path.data = path;
        node->as.field.dotted_path.length = total;
    }

    return node;
}
Node *ast_new_index(Arena *arena, Node *object, Node *index, SourceSpan span) {
    Node *node = new_node(arena, NODE_INDEX, span);
    node->as.index.object = object;
    node->as.index.index  = index;
    return node;
}

Node *ast_new_error(Arena *arena, Token token)
{
    Node *node = arena_alloc(arena, sizeof(Node));
    node->type = NODE_ERROR;
    node->span = token.span;
    node->line = token.line;
    node->as.error.token = token;
    return node;
}

Node *ast_new_program(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_PROGRAM, span);
    node->as.program.statements.items    = NULL;
    node->as.program.statements.count    = 0;
    node->as.program.statements.capacity = 0;
    return node;
}

Node *ast_new_module_decl(Arena *arena, const char *name, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_MODULE_DECL, span);
    node->as.module_decl.name.data = name;
    node->as.module_decl.name.length = (size_t)length;
    return node;
}

Node *ast_new_import_decl(
    Arena *arena,
    const char *name,
    int length,
    const char *alias,
    int alias_length,
    SourceSpan span
) {
    Node *node = new_node(arena, NODE_IMPORT_DECL, span);
    node->as.import_decl.name.data = name;
    node->as.import_decl.name.length = (size_t)length;
    node->as.import_decl.alias.data = alias;
    node->as.import_decl.alias.length = (size_t)alias_length;
    return node;
}

Node *ast_new_var_decl(Arena *arena, Type *type, const char *name, int length, Node *initializer, SourceSpan span) {
    Node *node = new_node(arena, NODE_VAR_DECL, span);
    node->as.var_decl.var_type    = type;
    node->as.var_decl.name.data   = name;
    node->as.var_decl.name.length = length;
    node->as.var_decl.initializer = initializer;
    return node;
}

Node *ast_new_var_decl_group(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_VAR_DECL_GROUP, span);
    node->as.var_decl_group.declarations.items = NULL;
    node->as.var_decl_group.declarations.count = 0;
    node->as.var_decl_group.declarations.capacity = 0;
    return node;
}

Node *ast_new_struct_field_decl(Arena *arena, Type *type, const char *name, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_STRUCT_FIELD_DECL, span);
    node->as.struct_field_decl.var_type    = type;
    node->as.struct_field_decl.name.data   = name;
    node->as.struct_field_decl.name.length = length;
    return node;
}

Node *ast_new_type_ref(Arena *arena, Type *source_type, SourceSpan span) {
    Node *node = new_node(arena, NODE_TYPE_REF, span);
    node->as.type_ref.source_type = source_type;
    return node;
}

Node *ast_new_func_param_decl(Arena *arena, Type *type, const char *name, int length, SourceSpan span) {
    Node *node = new_node(arena, NODE_FUNC_PARAM_DECL, span);
    node->as.param_decl.var_type    = type;
    node->as.param_decl.name.data   = name;
    node->as.param_decl.name.length = length;
    return node;
}

Node *ast_new_return(Arena *arena, Node *value, SourceSpan span) {
    Node *node = new_node(arena, NODE_RETURN, span);
    node->as.return_stmt.value = value;
    return node;
}

Node *ast_new_defer(Arena *arena, Node *statement, SourceSpan span) {
    Node *node = new_node(arena, NODE_DEFER, span);
    node->as.defer_stmt.statement = statement;
    return node;
}

Node*ast_new_while(Arena *arena, Node *cond, Node *body, SourceSpan span) {
    Node *node = new_node(arena, NODE_WHILE, span);
    node->as.while_stmt.condition = cond;
    node->as.while_stmt.body      = body;
    return node;
}

Node *ast_new_for(Arena *arena, Node *cond, Node *post, Node *body, SourceSpan span) {
    Node *node = new_node(arena, NODE_FOR, span);
    node->as.for_stmt.condition  = cond;
    node->as.for_stmt.post       = post;
    node->as.for_stmt.body       = body;
    return node;
}

Node *ast_new_break(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_BREAK, span);
    return node;
}

Node *ast_new_continue(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_CONTINUE, span);
    return node;
}

Node *ast_new_switch(Arena *arena,Node *expression,SourceSpan span) {
    Node *node = new_node(arena, NODE_SWITCH, span);

    node->as.switch_stmt.expression    = expression;
    node->as.switch_stmt.resolved_type = NULL;

    node->as.switch_stmt.cases.items    = NULL;
    node->as.switch_stmt.cases.count    = 0;
    node->as.switch_stmt.cases.capacity = 0;

    return node;
}

Node *ast_new_switch_case(Arena *arena, Node *value, Node *body, int is_default, SourceSpan span) {
    Node *node = new_node(arena, NODE_SWITCH_CASE, span);
    node->as.switch_case.value = value;
    node->as.switch_case.body = body;
    node->as.switch_case.is_default = is_default;

    return node;
}

Node *ast_new_func_decl(Arena *arena, const char *name, int name_length, Type *return_type, SourceSpan span) {
    Node *node = new_node(arena, NODE_FUNC_DECL, span);
    node->as.func_decl.name.data     = name;
    node->as.func_decl.name.length   = name_length;
    node->as.func_decl.return_type   = return_type;
    node->as.func_decl.body          = NULL;
    node->as.func_decl.resolved_type = NULL;
    node->as.func_decl.linkage       = FUNCTION_LINKAGE_COGLET;
    node->as.func_decl.is_repr_c      = 0;
    node->as.func_decl.is_discardable = 0;
    node->as.func_decl.c_call_conv    = C_CALL_DEFAULT;
    node->as.func_decl.is_variadic    = 0;
    node->as.func_decl.external_name = string_view_empty();

    node->as.func_decl.type_parameters.items    = NULL;
    node->as.func_decl.type_parameters.count    = 0;
    node->as.func_decl.type_parameters.capacity = 0;

    node->as.func_decl.params.items    = NULL;
    node->as.func_decl.params.count    = 0;
    node->as.func_decl.params.capacity = 0;

    return node;
}

Node *ast_new_struct_decl(Arena *arena, const char *name, int name_length, SourceSpan span) {
    Node *node = new_node(arena, NODE_STRUCT_DECL, span);
    node->as.struct_decl.name.data   = name;
    node->as.struct_decl.name.length = name_length;
    node->as.struct_decl.type_parameters.items = NULL;
    node->as.struct_decl.type_parameters.count = 0;
    node->as.struct_decl.type_parameters.capacity = 0;
    node->as.struct_decl.is_repr_c       = 0;
    node->as.struct_decl.repr_c_packed    = 0;
    node->as.struct_decl.repr_c_align     = 0;
    node->as.struct_decl.is_union         = 0;
    node->as.struct_decl.is_incomplete = 0;
    node->as.struct_decl.resolved_type  = NULL;

    node->as.struct_decl.fields.items    = NULL;
    node->as.struct_decl.fields.count    = 0;
    node->as.struct_decl.fields.capacity = 0;

    node->as.struct_decl.methods.items    = NULL;
    node->as.struct_decl.methods.count    = 0;
    node->as.struct_decl.methods.capacity = 0;

    node->as.struct_decl.operators.items    = NULL;
    node->as.struct_decl.operators.count    = 0;
    node->as.struct_decl.operators.capacity = 0;

    return node;
}

Node *ast_new_struct_init(Arena *arena, const char *name, int name_length, SourceSpan span) {
    Node *node = new_node(arena, NODE_STRUCT_INIT, span);
    node->as.struct_init.module_name = string_view_empty();
    node->as.struct_init.name.data   = name;
    node->as.struct_init.name.length = name_length;
    node->as.struct_init.type_arguments.items = NULL;
    node->as.struct_init.type_arguments.count = 0;
    node->as.struct_init.type_arguments.capacity = 0;

    node->as.struct_init.fields.items    = NULL;
    node->as.struct_init.fields.count    = 0;
    node->as.struct_init.fields.capacity = 0;

    return node;
}

Node *ast_new_enum_decl(Arena *arena, const char *name, int name_length, SourceSpan span) {
    Node *node = new_node(arena, NODE_ENUM_DECL, span);
    node->as.enum_decl.name.data   = name;
    node->as.enum_decl.name.length = name_length;

    node->as.enum_decl.backing_type  = NULL;
    node->as.enum_decl.is_repr_c     = 0;
    node->as.enum_decl.resolved_type = NULL;

    node->as.enum_decl.members.items    = NULL;
    node->as.enum_decl.members.count    = 0;
    node->as.enum_decl.members.capacity = 0;

    return node;
}

Node *ast_new_enum_member(Arena *arena, const char *name, int name_length, SourceSpan span) {
    Node *node = new_node(arena, NODE_ENUM_MEMBER, span);
    node->as.enum_member.name.data   = name;
    node->as.enum_member.name.length = name_length;

    node->as.enum_member.value = NULL;
    node->as.enum_member.resolved_value.magnitude   = 0;
    node->as.enum_member.resolved_value.is_negative = 0;

    return node;
}

Node *ast_new_field_init(Arena *arena, const char *name, int name_length, Node *value, SourceSpan span) {
    Node *node = new_node(arena, NODE_FIELD_INIT, span);
    node->as.field_init.name.data   = name;
    node->as.field_init.name.length = name_length;
    node->as.field_init.value       = value;
    return node;
}

Node *ast_new_const_decl(Arena *arena, Type *type, const char *name, int name_length, Node *value, SourceSpan span) {
    Node *node = new_node(arena, NODE_CONST_DECL, span);
    node->as.const_decl.const_type  = type;
    node->as.const_decl.name.data   = name;
    node->as.const_decl.name.length = name_length;
    node->as.const_decl.value       = value;
    return node;
}

Node *ast_new_array_literal(Arena *arena, SourceSpan span) {
    Node *node = new_node(arena, NODE_ARRAY_LITERAL, span);
    node->as.array_literal.elements.items    = NULL;
    node->as.array_literal.elements.count    = 0;
    node->as.array_literal.elements.capacity = 0;
    node->as.array_literal.is_zero_initializer = 0;
    return node;
}

Node *ast_new_zero_array_initializer(Arena *arena, SourceSpan span) {
    Node *node = ast_new_array_literal(arena, span);
    node->as.array_literal.is_zero_initializer = 1;
    return node;
}

Node *ast_clone(Arena *arena, const Node *node)
{
    if (!node)
        return NULL;

    Node *clone = new_node(arena, node->type, node->span);
    clone->is_exported = node->is_exported;

    switch (node->type)
    {
        case NODE_NUMBER:
            clone->as.number = node->as.number;
            break;

        case NODE_IDENT:
            clone->as.ident = node->as.ident;
            break;

        case NODE_STRING:
            clone->as.string_literal = node->as.string_literal;
            break;

        case NODE_CHAR:
            clone->as.char_literal = node->as.char_literal;
            break;

        case NODE_BOOL:
            clone->as.boolean = node->as.boolean;
            break;

        case NODE_UNARY:
            clone->as.unary.op      = node->as.unary.op;
            clone->as.unary.operand = ast_clone(arena, node->as.unary.operand);
            break;

        case NODE_BINARY:
            clone->as.binary.op    = node->as.binary.op;
            clone->as.binary.left  = ast_clone(arena, node->as.binary.left);
            clone->as.binary.right = ast_clone(arena, node->as.binary.right);
            break;

        case NODE_ASSIGN:
            clone->as.assign.target = ast_clone(arena, node->as.assign.target);
            clone->as.assign.value  = ast_clone(arena, node->as.assign.value);
            break;

        case NODE_COMPOUND_ASSIGN:
            clone->as.compound_assign.op     = node->as.compound_assign.op;
            clone->as.compound_assign.target = ast_clone(arena, node->as.compound_assign.target);
            clone->as.compound_assign.value  = ast_clone(arena, node->as.compound_assign.value);
            break;

            case NODE_CAST:
                clone->as.cast_expr.kind        = node->as.cast_expr.kind;
                clone->as.cast_expr.target_type = node->as.cast_expr.target_type;
                clone->as.cast_expr.expression =
                    ast_clone(arena, node->as.cast_expr.expression);

                break;

        case NODE_IF:
            clone->as.if_stmt.condition =
                ast_clone(arena, node->as.if_stmt.condition);

            clone->as.if_stmt.then_branch =
                ast_clone(arena, node->as.if_stmt.then_branch);

            clone->as.if_stmt.else_branch =
                ast_clone(arena, node->as.if_stmt.else_branch);
            break;

        case NODE_IF_EXPR:
            clone->as.if_expr.condition = ast_clone(arena, node->as.if_expr.condition);
            clone->as.if_expr.then_value = ast_clone(arena, node->as.if_expr.then_value);
            clone->as.if_expr.else_value = ast_clone(arena, node->as.if_expr.else_value);
            break;

        case NODE_EXPR_STMT:
            clone->as.expr_stmt.expr =
                ast_clone(arena, node->as.expr_stmt.expr);
            break;

        case NODE_STATIC_ASSERT:
            clone->as.static_assert_stmt.condition =
                ast_clone(arena, node->as.static_assert_stmt.condition);
            clone->as.static_assert_stmt.message =
                ast_clone(arena, node->as.static_assert_stmt.message);
            break;

        case NODE_BLOCK:
            clone->as.block.statements.items = NULL;
            clone->as.block.statements.count = 0;
            clone->as.block.statements.capacity = 0;

            for (int i = 0; i < node->as.block.statements.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.block.statements,
                    ast_clone(arena, node->as.block.statements.items[i])
                );
            }
            break;

        case NODE_CALL:
            clone->as.call.callee = ast_clone(arena, node->as.call.callee);

            clone->as.call.type_arguments.items = NULL;
            clone->as.call.type_arguments.count = node->as.call.type_arguments.count;
            clone->as.call.type_arguments.capacity = node->as.call.type_arguments.count;
            if (node->as.call.type_arguments.count > 0) {
                clone->as.call.type_arguments.items = arena_alloc(
                    arena,
                    sizeof(Type *) * (size_t)node->as.call.type_arguments.count
                );
                memcpy(
                    clone->as.call.type_arguments.items,
                    node->as.call.type_arguments.items,
                    sizeof(Type *) * (size_t)node->as.call.type_arguments.count
                );
            }

            clone->as.call.arguments.items    = NULL;
            clone->as.call.arguments.count    = 0;
            clone->as.call.arguments.capacity = 0;

            for (int i = 0; i < node->as.call.arguments.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.call.arguments,
                    ast_clone(arena, node->as.call.arguments.items[i])
                );
            }
            break;

        case NODE_FIELD:
            clone->as.field.object = ast_clone(arena, node->as.field.object);

            clone->as.field.name.data   = node->as.field.name.data;
            clone->as.field.name.length = node->as.field.name.length;
            clone->as.field.dotted_path = node->as.field.dotted_path;
            break;

        case NODE_TYPE_REF:
            clone->as.type_ref.source_type = node->as.type_ref.source_type;
            break;

        case NODE_INDEX:
            clone->as.index.object = ast_clone(arena, node->as.index.object);
            clone->as.index.index  = ast_clone(arena, node->as.index.index);
            break;

        case NODE_PROGRAM:
            clone->as.program.statements.items    = NULL;
            clone->as.program.statements.count    = 0;
            clone->as.program.statements.capacity = 0;

            for (int i = 0; i < node->as.program.statements.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.program.statements,
                    ast_clone(arena, node->as.program.statements.items[i])
                );
            }
            break;

        case NODE_MODULE_DECL:
            clone->as.module_decl.name = node->as.module_decl.name;
            break;

        case NODE_IMPORT_DECL:
            clone->as.import_decl.name = node->as.import_decl.name;
            clone->as.import_decl.alias = node->as.import_decl.alias;
            break;

        case NODE_VAR_DECL:
            clone->as.var_decl.var_type    = node->as.var_decl.var_type;
            clone->as.var_decl.name.data   = node->as.var_decl.name.data;
            clone->as.var_decl.name.length = node->as.var_decl.name.length;
            clone->as.var_decl.initializer =
                ast_clone(arena, node->as.var_decl.initializer);
            break;

        case NODE_VAR_DECL_GROUP:
            clone->as.var_decl_group.declarations.items = NULL;
            clone->as.var_decl_group.declarations.count = 0;
            clone->as.var_decl_group.declarations.capacity = 0;
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.var_decl_group.declarations,
                    ast_clone(arena, node->as.var_decl_group.declarations.items[i])
                );
            }
            break;

        case NODE_FUNC_PARAM_DECL:
            clone->as.param_decl.var_type    = node->as.param_decl.var_type;
            clone->as.param_decl.name.data   = node->as.param_decl.name.data;
            clone->as.param_decl.name.length = node->as.param_decl.name.length;
            clone->as.param_decl.is_pack     = node->as.param_decl.is_pack;
            break;

        case NODE_STRUCT_FIELD_DECL:
            clone->as.struct_field_decl.var_type =
                node->as.struct_field_decl.var_type;

            clone->as.struct_field_decl.name.data =
                node->as.struct_field_decl.name.data;

            clone->as.struct_field_decl.name.length =
                node->as.struct_field_decl.name.length;
            break;

        case NODE_FUNC_DECL:
            clone->as.func_decl.name.data     = node->as.func_decl.name.data;
            clone->as.func_decl.name.length   = node->as.func_decl.name.length;
            clone->as.func_decl.type_parameters.items = NULL;
            clone->as.func_decl.type_parameters.count = node->as.func_decl.type_parameters.count;
            clone->as.func_decl.type_parameters.capacity = node->as.func_decl.type_parameters.count;
            if (node->as.func_decl.type_parameters.count > 0) {
                clone->as.func_decl.type_parameters.items = arena_alloc(
                    arena,
                    sizeof(GenericTypeParameter) * (size_t)node->as.func_decl.type_parameters.count
                );
                memcpy(
                    clone->as.func_decl.type_parameters.items,
                    node->as.func_decl.type_parameters.items,
                    sizeof(GenericTypeParameter) * (size_t)node->as.func_decl.type_parameters.count
                );
            }
            clone->as.func_decl.return_type   = node->as.func_decl.return_type;
            clone->as.func_decl.resolved_type = NULL;
            clone->as.func_decl.linkage       = node->as.func_decl.linkage;
            clone->as.func_decl.is_repr_c      = node->as.func_decl.is_repr_c;
            clone->as.func_decl.is_discardable = node->as.func_decl.is_discardable;
            clone->as.func_decl.c_call_conv    = node->as.func_decl.c_call_conv;
            clone->as.func_decl.is_variadic    = node->as.func_decl.is_variadic;
            clone->as.func_decl.external_name = node->as.func_decl.external_name;

            clone->as.func_decl.params.items = NULL;
            clone->as.func_decl.params.count = 0;
            clone->as.func_decl.params.capacity = 0;

            for (int i = 0; i < node->as.func_decl.params.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.func_decl.params,
                    ast_clone(arena, node->as.func_decl.params.items[i])
                );
            }

            clone->as.func_decl.body =
                ast_clone(arena, node->as.func_decl.body);
            break;

        case NODE_STRUCT_DECL:
            clone->as.struct_decl.name.data   = node->as.struct_decl.name.data;
            clone->as.struct_decl.name.length = node->as.struct_decl.name.length;
            clone->as.struct_decl.type_parameters = node->as.struct_decl.type_parameters;
            clone->as.struct_decl.is_repr_c       = node->as.struct_decl.is_repr_c;
            clone->as.struct_decl.is_resource     = node->as.struct_decl.is_resource;
            clone->as.struct_decl.repr_c_packed    = node->as.struct_decl.repr_c_packed;
            clone->as.struct_decl.repr_c_align     = node->as.struct_decl.repr_c_align;
            clone->as.struct_decl.is_union         = node->as.struct_decl.is_union;
            clone->as.struct_decl.is_incomplete = node->as.struct_decl.is_incomplete;
            clone->as.struct_decl.resolved_type  = NULL;

            clone->as.struct_decl.fields.items = NULL;
            clone->as.struct_decl.fields.count = 0;
            clone->as.struct_decl.fields.capacity = 0;

            for (int i = 0; i < node->as.struct_decl.fields.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.struct_decl.fields,
                    ast_clone(arena, node->as.struct_decl.fields.items[i])
                );
            }


            clone->as.struct_decl.methods.items = NULL;
            clone->as.struct_decl.methods.count = 0;
            clone->as.struct_decl.methods.capacity = 0;

            for (int i = 0; i < node->as.struct_decl.methods.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.struct_decl.methods,
                    ast_clone(arena, node->as.struct_decl.methods.items[i])
                );
            }

            clone->as.struct_decl.operators.items = NULL;
            clone->as.struct_decl.operators.count = 0;
            clone->as.struct_decl.operators.capacity = 0;
            if (node->as.struct_decl.operators.count > 0) {
                size_t bytes = sizeof(StructOperatorDecl) *
                    (size_t)node->as.struct_decl.operators.count;
                clone->as.struct_decl.operators.items = arena_alloc(arena, bytes);
                memcpy(
                    clone->as.struct_decl.operators.items,
                    node->as.struct_decl.operators.items,
                    bytes
                );
                clone->as.struct_decl.operators.count =
                    node->as.struct_decl.operators.count;
                clone->as.struct_decl.operators.capacity =
                    node->as.struct_decl.operators.count;
            }
            break;

        case NODE_STRUCT_INIT:
            clone->as.struct_init.module_name = node->as.struct_init.module_name;
            clone->as.struct_init.name.data   = node->as.struct_init.name.data;
            clone->as.struct_init.name.length = node->as.struct_init.name.length;
            clone->as.struct_init.type_arguments = node->as.struct_init.type_arguments;

            clone->as.struct_init.fields.items    = NULL;
            clone->as.struct_init.fields.count    = 0;
            clone->as.struct_init.fields.capacity = 0;

            for (int i = 0; i < node->as.struct_init.fields.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.struct_init.fields,
                    ast_clone(arena, node->as.struct_init.fields.items[i])
                );
            }
            break;

        case NODE_FIELD_INIT:
            clone->as.field_init.name.data   = node->as.field_init.name.data;
            clone->as.field_init.name.length = node->as.field_init.name.length;

            clone->as.field_init.value =
                ast_clone(arena, node->as.field_init.value);
            break;

        case NODE_ENUM_DECL:
            clone->as.enum_decl.name.data =
                node->as.enum_decl.name.data;

            clone->as.enum_decl.name.length =
                node->as.enum_decl.name.length;

            clone->as.enum_decl.backing_type =
                node->as.enum_decl.backing_type;
            clone->as.enum_decl.is_repr_c =
                node->as.enum_decl.is_repr_c;

            /*
             * Semantic information should not be cloned.
             * The clone must be re-checked.
             */
            clone->as.enum_decl.resolved_type = NULL;

            clone->as.enum_decl.members.items    = NULL;
            clone->as.enum_decl.members.count    = 0;
            clone->as.enum_decl.members.capacity = 0;

            for (int i = 0;
                 i < node->as.enum_decl.members.count;
                 i++) {

                nodelist_push(
                    arena,
                    &clone->as.enum_decl.members,
                    ast_clone(
                        arena,
                        node->as.enum_decl.members.items[i]
                    )
                );
                 }

            break;

        case NODE_ENUM_MEMBER:
            clone->as.enum_member.name.data =
                node->as.enum_member.name.data;

            clone->as.enum_member.name.length =
                node->as.enum_member.name.length;

            clone->as.enum_member.value =
                ast_clone(
                    arena,
                    node->as.enum_member.value
                );

            /*
             * Semantic information.
             */
                clone->as.enum_member.resolved_value.magnitude   = 0;
                clone->as.enum_member.resolved_value.is_negative = 0;

            break;

        case NODE_RETURN:
            clone->as.return_stmt.value =
                ast_clone(arena, node->as.return_stmt.value);
            break;

        case NODE_DEFER:
            clone->as.defer_stmt.statement =
                ast_clone(arena, node->as.defer_stmt.statement);
            break;

        case NODE_WHILE:
            clone->as.while_stmt.condition =
                ast_clone(arena, node->as.while_stmt.condition);

            clone->as.while_stmt.body =
                ast_clone(arena, node->as.while_stmt.body);
            break;

        case NODE_FOR:
            clone->as.for_stmt.condition =
                ast_clone(arena, node->as.for_stmt.condition);

            clone->as.for_stmt.post =
                ast_clone(arena, node->as.for_stmt.post);

            clone->as.for_stmt.body =
                ast_clone(arena, node->as.for_stmt.body);
            break;

        case NODE_SWITCH:
            clone->as.switch_stmt.expression =
                ast_clone(arena, node->as.switch_stmt.expression);

            clone->as.switch_stmt.resolved_type = NULL;

            clone->as.switch_stmt.cases.items = NULL;
            clone->as.switch_stmt.cases.count = 0;
            clone->as.switch_stmt.cases.capacity = 0;

            for (int i = 0; i < node->as.switch_stmt.cases.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.switch_stmt.cases,
                    ast_clone(arena, node->as.switch_stmt.cases.items[i])
                );
            }

            break;

        case NODE_SWITCH_CASE:
            clone->as.switch_case.value =
                ast_clone(arena, node->as.switch_case.value);

            clone->as.switch_case.body =
                ast_clone(arena, node->as.switch_case.body);

            clone->as.switch_case.is_default =
                node->as.switch_case.is_default;

            break;

        case NODE_ARRAY_LITERAL:
            clone->as.array_literal.elements.items    = NULL;
            clone->as.array_literal.elements.count    = 0;
            clone->as.array_literal.elements.capacity = 0;
            clone->as.array_literal.is_zero_initializer =
                node->as.array_literal.is_zero_initializer;
            for (int i = 0; i < node->as.array_literal.elements.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.array_literal.elements,
                    ast_clone(arena, node->as.array_literal.elements.items[i])
                );
            }
            break;

        case NODE_NULL:
        case NODE_BREAK:
        case NODE_CONTINUE:
            // no payload, line/type already copied
            break;

        case NODE_ERROR:
            clone->as.error = node->as.error;
            break;

        default:
            fprintf(stderr,
                "ast_clone: unsupported node type %d\n",
                node->type);
            return NULL;
    }

    return clone;
}

// Simple growable array. Arena-backed, so like everything else here
// it's never individually freed -- doubling the backing storage just
// means the old (smaller) block becomes unreachable garbage inside
// the arena, which is fine: the whole arena goes away together later.
void nodelist_push(Arena *arena, NodeList *list, Node *node) {
    if (list->count == list->capacity) {
        int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        Node **new_items = arena_alloc(arena, sizeof(Node *) * new_capacity);
        if (list->items) {
            memcpy(new_items, list->items, sizeof(Node *) * list->count);
        }
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = node;
}

