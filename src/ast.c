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
    node->as.ident.start  = start;
    node->as.ident.length = length;
    return node;
}

Node *ast_new_string(Arena *arena, const char *start, int length, int line)
{
    Node *node = new_node(arena, NODE_STRING, line);
    node->as.string_literal.start  = start;
    node->as.string_literal.length = length;
    return node;
}

Node *ast_new_char(Arena *arena, const char *start, int length, int line)
{
    Node *node = new_node(arena, NODE_CHAR, line);
    node->as.char_literal.start  = start;
    node->as.char_literal.length = length;
    return node;
}

Node *ast_new_unary(Arena *arena, TokenType op, Node *operand, int line) {
    Node *node = new_node(arena, NODE_UNARY, line);
    node->as.unary.op      = op;
    node->as.unary.operand = operand;
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
    node->as.field.name   = name;
    node->as.field.length = length;
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
    node->as.var_decl.name        = name;
    node->as.var_decl.length      = length;
    node->as.var_decl.initializer = initializer;
    return node;
}

Node *ast_new_param_decl(Arena *arena, Type *type, const char *name, int length, Node *default_value, int line) {
    Node *node = new_node(arena, NODE_PARAM_DECL, line);
    node->as.param_decl.var_type      = type;
    node->as.param_decl.name          = name;
    node->as.param_decl.length        = length;
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
    node->as.func_decl.name        = name;
    node->as.func_decl.name_length = name_length;
    node->as.func_decl.return_type = return_type;
    node->as.func_decl.body        = NULL;

    node->as.func_decl.params.items    = NULL;
    node->as.func_decl.params.count    = 0;
    node->as.func_decl.params.capacity = 0;

    return node;
}

Node *ast_new_struct_decl(Arena *arena, const char *name, int name_length, int line) {
    Node *node = new_node(arena, NODE_STRUCT_DECL, line);
    node->as.struct_decl.name        = name;
    node->as.struct_decl.name_length = name_length;

    node->as.struct_decl.fields.items    = NULL;
    node->as.struct_decl.fields.count    = 0;
    node->as.struct_decl.fields.capacity = 0;

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

        case NODE_UNARY:
            clone->as.unary.op = node->as.unary.op;
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

        case NODE_CALL:
            clone->as.call.callee = ast_clone(arena, node->as.call.callee);

            clone->as.call.arguments.items    = NULL;
            clone->as.call.arguments.count    = 0;
            clone->as.call.arguments.capacity = 0;

            for (int i = 0; i < node->as.call.arguments.count; i++) {
                nodelist_push(
                    arena,
                    &clone->as.call.arguments,
                    ast_clone(
                        arena,
                        node->as.call.arguments.items[i]
                    )
                );
            }
            break;

        case NODE_FIELD:
            clone->as.field.object =
                ast_clone(arena, node->as.field.object);

            clone->as.field.name =
                node->as.field.name;

            clone->as.field.length =
                node->as.field.length;
            break;

        case NODE_INDEX:
            clone->as.index.object =
                ast_clone(arena, node->as.index.object);

            clone->as.index.index =
                ast_clone(arena, node->as.index.index);
            break;

        default:
            /*
             * Defaults should only contain expressions.
             * If you hit this, add support when that node becomes
             * legal inside a default expression.
             */
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

