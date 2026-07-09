#include "../include/semantic_anal.h"

#include <stdio.h>
#include <string.h>

static void semantic_error(SemanticContext *ctx, Node *node, const char *msg) {

    printf("semantic error at line %d: %s\n",node->line,msg);
    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error_name(SemanticContext *ctx, Node *node, const char *prefix, const char *name, int length) {

    printf("semantic error at line %d: %s '%.*s'\n", node->line, prefix,length,name);
    ctx->had_error = 1;
    ctx->error_count++;
}

// ============================================================
// scope management
// ============================================================

static Scope *scope_new(SemanticContext *ctx, Scope *parent) {

    Scope *scope = arena_alloc(ctx->arena, sizeof(Scope));
    scope->symbols = NULL;
    scope->parent = parent;
    return scope;
}

static void scope_push(SemanticContext *ctx) { ctx->current_scope = scope_new(ctx, ctx->current_scope); }
static void scope_pop(SemanticContext *ctx)  { ctx->current_scope = ctx->current_scope->parent; }

// ============================================================
// symbols
// ============================================================
static int names_equal(const char *a, int a_len, const char *b, int b_len) {
    return a_len == b_len && memcmp(a,b,a_len) == 0;
}

static Type *find_struct_field(Type *struct_type, const char *name, int length) {

    for(int i = 0; i < struct_type->field_count; i++) {

        StructField *field = &struct_type->fields[i];
        if(names_equal(field->name,field->length,name,length)) return field->type;
    }

    return NULL;
}

// searches current scope only
static Symbol *scope_find_local(Scope *scope, const char *name, int length) {

    for(Symbol *sym = scope->symbols; sym; sym = sym->next)
        if(names_equal(sym->name,sym->length,name,length)) return sym;

    return NULL;
}

// searches current + parents
static Symbol *scope_lookup(Scope *scope, const char *name, int length) {

    for(Scope *s = scope; s; s=s->parent) {
        Symbol *sym = scope_find_local(s,name,length);
        if(sym) return sym;
    }

    return NULL;
}

static void scope_define(SemanticContext *ctx, const char *name, int length, SymbolKind kind, Type *type) {

    Symbol *sym = arena_alloc(ctx->arena, sizeof(Symbol));

    sym->name = name;
    sym->length = length;
    sym->kind = kind;
    sym->type = type;

    sym->next = ctx->current_scope->symbols;

    ctx->current_scope->symbols = sym;
}

static Type *lookup_type(SemanticContext *ctx, const char *name, int length) {

    Symbol *sym = scope_lookup(ctx->current_scope, name, length);

    if(!sym || sym->kind != SYMBOL_TYPE)
        return NULL;

    return sym->type;
}

// Resolves a parsed Type into its fully-realized form: struct types get
// their name looked up against declared struct symbols (populating
// fields/field_count), and pointer/array types get their element
// resolved recursively. Leaves everything else untouched. `error_node`
// is used purely for diagnostic line info if resolution fails -- pass
// whatever Node this type came from (a var decl, a param, a field).
static Type *resolve_type(SemanticContext *ctx, Type *type, Node *error_node) {

    if (!type) return NULL;

    if (type->kind == TYPE_STRUCT) {
        Type *resolved = lookup_type(ctx, type->struct_name, type->struct_name_length);
        if (!resolved) {
            semantic_error(ctx, error_node, "unknown struct type");
            return type;   // leave unresolved rather than NULL, avoids extra cascading errors
        }
        return resolved;
    }

    if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY) {
        Type *resolved_element = resolve_type(ctx, type->element, error_node);
        if (resolved_element != type->element) {
            Type *copy = arena_alloc(ctx->arena, sizeof(Type));
            *copy = *type;
            copy->element = resolved_element;
            return copy;
        }
    }

    return type;
}

// ============================================================
// forward declarations
// ============================================================

static void check_node(SemanticContext *ctx, Node *node);

// ============================================================
// expressions
// ============================================================
// TODO: add a is bool type check
static int is_integer_kind(TypeKind k) {

    switch (k) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
            return 1;
        default:
            return 0;
    }
}

static int is_float_kind(TypeKind k) { return k == TYPE_F32 || k == TYPE_F64; }
static int is_numeric_type(Type *t)  { return t && (is_integer_kind(t->kind) || is_float_kind(t->kind)); }

static int type_equal(Type *a, Type *b) {

    if (a == NULL || b == NULL)
        return 0;

    if (a->kind != b->kind)
        return 0;

    if (a->kind == TYPE_POINTER || a->kind == TYPE_ARRAY)
        return type_equal(a->element, b->element);

    if (a->kind == TYPE_STRUCT)
        return names_equal(
            a->struct_name, a->struct_name_length,
            b->struct_name, b->struct_name_length);

    return 1;
}

static int is_int_literal_zero(Node *node) {

    return node && node->type == NODE_NUMBER &&
           !node->as.number.is_float && node->as.number.value == 0;
}

/**
 * Determines whether a value of one type may be used to initialize an object
 * of another type.
 *
 * Current compatibility rules:
 *   - Exact type matches are always accepted.
 *   - The integer literal 0 is treated as a null pointer constant and may
 *     initialize any pointer type.
 *   - Any integer type may initialize any other integer type.
 *   - Any floating-point type may initialize any other floating-point type.
 *
 * Note:
 *   Integer and floating-point compatibility is intentionally permissive.
 *   The function does not distinguish between widening and narrowing
 *   conversions, so potentially lossy initializations (e.g. i64 -> u8 or
 *   f64 -> f32) are accepted just as freely as lossless ones (e.g. u8 -> i64
 *   or f32 -> f64). This keeps initialization rules simple for now and is
 *   consistent with the current type system. A future refinement may restrict
 *   implicit conversions to widening cases and require explicit casts for
 *   narrowing conversions.
 *
 * @param declared  The declared type of the object being initialized.
 * @param init_type The type of the initializer expression.
 * @param init_node The initializer AST node, used to recognize special cases
 *                  such as the integer literal 0 for null pointer
 *                  initialization.
 *
 * @return 1 if the initializer is considered compatible with the declared
 *         type under the current implicit conversion rules, otherwise 0.
 */
static int initializer_compatible(Type *declared, Type *init_type, Node *init_node) {

    if (!declared || !init_type) return 1;

    if (type_equal(declared, init_type)) return 1;

    // integer literal 0 is a valid null-pointer constant for any pointer type
    if (declared->kind == TYPE_POINTER && is_int_literal_zero(init_node)) return 1;

    // any-width integer literal may initialize any-width integer type
    if (is_integer_kind(declared->kind) && is_integer_kind(init_type->kind)) return 1;

    // any-width float literal may initialize any-width float type
    if (is_float_kind(declared->kind) && is_float_kind(init_type->kind)) return 1;

    return 0;
}

static Type *check_expression(SemanticContext *ctx, Node *node) {

    if (!node) return NULL;

    switch(node->type)
    {
        case NODE_IDENT:
        {
            Symbol *sym = scope_lookup(ctx->current_scope, node->as.ident.start, node->as.ident.length);
            if (!sym) {
                semantic_error_name(ctx,node, "undefined identifier", node->as.ident.start, node->as.ident.length);
                return NULL;
            }

            return sym->type;
        }

        case NODE_UNARY:
        {
            Type *operand =check_expression(ctx, node->as.unary.operand);
            if (!operand) return NULL;

            switch(node->as.unary.op)
            {
                case TOK_MINUS:
                    if(!is_numeric_type(operand)) {
                        semantic_error(ctx,node, "unary '-' requires numeric operand");
                        return NULL;
                    }

                    return operand;

                default:
                    return operand;
            }
        }

        case NODE_BINARY:
        {
            Type *left  = check_expression(ctx, node->as.binary.left);
            Type *right = check_expression(ctx,node->as.binary.right);

            if(!left || !right) return NULL;

            switch(node->as.binary.op)
            {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                {
                    if(!is_numeric_type(left)) {
                        semantic_error(ctx, node, "left operand must be numeric");
                        return NULL;
                    }


                    if(!is_numeric_type(right)) {
                        semantic_error(ctx,node, "right operand must be numeric");
                        return NULL;
                    }

                    // TODO: is this just a temporary rule?
                    // result type follows left operand
                    return left;
                }


                case TOK_EQUAL_EQUAL:
                case TOK_BANG_EQUAL:
                case TOK_LESS:
                case TOK_GREATER:
                case TOK_LESS_EQUAL:
                case TOK_GREATER_EQUAL:
                {
                    if(!type_equal(left,right)) {
                        semantic_error(ctx,node, "comparison type mismatch");
                        return NULL;
                    }

                    Type *t = arena_alloc(ctx->arena, sizeof(Type));
                    t->kind = TYPE_BOOL;
                    t->element = NULL;
                    t->array_size = -1;

                    return t;
                }

                default:
                    return NULL;
            }
        }

        case NODE_ASSIGN:
        {
            Type *target =check_expression(ctx, node->as.assign.target);
            Type *value = check_expression(ctx, node->as.assign.value);

            if(!target || !value) return NULL;

            if(!initializer_compatible(target, value, node->as.assign.value)) {
                semantic_error(ctx,node, "assignment type mismatch");
                return NULL;
            }

            return target;
        }

        case NODE_CALL:
        {
            Type *callee = check_expression(ctx, node->as.call.callee);

            int argc = node->as.call.arguments.count;
            Type **arg_types = argc ? arena_alloc(ctx->arena, sizeof(Type*) * argc) : NULL;

            if (arg_types == NULL) return NULL;

            for (int i = 0; i < argc; i++) {
                arg_types[i] = check_expression(ctx, node->as.call.arguments.items[i]);
            }

            if (!callee) return NULL;

            if (callee->kind != TYPE_FUNCTION) {
                semantic_error(ctx, node, "called object is not a function");
                return NULL;
            }

            if (argc != callee->parameter_count) {
                semantic_error(ctx, node, "wrong number of arguments");
                return NULL;
            }

            for (int i = 0; i < argc; i++) {
                if (arg_types[i] && !type_equal(arg_types[i], callee->parameters[i])) {
                    semantic_error(ctx, node, "argument type mismatch");
                }
            }

            return callee->return_type;
        }

        case NODE_FIELD:
        {
            Type *object = check_expression(ctx, node->as.field.object);

            if(!object) return NULL;

            if(object->kind != TYPE_STRUCT) {
                semantic_error(ctx, node, "field access requires a struct");
                return NULL;
            }

            Type *field = find_struct_field(object,node->as.field.name, node->as.field.length);
            if(!field) {
                semantic_error(ctx, node, "unknown struct field");
                return NULL;
            }

            return field;
        }

        case NODE_INDEX:
        {
            Type *object = check_expression(ctx, node->as.index.object);
            Type *index  = check_expression(ctx, node->as.index.index);

            if(!object || !index) return NULL;

            if(!is_integer_kind(index->kind)) {
                semantic_error(ctx, node, "array index must be integer");
                return NULL;
            }

            if(object->kind != TYPE_ARRAY &&
               object->kind != TYPE_POINTER) {
                semantic_error(ctx, node, "object is not indexable");
                return NULL;
            }

            return object->element;
        }

        case NODE_NUMBER: {
            Type *t = arena_alloc(ctx->arena, sizeof(Type));
            t->kind       = node->as.number.is_float ? TYPE_F64 : TYPE_I32;
            t->element    = NULL;
            t->array_size = -1;
            return t;
        }

        case NODE_STRING: {
            Type *elem = arena_alloc(ctx->arena, sizeof(Type));
            elem->kind       = TYPE_U8;
            elem->element    = NULL;
            elem->array_size = -1;

            Type *t = arena_alloc(ctx->arena, sizeof(Type));
            t->kind       = TYPE_POINTER;
            t->element    = elem;
            t->array_size = -1;

            return t;
        }

        case NODE_CHAR: {

            Type *t = arena_alloc(ctx->arena, sizeof(Type));
            t->kind = TYPE_U8;
            t->element = NULL;
            t->array_size = -1;

            return t;
        }

        default:
            break;
    }

    // important fallback
    return NULL;
}

// ============================================================
// statements
// ============================================================
static void check_block(SemanticContext *ctx, Node *node) {

    scope_push(ctx);

    for(int i=0; i < node->as.block.statements.count; i++) {
        check_node(ctx, node->as.block.statements.items[i]);
    }

    scope_pop(ctx);
}

static void check_var_decl(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.var_decl.name, node->as.var_decl.length)) {
        semantic_error_name(ctx, node, "duplicate declaration", node->as.var_decl.name, node->as.var_decl.length);
        return;
    }

    Type *init_type = NULL;
    if (node->as.var_decl.initializer)
        init_type = check_expression(ctx, node->as.var_decl.initializer);

    Type *type = node->as.var_decl.var_type;

    // Step 1: resolve the declared type, independent of any initializer.
    if (!type) {
        type = init_type;   // ':=' inference
    } else {
        type = resolve_type(ctx, type, node);
    }

    // Step 2: if there's both a declared type and an initializer, check
    // they're compatible -- runs regardless of which branch above fired,
    // including the struct case, using whatever `type` ended up as.
    if (node->as.var_decl.var_type && init_type &&
        !initializer_compatible(type, init_type, node->as.var_decl.initializer)) {
        semantic_error(ctx, node, "initializer type does not match declared type");
    }

    scope_define(ctx, node->as.var_decl.name, node->as.var_decl.length, SYMBOL_VARIABLE, type);
}

static void check_program(SemanticContext *ctx, Node *node) {

    for(int i=0; i < node->as.program.statements.count; i++) {
        check_node(ctx, node->as.program.statements.items[i]);
    }
}

static void check_if(SemanticContext *ctx, Node *node) {

    Type *cond = check_expression(ctx, node->as.if_stmt.condition);

    if (cond && cond->kind != TYPE_BOOL)
        semantic_error(ctx, node->as.if_stmt.condition, "if condition must be a boolean expression");

    check_node(ctx, node->as.if_stmt.then_branch);

    if(node->as.if_stmt.else_branch)
        check_node(ctx, node->as.if_stmt.else_branch);
}

// Check functions -------------------------------------------
static Type *make_function_type(SemanticContext *ctx, Node *func) {

    Type *type = arena_alloc(ctx->arena, sizeof(Type));
    type->kind = TYPE_FUNCTION;

    type->parameter_count = func->as.func_decl.params.count;
    type->parameters      = arena_alloc(ctx->arena, sizeof(Type *) * type->parameter_count);

    for (int i = 0; i < type->parameter_count; i++) {
        Node *param = func->as.func_decl.params.items[i];
        type->parameters[i] = resolve_type(ctx, param->as.var_decl.var_type, param);
    }

    type->return_type = resolve_type(ctx, func->as.func_decl.return_type, func);

    return type;
}

static void check_function(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.func_decl.name, node->as.func_decl.name_length)) {

        semantic_error_name(ctx, node, "duplicate declaration",
            node->as.func_decl.name, node->as.func_decl.name_length);
        return;
    }

    // function itself is visible in current scope
    Type *func_type = make_function_type(ctx,node);

    scope_define(ctx,node->as.func_decl.name, node->as.func_decl.name_length, SYMBOL_FUNCTION, func_type);

    scope_push(ctx);

    for(int i = 0; i < node->as.func_decl.params.count; i++) {

        Node *param = node->as.func_decl.params.items[i];
        Type *param_type = resolve_type(ctx, param->as.var_decl.var_type, param);
        scope_define(ctx,param->as.var_decl.name, param->as.var_decl.length, SYMBOL_VARIABLE, param_type);
    }

    int saved_loop_depth = ctx->loop_depth;

    ctx->loop_depth = 0;
    ctx->function_depth++;

    check_node(ctx, node->as.func_decl.body);

    ctx->function_depth--;
    ctx->loop_depth = saved_loop_depth;

    scope_pop(ctx);
}

// ============================================================
// node dispatcher
// ============================================================
static Type *make_struct_type(SemanticContext *ctx, Node *node) {

    Type *type = arena_alloc(ctx->arena,sizeof(Type));

    type->kind = TYPE_STRUCT;

    type->struct_name        = node->as.struct_decl.name;
    type->struct_name_length = node->as.struct_decl.name_length;
    type->field_count        = node->as.struct_decl.fields.count;

    type->fields = arena_alloc(ctx->arena, sizeof(StructField) * type->field_count);

    for (int i = 0; i < type->field_count; i++) {

        Node *field = node->as.struct_decl.fields.items[i];
        type->fields[i].name = field->as.var_decl.name;
        type->fields[i].length = field->as.var_decl.length;
        type->fields[i].type = resolve_type(ctx, field->as.var_decl.var_type, field);
    }

    return type;
}

static void check_node(SemanticContext *ctx,Node *node) {

    if(!node) return;

    switch(node->type) {

    case NODE_BLOCK:     check_block(ctx,node);    break;
    case NODE_PROGRAM:   check_program(ctx,node);  break;
    case NODE_VAR_DECL:  check_var_decl(ctx,node); break;
    case NODE_FUNC_DECL: check_function(ctx,node); break;
    case NODE_IF:        check_if(ctx,node);       break;

    case NODE_EXPR_STMT: check_expression(ctx, node->as.expr_stmt.expr); break;
    case NODE_BREAK: if(ctx->loop_depth == 0) semantic_error(ctx,node,"break outside loop"); break;

    case NODE_STRUCT_DECL: {

        Type *type = make_struct_type(ctx,node);
        scope_define(ctx,node->as.struct_decl.name, node->as.struct_decl.name_length, SYMBOL_TYPE, type);
        break;
    }

    case NODE_WHILE: {

        Type *cond = check_expression(ctx, node->as.while_stmt.condition);

        if (cond && cond->kind != TYPE_BOOL)
            semantic_error(ctx, node->as.while_stmt.condition, "while condition must be a boolean expression");

        ctx->loop_depth++;
        check_node(ctx, node->as.while_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_FOR: {

        Type *cond = check_expression(ctx, node->as.for_stmt.condition);

        if (cond && cond->kind != TYPE_BOOL)
            semantic_error(ctx, node->as.for_stmt.condition, "for condition must be a boolean expression");

        check_expression(ctx, node->as.for_stmt.post);

        ctx->loop_depth++;
        check_node(ctx, node->as.for_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_CONTINUE:
        if(ctx->loop_depth == 0) semantic_error(ctx,node,"continue outside loop");
        break;

    case NODE_RETURN:
        if(ctx->function_depth == 0) semantic_error(ctx,node,"return outside function");
        check_expression(ctx, node->as.return_stmt.value);
        break;

    default:
        break;
    }
}

// ============================================================
// public entry
// ============================================================
void semantic_check(Node *program, SemanticContext *ctx) {
    ctx->had_error      = 0;
    ctx->loop_depth     = 0;
    ctx->function_depth = 0;
    ctx->current_scope = scope_new(ctx, NULL);
    check_node(ctx,  program);
}