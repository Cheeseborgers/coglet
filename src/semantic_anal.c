// src/semantic_anal.c
#include "semantic_anal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "string_decode.h"
#include "utils/utils.h"

static Type *new_type(SemanticContext *ctx, TypeKind kind)
{
    Type *type = arena_alloc(ctx->arena, sizeof(Type));
    memset(type, 0, sizeof(*type));
    type->kind = kind;
    type->array_size = -1;
    return type;
}

static void semantic_error_fmt(SemanticContext *ctx, const Node *node, const char *fmt, ...)
{
    fprintf(stderr, "semantic error at line %d: ", node->line);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);

    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error(SemanticContext *ctx, const Node *node, const char *msg) {

    fprintf(stderr, "semantic error at line %d: %s\n", node->line,msg);
    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error_name(
    SemanticContext *ctx, const Node *node, const char *prefix, const char *name, size_t length) {

    fprintf(stderr, "semantic error at line %d: %s '%.*s'\n", node->line, prefix, (int)length, name);
    ctx->had_error = 1;
    ctx->error_count++;
}

// ============================================================
// sematic expression helpers
// ============================================================
static SemExprInfo *sem_find_expr_info(SemanticContext *ctx, Node *node) {
    for (SemExprInfo *info = ctx->expr_infos; info; info = info->next) {
        if (info->node == node)
            return info;
    }

    return NULL;
}

static SemExprInfo *sem_get_or_create_expr_info(SemanticContext *ctx, Node *node) {
    SemExprInfo *info = sem_find_expr_info(ctx, node);
    if (info)
        return info;

    info = arena_alloc(ctx->arena, sizeof(*info));
    memset(info, 0, sizeof(*info));

    info->node = node;
    info->value_category = VALUE_CATEGORY_NONE;

    info->next = ctx->expr_infos;
    ctx->expr_infos = info;

    return info;
}

static void sem_record_expr_info(
    SemanticContext *ctx, Node *node, Type *type, Symbol *symbol, ValueCategory category) {

    if (!node)
        return;

    SemExprInfo *info = sem_get_or_create_expr_info(ctx, node);

    info->type = type;
    info->symbol = symbol;
    info->value_category = category;
}

// No successful semantic fact is available. The node was either not visited or did not check successfully.
static void sem_record_no_value(SemanticContext *ctx, Node *node) {
    sem_record_expr_info(ctx, node, NULL, NULL, VALUE_CATEGORY_NONE);
}

static int expression_is_lvalue(SemanticContext *ctx, Node *node) {
    SemExprInfo *info = sem_find_expr_info(ctx, node);
    return info && info->value_category == VALUE_CATEGORY_LVALUE;
}

static int require_lvalue(SemanticContext *ctx, Node *owner, Node *target, const char *message) {
    if (!expression_is_lvalue(ctx, target)) {
        semantic_error(ctx, owner, message);
        return 0;
    }

    return 1;
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
    if (!type)
        return NULL;

    switch (type->kind) {
        case TYPE_BOOL: return ctx->type_bool;
        case TYPE_VOID: return ctx->type_void;
        default: break;
    }

    if (type->kind == TYPE_NAMED) {
        Type *resolved =
            lookup_type(
                ctx,
                type->named_name.data,
                type->named_name.length
            );

        if (!resolved) {
            semantic_error_name(
                ctx,
                error_node,
                "unknown type",
                type->named_name.data,
                type->named_name.length
            );

            return NULL;
        }

        return resolved;
    }

    if (type->kind == TYPE_POINTER ||
        type->kind == TYPE_ARRAY) {

        Type *resolved_element =
            resolve_type(
                ctx,
                type->element,
                error_node
            );

        if (!resolved_element)
            return NULL;

        if (resolved_element != type->element) {
            Type *copy =
                arena_alloc(ctx->arena, sizeof(Type));

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
static int node_definitely_returns(Node *node);
static int block_definitely_returns(Node *node);
static int switch_definitely_returns(Node *node);
static int enum_switch_is_exhaustive(Node *node, Type *switch_type);

static void check_node(SemanticContext *ctx, Node *node);
static int  declare_struct_shell(SemanticContext *ctx, Node *node);
static void fill_struct_fields(SemanticContext *ctx, Node *node);
static int  declare_function_signature(SemanticContext *ctx, Node *node);
static void check_unreachable_in_block(SemanticContext *ctx, Node *block);
static void check_function_body(SemanticContext *ctx, Node *node);
static void check_const_decl(SemanticContext *ctx, Node *node);
static void check_switch_statement(SemanticContext *ctx, Node *node);
static int check_statement_expression(SemanticContext *ctx, Node *node);
static Type *check_value_expression(SemanticContext *ctx, Node *node);
static int check_assignment_statement(SemanticContext *ctx, Node *node);
static int check_compound_assignment_statement(SemanticContext *ctx, Node *node);
static int check_inc_dec_statement(SemanticContext *ctx, Node *node);
static int check_initializer_against_type(SemanticContext *ctx, Type *expected, Node *initializer);
static void format_type_name(Type *type, char *buffer, size_t buffer_size);
static int check_argument_against_parameter(SemanticContext *ctx, Type *expected, Node *argument);
static int check_array_initializer(SemanticContext *ctx, Type *expected, Node *initializer);
static int declare_enum_shell(SemanticContext *ctx, Node *node);
static void fill_enum_members(SemanticContext *ctx,Node *node);
static EnumMember *find_enum_member(Type *enum_type, const char *name, size_t length);
static Type *check_cast_expression(SemanticContext *ctx, Node *node);
static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out);
static int expression_is_compile_time_constant(SemanticContext *ctx, Node *node);
static int check_string_initializer(SemanticContext *ctx, Type *expected, Node *initializer);

// ============================================================
// expressions
// ============================================================
static int contains_void_type(Type *type)
{
    if (!type)
        return 0;

    if (type->kind == TYPE_VOID)
        return 1;

    if (type->kind == TYPE_POINTER ||
        type->kind == TYPE_ARRAY) {
        return contains_void_type(type->element);
        }

    if (type->kind == TYPE_FUNCTION) {
        /*
         * Function return type may be void, but parameter types may not.
         */
        for (int i = 0; i < type->parameter_count; i++) {
            if (contains_void_type(type->parameters[i]))
                return 1;
        }

        return 0;
    }

    return 0;
}

static void format_type_name(Type *type, char *buffer, size_t buffer_size) {

    if (!buffer || buffer_size == 0)
        return;

    if (!type) {
        snprintf(buffer, buffer_size, "<unknown>");
        return;
    }

    switch (type->kind) {
        case TYPE_VOID:
            snprintf(buffer, buffer_size, "void");
            return;

        case TYPE_BOOL:
            snprintf(buffer, buffer_size, "bool");
            return;

        case TYPE_I8:
            snprintf(buffer, buffer_size, "i8");
            return;

        case TYPE_I16:
            snprintf(buffer, buffer_size, "i16");
            return;

        case TYPE_I32:
            snprintf(buffer, buffer_size, "i32");
            return;

        case TYPE_I64:
            snprintf(buffer, buffer_size, "i64");
            return;

        case TYPE_U8:
            snprintf(buffer, buffer_size, "u8");
            return;

        case TYPE_U16:
            snprintf(buffer, buffer_size, "u16");
            return;

        case TYPE_U32:
            snprintf(buffer, buffer_size, "u32");
            return;

        case TYPE_U64:
            snprintf(buffer, buffer_size, "u64");
            return;

        case TYPE_F32:
            snprintf(buffer, buffer_size, "f32");
            return;

        case TYPE_F64:
            snprintf(buffer, buffer_size, "f64");
            return;

        case TYPE_NAMED:
            snprintf(
                buffer,
                buffer_size,
                "%.*s",
                (int)type->named_name.length,
                type->named_name.data
            );
            return;

        case TYPE_STRUCT:
            snprintf(
                buffer,
                buffer_size,
                "%.*s",
                (int)type->struct_name.length,
                type->struct_name.data
            );
            return;

        case TYPE_ENUM:
            snprintf(
                buffer,
                buffer_size,
                "%.*s",
                (int)type->enum_name.length,
                type->enum_name.data
            );
            return;

        case TYPE_POINTER: {
            char element[128];
            format_type_name(type->element, element, sizeof(element));
            snprintf(buffer, buffer_size, "*%s", element);
            return;
        }

        case TYPE_ARRAY: {
            char element[128];
            format_type_name(type->element, element, sizeof(element));

            if (type->array_size >= 0) {
                snprintf(
                    buffer,
                    buffer_size,
                    "[%d]%s",
                    type->array_size,
                    element
                );
            } else {
                snprintf(buffer, buffer_size, "[]%s", element);
            }

            return;
        }

        case TYPE_FUNCTION:
            snprintf(buffer, buffer_size, "function");
            return;
    }

    snprintf(buffer, buffer_size, "<unknown>");
}

static int type_equal(Type *a, Type *b) {

    if (a == NULL || b == NULL)
        return 0;

    if (a->kind != b->kind)
        return 0;

    if (a->kind == TYPE_POINTER)
        return type_equal(a->element, b->element);

    if (a->kind == TYPE_ARRAY)
        return a->array_size == b->array_size &&
               type_equal(a->element, b->element);

    if (a->kind == TYPE_NAMED) {
        return names_equal(
            a->named_name.data,
            a->named_name.length,
            b->named_name.data,
            b->named_name.length);
    }

    if (a->kind == TYPE_STRUCT)
        return names_equal(
            a->struct_name.data,
            a->struct_name.length,
            b->struct_name.data,
            b->struct_name.length);

    if (a->kind == TYPE_ENUM) {
        return names_equal(
            a->enum_name.data,
            a->enum_name.length,
            b->enum_name.data,
            b->enum_name.length);
    }

    if (a->kind == TYPE_FUNCTION) {
        if (a->parameter_count != b->parameter_count)
            return 0;

        for (int i = 0; i < a->parameter_count; i++) {
            if (!type_equal(a->parameters[i], b->parameters[i]))
                return 0;
        }

        return type_equal(a->return_type, b->return_type);
    }

    return 1;
}

static int invalid_value_type(Type *type)
{
    return contains_void_type(type);
}

static int invalid_return_type(Type *type)
{
    if (!type)
        return 0;

    if (type->kind == TYPE_VOID)
        return 0;

    return contains_void_type(type);
}

static int is_integer_kind(TypeKind k) {

    switch (k) {
        case TYPE_I8: case TYPE_I16: case TYPE_I32: case TYPE_I64:
        case TYPE_U8: case TYPE_U16: case TYPE_U32: case TYPE_U64:
            return 1;
        default:
            return 0;
    }
}

static int integer_value_fits_type(long long value, TypeKind kind)
{
    switch (kind) {
        case TYPE_I8:
            return value >= -128LL &&
                   value <= 127LL;

        case TYPE_I16:
            return value >= -32768LL &&
                   value <= 32767LL;

        case TYPE_I32:
            return value >= (-2147483647LL - 1LL) &&
                   value <= 2147483647LL;

        case TYPE_I64:
            /*
             * value is already stored as long long, so every value
             * represented here fits in i64.
             */
            return 1;

        case TYPE_U8:
            return value >= 0 &&
                   value <= 255LL;

        case TYPE_U16:
            return value >= 0 &&
                   value <= 65535LL;

        case TYPE_U32:
            return value >= 0 &&
                   (unsigned long long)value <= 4294967295ULL;

        case TYPE_U64:
            /*
             * ConstValue currently stores integers as signed long long.
             * Therefore every non-negative value it can represent fits
             * within u64, though values above LLONG_MAX cannot yet be
             * represented by the constant evaluator.
             */
            return value >= 0;

        default:
            return 0;
    }
}

static int is_u8_type(Type *type)    { return type && type->kind == TYPE_U8; }
static int is_integer_type(Type *t)  { return t && is_integer_kind(t->kind); }
static int is_float_kind(TypeKind k) { return k == TYPE_F32 || k == TYPE_F64; }
static int is_numeric_type(Type *t)  { return t && (is_integer_kind(t->kind) || is_float_kind(t->kind)); }
static int is_bool_type(Type *t)     { return t && t->kind == TYPE_BOOL; }

static int node_is_literal_true(Node *node) {
    return node && node->type == NODE_BOOL && node->as.boolean.value;
}

static int is_int_literal_zero(Node *node) {
    return node && node->type == NODE_NUMBER && !node->as.number.is_float && node->as.number.value == 0;
}

static int is_switchable_type(Type *type)
{
    if (!type)
        return 0;

    return is_integer_kind(type->kind) ||
           type->kind == TYPE_BOOL ||
           type->kind == TYPE_ENUM;
}

static int is_enum_type(Type *type)
{
    return type && type->kind == TYPE_ENUM;
}

static int is_bool_cast_pair(Type *to, Type *from)
{
    return is_bool_type(to) && is_bool_type(from);
}

static int is_numeric_cast_pair(Type *to, Type *from)
{
    return is_numeric_type(to) && is_numeric_type(from);
}

static int is_enum_to_integer_cast(Type *to, Type *from)
{
    return is_integer_kind(to->kind) && is_enum_type(from);
}

static int is_integer_to_enum_cast(Type *to, Type *from)
{
    return is_enum_type(to) && is_integer_kind(from->kind);
}

static int is_allowed_explicit_cast(Type *to, Type *from)
{
    if (!to || !from)
        return 0;

    /*
     * Casting a type to itself is always allowed.
     */
    if (type_equal(to, from))
        return 1;

    /*
     * bool is deliberately not part of numeric casts.
     * For now, only bool -> bool is allowed.
     */
    if (is_bool_cast_pair(to, from))
        return 1;

    /*
     * Numeric casts:
     *     i32 -> i64
     *     i64 -> u8
     *     f64 -> i32
     *     i32 -> f64
     */
    if (is_numeric_cast_pair(to, from))
        return 1;

    /*
     * Enum backing-value casts:
     *     raw: u16 = cast(u16, Color.Green);
     *     c: Color = cast(Color, 0);
     */
    if (is_enum_to_integer_cast(to, from))
        return 1;

    if (is_integer_to_enum_cast(to, from))
        return 1;

    return 0;
}

static int const_values_equal(ConstValue *a, ConstValue *b)
{
    if (!a || !b)
        return 0;

    if (a->kind != b->kind)
        return 0;

    switch (a->kind) {
        case CONST_VALUE_INT:
            return a->as.i == b->as.i;

        case CONST_VALUE_FLOAT:
            return a->as.f == b->as.f;

        case CONST_VALUE_BOOL:
            return a->as.b == b->as.b;
    }

    return 0;
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

static Type *const_value_default_type(SemanticContext *ctx,const ConstValue *v) {
    /*
     * If constant evaluation already knows the semantic type, trust it.
     *
     * Examples:
     *     Color.Red       -> Color
     *     DEFAULT_COLOR   -> Color
     *     true            -> bool
     */
    if (v->type)
        return v->type;

    /*
     * Fallback for old/simple constants that only have a raw value.
     * Numeric literals remain untyped so they can adapt to declared types.
     */
    Type *t = new_type(ctx, TYPE_I32);
    t->is_untyped = 1;

    switch (v->kind) {
        case CONST_VALUE_INT:
            t->kind = TYPE_I32;
            break;

        case CONST_VALUE_FLOAT:
            t->kind = TYPE_F64;
            break;

        case CONST_VALUE_BOOL:
            t->kind = TYPE_BOOL;
            t->is_untyped = 0;
            break;
    }

    return t;
}

// Recursively evaluates an expression that must be knowable at compile
// time: literals, other constants, and unary/binary ops over those.
// Anything reaching outside that (function calls, variables, struct
// inits, etc.) is rejected with a diagnostic.
static int eval_const_expr(SemanticContext *ctx, Node *node, ConstValue *out) {

    if (!node)
        return 0;

    /*
     * Avoid accidentally carrying stale data when callers reuse a ConstValue.
     */
    memset(out, 0, sizeof(*out));

    switch (node->type) {
        case NODE_NUMBER:
        {
            if (node->as.number.is_float) {
                out->kind = CONST_VALUE_FLOAT;
                out->as.f = node->as.number.value;

                out->type = new_type(ctx, TYPE_F64);
                out->type->is_untyped = 1;
            } else {
                out->kind = CONST_VALUE_INT;
                out->as.i = (long long)node->as.number.value;

                out->type = new_type(ctx, TYPE_I32);
                out->type->is_untyped = 1;
            }

            return 1;
        }

        case NODE_BOOL:
        {
            out->kind = CONST_VALUE_BOOL;
            out->as.b = node->as.boolean.value;
            out->type = ctx->type_bool;

            return 1;
        }

        case NODE_CAST:
            return eval_const_cast(ctx, node, out);
        case NODE_IDENT:
        {
            Symbol *sym =
                scope_lookup(
                    ctx->current_scope,
                    node->as.ident.data,
                    node->as.ident.length
                );

            if (!sym || sym->kind != SYMBOL_CONSTANT) {
                semantic_error(ctx, node,
                    "expression is not a compile-time constant"
                );

                return 0;
            }

            /*
             * Constants store their typed compile-time value.
             * This preserves enum constant types.
             */
            *out = sym->const_value;
            return 1;
        }

        case NODE_FIELD:
        {
            /*
             * Enum members are compile-time constants:
             *
             *     Color.Red
             *
             * The raw stored value is the enum member's integer value,
             * but the semantic type is the enum type itself.
             */
            if (node->as.field.object &&
                node->as.field.object->type == NODE_IDENT) {

                Node *object_node =
                    node->as.field.object;

                Symbol *sym =
                    scope_lookup(
                        ctx->current_scope,
                        object_node->as.ident.data,
                        object_node->as.ident.length
                    );

                if (sym &&
                    sym->kind == SYMBOL_TYPE &&
                    sym->type &&
                    sym->type->kind == TYPE_ENUM) {

                    EnumMember *member =
                        find_enum_member(
                            sym->type,
                            node->as.field.name.data,
                            node->as.field.name.length
                        );

                    if (!member) {
                        semantic_error_name(
                            ctx,
                            node,
                            "unknown enum member",
                            node->as.field.name.data,
                            node->as.field.name.length
                        );

                        return 0;
                    }

                    out->kind = CONST_VALUE_INT;
                    out->as.i = member->value;
                    out->type = sym->type;

                    return 1;
                }
            }

            semantic_error(ctx, node,
                "expression is not a compile-time constant"
            );

            return 0;
        }

        case NODE_UNARY:
        {
            ConstValue operand;

            if (!eval_const_expr(
                    ctx,
                    node->as.unary.operand,
                    &operand)) {
                return 0;
            }

            if (node->as.unary.op == TOK_MINUS) {
                if (operand.kind == CONST_VALUE_INT) {
                    out->kind = CONST_VALUE_INT;
                    out->as.i = -operand.as.i;
                    out->type = operand.type;
                    return 1;
                }

                if (operand.kind == CONST_VALUE_FLOAT) {
                    out->kind = CONST_VALUE_FLOAT;
                    out->as.f = -operand.as.f;
                    out->type = operand.type;
                    return 1;
                }

                semantic_error(ctx, node,
                    "unary '-' requires a numeric constant");

                return 0;
            }

            if (node->as.unary.op == TOK_BANG) {
                if (operand.kind != CONST_VALUE_BOOL) {
                    semantic_error(ctx, node,
                        "unary '!' requires a boolean constant");

                    return 0;
                }

                out->kind = CONST_VALUE_BOOL;
                out->as.b = !operand.as.b;
                out->type = ctx->type_bool;

                return 1;
            }

            semantic_error(ctx, node,
                "operator not allowed in a constant expression");

            return 0;
        }

        case NODE_BINARY:
        {
            ConstValue left;
            ConstValue right;

            if (!eval_const_expr(ctx, node->as.binary.left, &left))
                return 0;

            if (!eval_const_expr(ctx, node->as.binary.right, &right))
                return 0;

            switch (node->as.binary.op) {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                {
                    Type *left_type = const_value_default_type(ctx, &left);
                    Type *right_type = const_value_default_type(ctx, &right);

                    if (!is_numeric_type(left_type) ||
                        !is_numeric_type(right_type)) {

                        semantic_error(ctx, node,
                            "operands must be numeric constants");

                        return 0;
                    }

                    if (node->as.binary.op == TOK_PERCENT) {
                        if (!is_integer_kind(left_type->kind) ||
                            !is_integer_kind(right_type->kind)) {

                            semantic_error(ctx, node,
                                "'%' requires integer constants");

                            return 0;
                        }
                    }

                    Type *result_type =
                        common_numeric_type(left_type, right_type);

                    if (!result_type) {
                        semantic_error(ctx, node,
                            "constant operands have incompatible numeric types");

                        return 0;
                    }

                    /*
                     * Integer result path.
                     */
                    if (left.kind == CONST_VALUE_INT &&
                        right.kind == CONST_VALUE_INT &&
                        is_integer_kind(result_type->kind)) {

                        long long a = left.as.i;
                        long long b = right.as.i;

                        out->kind = CONST_VALUE_INT;
                        out->type = result_type;

                        switch (node->as.binary.op) {
                            case TOK_PLUS:
                                out->as.i = a + b;
                                return 1;

                            case TOK_MINUS:
                                out->as.i = a - b;
                                return 1;

                            case TOK_STAR:
                                out->as.i = a * b;
                                return 1;

                            case TOK_SLASH:
                                if (b == 0) {
                                    semantic_error(ctx, node,
                                        "division by zero in constant expression");

                                    return 0;
                                }

                                out->as.i = a / b;
                                return 1;

                            case TOK_PERCENT:
                                if (b == 0) {
                                    semantic_error(ctx, node,
                                        "division by zero in constant expression");

                                    return 0;
                                }

                                out->as.i = a % b;
                                return 1;

                            default:
                                return 0;
                        }
                    }

                    /*
                     * Floating result path.
                     */
                    if (node->as.binary.op == TOK_PERCENT) {
                        semantic_error(ctx, node,
                            "'%' requires integer constants");

                        return 0;
                    }

                    double a, b;

                    if (!const_value_to_double(&left, &a) ||
                        !const_value_to_double(&right, &b)) {

                        semantic_error(ctx, node,
                            "operands must be numeric constants");

                        return 0;
                    }

                    out->kind = CONST_VALUE_FLOAT;
                    out->type = result_type;

                    switch (node->as.binary.op) {
                        case TOK_PLUS:
                            out->as.f = a + b;
                            return 1;

                        case TOK_MINUS:
                            out->as.f = a - b;
                            return 1;

                        case TOK_STAR:
                            out->as.f = a * b;
                            return 1;

                        case TOK_SLASH:
                            if (b == 0.0) {
                                semantic_error(ctx, node,
                                    "division by zero in constant expression");

                                return 0;
                            }

                            out->as.f = a / b;
                            return 1;

                        default:
                            return 0;
                    }
                }

                case TOK_AND_AND:
                case TOK_OR_OR:
                {
                    if (left.kind != CONST_VALUE_BOOL ||
                        right.kind != CONST_VALUE_BOOL) {

                        semantic_error(ctx, node,
                            "operands must be boolean constants");

                        return 0;
                    }

                    out->kind = CONST_VALUE_BOOL;
                    out->as.b = (node->as.binary.op == TOK_AND_AND)
                        ? (left.as.b && right.as.b)
                        : (left.as.b || right.as.b);

                    out->type = ctx->type_bool;

                    return 1;
                }

                default:
                    semantic_error(ctx, node,
                        "operator not allowed in a constant expression");

                    return 0;
            }
        }

        default:
            semantic_error(ctx, node,
                "expression is not a compile-time constant");

            return 0;
    }
}

static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out) {

    ConstValue value;

    if (!eval_const_expr(
            ctx,
            node->as.cast_expr.expression,
            &value)) {
        return 0;
    }

    Type *target_type = resolve_type(
            ctx,
            node->as.cast_expr.target_type,
            node
        );

    if (!target_type)
        return 0;

    Type *source_type =
        const_value_default_type(ctx, &value);

    if (!is_allowed_explicit_cast(target_type, source_type)) {

        semantic_error(ctx, node, "invalid explicit cast");

        return 0;
    }

    /*
     * Store the constant value using the destination type.
     *
     * This is intentionally simple. Later you can add exact range checks
     * for narrowing integer casts.
     */
    memset(out, 0, sizeof(*out));
    out->type = target_type;

    if (target_type->kind == TYPE_ENUM) {
        /*
         * integer -> enum
         */
        if (value.kind != CONST_VALUE_INT) {
            semantic_error(ctx, node,
                "enum cast requires integer constant");

            return 0;
        }

        if (!target_type->enum_backing_type ||
            !integer_value_fits_type(
                value.as.i,
                target_type->enum_backing_type->kind)) {

            semantic_error(ctx, node,
                "enum cast value does not fit in backing type");

            return 0;
        }

        out->kind = CONST_VALUE_INT;
        out->as.i = value.as.i;
        return 1;
    }

    if (is_integer_kind(target_type->kind)) {
        long long integer_value = 0;

        if (value.kind == CONST_VALUE_INT) {
            integer_value = value.as.i;
        } else if (value.kind == CONST_VALUE_FLOAT) {
            integer_value = (long long)value.as.f;
        } else {
            semantic_error(ctx, node,
                "integer cast requires numeric constant");

            return 0;
        }

        if (!integer_value_fits_type(
                integer_value,
                target_type->kind)) {
            semantic_error(ctx, node,
                "integer cast value does not fit in target type");

            return 0;
        }

        out->kind = CONST_VALUE_INT;
        out->as.i = integer_value;
        return 1;
    }

    if (is_float_kind(target_type->kind)) {
        out->kind = CONST_VALUE_FLOAT;

        if (value.kind == CONST_VALUE_INT) {
            out->as.f = (double)value.as.i;
            return 1;
        }

        if (value.kind == CONST_VALUE_FLOAT) {
            out->as.f = value.as.f;
            return 1;
        }

        semantic_error(ctx, node,
            "float cast requires numeric constant");

        return 0;
    }

    if (target_type->kind == TYPE_BOOL) {
        if (value.kind != CONST_VALUE_BOOL) {
            semantic_error(ctx, node,
                "bool cast requires boolean constant");

            return 0;
        }

        out->kind = CONST_VALUE_BOOL;
        out->as.b = value.as.b;
        return 1;
    }

    semantic_error(ctx, node, "invalid constant cast");

    return 0;
}

static int expression_is_compile_time_constant(SemanticContext *ctx, Node *node) {
    if (!node)
        return 0;

    switch (node->type) {
        case NODE_NUMBER:
        case NODE_BOOL:
            return 1;

        case NODE_CAST:
            return expression_is_compile_time_constant(
                ctx,
                node->as.cast_expr.expression
            );

        case NODE_UNARY:
            return expression_is_compile_time_constant(
                ctx,
                node->as.unary.operand
            );

        case NODE_BINARY:
            return expression_is_compile_time_constant(
                       ctx,
                       node->as.binary.left
                   ) &&
                   expression_is_compile_time_constant(
                       ctx,
                       node->as.binary.right
                   );

        case NODE_IDENT:
        {
            Symbol *sym =
                scope_lookup(
                    ctx->current_scope,
                    node->as.ident.data,
                    node->as.ident.length
                );

            return sym && sym->kind == SYMBOL_CONSTANT;
        }

        case NODE_FIELD:
        {
            /*
             * Only enum members are compile-time constant fields for now:
             *
             *     SomeEnum.Member
             */
            if (!node->as.field.object ||
                node->as.field.object->type != NODE_IDENT) {
                return 0;
            }

            Node *object_node =
                node->as.field.object;

            Symbol *sym =
                scope_lookup(
                    ctx->current_scope,
                    object_node->as.ident.data,
                    object_node->as.ident.length
                );

            return sym &&
                   sym->kind == SYMBOL_TYPE &&
                   sym->type &&
                   sym->type->kind == TYPE_ENUM &&
                   find_enum_member(
                       sym->type,
                       node->as.field.name.data,
                       node->as.field.name.length
                   );
        }

        default:
            return 0;
    }
}

static int check_string_initializer(SemanticContext *ctx,Type *expected,Node *initializer) {

    if (!expected || !initializer)
        return 0;

    if (initializer->type != NODE_STRING) {
        semantic_error(ctx, initializer, "internal error: expected string literal");
        return 0;
    }

    if (expected->kind != TYPE_ARRAY) {
        semantic_error(ctx, initializer, "string literal can only initialize a byte array");
        return 0;
    }

    if (!is_u8_type(expected->element)) {
        semantic_error(ctx, initializer, "string literal destination must be u8 array");
        return 0;
    }

    StringDecodeInfo info = string_analyze(initializer->as.string_literal);

    if (!info.ok) {
        if (info.invalid_escape) {
            semantic_error_fmt(
                ctx, initializer,
                "invalid escape sequence '\\%c' in string literal",
                info.invalid_escape);
        } else {
            semantic_error(ctx, initializer,
                "unterminated escape sequence in string literal");
        }

        return 0;
    }

    /*
     * Stage 1 rule:
     *
     * "hello" initializes:
     *
     *     h e l l o \0
     *
     * so it requires u8[6].
     */
    int required_size = info.decoded_length + 1;

    if (expected->array_size != required_size) {
        semantic_error_fmt(
            ctx,
            initializer,
            "string literal requires destination array size %d, got %d",
            required_size,
            expected->array_size
        );
        return 0;
    }

    return 1;
}

static Type *check_expression(SemanticContext *ctx, Node *node) {

    if (!node) return NULL;

    switch(node->type)
    {
        case NODE_IDENT:
        {
            Symbol *sym =
                scope_lookup(
                    ctx->current_scope,
                    node->as.ident.data,
                    node->as.ident.length
                );

            if (!sym) {
                semantic_error_name(
                    ctx, node,
                    "undefined identifier",
                    node->as.ident.data,
                    node->as.ident.length
                );

                return NULL;
            }

            /*
             * Type names are not values.
             *
             * Valid:
             *     Color.Red
             *
             * Invalid:
             *     x: Color = Color;
             */
            if (sym->kind == SYMBOL_TYPE) {
                semantic_error_name(
                    ctx, node,
                    "type name cannot be used as a value",
                    node->as.ident.data,
                    node->as.ident.length
                );

                return NULL;
            }

            ValueCategory category = VALUE_CATEGORY_RVALUE;

            if (sym->kind == SYMBOL_VARIABLE)
                category = VALUE_CATEGORY_LVALUE;

            sem_record_expr_info(
                ctx, node,
                sym->type, sym,
                category);

            return sym->type;
        }

        case NODE_UNARY:
        {
            Type *operand = check_value_expression(ctx, node->as.unary.operand);

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

                    sem_record_expr_info(ctx, node, operand, NULL, VALUE_CATEGORY_RVALUE);
                    return operand;
                }

                case TOK_BANG:
                {
                    if(!is_bool_type(operand)) {
                        semantic_error(ctx,node,
                            "unary '!' requires boolean operand");

                        return NULL;
                    }

                    sem_record_expr_info(ctx, node, ctx->type_bool, NULL, VALUE_CATEGORY_RVALUE);
                    return ctx->type_bool;
                }

                default:
                    UNREACHABLE("node->as.unary.op");
            }
        }

        case NODE_BINARY:
        {
            Type *left = check_value_expression(ctx, node->as.binary.left);
            if (!left) return NULL;

            Type *right = check_value_expression(ctx, node->as.binary.right);
            if (!right) return NULL;

            switch(node->as.binary.op)
            {
                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                {
                    if (!is_numeric_type(left)) {
                        semantic_error(ctx, node,
                            "left operand must be numeric");

                        return NULL;
                    }

                    if (!is_numeric_type(right)) {
                        semantic_error(ctx, node,
                            "right operand must be numeric");

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

                            semantic_error(ctx, node,
                                "'%' requires integer operands");

                            return NULL;
                        }
                    }

                    Type *result = common_numeric_type(left, right);

                    if (!result) {
                        if (!left->is_untyped &&
                            !right->is_untyped &&
                            !type_equal(left, right)) {
                                semantic_error(ctx, node,
                                    "operands are different numeric types -- use an explicit cast");
                            } else {
                                semantic_error(ctx, node,
                                    "could not determine numeric result type");
                            }

                        return NULL;
                    }

                    sem_record_expr_info(ctx, node, result, NULL, VALUE_CATEGORY_RVALUE);
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

                    sem_record_expr_info(ctx, node, ctx->type_bool, NULL, VALUE_CATEGORY_RVALUE);
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
                            semantic_error(ctx, node,
                                "comparison operands have incompatible numeric types");

                            return NULL;
                        }

                        sem_record_expr_info(ctx, node, ctx->type_bool, NULL, VALUE_CATEGORY_RVALUE);
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

                        semantic_error(ctx, node, "comparison type mismatch");
                        return NULL;
                    }

                    sem_record_expr_info(ctx, node, ctx->type_bool, NULL, VALUE_CATEGORY_RVALUE);
                    return ctx->type_bool;
                }

                // ordered comparisons require numbers
                case TOK_LESS:
                case TOK_GREATER:
                case TOK_LESS_EQUAL:
                case TOK_GREATER_EQUAL:
                {
                    if (!is_numeric_type(left) || !is_numeric_type(right)) {

                        semantic_error(ctx, node,
                            "ordered comparison requires numeric operands");

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
                        semantic_error(ctx, node,
                            "comparison operands have incompatible numeric types");

                        return NULL;
                    }

                    sem_record_expr_info(ctx, node, ctx->type_bool, NULL, VALUE_CATEGORY_RVALUE);
                    return ctx->type_bool;
                }


                default:
                    return NULL;
            }
        }

        case NODE_INC_DEC:
            semantic_error(ctx, node,
                "increment/decrement cannot be used as a value; it is a statement");
            return NULL;

        case NODE_COMPOUND_ASSIGN:
            semantic_error(ctx, node,
                "compound assignment cannot be used as a value; it is a statement");
            return NULL;

        case NODE_ASSIGN:
            semantic_error(ctx, node,
                "assignment cannot be used as a value; it is a statement");
            return NULL;

        case NODE_CALL: {
            Type *callee = check_value_expression(ctx, node->as.call.callee);
            if (!callee)
                return NULL;

            if (callee->kind != TYPE_FUNCTION) {
                semantic_error(ctx, node, "called object is not a function");
                return NULL;
            }

            int argc = node->as.call.arguments.count;

            if (argc != callee->parameter_count) {
                semantic_error_fmt(
                    ctx, node,
                    "wrong number of arguments: expected %d, got %d",
                    callee->parameter_count,
                    argc
                );
                return NULL;
            }

            int ok = 1;

            for (int i = 0; i < argc; i++) {
                Node *arg        = node->as.call.arguments.items[i];
                Type *param_type = callee->parameters[i];

                if (!check_argument_against_parameter(ctx, param_type, arg))
                    ok = 0;
            }

            if (!ok)
                return NULL;

            ValueCategory category =
                callee->return_type->kind == TYPE_VOID
                ? VALUE_CATEGORY_NONE
                : VALUE_CATEGORY_RVALUE;

            sem_record_expr_info(
                ctx,
                node,
                callee->return_type,
                NULL,
                category
            );

            return callee->return_type;
        }

        case NODE_CAST:
        {
            Type *type = check_cast_expression(ctx, node);

            if (!type)
                return NULL;

            sem_record_expr_info(
                ctx,
                node,
                type,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return type;
        }

        case NODE_FIELD:
        {
            /*
             * Special case:
             *
             *     Color.Red
             *
             * The object side is an identifier naming a type, not a runtime value.
             */
            if (node->as.field.object &&
                node->as.field.object->type == NODE_IDENT)
            {

                Node *object_node = node->as.field.object;

                Symbol *sym =
                    scope_lookup(
                        ctx->current_scope,
                        object_node->as.ident.data,
                        object_node->as.ident.length);

                if (sym &&
                    sym->kind == SYMBOL_TYPE &&
                    sym->type &&
                    sym->type->kind == TYPE_ENUM)
                {

                    EnumMember *member =
                        find_enum_member(
                            sym->type,
                            node->as.field.name.data,
                            node->as.field.name.length);

                    if (!member) {
                        semantic_error_name(
                            ctx, node,
                            "unknown enum member",
                            node->as.field.name.data,
                            node->as.field.name.length
                        );

                        return NULL;
                }
                    sem_record_expr_info(
                        ctx,
                        node,
                        sym->type,
                        sym,
                        VALUE_CATEGORY_RVALUE);

                    return sym->type;
                }
            }

            /*
             * Normal runtime field access:
             *
             *     point.x
             */
            Type *object = check_value_expression(ctx, node->as.field.object);

            if (!object)
                return NULL;

            if (object->kind != TYPE_STRUCT) {
                semantic_error(
                    ctx,
                    node,
                    "field access requires a struct"
                );

                return NULL;
            }

            Type *field =
                find_struct_field(
                    object,
                    node->as.field.name.data,
                    node->as.field.name.length
                );

            if (!field) {
                semantic_error(ctx, node,
                    "unknown struct field"
                );

                return NULL;
            }

            SemExprInfo *object_info =
                sem_find_expr_info(ctx, node->as.field.object);

            ValueCategory category = VALUE_CATEGORY_RVALUE;

            if (object_info &&
                object_info->value_category == VALUE_CATEGORY_LVALUE) {
                category = VALUE_CATEGORY_LVALUE;
            }

            sem_record_expr_info(
                ctx,
                node,
                field,
                NULL,
                category
            );

            return field;
        }

        case NODE_INDEX:
        {
            Type *object = check_value_expression(ctx, node->as.index.object);

            if (!object)
                return NULL;

            Type *index = check_value_expression(ctx, node->as.index.index);

            if (!index)
                return NULL;

            if (!is_integer_kind(index->kind)) {

                semantic_error(ctx, node,
                    "array index must be integer");

                return NULL;
            }

            if (object->kind != TYPE_ARRAY && object->kind != TYPE_POINTER) {

                semantic_error(ctx, node,
                    "object is not indexable");

                return NULL;
            }

            /*
             * Compile-time bounds check for fixed-size arrays.
             *
             * Runtime indexes are allowed:
             *
             *     arr[i]
             *
             * Constant indexes are checked:
             *
             *     arr[3]
             */
            if (object->kind == TYPE_ARRAY &&
                object->array_size >= 0 &&
                expression_is_compile_time_constant(
                    ctx,
                    node->as.index.index)) {

                ConstValue index_value;

                if (eval_const_expr(
                        ctx,
                        node->as.index.index,
                        &index_value) &&
                    index_value.kind == CONST_VALUE_INT) {

                    if (index_value.as.i < 0 ||
                        index_value.as.i >= object->array_size) {

                        semantic_error(ctx, node,
                            "array index out of bounds");
                        }
                }
            }

            Type *element_type = object->element;

            SemExprInfo *object_info =
                sem_find_expr_info(ctx, node->as.index.object);

            ValueCategory category = VALUE_CATEGORY_RVALUE;

            if (object_info &&
                object_info->value_category == VALUE_CATEGORY_LVALUE) {
                category = VALUE_CATEGORY_LVALUE;
            }

            sem_record_expr_info(
                ctx,
                node,
                element_type,
                NULL,
                category
            );

            return element_type;
        }

        case NODE_NUMBER:
        {
            Type *type =
                new_type(ctx, node->as.number.is_float ? TYPE_F64 : TYPE_I32);

            type->is_untyped = 1;

            sem_record_expr_info(
                ctx,
                node,
                type,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return type;
        }

        case NODE_STRING:
            semantic_error(ctx, node,
                "string literal requires an expected byte array type");

            return NULL;

        case NODE_CHAR:
        {
            Type *t = new_type(ctx, TYPE_U8);

            sem_record_expr_info(
                ctx,
                node,
                t,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return t;
        }

        case NODE_BOOL:
        {
            sem_record_expr_info(
                ctx,
                node,
                ctx->type_bool,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

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
                            ctx, field_init,
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

                if (!check_initializer_against_type(ctx, field_type, value_node)) {
                    /*
                     * check_initializer_against_type already emitted the precise error.
                     * Avoid adding a second generic field initializer error here.
                     */
                    continue;
                }

            }

            /*
             * Pass 2:
             * Ensure every declared field in the struct has an initializer.
             */
            for (int field_index = 0; field_index < type->field_count; field_index++) {

                StructField *required_field = &type->fields[field_index];

                int found = 0;

                for (int init_index = 0;
                     init_index < inits->count;
                     init_index++) {

                    Node *field_init = inits->items[init_index];

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

            sem_record_expr_info(
                ctx,
                node,
                type,
                NULL,
                VALUE_CATEGORY_RVALUE);

            return type;
        }

        case NODE_ARRAY_LITERAL:
            semantic_error(ctx, node,
                "array literal requires an expected array type");
            return NULL;

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

    for(int i=0; i < node->as.block.statements.count; i++)
        check_node(ctx, node->as.block.statements.items[i]);

    check_unreachable_in_block(ctx, node);

    scope_pop(ctx);
}

static void check_const_decl(SemanticContext *ctx, Node *node) {
    if (scope_find_local(
            ctx->current_scope,
            node->as.const_decl.name.data,
            node->as.const_decl.name.length)) {

        semantic_error_name(
            ctx, node,
            "duplicate declaration",
            node->as.const_decl.name.data,
            node->as.const_decl.name.length);

        return;
    }

    ConstValue value;

    if (!eval_const_expr(ctx, node->as.const_decl.value, &value))
        return;

    Type *value_type = check_value_expression(ctx,node->as.const_decl.value);

    if (!value_type)
        return;

    Type *type = node->as.const_decl.const_type;

    if (type) {
        /*
         * Explicit typed constant:
         *
         *     X: i64 : 10;
         *     DEFAULT_COLOR: Color : Color.Red;
         */
        type = resolve_type(ctx, type, node);

        if (!type) {
            semantic_error(ctx, node,
                "could not resolve constant type");

            return;
        }

        if (invalid_value_type(type)) {
            semantic_error(ctx, node,
                "constant cannot have type void");

            return;
        }

        /*
         * Allow integer constants to be stored as float constants when the
         * declared destination is a float.
         *
         * Do not do this for enums. Enums are strongly typed and should
         * only accept enum values of the same enum type.
         */
        if (is_float_kind(type->kind) &&
            value.kind == CONST_VALUE_INT &&
            is_integer_kind(value_type->kind)) {

            long long integer_value = value.as.i;

            value.kind = CONST_VALUE_FLOAT;
            value.as.f = (double)integer_value;
            value.type = type;
            value_type = type;
        }

        if (!initializer_compatible(
                type,
                value_type,
                node->as.const_decl.value)) {

            semantic_error(ctx, node,
                "constant value does not match declared type");

            return;
        }

        /*
         * Store the declared type as the constant's final type.
         *
         * Example:
         *     X: i64 : 10;
         *
         * The literal 10 starts as untyped i32, but the constant X is i64.
         */
        value.type = type;
    } else {
        /*
         * Inferred constant:
         *
         *     X :: 10;
         *     DEFAULT_COLOR :: Color.Red;
         *
         * This now correctly preserves enum types because ConstValue
         * carries a Type*.
         */
        type = value.type
            ? value.type
            : const_value_default_type(ctx, &value);

        if (!type) {
            semantic_error(ctx, node,
                "could not infer constant type");

            return;
        }

        value.type = type;
    }

    Symbol *sym = arena_alloc(ctx->arena, sizeof(Symbol));
    memset(sym, 0, sizeof(*sym));

    sym->name = node->as.const_decl.name;
    sym->kind = SYMBOL_CONSTANT;
    sym->type = type;
    sym->const_value = value;

    sym->next = ctx->current_scope->symbols;
    ctx->current_scope->symbols = sym;
}

static void check_switch_statement(SemanticContext *ctx,Node *node) {

    Type *switch_type = check_value_expression(ctx, node->as.switch_stmt.expression);

    if (!switch_type)
        return;

    node->as.switch_stmt.resolved_type = switch_type;

    if (!is_switchable_type(switch_type)) {
        semantic_error(ctx, node,
            "switch expression must be integer, bool, or enum");

        /*
         * Still check case bodies for useful follow-up diagnostics.
         */
    }

    int seen_default = 0;

    typedef struct SeenCase {
        ConstValue value;
        Node *node;
    } SeenCase;

    int case_count = node->as.switch_stmt.cases.count;

    SeenCase *seen_cases = case_count
        ? arena_alloc(
            ctx->arena,
            sizeof(SeenCase) * case_count
        )
        : NULL;

    int seen_case_count = 0;

    for (int i = 0; i < case_count; i++) {
        Node *case_node = node->as.switch_stmt.cases.items[i];

        if (case_node->type != NODE_SWITCH_CASE)
            continue;

        if (case_node->as.switch_case.is_default) {

            if (seen_default) {
                semantic_error(ctx, case_node,
                    "duplicate default case");
            }

            seen_default = 1;

            check_node(ctx, case_node->as.switch_case.body);

            continue;
        }

        Node *case_value_node = case_node->as.switch_case.value;
        Type *case_type       = check_value_expression(ctx, case_value_node);

        ConstValue case_value;

        int has_const_value =
            eval_const_expr(ctx, case_value_node, &case_value);

        if (case_type &&
            !initializer_compatible(
                switch_type,
                case_type,
                case_value_node)) {

            semantic_error(ctx, case_node,
                "switch case type does not match switch expression type");
        }

        if (!has_const_value) {
            /*
             * eval_const_expr already reported the precise error.
             */
            check_node(ctx, case_node->as.switch_case.body);

            continue;
        }

        /*
         * Compare duplicate cases by their evaluated value.
         *
         * This works for integers, bools, and enums because enum members
         * are stored as CONST_VALUE_INT plus their enum Type*.
         */
        for (int j = 0; j < seen_case_count; j++) {
            if (const_values_equal(&seen_cases[j].value, &case_value)) {

                semantic_error(ctx, case_node,
                    "duplicate switch case");

                break;
            }
        }

        seen_cases[seen_case_count].value = case_value;
        seen_cases[seen_case_count].node  = case_node;
        seen_case_count++;

        check_node(ctx, case_node->as.switch_case.body);
    }
}

static int check_assignment_statement(SemanticContext *ctx, Node *node) {
    Node *target_node = node->as.assign.target;
    Node *value_node  = node->as.assign.value;

    Type *target_type = check_expression(ctx, target_node);
    if (!target_type) return 0;

    if (!require_lvalue(ctx, node, target_node, "assignment target is not assignable"))
        return 0;

    if (!check_initializer_against_type(ctx, target_type, value_node))
        return 0;

    sem_record_no_value(ctx, node);
    return 1;
}

static int check_compound_assignment_statement(SemanticContext *ctx, Node *node) {
    Node *target_node = node->as.compound_assign.target;
    Node *value_node  = node->as.compound_assign.value;

    Type *target_type = check_expression(ctx, target_node);

    if (!target_type)
        return 0;

    if (!require_lvalue(ctx, node, target_node, "compound assignment target is not assignable"))
        return 0;

    Type *value_type = check_value_expression(ctx, value_node);

    if (!value_type)
        return 0;

    switch (node->as.compound_assign.op) {
        case TOK_PLUS_EQUAL:
        case TOK_MINUS_EQUAL:
        case TOK_STAR_EQUAL:
        case TOK_SLASH_EQUAL:
        case TOK_PERCENT_EQUAL:
            break;
        default:
            semantic_error(ctx, node, "unsupported compound assignment operator");
            return 0;
    }

    if (node->as.compound_assign.op == TOK_PERCENT_EQUAL &&
        (!is_integer_type(target_type) || !is_integer_type(value_type))) {
        semantic_error(ctx, node, "modulo compound assignment operands must be integers");
        return 0;
    }

    if (!is_numeric_type(target_type)) {
        semantic_error(ctx, node, "compound assignment target must be numeric");
        return 0;
    }

    if (!is_numeric_type(value_type)) {
        semantic_error(ctx, node, "compound assignment value must be numeric");
        return 0;
    }

    Type *result_type = common_numeric_type(target_type, value_type);
    if (!result_type) {
        semantic_error(ctx, node, "compound assignment operands have incompatible numeric types");
        return 0;
    }

    if (!initializer_compatible(target_type, result_type, value_node)) {
        semantic_error(ctx, node, "compound assignment result does not fit target type");
        return 0;
    }

    sem_record_no_value(ctx, node);
    return 1;
}

static int check_inc_dec_statement(SemanticContext *ctx, Node *node) {
    Node *target = node->as.inc_dec.target;

    Type *target_type = check_expression(ctx, target);
    if (!target_type) return 0;

    if (!require_lvalue(ctx, node, target, "increment/decrement target is not assignable"))
        return 0;

    if (!is_numeric_type(target_type)) {
        semantic_error(ctx, node, "increment/decrement requires a numeric target");
        return 0;
    }

    sem_record_no_value(ctx, node);
    return 1;
}

// Entry point for expressions in statement position: a bare
// expression-statement, or a for-loop's post clause. The only place
// assignment/compound-assignment/increment/decrement are legal.
// Everything else -- calls, any other value-producing expression --
// delegates to check_expression with its value discarded.
static int check_statement_expression(SemanticContext *ctx, Node *node) {
    if (!node) return 0;

    switch (node->type) {
        case NODE_ASSIGN:          return check_assignment_statement(ctx, node);
        case NODE_COMPOUND_ASSIGN: return check_compound_assignment_statement(ctx, node);
        case NODE_INC_DEC:         return check_inc_dec_statement(ctx, node);
        default:                   return check_expression(ctx, node) != NULL;
    }
}

static Type *check_value_expression(SemanticContext *ctx, Node *node
) {
    Type *type = check_expression(ctx, node);

    if (!type)
        return NULL;

    SemExprInfo *info = sem_find_expr_info(ctx, node);

    if (!info ||
        type->kind == TYPE_VOID ||
        info->value_category == VALUE_CATEGORY_NONE) {

        semantic_error(ctx, node,
            "expression does not produce a value");

        return NULL;
    }

    return type;
}

static int check_initializer_against_type(SemanticContext *ctx, Type *expected, Node *initializer) {

    if (!expected || !initializer)
        return 0;

    if (initializer->type == NODE_ARRAY_LITERAL) {
        if (!check_array_initializer(ctx, expected, initializer))
            return 0;

        sem_record_expr_info(
            ctx,
            initializer,
            expected,
            NULL,
            VALUE_CATEGORY_RVALUE
        );

        return 1;
    }

    if (initializer->type == NODE_STRING) {
        if (!check_string_initializer(ctx, expected, initializer))
            return 0;

        sem_record_expr_info(
            ctx,
            initializer,
            expected,
            NULL,
            VALUE_CATEGORY_RVALUE
        );

        return 1;
    }

    Type *actual = check_value_expression(ctx, initializer);

    if (!actual)
        return 0;

    if (!initializer_compatible(
            expected,
            actual,
            initializer)) {

        semantic_error(
            ctx,
            initializer,
            "initializer type does not match declared type"
        );

        return 0;
    }

    return 1;
}

static int check_argument_against_parameter(
    SemanticContext *ctx,
    Type *expected,
    Node *argument
) {
    if (!expected || !argument)
        return 0;

    /*
     * Array and string literals are contextual initializers, same as
     * any other check_initializer_against_type call site (var decls,
     * struct fields, returns, assignment RHS). Delegate so call
     * arguments get the same treatment instead of hitting the bare
     * "literal requires an expected type" errors in check_expression.
     */
    if (argument->type == NODE_ARRAY_LITERAL || argument->type == NODE_STRING)
        return check_initializer_against_type(ctx, expected, argument);

    Type *actual = check_value_expression(ctx, argument);

    if (!actual)
        return 0;

    if (!initializer_compatible(expected, actual, argument)) {

        const int name_buffer_size = 128;
        char expected_name[name_buffer_size];
        char actual_name[name_buffer_size];

        format_type_name(expected, expected_name, sizeof(expected_name));
        format_type_name(actual, actual_name, sizeof(actual_name));

        semantic_error_fmt(
            ctx,
            argument,
            "argument type does not match parameter type: expected %s, got %s",
            expected_name,
            actual_name
        );

        return 0;
    }

    return 1;
}

static int check_array_initializer(SemanticContext *ctx, Type *expected, Node *initializer
) {
    if (!expected || !initializer)
        return 0;

    if (initializer->type != NODE_ARRAY_LITERAL) {
        semantic_error(ctx, initializer, "internal error: expected array literal");
        return 0;
    }

    if (expected->kind != TYPE_ARRAY) {
        semantic_error(ctx, initializer, "array literal can only initialize an array type");
        return 0;
    }

    int expected_count = expected->array_size;
    int actual_count   = initializer->as.array_literal.elements.count;

    if (expected_count >= 0 && actual_count != expected_count) {
        semantic_error(ctx, initializer,
            "array initializer element count does not match array size");

        return 0;
    }

    for (int i = 0; i < actual_count; i++) {

        Node *element = initializer->as.array_literal.elements.items[i];

        if (!check_initializer_against_type(ctx, expected->element, element))
            return 0;
    }

    return 1;
}

static void check_var_decl(SemanticContext *ctx, Node *node)
{
    if (scope_find_local(ctx->current_scope, node->as.var_decl.name.data, node->as.var_decl.name.length)) {
        semantic_error_name(
            ctx, node,
            "duplicate variable declaration",
            node->as.var_decl.name.data,
            node->as.var_decl.name.length
        );

        return;
    }

    Type *type = node->as.var_decl.var_type;
    Node *init = node->as.var_decl.initializer;

    /*
     * Resolve declared type first.
     *
     * Important for:
     *
     *     values: i32[3] = [1, 2, 3];
     *
     * The array literal needs the expected type i32[3].
     */
    if (type) {
        type = resolve_type(ctx, type, node);

        if (!type)
            return;

        if (invalid_value_type(type)) {
            semantic_error(ctx, node, "variable cannot have type void");
            return;
        }
    }

    if (init) {
        if (type) {
            if (!check_initializer_against_type(ctx, type, init))
                return;
        } else {
            Type *init_type = check_value_expression(ctx, init);

            if (!init_type)
                return;

            type = init_type;
        }
    }

    if (!type) {
        semantic_error(ctx, node, "could not infer variable type");
        return;
    }

    if (invalid_value_type(type)) {
        semantic_error(ctx, node, "variable cannot have type void");
        return;
    }

    scope_define(ctx, node->as.var_decl.name, SYMBOL_VARIABLE, type);
}

static void check_param_decl(
    SemanticContext *ctx,
    Node *node
) {
    if (scope_find_local(
            ctx->current_scope,
            node->as.param_decl.name.data,
            node->as.param_decl.name.length)) {

        semantic_error_name(
            ctx,
            node,
            "duplicate param declaration",
            node->as.param_decl.name.data,
            node->as.param_decl.name.length
        );

        return;
            }

    Type *type = node->as.param_decl.var_type;

    if (type) {
        type = resolve_type(ctx, type, node);

        if (!type)
            return;

        if (invalid_value_type(type)) {
            semantic_error(
                ctx,
                node,
                "parameter cannot have type void"
            );

            return;
        }
    }

    Node *default_value =
        node->as.param_decl.default_value;

    if (default_value) {
        if (type) {
            if (!check_initializer_against_type(
                    ctx,
                    type,
                    default_value)) {
                return;
                    }
        } else {
            type = check_value_expression(
                ctx,
                default_value
            );

            if (!type)
                return;
        }
    }

    if (!type) {
        semantic_error(
            ctx,
            node,
            "could not determine parameter type"
        );

        return;
    }

    if (invalid_value_type(type)) {
        semantic_error(
            ctx,
            node,
            "parameter cannot have type void"
        );

        return;
    }

    scope_define(
        ctx,
        node->as.param_decl.name,
        SYMBOL_VARIABLE,
        type
    );
}

static void check_program(SemanticContext *ctx, Node *node)
{
    NodeList *stmts = &node->as.program.statements;

    /*
     * Pass 1:
     * Register all named types first.
     *
     * This lets structs, enums, and functions refer to types declared
     * later in the file.
     */
    for (int i = 0; i < stmts->count; i++) {

        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_STRUCT_DECL)
            declare_struct_shell(ctx, stmt);

        if (stmt->type == NODE_ENUM_DECL)
            declare_enum_shell(ctx, stmt);
    }

    /*
     * Pass 2:
     * Fill in type bodies now that all type names are visible.
     */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_STRUCT_DECL)
            fill_struct_fields(ctx, stmt);

        if (stmt->type == NODE_ENUM_DECL)
            fill_enum_members(ctx, stmt);
    }

    /*
     * Pass 3:
     * Register function signatures.
     */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_FUNC_DECL)
            declare_function_signature(ctx, stmt);
    }

    /*
     * Pass 4:
     * Check function bodies and other top-level statements.
     */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_STRUCT_DECL || stmt->type == NODE_ENUM_DECL) {
            continue;
        }

        if (stmt->type == NODE_FUNC_DECL) {
            check_function_body(ctx, stmt);
            continue;
        }

        check_node(ctx, stmt);
    }
}

static void check_if(SemanticContext *ctx, Node *node) {

    Type *cond = check_value_expression(ctx, node->as.if_stmt.condition);

    /*
   * A NULL type means check_expression already reported an error.
   * Only report a boolean-type error when a valid type was returned.
   */
    if (cond && !is_bool_type(cond)) {

        semantic_error(ctx, node->as.if_stmt.condition,
            "if condition must be a boolean expression");
    }

    check_node(ctx, node->as.if_stmt.then_branch);

    if(node->as.if_stmt.else_branch)
        check_node(ctx, node->as.if_stmt.else_branch);
}

// Check functions -------------------------------------------
static Type *make_function_type(SemanticContext *ctx, Node *func)
{
    Type *type = new_type(ctx, TYPE_FUNCTION);

    type->parameter_count = func->as.func_decl.params.count;

    if (type->parameter_count > 0) {
        type->parameters = arena_alloc(
            ctx->arena,
            sizeof(Type *) * type->parameter_count
        );
    }

    for (int i = 0; i < type->parameter_count; i++) {

        Node *param      = func->as.func_decl.params.items[i];
        Type *param_type = resolve_type(ctx, param->as.param_decl.var_type, param);

        if (!param_type)
            return NULL;

        if (contains_void_type(param_type)) {

            semantic_error(ctx, param,
                "parameter cannot have type void");

            return NULL;
        }

        type->parameters[i] = param_type;
    }

    type->return_type = resolve_type(ctx, func->as.func_decl.return_type, func);

    if (!type->return_type)
        return NULL;


    if (invalid_return_type(type->return_type)) {

        semantic_error(ctx, func,
            "function return type cannot contain void");

        return NULL;
    }

    return type;
}

static int declare_function_signature(SemanticContext *ctx, Node *node)
{
    node->as.func_decl.resolved_type = NULL;

    if (scope_find_local(ctx->current_scope, node->as.func_decl.name.data, node->as.func_decl.name.length)) {

        semantic_error_name(
            ctx, node,
            "duplicate declaration",
            node->as.func_decl.name.data,
            node->as.func_decl.name.length
        );

        return 0;
    }

    Type *func_type = make_function_type(ctx, node);

    if (!func_type)
        return 0;

    scope_define(ctx, node->as.func_decl.name, SYMBOL_FUNCTION, func_type);

    node->as.func_decl.resolved_type = func_type;

    return 1;
}

static void check_unreachable_in_block(SemanticContext *ctx, Node *block) {

    if (!block || block->type != NODE_BLOCK)
        return;

    int unreachable = 0;

    for (int i = 0; i < block->as.block.statements.count; i++) {

        Node *stmt = block->as.block.statements.items[i];

        if (unreachable) {
            semantic_error(ctx, stmt,
                "unreachable statement");

            /*
             * Keep checking children so you still catch useful nested
             * errors inside unreachable code.
             */
        }

        if (node_definitely_returns(stmt))
            unreachable = 1;
         }
}

static void check_function_body(SemanticContext *ctx, Node *node)
{
    Type *func_type = node->as.func_decl.resolved_type;

    if (!func_type || func_type->kind != TYPE_FUNCTION)
        return;

    scope_push(ctx);

    for (int i = 0; i < node->as.func_decl.params.count; i++)
        check_param_decl(ctx, node->as.func_decl.params.items[i]);

    int saved_loop_depth    = ctx->loop_depth;
    Type *saved_return_type = ctx->current_return_type;

    ctx->loop_depth = 0;
    ctx->current_return_type = func_type->return_type;

    ctx->function_depth++;

    check_node(ctx, node->as.func_decl.body);

    /*
     * After normal semantic checking, enforce that non-void functions
     * cannot fall off the end.
     */
    if (func_type->return_type &&
        func_type->return_type->kind != TYPE_VOID &&
        !node_definitely_returns(node->as.func_decl.body)) {

        semantic_error(ctx, node,
            "non-void function may not return a value");
    }

    ctx->function_depth--;
    ctx->current_return_type = saved_return_type;
    ctx->loop_depth          = saved_loop_depth;

    scope_pop(ctx);
}

static void check_function(SemanticContext *ctx, Node *node)
{
    if (!declare_function_signature(ctx, node))
        return;

    check_function_body(ctx, node);
}

static int declare_struct_shell(SemanticContext *ctx, Node *node) {

    if (scope_find_local(ctx->current_scope, node->as.struct_decl.name.data, node->as.struct_decl.name.length)) {
        semantic_error_name(ctx, node, "duplicate declaration",
            node->as.struct_decl.name.data, node->as.struct_decl.name.length);

        return 0;
    }

    Type *type = new_type(ctx, TYPE_STRUCT);

    type->struct_name.data   = node->as.struct_decl.name.data;
    type->struct_name.length = node->as.struct_decl.name.length;

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
    type->fields      = arena_alloc(ctx->arena, sizeof(StructField) * type->field_count);

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

        Type *field_type  = resolve_type(ctx, field->as.struct_field_decl.var_type, field);

        if (!field_type) {
            type->fields[i].type = NULL;
            continue;
        }

        if (invalid_value_type(field_type)) {

            semantic_error(ctx, field,
                "struct field cannot have type void");

            type->fields[i].type = NULL;
            continue;
        }

        type->fields[i].type = field_type;
    }
}

// ===========================================================
// enums
// ===========================================================
static int declare_enum_shell(SemanticContext *ctx, Node *node) {
    if (scope_find_local(ctx->current_scope, node->as.enum_decl.name.data, node->as.enum_decl.name.length)) {

        semantic_error_name(
            ctx, node,
            "duplicate declaration",
            node->as.enum_decl.name.data,
            node->as.enum_decl.name.length);

        return 0;
    }

    Type *type      = new_type(ctx, TYPE_ENUM);
    type->enum_name = node->as.enum_decl.name;

    scope_define(ctx, node->as.enum_decl.name, SYMBOL_TYPE, type);

    node->as.enum_decl.resolved_type = type;

    return 1;
}

static EnumMember *find_enum_member(
    Type *enum_type,
    const char *name,
    size_t length
) {
    if (!enum_type || enum_type->kind != TYPE_ENUM)
        return NULL;


    for (int i = 0; i < enum_type->enum_member_count; i++) {

        EnumMember *member = &enum_type->enum_members[i];

        if (names_equal(member->name.data, member->name.length, name, length))
            return member;
    }

    return NULL;
}

static void fill_enum_members(SemanticContext *ctx, Node *node) {

    Type *type = node->as.enum_decl.resolved_type;

    if (!type)
        return;

    Type *backing_type = NULL;

    if (node->as.enum_decl.backing_type) {

        backing_type = resolve_type(ctx, node->as.enum_decl.backing_type, node);

        if (!backing_type)
            return;

    } else {
        /*
         * Default enum backing type.
         */
        backing_type = new_type(ctx, TYPE_I32);
    }

    if (!is_integer_kind(backing_type->kind)) {
        semantic_error(ctx, node,
            "enum backing type must be an integer type");

        return;
    }

    type->enum_backing_type = backing_type;

    int count = node->as.enum_decl.members.count;

    type->enum_members = count
        ? arena_alloc(
            ctx->arena,
            sizeof(EnumMember) * count
        )
        : NULL;

    type->enum_member_count = count;

    long long next_value = 0;

    for (int i = 0; i < count; i++) {

        Node *member_node      = node->as.enum_decl.members.items[i];
        StringView member_name = member_node->as.enum_member.name;

        int duplicate = 0;

        for (int j = 0; j < i; j++) {

            if (names_equal(
                    type->enum_members[j].name.data,
                    type->enum_members[j].name.length,
                    member_name.data,
                    member_name.length)) {

                semantic_error_name(
                    ctx, member_node,
                    "duplicate enum member",
                    member_name.data,
                    member_name.length
                );

                duplicate = 1;
                break;
            }
        }

        long long value    = next_value;
        int value_is_valid = 1;

        if (member_node->as.enum_member.value) {

            Node *value_node = member_node->as.enum_member.value;

            ConstValue constant;

            if (!eval_const_expr(ctx, value_node, &constant)) {
                /*
                 * eval_const_expr already reported the precise error.
                 * Keep recovery going with next_value, but avoid
                 * producing a misleading range error.
                 */
                value = next_value;
                value_is_valid = 0;
            } else if (constant.kind != CONST_VALUE_INT) {

                semantic_error(ctx, member_node,
                    "enum member value must be an integer constant");

                value = next_value;
                value_is_valid = 0;
            } else {
                value = constant.as.i;

                /*
                 * eval_const_expr already proved this is compile-time known
                 * and drives the actual enum member value below. This call
                 * only populates the side table with the ordinary semantic
                 * type/category, mirroring check_const_decl, so every
                 * expression node in the program has semantic info -- no
                 * exception needed in the future invariant verifier or dump.
                 */
                check_value_expression(ctx, value_node);
            }
        }

        if (value_is_valid && !integer_value_fits_type(value, backing_type->kind)) {

            semantic_error(ctx, member_node,
                "enum member value does not fit in backing type");

            /*
             * Still store the value for recovery, but don't advance
             * from an invalid out-of-range value.
             */
            value_is_valid = 0;
        }

        type->enum_members[i].name  = member_name;
        type->enum_members[i].value = value;
        member_node->as.enum_member.resolved_value = value;

        if (!duplicate && value_is_valid)
            next_value = value + 1;
    }
}

static Type *check_cast_expression(SemanticContext *ctx, Node *node) {

    Type *target_type = resolve_type(ctx, node->as.cast_expr.target_type, node);

    if (!target_type) {

        semantic_error(ctx, node, "could not resolve cast target type");

        return NULL;
    }

    if (invalid_value_type(target_type)) {

        semantic_error(ctx, node, "cannot cast to void");

        return NULL;
    }

    Type *source_type = check_value_expression(ctx, node->as.cast_expr.expression);

    if (!source_type)
        return NULL;

    if (!is_allowed_explicit_cast(target_type, source_type)) {

        semantic_error(ctx, node,
            "invalid explicit cast");

        return NULL;
    }

    /*
    * A compile-time-known cast must satisfy the same representability
    * checks everywhere it appears, even when its result is discarded.
    *
    * Constant declarations already reach eval_const_cast() through
    * eval_const_expr(). Reuse that path here so ordinary expression
    * contexts do not accidentally skip integer and enum backing-range
    * validation.
    */
    if (expression_is_compile_time_constant(
            ctx,
            node->as.cast_expr.expression)) {
        ConstValue ignored;

        if (!eval_const_cast(ctx, node, &ignored))
            return NULL;
            }

    return target_type;
}

static int block_definitely_returns(Node *node)
{
    if (!node || node->type != NODE_BLOCK)
        return 0;

    /*
     * A block definitely returns if execution cannot reach the end
     * of the block.
     *
     * For now, this means one of its statements definitely returns.
     *
     * Example:
     *     {
     *         return 1;
     *         x = 2; // unreachable later, but not checked yet
     *     }
     */
    for (int i = 0; i < node->as.block.statements.count; i++) {

        Node *stmt = node->as.block.statements.items[i];

        if (node_definitely_returns(stmt))
            return 1;
    }

    return 0;
}

static int enum_switch_is_exhaustive(Node *node, Type *switch_type) {

    if (!node ||
        node->type != NODE_SWITCH ||
        !switch_type ||
        switch_type->kind != TYPE_ENUM) {
        return 0;
    }

    /*
     * Empty enums cannot be exhaustively switched over by cases.
     * A default case is still handled separately by switch_definitely_returns().
     */
    if (switch_type->enum_member_count <= 0)
        return 0;

    /*
     * Exhaustiveness is value-based, not name-based.
     *
     * If an enum has aliases:
     *
     *     A = 1,
     *     B = 1,
     *
     * then one case with value 1 covers both at runtime.
     */
    for (int member_index = 0; member_index < switch_type->enum_member_count; member_index++) {

        EnumMember *member = &switch_type->enum_members[member_index];

        int found = 0;

        for (int case_index = 0; case_index < node->as.switch_stmt.cases.count; case_index++) {

            Node *case_node = node->as.switch_stmt.cases.items[case_index];

            if (!case_node ||
                case_node->type != NODE_SWITCH_CASE ||
                case_node->as.switch_case.is_default) {
                continue;
            }

            Node *value = case_node->as.switch_case.value;

            /*
             * Version 1: only recognize direct qualified enum members:
             *
             *     case Color.Red:
             *
             * This avoids re-running constant evaluation during return
             * analysis and keeps this pass side-effect-free.
             */
            if (!value ||
                value->type != NODE_FIELD ||
                !value->as.field.object ||
                value->as.field.object->type != NODE_IDENT) {
                continue;
            }

            Node *enum_name = value->as.field.object;

            if (!names_equal(
                    enum_name->as.ident.data,
                    enum_name->as.ident.length,
                    switch_type->enum_name.data,
                    switch_type->enum_name.length)) {
                continue;
            }

            EnumMember *case_member =
                find_enum_member(switch_type, value->as.field.name.data, value->as.field.name.length);

            if (!case_member)
                continue;

            if (case_member->value == member->value) {
                found = 1;
                break;
            }
        }

        if (!found)
            return 0;
    }

    return 1;
}

static int switch_definitely_returns(Node *node)
{
    if (!node || node->type != NODE_SWITCH)
        return 0;

    int has_default = 0;
    int has_true    = 0;
    int has_false   = 0;

    Type *switch_type = node->as.switch_stmt.resolved_type;

    if (node->as.switch_stmt.cases.count == 0)
        return 0;

    for (int i = 0; i < node->as.switch_stmt.cases.count; i++) {

        Node *case_node = node->as.switch_stmt.cases.items[i];

        if (!case_node || case_node->type != NODE_SWITCH_CASE)
            return 0;

        if (case_node->as.switch_case.is_default) {
            has_default = 1;
        } else {
            Node *value = case_node->as.switch_case.value;

            /*
             * Bool switches are exhaustive when both literal cases exist.
             */
            if (value && value->type == NODE_BOOL) {
                if (value->as.boolean.value)
                    has_true = 1;
                else
                    has_false = 1;
            }
        }

        /*
         * Coglet switch has no fallthrough, so every case body must
         * definitely return for the switch as a whole to definitely return.
         */
        if (!node_definitely_returns(case_node->as.switch_case.body))
            return 0;
    }

    if (has_default)
        return 1;

    if (has_true && has_false)
        return 1;

    if (enum_switch_is_exhaustive(node, switch_type))
        return 1;

    return 0;
}

static int node_definitely_returns(Node *node)
{
    if (!node)
        return 0;

    switch (node->type) {
        case NODE_RETURN:
            return 1;

        case NODE_BLOCK:
            return block_definitely_returns(node);

        case NODE_IF:
            /*
             * An if only definitely returns if both branches exist and
             * both definitely return.
             */
            if (!node->as.if_stmt.else_branch)
                return 0;

            return node_definitely_returns(node->as.if_stmt.then_branch) &&
                   node_definitely_returns(node->as.if_stmt.else_branch);

        case NODE_SWITCH:
            return switch_definitely_returns(node);

        case NODE_WHILE:
            /*
             * Only an obviously infinite while loop can definitely return.
             *
             * while true {
             *     return 1;
             * }
             *
             * while condition {
             *     return 1;
             * }
             *
             * The second one is not definitely returning, because the loop may
             * execute zero times.
             */
            if (!node_is_literal_true(node->as.while_stmt.condition))
                return 0;

            return node_definitely_returns(node->as.while_stmt.body);

        default:
            return 0;
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
    case NODE_EXPR_STMT:       check_statement_expression(ctx, node->as.expr_stmt.expr); break;

    case NODE_STRUCT_DECL: {
        declare_struct_shell(ctx, node);
        fill_struct_fields(ctx, node);
        break;
    }

    case NODE_ENUM_DECL: {
        declare_enum_shell(ctx, node);
        fill_enum_members(ctx, node);
        break;
    }

    case NODE_WHILE: {

        Type *cond = check_value_expression(ctx, node->as.while_stmt.condition);

        if (cond && !is_bool_type(cond)) {
            semantic_error(ctx,node->as.while_stmt.condition,
                "while condition must be a boolean expression");
        }

        ctx->loop_depth++;
        check_node(ctx, node->as.while_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_BREAK:
        if (ctx->loop_depth <= 0) {
            semantic_error(
                ctx,
                node,
                "break statement not inside loop"
            );
        }
        break;

    case NODE_CONTINUE:

        if (ctx->loop_depth <= 0) {
            semantic_error(ctx, node,
                "continue statement not inside loop");
        }

        break;

    case NODE_SWITCH:
        check_switch_statement(ctx, node);
        break;

    case NODE_SWITCH_CASE:
        /*
         * Normally switch cases are checked by check_switch_statement().
         * This fallback is only for malformed/manual ASTs.
         */
        if (!node->as.switch_case.is_default)
            check_value_expression(ctx, node->as.switch_case.value);

        check_node(ctx, node->as.switch_case.body);

        break;

    case NODE_FOR: {
        if (node->as.for_stmt.condition) {
            Type *cond = check_value_expression(ctx, node->as.for_stmt.condition);

            if (cond && !is_bool_type(cond)) {
                semantic_error(ctx,node->as.for_stmt.condition,
                    "for condition must be a boolean expression");
            }
        }

        if (node->as.for_stmt.post)
            check_statement_expression(ctx, node->as.for_stmt.post);

        ctx->loop_depth++;
        check_node(ctx, node->as.for_stmt.body);
        ctx->loop_depth--;

        break;
    }

    case NODE_RETURN: {
        if (ctx->function_depth == 0) {
            semantic_error(ctx, node, "return outside function");
            break;
        }

        Type *expected = ctx->current_return_type;
        Node *value    = node->as.return_stmt.value;

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

        if (expected->kind == TYPE_VOID) {
            semantic_error(ctx, node,
                "void function cannot return a value");

            break;
        }

        check_initializer_against_type(ctx, expected, value);

        break;
    }

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
    ctx->error_count    = 0;

    ctx->current_return_type = NULL;

    ctx->type_bool = new_type(ctx, TYPE_BOOL);
    ctx->type_void = new_type(ctx, TYPE_VOID);

    ctx->current_scope = scope_new(ctx, NULL);

    ctx->expr_infos = NULL;

    check_node(ctx,  program);
}

SemExprInfo *semantic_get_expr_info(SemanticContext *ctx, Node *node) {
    return sem_find_expr_info(ctx, node);
}