// src/semantic_anal.c
#include "semantic_anal.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>

#include "string_decode.h"
#include "utils/utils.h"

static Type *new_type(SemanticContext *ctx, TypeKind kind)
{
    Type *type = arena_alloc(ctx->arena, sizeof(Type));
    memset(type, 0, sizeof(*type));

    type->kind       = kind;
    type->array_size = -1;

    return type;
}

static Type *builtin_type(SemanticContext *ctx, TypeKind kind) {
    switch (kind) {
        case TYPE_I8:  return ctx->type_i8;
        case TYPE_I16: return ctx->type_i16;
        case TYPE_I32: return ctx->type_i32;
        case TYPE_I64: return ctx->type_i64;

        case TYPE_U8:  return ctx->type_u8;
        case TYPE_U16: return ctx->type_u16;
        case TYPE_U32: return ctx->type_u32;
        case TYPE_U64: return ctx->type_u64;

        case TYPE_F32: return ctx->type_f32;
        case TYPE_F64: return ctx->type_f64;

        case TYPE_BOOL:return ctx->type_bool;
        case TYPE_VOID:return ctx->type_void;
        case TYPE_NULL:return ctx->type_null;
        default:
            return NULL;
    }
}

static void assert_canonical_builtin_type(SemanticContext *ctx, Type *type) {

#ifndef NDEBUG
    if (!type)
        return;

    Type *canonical =
        builtin_type(ctx, type->kind);

    assert(!canonical || type == canonical);
#else
    (void)ctx;
    (void)type;
#endif
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

static SemExprInfo *sem_find_expr_info(SemanticContext *ctx, Node *node) {

    for (SemExprInfo *info = ctx->expr_infos; info; info = info->next) {
        if (info->node == node)
            return info;
    }

    return NULL;
}

static SemExprInfo *sem_get_or_create_expr_info(SemanticContext *ctx, Node *node) {

    SemExprInfo *info = sem_find_expr_info(ctx, node);
    if (info) return info;

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

    assert_canonical_builtin_type(ctx, type);

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

    *sym = (Symbol){
        .name = name,
        .kind = kind,
        .type = type,
        .next = ctx->current_scope->symbols,
    };

    ctx->current_scope->symbols = sym;
}

static Type *lookup_type(SemanticContext *ctx, const char *name, size_t length) {

    Symbol *sym = scope_lookup(ctx->current_scope, name, length);
    if(!sym || sym->kind != SYMBOL_TYPE) return NULL;
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

    Type *canonical = builtin_type(ctx, type->kind);

    if (canonical)
        return canonical;

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

    if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY) {

        Type *resolved_element =
            resolve_type(ctx, type->element, error_node);

        if (!resolved_element)
            return NULL;

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
static EnumMember *find_enum_member_by_value(Type *enum_type, IntegerValue value);
static Type *check_value_expression(SemanticContext *ctx, Node *node);
static Type *check_cast_expression(SemanticContext *ctx, Node *node);
static Type *concretize_inferred_type(SemanticContext *ctx, Node *expression, Type *type);

static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out);
static int expression_is_compile_time_constant(SemanticContext *ctx, Node *node);
static int eval_const_expr(SemanticContext *ctx, Node *node, ConstValue *out);
static int check_string_initializer(SemanticContext *ctx, Type *expected, Node *initializer);

// ============================================================
// expressions
// ============================================================
static int contains_void_type(Type *type) {

    if (!type) return 0;

    if (type->kind == TYPE_VOID) return 1;

    if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY)
        return contains_void_type(type->element);

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

    if (!buffer || buffer_size == 0) return;

    if (!type) {
        snprintf(buffer, buffer_size, "<unknown>");
        return;
    }

    switch (type->kind) {
        case TYPE_VOID: snprintf(buffer, buffer_size, "void"); return;
        case TYPE_BOOL: snprintf(buffer, buffer_size, "bool"); return;
        case TYPE_NULL: snprintf(buffer, buffer_size, "null"); return;

        case TYPE_I8:   snprintf(buffer, buffer_size, "i8");   return;
        case TYPE_I16:  snprintf(buffer, buffer_size, "i16");  return;
        case TYPE_I32:  snprintf(buffer, buffer_size, "i32");  return;
        case TYPE_I64:  snprintf(buffer, buffer_size, "i64");  return;
        case TYPE_U8:   snprintf(buffer, buffer_size, "u8");   return;
        case TYPE_U16:  snprintf(buffer, buffer_size, "u16");  return;
        case TYPE_U32:  snprintf(buffer, buffer_size, "u32");  return;
        case TYPE_U64:  snprintf(buffer, buffer_size, "u64");  return;
        case TYPE_F32:  snprintf(buffer, buffer_size, "f32");  return;
        case TYPE_F64:  snprintf(buffer, buffer_size, "f64");  return;
        case TYPE_UNTYPED_INT: snprintf(buffer, buffer_size, "untyped-int"); return;
        case TYPE_UNTYPED_FLOAT: snprintf(buffer, buffer_size, "untyped-float"); return;

        case TYPE_FUNCTION: snprintf(buffer, buffer_size, "function"); return;

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
            snprintf(buffer, buffer_size, "%s*", element);
            return;
        }

        case TYPE_ARRAY: {
            char element[128];
            format_type_name(type->element, element, sizeof(element));

            if (type->array_size >= 0) {
                snprintf(
                    buffer,
                    buffer_size,
                    "%s[%d]",
                    element,
                    type->array_size
                );
            } else {
                snprintf(buffer, buffer_size, "%s[]", element);
            }

            return;
        }
    }

    snprintf(buffer, buffer_size, "<unknown>");
}

static int type_equal(const Type *a, const Type *b) {

    /*
     * A missing type is not a semantic type, including when both
     * arguments are NULL.
     */
    if (!a || !b) return 0;

    /*
     * Canonical built-ins and nominal struct/enum declarations
     * normally finish here.
     */
    if (a == b) return 1;

    if (a->kind != b->kind) return 0;

    switch (a->kind) {
        /*
         * These types have no additional identity-bearing fields.
         * Once their kinds match, they are equal.
         */
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

        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:

        case TYPE_NULL:
            return 1;

        /*
         * Compound structural types.
         */
        case TYPE_POINTER:
            return type_equal(
                a->element,
                b->element
            );

        case TYPE_ARRAY:
            return a->array_size == b->array_size &&
                   type_equal(
                       a->element,
                       b->element
                   );

        case TYPE_FUNCTION:
            if (a->parameter_count !=
                b->parameter_count) {
                return 0;
            }

            for (int i = 0;
                 i < a->parameter_count;
                 i++) {
                if (!type_equal(
                        a->parameters[i],
                        b->parameters[i]
                    )) {
                    return 0;
                }
            }

            return type_equal(
                a->return_type,
                b->return_type
            );

        /*
         * Parsed named types may be compared before resolution in
         * defensive or diagnostic paths. At this stage their source
         * names are the only available identity.
         */
        case TYPE_NAMED:
            return names_equal(
                a->named_name.data,
                a->named_name.length,
                b->named_name.data,
                b->named_name.length
            );

        /*
         * Structs and enums are nominal.
         *
         * Equal declaration identities were already accepted by the
         * `a == b` fast path. Reaching these cases means the objects
         * came from different declarations.
         */
        case TYPE_STRUCT:
        case TYPE_ENUM:
            return 0;
    }

    /*
     * Do not silently consider a newly introduced TypeKind equal.
     *
     * Keeping the switch without a default also allows compiler
     * warnings such as -Wswitch to report newly added enum members.
     */
    assert(!"unhandled TypeKind in type_equal");
    return 0;
}

static int invalid_value_type(Type *type) { return contains_void_type(type); }

static int invalid_return_type(Type *type) {

    if (!type) return 0;

    if (type->kind == TYPE_VOID) return 0;

    return contains_void_type(type);
}

static int is_concrete_integer_kind(TypeKind kind) {

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

static int is_unsigned_integer_kind(TypeKind kind) {

    switch (kind) {
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
            return 1;

        default:
            return 0;
    }
}

static int is_signed_integer_kind(TypeKind kind)
{
    switch (kind) {
        case TYPE_I8:
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
            return 1;

        default:
            return 0;
    }
}

static int integer_kind_bit_width(TypeKind kind, unsigned *out_width) {

    switch (kind) {
        case TYPE_I8:
        case TYPE_U8:
            *out_width = 8;
            return 1;

        case TYPE_I16:
        case TYPE_U16:
            *out_width = 16;
            return 1;

        case TYPE_I32:
        case TYPE_U32:
            *out_width = 32;
            return 1;

        case TYPE_I64:
        case TYPE_U64:
            *out_width = 64;
            return 1;

        default:
            return 0;
    }
}

static int is_integer_kind(TypeKind kind) { return is_concrete_integer_kind(kind) || kind == TYPE_UNTYPED_INT;}
static int is_concrete_float_kind(TypeKind kind) { return kind == TYPE_F32 || kind == TYPE_F64; }
static int is_float_kind(TypeKind kind) { return is_concrete_float_kind(kind) || kind == TYPE_UNTYPED_FLOAT; }
static int is_untyped_numeric_kind(TypeKind kind) { return kind == TYPE_UNTYPED_INT || kind == TYPE_UNTYPED_FLOAT; }
static int is_untyped_numeric_type(const Type *type) { return type && is_untyped_numeric_kind(type->kind); }

static IntegerValue integer_value_make(uint64_t magnitude, int is_negative) {

    IntegerValue value;
    value.magnitude   = magnitude;
    value.is_negative = magnitude != 0 && is_negative;
    return value;
}

static IntegerValue integer_value_negated(IntegerValue value) {
    return integer_value_make(value.magnitude, !value.is_negative);
}

static int integer_values_equal(IntegerValue a, IntegerValue b) {

    return a.magnitude == b.magnitude &&
           a.is_negative == b.is_negative;
}

static int integer_value_compare(IntegerValue a, IntegerValue b) {

    if (a.is_negative != b.is_negative)
        return a.is_negative ? -1 : 1;

    if (a.magnitude == b.magnitude)
        return 0;

    if (a.is_negative)
        return a.magnitude > b.magnitude ? -1 : 1;

    return a.magnitude < b.magnitude ? -1 : 1;
}

static int integer_value_fits_type(IntegerValue value, TypeKind kind) {

    switch (kind) {
        case TYPE_I8:
            return value.is_negative
                ? value.magnitude <= UINT64_C(128)
                : value.magnitude <= UINT64_C(127);

        case TYPE_I16:
            return value.is_negative
                ? value.magnitude <= UINT64_C(32768)
                : value.magnitude <= UINT64_C(32767);

        case TYPE_I32:
            return value.is_negative
                ? value.magnitude <= UINT64_C(2147483648)
                : value.magnitude <= UINT64_C(2147483647);

        case TYPE_I64:
            return value.is_negative
                ? value.magnitude <= (UINT64_C(1) << 63)
                : value.magnitude <= INT64_MAX;

        case TYPE_U8:
            return !value.is_negative &&
                   value.magnitude <= UINT64_C(255);

        case TYPE_U16:
            return !value.is_negative &&
                   value.magnitude <= UINT64_C(65535);

        case TYPE_U32:
            return !value.is_negative &&
                   value.magnitude <= UINT32_MAX;

        case TYPE_U64:
            return !value.is_negative;

        case TYPE_UNTYPED_INT:
            /*
             * Untyped integers retain an exact uint64_t magnitude and
             * may be negative down to the i64 minimum.
             */
            return !value.is_negative ||
                   value.magnitude <= (UINT64_C(1) << 63);

        default:
            return 0;
    }
}

static uint64_t integer_width_mask(unsigned width)
{
    return width == 64
        ? UINT64_MAX
        : (UINT64_C(1) << width) - 1;
}

static int integer_value_to_bit_pattern(IntegerValue value, TypeKind kind, uint64_t *out_pattern) {

    unsigned width;

    if (!integer_kind_bit_width(kind, &width) ||
        !integer_value_fits_type(value, kind)) {
        return 0;
    }

    uint64_t mask = integer_width_mask(width);

    if (value.is_negative) {
        *out_pattern =
            (~value.magnitude + UINT64_C(1)) & mask;
    } else {
        *out_pattern = value.magnitude & mask;
    }

    return 1;
}

static int integer_value_from_bit_pattern(uint64_t pattern, TypeKind kind, IntegerValue *out) {

    unsigned width;

    if (!integer_kind_bit_width(kind, &width))
        return 0;

    uint64_t mask = integer_width_mask(width);
    pattern &= mask;

    if (is_signed_integer_kind(kind)) {
        uint64_t sign_bit =
            UINT64_C(1) << (width - 1);

        if ((pattern & sign_bit) != 0) {
            uint64_t magnitude =
                (~pattern + UINT64_C(1)) & mask;

            *out = integer_value_make(
                magnitude,
                1
            );

            return 1;
        }
    }

    *out = integer_value_make(
        pattern,
        0
    );

    return 1;
}

static int integer_value_bitwise_not(IntegerValue operand, TypeKind kind, IntegerValue *out) {

    unsigned width;
    uint64_t pattern;

    if (!integer_kind_bit_width(kind, &width) ||
        !integer_value_to_bit_pattern(
            operand,
            kind,
            &pattern
        )) {
        return 0;
    }

    pattern =
        (~pattern) & integer_width_mask(width);

    return integer_value_from_bit_pattern(
        pattern,
        kind,
        out
    );
}

static int integer_values_bitwise(
    IntegerValue left,
    IntegerValue right,
    TypeKind kind,
    TokenType operation,
    IntegerValue *out
) {
    uint64_t left_pattern;
    uint64_t right_pattern;
    uint64_t result_pattern;

    if (!integer_value_to_bit_pattern(
            left,
            kind,
            &left_pattern
        ) ||
        !integer_value_to_bit_pattern(
            right,
            kind,
            &right_pattern
        )) {
        return 0;
    }

    switch (operation) {
        case TOK_AND:
            result_pattern =
                left_pattern & right_pattern;
            break;

        case TOK_OR:
            result_pattern =
                left_pattern | right_pattern;
            break;

        case TOK_XOR:
            result_pattern =
                left_pattern ^ right_pattern;
            break;

        default:
            return 0;
    }

    return integer_value_from_bit_pattern(
        result_pattern,
        kind,
        out
    );
}

typedef enum {
    SHIFT_COUNT_VALID,
    SHIFT_COUNT_NEGATIVE,
    SHIFT_COUNT_OUT_OF_RANGE,
} ShiftCountStatus;

static ShiftCountStatus classify_shift_count(IntegerValue value, unsigned bit_width, unsigned *out_count) {
    if (value.is_negative)
        return SHIFT_COUNT_NEGATIVE;

    if (value.magnitude >= bit_width)
        return SHIFT_COUNT_OUT_OF_RANGE;

    *out_count = (unsigned)value.magnitude;
    return SHIFT_COUNT_VALID;
}

static int integer_value_shift(
    IntegerValue operand,
    TypeKind kind,
    TokenType operation,
    unsigned count,
    IntegerValue *out
) {
    unsigned width;
    uint64_t pattern;

    if (!integer_kind_bit_width(kind, &width) ||
        count >= width ||
        !integer_value_to_bit_pattern(
            operand,
            kind,
            &pattern
        )) {
        return 0;
    }

    uint64_t mask = integer_width_mask(width);
    uint64_t result_pattern;

    switch (operation) {
        case TOK_SHIFT_LEFT:
            /*
             * Left shift is a fixed-width bit-pattern operation.
             * Bits shifted beyond the type width are discarded.
             */
            result_pattern = (pattern << count) & mask;
            break;

        case TOK_SHIFT_RIGHT:
            /*
             * Begin with a logical shift performed on the unsigned
             * representation.
             */
            result_pattern = pattern >> count;

            /*
             * Signed right shift is explicitly arithmetic.
             *
             * Do not rely on the host C implementation's behavior
             * when shifting a negative signed integer.
             */
            if (count != 0 &&
                is_signed_integer_kind(kind)) {
                uint64_t sign_bit = UINT64_C(1) << (width - 1);

                if ((pattern & sign_bit) != 0) {
                    uint64_t sign_fill = mask ^ (mask >> count);

                    result_pattern |= sign_fill;
                }
            }

            break;

        default:
            return 0;
    }

    return integer_value_from_bit_pattern(
        result_pattern,
        kind,
        out
    );
}

static int default_integer_kind_for_value(IntegerValue value, TypeKind *out_kind) {

    if (value.is_negative) {
        if (value.magnitude <= UINT64_C(2147483648)) {
            *out_kind = TYPE_I32;
        } else if (value.magnitude <= (UINT64_C(1) << 63)) {
            *out_kind = TYPE_I64;
        } else {
            return 0;
        }
    } else if (value.magnitude <= INT32_MAX) {
        *out_kind = TYPE_I32;
    } else if (value.magnitude <= INT64_MAX) {
        *out_kind = TYPE_I64;
    } else {
        *out_kind = TYPE_U64;
    }

    return 1;
}

static Type *untyped_integer_type_for_value(SemanticContext *ctx, IntegerValue value) {

    TypeKind ignored_kind;

    if (!default_integer_kind_for_value(value, &ignored_kind))
        return NULL;

    return new_type(ctx, TYPE_UNTYPED_INT);
}

static double integer_value_to_double(IntegerValue value)
{
    double result = (double)value.magnitude;

    return value.is_negative ? -result : result;
}

static int double_to_integer_value(double value, IntegerValue *out) {

    if (!isfinite(value)) return 0;

    int is_negative  = value < 0.0;
    double magnitude = is_negative ? -value : value;

    /*
     * 2^64 is the first double outside the uint64_t magnitude
     * domain. The cast below is therefore performed only for a
     * representable nonnegative integral part.
     */
    if (magnitude >= 18446744073709551616.0)
        return 0;

    *out = integer_value_make((uint64_t)magnitude, is_negative);

    return 1;
}

static int round_float_for_type(double value, TypeKind kind, double *out) {

    if (kind == TYPE_UNTYPED_FLOAT)
        kind = TYPE_F64;

    if (kind == TYPE_F32) {
        /*
         * Finite values must fit in the finite f32 range.
         *
         * Infinity and NaN are representable IEEE-754 values and
         * remain infinity or NaN when converted to f32.
         */
        if (isfinite(value) && (value > FLT_MAX || value < -FLT_MAX)) {
            return 0;
        }

        *out = (double)(float)value;
        return 1;
    }

    if (kind == TYPE_F64) {
        /*
         * `value` is already represented as a C double. This includes
         * finite values, infinities, NaNs, and signed zero.
         */
        *out = value;
        return 1;
    }

    return 0;
}

static int integer_value_add(IntegerValue left, IntegerValue right, IntegerValue *out) {

    if (left.is_negative == right.is_negative) {
        if (UINT64_MAX - left.magnitude < right.magnitude)
            return 0;

        *out = integer_value_make(
            left.magnitude + right.magnitude,
            left.is_negative
        );

        return 1;
    }

    if (left.magnitude >= right.magnitude) {
        *out = integer_value_make(
            left.magnitude - right.magnitude,
            left.is_negative
        );
    } else {
        *out = integer_value_make(
            right.magnitude - left.magnitude,
            right.is_negative
        );
    }

    return 1;
}

static int integer_value_subtract(IntegerValue left, IntegerValue right, IntegerValue *out) {

    return integer_value_add(
        left,
        integer_value_negated(right),
        out
    );
}

static int integer_value_multiply(IntegerValue left, IntegerValue right, IntegerValue *out) {

    if (left.magnitude != 0 &&
        right.magnitude > UINT64_MAX / left.magnitude) {
        return 0;
    }

    *out = integer_value_make(
        left.magnitude * right.magnitude,
        left.is_negative != right.is_negative
    );

    return 1;
}

static int signed_integer_min_magnitude(TypeKind kind, uint64_t *out) {

    switch (kind) {
        case TYPE_I8:
            *out = UINT64_C(1) << 7;
            return 1;

        case TYPE_I16:
            *out = UINT64_C(1) << 15;
            return 1;

        case TYPE_I32:
            *out = UINT64_C(1) << 31;
            return 1;

        case TYPE_I64:
            *out = UINT64_C(1) << 63;
            return 1;

        default:
            return 0;
    }
}

static int integer_division_overflows(IntegerValue left, IntegerValue right, TypeKind result_kind) {

    uint64_t minimum_magnitude;

    return signed_integer_min_magnitude(
               result_kind,
               &minimum_magnitude
           ) &&
           left.is_negative &&
           left.magnitude == minimum_magnitude &&
           right.is_negative &&
           right.magnitude == 1;
}

static int is_u8_type(Type *type)    { return type && type->kind == TYPE_U8; }
static int is_integer_type(Type *t)  { return t && is_integer_kind(t->kind); }
static int is_numeric_type(Type *t)  { return t && (is_integer_kind(t->kind) || is_float_kind(t->kind)); }
static int is_bool_type(Type *t)     { return t && t->kind == TYPE_BOOL; }
static int is_enum_type(Type *type)  { return type && type->kind == TYPE_ENUM; }
static int is_null_type(const Type *type) { return type && type->kind == TYPE_NULL;}
static int is_bool_cast_pair(Type *to, Type *from)       { return is_bool_type(to) && is_bool_type(from); }
static int is_numeric_cast_pair(Type *to, Type *from)    { return is_numeric_type(to) && is_numeric_type(from); }
static int is_enum_to_integer_cast(Type *to, Type *from) { return is_integer_kind(to->kind) && is_enum_type(from);}
static int is_integer_to_enum_cast(Type *to, Type *from) { return is_enum_type(to) && is_integer_kind(from->kind); }
static int is_literal_true(Node *node) { return node && node->type == NODE_BOOL && node->as.boolean.value; }

static int is_equality_comparable_type(const Type *type) {

    if (!type) return 0;

    /* Null comparisons already have their own contextual handling */
    return is_integer_kind(type->kind) ||
           is_float_kind(type->kind) ||
           type->kind == TYPE_BOOL ||
           type->kind == TYPE_ENUM ||
           type->kind == TYPE_POINTER;
}

static int is_switchable_type(Type *type) {

    if (!type) return 0;

    return is_integer_kind(type->kind) ||
           type->kind == TYPE_BOOL ||
           type->kind == TYPE_ENUM;
}

static int is_null_to_pointer_cast(Type *to, Type *from) {
    return to && to->kind == TYPE_POINTER && is_null_type(from);
}

static int is_allowed_explicit_cast(Type *to, Type *from) {

    if (!to || !from) return 0;

    /*
     * Casting a type to itself is always allowed.
     */
    if (type_equal(to, from))
        return 1;

    /*
     * A null literal may be given an explicit concrete pointer type:
     *
     *     cast(i32*, null)
     *
     * The reverse conversion is not allowed, and integers do not
     * implicitly or explicitly become pointers through this rule.
     */
    if (is_null_to_pointer_cast(to, from))
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

static int is_pointer_null_pair(const Type *left, const Type *right) {

    if (!left || !right) return 0;

    return
        (left->kind == TYPE_POINTER &&
         right->kind == TYPE_NULL) ||
        (left->kind == TYPE_NULL &&
         right->kind == TYPE_POINTER);
}

static int const_values_equal(ConstValue *a, ConstValue *b)
{
    if (!a || !b) return 0;

    if (a->kind != b->kind) return 0;

    switch (a->kind) {
        case CONST_VALUE_INT:
            return integer_values_equal(
                a->as.integer,
                b->as.integer
            );

        case CONST_VALUE_FLOAT:
            return a->as.floating == b->as.floating;

        case CONST_VALUE_BOOL:
            return a->as.boolean == b->as.boolean;

        case CONST_VALUE_NULL:
            return 1;
    }

    return 0;
}

static int default_numeric_operation_rank(TypeKind kind) {

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

static int default_numeric_kind_for_constant(const ConstValue *value, TypeKind *out_kind) {

    if (value->type &&
        !is_untyped_numeric_type(value->type)) {
        if (!is_concrete_integer_kind(value->type->kind) &&
            !is_concrete_float_kind(value->type->kind)) {
            return 0;
        }

        *out_kind = value->type->kind;
        return 1;
    }

    switch (value->kind) {
        case CONST_VALUE_INT:
            return default_integer_kind_for_value(
                value->as.integer,
                out_kind
            );

        case CONST_VALUE_FLOAT:
            *out_kind = TYPE_F64;
            return 1;

        default:
            return 0;
    }
}

static int constant_numeric_operation_kind(
    const ConstValue *left,
    const ConstValue *right,
    Type *common_type,
    TypeKind *out_kind
) {
    if (!common_type)
        return 0;

    if (is_concrete_integer_kind(common_type->kind) ||
        is_concrete_float_kind(common_type->kind)) {
        *out_kind = common_type->kind;
        return 1;
    }

    if (!is_untyped_numeric_type(common_type))
        return 0;

    TypeKind left_kind;
    TypeKind right_kind;

    if (!default_numeric_kind_for_constant(left, &left_kind) ||
        !default_numeric_kind_for_constant(right, &right_kind)) {
        return 0;
    }

    int left_rank  = default_numeric_operation_rank(left_kind);
    int right_rank = default_numeric_operation_rank(right_kind);

    if (!left_rank || !right_rank)
        return 0;

    if (left_rank != right_rank) {
        *out_kind = left_rank > right_rank
            ? left_kind
            : right_kind;
        return 1;
    }

    if (left_kind == TYPE_U64 || right_kind == TYPE_U64) {
        *out_kind = TYPE_U64;
        return 1;
    }

    *out_kind = left_kind;
    return 1;
}


// Determines the semantic result type of a numeric binary operation.
//
//   - If exactly one operand is untyped, the result is the other
//     concrete operand's type.
//   - If both operands are untyped, the result remains untyped-int or
//     untyped-float. Constant evaluation separately selects a provisional
//     concrete operation kind for overflow and rounding behaviour.
//   - If both operands are concrete, they must be exactly equal.
static Type *common_numeric_type(Type *a, Type *b)
{
    if (!a || !b ||
        !is_numeric_type(a) ||
        !is_numeric_type(b)) {
        return NULL;
    }

    int a_is_untyped = is_untyped_numeric_type(a);
    int b_is_untyped = is_untyped_numeric_type(b);

    if (a_is_untyped && !b_is_untyped)
        return b;

    if (b_is_untyped && !a_is_untyped)
        return a;

    if (a_is_untyped && b_is_untyped) {
        if (a->kind == TYPE_UNTYPED_FLOAT)
            return a;

        if (b->kind == TYPE_UNTYPED_FLOAT)
            return b;

        return a;
    }

    if (!type_equal(a, b))
        return NULL;

    return a;
}

static Type *common_integer_type(Type *a, Type *b) {

    if (!a || !b ||
        !is_integer_type(a) ||
        !is_integer_type(b)) {
        return NULL;
    }

    int a_is_untyped =
        a->kind == TYPE_UNTYPED_INT;

    int b_is_untyped =
        b->kind == TYPE_UNTYPED_INT;

    if (a_is_untyped && !b_is_untyped)
        return b;

    if (b_is_untyped && !a_is_untyped)
        return a;

    if (a_is_untyped && b_is_untyped)
        return a;

    if (!type_equal(a, b))
        return NULL;

    return a;
}

static int is_integer_zero_to_pointer(const Type *expected, const Node *value) {
    return expected &&
           expected->kind == TYPE_POINTER &&
           value &&
           value->type == NODE_NUMBER &&
           value->as.number.kind == NUMBER_LITERAL_INTEGER &&
           value->as.number.value.integer == 0;
}

/**
 * Determines whether a value of one type may be used where another type is
 * expected.
 *
 * Current compatibility rules:
 *   - Exact type matches are accepted.
 *   - A null literal contextually adapts to any raw pointer type.
 *   - An untyped integer may adapt to a concrete integer or floating-point
 *     type.
 *   - An untyped floating-point value may adapt to a concrete floating-point
 *     type.
 *   - Concrete numeric types do not implicitly widen or narrow.
 *
 * Integer zero is not a null-pointer constant. The only source-level null
 * pointer value is `null`.
 */
static int initializer_compatible(Type *declared, Type *init_type) {

    if (!declared || !init_type)
        return 1;

    if (type_equal(declared, init_type))
        return 1;

    /*
     * A null literal contextually adapts to any raw pointer type:
     *
     *     T* <- null
     *
     * TYPE_NULL is not globally equal to TYPE_POINTER.
     */
    if (declared->kind == TYPE_POINTER &&
        is_null_type(init_type)) {
        return 1;
        }

    if (init_type->kind == TYPE_UNTYPED_INT) {
        return is_concrete_integer_kind(declared->kind) ||
               is_concrete_float_kind(declared->kind);
    }

    if (init_type->kind == TYPE_UNTYPED_FLOAT)
        return is_concrete_float_kind(declared->kind);

    return 0;
}

// ============================================================
// compile-time constant evaluation
// ============================================================

static int const_value_to_float_type(const ConstValue *value, TypeKind target_kind, double *out) {

    if (!value || !out || !is_float_kind(target_kind))
        return 0;

    switch (value->kind) {
        case CONST_VALUE_INT:
            if (target_kind == TYPE_F32) {
                /*
                 * Convert directly from the exact integer magnitude to
                 * float. Going through double first can double-round a
                 * sufficiently large integer.
                 */
                float converted = (float)value->as.integer.magnitude;

                if (value->as.integer.is_negative)
                    converted = -converted;

                if (!isfinite(converted))
                    return 0;

                *out = (double)converted;
                return 1;
            }

            *out = integer_value_to_double(
                value->as.integer
            );

            return isfinite(*out);

        case CONST_VALUE_FLOAT:
            return round_float_for_type(
                value->as.floating,
                target_kind,
                out
            );

        default:
            return 0;
    }
}

static Type *const_value_default_type(SemanticContext *ctx, const ConstValue *value) {

    if (value->type) return value->type;

    switch (value->kind) {
        case CONST_VALUE_INT:
            return untyped_integer_type_for_value(
                ctx,
                value->as.integer
            );

        case CONST_VALUE_FLOAT:
            return new_type(ctx, TYPE_UNTYPED_FLOAT);

        case CONST_VALUE_BOOL:
            return ctx->type_bool;

        case CONST_VALUE_NULL:
            return ctx->type_null;
    }

    return NULL;
}

static Type *default_concrete_type_for_constant(
    SemanticContext *ctx,
    const ConstValue *value
) {
    if (value->type &&
        !is_untyped_numeric_type(value->type)) {

        /*
         * Null deliberately has no default concrete pointer type.
         */
        if (is_null_type(value->type))
            return NULL;

        /*
         * Preserve compound and named types, but normalize any concrete
         * built-in scalar that reaches this function.
         */
        Type *canonical =
            builtin_type(ctx, value->type->kind);

        return canonical
            ? canonical
            : value->type;
        }

    switch (value->kind) {
        case CONST_VALUE_INT:
        {
            TypeKind kind;

            if (!default_integer_kind_for_value(
                    value->as.integer,
                    &kind)) {
                return NULL;
            }

            return builtin_type(ctx, kind);
        }

        case CONST_VALUE_FLOAT:
            return ctx->type_f64;

        case CONST_VALUE_BOOL:
            return ctx->type_bool;

        case CONST_VALUE_NULL:
            return NULL;
    }

    return NULL;
}

static int integer_constant_result(
    SemanticContext *ctx,
    Node *node,
    Type *result_type,
    TypeKind operation_kind,
    IntegerValue value,
    ConstValue *out
) {
    if (!result_type ||
        !is_integer_kind(result_type->kind) ||
        !is_concrete_integer_kind(operation_kind)) {
        semantic_error(
            ctx,
            node,
            "integer constant expression has no integer operation type"
        );

        return 0;
    }

    if (!integer_value_fits_type(value, operation_kind) ||
        (result_type->kind == TYPE_UNTYPED_INT &&
         !integer_value_fits_type(value, TYPE_UNTYPED_INT))) {
        semantic_error(
            ctx,
            node,
            "integer overflow in constant expression"
        );

        return 0;
    }

    out->kind = CONST_VALUE_INT;
    out->as.integer = value;
    out->type = result_type;

    return 1;
}

static int float_constant_result(
    SemanticContext *ctx,
    Node *node,
    Type *result_type,
    TypeKind operation_kind,
    double value,
    ConstValue *out
) {
    double rounded;

    if (!result_type ||
        !is_float_kind(result_type->kind) ||
        !is_concrete_float_kind(operation_kind) ||
        !round_float_for_type(value, operation_kind, &rounded)) {
        semantic_error(
            ctx,
            node,
            "floating-point overflow in constant expression"
        );

        return 0;
    }

    out->kind = CONST_VALUE_FLOAT;
    out->as.floating = rounded;
    out->type = result_type;

    return 1;
}

static int eval_float_binary_operation(
    SemanticContext *ctx,
    Node *node,
    Type *result_type,
    TypeKind operation_kind,
    const ConstValue *left,
    const ConstValue *right,
    ConstValue *out
) {
    if (!result_type ||
        !is_float_kind(result_type->kind) ||
        !is_concrete_float_kind(operation_kind)) {

        semantic_error(ctx, node,
            "floating-point constant expression has no floating-point operation type");

        return 0;
    }

    double left_value;
    double right_value;

    if (!const_value_to_float_type(left,  operation_kind, &left_value) ||
        !const_value_to_float_type(right, operation_kind, &right_value)) {

        semantic_error(ctx, node,
            "floating-point constant operand does not fit operation type");

        return 0;
    }

    /*
     * Perform f32 operations at f32 precision rather than calculating
     * them as f64 and rounding afterward.
     */
    if (operation_kind == TYPE_F32) {
        float left_f  = (float)left_value;
        float right_f = (float)right_value;
        float result_f;

        switch (node->as.binary.op) {
            case TOK_PLUS:
                result_f = left_f + right_f;
                break;

            case TOK_MINUS:
                result_f = left_f - right_f;
                break;

            case TOK_STAR:
                result_f = left_f * right_f;
                break;

            case TOK_SLASH:
                /*
                 * IEEE-754 defines floating-point division by zero:
                 *
                 *     nonzero / zero -> signed infinity
                 *     zero / zero    -> NaN
                 */
                result_f = left_f / right_f;
                break;

            default:
                return 0;
        }

        return float_constant_result(
            ctx,
            node,
            result_type,
            operation_kind,
            (double)result_f,
            out
        );
    }

    double result;

    switch (node->as.binary.op) {
        case TOK_PLUS:
            result = left_value + right_value;
            break;

        case TOK_MINUS:
            result = left_value - right_value;
            break;

        case TOK_STAR:
            result = left_value * right_value;
            break;

        case TOK_SLASH:
            result = left_value / right_value;
            break;

        default:
            return 0;
    }

    return float_constant_result(
        ctx,
        node,
        result_type,
        operation_kind,
        result,
        out
    );
}

static int eval_const_comparison(
    SemanticContext *ctx,
    Node *node,
    const ConstValue *left,
    const ConstValue *right,
    ConstValue *out
) {
    Type *left_type = const_value_default_type(ctx, left);
    Type *right_type = const_value_default_type(ctx, right);

    int comparison = 0;

    if (is_numeric_type(left_type) &&
        is_numeric_type(right_type)) {

        Type *common = common_numeric_type(
            left_type,
            right_type
        );

        TypeKind operation_kind;

        if (!common ||
            !constant_numeric_operation_kind(
                left,
                right,
                common,
                &operation_kind)) {

            semantic_error(ctx, node,
                "comparison operands have incompatible numeric types");

            return 0;
        }

        if (is_concrete_integer_kind(operation_kind)) {
            if (left->kind != CONST_VALUE_INT ||
                right->kind != CONST_VALUE_INT ||
                !integer_value_fits_type(
                    left->as.integer,
                    operation_kind) ||
                !integer_value_fits_type(
                    right->as.integer,
                    operation_kind)) {

                semantic_error(ctx, node,
                    "integer constant operand does not fit comparison type");

                return 0;
            }

            comparison = integer_value_compare(
                left->as.integer,
                right->as.integer
            );
                } else {
            double left_value;
            double right_value;

            if (!is_concrete_float_kind(operation_kind) ||
                !const_value_to_float_type(
                    left,
                    operation_kind,
                    &left_value) ||
                !const_value_to_float_type(
                    right,
                    operation_kind,
                    &right_value
                )) {

                semantic_error(ctx, node,
                    "floating-point comparison operand does not fit comparison type");

                return 0;
            }

            /*
             * Floating-point comparisons must use the IEEE-754
             * operators directly.
             *
             * A three-way comparison cannot represent the unordered
             * NaN case:
             *
             *     NaN == NaN  -> false
             *     NaN != NaN  -> true
             *
             * All ordered comparisons involving NaN are false.
             */
            out->kind = CONST_VALUE_BOOL;
            out->type = ctx->type_bool;

            switch (node->as.binary.op) {
                case TOK_EQUAL_EQUAL:
                    out->as.boolean =
                        left_value == right_value;
                    break;

                case TOK_BANG_EQUAL:
                    out->as.boolean =
                        left_value != right_value;
                    break;

                case TOK_LESS:
                    out->as.boolean =
                        left_value < right_value;
                    break;

                case TOK_LESS_EQUAL:
                    out->as.boolean =
                        left_value <= right_value;
                    break;

                case TOK_GREATER:
                    out->as.boolean =
                        left_value > right_value;
                    break;

                case TOK_GREATER_EQUAL:
                    out->as.boolean =
                        left_value >= right_value;
                    break;

                default:
                    return 0;
            }

            return 1;
        }
    } else if (
        left->kind == CONST_VALUE_BOOL &&
        right->kind == CONST_VALUE_BOOL
    ) {
        if (node->as.binary.op != TOK_EQUAL_EQUAL &&
            node->as.binary.op != TOK_BANG_EQUAL) {

            semantic_error(ctx, node,
                "ordered comparison requires numeric constants");

            return 0;
        }

        comparison =
            left->as.boolean == right->as.boolean
                ? 0
                : (left->as.boolean ? 1 : -1);
    } else if (
        left->kind == CONST_VALUE_NULL &&
        right->kind == CONST_VALUE_NULL
    ) {
        /*
         * Semantic checking has already ensured that the source
         * comparison has a concrete pointer context.
         *
         * For example:
         *
         *     NONE: i32* : null;
         *     IS_NONE :: NONE == null;
         *
         * Both operands evaluate to CONST_VALUE_NULL even though
         * their semantic types are i32* and TYPE_NULL.
         */
        if (node->as.binary.op != TOK_EQUAL_EQUAL &&
            node->as.binary.op != TOK_BANG_EQUAL) {

            semantic_error(ctx, node,
                "ordered comparison requires numeric constants");

            return 0;
        }

        /*
         * Every null pointer value is equal to every other null
         * pointer value after type checking has approved the
         * comparison.
         */
        comparison = 0;
    } else if (
        left_type &&
        right_type &&
        left_type->kind == TYPE_ENUM &&
        type_equal(left_type, right_type) &&
        left->kind == CONST_VALUE_INT &&
        right->kind == CONST_VALUE_INT &&
        (
            node->as.binary.op == TOK_EQUAL_EQUAL ||
            node->as.binary.op == TOK_BANG_EQUAL
        )
    ) {
        comparison = integer_value_compare(
            left->as.integer,
            right->as.integer
        );
    } else {
        semantic_error(ctx, node,
            "comparison operands are not compatible constants");

        return 0;
    }

    out->kind = CONST_VALUE_BOOL;
    out->type = ctx->type_bool;

    switch (node->as.binary.op) {
        case TOK_EQUAL_EQUAL:
            out->as.boolean = comparison == 0;
            break;

        case TOK_BANG_EQUAL:
            out->as.boolean = comparison != 0;
            break;

        case TOK_LESS:
            out->as.boolean = comparison < 0;
            break;

        case TOK_LESS_EQUAL:
            out->as.boolean = comparison <= 0;
            break;

        case TOK_GREATER:
            out->as.boolean = comparison > 0;
            break;

        case TOK_GREATER_EQUAL:
            out->as.boolean = comparison >= 0;
            break;

        default:
            return 0;
    }

    return 1;
}

// Recursively evaluates an expression that must be knowable at compile
// time: literals, other constants, and unary/binary ops over those.
// Anything reaching outside that (function calls, variables, struct
// inits, etc.) is rejected with a diagnostic.
static int eval_const_expr(SemanticContext *ctx, Node *node, ConstValue *out) {

    if (!node) return 0;

    memset(out, 0, sizeof(*out));

    switch (node->type) {
        case NODE_NUMBER:
            if (node->as.number.kind == NUMBER_LITERAL_FLOAT) {
                out->kind = CONST_VALUE_FLOAT;
                out->as.floating = node->as.number.value.floating;
                out->type = new_type(ctx, TYPE_UNTYPED_FLOAT);
            } else {
                out->kind = CONST_VALUE_INT;
                out->as.integer = integer_value_make(
                    node->as.number.value.integer,
                    0
                );

                out->type = untyped_integer_type_for_value(
                    ctx,
                    out->as.integer
                );
            }

            return 1;

        case NODE_BOOL:
            out->kind = CONST_VALUE_BOOL;
            out->as.boolean = node->as.boolean.value;
            out->type = ctx->type_bool;
            return 1;

        case NODE_NULL:
            out->kind = CONST_VALUE_NULL;
            out->type = ctx->type_null;
            return 1;

        case NODE_CAST:
            return eval_const_cast(ctx, node, out);

        case NODE_IDENT:
        {
            Symbol *symbol = scope_lookup(
                ctx->current_scope,
                node->as.ident.data,
                node->as.ident.length
            );

            if (!symbol ||
                symbol->kind != SYMBOL_CONSTANT) {

                semantic_error(ctx, node,
                    "expression is not a compile-time constant");

                return 0;
            }

            *out = symbol->const_value;
            return 1;
        }

        case NODE_FIELD:
        {
            if (node->as.field.object &&
                node->as.field.object->type == NODE_IDENT) {
                Node *object = node->as.field.object;

                Symbol *symbol = scope_lookup(
                    ctx->current_scope,
                    object->as.ident.data,
                    object->as.ident.length
                );

                if (symbol &&
                    symbol->kind == SYMBOL_TYPE &&
                    symbol->type &&
                    symbol->type->kind == TYPE_ENUM) {
                    EnumMember *member = find_enum_member(
                        symbol->type,
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
                    out->as.integer = member->value;
                    out->type = symbol->type;

                    return 1;
                }
            }

            semantic_error(ctx, node,
                "expression is not a compile-time constant");

            return 0;
        }

        case NODE_UNARY:
        {
            ConstValue operand;

            if (!eval_const_expr(
                    ctx,
                    node->as.unary.operand,
                    &operand
                )) {
                return 0;
            }

            if (node->as.unary.op == TOK_MINUS) {
                if (operand.kind == CONST_VALUE_INT) {

                    Type *result_type =
                        const_value_default_type(ctx, &operand);

                    /*
                     * Typed unsigned constants follow the same rule as ordinary
                     * unsigned expressions. Coglet has not defined wrapping unary
                     * negation for unsigned integers.
                     */
                    if (result_type &&
                        is_unsigned_integer_kind(result_type->kind)) {
                        semantic_error(ctx, node,
                            "unary '-' cannot be applied to an unsigned value");

                        return 0;
                    }

                    IntegerValue value =
                        integer_value_negated(operand.as.integer);

                    TypeKind operation_kind;

                    /*
                     * Untyped negation chooses its provisional concrete
                     * operation kind from the resulting mathematical value.
                     * This preserves spellings such as the i64 minimum.
                     */
                    if (result_type &&
                        result_type->kind == TYPE_UNTYPED_INT) {
                        if (!default_integer_kind_for_value(
                                value,
                                &operation_kind)) {

                            semantic_error(ctx, node,
                                "integer overflow in constant expression");

                            return 0;
                        }
                    } else if (result_type &&
                               is_concrete_integer_kind(
                               result_type->kind)) {
                        operation_kind = result_type->kind;
                    } else {

                        semantic_error(ctx, node,
                            "integer constant expression has no integer type");

                        return 0;
                    }

                    return integer_constant_result(
                        ctx,
                        node,
                        result_type,
                        operation_kind,
                        value,
                        out
                    );
                }

                if (operand.kind == CONST_VALUE_FLOAT) {

                    Type *result_type =
                        const_value_default_type(ctx, &operand);

                    TypeKind operation_kind;

                    if (!default_numeric_kind_for_constant(
                            &operand,
                            &operation_kind) ||
                        !is_concrete_float_kind(operation_kind)) {

                        semantic_error(ctx, node,
                            "floating-point constant expression has no floating-point type");

                        return 0;
                    }

                    return float_constant_result(
                        ctx,
                        node,
                        result_type,
                        operation_kind,
                        -operand.as.floating,
                        out
                    );
                }

                semantic_error(ctx, node,
                    "unary '-' requires a numeric constant");

                return 0;
            }

            if (node->as.unary.op == TOK_TILDE) {
                if (operand.kind != CONST_VALUE_INT) {
                    semantic_error(ctx, node,
                        "unary '~' requires an integer constant");

                    return 0;
                }

                Type *result_type =
                    const_value_default_type(
                        ctx,
                        &operand
                    );

                TypeKind operation_kind;

                if (result_type &&
                    is_concrete_integer_kind(
                        result_type->kind
                    )) {
                        operation_kind = result_type->kind;
                    } else if (
                        result_type &&
                        result_type->kind == TYPE_UNTYPED_INT
                    ) {
                        if (!default_integer_kind_for_value(
                                operand.as.integer,
                                &operation_kind
                            )) {
                            semantic_error(ctx, node,
                                "integer constant expression has no integer operation type");

                            return 0;
                            }
                    } else {
                        semantic_error(ctx, node,
                            "unary '~' requires an integer constant");

                        return 0;
                    }

                IntegerValue value;

                if (!integer_value_bitwise_not(
                        operand.as.integer,
                        operation_kind,
                        &value
                    )) {
                    semantic_error(ctx, node,
                        "integer constant operand does not fit operation type");

                    return 0;
                }

                return integer_constant_result(
                    ctx,
                    node,
                    result_type,
                    operation_kind,
                    value,
                    out
                );
            }

            if (node->as.unary.op == TOK_BANG) {
                if (operand.kind != CONST_VALUE_BOOL) {

                    semantic_error(ctx, node,
                        "unary '!' requires a boolean constant");

                    return 0;
                }

                out->kind = CONST_VALUE_BOOL;
                out->as.boolean = !operand.as.boolean;
                out->type = ctx->type_bool;

                return 1;
            }

            semantic_error(ctx,node,
                "operator not allowed in a constant expression");

            return 0;
        }

        case NODE_BINARY:
        {
            ConstValue left;

            if (!eval_const_expr(
                    ctx,
                    node->as.binary.left,
                    &left
                )) {
                return 0;
            }

            if (node->as.binary.op == TOK_AND_AND ||
                node->as.binary.op == TOK_OR_OR) {
                if (left.kind != CONST_VALUE_BOOL) {
                    semantic_error(ctx, node,
                        "operands must be boolean constants");

                    return 0;
                }

                if ((node->as.binary.op == TOK_AND_AND &&
                     !left.as.boolean) ||
                    (node->as.binary.op == TOK_OR_OR &&
                     left.as.boolean)) {
                    out->kind = CONST_VALUE_BOOL;
                    out->as.boolean = left.as.boolean;
                    out->type = ctx->type_bool;

                    return 1;
                }
            }

            ConstValue right;

            if (!eval_const_expr(
                    ctx,
                    node->as.binary.right,
                    &right
                )) {
                return 0;
            }

            switch (node->as.binary.op) {
                case TOK_AND_AND:
                case TOK_OR_OR:
                    if (left.kind != CONST_VALUE_BOOL ||
                        right.kind != CONST_VALUE_BOOL) {
                        semantic_error(ctx, node,
                            "operands must be boolean constants");

                        return 0;
                    }

                    out->kind = CONST_VALUE_BOOL;
                    out->as.boolean =
                        node->as.binary.op == TOK_AND_AND
                        ? left.as.boolean && right.as.boolean
                        : left.as.boolean || right.as.boolean;
                    out->type = ctx->type_bool;

                    return 1;

                case TOK_EQUAL_EQUAL:
                case TOK_BANG_EQUAL:
                case TOK_LESS:
                case TOK_LESS_EQUAL:
                case TOK_GREATER:
                case TOK_GREATER_EQUAL:
                    return eval_const_comparison(
                        ctx,
                        node,
                        &left,
                        &right,
                        out
                    );

                case TOK_AND:
                case TOK_OR:
                case TOK_XOR:
                {
                    Type *left_type =
                        const_value_default_type(ctx, &left);

                    Type *right_type =
                        const_value_default_type(ctx, &right);

                    if (left.kind != CONST_VALUE_INT ||
                        right.kind != CONST_VALUE_INT ||
                        !is_integer_type(left_type) ||
                        !is_integer_type(right_type)) {
                        semantic_error(ctx, node,
                            "bitwise operators require integer constants");

                        return 0;
                    }

                    Type *result_type =
                        common_integer_type(left_type, right_type);

                    TypeKind operation_kind;

                    if (!result_type ||
                        !constant_numeric_operation_kind(
                            &left,
                            &right,
                            result_type,
                            &operation_kind
                        ) ||
                        !is_concrete_integer_kind(operation_kind)) {
                        semantic_error(ctx, node,
                            "constant operands have incompatible integer types");

                        return 0;
                    }

                    if (!integer_value_fits_type(
                            left.as.integer,
                            operation_kind
                        ) ||
                        !integer_value_fits_type(
                            right.as.integer,
                            operation_kind
                        )) {
                        semantic_error(ctx, node,
                            "integer constant operand does not fit operation type");

                        return 0;
                    }

                    IntegerValue value;

                    if (!integer_values_bitwise(
                            left.as.integer,
                            right.as.integer,
                            operation_kind,
                            node->as.binary.op,
                            &value
                        )) {
                        semantic_error(ctx, node,
                            "integer constant expression has no integer operation type");

                        return 0;
                    }

                    return integer_constant_result(
                        ctx,
                        node,
                        result_type,
                        operation_kind,
                        value,
                        out
                    );
                }

                case TOK_SHIFT_LEFT:
                case TOK_SHIFT_RIGHT:
                {
                    Type *left_type =
                        const_value_default_type(ctx, &left);

                    Type *right_type =
                        const_value_default_type(ctx, &right);

                    if (left.kind != CONST_VALUE_INT || !is_integer_type(left_type)) {
                        semantic_error(ctx, node,
                            "left operand of shift must be an integer constant");

                        return 0;
                    }

                    if (right.kind != CONST_VALUE_INT || !is_integer_type(right_type)) {
                        semantic_error(ctx, node,
                            "right operand of shift must be an integer constant");

                        return 0;
                    }

                    /*
                     * The left operand alone determines the shift width and
                     * signedness.
                     */
                    TypeKind operation_kind;

                    if (is_concrete_integer_kind(left_type->kind)) {
                        operation_kind = left_type->kind;
                    } else if (
                        left_type->kind == TYPE_UNTYPED_INT &&
                        default_integer_kind_for_value(
                            left.as.integer,
                            &operation_kind
                        )
                    ) {
                        /*
                         * An untyped left operand uses its ordinary default integer
                         * width. For example, `1 << count` uses i32.
                         */
                    } else {
                        semantic_error(ctx, node,
                            "shift expression has no integer operation type");

                        return 0;
                    }

                    if (!integer_value_fits_type(left.as.integer, operation_kind)) {
                        semantic_error(ctx, node,
                            "left operand does not fit shift operation type");

                        return 0;
                    }

                    unsigned width;

                    if (!integer_kind_bit_width(operation_kind, &width)) {
                        semantic_error(ctx, node,
                            "shift expression has no integer operation type");

                        return 0;
                    }

                    unsigned count;

                    ShiftCountStatus count_status =
                        classify_shift_count(right.as.integer, width, &count);

                    if (count_status == SHIFT_COUNT_NEGATIVE) {
                        semantic_error(ctx, node->as.binary.right,
                            "shift count cannot be negative");

                        return 0;
                    }

                    if (count_status == SHIFT_COUNT_OUT_OF_RANGE) {
                        semantic_error(ctx, node->as.binary.right,
                            "shift count must be less than left operand bit width");

                        return 0;
                    }

                    IntegerValue value;

                    if (!integer_value_shift(
                            left.as.integer,
                            operation_kind,
                            node->as.binary.op,
                            count,
                            &value
                        )) {
                        semantic_error(ctx, node,
                            "could not evaluate integer shift");

                        return 0;
                    }

                    /*
                     * Preserve an untyped result when the left operand was untyped,
                     * while still evaluating with a concrete provisional width.
                     */
                    return integer_constant_result(
                        ctx,
                        node,
                        left_type,
                        operation_kind,
                        value,
                        out
                    );
                }

                case TOK_PLUS:
                case TOK_MINUS:
                case TOK_STAR:
                case TOK_SLASH:
                case TOK_PERCENT:
                {
                    Type *left_type =
                        const_value_default_type(ctx, &left);

                    Type *right_type =
                        const_value_default_type(ctx, &right);

                    if (!is_numeric_type(left_type) ||
                        !is_numeric_type(right_type)) {

                        semantic_error(ctx, node,
                            "operands must be numeric constants");

                        return 0;
                    }

                    if (node->as.binary.op == TOK_PERCENT &&
                        (!is_integer_kind(left_type->kind) ||
                         !is_integer_kind(right_type->kind))) {

                        semantic_error(ctx, node,
                            "'%' requires integer constants");

                        return 0;
                    }

                    Type *result_type = common_numeric_type(
                        left_type,
                        right_type
                    );

                    TypeKind operation_kind;

                    if (!result_type ||
                        !constant_numeric_operation_kind(
                            &left,
                            &right,
                            result_type,
                            &operation_kind
                        )) {

                        semantic_error(ctx, node,
                            "constant operands have incompatible numeric types");

                        return 0;
                    }

                    if (left.kind == CONST_VALUE_INT &&
                        right.kind == CONST_VALUE_INT &&
                        is_concrete_integer_kind(operation_kind)) {
                        if (!integer_value_fits_type(
                                left.as.integer,
                                operation_kind
                            ) ||
                            !integer_value_fits_type(
                                right.as.integer,
                                operation_kind
                            )) {

                            semantic_error(ctx, node,
                                "integer constant operand does not fit operation type");

                            return 0;
                        }

                        if ((node->as.binary.op == TOK_SLASH ||
                             node->as.binary.op == TOK_PERCENT) &&
                            integer_division_overflows(
                                left.as.integer,
                                right.as.integer,
                                operation_kind
                            )) {

                            semantic_error(ctx, node,
                                "integer overflow in constant expression");

                            return 0;
                        }

                        IntegerValue value;
                        int ok = 0;

                        switch (node->as.binary.op) {
                            case TOK_PLUS:
                                ok = integer_value_add(
                                    left.as.integer,
                                    right.as.integer,
                                    &value
                                );
                                break;

                            case TOK_MINUS:
                                ok = integer_value_subtract(
                                    left.as.integer,
                                    right.as.integer,
                                    &value
                                );
                                break;

                            case TOK_STAR:
                                ok = integer_value_multiply(
                                    left.as.integer,
                                    right.as.integer,
                                    &value
                                );
                                break;

                            case TOK_SLASH:
                                if (right.as.integer.magnitude == 0) {

                                    semantic_error(ctx, node,
                                        "division by zero in constant expression");

                                    return 0;
                                }

                                value = integer_value_make(
                                    left.as.integer.magnitude /
                                        right.as.integer.magnitude,
                                    left.as.integer.is_negative !=
                                        right.as.integer.is_negative
                                );

                                ok = 1;
                                break;

                            case TOK_PERCENT:
                                if (right.as.integer.magnitude == 0) {

                                    semantic_error(ctx, node,
                                        "remainder by zero in constant expression");

                                    return 0;
                                }

                                value = integer_value_make(
                                    left.as.integer.magnitude %
                                        right.as.integer.magnitude,
                                    left.as.integer.is_negative
                                );

                                ok = 1;
                                break;

                            default:
                                break;
                        }

                        if (!ok) {

                            semantic_error(ctx, node,
                                "integer overflow in constant expression");

                            return 0;
                        }

                        return integer_constant_result(
                            ctx,
                            node,
                            result_type,
                            operation_kind,
                            value,
                            out
                        );
                    }

                    if (node->as.binary.op == TOK_PERCENT) {

                        semantic_error(ctx, node,
                            "'%' requires integer constants");

                        return 0;
                    }

                    return eval_float_binary_operation(
                        ctx,
                        node,
                        result_type,
                        operation_kind,
                        &left,
                        &right,
                        out
                    );
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

static Type *concretize_inferred_type(SemanticContext *ctx, Node *expression, Type *type) {

    /*
     * Unlike an untyped numeric literal, null has no sensible
     * default concrete type.
     *
     *     value := 10;    // can default to a concrete integer type
     *     value := null;  // cannot determine the pointee type
     */
    if (is_null_type(type)) {
        semantic_error(ctx, expression,
            "cannot infer a concrete pointer type from null");

        return NULL;
    }

    if (!is_untyped_numeric_type(type))
        return type;

    if (!expression_is_compile_time_constant(ctx, expression)) {
        semantic_error(ctx, expression,
            "cannot infer a concrete type from a non-constant untyped expression");

        return NULL;
    }

    ConstValue value;

    if (!eval_const_expr(ctx, expression, &value))
        return NULL;

    Type *concrete = default_concrete_type_for_constant(
        ctx,
        &value
    );

    if (!concrete) {
        semantic_error(ctx, expression,
            "could not determine a default concrete numeric type");
    }

    return concrete;
}

static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out) {

    ConstValue value;

    if (!eval_const_expr(
            ctx,
            node->as.cast_expr.expression,
            &value
        )) {
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

    if (!is_allowed_explicit_cast(
            target_type,
            source_type
        )) {

        semantic_error(ctx, node,
            "invalid explicit cast");

        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->type = target_type;

    /*
    * A null-to-pointer cast remains a null constant, but carries
    * the concrete destination pointer type.
    */
    if (target_type->kind == TYPE_POINTER) {
        if (value.kind != CONST_VALUE_NULL) {
            semantic_error(ctx, node,
                "pointer cast requires a null constant");

            return 0;
        }

        out->kind = CONST_VALUE_NULL;
        return 1;
    }

    if (target_type->kind == TYPE_ENUM) {
        if (value.kind != CONST_VALUE_INT) {

            semantic_error(ctx, node,
                "enum cast requires an integer constant");

            return 0;
        }

        if (!target_type->enum_backing_type ||
            !integer_value_fits_type(
                value.as.integer,
                target_type->enum_backing_type->kind
            )) {

            semantic_error(ctx, node,
                "enum cast value does not fit in backing type");

            return 0;
        }

        if (!find_enum_member_by_value(
                target_type,
                value.as.integer
            )) {
            semantic_error_fmt(
                ctx,
                node,
                "integer value is not a declared member of enum %.*s",
                (int)target_type->enum_name.length,
                target_type->enum_name.data
            );

            return 0;
            }

        out->kind = CONST_VALUE_INT;
        out->as.integer = value.as.integer;
        out->type = target_type;

        return 1;
    }

    if (is_integer_kind(target_type->kind)) {
        IntegerValue integer_value;

        if (value.kind == CONST_VALUE_INT) {
            integer_value = value.as.integer;
        } else if (value.kind == CONST_VALUE_FLOAT) {
            /*
             * C-like explicit conversion: truncate toward zero,
             * then verify the resulting mathematical integer.
             */
            if (!double_to_integer_value(
                    value.as.floating,
                    &integer_value
                )) {

                semantic_error(ctx, node,
                    "integer cast value does not fit in target type");

                return 0;
            }
        } else {

            semantic_error(ctx, node,
                "integer cast requires numeric constant");

            return 0;
        }

        if (!integer_value_fits_type(
                integer_value,
                target_type->kind
            )) {

            semantic_error(ctx, node,
                "integer cast value does not fit in target type");

            return 0;
        }

        out->kind = CONST_VALUE_INT;
        out->as.integer = integer_value;

        return 1;
    }

    if (is_float_kind(target_type->kind)) {
        double float_value;

        if (!const_value_to_float_type(
                &value,
                target_type->kind,
                &float_value)) {

            semantic_error(ctx, node,
                "float cast value does not fit in target type");

            return 0;
                }

        out->kind = CONST_VALUE_FLOAT;
        out->as.floating = float_value;

        return 1;
    }

    if (target_type->kind == TYPE_BOOL) {
        if (value.kind != CONST_VALUE_BOOL) {

            semantic_error(ctx,node,
                "bool cast requires boolean constant");

            return 0;
        }

        out->kind = CONST_VALUE_BOOL;
        out->as.boolean = value.as.boolean;

        return 1;
    }

    semantic_error(ctx, node,
        "invalid constant cast");

    return 0;
}

static int coerce_constant_to_type(
    SemanticContext *ctx,
    Node *node,
    const ConstValue *value,
    Type *target_type,
    const char *integer_range_message,
    const char *float_range_message,
    ConstValue *out
) {
    *out = *value;
    out->type = target_type;

    if (is_integer_kind(target_type->kind)) {
        if (value->kind != CONST_VALUE_INT ||
            !integer_value_fits_type(
                value->as.integer,
                target_type->kind
            )) {

            semantic_error(ctx,node,
                integer_range_message);

            return 0;
        }

        return 1;
    }

    if (is_float_kind(target_type->kind)) {

        double float_value;

        if (!const_value_to_float_type(
                value,
                target_type->kind,
                &float_value)) {

            semantic_error(ctx, node,
                float_range_message);

            return 0;
        }

        out->kind = CONST_VALUE_FLOAT;
        out->as.floating = float_value;

        return 1;
    }

    return 1;
}

static int check_constant_value_against_type(
    SemanticContext *ctx,
    Node *node,
    Type *target_type,
    const char *integer_range_message,
    const char *float_range_message
) {
    if (!expression_is_compile_time_constant(ctx, node))
        return 1;

    ConstValue value;
    ConstValue converted;

    if (!eval_const_expr(ctx, node, &value))
        return 0;

    return coerce_constant_to_type(
        ctx,
        node,
        &value,
        target_type,
        integer_range_message,
        float_range_message,
        &converted
    );
}

static int check_binary_constant_operands(
    SemanticContext *ctx,
    Node *node,
    Type *left_type,
    Type *right_type,
    Type *operation_type,
    const char *integer_range_message,
    const char *float_range_message,
    Type **evaluated_type
) {
    if (expression_is_compile_time_constant(ctx, node)) {
        ConstValue value;

        if (!eval_const_expr(ctx, node, &value))
            return 0;

        if (evaluated_type && value.type)
            *evaluated_type = value.type;

        return 1;
    }

    Node *operands[2] = {
        node->as.binary.left,
        node->as.binary.right
    };

    Type *operand_types[2] = {
        left_type,
        right_type
    };

    for (int i = 0; i < 2; i++) {
        if (!is_untyped_numeric_type(operand_types[i]))
            continue;

        if (!check_constant_value_against_type(
                ctx,
                operands[i],
                operation_type,
                integer_range_message,
                float_range_message
            )) {
            return 0;
        }
    }

    return 1;
}

static int check_known_integer_zero_divisor(
    SemanticContext *ctx,
    TokenType operation,
    Node *divisor,
    Type *operation_type
) {
    if (!divisor || !operation_type)
        return 1;

    int is_division =
        operation == TOK_SLASH ||
        operation == TOK_SLASH_EQUAL;

    int is_remainder =
        operation == TOK_PERCENT ||
        operation == TOK_PERCENT_EQUAL;

    if (!is_division && !is_remainder)
        return 1;

    /*
     * Floating-point division by zero is a separate language-design
     * decision. This rule currently applies only to integer operations.
     */
    if (!is_integer_kind(operation_type->kind))
        return 1;

    if (!expression_is_compile_time_constant(ctx, divisor))
        return 1;

    ConstValue value;

    if (!eval_const_expr(ctx, divisor, &value)) {
        return 0;
    }

    if (value.kind != CONST_VALUE_INT ||
        value.as.integer.magnitude != 0) {
        return 1;
    }

    semantic_error(ctx, divisor,
        is_division ? "division by zero" : "remainder by zero");

    return 0;
}

static int check_known_shift_count(SemanticContext *ctx, Node *count_node, Type *left_type) {

    if (!count_node || !left_type) return 1;

    unsigned width;

    if (!integer_kind_bit_width(left_type->kind, &width)) {
        return 1;
    }

    /*
     * Unknown runtime counts are accepted by the frontend. A future
     * execution layer must enforce the same range rule dynamically.
     */
    if (!expression_is_compile_time_constant(ctx, count_node)) {
        return 1;
    }

    ConstValue count;

    if (!eval_const_expr(ctx, count_node, &count)) {
        return 0;
    }

    if (count.kind != CONST_VALUE_INT) {
        semantic_error(ctx, count_node,
            "shift count must be integer");

        return 0;
    }

    unsigned ignored_count;

    ShiftCountStatus status =
        classify_shift_count(count.as.integer, width, &ignored_count);

    if (status == SHIFT_COUNT_NEGATIVE) {
        semantic_error(ctx, count_node,
            "shift count cannot be negative");

        return 0;
    }

    if (status == SHIFT_COUNT_OUT_OF_RANGE) {
        semantic_error(ctx,count_node,
            "shift count must be less than left operand bit width");

        return 0;
    }

    return 1;
}

static int expression_is_compile_time_constant(SemanticContext *ctx, Node *node) {

    if (!node) return 0;

    switch (node->type) {
        case NODE_NUMBER:
        case NODE_BOOL:
        case NODE_NULL:
            return 1;

        case NODE_CAST:
            return expression_is_compile_time_constant(
                ctx,
                node->as.cast_expr.expression
            );

        case NODE_UNARY:
            if (node->as.unary.op != TOK_MINUS &&
                node->as.unary.op != TOK_BANG &&
                node->as.unary.op != TOK_TILDE) {
                return 0;
                }

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
                case TOK_AND:
                {
                    /* Address-of implementation */
                    if (!require_lvalue(
                            ctx,
                            node,
                            node->as.unary.operand,
                            "address-of operand is not assignable")) {
                        return NULL;
                    }

                    Type *pointer = new_type(ctx, TYPE_POINTER);

                    pointer->element = operand;

                    sem_record_expr_info(
                        ctx,
                        node,
                        pointer,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

                    return pointer;
                }

                case TOK_STAR:
                {
                    /*
                     * TYPE_NULL has no pointee type by itself. Although null can
                     * contextually adapt to a concrete pointer type, dereference
                     * provides no context from which to determine that type.
                     */
                    if (is_null_type(operand)) {
                        semantic_error(ctx, node,
                            "cannot dereference null");

                        return NULL;
                    }

                    if (operand->kind != TYPE_POINTER) {
                        semantic_error(ctx, node,
                            "unary '*' requires a pointer operand");

                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        operand->element,
                        NULL,
                        VALUE_CATEGORY_LVALUE
                    );

                    return operand->element;
                }

                case TOK_MINUS:
                {

                    /*
                     * Numeric negation is defined for signed integers,
                     * floating-point values, and untyped numeric literals.
                     */
                    if (!is_numeric_type(operand)) {
                        semantic_error(ctx, node,
                            "unary '-' requires numeric operand");

                        return NULL;
                    }

                    /*
                     * NOTE: Coglet does not currently define wrapping or modular unary
                     * negation for unsigned integers.
                     */
                    if (is_unsigned_integer_kind(operand->kind)) {
                        semantic_error(ctx, node,
                            "unary '-' cannot be applied to an unsigned value");

                        return NULL;
                    }

                    Type *result = operand;

                    if (expression_is_compile_time_constant(ctx, node)) {
                        ConstValue constant;

                        if (!eval_const_expr(ctx, node, &constant))
                            return NULL;

                        result = constant.type
                            ? constant.type
                            : const_value_default_type(
                                ctx,
                                &constant);
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        result,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

                    return result;
                }

                case TOK_TILDE:
                {
                    if (!is_integer_type(operand)) {
                        semantic_error(ctx, node,
                            "unary '~' requires integer operand");

                        return NULL;
                    }

                    Type *result = operand;

                    if (expression_is_compile_time_constant(ctx, node)) {

                        ConstValue constant;

                        if (!eval_const_expr(ctx, node, &constant)) {
                            return NULL;
                        }

                        result = constant.type
                            ? constant.type
                            : const_value_default_type(ctx, &constant);
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        result,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

                    return result;
                }

                case TOK_BANG:
                {
                    /* Logical-not implementation */
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
                        if (!is_untyped_numeric_type(left) &&
                            !is_untyped_numeric_type(right) &&
                            !type_equal(left, right)) {
                                semantic_error(ctx, node,
                                    "operands are different numeric types -- use an explicit cast");
                            } else {
                                semantic_error(ctx, node,
                                    "could not determine numeric result type");
                            }

                        return NULL;
                    }

                    if (!check_binary_constant_operands(
                            ctx,
                            node,
                            left,
                            right,
                            result,
                            "integer constant operand does not fit operation type",
                            "floating-point constant operand does not fit operation type",
                            &result)) {
                        return NULL;
                    }

                    if (!check_known_integer_zero_divisor(
                            ctx,
                            node->as.binary.op,
                            node->as.binary.right,
                            result)) {
                        return NULL;
                    }

                    sem_record_expr_info(ctx, node, result, NULL, VALUE_CATEGORY_RVALUE);
                    return result;
                }

                case TOK_AND:
                case TOK_OR:
                case TOK_XOR:
                {
                    if (!is_integer_type(left)) {
                        semantic_error(ctx, node,
                            "left operand of bitwise operator must be integer");

                        return NULL;
                    }

                    if (!is_integer_type(right)) {
                        semantic_error(ctx, node,
                            "right operand of bitwise operator must be integer");

                        return NULL;
                    }

                    Type *result =
                        common_integer_type(left, right);

                    if (!result) {
                        semantic_error(ctx, node,
                            "bitwise operands have incompatible integer types -- use an explicit cast");

                        return NULL;
                    }

                    if (!check_binary_constant_operands(
                            ctx,
                            node,
                            left,
                            right,
                            result,
                            "integer constant operand does not fit operation type",
                            "bitwise operators do not accept floating-point operands",
                            &result
                        )) {
                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        result,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

                    return result;
                }

                case TOK_SHIFT_LEFT:
                case TOK_SHIFT_RIGHT:
                {
                    if (!is_integer_type(left)) {
                        semantic_error(ctx, node,
                            "left operand of shift operator must be integer");

                        return NULL;
                    }

                    if (!is_integer_type(right)) {
                        semantic_error(ctx, node,
                            "right operand of shift operator must be integer");

                        return NULL;
                    }

                    /*
                     * Fully constant shifts are evaluated here. The evaluator selects
                     * the left operand's operation width and validates the count.
                     */
                    if (expression_is_compile_time_constant(ctx, node)) {
                        ConstValue constant;

                        if (!eval_const_expr(ctx, node, &constant)) {
                            return NULL;
                        }

                        Type *result = constant.type
                            ? constant.type
                            : const_value_default_type(
                                ctx,
                                &constant
                            );

                        sem_record_expr_info(
                            ctx,
                            node,
                            result,
                            NULL,
                            VALUE_CATEGORY_RVALUE
                        );

                        return result;
                    }

                    /*
                     * The result type is always determined by the left operand.
                     *
                     * A nonconstant expression such as `1 << runtime_count` cannot
                     * retain an untyped runtime result, so the left constant receives
                     * its ordinary concrete default type.
                     */
                    Type *result = left;

                    if (left->kind == TYPE_UNTYPED_INT) {
                        ConstValue left_constant;

                        if (!eval_const_expr(ctx, node->as.binary.left, &left_constant)) {
                            return NULL;
                        }

                        result = default_concrete_type_for_constant(ctx, &left_constant);
                    }

                    if (!result ||
                        !is_concrete_integer_kind(result->kind)) {
                        semantic_error(ctx, node,
                            "could not determine shift result type");

                        return NULL;
                    }

                    if (!check_known_shift_count(
                            ctx,
                            node->as.binary.right,
                            result)) {
                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        result,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

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
                    int left_is_null  = is_null_type(left);
                    int right_is_null = is_null_type(right);

                    /*
                     * TYPE_NULL equals TYPE_NULL structurally, but a comparison
                     * needs a concrete pointer type to provide context.
                     */
                    if (left_is_null && right_is_null) {
                        semantic_error(ctx, node,
                            "cannot compare null without a pointer type");

                        return NULL;
                    }

                    /*
                    * Exactly one null operand and one concrete pointer operand.
                    */
                    if (is_pointer_null_pair(left, right)) {
                        sem_record_expr_info(
                            ctx,
                            node,
                            ctx->type_bool,
                            NULL,
                            VALUE_CATEGORY_RVALUE
                        );

                        return ctx->type_bool;
                    }

                    /*
                     * Any other null combination is invalid, including null == 0.
                     */
                    if (left_is_null || right_is_null) {
                        semantic_error(ctx, node,
                            "null may only be compared with a pointer");

                        return NULL;
                    }

                    if (is_numeric_type(left) && is_numeric_type(right)) {
                        Type *common = common_numeric_type(left, right);

                        if (!common) {
                            semantic_error(ctx, node,
                                "comparison operands have incompatible numeric types");

                            return NULL;
                        }

                        if (!check_binary_constant_operands(
                                ctx,
                                node,
                                left,
                                right,
                                common,
                                "integer constant operand does not fit comparison type",
                                "floating-point constant operand does not fit comparison type",
                                NULL)) {
                            return NULL;
                        }

                        sem_record_expr_info(
                            ctx,
                            node,
                            ctx->type_bool,
                            NULL,
                            VALUE_CATEGORY_RVALUE
                        );

                        return ctx->type_bool;
                    }

                    /*
                    * Non-numeric operands must first have the same semantic type.
                    *
                    * This preserves diagnostics such as comparing two different enum
                    * types or pointers with different pointee types.
                    */
                    if (!type_equal(left, right)) {
                        semantic_error(ctx, node,
                            "comparison type mismatch");

                        return NULL;
                    }

                    /*
                     * Structural type equality does not imply that the language defines
                     * an equality operation for that type.
                     *
                     * Arrays, structs, and functions may be structurally equal types,
                     * but Coglet does not currently define value equality for them.
                     */
                    if (!is_equality_comparable_type(left) ||
                        !is_equality_comparable_type(right)) {
                        semantic_error(ctx, node,
                            "type does not support equality comparison");

                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        ctx->type_bool,
                        NULL,
                        VALUE_CATEGORY_RVALUE
                    );

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

                    if (!check_binary_constant_operands(
                            ctx,
                            node,
                            left,
                            right,
                            common,
                            "integer constant operand does not fit comparison type",
                            "floating-point constant operand does not fit comparison type",
                            NULL)) {
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

            if (!ok) return NULL;

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

            if (!type) return NULL;

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
                    sym->type->kind == TYPE_ENUM) {

                    EnumMember *member =
                        find_enum_member(
                            sym->type,
                            node->as.field.name.data,
                            node->as.field.name.length);

                    if (!member) {
                        semantic_error_name(
                            ctx,
                            node,
                            "unknown enum member",
                            node->as.field.name.data,
                            node->as.field.name.length);

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

            if (!object) return NULL;

            if (object->kind != TYPE_STRUCT) {
                semantic_error(ctx, node,
                    "field access requires a struct");

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

            if (object->kind == TYPE_POINTER ||
                (object_info && object_info->value_category == VALUE_CATEGORY_LVALUE)) {
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

            if (!object) return NULL;

            Type *index = check_value_expression(ctx, node->as.index.index);

            if (!index) return NULL;

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
                    ctx,node->as.index.index, &index_value) &&
                    index_value.kind == CONST_VALUE_INT) {
                    if (index_value.as.integer.is_negative ||
                        index_value.as.integer.magnitude >=
                            (uint64_t)object->array_size) {

                        semantic_error(ctx, node,
                            "array index out of bounds");
                    }
                }
            }

            Type *element_type = object->element;

            SemExprInfo *object_info =
                sem_find_expr_info(ctx, node->as.index.object);

            ValueCategory category = VALUE_CATEGORY_RVALUE;

            /*
             * Indexing a pointer denotes the storage to which the pointer
             * points. The pointer expression itself does not need to be a
             * lvalue.
             *
             * Indexing a fixed array inherits the array expression's value
             * category, so indexing a temporary array remains a rvalue.
             */
            if (object->kind == TYPE_POINTER ||
                (object_info &&
                 object_info->value_category ==
                     VALUE_CATEGORY_LVALUE)) {
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
            Type *type;

            if (node->as.number.kind == NUMBER_LITERAL_FLOAT) {
                type = new_type(ctx, TYPE_UNTYPED_FLOAT);
            } else {
                type = untyped_integer_type_for_value(
                    ctx,
                    integer_value_make(
                        node->as.number.value.integer,
                        0
                    )
                );
            }

            sem_record_expr_info(
                ctx,
                node,
                type,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return type;
        }

        case NODE_NULL:
        {
            sem_record_expr_info(
                ctx,
                node,
                ctx->type_null,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return ctx->type_null;
        }

        case NODE_STRING:
            semantic_error(ctx, node,
                "string literal requires an expected byte array type");

            return NULL;

        case NODE_CHAR:
        {
            sem_record_expr_info(
                ctx,
                node,
                ctx->type_u8,
                NULL,
                VALUE_CATEGORY_RVALUE
            );

            return ctx->type_u8;
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

    if (!eval_const_expr(ctx, node->as.const_decl.value, &value)) {
        return;
    }

    Type *value_type = check_value_expression(
        ctx,
        node->as.const_decl.value
    );

    if (!value_type)
        return;

    Type *type = node->as.const_decl.const_type;

    if (type) {
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

        if (!initializer_compatible(type, value_type)) {
            if (is_integer_zero_to_pointer(
                    type,
                    node->as.const_decl.value
                )) {
                semantic_error(ctx, node->as.const_decl.value,
                    "integer zero is not a pointer; use null");
                } else {
                    semantic_error(ctx, node,
                        "constant value does not match declared type");
                }

            return;
    }
        ConstValue converted;

        if (!coerce_constant_to_type(
                ctx,
                node->as.const_decl.value,
                &value,
                type,
                "constant integer value does not fit declared type",
                "constant floating-point value does not fit declared type",
                &converted
            )) {
            return;
        }

        value = converted;

    } else {
        if (value_type->kind == TYPE_NULL) {
            semantic_error(ctx, node->as.const_decl.value,
                "cannot infer a concrete pointer type from null");

            return;
        }

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

    Symbol *sym = arena_new(ctx->arena, Symbol);
    sym->name   = node->as.const_decl.name;
    sym->kind   = SYMBOL_CONSTANT;
    sym->type   = type;
    sym->const_value = value;

    sym->next = ctx->current_scope->symbols;
    ctx->current_scope->symbols = sym;
}

static void check_switch_statement(SemanticContext *ctx, Node *node) {

    Type *switch_type =
        check_value_expression(ctx, node->as.switch_stmt.expression);

    if (!switch_type)
        return;

    node->as.switch_stmt.resolved_type = switch_type;

    int switch_type_is_valid =
        is_switchable_type(switch_type);

    if (!switch_type_is_valid) {
        semantic_error(ctx, node,
            "switch expression must be integer, bool, or enum");
    }

    typedef struct SeenCase {
        ConstValue value;
    } SeenCase;

    int case_count =
        node->as.switch_stmt.cases.count;

    SeenCase *seen_cases =
        case_count > 0
        ? arena_alloc(
            ctx->arena,
            sizeof(SeenCase) * case_count
        )
        : NULL;

    int seen_case_count = 0;
    int seen_default = 0;

    for (int i = 0; i < case_count; i++) {
        Node *case_node =
            node->as.switch_stmt.cases.items[i];

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

        Node *case_value_node =
            case_node->as.switch_case.value;

        Type *case_type =
            check_value_expression(ctx, case_value_node);

        if (!switch_type_is_valid || !case_type) {
            check_node(ctx, case_node->as.switch_case.body);
            continue;
        }

        if (!initializer_compatible(switch_type, case_type)) {
            semantic_error(ctx, case_node,
                "switch case type does not match switch expression type");

            check_node(ctx, case_node->as.switch_case.body);

            continue;
        }

        ConstValue case_value;

        if (!eval_const_expr(
                ctx,
                case_value_node,
                &case_value
            )) {
            check_node(ctx, case_node->as.switch_case.body);

            continue;
        }

        ConstValue converted_case;

        if (!coerce_constant_to_type(
                ctx,
                case_value_node,
                &case_value,
                switch_type,
                "switch case value does not fit switch expression type",
                "switch case value does not fit switch expression type",
                &converted_case
            )) {
            check_node(ctx, case_node->as.switch_case.body);

            continue;
        }

        for (int j = 0; j < seen_case_count; j++) {
            if (const_values_equal(&seen_cases[j].value, &converted_case)) {
                semantic_error(ctx, case_node,
                    "duplicate switch case");

                break;
            }
        }

        seen_cases[seen_case_count].value = converted_case;

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

static int check_compound_assignment_statement(SemanticContext *ctx,Node *node) {

    Node *target_node =
        node->as.compound_assign.target;

    Node *value_node =
        node->as.compound_assign.value;

    TokenType operation =
        node->as.compound_assign.op;

    Type *target_type =
        check_expression(ctx, target_node);

    if (!target_type) return 0;

    if (!require_lvalue(
            ctx,
            node,
            target_node,
            "compound assignment target is not assignable"
        )) {
        return 0;
    }

    Type *value_type =
        check_value_expression(ctx, value_node);

    if (!value_type) return 0;

    switch (operation) {
        /*
         * Arithmetic compound assignments preserve their existing
         * numeric compatibility and constant-range rules.
         */
        case TOK_PLUS_EQUAL:
        case TOK_MINUS_EQUAL:
        case TOK_STAR_EQUAL:
        case TOK_SLASH_EQUAL:
        case TOK_PERCENT_EQUAL:
        {
            if (!is_numeric_type(target_type)) {
                semantic_error(ctx, node,
                    "compound assignment target must be numeric");

                return 0;
            }

            if (!is_numeric_type(value_type)) {
                semantic_error(ctx, node,
                    "compound assignment value must be numeric");

                return 0;
            }

            if (operation == TOK_PERCENT_EQUAL &&
                (!is_integer_type(target_type) ||
                 !is_integer_type(value_type))) {
                semantic_error(ctx, node,
                    "modulo compound assignment operands must be integers");

                return 0;
            }

            Type *result_type =
                common_numeric_type(target_type, value_type);

            if (!result_type) {
                semantic_error(ctx, node,
                    "compound assignment operands have incompatible numeric types");

                return 0;
            }

            if (!initializer_compatible(target_type, result_type)) {
                semantic_error(ctx, node,
                    "compound assignment result does not fit target type");

                return 0;
            }

            if (is_untyped_numeric_type(value_type) &&
                !check_constant_value_against_type(
                    ctx,
                    value_node,
                    target_type,
                    "integer constant operand does not fit compound assignment type",
                    "floating-point constant operand does not fit compound assignment type"
                )) {
                return 0;
            }

            if (!check_known_integer_zero_divisor(
                    ctx,
                    operation,
                    value_node,
                    result_type
                )) {
                return 0;
            }

            break;
        }

        /*
         * Bitwise compound assignments use the target type as the
         * operation type.
         *
         * Concrete integer operands must match exactly. An untyped
         * integer constant may adapt when its exact value fits the
         * target type.
         */
        case TOK_AND_EQUAL:
        case TOK_OR_EQUAL:
        case TOK_XOR_EQUAL:
        {
            if (!is_integer_type(target_type)) {
                semantic_error(ctx, node,
                    "bitwise compound assignment target must be integer");

                return 0;
            }

            if (!is_integer_type(value_type)) {
                semantic_error(ctx, node,
                    "bitwise compound assignment value must be integer");

                return 0;
            }

            Type *result_type =
                common_integer_type(target_type, value_type);

            if (!result_type) {
                semantic_error(ctx, node,
                    "bitwise compound assignment operands have incompatible integer types -- use an explicit cast");

                return 0;
            }

            if (value_type->kind == TYPE_UNTYPED_INT &&
                !check_constant_value_against_type(
                    ctx,
                    value_node,
                    target_type,
                    "integer constant operand does not fit compound assignment type",
                    "bitwise compound assignment does not accept floating-point constants"
                )) {
                return 0;
            }

            break;
        }

        /*
         * Shift compound assignments use the target's width and
         * signedness. The count may have any integer type.
         */
        case TOK_SHIFT_LEFT_EQUAL:
        case TOK_SHIFT_RIGHT_EQUAL:
        {
            if (!is_integer_type(target_type)) {
                semantic_error(ctx, node,
                    "shift compound assignment target must be integer");

                return 0;
            }

            if (!is_integer_type(value_type)) {
                semantic_error(ctx, node,
                    "shift compound assignment count must be integer");

                return 0;
            }

            if (!check_known_shift_count(ctx, value_node, target_type)) {
                return 0;
            }

            break;
        }

        default:
            semantic_error(ctx,node,
                "unsupported compound assignment operator");

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
        semantic_error(ctx, node,
            "increment/decrement requires a numeric target");
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

    if (!type) return NULL;

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

    if (!initializer_compatible(expected, actual)) {
        if (is_integer_zero_to_pointer(expected, initializer)) {
            semantic_error(ctx, initializer,
                "integer zero is not a pointer; use null");
            } else {
                semantic_error(ctx, initializer,
                    "initializer type does not match declared type");
            }

        return 0;
    }

    if (!check_constant_value_against_type(
        ctx,
        initializer,
        expected,
        "integer constant does not fit destination type",
        "floating-point constant does not fit destination type"
    )) {
        return 0;
    }

    return 1;
}

static int check_argument_against_parameter(SemanticContext *ctx, Type *expected, Node *argument) {

    if (!expected || !argument) return 0;

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

    if (!actual) return 0;

    if (!initializer_compatible(expected, actual)) {
        if (is_integer_zero_to_pointer(expected, argument)) {
            semantic_error(ctx, argument,
                "integer zero is not a pointer; use null");

            return 0;
        }

        const int name_buffer_size = 128;
        char expected_name[name_buffer_size];
        char actual_name[name_buffer_size];

        format_type_name(
            expected,
            expected_name,
            sizeof(expected_name)
        );

        format_type_name(
            actual,
            actual_name,
            sizeof(actual_name)
        );

        semantic_error_fmt(
            ctx,
            argument,
            "argument type does not match parameter type: expected %s, got %s",
            expected_name,
            actual_name
        );

        return 0;
    }

    if (!check_constant_value_against_type(
        ctx,
        argument,
        expected,
        "integer argument does not fit parameter type",
        "floating-point argument does not fit parameter type")) {

        return 0;
    }

    return 1;
}

static int check_array_initializer(SemanticContext *ctx, Type *expected, Node *initializer) {

    if (!expected || !initializer)
        return 0;

    if (initializer->type != NODE_ARRAY_LITERAL) {
        semantic_error(ctx, initializer,
            "internal error: expected array literal");
        return 0;
    }

    if (expected->kind != TYPE_ARRAY) {
        semantic_error(ctx, initializer,
            "array literal can only initialize an array type");
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

static void check_var_decl(SemanticContext *ctx, Node *node) {

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

            type = concretize_inferred_type(
                ctx,
                init,
                init_type
            );

            if (!type)
                return;
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

static void check_param_decl(SemanticContext *ctx, Node *node) {
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
            semantic_error(ctx, node,
                "parameter cannot have type void");

            return;
        }
    }

    Node *default_value = node->as.param_decl.default_value;

    if (default_value) {
        if (type) {

            if (!check_initializer_against_type(ctx,type, default_value)) {
                return;
            }

        } else {
            Type *default_type =
                check_value_expression(
                    ctx,
                    default_value
                );

            if (!default_type)
                return;

            type = concretize_inferred_type(
                ctx,
                default_value,
                default_type
            );

            if (!type)
                return;
        }
    }

    if (!type) {
        semantic_error(ctx, node,
            "could not determine parameter type");

        return;
    }

    if (invalid_value_type(type)) {
        semantic_error(ctx, node,
            "parameter cannot have type void");

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

        if (!param_type) return NULL;

        if (contains_void_type(param_type)) {

            semantic_error(ctx, param,
                "parameter cannot have type void");

            return NULL;
        }

        type->parameters[i] = param_type;
    }

    type->return_type = resolve_type(ctx, func->as.func_decl.return_type, func);

    if (!type->return_type) return NULL;


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

    Symbol *sym =
        scope_find_local(ctx->current_scope, node->as.struct_decl.name.data, node->as.struct_decl.name.length);

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

static EnumMember *find_enum_member(Type *enum_type, const char *name, size_t length) {

    if (!enum_type || enum_type->kind != TYPE_ENUM) return NULL;

    for (int i = 0; i < enum_type->enum_member_count; i++) {

        EnumMember *member = &enum_type->enum_members[i];

        if (names_equal(member->name.data, member->name.length, name, length))
            return member;
    }

    return NULL;
}

static EnumMember *find_enum_member_by_value(Type *enum_type, IntegerValue value) {

    if (!enum_type || enum_type->kind != TYPE_ENUM)
        return NULL;

    for (int i = 0;
         i < enum_type->enum_member_count;
         i++) {
        EnumMember *member =
            &enum_type->enum_members[i];

        if (integer_values_equal(
                member->value,
                value
            )) {
            return member;
            }
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

        // Default enum backing type.
        backing_type = ctx->type_i32;
    }

    if (!is_integer_kind(backing_type->kind)) {
        semantic_error(ctx, node,
            "enum backing type must be an integer type");

        return;
    }

    type->enum_backing_type = backing_type;
    assert_canonical_builtin_type(ctx, backing_type);

    int count = node->as.enum_decl.members.count;

    type->enum_members = count
        ? arena_alloc(
            ctx->arena,
            sizeof(EnumMember) * count
        )
        : NULL;

    type->enum_member_count = count;

    IntegerValue next_value = integer_value_make(0, 0);
    int next_value_available = 1;

    for (int i = 0; i < count; i++) {

        Node *member_node = node->as.enum_decl.members.items[i];

        StringView member_name = member_node->as.enum_member.name;

        int duplicate = 0;

        for (int j = 0; j < i; j++) {
            if (names_equal(
                    type->enum_members[j].name.data,
                    type->enum_members[j].name.length,
                    member_name.data,
                    member_name.length
                )) {
                semantic_error_name(
                    ctx,
                    member_node,
                    "duplicate enum member",
                    member_name.data,
                    member_name.length
                );

                duplicate = 1;
                break;
            }
        }

        IntegerValue value = next_value;
        int value_is_valid = next_value_available;

        if (member_node->as.enum_member.value) {

            Node *value_node = member_node->as.enum_member.value;
            ConstValue constant;

            if (!eval_const_expr(ctx, value_node, &constant)) {

                value_is_valid = 0;

            } else if (constant.kind != CONST_VALUE_INT) {

                semantic_error(ctx, member_node,
                    "enum member value must be an integer constant");

                value_is_valid = 0;
            } else {
                value = constant.as.integer;

                /*
                 * Populate the normal expression side table after
                 * successful constant evaluation.
                 */
                check_value_expression(ctx, value_node);
            }

        } else if (!next_value_available) {

            semantic_error(ctx, member_node,
                "implicit enum member value exceeds u64 range");

            value_is_valid = 0;
        }

        if (value_is_valid && !integer_value_fits_type(value, backing_type->kind)) {

            semantic_error(ctx, member_node,
                "enum member value does not fit in backing type");

            value_is_valid = 0;
        }

        type->enum_members[i].name = member_name;
        type->enum_members[i].value = value;
        member_node->as.enum_member.resolved_value = value;

        if (!duplicate && value_is_valid) {
            IntegerValue one =
                integer_value_make(1, 0);

            next_value_available =
                integer_value_add(
                    value,
                    one,
                    &next_value);
        }
    }
}

static Type *check_cast_expression(SemanticContext *ctx, Node *node) {

    Type *target_type = resolve_type(
        ctx,
        node->as.cast_expr.target_type,
        node
    );

    if (!target_type) {
        semantic_error(ctx, node,
            "could not resolve cast target type");

        return NULL;
    }

    if (invalid_value_type(target_type)) {
        semantic_error(ctx, node,
            "cannot cast to void");

        return NULL;
    }

    Node *source_expression = node->as.cast_expr.expression;

    Type *source_type = check_value_expression(
        ctx,
        source_expression
    );

    if (!source_type) return NULL;

    if (!is_allowed_explicit_cast(
            target_type,
            source_type
        )) {
        semantic_error(ctx, node,
            "invalid explicit cast");

        return NULL;
    }

    int source_is_constant =
        expression_is_compile_time_constant(
            ctx,
            source_expression
        );

    /*
     * Closed enums may only be constructed from declared member values.
     *
     * For a compile-time integer, eval_const_cast() can prove that:
     *
     *   1. the integer fits the enum backing type;
     *   2. the integer equals a declared enum member value.
     *
     * For a runtime integer, the compiler cannot prove membership without
     * emitting a runtime check. Runtime checked enum conversion is not yet
     * implemented, so reject it for now.
     */
    if (is_integer_to_enum_cast(
            target_type,
            source_type
        )) {
        if (!source_is_constant) {
            semantic_error(ctx, node,
                "runtime integer-to-enum cast is not supported");

            return NULL;
        }

        ConstValue ignored;

        if (!eval_const_cast(ctx, node, &ignored)) {
            return NULL;
        }

        return target_type;
    }

    /*
     * Every other compile-time-known cast must still satisfy its
     * representability rules, even when the result is discarded.
     *
     * Examples checked here:
     *
     *   cast(u8, 256)
     *   cast(i8, -129)
     *   cast(f32, very_large_value)
     *   cast(u16, SomeEnum.Member)
     *
     * Constant declarations also reach eval_const_cast() through
     * eval_const_expr(), but ordinary expression statements and nested
     * expression contexts need the same validation.
     */
    if (source_is_constant) {
        ConstValue ignored;

        if (!eval_const_cast(ctx, node, &ignored)) {
            return NULL;
        }
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

            if (integer_values_equal(case_member->value, member->value)) {
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
            if (!is_literal_true(node->as.while_stmt.condition))
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
                semantic_error(ctx, node,
                    "break statement not inside loop");
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
                    semantic_error(ctx, node->as.for_stmt.condition,
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
                semantic_error(ctx, node,
                    "return outside function");
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

    ctx->type_i8  = new_type(ctx, TYPE_I8);
    ctx->type_i16 = new_type(ctx, TYPE_I16);
    ctx->type_i32 = new_type(ctx, TYPE_I32);
    ctx->type_i64 = new_type(ctx, TYPE_I64);

    ctx->type_u8  = new_type(ctx, TYPE_U8);
    ctx->type_u16 = new_type(ctx, TYPE_U16);
    ctx->type_u32 = new_type(ctx, TYPE_U32);
    ctx->type_u64 = new_type(ctx, TYPE_U64);

    ctx->type_f32 = new_type(ctx, TYPE_F32);
    ctx->type_f64 = new_type(ctx, TYPE_F64);

    ctx->type_bool = new_type(ctx, TYPE_BOOL);
    ctx->type_void = new_type(ctx, TYPE_VOID);
    ctx->type_null = new_type(ctx, TYPE_NULL);

    ctx->current_scope = scope_new(ctx, NULL);
    ctx->expr_infos = NULL;

    check_node(ctx,  program);
}

SemExprInfo *semantic_get_expr_info(SemanticContext *ctx, Node *node) {
    return sem_find_expr_info(ctx, node);
}
