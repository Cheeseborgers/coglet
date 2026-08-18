// src/semantic_anal.c
#include "semantic_anal.h"

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>

#include "string_decode.h"
#include "utils/constant_values.h"
#include "utils/utils.h"

struct LoopFlowContext {
    size_t active_variable_count;

    FlowState break_flow;
    int has_break_flow;

    FlowState continue_flow;
    int has_continue_flow;

    LoopFlowContext *parent;
};

typedef enum GenericSpecializationState {
    GENERIC_SPECIALIZATION_CHECKING,
    GENERIC_SPECIALIZATION_VALID,
    GENERIC_SPECIALIZATION_INVALID,
} GenericSpecializationState;

struct SliceTypeIntern {
    Type *type;
    SliceTypeIntern *next;
};

static int type_equal(const Type *a, const Type *b);
static Type *resolve_generic_struct_application(
    SemanticContext *ctx,
    Symbol *symbol,
    Type *const *source_arguments,
    int source_argument_count,
    Node *use_node
);
static GenericStructSpecialization *instantiate_generic_struct(
    SemanticContext *ctx,
    Symbol *template_symbol,
    Type *const *type_arguments,
    int type_argument_count,
    Node *use_node
);
static StructMethodBinding *find_struct_method_binding(
    SemanticContext *ctx,
    Type *owner_type,
    StringView name
);
static StructOperatorBinding *find_struct_operator_binding(
    SemanticContext *ctx,
    Type *owner_type,
    TokenType op,
    int is_unary
);
static int register_struct_method_signatures(SemanticContext *ctx, Node *owner_decl);
static int register_struct_operator_bindings(SemanticContext *ctx, Node *owner_decl);
static void check_struct_method_bodies(SemanticContext *ctx, Node *owner_decl);

static const char *source_operator_spelling(TokenType op)
{
    switch (op) {
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        default: return "<operator>";
    }
}

struct GenericSpecialization {
    SemDeclId template_id;
    Node *template_decl;
    Node *function;
    Symbol *symbol;
    Type *function_type;
    Type **type_arguments;
    int type_argument_count;
    GenericSpecializationState state;

    /* Active-instantiation chain, distinct from the cache list. */
    GenericSpecialization *active_parent;
    GenericSpecialization *next;
};

struct GenericStructSpecialization {
    SemDeclId template_id;
    Node *template_decl;
    Node *declaration;
    Type *type;
    Type **type_arguments;
    int type_argument_count;
    GenericSpecializationState state;

    GenericStructSpecialization *active_parent;
    GenericStructSpecialization *next;
};

struct StructMethodBinding {
    Type *owner_type;
    Node *owner_decl;
    StringView source_name;
    Node *function;
    Symbol *symbol;
    Type *function_type;
    int is_instance;
    StructMethodBinding *next;
};

struct StructOperatorBinding {
    Type *owner_type;
    TokenType op;
    int is_unary;
    StructMethodBinding *method;
    StructOperatorBinding *next;
};

static Type *new_type(SemanticContext *ctx, TypeKind kind)
{
    Type *type = arena_new(ctx->arena, Type);

    type->kind                = kind;
    type->pointer_access      = POINTER_ACCESS_MUTABLE;
    type->pointer_is_volatile = 0;
    type->array_size          = -1;
    type->struct_generic_template_id = (size_t)-1;

    return type;
}

static Type *intern_slice_type(
    SemanticContext *ctx,
    Type *element,
    PointerAccess access
) {
    if (!ctx || !element)
        return NULL;

    for (SliceTypeIntern *it = ctx->slice_types; it; it = it->next) {
        if (it->type && it->type->pointer_access == access &&
            type_equal(it->type->element, element)) {
            return it->type;
        }
    }

    Type *type = new_type(ctx, TYPE_SLICE);
    type->element = element;
    type->pointer_access = access;

    SliceTypeIntern *entry = arena_new(ctx->arena, SliceTypeIntern);
    entry->type = type;
    entry->next = ctx->slice_types;
    ctx->slice_types = entry;
    return type;
}

static Type *builtin_type(SemanticContext *ctx, TypeKind kind) {

    switch (kind) {
        case TYPE_S8:  return ctx->type_s8;
        case TYPE_S16: return ctx->type_s16;
        case TYPE_S32: return ctx->type_s32;
        case TYPE_S64: return ctx->type_s64;

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
    if (!type) return;

    Type *canonical = builtin_type(ctx, type->kind);

    assert(!canonical || type == canonical);
#else
    (void)ctx;
    (void)type;
#endif
}

static void semantic_error_fmt(SemanticContext *ctx, const Node *node, const char *fmt, ...)
{
    assert(ctx);
    assert(node);

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);

    int required = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (required < 0) {
        va_end(args);
        diagnostic_add(
            &ctx->diagnostics,
            DIAGNOSTIC_ERROR,
            DIAGNOSTIC_PHASE_SEMANTIC,
            node->span,
            "semantic diagnostic formatting failed"
        );
    } else {
        char *message = arena_alloc(ctx->arena, (size_t)required + 1);
        vsnprintf(message, (size_t)required + 1, fmt, args);
        va_end(args);

        diagnostic_add(
            &ctx->diagnostics,
            DIAGNOSTIC_ERROR,
            DIAGNOSTIC_PHASE_SEMANTIC,
            node->span,
            message
        );
    }

    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error(SemanticContext *ctx, const Node *node, const char *msg) {
    assert(ctx);
    assert(node);

    diagnostic_add(
        &ctx->diagnostics,
        DIAGNOSTIC_ERROR,
        DIAGNOSTIC_PHASE_SEMANTIC,
        node->span,
        msg
    );

    ctx->had_error = 1;
    ctx->error_count++;
}

static void semantic_error_name(
    SemanticContext *ctx, const Node *node, const char *prefix, const char *name, size_t length) {
    assert(ctx);
    assert(node);

    diagnostic_add_fmt(
        &ctx->diagnostics,
        DIAGNOSTIC_ERROR,
        DIAGNOSTIC_PHASE_SEMANTIC,
        node->span,
        "%s '%.*s'",
        prefix,
        (int)length,
        name
    );

    ctx->had_error = 1;
    ctx->error_count++;
}

static SemDeclInfo *sem_find_decl_info(SemanticContext *ctx, Node *node) {

    for (SemDeclInfo *info = ctx->decl_infos; info; info = info->next) {
        if (info->node == node)
            return info;
    }

    return NULL;
}

static SemDeclInfo *sem_find_decl_info_by_id(SemanticContext *ctx, SemDeclId id) {

    if (id == INVALID_SEM_DECL_ID)
        return NULL;

    for (SemDeclInfo *info = ctx->decl_infos; info; info = info->next) {
        if (info->id == id)
            return info;
    }

    return NULL;
}

static SemDeclInfo *sem_get_or_create_decl_info(SemanticContext *ctx, Node *node) {

    assert(node);

    SemDeclInfo *info = sem_find_decl_info(ctx, node);
    if (info) return info;

    assert(ctx->next_declaration_id != INVALID_SEM_DECL_ID);

    info = arena_alloc(ctx->arena, sizeof(*info));
    memset(info, 0, sizeof(*info));

    info->id = ctx->next_declaration_id++;
    info->node = node;
    info->is_exported = node->is_exported;
    info->next = ctx->decl_infos;
    ctx->decl_infos = info;

    return info;
}

static SemDeclInfo *sem_record_decl_info(
    SemanticContext *ctx, Node *node, Type *type, Symbol *symbol) {

    assert(node);
    assert(type);
    assert_canonical_builtin_type(ctx, type);

    SemDeclInfo *info = sem_get_or_create_decl_info(ctx, node);

    if (info->type)
        assert(info->type == type);

    if (info->symbol && symbol)
        assert(info->symbol == symbol);

    info->type = type;

    if (symbol) {
        info->symbol = symbol;

        if (symbol->declaration)
            assert(symbol->declaration == node);

        if (symbol->declaration_id != INVALID_SEM_DECL_ID)
            assert(symbol->declaration_id == info->id);

        symbol->declaration = node;
        symbol->declaration_id = info->id;
    }

    return info;
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
    info->contextual_type = NULL;
    info->contextual_conversion = SEM_CONTEXT_CONVERSION_NONE;
    info->value_category = VALUE_CATEGORY_NONE;
    info->value_access   = VALUE_ACCESS_NONE;
    info->value_is_volatile = 0;
    info->resolved_operator_function_id = INVALID_SEM_DECL_ID;
    info->has_constant_value = 0;

    info->next = ctx->expr_infos;
    ctx->expr_infos = info;

    return info;
}

static void sem_record_context_conversion(
    SemanticContext *ctx,
    Node *node,
    Type *target_type,
    SemContextConversionKind conversion
) {
    assert(ctx);
    assert(node);
    assert(target_type);
    assert(conversion != SEM_CONTEXT_CONVERSION_NONE);

    SemExprInfo *info = sem_find_expr_info(ctx, node);
    assert(info);
    assert(info->type);

    /*
     * One AST expression has exactly one parent/use-site in a valid parsed
     * program. Re-recording is allowed only when a checker reaches the same
     * semantic decision through a shared helper.
     */
    if (info->contextual_conversion != SEM_CONTEXT_CONVERSION_NONE) {
        assert(info->contextual_conversion == conversion);
        assert(info->contextual_type == target_type);
        return;
    }

    info->contextual_type = target_type;
    info->contextual_conversion = conversion;
}

static void sem_record_expr_info(
    SemanticContext *ctx, Node *node, Type *type, Symbol *symbol, ValueCategory category) {

    if (!node) return;

    assert_canonical_builtin_type(ctx, type);

    SemExprInfo *info = sem_get_or_create_expr_info(ctx, node);

    info->type = type;
    info->symbol = symbol;
    info->value_category = category;

    switch (category) {
        case VALUE_CATEGORY_NONE:
        case VALUE_CATEGORY_RVALUE:
            info->value_access =
                VALUE_ACCESS_NONE;
            info->value_is_volatile = 0;
            break;

        case VALUE_CATEGORY_LVALUE:
            /*
             * Existing behaviour remains unchanged during Stage 4A.
             * Readonly lvalues will be recorded explicitly in Stage 4B.
             */
            info->value_access =
                VALUE_ACCESS_WRITABLE;
            break;
    }
}

static void sem_record_lvalue_info_qualified(
    SemanticContext *ctx, Node *node, Type *type, Symbol *symbol,
    ValueAccess access, int is_volatile) {

    assert(access == VALUE_ACCESS_READONLY || access == VALUE_ACCESS_WRITABLE);

    sem_record_expr_info(
        ctx,
        node,
        type,
        symbol,
        VALUE_CATEGORY_LVALUE
    );

    SemExprInfo *info =
        sem_find_expr_info(ctx, node);

    assert(info);
    info->value_access = access;
    info->value_is_volatile = !!is_volatile;
}

/*
 * Records that a successfully checked node does not produce a value.
 *
 * This is used for statement-only expressions such as assignment,
 * compound assignment, increment, and decrement.
 */
static void sem_record_no_value(SemanticContext *ctx, Node *node) {
    sem_record_expr_info(ctx, node, NULL, NULL, VALUE_CATEGORY_NONE);
}

static int expression_is_lvalue(
    SemanticContext *ctx,
    Node *node
) {
    SemExprInfo *info =
        sem_find_expr_info(ctx, node);

    return info &&
           info->value_category ==
               VALUE_CATEGORY_LVALUE;
}

static int require_lvalue(
    SemanticContext *ctx,
    Node *owner,
    Node *target,
    const char *message
) {
    if (!expression_is_lvalue(ctx, target)) {
        semantic_error(ctx, owner, message);
        return 0;
    }

    return 1;
}

static int require_writable_lvalue(
    SemanticContext *ctx,
    Node *owner,
    Node *target,
    const char *description
) {
    SemExprInfo *info =
        sem_find_expr_info(ctx, target);

    if (!info ||
        info->value_category !=
            VALUE_CATEGORY_LVALUE) {
        semantic_error_fmt(
            ctx,
            owner,
            "%s is not assignable",
            description
        );

        return 0;
    }

    switch (info->value_access) {
        case VALUE_ACCESS_WRITABLE:
            return 1;

        case VALUE_ACCESS_READONLY:
            semantic_error_fmt(
                ctx,
                owner,
                "%s is readonly",
                description
            );

            return 0;

        case VALUE_ACCESS_NONE:
            UNREACHABLE(
                "lvalue has no storage access"
            );
    }

    UNREACHABLE("ValueAccess");
}

static ValueAccess value_access_from_pointer_access(
    PointerAccess access
) {
    switch (access) {
        case POINTER_ACCESS_MUTABLE:
            return VALUE_ACCESS_WRITABLE;

        case POINTER_ACCESS_READONLY:
            return VALUE_ACCESS_READONLY;
    }

    UNREACHABLE("PointerAccess");
}

static PointerAccess pointer_access_from_value_access(
    ValueAccess access
) {
    switch (access) {
        case VALUE_ACCESS_WRITABLE:
            return POINTER_ACCESS_MUTABLE;

        case VALUE_ACCESS_READONLY:
            return POINTER_ACCESS_READONLY;

        case VALUE_ACCESS_NONE:
            UNREACHABLE(
                "non-lvalue has no pointer access"
            );
    }

    UNREACHABLE("ValueAccess");
}

// ============================================================
// definite-assignment flow state
// ============================================================

static void flow_init(FlowState *state, size_t owner_id) {
    *state = (FlowState){
        .owner_id  = owner_id,
        .reachable = 1,
    };
}

static void flow_reserve(SemanticContext *ctx, FlowState *state, size_t minimum_capacity) {

    if (state->capacity >= minimum_capacity)
        return;

    size_t new_capacity = state->capacity ? state->capacity : 8;

    while (new_capacity < minimum_capacity) {

        if (new_capacity > ((size_t)-1) / 2) {
            new_capacity = minimum_capacity;
            break;
        }

        new_capacity *= 2;
    }

    unsigned char *initialized = arena_zalloc(ctx->arena, new_capacity);

    if (state->initialized) {
        memcpy(initialized, state->initialized, state->count);
    }

    state->initialized = initialized;
    state->capacity    = new_capacity;
}

static void flow_truncate_to(FlowState *state, size_t count) {

    assert(count <= state->count);

    if (count == state->count)
        return;

    memset(state->initialized + count, 0, state->count - count);

    state->count = count;
}

static int symbol_has_flow_state(const Symbol *symbol) {

    if (!symbol)
        return 0;

    if (symbol->kind != SYMBOL_VARIABLE)
        return 0;

    switch (symbol->variable_storage) {
        case VARIABLE_STORAGE_LOCAL:
        case VARIABLE_STORAGE_PARAMETER:
            assert(symbol->flow_owner_id != INVALID_FLOW_OWNER_ID);
            assert(symbol->variable_id != INVALID_VARIABLE_ID);

            return 1;

        case VARIABLE_STORAGE_GLOBAL:
        case VARIABLE_STORAGE_NONE:
            return 0;
    }

    assert(0 && "unhandled variable storage classification");

    return 0;
}

static int symbol_belongs_to_flow(const Symbol *symbol, const FlowState *state) {

    assert(symbol);
    assert(state);
    assert(symbol_has_flow_state(symbol));

    return symbol->flow_owner_id == state->owner_id;
}

/*
 * Records the initial path-dependent state of a newly declared
 * local variable or parameter.
 */
static void flow_register_variable(SemanticContext *ctx, const Symbol *symbol, int initialized) {

    assert(symbol_has_flow_state(symbol));
    assert(symbol_belongs_to_flow(symbol, &ctx->flow));

    FlowState *state      = &ctx->flow;
    size_t variable_id    = symbol->variable_id;
    size_t required_count = variable_id + 1;

    flow_reserve(ctx, state, required_count);

    state->initialized[variable_id] =
        initialized ? 1 : 0;

    if (state->count < required_count)
        state->count = required_count;
}

static void flow_mark_variable_initialized(SemanticContext *ctx, const Symbol *symbol) {

    /*
     * Only parameters and function-local variables have
     * flow-state slots.
     */
    if (!symbol_has_flow_state(symbol))
        return;

    assert(symbol_belongs_to_flow(symbol, &ctx->flow));
    assert(symbol->variable_id < ctx->flow.count);

    ctx->flow.initialized[symbol->variable_id] = 1;
}

static int flow_variable_is_initialized(const SemanticContext *ctx, const Symbol *symbol) {

    if (!symbol_has_flow_state(symbol))
        return 1;

    assert(symbol_belongs_to_flow(symbol, &ctx->flow));
    assert(symbol->variable_id < ctx->flow.count);

    return ctx->flow.initialized[symbol->variable_id] != 0;
}

static FlowState flow_clone(SemanticContext *ctx, const FlowState *source) {

    FlowState clone;

    flow_init(&clone, source->owner_id);
    flow_reserve(ctx, &clone, source->count);

    if (source->count > 0)
        memcpy(clone.initialized, source->initialized, source->count);

    clone.count = source->count;
    clone.reachable = source->reachable;

    return clone;
}

static FlowState flow_merge_continuing_paths(
    SemanticContext *ctx,
    const FlowState *left,
    const FlowState *right,
    size_t active_variable_count) {

    assert(left);
    assert(right);
    assert(left->owner_id == right->owner_id);
    assert(active_variable_count <= left->count);
    assert(active_variable_count <= right->count);

    if (!left->reachable && !right->reachable) {

        FlowState result = flow_clone(ctx, left);
        flow_truncate_to(&result, active_variable_count);

        result.reachable = 0;

        return result;
    }

    if (left->reachable && !right->reachable) {

        FlowState result = flow_clone(ctx, left);
        flow_truncate_to(&result, active_variable_count);

        return result;
    }

    if (!left->reachable && right->reachable) {

        FlowState result = flow_clone(ctx, right);
        flow_truncate_to(&result, active_variable_count);

        return result;
    }

    FlowState result = flow_clone(ctx, left);
    flow_truncate_to(&result, active_variable_count);

    for (size_t i = 0; i < active_variable_count; i++) {
        result.initialized[i] = left->initialized[i] && right->initialized[i];
    }

    result.reachable = 1;

    return result;
}

static void flow_accumulate_reachable_path(
    SemanticContext *ctx,
    FlowState *accumulator,
    int *has_accumulator,
    const FlowState *path,
    size_t active_variable_count
) {
    assert(accumulator);
    assert(has_accumulator);
    assert(path);
    assert(active_variable_count <= path->count);

    if (*has_accumulator)
        assert(accumulator->owner_id == path->owner_id);

    if (!path->reachable)
        return;

    if (!*has_accumulator) {
        *accumulator = flow_clone(ctx, path);

        flow_truncate_to(accumulator, active_variable_count);

        *has_accumulator = 1;
        return;
    }

    *accumulator =
        flow_merge_continuing_paths(
            ctx,
            accumulator,
            path,
            active_variable_count
        );
}

static void loop_record_break(SemanticContext *ctx) {
    LoopFlowContext *loop =
        ctx->current_loop;

    assert(loop);

    flow_accumulate_reachable_path(
        ctx,
        &loop->break_flow,
        &loop->has_break_flow,
        &ctx->flow,
        loop->active_variable_count
    );

    ctx->flow.reachable = 0;
}

static void loop_record_continue(SemanticContext *ctx) {

    LoopFlowContext *loop = ctx->current_loop;

    assert(loop);

    flow_accumulate_reachable_path(
        ctx,
        &loop->continue_flow,
        &loop->has_continue_flow,
        &ctx->flow,
        loop->active_variable_count
    );

    ctx->flow.reachable = 0;
}

static FlowState loop_iteration_flow(
    SemanticContext *ctx, const LoopFlowContext *loop, const FlowState *body_flow) {

    FlowState result = flow_clone(ctx, body_flow);

    flow_truncate_to(&result, loop->active_variable_count);

    if (loop->has_continue_flow) {
        result =
            flow_merge_continuing_paths(
                ctx,
                &result,
                &loop->continue_flow,
                loop->active_variable_count
            );
    }

    return result;
}

static FlowState loop_conservative_exit_flow(
    SemanticContext *ctx, const LoopFlowContext *loop, const FlowState *incoming) {

    /*
    * Coglet does not currently perform loop fixed-point analysis.
     *
    * For a loop that may terminate normally, preserve the unchanged
    * incoming state as a conservative possible exit. Initialization
    * performed only during an iteration therefore cannot become
    * definitely initialized after the loop.
    *
    * Literal-true loops with no reachable break are handled separately
    * and never reach this helper as continuing control flow.
    */
    FlowState result = flow_clone(ctx, incoming);

    /*
    * Reachable break paths are additional possible exits. Merge them
    * with the conservative incoming path.
    */
    if (loop->has_break_flow) {
        result =
            flow_merge_continuing_paths(
                ctx,
                &result,
                &loop->break_flow,
                loop->active_variable_count
            );
    }

    return result;
}

// ============================================================
// scope management
// ============================================================

static Scope *scope_new(SemanticContext *ctx, Scope *parent) {

    Scope *scope = arena_new(ctx->arena, Scope);

    *scope = (Scope){
        .symbols = NULL,
        .flow_count_mark = ctx->flow.count,
        .parent = parent,
    };

    return scope;
}

static void scope_push(SemanticContext *ctx) { ctx->current_scope = scope_new(ctx, ctx->current_scope); }

static void scope_pop(SemanticContext *ctx) {

    Scope *scope = ctx->current_scope;

    /*
    * The root scope created by semantic_check() is never popped.
    * Only function and block scopes reach this helper.
    */
    assert(scope);
    assert(scope->parent);

    flow_truncate_to(&ctx->flow, scope->flow_count_mark);

    ctx->current_scope = scope->parent;
}

// ============================================================
// symbols
// ============================================================
static int names_equal(const char *a, size_t a_len, const char *b, size_t b_len) {
    return a_len == b_len && memcmp(a,b,a_len) == 0;
}

static Type *find_struct_field(const Type *struct_type, const char *name, size_t length) {

    for(int i = 0; i < struct_type->field_count; i++) {
        StructField *field = &struct_type->fields[i];
        if(names_equal(field->name.data, field->name.length, name, length))
            return field->type;
    }

    return NULL;
}

static void assert_symbol_builtin_invariant(const Symbol *symbol);

// searches current scope only
static Symbol *scope_find_local(const Scope *scope, const char *name, size_t length) {

    for(Symbol *sym = scope->symbols; sym; sym = sym->next)
        if(names_equal(sym->name.data, sym->name.length, name, length))
            return sym;

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

static Scope *scope_find_defining_scope(Scope *scope, const char *name, size_t length) {
    for (Scope *s = scope; s; s = s->parent) {
        if (scope_find_local(s, name, length))
            return s;
    }
    return NULL;
}

static int scope_count_local_functions_named(
    const Scope *scope,
    const char *name,
    size_t length
) {
    int count = 0;
    if (!scope)
        return 0;
    for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
        if (sym->kind == SYMBOL_FUNCTION &&
            names_equal(sym->name.data, sym->name.length, name, length)) {
            count++;
        }
    }
    return count;
}


static EnumMember *find_enum_member(Type *enum_type, const char *name, size_t length);

static SemanticSourceModule *semantic_source_module(
    SemanticContext *ctx,
    SourceFileId source_id
) {
    if (!ctx || source_id == SOURCE_FILE_ID_INVALID)
        return NULL;

    for (size_t i = 0; i < ctx->source_module_count; i++)
        if (ctx->source_modules[i].source_id == source_id)
            return &ctx->source_modules[i];

    return NULL;
}

static SemanticModule *semantic_find_module(
    SemanticContext *ctx,
    StringView name
) {
    if (!ctx || name.length == 0)
        return NULL;

    for (SemanticModule *module = ctx->modules; module; module = module->next)
        if (!module->is_root && string_view_equals(module->name, name))
            return module;

    return NULL;
}

static SemanticModule *semantic_create_module(
    SemanticContext *ctx,
    StringView name,
    int is_root
) {
    SemanticModule *module = arena_new(ctx->arena, SemanticModule);
    module->name = name;
    module->is_root = is_root;
    module->scope = scope_new(ctx, ctx->builtin_scope);
    module->next = ctx->modules;
    ctx->modules = module;
    return module;
}

static void semantic_select_source_module(
    SemanticContext *ctx,
    SourceFileId source_id
) {
    SemanticSourceModule *source = semantic_source_module(ctx, source_id);
    assert(source && source->module);
    ctx->current_source_id = source_id;
    ctx->current_module = source->module;
    ctx->current_scope = source->module->scope;
}

static StringView semantic_import_qualifier(
    const SemanticModule *module,
    StringView alias
);

static int semantic_source_imports_module(
    const SemanticSourceModule *source,
    const SemanticModule *module
) {
    if (!source || !module)
        return 0;

    for (size_t i = 0; i < source->import_count; i++)
        if (source->imports[i].module == module)
            return 1;

    return 0;
}

static SemanticModule *semantic_source_resolve_import_qualifier(
    const SemanticSourceModule *source,
    StringView name
) {
    if (!source || name.length == 0)
        return NULL;

    for (size_t i = 0; i < source->import_count; i++) {
        const SemanticImportBinding *binding = &source->imports[i];
        StringView qualifier = binding->alias.length
            ? binding->alias
            : binding->module->name;
        if (string_view_equals(qualifier, name))
            return binding->module;
    }

    return NULL;
}

static SemanticModule *semantic_resolve_module_qualifier(
    SemanticContext *ctx,
    StringView name,
    Node *use_node
) {
    if (ctx->current_module && string_view_equals(ctx->current_module->name, name))
        return ctx->current_module;

    SemanticSourceModule *source =
        semantic_source_module(ctx, ctx->current_source_id);
    SemanticModule *module = semantic_source_resolve_import_qualifier(source, name);
    if (module)
        return module;

    SemanticModule *known = semantic_find_module(ctx, name);
    if (known) {
        semantic_error_fmt(
            ctx,
            use_node,
            "module '%.*s' is not imported in this file",
            (int)name.length,
            name.data
        );
    } else {
        semantic_error_fmt(
            ctx,
            use_node,
            "unknown module or import alias '%.*s'",
            (int)name.length,
            name.data
        );
    }
    return NULL;
}

static Symbol *semantic_lookup_symbol_in_module(
    SemanticContext *ctx,
    SemanticModule *module,
    StringView qualifier,
    StringView member_name,
    Node *use_node,
    int diagnose
) {
    if (!module)
        return NULL;

    Symbol *symbol = scope_find_local(
        module->scope,
        member_name.data,
        member_name.length
    );

    if (!symbol) {
        if (diagnose) {
            semantic_error_fmt(
                ctx,
                use_node,
                "module '%.*s' has no member '%.*s'",
                (int)qualifier.length,
                qualifier.data,
                (int)member_name.length,
                member_name.data
            );
        }
        return NULL;
    }

    if (module != ctx->current_module) {
        SemDeclInfo *info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
        if (!info || !info->is_exported) {
            if (diagnose) {
                semantic_error_fmt(
                    ctx,
                    use_node,
                    "module '%.*s' member '%.*s' is private",
                    (int)qualifier.length,
                    qualifier.data,
                    (int)member_name.length,
                    member_name.data
                );
            }
            return NULL;
        }
    }

    return symbol;
}

static Symbol *semantic_lookup_qualified_symbol(
    SemanticContext *ctx,
    StringView module_name,
    StringView member_name,
    Node *use_node
) {
    SemanticModule *module =
        semantic_resolve_module_qualifier(ctx, module_name, use_node);
    if (!module)
        return NULL;
    return semantic_lookup_symbol_in_module(
        ctx, module, module_name, member_name, use_node, 1);
}

static SemanticModule *semantic_visible_module_qualifier(
    SemanticContext *ctx,
    StringView name
) {
    if (ctx->current_module && string_view_equals(ctx->current_module->name, name))
        return ctx->current_module;

    SemanticSourceModule *source =
        semantic_source_module(ctx, ctx->current_source_id);
    return semantic_source_resolve_import_qualifier(source, name);
}

static Symbol *semantic_lookup_visible_qualified_symbol_no_diag(
    SemanticContext *ctx,
    StringView module_name,
    StringView member_name
) {
    SemanticModule *module = semantic_visible_module_qualifier(ctx, module_name);
    if (!module)
        return NULL;
    return semantic_lookup_symbol_in_module(
        ctx, module, module_name, member_name, NULL, 0);
}

static StringView dotted_first_component(StringView path)
{
    StringView first = path;
    for (size_t i = 0; i < path.length; ++i) {
        if (path.data[i] == '.') {
            first.length = i;
            break;
        }
    }
    return first;
}

static int dotted_module_suffix(
    StringView path,
    StringView module_name,
    size_t expected_components,
    StringView *out_first,
    StringView *out_second
) {
    if (path.length <= module_name.length + 1 ||
        memcmp(path.data, module_name.data, module_name.length) != 0 ||
        path.data[module_name.length] != '.') {
        return 0;
    }

    StringView suffix = {
        path.data + module_name.length + 1,
        path.length - module_name.length - 1
    };

    size_t component_count = 1;
    size_t first_dot = suffix.length;
    for (size_t i = 0; i < suffix.length; ++i) {
        if (suffix.data[i] != '.')
            continue;
        if (first_dot == suffix.length)
            first_dot = i;
        component_count++;
    }

    if (component_count != expected_components)
        return 0;

    if (out_first) {
        out_first->data = suffix.data;
        out_first->length = expected_components == 1 ? suffix.length : first_dot;
    }
    if (out_second) {
        if (expected_components == 2) {
            out_second->data = suffix.data + first_dot + 1;
            out_second->length = suffix.length - first_dot - 1;
        } else {
            *out_second = string_view_empty();
        }
    }
    return 1;
}

static int semantic_dotted_path_shadowed(
    SemanticContext *ctx,
    StringView path
) {
    StringView first = dotted_first_component(path);
    return first.length != 0 && scope_lookup(
        ctx->current_scope, first.data, first.length) != NULL;
}

static int path_has_module_qualifier_prefix(StringView path, StringView qualifier)
{
    return qualifier.length != 0 &&
           path.length > qualifier.length + 1 &&
           memcmp(path.data, qualifier.data, qualifier.length) == 0 &&
           path.data[qualifier.length] == '.';
}

static int semantic_has_visible_module_prefix(
    SemanticContext *ctx,
    StringView path
) {
    if (!ctx || path.length == 0 || semantic_dotted_path_shadowed(ctx, path))
        return 0;

    if (ctx->current_module && !ctx->current_module->is_root &&
        path_has_module_qualifier_prefix(path, ctx->current_module->name)) {
        return 1;
    }

    SemanticSourceModule *source =
        semantic_source_module(ctx, ctx->current_source_id);
    if (!source)
        return 0;
    for (size_t i = 0; i < source->import_count; i++) {
        StringView qualifier = semantic_import_qualifier(
            source->imports[i].module, source->imports[i].alias);
        if (path_has_module_qualifier_prefix(path, qualifier))
            return 1;
    }
    return 0;
}

static SemanticModule *semantic_longest_module_prefix(
    SemanticContext *ctx,
    StringView path,
    size_t suffix_components,
    int visible_only,
    StringView *out_first,
    StringView *out_second
) {
    if (!ctx || path.length == 0 || semantic_dotted_path_shadowed(ctx, path))
        return NULL;

    SemanticModule *best = NULL;
    StringView best_qualifier = string_view_empty();

    if (visible_only) {
        if (ctx->current_module && !ctx->current_module->is_root &&
            path_has_module_qualifier_prefix(path, ctx->current_module->name)) {
            best = ctx->current_module;
            best_qualifier = ctx->current_module->name;
        }

        SemanticSourceModule *source =
            semantic_source_module(ctx, ctx->current_source_id);
        if (source) {
            for (size_t i = 0; i < source->import_count; i++) {
                SemanticImportBinding *binding = &source->imports[i];
                StringView qualifier = semantic_import_qualifier(
                    binding->module, binding->alias);
                if (!path_has_module_qualifier_prefix(path, qualifier))
                    continue;
                if (!best || qualifier.length > best_qualifier.length) {
                    best = binding->module;
                    best_qualifier = qualifier;
                }
            }
        }
    } else {
        for (SemanticModule *module = ctx->modules; module; module = module->next) {
            if (module->is_root ||
                !path_has_module_qualifier_prefix(path, module->name)) {
                continue;
            }
            if (!best || module->name.length > best_qualifier.length) {
                best = module;
                best_qualifier = module->name;
            }
        }
    }

    if (!best)
        return NULL;

    StringView first = string_view_empty();
    StringView second = string_view_empty();
    if (!dotted_module_suffix(
            path,
            best_qualifier,
            suffix_components,
            &first,
            &second)) {
        return NULL;
    }

    if (out_first)
        *out_first = first;
    if (out_second)
        *out_second = second;
    return best;
}

static SemanticModule *semantic_resolve_qualified_field_location(
    SemanticContext *ctx,
    Node *node,
    int diagnose,
    StringView *out_member,
    int *out_recognized
) {
    if (out_recognized)
        *out_recognized = 0;
    if (out_member)
        *out_member = string_view_empty();

    if (!node || node->type != NODE_FIELD ||
        node->as.field.dotted_path.length == 0) {
        return NULL;
    }

    StringView member_name = string_view_empty();
    int found_only_as_nonvisible_module = 0;
    SemanticModule *module = semantic_longest_module_prefix(
        ctx,
        node->as.field.dotted_path,
        1,
        1,
        &member_name,
        NULL
    );

    if (!module && diagnose &&
        !semantic_has_visible_module_prefix(ctx, node->as.field.dotted_path)) {
        module = semantic_longest_module_prefix(
            ctx,
            node->as.field.dotted_path,
            1,
            0,
            &member_name,
            NULL
        );
        found_only_as_nonvisible_module = module != NULL;
    }

    if (!module)
        return NULL;

    if (out_recognized)
        *out_recognized = 1;

    if (found_only_as_nonvisible_module) {
        semantic_error_fmt(
            ctx,
            node,
            "module '%.*s' is not imported in this file",
            (int)module->name.length,
            module->name.data
        );
        return NULL;
    }

    if (out_member)
        *out_member = member_name;
    return module;
}

static Symbol *semantic_lookup_qualified_field_symbol(
    SemanticContext *ctx,
    Node *node,
    int diagnose,
    SemanticModule **out_module,
    StringView *out_member,
    int *out_recognized
) {
    StringView member_name = string_view_empty();
    SemanticModule *module = semantic_resolve_qualified_field_location(
        ctx, node, diagnose, &member_name, out_recognized);
    if (!module)
        return NULL;

    Symbol *symbol = semantic_lookup_symbol_in_module(
        ctx, module, module->name, member_name, node, diagnose);
    if (!symbol)
        return NULL;

    if (out_module)
        *out_module = module;
    if (out_member)
        *out_member = member_name;
    return symbol;
}

/*
 * Returns 1 on success, 0 when the expression is not a module-qualified enum
 * member shape, and -1 when the shape was recognized but produced a diagnostic.
 */
static int semantic_qualified_enum_member(
    SemanticContext *ctx,
    Node *node,
    int diagnose,
    Symbol **out_symbol,
    EnumMember **out_member
) {
    if (!node || node->type != NODE_FIELD ||
        node->as.field.dotted_path.length == 0) {
        return 0;
    }

    StringView type_name = string_view_empty();
    StringView member_name = string_view_empty();
    int found_only_as_nonvisible_module = 0;
    SemanticModule *module = semantic_longest_module_prefix(
        ctx,
        node->as.field.dotted_path,
        2,
        1,
        &type_name,
        &member_name
    );

    if (!module && diagnose &&
        !semantic_has_visible_module_prefix(ctx, node->as.field.dotted_path)) {
        module = semantic_longest_module_prefix(
            ctx,
            node->as.field.dotted_path,
            2,
            0,
            &type_name,
            &member_name
        );
        found_only_as_nonvisible_module = module != NULL;
    }

    if (!module)
        return 0;

    if (found_only_as_nonvisible_module) {
        semantic_error_fmt(
            ctx,
            node,
            "module '%.*s' is not imported in this file",
            (int)module->name.length,
            module->name.data
        );
        return -1;
    }

    Symbol *symbol = semantic_lookup_symbol_in_module(
        ctx, module, module->name, type_name, node, diagnose);
    if (!symbol)
        return diagnose ? -1 : 0;

    /* A global subobject such as `state.pair.x` is not enum qualification. */
    if (symbol->kind != SYMBOL_TYPE)
        return 0;

    if (!symbol->type || symbol->type->kind != TYPE_ENUM) {
        if (diagnose) {
            semantic_error_fmt(
                ctx,
                node,
                "'%.*s.%.*s' is not an enum type",
                (int)module->name.length,
                module->name.data,
                (int)type_name.length,
                type_name.data
            );
            return -1;
        }
        return 0;
    }

    EnumMember *member = find_enum_member(
        symbol->type,
        member_name.data,
        member_name.length
    );
    if (!member) {
        if (diagnose) {
            semantic_error_name(
                ctx,
                node,
                "unknown enum member",
                member_name.data,
                member_name.length
            );
            return -1;
        }
        return 0;
    }

    if (out_symbol)
        *out_symbol = symbol;
    if (out_member)
        *out_member = member;
    return 1;
}

static int semantic_qualified_enum_member_no_diag(
    SemanticContext *ctx,
    Node *node,
    Symbol **out_symbol,
    EnumMember **out_member
) {
    return semantic_qualified_enum_member(
        ctx, node, 0, out_symbol, out_member) == 1;
}

/*
 * Internal symbol constructor.
 *
 * The complete symbol classification is supplied atomically so a
 * SYMBOL_BUILTIN never temporarily exists with BUILTIN_NONE.
 */
static Symbol *scope_define_symbol(
    SemanticContext *ctx,
    StringView name,
    SymbolKind kind,
    BuiltinKind builtin_kind,
    Type *type
) {
    assert(
        kind == SYMBOL_BUILTIN
            ? builtin_kind != BUILTIN_NONE
            : builtin_kind == BUILTIN_NONE
    );

    Symbol *symbol =
        arena_new(ctx->arena, Symbol);

    *symbol = (Symbol){
        .name             = name,
        .kind             = kind,
        .builtin_kind     = builtin_kind,
        .type             = type,
        .declaration      = NULL,
        .declaration_id   = INVALID_SEM_DECL_ID,
        .variable_storage = VARIABLE_STORAGE_NONE,
        .flow_owner_id    = INVALID_FLOW_OWNER_ID,
        .variable_id      = INVALID_VARIABLE_ID,
        .next             = ctx->current_scope->symbols,
    };

    assert_symbol_builtin_invariant(symbol);

    ctx->current_scope->symbols =
        symbol;

    return symbol;
}

static Symbol *scope_define(
    SemanticContext *ctx,
    StringView name,
    SymbolKind kind,
    Type *type
) {
    assert(kind != SYMBOL_BUILTIN);

    return scope_define_symbol(ctx, name, kind, BUILTIN_NONE, type);
}

static Symbol *scope_define_declared(
    SemanticContext *ctx,
    Node *declaration,
    StringView name,
    SymbolKind kind,
    Type *type
) {
    assert(declaration);
    assert(kind != SYMBOL_BUILTIN);
    assert(type);

    Symbol *symbol = scope_define(ctx, name, kind, type);
    sem_record_decl_info(ctx, declaration, type, symbol);
    return symbol;
}

static Symbol *scope_predeclare_declared(
    SemanticContext *ctx,
    Node *declaration,
    StringView name,
    SymbolKind kind
) {
    assert(declaration);
    assert(kind != SYMBOL_BUILTIN);

    Symbol *symbol = scope_define(ctx, name, kind, NULL);
    SemDeclInfo *info = sem_get_or_create_decl_info(ctx, declaration);

    assert(!info->symbol);
    info->symbol = symbol;

    symbol->declaration = declaration;
    symbol->declaration_id = info->id;
    return symbol;
}

static void assert_symbol_builtin_invariant(const Symbol *symbol) {

#ifndef NDEBUG
    assert(symbol);

    if (symbol->kind == SYMBOL_BUILTIN) {
        assert(symbol->builtin_kind != BUILTIN_NONE);
        assert(symbol->type == NULL);
    } else {
        assert(symbol->builtin_kind == BUILTIN_NONE);
    }
#else
    (void)symbol;
#endif

}

/*
 * Defines a compiler-provided builtin in the current scope.
 *
 * Builtins do not have an ordinary fixed Type yet. Their result and
 * argument types are determined by the central builtin call checker.
 *
 * Registration is performed only after the call checker understands
 * SYMBOL_BUILTIN.
 */
static Symbol *scope_define_builtin(SemanticContext *ctx, const char *name, BuiltinKind builtin_kind) {

    assert(name);
    assert(builtin_kind != BUILTIN_NONE);

    return scope_define_symbol(
        ctx,
        string_view_from_cstr(name),
        SYMBOL_BUILTIN,
        builtin_kind,
        NULL);
}

static Type *fixed_integer_type_for_c_abi_bits(
    SemanticContext *ctx,
    unsigned bit_width,
    int is_signed
) {
    switch (bit_width) {
        case 8:  return is_signed ? ctx->type_s8  : ctx->type_u8;
        case 16: return is_signed ? ctx->type_s16 : ctx->type_u16;
        case 32: return is_signed ? ctx->type_s32 : ctx->type_u32;
        case 64: return is_signed ? ctx->type_s64 : ctx->type_u64;
        default: return NULL;
    }

    return NULL;
}

static int register_c_abi_type_alias(
    SemanticContext *ctx,
    const char *name,
    Type *type,
    unsigned bit_width
) {
    if (!type) {
        fprintf(
            stderr,
            "semantic error: target C ABI type '%s' has unsupported width %u bits\n",
            name,
            bit_width
        );
        ctx->had_error = 1;
        ctx->error_count++;
        return 0;
    }

    scope_define(
        ctx,
        string_view_from_cstr(name),
        SYMBOL_TYPE,
        type
    );

    return 1;
}

/*
 * Registers target C integer-family ABI aliases.
 *
 * Source-level aliases remain transparent after semantic resolution, but their
 * exact spelling is retained separately by SemAbiType when a declaration sits
 * on a C ABI boundary. TargetInfo is the sole authority for deciding which
 * canonical Coglet integer type an alias resolves to.
 */
static void register_target_c_integer_alias(
    SemanticContext *ctx,
    const char *name,
    unsigned bit_width,
    int is_signed
) {
    Type *type = fixed_integer_type_for_c_abi_bits(ctx, bit_width, is_signed);
    register_c_abi_type_alias(ctx, name, type, bit_width);
}

static void register_target_c_float_alias(
    SemanticContext *ctx,
    const char *name,
    Type *type,
    TargetFloatFormat actual_format,
    TargetFloatFormat expected_format,
    unsigned expected_bits
) {
    if (actual_format != expected_format) {
        fprintf(
            stderr,
            "semantic error: target C ABI type '%s' is not compatible with Coglet's IEEE binary%u type\n",
            name,
            expected_bits
        );
        ctx->had_error = 1;
        ctx->error_count++;
        return;
    }

    scope_define(
        ctx,
        string_view_from_cstr(name),
        SYMBOL_TYPE,
        type
    );
}

static void register_target_c_abi_type_aliases(SemanticContext *ctx) {
    const TargetInfo *target = &ctx->target;

    register_target_c_integer_alias(
        ctx, "c_char", target->c_char_bits, target->c_char_is_signed
    );
    register_target_c_integer_alias(ctx, "c_schar", target->c_char_bits, 1);
    register_target_c_integer_alias(ctx, "c_uchar", target->c_char_bits, 0);

    register_target_c_integer_alias(ctx, "c_short", target->c_short_bits, 1);
    register_target_c_integer_alias(ctx, "c_ushort", target->c_short_bits, 0);
    register_target_c_integer_alias(ctx, "c_int", target->c_int_bits, 1);
    register_target_c_integer_alias(ctx, "c_uint", target->c_int_bits, 0);
    register_target_c_integer_alias(ctx, "c_long", target->c_long_bits, 1);
    register_target_c_integer_alias(ctx, "c_ulong", target->c_long_bits, 0);
    register_target_c_integer_alias(ctx, "c_longlong", target->c_long_long_bits, 1);
    register_target_c_integer_alias(ctx, "c_ulonglong", target->c_long_long_bits, 0);

    register_target_c_integer_alias(ctx, "c_size", target->c_size_bits, 0);

    /*
     * C `_Bool` has the same logical value domain Coglet exposes through
     * `bool`; ABI layout is retained in TargetInfo for later native lowering.
     */
    scope_define(
        ctx,
        string_view_from_cstr("c_bool"),
        SYMBOL_TYPE,
        ctx->type_bool
    );

    register_target_c_float_alias(
        ctx,
        "c_float",
        ctx->type_f32,
        target->c_float_format,
        TARGET_FLOAT_FORMAT_IEEE_BINARY32,
        32
    );

    register_target_c_float_alias(
        ctx,
        "c_double",
        ctx->type_f64,
        target->c_double_format,
        TARGET_FLOAT_FORMAT_IEEE_BINARY64,
        64
    );
}

static void register_builtin_symbols(SemanticContext *ctx) {

    register_target_c_abi_type_aliases(ctx);

    scope_define_builtin(ctx, "wrapping_add", BUILTIN_WRAPPING_ADD);
    scope_define_builtin(ctx, "wrapping_sub", BUILTIN_WRAPPING_SUB);
    scope_define_builtin(ctx, "wrapping_mul", BUILTIN_WRAPPING_MUL);
    scope_define_builtin(ctx, "wrapping_neg", BUILTIN_WRAPPING_NEG);
    scope_define_builtin(ctx, "size_of", BUILTIN_SIZE_OF);
    scope_define_builtin(ctx, "align_of", BUILTIN_ALIGN_OF);
    scope_define_builtin(ctx, "slice", BUILTIN_SLICE);
}

/*
 * Assigns variable-storage metadata and stable per-function
 * flow identity.
 */
static void classify_variable_symbol(SemanticContext *ctx, Symbol *symbol, VariableStorage storage) {

    assert(symbol);
    assert(symbol->kind == SYMBOL_VARIABLE);
    assert(symbol->variable_storage == VARIABLE_STORAGE_NONE);

    symbol->variable_storage = storage;

    switch (storage) {
        case VARIABLE_STORAGE_GLOBAL:
            symbol->flow_owner_id = INVALID_FLOW_OWNER_ID;
            symbol->variable_id   = INVALID_VARIABLE_ID;

            break;

        case VARIABLE_STORAGE_LOCAL:
        case VARIABLE_STORAGE_PARAMETER:
            assert(ctx->flow.owner_id != INVALID_FLOW_OWNER_ID);

            symbol->flow_owner_id = ctx->flow.owner_id;
            symbol->variable_id   = ctx->next_variable_id++;

            break;

        case VARIABLE_STORAGE_NONE:
            assert(0 && "variable symbol requires a storage classification");
            break;
    }
}

static int extern_c_type_supported(const Type *type, int allow_void);

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
        Symbol *symbol = NULL;
        if (type->named_module.length != 0) {
            symbol = semantic_lookup_qualified_symbol(
                ctx,
                type->named_module,
                type->named_name,
                error_node
            );
            if (!symbol)
                return NULL;
            if (symbol->kind != SYMBOL_TYPE) {
                semantic_error_fmt(
                    ctx,
                    error_node,
                    "'%.*s.%.*s' is not a type",
                    (int)type->named_module.length,
                    type->named_module.data,
                    (int)type->named_name.length,
                    type->named_name.data
                );
                return NULL;
            }
        } else {
            symbol = scope_lookup(
                ctx->current_scope,
                type->named_name.data,
                type->named_name.length
            );
            if (!symbol || symbol->kind != SYMBOL_TYPE) {
                semantic_error_name(
                    ctx,
                    error_node,
                    "unknown type",
                    type->named_name.data,
                    type->named_name.length
                );
                return NULL;
            }
        }

        return resolve_generic_struct_application(
            ctx,
            symbol,
            type->type_arguments,
            type->type_argument_count,
            error_node
        );
    }

    if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY ||
        type->kind == TYPE_SLICE) {

        Type *resolved_element =
            resolve_type(ctx, type->element, error_node);

        if (!resolved_element)
            return NULL;

        if (type->kind == TYPE_SLICE) {
            return intern_slice_type(
                ctx,
                resolved_element,
                type->pointer_access
            );
        }

        if (resolved_element != type->element) {

            Type *copy = arena_alloc(ctx->arena, sizeof(Type));

            *copy = *type;
            copy->element = resolved_element;

            return copy;
        }
    }

    if (type->kind == TYPE_FUNCTION) {
        Type **resolved_parameters = type->parameters;
        int changed = 0;

        if (type->parameter_count > 0) {
            resolved_parameters = arena_alloc(
                ctx->arena,
                sizeof(Type *) * (size_t)type->parameter_count
            );

            for (int i = 0; i < type->parameter_count; i++) {
                resolved_parameters[i] =
                    resolve_type(ctx, type->parameters[i], error_node);

                if (!resolved_parameters[i])
                    return NULL;

                if (resolved_parameters[i] != type->parameters[i])
                    changed = 1;
            }
        }

        Type *resolved_return =
            resolve_type(ctx, type->return_type, error_node);

        if (!resolved_return)
            return NULL;

        if (resolved_return != type->return_type)
            changed = 1;

        Type *resolved_function = type;

        if (changed) {
            Type *copy = arena_alloc(ctx->arena, sizeof(Type));
            *copy = *type;
            copy->parameters = resolved_parameters;
            copy->return_type = resolved_return;
            resolved_function = copy;
        }

        if (resolved_function->function_abi == FUNCTION_ABI_C &&
            resolved_function->function_is_variadic &&
            resolved_function->function_call_conv == C_CALL_STDCALL) {
            semantic_error(
                ctx,
                error_node,
                "stdcall C function pointers cannot be variadic"
            );
            return NULL;
        }

        if (resolved_function->function_abi == FUNCTION_ABI_C &&
            !extern_c_type_supported(resolved_function, 0)) {
            semantic_error(
                ctx,
                error_node,
                "cfn signature contains a type not supported by the current C ABI subset"
            );
            return NULL;
        }

        return resolved_function;
    }

    return type;
}

static SemCScalarKind native_c_scalar_kind(const Type *source_type)
{
    if (!source_type || source_type->kind != TYPE_NAMED ||
        source_type->named_module.length != 0)
        return SEM_C_SCALAR_NONE;

    StringView name = source_type->named_name;

#define C_SCALAR_ALIAS(text, kind) \
    if (names_equal(name.data, name.length, text, sizeof(text) - 1)) return kind

    C_SCALAR_ALIAS("c_char",      SEM_C_SCALAR_CHAR);
    C_SCALAR_ALIAS("c_schar",     SEM_C_SCALAR_SCHAR);
    C_SCALAR_ALIAS("c_uchar",     SEM_C_SCALAR_UCHAR);
    C_SCALAR_ALIAS("c_short",     SEM_C_SCALAR_SHORT);
    C_SCALAR_ALIAS("c_ushort",    SEM_C_SCALAR_USHORT);
    C_SCALAR_ALIAS("c_int",       SEM_C_SCALAR_INT);
    C_SCALAR_ALIAS("c_uint",      SEM_C_SCALAR_UINT);
    C_SCALAR_ALIAS("c_long",      SEM_C_SCALAR_LONG);
    C_SCALAR_ALIAS("c_ulong",     SEM_C_SCALAR_ULONG);
    C_SCALAR_ALIAS("c_longlong",  SEM_C_SCALAR_LONGLONG);
    C_SCALAR_ALIAS("c_ulonglong", SEM_C_SCALAR_ULONGLONG);
    C_SCALAR_ALIAS("c_size",      SEM_C_SCALAR_SIZE);
    C_SCALAR_ALIAS("c_bool",      SEM_C_SCALAR_BOOL);
    C_SCALAR_ALIAS("c_float",     SEM_C_SCALAR_FLOAT);
    C_SCALAR_ALIAS("c_double",    SEM_C_SCALAR_DOUBLE);

#undef C_SCALAR_ALIAS

    return SEM_C_SCALAR_NONE;
}

/*
 * Captures the source spelling needed to cross a native-C ABI boundary while
 * retaining the fully resolved semantic type beside it. This is intentionally
 * not another type checker: all legality and type resolution has already
 * happened before this helper is called.
 */
static SemAbiType *make_sem_abi_type(
    SemanticContext *ctx,
    const Type *source_type,
    Type *semantic_type
) {
    assert(ctx);
    assert(source_type);
    assert(semantic_type);

    SemAbiType *abi_type = arena_alloc(ctx->arena, sizeof(*abi_type));
    memset(abi_type, 0, sizeof(*abi_type));
    abi_type->semantic_type = semantic_type;

    SemCScalarKind c_scalar = native_c_scalar_kind(source_type);
    if (c_scalar != SEM_C_SCALAR_NONE) {
        abi_type->kind = SEM_ABI_TYPE_C_SCALAR;
        abi_type->c_scalar_kind = c_scalar;
        return abi_type;
    }

    switch (source_type->kind) {
        case TYPE_POINTER:
            assert(semantic_type->kind == TYPE_POINTER);
            assert(source_type->element);
            assert(semantic_type->element);

            abi_type->kind = SEM_ABI_TYPE_POINTER;
            abi_type->element = make_sem_abi_type(
                ctx,
                source_type->element,
                semantic_type->element
            );
            return abi_type;

        case TYPE_OPAQUE_POINTER:
            assert(semantic_type->kind == TYPE_OPAQUE_POINTER);
            abi_type->kind = SEM_ABI_TYPE_OPAQUE_POINTER;
            return abi_type;

        case TYPE_ARRAY:
            assert(semantic_type->kind == TYPE_ARRAY);
            assert(source_type->element);
            assert(semantic_type->element);

            abi_type->kind = SEM_ABI_TYPE_ARRAY;
            abi_type->element = make_sem_abi_type(
                ctx,
                source_type->element,
                semantic_type->element
            );
            return abi_type;

        case TYPE_SLICE:
            assert(semantic_type->kind == TYPE_SLICE);
            abi_type->kind = SEM_ABI_TYPE_SEMANTIC;
            return abi_type;

        case TYPE_FUNCTION:
            assert(semantic_type->kind == TYPE_FUNCTION);
            assert(source_type->parameter_count == semantic_type->parameter_count);

            abi_type->kind = SEM_ABI_TYPE_FUNCTION;
            abi_type->parameter_count = semantic_type->parameter_count;

            if (abi_type->parameter_count > 0) {
                abi_type->parameters = arena_alloc(
                    ctx->arena,
                    sizeof(SemAbiType *) * (size_t)abi_type->parameter_count
                );

                for (int i = 0; i < abi_type->parameter_count; i++) {
                    abi_type->parameters[i] = make_sem_abi_type(
                        ctx,
                        source_type->parameters[i],
                        semantic_type->parameters[i]
                    );
                }
            }

            abi_type->return_type = make_sem_abi_type(
                ctx,
                source_type->return_type,
                semantic_type->return_type
            );
            return abi_type;

        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_NAMED:
        case TYPE_STRUCT:
        case TYPE_ENUM:
            abi_type->kind = SEM_ABI_TYPE_SEMANTIC;
            return abi_type;

        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
            assert(!"non-concrete type cannot appear in normalized ABI metadata");
            break;
    }

    return abi_type;
}

/*
 * Returns whether a declaration must retain exact C-facing spelling for its
 * addressable object representation. Ordinary semantic types deliberately do
 * not carry redundant ABI metadata; C scalar spellings and recursive objects
 * containing them do. C function values retain their full callable ABI.
 */
static int sem_abi_type_requires_storage_spelling(const SemAbiType *abi_type)
{
    if (!abi_type)
        return 0;
    switch (abi_type->kind) {
        case SEM_ABI_TYPE_C_SCALAR:
            return abi_type->c_scalar_kind == SEM_C_SCALAR_BOOL;
        case SEM_ABI_TYPE_FUNCTION:
            return 1;
        case SEM_ABI_TYPE_POINTER:
        case SEM_ABI_TYPE_ARRAY:
            return sem_abi_type_requires_storage_spelling(abi_type->element);
        case SEM_ABI_TYPE_SEMANTIC:
        case SEM_ABI_TYPE_OPAQUE_POINTER:
            return 0;
    }
    return 0;
}

// ============================================================
// forward declarations
// ============================================================
static void check_node(SemanticContext *ctx, Node *node);
static int  declare_struct_shell(SemanticContext *ctx, Node *node);
static void fill_struct_fields(SemanticContext *ctx, Node *node);
static void validate_repr_c_struct_layouts(SemanticContext *ctx, Node *program);
static void validate_concrete_coglet_struct_layouts(SemanticContext *ctx);
static int  declare_function_signature(SemanticContext *ctx, Node *node);
static int  declare_generic_function_template(SemanticContext *ctx, Node *node);
static int  declare_generic_struct_template(SemanticContext *ctx, Node *node);
static void check_function_body(SemanticContext *ctx, Node *node);
static void check_const_decl(SemanticContext *ctx, Node *node);
static int ensure_constant_symbol_checked(SemanticContext *ctx, Symbol *symbol);
static int ensure_global_variable_symbol_checked(SemanticContext *ctx, Symbol *symbol);
static void check_if_statement(SemanticContext *ctx, Node *node);
static void check_switch_statement(SemanticContext *ctx, Node *node);
static void check_while_statement(SemanticContext *ctx, Node *node);
static void check_for_statement(SemanticContext *ctx, Node *node);
static int check_statement_expression(SemanticContext *ctx, Node *node);
static int check_assignment_statement(SemanticContext *ctx, Node *node);
static int check_compound_assignment_statement(SemanticContext *ctx, Node *node);
static int check_inc_dec_statement(SemanticContext *ctx, Node *node);
static int check_initializer_against_type(SemanticContext *ctx, Type *expected, Node *initializer);
static int check_c_variadic_argument(SemanticContext *ctx, Node *argument);
static void format_type_name(Type *type, char *buffer, size_t buffer_size);
static int source_type_is_readonly_c_char_pointer(const Type *type);
static int check_extern_c_string_argument(SemanticContext *ctx, Type *expected, Node *argument);
static int check_argument_against_parameter(SemanticContext *ctx, Type *expected, Node *argument);
static int check_array_initializer(SemanticContext *ctx, Type *expected, Node *initializer);
static int declare_enum_shell(SemanticContext *ctx, Node *node);
static void fill_enum_members(SemanticContext *ctx,Node *node);
static EnumMember *find_enum_member(Type *enum_type, const char *name, size_t length);
static EnumMember *find_enum_member_by_value(Type *enum_type, IntegerValue value);
static Type *check_value_expression(SemanticContext *ctx, Node *node);
static Type *make_function_type(SemanticContext *ctx, Node *func);
static Type *check_checked_cast_expression(SemanticContext *ctx, Node *node);
static Type *check_reinterpret_expression(SemanticContext *ctx, Node *node);
static Type *concretize_inferred_type(SemanticContext *ctx, Node *expression, Type *type);
static int switch_case_values_are_exhaustive(
    Type *switch_type, const ConstValue *case_values, int case_value_count, int has_default);

static int eval_const_checked_cast(SemanticContext *ctx, Node *node, ConstValue *out);
static int expression_is_compile_time_constant(SemanticContext *ctx, Node *node);
static int eval_const_expr(SemanticContext *ctx, Node *node, ConstValue *out);
static int eval_const_expr_impl(SemanticContext *ctx, Node *node, ConstValue *out);
static int try_coerce_constant_to_type(
    const ConstValue *value,
    Type *target_type,
    ConstValue *out);
static int check_string_initializer(SemanticContext *ctx, Type *expected, Node *initializer);
static int eval_const_builtin_call(SemanticContext *ctx, Node *call, ConstValue *out);
static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out);

// ============================================================
// expressions
// ============================================================
static int contains_void_type(Type *type) {

    if (!type) return 0;

    if (type->kind == TYPE_VOID) return 1;

    if (type->kind == TYPE_POINTER || type->kind == TYPE_ARRAY ||
        type->kind == TYPE_SLICE)
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

static const char *c_calling_convention_name(CCallingConvention convention)
{
    switch (convention) {
        case C_CALL_DEFAULT: return NULL;
        case C_CALL_CDECL: return "cdecl";
        case C_CALL_STDCALL: return "stdcall";
        case C_CALL_SYSV64: return "sysv64";
        case C_CALL_WIN64: return "win64";
    }

    return NULL;
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

        case TYPE_S8:   snprintf(buffer, buffer_size, "s8");   return;
        case TYPE_S16:  snprintf(buffer, buffer_size, "s16");  return;
        case TYPE_S32:  snprintf(buffer, buffer_size, "s32");  return;
        case TYPE_S64:  snprintf(buffer, buffer_size, "s64");  return;
        case TYPE_U8:   snprintf(buffer, buffer_size, "u8");   return;
        case TYPE_U16:  snprintf(buffer, buffer_size, "u16");  return;
        case TYPE_U32:  snprintf(buffer, buffer_size, "u32");  return;
        case TYPE_U64:  snprintf(buffer, buffer_size, "u64");  return;
        case TYPE_F32:  snprintf(buffer, buffer_size, "f32");  return;
        case TYPE_F64:  snprintf(buffer, buffer_size, "f64");  return;

        case TYPE_FUNCTION: {
            const char *prefix =
                type->function_abi == FUNCTION_ABI_C ? "cfn" : "fn";
            const char *call =
                type->function_abi == FUNCTION_ABI_C
                    ? c_calling_convention_name(type->function_call_conv)
                    : NULL;

            if (call) {
                snprintf(
                    buffer,
                    buffer_size,
                    type->function_is_variadic
                        ? "%s(call=%s, ..., ...)"
                        : "%s(call=%s, ...)",
                    prefix,
                    call
                );
            } else {
                snprintf(
                    buffer,
                    buffer_size,
                    type->function_is_variadic ? "%s(..., ...)" : "%s(...)",
                    prefix
                );
            }
            return;
        }
        case TYPE_UNTYPED_INT: snprintf(buffer, buffer_size, "untyped-int"); return;
        case TYPE_UNTYPED_FLOAT: snprintf(buffer, buffer_size, "untyped-float"); return;

        case TYPE_NAMED: {
            size_t used = 0;
            int written = type->named_module.length != 0
                ? snprintf(
                    buffer,
                    buffer_size,
                    "%.*s.%.*s",
                    (int)type->named_module.length,
                    type->named_module.data,
                    (int)type->named_name.length,
                    type->named_name.data
                )
                : snprintf(
                    buffer,
                    buffer_size,
                    "%.*s",
                    (int)type->named_name.length,
                    type->named_name.data
                );
            if (written < 0)
                return;
            used = (size_t)written < buffer_size ? (size_t)written : buffer_size - 1;
            if (type->type_argument_count > 0 && used + 3 < buffer_size) {
                buffer[used++] = '<';
                buffer[used] = '\0';
                for (int i = 0; i < type->type_argument_count; i++) {
                    char argument[192];
                    format_type_name(type->type_arguments[i], argument, sizeof(argument));
                    written = snprintf(
                        buffer + used,
                        buffer_size - used,
                        "%s%s",
                        i == 0 ? "" : ", ",
                        argument
                    );
                    if (written < 0)
                        return;
                    if ((size_t)written >= buffer_size - used) {
                        buffer[buffer_size - 1] = '\0';
                        return;
                    }
                    used += (size_t)written;
                }
                if (used + 1 < buffer_size) {
                    buffer[used++] = '>';
                    buffer[used] = '\0';
                }
            }
            return;
        }

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

        case TYPE_OPAQUE_POINTER:
            if (type->pointer_access == POINTER_ACCESS_READONLY &&
                type->pointer_is_volatile) {
                snprintf(buffer, buffer_size, "readonly volatile opaque*");
            } else if (type->pointer_access == POINTER_ACCESS_READONLY) {
                snprintf(buffer, buffer_size, "readonly opaque*");
            } else if (type->pointer_is_volatile) {
                snprintf(buffer, buffer_size, "volatile opaque*");
            } else {
                snprintf(buffer, buffer_size, "opaque*");
            }
            return;

        case TYPE_POINTER: {
            char element[128];

            format_type_name(
                type->element,
                element,
                sizeof(element)
            );

            switch (type->pointer_access) {
                case POINTER_ACCESS_MUTABLE:
                    snprintf(
                        buffer,
                        buffer_size,
                        type->pointer_is_volatile ? "volatile %s*" : "%s*",
                        element
                    );
                    return;

                case POINTER_ACCESS_READONLY:
                    snprintf(
                        buffer,
                        buffer_size,
                        type->pointer_is_volatile
                            ? "readonly volatile %s*"
                            : "readonly %s*",
                        element
                    );
                    return;
            }

            snprintf(
                buffer,
                buffer_size,
                "<invalid-pointer-access>"
            );
            return;
        }

        case TYPE_ARRAY: {
            char element[128];
            format_type_name(type->element, element, sizeof(element));
            snprintf(buffer, buffer_size, "%s[%d]", element, type->array_size);
            return;
        }

        case TYPE_SLICE: {
            char element[128];
            format_type_name(type->element, element, sizeof(element));
            snprintf(
                buffer,
                buffer_size,
                type->pointer_access == POINTER_ACCESS_READONLY
                    ? "readonly %s[]"
                    : "%s[]",
                element
            );
            return;
        }
    }

    snprintf(buffer, buffer_size, "<unknown>");
}

void semantic_format_type_name(Type *type, char *buffer, size_t buffer_size)
{
    format_type_name(type, buffer, buffer_size);
}

static int is_valid_pointer_access(PointerAccess access) {

    switch (access) {
        case POINTER_ACCESS_MUTABLE:
        case POINTER_ACCESS_READONLY:
            return 1;
    }

    return 0;
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

        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:

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
        case TYPE_OPAQUE_POINTER:
            if (!is_valid_pointer_access(a->pointer_access) ||
                !is_valid_pointer_access(b->pointer_access)) {
                return 0;
            }

            return a->pointer_access == b->pointer_access &&
                   a->pointer_is_volatile == b->pointer_is_volatile;

        case TYPE_POINTER:
            if (!is_valid_pointer_access(a->pointer_access) ||
                !is_valid_pointer_access(b->pointer_access)) {
                return 0;
            }

            return a->pointer_access == b->pointer_access &&
                a->pointer_is_volatile == b->pointer_is_volatile &&
                type_equal(
                    a->element,
                    b->element);

        case TYPE_ARRAY:
            return a->array_size == b->array_size &&
                   type_equal(a->element, b->element);

        case TYPE_SLICE:
            if (!is_valid_pointer_access(a->pointer_access) ||
                !is_valid_pointer_access(b->pointer_access)) {
                return 0;
            }
            return a->pointer_access == b->pointer_access &&
                   type_equal(a->element, b->element);

        case TYPE_FUNCTION:
            if (a->function_abi != b->function_abi ||
                a->function_call_conv != b->function_call_conv ||
                a->function_is_variadic != b->function_is_variadic)
                return 0;

            if (a->parameter_count != b->parameter_count) {
                return 0;
            }

            for (int i = 0; i < a->parameter_count;i++) {
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
            if (!names_equal(
                    a->named_module.data,
                    a->named_module.length,
                    b->named_module.data,
                    b->named_module.length) ||
                !names_equal(
                    a->named_name.data,
                    a->named_name.length,
                    b->named_name.data,
                    b->named_name.length) ||
                a->type_argument_count != b->type_argument_count) {
                return 0;
            }
            for (int i = 0; i < a->type_argument_count; i++) {
                if (!type_equal(a->type_arguments[i], b->type_arguments[i]))
                    return 0;
            }
            return 1;

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

/*
 * Incomplete #repr(c) structs model foreign object types whose layout is not
 * available to Coglet. They may appear behind raw pointers, but never as
 * inline/by-value storage (including through arrays).
 */
static int contains_incomplete_struct_by_value(const Type *type)
{
    if (!type) return 0;

    if (type->kind == TYPE_STRUCT)
        return type->struct_is_incomplete;

    if (type->kind == TYPE_ARRAY)
        return contains_incomplete_struct_by_value(type->element);

    return 0;
}

static int invalid_return_type(Type *type) {

    if (!type) return 0;

    if (type->kind == TYPE_VOID) return 0;

    return contains_void_type(type);
}

typedef struct IntegerTypeInfo {
    unsigned bit_width;
    int is_signed;
} IntegerTypeInfo;

/*
 * Returns the fixed-width representation facts for a concrete
 * Coglet integer type.
 *
 * This is the single source of truth for integer width and
 * signedness. Untyped integers are exact compile-time values and
 * deliberately have no fixed-width representation here.
 */
static int integer_type_info(TypeKind kind, IntegerTypeInfo *out) {

    IntegerTypeInfo info = {
        .bit_width = 0,
        .is_signed = 0
    };

    switch (kind) {
        case TYPE_S8:
            info.bit_width = 8;
            info.is_signed = 1;
            break;

        case TYPE_S16:
            info.bit_width = 16;
            info.is_signed = 1;
            break;

        case TYPE_S32:
            info.bit_width = 32;
            info.is_signed = 1;
            break;

        case TYPE_S64:
            info.bit_width = 64;
            info.is_signed = 1;
            break;

        case TYPE_U8:
            info.bit_width = 8;
            info.is_signed = 0;
            break;

        case TYPE_U16:
            info.bit_width = 16;
            info.is_signed = 0;
            break;

        case TYPE_U32:
            info.bit_width = 32;
            info.is_signed = 0;
            break;

        case TYPE_U64:
            info.bit_width = 64;
            info.is_signed = 0;
            break;

        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_POINTER:
        case TYPE_OPAQUE_POINTER:
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_NAMED:
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_FUNCTION:
            return 0;
    }

    if (out)
        *out = info;

    return 1;
}

static int is_concrete_integer_kind(TypeKind kind) {
    return integer_type_info(kind, NULL);
}

static int is_signed_integer_kind(TypeKind kind) {
    IntegerTypeInfo info;

    return integer_type_info(kind, &info) &&
           info.is_signed;
}

static int is_unsigned_integer_kind(TypeKind kind) {
    IntegerTypeInfo info;

    return integer_type_info(kind, &info) &&
           !info.is_signed;
}

static int integer_kind_bit_width(TypeKind kind, unsigned *out_width) {

    IntegerTypeInfo info;

    if (!out_width || !integer_type_info(kind, &info))
        return 0;

    *out_width = info.bit_width;
    return 1;
}

static int is_integer_kind(TypeKind kind) { return is_concrete_integer_kind(kind) || kind == TYPE_UNTYPED_INT;}
static int is_concrete_float_kind(TypeKind kind) { return kind == TYPE_F32 || kind == TYPE_F64; }
static int is_float_kind(TypeKind kind) { return is_concrete_float_kind(kind) || kind == TYPE_UNTYPED_FLOAT; }
static int is_untyped_numeric_kind(TypeKind kind) { return kind == TYPE_UNTYPED_INT || kind == TYPE_UNTYPED_FLOAT; }
static int is_untyped_numeric_type(const Type *type) { return type && is_untyped_numeric_kind(type->kind); }

static IntegerValue integer_value_make(uint64_t magnitude, int is_negative) {

    IntegerValue value = {
        .magnitude   = magnitude,
        .is_negative = magnitude != 0 && is_negative
    };

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

static uint64_t integer_width_mask(unsigned width) {

    assert(width >= 1);
    assert(width <= 64);

    return width == 64
        ? UINT64_MAX
        : (UINT64_C(1) << width) - UINT64_C(1);
}

static int integer_value_fits_type( IntegerValue value, TypeKind kind) {
    /*
     * Untyped integers retain an exact uint64_t magnitude.
     *
     * Positive values may use the complete uint64_t magnitude
     * domain. Negative values are limited to the s64 minimum
     * magnitude so they can receive an ordinary concrete default
     * type.
     */
    if (kind == TYPE_UNTYPED_INT) {
        return !value.is_negative ||
               value.magnitude <= (UINT64_C(1) << 63);
    }

    IntegerTypeInfo info;

    if (!integer_type_info(kind, &info))
        return 0;

    if (!info.is_signed) {
        return !value.is_negative &&
               value.magnitude <=
                   integer_width_mask(info.bit_width);
    }

    uint64_t minimum_magnitude =
        UINT64_C(1) << (info.bit_width - 1);

    if (value.is_negative)
        return value.magnitude <= minimum_magnitude;

    return value.magnitude < minimum_magnitude;
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

            *out = integer_value_make(magnitude, 1);

            return 1;
        }
    }

    *out = integer_value_make(pattern, 0);

    return 1;
}

/*
 * Converts an exact mathematical integer to the low N bits of the
 * target integer type.
 *
 * Unlike integer_value_to_bit_pattern(), the source value does not
 * need to fit the destination type. Truncation is explicitly modulo
 * 2^N.
 */
static int truncate_integer_value(IntegerValue value, TypeKind target_kind, IntegerValue *out) {

    assert(out);

    unsigned width;

    if (!integer_kind_bit_width(target_kind, &width))
        return 0;

    uint64_t mask    = integer_width_mask(width);
    uint64_t pattern = value.magnitude & mask;

    if (value.is_negative) {
        pattern = (UINT64_C(0) - pattern) & mask;
    }

    return integer_value_from_bit_pattern(
        pattern,
        target_kind,
        out
    );
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

/*
 * Evaluates wrapping addition, subtraction, or multiplication on
 * fixed-width integer bit patterns.
 *
 * Host unsigned arithmetic is defined modulo 2^64. Masking the
 * result then applies the corresponding modulo 2^N rule for every
 * narrower Coglet integer type.
 */
static int evaluate_wrapping_integer_binary(
    BuiltinKind builtin_kind,
    IntegerValue left,
    IntegerValue right,
    TypeKind operation_kind,
    IntegerValue *out
) {
    assert(out);
    assert(is_concrete_integer_kind(operation_kind));

    unsigned width;
    uint64_t left_pattern;
    uint64_t right_pattern;

    if (!integer_kind_bit_width(
            operation_kind,
            &width
        ) ||
        !integer_value_to_bit_pattern(
            left,
            operation_kind,
            &left_pattern
        ) ||
        !integer_value_to_bit_pattern(
            right,
            operation_kind,
            &right_pattern
        )) {
        return 0;
    }

    uint64_t result_pattern = 0;

    switch (builtin_kind) {
        case BUILTIN_WRAPPING_ADD:
            result_pattern =
                left_pattern + right_pattern;
            break;

        case BUILTIN_WRAPPING_SUB:
            result_pattern =
                left_pattern - right_pattern;
            break;

        case BUILTIN_WRAPPING_MUL:
            result_pattern =
                left_pattern * right_pattern;
            break;

        case BUILTIN_WRAPPING_NEG:
        case BUILTIN_SIZE_OF:
        case BUILTIN_ALIGN_OF:
        case BUILTIN_SLICE:
        case BUILTIN_NONE:
            UNREACHABLE(
                "non-binary wrapping builtin"
            );
    }

    result_pattern &=
        integer_width_mask(width);

    return integer_value_from_bit_pattern(
        result_pattern,
        operation_kind,
        out
    );
}

/*
 * Wrapping negation computes zero minus the operand modulo 2^N.
 *
 * This is valid for both signed and unsigned concrete integers.
 */
static int evaluate_wrapping_integer_negation(
    IntegerValue operand,
    TypeKind operation_kind,
    IntegerValue *out
) {
    assert(out);
    assert(is_concrete_integer_kind(operation_kind));

    unsigned width;
    uint64_t operand_pattern;

    if (!integer_kind_bit_width(operation_kind, &width) ||
        !integer_value_to_bit_pattern(
            operand,
            operation_kind,
            &operand_pattern
        )) {
        return 0;
    }

    uint64_t result_pattern =
        (UINT64_C(0) - operand_pattern) &
        integer_width_mask(width);

    return integer_value_from_bit_pattern(
        result_pattern,
        operation_kind,
        out
    );
}

typedef enum CheckedIntegerCastStatus {
    CHECKED_INTEGER_CAST_VALID,
    CHECKED_INTEGER_CAST_OUT_OF_RANGE,
} CheckedIntegerCastStatus;

typedef enum CheckedFloatToIntegerCastStatus {
    CHECKED_FLOAT_TO_INTEGER_CAST_VALID,
    CHECKED_FLOAT_TO_INTEGER_CAST_NON_FINITE,
    CHECKED_FLOAT_TO_INTEGER_CAST_OUT_OF_RANGE,
} CheckedFloatToIntegerCastStatus;

/*
 * Classifies a checked conversion of an exact integer value to a
 * concrete integer type.
 *
 * Coglet's ordinary cast preserves the mathematical value. It does
 * not discard high bits, reinterpret the source representation, or
 * reduce the value modulo the destination width.
 */
static CheckedIntegerCastStatus
classify_checked_integer_cast(
    IntegerValue value,
    TypeKind target_kind
) {
    assert(is_concrete_integer_kind(target_kind));

    if (!integer_value_fits_type(value, target_kind))
        return CHECKED_INTEGER_CAST_OUT_OF_RANGE;

    return CHECKED_INTEGER_CAST_VALID;
}

/*
 * Classifies a checked floating-point-to-integer conversion.
 *
 * Coglet:
 *
 * 1. rejects NaN and infinity;
 * 2. truncates finite values toward zero;
 * 3. requires the truncated mathematical integer to fit the
 *    destination type.
 */
static CheckedFloatToIntegerCastStatus
classify_checked_float_to_integer_cast(
    double source,
    TypeKind target_kind,
    IntegerValue *out
) {
    assert(out);
    assert(is_concrete_integer_kind(target_kind));

    if (!isfinite(source))
        return CHECKED_FLOAT_TO_INTEGER_CAST_NON_FINITE;

    int is_negative =
        source < 0.0;

    double magnitude =
        is_negative
            ? -source
            : source;

    /*
     * 2^64 is the first value outside IntegerValue's uint64_t
     * magnitude domain.
     *
     * After proving:
     *
     *     0 <= magnitude < 2^64
     *
     * converting magnitude to uint64_t is defined and truncates
     * toward zero.
     */
    if (magnitude >= 18446744073709551616.0) {
        return
            CHECKED_FLOAT_TO_INTEGER_CAST_OUT_OF_RANGE;
    }

    IntegerValue value =
        integer_value_make(
            (uint64_t)magnitude,
            is_negative
        );

    CheckedIntegerCastStatus integer_status =
        classify_checked_integer_cast(
            value,
            target_kind
        );

    switch (integer_status) {
        case CHECKED_INTEGER_CAST_VALID:
            break;

        case CHECKED_INTEGER_CAST_OUT_OF_RANGE:
            return
                CHECKED_FLOAT_TO_INTEGER_CAST_OUT_OF_RANGE;
    }

    *out = value;

    return CHECKED_FLOAT_TO_INTEGER_CAST_VALID;
}

static int default_integer_kind_for_value(IntegerValue value, TypeKind *out_kind) {

    if (!out_kind) return 0;

    if (integer_value_fits_type(value, TYPE_S32)) {
        *out_kind = TYPE_S32;
        return 1;
    }

    if (integer_value_fits_type(value, TYPE_S64)) {
        *out_kind = TYPE_S64;
        return 1;
    }

    if (integer_value_fits_type(value, TYPE_U64)) {
        *out_kind = TYPE_U64;
        return 1;
    }

    return 0;
}

static Type *untyped_integer_type_for_value(SemanticContext *ctx, IntegerValue value) {

    TypeKind ignored_kind;

    if (!default_integer_kind_for_value(value, &ignored_kind))
        return NULL;

    return new_type(ctx, TYPE_UNTYPED_INT);
}

static double integer_value_to_double(IntegerValue value){

    double result = (double)value.magnitude;
    return value.is_negative ? -result : result;
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
        if (isfinite(value) && (value > COGLET_F32_MAX || value < -COGLET_F32_MAX)) {
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
        right.magnitude > UINT64_MAX / left.magnitude)
        return 0;

    *out = integer_value_make(
        left.magnitude * right.magnitude,
        left.is_negative != right.is_negative
    );

    return 1;
}

typedef enum CheckedIntegerArithmeticStatus {
    CHECKED_INTEGER_ARITHMETIC_VALID,
    CHECKED_INTEGER_ARITHMETIC_OVERFLOW,
} CheckedIntegerArithmeticStatus;

/*
 * Applies Coglet's ordinary checked-arithmetic rule to an exact
 * integer result.
 *
 * The result is valid only when its mathematical value fits the
 * concrete operation type.
 */
static CheckedIntegerArithmeticStatus classify_checked_integer_result(
    IntegerValue value, TypeKind operation_kind) {

    assert(is_concrete_integer_kind(operation_kind));

    if (!integer_value_fits_type(value, operation_kind))
        return CHECKED_INTEGER_ARITHMETIC_OVERFLOW;

    return CHECKED_INTEGER_ARITHMETIC_VALID;
}

/*
 * Evaluates ordinary checked integer addition, subtraction, or
 * multiplication.
 *
 * The operands must already fit the concrete operation type.
 *
 * Failure occurs when either:
 *
 * - the exact sign-and-magnitude result exceeds the constant
 *   evaluator's uint64_t magnitude domain; or
 * - the mathematical result does not fit the operation type.
 *
 * Both conditions represent ordinary Coglet integer overflow.
 */
static CheckedIntegerArithmeticStatus evaluate_checked_integer_binary(
    TokenType operation,
    IntegerValue left,
    IntegerValue right,
    TypeKind operation_kind,
    IntegerValue *out
) {
    assert(out);
    assert(is_concrete_integer_kind(operation_kind));
    assert(integer_value_fits_type(left, operation_kind));
    assert(integer_value_fits_type(right, operation_kind));

    IntegerValue value;
    int exact_result_available;

    switch (operation) {
        case TOK_PLUS:
            exact_result_available =
                integer_value_add(left, right, &value);
            break;

        case TOK_MINUS:
            exact_result_available =
                integer_value_subtract(left, right, &value);
            break;

        case TOK_STAR:
            exact_result_available =
                integer_value_multiply(left, right, &value);
            break;

        default:
            UNREACHABLE(
                "checked integer arithmetic operation"
            );
    }

    if (!exact_result_available)
        return CHECKED_INTEGER_ARITHMETIC_OVERFLOW;

    CheckedIntegerArithmeticStatus status =
        classify_checked_integer_result(value, operation_kind);

    if (status != CHECKED_INTEGER_ARITHMETIC_VALID)
        return status;

    *out = value;

    return CHECKED_INTEGER_ARITHMETIC_VALID;
}

/*
 * Evaluates ordinary signed integer negation.
 *
 * For an already typed operand, semantic analysis guarantees that
 * the operand fits the operation type.
 *
 * An untyped operand is different: its positive mathematical value
 * may not fit the selected operation type even though its negated
 * result does. This is required for:
 *
 *     -9223372036854775808
 *
 * Therefore only the negated result is checked here.
 */
static CheckedIntegerArithmeticStatus
evaluate_checked_integer_negation(
    IntegerValue operand,
    TypeKind operation_kind,
    IntegerValue *out
) {
    assert(out);
    assert(is_signed_integer_kind(operation_kind));

    IntegerValue value =
        integer_value_negated(operand);

    CheckedIntegerArithmeticStatus status =
        classify_checked_integer_result(
            value,
            operation_kind
        );

    if (status != CHECKED_INTEGER_ARITHMETIC_VALID)
        return status;

    *out = value;

    return CHECKED_INTEGER_ARITHMETIC_VALID;
}

static int signed_integer_min_magnitude( TypeKind kind, uint64_t *out) {

    IntegerTypeInfo info;

    if (!out || !integer_type_info(kind, &info) || !info.is_signed) {
        return 0;
    }

    *out = UINT64_C(1) << (info.bit_width - 1);

    return 1;
}

typedef enum IntegerDivisionStatus {
    INTEGER_DIVISION_VALID,
    INTEGER_DIVISION_ZERO_DIVISOR,
    INTEGER_DIVISION_SIGNED_OVERFLOW,
} IntegerDivisionStatus;

/*
 * Classifies the failure conditions shared by integer division
 * and remainder.
 *
 * Coglet traps for both:
 *
 *     SIGNED_MIN / -1
 *     SIGNED_MIN % -1
 *
 * The result type determines whether the signed-overflow case
 * exists and which minimum magnitude applies.
 */
static IntegerDivisionStatus classify_integer_division(IntegerValue left, IntegerValue right, TypeKind result_kind) {

    assert(is_concrete_integer_kind(result_kind));

    if (right.magnitude == 0)
        return INTEGER_DIVISION_ZERO_DIVISOR;

    uint64_t minimum_magnitude;

    if (signed_integer_min_magnitude(
            result_kind,
            &minimum_magnitude
        ) &&
        left.is_negative &&
        left.magnitude == minimum_magnitude &&
        right.is_negative &&
        right.magnitude == 1) {
        return INTEGER_DIVISION_SIGNED_OVERFLOW;
    }

    return INTEGER_DIVISION_VALID;
}

static int is_u8_type(Type *type)    { return type && type->kind == TYPE_U8; }
static int is_integer_type(Type *t)  { return t && is_integer_kind(t->kind); }
static int is_numeric_type(Type *t)  { return t && (is_integer_kind(t->kind) || is_float_kind(t->kind)); }
static int is_bool_type(Type *t)     { return t && t->kind == TYPE_BOOL; }
static int is_enum_type(Type *type)  { return type && type->kind == TYPE_ENUM; }
static int is_null_type(const Type *type) { return type && type->kind == TYPE_NULL;}
static int is_typed_pointer_type(const Type *type) {
    return type && type->kind == TYPE_POINTER;
}
static int is_opaque_pointer_type(const Type *type) {
    return type && type->kind == TYPE_OPAQUE_POINTER;
}
static int is_raw_pointer_type(const Type *type) {
    return is_typed_pointer_type(type) || is_opaque_pointer_type(type);
}
static int is_slice_type(const Type *type) {
    return type && type->kind == TYPE_SLICE;
}

static int is_c_function_pointer_type(const Type *type) {
    return type &&
           type->kind == TYPE_FUNCTION &&
           type->function_abi == FUNCTION_ABI_C;
}
static int is_nullable_pointer_type(const Type *type) {
    return is_raw_pointer_type(type) || is_c_function_pointer_type(type);
}
static int is_bool_cast_pair(Type *to, Type *from)       { return is_bool_type(to) && is_bool_type(from); }
static int is_numeric_cast_pair(Type *to, Type *from)    { return is_numeric_type(to) && is_numeric_type(from); }
static int is_enum_to_integer_cast(Type *to, Type *from) { return is_integer_kind(to->kind) && is_enum_type(from);}
static int is_integer_to_enum_cast(Type *to, Type *from) { return is_enum_type(to) && is_integer_kind(from->kind); }
static int expression_is_compile_time_true(SemanticContext *ctx, Node *node) {
    if (!ctx || !node ||
        !expression_is_compile_time_constant(ctx, node)) {
        return 0;
    }

    ConstValue value;

    if (!eval_const_expr(ctx, node, &value))
        return 0;

    return value.kind == CONST_VALUE_BOOL && value.as.boolean;
}

static int is_equality_comparable_type(const Type *type) {

    if (!type) return 0;

    /* Null comparisons already have their own contextual handling */
    return is_integer_kind(type->kind) ||
           is_float_kind(type->kind) ||
           type->kind == TYPE_BOOL ||
           type->kind == TYPE_ENUM ||
           is_raw_pointer_type(type) ||
           is_c_function_pointer_type(type);
}

static int is_switchable_type(Type *type) {

    if (!type) return 0;

    return is_integer_kind(type->kind) ||
           type->kind == TYPE_BOOL ||
           type->kind == TYPE_ENUM;
}

static int is_null_to_pointer_cast(Type *to, Type *from) {
    return is_nullable_pointer_type(to) && is_null_type(from);
}

/*
 * Compares raw-pointer family and immediate pointee identity while ignoring
 * only the outer pointer's readonly/volatile qualifiers. This is the basis for
 * safe shallow qualifier addition and pointer equality compatibility. Nested
 * pointer qualifiers remain part of the pointee type and therefore must match.
 */
static int pointer_identity_equal_ignoring_qualifiers(
    const Type *left,
    const Type *right
) {
    if (!is_raw_pointer_type(left) || !is_raw_pointer_type(right))
        return 0;

    if (left->kind != right->kind)
        return 0;

    if (left->kind == TYPE_OPAQUE_POINTER)
        return 1;

    return type_equal(left->element, right->element);
}

/*
 * Safe immediate pointer qualification may only add restrictions/observability:
 *
 *     T*                  -> readonly T*
 *     T*                  -> volatile T*
 *     T*                  -> readonly volatile T*
 *     volatile T*         -> readonly volatile T*
 *     readonly T*         -> readonly volatile T*
 *
 * Neither readonly nor volatile may be discarded implicitly. Qualifiers are
 * never added recursively through nested pointer layers.
 */
static int pointer_qualification_conversion_allowed(
    const Type *target,
    const Type *source
) {
    if (!pointer_identity_equal_ignoring_qualifiers(target, source))
        return 0;

    if (source->pointer_access == POINTER_ACCESS_READONLY &&
        target->pointer_access == POINTER_ACCESS_MUTABLE)
        return 0;

    if (source->pointer_is_volatile && !target->pointer_is_volatile)
        return 0;

    return target->pointer_access != source->pointer_access ||
           target->pointer_is_volatile != source->pointer_is_volatile;
}

static int slice_qualification_conversion_allowed(
    const Type *target,
    const Type *source
) {
    if (!is_slice_type(target) || !is_slice_type(source) ||
        !type_equal(target->element, source->element)) {
        return 0;
    }

    return source->pointer_access == POINTER_ACCESS_MUTABLE &&
           target->pointer_access == POINTER_ACCESS_READONLY;
}

static int array_to_slice_conversion_allowed(
    const Type *target,
    const Type *source
) {
    return is_slice_type(target) && source && source->kind == TYPE_ARRAY &&
           source->array_size >= 0 &&
           type_equal(target->element, source->element);
}

static SemContextConversionKind classify_context_conversion(
    Type *target,
    Type *source
) {
    if (!target || !source || type_equal(target, source))
        return SEM_CONTEXT_CONVERSION_NONE;

    if (source->kind == TYPE_UNTYPED_INT) {
        if (is_concrete_integer_kind(target->kind))
            return SEM_CONTEXT_CONVERSION_INT_MATERIALIZE;

        if (is_concrete_float_kind(target->kind))
            return SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE;
    }

    if (source->kind == TYPE_UNTYPED_FLOAT &&
        is_concrete_float_kind(target->kind)) {
        return SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE;
    }

    if (is_null_type(source) && is_nullable_pointer_type(target))
        return SEM_CONTEXT_CONVERSION_NULL_TO_POINTER;

    if (pointer_qualification_conversion_allowed(target, source))
        return SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION;

    if (slice_qualification_conversion_allowed(target, source))
        return SEM_CONTEXT_CONVERSION_SLICE_QUALIFICATION;

    if (array_to_slice_conversion_allowed(target, source))
        return SEM_CONTEXT_CONVERSION_ARRAY_TO_SLICE;

    return SEM_CONTEXT_CONVERSION_NONE;
}

static void sem_record_context_conversion_if_needed(
    SemanticContext *ctx,
    Node *node,
    Type *target,
    Type *source
) {
    SemContextConversionKind conversion =
        classify_context_conversion(target, source);

    if (conversion == SEM_CONTEXT_CONVERSION_NONE)
        return;

    sem_record_context_conversion(
        ctx,
        node,
        target,
        conversion
    );
}

/*
 * Pointer equality does not access the pointee, so immediate readonly/volatile
 * qualifier differences do not make otherwise identical raw pointers
 * incomparable.
 */
static int pointer_equality_compatible(const Type *left, const Type *right) {
    return pointer_identity_equal_ignoring_qualifiers(left, right);
}

static int reinterpret_pointer_conversion_allowed(
    const Type *target,
    const Type *source
) {
    if (!is_raw_pointer_type(target) || !is_raw_pointer_type(source))
        return 0;

    /*
     * `reinterpret` is deliberately not a general T* -> U* operation.
     * Exactly one side must be the top-level opaque pointer kind.
     */
    if (is_opaque_pointer_type(target) == is_opaque_pointer_type(source))
        return 0;

    /* Never recover write permission from a readonly address. */
    if (source->pointer_access == POINTER_ACCESS_READONLY &&
        target->pointer_access == POINTER_ACCESS_MUTABLE) {
        return 0;
    }

    /* Never discard volatile access semantics. */
    if (source->pointer_is_volatile && !target->pointer_is_volatile)
        return 0;

    return 1;
}

static int is_allowed_explicit_cast(Type *to, Type *from) {

    if (!to || !from) return 0;

    /*
     * Casting a type to itself is always allowed.
     */
    if (type_equal(to, from))
        return 1;

    /*
     * Explicitly removing write permission is safe and preserves
     * the pointer value:
     *
     *     cast(readonly T*, mutable_pointer)
     *
     * The reverse conversion remains invalid.
     */
    if (pointer_qualification_conversion_allowed(to, from))
        return 1;

    /*
     * A null literal may be given an explicit concrete pointer type:
     *
     *     cast(s32*, null)
     *     cast(readonly s32*, null)
     *
     * The reverse conversion is not allowed, and integers do not
     * become pointers through this rule.
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
     *
     *     s32 -> s64
     *     s64 -> u8
     *     f64 -> s32
     *     s32 -> f64
     */
    if (is_numeric_cast_pair(to, from))
        return 1;

    /*
     * Enum backing-value casts:
     *
     *     raw: u16 = cast(u16, Color.Green);
     *     color: Color = cast(Color, 0);
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
        (is_nullable_pointer_type(left) &&
         right->kind == TYPE_NULL) ||
        (left->kind == TYPE_NULL &&
         is_nullable_pointer_type(right));
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
        case TYPE_S8:
        case TYPE_U8:
            return 1;

        case TYPE_S16:
        case TYPE_U16:
            return 2;

        case TYPE_S32:
        case TYPE_U32:
            return 3;

        case TYPE_S64:
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
    return is_nullable_pointer_type(expected) &&
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
 *   - A mutable pointer may adapt to a readonly pointer with the same
 *     immediate pointee type.
 *   - A null literal contextually adapts to any raw pointer type.
 *   - An untyped integer may adapt to a concrete integer or floating-point
 *     type.
 *   - An untyped floating-point value may adapt to a concrete floating-point
 *     type.
 *   - Concrete numeric types do not implicitly widen or narrow.
 *
 * Readonly pointer access cannot implicitly become mutable pointer access.
 * Readonly access is not recursively added through nested pointers.
 *
 * Integer zero is not a null-pointer constant. The only source-level null
 * pointer value is `null`.
 */
static int initializer_compatible(Type *declared, Type *init_type) {

    if (!declared || !init_type)
        return 1;

    if (type_equal(declared, init_type))
        return 1;

    if (pointer_qualification_conversion_allowed(declared, init_type))
        return 1;

    if (slice_qualification_conversion_allowed(declared, init_type) ||
        array_to_slice_conversion_allowed(declared, init_type)) {
        return 1;
    }

    /*
     * A null literal contextually adapts to any raw pointer type:
     *
     *     T* <- null
     *     readonly T* <- null
     *
     * TYPE_NULL is not globally equal to TYPE_POINTER.
     */
    if (is_nullable_pointer_type(declared) && is_null_type(init_type)) {
        return 1;
    }

    if (init_type->kind == TYPE_UNTYPED_INT) {
        return is_concrete_integer_kind(declared->kind) ||
               is_concrete_float_kind(declared->kind);
    }

    if (init_type->kind == TYPE_UNTYPED_FLOAT) {
        return is_concrete_float_kind(declared->kind);
    }

    return 0;
}

static int check_array_to_slice_source_access(
    SemanticContext *ctx,
    Type *target,
    Type *source,
    Node *expression
) {
    if (!array_to_slice_conversion_allowed(target, source))
        return 1;

    SemExprInfo *info = sem_find_expr_info(ctx, expression);
    if (!info || info->value_category != VALUE_CATEGORY_LVALUE) {
        semantic_error(
            ctx,
            expression,
            "array-to-slice conversion requires addressable array storage"
        );
        return 0;
    }

    if (info->value_is_volatile) {
        semantic_error(
            ctx,
            expression,
            "cannot create slice from volatile array storage"
        );
        return 0;
    }

    if (target->pointer_access == POINTER_ACCESS_MUTABLE &&
        info->value_access != VALUE_ACCESS_WRITABLE) {
        semantic_error(
            ctx,
            expression,
            "cannot create mutable slice from readonly array storage"
        );
        return 0;
    }

    return 1;
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
         *     NONE: s32* : null;
         *     IS_NONE :: NONE == null;
         *
         * Both operands evaluate to CONST_VALUE_NULL even though
         * their semantic types are s32* and TYPE_NULL.
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
    ConstValue value;

    if (!eval_const_expr_impl(ctx, node, &value))
        return 0;

    *out = value;

    /*
     * Cache the intrinsic result while lexical scope is still available.
     * Later compiler stages must not re-run constant evaluation because local
     * scopes have been popped by the time semantic_check() returns.
     */
    SemExprInfo *info = sem_get_or_create_expr_info(ctx, node);
    info->has_constant_value = 1;
    info->constant_value = value;

    return 1;
}

static int eval_const_expr_impl(SemanticContext *ctx, Node *node, ConstValue *out) {

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
                out->as.integer =
                    integer_value_make(node->as.number.value.integer, 0);

                out->type =
                    untyped_integer_type_for_value(ctx,out->as.integer);
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

        case NODE_CALL:
            return eval_const_builtin_call(ctx, node, out);

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

            if (!ensure_constant_symbol_checked(ctx, symbol))
                return 0;

            SemDeclInfo *declaration =
                sem_find_decl_info_by_id(ctx, symbol->declaration_id);

            if (!declaration || !declaration->has_constant_value)
                return 0;

            *out = declaration->constant_value;
            return 1;
        }

        case NODE_FIELD:
        {
            Symbol *qualified_enum = NULL;
            EnumMember *qualified_member = NULL;
            if (semantic_qualified_enum_member_no_diag(
                    ctx,
                    node,
                    &qualified_enum,
                    &qualified_member
                )) {
                out->kind = CONST_VALUE_INT;
                out->as.integer = qualified_member->value;
                out->type = qualified_enum->type;
                return 1;
            }

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

            /*
             * Module qualification is resolved from the complete canonical
             * dotted chain so constants in hierarchical modules remain usable
             * during declaration-time evaluation.
             */
            int qualified_recognized = 0;
            Symbol *qualified = semantic_lookup_qualified_field_symbol(
                ctx,
                node,
                1,
                NULL,
                NULL,
                &qualified_recognized
            );
            if (qualified_recognized) {
                if (!qualified)
                    return 0;

                if (qualified->kind == SYMBOL_CONSTANT) {
                    if (!ensure_constant_symbol_checked(ctx, qualified))
                        return 0;

                    SemDeclInfo *declaration =
                        sem_find_decl_info_by_id(ctx, qualified->declaration_id);
                    if (!declaration || !declaration->has_constant_value)
                        return 0;

                    *out = declaration->constant_value;
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
                 * unsigned expressions. Coglet does not define ordinary
                 * wrapping unary negation for unsigned integers.
                 */
                if (result_type &&
                    is_unsigned_integer_kind(result_type->kind)) {
                    semantic_error(
                        ctx,
                        node,
                        "unary '-' cannot be applied to an unsigned value"
                    );

                    return 0;
                }

                /*
                 * The mathematical result is needed before selecting the
                 * provisional type of untyped integer expression.
                 *
                 * This preserves the special but valid spelling:
                 *
                 *     -9223372036854775808
                 *
                 * The positive literal is representable in the exact untyped
                 * domain, and its negated result receives s64 as its default
                 * concrete type.
                 */
                IntegerValue mathematical_value =
                    integer_value_negated(operand.as.integer);

                TypeKind operation_kind;

            if (result_type &&
                result_type->kind == TYPE_UNTYPED_INT) {
                if (!default_integer_kind_for_value(
                        mathematical_value,
                        &operation_kind
                    )) {
                    semantic_error(
                        ctx,
                        node,
                        "integer overflow in constant expression"
                    );

                    return 0;
                }
            } else if (
                result_type &&
                is_signed_integer_kind(result_type->kind)
            ) {
                operation_kind = result_type->kind;
            } else {
                semantic_error(
                    ctx,
                    node,
                    "integer constant expression has no integer type"
                );

                return 0;
            }

                IntegerValue value;

                CheckedIntegerArithmeticStatus status =
                    evaluate_checked_integer_negation(
                        operand.as.integer,
                        operation_kind,
                        &value);

                switch (status) {
                    case CHECKED_INTEGER_ARITHMETIC_VALID:
                        break;

                    case CHECKED_INTEGER_ARITHMETIC_OVERFLOW:
                        semantic_error(
                            ctx,
                            node,
                            "integer overflow in constant expression"
                        );

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
                         * width. For example, `1 << count` uses s32.
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
                                operation_kind) ||
                            !integer_value_fits_type(
                                right.as.integer,
                                operation_kind)) {

                            semantic_error(ctx, node,
                                "integer constant operand does not fit operation type");

                            return 0;
                        }

                        IntegerValue value;

                        switch (node->as.binary.op) {
                            case TOK_PLUS:
                            case TOK_MINUS:
                            case TOK_STAR:
                            {
                                CheckedIntegerArithmeticStatus status =
                                    evaluate_checked_integer_binary(
                                        node->as.binary.op,
                                        left.as.integer,
                                        right.as.integer,
                                        operation_kind,
                                        &value);

                                switch (status) {
                                    case CHECKED_INTEGER_ARITHMETIC_VALID:
                                        break;

                                    case CHECKED_INTEGER_ARITHMETIC_OVERFLOW:
                                        semantic_error(
                                            ctx,
                                            node,
                                            "integer overflow in constant expression"
                                        );

                                        return 0;
                                }

                                break;
                            }

                            case TOK_SLASH:
                            case TOK_PERCENT:
                            {
                                IntegerDivisionStatus status =
                                    classify_integer_division(
                                        left.as.integer,
                                        right.as.integer,
                                        operation_kind
                                    );

                                switch (status) {
                                    case INTEGER_DIVISION_VALID:
                                        break;

                                    case INTEGER_DIVISION_ZERO_DIVISOR:
                                        semantic_error(ctx,node,
                                            node->as.binary.op == TOK_SLASH
                                                ? "division by zero in constant expression"
                                                : "remainder by zero in constant expression");

                                        return 0;

                                    case INTEGER_DIVISION_SIGNED_OVERFLOW:
                                        semantic_error(ctx,node,
                                            "integer overflow in constant expression");

                                        return 0;
                                }

                                if (node->as.binary.op == TOK_SLASH) {
                                    value = integer_value_make(
                                        left.as.integer.magnitude /
                                            right.as.integer.magnitude,
                                        left.as.integer.is_negative !=
                                            right.as.integer.is_negative
                                    );
                                } else {
                                    value = integer_value_make(
                                        left.as.integer.magnitude %
                                            right.as.integer.magnitude,
                                        left.as.integer.is_negative
                                    );
                                }

                                break;
                            }

                            default:
                                UNREACHABLE("integer constant arithmetic operator");
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
        return NULL;
    }

    sem_record_context_conversion_if_needed(
        ctx,
        expression,
        concrete,
        type
    );

    return concrete;
}

static int eval_const_checked_cast(SemanticContext *ctx, Node *node, ConstValue *out) {

    ConstValue value;

    if (!eval_const_expr(ctx, node->as.cast_expr.expression, &value))
        return 0;

    Type *target_type = resolve_type(
        ctx,
        node->as.cast_expr.target_type,
        node
    );

    if (!target_type) return 0;

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
     * A null-to-pointer cast remains a null constant but carries
     * the concrete destination pointer type.
     */
    if (is_raw_pointer_type(target_type)) {
        if (value.kind != CONST_VALUE_NULL) {
            semantic_error(ctx, node,
                "pointer cast requires a null constant");

            return 0;
        }

        out->kind = CONST_VALUE_NULL;
        return 1;
    }

    /*
     * Closed enum construction has two requirements:
     *
     * 1. the integer value must fit the enum backing type;
     * 2. the value must equal a declared runtime member value.
     */
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

        assert(is_concrete_integer_kind(target_type->kind));

        /*
         * Integer and enum constants already carry an exact
         * mathematical IntegerValue. Checked casting only needs to
         * prove that this value fits the destination integer type.
         */
        if (value.kind == CONST_VALUE_INT) {
            CheckedIntegerCastStatus status =
                classify_checked_integer_cast(
                    value.as.integer,
                    target_type->kind
                );

            switch (status) {
                case CHECKED_INTEGER_CAST_VALID:
                    break;

                case CHECKED_INTEGER_CAST_OUT_OF_RANGE:
                    semantic_error(ctx, node,
                        "integer cast value does not fit in target type");

                    return 0;
            }

            out->kind = CONST_VALUE_INT;
            out->as.integer = value.as.integer;

            return 1;
        }

        /*
        * Checked floating-point-to-integer conversion:
        *
        * 1. reject NaN and infinity;
        * 2. truncate finite values toward zero;
        * 3. require the truncated mathematical integer to fit the
        *    destination type.
         */
        if (value.kind == CONST_VALUE_FLOAT) {
            IntegerValue integer_value;

            CheckedFloatToIntegerCastStatus status =
                classify_checked_float_to_integer_cast(
                    value.as.floating,
                    target_type->kind,
                    &integer_value
                );

            switch (status) {
                case CHECKED_FLOAT_TO_INTEGER_CAST_VALID:
                    break;

                case CHECKED_FLOAT_TO_INTEGER_CAST_NON_FINITE:
                case CHECKED_FLOAT_TO_INTEGER_CAST_OUT_OF_RANGE:
                    semantic_error(
                        ctx,
                        node,
                        "integer cast value does not fit in target type"
                    );

                    return 0;
            }

            out->kind = CONST_VALUE_INT;
            out->as.integer = integer_value;

            return 1;
        }

        semantic_error(ctx, node,
            "integer cast requires numeric constant");

        return 0;
    }

    if (is_float_kind(target_type->kind)) {
        double float_value;

        if (!const_value_to_float_type(
                &value,
                target_type->kind,
                &float_value)) {
            semantic_error(ctx,node,
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

static int eval_const_truncating_cast(SemanticContext *ctx,Node *node,ConstValue *out) {

    assert(node);
    assert(node->type == NODE_CAST);
    assert(node->as.cast_expr.kind == CAST_TRUNCATING);

    ConstValue value;

    if (!eval_const_expr(
            ctx,
            node->as.cast_expr.expression,
            &value
        )) {
        return 0;
        }

    Type *target_type =
        resolve_type(
            ctx,
            node->as.cast_expr.target_type,
            node
        );

    if (!target_type)
        return 0;

    if (!is_concrete_integer_kind(target_type->kind)) {
        semantic_error(ctx, node,
            "truncate target must be a concrete integer type");

        return 0;
    }

    if (value.kind != CONST_VALUE_INT) {
        semantic_error(ctx, node,
            "truncate source must be an integer");

        return 0;
    }

    IntegerValue result;

    if (!truncate_integer_value(
            value.as.integer,
            target_type->kind,
            &result
        )) {
        semantic_error(ctx, node,
            "could not evaluate truncating integer conversion");

        return 0;
    }

    memset(out, 0, sizeof(*out));

    out->kind       = CONST_VALUE_INT;
    out->as.integer = result;
    out->type       = target_type;

    return 1;
}

static int eval_const_cast(SemanticContext *ctx, Node *node, ConstValue *out) {

    assert(node);
    assert(node->type == NODE_CAST);

    switch (node->as.cast_expr.kind) {
        case CAST_CHECKED:
            return eval_const_checked_cast(
                ctx,
                node,
                out
            );

        case CAST_TRUNCATING:
            return eval_const_truncating_cast(
                ctx,
                node,
                out
            );

        case CAST_REINTERPRET:
            semantic_error(ctx, node,
                "reinterpret is not a compile-time constant conversion");
            return 0;
    }

    UNREACHABLE("CastKind");
}

static int try_coerce_constant_to_type(
    const ConstValue *value,
    Type *target_type,
    ConstValue *out
) {
    assert(value);
    assert(target_type);
    assert(out);

    *out = *value;
    out->type = target_type;

    if (is_integer_kind(target_type->kind)) {
        return value->kind == CONST_VALUE_INT &&
               integer_value_fits_type(
                   value->as.integer,
                   target_type->kind
               );
    }

    if (is_float_kind(target_type->kind)) {
        double float_value;

        if (!const_value_to_float_type(
                value,
                target_type->kind,
                &float_value)) {
            return 0;
        }

        out->kind = CONST_VALUE_FLOAT;
        out->as.floating = float_value;
        return 1;
    }

    /*
     * Non-numeric constant conversions preserve the payload and only attach
     * the already-approved semantic destination type. This covers enum values
     * and null-to-pointer contextualization.
     */
    return 1;
}

static int coerce_constant_to_type(
    SemanticContext *ctx,
    Node *node,
    const ConstValue *value,
    Type *target_type,
    const char *integer_range_message,
    const char *float_range_message,
    ConstValue *out) {

    if (try_coerce_constant_to_type(
            value,
            target_type,
            out)) {
        return 1;
    }

    semantic_error(
        ctx,
        node,
        is_float_kind(target_type->kind)
            ? float_range_message
            : integer_range_message
    );

    return 0;
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

        sem_record_context_conversion_if_needed(
            ctx,
            operands[i],
            operation_type,
            operand_types[i]
        );
    }

    return 1;
}

static int check_known_integer_divisor(
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
            if (node->as.cast_expr.kind == CAST_REINTERPRET)
                return 0;

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

        case NODE_CALL:
        {
            Node *callee =
                node->as.call.callee;

            if (!callee ||
                callee->type != NODE_IDENT) {
                return 0;
                }

            Symbol *symbol =
                scope_lookup(
                    ctx->current_scope,
                    callee->as.ident.data,
                    callee->as.ident.length
                );

            if (!symbol || symbol->kind != SYMBOL_BUILTIN)
                return 0;

            switch (symbol->builtin_kind) {
                case BUILTIN_WRAPPING_ADD:
                case BUILTIN_WRAPPING_SUB:
                case BUILTIN_WRAPPING_MUL:
                case BUILTIN_WRAPPING_NEG:
                    break;

                case BUILTIN_SIZE_OF:
                case BUILTIN_ALIGN_OF:
                case BUILTIN_SLICE:
                case BUILTIN_NONE:
                    return 0;
            }

            for (int i = 0; i < node->as.call.arguments.count; i++) {
                if (!expression_is_compile_time_constant(
                        ctx,
                        node->as.call.arguments.items[i]
                    )) {
                    return 0;
                }
            }

            return 1;
        }

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
            /* Both `SomeEnum.Member` and `module.SomeEnum.Member`. */
            if (semantic_qualified_enum_member_no_diag(
                    ctx, node, NULL, NULL
                )) {
                return 1;
            }

            if (node->as.field.object &&
                node->as.field.object->type == NODE_IDENT) {
                Node *object_node = node->as.field.object;
                Symbol *sym = scope_lookup(
                    ctx->current_scope,
                    object_node->as.ident.data,
                    object_node->as.ident.length
                );

                if (sym) {
                    return sym->kind == SYMBOL_TYPE &&
                           sym->type &&
                           sym->type->kind == TYPE_ENUM &&
                           find_enum_member(
                               sym->type,
                               node->as.field.name.data,
                               node->as.field.name.length
                           );
                }
            }

            Symbol *member = semantic_lookup_qualified_field_symbol(
                ctx, node, 0, NULL, NULL, NULL);
            return member && member->kind == SYMBOL_CONSTANT;
        }

        default:
            return 0;
    }
}

static int check_string_initializer(SemanticContext *ctx, Type *expected, Node *initializer) {

    if (!expected || !initializer)
        return 0;

    if (initializer->type != NODE_STRING) {
        semantic_error(ctx, initializer, "internal error: expected string literal");
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

    if (expected->kind == TYPE_SLICE) {
        if (!is_u8_type(expected->element)) {
            semantic_error(ctx, initializer,
                "string literal slice destination must have element type u8");
            return 0;
        }

        if (expected->pointer_access != POINTER_ACCESS_READONLY) {
            semantic_error(ctx, initializer,
                "string literal can only initialize readonly u8[]");
            return 0;
        }

        return 1;
    }

    if (expected->kind != TYPE_ARRAY) {
        semantic_error(ctx, initializer,
            "string literal requires a u8 array or readonly u8[] slice");
        return 0;
    }

    if (!is_u8_type(expected->element)) {
        semantic_error(ctx, initializer, "string literal destination must be u8 array");
        return 0;
    }

    /* Fixed-array string initialization includes the trailing NUL byte. */
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

typedef enum {
    IDENTIFIER_USE_READ,
    IDENTIFIER_USE_WRITE_TARGET,
} IdentifierUse;


static GenericTypeParameterList generic_decl_type_parameters(const Node *decl)
{
    if (!decl)
        return (GenericTypeParameterList){0};
    if (decl->type == NODE_FUNC_DECL)
        return decl->as.func_decl.type_parameters;
    if (decl->type == NODE_STRUCT_DECL)
        return decl->as.struct_decl.type_parameters;
    return (GenericTypeParameterList){0};
}

static StringView generic_decl_name(const Node *decl)
{
    if (!decl)
        return string_view_empty();
    if (decl->type == NODE_FUNC_DECL)
        return decl->as.func_decl.name;
    if (decl->type == NODE_STRUCT_DECL)
        return decl->as.struct_decl.name;
    return string_view_empty();
}

static int generic_type_parameter_index(const Node *decl, StringView name)
{
    GenericTypeParameterList parameters = generic_decl_type_parameters(decl);
    for (int i = 0; i < parameters.count; i++) {
        if (string_view_equals(parameters.items[i].name, name))
            return i;
    }
    return -1;
}

static int source_type_is_generic_parameter(const Node *decl, const Type *type, int *out_index)
{
    if (!type || type->kind != TYPE_NAMED || type->named_module.length != 0 ||
        type->type_argument_count != 0)
        return 0;

    int index = generic_type_parameter_index(decl, type->named_name);
    if (index < 0)
        return 0;

    if (out_index)
        *out_index = index;
    return 1;
}

static int source_type_contains_generic_parameter(const Node *decl, const Type *type)
{
    if (!type)
        return 0;
    if (source_type_is_generic_parameter(decl, type, NULL))
        return 1;

    switch (type->kind) {
        case TYPE_POINTER:
        case TYPE_ARRAY:
        case TYPE_SLICE:
            return source_type_contains_generic_parameter(decl, type->element);

        case TYPE_FUNCTION:
            for (int i = 0; i < type->parameter_count; i++) {
                if (source_type_contains_generic_parameter(decl, type->parameters[i]))
                    return 1;
            }
            return source_type_contains_generic_parameter(decl, type->return_type);

        case TYPE_NAMED:
            for (int i = 0; i < type->type_argument_count; i++) {
                if (source_type_contains_generic_parameter(decl, type->type_arguments[i]))
                    return 1;
            }
            return 0;

        default:
            return 0;
    }
}

typedef enum GenericBuiltinConstraint {
    GENERIC_CONSTRAINT_NONE,
    GENERIC_CONSTRAINT_INTEGER,
    GENERIC_CONSTRAINT_SIGNED_INTEGER,
    GENERIC_CONSTRAINT_UNSIGNED_INTEGER,
    GENERIC_CONSTRAINT_FLOATING,
    GENERIC_CONSTRAINT_NUMERIC,
    GENERIC_CONSTRAINT_ORDERED,
    GENERIC_CONSTRAINT_INVALID,
} GenericBuiltinConstraint;

static GenericBuiltinConstraint generic_builtin_constraint(StringView name)
{
    if (string_view_is_empty(name))
        return GENERIC_CONSTRAINT_NONE;
    if (string_view_equals_cstr(name, "integer"))
        return GENERIC_CONSTRAINT_INTEGER;
    if (string_view_equals_cstr(name, "signed_integer"))
        return GENERIC_CONSTRAINT_SIGNED_INTEGER;
    if (string_view_equals_cstr(name, "unsigned_integer"))
        return GENERIC_CONSTRAINT_UNSIGNED_INTEGER;
    if (string_view_equals_cstr(name, "floating"))
        return GENERIC_CONSTRAINT_FLOATING;
    if (string_view_equals_cstr(name, "numeric"))
        return GENERIC_CONSTRAINT_NUMERIC;
    if (string_view_equals_cstr(name, "ordered"))
        return GENERIC_CONSTRAINT_ORDERED;
    return GENERIC_CONSTRAINT_INVALID;
}

static int type_satisfies_generic_constraint(
    const Type *type,
    GenericBuiltinConstraint constraint
) {
    if (!type)
        return 0;

    switch (constraint) {
        case GENERIC_CONSTRAINT_NONE:
            return 1;

        case GENERIC_CONSTRAINT_INTEGER:
            return is_concrete_integer_kind(type->kind);

        case GENERIC_CONSTRAINT_SIGNED_INTEGER:
            return is_signed_integer_kind(type->kind);

        case GENERIC_CONSTRAINT_UNSIGNED_INTEGER:
            return is_unsigned_integer_kind(type->kind);

        case GENERIC_CONSTRAINT_FLOATING:
            return is_concrete_float_kind(type->kind);

        case GENERIC_CONSTRAINT_NUMERIC:
        case GENERIC_CONSTRAINT_ORDERED:
            /*
             * Coglet's current arithmetic and ordered-comparison domains are
             * exactly the concrete integer and floating-point types. Keep the
             * names distinct so a declaration states its intent without
             * granting operations that ordinary semantic analysis would reject.
             */
            return is_concrete_integer_kind(type->kind) ||
                   is_concrete_float_kind(type->kind);

        case GENERIC_CONSTRAINT_INVALID:
            return 0;
    }

    return 0;
}

static int check_generic_type_argument_constraints(
    SemanticContext *ctx,
    Node *template_decl,
    Type *const *type_arguments,
    int type_argument_count,
    Node *call_site
) {
    GenericTypeParameterList parameters = generic_decl_type_parameters(template_decl);
    int parameter_count = parameters.count;
    int count = parameter_count < type_argument_count
        ? parameter_count
        : type_argument_count;

    for (int i = 0; i < count; i++) {
        GenericTypeParameter parameter =
            parameters.items[i];
        GenericBuiltinConstraint constraint =
            generic_builtin_constraint(parameter.constraint);

        /* Template declaration checking rejects unknown constraint names. */
        assert(constraint != GENERIC_CONSTRAINT_INVALID);

        if (type_satisfies_generic_constraint(type_arguments[i], constraint))
            continue;

        char type_name[192];
        format_type_name(type_arguments[i], type_name, sizeof(type_name));
        semantic_error_fmt(
            ctx,
            call_site,
            "cannot instantiate '%.*s': type argument %.*s = %s does not satisfy constraint '%.*s'",
            (int)generic_decl_name(template_decl).length,
            generic_decl_name(template_decl).data,
            (int)parameter.name.length,
            parameter.name.data,
            type_name,
            (int)parameter.constraint.length,
            parameter.constraint.data
        );
        return 0;
    }

    return 1;
}

static int symbol_is_generic_template(const Symbol *symbol)
{
    return symbol &&
        symbol->kind == SYMBOL_FUNCTION &&
        symbol->declaration &&
        symbol->declaration->type == NODE_FUNC_DECL &&
        symbol->declaration->as.func_decl.type_parameters.count > 0;
}

static int symbol_is_generic_struct_template(const Symbol *symbol)
{
    return symbol &&
        symbol->kind == SYMBOL_TYPE &&
        symbol->declaration &&
        symbol->declaration->type == NODE_STRUCT_DECL &&
        symbol->declaration->as.struct_decl.type_parameters.count > 0;
}

static Symbol *resolve_generic_template_callee_no_diag(SemanticContext *ctx, Node *callee)
{
    if (!callee)
        return NULL;

    if (callee->type == NODE_IDENT) {
        Symbol *symbol = scope_lookup(
            ctx->current_scope,
            callee->as.ident.data,
            callee->as.ident.length
        );
        return symbol_is_generic_template(symbol) ? symbol : NULL;
    }

    if (callee->type == NODE_FIELD) {
        SemanticModule *module = NULL;
        StringView member_name = string_view_empty();
        int recognized = 0;
        Symbol *symbol = semantic_lookup_qualified_field_symbol(
            ctx,
            callee,
            0,
            &module,
            &member_name,
            &recognized
        );
        (void)module;
        (void)member_name;
        return recognized && symbol_is_generic_template(symbol) ? symbol : NULL;
    }

    return NULL;
}

static void format_generic_bindings(
    const Node *template_decl,
    Type *const *type_arguments,
    int type_argument_count,
    char *buffer,
    size_t buffer_size
) {
    if (!buffer || buffer_size == 0)
        return;

    buffer[0] = '\0';
    size_t used = 0;
    GenericTypeParameterList parameters = generic_decl_type_parameters(template_decl);
    int count = parameters.count;
    if (type_argument_count < count)
        count = type_argument_count;

    for (int i = 0; i < count; i++) {
        char type_name[192];
        format_type_name(type_arguments[i], type_name, sizeof(type_name));
        StringView parameter = parameters.items[i].name;

        int written = snprintf(
            buffer + used,
            used < buffer_size ? buffer_size - used : 0,
            "%s%.*s = %s",
            i == 0 ? "" : ", ",
            (int)parameter.length,
            parameter.data,
            type_name
        );
        if (written < 0)
            return;
        size_t added = (size_t)written;
        if (used + added >= buffer_size) {
            buffer[buffer_size - 1] = '\0';
            return;
        }
        used += added;
    }
}

static StringView make_generic_specialization_name(
    SemanticContext *ctx,
    const Node *template_decl,
    Type *const *type_arguments,
    int type_argument_count
) {
    char concrete_types[1024];
    concrete_types[0] = '\0';
    size_t used = 0;

    for (int i = 0; i < type_argument_count; i++) {
        char type_name[192];
        format_type_name(type_arguments[i], type_name, sizeof(type_name));
        int written = snprintf(
            concrete_types + used,
            used < sizeof(concrete_types) ? sizeof(concrete_types) - used : 0,
            "%s%s",
            i == 0 ? "" : ", ",
            type_name
        );
        if (written < 0)
            break;
        size_t added = (size_t)written;
        if (used + added >= sizeof(concrete_types)) {
            used = sizeof(concrete_types) - 1;
            concrete_types[used] = '\0';
            break;
        }
        used += added;
    }

    StringView template_name = generic_decl_name(template_decl);
    size_t name_length = template_name.length;
    size_t types_length = strlen(concrete_types);
    size_t total = name_length + 1 + types_length + 1;
    char *text = arena_alloc(ctx->arena, total);
    memcpy(text, template_name.data, name_length);
    text[name_length] = '<';
    memcpy(text + name_length + 1, concrete_types, types_length);
    text[total - 1] = '>';
    return string_view(text, total);
}

static GenericSpecialization *find_generic_specialization(
    SemanticContext *ctx,
    SemDeclId template_id,
    Type *const *type_arguments,
    int type_argument_count
) {
    for (GenericSpecialization *spec = ctx->generic_specializations; spec; spec = spec->next) {
        if (spec->template_id != template_id ||
            spec->type_argument_count != type_argument_count) {
            continue;
        }

        int equal = 1;
        for (int i = 0; i < type_argument_count; i++) {
            if (!type_equal(spec->type_arguments[i], type_arguments[i])) {
                equal = 0;
                break;
            }
        }
        if (equal)
            return spec;
    }
    return NULL;
}

static int active_generic_template_depth(SemanticContext *ctx, SemDeclId template_id)
{
    int depth = 0;
    for (GenericSpecialization *spec = ctx->active_generic_specialization;
         spec;
         spec = spec->active_parent) {
        if (spec->template_id == template_id)
            depth++;
    }
    return depth;
}

static void define_generic_type_aliases(
    SemanticContext *ctx,
    const Node *template_decl,
    Type *const *type_arguments
) {
    GenericTypeParameterList parameters = generic_decl_type_parameters(template_decl);
    for (int i = 0; i < parameters.count; i++) {
        scope_define(ctx, parameters.items[i].name, SYMBOL_TYPE, type_arguments[i]);
    }
}

static GenericStructSpecialization *find_generic_struct_specialization(
    SemanticContext *ctx,
    SemDeclId template_id,
    Type *const *type_arguments,
    int type_argument_count
) {
    for (GenericStructSpecialization *spec = ctx->generic_struct_specializations;
         spec;
         spec = spec->next) {
        if (spec->template_id != template_id ||
            spec->type_argument_count != type_argument_count) {
            continue;
        }
        int equal = 1;
        for (int i = 0; i < type_argument_count; i++) {
            if (!type_equal(spec->type_arguments[i], type_arguments[i])) {
                equal = 0;
                break;
            }
        }
        if (equal)
            return spec;
    }
    return NULL;
}

static int active_generic_struct_template_depth(
    SemanticContext *ctx,
    SemDeclId template_id
) {
    int depth = 0;
    for (GenericStructSpecialization *spec = ctx->active_generic_struct_specialization;
         spec;
         spec = spec->active_parent) {
        if (spec->template_id == template_id)
            depth++;
    }
    return depth;
}

static Type *resolve_generic_struct_application(
    SemanticContext *ctx,
    Symbol *symbol,
    Type *const *source_arguments,
    int source_argument_count,
    Node *use_node
) {
    if (!symbol || symbol->kind != SYMBOL_TYPE)
        return NULL;

    if (!symbol_is_generic_struct_template(symbol)) {
        if (source_argument_count > 0) {
            semantic_error_fmt(
                ctx,
                use_node,
                "type '%.*s' is not generic",
                (int)symbol->name.length,
                symbol->name.data
            );
            return NULL;
        }
        return symbol->type;
    }

    Node *template_decl = symbol->declaration;
    GenericTypeParameterList parameters = generic_decl_type_parameters(template_decl);
    if (source_argument_count == 0) {
        semantic_error_fmt(
            ctx,
            use_node,
            "generic struct '%.*s' requires %d type argument%s",
            (int)template_decl->as.struct_decl.name.length,
            template_decl->as.struct_decl.name.data,
            parameters.count,
            parameters.count == 1 ? "" : "s"
        );
        return NULL;
    }
    if (source_argument_count != parameters.count) {
        semantic_error_fmt(
            ctx,
            use_node,
            "wrong number of generic type arguments for '%.*s': expected %d, got %d",
            (int)template_decl->as.struct_decl.name.length,
            template_decl->as.struct_decl.name.data,
            parameters.count,
            source_argument_count
        );
        return NULL;
    }

    Type **type_arguments = arena_alloc(
        ctx->arena,
        sizeof(Type *) * (size_t)source_argument_count
    );
    for (int i = 0; i < source_argument_count; i++) {
        Type *resolved = resolve_type(ctx, source_arguments[i], use_node);
        if (!resolved)
            return NULL;
        if (resolved->kind == TYPE_NAMED || is_untyped_numeric_type(resolved) ||
            is_null_type(resolved)) {
            semantic_error(ctx, use_node, "generic type arguments must resolve to concrete types");
            return NULL;
        }
        type_arguments[i] = resolved;
    }

    if (!check_generic_type_argument_constraints(
            ctx,
            template_decl,
            type_arguments,
            source_argument_count,
            use_node)) {
        return NULL;
    }

    GenericStructSpecialization *spec = instantiate_generic_struct(
        ctx,
        symbol,
        type_arguments,
        source_argument_count,
        use_node
    );
    if (!spec)
        return NULL;

    if (spec->state == GENERIC_SPECIALIZATION_INVALID) {
        if (!ctx->active_generic_struct_specialization) {
            char bindings[1024];
            format_generic_bindings(
                template_decl,
                type_arguments,
                source_argument_count,
                bindings,
                sizeof(bindings)
            );
            semantic_error_fmt(
                ctx,
                use_node,
                "cannot instantiate '%.*s' with %s: generic struct is invalid for these concrete types",
                (int)template_decl->as.struct_decl.name.length,
                template_decl->as.struct_decl.name.data,
                bindings
            );
        }
        return NULL;
    }

    return spec->type;
}

static Symbol *new_specialization_symbol(
    SemanticContext *ctx,
    Node *function,
    Type *function_type
) {
    Symbol *symbol = arena_new(ctx->arena, Symbol);
    *symbol = (Symbol){
        .name = function->as.func_decl.name,
        .kind = SYMBOL_FUNCTION,
        .builtin_kind = BUILTIN_NONE,
        .type = function_type,
        .declaration = NULL,
        .declaration_id = INVALID_SEM_DECL_ID,
        .variable_storage = VARIABLE_STORAGE_NONE,
        .flow_owner_id = INVALID_FLOW_OWNER_ID,
        .variable_id = INVALID_VARIABLE_ID,
        .next = NULL,
    };
    return symbol;
}

static void record_specialization_signature(
    SemanticContext *ctx,
    Node *function,
    Type *function_type,
    Symbol *symbol
) {
    function->as.func_decl.resolved_type = function_type;
    SemDeclInfo *info = sem_record_decl_info(ctx, function, function_type, symbol);
    info->is_generic_specialization = 1;
    info->abi_kind = SEM_DECL_ABI_FUNCTION;
    info->abi.function.abi = FUNCTION_ABI_COGLET;
    info->abi.function.linkage = SEM_FUNCTION_LINKAGE_INTERNAL;
    info->abi.function.c_call_conv = C_CALL_DEFAULT;
    info->abi.function.is_variadic = 0;

    for (int i = 0; i < function->as.func_decl.params.count; i++) {
        sem_record_decl_info(
            ctx,
            function->as.func_decl.params.items[i],
            function_type->parameters[i],
            NULL
        );
    }
}

static GenericSpecialization *instantiate_generic_function(
    SemanticContext *ctx,
    Symbol *template_symbol,
    Type *const *type_arguments,
    int type_argument_count,
    Node *call_site
) {
    assert(symbol_is_generic_template(template_symbol));
    Node *template_decl = template_symbol->declaration;
    SemDeclId template_id = template_symbol->declaration_id;

    GenericSpecialization *cached = find_generic_specialization(
        ctx,
        template_id,
        type_arguments,
        type_argument_count
    );
    if (cached)
        return cached;

    enum { GENERIC_SPECIALIZATION_RECURSION_LIMIT = 32 };
    if (active_generic_template_depth(ctx, template_id) >=
        GENERIC_SPECIALIZATION_RECURSION_LIMIT) {
        char bindings[1024];
        format_generic_bindings(
            template_decl,
            type_arguments,
            type_argument_count,
            bindings,
            sizeof(bindings)
        );
        semantic_error_fmt(
            ctx,
            call_site,
            "generic specialization recursion for '%.*s' exceeded %d changing instantiations (%s); likely non-terminating specialization",
            (int)template_decl->as.func_decl.name.length,
            template_decl->as.func_decl.name.data,
            GENERIC_SPECIALIZATION_RECURSION_LIMIT,
            bindings
        );
        return NULL;
    }

    GenericSpecialization *spec = arena_new(ctx->arena, GenericSpecialization);
    memset(spec, 0, sizeof(*spec));
    spec->template_id = template_id;
    spec->template_decl = template_decl;
    spec->type_argument_count = type_argument_count;
    spec->state = GENERIC_SPECIALIZATION_CHECKING;
    spec->type_arguments = arena_alloc(
        ctx->arena,
        sizeof(Type *) * (size_t)type_argument_count
    );
    memcpy(
        spec->type_arguments,
        type_arguments,
        sizeof(Type *) * (size_t)type_argument_count
    );
    spec->next = ctx->generic_specializations;
    ctx->generic_specializations = spec;

    Node *function = ast_clone(ctx->arena, template_decl);
    function->is_exported = 0;
    function->as.func_decl.type_parameters.items = NULL;
    function->as.func_decl.type_parameters.count = 0;
    function->as.func_decl.type_parameters.capacity = 0;
    function->as.func_decl.name = make_generic_specialization_name(
        ctx,
        template_decl,
        type_arguments,
        type_argument_count
    );
    spec->function = function;

    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    semantic_select_source_module(ctx, template_decl->span.file_id);
    scope_push(ctx);
    define_generic_type_aliases(ctx, template_decl, type_arguments);

    int errors_before = ctx->error_count;
    Type *function_type = make_function_type(ctx, function);
    if (function_type) {
        Symbol *symbol = new_specialization_symbol(ctx, function, function_type);
        spec->function_type = function_type;
        spec->symbol = symbol;
        record_specialization_signature(ctx, function, function_type, symbol);

        spec->active_parent = ctx->active_generic_specialization;
        ctx->active_generic_specialization = spec;
        check_function_body(ctx, function);
        ctx->active_generic_specialization = spec->active_parent;
    }

    spec->state = ctx->error_count == errors_before && spec->symbol
        ? GENERIC_SPECIALIZATION_VALID
        : GENERIC_SPECIALIZATION_INVALID;

    scope_pop(ctx);
    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;

    return spec;
}

static Symbol *lookup_generic_struct_pattern_symbol(
    SemanticContext *ctx,
    Node *template_decl,
    const Type *pattern
) {
    assert(ctx);
    assert(template_decl);
    assert(pattern && pattern->kind == TYPE_NAMED);

    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    semantic_select_source_module(ctx, template_decl->span.file_id);

    Symbol *symbol = NULL;
    if (pattern->named_module.length != 0) {
        symbol = semantic_lookup_visible_qualified_symbol_no_diag(
            ctx, pattern->named_module, pattern->named_name);
    } else {
        symbol = scope_lookup(
            ctx->current_scope,
            pattern->named_name.data,
            pattern->named_name.length
        );
    }

    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;
    return symbol;
}

static int infer_generic_argument_candidate(
    SemanticContext *ctx,
    Node *template_decl,
    const Type *pattern,
    Type *actual,
    Node *argument,
    Type **inferred
) {
    int parameter_index = -1;
    if (source_type_is_generic_parameter(template_decl, pattern, &parameter_index)) {
        Type *candidate = actual;
        if (candidate && is_untyped_numeric_type(candidate)) {
            candidate = concretize_inferred_type(ctx, argument, candidate);
            if (!candidate)
                return 0;
        }
        if (!candidate || is_null_type(candidate))
            return 1;

        if (!inferred[parameter_index]) {
            inferred[parameter_index] = candidate;
            return 1;
        }

        if (!type_equal(inferred[parameter_index], candidate)) {
            char previous_name[128];
            char candidate_name[128];
            format_type_name(inferred[parameter_index], previous_name, sizeof(previous_name));
            format_type_name(candidate, candidate_name, sizeof(candidate_name));
            StringView parameter = template_decl->as.func_decl.type_parameters.items[parameter_index].name;
            semantic_error_fmt(
                ctx,
                argument,
                "conflicting inference for generic type parameter '%.*s': %s versus %s",
                (int)parameter.length,
                parameter.data,
                previous_name,
                candidate_name
            );
            return 0;
        }
        return 1;
    }

    if (!pattern || !actual)
        return 1;

    if (pattern->kind == TYPE_POINTER && actual->kind == TYPE_POINTER)
        return infer_generic_argument_candidate(
            ctx, template_decl, pattern->element, actual->element, argument, inferred
        );

    if (pattern->kind == TYPE_ARRAY && actual->kind == TYPE_ARRAY)
        return infer_generic_argument_candidate(
            ctx, template_decl, pattern->element, actual->element, argument, inferred
        );

    if (pattern->kind == TYPE_SLICE &&
        (actual->kind == TYPE_SLICE || actual->kind == TYPE_ARRAY)) {
        return infer_generic_argument_candidate(
            ctx, template_decl, pattern->element, actual->element, argument, inferred
        );
    }

    if (pattern->kind == TYPE_NAMED && pattern->type_argument_count > 0 &&
        actual->kind == TYPE_STRUCT &&
        actual->struct_generic_template_id != (size_t)-1) {
        Symbol *symbol = lookup_generic_struct_pattern_symbol(
            ctx, template_decl, pattern);

        if (symbol_is_generic_struct_template(symbol) &&
            symbol->declaration_id == actual->struct_generic_template_id &&
            pattern->type_argument_count == actual->struct_type_argument_count) {
            for (int i = 0; i < pattern->type_argument_count; i++) {
                if (!infer_generic_argument_candidate(
                        ctx,
                        template_decl,
                        pattern->type_arguments[i],
                        actual->struct_type_arguments[i],
                        argument,
                        inferred)) {
                    return 0;
                }
            }
        }
        return 1;
    }

    if (pattern->kind == TYPE_FUNCTION && actual->kind == TYPE_FUNCTION) {
        int count = pattern->parameter_count < actual->parameter_count
            ? pattern->parameter_count
            : actual->parameter_count;
        for (int i = 0; i < count; i++) {
            if (!infer_generic_argument_candidate(
                    ctx,
                    template_decl,
                    pattern->parameters[i],
                    actual->parameters[i],
                    argument,
                    inferred)) {
                return 0;
            }
        }
        return infer_generic_argument_candidate(
            ctx,
            template_decl,
            pattern->return_type,
            actual->return_type,
            argument,
            inferred
        );
    }

    return 1;
}

static Type *check_generic_call(SemanticContext *ctx, Node *call, Symbol *template_symbol)
{
    Node *template_decl = template_symbol->declaration;
    int type_parameter_count = template_decl->as.func_decl.type_parameters.count;
    int parameter_count = template_decl->as.func_decl.params.count;
    int argc = call->as.call.arguments.count;

    if (argc != parameter_count) {
        semantic_error_fmt(
            ctx,
            call,
            "wrong number of arguments: expected %d, got %d",
            parameter_count,
            argc
        );
        return NULL;
    }

    Type **type_arguments = arena_alloc(
        ctx->arena,
        sizeof(Type *) * (size_t)type_parameter_count
    );
    memset(type_arguments, 0, sizeof(Type *) * (size_t)type_parameter_count);

    if (call->as.call.type_arguments.count > 0) {
        if (call->as.call.type_arguments.count != type_parameter_count) {
            semantic_error_fmt(
                ctx,
                call,
                "wrong number of generic type arguments: expected %d, got %d",
                type_parameter_count,
                call->as.call.type_arguments.count
            );
            return NULL;
        }

        for (int i = 0; i < type_parameter_count; i++) {
            Type *resolved = resolve_type(ctx, call->as.call.type_arguments.items[i], call);
            if (!resolved)
                return NULL;
            if (is_untyped_numeric_type(resolved) || is_null_type(resolved) ||
                resolved->kind == TYPE_NAMED) {
                semantic_error(ctx, call, "generic type arguments must resolve to concrete types");
                return NULL;
            }
            type_arguments[i] = resolved;
        }
    } else {
        int inference_ok = 1;
        for (int i = 0; i < parameter_count; i++) {
            Type *pattern = template_decl->as.func_decl.params.items[i]->as.param_decl.var_type;
            if (!source_type_contains_generic_parameter(template_decl, pattern))
                continue;

            Node *argument = call->as.call.arguments.items[i];
            if (argument->type == NODE_ARRAY_LITERAL || argument->type == NODE_STRING)
                continue;

            Type *actual = check_value_expression(ctx, argument);
            if (!actual) {
                inference_ok = 0;
                continue;
            }
            if (!infer_generic_argument_candidate(
                    ctx,
                    template_decl,
                    pattern,
                    actual,
                    argument,
                    type_arguments)) {
                inference_ok = 0;
            }
        }
        if (!inference_ok)
            return NULL;

        for (int i = 0; i < type_parameter_count; i++) {
            if (!type_arguments[i]) {
                StringView parameter = template_decl->as.func_decl.type_parameters.items[i].name;
                semantic_error_fmt(
                    ctx,
                    call,
                    "cannot infer generic type parameter '%.*s'; provide an explicit type argument",
                    (int)parameter.length,
                    parameter.data
                );
                return NULL;
            }
        }
    }

    if (!check_generic_type_argument_constraints(
            ctx,
            template_decl,
            type_arguments,
            type_parameter_count,
            call)) {
        return NULL;
    }

    GenericSpecialization *spec = instantiate_generic_function(
        ctx,
        template_symbol,
        type_arguments,
        type_parameter_count,
        call
    );
    if (!spec)
        return NULL;

    if (spec->state == GENERIC_SPECIALIZATION_INVALID) {
        if (!ctx->active_generic_specialization) {
            char bindings[1024];
            format_generic_bindings(
                template_decl,
                type_arguments,
                type_parameter_count,
                bindings,
                sizeof(bindings)
            );
            semantic_error_fmt(
                ctx,
                call,
                "cannot instantiate '%.*s' with %s: generic body is invalid for these concrete types",
                (int)template_decl->as.func_decl.name.length,
                template_decl->as.func_decl.name.data,
                bindings
            );
        }

        /*
         * Preserve the concrete signature after a failed specialization so one
         * primary generic error does not cascade into unrelated undefined-name
         * diagnostics in the caller. Lowering never runs after semantic errors.
         */
        if (spec->symbol && spec->function_type) {
            sem_record_expr_info(
                ctx,
                call->as.call.callee,
                spec->function_type,
                spec->symbol,
                VALUE_CATEGORY_RVALUE
            );
            ValueCategory category =
                spec->function_type->return_type->kind == TYPE_VOID
                    ? VALUE_CATEGORY_NONE
                    : VALUE_CATEGORY_RVALUE;
            sem_record_expr_info(
                ctx,
                call,
                spec->function_type->return_type,
                NULL,
                category
            );
            return spec->function_type->return_type;
        }
        return NULL;
    }

    if (!spec->symbol || !spec->function_type)
        return NULL;

    sem_record_expr_info(
        ctx,
        call->as.call.callee,
        spec->function_type,
        spec->symbol,
        VALUE_CATEGORY_RVALUE
    );

    int ok = 1;
    for (int i = 0; i < argc; i++) {
        if (!check_argument_against_parameter(
                ctx,
                spec->function_type->parameters[i],
                call->as.call.arguments.items[i])) {
            ok = 0;
        }
    }
    if (!ok)
        return NULL;

    ValueCategory category = spec->function_type->return_type->kind == TYPE_VOID
        ? VALUE_CATEGORY_NONE
        : VALUE_CATEGORY_RVALUE;
    sem_record_expr_info(
        ctx,
        call,
        spec->function_type->return_type,
        NULL,
        category
    );
    return spec->function_type->return_type;
}

static void prepend_call_argument(SemanticContext *ctx, Node *call, Node *argument)
{
    assert(call && call->type == NODE_CALL);
    int old_count = call->as.call.arguments.count;
    Node **items = arena_alloc(
        ctx->arena,
        sizeof(Node *) * (size_t)(old_count + 1)
    );
    items[0] = argument;
    if (old_count > 0) {
        memcpy(
            items + 1,
            call->as.call.arguments.items,
            sizeof(Node *) * (size_t)old_count
        );
    }
    call->as.call.arguments.items = items;
    call->as.call.arguments.count = old_count + 1;
    call->as.call.arguments.capacity = old_count + 1;
}

static Type *associated_method_owner_type(SemanticContext *ctx, Node *object)
{
    if (!object)
        return NULL;

    if (object->type == NODE_TYPE_REF)
        return resolve_type(ctx, object->as.type_ref.source_type, object);

    if (object->type == NODE_IDENT) {
        Symbol *symbol = scope_lookup(
            ctx->current_scope,
            object->as.ident.data,
            object->as.ident.length
        );
        return symbol && symbol->kind == SYMBOL_TYPE ? symbol->type : NULL;
    }

    if (object->type == NODE_FIELD) {
        int recognized = 0;
        Symbol *symbol = semantic_lookup_qualified_field_symbol(
            ctx,
            object,
            0,
            NULL,
            NULL,
            &recognized
        );
        if (recognized && symbol && symbol->kind == SYMBOL_TYPE)
            return symbol->type;
    }

    return NULL;
}

/*
 * Recognize a source method call and rewrite it to an ordinary function call.
 *
 *     value.length()          -> Struct.length(&value)
 *     Vec3::<f32>.new(...)    -> Vec3<f32>.new(...)
 *
 * The rewritten callee/implicit receiver are ordinary checked AST nodes, so
 * CogIR and both backends remain completely unaware of methods.
 *
 * Returns 1 when rewritten, 0 when this is not a method call, and -1 after a
 * method-specific diagnostic.
 */
static int prepare_struct_method_call(
    SemanticContext *ctx,
    Node *call,
    StructMethodBinding **out_binding
) {
    *out_binding = NULL;
    if (!call || call->type != NODE_CALL ||
        !call->as.call.callee || call->as.call.callee->type != NODE_FIELD) {
        return 0;
    }

    Node *field = call->as.call.callee;
    StringView method_name = field->as.field.name;

    /*
     * Preserve ordinary module-qualified calls such as `std.io.println(...)`.
     * They use the same dotted NODE_FIELD syntax as source method calls, so
     * resolve the complete dotted callee first before treating its prefix as a
     * runtime receiver.
     */
    if (field->as.field.dotted_path.length != 0) {
        int recognized = 0;
        Symbol *qualified = semantic_lookup_qualified_field_symbol(
            ctx,
            field,
            0,
            NULL,
            NULL,
            &recognized
        );
        if (recognized && qualified)
            return 0;
    }

    Type *owner_type = associated_method_owner_type(ctx, field->as.field.object);
    if (owner_type && owner_type->kind == TYPE_STRUCT) {
        StructMethodBinding *binding = find_struct_method_binding(
            ctx, owner_type, method_name);
        if (!binding) {
            semantic_error_fmt(
                ctx,
                call,
                "struct type '%.*s' has no associated function '%.*s'",
                (int)owner_type->struct_name.length,
                owner_type->struct_name.data,
                (int)method_name.length,
                method_name.data
            );
            return -1;
        }
        if (binding->is_instance) {
            semantic_error_fmt(
                ctx,
                call,
                "instance method '%.*s' must be called on a value",
                (int)method_name.length,
                method_name.data
            );
            return -1;
        }
        if (call->as.call.type_arguments.count > 0) {
            semantic_error(ctx, call, "generic methods are not supported");
            return -1;
        }

        Node *callee = ast_new_ident(
            ctx->arena,
            binding->function->as.func_decl.name.data,
            (int)binding->function->as.func_decl.name.length,
            field->span
        );
        call->as.call.callee = callee;
        sem_record_expr_info(
            ctx,
            callee,
            binding->function_type,
            binding->symbol,
            VALUE_CATEGORY_RVALUE
        );
        *out_binding = binding;
        return 1;
    }

    Node *object = field->as.field.object;
    Type *object_type = check_value_expression(ctx, object);
    if (!object_type)
        return 0;

    Type *instance_owner = object_type;
    if (instance_owner->kind == TYPE_POINTER)
        instance_owner = instance_owner->element;
    if (!instance_owner || instance_owner->kind != TYPE_STRUCT)
        return 0;

    StructMethodBinding *binding = find_struct_method_binding(
        ctx, instance_owner, method_name);
    if (!binding)
        return 0;
    if (!binding->is_instance) {
        semantic_error_fmt(
            ctx,
            call,
            "associated function '%.*s' must be called through the type",
            (int)method_name.length,
            method_name.data
        );
        return -1;
    }
    if (call->as.call.type_arguments.count > 0) {
        semantic_error(ctx, call, "generic methods are not supported");
        return -1;
    }

    Type *receiver = binding->function_type->parameters[0];
    Node *receiver_argument = object;
    if (receiver->kind == TYPE_POINTER && !type_equal(object_type, receiver)) {
        if (!type_equal(object_type, receiver->element))
            return 0;
        if (!expression_is_lvalue(ctx, object)) {
            semantic_error(
                ctx,
                call,
                "pointer-receiver method requires an addressable value"
            );
            return -1;
        }
        receiver_argument = ast_new_unary(
            ctx->arena,
            TOK_AND,
            object,
            object->span
        );
    } else if (!type_equal(object_type, receiver)) {
        return 0;
    }

    prepend_call_argument(ctx, call, receiver_argument);
    Node *callee = ast_new_ident(
        ctx->arena,
        binding->function->as.func_decl.name.data,
        (int)binding->function->as.func_decl.name.length,
        field->span
    );
    call->as.call.callee = callee;
    sem_record_expr_info(
        ctx,
        callee,
        binding->function_type,
        binding->symbol,
        VALUE_CATEGORY_RVALUE
    );
    *out_binding = binding;
    return 1;
}

static int ensure_struct_method_body_checked(
    SemanticContext *ctx,
    StructMethodBinding *binding,
    Node *use_node
) {
    SemDeclInfo *info = sem_find_decl_info(ctx, binding->function);
    if (!info || !info->is_deferred_generic_method)
        return 1;
    if (info->semantic_check_complete)
        return 1;

    if (info->deferred_method_check_failed) {
        char owner_name[128];
        format_type_name(binding->owner_type, owner_name, sizeof(owner_name));
        semantic_error_fmt(
            ctx,
            use_node,
            "method '%.*s' is not valid for %s",
            (int)binding->source_name.length,
            binding->source_name.data,
            owner_name
        );
        return 0;
    }

    /* Ordinary recursive calls reuse the in-progress concrete signature. */
    if (info->semantic_check_started)
        return 1;

    Type *owner_type = binding->owner_type;
    if (owner_type->struct_generic_template_id == INVALID_SEM_DECL_ID) {
        semantic_error(ctx, use_node,
            "internal error: deferred method has no generic struct template identity");
        return 0;
    }

    SemDeclInfo *template_info = semantic_get_decl_info_by_id(
        ctx, owner_type->struct_generic_template_id);
    if (!template_info || !template_info->node ||
        template_info->node->type != NODE_STRUCT_DECL) {
        semantic_error(ctx, use_node,
            "internal error: deferred method generic struct template is unavailable");
        return 0;
    }

    Node *template_decl = template_info->node;
    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    info->semantic_check_started = 1;
    int errors_before = ctx->error_count;

    semantic_select_source_module(ctx, template_decl->span.file_id);
    scope_push(ctx);
    define_generic_type_aliases(
        ctx,
        template_decl,
        owner_type->struct_type_arguments
    );
    scope_define(
        ctx,
        string_view_from_cstr("Self"),
        SYMBOL_TYPE,
        owner_type
    );
    check_function_body(ctx, binding->function);
    scope_pop(ctx);

    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;

    info->semantic_check_started = 0;
    if (ctx->error_count == errors_before) {
        info->semantic_check_complete = 1;
        return 1;
    }

    info->deferred_method_check_failed = 1;
    char owner_name[128];
    format_type_name(owner_type, owner_name, sizeof(owner_name));
    semantic_error_fmt(
        ctx,
        use_node,
        "cannot use method '%.*s' for %s because its body is not valid for this specialization",
        (int)binding->source_name.length,
        binding->source_name.data,
        owner_name
    );
    return 0;
}

static Type *check_prepared_struct_method_call(
    SemanticContext *ctx,
    Node *call,
    StructMethodBinding *binding
) {
    if (!ensure_struct_method_body_checked(ctx, binding, call))
        return NULL;

    Type *callee = binding->function_type;
    int argc = call->as.call.arguments.count;
    if (argc != callee->parameter_count) {
        semantic_error_fmt(
            ctx,
            call,
            "wrong number of arguments to method '%.*s': expected %d, got %d",
            (int)binding->source_name.length,
            binding->source_name.data,
            callee->parameter_count - (binding->is_instance ? 1 : 0),
            argc - (binding->is_instance ? 1 : 0)
        );
        return NULL;
    }

    int ok = 1;
    for (int i = 0; i < argc; i++) {
        if (!check_argument_against_parameter(
                ctx,
                callee->parameters[i],
                call->as.call.arguments.items[i])) {
            ok = 0;
        }
    }
    if (!ok)
        return NULL;

    ValueCategory category = callee->return_type->kind == TYPE_VOID
        ? VALUE_CATEGORY_NONE
        : VALUE_CATEGORY_RVALUE;
    sem_record_expr_info(ctx, call, callee->return_type, NULL, category);
    return callee->return_type;
}

static Type *rewrite_struct_operator_call(
    SemanticContext *ctx,
    Node *node,
    StructOperatorBinding *operator_binding,
    Node *left,
    Node *right
) {
    assert(node);
    assert(operator_binding && operator_binding->method);
    StructMethodBinding *method = operator_binding->method;
    SourceSpan span = node->span;

    Node *callee = ast_new_ident(
        ctx->arena,
        method->function->as.func_decl.name.data,
        (int)method->function->as.func_decl.name.length,
        span
    );
    sem_record_expr_info(
        ctx,
        callee,
        method->function_type,
        method->symbol,
        VALUE_CATEGORY_RVALUE
    );

    memset(&node->as, 0, sizeof(node->as));
    node->type = NODE_CALL;
    node->span = span;
    node->as.call.callee = callee;
    nodelist_push(ctx->arena, &node->as.call.arguments, left);
    if (right)
        nodelist_push(ctx->arena, &node->as.call.arguments, right);

    return check_prepared_struct_method_call(ctx, node, method);
}

static Type *check_identifier_expression(SemanticContext *ctx, Node *node,IdentifierUse use) {

    assert(node);
    assert(node->type == NODE_IDENT);

    Symbol *sym =
        scope_lookup(
            ctx->current_scope,
            node->as.ident.data,
            node->as.ident.length
        );

    if (!sym) {
        semantic_error_name(
            ctx,
            node,
            "undefined identifier",
            node->as.ident.data,
            node->as.ident.length
        );

        return NULL;
    }

    /*
    * Builtins have compiler-defined polymorphic signatures and are not
    * first-class function values.
    *
    * NODE_CALL handles builtin identifiers through its dedicated
    * dispatch path before ordinary value checking reaches here.
    */
    if (sym->kind == SYMBOL_BUILTIN) {
        semantic_error_fmt(
            ctx,
            node,
            "builtin '%.*s' can only be used as a call target",
            (int)node->as.ident.length,
            node->as.ident.data
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
            ctx,
            node,
            "type name cannot be used as a value",
            node->as.ident.data,
            node->as.ident.length
        );

        return NULL;
    }

    if (symbol_is_generic_template(sym)) {
        semantic_error_fmt(
            ctx,
            node,
            "generic function '%.*s' can only be used as a call target",
            (int)node->as.ident.length,
            node->as.ident.data
        );
        return NULL;
    }

    if (sym->kind == SYMBOL_FUNCTION) {
        Scope *defining_scope = scope_find_defining_scope(
            ctx->current_scope, node->as.ident.data, node->as.ident.length);
        if (scope_count_local_functions_named(
                defining_scope, node->as.ident.data, node->as.ident.length) > 1) {
            semantic_error_fmt(
                ctx,
                node,
                "overloaded function '%.*s' can only be used as a call target",
                (int)node->as.ident.length,
                node->as.ident.data
            );
            return NULL;
        }
    }

    if (sym->kind == SYMBOL_CONSTANT && !sym->type) {
        if (!ensure_constant_symbol_checked(ctx, sym))
            return NULL;
    }

    if (sym->kind == SYMBOL_VARIABLE && !sym->type) {
        /*
         * Function bodies may be checked before a later physical source file
         * reaches its global declaration. Force only semantic declaration
         * checking here; CogIR runtime initialization order is unchanged.
         * Top-level runtime initializers retain source-order visibility.
         */
        if (ctx->function_depth == 0 ||
            !ensure_global_variable_symbol_checked(ctx, sym)) {
            semantic_error_name(
                ctx,
                node,
                "undefined identifier",
                node->as.ident.data,
                node->as.ident.length
            );
            return NULL;
        }
    }

    ValueCategory category =
        VALUE_CATEGORY_RVALUE;

    if (sym->kind == SYMBOL_VARIABLE)
        category = VALUE_CATEGORY_LVALUE;

    /*
     * Symbol resolution, type, and value category remain valid
     * semantic facts even if flow analysis rejects this use.
     */
    sem_record_expr_info(
        ctx,
        node,
        sym->type,
        sym,
        category
    );


    /*
     * Nested functions do not currently implement closure capture.
     *
     * Globals, constants, types, and function symbols remain
     * accessible because they do not belong to a function-local
     * flow state.
     */
    if (symbol_has_flow_state(sym) &&
        !symbol_belongs_to_flow(sym, &ctx->flow)) {

        semantic_error_fmt(
            ctx,
            node,
            "nested function cannot capture variable '%.*s' from an enclosing function",
            (int)node->as.ident.length,
            node->as.ident.data);

        return NULL;
    }


    switch (use) {
        case IDENTIFIER_USE_READ:
            if (!flow_variable_is_initialized(
                    ctx,
                    sym
                )) {
                semantic_error_fmt(
                    ctx,
                    node,
                    "variable '%.*s' may be uninitialized",
                    (int)node->as.ident.length,
                    node->as.ident.data
                );

                return NULL;
                }

            break;

        case IDENTIFIER_USE_WRITE_TARGET:
            break;
    }

    return sym->type;
}

typedef enum WrappingBuiltinTypeStatus {
    WRAPPING_BUILTIN_TYPE_VALID,
    WRAPPING_BUILTIN_TYPE_REQUIRES_CONCRETE_INTEGER,
    WRAPPING_BUILTIN_TYPE_MISMATCHED_ARGUMENTS,
} WrappingBuiltinTypeStatus;

static const char *builtin_kind_name(BuiltinKind kind) {
    switch (kind) {
        case BUILTIN_NONE:         return "<none>";
        case BUILTIN_WRAPPING_ADD: return "wrapping_add";
        case BUILTIN_WRAPPING_SUB: return "wrapping_sub";
        case BUILTIN_WRAPPING_MUL: return "wrapping_mul";
        case BUILTIN_WRAPPING_NEG: return "wrapping_neg";
        case BUILTIN_SIZE_OF:      return "size_of";
        case BUILTIN_ALIGN_OF:     return "align_of";
        case BUILTIN_SLICE:        return "slice";
    }

    UNREACHABLE("BuiltinKind");
}

static Symbol *resolve_builtin_callee(SemanticContext *ctx, Node *callee) {

    if (!callee ||
        callee->type != NODE_IDENT) {
        return NULL;
    }

    Symbol *symbol =
        scope_lookup(
            ctx->current_scope,
            callee->as.ident.data,
            callee->as.ident.length
        );

    if (!symbol ||
        symbol->kind != SYMBOL_BUILTIN) {
        return NULL;
    }

    assert_symbol_builtin_invariant(symbol);

    return symbol;
}

static int check_builtin_argument_count(SemanticContext *ctx, Node *call, const Symbol *builtin, int expected_count) {

    assert(call);
    assert(call->type == NODE_CALL);
    assert(builtin);
    assert(builtin->kind == SYMBOL_BUILTIN);

    int actual_count =
        call->as.call.arguments.count;

    if (actual_count == expected_count)
        return 1;

    semantic_error_fmt(
        ctx,
        call,
        "wrong number of arguments to builtin '%s': expected %d, got %d",
        builtin_kind_name(builtin->builtin_kind),
        expected_count,
        actual_count
    );

    return 0;
}

static WrappingBuiltinTypeStatus
    classify_wrapping_binary_types(Type *left_type, Type *right_type, Type **out_result_type) {

    assert(out_result_type);

    if (!left_type ||
        !right_type ||
        !is_concrete_integer_kind(left_type->kind) ||
        !is_concrete_integer_kind(right_type->kind)) {
        return
            WRAPPING_BUILTIN_TYPE_REQUIRES_CONCRETE_INTEGER;
        }

    if (!type_equal(left_type, right_type)) {
        return
            WRAPPING_BUILTIN_TYPE_MISMATCHED_ARGUMENTS;
    }

    *out_result_type = left_type;

    return WRAPPING_BUILTIN_TYPE_VALID;
}

static void report_wrapping_builtin_type_error(
    SemanticContext *ctx, Node *owner, const Symbol *builtin, WrappingBuiltinTypeStatus status) {

    assert(ctx);
    assert(owner);
    assert(builtin);
    assert(builtin->kind == SYMBOL_BUILTIN);

    switch (status) {
        case WRAPPING_BUILTIN_TYPE_VALID:
            UNREACHABLE("valid wrapping builtin type reported as error");

        case WRAPPING_BUILTIN_TYPE_REQUIRES_CONCRETE_INTEGER:
            if (builtin->builtin_kind ==
                BUILTIN_WRAPPING_NEG) {
                semantic_error_fmt(
                    ctx,
                    owner,
                    "builtin '%s' requires a concrete integer argument",
                    builtin_kind_name(
                        builtin->builtin_kind
                    )
                );
                } else {
                    semantic_error_fmt(
                        ctx,
                        owner,
                        "builtin '%s' requires concrete integer arguments",
                        builtin_kind_name(
                            builtin->builtin_kind
                        )
                    );
                }

            return;

        case WRAPPING_BUILTIN_TYPE_MISMATCHED_ARGUMENTS:
            semantic_error_fmt(
                ctx,
                owner,
                "builtin '%s' requires arguments of the same integer type",
                builtin_kind_name(
                    builtin->builtin_kind
                )
            );

            return;
    }

    UNREACHABLE("WrappingBuiltinTypeStatus");
}

/*
 * Checks wrapping_add, wrapping_sub, and wrapping_mul.
 *
 * Wrapping width and signed interpretation are determined by the
 * common concrete integer type. Untyped integer arguments are
 * intentionally rejected because wrapping requires an explicit
 * fixed width.
 */
static Type *check_wrapping_binary_builtin_call(SemanticContext *ctx, Node *call, const Symbol *builtin) {

    if (!check_builtin_argument_count(ctx, call, builtin, 2))
        return NULL;

    Node *left_argument =
        call->as.call.arguments.items[0];

    Node *right_argument =
        call->as.call.arguments.items[1];

    Type *left_type =
        check_value_expression(ctx, left_argument);

    Type *right_type =
        check_value_expression(ctx, right_argument);

    if (!left_type || !right_type)
        return NULL;

    Type *result_type = NULL;

    WrappingBuiltinTypeStatus status =
        classify_wrapping_binary_types(left_type, right_type, &result_type);

    if (status != WRAPPING_BUILTIN_TYPE_VALID) {
        report_wrapping_builtin_type_error(
            ctx,
            call,
            builtin,
            status
        );

        return NULL;
    }

    assert(result_type);

    return result_type;
}

static WrappingBuiltinTypeStatus classify_wrapping_neg_type(Type *argument_type, Type **out_result_type) {

    assert(out_result_type);

    if (!argument_type ||
        !is_concrete_integer_kind(argument_type->kind)) {
        return
            WRAPPING_BUILTIN_TYPE_REQUIRES_CONCRETE_INTEGER;
        }

    *out_result_type = argument_type;

    return WRAPPING_BUILTIN_TYPE_VALID;
}

/*
 * Explicit wrapping negation is valid for both signed and unsigned
 * concrete integer types.
 *
 * For an unsigned value, the operation computes zero minus the value
 * modulo 2^N.
 */
static Type *check_wrapping_neg_builtin_call(SemanticContext *ctx, Node *call, const Symbol *builtin) {

    if (!check_builtin_argument_count(ctx, call, builtin, 1))
        return NULL;

    Node *argument =
        call->as.call.arguments.items[0];

    Type *argument_type =
        check_value_expression(ctx, argument);

    if (!argument_type)
        return NULL;

    Type *result_type = NULL;

    WrappingBuiltinTypeStatus status =
        classify_wrapping_neg_type(argument_type, &result_type);

    if (status != WRAPPING_BUILTIN_TYPE_VALID) {
        report_wrapping_builtin_type_error(
            ctx,
            call,
            builtin,
            status
        );

        return NULL;
    }

    assert(result_type);

    return result_type;
}

static int type_has_runtime_layout(const Type *type)
{
    if (!type)
        return 0;

    switch (type->kind) {
        case TYPE_VOID:
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_NAMED:
        case TYPE_FUNCTION:
            return 0;

        case TYPE_STRUCT:
            return !type->struct_is_incomplete;

        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_POINTER:
        case TYPE_OPAQUE_POINTER:
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_ENUM:
            return 1;
    }

    return 0;
}

static Type *check_type_layout_builtin_call(
    SemanticContext *ctx,
    Node *call,
    const Symbol *builtin
) {
    assert(builtin->builtin_kind == BUILTIN_SIZE_OF ||
           builtin->builtin_kind == BUILTIN_ALIGN_OF);

    if (!check_builtin_argument_count(ctx, call, builtin, 0))
        return NULL;

    if (call->as.call.type_arguments.count != 1) {
        semantic_error_fmt(
            ctx,
            call,
            "builtin '%s' requires exactly one explicit type argument",
            builtin_kind_name(builtin->builtin_kind)
        );
        return NULL;
    }

    Type *queried = resolve_type(ctx, call->as.call.type_arguments.items[0], call);
    if (!queried)
        return NULL;

    if (!type_has_runtime_layout(queried)) {
        char type_name[192];
        format_type_name(queried, type_name, sizeof(type_name));
        semantic_error_fmt(
            ctx,
            call,
            "builtin '%s' requires a complete runtime object type, got %s",
            builtin_kind_name(builtin->builtin_kind),
            type_name
        );
        return NULL;
    }

    SemExprInfo *info = sem_get_or_create_expr_info(ctx, call);
    info->builtin_type_argument = queried;
    return ctx->type_u64;
}

static Type *check_slice_builtin_call(
    SemanticContext *ctx,
    Node *call,
    const Symbol *builtin
) {
    assert(builtin->builtin_kind == BUILTIN_SLICE);

    if (!check_builtin_argument_count(ctx, call, builtin, 2))
        return NULL;

    if (call->as.call.type_arguments.count != 0) {
        semantic_error(ctx, call, "builtin 'slice' infers its element type from the pointer argument");
        return NULL;
    }

    Node *pointer_arg = call->as.call.arguments.items[0];
    Type *pointer_type = check_value_expression(ctx, pointer_arg);
    if (!pointer_type)
        return NULL;

    if (pointer_type->kind != TYPE_POINTER) {
        semantic_error(ctx, pointer_arg, "builtin 'slice' requires a typed raw pointer as its first argument");
        return NULL;
    }

    if (pointer_type->pointer_is_volatile) {
        semantic_error(ctx, pointer_arg, "builtin 'slice' does not support volatile pointer storage");
        return NULL;
    }

    if (!check_argument_against_parameter(
            ctx,
            ctx->type_u64,
            call->as.call.arguments.items[1])) {
        return NULL;
    }

    return intern_slice_type(ctx, pointer_type->element, pointer_type->pointer_access);
}

static Type *check_builtin_call(SemanticContext *ctx, Node *call, Symbol *builtin) {

    assert(call);
    assert(call->type == NODE_CALL);
    assert(builtin);
    assert(builtin->kind == SYMBOL_BUILTIN);

    Type *result_type = NULL;

    switch (builtin->builtin_kind) {
        case BUILTIN_WRAPPING_ADD:
        case BUILTIN_WRAPPING_SUB:
        case BUILTIN_WRAPPING_MUL:
            result_type =
                check_wrapping_binary_builtin_call(ctx, call, builtin);
            break;

        case BUILTIN_WRAPPING_NEG:
            result_type =
                check_wrapping_neg_builtin_call(ctx,
                call,
                    builtin
                );
            break;

        case BUILTIN_SIZE_OF:
        case BUILTIN_ALIGN_OF:
            result_type = check_type_layout_builtin_call(ctx, call, builtin);
            break;

        case BUILTIN_SLICE:
            result_type = check_slice_builtin_call(ctx, call, builtin);
            break;

        case BUILTIN_NONE:
            UNREACHABLE("SYMBOL_BUILTIN with BUILTIN_NONE");
    }

    if (!result_type)
        return NULL;

    /*
     * The builtin identifier is a successfully resolved call target,
     * but it is not itself a first-class value with a fixed function
     * type.
     */
    sem_record_expr_info(
        ctx,
        call->as.call.callee,
        NULL,
        builtin,
        VALUE_CATEGORY_NONE
    );

    /*
     * Recording the builtin symbol on the call preserves its stable
     * compiler identity for constant evaluation and future lowering.
     */
    sem_record_expr_info(
        ctx,
        call,
        result_type,
        builtin,
        VALUE_CATEGORY_RVALUE
    );

    return result_type;
}

typedef struct FunctionOverloadTarget {
    Scope *scope;
    StringView name;
    SemanticModule *module;
    int require_exported;
} FunctionOverloadTarget;

static int overload_symbol_visible(
    SemanticContext *ctx,
    const FunctionOverloadTarget *target,
    Symbol *symbol
) {
    if (!target->require_exported)
        return 1;
    SemDeclInfo *info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
    return info && info->is_exported;
}

/*
 * Returns 1 when the callee denotes an overload set, 0 when ordinary call
 * resolution should continue, and -1 when module qualification already emitted
 * a diagnostic.
 */
static int resolve_function_overload_target(
    SemanticContext *ctx,
    Node *callee,
    FunctionOverloadTarget *out
) {
    memset(out, 0, sizeof(*out));

    if (callee->type == NODE_IDENT) {
        Scope *scope = scope_find_defining_scope(
            ctx->current_scope,
            callee->as.ident.data,
            callee->as.ident.length
        );
        if (!scope || scope_count_local_functions_named(
                scope, callee->as.ident.data, callee->as.ident.length) < 2) {
            return 0;
        }
        out->scope = scope;
        out->name = string_view(callee->as.ident.data, callee->as.ident.length);
        return 1;
    }

    if (callee->type == NODE_FIELD) {
        StringView member = string_view_empty();
        int recognized = 0;
        SemanticModule *module = semantic_resolve_qualified_field_location(
            ctx, callee, 1, &member, &recognized);
        if (recognized && !module)
            return -1;
        if (!module)
            return 0;
        if (scope_count_local_functions_named(
                module->scope, member.data, member.length) < 2) {
            return 0;
        }
        out->scope = module->scope;
        out->name = member;
        out->module = module;
        out->require_exported = module != ctx->current_module;
        return 1;
    }

    return 0;
}

static int append_overload_type_name(
    char *buffer,
    size_t buffer_size,
    size_t *at,
    Type *type
) {
    char type_name[128];
    format_type_name(type, type_name, sizeof(type_name));
    size_t length = strlen(type_name);
    if (*at + length + 1 >= buffer_size)
        return 0;
    memcpy(buffer + *at, type_name, length);
    *at += length;
    buffer[*at] = '\0';
    return 1;
}

static Type *check_overloaded_function_call(
    SemanticContext *ctx,
    Node *call,
    const FunctionOverloadTarget *target
) {
    if (call->as.call.type_arguments.count > 0) {
        semantic_error(ctx, call,
            "explicit type arguments are not supported on overloaded functions");
        return NULL;
    }

    const int argc = call->as.call.arguments.count;
    Type **actual_types = argc > 0
        ? arena_alloc(ctx->arena, sizeof(Type *) * (size_t)argc)
        : NULL;

    for (int i = 0; i < argc; i++) {
        Node *argument = call->as.call.arguments.items[i];
        SemExprInfo *existing = sem_find_expr_info(ctx, argument);
        Type *actual = existing ? existing->type : check_value_expression(ctx, argument);
        if (!actual)
            return NULL;
        if (is_untyped_numeric_type(actual)) {
            actual = concretize_inferred_type(ctx, argument, actual);
            if (!actual)
                return NULL;
        }
        actual_types[i] = actual;
    }

    Symbol *selected = NULL;
    int match_count = 0;
    int visible_candidate_count = 0;

    for (Symbol *sym = target->scope->symbols; sym; sym = sym->next) {
        if (sym->kind != SYMBOL_FUNCTION ||
            !names_equal(sym->name.data, sym->name.length,
                         target->name.data, target->name.length) ||
            !sym->type || sym->type->kind != TYPE_FUNCTION ||
            !overload_symbol_visible(ctx, target, sym)) {
            continue;
        }

        visible_candidate_count++;
        Type *function_type = sym->type;
        if (function_type->function_is_variadic ||
            function_type->parameter_count != argc) {
            continue;
        }

        int exact = 1;
        for (int i = 0; i < argc; i++) {
            if (!type_equal(function_type->parameters[i], actual_types[i])) {
                exact = 0;
                break;
            }
        }
        if (!exact)
            continue;

        selected = sym;
        match_count++;
    }

    if (match_count != 1) {
        char actual_buffer[512] = {0};
        size_t at = 0;
        actual_buffer[at++] = '(';
        actual_buffer[at] = '\0';
        for (int i = 0; i < argc; i++) {
            if (i != 0) {
                if (at + 3 >= sizeof(actual_buffer))
                    break;
                actual_buffer[at++] = ',';
                actual_buffer[at++] = ' ';
                actual_buffer[at] = '\0';
            }
            if (!append_overload_type_name(
                    actual_buffer, sizeof(actual_buffer), &at, actual_types[i])) {
                break;
            }
        }
        if (at + 2 < sizeof(actual_buffer)) {
            actual_buffer[at++] = ')';
            actual_buffer[at] = '\0';
        }

        if (visible_candidate_count == 0 && target->module) {
            semantic_error_fmt(
                ctx,
                call,
                "module '%.*s' has no exported overload '%.*s'",
                (int)target->module->name.length,
                target->module->name.data,
                (int)target->name.length,
                target->name.data
            );
        } else if (match_count == 0) {
            semantic_error_fmt(
                ctx,
                call,
                "no exact overload of '%.*s' matches argument types %s",
                (int)target->name.length,
                target->name.data,
                actual_buffer
            );
        } else {
            semantic_error_fmt(
                ctx,
                call,
                "ambiguous exact overload of '%.*s' for argument types %s",
                (int)target->name.length,
                target->name.data,
                actual_buffer
            );
        }
        return NULL;
    }

    sem_record_expr_info(
        ctx,
        call->as.call.callee,
        selected->type,
        selected,
        VALUE_CATEGORY_RVALUE
    );

    int ok = 1;
    for (int i = 0; i < argc; i++) {
        if (!check_argument_against_parameter(
                ctx, selected->type->parameters[i], call->as.call.arguments.items[i])) {
            ok = 0;
        }
    }
    if (!ok)
        return NULL;

    Type *result_type = selected->type->return_type;
    sem_record_expr_info(
        ctx,
        call,
        result_type,
        NULL,
        result_type->kind == TYPE_VOID ? VALUE_CATEGORY_NONE : VALUE_CATEGORY_RVALUE
    );
    return result_type;
}

static int eval_const_wrapping_binary_call(SemanticContext *ctx, Node *call, Symbol *builtin, ConstValue *out) {

    if (!check_builtin_argument_count(ctx, call, builtin, 2))
        return 0;

    ConstValue left;
    ConstValue right;

    if (!eval_const_expr(ctx, call->as.call.arguments.items[0], &left))
        return 0;

    if (!eval_const_expr(
            ctx,
            call->as.call.arguments.items[1],
            &right
        )) {
        return 0;
        }

    Type *left_type =
        const_value_default_type(ctx, &left);

    Type *right_type =
        const_value_default_type(ctx, &right);

    Type *result_type = NULL;

    WrappingBuiltinTypeStatus type_status =
        classify_wrapping_binary_types(
            left_type,
            right_type,
            &result_type
        );

    if (type_status != WRAPPING_BUILTIN_TYPE_VALID) {
        report_wrapping_builtin_type_error(
            ctx,
            call,
            builtin,
            type_status
        );

        return 0;
    }

    assert(result_type);
    assert(left.kind == CONST_VALUE_INT);
    assert(right.kind == CONST_VALUE_INT);

    IntegerValue result;

    if (!evaluate_wrapping_integer_binary(
            builtin->builtin_kind,
            left.as.integer,
            right.as.integer,
            result_type->kind,
            &result
        )) {
        semantic_error(ctx, call,
            "internal error: could not evaluate wrapping integer builtin");

        return 0;
    }

    return integer_constant_result(
        ctx,
        call,
        result_type,
        result_type->kind,
        result,
        out
    );
}

static int eval_const_wrapping_neg_call(SemanticContext *ctx, Node *call, Symbol *builtin, ConstValue *out) {

    if (!check_builtin_argument_count(ctx, call, builtin, 1))
        return 0;

    ConstValue operand;

    if (!eval_const_expr(ctx, call->as.call.arguments.items[0], &operand))
        return 0;

    Type *operand_type =
        const_value_default_type(ctx,  &operand);

    Type *result_type = NULL;

    WrappingBuiltinTypeStatus type_status =
        classify_wrapping_neg_type(operand_type, &result_type);

    if (type_status != WRAPPING_BUILTIN_TYPE_VALID) {
        report_wrapping_builtin_type_error(
            ctx,
            call,
            builtin,
            type_status
        );

        return 0;
    }

    assert(result_type);
    assert(operand.kind == CONST_VALUE_INT);

    IntegerValue result;

    if (!evaluate_wrapping_integer_negation(
            operand.as.integer,
            result_type->kind,
            &result
        )) {
        semantic_error(ctx, call,
            "internal error: could not evaluate wrapping integer builtin");

        return 0;
    }

    return integer_constant_result(
        ctx,
        call,
        result_type,
        result_type->kind,
        result,
        out
    );
}

static int eval_const_builtin_call(SemanticContext *ctx, Node *call, ConstValue *out) {

    assert(call);
    assert(call->type == NODE_CALL);

    Symbol *builtin =
        resolve_builtin_callee(ctx, call->as.call.callee);

    if (!builtin) {
        semantic_error(ctx, call,
            "expression is not a compile-time constant");

        return 0;
    }

    switch (builtin->builtin_kind) {
        case BUILTIN_WRAPPING_ADD:
        case BUILTIN_WRAPPING_SUB:
        case BUILTIN_WRAPPING_MUL:
            return eval_const_wrapping_binary_call(
                ctx,
                call,
                builtin,
                out
            );

        case BUILTIN_WRAPPING_NEG:
            return eval_const_wrapping_neg_call(
                ctx,
                call,
                builtin,
                out
            );

        case BUILTIN_SIZE_OF:
        case BUILTIN_ALIGN_OF:
        case BUILTIN_SLICE:
            semantic_error(ctx, call, "expression is not a compile-time constant");
            return 0;

        case BUILTIN_NONE:
            UNREACHABLE(
                "SYMBOL_BUILTIN with BUILTIN_NONE"
            );
    }

    UNREACHABLE("BuiltinKind");
}

static Type *check_truncating_cast_expression(SemanticContext *ctx, Node *node) {

    assert(node);
    assert(node->type == NODE_CAST);
    assert(node->as.cast_expr.kind == CAST_TRUNCATING);

    Type *target_type =
        resolve_type(ctx, node->as.cast_expr.target_type, node);

    if (!target_type) {
        semantic_error(
            ctx,
            node,
            "could not resolve truncate target type"
        );

        return NULL;
    }

    if (!is_concrete_integer_kind(target_type->kind)) {
        semantic_error(ctx, node,
            "truncate target must be a concrete integer type");

        return NULL;
    }

    Node *source_expression =
        node->as.cast_expr.expression;

    Type *source_type =
        check_value_expression(ctx, source_expression);

    if (!source_type)
        return NULL;

    /*
     * Untyped integer constants are accepted:
     *
     *     truncate(u8, 256)
     *
     * Runtime expressions always have a concrete integer type.
     */
    if (!is_integer_kind(source_type->kind)) {
        semantic_error(ctx, node,
            "truncate source must be an integer");

        return NULL;
    }

    /*
     * Evaluate known values even when the result is discarded.
     * This verifies the evaluator and preserves the general rule
     * that compile-time-known conversions are checked immediately.
     *
     * Truncation itself cannot fail because of range.
     */
    if (expression_is_compile_time_constant(ctx, source_expression)) {
        ConstValue ignored;

        if (!eval_const_cast(ctx, node, &ignored))
            return NULL;

    }

    return target_type;
}

static Type *check_reinterpret_expression(SemanticContext *ctx, Node *node) {

    assert(node);
    assert(node->type == NODE_CAST);
    assert(node->as.cast_expr.kind == CAST_REINTERPRET);

    Type *target_type =
        resolve_type(ctx, node->as.cast_expr.target_type, node);

    if (!target_type) {
        semantic_error(ctx, node,
            "could not resolve reinterpret target type");
        return NULL;
    }

    Node *source_expression = node->as.cast_expr.expression;
    Type *source_type = check_value_expression(ctx, source_expression);

    if (!source_type)
        return NULL;

    if (!is_raw_pointer_type(target_type) ||
        !is_raw_pointer_type(source_type)) {
        semantic_error(ctx, node,
            "reinterpret requires raw pointer source and target types");
        return NULL;
    }

    if (source_type->pointer_access == POINTER_ACCESS_READONLY &&
        target_type->pointer_access == POINTER_ACCESS_MUTABLE) {
        semantic_error(ctx, node,
            "reinterpret cannot discard readonly pointer access");
        return NULL;
    }

    if (source_type->pointer_is_volatile && !target_type->pointer_is_volatile) {
        semantic_error(ctx, node,
            "reinterpret cannot discard volatile pointer access");
        return NULL;
    }

    if (!reinterpret_pointer_conversion_allowed(target_type, source_type)) {
        semantic_error(ctx, node,
            "reinterpret must cross between typed and opaque raw pointers");
        return NULL;
    }

    return target_type;
}

static Type *check_cast_expression(SemanticContext *ctx,Node *node) {

    assert(node);
    assert(node->type == NODE_CAST);

    switch (node->as.cast_expr.kind) {
        case CAST_CHECKED:
            return check_checked_cast_expression(ctx,node);

        case CAST_TRUNCATING:
            return check_truncating_cast_expression(ctx,node);

        case CAST_REINTERPRET:
            return check_reinterpret_expression(ctx,node);
    }

    UNREACHABLE("CastKind");
}

static Type *check_expression(SemanticContext *ctx, Node *node) {

    if (!node) return NULL;

    switch(node->type)
    {
        case NODE_IDENT:
            return check_identifier_expression(ctx, node, IDENTIFIER_USE_READ);

        case NODE_TYPE_REF:
            semantic_error(ctx, node,
                "type reference can only qualify an associated function call");
            return NULL;

        case NODE_UNARY:
        {
            Type *operand = check_value_expression(ctx, node->as.unary.operand);

            if (!operand) return NULL;

            switch(node->as.unary.op)
            {
                case TOK_AND:
                {
                    Node *operand_node =
                        node->as.unary.operand;

                    /*
                     * Both writable and readonly storage have an address.
                     *
                     * Address-of therefore requires a lvalue, but it does not
                     * require a writable lvalue.
                     */
                    if (!require_lvalue(
                            ctx,
                            node,
                            operand_node,
                            "address-of operand is not assignable"
                        )) {
                        return NULL;
                        }

                    SemExprInfo *operand_info =
                        sem_find_expr_info(
                            ctx,
                            operand_node
                        );

                    assert(operand_info);
                    assert(
                        operand_info->value_access ==
                            VALUE_ACCESS_WRITABLE ||
                        operand_info->value_access ==
                            VALUE_ACCESS_READONLY
                    );

                    Type *pointer =
                        new_type(ctx, TYPE_POINTER);

                    pointer->element = operand;

                    /*
                     * Taking the address of qualified storage must preserve
                     * both write permission and volatile access semantics.
                     *
                     *     &writable ordinary storage -> T*
                     *     &readonly storage          -> readonly T*
                     *     &volatile storage          -> volatile T*
                     */
                    pointer->pointer_access =
                        pointer_access_from_value_access(
                            operand_info->value_access
                        );
                    pointer->pointer_is_volatile =
                        operand_info->value_is_volatile;

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

                    if (operand->kind == TYPE_OPAQUE_POINTER) {
                        semantic_error(ctx, node,
                            "cannot dereference an opaque pointer");

                        return NULL;
                    }

                    if (operand->kind != TYPE_POINTER) {
                        semantic_error(ctx, node,
                            "unary '*' requires a pointer operand");

                        return NULL;
                    }

                    if (operand->element &&
                        operand->element->kind == TYPE_STRUCT &&
                        operand->element->struct_is_incomplete) {
                        semantic_error(ctx, node,
                            "cannot dereference a pointer to an incomplete C struct");
                        return NULL;
                    }

                    sem_record_lvalue_info_qualified(
                        ctx,
                        node,
                        operand->element,
                        NULL,
                        value_access_from_pointer_access(
                            operand->pointer_access),
                        operand->pointer_is_volatile);

                    return operand->element;
                }

                case TOK_MINUS:
                {

                    if (operand->kind == TYPE_STRUCT) {
                        StructOperatorBinding *binding =
                            find_struct_operator_binding(
                                ctx, operand, TOK_MINUS, 1);
                        if (!binding) {
                            char type_name[128];
                            format_type_name(operand, type_name, sizeof(type_name));
                            semantic_error_fmt(
                                ctx,
                                node,
                                "unary operator '-' is not defined for %s",
                                type_name
                            );
                            return NULL;
                        }
                        return rewrite_struct_operator_call(
                            ctx,
                            node,
                            binding,
                            node->as.unary.operand,
                            NULL
                        );
                    }

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

            if (left->kind == TYPE_STRUCT &&
                (node->as.binary.op == TOK_PLUS ||
                 node->as.binary.op == TOK_MINUS ||
                 node->as.binary.op == TOK_STAR ||
                 node->as.binary.op == TOK_SLASH)) {
                TokenType op = node->as.binary.op;
                Node *left_node = node->as.binary.left;
                Node *right_node = node->as.binary.right;
                StructOperatorBinding *binding =
                    find_struct_operator_binding(ctx, left, op, 0);
                if (!binding) {
                    char type_name[128];
                    format_type_name(left, type_name, sizeof(type_name));
                    semantic_error_fmt(
                        ctx,
                        node,
                        "operator '%s' is not defined for %s",
                        source_operator_spelling(op),
                        type_name
                    );
                    return NULL;
                }
                return rewrite_struct_operator_call(
                    ctx,
                    node,
                    binding,
                    left_node,
                    right_node
                );
            }

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
                     *     some_s32 % 2
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

                    if (!check_known_integer_divisor(
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
                        result = concretize_inferred_type(
                            ctx,
                            node->as.binary.left,
                            left
                        );
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

                    if (right->kind == TYPE_UNTYPED_INT &&
                        !concretize_inferred_type(
                            ctx,
                            node->as.binary.right,
                            right
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
                    int left_is_null =
                        is_null_type(left);

                    int right_is_null =
                        is_null_type(right);

                    /*
                     * `null == null` has no concrete pointer type to provide
                     * comparison context.
                     */
                    if (left_is_null &&
                        right_is_null) {
                        semantic_error(
                            ctx,
                            node,
                            "cannot compare null without a pointer type"
                        );

                        return NULL;
                    }

                    /*
                     * Either mutable or readonly pointers may be compared with null.
                     */
                    if (is_pointer_null_pair(
                            left,
                            right
                        )) {
                        if (left_is_null) {
                            sem_record_context_conversion_if_needed(
                                ctx,
                                node->as.binary.left,
                                right,
                                left
                            );
                        } else {
                            sem_record_context_conversion_if_needed(
                                ctx,
                                node->as.binary.right,
                                left,
                                right
                            );
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
                    * Any other null combination is invalid, including null == 0.
                    */
                    if (left_is_null ||
                        right_is_null) {
                            semantic_error(
                                ctx,
                                node,
                                "null may only be compared with a pointer"
                            );

                            return NULL;
                    }

                    if (is_numeric_type(left) && is_numeric_type(right)) {
                        Type *common =
                            common_numeric_type(left, right);

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
                            VALUE_CATEGORY_RVALUE);

                            return ctx->type_bool;
                    }

                    /*
                     * Non-numeric operands normally require exact type equality.
                     *
                     * Pointer comparison additionally allows immediate readonly/volatile
                     * qualifier differences when the pointee types are otherwise
                     * exactly equal.
                     */
                    int compatible_types =
                        type_equal(left, right) ||
                        pointer_equality_compatible(
                            left,
                            right
                        );

                    if (!compatible_types) {
                        semantic_error(
                            ctx,
                            node,
                            "comparison type mismatch"
                        );

                        return NULL;
                    }

                    /*
                     * Equal or compatible types do not necessarily support value
                     * equality. Structs, arrays, and functions remain unsupported.
                     */
                    if (!is_equality_comparable_type(left) ||
                        !is_equality_comparable_type(right)) {
                        semantic_error(
                            ctx,
                            node,
                            "type does not support equality comparison"
                        );

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
                     *     x: s64;
                     *     x < 10;
                     *
                     * Invalid:
                     *     a: s32;
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
            StructMethodBinding *method_binding = NULL;
            int method_status = prepare_struct_method_call(
                ctx, node, &method_binding);
            if (method_status < 0)
                return NULL;
            if (method_status > 0)
                return check_prepared_struct_method_call(
                    ctx, node, method_binding);

            Symbol *generic_template =
                resolve_generic_template_callee_no_diag(ctx, node->as.call.callee);

            if (generic_template)
                return check_generic_call(ctx, node, generic_template);

            FunctionOverloadTarget overload_target;
            int overload_status = resolve_function_overload_target(
                ctx, node->as.call.callee, &overload_target);
            if (overload_status < 0)
                return NULL;
            if (overload_status > 0)
                return check_overloaded_function_call(ctx, node, &overload_target);

            Symbol *builtin =
                resolve_builtin_callee(ctx,node->as.call.callee);

            if (builtin) {
                if (node->as.call.type_arguments.count > 0 &&
                    builtin->builtin_kind != BUILTIN_SIZE_OF &&
                    builtin->builtin_kind != BUILTIN_ALIGN_OF) {
                    semantic_error(ctx, node,
                        "explicit type arguments require a generic function or type-layout builtin");
                    return NULL;
                }
                return check_builtin_call(ctx, node, builtin);
            }

            /*
            * Ordinary function-call path.
            */
            Type *callee = check_value_expression(ctx, node->as.call.callee);
            if (!callee)
                return NULL;

            if (callee->kind != TYPE_FUNCTION) {
                semantic_error(ctx, node, "called object is not a function");
                return NULL;
            }

            if (node->as.call.type_arguments.count > 0) {
                semantic_error(ctx, node,
                    "explicit type arguments require a generic function");
                return NULL;
            }

            int argc = node->as.call.arguments.count;

            if ((!callee->function_is_variadic && argc != callee->parameter_count) ||
                (callee->function_is_variadic && argc < callee->parameter_count)) {
                if (callee->function_is_variadic) {
                    semantic_error_fmt(
                        ctx, node,
                        "wrong number of arguments: expected at least %d, got %d",
                        callee->parameter_count,
                        argc
                    );
                } else {
                    semantic_error_fmt(
                        ctx, node,
                        "wrong number of arguments: expected %d, got %d",
                        callee->parameter_count,
                        argc
                    );
                }
                return NULL;
            }

            int ok = 1;

            SemExprInfo *callee_info =
                sem_find_expr_info(ctx, node->as.call.callee);

            Node *callee_decl =
                callee_info &&
                callee_info->symbol &&
                callee_info->symbol->kind == SYMBOL_FUNCTION
                    ? callee_info->symbol->declaration
                    : NULL;

            const int is_extern_c =
                callee_decl &&
                callee_decl->type == NODE_FUNC_DECL &&
                callee_decl->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C;

            for (int i = 0; i < argc; i++) {
                Node *arg = node->as.call.arguments.items[i];

                if (i >= callee->parameter_count) {
                    assert(callee->function_is_variadic);

                    if (!check_c_variadic_argument(ctx, arg))
                        ok = 0;

                    continue;
                }

                Type *param_type = callee->parameters[i];

                if (is_extern_c &&
                    arg->type == NODE_STRING &&
                    source_type_is_readonly_c_char_pointer(
                        callee_decl->as.func_decl.params.items[i]
                            ->as.param_decl.var_type
                    )) {
                    if (!check_extern_c_string_argument(ctx, param_type, arg))
                        ok = 0;

                    continue;
                }

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
            Type *type =
                check_cast_expression(
                    ctx,
                    node
                );

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
             * Module-qualified enum constant. The module prefix may itself be
             * dotted, for example `std.math.Mode.Red`.
             */
            Symbol *qualified_enum = NULL;
            EnumMember *qualified_enum_member = NULL;
            int qualified_enum_status = semantic_qualified_enum_member(
                ctx,
                node,
                1,
                &qualified_enum,
                &qualified_enum_member
            );
            if (qualified_enum_status < 0)
                return NULL;
            if (qualified_enum_status > 0) {
                sem_record_expr_info(
                    ctx,
                    node,
                    qualified_enum->type,
                    qualified_enum,
                    VALUE_CATEGORY_RVALUE
                );
                return qualified_enum->type;
            }

            /* Unqualified enum constant: `Color.Red`. */
            if (node->as.field.object &&
                node->as.field.object->type == NODE_IDENT) {
                Node *object_node = node->as.field.object;
                Symbol *symbol = scope_lookup(
                    ctx->current_scope,
                    object_node->as.ident.data,
                    object_node->as.ident.length
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
                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        symbol->type,
                        symbol,
                        VALUE_CATEGORY_RVALUE
                    );
                    return symbol->type;
                }
            }

            /*
             * Imported/current-module declaration qualification. Pure dotted
             * identifier chains are matched against the longest visible module
             * prefix. This permits both `std.io.print` and ordinary runtime
             * chains such as `state.data.point.x` without backend/name rules.
             */
            SemanticModule *qualified_module = NULL;
            StringView qualified_member_name = string_view_empty();
            int qualified_recognized = 0;
            Symbol *qualified_member = semantic_lookup_qualified_field_symbol(
                ctx,
                node,
                1,
                &qualified_module,
                &qualified_member_name,
                &qualified_recognized
            );
            if (qualified_recognized) {
                if (!qualified_member)
                    return NULL;

                if (qualified_member->kind == SYMBOL_FUNCTION &&
                    scope_count_local_functions_named(
                        qualified_module->scope,
                        qualified_member_name.data,
                        qualified_member_name.length) > 1) {
                    semantic_error_fmt(
                        ctx,
                        node,
                        "overloaded function '%.*s' can only be used as a call target",
                        (int)qualified_member_name.length,
                        qualified_member_name.data
                    );
                    return NULL;
                }

                if (qualified_member->kind == SYMBOL_FUNCTION ||
                    qualified_member->kind == SYMBOL_CONSTANT) {
                    if (qualified_member->kind == SYMBOL_CONSTANT &&
                        !qualified_member->type &&
                        !ensure_constant_symbol_checked(ctx, qualified_member)) {
                        return NULL;
                    }

                    sem_record_expr_info(
                        ctx,
                        node,
                        qualified_member->type,
                        qualified_member,
                        VALUE_CATEGORY_RVALUE
                    );
                    return qualified_member->type;
                }

                if (qualified_member->kind == SYMBOL_VARIABLE) {
                    if (!qualified_member->type) {
                        if (ctx->function_depth == 0 ||
                            !ensure_global_variable_symbol_checked(
                                ctx, qualified_member)) {
                            semantic_error_fmt(
                                ctx,
                                node,
                                "global variable '%.*s.%.*s' is used before its declaration",
                                (int)qualified_module->name.length,
                                qualified_module->name.data,
                                (int)qualified_member_name.length,
                                qualified_member_name.data
                            );
                            return NULL;
                        }
                    }

                    assert(
                        qualified_member->variable_storage ==
                        VARIABLE_STORAGE_GLOBAL
                    );
                    sem_record_expr_info(
                        ctx,
                        node,
                        qualified_member->type,
                        qualified_member,
                        VALUE_CATEGORY_LVALUE
                    );
                    return qualified_member->type;
                }

                if (qualified_member->kind == SYMBOL_TYPE) {
                    semantic_error_fmt(
                        ctx,
                        node,
                        "type '%.*s.%.*s' cannot be used as a value",
                        (int)qualified_module->name.length,
                        qualified_module->name.data,
                        (int)qualified_member_name.length,
                        qualified_member_name.data
                    );
                } else {
                    semantic_error(
                        ctx,
                        node,
                        "module member cannot be used as a value"
                    );
                }
                return NULL;
            }

            /*
             * Normal runtime field access:
             *
             *     point.x
             *     (*pointer).x
             */
            Node *object_node =
                node->as.field.object;

            Type *object_type =
                check_value_expression(
                    ctx,
                    object_node
                );

            if (!object_type)
                return NULL;

            if (object_type->kind == TYPE_SLICE) {
                Type *field_type = NULL;

                if (names_equal(
                        node->as.field.name.data,
                        node->as.field.name.length,
                        "len",
                        sizeof("len") - 1)) {
                    field_type = ctx->type_u64;
                } else if (names_equal(
                               node->as.field.name.data,
                               node->as.field.name.length,
                               "data",
                               sizeof("data") - 1)) {
                    field_type = new_type(ctx, TYPE_POINTER);
                    field_type->element = object_type->element;
                    field_type->pointer_access = object_type->pointer_access;
                } else {
                    semantic_error(
                        ctx,
                        node,
                        "unknown slice field; expected 'data' or 'len'"
                    );
                    return NULL;
                }

                /* Slice metadata is observational; mutate/rebind the slice as a whole. */
                sem_record_expr_info(
                    ctx,
                    node,
                    field_type,
                    NULL,
                    VALUE_CATEGORY_RVALUE
                );
                return field_type;
            }

            if (object_type->kind != TYPE_STRUCT) {
                semantic_error(
                    ctx,
                    node,
                    "field access requires a struct or slice"
                );

                return NULL;
            }

            if (object_type->struct_is_incomplete) {
                semantic_error(ctx, node,
                    "cannot access fields of an incomplete C struct");
                return NULL;
            }

            if (object_type->struct_is_union) {
                semantic_error(ctx, node,
                    "direct C union member access is not supported yet");
                return NULL;
            }

            Type *field_type =
                find_struct_field(
                    object_type,
                    node->as.field.name.data,
                    node->as.field.name.length
                );

            if (!field_type) {
                semantic_error(
                    ctx,
                    node,
                    "unknown struct field"
                );

                return NULL;
            }

            SemExprInfo *object_info =
                sem_find_expr_info(
                    ctx,
                    object_node
                );

            /*
             * A field of an lvalue is also an lvalue and inherits the
             * same access permission and volatile-access property.
             *
             *     writable_point.x       -> writable lvalue
             *     (*readonly_point).x    -> readonly lvalue
             *
             * A field selected from a temporary struct remains an rvalue.
             */
            if (object_info &&
                object_info->value_category ==
                    VALUE_CATEGORY_LVALUE) {
                sem_record_lvalue_info_qualified(
                    ctx,
                    node,
                    field_type,
                    NULL,
                    object_info->value_access,
                    object_info->value_is_volatile
                );
            } else {
                sem_record_expr_info(
                    ctx,
                    node,
                    field_type,
                    NULL,
                    VALUE_CATEGORY_RVALUE
                );
            }

            return field_type;
        }

        case NODE_INDEX:
        {
            Node *object_node =
                node->as.index.object;

            Node *index_node =
                node->as.index.index;

            Type *object_type =
                check_value_expression(
                    ctx,
                    object_node
                );

            if (!object_type)
                return NULL;

            Type *index_type =
                check_value_expression(
                    ctx,
                    index_node
                );

            if (!index_type)
                return NULL;

            if (!is_integer_kind(index_type->kind)) {
                semantic_error(
                    ctx,
                    node,
                    "array index must be integer"
                );

                return NULL;
            }

            if (index_type->kind == TYPE_UNTYPED_INT &&
                !concretize_inferred_type(
                    ctx,
                    index_node,
                    index_type
                )) {
                return NULL;
            }

            if (object_type->kind == TYPE_OPAQUE_POINTER) {
                semantic_error(
                    ctx,
                    node,
                    "cannot index an opaque pointer"
                );

                return NULL;
            }

            if (object_type->kind != TYPE_ARRAY &&
                object_type->kind != TYPE_POINTER &&
                object_type->kind != TYPE_SLICE) {
                semantic_error(
                    ctx,
                    node,
                    "object is not indexable"
                );

                return NULL;
            }

            if ((object_type->kind == TYPE_POINTER ||
                 object_type->kind == TYPE_SLICE) &&
                object_type->element &&
                object_type->element->kind == TYPE_STRUCT &&
                object_type->element->struct_is_incomplete) {
                semantic_error(ctx, node,
                    object_type->kind == TYPE_SLICE
                        ? "cannot index a slice of an incomplete C struct"
                        : "cannot index a pointer to an incomplete C struct");
                return NULL;
            }

            /*
             * Compile-time bounds checking applies only to fixed arrays.
             * Raw pointers carry no length information.
             */
            if (object_type->kind == TYPE_ARRAY &&
                object_type->array_size >= 0 &&
                expression_is_compile_time_constant(ctx, index_node)) {

                ConstValue index_value;

                if (eval_const_expr(
                        ctx,
                        index_node,
                        &index_value
                    ) &&
                    index_value.kind ==
                        CONST_VALUE_INT) {
                    if (index_value.as.integer.is_negative ||
                        index_value.as.integer.magnitude >=
                            (uint64_t)object_type->array_size) {
                        semantic_error(ctx, node,
                            "array index out of bounds");
                    }
                }
            }

            Type *element_type =
                object_type->element;

            SemExprInfo *object_info =
                sem_find_expr_info(ctx, object_node);

            if (object_type->kind ==
                TYPE_POINTER) {
                /*
                 * Pointer indexing denotes pointee storage regardless of
                 * whether the pointer expression itself is an lvalue.
                 *
                 * Access comes from the pointer type:
                 *
                 *     T*[i]                    -> writable ordinary lvalue
                 *     readonly T*[i]           -> readonly ordinary lvalue
                 *     volatile T*[i]           -> writable volatile lvalue
                 *     readonly volatile T*[i]  -> readonly volatile lvalue
                 */
                sem_record_lvalue_info_qualified(
                    ctx,
                    node,
                    element_type,
                    NULL,
                    value_access_from_pointer_access(
                        object_type->pointer_access
                    ),
                    object_type->pointer_is_volatile
                );
            } else if (object_type->kind == TYPE_SLICE) {
                sem_record_lvalue_info_qualified(
                    ctx,
                    node,
                    element_type,
                    NULL,
                    value_access_from_pointer_access(
                        object_type->pointer_access
                    ),
                    0
                );
            } else if (
                object_info &&
                object_info->value_category ==
                    VALUE_CATEGORY_LVALUE
            ) {
                /*
                 * Array indexing inherits access from the array storage.
                 */
                sem_record_lvalue_info_qualified(
                    ctx,
                    node,
                    element_type,
                    NULL,
                    object_info->value_access,
                    object_info->value_is_volatile
                );
            } else {
                /*
                 * Indexing a temporary array produces a rvalue.
                 */
                sem_record_expr_info(
                    ctx,
                    node,
                    element_type,
                    NULL,
                    VALUE_CATEGORY_RVALUE
                );
            }

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
        {
            Type *type = intern_slice_type(
                ctx,
                ctx->type_u8,
                POINTER_ACCESS_READONLY
            );

            if (!type || !check_string_initializer(ctx, type, node))
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
            Type *type = NULL;
            Symbol *symbol = NULL;

            if (node->as.struct_init.module_name.length != 0) {
                symbol = semantic_lookup_qualified_symbol(
                    ctx,
                    node->as.struct_init.module_name,
                    node->as.struct_init.name,
                    node
                );
                if (!symbol)
                    return NULL;

                if (symbol->kind != SYMBOL_TYPE) {
                    semantic_error_fmt(
                        ctx,
                        node,
                        "'%.*s.%.*s' is not a struct type",
                        (int)node->as.struct_init.module_name.length,
                        node->as.struct_init.module_name.data,
                        (int)node->as.struct_init.name.length,
                        node->as.struct_init.name.data
                    );
                    return NULL;
                }
            } else {
                symbol = scope_lookup(
                    ctx->current_scope,
                    node->as.struct_init.name.data,
                    node->as.struct_init.name.length
                );

                if (!symbol || symbol->kind != SYMBOL_TYPE) {
                    semantic_error_name(
                        ctx,
                        node,
                        "unknown struct type",
                        node->as.struct_init.name.data,
                        node->as.struct_init.name.length
                    );
                    return NULL;
                }
            }

            type = resolve_generic_struct_application(
                ctx,
                symbol,
                node->as.struct_init.type_arguments.items,
                node->as.struct_init.type_arguments.count,
                node
            );
            if (!type)
                return NULL;
            if (type->kind != TYPE_STRUCT) {
                semantic_error_fmt(
                    ctx,
                    node,
                    "'%.*s' is not a struct type",
                    (int)node->as.struct_init.name.length,
                    node->as.struct_init.name.data
                );
                return NULL;
            }

            if (type->struct_is_incomplete) {
                semantic_error(ctx, node,
                    "cannot construct an incomplete C struct");
                return NULL;
            }

            if (type->struct_is_union) {
                semantic_error(ctx, node,
                    "direct C union construction is not supported yet");
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
            semantic_error(
                ctx,
                node,
                node->as.array_literal.is_zero_initializer
                    ? "zero initializer requires an expected array type"
                    : "array literal requires an expected array type"
            );
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

    /*
     * If this entire block was already unreachable from its parent,
     * the parent reports the block statement itself. Do not cascade
     * an unreachable diagnostic onto every child statement.
     */
    int diagnose_local_unreachable =
        ctx->flow.reachable;

    for (int i = 0; i < node->as.block.statements.count; i++) {
        Node *statement =
            node->as.block.statements.items[i];

        if (diagnose_local_unreachable &&
            !ctx->flow.reachable) {
            semantic_error(ctx, statement,
                "unreachable statement");
        }

        /*
         * Continue semantic checking even when the statement is
         * unreachable so useful nested diagnostics are retained.
         *
         * Flow operations preserve reachable == 0 once the current
         * path has stopped.
         */
        check_node(ctx, statement);
    }

    scope_pop(ctx);
}

static void check_const_decl(SemanticContext *ctx, Node *node) {
    SemDeclInfo *decl_info = sem_get_or_create_decl_info(ctx, node);

    if (decl_info->semantic_check_complete)
        return;

    if (decl_info->semantic_check_started) {
        semantic_error_name(
            ctx,
            node,
            "cyclic constant definition",
            node->as.const_decl.name.data,
            node->as.const_decl.name.length
        );
        return;
    }

    Symbol *existing = scope_find_local(
        ctx->current_scope,
        node->as.const_decl.name.data,
        node->as.const_decl.name.length
    );

    /*
     * Top-level constants are predeclared before any constant initializer is
     * checked so module qualification is independent of physical input order.
     * Local constants still arrive here without a predeclared symbol.
     */
    if (existing && existing->declaration != node) {
        semantic_error_name(
            ctx, node,
            "duplicate declaration",
            node->as.const_decl.name.data,
            node->as.const_decl.name.length);
        return;
    }

    decl_info->semantic_check_started = 1;

    ConstValue value;

    if (!eval_const_expr(ctx, node->as.const_decl.value, &value)) {
        decl_info->semantic_check_started = 0;
        return;
    }

    Type *value_type = check_value_expression(
        ctx,
        node->as.const_decl.value
    );

    if (!value_type) {
        decl_info->semantic_check_started = 0;
        return;
    }

    Type *type = node->as.const_decl.const_type;

    if (type) {
        type = resolve_type(ctx, type, node);

        if (!type) {
            semantic_error(ctx, node,
                "could not resolve constant type");
            decl_info->semantic_check_started = 0;
            return;
        }

        if (invalid_value_type(type)) {
            semantic_error(ctx, node,
                "constant cannot have type void");
            decl_info->semantic_check_started = 0;
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

            decl_info->semantic_check_started = 0;
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
            decl_info->semantic_check_started = 0;
            return;
        }

        value = converted;

        sem_record_context_conversion_if_needed(
            ctx,
            node->as.const_decl.value,
            type,
            value_type
        );

    } else {
        if (value_type->kind == TYPE_NULL) {
            semantic_error(ctx, node->as.const_decl.value,
                "cannot infer a concrete pointer type from null");
            decl_info->semantic_check_started = 0;
            return;
        }

        type = value.type
            ? value.type
            : const_value_default_type(ctx, &value);

        if (!type) {
            semantic_error(ctx, node,
                "could not infer constant type");
            decl_info->semantic_check_started = 0;
            return;
        }

        value.type = type;
    }

    Symbol *symbol = existing;
    if (!symbol) {
        symbol = scope_define_declared(
            ctx,
            node,
            node->as.const_decl.name,
            SYMBOL_CONSTANT,
            type
        );
    } else {
        assert(symbol->kind == SYMBOL_CONSTANT);
        assert(!symbol->type);
        symbol->type = type;
        sem_record_decl_info(ctx, node, type, symbol);
    }

    (void)symbol;
    decl_info = sem_find_decl_info(ctx, node);
    assert(decl_info);
    decl_info->has_constant_value = 1;
    decl_info->constant_value = value;
    decl_info->semantic_check_started = 0;
    decl_info->semantic_check_complete = 1;
}

static int ensure_constant_symbol_checked(
    SemanticContext *ctx,
    Symbol *symbol
) {
    if (!ctx || !symbol || symbol->kind != SYMBOL_CONSTANT ||
        !symbol->declaration ||
        symbol->declaration->type != NODE_CONST_DECL) {
        return 0;
    }

    SemDeclInfo *info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
    if (info && info->semantic_check_complete)
        return info->has_constant_value;

    if (info && info->semantic_check_started) {
        semantic_error_name(
            ctx,
            symbol->declaration,
            "cyclic constant definition",
            symbol->name.data,
            symbol->name.length
        );
        /* Terminal invalid state: do not re-enter the same cycle later. */
        info->semantic_check_complete = 1;
        return 0;
    }

    SourceFileId saved_source_id = ctx->current_source_id;
    SemanticModule *saved_module = ctx->current_module;
    Scope *saved_scope = ctx->current_scope;

    semantic_select_source_module(ctx, symbol->declaration->span.file_id);
    check_const_decl(ctx, symbol->declaration);

    ctx->current_source_id = saved_source_id;
    ctx->current_module = saved_module;
    ctx->current_scope = saved_scope;

    info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
    return info && info->semantic_check_complete && info->has_constant_value;
}

static void check_switch_statement(SemanticContext *ctx, Node *node) {

    Type *switch_type =
        check_value_expression(ctx, node->as.switch_stmt.expression);

    if (!switch_type)
        return;

    Type *comparison_type = switch_type;

    if (switch_type->kind == TYPE_UNTYPED_INT) {
        comparison_type = concretize_inferred_type(
            ctx,
            node->as.switch_stmt.expression,
            switch_type
        );

        if (!comparison_type)
            return;
    }

    node->as.switch_stmt.resolved_type =
        comparison_type;

    int switch_type_is_valid =
        is_switchable_type(comparison_type);

    if (!switch_type_is_valid) {
        semantic_error(ctx, node,
            "switch expression must be integer, bool, or enum");
    }

    int case_count =
    node->as.switch_stmt.cases.count;

    /*
     * Only successfully checked and converted case values enter this
     * array. Invalid case expressions must not contribute duplicate or
     * exhaustiveness information.
     */
    ConstValue *checked_case_values =
        case_count > 0
            ? arena_alloc(
                ctx->arena,
                sizeof(ConstValue) * case_count
            )
            : NULL;

    int checked_case_value_count = 0;
    int seen_default             = 0;

    /*
     * Every case begins from the state that exists after evaluating
     * the switch expression.
     */
    FlowState incoming =
        flow_clone(ctx, &ctx->flow);

    size_t active_variable_count =
        incoming.count;

    FlowState merged_case_flow =
        (FlowState){0};

    int has_case_flow = 0;

    for (int i = 0; i < case_count; i++) {
        Node *case_node =
            node->as.switch_stmt.cases.items[i];

        if (!case_node ||
            case_node->type != NODE_SWITCH_CASE) {
            continue;
        }

        /*
         * Cases do not execute sequentially and Coglet switches do
         * not fall through. Reset to the common incoming state before
         * checking each case label and body.
         */
        ctx->flow =
            flow_clone(ctx, &incoming);

        if (case_node->as.switch_case.is_default) {
            if (seen_default) {
                semantic_error(ctx, case_node,
                    "duplicate default case");
            }

            seen_default = 1;
        } else {
            Node *case_value_node =
                case_node->as.switch_case.value;

            Type *case_type =
                check_value_expression(ctx, case_value_node);

            if (switch_type_is_valid &&
                case_type) {
                if (!initializer_compatible(comparison_type, case_type)) {
                    semantic_error(ctx, case_node,
                        "switch case type does not match switch expression type");
                } else {
                    ConstValue case_value;

                    if (eval_const_expr(ctx, case_value_node, &case_value)) {

                        ConstValue converted_case;

                        if (coerce_constant_to_type(
                                ctx,
                                case_value_node,
                                &case_value,
                                comparison_type,
                                "switch case value does not fit switch expression type",
                                "switch case value does not fit switch expression type",
                                &converted_case
                            )) {

                            sem_record_context_conversion_if_needed(
                                ctx,
                                case_value_node,
                                comparison_type,
                                case_type
                            );

                            int duplicate_case = 0;

                            for (int j = 0;
                                 j < checked_case_value_count;
                                 j++) {
                                if (const_values_equal(
                                        &checked_case_values[j],
                                        &converted_case
                                    )) {
                                    semantic_error(ctx, case_node,
                                        "duplicate switch case");

                                    duplicate_case = 1;
                                    break;
                                    }
                                 }

                            if (!duplicate_case) {
                                checked_case_values[
                                    checked_case_value_count++
                                ] = converted_case;
                            }
                        }
                    }
                }
            }
        }

        check_node(ctx, case_node->as.switch_case.body);

        FlowState case_flow = ctx->flow;

        if (!has_case_flow) {
            merged_case_flow =
                flow_clone(ctx, &case_flow);

            flow_truncate_to(
                &merged_case_flow,
                active_variable_count
            );

            has_case_flow = 1;
        } else {
            merged_case_flow =
                flow_merge_continuing_paths(
                    ctx,
                    &merged_case_flow,
                    &case_flow,
                    active_variable_count
                );
        }
    }

    /*
     * An empty switch has no executable case and therefore leaves
     * the incoming state unchanged.
     */
    if (!has_case_flow) {
        ctx->flow = incoming;
        return;
    }

    int is_exhaustive =
    switch_case_values_are_exhaustive(
        comparison_type,
        checked_case_values,
        checked_case_value_count,
        seen_default
    );

    if (is_exhaustive) {
        ctx->flow = merged_case_flow;
        return;
    }

    /*
     * A non-exhaustive switch has an implicit path on which no case
     * matches. That path retains the incoming initialization state.
     */
    ctx->flow =
        flow_merge_continuing_paths(
            ctx,
            &merged_case_flow,
            &incoming,
            active_variable_count
        );
}

static Type *check_assignment_target_expression(SemanticContext *ctx, Node *target) {

    assert(target);

    /*
     * A direct variable assignment is a write-only use of the
     * identifier:
     *
     *     value = 10;
     *
     * Other target forms must evaluate their component expressions:
     *
     *     value.field = 10;
     *     value[index] = 10;
     *     *pointer = 10;
     *
     * Those expressions remain ordinary reads of their bases,
     * indexes, or pointers.
     */
    if (target->type == NODE_IDENT)
        return check_identifier_expression(ctx, target, IDENTIFIER_USE_WRITE_TARGET);

    return check_expression(ctx, target);
}

static Symbol *direct_assignment_target_symbol(SemanticContext *ctx, Node *target) {

    assert(target);

    if (target->type != NODE_IDENT)
        return NULL;

    SemExprInfo *info =
        sem_find_expr_info(ctx, target);

    if (!info)
        return NULL;

    if (!info->symbol)
        return NULL;

    if (info->symbol->kind != SYMBOL_VARIABLE)
        return NULL;

    return info->symbol;
}

static int check_assignment_statement(SemanticContext *ctx, Node *node) {

    Node *target_node =
        node->as.assign.target;

    Node *value_node =
        node->as.assign.value;

    Type *target_type =
        check_assignment_target_expression(ctx, target_node);

    if (!target_type)
        return 0;

    if (!require_writable_lvalue(ctx, node, target_node, "assignment target"))
        return 0;

    if (!check_initializer_against_type(ctx, target_type, value_node))
        return 0;

    Symbol *target_symbol =
        direct_assignment_target_symbol(ctx, target_node);

    flow_mark_variable_initialized(ctx, target_symbol);

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

    if (!require_writable_lvalue(ctx, node, target_node, "compound assignment target"))
        return 0;

    if (target_type->kind == TYPE_STRUCT) {
        TokenType binary_op = TOK_EOF;
        switch (operation) {
            case TOK_PLUS_EQUAL:  binary_op = TOK_PLUS;  break;
            case TOK_MINUS_EQUAL: binary_op = TOK_MINUS; break;
            case TOK_STAR_EQUAL:  binary_op = TOK_STAR;  break;
            case TOK_SLASH_EQUAL: binary_op = TOK_SLASH; break;
            default: break;
        }

        if (binary_op != TOK_EOF) {
            StructOperatorBinding *binding = find_struct_operator_binding(
                ctx, target_type, binary_op, 0);
            if (!binding) {
                char type_name[128];
                format_type_name(target_type, type_name, sizeof(type_name));
                semantic_error_fmt(
                    ctx,
                    node,
                    "operator '%s' is not defined for %s",
                    source_operator_spelling(binary_op),
                    type_name
                );
                return 0;
            }

            if (!ensure_struct_method_body_checked(ctx, binding->method, node))
                return 0;

            Type *function_type = binding->method->function_type;
            assert(function_type->parameter_count == 2);
            if (!check_argument_against_parameter(
                    ctx, function_type->parameters[1], value_node)) {
                return 0;
            }

            sem_record_no_value(ctx, node);
            SemExprInfo *info = sem_find_expr_info(ctx, node);
            assert(info);
            info->resolved_operator_function_id =
                binding->method->symbol->declaration_id;
            return 1;
        }
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

            sem_record_context_conversion_if_needed(
                ctx,
                value_node,
                target_type,
                value_type
            );

            if (!check_known_integer_divisor(
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

            sem_record_context_conversion_if_needed(
                ctx,
                value_node,
                target_type,
                value_type
            );

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

            if (value_type->kind == TYPE_UNTYPED_INT &&
                !concretize_inferred_type(
                    ctx,
                    value_node,
                    value_type
                )) {
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

    if (!require_writable_lvalue(ctx, node, target, "increment/decrement target"))
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

    if (!check_array_to_slice_source_access(
            ctx, expected, actual, initializer)) {
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

    sem_record_context_conversion_if_needed(
        ctx,
        initializer,
        expected,
        actual
    );

    return 1;
}

static int source_type_is_readonly_c_char_pointer(const Type *type) {

    return type &&
           type->kind == TYPE_POINTER &&
           type->pointer_access == POINTER_ACCESS_READONLY &&
           !type->pointer_is_volatile &&
           type->element &&
           type->element->kind == TYPE_NAMED &&
           type->element->named_module.length == 0 &&
           names_equal(
               type->element->named_name.data,
               type->element->named_name.length,
               "c_char",
               sizeof("c_char") - 1
           );
}

/*
 * C string literals are a deliberately narrow contextual conversion. They do
 * not introduce general array-to-pointer decay: only a direct string-literal
 * argument to a #extern(c) parameter spelled `readonly c_char*` gets this
 * treatment. The resulting semantic type is the resolved pointer parameter.
 */
static int check_extern_c_string_argument(
    SemanticContext *ctx,
    Type *expected,
    Node *argument
) {
    assert(expected);
    assert(argument && argument->type == NODE_STRING);

    StringDecodeInfo info = string_analyze(argument->as.string_literal);

    if (!info.ok) {
        if (info.invalid_escape) {
            semantic_error_fmt(
                ctx,
                argument,
                "invalid escape sequence '\\%c' in string literal",
                info.invalid_escape
            );
        } else {
            semantic_error(
                ctx,
                argument,
                "unterminated escape sequence in string literal"
            );
        }

        return 0;
    }

    sem_record_expr_info(
        ctx,
        argument,
        expected,
        NULL,
        VALUE_CATEGORY_RVALUE
    );

    sem_record_context_conversion(
        ctx,
        argument,
        expected,
        SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER
    );

    return 1;
}

static int check_c_variadic_argument(SemanticContext *ctx, Node *argument) {

    if (!argument)
        return 0;

    /*
     * A direct string literal in a C variadic position has the same
     * readonly byte-string intent as the fixed #extern(c) string conversion,
     * but there is no expected parameter type to supply context. Record a
     * readonly native-c-char pointer type explicitly; the host C compiler then
     * performs the ordinary array-to-pointer conversion at the ABI boundary.
     */
    if (argument->type == NODE_STRING) {
        Type *c_char = fixed_integer_type_for_c_abi_bits(
            ctx,
            ctx->target.c_char_bits,
            ctx->target.c_char_is_signed
        );

        if (!c_char) {
            semantic_error(ctx, argument,
                "native C char type is not representable for variadic call");
            return 0;
        }

        Type *pointer = new_type(ctx, TYPE_POINTER);
        pointer->element = c_char;
        pointer->pointer_access = POINTER_ACCESS_READONLY;

        return check_extern_c_string_argument(ctx, pointer, argument);
    }

    Type *actual = check_value_expression(ctx, argument);
    if (!actual)
        return 0;

    /*
     * Coglet integer literals have no C suffix/type spelling. Keep the first
     * variadic slice conservative and context-independent: an untyped integer
     * literal is accepted only when it fits native C int, matching the most
     * common default-promoted C argument type. Wider integer values must first
     * acquire an explicit concrete type once backend cast lowering supports
     * that expression form.
     */
    if (actual->kind == TYPE_UNTYPED_INT) {
        Type *c_int = fixed_integer_type_for_c_abi_bits(
            ctx,
            ctx->target.c_int_bits,
            1
        );

        if (!c_int ||
            !check_constant_value_against_type(
                ctx,
                argument,
                c_int,
                "C variadic integer literal does not fit native c_int",
                "C variadic integer argument must be integral"
            )) {
            return 0;
        }

        sem_record_context_conversion_if_needed(
            ctx,
            argument,
            c_int,
            actual
        );

        return 1;
    }

    /* Unsuffixed C floating literals are double, which is the required
     * default-promotion destination for Coglet's untyped/f32 values. */
    if (actual->kind == TYPE_UNTYPED_FLOAT) {
        sem_record_context_conversion_if_needed(
            ctx,
            argument,
            ctx->type_f64,
            actual
        );
        return 1;
    }

    switch (actual->kind) {
        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_POINTER:
        case TYPE_OPAQUE_POINTER:
            if (extern_c_type_supported(actual, 0))
                return 1;
            break;

        case TYPE_ENUM:
            if (actual->enum_is_repr_c)
                return 1;
            break;

        case TYPE_FUNCTION:
            if (actual->function_abi == FUNCTION_ABI_C &&
                extern_c_type_supported(actual, 0)) {
                return 1;
            }
            break;

        case TYPE_VOID:
        case TYPE_NULL:
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_NAMED:
        case TYPE_STRUCT:
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
            break;
    }

    char actual_name[128];
    format_type_name(actual, actual_name, sizeof(actual_name));

    semantic_error_fmt(
        ctx,
        argument,
        "type '%s' is not supported as a C variadic argument",
        actual_name
    );

    return 0;
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

    /*
     * Method-call rewriting can prepend an expression that was already
     * semantically checked while resolving the receiver. Reuse that frozen
     * expression fact rather than re-walking a rewritten synthetic callee.
     * Ordinary source AST nodes are still checked on first use.
     */
    SemExprInfo *existing_info = sem_find_expr_info(ctx, argument);
    Type *actual = existing_info ? existing_info->type
                                 : check_value_expression(ctx, argument);

    if (!actual ||
        (existing_info &&
         (actual->kind == TYPE_VOID ||
          existing_info->value_category == VALUE_CATEGORY_NONE))) {
        if (existing_info)
            semantic_error(ctx, argument, "expression does not produce a value");
        return 0;
    }

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

    if (!check_array_to_slice_source_access(
            ctx, expected, actual, argument)) {
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

    sem_record_context_conversion_if_needed(
        ctx,
        argument,
        expected,
        actual
    );

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
        semantic_error(
            ctx,
            initializer,
            initializer->as.array_literal.is_zero_initializer
                ? "zero initializer can only initialize an array type"
                : "array literal can only initialize an array type"
        );
        return 0;
    }

    /*
     * `{0}` is one contextual initializer for the entire fixed-size array,
     * not a one-element array literal and not C partial-initializer syntax.
     * Lowering maps it to CogIR's existing semantic aggregate-zero constant.
     */
    if (initializer->as.array_literal.is_zero_initializer)
        return 1;

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

    SemDeclInfo *decl_info = sem_get_or_create_decl_info(ctx, node);
    if (decl_info->semantic_check_complete)
        return;

    Symbol *existing = scope_find_local(
        ctx->current_scope,
        node->as.var_decl.name.data,
        node->as.var_decl.name.length
    );

    if (existing && existing->declaration != node) {
        semantic_error_name(
            ctx, node,
            "duplicate variable declaration",
            node->as.var_decl.name.data,
            node->as.var_decl.name.length
        );

        return;
    }

    Type *type = node->as.var_decl.var_type;
    Type *source_type = type;
    Node *init = node->as.var_decl.initializer;

    /*
     * Resolve declared type first.
     *
     * Important for:
     *
     *     values: s32[3] = [1, 2, 3];
     *
     * The array literal needs the expected type s32[3].
     */
    if (type) {
        type = resolve_type(ctx, type, node);

        if (!type)
            return;

        if (invalid_value_type(type)) {
            semantic_error(ctx, node, "variable cannot have type void");
            return;
        }

        if (contains_incomplete_struct_by_value(type)) {
            semantic_error(ctx, node,
                "incomplete C struct cannot be stored by value; use a pointer");
            return;
        }
    }

    if (init) {
        if (type) {
            if (!check_initializer_against_type(ctx, type, init))
                return;

        } else {
            Type *init_type = check_value_expression(ctx, init);

            if (!init_type) return;

            type = concretize_inferred_type(
                ctx,
                init,
                init_type);

            if (!type) return;
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

    if (contains_incomplete_struct_by_value(type)) {
        semantic_error(ctx, node,
            "incomplete C struct cannot be stored by value; use a pointer");
        return;
    }

    VariableStorage storage =
        ctx->function_depth > 0
            ? VARIABLE_STORAGE_LOCAL
            : VARIABLE_STORAGE_GLOBAL;

    Symbol *symbol = existing;
    if (!symbol) {
        symbol = scope_define_declared(
            ctx,
            node,
            node->as.var_decl.name,
            SYMBOL_VARIABLE,
            type);
    } else {
        assert(symbol->kind == SYMBOL_VARIABLE);
        assert(!symbol->type);
        symbol->type = type;
        sem_record_decl_info(ctx, node, type, symbol);
    }

    classify_variable_symbol(ctx, symbol, storage);

    /*
     * Preserve exact C-facing object spelling only where canonical semantic
     * identity is insufficient for storage/call lowering. This includes cfn
     * values and explicitly typed c_* scalar objects (recursively through
     * pointers/arrays), while ordinary Coglet values remain metadata-free.
     */
    if (source_type) {
        SemAbiType *declared_abi = make_sem_abi_type(ctx, source_type, type);
        if (sem_abi_type_requires_storage_spelling(declared_abi)) {
            SemDeclInfo *decl_info = sem_find_decl_info(ctx, node);
            assert(decl_info);
            decl_info->abi_type = declared_abi;
        }
    }

    if (storage == VARIABLE_STORAGE_LOCAL) {
        flow_register_variable(
            ctx,
            symbol,
            init != NULL
        );
    }

    decl_info = sem_find_decl_info(ctx, node);
    assert(decl_info);
    decl_info->semantic_check_complete = 1;
}

static int ensure_global_variable_symbol_checked(
    SemanticContext *ctx,
    Symbol *symbol
) {
    if (!ctx || !symbol || symbol->kind != SYMBOL_VARIABLE ||
        !symbol->declaration ||
        symbol->declaration->type != NODE_VAR_DECL) {
        return 0;
    }

    SemDeclInfo *info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
    if (info && info->semantic_check_complete)
        return symbol->type != NULL;

    if (info && info->semantic_check_started) {
        semantic_error_name(
            ctx,
            symbol->declaration,
            "cyclic global variable definition",
            symbol->name.data,
            symbol->name.length
        );
        return 0;
    }

    if (!info)
        return 0;

    info->semantic_check_started = 1;

    SourceFileId saved_source_id = ctx->current_source_id;
    SemanticModule *saved_module = ctx->current_module;
    Scope *saved_scope = ctx->current_scope;
    int saved_function_depth = ctx->function_depth;

    semantic_select_source_module(ctx, symbol->declaration->span.file_id);
    ctx->function_depth = 0;
    check_var_decl(ctx, symbol->declaration);

    ctx->current_source_id = saved_source_id;
    ctx->current_module = saved_module;
    ctx->current_scope = saved_scope;
    ctx->function_depth = saved_function_depth;

    info = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
    if (info)
        info->semantic_check_started = 0;

    return info && info->semantic_check_complete && symbol->type;
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

    SemDeclInfo *signature_info =
        sem_find_decl_info(ctx, node);

    Type *type = signature_info
        ? signature_info->type
        : node->as.param_decl.var_type;

    if (type && !signature_info) {
        type = resolve_type(ctx, type, node);

        if (!type)
            return;

        if (invalid_value_type(type)) {
            semantic_error(ctx, node,
                "parameter cannot have type void");

            return;
        }

        if (contains_incomplete_struct_by_value(type)) {
            semantic_error(ctx, node,
                "incomplete C struct cannot be passed by value; use a pointer");
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
                check_value_expression(ctx, default_value);

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

    if (contains_incomplete_struct_by_value(type)) {
        semantic_error(ctx, node,
            "incomplete C struct cannot be passed by value; use a pointer");
        return;
    }

    Symbol *symbol = scope_define_declared(
        ctx,
        node,
        node->as.param_decl.name,
        SYMBOL_VARIABLE,
        type);

    classify_variable_symbol(ctx, symbol, VARIABLE_STORAGE_PARAMETER);

    flow_register_variable(ctx, symbol, 1);
}

static StringView semantic_import_qualifier(
    const SemanticModule *module,
    StringView alias
) {
    return alias.length ? alias : module->name;
}

static int semantic_source_has_import_qualifier(
    const SemanticSourceModule *source,
    StringView qualifier
) {
    if (!source)
        return 0;
    for (size_t i = 0; i < source->import_count; i++) {
        StringView existing = semantic_import_qualifier(
            source->imports[i].module,
            source->imports[i].alias
        );
        if (string_view_equals(existing, qualifier))
            return 1;
    }
    return 0;
}

static void semantic_append_import(
    SemanticContext *ctx,
    SemanticSourceModule *source,
    SemanticModule *module,
    StringView alias
) {
    if (source->import_count >= source->import_capacity) {
        size_t new_capacity = source->import_capacity ? source->import_capacity * 2 : 4;
        SemanticImportBinding *grown = arena_alloc(
            ctx->arena,
            new_capacity * sizeof(*grown)
        );
        if (source->imports && source->import_count) {
            memcpy(
                grown,
                source->imports,
                source->import_count * sizeof(*grown)
            );
        }
        source->imports = grown;
        source->import_capacity = new_capacity;
    }

    source->imports[source->import_count++] = (SemanticImportBinding){
        .module = module,
        .alias = alias,
    };
}

static void prepare_program_modules(SemanticContext *ctx, Node *program)
{
    NodeList *stmts = &program->as.program.statements;

    /* First discover each physical file's declared module. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        SemanticSourceModule *source =
            semantic_source_module(ctx, stmt->span.file_id);
        if (!source)
            continue;

        if (stmt->type == NODE_MODULE_DECL) {
            if (source->module_decl) {
                semantic_error(ctx, stmt, "duplicate module declaration in source file");
                continue;
            }

            if (source->saw_import_directive) {
                semantic_error(ctx, stmt, "module declaration must precede imports");
            }
            if (source->saw_non_directive) {
                semantic_error(ctx, stmt, "module declaration must precede other top-level declarations/statements");
            }

            SemanticModule *module =
                semantic_find_module(ctx, stmt->as.module_decl.name);
            if (!module) {
                module = semantic_create_module(
                    ctx,
                    stmt->as.module_decl.name,
                    0
                );
            }

            source->module = module;
            source->module_decl = stmt;
            continue;
        }

        if (stmt->type == NODE_IMPORT_DECL) {
            source->saw_import_directive = 1;
            if (source->saw_non_directive) {
                semantic_error(ctx, stmt, "import declarations must precede other top-level declarations/statements");
            }
            continue;
        }

        source->saw_non_directive = 1;
    }

    /* Resolve file-scoped imports only after all module names are known. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type != NODE_IMPORT_DECL)
            continue;

        SemanticSourceModule *source =
            semantic_source_module(ctx, stmt->span.file_id);
        if (!source)
            continue;

        SemanticModule *module =
            semantic_find_module(ctx, stmt->as.import_decl.name);
        if (!module) {
            semantic_error_fmt(
                ctx,
                stmt,
                "unknown module '%.*s'",
                (int)stmt->as.import_decl.name.length,
                stmt->as.import_decl.name.data
            );
            continue;
        }

        if (module == source->module) {
            semantic_error_fmt(
                ctx,
                stmt,
                "module '%.*s' cannot import itself",
                (int)module->name.length,
                module->name.data
            );
            continue;
        }

        if (semantic_source_imports_module(source, module)) {
            semantic_error_fmt(
                ctx,
                stmt,
                "duplicate import of module '%.*s'",
                (int)module->name.length,
                module->name.data
            );
            continue;
        }

        StringView qualifier = semantic_import_qualifier(
            module,
            stmt->as.import_decl.alias
        );
        if (source->module && string_view_equals(source->module->name, qualifier)) {
            semantic_error_fmt(
                ctx,
                stmt,
                "import qualifier '%.*s' conflicts with the current module name",
                (int)qualifier.length,
                qualifier.data
            );
            continue;
        }
        if (semantic_source_has_import_qualifier(source, qualifier)) {
            semantic_error_fmt(
                ctx,
                stmt,
                "duplicate import qualifier '%.*s'",
                (int)qualifier.length,
                qualifier.data
            );
            continue;
        }

        semantic_append_import(
            ctx, source, module, stmt->as.import_decl.alias);
    }
}

static void validate_source_entry_signature(SemanticContext *ctx, Node *node)
{
    if (!node || node->type != NODE_FUNC_DECL ||
        !ctx->current_module || !ctx->current_module->is_root ||
        !names_equal(node->as.func_decl.name.data, node->as.func_decl.name.length, "main", 4) ||
        !node->as.func_decl.resolved_type) {
        return;
    }

    Type *function_type = node->as.func_decl.resolved_type;
    Type *source_return = node->as.func_decl.return_type;
    if (function_type->kind != TYPE_FUNCTION ||
        function_type->function_abi != FUNCTION_ABI_COGLET ||
        node->as.func_decl.linkage != FUNCTION_LINKAGE_COGLET ||
        node->as.func_decl.is_repr_c ||
        node->as.func_decl.is_variadic ||
        node->as.func_decl.params.count != 0 ||
        !source_return || source_return->kind != TYPE_S32) {
        semantic_error(
            ctx,
            node,
            "executable entry point must have signature 'main::() -> s32'"
        );
        return;
    }

    SemDeclInfo *info = sem_find_decl_info(ctx, node);
    assert(info);
    info->is_executable_entry = 1;
}

static SemDeclInfo *semantic_find_nominal_decl_for_type(
    SemanticContext *ctx,
    const Type *type
)
{
    if (!ctx || !type ||
        (type->kind != TYPE_STRUCT && type->kind != TYPE_ENUM)) {
        return NULL;
    }

    for (SemDeclInfo *info = ctx->decl_infos; info; info = info->next) {
        if (info->type != type || !info->node)
            continue;
        if (info->node->type == NODE_STRUCT_DECL ||
            info->node->type == NODE_ENUM_DECL) {
            return info;
        }
    }

    return NULL;
}

static int semantic_export_type_is_public(
    SemanticContext *ctx,
    const Type *type,
    Node *export_node,
    StringView export_name
)
{
    if (!type)
        return 1;

    switch (type->kind) {
        case TYPE_VOID:
        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_OPAQUE_POINTER:
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
            return 1;

        case TYPE_POINTER:
        case TYPE_ARRAY:
        case TYPE_SLICE:
            return semantic_export_type_is_public(
                ctx,
                type->element,
                export_node,
                export_name
            );

        case TYPE_FUNCTION:
            for (int i = 0; i < type->parameter_count; i++) {
                if (!semantic_export_type_is_public(
                        ctx,
                        type->parameters[i],
                        export_node,
                        export_name
                    )) {
                    return 0;
                }
            }
            return semantic_export_type_is_public(
                ctx,
                type->return_type,
                export_node,
                export_name
            );

        case TYPE_STRUCT:
        {
            if (type->struct_generic_template_id != (size_t)-1) {
                for (int i = 0; i < type->struct_type_argument_count; i++) {
                    if (!semantic_export_type_is_public(
                            ctx,
                            type->struct_type_arguments[i],
                            export_node,
                            export_name
                        )) {
                        return 0;
                    }
                }

                SemDeclInfo *template_info = semantic_get_decl_info_by_id(
                    ctx,
                    type->struct_generic_template_id
                );
                if (!template_info || template_info->is_exported)
                    return 1;

                StringView private_name = generic_decl_name(template_info->node);
                semantic_error_fmt(
                    ctx,
                    export_node,
                    "exported declaration '%.*s' exposes private type '%.*s'",
                    (int)export_name.length,
                    export_name.data,
                    (int)private_name.length,
                    private_name.data
                );
                return 0;
            }

            SemDeclInfo *nominal = semantic_find_nominal_decl_for_type(ctx, type);
            if (!nominal || nominal->is_exported)
                return 1;

            semantic_error_fmt(
                ctx,
                export_node,
                "exported declaration '%.*s' exposes private type '%.*s'",
                (int)export_name.length,
                export_name.data,
                (int)type->struct_name.length,
                type->struct_name.data
            );
            return 0;
        }

        case TYPE_ENUM:
        {
            SemDeclInfo *nominal = semantic_find_nominal_decl_for_type(ctx, type);
            if (!nominal || nominal->is_exported)
                return 1;

            semantic_error_fmt(
                ctx,
                export_node,
                "exported declaration '%.*s' exposes private type '%.*s'",
                (int)export_name.length,
                export_name.data,
                (int)type->enum_name.length,
                type->enum_name.data
            );
            return 0;
        }

        case TYPE_NAMED:
            assert(!"resolved exported interface contains TYPE_NAMED");
            return 0;
    }

    assert(!"unhandled TypeKind in semantic_export_type_is_public");
    return 0;
}

static StringView semantic_declaration_name(Node *node)
{
    if (!node)
        return string_view_empty();

    switch (node->type) {
        case NODE_VAR_DECL:
            return node->as.var_decl.name;
        case NODE_FUNC_DECL:
            return node->as.func_decl.name;
        case NODE_STRUCT_DECL:
            return node->as.struct_decl.name;
        case NODE_ENUM_DECL:
            return node->as.enum_decl.name;
        case NODE_CONST_DECL:
            return node->as.const_decl.name;
        default:
            return string_view_empty();
    }
}

static int validate_generic_export_source_type(
    SemanticContext *ctx,
    Node *generic_decl,
    Type *source_type,
    Node *owner,
    StringView owner_name
) {
    if (!source_type)
        return 1;
    if (generic_decl->type == NODE_STRUCT_DECL &&
        source_type->kind == TYPE_NAMED &&
        source_type->named_module.length == 0 &&
        source_type->type_argument_count == 0 &&
        source_type->named_name.length == 4 &&
        memcmp(source_type->named_name.data, "Self", 4) == 0) {
        return 1;
    }
    if (source_type_is_generic_parameter(generic_decl, source_type, NULL))
        return 1;

    if (source_type->kind == TYPE_POINTER || source_type->kind == TYPE_ARRAY ||
        source_type->kind == TYPE_SLICE) {
        return validate_generic_export_source_type(
            ctx,
            generic_decl,
            source_type->element,
            owner,
            owner_name
        );
    }

    if (source_type->kind == TYPE_NAMED && source_type->type_argument_count > 0) {
        for (int i = 0; i < source_type->type_argument_count; i++) {
            if (!validate_generic_export_source_type(
                    ctx,
                    generic_decl,
                    source_type->type_arguments[i],
                    owner,
                    owner_name)) {
                return 0;
            }
        }

        Symbol *symbol = source_type->named_module.length != 0
            ? semantic_lookup_visible_qualified_symbol_no_diag(
                ctx, source_type->named_module, source_type->named_name)
            : scope_lookup(
                ctx->current_scope,
                source_type->named_name.data,
                source_type->named_name.length
            );
        if (!symbol || symbol->kind != SYMBOL_TYPE) {
            /* Reuse the ordinary resolver for a precise unknown/not-a-type diagnostic. */
            return resolve_type(ctx, source_type, owner) != NULL;
        }

        if (symbol_is_generic_struct_template(symbol)) {
            SemDeclInfo *nominal = sem_find_decl_info_by_id(ctx, symbol->declaration_id);
            if (nominal && nominal->is_exported)
                return 1;

            semantic_error_fmt(
                ctx,
                owner,
                "exported declaration '%.*s' exposes private type '%.*s'",
                (int)owner_name.length,
                owner_name.data,
                (int)symbol->name.length,
                symbol->name.data
            );
            return 0;
        }
    }

    if (source_type->kind == TYPE_FUNCTION) {
        for (int i = 0; i < source_type->parameter_count; i++) {
            if (!validate_generic_export_source_type(
                    ctx,
                    generic_decl,
                    source_type->parameters[i],
                    owner,
                    owner_name)) {
                return 0;
            }
        }
        return validate_generic_export_source_type(
            ctx,
            generic_decl,
            source_type->return_type,
            owner,
            owner_name
        );
    }

    Type *resolved = resolve_type(ctx, source_type, owner);
    if (!resolved)
        return 0;
    return semantic_export_type_is_public(ctx, resolved, owner, owner_name);
}

static void validate_generic_function_export(SemanticContext *ctx, Node *node)
{
    StringView name = node->as.func_decl.name;
    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    semantic_select_source_module(ctx, node->span.file_id);

    int ok = 1;
    for (int i = 0; i < node->as.func_decl.params.count && ok; i++) {
        ok = validate_generic_export_source_type(
            ctx,
            node,
            node->as.func_decl.params.items[i]->as.param_decl.var_type,
            node,
            name
        );
    }
    if (ok) {
        validate_generic_export_source_type(
            ctx,
            node,
            node->as.func_decl.return_type,
            node,
            name
        );
    }

    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;
}

static void validate_generic_struct_export(SemanticContext *ctx, Node *node)
{
    StringView name = node->as.struct_decl.name;
    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    semantic_select_source_module(ctx, node->span.file_id);
    for (int i = 0; i < node->as.struct_decl.fields.count; i++) {
        Node *field = node->as.struct_decl.fields.items[i];
        if (!validate_generic_export_source_type(
                ctx,
                node,
                field->as.struct_field_decl.var_type,
                node,
                name)) {
            break;
        }
    }

    for (int i = 0; i < node->as.struct_decl.methods.count; i++) {
        Node *method = node->as.struct_decl.methods.items[i];
        int ok = 1;
        for (int j = 0; j < method->as.func_decl.params.count && ok; j++) {
            ok = validate_generic_export_source_type(
                ctx,
                node,
                method->as.func_decl.params.items[j]->as.param_decl.var_type,
                node,
                name
            );
        }
        if (ok) {
            ok = validate_generic_export_source_type(
                ctx,
                node,
                method->as.func_decl.return_type,
                node,
                name
            );
        }
        if (!ok)
            break;
    }

    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;
}

static void validate_one_exported_declaration(
    SemanticContext *ctx,
    Node *node
)
{
    if (!node || !node->is_exported)
        return;

    SemanticSourceModule *source = semantic_source_module(ctx, node->span.file_id);
    if (!source || !source->module)
        return;

    StringView name = semantic_declaration_name(node);

    if (source->module->is_root) {
        semantic_error(
            ctx,
            node,
            "export is valid only for declarations in a named module"
        );
        return;
    }

    if (node->type == NODE_FUNC_DECL &&
        node->as.func_decl.type_parameters.count > 0) {
        validate_generic_function_export(ctx, node);
        return;
    }

    if (node->type == NODE_STRUCT_DECL &&
        node->as.struct_decl.type_parameters.count > 0) {
        validate_generic_struct_export(ctx, node);
        return;
    }

    SemDeclInfo *info = sem_find_decl_info(ctx, node);
    if (!info || !info->type)
        return;

    if (node->type == NODE_STRUCT_DECL) {
        Type *type = info->type;
        if (type->kind != TYPE_STRUCT)
            return;

        for (int i = 0; i < type->field_count; i++) {
            if (!semantic_export_type_is_public(
                    ctx,
                    type->fields[i].type,
                    node,
                    name
                )) {
                return;
            }
        }
        for (int i = 0; i < node->as.struct_decl.methods.count; i++) {
            Node *method = node->as.struct_decl.methods.items[i];
            if (method->as.func_decl.resolved_type &&
                !semantic_export_type_is_public(
                    ctx,
                    method->as.func_decl.resolved_type,
                    node,
                    name
                )) {
                return;
            }
        }
        return;
    }

    semantic_export_type_is_public(ctx, info->type, node, name);
}

static void validate_program_exports(SemanticContext *ctx, Node *program)
{
    NodeList *stmts = &program->as.program.statements;

    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (!stmt)
            continue;

        if (stmt->type == NODE_VAR_DECL_GROUP) {
            if (stmt->is_exported) {
                SemanticSourceModule *source =
                    semantic_source_module(ctx, stmt->span.file_id);
                if (source && source->module && source->module->is_root) {
                    semantic_error(
                        ctx,
                        stmt,
                        "export is valid only for declarations in a named module"
                    );
                    continue;
                }
            }

            for (int j = 0; j < stmt->as.var_decl_group.declarations.count; j++)
                validate_one_exported_declaration(
                    ctx,
                    stmt->as.var_decl_group.declarations.items[j]
                );
            continue;
        }

        validate_one_exported_declaration(ctx, stmt);
    }
}

static void check_program(SemanticContext *ctx, Node *node)
{
    NodeList *stmts = &node->as.program.statements;
    prepare_program_modules(ctx, node);

    /* Pass 1: register nominal type shells in each module namespace. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type == NODE_MODULE_DECL || stmt->type == NODE_IMPORT_DECL)
            continue;

        semantic_select_source_module(ctx, stmt->span.file_id);

        if (stmt->type == NODE_STRUCT_DECL) {
            if (stmt->as.struct_decl.type_parameters.count > 0)
                declare_generic_struct_template(ctx, stmt);
            else
                declare_struct_shell(ctx, stmt);
        }
        if (stmt->type == NODE_ENUM_DECL)
            declare_enum_shell(ctx, stmt);
    }

    /* Pass 2: fill type bodies after every module's shells exist. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type == NODE_MODULE_DECL || stmt->type == NODE_IMPORT_DECL)
            continue;

        semantic_select_source_module(ctx, stmt->span.file_id);

        if (stmt->type == NODE_STRUCT_DECL &&
            stmt->as.struct_decl.type_parameters.count == 0)
            fill_struct_fields(ctx, stmt);
        if (stmt->type == NODE_ENUM_DECL)
            fill_enum_members(ctx, stmt);
    }

    validate_repr_c_struct_layouts(ctx, node);

    /* Pass 3: register all function signatures in their module namespaces. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type != NODE_STRUCT_DECL ||
            stmt->as.struct_decl.type_parameters.count > 0) {
            continue;
        }

        semantic_select_source_module(ctx, stmt->span.file_id);
        register_struct_method_signatures(ctx, stmt);
        register_struct_operator_bindings(ctx, stmt);
    }

    /* Pass 3b: register all top-level function signatures. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type != NODE_FUNC_DECL)
            continue;

        semantic_select_source_module(ctx, stmt->span.file_id);
        if (stmt->as.func_decl.type_parameters.count > 0) {
            declare_generic_function_template(ctx, stmt);
        } else {
            declare_function_signature(ctx, stmt);
            validate_source_entry_signature(ctx, stmt);
        }
    }

    /*
     * Pass 4: predeclare top-level data names without checking their values.
     *
     * This makes module namespace membership independent of physical file
     * order while preserving the historical source-order diagnostic/checking
     * pass below. Constants may be evaluated lazily across modules; globals
     * may be forced when referenced from an earlier function body. Runtime
     * module-initializer execution order remains a CogIR input-order property.
     */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];

        if (stmt->type == NODE_CONST_DECL) {
            semantic_select_source_module(ctx, stmt->span.file_id);
            if (!scope_find_local(
                    ctx->current_scope,
                    stmt->as.const_decl.name.data,
                    stmt->as.const_decl.name.length
                )) {
                scope_predeclare_declared(
                    ctx,
                    stmt,
                    stmt->as.const_decl.name,
                    SYMBOL_CONSTANT
                );
            }
            continue;
        }

        if (stmt->type == NODE_VAR_DECL) {
            semantic_select_source_module(ctx, stmt->span.file_id);
            if (!scope_find_local(
                    ctx->current_scope,
                    stmt->as.var_decl.name.data,
                    stmt->as.var_decl.name.length
                )) {
                scope_predeclare_declared(
                    ctx,
                    stmt,
                    stmt->as.var_decl.name,
                    SYMBOL_VARIABLE
                );
            }
            continue;
        }

        if (stmt->type == NODE_VAR_DECL_GROUP) {
            semantic_select_source_module(ctx, stmt->span.file_id);
            for (int j = 0; j < stmt->as.var_decl_group.declarations.count; j++) {
                Node *decl = stmt->as.var_decl_group.declarations.items[j];
                if (!scope_find_local(
                        ctx->current_scope,
                        decl->as.var_decl.name.data,
                        decl->as.var_decl.name.length
                    )) {
                    scope_predeclare_declared(
                        ctx,
                        decl,
                        decl->as.var_decl.name,
                        SYMBOL_VARIABLE
                    );
                }
            }
        }
    }

    /* Pass 5: check bodies and runtime-bearing top-level syntax in source order. */
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type == NODE_MODULE_DECL || stmt->type == NODE_IMPORT_DECL ||
            stmt->type == NODE_ENUM_DECL) {
            continue;
        }

        if (stmt->type == NODE_STRUCT_DECL) {
            if (stmt->as.struct_decl.type_parameters.count == 0)
                check_struct_method_bodies(ctx, stmt);
            continue;
        }

        semantic_select_source_module(ctx, stmt->span.file_id);

        if (stmt->type == NODE_FUNC_DECL) {
            if (stmt->as.func_decl.type_parameters.count == 0)
                check_function_body(ctx, stmt);
            continue;
        }

        check_node(ctx, stmt);
    }

    validate_concrete_coglet_struct_layouts(ctx);
    validate_program_exports(ctx, node);

    ctx->current_module = ctx->root_module;
    ctx->current_scope = ctx->root_module->scope;
}

static void check_if_statement(SemanticContext *ctx, Node *node) {

    Type *cond =
        check_value_expression(ctx, node->as.if_stmt.condition);

    /*
     * A NULL type means expression checking already reported an
     * error. Only report the Boolean error for a valid type.
     */
    if (cond && !is_bool_type(cond)) {
        semantic_error(ctx, node->as.if_stmt.condition,
            "if condition must be a boolean expression");
    }

    /*
     * Both branches begin with exactly the state that exists after
     * checking the condition.
     */
    FlowState incoming =
        flow_clone(ctx, &ctx->flow);

    size_t active_variable_count =
        incoming.count;

    /*
     * Then branch.
     */
    ctx->flow =
        flow_clone(ctx, &incoming);

    check_node(ctx, node->as.if_stmt.then_branch);

    FlowState then_flow = ctx->flow;

    /*
     * Else branch.
     *
     * When there is no explicit else, the false path performs no
     * statements and therefore retains the incoming state.
     */
    ctx->flow =
        flow_clone(ctx, &incoming);

    if (node->as.if_stmt.else_branch) {
        check_node(ctx, node->as.if_stmt.else_branch);
    }

    FlowState else_flow = ctx->flow;

    ctx->flow =
        flow_merge_continuing_paths(
            ctx,
            &then_flow,
            &else_flow,
            active_variable_count
        );
}

static void check_while_statement(SemanticContext *ctx, Node *node) {

    Type *condition_type =
        check_value_expression(ctx, node->as.while_stmt.condition);

    if (condition_type && !is_bool_type(condition_type)) {
        semantic_error(ctx,node->as.while_stmt.condition,
            "while condition must be a boolean expression");
    }

    int condition_is_always_true =
        condition_type &&
        is_bool_type(condition_type) &&
        expression_is_compile_time_true(ctx, node->as.while_stmt.condition);

    FlowState incoming =
        flow_clone(ctx, &ctx->flow);

    LoopFlowContext loop = {
        .active_variable_count = incoming.count,
        .parent = ctx->current_loop,
    };

    ctx->current_loop = &loop;
    ctx->flow = flow_clone(ctx, &incoming);

    ctx->loop_depth++;

    check_node(ctx, node->as.while_stmt.body);

    ctx->loop_depth--;
    ctx->current_loop = loop.parent;

    /*
    * A literal-true loop cannot reach its following statement unless
    * some reachable path executes break.
    *
    * Body fallthrough and continue both begin another iteration.
    * Return leaves the function rather than the loop.
    */
    if (condition_is_always_true && !loop.has_break_flow) {
        ctx->flow = flow_clone(ctx, &incoming);

        ctx->flow.reachable = 0;
        return;
    }

    ctx->flow =
        loop_conservative_exit_flow(
            ctx,
            &loop,
            &incoming);
}

static void check_for_statement(SemanticContext *ctx, Node *node) {

    int condition_is_always_true =
        node->as.for_stmt.condition == NULL;

    if (node->as.for_stmt.condition) {
        Type *condition_type =
            check_value_expression(ctx, node->as.for_stmt.condition);

        if (condition_type && !is_bool_type(condition_type)) {
            semantic_error(ctx, node->as.for_stmt.condition,
                "for condition must be a boolean expression");
        }

        if (condition_type && is_bool_type(condition_type)) {
            condition_is_always_true =
                expression_is_compile_time_true(
                    ctx,
                    node->as.for_stmt.condition
                );
        }
    }

    FlowState incoming =
        flow_clone(ctx, &ctx->flow);

    LoopFlowContext loop = {
        .active_variable_count = incoming.count,
        .parent = ctx->current_loop,
    };

    ctx->current_loop = &loop;
    ctx->flow = flow_clone(ctx, &incoming);

    ctx->loop_depth++;

    check_node(ctx, node->as.for_stmt.body);

    FlowState body_flow = ctx->flow;

    /*
     * Both ordinary body fallthrough and continue to execute the post
     * expression. Break and return paths do not.
     */
    ctx->flow =
        loop_iteration_flow(
            ctx,
            &loop,
            &body_flow
        );

    if (node->as.for_stmt.post) {
        check_statement_expression(
            ctx,
            node->as.for_stmt.post
        );
    }

    ctx->loop_depth--;
    ctx->current_loop = loop.parent;

    if (condition_is_always_true && !loop.has_break_flow) {
        ctx->flow = flow_clone(ctx, &incoming);

        ctx->flow.reachable = 0;
        return;
    }

    ctx->flow =
        loop_conservative_exit_flow(
            ctx,
            &loop,
            &incoming);
}

// Check functions -------------------------------------------
static int extern_c_type_supported(const Type *type, int allow_void)
{
    if (!type)
        return 0;

    switch (type->kind) {
        case TYPE_VOID:
            return allow_void;

        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_OPAQUE_POINTER:
            return 1;

        case TYPE_POINTER:
            /*
             * Typed raw pointers may point at supported scalar/raw-pointer
             * types or explicitly C-represented structs. Incomplete C structs
             * are specifically useful here: only their address crosses ABI.
             */
            if (type->element && type->element->kind == TYPE_STRUCT)
                return type->element->struct_is_repr_c;
            return extern_c_type_supported(type->element, 0);

        case TYPE_STRUCT:
            return type->struct_is_repr_c && !type->struct_is_incomplete;

        case TYPE_ENUM:
            return type->enum_is_repr_c;

        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_ARRAY:
        case TYPE_SLICE:
        case TYPE_NAMED:
            return 0;

        case TYPE_FUNCTION:
            if (type->function_abi != FUNCTION_ABI_C)
                return 0;

            for (int i = 0; i < type->parameter_count; i++) {
                if (!extern_c_type_supported(type->parameters[i], 0))
                    return 0;
            }

            return extern_c_type_supported(type->return_type, 1);
    }

    assert(!"unhandled TypeKind in extern_c_type_supported");
    return 0;
}

static int validate_c_abi_function_signature(
    SemanticContext *ctx,
    Node *func,
    Type *type,
    const char *annotation
)
{
    assert(func);
    assert(type && type->kind == TYPE_FUNCTION);

    int ok = 1;

    if (type->function_is_variadic &&
        type->function_call_conv == C_CALL_STDCALL) {
        semantic_error_fmt(
            ctx,
            func,
            "%s cannot use call=stdcall with C variadics",
            annotation
        );
        ok = 0;
    }

    for (int i = 0; i < type->parameter_count; i++) {
        Node *param = func->as.func_decl.params.items[i];

        if (param->as.param_decl.default_value) {
            semantic_error_fmt(
                ctx,
                param,
                "%s parameters cannot have default values",
                annotation
            );
            ok = 0;
        }

        if (extern_c_type_supported(type->parameters[i], 0))
            continue;

        char type_name[128];
        format_type_name(type->parameters[i], type_name, sizeof(type_name));

        semantic_error_fmt(
            ctx,
            param,
            "%s parameter type '%s' is not supported by the current C ABI subset",
            annotation,
            type_name
        );
        ok = 0;
    }

    if (!extern_c_type_supported(type->return_type, 1)) {
        char type_name[128];
        format_type_name(type->return_type, type_name, sizeof(type_name));

        semantic_error_fmt(
            ctx,
            func,
            "%s return type '%s' is not supported by the current C ABI subset",
            annotation,
            type_name
        );
        ok = 0;
    }

    return ok;
}

static Type *make_function_type(SemanticContext *ctx, Node *func)
{
    Type *type = new_type(ctx, TYPE_FUNCTION);

    type->function_abi =
        (func->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C ||
         func->as.func_decl.is_repr_c)
            ? FUNCTION_ABI_C
            : FUNCTION_ABI_COGLET;

    type->function_call_conv =
        type->function_abi == FUNCTION_ABI_C
            ? func->as.func_decl.c_call_conv
            : C_CALL_DEFAULT;

    type->parameter_count = func->as.func_decl.params.count;
    type->function_is_variadic = func->as.func_decl.is_variadic;

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

        if (contains_incomplete_struct_by_value(param_type)) {
            semantic_error(ctx, param,
                "incomplete C struct cannot be passed by value; use a pointer");
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

    if (contains_incomplete_struct_by_value(type->return_type)) {
        semantic_error(ctx, func,
            "incomplete C struct cannot be returned by value; use a pointer");
        return NULL;
    }

    return type;
}

static int function_decl_supports_overloading(const Node *node) {
    return node && node->type == NODE_FUNC_DECL &&
           node->as.func_decl.type_parameters.count == 0 &&
           node->as.func_decl.linkage == FUNCTION_LINKAGE_COGLET &&
           !node->as.func_decl.is_repr_c &&
           !node->as.func_decl.is_variadic;
}

static int function_types_have_same_parameters(const Type *a, const Type *b) {
    if (!a || !b || a->kind != TYPE_FUNCTION || b->kind != TYPE_FUNCTION ||
        a->parameter_count != b->parameter_count) {
        return 0;
    }
    for (int i = 0; i < a->parameter_count; i++) {
        if (!type_equal(a->parameters[i], b->parameters[i]))
            return 0;
    }
    return 1;
}

static int validate_function_overload_set_member(
    SemanticContext *ctx,
    Node *node,
    Type *func_type
) {
    Scope *scope = ctx->current_scope;
    int same_name_count = 0;

    for (Symbol *sym = scope->symbols; sym; sym = sym->next) {
        if (!names_equal(
                sym->name.data, sym->name.length,
                node->as.func_decl.name.data, node->as.func_decl.name.length)) {
            continue;
        }
        same_name_count++;

        if (sym->kind != SYMBOL_FUNCTION || !sym->declaration ||
            !function_decl_supports_overloading(sym->declaration) ||
            !function_decl_supports_overloading(node)) {
            semantic_error_name(
                ctx, node,
                "duplicate declaration",
                node->as.func_decl.name.data,
                node->as.func_decl.name.length
            );
            return 0;
        }

        if (function_types_have_same_parameters(sym->type, func_type)) {
            semantic_error_fmt(
                ctx,
                node,
                "duplicate overload for function '%.*s' with the same parameter types",
                (int)node->as.func_decl.name.length,
                node->as.func_decl.name.data
            );
            return 0;
        }
    }

    if (same_name_count > 0 &&
        ctx->current_module && ctx->current_module->is_root &&
        names_equal(
            node->as.func_decl.name.data, node->as.func_decl.name.length,
            "main", 4)) {
        semantic_error(ctx, node, "executable entry point 'main' cannot be overloaded");
        return 0;
    }

    return 1;
}

static int validate_generic_parameter_declarations(
    SemanticContext *ctx,
    Node *node,
    GenericTypeParameterList parameters
) {
    for (int i = 0; i < parameters.count; i++) {
        GenericTypeParameter type_parameter = parameters.items[i];
        if (generic_builtin_constraint(type_parameter.constraint) ==
            GENERIC_CONSTRAINT_INVALID) {
            semantic_error_fmt(
                ctx,
                node,
                "unknown generic type constraint '%.*s'; expected one of: integer, signed_integer, unsigned_integer, floating, numeric, ordered",
                (int)type_parameter.constraint.length,
                type_parameter.constraint.data
            );
            return 0;
        }
        for (int j = 0; j < i; j++) {
            if (string_view_equals(type_parameter.name, parameters.items[j].name)) {
                semantic_error_fmt(
                    ctx,
                    node,
                    "duplicate generic type parameter '%.*s'",
                    (int)type_parameter.name.length,
                    type_parameter.name.data
                );
                return 0;
            }
        }
    }
    return 1;
}

static int declare_generic_struct_template(SemanticContext *ctx, Node *node)
{
    assert(node && node->type == NODE_STRUCT_DECL);
    assert(node->as.struct_decl.type_parameters.count > 0);

    node->as.struct_decl.resolved_type = NULL;

    if (scope_find_local(
            ctx->current_scope,
            node->as.struct_decl.name.data,
            node->as.struct_decl.name.length)) {
        semantic_error_name(
            ctx,
            node,
            "duplicate declaration",
            node->as.struct_decl.name.data,
            node->as.struct_decl.name.length
        );
        return 0;
    }

    if (node->as.struct_decl.is_repr_c || node->as.struct_decl.is_union ||
        node->as.struct_decl.is_incomplete) {
        semantic_error(
            ctx,
            node,
            "generic structs are currently limited to ordinary complete Coglet structs"
        );
        return 0;
    }

    if (!validate_generic_parameter_declarations(
            ctx, node, node->as.struct_decl.type_parameters)) {
        return 0;
    }

    for (int i = 0; i < node->as.struct_decl.methods.count; i++) {
        Node *method = node->as.struct_decl.methods.items[i];
        if (method->as.func_decl.type_parameters.count > 0) {
            semantic_error(ctx, method, "generic methods are not supported");
            return 0;
        }
    }

    scope_predeclare_declared(
        ctx,
        node,
        node->as.struct_decl.name,
        SYMBOL_TYPE
    );
    SemDeclInfo *info = sem_find_decl_info(ctx, node);
    assert(info && info->symbol);
    info->is_generic_template = 1;
    return 1;
}

static int declare_generic_function_template(SemanticContext *ctx, Node *node)
{
    assert(node && node->type == NODE_FUNC_DECL);
    assert(node->as.func_decl.type_parameters.count > 0);

    node->as.func_decl.resolved_type = NULL;

    if (scope_find_local(
            ctx->current_scope,
            node->as.func_decl.name.data,
            node->as.func_decl.name.length)) {
        semantic_error_name(
            ctx,
            node,
            "duplicate declaration",
            node->as.func_decl.name.data,
            node->as.func_decl.name.length
        );
        return 0;
    }

    if (node->as.func_decl.linkage != FUNCTION_LINKAGE_COGLET ||
        node->as.func_decl.is_repr_c ||
        node->as.func_decl.is_variadic) {
        semantic_error(
            ctx,
            node,
            "generic functions are currently limited to ordinary non-variadic Coglet definitions"
        );
        return 0;
    }

    if (!validate_generic_parameter_declarations(
            ctx, node, node->as.func_decl.type_parameters)) {
        return 0;
    }

    scope_predeclare_declared(
        ctx,
        node,
        node->as.func_decl.name,
        SYMBOL_FUNCTION
    );
    SemDeclInfo *info = sem_find_decl_info(ctx, node);
    assert(info && info->symbol);
    info->is_generic_template = 1;
    return 1;
}

static int declare_function_signature(SemanticContext *ctx, Node *node)
{
    node->as.func_decl.resolved_type = NULL;

    Type *func_type = make_function_type(ctx, node);

    if (!func_type)
        return 0;

    if (!validate_function_overload_set_member(ctx, node, func_type))
        return 0;

    if (node->as.func_decl.is_variadic &&
        node->as.func_decl.linkage != FUNCTION_LINKAGE_EXTERN_C) {
        semantic_error(
            ctx,
            node,
            "C variadics are currently allowed only on #extern(c) declarations and cfn types"
        );
        return 0;
    }

    if (node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C &&
        !validate_c_abi_function_signature(ctx, node, func_type, "#extern(c)")) {
        return 0;
    }

    if (node->as.func_decl.is_repr_c &&
        !validate_c_abi_function_signature(ctx, node, func_type, "#repr(c) function")) {
        return 0;
    }

    scope_define_declared(
        ctx,
        node,
        node->as.func_decl.name,
        SYMBOL_FUNCTION,
        func_type
    );

    node->as.func_decl.resolved_type = func_type;

    SemDeclInfo *func_info = sem_find_decl_info(ctx, node);
    assert(func_info);
    assert(func_info->abi_kind == SEM_DECL_ABI_NONE);

    func_info->abi_kind = SEM_DECL_ABI_FUNCTION;
    func_info->abi.function.abi = func_type->function_abi;
    func_info->abi.function.linkage =
        node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C
            ? SEM_FUNCTION_LINKAGE_EXTERNAL
            : SEM_FUNCTION_LINKAGE_INTERNAL;
    func_info->abi.function.c_call_conv = func_type->function_call_conv;
    func_info->abi.function.is_variadic = func_type->function_is_variadic;


    if (func_info->abi.function.linkage == SEM_FUNCTION_LINKAGE_EXTERNAL) {
        func_info->abi.function.external_symbol =
            string_view_is_empty(node->as.func_decl.external_name)
                ? node->as.func_decl.name
                : node->as.func_decl.external_name;
    }

    if (func_type->function_abi == FUNCTION_ABI_C) {
        func_info->abi.function.return_abi_type = make_sem_abi_type(
            ctx,
            node->as.func_decl.return_type,
            func_type->return_type
        );
    }

    /*
     * Parameter declarations have semantic identity as part of the function
     * signature even when there is no body (for example #extern(c)). A body
     * later attaches the lexical parameter Symbol to this same declaration
     * record rather than allocating a second identity.
     */
    for (int i = 0; i < node->as.func_decl.params.count; i++) {
        Node *param = node->as.func_decl.params.items[i];
        SemDeclInfo *param_info = sem_record_decl_info(
            ctx,
            param,
            func_type->parameters[i],
            NULL
        );

        if (func_type->function_abi == FUNCTION_ABI_C ||
            (func_type->parameters[i]->kind == TYPE_FUNCTION &&
             func_type->parameters[i]->function_abi == FUNCTION_ABI_C)) {
            param_info->abi_type = make_sem_abi_type(
                ctx,
                param->as.param_decl.var_type,
                func_type->parameters[i]
            );
        }
    }

    return 1;
}

static void check_function_body(SemanticContext *ctx, Node *node)
{
    Type *func_type =
        node->as.func_decl.resolved_type;

    if (!func_type || func_type->kind != TYPE_FUNCTION)
        return;

    if (node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C) {
        assert(node->as.func_decl.body == NULL);
        return;
    }

    size_t saved_next_variable_id =
        ctx->next_variable_id;

    FlowState saved_flow =
        ctx->flow;

    /*
     * Flow-owner IDs are never reused during this semantic check.
     * Variable IDs restart within each new function owner.
     */
    size_t flow_owner_id =
        ctx->next_flow_owner_id++;

    assert(flow_owner_id != INVALID_FLOW_OWNER_ID);

    ctx->next_variable_id = 0;

    flow_init(
        &ctx->flow,
        flow_owner_id
    );

    scope_push(ctx);

    for (int i = 0; i < node->as.func_decl.params.count; i++)
        check_param_decl(ctx, node->as.func_decl.params.items[i]);

    int saved_loop_depth        = ctx->loop_depth;

    LoopFlowContext *saved_loop = ctx->current_loop;

    Type *saved_return_type     = ctx->current_return_type;

    ctx->loop_depth = 0;
    ctx->current_loop = NULL;
    ctx->current_return_type = func_type->return_type;

    ctx->function_depth++;

    check_node(ctx, node->as.func_decl.body);

    /*
    * A non-void function is invalid only when normal control flow can
    * reach the end of its body.
    *
    * This accepts both:
    *
    *     return value;
    *
    * and provably non-terminating control flow such as:
    *
    *     while true {
    *     }
    */
    if (func_type->return_type &&
        func_type->return_type->kind != TYPE_VOID &&
        ctx->flow.reachable) {
        semantic_error(
            ctx,
            node,
            "non-void function may not return a value"
        );
    }

    ctx->function_depth--;
    ctx->current_return_type = saved_return_type;
    ctx->loop_depth          = saved_loop_depth;
    ctx->current_loop        = saved_loop;

    scope_pop(ctx);

    ctx->next_variable_id = saved_next_variable_id;

    ctx->flow = saved_flow;
}

static void check_function(SemanticContext *ctx, Node *node)
{
    if (node->as.func_decl.type_parameters.count > 0) {
        semantic_error(ctx, node,
            "generic function declarations must be top level in the initial implementation");
        return;
    }

    if (node->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C &&
        ctx->function_depth > 0) {
        semantic_error(ctx, node, "#extern(c) declarations must be at top level");
        return;
    }

    if (node->as.func_decl.is_repr_c && ctx->function_depth > 0) {
        semantic_error(ctx, node, "#repr(c) functions must be at top level");
        return;
    }

    if (!declare_function_signature(ctx, node))
        return;

    check_function_body(ctx, node);
}

static int string_view_is_self(StringView name)
{
    return name.length == 4 && memcmp(name.data, "self", 4) == 0;
}

static StructMethodBinding *find_struct_method_binding(
    SemanticContext *ctx,
    Type *owner_type,
    StringView name
) {
    for (StructMethodBinding *binding = ctx->struct_methods;
         binding;
         binding = binding->next) {
        if (binding->owner_type == owner_type &&
            string_view_equals(binding->source_name, name)) {
            return binding;
        }
    }
    return NULL;
}

static StructOperatorBinding *find_struct_operator_binding(
    SemanticContext *ctx,
    Type *owner_type,
    TokenType op,
    int is_unary
) {
    for (StructOperatorBinding *binding = ctx->struct_operators;
         binding;
         binding = binding->next) {
        if (binding->owner_type == owner_type &&
            binding->op == op &&
            binding->is_unary == !!is_unary) {
            return binding;
        }
    }
    return NULL;
}

static StringView make_struct_method_function_name(
    SemanticContext *ctx,
    Type *owner_type,
    StringView method_name
) {
    StringView module_name = ctx->current_module
        ? ctx->current_module->name
        : string_view_empty();
    size_t module_prefix = module_name.length ? module_name.length + 1 : 0;
    size_t length = module_prefix + owner_type->struct_name.length + 1 + method_name.length;
    char *buffer = arena_alloc(ctx->arena, length + 1);
    size_t at = 0;
    if (module_name.length) {
        memcpy(buffer + at, module_name.data, module_name.length);
        at += module_name.length;
        buffer[at++] = '.';
    }
    memcpy(buffer + at, owner_type->struct_name.data, owner_type->struct_name.length);
    at += owner_type->struct_name.length;
    buffer[at++] = '.';
    memcpy(buffer + at, method_name.data, method_name.length);
    at += method_name.length;
    buffer[at] = '\0';
    return string_view(buffer, length);
}

static int record_struct_method_signature(
    SemanticContext *ctx,
    Node *owner_decl,
    Node *method
) {
    assert(owner_decl && owner_decl->type == NODE_STRUCT_DECL);
    assert(method && method->type == NODE_FUNC_DECL);
    Type *owner_type = owner_decl->as.struct_decl.resolved_type;
    assert(owner_type && owner_type->kind == TYPE_STRUCT);

    StringView source_name = method->as.func_decl.name;
    if (find_struct_method_binding(ctx, owner_type, source_name)) {
        semantic_error_fmt(
            ctx,
            method,
            "duplicate method '%.*s' on struct '%.*s'",
            (int)source_name.length,
            source_name.data,
            (int)owner_type->struct_name.length,
            owner_type->struct_name.data
        );
        return 0;
    }

    for (int i = 0; i < owner_type->field_count; i++) {
        if (string_view_equals(owner_type->fields[i].name, source_name)) {
            semantic_error_fmt(
                ctx,
                method,
                "method '%.*s' conflicts with a field of the same name",
                (int)source_name.length,
                source_name.data
            );
            return 0;
        }
    }

    if (method->as.func_decl.linkage != FUNCTION_LINKAGE_COGLET ||
        method->as.func_decl.is_repr_c ||
        method->as.func_decl.is_variadic ||
        method->as.func_decl.type_parameters.count > 0) {
        semantic_error(
            ctx,
            method,
            "generic methods are not supported"
        );
        return 0;
    }

    int is_instance = 0;
    for (int i = 0; i < method->as.func_decl.params.count; i++) {
        Node *param = method->as.func_decl.params.items[i];
        if (!string_view_is_self(param->as.param_decl.name))
            continue;
        if (i != 0) {
            semantic_error(ctx, param, "'self' must be the first method parameter");
            return 0;
        }
        is_instance = 1;
    }

    scope_push(ctx);
    scope_define(
        ctx,
        string_view_from_cstr("Self"),
        SYMBOL_TYPE,
        owner_type
    );
    Type *function_type = make_function_type(ctx, method);
    scope_pop(ctx);
    if (!function_type)
        return 0;

    if (is_instance) {
        Type *receiver = function_type->parameters[0];
        int valid_receiver = type_equal(receiver, owner_type) ||
            (receiver->kind == TYPE_POINTER && type_equal(receiver->element, owner_type));
        if (!valid_receiver) {
            semantic_error_fmt(
                ctx,
                method->as.func_decl.params.items[0],
                "method receiver 'self' must have type '%.*s' or a pointer to it",
                (int)owner_type->struct_name.length,
                owner_type->struct_name.data
            );
            return 0;
        }
    }

    method->as.func_decl.name = make_struct_method_function_name(
        ctx, owner_type, source_name);
    method->as.func_decl.resolved_type = function_type;

    Symbol *symbol = arena_new(ctx->arena, Symbol);
    *symbol = (Symbol){
        .name = method->as.func_decl.name,
        .kind = SYMBOL_FUNCTION,
        .builtin_kind = BUILTIN_NONE,
        .type = function_type,
        .declaration = NULL,
        .declaration_id = INVALID_SEM_DECL_ID,
        .variable_storage = VARIABLE_STORAGE_NONE,
        .flow_owner_id = INVALID_FLOW_OWNER_ID,
        .variable_id = INVALID_VARIABLE_ID,
        .next = NULL,
    };

    SemDeclInfo *func_info = sem_record_decl_info(ctx, method, function_type, symbol);
    SemDeclInfo *owner_info = sem_find_decl_info(ctx, owner_decl);
    func_info->is_deferred_generic_method =
        owner_info && owner_info->is_generic_specialization;
    func_info->abi_kind = SEM_DECL_ABI_FUNCTION;
    func_info->abi.function.abi = FUNCTION_ABI_COGLET;
    func_info->abi.function.linkage = SEM_FUNCTION_LINKAGE_INTERNAL;
    func_info->abi.function.c_call_conv = C_CALL_DEFAULT;
    func_info->abi.function.is_variadic = 0;

    for (int i = 0; i < method->as.func_decl.params.count; i++) {
        sem_record_decl_info(
            ctx,
            method->as.func_decl.params.items[i],
            function_type->parameters[i],
            NULL
        );
    }

    StructMethodBinding *binding = arena_new(ctx->arena, StructMethodBinding);
    *binding = (StructMethodBinding){
        .owner_type = owner_type,
        .owner_decl = owner_decl,
        .source_name = source_name,
        .function = method,
        .symbol = symbol,
        .function_type = function_type,
        .is_instance = is_instance,
        .next = ctx->struct_methods,
    };
    ctx->struct_methods = binding;
    return 1;
}

static int register_struct_method_signatures(SemanticContext *ctx, Node *owner_decl)
{
    if (!owner_decl || owner_decl->type != NODE_STRUCT_DECL ||
        owner_decl->as.struct_decl.methods.count == 0) {
        return 1;
    }

    if (owner_decl->as.struct_decl.is_repr_c || owner_decl->as.struct_decl.is_union ||
        owner_decl->as.struct_decl.is_incomplete) {
        semantic_error(
            ctx,
            owner_decl,
            "methods are currently supported only on ordinary complete Coglet structs"
        );
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < owner_decl->as.struct_decl.methods.count; i++) {
        if (!record_struct_method_signature(
                ctx, owner_decl, owner_decl->as.struct_decl.methods.items[i])) {
            ok = 0;
        }
    }
    return ok;
}

static int register_struct_operator_bindings(SemanticContext *ctx, Node *owner_decl)
{
    if (!owner_decl || owner_decl->type != NODE_STRUCT_DECL ||
        owner_decl->as.struct_decl.operators.count == 0) {
        return 1;
    }

    Type *owner_type = owner_decl->as.struct_decl.resolved_type;
    if (!owner_type || owner_type->kind != TYPE_STRUCT)
        return 0;

    if (owner_decl->as.struct_decl.is_repr_c || owner_decl->as.struct_decl.is_union ||
        owner_decl->as.struct_decl.is_incomplete) {
        semantic_error(
            ctx,
            owner_decl,
            "operators are currently supported only on ordinary complete Coglet structs"
        );
        return 0;
    }

    int ok = 1;
    for (int i = 0; i < owner_decl->as.struct_decl.operators.count; i++) {
        StructOperatorDecl declaration = owner_decl->as.struct_decl.operators.items[i];

        if (find_struct_operator_binding(
                ctx, owner_type, declaration.op, declaration.is_unary)) {
            semantic_error_fmt(
                ctx,
                owner_decl,
                "duplicate %soperator mapping for '%s' on struct '%.*s'",
                declaration.is_unary ? "unary " : "",
                source_operator_spelling(declaration.op),
                (int)owner_type->struct_name.length,
                owner_type->struct_name.data
            );
            ok = 0;
            continue;
        }

        StructMethodBinding *method = find_struct_method_binding(
            ctx, owner_type, declaration.method_name);
        if (!method) {
            semantic_error_fmt(
                ctx,
                owner_decl,
                "operator '%s' refers to unknown method '%.*s'",
                source_operator_spelling(declaration.op),
                (int)declaration.method_name.length,
                declaration.method_name.data
            );
            ok = 0;
            continue;
        }

        if (!method->is_instance) {
            semantic_error_fmt(
                ctx,
                method->function,
                "operator '%s' must map to an instance method",
                source_operator_spelling(declaration.op)
            );
            ok = 0;
            continue;
        }

        Type *function_type = method->function_type;
        int expected_parameter_count = declaration.is_unary ? 1 : 2;
        if (function_type->parameter_count != expected_parameter_count) {
            if (declaration.is_unary) {
                semantic_error_fmt(
                    ctx,
                    method->function,
                    "unary operator '%s' method must have only the 'self' parameter",
                    source_operator_spelling(declaration.op)
                );
            } else {
                semantic_error_fmt(
                    ctx,
                    method->function,
                    "binary operator '%s' method must take exactly one right-hand operand",
                    source_operator_spelling(declaration.op)
                );
            }
            ok = 0;
            continue;
        }

        /* Operators are pure value transformations in this first version. */
        if (!type_equal(function_type->parameters[0], owner_type)) {
            semantic_error_fmt(
                ctx,
                method->function,
                "operator '%s' receiver must be 'self: Self' by value",
                source_operator_spelling(declaration.op)
            );
            ok = 0;
            continue;
        }

        if (!type_equal(function_type->return_type, owner_type)) {
            semantic_error_fmt(
                ctx,
                method->function,
                "operator '%s' method must return Self",
                source_operator_spelling(declaration.op)
            );
            ok = 0;
            continue;
        }

        StructOperatorBinding *binding = arena_new(ctx->arena, StructOperatorBinding);
        *binding = (StructOperatorBinding){
            .owner_type = owner_type,
            .op = declaration.op,
            .is_unary = declaration.is_unary,
            .method = method,
            .next = ctx->struct_operators,
        };
        ctx->struct_operators = binding;
    }

    return ok;
}

static void check_struct_method_bodies(SemanticContext *ctx, Node *owner_decl)
{
    if (!owner_decl || owner_decl->type != NODE_STRUCT_DECL)
        return;
    for (int i = 0; i < owner_decl->as.struct_decl.methods.count; i++) {
        Node *method = owner_decl->as.struct_decl.methods.items[i];
        if (method->as.func_decl.resolved_type) {
            scope_push(ctx);
            scope_define(
                ctx,
                string_view_from_cstr("Self"),
                SYMBOL_TYPE,
                owner_decl->as.struct_decl.resolved_type
            );
            check_function_body(ctx, method);
            scope_pop(ctx);
        }
    }
}

static int declare_struct_shell(SemanticContext *ctx, Node *node) {

    if (node->as.struct_decl.is_union && !node->as.struct_decl.is_repr_c) {
        semantic_error(ctx, node, "union declarations currently require #repr(c)");
        return 0;
    }

    if (node->as.struct_decl.is_union && node->as.struct_decl.is_incomplete) {
        semantic_error(ctx, node, "incomplete #repr(c) unions are not supported yet");
        return 0;
    }

    if (scope_find_local(ctx->current_scope, node->as.struct_decl.name.data, node->as.struct_decl.name.length)) {
        semantic_error_name(ctx, node, "duplicate declaration",
            node->as.struct_decl.name.data, node->as.struct_decl.name.length);

        return 0;
    }

    Type *type = new_type(ctx, TYPE_STRUCT);

    type->struct_name.data   = node->as.struct_decl.name.data;
    type->struct_name.length = node->as.struct_decl.name.length;
    type->struct_is_repr_c       = node->as.struct_decl.is_repr_c;
    type->struct_repr_c_packed   = node->as.struct_decl.repr_c_packed;
    type->struct_repr_c_align    = node->as.struct_decl.repr_c_align;
    type->struct_is_union        = node->as.struct_decl.is_union;
    type->struct_is_incomplete   = node->as.struct_decl.is_incomplete;

    node->as.struct_decl.resolved_type = type;

    scope_define_declared(
        ctx,
        node,
        node->as.struct_decl.name,
        SYMBOL_TYPE,
        type
    );

    SemDeclInfo *decl_info = sem_find_decl_info(ctx, node);
    assert(decl_info);
    assert(decl_info->abi_kind == SEM_DECL_ABI_NONE);

    decl_info->abi_kind = SEM_DECL_ABI_AGGREGATE;
    decl_info->abi.aggregate.representation =
        type->struct_is_repr_c ? SEM_ABI_REPR_C : SEM_ABI_REPR_COGLET;
    decl_info->abi.aggregate.aggregate_kind =
        type->struct_is_union ? SEM_AGGREGATE_UNION : SEM_AGGREGATE_STRUCT;
    decl_info->abi.aggregate.is_incomplete = type->struct_is_incomplete;
    decl_info->abi.aggregate.is_packed = type->struct_repr_c_packed;
    decl_info->abi.aggregate.explicit_alignment =
        type->struct_repr_c_align > 0
            ? (unsigned)type->struct_repr_c_align
            : 0u;

    return 1;
}

static int repr_c_struct_field_type_supported(const Type *type)
{
    if (!type) return 0;

    switch (type->kind) {
        case TYPE_BOOL:
        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
        case TYPE_F32:
        case TYPE_F64:
        case TYPE_OPAQUE_POINTER:
            return 1;

        case TYPE_POINTER:
            /*
             * Pointer fields may reference supported scalar types and other
             * #repr(c) structs, including incomplete foreign structs. The
             * pointee itself is not laid out inline.
             */
            if (!type->element) return 0;
            if (type->element->kind == TYPE_STRUCT)
                return type->element->struct_is_repr_c;
            return repr_c_struct_field_type_supported(type->element);

        case TYPE_STRUCT:
            return type->struct_is_repr_c && !type->struct_is_incomplete;

        case TYPE_ENUM:
            return type->enum_is_repr_c;

        case TYPE_ARRAY:
            /*
             * C-compatible aggregate fields may contain fixed-size arrays.
             * Unsized and zero-length arrays do not have a portable standard-C
             * object layout, so keep them outside the ABI subset.
             */
            return type->array_size > 0 &&
                   type->element != NULL &&
                   repr_c_struct_field_type_supported(type->element);

        case TYPE_SLICE:
        case TYPE_VOID:
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_NAMED:
            return 0;

        case TYPE_FUNCTION:
            return extern_c_type_supported(type, 0);
    }

    assert(!"unhandled TypeKind in repr_c_struct_field_type_supported");
    return 0;
}

static int find_repr_c_struct_node_index(Node **nodes, int count, const Type *type)
{
    for (int i = 0; i < count; i++) {
        if (nodes[i]->as.struct_decl.resolved_type == type)
            return i;
    }

    return -1;
}

static void validate_repr_c_struct_layout_dfs(
    SemanticContext *ctx,
    Node **nodes,
    int count,
    unsigned char *states,
    int index
) {
    if (states[index] == 2)
        return;

    if (states[index] == 1)
        return;

    states[index] = 1;

    Node *decl = nodes[index];
    Type *type = decl->as.struct_decl.resolved_type;

    if (type && type->fields) {
        for (int i = 0; i < type->field_count; i++) {
            Type *field_type = type->fields[i].type;

            /*
             * Direct struct-valued fields and fixed arrays of structs both
             * contribute to inline layout. Raw pointers may participate in
             * arbitrary recursive graphs because their pointee is not laid
             * out inline.
             */
            while (field_type && field_type->kind == TYPE_ARRAY)
                field_type = field_type->element;

            if (!field_type || field_type->kind != TYPE_STRUCT)
                continue;

            int dependency =
                find_repr_c_struct_node_index(nodes, count, field_type);

            if (dependency < 0)
                continue;

            Node *field = decl->as.struct_decl.fields.items[i];

            if (states[dependency] == 1) {
                semantic_error_fmt(
                    ctx,
                    field,
                    "#repr(c) by-value field '%.*s' creates a recursive %s layout",
                    (int)field->as.struct_field_decl.name.length,
                    field->as.struct_field_decl.name.data,
                    decl->as.struct_decl.is_union ? "union" : "struct"
                );
                continue;
            }

            validate_repr_c_struct_layout_dfs(
                ctx,
                nodes,
                count,
                states,
                dependency
            );
        }
    }

    states[index] = 2;
}

static void validate_repr_c_struct_layouts(SemanticContext *ctx, Node *program)
{
    NodeList *stmts = &program->as.program.statements;
    int count = 0;

    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type == NODE_STRUCT_DECL && stmt->as.struct_decl.is_repr_c)
            count++;
    }

    if (count == 0)
        return;

    Node **nodes = arena_alloc(ctx->arena, sizeof(Node *) * (size_t)count);
    unsigned char *states = arena_alloc(ctx->arena, (size_t)count);
    memset(states, 0, (size_t)count);

    int at = 0;
    for (int i = 0; i < stmts->count; i++) {
        Node *stmt = stmts->items[i];
        if (stmt->type == NODE_STRUCT_DECL && stmt->as.struct_decl.is_repr_c)
            nodes[at++] = stmt;
    }

    for (int i = 0; i < count; i++) {
        if (states[i] == 0)
            validate_repr_c_struct_layout_dfs(ctx, nodes, count, states, i);
    }
}

static void fill_struct_fields(SemanticContext *ctx, Node *node) {

    Symbol *sym =
        scope_find_local(
            ctx->current_scope,
            node->as.struct_decl.name.data,
            node->as.struct_decl.name.length);

    // shell registration failed (duplicate name), nothing to fill in
    if (!sym) return;

    Type *type = sym->type;

    if (node->as.struct_decl.is_incomplete) {
        if (!node->as.struct_decl.is_repr_c) {
            semantic_error(ctx, node, "incomplete struct declarations require #repr(c)");
        }

        if (node->as.struct_decl.repr_c_packed ||
            node->as.struct_decl.repr_c_align > 0) {
            semantic_error(
                ctx,
                node,
                "incomplete #repr(c) structs cannot specify packed or alignment controls"
            );
        }

        /* An incomplete declaration intentionally has no Coglet field layout. */
        type->field_count = 0;
        type->fields = NULL;
        return;
    }

    if (node->as.struct_decl.repr_c_align > 0) {
        unsigned align = (unsigned)node->as.struct_decl.repr_c_align;
        if ((align & (align - 1u)) != 0) {
            semantic_error(
                ctx,
                node,
                "#repr(c) alignment must be a power of two"
            );
        }
    }

    if (node->as.struct_decl.is_repr_c &&
        !node->as.struct_decl.is_incomplete &&
        node->as.struct_decl.fields.count == 0) {
        semantic_error(
            ctx,
            node,
            node->as.struct_decl.is_union
                ? "#repr(c) unions must contain at least one field"
                : "#repr(c) structs must contain at least one field"
        );
    }

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

            semantic_error(
                ctx,
                field,
                node->as.struct_decl.is_union
                    ? "union field cannot have type void"
                    : "struct field cannot have type void"
            );

            type->fields[i].type = NULL;
            continue;
        }

        if (contains_incomplete_struct_by_value(field_type)) {
            semantic_error(ctx, field,
                "incomplete C struct cannot be stored by value; use a pointer");

            type->fields[i].type = NULL;
            continue;
        }

        if (node->as.struct_decl.is_repr_c &&
            !repr_c_struct_field_type_supported(field_type)) {
            char type_name[128];
            format_type_name(field_type, type_name, sizeof(type_name));

            semantic_error_fmt(
                ctx,
                field,
                node->as.struct_decl.is_union
                    ? "#repr(c) union field type '%s' is not supported by the current C ABI subset"
                    : "#repr(c) struct field type '%s' is not supported by the current C ABI subset",
                type_name
            );

            type->fields[i].type = NULL;
            continue;
        }

        type->fields[i].type = field_type;
        SemDeclInfo *field_info =
            sem_record_decl_info(ctx, field, field_type, NULL);

        if (type->struct_is_repr_c) {
            field_info->abi_type = make_sem_abi_type(
                ctx,
                field->as.struct_field_decl.var_type,
                field_type
            );
        }
    }
}

static GenericStructSpecialization *instantiate_generic_struct(
    SemanticContext *ctx,
    Symbol *template_symbol,
    Type *const *type_arguments,
    int type_argument_count,
    Node *use_node
) {
    assert(ctx);
    assert(template_symbol);
    assert(symbol_is_generic_struct_template(template_symbol));

    Node *template_decl = template_symbol->declaration;
    SemDeclId template_id = template_symbol->declaration_id;

    GenericStructSpecialization *cached = find_generic_struct_specialization(
        ctx, template_id, type_arguments, type_argument_count);
    if (cached)
        return cached;

    enum { MAX_ACTIVE_GENERIC_STRUCT_SPECIALIZATIONS = 32 };
    if (active_generic_struct_template_depth(ctx, template_id) >=
        MAX_ACTIVE_GENERIC_STRUCT_SPECIALIZATIONS) {
        semantic_error_fmt(
            ctx,
            use_node,
            "generic struct '%.*s' appears to instantiate recursively with ever-changing type arguments",
            (int)template_decl->as.struct_decl.name.length,
            template_decl->as.struct_decl.name.data
        );
        return NULL;
    }

    GenericStructSpecialization *spec =
        arena_new(ctx->arena, GenericStructSpecialization);
    memset(spec, 0, sizeof(*spec));
    spec->template_id = template_id;
    spec->template_decl = template_decl;
    spec->type_argument_count = type_argument_count;
    spec->state = GENERIC_SPECIALIZATION_CHECKING;
    spec->type_arguments = arena_alloc(
        ctx->arena,
        sizeof(Type *) * (size_t)type_argument_count
    );
    memcpy(
        spec->type_arguments,
        type_arguments,
        sizeof(Type *) * (size_t)type_argument_count
    );
    spec->next = ctx->generic_struct_specializations;
    ctx->generic_struct_specializations = spec;

    Node *decl = ast_clone(ctx->arena, template_decl);
    decl->is_exported = 0;
    decl->as.struct_decl.type_parameters.items = NULL;
    decl->as.struct_decl.type_parameters.count = 0;
    decl->as.struct_decl.type_parameters.capacity = 0;
    decl->as.struct_decl.name = make_generic_specialization_name(
        ctx,
        template_decl,
        type_arguments,
        type_argument_count
    );
    spec->declaration = decl;

    Scope *saved_scope = ctx->current_scope;
    SemanticModule *saved_module = ctx->current_module;
    SourceFileId saved_source_id = ctx->current_source_id;

    semantic_select_source_module(ctx, template_decl->span.file_id);
    scope_push(ctx);
    define_generic_type_aliases(ctx, template_decl, type_arguments);

    int errors_before = ctx->error_count;
    if (declare_struct_shell(ctx, decl)) {
        spec->type = decl->as.struct_decl.resolved_type;
        assert(spec->type);
        spec->type->struct_generic_template_id = template_id;
        spec->type->struct_type_argument_count = type_argument_count;
        spec->type->struct_type_arguments = arena_alloc(
            ctx->arena,
            sizeof(Type *) * (size_t)type_argument_count
        );
        memcpy(
            spec->type->struct_type_arguments,
            type_arguments,
            sizeof(Type *) * (size_t)type_argument_count
        );

        SemDeclInfo *info = sem_find_decl_info(ctx, decl);
        assert(info);
        info->is_generic_specialization = 1;

        spec->active_parent = ctx->active_generic_struct_specialization;
        ctx->active_generic_struct_specialization = spec;
        fill_struct_fields(ctx, decl);
        register_struct_method_signatures(ctx, decl);
        register_struct_operator_bindings(ctx, decl);
        /*
         * Concrete generic-struct method signatures are resolved here, but
         * bodies are checked lazily on first call. This mirrors generic
         * function instantiation: unused methods do not impose operations that
         * may be invalid for an otherwise valid struct specialization.
         */
        ctx->active_generic_struct_specialization = spec->active_parent;
    }

    spec->state = ctx->error_count == errors_before && spec->type
        ? GENERIC_SPECIALIZATION_VALID
        : GENERIC_SPECIALIZATION_INVALID;

    scope_pop(ctx);
    ctx->current_scope = saved_scope;
    ctx->current_module = saved_module;
    ctx->current_source_id = saved_source_id;

    return spec;
}

static int find_concrete_coglet_struct_index(
    Type **types,
    int count,
    const Type *type
) {
    for (int i = 0; i < count; i++) {
        if (types[i] == type)
            return i;
    }
    return -1;
}

static void validate_concrete_coglet_struct_layout_dfs(
    SemanticContext *ctx,
    Node **nodes,
    Type **types,
    int count,
    unsigned char *states,
    int index
) {
    if (states[index] == 2)
        return;
    if (states[index] == 1)
        return;

    states[index] = 1;
    Type *type = types[index];
    Node *decl = nodes[index];
    if (type && type->fields) {
        for (int i = 0; i < type->field_count; i++) {
            Type *field_type = type->fields[i].type;
            while (field_type && field_type->kind == TYPE_ARRAY)
                field_type = field_type->element;

            /* Pointers and slices are views/references and do not contribute inline layout. */
            if (!field_type || field_type->kind != TYPE_STRUCT ||
                field_type->struct_is_repr_c) {
                continue;
            }

            int dependency = find_concrete_coglet_struct_index(
                types, count, field_type);
            if (dependency < 0)
                continue;

            Node *field = decl->as.struct_decl.fields.items[i];
            if (states[dependency] == 1) {
                semantic_error_fmt(
                    ctx,
                    field,
                    "by-value field '%.*s' creates a recursive struct layout in '%.*s'",
                    (int)field->as.struct_field_decl.name.length,
                    field->as.struct_field_decl.name.data,
                    (int)type->struct_name.length,
                    type->struct_name.data
                );
                continue;
            }

            validate_concrete_coglet_struct_layout_dfs(
                ctx, nodes, types, count, states, dependency);
        }
    }
    states[index] = 2;
}

static void validate_concrete_coglet_struct_layouts(SemanticContext *ctx)
{
    int count = 0;
    for (SemDeclId id = 0; id < ctx->next_declaration_id; id++) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(ctx, id);
        if (!info || info->is_generic_template || !info->node || !info->type)
            continue;
        if (info->node->type == NODE_STRUCT_DECL &&
            info->type->kind == TYPE_STRUCT &&
            !info->type->struct_is_repr_c) {
            count++;
        }
    }
    if (count == 0)
        return;

    Node **nodes = arena_alloc(ctx->arena, sizeof(Node *) * (size_t)count);
    Type **types = arena_alloc(ctx->arena, sizeof(Type *) * (size_t)count);
    unsigned char *states = arena_alloc(ctx->arena, (size_t)count);
    memset(states, 0, (size_t)count);

    int at = 0;
    for (SemDeclId id = 0; id < ctx->next_declaration_id; id++) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(ctx, id);
        if (!info || info->is_generic_template || !info->node || !info->type)
            continue;
        if (info->node->type == NODE_STRUCT_DECL &&
            info->type->kind == TYPE_STRUCT &&
            !info->type->struct_is_repr_c) {
            nodes[at] = info->node;
            types[at] = info->type;
            at++;
        }
    }

    for (int i = 0; i < count; i++) {
        if (states[i] == 0)
            validate_concrete_coglet_struct_layout_dfs(
                ctx, nodes, types, count, states, i);
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
    type->enum_is_repr_c = node->as.enum_decl.is_repr_c;

    scope_define_declared(
        ctx,
        node,
        node->as.enum_decl.name,
        SYMBOL_TYPE,
        type
    );

    node->as.enum_decl.resolved_type = type;

    SemDeclInfo *decl_info = sem_find_decl_info(ctx, node);
    assert(decl_info);
    assert(decl_info->abi_kind == SEM_DECL_ABI_NONE);

    decl_info->abi_kind = SEM_DECL_ABI_ENUM;
    decl_info->abi.enumeration.representation =
        type->enum_is_repr_c ? SEM_ABI_REPR_C : SEM_ABI_REPR_COGLET;

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

static int repr_c_enum_backing_type_syntax_supported(const Type *type)
{
    if (!type || type->kind != TYPE_NAMED ||
        type->named_module.length != 0)
        return 0;

    StringView name = type->named_name;
    static const char *const supported[] = {
        "c_char", "c_schar", "c_uchar",
        "c_short", "c_ushort",
        "c_int", "c_uint",
        "c_long", "c_ulong",
        "c_longlong", "c_ulonglong",
        "c_size",
    };

    for (size_t i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        size_t length = strlen(supported[i]);
        if (name.length == length && memcmp(name.data, supported[i], length) == 0)
            return 1;
    }

    return 0;
}

static void fill_enum_members(SemanticContext *ctx, Node *node) {

    Type *type = node->as.enum_decl.resolved_type;

    if (!type) return;

    if (node->as.enum_decl.is_repr_c) {
        if (!node->as.enum_decl.backing_type) {
            semantic_error(ctx, node, "#repr(c) enums require an explicit native C integer backing type");
            return;
        }

        if (!repr_c_enum_backing_type_syntax_supported(node->as.enum_decl.backing_type)) {
            semantic_error(ctx, node, "#repr(c) enum backing type must be a native C integer alias");
            return;
        }
    }

    Type *backing_type = NULL;

    if (node->as.enum_decl.backing_type) {

        backing_type = resolve_type(ctx, node->as.enum_decl.backing_type, node);

        if (!backing_type)
            return;

    } else {
        backing_type = ctx->type_s32;  // Default enum backing type.
    }

    if (!is_integer_kind(backing_type->kind)) {
        semantic_error(ctx, node,
            "enum backing type must be an integer type");

        return;
    }

    type->enum_backing_type = backing_type;
    assert_canonical_builtin_type(ctx, backing_type);

    if (type->enum_is_repr_c) {
        SemDeclInfo *decl_info = sem_find_decl_info(ctx, node);
        assert(decl_info);
        assert(decl_info->abi_kind == SEM_DECL_ABI_ENUM);

        decl_info->abi.enumeration.backing_abi_type = make_sem_abi_type(
            ctx,
            node->as.enum_decl.backing_type,
            backing_type
        );
    }

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

        if (value_is_valid && member_node->as.enum_member.value) {
            Node *value_node = member_node->as.enum_member.value;
            SemExprInfo *value_info = sem_find_expr_info(ctx, value_node);

            if (value_info && value_info->type) {
                sem_record_context_conversion_if_needed(
                    ctx,
                    value_node,
                    backing_type,
                    value_info->type
                );
            }
        }

        type->enum_members[i].name = member_name;
        type->enum_members[i].value = value;
        member_node->as.enum_member.resolved_value = value;

        if (!duplicate && value_is_valid) {
            SemDeclInfo *member_info =
                sem_record_decl_info(ctx, member_node, type, NULL);

            member_info->has_constant_value = 1;
            member_info->constant_value = (ConstValue){
                .kind = CONST_VALUE_INT,
                .type = type,
                .as.integer = value,
            };

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

static Type *check_checked_cast_expression(SemanticContext *ctx, Node *node) {

    assert(node);
    assert(node->type == NODE_CAST);
    assert(node->as.cast_expr.kind == CAST_CHECKED);

    Type *target_type = resolve_type(
        ctx,
        node->as.cast_expr.target_type,
        node);

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

    Type *source_type =
        check_value_expression(ctx, source_expression);

    if (!source_type) return NULL;

    if (!is_allowed_explicit_cast(target_type, source_type)) {
        semantic_error(ctx, node,
            "invalid explicit cast");

        return NULL;
    }

    int source_is_constant =
        expression_is_compile_time_constant(ctx, source_expression);

    /*
     * Closed enums may only be constructed from declared member values.
     *
     * For a compile-time integer, eval_const_checked_cast() can prove that:
     *
     *   1. the integer fits the enum backing type;
     *   2. the integer equals a declared enum member value.
     *
     * For a runtime integer, the compiler cannot prove membership without
     * emitting a runtime check. Runtime checked enum conversion is not yet
     * implemented, so reject it for now.
     */
    if (is_integer_to_enum_cast(target_type, source_type)) {

        if (!source_is_constant) {
            semantic_error(ctx, node,
                "runtime integer-to-enum cast is not supported");

            return NULL;
        }

        ConstValue ignored;

        if (!eval_const_checked_cast(ctx, node, &ignored)) {
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
     *   cast(s8, -129)
     *   cast(f32, very_large_value)
     *   cast(u16, SomeEnum.Member)
     *
     * Constant declarations also reach eval_const_checked_cast() through
     * eval_const_expr(), but ordinary expression statements and nested
     * expression contexts need the same validation.
     */
    if (source_is_constant) {
        ConstValue ignored;

        if (!eval_const_checked_cast(ctx, node, &ignored)) {
            return NULL;
        }
    }

    return target_type;
}

static int switch_case_values_are_exhaustive(
    Type *switch_type,
    const ConstValue *case_values,
    int case_value_count,
    int has_default) {

    /*
     * A default case covers every runtime value regardless of the
     * switch expression's type.
     *
     * Handling this first also prevents secondary control-flow
     * diagnostics when the switch expression type itself is invalid.
     */
    if (has_default)
        return 1;

    if (!switch_type ||
        !case_values ||
        case_value_count <= 0) {
        return 0;
    }

    if (switch_type->kind == TYPE_BOOL) {
        int has_true  = 0;
        int has_false = 0;

        for (int i = 0; i < case_value_count; i++) {
            const ConstValue *value =
                &case_values[i];

            assert(value->type == switch_type);
            assert(value->kind == CONST_VALUE_BOOL);

            if (value->as.boolean)
                has_true = 1;
            else
                has_false = 1;
        }

        return has_true && has_false;
    }

    if (switch_type->kind != TYPE_ENUM)
        return 0;

    /*
     * Keep the existing policy that an empty enum is not considered
     * exhaustive through explicit cases alone.
     */
    if (switch_type->enum_member_count <= 0)
        return 0;

    /*
     * Exhaustiveness is value-based rather than member-name-based.
     *
     * When enum members are aliases:
     *
     *     Red     = 0,
     *     Crimson = 0,
     *
     * one runtime case for value zero covers both member names.
     */
    for (int member_index = 0; member_index < switch_type->enum_member_count; member_index++) {
        const EnumMember *member =
            &switch_type->enum_members[member_index];

        int found = 0;

        for (int case_index = 0; case_index < case_value_count; case_index++) {

            const ConstValue *case_value =
                &case_values[case_index];

            assert(case_value->type == switch_type);
            assert(case_value->kind == CONST_VALUE_INT);

            if (integer_values_equal(case_value->as.integer, member->value)) {
                found = 1;
                break;
            }
        }

        if (!found)
            return 0;
    }

    return 1;
}

// ============================================================
// node dispatcher
// ============================================================
static void check_node(SemanticContext *ctx,Node *node) {

    if(!node) return;

    switch(node->type)
    {
        case NODE_BLOCK:           check_block(ctx,node);            break;
        case NODE_PROGRAM:         check_program(ctx,node);          break;
        case NODE_VAR_DECL:        check_var_decl(ctx,node);         break;
        case NODE_VAR_DECL_GROUP:
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++)
                check_var_decl(ctx, node->as.var_decl_group.declarations.items[i]);
            break;
        case NODE_FUNC_PARAM_DECL: check_param_decl(ctx,node);       break;
        case NODE_FUNC_DECL:       check_function(ctx,node);         break;
        case NODE_IF:              check_if_statement(ctx,node);     break;
        case NODE_FOR:             check_for_statement(ctx, node);   break;
        case NODE_WHILE:           check_while_statement(ctx, node); break;
        case NODE_CONST_DECL:      check_const_decl(ctx,node);       break;
        case NODE_EXPR_STMT:       check_statement_expression(ctx, node->as.expr_stmt.expr); break;

        case NODE_STRUCT_DECL: {
            if (node->as.struct_decl.type_parameters.count > 0) {
                semantic_error(ctx, node,
                    "generic struct declarations must be top level in the initial implementation");
                break;
            }

            if (node->as.struct_decl.is_union && !node->as.struct_decl.is_repr_c) {
                semantic_error(ctx, node, "union declarations currently require #repr(c)");
                break;
            }

            if (node->as.struct_decl.is_repr_c && ctx->function_depth > 0) {
                semantic_error(
                    ctx,
                    node,
                    node->as.struct_decl.is_union
                        ? "#repr(c) union declarations must be at top level"
                        : "#repr(c) struct declarations must be at top level"
                );
                break;
            }

            declare_struct_shell(ctx, node);
            fill_struct_fields(ctx, node);
            break;
        }

        case NODE_ENUM_DECL: {
            if (node->as.enum_decl.is_repr_c && ctx->function_depth > 0) {
                semantic_error(ctx, node, "#repr(c) enum declarations must be at top level");
                break;
            }

            declare_enum_shell(ctx, node);
            fill_enum_members(ctx, node);
            break;
        }

        case NODE_BREAK:
            if (ctx->loop_depth <= 0) {
                semantic_error(ctx, node,
                    "break statement not inside loop");

                break;
            }

            assert(ctx->current_loop);

            loop_record_break(ctx);
            break;

        case NODE_CONTINUE:
            if (ctx->loop_depth <= 0) {
                semantic_error(ctx, node,
                    "continue statement not inside loop");

                break;
            }

            assert(ctx->current_loop);

            loop_record_continue(ctx);
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

        case NODE_RETURN: {
            if (ctx->function_depth == 0) {
                semantic_error(ctx, node,
                    "return outside function");
                break;
            }

            /*
             * A return statement prevents this control-flow path from
             * reaching later statements, even when the return itself has
             * a type or value error.
             */
            ctx->flow.reachable = 0;

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
void semantic_check(
    Node *program,
    SemanticContext *ctx,
    const TargetInfo *target,
    SourceManager *sources
) {

    assert(target);
    assert(sources);
    ctx->target = *target;
    ctx->sources = sources;
    diagnostic_list_init(&ctx->diagnostics, ctx->arena);

    ctx->had_error          = 0;
    ctx->loop_depth         = 0;
    ctx->function_depth     = 0;
    ctx->error_count        = 0;
    ctx->next_flow_owner_id = 0;
    ctx->next_variable_id   = 0;
    ctx->next_declaration_id = 0;
    ctx->current_loop       = NULL;

    /*
     * Top-level flow has no local-variable owner. Each function
     * receives a unique owner when its body is checked.
     */
    flow_init(&ctx->flow, INVALID_FLOW_OWNER_ID);

    ctx->current_return_type = NULL;

    ctx->type_s8  = new_type(ctx, TYPE_S8);
    ctx->type_s16 = new_type(ctx, TYPE_S16);
    ctx->type_s32 = new_type(ctx, TYPE_S32);
    ctx->type_s64 = new_type(ctx, TYPE_S64);

    ctx->type_u8  = new_type(ctx, TYPE_U8);
    ctx->type_u16 = new_type(ctx, TYPE_U16);
    ctx->type_u32 = new_type(ctx, TYPE_U32);
    ctx->type_u64 = new_type(ctx, TYPE_U64);

    ctx->type_f32 = new_type(ctx, TYPE_F32);
    ctx->type_f64 = new_type(ctx, TYPE_F64);

    ctx->type_bool = new_type(ctx, TYPE_BOOL);
    ctx->type_void = new_type(ctx, TYPE_VOID);
    ctx->type_null = new_type(ctx, TYPE_NULL);

    /*
     * Builtins live in a parent scope shared by every source module. Module
     * namespaces themselves are siblings, so declarations never leak merely
     * because their physical files participate in the same compilation unit.
     */
    ctx->builtin_scope = scope_new(ctx, NULL);
    ctx->current_scope = ctx->builtin_scope;
    ctx->modules = NULL;
    ctx->root_module = NULL;
    ctx->current_module = NULL;
    ctx->source_modules = NULL;
    ctx->source_module_count = 0;
    ctx->current_source_id = SOURCE_FILE_ID_INVALID;
    ctx->decl_infos = NULL;
    ctx->expr_infos = NULL;

    register_builtin_symbols(ctx);

    ctx->root_module = semantic_create_module(ctx, string_view_empty(), 1);
    ctx->current_module = ctx->root_module;
    ctx->current_scope = ctx->root_module->scope;

    if (sources->count > 0) {
        ctx->source_modules = arena_alloc(
            ctx->arena,
            sources->count * sizeof(*ctx->source_modules)
        );
        memset(
            ctx->source_modules,
            0,
            sources->count * sizeof(*ctx->source_modules)
        );

        size_t source_index = 0;
        for (const SourceFile *source = sources->first;
             source;
             source = source->next) {
            assert(source_index < sources->count);
            ctx->source_modules[source_index].source_id = source->id;
            ctx->source_modules[source_index].module = ctx->root_module;
            source_index++;
        }
        ctx->source_module_count = source_index;
    }

    check_node(ctx, program);
}

SemDeclInfo *semantic_get_decl_info(SemanticContext *ctx, Node *node) {
    return sem_find_decl_info(ctx, node);
}

SemDeclInfo *semantic_get_decl_info_by_id(SemanticContext *ctx, SemDeclId id) {
    return sem_find_decl_info_by_id(ctx, id);
}

SemExprInfo *semantic_get_expr_info(SemanticContext *ctx, Node *node) {
    return sem_find_expr_info(ctx, node);
}

Type *semantic_get_effective_expr_type(SemanticContext *ctx, Node *node) {
    SemExprInfo *info = sem_find_expr_info(ctx, node);

    if (!info)
        return NULL;

    return info->contextual_type
        ? info->contextual_type
        : info->type;
}

int semantic_get_constant_value(
    SemanticContext *ctx,
    Node *node,
    ConstValue *out
) {
    if (!ctx || !node || !out)
        return 0;

    SemExprInfo *expr_info = sem_find_expr_info(ctx, node);

    if (expr_info && expr_info->has_constant_value) {
        ConstValue value = expr_info->constant_value;

        if (expr_info->contextual_type) {
            ConstValue converted;

            if (!try_coerce_constant_to_type(
                    &value,
                    expr_info->contextual_type,
                    &converted)) {
                /*
                 * Semantic analysis records contextual conversions only after
                 * validating them. Failure here therefore indicates an internal
                 * inconsistency, not a user-program diagnostic.
                 */
                return 0;
            }

            value = converted;
        } else if (expr_info->type) {
            /*
             * Untyped constant evaluation may allocate an equivalent temporary
             * semantic Type. Normalize the exported value to the exact checked
             * expression type so downstream phases can use pointer identity.
             */
            value.type = expr_info->type;
        }

        *out = value;
        return 1;
    }

    SemDeclInfo *decl_info = sem_find_decl_info(ctx, node);

    if (decl_info && decl_info->has_constant_value) {
        *out = decl_info->constant_value;
        return 1;
    }

    return 0;
}
