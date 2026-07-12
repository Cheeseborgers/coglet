#include "../include/ast.h"

#include <stdio.h>
#include <string.h>

#include "../include/utils/utils.h"

static Node *new_node(Arena *arena, NodeType type, int line) {
    Node *node = arena_alloc(arena, sizeof(Node));
    node->line = line;
    node->type = type;
    return node;
}

Node *ast_new_number(Arena *arena, double value, int is_float, int line) {
    Node *node = new_node(arena, NODE_NUMBER, line);
    node->as.number.value    = value;
    node->as.number.is_float = is_float;
    return node;
}

Node *ast_new_ident(Arena *arena, const char *start, int length, int line) {
    Node *node = new_node(arena, NODE_IDENT, line);
    node->as.ident.data  = start;
    node->as.ident.length = length;
    return node;
}

Node *ast_new_string(Arena *arena, const char *start, int length, int line)
{
    Node *node = new_node(arena, NODE_STRING, line);
    node->as.string_literal.data   = start;
    node->as.string_literal.length = length;
    return node;
}

Node *ast_new_char(Arena *arena, const char *start, int length, int line)
{
    Node *node = new_node(arena, NODE_CHAR, line);
    node->as.char_literal.data   = start;
    node->as.char_literal.length = length;
    return node;
}

Node *ast_new_bool(Arena *arena, int value, int line)
{
    Node *node = new_node(arena, NODE_BOOL, line);
    node->as.boolean.value = value;
    return node;
}

Node *ast_new_unary(Arena *arena, TokenType op, Node *operand, int line) {
    Node *node = new_node(arena, NODE_UNARY, line);
    node->as.unary.op      = op;
    node->as.unary.operand = operand;
    return node;
}

Node *ast_new_inc_dec(Arena *arena, TokenType op, Node *target, int is_prefix, int line) {
    Node *node = new_node(arena, NODE_INC_DEC, line);
    node->as.inc_dec.op = op;
    node->as.inc_dec.target = target;
    node->as.inc_dec.is_prefix = is_prefix;
    return node;
}

Node *ast_new_binary(Arena *arena, TokenType op, Node *left, Node *right, int line) {
    Node *node = new_node(arena, NODE_BINARY, line);
    node->as.binary.op     = op;
    node->as.binary.left   = left;
    node->as.binary.right  = right;
    return node;
}

Node *ast_new_assign(Arena *arena,Node *target,Node *value,int line) {
    Node *node = new_node(arena, NODE_ASSIGN, line);
    node->as.assign.target = target;
    node->as.assign.value  = value;
    return node;
}

Node *ast_new_if(Arena *arena, Node *cond, Node *then_b, Node *else_b, int line) {
    Node *node = new_node(arena, NODE_IF, line);
    node->as.if_stmt.condition   = cond;
    node->as.if_stmt.then_branch = then_b;
    node->as.if_stmt.else_branch = else_b;
    return node;
}

Node *ast_new_expr_stmt(Arena *arena, Node *expr, int line) {
    Node *node = new_node(arena, NODE_EXPR_STMT, line);
    node->as.expr_stmt.expr = expr;
    return node;
}

Node *ast_new_block(Arena *arena, int line) {
    Node *node = new_node(arena, NODE_BLOCK, line);
    node->as.block.statements.items    = NULL;
    node->as.block.statements.count    = 0;
    node->as.block.statements.capacity = 0;
    return node;
}

Node *ast_new_call(Arena *arena, Node *callee, int line) {
    Node *node = new_node(arena, NODE_CALL, line);
    node->as.call.callee             = callee;
    node->as.call.arguments.items    = NULL;
    node->as.call.arguments.count    = 0;
    node->as.call.arguments.capacity = 0;
    return node;
}

Node *ast_new_field(Arena *arena, Node *object, const char *name, int length, int line ) {
    Node *node = new_node(arena, NODE_FIELD, line);
    node->as.field.object = object;
    node->as.field.name.data   = name;
    node->as.field.name.length = length;
    return node;
}
Node *ast_new_index(Arena *arena, Node *object, Node *index, int line) {
    Node *node = new_node(arena, NODE_INDEX, line);
    node->as.index.object = object;
    node->as.index.index  = index;
    return node;
}

Node *ast_new_error(Arena *arena, Token token)
{
    Node *node = arena_alloc(arena, sizeof(Node));
    node->type = NODE_ERROR;
    node->line = token.line;
    node->as.error.token = token;
    return node;
}

Node *ast_new_program(Arena *arena, int line) {
    Node *node = new_node(arena, NODE_PROGRAM, line);
    node->as.program.statements.items    = NULL;
    node->as.program.statements.count    = 0;
    node->as.program.statements.capacity = 0;
    return node;
}

Node *ast_new_var_decl(Arena *arena, Type *type, const char *name, int length, Node *initializer, int line) {
    Node *node = new_node(arena, NODE_VAR_DECL, line);
    node->as.var_decl.var_type    = type;
    node->as.var_decl.name.data   = name;
    node->as.var_decl.name.length = length;
    node->as.var_decl.initializer = initializer;
    return node;
}

Node *ast_new_struct_field_decl(Arena *arena, Type *type, const char *name, int length, int line) {
    Node *node = new_node(arena, NODE_STRUCT_FIELD_DECL, line);
    node->as.struct_field_decl.var_type    = type;
    node->as.struct_field_decl.name.data   = name;
    node->as.struct_field_decl.name.length = length;
    return node;
}

Node *ast_new_func_param_decl(Arena *arena, Type *type, const char *name, int length, Node *default_value, int line) {
    Node *node = new_node(arena, NODE_FUNC_PARAM_DECL, line);
    node->as.param_decl.var_type      = type;
    node->as.param_decl.name.data     = name;
    node->as.param_decl.name.length   = length;
    node->as.param_decl.default_value = default_value;
    return node;
}

Node *ast_new_return(Arena *arena, Node *value, int line) {
    Node *node = new_node(arena, NODE_RETURN, line);
    node->as.return_stmt.value = value;
    return node;
}

Node*ast_new_while(Arena *arena, Node *cond, Node *body, int line) {
    Node *node = new_node(arena, NODE_WHILE, line);
    node->as.while_stmt.condition = cond;
    node->as.while_stmt.body      = body;
    return node;
}

Node *ast_new_for(Arena *arena, Node *cond, Node *post, Node *body, int line) {
    Node *node = new_node(arena, NODE_FOR, line);
    node->as.for_stmt.condition  = cond;
    node->as.for_stmt.post       = post;
    node->as.for_stmt.body       = body;
    return node;
}

Node *ast_new_break(Arena *arena, int line) {
    Node *node = new_node(arena, NODE_BREAK, line);
    return node;
}

Node *ast_new_continue(Arena *arena, int line) {
    Node *node = new_node(arena, NODE_CONTINUE, line);
    return node;
}

Node *ast_new_func_decl(Arena *arena, const char *name, int name_length, Type *return_type, int line) {
    Node *node = new_node(arena, NODE_FUNC_DECL, line);
    node->as.func_decl.name.data     = name;
    node->as.func_decl.name.length   = name_length;
    node->as.func_decl.return_type   = return_type;
    node->as.func_decl.body          = NULL;
    node->as.func_decl.resolved_type = NULL;

    node->as.func_decl.params.items    = NULL;
    node->as.func_decl.params.count    = 0;
    node->as.func_decl.params.capacity = 0;

    return node;
}

Node *ast_new_struct_decl(Arena *arena, const char *name, int name_length, int line) {
    Node *node = new_node(arena, NODE_STRUCT_DECL, line);
    node->as.struct_decl.name.data   = name;
    node->as.struct_decl.name.length = name_length;

    node->as.struct_decl.fields.items    = NULL;
    node->as.struct_decl.fields.count    = 0;
    node->as.struct_decl.fields.capacity = 0;

    return node;
}

Node *ast_new_struct_init(Arena *arena, const char *name, int name_length, int line) {
    Node *node = new_node(arena, NODE_STRUCT_INIT, line);
    node->as.struct_init.name.data   = name;
    node->as.struct_init.name.length = name_length;

    node->as.struct_init.fields.items    = NULL;
    node->as.struct_init.fields.count    = 0;
    node->as.struct_init.fields.capacity = 0;

    return node;
}

Node *ast_new_enum_decl(Arena *arena, const char *name, int name_length, int line) {
    Node *node = new_node(arena, NODE_ENUM_DECL, line);
    node->as.enum_decl.name.data   = name;
    node->as.enum_decl.name.length = name_length;

    node->as.enum_decl.backing_type  = NULL;
    node->as.enum_decl.resolved_type = NULL;

    node->as.enum_decl.members.items    = NULL;
    node->as.enum_decl.members.count    = 0;
    node->as.enum_decl.members.capacity = 0;

    return node;
}
Node *ast_new_enum_member(Arena *arena, const char *name, int name_length, int line) {
    Node *node = new_node(arena, NODE_ENUM_MEMBER, line);
    node->as.enum_member.name.data   = name;
    node->as.enum_member.name.length = name_length;

    node->as.enum_member.value          = NULL;
    node->as.enum_member.resolved_value = 0;

    return node;
}

Node *ast_new_field_init(Arena *arena, const char *name, int name_length, Node *value, int line) {
    Node *node = new_node(arena, NODE_FIELD_INIT, line);
    node->as.field_init.name.data   = name;
    node->as.field_init.name.length = name_length;
    node->as.field_init.value       = value;
    return node;
}

Node *ast_new_const_decl(Arena *arena, Type *type, const char *name, int name_length, Node *value, int line) {
    Node *node = new_node(arena, NODE_CONST_DECL, line);
    node->as.const_decl.const_type  = type;
    node->as.const_decl.name.data   = name;
    node->as.const_decl.name.length = name_length;
    node->as.const_decl.value       = value;
    return node;
}


Node *ast_clone(Arena *arena, const Node *node)
{
    if (!node)
        return NULL;

    Node *clone = new_node(arena, node->type, node->line);

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
            clone->as.unary.op = node->as.unary.op;
            clone->as.unary.operand = ast_clone(arena, node->as.unary.operand);
            break;

        case NODE_BINARY:
            clone->as.binary.op = node->as.binary.op;
            clone->as.binary.left = ast_clone(arena, node->as.binary.left);
            clone->as.binary.right = ast_clone(arena, node->as.binary.right);
            break;

        case NODE_ASSIGN:
            clone->as.assign.target = ast_clone(arena, node->as.assign.target);
            clone->as.assign.value = ast_clone(arena, node->as.assign.value);
            break;

        case NODE_IF:
            clone->as.if_stmt.condition =
                ast_clone(arena, node->as.if_stmt.condition);

            clone->as.if_stmt.then_branch =
                ast_clone(arena, node->as.if_stmt.then_branch);

            clone->as.if_stmt.else_branch =
                ast_clone(arena, node->as.if_stmt.else_branch);
            break;

        case NODE_EXPR_STMT:
            clone->as.expr_stmt.expr =
                ast_clone(arena, node->as.expr_stmt.expr);
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
            clone->as.call.callee =
                ast_clone(arena, node->as.call.callee);

            clone->as.call.arguments.items = NULL;
            clone->as.call.arguments.count = 0;
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
            clone->as.field.object =
                ast_clone(arena, node->as.field.object);

            clone->as.field.name.data = node->as.field.name.data;
            clone->as.field.name.length = node->as.field.name.length;
            break;

        case NODE_INDEX:
            clone->as.index.object =
                ast_clone(arena, node->as.index.object);

            clone->as.index.index =
                ast_clone(arena, node->as.index.index);
            break;

        case NODE_PROGRAM:
            clone->as.program.statements.items = NULL;
            clone->as.program.statements.count = 0;
            clone->as.program.statements.capacity = 0;

            for (int i = 0; i < node->as.program.statements.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.program.statements,
                    ast_clone(arena, node->as.program.statements.items[i])
                );
            }
            break;

        case NODE_VAR_DECL:
            clone->as.var_decl.var_type = node->as.var_decl.var_type;
            clone->as.var_decl.name.data = node->as.var_decl.name.data;
            clone->as.var_decl.name.length = node->as.var_decl.name.length;
            clone->as.var_decl.initializer =
                ast_clone(arena, node->as.var_decl.initializer);
            break;

        case NODE_FUNC_PARAM_DECL:
            clone->as.param_decl.var_type = node->as.param_decl.var_type;
            clone->as.param_decl.name.data = node->as.param_decl.name.data;
            clone->as.param_decl.name.length = node->as.param_decl.name.length;
            clone->as.param_decl.default_value =
                ast_clone(arena, node->as.param_decl.default_value);
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
            clone->as.func_decl.return_type   = node->as.func_decl.return_type;
            clone->as.func_decl.resolved_type = NULL;

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
            break;

        case NODE_STRUCT_INIT:
            clone->as.struct_init.name.data   = node->as.struct_init.name.data;
            clone->as.struct_init.name.length = node->as.struct_init.name.length;

            clone->as.struct_init.fields.items = NULL;
            clone->as.struct_init.fields.count = 0;
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

                /*
                 * Semantic information should not be cloned.
                 * The clone must be re-checked.
                 */
                clone->as.enum_decl.resolved_type = NULL;

                clone->as.enum_decl.members.items = NULL;
                clone->as.enum_decl.members.count = 0;
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
                clone->as.enum_member.resolved_value = 0;

                break;

        case NODE_RETURN:
            clone->as.return_stmt.value =
                ast_clone(arena, node->as.return_stmt.value);
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

