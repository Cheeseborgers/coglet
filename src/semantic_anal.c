#include "semantic_anal.h"

#include <stdio.h>
#include <string.h>

static void semantic_error(SemanticContext *ctx, const Node *node, const char *msg) {

    printf("semantic error at line %d: %s\n",node->line,msg);
    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error_name(
    SemanticContext *ctx, const Node *node, const char *prefix, const char *name, size_t length) {

    printf("semantic error at line %d: %s '%.*s'\n", node->line, prefix, (int)length, name);
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
static int names_equal(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a,b,a_len) == 0;
}

static Type *find_struct_field(const Type *struct_type, const char *name, size_t length) {

    for(int i = 0; i < struct_type->field_count; i++) {

        StructField *field = &struct_type->fields[i];
        if(names_equal(field->name.data, field->name.length, name, length)) return field->type;
    }

    return NULL;
}

// searches current scope only
static Symbol *scope_find_local(const Scope *scope, const char *name, size_t length) {

    for(Symbol *sym = scope->symbols; sym; sym = sym->next)
        if(names_equal(sym->name.data, sym->name.length, name, length)) return sym;

    return NULL;
}

// searches current + parents
static Symbol *scope_lookup(Scope *scope, const char *name, size_t length) {

    for(Scope *s = scope; s; s=s->parent) {
        Symbol *sym = scope_find_local(s,name,length);
        if(sym) return sym;
    }

    return NULL;
}

static void scope_define(SemanticContext *ctx, StringView name, SymbolKind kind, Type *type) {

    Symbol *sym = arena_alloc(ctx->arena, sizeof(Symbol));

    sym->name        = name;
    sym->kind        = kind;
    sym->type        = type;

    sym->next = ctx->current_scope->symbols;

    ctx->current_scope->symbols = sym;
}

static Type *lookup_type(SemanticContext *ctx, const char *name, size_t length) {

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

    switch (type->kind) {
        case TYPE_BOOL: return ctx->type_bool;
        case TYPE_VOID: return ctx->type_void;
        default: break;
    }

    if (type->kind == TYPE_STRUCT) {
        Type *resolved = lookup_type(ctx, type->struct_name.data, type->struct_name.length);
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
static int  declare_struct_shell(SemanticContext *ctx, Node *node);
static void fill_struct_fields(SemanticContext *ctx, Node *node);
static int  declare_function_signature(SemanticContext *ctx, Node *node);
static void check_function_body(SemanticContext *ctx, Node *node);
static void check_const_decl(SemanticContext *ctx, Node *node);

// ============================================================
// expressions
// ============================================================
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

static int is_assignable_node(Node *node)
{
    if (!node)
        return 0;

    switch (node->type) {
        case NODE_IDENT:
        case NODE_FIELD:
        case NODE_INDEX:
            return 1;

        default:
            return 0;
    }
}

// TODO: integer -> float promotion rules
static int type_width(TypeKind kind)
{
    switch(kind)
    {
        case TYPE_I8:
        case TYPE_U8:
            return 8;

        case TYPE_I16:
        case TYPE_U16:
            return 16;

        case TYPE_I32:
        case TYPE_U32:
        case TYPE_F32:
            return 32;

        case TYPE_I64:
        case TYPE_U64:
        case TYPE_F64:
            return 64;

        default:
            return 0;
    }
}

static int numeric_promotion_rank(TypeKind kind)
{
    switch(kind)
    {
        case TYPE_I8:
        case TYPE_U8:
            return 1;

        case TYPE_I16:
        case TYPE_U16:
            return 2;

        case TYPE_I32:
        case TYPE_U32:
            return 3;

        case TYPE_I64:
        case TYPE_U64:
            return 4;

        case TYPE_F32:
            return 5;

        case TYPE_F64:
            return 6;

        default:
            return 0;
    }
}

static int type_equal(Type *a, Type *b) {

    if (a == NULL || b == NULL)
        return 0;

    if (a->kind != b->kind)
        return 0;

    if (a->kind == TYPE_POINTER || a->kind == TYPE_ARRAY)
        return type_equal(a->element, b->element);

    if (a->kind == TYPE_STRUCT)
        return names_equal(
            a->struct_name.data, a->struct_name.length,
            b->struct_name.data, b->struct_name.length);

    return 1;
}

// Determines the result type of a numeric binary operation.
//
//   - If exactly one operand is untyped, the result is the other
//     (concrete) operand's type -- the untyped side just adapts.
//   - If both operands are untyped, the result is the higher-ranked
//     of the two (matches literal-vs-literal arithmetic like `1 + 2.0`).
//   - If both operands are concrete, they must be the exact same type;
//     mixing two named numeric types requires an explicit cast.
static Type *common_numeric_type(Type *a, Type *b) {

    if (!a || !b)
        return NULL;

    int rank_a = numeric_promotion_rank(a->kind);
    int rank_b = numeric_promotion_rank(b->kind);

    if (!rank_a || !rank_b)
        return NULL;

    if (a->is_untyped && !b->is_untyped) return b;
    if (b->is_untyped && !a->is_untyped) return a;

    if (a->is_untyped && b->is_untyped)
        return rank_a >= rank_b ? a : b;

    // both concrete -- must match exactly
    if (!type_equal(a, b))
        return NULL;

    return a;
}

static int is_bool_type(Type *t) { return t && t->kind == TYPE_BOOL; }

static int is_int_literal_zero(Node *node) {
    return node && node->type == NODE_NUMBER && !node->as.number.is_float && node->as.number.value == 0;
}

/**
 * Determines whether a value of one type may be used to initialize an object
 * of another type.
 *
 * Current compatibility rules:
 *   - Exact type matches are always accepted.
 *   - The integer literal 0 is treated as a null pointer constant and may
 *     initialize any pointer type.
 *   - An UNTYPED numeric literal (or constant expression) adapts to any
 *     compatible declared numeric kind: an untyped int can initialize any
 *     integer or float type, and an untyped float can initialize any float
 *     type.
 *   - Two CONCRETE types (e.g. two variables, or a variable and a declared
 *     type) must match exactly. There is no implicit narrowing/widening
 *     between named numeric types -- use an explicit cast.
 *
 * @param declared  The declared type of the object being initialized.
 * @param init_type The type of the initializer expression.
 * @param init_node The initializer AST node, used to recognize special cases
 *                  such as the integer literal 0 for null pointer
 *                  initialization.
 *
 * @return 1 if the initializer is considered compatible with the declared
 *         type under the current rules, otherwise 0.
 */
static int initializer_compatible(Type *declared, Type *init_type, Node *init_node) {

    if (!declared || !init_type) return 1;

    if (type_equal(declared, init_type)) return 1;

    // integer literal 0 is a valid null-pointer constant for any pointer type
    if (declared->kind == TYPE_POINTER && is_int_literal_zero(init_node)) return 1;

    // an untyped numeric literal/constant adapts to any compatible declared kind
    if (init_type->is_untyped) {

        if (is_integer_kind(init_type->kind) &&
            (is_integer_kind(declared->kind) || is_float_kind(declared->kind)))
            return 1;

        if (is_float_kind(init_type->kind) && is_float_kind(declared->kind))
            return 1;
    }

    // two concrete types must match exactly
    return 0;
}

// ============================================================
// compile-time constant evaluation
// ============================================================

static int const_value_to_double(const ConstValue *v, double *out) {
    switch (v->kind) {
        case CONST_VALUE_INT:   *out = (double)v->as.i; return 1;
        case CONST_VALUE_FLOAT: *out = v->as.f;          return 1;
        default: return 0;
    }
}

static Type *const_value_default_type(SemanticContext *ctx, const ConstValue *v) {

    Type *t = arena_alloc(ctx->arena, sizeof(Type));
    t->is_untyped = 1;   // a constant's own value is still untyped until it lands somewhere concrete
    t->element = NULL;
    t->array_size = -1;
    t->struct_name.data = NULL;
    t->struct_name.length = 0;

    switch (v->kind) {
        case CONST_VALUE_INT:   t->kind = TYPE_I32;  break;  // matches NODE_NUMBER's untyped-int default
        case CONST_VALUE_FLOAT: t->kind = TYPE_F64;  break;
        case CONST_VALUE_BOOL:  t->kind = TYPE_BOOL; break;
    }

    return t;
}

// Recursively evaluates an expression that must be knowable at compile
// time: literals, other constants, and unary/binary ops over those.
// Anything reaching outside that (function calls, variables, struct
// inits, etc.) is rejected with a diagnostic.
static int eval_const_expr(SemanticContext *ctx, Node *node, ConstValue *out) {

    if (!node) return 0;

    switch (node->type) {

        case NODE_NUMBER:
            if (node->as.number.is_float) {
                out->kind = CONST_VALUE_FLOAT;
                out->as.f = node->as.number.value;
            } else {
                out->kind = CONST_VALUE_INT;
                out->as.i = (long long)node->as.number.value;
            }
            return 1;

        case NODE_BOOL:
            out->kind = CONST_VALUE_BOOL;
            out->as.b = node->as.boolean.value;
            return 1;

        case NODE_IDENT:
        {
            Symbol *sym = scope_lookup(ctx->current_scope, node->as.ident.data, node->as.ident.length);

            if (!sym || sym->kind != SYMBOL_CONSTANT) {
                semantic_error(ctx, node, "expression is not a compile-time constant");
                return 0;
            }

            *out = sym->const_value;
            return 1;
        }

        case NODE_UNARY:
        {
            ConstValue operand;
            if (!eval_const_expr(ctx, node->as.unary.operand, &operand)) return 0;

            if (node->as.unary.op == TOK_MINUS) {
                if (operand.kind == CONST_VALUE_INT) {
                    out->kind = CONST_VALUE_INT;
                    out->as.i = -operand.as.i;
                    return 1;
                }
                if (operand.kind == CONST_VALUE_FLOAT) {
                    out->kind = CONST_VALUE_FLOAT;
                    out->as.f = -operand.as.f;
                    return 1;
                }
                semantic_error(ctx, node, "unary '-' requires a numeric constant");
                return 0;
            }

            if (node->as.unary.op == TOK_BANG) {
                if (operand.kind != CONST_VALUE_BOOL) {
                    semantic_error(ctx, node, "unary '!' requires a boolean constant");
                    return 0;
                }
                out->kind = CONST_VALUE_BOOL;
                out->as.b = !operand.as.b;
                return 1;
            }

            semantic_error(ctx, node, "operator not allowed in a constant expression");
            return 0;
        }

        case NODE_BINARY:
        {
            ConstValue left, right;
            if (!eval_const_expr(ctx, node->as.binary.left, &left))  return 0;
            if (!eval_const_expr(ctx, node->as.binary.right, &right)) return 0;

            switch (node->as.binary.op) {

                case TOK_PLUS: case TOK_MINUS: case TOK_STAR:
                case TOK_SLASH: case TOK_PERCENT:
                {
                    if (left.kind == CONST_VALUE_INT && right.kind == CONST_VALUE_INT) {

                        long long a = left.as.i, b = right.as.i;
                        out->kind = CONST_VALUE_INT;

                        switch (node->as.binary.op) {
                            case TOK_PLUS:  out->as.i = a + b; return 1;
                            case TOK_MINUS: out->as.i = a - b; return 1;
                            case TOK_STAR:  out->as.i = a * b; return 1;
                            case TOK_SLASH:
                                if (b == 0) {
                                    semantic_error(ctx, node, "division by zero in constant expression"); return 0;
                                }
                                out->as.i = a / b; return 1;
                            case TOK_PERCENT:
                                if (b == 0) {
                                    semantic_error(ctx, node, "division by zero in constant expression"); return 0;
                                }
                                out->as.i = a % b; return 1;
                            default: return 0;
                        }
                    }

                    if (node->as.binary.op == TOK_PERCENT) {
                        semantic_error(ctx, node, "'%' requires integer constants");
                        return 0;
                    }

                    double a, b;
                    if (!const_value_to_double(&left, &a) || !const_value_to_double(&right, &b)) {
                        semantic_error(ctx, node, "operands must be numeric constants");
                        return 0;
                    }

                    out->kind = CONST_VALUE_FLOAT;

                    switch (node->as.binary.op) {
                        case TOK_PLUS:  out->as.f = a + b; return 1;
                        case TOK_MINUS: out->as.f = a - b; return 1;
                        case TOK_STAR:  out->as.f = a * b; return 1;
                        case TOK_SLASH:
                            if (b == 0.0) {
                                semantic_error(ctx, node, "division by zero in constant expression"); return 0;
                            }
                            out->as.f = a / b; return 1;
                        default: return 0;
                    }
                }

                case TOK_AND_AND:
                case TOK_OR_OR:
                {
                    if (left.kind != CONST_VALUE_BOOL || right.kind != CONST_VALUE_BOOL) {
                        semantic_error(ctx, node, "operands must be boolean constants");
                        return 0;
                    }

                    out->kind = CONST_VALUE_BOOL;
                    out->as.b = (node->as.binary.op == TOK_AND_AND)
                        ? (left.as.b && right.as.b)
                        : (left.as.b || right.as.b);
                    return 1;
                }

                default:
                    semantic_error(ctx, node, "operator not allowed in a constant expression");
                    return 0;
            }
        }

        default:
            semantic_error(ctx, node, "expression is not a compile-time constant");
            return 0;
    }
}

static Type *check_expression(SemanticContext *ctx, Node *node) {

    if (!node) return NULL;

    switch(node->type)
    {
        case NODE_IDENT:
        {
            Symbol *sym = scope_lookup(ctx->current_scope, node->as.ident.data, node->as.ident.length);

            if (!sym) {
                semantic_error_name(ctx,node,
                    "undefined identifier",
                    node->as.ident.data,
                    node->as.ident.length);
                return NULL;
            }

            return sym->type;
        }


        case NODE_UNARY:
        {
            Type *operand = check_expression(ctx, node->as.unary.operand);

            if (!operand) return NULL;

            switch(node->as.unary.op)
            {
                case TOK_MINUS:
                {
                    if(!is_numeric_type(operand)) {
                        semantic_error(ctx,node,
                            "unary '-' requires numeric operand");
                        return NULL;
                    }

                    return operand;
                }


                case TOK_BANG:
                {
                    if(!is_bool_type(operand)) {
                        semantic_error(ctx,node,
                            "unary '!' requires boolean operand");
                        return NULL;
                    }

                    return operand;
                }


                default:
                    return operand;
            }
        }


        case NODE_BINARY:
        {
            Type *left  = check_expression(ctx, node->as.binary.left);
            Type *right = check_expression(ctx, node->as.binary.right);

            if(!left || !right) return NULL;


            switch(node->as.binary.op)
            {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                {
                    if (!is_numeric_type(left)) {
                        semantic_error(
                            ctx,
                            node,
                            "left operand must be numeric"
                        );
                        return NULL;
                    }

                    if (!is_numeric_type(right)) {
                        semantic_error(
                            ctx,
                            node,
                            "right operand must be numeric"
                        );
                        return NULL;
                    }

                    /*
                     * Remainder is only defined for integer operands.
                     *
                     * Valid:
                     *     10 % 3
                     *     some_i32 % 2
                     *
                     * Invalid:
                     *     10.5 % 2
                     *     some_f64 % 2.0
                     */
                    if (node->as.binary.op == TOK_PERCENT) {
                        if (!is_integer_kind(left->kind) ||
                            !is_integer_kind(right->kind)) {

                            semantic_error(
                                ctx,
                                node,
                                "'%' requires integer operands"
                            );

                            return NULL;
                            }
                    }

                    Type *result = common_numeric_type(left, right);

                    if (!result) {
                        if (!left->is_untyped &&
                            !right->is_untyped &&
                            !type_equal(left, right)) {

                            semantic_error(
                                ctx,
                                node,
                                "operands are different numeric types -- use an explicit cast"
                            );
                            } else {
                                semantic_error(
                                    ctx,
                                    node,
                                    "could not determine numeric result type"
                                );
                            }

                        return NULL;
                    }

                    return result;
                }


                // logical boolean operators
                case TOK_AND_AND:
                case TOK_OR_OR:
                {
                    if(left->kind != TYPE_BOOL) {
                        semantic_error(ctx,node,
                            "left operand must be boolean");
                        return NULL;
                    }

                    if(right->kind != TYPE_BOOL) {
                        semantic_error(ctx,node,
                            "right operand must be boolean");
                        return NULL;
                    }

                    return ctx->type_bool;
                }


                case TOK_EQUAL_EQUAL:
                case TOK_BANG_EQUAL:
                {
                    /*
                     * Numeric equality follows the same compatibility rules
                     * as numeric arithmetic.
                     *
                     * Examples:
                     *     x: i64;
                     *     x == 10;      // valid: 10 is an untyped literal
                     *
                     *     a: i32;
                     *     b: i64;
                     *     a == b;       // invalid: two different concrete types
                     */
                    if (is_numeric_type(left) && is_numeric_type(right)) {
                        Type *common = common_numeric_type(left, right);

                        if (!common) {
                            semantic_error(
                                ctx,
                                node,
                                "comparison operands have incompatible numeric types"
                            );

                            return NULL;
                        }

                        return ctx->type_bool;
                    }

                    /*
                     * Non-numeric equality still requires exact type equality.
                     *
                     * Examples:
                     *     true == false
                     *     pointer_a == pointer_b
                     *     struct_a == struct_b
                     */
                    if (!type_equal(left, right)) {
                        semantic_error(
                            ctx,
                            node,
                            "comparison type mismatch"
                        );

                        return NULL;
                    }

                    return ctx->type_bool;
                }


                // ordered comparisons require numbers
                case TOK_LESS:
                case TOK_GREATER:
                case TOK_LESS_EQUAL:
                case TOK_GREATER_EQUAL:
                {
                    if (!is_numeric_type(left) ||
                        !is_numeric_type(right)) {

                        semantic_error(
                            ctx,
                            node,
                            "ordered comparison requires numeric operands"
                        );

                        return NULL;
                        }

                    /*
                     * Require the two numeric operands to be compatible.
                     *
                     * Valid:
                     *     x: i64;
                     *     x < 10;
                     *
                     * Invalid:
                     *     a: i32;
                     *     b: f64;
                     *     a < b;
                     */
                    Type *common = common_numeric_type(left, right);

                    if (!common) {
                        semantic_error(
                            ctx,
                            node,
                            "comparison operands have incompatible numeric types"
                        );

                        return NULL;
                    }

                    return ctx->type_bool;
                }


                default:
                    return NULL;
            }
        }

        case NODE_INC_DEC: {
            Node *target = node->as.inc_dec.target;

            if (!is_assignable_node(target)) {
                semantic_error(
                    ctx,
                    node,
                    "increment/decrement target is not assignable");
                return NULL;
            }

            Type *target_type = check_expression(ctx, target);

            if (!target_type)
                return NULL;

            if (!is_numeric_type(target_type)) {
                semantic_error(
                    ctx,
                    node,
                    "increment/decrement requires a numeric target");
                return NULL;
            }

            if (target->type == NODE_IDENT) {
                Symbol *sym = scope_lookup(
                    ctx->current_scope,
                    target->as.ident.data,
                    target->as.ident.length);

                if (sym && sym->kind == SYMBOL_CONSTANT) {
                    semantic_error_name(
                        ctx,
                        node,
                        "cannot modify constant",
                        target->as.ident.data,
                        target->as.ident.length);
                    return NULL;
                }
            }

            return target_type;
        }


        case NODE_ASSIGN:
        {
            Node *target_node = node->as.assign.target;

            if (!is_assignable_node(target_node)) {
                semantic_error(
                    ctx,
                    node,
                    "assignment target is not assignable"
                );
                return NULL;
            }

            if (target_node->type == NODE_IDENT) {
                Symbol *sym = scope_lookup(
                    ctx->current_scope,
                    target_node->as.ident.data,
                    target_node->as.ident.length
                );

                if (sym && sym->kind == SYMBOL_CONSTANT) {
                    semantic_error_name(
                        ctx,
                        node,
                        "cannot assign to constant",
                        target_node->as.ident.data,
                        target_node->as.ident.length
                    );
                    return NULL;
                }
            }

            Type *target_type =
                check_expression(ctx, target_node);

            Type *value_type =
                check_expression(ctx, node->as.assign.value);

            if (!target_type || !value_type)
                return NULL;

            if (!initializer_compatible(
                    target_type,
                    value_type,
                    node->as.assign.value)) {

                semantic_error(
                    ctx,
                    node,
                    "assignment type mismatch"
                );

                return NULL;
                    }

            return target_type;
        }


        case NODE_CALL:
        {
            Type *callee = check_expression(ctx, node->as.call.callee);

            int argc = node->as.call.arguments.count;
            Type **arg_types = argc ?
                arena_alloc(ctx->arena, sizeof(Type*) * argc) :
                NULL;

            if (argc && !arg_types)
                return NULL;


            for (int i = 0; i < argc; i++) {
                arg_types[i] =
                    check_expression(ctx,node->as.call.arguments.items[i]);
            }


            if (!callee)
                return NULL;


            if (callee->kind != TYPE_FUNCTION) {
                semantic_error(ctx,node,
                    "called object is not a function");
                return NULL;
            }


            if (argc != callee->parameter_count) {
                semantic_error(ctx,node,
                    "wrong number of arguments");
                return NULL;
            }


            for (int i = 0; i < argc; i++) {
                Node *arg = node->as.call.arguments.items[i];

                if (arg_types[i] &&
                    !initializer_compatible(
                        callee->parameters[i],
                        arg_types[i],
                        arg)) {
                    semantic_error(ctx, arg,
                        "argument type mismatch");
                        }
            }

            return callee->return_type;
        }


        case NODE_FIELD:
        {
            Type *object = check_expression(ctx,node->as.field.object);

            if(!object)
                return NULL;


            if(object->kind != TYPE_STRUCT) {
                semantic_error(ctx,node,
                    "field access requires a struct");
                return NULL;
            }


            Type *field =
                find_struct_field(object,
                    node->as.field.name.data,
                    node->as.field.name.length);


            if(!field) {
                semantic_error(ctx,node,
                    "unknown struct field");
                return NULL;
            }


            return field;
        }


        case NODE_INDEX:
        {
            Type *object =
                check_expression(ctx,node->as.index.object);

            Type *index =
                check_expression(ctx,node->as.index.index);


            if(!object || !index)
                return NULL;


            if(!is_integer_kind(index->kind)) {
                semantic_error(ctx,node,
                    "array index must be integer");
                return NULL;
            }


            if(object->kind != TYPE_ARRAY &&
               object->kind != TYPE_POINTER) {

                semantic_error(ctx,node,
                    "object is not indexable");
                return NULL;
            }


            return object->element;
        }


        case NODE_NUMBER:
        {
            Type *t = arena_alloc(ctx->arena,sizeof(Type));

            t->kind =
                node->as.number.is_float ?
                TYPE_F64 :
                TYPE_I32;

            t->is_untyped = 1;   // a bare literal, adapts to context
            t->element = NULL;
            t->array_size = -1;

            return t;
        }


        case NODE_STRING:
        {
            Type *elem = arena_alloc(ctx->arena,sizeof(Type));

            elem->kind = TYPE_U8;
            elem->is_untyped = 0;
            elem->element = NULL;
            elem->array_size = -1;


            Type *t = arena_alloc(ctx->arena,sizeof(Type));

            t->kind = TYPE_POINTER;
            t->is_untyped = 0;
            t->element = elem;
            t->array_size = -1;

            return t;
        }


        case NODE_CHAR:
        {
            Type *t = arena_alloc(ctx->arena,sizeof(Type));

            t->kind = TYPE_U8;
            t->is_untyped = 0;
            t->element = NULL;
            t->array_size = -1;

            return t;
        }

        case NODE_BOOL:
        {
            return ctx->type_bool;
        }

        case NODE_STRUCT_INIT:
        {
            Type *type = lookup_type(
                ctx,
                node->as.struct_init.name.data,
                node->as.struct_init.name.length
            );

            if (!type || type->kind != TYPE_STRUCT) {
                semantic_error_name(
                    ctx,
                    node,
                    "unknown struct type",
                    node->as.struct_init.name.data,
                    node->as.struct_init.name.length
                );

                return NULL;
            }

            NodeList *inits = &node->as.struct_init.fields;

            /*
             * Pass 1:
             * Validate every field initializer supplied by the user.
             *
             * Checks:
             *  - duplicate field initializers
             *  - unknown field names
             *  - field value type compatibility
             */
            for (int i = 0; i < inits->count; i++) {
                Node *field_init = inits->items[i];

                const char *field_name =
                    field_init->as.field_init.name.data;

                size_t field_name_length =
                    field_init->as.field_init.name.length;

                int is_duplicate = 0;

                for (int j = 0; j < i; j++) {
                    Node *previous_init = inits->items[j];

                    if (names_equal(
                            previous_init->as.field_init.name.data,
                            previous_init->as.field_init.name.length,
                            field_name,
                            field_name_length)) {

                        semantic_error_name(
                            ctx,
                            field_init,
                            "duplicate field initializer",
                            field_name,
                            field_name_length
                        );

                        is_duplicate = 1;
                        break;
                    }
                }

                /*
                 * The duplicate was already reported. Avoid checking its
                 * value again and producing unnecessary follow-up errors.
                 */
                if (is_duplicate)
                    continue;

                Type *field_type = find_struct_field(
                    type,
                    field_name,
                    field_name_length
                );

                if (!field_type) {
                    semantic_error_name(
                        ctx,
                        field_init,
                        "unknown struct field",
                        field_name,
                        field_name_length
                    );

                    continue;
                }

                Node *value_node = field_init->as.field_init.value;

                Type *value_type =
                    check_expression(ctx, value_node);

                if (value_type &&
                    !initializer_compatible(
                        field_type,
                        value_type,
                        value_node)) {

                    semantic_error(
                        ctx,
                        field_init,
                        "field initializer type mismatch"
                    );
                }
            }

            /*
             * Pass 2:
             * Ensure every declared field in the struct has an initializer.
             */
            for (int field_index = 0;
                 field_index < type->field_count;
                 field_index++) {

                StructField *required_field =
                    &type->fields[field_index];

                int found = 0;

                for (int init_index = 0;
                     init_index < inits->count;
                     init_index++) {

                    Node *field_init =
                        inits->items[init_index];

                    if (names_equal(
                            required_field->name.data,
                            required_field->name.length,
                            field_init->as.field_init.name.data,
                            field_init->as.field_init.name.length)) {

                        found = 1;
                        break;
                    }
                }

                if (!found) {
                    semantic_error_name(
                        ctx,
                        node,
                        "missing struct field initializer",
                        required_field->name.data,
                        required_field->name.length
                    );
                }
            }

            return type;
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

static void check_const_decl(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.const_decl.name.data, node->as.const_decl.name.length)) {
        semantic_error_name(
            ctx, node, "duplicate declaration", node->as.const_decl.name.data, node->as.const_decl.name.length);
        return;
    }

    ConstValue value;
    if (!eval_const_expr(ctx, node->as.const_decl.value, &value)) {
        return;   // eval_const_expr already reported the specific error
    }

    Type *type = node->as.const_decl.const_type;

    if (type) {
        type = resolve_type(ctx, type, node);

        // an untyped int literal is allowed to initialize a float-typed
        // constant -- promote the stored value so later constant folding
        // (e.g. another constant referencing this one) sees a float.
        if (is_float_kind(type->kind) && value.kind == CONST_VALUE_INT) {
            value.kind = CONST_VALUE_FLOAT;
            value.as.f = (double)value.as.i;
        }

        Type *value_type = const_value_default_type(ctx, &value);

        if (!initializer_compatible(type, value_type, node->as.const_decl.value)) {
            semantic_error(ctx, node, "constant value does not match declared type");
        }
    } else {
        type = const_value_default_type(ctx, &value);
    }

    Symbol *sym = arena_alloc(ctx->arena, sizeof(Symbol));
    sym->name.data   = node->as.const_decl.name.data;
    sym->name.length = node->as.const_decl.name.length;
    sym->kind        = SYMBOL_CONSTANT;
    sym->type        = type;
    sym->const_value = value;
    sym->next        = ctx->current_scope->symbols;

    ctx->current_scope->symbols = sym;
}

static void check_var_decl(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.var_decl.name.data, node->as.var_decl.name.length)) {
        semantic_error_name(
            ctx, node, "duplicate variable declaration", node->as.var_decl.name.data, node->as.var_decl.name.length);
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

    scope_define(ctx, node->as.var_decl.name, SYMBOL_VARIABLE, type);
}

static void check_param_decl(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.param_decl.name.data, node->as.param_decl.name.length)) {
        semantic_error_name(
            ctx, node, "duplicate param declaration", node->as.param_decl.name.data, node->as.param_decl.name.length);
        return;
    }

    Type *init_type = NULL;
    if (node->as.param_decl.default_value)
        init_type = check_expression(ctx, node->as.param_decl.default_value);

    Type *type = node->as.param_decl.var_type;

    // Step 1: resolve the declared type, independent of any default_value.
    if (!type) {
        type = init_type;   // ':=' inference
    } else {
        type = resolve_type(ctx, type, node);
    }

    // Step 2: if there's both a declared type and a default_value, check
    // they're compatible -- runs regardless of which branch above fired,
    if (node->as.param_decl.var_type && init_type &&
        !initializer_compatible(type, init_type, node->as.param_decl.default_value)) {
        semantic_error(ctx, node, "default value type does not match declared type");
        }

    scope_define(ctx, node->as.param_decl.name, SYMBOL_VARIABLE, type);
}

static void check_program(SemanticContext *ctx, Node *node) {

    NodeList *stmts = &node->as.program.statements;

    // Pass 1: register every struct name so any struct can reference any
    // other struct -- including ones declared later in the file -- as a
    // field type.
    for (int i = 0; i < stmts->count; i++) {
        if (stmts->items[i]->type == NODE_STRUCT_DECL)
            declare_struct_shell(ctx, stmts->items[i]);
    }

    // Pass 2: now that all struct names are visible, resolve each
    // struct's field types.
    for (int i = 0; i < stmts->count; i++) {
        if (stmts->items[i]->type == NODE_STRUCT_DECL)
            fill_struct_fields(ctx, stmts->items[i]);
    }

    // Pass 3: register every function's signature, so functions can call
    // each other regardless of declaration order, and so param/return
    // types can reference any struct from passes 1-2.
    for (int i = 0; i < stmts->count; i++) {
        if (stmts->items[i]->type == NODE_FUNC_DECL)
            declare_function_signature(ctx, stmts->items[i]);
    }

    // Pass 4: check function bodies (every signature is visible now) and
    // any other top-level statements.
    for (int i = 0; i < stmts->count; i++) {

        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_STRUCT_DECL) continue;   // fully handled in passes 1-2

        if (stmt->type == NODE_FUNC_DECL) {
            check_function_body(ctx, stmt);
            continue;
        }

        check_node(ctx, stmt);
    }
}

static void check_if(SemanticContext *ctx, Node *node) {

    Type *cond = check_expression(ctx, node->as.if_stmt.condition);

    if (!is_bool_type(cond))
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
        type->parameters[i] = resolve_type(ctx, param->as.param_decl.var_type, param);
    }

    type->return_type = resolve_type(ctx, func->as.func_decl.return_type, func);

    return type;
}

static int declare_function_signature(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.func_decl.name.data, node->as.func_decl.name.length)) {
        semantic_error_name(ctx, node, "duplicate declaration",
            node->as.func_decl.name.data, node->as.func_decl.name.length);
        return 0;
    }

    Type *func_type = make_function_type(ctx, node);
    scope_define(ctx, node->as.func_decl.name, SYMBOL_FUNCTION, func_type);
    return 1;
}

static void check_function_body(SemanticContext *ctx, Node *node) {

    scope_push(ctx);

    for (int i = 0; i < node->as.func_decl.params.count; i++) {
        Node *param = node->as.func_decl.params.items[i];
        check_param_decl(ctx, param);
    }

    int saved_loop_depth = ctx->loop_depth;
    Type *saved_return_type = ctx->current_return_type;

    ctx->loop_depth = 0;
    ctx->current_return_type =
        resolve_type(ctx, node->as.func_decl.return_type, node);

    ctx->function_depth++;

    check_node(ctx, node->as.func_decl.body);

    ctx->function_depth--;
    ctx->current_return_type = saved_return_type;
    ctx->loop_depth = saved_loop_depth;

    scope_pop(ctx);
}

static void check_function(SemanticContext *ctx, Node *node) {
    declare_function_signature(ctx, node);
    check_function_body(ctx, node);
}

static int declare_struct_shell(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.struct_decl.name.data, node->as.struct_decl.name.length)) {
        semantic_error_name(ctx, node, "duplicate declaration",
            node->as.struct_decl.name.data, node->as.struct_decl.name.length);
        return 0;
    }

    Type *type = arena_alloc(ctx->arena, sizeof(Type));

    type->kind                = TYPE_STRUCT;
    type->element             = NULL;
    type->array_size          = -1;
    type->struct_name.data    = node->as.struct_decl.name.data;
    type->struct_name.length  = node->as.struct_decl.name.length;
    type->field_count         = 0;
    type->fields              = NULL;   // filled in by fill_struct_fields once all struct names are visible
    type->parameters          = NULL;
    type->parameter_count     = 0;
    type->return_type         = NULL;

    scope_define(ctx, node->as.struct_decl.name, SYMBOL_TYPE, type);

    return 1;
}

static void fill_struct_fields(SemanticContext *ctx, Node *node) {

    Symbol *sym = scope_find_local(ctx->current_scope, node->as.struct_decl.name.data, node->as.struct_decl.name.length);

    // shell registration failed (duplicate name), nothing to fill in
    if (!sym) return;

    Type *type = sym->type;

    // A duplicate struct name means declare_struct_shell already reported
    // the error and left the *first* declaration's symbol in place. Don't
    // let a later duplicate silently overwrite the first struct's fields.
    if (type->fields != NULL) return;

    type->field_count = node->as.struct_decl.fields.count;
    type->fields = arena_alloc(ctx->arena, sizeof(StructField) * type->field_count);

    for (int i = 0; i < type->field_count; i++) {

        Node *field = node->as.struct_decl.fields.items[i];

        for (int j = 0; j < i; j++) {
            if (names_equal(type->fields[j].name.data, type->fields[j].name.length,
                             field->as.struct_field_decl.name.data, field->as.struct_field_decl.name.length)) {
                semantic_error_name(ctx, field, "duplicate struct field",
                    field->as.struct_field_decl.name.data, field->as.struct_field_decl.name.length);
            }
        }

        type->fields[i].name.data   = field->as.struct_field_decl.name.data;
        type->fields[i].name.length = field->as.struct_field_decl.name.length;
        type->fields[i].type   = resolve_type(ctx, field->as.struct_field_decl.var_type, field);
    }
}

// ============================================================
// node dispatcher
// ============================================================
static void check_node(SemanticContext *ctx,Node *node) {

    if(!node) return;

    switch(node->type) {

    case NODE_BLOCK:           check_block(ctx,node);      break;
    case NODE_PROGRAM:         check_program(ctx,node);    break;
    case NODE_VAR_DECL:        check_var_decl(ctx,node);   break;
    case NODE_FUNC_PARAM_DECL: check_param_decl(ctx,node); break;
    case NODE_FUNC_DECL:       check_function(ctx,node);   break;
    case NODE_IF:              check_if(ctx,node);         break;
    case NODE_CONST_DECL:      check_const_decl(ctx,node); break;

    case NODE_EXPR_STMT: check_expression(ctx, node->as.expr_stmt.expr); break;
    case NODE_BREAK: if(ctx->loop_depth == 0) semantic_error(ctx,node,"break outside loop"); break;

    case NODE_STRUCT_DECL: {
        declare_struct_shell(ctx, node);
        fill_struct_fields(ctx, node);
        break;
    }

    case NODE_WHILE: {

        Type *cond = check_expression(ctx, node->as.while_stmt.condition);

        if (!is_bool_type(cond))
            semantic_error(ctx, node->as.while_stmt.condition, "while condition must be a boolean expression");

        ctx->loop_depth++;
        check_node(ctx, node->as.while_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_FOR: {
        if (node->as.for_stmt.condition) {
            Type *cond =
                check_expression(ctx, node->as.for_stmt.condition);

            if (!is_bool_type(cond)) {
                semantic_error(
                    ctx,
                    node->as.for_stmt.condition,
                    "for condition must be a boolean expression");
            }
        }

        if (node->as.for_stmt.post) {
            check_expression(ctx, node->as.for_stmt.post);
        }

        ctx->loop_depth++;
        check_node(ctx, node->as.for_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_CONTINUE:
        if(ctx->loop_depth == 0) semantic_error(ctx,node,"continue outside loop");
        break;

    case NODE_RETURN: {
        if (ctx->function_depth == 0) {
            semantic_error(ctx, node, "return outside function");
            break;
        }

        Type *expected = ctx->current_return_type;
        Node *value = node->as.return_stmt.value;

        if (!expected) {
            semantic_error(ctx, node,
                "could not determine function return type");
            break;
        }

        if (!value) {
            if (expected->kind != TYPE_VOID) {
                semantic_error(ctx, node,
                    "non-void function must return a value");
            }
            break;
        }

        Type *actual = check_expression(ctx, value);

        if (expected->kind == TYPE_VOID) {
            semantic_error(ctx, node,
                "void function cannot return a value");
            break;
        }

        if (actual &&
            !initializer_compatible(expected, actual, value)) {
            semantic_error(ctx, node,
                "return type does not match function return type");
            }

        break;
    }

    default:
        break;
    }
}

static Type *make_builtin_type(SemanticContext *ctx, TypeKind kind)
{
    Type *type = arena_alloc(ctx->arena, sizeof(Type));
    memset(type, 0, sizeof(*type));

    type->kind = kind;
    type->array_size = -1;

    return type;
}

// ============================================================
// public entry
// ============================================================
void semantic_check(Node *program, SemanticContext *ctx) {
    ctx->had_error      = 0;
    ctx->loop_depth     = 0;
    ctx->function_depth = 0;
    ctx->error_count    = 0;

    ctx->current_return_type = NULL;

    ctx->type_bool = make_builtin_type(ctx, TYPE_BOOL);
    ctx->type_void = make_builtin_type(ctx, TYPE_VOID);

    ctx->current_scope = scope_new(ctx, NULL);
    check_node(ctx,  program);
}