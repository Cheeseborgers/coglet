#include "ir/cog_ir_lower.h"
#include "string_decode.h"

#include <stdlib.h>
#include <string.h>

static void lower_error(CogIrLowerContext *ctx, SourceSpan span, const char *message)
{
    if (!ctx)
        return;
    ctx->failed = 1;
    if (ctx->diagnostics)
        diagnostic_add(ctx->diagnostics, DIAGNOSTIC_ERROR, DIAGNOSTIC_PHASE_IR, span, message);
}

static int module_is_empty(const CogIrModule *module)
{
    return module && module->arena && !module->is_frozen &&
           module->sources.count == 0 && module->type_count == 0 &&
           module->abi_type_count == 0 && module->constant_count == 0 &&
           module->global_count == 0 && module->function_count == 0 &&
           module->init_function == COG_IR_FUNCTION_INVALID;
}

static void *grow_heap_array(void *old_data, size_t element_size, size_t *capacity)
{
    size_t new_capacity = *capacity ? *capacity * 2u : 32u;
    void *new_data = realloc(old_data, new_capacity * element_size);
    if (!new_data)
        return NULL;
    *capacity = new_capacity;
    return new_data;
}

static int remember_type(CogIrLowerContext *ctx, const Type *semantic_type, CogIrTypeId ir_type)
{
    for (size_t i = 0; i < ctx->type_map_count; ++i) {
        if (ctx->type_map[i].semantic_type == semantic_type) {
            if (ctx->type_map[i].ir_type != ir_type) {
                lower_error(ctx, source_span_invalid(), "semantic type mapped to two different CogIR types");
                return 0;
            }
            return 1;
        }
    }
    if (ctx->type_map_count == ctx->type_map_capacity) {
        void *grown = grow_heap_array(ctx->type_map, sizeof(*ctx->type_map), &ctx->type_map_capacity);
        if (!grown) {
            lower_error(ctx, source_span_invalid(), "out of memory while growing CogIR semantic type map");
            return 0;
        }
        ctx->type_map = grown;
    }
    ctx->type_map[ctx->type_map_count++] = (CogIrLowerTypeMapEntry){ semantic_type, ir_type };
    return 1;
}

static CogIrTypeId find_type(const CogIrLowerContext *ctx, const Type *semantic_type)
{
    for (size_t i = 0; i < ctx->type_map_count; ++i)
        if (ctx->type_map[i].semantic_type == semantic_type)
            return ctx->type_map[i].ir_type;
    return COG_IR_TYPE_INVALID;
}

static int remember_abi_type(CogIrLowerContext *ctx, const SemAbiType *semantic_type, CogIrAbiTypeId ir_type)
{
    for (size_t i = 0; i < ctx->abi_type_map_count; ++i) {
        if (ctx->abi_type_map[i].semantic_type == semantic_type) {
            if (ctx->abi_type_map[i].ir_type != ir_type) {
                lower_error(ctx, source_span_invalid(), "semantic ABI type mapped to two different CogIR ABI types");
                return 0;
            }
            return 1;
        }
    }
    if (ctx->abi_type_map_count == ctx->abi_type_map_capacity) {
        void *grown = grow_heap_array(ctx->abi_type_map, sizeof(*ctx->abi_type_map), &ctx->abi_type_map_capacity);
        if (!grown) {
            lower_error(ctx, source_span_invalid(), "out of memory while growing CogIR ABI type map");
            return 0;
        }
        ctx->abi_type_map = grown;
    }
    ctx->abi_type_map[ctx->abi_type_map_count++] = (CogIrLowerAbiTypeMapEntry){ semantic_type, ir_type };
    return 1;
}

static CogIrAbiTypeId find_abi_type(const CogIrLowerContext *ctx, const SemAbiType *semantic_type)
{
    for (size_t i = 0; i < ctx->abi_type_map_count; ++i)
        if (ctx->abi_type_map[i].semantic_type == semantic_type)
            return ctx->abi_type_map[i].ir_type;
    return COG_IR_ABI_TYPE_INVALID;
}

static CogIrAbiRepresentation lower_function_abi(FunctionAbi abi)
{
    return abi == FUNCTION_ABI_C ? COG_IR_ABI_C : COG_IR_ABI_COGLET;
}

static CogIrCallingConvention lower_calling_convention(CCallingConvention convention)
{
    switch (convention) {
        case C_CALL_DEFAULT: return COG_IR_CALL_DEFAULT;
        case C_CALL_CDECL: return COG_IR_CALL_CDECL;
        case C_CALL_STDCALL: return COG_IR_CALL_STDCALL;
        case C_CALL_SYSV64: return COG_IR_CALL_SYSV64;
        case C_CALL_WIN64: return COG_IR_CALL_WIN64;
    }
    return COG_IR_CALL_DEFAULT;
}

static CogIrCScalarKind lower_c_scalar_kind(SemCScalarKind kind)
{
    switch (kind) {
        case SEM_C_SCALAR_NONE: return COG_IR_C_SCALAR_NONE;
        case SEM_C_SCALAR_CHAR: return COG_IR_C_SCALAR_CHAR;
        case SEM_C_SCALAR_SCHAR: return COG_IR_C_SCALAR_SCHAR;
        case SEM_C_SCALAR_UCHAR: return COG_IR_C_SCALAR_UCHAR;
        case SEM_C_SCALAR_SHORT: return COG_IR_C_SCALAR_SHORT;
        case SEM_C_SCALAR_USHORT: return COG_IR_C_SCALAR_USHORT;
        case SEM_C_SCALAR_INT: return COG_IR_C_SCALAR_INT;
        case SEM_C_SCALAR_UINT: return COG_IR_C_SCALAR_UINT;
        case SEM_C_SCALAR_LONG: return COG_IR_C_SCALAR_LONG;
        case SEM_C_SCALAR_ULONG: return COG_IR_C_SCALAR_ULONG;
        case SEM_C_SCALAR_LONGLONG: return COG_IR_C_SCALAR_LONGLONG;
        case SEM_C_SCALAR_ULONGLONG: return COG_IR_C_SCALAR_ULONGLONG;
        case SEM_C_SCALAR_SIZE: return COG_IR_C_SCALAR_SIZE;
        case SEM_C_SCALAR_BOOL: return COG_IR_C_SCALAR_BOOL;
        case SEM_C_SCALAR_FLOAT: return COG_IR_C_SCALAR_FLOAT;
        case SEM_C_SCALAR_DOUBLE: return COG_IR_C_SCALAR_DOUBLE;
    }
    return COG_IR_C_SCALAR_NONE;
}

int cog_ir_lower_context_init(
    CogIrLowerContext *ctx,
    const CompileResult *frontend,
    CogIrModule *module,
    DiagnosticList *diagnostics
) {
    if (!ctx || !frontend || !module || frontend->status != COMPILE_STATUS_OK ||
        !module_is_empty(module) || !target_info_equal(&frontend->target, &module->target))
        return 0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->frontend = frontend;
    ctx->module = module;
    ctx->diagnostics = diagnostics;
    ctx->decl_binding_count = frontend->sem.next_declaration_id;

    if (ctx->decl_binding_count) {
        ctx->decl_bindings = calloc(ctx->decl_binding_count, sizeof(*ctx->decl_bindings));
        if (!ctx->decl_bindings) {
            lower_error(ctx, source_span_invalid(), "out of memory while allocating CogIR declaration map");
            return 0;
        }
        for (size_t i = 0; i < ctx->decl_binding_count; ++i) {
            ctx->decl_bindings[i].semantic_id = (SemDeclId)i;
            ctx->decl_bindings[i].kind = COG_IR_LOWER_DECL_NONE;
            ctx->decl_bindings[i].type = COG_IR_TYPE_INVALID;
            ctx->decl_bindings[i].as.nominal_type = COG_IR_TYPE_INVALID;
        }
    }
    return 1;
}

void cog_ir_lower_context_destroy(CogIrLowerContext *ctx)
{
    if (!ctx)
        return;
    free(ctx->type_map);
    free(ctx->abi_type_map);
    free(ctx->decl_bindings);
    memset(ctx, 0, sizeof(*ctx));
}

CogIrTypeId cog_ir_lower_type(CogIrLowerContext *ctx, const Type *type)
{
    if (!ctx || !type || ctx->failed)
        return COG_IR_TYPE_INVALID;
    CogIrTypeId existing = find_type(ctx, type);
    if (existing != COG_IR_TYPE_INVALID)
        return existing;

    CogIrTypeId result = COG_IR_TYPE_INVALID;
    switch (type->kind) {
        case TYPE_VOID: result = cog_ir_type_void(ctx->module); break;
        case TYPE_BOOL: result = cog_ir_type_bool(ctx->module); break;
        case TYPE_S8: result = cog_ir_type_integer(ctx->module, 8, 1); break;
        case TYPE_S16: result = cog_ir_type_integer(ctx->module, 16, 1); break;
        case TYPE_S32: result = cog_ir_type_integer(ctx->module, 32, 1); break;
        case TYPE_S64: result = cog_ir_type_integer(ctx->module, 64, 1); break;
        case TYPE_U8: result = cog_ir_type_integer(ctx->module, 8, 0); break;
        case TYPE_U16: result = cog_ir_type_integer(ctx->module, 16, 0); break;
        case TYPE_U32: result = cog_ir_type_integer(ctx->module, 32, 0); break;
        case TYPE_U64: result = cog_ir_type_integer(ctx->module, 64, 0); break;
        case TYPE_F32: result = cog_ir_type_float(ctx->module, 32); break;
        case TYPE_F64: result = cog_ir_type_float(ctx->module, 64); break;
        case TYPE_POINTER: {
            CogIrTypeId element = cog_ir_lower_type(ctx, type->element);
            if (element != COG_IR_TYPE_INVALID)
                result = cog_ir_type_pointer(ctx->module, element,
                    type->pointer_access == POINTER_ACCESS_READONLY, type->pointer_is_volatile);
            break;
        }
        case TYPE_OPAQUE_POINTER:
            result = cog_ir_type_opaque_pointer(ctx->module,
                type->pointer_access == POINTER_ACCESS_READONLY, type->pointer_is_volatile);
            break;
        case TYPE_ARRAY: {
            if (type->array_size < 0) {
                lower_error(ctx, source_span_invalid(), "cannot lower unsized semantic array type to CogIR");
                return COG_IR_TYPE_INVALID;
            }
            CogIrTypeId element = cog_ir_lower_type(ctx, type->element);
            if (element != COG_IR_TYPE_INVALID)
                result = cog_ir_type_array(ctx->module, element, (size_t)type->array_size);
            break;
        }
        case TYPE_SLICE: {
            CogIrTypeId element = cog_ir_lower_type(ctx, type->element);
            if (element == COG_IR_TYPE_INVALID)
                break;

            CogIrTypeId data_type = cog_ir_type_pointer(
                ctx->module,
                element,
                type->pointer_access == POINTER_ACCESS_READONLY,
                0
            );
            CogIrTypeId length_type = cog_ir_type_integer(ctx->module, 64, 0);
            if (data_type == COG_IR_TYPE_INVALID || length_type == COG_IR_TYPE_INVALID)
                break;

            char debug_name[256];
            semantic_format_type_name((Type *)type, debug_name, sizeof(debug_name));
            result = cog_ir_declare_nominal_type(
                ctx->module,
                COG_IR_TYPE_STRUCT,
                string_view_from_cstr(debug_name),
                source_span_invalid()
            );
            if (result == COG_IR_TYPE_INVALID)
                break;

            /* Remember before defining fields so recursive nominal elements remain safe. */
            if (!remember_type(ctx, type, result))
                return COG_IR_TYPE_INVALID;

            CogIrAggregateField fields[2];
            memset(fields, 0, sizeof(fields));
            fields[0].debug_name = string_view_from_cstr("data");
            fields[0].type = data_type;
            fields[0].abi_type = COG_IR_ABI_TYPE_INVALID;
            fields[0].span = source_span_invalid();
            fields[1].debug_name = string_view_from_cstr("len");
            fields[1].type = length_type;
            fields[1].abi_type = COG_IR_ABI_TYPE_INVALID;
            fields[1].span = source_span_invalid();

            if (!cog_ir_define_aggregate_type(
                    ctx->module,
                    result,
                    fields,
                    2,
                    0,
                    0,
                    0)) {
                lower_error(ctx, source_span_invalid(), "failed to define frozen CogIR slice representation");
                return COG_IR_TYPE_INVALID;
            }
            break;
        }
        case TYPE_FUNCTION: {
            CogIrTypeId return_type = cog_ir_lower_type(ctx, type->return_type);
            if (return_type == COG_IR_TYPE_INVALID)
                break;
            CogIrTypeId *parameters = NULL;
            if (type->parameter_count > 0) {
                parameters = malloc((size_t)type->parameter_count * sizeof(*parameters));
                if (!parameters) {
                    lower_error(ctx, source_span_invalid(), "out of memory while lowering function type");
                    return COG_IR_TYPE_INVALID;
                }
            }
            int ok = 1;
            for (int i = 0; i < type->parameter_count; ++i) {
                parameters[i] = cog_ir_lower_type(ctx, type->parameters[i]);
                if (parameters[i] == COG_IR_TYPE_INVALID) { ok = 0; break; }
            }
            if (ok)
                result = cog_ir_type_function(ctx->module, return_type, parameters,
                    (size_t)type->parameter_count, lower_function_abi(type->function_abi),
                    lower_calling_convention(type->function_call_conv), type->function_is_variadic);
            free(parameters);
            break;
        }
        case TYPE_STRUCT:
        case TYPE_ENUM:
            lower_error(ctx, source_span_invalid(), "nominal semantic type was not predeclared before structural lowering");
            return COG_IR_TYPE_INVALID;
        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_NAMED:
            lower_error(ctx, source_span_invalid(), "frontend-only semantic type cannot appear in CogIR");
            return COG_IR_TYPE_INVALID;
    }

    if (result == COG_IR_TYPE_INVALID) {
        lower_error(ctx, source_span_invalid(), "failed to construct CogIR runtime type");
        return COG_IR_TYPE_INVALID;
    }
    return remember_type(ctx, type, result) ? result : COG_IR_TYPE_INVALID;
}

CogIrAbiTypeId cog_ir_lower_abi_type(CogIrLowerContext *ctx, const SemAbiType *type)
{
    if (!ctx || !type || ctx->failed)
        return COG_IR_ABI_TYPE_INVALID;
    CogIrAbiTypeId existing = find_abi_type(ctx, type);
    if (existing != COG_IR_ABI_TYPE_INVALID)
        return existing;

    CogIrTypeId runtime = cog_ir_lower_type(ctx, type->semantic_type);
    if (runtime == COG_IR_TYPE_INVALID)
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiTypeId result = COG_IR_ABI_TYPE_INVALID;
    switch (type->kind) {
        case SEM_ABI_TYPE_SEMANTIC:
            result = cog_ir_abi_type_semantic(ctx->module, runtime); break;
        case SEM_ABI_TYPE_C_SCALAR: {
            CogIrCScalarKind scalar = lower_c_scalar_kind(type->c_scalar_kind);
            if (scalar == COG_IR_C_SCALAR_NONE) {
                lower_error(ctx, source_span_invalid(), "invalid normalized C scalar ABI type");
                return COG_IR_ABI_TYPE_INVALID;
            }
            result = cog_ir_abi_type_c_scalar(ctx->module, runtime, scalar); break;
        }
        case SEM_ABI_TYPE_POINTER: {
            CogIrAbiTypeId element = cog_ir_lower_abi_type(ctx, type->element);
            if (element != COG_IR_ABI_TYPE_INVALID)
                result = cog_ir_abi_type_pointer(ctx->module, runtime, element);
            break;
        }
        case SEM_ABI_TYPE_OPAQUE_POINTER:
            result = cog_ir_abi_type_opaque_pointer(ctx->module, runtime); break;
        case SEM_ABI_TYPE_ARRAY: {
            CogIrAbiTypeId element = cog_ir_lower_abi_type(ctx, type->element);
            if (element != COG_IR_ABI_TYPE_INVALID)
                result = cog_ir_abi_type_array(ctx->module, runtime, element);
            break;
        }
        case SEM_ABI_TYPE_FUNCTION: {
            CogIrAbiTypeId ret = cog_ir_lower_abi_type(ctx, type->return_type);
            if (ret == COG_IR_ABI_TYPE_INVALID) break;
            CogIrAbiTypeId *params = NULL;
            if (type->parameter_count > 0) {
                params = malloc((size_t)type->parameter_count * sizeof(*params));
                if (!params) {
                    lower_error(ctx, source_span_invalid(), "out of memory while lowering ABI function type");
                    return COG_IR_ABI_TYPE_INVALID;
                }
            }
            int ok = 1;
            for (int i = 0; i < type->parameter_count; ++i) {
                params[i] = cog_ir_lower_abi_type(ctx, type->parameters[i]);
                if (params[i] == COG_IR_ABI_TYPE_INVALID) { ok = 0; break; }
            }
            if (ok)
                result = cog_ir_abi_type_function(ctx->module, runtime, ret, params, (size_t)type->parameter_count);
            free(params);
            break;
        }
    }
    if (result == COG_IR_ABI_TYPE_INVALID) {
        lower_error(ctx, source_span_invalid(), "failed to construct CogIR ABI type");
        return COG_IR_ABI_TYPE_INVALID;
    }
    return remember_abi_type(ctx, type, result) ? result : COG_IR_ABI_TYPE_INVALID;
}

static unsigned integer_type_width(const Type *type)
{
    if (!type) return 0;
    if (type->kind == TYPE_ENUM) type = type->enum_backing_type;
    if (!type) return 0;
    switch (type->kind) {
        case TYPE_S8: case TYPE_U8: return 8;
        case TYPE_S16: case TYPE_U16: return 16;
        case TYPE_S32: case TYPE_U32: return 32;
        case TYPE_S64: case TYPE_U64: return 64;
        default: return 0;
    }
}

static uint64_t integer_value_bits(IntegerValue value, unsigned width)
{
    uint64_t mask = width == 64 ? UINT64_MAX : ((UINT64_C(1) << width) - UINT64_C(1));
    return value.is_negative ? ((~value.magnitude + UINT64_C(1)) & mask) : (value.magnitude & mask);
}

CogIrConstId cog_ir_lower_const_value(CogIrLowerContext *ctx, const ConstValue *value)
{
    if (!ctx || !value || !value->type || ctx->failed)
        return COG_IR_CONST_INVALID;
    CogIrTypeId type = cog_ir_lower_type(ctx, value->type);
    if (type == COG_IR_TYPE_INVALID) return COG_IR_CONST_INVALID;

    switch (value->kind) {
        case CONST_VALUE_BOOL:
            return cog_ir_const_bool(ctx->module, type, value->as.boolean);
        case CONST_VALUE_INT: {
            unsigned width = integer_type_width(value->type);
            if (!width) {
                lower_error(ctx, source_span_invalid(), "integer ConstValue has non-integer/non-enum type");
                return COG_IR_CONST_INVALID;
            }
            return cog_ir_const_integer(ctx->module, type, integer_value_bits(value->as.integer, width));
        }
        case CONST_VALUE_FLOAT:
            if (value->type->kind == TYPE_F32) {
                float rounded = (float)value->as.floating;
                uint32_t bits; memcpy(&bits, &rounded, sizeof(bits));
                return cog_ir_const_float32(ctx->module, type, bits);
            }
            if (value->type->kind == TYPE_F64) {
                uint64_t bits; double exact = value->as.floating; memcpy(&bits, &exact, sizeof(bits));
                return cog_ir_const_float64(ctx->module, type, bits);
            }
            lower_error(ctx, source_span_invalid(), "floating ConstValue has non-floating concrete type");
            return COG_IR_CONST_INVALID;
        case CONST_VALUE_NULL:
            return cog_ir_const_null(ctx->module, type);
    }
    lower_error(ctx, source_span_invalid(), "unknown semantic constant value kind");
    return COG_IR_CONST_INVALID;
}

const CogIrLowerDeclBinding *cog_ir_lower_get_decl_binding(const CogIrLowerContext *ctx, SemDeclId id)
{
    if (!ctx || id == INVALID_SEM_DECL_ID || id >= ctx->decl_binding_count) return NULL;
    return &ctx->decl_bindings[id];
}

static CogIrLowerDeclBinding *get_decl_binding_mut(CogIrLowerContext *ctx, SemDeclId id)
{
    if (!ctx || id == INVALID_SEM_DECL_ID || id >= ctx->decl_binding_count) return NULL;
    return &ctx->decl_bindings[id];
}

static int predeclare_nominal_types(CogIrLowerContext *ctx)
{
    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (SemDeclId id = 0; id < ctx->decl_binding_count; ++id) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(sem, id);
        if (!info || !info->node) {
            lower_error(ctx, source_span_invalid(), "semantic declaration table is incomplete during CogIR lowering");
            return 0;
        }
        if (info->is_generic_template)
            continue;
        if (!info->type) {
            lower_error(ctx, source_span_invalid(), "semantic declaration table is incomplete during CogIR lowering");
            return 0;
        }
        CogIrTypeKind kind; StringView name;
        if (info->node->type == NODE_STRUCT_DECL) {
            kind = info->abi_kind == SEM_DECL_ABI_AGGREGATE && info->abi.aggregate.aggregate_kind == SEM_AGGREGATE_UNION
                ? COG_IR_TYPE_UNION : COG_IR_TYPE_STRUCT;
            name = info->node->as.struct_decl.name;
        } else if (info->node->type == NODE_ENUM_DECL) {
            kind = COG_IR_TYPE_ENUM; name = info->node->as.enum_decl.name;
        } else continue;

        CogIrTypeId ir_type = cog_ir_declare_nominal_type(ctx->module, kind, name, info->node->span);
        if (ir_type == COG_IR_TYPE_INVALID || !remember_type(ctx, info->type, ir_type)) {
            lower_error(ctx, info->node->span, "failed to predeclare nominal CogIR type"); return 0;
        }
        CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, id);
        binding->kind = COG_IR_LOWER_DECL_TYPE; binding->type = ir_type; binding->as.nominal_type = ir_type;
    }
    return 1;
}

static int define_aggregate(CogIrLowerContext *ctx, SemDeclInfo *info)
{
    Node *node = info->node; CogIrTypeId owner = find_type(ctx, info->type);
    if (owner == COG_IR_TYPE_INVALID) return 0;
    if (info->abi_kind == SEM_DECL_ABI_AGGREGATE && info->abi.aggregate.is_incomplete) {
        if (!cog_ir_mark_incomplete_aggregate_type(ctx->module, owner)) {
            lower_error(ctx, node->span, "failed to mark incomplete CogIR aggregate"); return 0;
        }
        return 1;
    }

    size_t count = (size_t)node->as.struct_decl.fields.count;
    CogIrAggregateField *fields = count ? calloc(count, sizeof(*fields)) : NULL;
    if (count && !fields) { lower_error(ctx, node->span, "out of memory while lowering aggregate fields"); return 0; }
    int ok = 1; SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (size_t i = 0; i < count; ++i) {
        Node *field_node = node->as.struct_decl.fields.items[i];
        SemDeclInfo *field_info = semantic_get_decl_info(sem, field_node);
        if (!field_info || !field_info->type) { lower_error(ctx, field_node->span, "missing semantic field metadata during CogIR lowering"); ok = 0; break; }
        fields[i].debug_name = field_node->as.struct_field_decl.name; fields[i].span = field_node->span;
        fields[i].type = cog_ir_lower_type(ctx, field_info->type);
        fields[i].abi_type = field_info->abi_type ? cog_ir_lower_abi_type(ctx, field_info->abi_type) : COG_IR_ABI_TYPE_INVALID;
        if (fields[i].type == COG_IR_TYPE_INVALID || (field_info->abi_type && fields[i].abi_type == COG_IR_ABI_TYPE_INVALID)) { ok = 0; break; }
        CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, field_info->id);
        if (!binding) { lower_error(ctx, field_node->span, "field declaration ID is outside CogIR declaration map"); ok = 0; break; }
        binding->kind = COG_IR_LOWER_DECL_FIELD; binding->type = fields[i].type;
        binding->as.field.owner_type = owner; binding->as.field.index = (uint32_t)i;
    }
    if (ok) {
        int repr = info->abi_kind == SEM_DECL_ABI_AGGREGATE && info->abi.aggregate.representation == SEM_ABI_REPR_C;
        ok = cog_ir_define_aggregate_type(ctx->module, owner, fields, count, repr,
            repr ? info->abi.aggregate.is_packed : 0, repr ? info->abi.aggregate.explicit_alignment : 0);
        if (!ok) lower_error(ctx, node->span, "failed to define CogIR aggregate type");
    }
    free(fields); return ok;
}

static int define_enum(CogIrLowerContext *ctx, SemDeclInfo *info)
{
    Node *node = info->node; CogIrTypeId owner = find_type(ctx, info->type);
    CogIrTypeId backing = cog_ir_lower_type(ctx, info->type->enum_backing_type);
    if (owner == COG_IR_TYPE_INVALID || backing == COG_IR_TYPE_INVALID) return 0;
    int repr = info->abi_kind == SEM_DECL_ABI_ENUM && info->abi.enumeration.representation == SEM_ABI_REPR_C;
    CogIrAbiTypeId backing_abi = COG_IR_ABI_TYPE_INVALID;
    if (repr) { backing_abi = cog_ir_lower_abi_type(ctx, info->abi.enumeration.backing_abi_type); if (backing_abi == COG_IR_ABI_TYPE_INVALID) return 0; }

    size_t count = (size_t)node->as.enum_decl.members.count;
    CogIrEnumMember *members = count ? calloc(count, sizeof(*members)) : NULL;
    if (count && !members) { lower_error(ctx, node->span, "out of memory while lowering enum members"); return 0; }
    int ok = 1; SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem; unsigned width = integer_type_width(info->type);
    for (size_t i = 0; i < count; ++i) {
        Node *member_node = node->as.enum_decl.members.items[i]; SemDeclInfo *member_info = semantic_get_decl_info(sem, member_node);
        if (!member_info || !member_info->has_constant_value || !width) { lower_error(ctx, member_node->span, "missing enum-member constant metadata during CogIR lowering"); ok = 0; break; }
        members[i].debug_name = member_node->as.enum_member.name; members[i].span = member_node->span;
        members[i].bits = integer_value_bits(member_info->constant_value.as.integer, width);
        CogIrConstId constant = cog_ir_lower_const_value(ctx, &member_info->constant_value);
        if (constant == COG_IR_CONST_INVALID) { ok = 0; break; }
        CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, member_info->id);
        if (!binding) { lower_error(ctx, member_node->span, "enum-member declaration ID is outside CogIR declaration map"); ok = 0; break; }
        binding->kind = COG_IR_LOWER_DECL_ENUM_MEMBER; binding->type = owner;
        binding->as.enum_member.owner_type = owner; binding->as.enum_member.index = (uint32_t)i; binding->as.enum_member.constant = constant;
    }
    if (ok) {
        ok = cog_ir_define_enum_type(ctx->module, owner, backing, members, count, repr, backing_abi);
        if (!ok) lower_error(ctx, node->span, "failed to define CogIR enum type");
    }
    free(members); return ok;
}

static int define_nominal_types(CogIrLowerContext *ctx)
{
    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (SemDeclId id = 0; id < ctx->decl_binding_count; ++id) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(sem, id);
        if (!info || !info->node) return 0;
        if (info->node->type == NODE_STRUCT_DECL) { if (!define_aggregate(ctx, info)) return 0; }
        else if (info->node->type == NODE_ENUM_DECL) { if (!define_enum(ctx, info)) return 0; }
    }
    return 1;
}

static int lower_function_declaration(CogIrLowerContext *ctx, SemDeclInfo *info)
{
    Node *node = info->node; CogIrTypeId function_type = cog_ir_lower_type(ctx, info->type);
    if (function_type == COG_IR_TYPE_INVALID) return 0;
    CogIrFunctionAbi abi; CogIrFunctionAbi *abi_ptr = NULL; memset(&abi, 0, sizeof(abi));
    if (info->abi_kind == SEM_DECL_ABI_FUNCTION && info->abi.function.abi == FUNCTION_ABI_C) {
        abi.abi = COG_IR_ABI_C; abi.calling_convention = lower_calling_convention(info->abi.function.c_call_conv);
        abi.is_variadic = info->abi.function.is_variadic; abi.external_symbol = info->abi.function.external_symbol;
        abi.return_abi_type = cog_ir_lower_abi_type(ctx, info->abi.function.return_abi_type);
        abi.parameter_count = (size_t)node->as.func_decl.params.count;
        if (abi.return_abi_type == COG_IR_ABI_TYPE_INVALID) return 0;
        if (abi.parameter_count) {
            abi.parameter_abi_types = malloc(abi.parameter_count * sizeof(*abi.parameter_abi_types));
            if (!abi.parameter_abi_types) { lower_error(ctx, node->span, "out of memory while lowering function ABI metadata"); return 0; }
        }
        SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
        for (size_t i = 0; i < abi.parameter_count; ++i) {
            Node *param = node->as.func_decl.params.items[i]; SemDeclInfo *pi = semantic_get_decl_info(sem, param);
            if (!pi || !pi->abi_type) { lower_error(ctx, param->span, "missing normalized parameter ABI metadata during CogIR lowering"); free(abi.parameter_abi_types); return 0; }
            abi.parameter_abi_types[i] = cog_ir_lower_abi_type(ctx, pi->abi_type);
            if (abi.parameter_abi_types[i] == COG_IR_ABI_TYPE_INVALID) { free(abi.parameter_abi_types); return 0; }
        }
        abi_ptr = &abi;
    }
    CogIrLinkage linkage = info->abi_kind == SEM_DECL_ABI_FUNCTION && info->abi.function.linkage == SEM_FUNCTION_LINKAGE_EXTERNAL
        ? COG_IR_LINKAGE_EXTERNAL : COG_IR_LINKAGE_INTERNAL;
    CogIrFunctionId function = cog_ir_add_function(ctx->module, node->as.func_decl.name, node->span,
        function_type, COG_IR_FUNCTION_DECLARATION, linkage, 0, abi_ptr);
    free(abi.parameter_abi_types);
    if (function == COG_IR_FUNCTION_INVALID) { lower_error(ctx, node->span, "failed to predeclare CogIR function"); return 0; }
    CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, info->id);
    binding->kind = COG_IR_LOWER_DECL_FUNCTION; binding->type = function_type; binding->as.function = function;

    if (info->is_executable_entry &&
        !cog_ir_set_entry_function(ctx->module, function)) {
        lower_error(ctx, node->span, "failed to record CogIR source entry function");
        return 0;
    }

    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (int i = 0; i < node->as.func_decl.params.count; ++i) {
        Node *param = node->as.func_decl.params.items[i];
        SemDeclInfo *pi = semantic_get_decl_info(sem, param);
        if (!pi)
            continue;
        CogIrLowerDeclBinding *pb = get_decl_binding_mut(ctx, pi->id);
        if (!pb)
            continue;
        pb->kind = COG_IR_LOWER_DECL_PARAMETER_PENDING;
        pb->type = cog_ir_lower_type(ctx, pi->type);
    }
    return !ctx->failed;
}

static int lower_remaining_declarations(CogIrLowerContext *ctx)
{
    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (SemDeclId id = 0; id < ctx->decl_binding_count; ++id) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(sem, id);
        if (!info || !info->node) return 0;
        if (info->is_generic_template)
            continue;
        if (!info->type) return 0;
        CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, id); if (!binding) return 0;
        if (binding->kind != COG_IR_LOWER_DECL_NONE) continue;
        switch (info->node->type) {
            case NODE_FUNC_DECL:
                if (!lower_function_declaration(ctx, info))
                    return 0;
                break;
            case NODE_VAR_DECL: {
                CogIrTypeId type = cog_ir_lower_type(ctx, info->type); if (type == COG_IR_TYPE_INVALID) return 0; binding->type = type;
                if (info->symbol && info->symbol->variable_storage == VARIABLE_STORAGE_GLOBAL) {
                    CogIrConstId zero = cog_ir_const_zero(ctx->module, type);
                    CogIrAbiTypeId abi_type = info->abi_type
                        ? cog_ir_lower_abi_type(ctx, info->abi_type)
                        : COG_IR_ABI_TYPE_INVALID;
                    if (info->abi_type && abi_type == COG_IR_ABI_TYPE_INVALID)
                        return 0;
                    CogIrGlobalId global = cog_ir_add_global(ctx->module, info->node->as.var_decl.name, info->node->span,
                        type, abi_type, COG_IR_LINKAGE_INTERNAL, 0, 0, zero);
                    if (zero == COG_IR_CONST_INVALID || global == COG_IR_GLOBAL_INVALID) { lower_error(ctx, info->node->span, "failed to lower global storage metadata to CogIR"); return 0; }
                    binding->kind = COG_IR_LOWER_DECL_GLOBAL; binding->as.global = global;
                } else binding->kind = COG_IR_LOWER_DECL_LOCAL_PENDING;
                break;
            }
            case NODE_CONST_DECL: {
                ConstValue value;
                if (!semantic_get_constant_value(sem, info->node, &value)) {
                    lower_error(ctx, info->node->span, "missing checked constant declaration value during CogIR lowering");
                    return 0;
                }
                binding->kind = COG_IR_LOWER_DECL_CONSTANT;
                if (value.type->kind == TYPE_UNTYPED_INT || value.type->kind == TYPE_UNTYPED_FLOAT) {
                    /*
                     * Adaptable constants deliberately have no runtime type at
                     * declaration time. Each use is materialized from that use's
                     * SemExprInfo/ConstValue into a concrete CogIR constant.
                     */
                    binding->type = COG_IR_TYPE_INVALID;
                    binding->as.constant = COG_IR_CONST_INVALID;
                } else {
                    CogIrConstId constant = cog_ir_lower_const_value(ctx, &value);
                    if (constant == COG_IR_CONST_INVALID) return 0;
                    binding->type = cog_ir_lower_type(ctx, value.type);
                    binding->as.constant = constant;
                }
                break;
            }
            case NODE_FUNC_PARAM_DECL:
                binding->kind = COG_IR_LOWER_DECL_PARAMETER_PENDING; binding->type = cog_ir_lower_type(ctx, info->type); break;
            case NODE_STRUCT_FIELD_DECL:
            case NODE_ENUM_MEMBER:
                lower_error(ctx, info->node->span, "aggregate member was not bound during CogIR nominal type lowering"); return 0;
            case NODE_STRUCT_DECL:
            case NODE_ENUM_DECL:
                lower_error(ctx, info->node->span, "nominal declaration lost its CogIR binding"); return 0;
            default:
                lower_error(ctx, info->node->span, "unsupported semantic declaration kind in CogIR metadata lowering"); return 0;
        }
    }
    return !ctx->failed;
}

int cog_ir_lower_prepare_metadata(CogIrLowerContext *ctx)
{
    if (!ctx || !ctx->frontend || !ctx->module || ctx->failed || ctx->metadata_prepared ||
        ctx->module->is_frozen || ctx->module->sources.count != 0)
        return 0;
    if (!cog_ir_module_copy_sources(ctx->module, &ctx->frontend->sources)) {
        lower_error(ctx, source_span_invalid(), "failed to copy frontend source provenance into CogIR"); return 0;
    }
    if (!predeclare_nominal_types(ctx) || !define_nominal_types(ctx) || !lower_remaining_declarations(ctx)) return 0;
    for (size_t i = 0; i < ctx->decl_binding_count; ++i) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(
            (SemanticContext *)&ctx->frontend->sem,
            (SemDeclId)i
        );
        if (info && info->is_generic_template)
            continue;

        CogIrLowerDeclBinding *binding = &ctx->decl_bindings[i];
        int adaptable_constant = binding->kind == COG_IR_LOWER_DECL_CONSTANT &&
                                 binding->type == COG_IR_TYPE_INVALID &&
                                 binding->as.constant == COG_IR_CONST_INVALID;
        if (binding->kind == COG_IR_LOWER_DECL_NONE ||
            (binding->type == COG_IR_TYPE_INVALID && !adaptable_constant)) {
            lower_error(ctx, source_span_invalid(), "CogIR declaration map is incomplete after metadata lowering"); return 0;
        }
    }
    if (!ctx->failed)
        ctx->metadata_prepared = 1;
    return !ctx->failed;
}

typedef struct ExecLoopContext ExecLoopContext;

struct ExecLoopContext {
    CogIrBlockId break_target;
    CogIrBlockId continue_target;
    int has_break;
    ExecLoopContext *parent;
};

typedef struct ExecLowerState {
    CogIrLowerContext *lower;
    CogIrFunctionId function;
    CogIrBlockId block;
    ExecLoopContext *loop;
} ExecLowerState;

typedef struct LoweredPlace {
    CogIrValueId address;
    CogIrTypeId type;
    int is_volatile;
    int is_writable;
} LoweredPlace;

static int block_is_open(const ExecLowerState *state)
{
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrBlock *block = function ? cog_ir_get_block(function, state->block) : NULL;
    return block && block->terminator.kind == COG_IR_TERMINATOR_NONE;
}

static CogIrBlockId add_block(ExecLowerState *state, const char *name, SourceSpan span)
{
    CogIrBlockId block = cog_ir_add_block(
        state->lower->module,
        state->function,
        string_view_from_cstr(name),
        span
    );
    if (block == COG_IR_BLOCK_INVALID)
        lower_error(state->lower, span, "failed to create CogIR basic block");
    return block;
}

static int set_branch_args(
    ExecLowerState *state,
    CogIrBlockId target,
    const CogIrValueId *arguments,
    size_t argument_count,
    SourceSpan span
) {
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_BR;
    term.span = span;
    term.as.branch.edge.target = target;
    term.as.branch.edge.arguments = (CogIrValueId *)arguments;
    term.as.branch.edge.argument_count = argument_count;
    if (!cog_ir_set_terminator(state->lower->module, state->function, state->block, &term)) {
        lower_error(state->lower, span, "failed to emit CogIR branch terminator");
        return 0;
    }
    return 1;
}

static int set_branch(ExecLowerState *state, CogIrBlockId target, SourceSpan span)
{
    return set_branch_args(state, target, NULL, 0, span);
}

static int set_cond_branch(
    ExecLowerState *state,
    CogIrValueId condition,
    CogIrBlockId true_target,
    const CogIrValueId *true_arguments,
    size_t true_argument_count,
    CogIrBlockId false_target,
    const CogIrValueId *false_arguments,
    size_t false_argument_count,
    SourceSpan span
) {
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_COND_BR;
    term.span = span;
    term.as.cond_branch.condition = condition;
    term.as.cond_branch.if_true.target = true_target;
    term.as.cond_branch.if_true.arguments = (CogIrValueId *)true_arguments;
    term.as.cond_branch.if_true.argument_count = true_argument_count;
    term.as.cond_branch.if_false.target = false_target;
    term.as.cond_branch.if_false.arguments = (CogIrValueId *)false_arguments;
    term.as.cond_branch.if_false.argument_count = false_argument_count;
    if (!cog_ir_set_terminator(state->lower->module, state->function, state->block, &term)) {
        lower_error(state->lower, span, "failed to emit CogIR conditional branch terminator");
        return 0;
    }
    return 1;
}

static int set_unreachable(ExecLowerState *state, SourceSpan span)
{
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_UNREACHABLE;
    term.span = span;
    if (!cog_ir_set_terminator(state->lower->module, state->function, state->block, &term)) {
        lower_error(state->lower, span, "failed to emit CogIR unreachable terminator");
        return 0;
    }
    return 1;
}

static CogIrValueId emit_instruction_value(
    ExecLowerState *state,
    CogIrInstruction instruction
) {
    CogIrValueId value = COG_IR_VALUE_INVALID;
    if (!cog_ir_emit(state->lower->module, state->function, state->block, &instruction, &value)) {
        lower_error(state->lower, instruction.span, "failed to emit CogIR instruction");
        return COG_IR_VALUE_INVALID;
    }
    return value;
}

static int annotate_value_abi(
    ExecLowerState *state,
    CogIrValueId value,
    CogIrAbiTypeId abi_type,
    SourceSpan span
) {
    if (abi_type == COG_IR_ABI_TYPE_INVALID)
        return 1;
    if (!cog_ir_set_value_abi_type(
            state->lower->module, state->function, value, abi_type)) {
        lower_error(state->lower, span, "failed to attach CogIR value ABI metadata");
        return 0;
    }
    return 1;
}


static int annotate_address_abi(
    ExecLowerState *state,
    CogIrValueId address,
    CogIrAbiTypeId element_abi,
    SourceSpan span
) {
    if (element_abi == COG_IR_ABI_TYPE_INVALID)
        return 1;
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *value = function ? cog_ir_get_value(function, address) : NULL;
    if (!value) {
        lower_error(state->lower, span, "cannot annotate invalid CogIR address ABI metadata");
        return 0;
    }
    CogIrAbiTypeId pointer_abi = cog_ir_abi_type_pointer(
        state->lower->module, value->type, element_abi);
    if (pointer_abi == COG_IR_ABI_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to construct CogIR address ABI metadata");
        return 0;
    }
    return annotate_value_abi(state, address, pointer_abi, span);
}

static int annotate_slot_abi(
    ExecLowerState *state,
    CogIrSlotId slot,
    CogIrAbiTypeId abi_type,
    SourceSpan span
) {
    if (abi_type == COG_IR_ABI_TYPE_INVALID)
        return 1;
    if (!cog_ir_set_slot_abi_type(
            state->lower->module, state->function, slot, abi_type)) {
        lower_error(state->lower, span, "failed to attach CogIR slot ABI metadata");
        return 0;
    }
    return 1;
}

static CogIrAbiTypeId function_value_abi_type(
    ExecLowerState *state,
    CogIrFunctionId function_id,
    SourceSpan span
) {
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, function_id);
    if (!function || function->abi.abi != COG_IR_ABI_C)
        return COG_IR_ABI_TYPE_INVALID;
    CogIrAbiTypeId abi_type = cog_ir_abi_type_function(
        state->lower->module,
        function->type,
        function->abi.return_abi_type,
        function->abi.parameter_abi_types,
        function->abi.parameter_count
    );
    if (abi_type == COG_IR_ABI_TYPE_INVALID)
        lower_error(state->lower, span, "failed to construct CogIR C function-pointer ABI metadata");
    return abi_type;
}

static int emit_instruction_void(ExecLowerState *state, CogIrInstruction instruction)
{
    if (!cog_ir_emit(state->lower->module, state->function, state->block, &instruction, NULL)) {
        lower_error(state->lower, instruction.span, "failed to emit CogIR instruction");
        return 0;
    }
    return 1;
}

static CogIrValueId emit_constant_value(ExecLowerState *state, CogIrConstId constant, SourceSpan span)
{
    const CogIrConstant *value = cog_ir_get_constant(state->lower->module, constant);
    if (!value) {
        lower_error(state->lower, span, "invalid CogIR constant during executable lowering");
        return COG_IR_VALUE_INVALID;
    }
    CogIrInstruction instruction = {
        .op = COG_IR_OP_CONST,
        .result_type = value->type,
        .span = span,
        .as.constant = { .constant = constant },
    };
    return emit_instruction_value(state, instruction);
}

static CogIrValueId emit_local_address(
    ExecLowerState *state,
    CogIrSlotId slot,
    CogIrTypeId pointee,
    SourceSpan span
) {
    CogIrTypeId pointer = cog_ir_type_pointer(state->lower->module, pointee, 0, 0);
    if (pointer == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to construct local address type");
        return COG_IR_VALUE_INVALID;
    }
    CogIrInstruction instruction = {
        .op = COG_IR_OP_LOCAL_ADDR,
        .result_type = pointer,
        .span = span,
        .as.local_addr = { .slot = slot },
    };
    CogIrValueId value = emit_instruction_value(state, instruction);
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrSlot *ir_slot = function ? cog_ir_get_slot(function, slot) : NULL;
    if (value != COG_IR_VALUE_INVALID && ir_slot &&
        !annotate_address_abi(state, value, ir_slot->abi_type, span))
        return COG_IR_VALUE_INVALID;
    return value;
}

static CogIrValueId emit_global_address(
    ExecLowerState *state,
    CogIrGlobalId global,
    CogIrTypeId pointee,
    int readonly,
    SourceSpan span
) {
    CogIrTypeId pointer = cog_ir_type_pointer(state->lower->module, pointee, readonly, 0);
    if (pointer == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to construct global address type");
        return COG_IR_VALUE_INVALID;
    }
    CogIrInstruction instruction = {
        .op = COG_IR_OP_GLOBAL_ADDR,
        .result_type = pointer,
        .span = span,
        .as.global_addr = { .global = global },
    };
    CogIrValueId value = emit_instruction_value(state, instruction);
    const CogIrGlobal *ir_global = cog_ir_get_global(state->lower->module, global);
    if (value != COG_IR_VALUE_INVALID && ir_global &&
        !annotate_address_abi(state, value, ir_global->abi_type, span))
        return COG_IR_VALUE_INVALID;
    return value;
}

static CogIrValueId emit_load(
    ExecLowerState *state,
    CogIrValueId address,
    CogIrTypeId type,
    int is_volatile,
    SourceSpan span
) {
    CogIrInstruction instruction = {
        .op = COG_IR_OP_LOAD,
        .result_type = type,
        .span = span,
        .as.load = { .address = address, .is_volatile = is_volatile },
    };
    return emit_instruction_value(state, instruction);
}

static int emit_store(
    ExecLowerState *state,
    CogIrValueId address,
    CogIrValueId value,
    int is_volatile,
    SourceSpan span
) {
    CogIrInstruction instruction = {
        .op = COG_IR_OP_STORE,
        .result_type = COG_IR_TYPE_INVALID,
        .span = span,
        .as.store = { .address = address, .value = value, .is_volatile = is_volatile },
    };
    return emit_instruction_void(state, instruction);
}

static CogIrLowerDeclBinding *binding_for_expr_ident(CogIrLowerContext *ctx, Node *node)
{
    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    SemExprInfo *expr = semantic_get_expr_info(sem, node);
    if (!expr || !expr->symbol || expr->symbol->declaration_id == INVALID_SEM_DECL_ID)
        return NULL;
    return get_decl_binding_mut(ctx, expr->symbol->declaration_id);
}

static CogIrValueId emit_function_reference(
    ExecLowerState *state,
    const CogIrLowerDeclBinding *binding,
    SourceSpan span
) {
    if (!binding || binding->kind != COG_IR_LOWER_DECL_FUNCTION)
        return COG_IR_VALUE_INVALID;
    CogIrInstruction instruction = {
        .op = COG_IR_OP_FUNCTION_REF,
        .result_type = binding->type,
        .span = span,
        .as.function_ref = { .function = binding->as.function },
    };
    CogIrValueId value = emit_instruction_value(state, instruction);
    if (value == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrAbiTypeId abi_type = function_value_abi_type(
        state, binding->as.function, span);
    if (abi_type != COG_IR_ABI_TYPE_INVALID &&
        !annotate_value_abi(state, value, abi_type, span)) {
        return COG_IR_VALUE_INVALID;
    }
    return value;
}

static CogIrValueId lower_expression(ExecLowerState *state, Node *node);
static int expression_may_create_cfg(ExecLowerState *state, Node *node);
static CogIrSlotId spill_value(ExecLowerState *state, CogIrValueId value, SourceSpan span);
static CogIrValueId reload_spill(ExecLowerState *state, CogIrSlotId slot, SourceSpan span);

static CogIrValueId lower_identifier_place(ExecLowerState *state, Node *node, int *is_volatile)
{
    CogIrLowerDeclBinding *binding = binding_for_expr_ident(state->lower, node);
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemExprInfo *expr = semantic_get_expr_info(sem, node);
    if (is_volatile)
        *is_volatile = expr ? expr->value_is_volatile : 0;
    if (!binding) {
        lower_error(state->lower, node->span, "identifier has no declaration binding during CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    switch (binding->kind) {
        case COG_IR_LOWER_DECL_LOCAL:
            if (binding->as.local.function != state->function) {
                lower_error(state->lower, node->span, "local declaration belongs to a different function");
                return COG_IR_VALUE_INVALID;
            }
            return emit_local_address(state, binding->as.local.slot, binding->type, node->span);
        case COG_IR_LOWER_DECL_PARAMETER:
            if (binding->as.parameter.function != state->function) {
                lower_error(state->lower, node->span, "parameter declaration belongs to a different function");
                return COG_IR_VALUE_INVALID;
            }
            return emit_local_address(state, binding->as.parameter.slot, binding->type, node->span);
        case COG_IR_LOWER_DECL_GLOBAL: {
            const CogIrGlobal *global = cog_ir_get_global(state->lower->module, binding->as.global);
            if (!global) {
                lower_error(state->lower, node->span, "global declaration binding is invalid");
                return COG_IR_VALUE_INVALID;
            }
            return emit_global_address(state, binding->as.global, binding->type, global->is_readonly, node->span);
        }
        default:
            lower_error(state->lower, node->span, "expression is not an addressable declaration in this CogIR lowering slice");
            return COG_IR_VALUE_INVALID;
    }
}


static int string_view_equal(StringView a, StringView b)
{
    return a.length == b.length && (!a.length || memcmp(a.data, b.data, a.length) == 0);
}

static Type *effective_semantic_type(ExecLowerState *state, Node *node)
{
    return semantic_get_effective_expr_type(
        (SemanticContext *)&state->lower->frontend->sem,
        node
    );
}

static int semantic_struct_field_index(Type *type, StringView name)
{
    if (!type)
        return -1;

    if (type->kind == TYPE_SLICE) {
        if (name.length == sizeof("data") - 1 &&
            memcmp(name.data, "data", sizeof("data") - 1) == 0)
            return 0;
        if (name.length == sizeof("len") - 1 &&
            memcmp(name.data, "len", sizeof("len") - 1) == 0)
            return 1;
        return -1;
    }

    if (type->kind != TYPE_STRUCT)
        return -1;
    for (int i = 0; i < type->field_count; ++i)
        if (string_view_equal(type->fields[i].name, name))
            return i;
    return -1;
}

static int semantic_enum_member_index(Type *type, StringView name)
{
    if (!type || type->kind != TYPE_ENUM)
        return -1;
    for (int i = 0; i < type->enum_member_count; ++i)
        if (string_view_equal(type->enum_members[i].name, name))
            return i;
    return -1;
}

static CogIrValueId emit_field_address(
    ExecLowerState *state,
    CogIrValueId base,
    uint32_t field_index,
    CogIrTypeId field_type,
    int readonly,
    int is_volatile,
    SourceSpan span
) {
    CogIrTypeId pointer = cog_ir_type_pointer(
        state->lower->module,
        field_type,
        readonly,
        is_volatile
    );
    if (pointer == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to construct field address type");
        return COG_IR_VALUE_INVALID;
    }
    CogIrInstruction instruction = {
        .op = COG_IR_OP_FIELD_ADDR,
        .result_type = pointer,
        .span = span,
        .as.field_addr = { .base = base, .field_index = field_index },
    };
    CogIrValueId value = emit_instruction_value(state, instruction);
    if (value == COG_IR_VALUE_INVALID)
        return value;
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *base_value = function ? cog_ir_get_value(function, base) : NULL;
    const CogIrType *base_pointer = base_value
        ? cog_ir_get_type(state->lower->module, base_value->type) : NULL;
    const CogIrType *aggregate = base_pointer && base_pointer->kind == COG_IR_TYPE_POINTER
        ? cog_ir_get_type(state->lower->module, base_pointer->as.pointer.pointee) : NULL;
    if (aggregate && (aggregate->kind == COG_IR_TYPE_STRUCT || aggregate->kind == COG_IR_TYPE_UNION) &&
        field_index < aggregate->as.aggregate.field_count &&
        !annotate_address_abi(
            state, value, aggregate->as.aggregate.fields[field_index].abi_type, span))
        return COG_IR_VALUE_INVALID;
    return value;
}

static CogIrValueId emit_index_address(
    ExecLowerState *state,
    CogIrOp op,
    CogIrValueId base,
    CogIrValueId index,
    CogIrTypeId element_type,
    int readonly,
    int is_volatile,
    SourceSpan span
) {
    CogIrTypeId pointer = cog_ir_type_pointer(
        state->lower->module,
        element_type,
        readonly,
        is_volatile
    );
    if (pointer == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to construct indexed address type");
        return COG_IR_VALUE_INVALID;
    }
    CogIrInstruction instruction = {
        .op = op,
        .result_type = pointer,
        .span = span,
        .as.index_addr = { .base = base, .index = index },
    };
    CogIrValueId value = emit_instruction_value(state, instruction);
    if (value == COG_IR_VALUE_INVALID)
        return value;
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *base_value = function ? cog_ir_get_value(function, base) : NULL;
    const CogIrAbiType *base_abi = base_value && base_value->abi_type != COG_IR_ABI_TYPE_INVALID
        ? cog_ir_get_abi_type(state->lower->module, base_value->abi_type) : NULL;
    CogIrAbiTypeId element_abi = COG_IR_ABI_TYPE_INVALID;
    if (base_abi && base_abi->kind == COG_IR_ABI_TYPE_POINTER) {
        const CogIrAbiType *pointee = cog_ir_get_abi_type(state->lower->module, base_abi->element_type);
        if (op == COG_IR_OP_ARRAY_ELEM_ADDR && pointee && pointee->kind == COG_IR_ABI_TYPE_ARRAY)
            element_abi = pointee->element_type;
        else if (op == COG_IR_OP_PTR_INDEX_ADDR)
            element_abi = base_abi->element_type;
    }
    if (!annotate_address_abi(state, value, element_abi, span))
        return COG_IR_VALUE_INVALID;
    return value;
}

static CogIrValueId emit_aggregate_value(
    ExecLowerState *state,
    CogIrOp op,
    CogIrTypeId type,
    CogIrValueId *values,
    size_t value_count,
    SourceSpan span
) {
    CogIrInstruction instruction = {
        .op = op,
        .result_type = type,
        .span = span,
        .as.aggregate = { .values = values, .value_count = value_count },
    };
    return emit_instruction_value(state, instruction);
}

static CogIrValueId emit_conversion(
    ExecLowerState *state,
    CogIrOp op,
    CogIrValueId operand,
    CogIrTypeId target_type,
    SourceSpan span
) {
    CogIrInstruction instruction = {
        .op = op,
        .result_type = target_type,
        .span = span,
        .as.conversion = { .operand = operand, .target_type = target_type },
    };
    return emit_instruction_value(state, instruction);
}

static int lower_place(ExecLowerState *state, Node *node, LoweredPlace *out)
{
    if (!state || !node || !out)
        return 0;

    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemExprInfo *expr = semantic_get_expr_info(sem, node);
    /* A place always denotes its intrinsic storage type; contextual conversion
     * belongs to the value loaded from that storage, never to its address. */
    Type *sem_type = expr ? expr->type : NULL;
    if (!expr || !sem_type || expr->value_category != VALUE_CATEGORY_LVALUE) {
        lower_error(state->lower, node->span, "expression is not an addressable place during CogIR lowering");
        return 0;
    }

    CogIrTypeId value_type = cog_ir_lower_type(state->lower, sem_type);
    if (value_type == COG_IR_TYPE_INVALID)
        return 0;

    memset(out, 0, sizeof(*out));
    out->address = COG_IR_VALUE_INVALID;
    out->type = value_type;
    out->is_volatile = !!expr->value_is_volatile;
    out->is_writable = expr->value_access == VALUE_ACCESS_WRITABLE;

    switch (node->type) {
        case NODE_IDENT:
            out->address = lower_identifier_place(state, node, &out->is_volatile);
            return out->address != COG_IR_VALUE_INVALID;

        case NODE_UNARY:
            if (node->as.unary.op == TOK_STAR) {
                out->address = lower_expression(state, node->as.unary.operand);
                return out->address != COG_IR_VALUE_INVALID;
            }
            break;

        case NODE_FIELD: {
            /*
             * A module-qualified global is semantically a direct declaration
             * reference, not a runtime struct field. Namespace information is
             * already erased into the resolved SemDeclId, so lower it through
             * the same global binding used by an unqualified identifier.
             */
            if (expr->symbol &&
                expr->symbol->kind == SYMBOL_VARIABLE &&
                expr->symbol->variable_storage == VARIABLE_STORAGE_GLOBAL) {
                out->address = lower_identifier_place(
                    state, node, &out->is_volatile);
                return out->address != COG_IR_VALUE_INVALID;
            }

            LoweredPlace base;
            if (!lower_place(state, node->as.field.object, &base))
                return 0;
            Type *object_type = semantic_get_effective_expr_type(sem, node->as.field.object);
            int field_index = semantic_struct_field_index(object_type, node->as.field.name);
            if (field_index < 0) {
                lower_error(state->lower, node->span, "struct field has no semantic field index during CogIR lowering");
                return 0;
            }
            out->address = emit_field_address(
                state,
                base.address,
                (uint32_t)field_index,
                value_type,
                !out->is_writable,
                out->is_volatile,
                node->span
            );
            return out->address != COG_IR_VALUE_INVALID;
        }

        case NODE_INDEX: {
            Type *object_type = semantic_get_effective_expr_type(sem, node->as.index.object);
            if (!object_type) {
                lower_error(state->lower, node->span, "indexed object has no semantic type during CogIR lowering");
                return 0;
            }

            CogIrValueId base = COG_IR_VALUE_INVALID;
            CogIrOp op;
            if (object_type->kind == TYPE_ARRAY) {
                LoweredPlace array_place;
                if (!lower_place(state, node->as.index.object, &array_place))
                    return 0;
                base = array_place.address;
                op = COG_IR_OP_ARRAY_ELEM_ADDR;
            } else if (object_type->kind == TYPE_POINTER) {
                base = lower_expression(state, node->as.index.object);
                op = COG_IR_OP_PTR_INDEX_ADDR;
            } else if (object_type->kind == TYPE_SLICE) {
                CogIrValueId slice = lower_expression(state, node->as.index.object);
                CogIrTypeId element = cog_ir_lower_type(state->lower, object_type->element);
                CogIrTypeId data_type = cog_ir_type_pointer(
                    state->lower->module,
                    element,
                    object_type->pointer_access == POINTER_ACCESS_READONLY,
                    0
                );
                if (slice == COG_IR_VALUE_INVALID || element == COG_IR_TYPE_INVALID ||
                    data_type == COG_IR_TYPE_INVALID)
                    return 0;
                CogIrInstruction extract = {
                    .op = COG_IR_OP_EXTRACT_FIELD,
                    .result_type = data_type,
                    .span = node->as.index.object->span,
                    .as.extract = { .aggregate = slice, .index = 0 },
                };
                base = emit_instruction_value(state, extract);
                op = COG_IR_OP_PTR_INDEX_ADDR;
            } else {
                lower_error(state->lower, node->span, "indexed expression is neither an array, slice, nor typed pointer");
                return 0;
            }
            if (base == COG_IR_VALUE_INVALID)
                return 0;

            CogIrSlotId base_spill = COG_IR_SLOT_INVALID;
            if (expression_may_create_cfg(state, node->as.index.index)) {
                base_spill = spill_value(state, base, node->as.index.object->span);
                if (base_spill == COG_IR_SLOT_INVALID)
                    return 0;
            }
            CogIrValueId index = lower_expression(state, node->as.index.index);
            if (index == COG_IR_VALUE_INVALID)
                return 0;
            if (base_spill != COG_IR_SLOT_INVALID) {
                base = reload_spill(state, base_spill, node->as.index.object->span);
                if (base == COG_IR_VALUE_INVALID)
                    return 0;
            }

            out->address = emit_index_address(
                state,
                op,
                base,
                index,
                value_type,
                !out->is_writable,
                out->is_volatile,
                node->span
            );
            return out->address != COG_IR_VALUE_INVALID;
        }

        default:
            break;
    }

    lower_error(state->lower, node->span, "place expression is outside the current CogIR data/address lowering slice");
    return 0;
}

static CogIrOp lower_arithmetic_binary_op(CogIrLowerContext *ctx, Node *node, CogIrTypeId type)
{
    const CogIrType *ir_type = cog_ir_get_type(ctx->module, type);
    if (!ir_type)
        return (CogIrOp)-1;

    if (ir_type->kind == COG_IR_TYPE_INTEGER) {
        switch (node->as.binary.op) {
            case TOK_PLUS: return COG_IR_OP_IADD_CHECKED;
            case TOK_MINUS: return COG_IR_OP_ISUB_CHECKED;
            case TOK_STAR: return COG_IR_OP_IMUL_CHECKED;
            case TOK_SLASH: return COG_IR_OP_IDIV_CHECKED;
            case TOK_PERCENT: return COG_IR_OP_IREM_CHECKED;
            case TOK_AND: return COG_IR_OP_BIT_AND;
            case TOK_OR: return COG_IR_OP_BIT_OR;
            case TOK_XOR: return COG_IR_OP_BIT_XOR;
            case TOK_SHIFT_LEFT: return COG_IR_OP_SHL_CHECKED_COUNT;
            case TOK_SHIFT_RIGHT:
                return ir_type->as.integer.is_signed
                    ? COG_IR_OP_SHR_SIGNED_CHECKED_COUNT
                    : COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT;
            default: return (CogIrOp)-1;
        }
    }
    if (ir_type->kind == COG_IR_TYPE_FLOAT) {
        switch (node->as.binary.op) {
            case TOK_PLUS: return COG_IR_OP_FADD;
            case TOK_MINUS: return COG_IR_OP_FSUB;
            case TOK_STAR: return COG_IR_OP_FMUL;
            case TOK_SLASH: return COG_IR_OP_FDIV;
            default: return (CogIrOp)-1;
        }
    }
    return (CogIrOp)-1;
}

static CogIrValueId emit_pointer_qualify(
    ExecLowerState *state,
    CogIrValueId operand,
    CogIrTypeId target_type,
    SourceSpan span
) {
    CogIrInstruction instruction = {
        .op = COG_IR_OP_PTR_QUALIFY,
        .result_type = target_type,
        .span = span,
        .as.conversion = { .operand = operand, .target_type = target_type },
    };
    return emit_instruction_value(state, instruction);
}

static CogIrValueId emit_slice_value(
    ExecLowerState *state,
    Type *slice_type,
    CogIrValueId data,
    CogIrValueId length,
    SourceSpan span
) {
    CogIrTypeId type = cog_ir_lower_type(state->lower, slice_type);
    if (type == COG_IR_TYPE_INVALID || data == COG_IR_VALUE_INVALID ||
        length == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrValueId fields[2] = { data, length };
    return emit_aggregate_value(
        state,
        COG_IR_OP_MAKE_STRUCT,
        type,
        fields,
        2,
        span
    );
}

static CogIrValueId lower_array_to_slice_conversion(
    ExecLowerState *state,
    Node *node,
    Type *target_slice
) {
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemExprInfo *info = semantic_get_expr_info(sem, node);
    Type *source = info ? info->type : NULL;
    if (!source || source->kind != TYPE_ARRAY || !target_slice ||
        target_slice->kind != TYPE_SLICE || source->array_size < 0) {
        lower_error(state->lower, node->span,
            "invalid array-to-slice contextual conversion reached CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    CogIrTypeId element = cog_ir_lower_type(state->lower, source->element);
    CogIrTypeId data_type = cog_ir_type_pointer(
        state->lower->module,
        element,
        target_slice->pointer_access == POINTER_ACCESS_READONLY,
        0
    );
    CogIrTypeId length_type = cog_ir_type_integer(state->lower->module, 64, 0);
    if (element == COG_IR_TYPE_INVALID || data_type == COG_IR_TYPE_INVALID ||
        length_type == COG_IR_TYPE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrValueId data = COG_IR_VALUE_INVALID;
    if (source->array_size == 0) {
        CogIrConstId null_value = cog_ir_const_null(state->lower->module, data_type);
        data = null_value == COG_IR_CONST_INVALID
            ? COG_IR_VALUE_INVALID
            : emit_constant_value(state, null_value, node->span);
    } else {
        LoweredPlace place;
        if (!lower_place(state, node, &place))
            return COG_IR_VALUE_INVALID;

        CogIrConstId zero_constant = cog_ir_const_integer(
            state->lower->module, length_type, 0);
        CogIrValueId zero = emit_constant_value(state, zero_constant, node->span);
        if (zero == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;

        /* ARRAY_ELEM_ADDR preserves the access qualifiers of the array
         * storage pointer.  If the slice weakens mutable storage to readonly,
         * perform that as an explicit pointer qualification afterwards. */
        CogIrTypeId source_data_type = cog_ir_type_pointer(
            state->lower->module,
            element,
            !place.is_writable,
            place.is_volatile
        );
        data = emit_index_address(
            state,
            COG_IR_OP_ARRAY_ELEM_ADDR,
            place.address,
            zero,
            element,
            !place.is_writable,
            place.is_volatile,
            node->span
        );
        if (data == COG_IR_VALUE_INVALID || source_data_type == COG_IR_TYPE_INVALID)
            return COG_IR_VALUE_INVALID;

        if (source_data_type != data_type) {
            data = emit_pointer_qualify(state, data, data_type, node->span);
            if (data == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;
        }
    }

    CogIrConstId length_constant = cog_ir_const_integer(
        state->lower->module,
        length_type,
        (uint64_t)source->array_size
    );
    CogIrValueId length = emit_constant_value(
        state, length_constant, node->span);
    return emit_slice_value(state, target_slice, data, length, node->span);
}

static CogIrValueId lower_slice_qualification_conversion(
    ExecLowerState *state,
    Node *node,
    CogIrValueId value,
    Type *source_slice,
    Type *target_slice
) {
    if (!source_slice || source_slice->kind != TYPE_SLICE ||
        !target_slice || target_slice->kind != TYPE_SLICE) {
        lower_error(state->lower, node->span,
            "invalid slice qualification conversion reached CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    CogIrTypeId element = cog_ir_lower_type(state->lower, source_slice->element);
    CogIrTypeId source_data_type = cog_ir_type_pointer(
        state->lower->module,
        element,
        source_slice->pointer_access == POINTER_ACCESS_READONLY,
        0
    );
    CogIrTypeId target_data_type = cog_ir_type_pointer(
        state->lower->module,
        element,
        target_slice->pointer_access == POINTER_ACCESS_READONLY,
        0
    );
    CogIrTypeId length_type = cog_ir_type_integer(state->lower->module, 64, 0);
    if (element == COG_IR_TYPE_INVALID || source_data_type == COG_IR_TYPE_INVALID ||
        target_data_type == COG_IR_TYPE_INVALID || length_type == COG_IR_TYPE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrInstruction data_extract = {
        .op = COG_IR_OP_EXTRACT_FIELD,
        .result_type = source_data_type,
        .span = node->span,
        .as.extract = { .aggregate = value, .index = 0 },
    };
    CogIrValueId data = emit_instruction_value(state, data_extract);

    CogIrInstruction length_extract = {
        .op = COG_IR_OP_EXTRACT_FIELD,
        .result_type = length_type,
        .span = node->span,
        .as.extract = { .aggregate = value, .index = 1 },
    };
    CogIrValueId length = emit_instruction_value(state, length_extract);
    if (data == COG_IR_VALUE_INVALID || length == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    if (source_data_type != target_data_type) {
        data = emit_pointer_qualify(
            state, data, target_data_type, node->span);
        if (data == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;
    }

    return emit_slice_value(state, target_slice, data, length, node->span);
}

static int normalize_pointer_comparison_operands(
    ExecLowerState *state,
    CogIrValueId *lhs_value,
    CogIrValueId *rhs_value,
    SourceSpan span
) {
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *lhs = function ? cog_ir_get_value(function, *lhs_value) : NULL;
    const CogIrValue *rhs = function ? cog_ir_get_value(function, *rhs_value) : NULL;
    if (!lhs || !rhs)
        return 0;
    if (lhs->type == rhs->type)
        return 1;

    CogIrTypeId lhs_type_id = lhs->type;
    CogIrTypeId rhs_type_id = rhs->type;
    const CogIrType *lhs_type = cog_ir_get_type(state->lower->module, lhs_type_id);
    const CogIrType *rhs_type = cog_ir_get_type(state->lower->module, rhs_type_id);
    CogIrTypeId common = COG_IR_TYPE_INVALID;

    if (lhs_type && rhs_type && lhs_type->kind == COG_IR_TYPE_POINTER && rhs_type->kind == COG_IR_TYPE_POINTER &&
        lhs_type->as.pointer.pointee == rhs_type->as.pointer.pointee) {
        common = cog_ir_type_pointer(
            state->lower->module,
            lhs_type->as.pointer.pointee,
            lhs_type->as.pointer.is_readonly || rhs_type->as.pointer.is_readonly,
            lhs_type->as.pointer.is_volatile || rhs_type->as.pointer.is_volatile
        );
    } else if (lhs_type && rhs_type && lhs_type->kind == COG_IR_TYPE_OPAQUE_POINTER &&
               rhs_type->kind == COG_IR_TYPE_OPAQUE_POINTER) {
        common = cog_ir_type_opaque_pointer(
            state->lower->module,
            lhs_type->as.opaque_pointer.is_readonly || rhs_type->as.opaque_pointer.is_readonly,
            lhs_type->as.opaque_pointer.is_volatile || rhs_type->as.opaque_pointer.is_volatile
        );
    }

    if (common == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "pointer comparison operands do not lower to a common CogIR type");
        return 0;
    }
    if (lhs_type_id != common) {
        *lhs_value = emit_pointer_qualify(state, *lhs_value, common, span);
        if (*lhs_value == COG_IR_VALUE_INVALID)
            return 0;
    }
    if (rhs_type_id != common) {
        *rhs_value = emit_pointer_qualify(state, *rhs_value, common, span);
        if (*rhs_value == COG_IR_VALUE_INVALID)
            return 0;
    }
    return 1;
}

static CogIrOp comparison_op_for_values(
    ExecLowerState *state,
    Node *node,
    CogIrValueId *lhs_value,
    CogIrValueId *rhs_value
) {
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *lhs = function ? cog_ir_get_value(function, *lhs_value) : NULL;
    const CogIrValue *rhs = function ? cog_ir_get_value(function, *rhs_value) : NULL;
    if (!lhs || !rhs)
        return (CogIrOp)-1;

    const CogIrType *type = cog_ir_get_type(state->lower->module, lhs->type);
    if (!type)
        return (CogIrOp)-1;

    if ((type->kind == COG_IR_TYPE_POINTER || type->kind == COG_IR_TYPE_OPAQUE_POINTER) && lhs->type != rhs->type) {
        if (!normalize_pointer_comparison_operands(state, lhs_value, rhs_value, node->span))
            return (CogIrOp)-1;
        function = cog_ir_get_function(state->lower->module, state->function);
        lhs = function ? cog_ir_get_value(function, *lhs_value) : NULL;
        rhs = function ? cog_ir_get_value(function, *rhs_value) : NULL;
        type = lhs ? cog_ir_get_type(state->lower->module, lhs->type) : NULL;
    }

    if (!lhs || !rhs || !type || lhs->type != rhs->type)
        return (CogIrOp)-1;

    switch (type->kind) {
        case COG_IR_TYPE_INTEGER:
            switch (node->as.binary.op) {
                case TOK_EQUAL_EQUAL: return COG_IR_OP_ICMP_EQ;
                case TOK_BANG_EQUAL: return COG_IR_OP_ICMP_NE;
                case TOK_LESS: return type->as.integer.is_signed ? COG_IR_OP_ICMP_SLT : COG_IR_OP_ICMP_ULT;
                case TOK_LESS_EQUAL: return type->as.integer.is_signed ? COG_IR_OP_ICMP_SLE : COG_IR_OP_ICMP_ULE;
                case TOK_GREATER: return type->as.integer.is_signed ? COG_IR_OP_ICMP_SGT : COG_IR_OP_ICMP_UGT;
                case TOK_GREATER_EQUAL: return type->as.integer.is_signed ? COG_IR_OP_ICMP_SGE : COG_IR_OP_ICMP_UGE;
                default: return (CogIrOp)-1;
            }

        case COG_IR_TYPE_FLOAT:
            switch (node->as.binary.op) {
                case TOK_EQUAL_EQUAL: return COG_IR_OP_FCMP_EQ;
                case TOK_BANG_EQUAL: return COG_IR_OP_FCMP_NE;
                case TOK_LESS: return COG_IR_OP_FCMP_LT;
                case TOK_LESS_EQUAL: return COG_IR_OP_FCMP_LE;
                case TOK_GREATER: return COG_IR_OP_FCMP_GT;
                case TOK_GREATER_EQUAL: return COG_IR_OP_FCMP_GE;
                default: return (CogIrOp)-1;
            }

        case COG_IR_TYPE_BOOL:
        case COG_IR_TYPE_ENUM:
            if (node->as.binary.op == TOK_EQUAL_EQUAL) return COG_IR_OP_ICMP_EQ;
            if (node->as.binary.op == TOK_BANG_EQUAL) return COG_IR_OP_ICMP_NE;
            return (CogIrOp)-1;

        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER:
        case COG_IR_TYPE_FUNCTION:
            if (node->as.binary.op == TOK_EQUAL_EQUAL) return COG_IR_OP_PTR_EQ;
            if (node->as.binary.op == TOK_BANG_EQUAL) return COG_IR_OP_PTR_NE;
            return (CogIrOp)-1;

        default:
            return (CogIrOp)-1;
    }
}

static int is_comparison_token(TokenType op)
{
    return op == TOK_EQUAL_EQUAL || op == TOK_BANG_EQUAL || op == TOK_LESS ||
           op == TOK_LESS_EQUAL || op == TOK_GREATER || op == TOK_GREATER_EQUAL;
}

static int expression_may_create_cfg(ExecLowerState *state, Node *node)
{
    if (!node)
        return 0;
    ConstValue constant;
    if (semantic_get_constant_value((SemanticContext *)&state->lower->frontend->sem, node, &constant))
        return 0;
    switch (node->type) {
        case NODE_BINARY:
            if (node->as.binary.op == TOK_AND_AND || node->as.binary.op == TOK_OR_OR)
                return 1;
            return expression_may_create_cfg(state, node->as.binary.left) ||
                   expression_may_create_cfg(state, node->as.binary.right);
        case NODE_UNARY:
            return expression_may_create_cfg(state, node->as.unary.operand);
        case NODE_CAST:
            return expression_may_create_cfg(state, node->as.cast_expr.expression);
        case NODE_FIELD:
            return expression_may_create_cfg(state, node->as.field.object);
        case NODE_INDEX:
            return expression_may_create_cfg(state, node->as.index.object) ||
                   expression_may_create_cfg(state, node->as.index.index);
        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.elements.count; ++i)
                if (expression_may_create_cfg(state, node->as.array_literal.elements.items[i]))
                    return 1;
            return 0;
        case NODE_STRUCT_INIT:
            for (int i = 0; i < node->as.struct_init.fields.count; ++i)
                if (expression_may_create_cfg(state, node->as.struct_init.fields.items[i]->as.field_init.value))
                    return 1;
            return 0;
        case NODE_CALL:
            if (expression_may_create_cfg(state, node->as.call.callee))
                return 1;
            for (int i = 0; i < node->as.call.arguments.count; ++i)
                if (expression_may_create_cfg(state, node->as.call.arguments.items[i]))
                    return 1;
            return 0;
        default:
            return 0;
    }
}

static CogIrSlotId spill_value(ExecLowerState *state, CogIrValueId value, SourceSpan span)
{
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *ir_value = function ? cog_ir_get_value(function, value) : NULL;
    if (!ir_value) {
        lower_error(state->lower, span, "cannot spill invalid CogIR value across CFG expression");
        return COG_IR_SLOT_INVALID;
    }
    CogIrTypeId type = ir_value->type;
    CogIrSlotId slot = cog_ir_add_slot(
        state->lower->module,
        state->function,
        COG_IR_SLOT_COMPILER_TEMP,
        COG_IR_PARAMETER_INDEX_INVALID,
        string_view_from_cstr(".cfg.tmp"),
        span,
        type
    );
    if (slot == COG_IR_SLOT_INVALID) {
        lower_error(state->lower, span, "failed to allocate CogIR CFG spill slot");
        return COG_IR_SLOT_INVALID;
    }
    if (!annotate_slot_abi(state, slot, ir_value->abi_type, span))
        return COG_IR_SLOT_INVALID;
    CogIrValueId address = emit_local_address(state, slot, type, span);
    if (address == COG_IR_VALUE_INVALID || !emit_store(state, address, value, 0, span))
        return COG_IR_SLOT_INVALID;
    return slot;
}

static CogIrValueId reload_spill(ExecLowerState *state, CogIrSlotId slot, SourceSpan span)
{
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrSlot *ir_slot = function ? cog_ir_get_slot(function, slot) : NULL;
    if (!ir_slot) {
        lower_error(state->lower, span, "invalid CogIR CFG spill slot");
        return COG_IR_VALUE_INVALID;
    }
    CogIrTypeId type = ir_slot->type;
    CogIrValueId address = emit_local_address(state, slot, type, span);
    if (address == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;
    CogIrValueId value = emit_load(state, address, type, 0, span);
    if (value == COG_IR_VALUE_INVALID ||
        !annotate_value_abi(state, value, ir_slot->abi_type, span))
        return COG_IR_VALUE_INVALID;
    return value;
}

static CogIrValueId lower_logical_expression(ExecLowerState *state, Node *node)
{
    CogIrTypeId bool_type = cog_ir_type_bool(state->lower->module);
    CogIrValueId lhs = lower_expression(state, node->as.binary.left);
    if (lhs == COG_IR_VALUE_INVALID || !block_is_open(state))
        return COG_IR_VALUE_INVALID;

    CogIrBlockId rhs_block = add_block(state, node->as.binary.op == TOK_AND_AND ? "logic.and.rhs" : "logic.or.rhs", node->span);
    CogIrBlockId join_block = add_block(state, node->as.binary.op == TOK_AND_AND ? "logic.and.join" : "logic.or.join", node->span);
    if (rhs_block == COG_IR_BLOCK_INVALID || join_block == COG_IR_BLOCK_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrValueId join_value = cog_ir_add_block_parameter(
        state->lower->module,
        state->function,
        join_block,
        bool_type,
        string_view_from_cstr("logic.value"),
        node->span
    );
    if (join_value == COG_IR_VALUE_INVALID) {
        lower_error(state->lower, node->span, "failed to create short-circuit block parameter");
        return COG_IR_VALUE_INVALID;
    }

    CogIrConstId short_const = cog_ir_const_bool(
        state->lower->module,
        bool_type,
        node->as.binary.op == TOK_OR_OR
    );
    CogIrValueId short_value = emit_constant_value(state, short_const, node->span);
    if (short_value == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    if (node->as.binary.op == TOK_AND_AND) {
        if (!set_cond_branch(state, lhs, rhs_block, NULL, 0, join_block, &short_value, 1, node->span))
            return COG_IR_VALUE_INVALID;
    } else {
        if (!set_cond_branch(state, lhs, join_block, &short_value, 1, rhs_block, NULL, 0, node->span))
            return COG_IR_VALUE_INVALID;
    }

    state->block = rhs_block;
    CogIrValueId rhs = lower_expression(state, node->as.binary.right);
    if (rhs == COG_IR_VALUE_INVALID || !block_is_open(state))
        return COG_IR_VALUE_INVALID;
    if (!set_branch_args(state, join_block, &rhs, 1, node->span))
        return COG_IR_VALUE_INVALID;

    state->block = join_block;
    return join_value;
}

static CogIrValueId lower_wrapping_builtin_call(
    ExecLowerState *state,
    Node *node,
    BuiltinKind builtin_kind
) {
    size_t expected_count = builtin_kind == BUILTIN_WRAPPING_NEG ? 1u : 2u;
    if ((size_t)node->as.call.arguments.count != expected_count) {
        lower_error(state->lower, node->span, "wrapping builtin has invalid checked argument count during CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    Type *result_sem_type = semantic_get_effective_expr_type(sem, node);
    CogIrTypeId result_type = cog_ir_lower_type(state->lower, result_sem_type);
    if (result_type == COG_IR_TYPE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrOp op = (CogIrOp)-1;
    switch (builtin_kind) {
        case BUILTIN_WRAPPING_ADD: op = COG_IR_OP_IADD_WRAP; break;
        case BUILTIN_WRAPPING_SUB: op = COG_IR_OP_ISUB_WRAP; break;
        case BUILTIN_WRAPPING_MUL: op = COG_IR_OP_IMUL_WRAP; break;
        case BUILTIN_WRAPPING_NEG: op = COG_IR_OP_INEG_WRAP; break;
        case BUILTIN_NONE:
            break;
    }
    if ((int)op < 0) {
        lower_error(state->lower, node->span, "unknown wrapping builtin during CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    Node *first_node = node->as.call.arguments.items[0];
    CogIrValueId first = lower_expression(state, first_node);
    if (first == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    if (expected_count == 1u) {
        CogIrInstruction instruction = {
            .op = op,
            .result_type = result_type,
            .span = node->span,
            .as.unary = { .operand = first },
        };
        return emit_instruction_value(state, instruction);
    }

    Node *second_node = node->as.call.arguments.items[1];
    CogIrSlotId first_spill = COG_IR_SLOT_INVALID;
    if (expression_may_create_cfg(state, second_node)) {
        first_spill = spill_value(state, first, first_node->span);
        if (first_spill == COG_IR_SLOT_INVALID)
            return COG_IR_VALUE_INVALID;
    }

    CogIrValueId second = lower_expression(state, second_node);
    if (second == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    if (first_spill != COG_IR_SLOT_INVALID) {
        first = reload_spill(state, first_spill, first_node->span);
        if (first == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;
    }

    CogIrInstruction instruction = {
        .op = op,
        .result_type = result_type,
        .span = node->span,
        .as.binary = { .lhs = first, .rhs = second },
    };
    return emit_instruction_value(state, instruction);
}

static CogIrTypeId c_variadic_promotion_target(
    CogIrModule *module,
    CogIrTypeId source_type
)
{
    const CogIrType *source = cog_ir_get_type(module, source_type);
    if (!source)
        return COG_IR_TYPE_INVALID;

    if (source->kind == COG_IR_TYPE_BOOL)
        return cog_ir_type_integer(module, module->target.c_int_bits, 1);

    if (source->kind == COG_IR_TYPE_FLOAT && source->as.floating.bits == 32)
        return cog_ir_type_float(module, 64);

    const CogIrType *integer = source;
    if (source->kind == COG_IR_TYPE_ENUM)
        integer = cog_ir_get_type(module, source->as.enumeration.backing_type);

    if (integer && integer->kind == COG_IR_TYPE_INTEGER &&
        integer->as.integer.bits < module->target.c_int_bits) {
        /* A strictly wider C int represents every value of both the signed and
         * unsigned source width, so the C integer promotions select int. */
        return cog_ir_type_integer(module, module->target.c_int_bits, 1);
    }

    return source_type;
}

static CogIrValueId lower_c_variadic_argument(
    ExecLowerState *state,
    CogIrValueId value,
    SourceSpan span
)
{
    const CogIrFunction *function =
        cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *source_value = function ? cog_ir_get_value(function, value) : NULL;
    if (!source_value) {
        lower_error(state->lower, span, "C variadic argument has no CogIR value");
        return COG_IR_VALUE_INVALID;
    }

    CogIrTypeId target =
        c_variadic_promotion_target(state->lower->module, source_value->type);
    if (target == COG_IR_TYPE_INVALID) {
        lower_error(state->lower, span, "failed to determine C variadic promotion type");
        return COG_IR_VALUE_INVALID;
    }
    if (target == source_value->type)
        return value;

    return emit_conversion(
        state,
        COG_IR_OP_C_VARARG_PROMOTE,
        value,
        target,
        span
    );
}

static CogIrValueId lower_call_expression(ExecLowerState *state, Node *node)
{
    size_t count = (size_t)node->as.call.arguments.count;
    int needs_spills = expression_may_create_cfg(state, node);

    CogIrValueId callee = lower_expression(state, node->as.call.callee);
    if (callee == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    const CogIrFunction *function =
        cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *callee_value = function ? cog_ir_get_value(function, callee) : NULL;
    const CogIrType *callee_type =
        callee_value ? cog_ir_get_type(state->lower->module, callee_value->type) : NULL;
    if (!callee_type || callee_type->kind != COG_IR_TYPE_FUNCTION) {
        lower_error(state->lower, node->span, "call callee has no CogIR function type");
        return COG_IR_VALUE_INVALID;
    }
    const size_t fixed_parameter_count = callee_type->as.function.parameter_count;
    const int is_c_variadic =
        callee_type->as.function.abi == COG_IR_ABI_C &&
        callee_type->as.function.is_variadic;

    CogIrSlotId callee_slot = COG_IR_SLOT_INVALID;
    CogIrSlotId *argument_slots = NULL;
    CogIrValueId *arguments = count ? calloc(count, sizeof(*arguments)) : NULL;
    if (count && !arguments) {
        lower_error(state->lower, node->span, "out of memory while lowering call arguments");
        return COG_IR_VALUE_INVALID;
    }
    if (needs_spills && count) {
        argument_slots = calloc(count, sizeof(*argument_slots));
        if (!argument_slots) {
            free(arguments);
            lower_error(state->lower, node->span, "out of memory while preserving call arguments across CFG");
            return COG_IR_VALUE_INVALID;
        }
        for (size_t i = 0; i < count; ++i)
            argument_slots[i] = COG_IR_SLOT_INVALID;
    }

    if (needs_spills) {
        callee_slot = spill_value(state, callee, node->as.call.callee->span);
        if (callee_slot == COG_IR_SLOT_INVALID) {
            free(arguments); free(argument_slots);
            return COG_IR_VALUE_INVALID;
        }
    }

    for (size_t i = 0; i < count; ++i) {
        CogIrValueId argument = lower_expression(state, node->as.call.arguments.items[i]);
        if (argument == COG_IR_VALUE_INVALID) {
            free(arguments); free(argument_slots);
            return COG_IR_VALUE_INVALID;
        }
        if (is_c_variadic && i >= fixed_parameter_count) {
            argument = lower_c_variadic_argument(
                state,
                argument,
                node->as.call.arguments.items[i]->span
            );
            if (argument == COG_IR_VALUE_INVALID) {
                free(arguments); free(argument_slots);
                return COG_IR_VALUE_INVALID;
            }
        }
        if (needs_spills) {
            argument_slots[i] = spill_value(state, argument, node->as.call.arguments.items[i]->span);
            if (argument_slots[i] == COG_IR_SLOT_INVALID) {
                free(arguments); free(argument_slots);
                return COG_IR_VALUE_INVALID;
            }
        } else {
            arguments[i] = argument;
        }
    }

    if (needs_spills) {
        callee = reload_spill(state, callee_slot, node->as.call.callee->span);
        if (callee == COG_IR_VALUE_INVALID) {
            free(arguments); free(argument_slots);
            return COG_IR_VALUE_INVALID;
        }
        for (size_t i = 0; i < count; ++i) {
            arguments[i] = reload_spill(state, argument_slots[i], node->as.call.arguments.items[i]->span);
            if (arguments[i] == COG_IR_VALUE_INVALID) {
                free(arguments); free(argument_slots);
                return COG_IR_VALUE_INVALID;
            }
        }
    }

    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemExprInfo *call_info = semantic_get_expr_info(sem, node);
    Type *result_sem_type = call_info ? call_info->type : NULL;
    CogIrTypeId result_type = COG_IR_TYPE_INVALID;
    if (result_sem_type && result_sem_type->kind != TYPE_VOID) {
        result_type = cog_ir_lower_type(state->lower, result_sem_type);
        if (result_type == COG_IR_TYPE_INVALID) {
            free(arguments); free(argument_slots);
            return COG_IR_VALUE_INVALID;
        }
    }

    CogIrInstruction instruction = {
        .op = COG_IR_OP_CALL,
        .result_type = result_type,
        .span = node->span,
        .as.call = {
            .callee = callee,
            .arguments = arguments,
            .argument_count = count,
        },
    };
    CogIrValueId result = COG_IR_VALUE_INVALID;
    int ok = cog_ir_emit(state->lower->module, state->function, state->block, &instruction,
                         result_type == COG_IR_TYPE_INVALID ? NULL : &result);
    free(arguments); free(argument_slots);
    if (!ok) {
        lower_error(state->lower, node->span, "failed to emit CogIR call");
        return COG_IR_VALUE_INVALID;
    }

    if (result_type != COG_IR_TYPE_INVALID) {
        const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
        const CogIrValue *callee_value = function ? cog_ir_get_value(function, callee) : NULL;
        const CogIrAbiType *callee_abi = callee_value && callee_value->abi_type != COG_IR_ABI_TYPE_INVALID
            ? cog_ir_get_abi_type(state->lower->module, callee_value->abi_type)
            : NULL;
        if (callee_abi && callee_abi->kind == COG_IR_ABI_TYPE_FUNCTION) {
            const CogIrAbiType *return_abi = cog_ir_get_abi_type(
                state->lower->module, callee_abi->return_type);
            if (return_abi && return_abi->runtime_type == result_type &&
                !annotate_value_abi(state, result, return_abi->id, node->span))
                return COG_IR_VALUE_INVALID;
        }
    }

    return result_type == COG_IR_TYPE_INVALID ? COG_IR_VALUE_INVALID : result;
}


static CogIrValueId lower_character_literal(ExecLowerState *state, Node *node)
{
    StringDecodeInfo info = string_analyze(node->as.char_literal);
    if (!info.ok || info.decoded_length != 1) {
        lower_error(state->lower, node->span, "invalid character literal reached CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }
    char decoded = 0;
    if (!string_decode_into(node->as.char_literal, &decoded).ok)
        return COG_IR_VALUE_INVALID;
    CogIrTypeId type = cog_ir_lower_type(state->lower, effective_semantic_type(state, node));
    CogIrConstId constant = cog_ir_const_integer(
        state->lower->module,
        type,
        (uint8_t)decoded
    );
    return constant == COG_IR_CONST_INVALID ? COG_IR_VALUE_INVALID
                                            : emit_constant_value(state, constant, node->span);
}

static CogIrValueId lower_array_value_from_bytes(
    ExecLowerState *state,
    CogIrTypeId array_type,
    CogIrTypeId element_type,
    const unsigned char *bytes,
    size_t count,
    SourceSpan span
) {
    CogIrValueId *values = count ? calloc(count, sizeof(*values)) : NULL;
    if (count && !values) {
        lower_error(state->lower, span, "out of memory while lowering string bytes");
        return COG_IR_VALUE_INVALID;
    }
    for (size_t i = 0; i < count; ++i) {
        CogIrConstId c = cog_ir_const_integer(state->lower->module, element_type, bytes[i]);
        values[i] = c == COG_IR_CONST_INVALID ? COG_IR_VALUE_INVALID
                                              : emit_constant_value(state, c, span);
        if (values[i] == COG_IR_VALUE_INVALID) {
            free(values);
            return COG_IR_VALUE_INVALID;
        }
    }
    CogIrValueId result = emit_aggregate_value(
        state,
        COG_IR_OP_MAKE_ARRAY,
        array_type,
        values,
        count,
        span
    );
    free(values);
    return result;
}

static int decode_string_bytes(Node *node, unsigned char **out_bytes, size_t *out_count)
{
    *out_bytes = NULL;
    *out_count = 0;
    StringDecodeInfo info = string_analyze(node->as.string_literal);
    if (!info.ok)
        return 0;
    size_t count = (size_t)info.decoded_length + 1;
    unsigned char *bytes = calloc(count ? count : 1, 1);
    if (!bytes)
        return 0;
    if (info.decoded_length && !string_decode_into(node->as.string_literal, (char *)bytes).ok) {
        free(bytes);
        return 0;
    }
    bytes[count - 1] = 0;
    *out_bytes = bytes;
    *out_count = count;
    return 1;
}

static CogIrValueId lower_string_literal(ExecLowerState *state, Node *node)
{
    Type *sem_type = effective_semantic_type(state, node);
    CogIrTypeId result_type = cog_ir_lower_type(state->lower, sem_type);
    const CogIrType *ir_type = cog_ir_get_type(state->lower->module, result_type);
    if (!sem_type || result_type == COG_IR_TYPE_INVALID || !ir_type)
        return COG_IR_VALUE_INVALID;

    unsigned char *bytes = NULL;
    size_t count = 0;
    if (!decode_string_bytes(node, &bytes, &count)) {
        lower_error(state->lower, node->span, "invalid string literal reached CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    if (sem_type->kind == TYPE_SLICE) {
        CogIrTypeId element_type = cog_ir_lower_type(state->lower, sem_type->element);
        CogIrTypeId array_type = cog_ir_type_array(state->lower->module, element_type, count);
        CogIrConstId *elements = count ? calloc(count, sizeof(*elements)) : NULL;
        if (element_type == COG_IR_TYPE_INVALID || array_type == COG_IR_TYPE_INVALID ||
            (count && !elements)) {
            free(bytes);
            free(elements);
            lower_error(state->lower, node->span, "failed to construct string-slice backing array");
            return COG_IR_VALUE_INVALID;
        }

        for (size_t i = 0; i < count; ++i) {
            elements[i] = cog_ir_const_integer(state->lower->module, element_type, bytes[i]);
            if (elements[i] == COG_IR_CONST_INVALID) {
                free(bytes);
                free(elements);
                return COG_IR_VALUE_INVALID;
            }
        }
        CogIrConstId initializer = cog_ir_const_array(
            state->lower->module, array_type, elements, count);
        free(elements);
        free(bytes);
        if (initializer == COG_IR_CONST_INVALID)
            return COG_IR_VALUE_INVALID;

        CogIrGlobalId global = cog_ir_add_global(
            state->lower->module,
            string_view_from_cstr(".str"),
            node->span,
            array_type,
            COG_IR_ABI_TYPE_INVALID,
            COG_IR_LINKAGE_INTERNAL,
            1,
            1,
            initializer
        );
        if (global == COG_IR_GLOBAL_INVALID) {
            lower_error(state->lower, node->span, "failed to create string-slice backing global");
            return COG_IR_VALUE_INVALID;
        }

        CogIrValueId base = emit_global_address(state, global, array_type, 1, node->span);
        CogIrTypeId index_type = cog_ir_type_integer(state->lower->module, 64, 0);
        CogIrConstId zero = cog_ir_const_integer(state->lower->module, index_type, 0);
        CogIrValueId index = emit_constant_value(state, zero, node->span);
        if (base == COG_IR_VALUE_INVALID || index == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;

        CogIrValueId data = emit_index_address(
            state,
            COG_IR_OP_ARRAY_ELEM_ADDR,
            base,
            index,
            element_type,
            1,
            0,
            node->span
        );
        CogIrConstId length_constant = cog_ir_const_integer(
            state->lower->module,
            index_type,
            count ? (uint64_t)(count - 1) : 0
        );
        CogIrValueId length = emit_constant_value(
            state, length_constant, node->span);
        if (data == COG_IR_VALUE_INVALID || length == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;

        CogIrValueId fields[2] = { data, length };
        return emit_aggregate_value(
            state,
            COG_IR_OP_MAKE_STRUCT,
            result_type,
            fields,
            2,
            node->span
        );
    }

    if (ir_type->kind == COG_IR_TYPE_ARRAY) {
        CogIrValueId value = lower_array_value_from_bytes(
            state,
            result_type,
            ir_type->as.array.element_type,
            bytes,
            count,
            node->span
        );
        free(bytes);
        return value;
    }

    if (ir_type->kind == COG_IR_TYPE_POINTER) {
        CogIrTypeId element_type = ir_type->as.pointer.pointee;
        CogIrTypeId array_type = cog_ir_type_array(state->lower->module, element_type, count);
        CogIrConstId *elements = count ? calloc(count, sizeof(*elements)) : NULL;
        if (array_type == COG_IR_TYPE_INVALID || (count && !elements)) {
            free(bytes); free(elements);
            lower_error(state->lower, node->span, "failed to construct C string backing array");
            return COG_IR_VALUE_INVALID;
        }
        for (size_t i = 0; i < count; ++i) {
            elements[i] = cog_ir_const_integer(state->lower->module, element_type, bytes[i]);
            if (elements[i] == COG_IR_CONST_INVALID) {
                free(bytes); free(elements);
                return COG_IR_VALUE_INVALID;
            }
        }
        CogIrConstId initializer = cog_ir_const_array(state->lower->module, array_type, elements, count);
        free(elements); free(bytes);
        if (initializer == COG_IR_CONST_INVALID)
            return COG_IR_VALUE_INVALID;

        /*
         * Direct #extern(c) string literals are semantically contextualized as
         * readonly c_char*. Preserve that exact C object spelling on the
         * compiler-generated backing array so native call lowering can pass a
         * `const char *` without signed-char pointer warnings.
         */
        CogIrAbiTypeId char_abi = cog_ir_abi_type_c_scalar(
            state->lower->module, element_type, COG_IR_C_SCALAR_CHAR);
        CogIrAbiTypeId array_abi = char_abi == COG_IR_ABI_TYPE_INVALID
            ? COG_IR_ABI_TYPE_INVALID
            : cog_ir_abi_type_array(state->lower->module, array_type, char_abi);
        if (array_abi == COG_IR_ABI_TYPE_INVALID) {
            lower_error(state->lower, node->span, "failed to preserve C string ABI spelling");
            return COG_IR_VALUE_INVALID;
        }

        CogIrGlobalId global = cog_ir_add_global(
            state->lower->module,
            string_view_from_cstr(".str"),
            node->span,
            array_type,
            array_abi,
            COG_IR_LINKAGE_INTERNAL,
            1,
            1,
            initializer
        );
        if (global == COG_IR_GLOBAL_INVALID) {
            lower_error(state->lower, node->span, "failed to create C string backing global");
            return COG_IR_VALUE_INVALID;
        }
        CogIrValueId base = emit_global_address(state, global, array_type, 1, node->span);
        CogIrTypeId index_type = cog_ir_type_integer(state->lower->module, 32, 0);
        CogIrConstId zero = cog_ir_const_integer(state->lower->module, index_type, 0);
        CogIrValueId index = emit_constant_value(state, zero, node->span);
        if (base == COG_IR_VALUE_INVALID || index == COG_IR_VALUE_INVALID)
            return COG_IR_VALUE_INVALID;
        return emit_index_address(
            state,
            COG_IR_OP_ARRAY_ELEM_ADDR,
            base,
            index,
            element_type,
            ir_type->as.pointer.is_readonly,
            ir_type->as.pointer.is_volatile,
            node->span
        );
    }

    free(bytes);
    lower_error(state->lower, node->span, "string literal has unsupported CogIR destination type");
    return COG_IR_VALUE_INVALID;
}

static CogIrValueId lower_array_literal(ExecLowerState *state, Node *node)
{
    Type *sem_type = effective_semantic_type(state, node);
    CogIrTypeId type = cog_ir_lower_type(state->lower, sem_type);
    const CogIrType *ir_type = cog_ir_get_type(state->lower->module, type);
    if (!ir_type || ir_type->kind != COG_IR_TYPE_ARRAY) {
        lower_error(state->lower, node->span, "array initializer has invalid CogIR type");
        return COG_IR_VALUE_INVALID;
    }

    if (node->as.array_literal.is_zero_initializer) {
        CogIrConstId zero = cog_ir_const_zero(state->lower->module, type);
        if (zero == COG_IR_CONST_INVALID) {
            lower_error(state->lower, node->span, "failed to lower array zero initializer");
            return COG_IR_VALUE_INVALID;
        }
        return emit_constant_value(state, zero, node->span);
    }

    size_t count = (size_t)node->as.array_literal.elements.count;
    if (ir_type->as.array.length != count) {
        lower_error(state->lower, node->span, "array literal type/count mismatch during CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    CogIrValueId *values = count ? calloc(count, sizeof(*values)) : NULL;
    CogIrSlotId *spills = count ? calloc(count, sizeof(*spills)) : NULL;
    int needs_spills = expression_may_create_cfg(state, node);
    if (count && (!values || !spills)) {
        free(values); free(spills);
        lower_error(state->lower, node->span, "out of memory while lowering array literal");
        return COG_IR_VALUE_INVALID;
    }
    for (size_t i = 0; i < count; ++i)
        spills[i] = COG_IR_SLOT_INVALID;

    for (size_t i = 0; i < count; ++i) {
        Node *element = node->as.array_literal.elements.items[i];
        CogIrValueId value = lower_expression(state, element);
        if (value == COG_IR_VALUE_INVALID) goto fail;
        if (needs_spills) {
            spills[i] = spill_value(state, value, element->span);
            if (spills[i] == COG_IR_SLOT_INVALID) goto fail;
        } else {
            values[i] = value;
        }
    }
    if (needs_spills) {
        for (size_t i = 0; i < count; ++i) {
            values[i] = reload_spill(state, spills[i], node->as.array_literal.elements.items[i]->span);
            if (values[i] == COG_IR_VALUE_INVALID) goto fail;
        }
    }
    {
        CogIrValueId result = emit_aggregate_value(state, COG_IR_OP_MAKE_ARRAY, type, values, count, node->span);
        free(values); free(spills);
        return result;
    }
fail:
    free(values); free(spills);
    return COG_IR_VALUE_INVALID;
}

static CogIrValueId lower_struct_initializer(ExecLowerState *state, Node *node)
{
    Type *sem_type = effective_semantic_type(state, node);
    CogIrTypeId type = cog_ir_lower_type(state->lower, sem_type);
    const CogIrType *ir_type = cog_ir_get_type(state->lower->module, type);
    if (!sem_type || sem_type->kind != TYPE_STRUCT || !ir_type || ir_type->kind != COG_IR_TYPE_STRUCT) {
        lower_error(state->lower, node->span, "struct initializer has invalid CogIR type");
        return COG_IR_VALUE_INVALID;
    }
    size_t field_count = (size_t)sem_type->field_count;
    CogIrValueId *fields = field_count ? calloc(field_count, sizeof(*fields)) : NULL;
    CogIrSlotId *spills = field_count ? calloc(field_count, sizeof(*spills)) : NULL;
    int needs_spills = expression_may_create_cfg(state, node);
    if (field_count && (!fields || !spills)) {
        free(fields); free(spills);
        lower_error(state->lower, node->span, "out of memory while lowering struct initializer");
        return COG_IR_VALUE_INVALID;
    }
    for (size_t i = 0; i < field_count; ++i) spills[i] = COG_IR_SLOT_INVALID;

    for (int i = 0; i < node->as.struct_init.fields.count; ++i) {
        Node *field_init = node->as.struct_init.fields.items[i];
        int field_index = semantic_struct_field_index(sem_type, field_init->as.field_init.name);
        if (field_index < 0) {
            lower_error(state->lower, field_init->span, "struct initializer field has no semantic index");
            goto fail;
        }
        Node *value_node = field_init->as.field_init.value;
        CogIrValueId value = lower_expression(state, value_node);
        if (value == COG_IR_VALUE_INVALID) goto fail;
        if (needs_spills) {
            spills[field_index] = spill_value(state, value, value_node->span);
            if (spills[field_index] == COG_IR_SLOT_INVALID) goto fail;
        } else {
            fields[field_index] = value;
        }
    }
    if (needs_spills) {
        for (size_t i = 0; i < field_count; ++i) {
            if (spills[i] == COG_IR_SLOT_INVALID) {
                lower_error(state->lower, node->span, "struct initializer is missing a lowered field");
                goto fail;
            }
            fields[i] = reload_spill(state, spills[i], node->span);
            if (fields[i] == COG_IR_VALUE_INVALID) goto fail;
        }
    }
    {
        CogIrValueId result = emit_aggregate_value(state, COG_IR_OP_MAKE_STRUCT, type, fields, field_count, node->span);
        free(fields); free(spills);
        return result;
    }
fail:
    free(fields); free(spills);
    return COG_IR_VALUE_INVALID;
}


static CogIrValueId emit_semantic_constant_as_type(ExecLowerState *state,
                                                    const ConstValue *value,
                                                    Type *target_sem,
                                                    SourceSpan span)
{
    if (!state || !value || !target_sem)
        return COG_IR_VALUE_INVALID;

    CogIrTypeId target = cog_ir_lower_type(state->lower, target_sem);
    if (target == COG_IR_TYPE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrConstId constant = COG_IR_CONST_INVALID;
    switch (value->kind) {
        case CONST_VALUE_NULL:
            constant = cog_ir_const_null(state->lower->module, target);
            break;

        case CONST_VALUE_BOOL:
            if (target_sem->kind != TYPE_BOOL)
                return COG_IR_VALUE_INVALID;
            constant = cog_ir_const_bool(state->lower->module, target, value->as.boolean);
            break;

        case CONST_VALUE_INT: {
            unsigned width = integer_type_width(target_sem);
            if (width) {
                constant = cog_ir_const_integer(state->lower->module, target,
                                                integer_value_bits(value->as.integer, width));
                break;
            }
            if (target_sem->kind == TYPE_F32 || target_sem->kind == TYPE_F64) {
                double converted = (double)value->as.integer.magnitude;
                if (value->as.integer.is_negative)
                    converted = -converted;
                if (target_sem->kind == TYPE_F32) {
                    float rounded = (float)converted;
                    uint32_t bits;
                    memcpy(&bits, &rounded, sizeof(bits));
                    constant = cog_ir_const_float32(state->lower->module, target, bits);
                } else {
                    uint64_t bits;
                    memcpy(&bits, &converted, sizeof(bits));
                    constant = cog_ir_const_float64(state->lower->module, target, bits);
                }
            }
            break;
        }

        case CONST_VALUE_FLOAT:
            if (target_sem->kind == TYPE_F32) {
                float rounded = (float)value->as.floating;
                uint32_t bits;
                memcpy(&bits, &rounded, sizeof(bits));
                constant = cog_ir_const_float32(state->lower->module, target, bits);
            } else if (target_sem->kind == TYPE_F64) {
                uint64_t bits;
                double exact = value->as.floating;
                memcpy(&bits, &exact, sizeof(bits));
                constant = cog_ir_const_float64(state->lower->module, target, bits);
            } else {
                unsigned width = integer_type_width(target_sem);
                if (width) {
                    IntegerValue converted = {0};
                    double source = value->as.floating;
                    converted.is_negative = source < 0.0;
                    if (converted.is_negative) source = -source;
                    converted.magnitude = (uint64_t)source;
                    constant = cog_ir_const_integer(state->lower->module, target,
                                                    integer_value_bits(converted, width));
                }
            }
            break;
    }

    if (constant == COG_IR_CONST_INVALID) {
        lower_error(state->lower, span, "constant cannot be materialized as the checked cast target type");
        return COG_IR_VALUE_INVALID;
    }
    return emit_constant_value(state, constant, span);
}

static CogIrValueId lower_cast_expression(ExecLowerState *state, Node *node)
{
    Type *target_sem = effective_semantic_type(state, node);
    CogIrTypeId target = cog_ir_lower_type(state->lower, target_sem);
    if (target == COG_IR_TYPE_INVALID)
        return COG_IR_VALUE_INVALID;

    /*
     * An explicit cast supplies the concrete context for an otherwise frontend-only
     * adaptable literal (`untyped-int`, `untyped-float`, or `null`).  Those source
     * values must never be materialized as standalone CogIR types.  Semantic analysis
     * has already checked representability, so materialize the source constant directly
     * in the cast destination representation.
     */
    ConstValue source_constant;
    if (semantic_get_constant_value((SemanticContext *)&state->lower->frontend->sem,
                                    node->as.cast_expr.expression, &source_constant) &&
        source_constant.type &&
        (source_constant.type->kind == TYPE_UNTYPED_INT ||
         source_constant.type->kind == TYPE_UNTYPED_FLOAT ||
         source_constant.type->kind == TYPE_NULL)) {
        return emit_semantic_constant_as_type(state, &source_constant, target_sem, node->span);
    }

    CogIrValueId operand = lower_expression(state, node->as.cast_expr.expression);
    if (operand == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrOp op;
    switch (node->as.cast_expr.kind) {
        case CAST_TRUNCATING:
            op = COG_IR_OP_INT_TRUNCATE;
            break;
        case CAST_REINTERPRET:
            op = COG_IR_OP_PTR_REINTERPRET;
            break;
        case CAST_CHECKED: {
            const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
            const CogIrValue *value = function ? cog_ir_get_value(function, operand) : NULL;
            const CogIrType *source = value ? cog_ir_get_type(state->lower->module, value->type) : NULL;
            const CogIrType *dest = cog_ir_get_type(state->lower->module, target);
            if (source && dest &&
                ((source->kind == COG_IR_TYPE_POINTER && dest->kind == COG_IR_TYPE_POINTER) ||
                 (source->kind == COG_IR_TYPE_OPAQUE_POINTER && dest->kind == COG_IR_TYPE_OPAQUE_POINTER)))
                op = COG_IR_OP_PTR_QUALIFY;
            else
                op = COG_IR_OP_CAST_CHECKED;
            break;
        }
        default:
            lower_error(state->lower, node->span, "unknown cast kind during CogIR lowering");
            return COG_IR_VALUE_INVALID;
    }
    return emit_conversion(state, op, operand, target, node->span);
}

static CogIrValueId lower_expression_raw(ExecLowerState *state, Node *node)
{
    if (!node) {
        lower_error(state->lower, source_span_invalid(), "missing expression during CogIR lowering");
        return COG_IR_VALUE_INVALID;
    }

    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    ConstValue constant_value;
    if (semantic_get_constant_value(sem, node, &constant_value) && constant_value.type &&
        constant_value.type->kind != TYPE_UNTYPED_INT &&
        constant_value.type->kind != TYPE_UNTYPED_FLOAT &&
        constant_value.type->kind != TYPE_NULL) {
        CogIrConstId constant = cog_ir_lower_const_value(state->lower, &constant_value);
        if (constant == COG_IR_CONST_INVALID)
            return COG_IR_VALUE_INVALID;
        return emit_constant_value(state, constant, node->span);
    }

    /*
     * Some literals are checked as ordinary runtime operands without forcing
     * the semantic constant evaluator to run on the literal node itself.
     * Their effective type still records the contextual materialization, so
     * construct the already-checked ConstValue directly here.
     */
    if (node->type == NODE_NUMBER || node->type == NODE_BOOL || node->type == NODE_NULL) {
        Type *effective_type = semantic_get_effective_expr_type(sem, node);
        if (!effective_type || effective_type->kind == TYPE_UNTYPED_INT ||
            effective_type->kind == TYPE_UNTYPED_FLOAT || effective_type->kind == TYPE_NULL) {
            lower_error(state->lower, node->span, "literal has no concrete effective type during CogIR lowering");
            return COG_IR_VALUE_INVALID;
        }
        memset(&constant_value, 0, sizeof(constant_value));
        constant_value.type = effective_type;
        if (node->type == NODE_BOOL) {
            constant_value.kind = CONST_VALUE_BOOL;
            constant_value.as.boolean = node->as.boolean.value;
        } else if (node->type == NODE_NULL) {
            constant_value.kind = CONST_VALUE_NULL;
        } else if (node->as.number.kind == NUMBER_LITERAL_FLOAT) {
            constant_value.kind = CONST_VALUE_FLOAT;
            constant_value.as.floating = node->as.number.value.floating;
        } else {
            constant_value.kind = CONST_VALUE_INT;
            constant_value.as.integer.magnitude = node->as.number.value.integer;
            constant_value.as.integer.is_negative = 0;
        }
        CogIrConstId constant = cog_ir_lower_const_value(state->lower, &constant_value);
        if (constant == COG_IR_CONST_INVALID)
            return COG_IR_VALUE_INVALID;
        return emit_constant_value(state, constant, node->span);
    }

    switch (node->type) {
        case NODE_CHAR:
            return lower_character_literal(state, node);

        case NODE_STRING:
            return lower_string_literal(state, node);

        case NODE_ARRAY_LITERAL:
            return lower_array_literal(state, node);

        case NODE_STRUCT_INIT:
            return lower_struct_initializer(state, node);

        case NODE_CAST:
            return lower_cast_expression(state, node);

        case NODE_FIELD:
        case NODE_INDEX: {
            SemExprInfo *expr = semantic_get_expr_info(sem, node);
            if (node->type == NODE_FIELD && expr && expr->symbol &&
                expr->symbol->kind == SYMBOL_FUNCTION) {
                CogIrLowerDeclBinding *binding = binding_for_expr_ident(state->lower, node);
                if (!binding || binding->kind != COG_IR_LOWER_DECL_FUNCTION) {
                    lower_error(state->lower, node->span,
                                "qualified function has no CogIR declaration binding");
                    return COG_IR_VALUE_INVALID;
                }
                return emit_function_reference(state, binding, node->span);
            }
            if (node->type == NODE_FIELD && expr && expr->symbol &&
                expr->symbol->kind == SYMBOL_CONSTANT) {
                CogIrLowerDeclBinding *binding = binding_for_expr_ident(state->lower, node);
                if (!binding || binding->kind != COG_IR_LOWER_DECL_CONSTANT) {
                    lower_error(state->lower, node->span,
                                "qualified constant has no CogIR declaration binding");
                    return COG_IR_VALUE_INVALID;
                }
                return emit_constant_value(state, binding->as.constant, node->span);
            }
            if (node->type == NODE_FIELD && expr && expr->symbol && expr->symbol->kind == SYMBOL_TYPE &&
                expr->type && expr->type->kind == TYPE_ENUM) {
                int member = semantic_enum_member_index(expr->type, node->as.field.name);
                unsigned width = integer_type_width(expr->type);
                CogIrTypeId enum_type = cog_ir_lower_type(state->lower, expr->type);
                if (member < 0 || !width || enum_type == COG_IR_TYPE_INVALID) {
                    lower_error(state->lower, node->span, "enum member has no CogIR constant mapping");
                    return COG_IR_VALUE_INVALID;
                }
                uint64_t bits = integer_value_bits(expr->type->enum_members[member].value, width);
                CogIrConstId constant = cog_ir_const_integer(state->lower->module, enum_type, bits);
                return constant == COG_IR_CONST_INVALID ? COG_IR_VALUE_INVALID
                                                        : emit_constant_value(state, constant, node->span);
            }
            if (expr && expr->value_category == VALUE_CATEGORY_LVALUE) {
                LoweredPlace place;
                if (!lower_place(state, node, &place))
                    return COG_IR_VALUE_INVALID;
                return emit_load(state, place.address, place.type, place.is_volatile, node->span);
            }
            if (node->type == NODE_FIELD) {
                CogIrValueId aggregate = lower_expression(state, node->as.field.object);
                Type *object_type = semantic_get_effective_expr_type(sem, node->as.field.object);
                int field_index = semantic_struct_field_index(object_type, node->as.field.name);
                CogIrTypeId result_type = cog_ir_lower_type(state->lower, semantic_get_effective_expr_type(sem, node));
                if (aggregate == COG_IR_VALUE_INVALID || field_index < 0 || result_type == COG_IR_TYPE_INVALID)
                    return COG_IR_VALUE_INVALID;
                CogIrInstruction instruction = {
                    .op = COG_IR_OP_EXTRACT_FIELD,
                    .result_type = result_type,
                    .span = node->span,
                    .as.extract = { .aggregate = aggregate, .index = (uint32_t)field_index },
                };
                return emit_instruction_value(state, instruction);
            }
            lower_error(state->lower, node->span, "non-place array indexing is not supported by CogIR lowering");
            return COG_IR_VALUE_INVALID;
        }

        case NODE_IDENT: {
            CogIrLowerDeclBinding *binding = binding_for_expr_ident(state->lower, node);
            if (!binding) {
                lower_error(state->lower, node->span, "unbound identifier during CogIR executable lowering");
                return COG_IR_VALUE_INVALID;
            }
            if (binding->kind == COG_IR_LOWER_DECL_FUNCTION)
                return emit_function_reference(state, binding, node->span);
            if (binding->kind == COG_IR_LOWER_DECL_CONSTANT || binding->kind == COG_IR_LOWER_DECL_ENUM_MEMBER) {
                CogIrConstId constant = binding->kind == COG_IR_LOWER_DECL_CONSTANT
                    ? binding->as.constant : binding->as.enum_member.constant;
                return emit_constant_value(state, constant, node->span);
            }
            int is_volatile = 0;
            CogIrValueId address = lower_identifier_place(state, node, &is_volatile);
            if (address == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;
            CogIrValueId value = emit_load(state, address, binding->type, is_volatile, node->span);
            if (value == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;

            CogIrSlotId slot = COG_IR_SLOT_INVALID;
            if (binding->kind == COG_IR_LOWER_DECL_LOCAL)
                slot = binding->as.local.slot;
            else if (binding->kind == COG_IR_LOWER_DECL_PARAMETER)
                slot = binding->as.parameter.slot;
            if (slot != COG_IR_SLOT_INVALID) {
                const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
                const CogIrSlot *ir_slot = function ? cog_ir_get_slot(function, slot) : NULL;
                if (ir_slot && !annotate_value_abi(state, value, ir_slot->abi_type, node->span))
                    return COG_IR_VALUE_INVALID;
            }
            return value;
        }

        case NODE_BINARY: {
            if (node->as.binary.op == TOK_AND_AND || node->as.binary.op == TOK_OR_OR)
                return lower_logical_expression(state, node);

            CogIrValueId lhs = lower_expression(state, node->as.binary.left);
            if (lhs == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;
            CogIrSlotId lhs_spill = COG_IR_SLOT_INVALID;
            if (expression_may_create_cfg(state, node->as.binary.right)) {
                lhs_spill = spill_value(state, lhs, node->as.binary.left->span);
                if (lhs_spill == COG_IR_SLOT_INVALID)
                    return COG_IR_VALUE_INVALID;
            }
            CogIrValueId rhs = lower_expression(state, node->as.binary.right);
            if (rhs == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;
            if (lhs_spill != COG_IR_SLOT_INVALID) {
                lhs = reload_spill(state, lhs_spill, node->as.binary.left->span);
                if (lhs == COG_IR_VALUE_INVALID)
                    return COG_IR_VALUE_INVALID;
            }

            Type *sem_type = semantic_get_effective_expr_type(sem, node);
            CogIrTypeId result_type = cog_ir_lower_type(state->lower, sem_type);
            if (result_type == COG_IR_TYPE_INVALID)
                return COG_IR_VALUE_INVALID;

            CogIrOp op;
            if (is_comparison_token(node->as.binary.op))
                op = comparison_op_for_values(state, node, &lhs, &rhs);
            else
                op = lower_arithmetic_binary_op(state->lower, node, result_type);

            if ((int)op < 0) {
                lower_error(state->lower, node->span, "binary operation is outside the current CogIR executable-lowering slice");
                return COG_IR_VALUE_INVALID;
            }
            CogIrInstruction instruction = {
                .op = op,
                .result_type = result_type,
                .span = node->span,
                .as.binary = { .lhs = lhs, .rhs = rhs },
            };
            return emit_instruction_value(state, instruction);
        }

        case NODE_UNARY: {
            if (node->as.unary.op == TOK_AND) {
                LoweredPlace place;
                if (!lower_place(state, node->as.unary.operand, &place))
                    return COG_IR_VALUE_INVALID;
                return place.address;
            }
            if (node->as.unary.op == TOK_STAR) {
                LoweredPlace place;
                if (!lower_place(state, node, &place))
                    return COG_IR_VALUE_INVALID;
                return emit_load(state, place.address, place.type, place.is_volatile, node->span);
            }

            Type *sem_type = semantic_get_effective_expr_type(sem, node);
            CogIrTypeId result_type = cog_ir_lower_type(state->lower, sem_type);
            CogIrValueId operand = lower_expression(state, node->as.unary.operand);
            if (result_type == COG_IR_TYPE_INVALID || operand == COG_IR_VALUE_INVALID)
                return COG_IR_VALUE_INVALID;
            const CogIrType *type = cog_ir_get_type(state->lower->module, result_type);
            CogIrOp op = (CogIrOp)-1;
            if (node->as.unary.op == TOK_MINUS && type && type->kind == COG_IR_TYPE_INTEGER)
                op = COG_IR_OP_INEG_CHECKED;
            else if (node->as.unary.op == TOK_MINUS && type && type->kind == COG_IR_TYPE_FLOAT)
                op = COG_IR_OP_FNEG;
            else if (node->as.unary.op == TOK_BANG && type && type->kind == COG_IR_TYPE_BOOL)
                op = COG_IR_OP_BOOL_NOT;
            else if (node->as.unary.op == TOK_TILDE && type && type->kind == COG_IR_TYPE_INTEGER)
                op = COG_IR_OP_BIT_NOT;
            if ((int)op < 0) {
                lower_error(state->lower, node->span, "unary operation is outside the current CogIR executable-lowering slice");
                return COG_IR_VALUE_INVALID;
            }
            CogIrInstruction instruction = {
                .op = op,
                .result_type = result_type,
                .span = node->span,
                .as.unary = { .operand = operand },
            };
            return emit_instruction_value(state, instruction);
        }

        case NODE_CALL: {
            SemExprInfo *call_info = semantic_get_expr_info(sem, node);
            if (call_info && call_info->symbol && call_info->symbol->kind == SYMBOL_BUILTIN)
                return lower_wrapping_builtin_call(state, node, call_info->symbol->builtin_kind);
            return lower_call_expression(state, node);
        }

        default:
            lower_error(state->lower, node->span, "expression is outside the current CogIR executable-lowering slice");
            return COG_IR_VALUE_INVALID;
    }
}

static CogIrValueId lower_expression(ExecLowerState *state, Node *node)
{
    if (!node)
        return COG_IR_VALUE_INVALID;

    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemExprInfo *info = semantic_get_expr_info(sem, node);

    /* Array-to-slice adaptation needs the source place address, not an array
     * value load, so lower it before the ordinary raw-expression path. */
    if (info &&
        info->contextual_conversion == SEM_CONTEXT_CONVERSION_ARRAY_TO_SLICE &&
        info->contextual_type) {
        return lower_array_to_slice_conversion(
            state, node, info->contextual_type);
    }

    CogIrValueId value = lower_expression_raw(state, node);
    if (value == COG_IR_VALUE_INVALID)
        return value;

    if (!info || info->contextual_conversion == SEM_CONTEXT_CONVERSION_NONE || !info->contextual_type)
        return value;

    CogIrTypeId target = cog_ir_lower_type(state->lower, info->contextual_type);
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrValue *ir_value = function ? cog_ir_get_value(function, value) : NULL;
    if (target == COG_IR_TYPE_INVALID || !ir_value)
        return COG_IR_VALUE_INVALID;
    if (ir_value->type == target)
        return value; /* constants/string literals may already be materialized to the effective type */

    switch (info->contextual_conversion) {
        case SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION:
            return emit_pointer_qualify(state, value, target, node->span);

        case SEM_CONTEXT_CONVERSION_SLICE_QUALIFICATION:
            return lower_slice_qualification_conversion(
                state, node, value, info->type, info->contextual_type);

        case SEM_CONTEXT_CONVERSION_ARRAY_TO_SLICE:
            /* Handled before raw expression lowering above. */
            lower_error(state->lower, node->span,
                "array-to-slice conversion reached late contextual lowering");
            return COG_IR_VALUE_INVALID;

        case SEM_CONTEXT_CONVERSION_INT_MATERIALIZE:
        case SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE:
        case SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE:
        case SEM_CONTEXT_CONVERSION_NULL_TO_POINTER:
        case SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER:
            lower_error(state->lower, node->span, "contextual materialization did not produce its concrete CogIR type");
            return COG_IR_VALUE_INVALID;

        case SEM_CONTEXT_CONVERSION_NONE:
            break;
    }
    return value;
}

static int lower_assignment_statement(ExecLowerState *state, Node *node)
{
    Node *target = node->as.assign.target;
    int rhs_cfg = expression_may_create_cfg(state, node->as.assign.value);

    /*
     * A plain identifier address is side-effect-free and can be materialized
     * after a CFG-producing RHS. Keep that common case spill-free while
     * complex places are evaluated once and preserved across the RHS.
     */
    if (target && target->type == NODE_IDENT && rhs_cfg) {
        CogIrValueId value = lower_expression(state, node->as.assign.value);
        LoweredPlace place;
        if (value == COG_IR_VALUE_INVALID || !lower_place(state, target, &place))
            return 0;
        return emit_store(state, place.address, value, place.is_volatile, node->span);
    }

    LoweredPlace place;
    if (!lower_place(state, target, &place))
        return 0;

    CogIrValueId address = place.address;
    CogIrSlotId address_spill = COG_IR_SLOT_INVALID;
    if (rhs_cfg) {
        address_spill = spill_value(state, address, target->span);
        if (address_spill == COG_IR_SLOT_INVALID)
            return 0;
    }

    CogIrValueId value = lower_expression(state, node->as.assign.value);
    if (value == COG_IR_VALUE_INVALID)
        return 0;
    if (address_spill != COG_IR_SLOT_INVALID) {
        address = reload_spill(state, address_spill, target->span);
        if (address == COG_IR_VALUE_INVALID)
            return 0;
    }
    return emit_store(state, address, value, place.is_volatile, node->span);
}

static CogIrOp compound_assignment_op(CogIrModule *module, CogIrTypeId type_id, TokenType op)
{
    const CogIrType *type = cog_ir_get_type(module, type_id);
    if (!type)
        return (CogIrOp)-1;
    if (type->kind == COG_IR_TYPE_FLOAT) {
        switch (op) {
            case TOK_PLUS_EQUAL: return COG_IR_OP_FADD;
            case TOK_MINUS_EQUAL: return COG_IR_OP_FSUB;
            case TOK_STAR_EQUAL: return COG_IR_OP_FMUL;
            case TOK_SLASH_EQUAL: return COG_IR_OP_FDIV;
            default: return (CogIrOp)-1;
        }
    }
    if (type->kind != COG_IR_TYPE_INTEGER)
        return (CogIrOp)-1;
    switch (op) {
        case TOK_PLUS_EQUAL: return COG_IR_OP_IADD_CHECKED;
        case TOK_MINUS_EQUAL: return COG_IR_OP_ISUB_CHECKED;
        case TOK_STAR_EQUAL: return COG_IR_OP_IMUL_CHECKED;
        case TOK_SLASH_EQUAL: return COG_IR_OP_IDIV_CHECKED;
        case TOK_PERCENT_EQUAL: return COG_IR_OP_IREM_CHECKED;
        case TOK_AND_EQUAL: return COG_IR_OP_BIT_AND;
        case TOK_OR_EQUAL: return COG_IR_OP_BIT_OR;
        case TOK_XOR_EQUAL: return COG_IR_OP_BIT_XOR;
        case TOK_SHIFT_LEFT_EQUAL: return COG_IR_OP_SHL_CHECKED_COUNT;
        case TOK_SHIFT_RIGHT_EQUAL:
            return type->as.integer.is_signed ? COG_IR_OP_SHR_SIGNED_CHECKED_COUNT : COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT;
        default: return (CogIrOp)-1;
    }
}

static int lower_compound_assignment_statement(ExecLowerState *state, Node *node)
{
    Node *target = node->as.compound_assign.target;
    LoweredPlace place;
    if (!lower_place(state, target, &place))
        return 0;

    CogIrValueId address = place.address;
    CogIrValueId lhs = emit_load(state, address, place.type, place.is_volatile, target->span);
    if (lhs == COG_IR_VALUE_INVALID)
        return 0;

    CogIrSlotId address_spill = COG_IR_SLOT_INVALID;
    CogIrSlotId lhs_spill = COG_IR_SLOT_INVALID;
    if (expression_may_create_cfg(state, node->as.compound_assign.value)) {
        address_spill = spill_value(state, address, target->span);
        lhs_spill = spill_value(state, lhs, target->span);
        if (address_spill == COG_IR_SLOT_INVALID || lhs_spill == COG_IR_SLOT_INVALID)
            return 0;
    }

    CogIrValueId rhs = lower_expression(state, node->as.compound_assign.value);
    if (rhs == COG_IR_VALUE_INVALID)
        return 0;
    if (address_spill != COG_IR_SLOT_INVALID) {
        address = reload_spill(state, address_spill, target->span);
        lhs = reload_spill(state, lhs_spill, target->span);
        if (address == COG_IR_VALUE_INVALID || lhs == COG_IR_VALUE_INVALID)
            return 0;
    }

    CogIrOp op = compound_assignment_op(state->lower->module, place.type, node->as.compound_assign.op);
    if ((int)op < 0) {
        lower_error(state->lower, node->span, "compound assignment is outside the current CogIR lowering slice");
        return 0;
    }
    CogIrInstruction instruction = {
        .op = op,
        .result_type = place.type,
        .span = node->span,
        .as.binary = { .lhs = lhs, .rhs = rhs },
    };
    CogIrValueId result = emit_instruction_value(state, instruction);
    return result != COG_IR_VALUE_INVALID && emit_store(state, address, result, place.is_volatile, node->span);
}

static CogIrConstId one_constant_for_type(CogIrLowerContext *ctx, CogIrTypeId type_id)
{
    const CogIrType *type = cog_ir_get_type(ctx->module, type_id);
    if (!type)
        return COG_IR_CONST_INVALID;
    if (type->kind == COG_IR_TYPE_INTEGER)
        return cog_ir_const_integer(ctx->module, type_id, 1);
    if (type->kind == COG_IR_TYPE_FLOAT && type->as.floating.bits == 32)
        return cog_ir_const_float32(ctx->module, type_id, UINT32_C(0x3f800000));
    if (type->kind == COG_IR_TYPE_FLOAT && type->as.floating.bits == 64)
        return cog_ir_const_float64(ctx->module, type_id, UINT64_C(0x3ff0000000000000));
    return COG_IR_CONST_INVALID;
}

static int lower_inc_dec_statement(ExecLowerState *state, Node *node)
{
    Node *target = node->as.inc_dec.target;
    LoweredPlace place;
    if (!lower_place(state, target, &place))
        return 0;
    CogIrValueId lhs = emit_load(state, place.address, place.type, place.is_volatile, target->span);
    CogIrConstId one_const = one_constant_for_type(state->lower, place.type);
    CogIrValueId one = one_const == COG_IR_CONST_INVALID ? COG_IR_VALUE_INVALID
        : emit_constant_value(state, one_const, node->span);
    const CogIrType *type = cog_ir_get_type(state->lower->module, place.type);
    CogIrOp op = (CogIrOp)-1;
    if (type && type->kind == COG_IR_TYPE_INTEGER)
        op = node->as.inc_dec.op == TOK_PLUS_PLUS ? COG_IR_OP_IADD_CHECKED : COG_IR_OP_ISUB_CHECKED;
    else if (type && type->kind == COG_IR_TYPE_FLOAT)
        op = node->as.inc_dec.op == TOK_PLUS_PLUS ? COG_IR_OP_FADD : COG_IR_OP_FSUB;
    if (lhs == COG_IR_VALUE_INVALID || one == COG_IR_VALUE_INVALID || (int)op < 0) {
        lower_error(state->lower, node->span, "increment/decrement is outside the current CogIR lowering slice");
        return 0;
    }
    CogIrInstruction instruction = {
        .op = op,
        .result_type = place.type,
        .span = node->span,
        .as.binary = { .lhs = lhs, .rhs = one },
    };
    CogIrValueId result = emit_instruction_value(state, instruction);
    return result != COG_IR_VALUE_INVALID && emit_store(state, place.address, result, place.is_volatile, node->span);
}

static int lower_global_initializer(ExecLowerState *state, Node *node);

static int bind_local_declaration(ExecLowerState *state, Node *node)
{
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemDeclInfo *info = semantic_get_decl_info(sem, node);
    CogIrLowerDeclBinding *binding = info ? get_decl_binding_mut(state->lower, info->id) : NULL;
    if (!info || !binding || binding->kind != COG_IR_LOWER_DECL_LOCAL_PENDING) {
        lower_error(state->lower, node->span, "local declaration has no pending CogIR binding");
        return 0;
    }
    CogIrSlotId slot = cog_ir_add_slot(
        state->lower->module,
        state->function,
        COG_IR_SLOT_SOURCE_LOCAL,
        COG_IR_PARAMETER_INDEX_INVALID,
        node->as.var_decl.name,
        node->span,
        binding->type
    );
    if (slot == COG_IR_SLOT_INVALID) {
        lower_error(state->lower, node->span, "failed to allocate CogIR local slot");
        return 0;
    }
    CogIrAbiTypeId declared_abi = info->abi_type
        ? cog_ir_lower_abi_type(state->lower, info->abi_type)
        : COG_IR_ABI_TYPE_INVALID;
    if (info->abi_type && declared_abi == COG_IR_ABI_TYPE_INVALID)
        return 0;
    if (!annotate_slot_abi(state, slot, declared_abi, node->span))
        return 0;

    binding->kind = COG_IR_LOWER_DECL_LOCAL;
    binding->as.local.function = state->function;
    binding->as.local.slot = slot;

    if (node->as.var_decl.initializer) {
        CogIrValueId address = COG_IR_VALUE_INVALID;
        CogIrValueId value = COG_IR_VALUE_INVALID;
        if (expression_may_create_cfg(state, node->as.var_decl.initializer)) {
            value = lower_expression(state, node->as.var_decl.initializer);
            if (value != COG_IR_VALUE_INVALID)
                address = emit_local_address(state, slot, binding->type, node->span);
        } else {
            address = emit_local_address(state, slot, binding->type, node->span);
            if (address != COG_IR_VALUE_INVALID)
                value = lower_expression(state, node->as.var_decl.initializer);
        }
        if (address == COG_IR_VALUE_INVALID || value == COG_IR_VALUE_INVALID)
            return 0;
        if (declared_abi == COG_IR_ABI_TYPE_INVALID) {
            const CogIrType *slot_type = cog_ir_get_type(state->lower->module, binding->type);
            const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
            const CogIrValue *ir_value = function ? cog_ir_get_value(function, value) : NULL;
            if (slot_type && slot_type->kind == COG_IR_TYPE_FUNCTION &&
                ir_value && ir_value->abi_type != COG_IR_ABI_TYPE_INVALID &&
                !annotate_slot_abi(state, slot, ir_value->abi_type, node->span))
                return 0;
        }
        if (!emit_store(state, address, value, 0, node->span))
            return 0;
    }
    return 1;
}

static int lower_variable_declaration(ExecLowerState *state, Node *node)
{
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemDeclInfo *info = semantic_get_decl_info(sem, node);
    CogIrLowerDeclBinding *binding = info ? get_decl_binding_mut(state->lower, info->id) : NULL;
    if (!binding) {
        lower_error(state->lower, node->span, "variable declaration has no CogIR binding");
        return 0;
    }
    if (binding->kind == COG_IR_LOWER_DECL_GLOBAL)
        return lower_global_initializer(state, node);
    return bind_local_declaration(state, node);
}

static int lower_statement(ExecLowerState *state, Node *node);

static int lower_block_statements(ExecLowerState *state, Node *block)
{
    if (!block || block->type != NODE_BLOCK) {
        lower_error(state->lower, block ? block->span : source_span_invalid(), "expected block during CogIR lowering");
        return 0;
    }
    for (int i = 0; i < block->as.block.statements.count; ++i) {
        if (!block_is_open(state))
            break;
        if (!lower_statement(state, block->as.block.statements.items[i]))
            return 0;
    }
    return 1;
}

static int lower_return_statement(ExecLowerState *state, Node *node)
{
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_RET;
    term.span = node->span;
    if (node->as.return_stmt.value) {
        CogIrValueId value = lower_expression(state, node->as.return_stmt.value);
        if (value == COG_IR_VALUE_INVALID)
            return 0;
        term.as.ret.has_value = 1;
        term.as.ret.value = value;
    }
    if (!cog_ir_set_terminator(state->lower->module, state->function, state->block, &term)) {
        lower_error(state->lower, node->span, "failed to emit CogIR return terminator");
        return 0;
    }
    return 1;
}

static int lower_if_statement(ExecLowerState *state, Node *node)
{
    CogIrValueId condition = lower_expression(state, node->as.if_stmt.condition);
    if (condition == COG_IR_VALUE_INVALID || !block_is_open(state))
        return 0;

    CogIrBlockId then_block = add_block(state, "if.then", node->as.if_stmt.then_branch->span);
    CogIrBlockId merge_block = add_block(state, "if.end", node->span);
    CogIrBlockId else_block = node->as.if_stmt.else_branch
        ? add_block(state, "if.else", node->as.if_stmt.else_branch->span)
        : merge_block;
    if (then_block == COG_IR_BLOCK_INVALID || merge_block == COG_IR_BLOCK_INVALID || else_block == COG_IR_BLOCK_INVALID)
        return 0;

    if (!set_cond_branch(state, condition, then_block, NULL, 0, else_block, NULL, 0, node->span))
        return 0;

    state->block = then_block;
    if (!lower_statement(state, node->as.if_stmt.then_branch))
        return 0;
    int then_falls_through = block_is_open(state);
    if (then_falls_through && !set_branch(state, merge_block, node->span))
        return 0;

    int else_falls_through = !node->as.if_stmt.else_branch;
    if (node->as.if_stmt.else_branch) {
        state->block = else_block;
        if (!lower_statement(state, node->as.if_stmt.else_branch))
            return 0;
        else_falls_through = block_is_open(state);
        if (else_falls_through && !set_branch(state, merge_block, node->span))
            return 0;
    }

    state->block = merge_block;
    if (!then_falls_through && !else_falls_through)
        return set_unreachable(state, node->span);
    return 1;
}

static int const_bool_value(CogIrLowerContext *ctx, Node *node, int *known, int *value)
{
    *known = 0;
    *value = 0;
    ConstValue constant;
    if (!semantic_get_constant_value((SemanticContext *)&ctx->frontend->sem, node, &constant))
        return 1;
    if (constant.kind != CONST_VALUE_BOOL)
        return 1;
    *known = 1;
    *value = !!constant.as.boolean;
    return 1;
}

static int lower_break_statement(ExecLowerState *state, Node *node)
{
    if (!state->loop) {
        lower_error(state->lower, node->span, "break has no active CogIR loop target");
        return 0;
    }
    state->loop->has_break = 1;
    return set_branch(state, state->loop->break_target, node->span);
}

static int lower_continue_statement(ExecLowerState *state, Node *node)
{
    if (!state->loop) {
        lower_error(state->lower, node->span, "continue has no active CogIR loop target");
        return 0;
    }
    return set_branch(state, state->loop->continue_target, node->span);
}

static int lower_while_statement(ExecLowerState *state, Node *node)
{
    CogIrBlockId condition_block = add_block(state, "while.cond", node->as.while_stmt.condition->span);
    CogIrBlockId body_block = add_block(state, "while.body", node->as.while_stmt.body->span);
    CogIrBlockId exit_block = add_block(state, "while.end", node->span);
    if (condition_block == COG_IR_BLOCK_INVALID || body_block == COG_IR_BLOCK_INVALID || exit_block == COG_IR_BLOCK_INVALID)
        return 0;
    if (!set_branch(state, condition_block, node->span))
        return 0;

    state->block = condition_block;
    CogIrValueId condition = lower_expression(state, node->as.while_stmt.condition);
    if (condition == COG_IR_VALUE_INVALID || !block_is_open(state))
        return 0;
    int known = 0, value = 0;
    const_bool_value(state->lower, node->as.while_stmt.condition, &known, &value);
    if (known) {
        if (!set_branch(state, value ? body_block : exit_block, node->span))
            return 0;
    } else if (!set_cond_branch(state, condition, body_block, NULL, 0, exit_block, NULL, 0, node->span)) {
        return 0;
    }

    ExecLoopContext loop = {
        .break_target = exit_block,
        .continue_target = condition_block,
        .parent = state->loop,
    };
    state->loop = &loop;
    state->block = body_block;
    if (!lower_statement(state, node->as.while_stmt.body))
        return 0;
    if (block_is_open(state) && !set_branch(state, condition_block, node->span))
        return 0;
    state->loop = loop.parent;

    state->block = exit_block;
    if (known && value && !loop.has_break)
        return set_unreachable(state, node->span);
    return 1;
}

static int lower_statement_expression(ExecLowerState *state, Node *node)
{
    if (!node)
        return 1;
    switch (node->type) {
        case NODE_ASSIGN:
            return lower_assignment_statement(state, node);
        case NODE_COMPOUND_ASSIGN:
            return lower_compound_assignment_statement(state, node);
        case NODE_INC_DEC:
            return lower_inc_dec_statement(state, node);
        default: {
            SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
            Type *type = semantic_get_effective_expr_type(sem, node);
            if (type && (type->kind == TYPE_UNTYPED_INT || type->kind == TYPE_UNTYPED_FLOAT)) {
                ConstValue ignored;
                if (semantic_get_constant_value(sem, node, &ignored))
                    return 1; /* discarded adaptable compile-time value has no runtime effect */
            }
            CogIrValueId value = lower_expression(state, node);
            return (type && type->kind == TYPE_VOID) || value != COG_IR_VALUE_INVALID;
        }
    }
}

static int lower_for_statement(ExecLowerState *state, Node *node)
{
    CogIrBlockId condition_block = add_block(state, "for.cond", node->span);
    CogIrBlockId body_block = add_block(state, "for.body", node->as.for_stmt.body->span);
    CogIrBlockId post_block = add_block(state, "for.post", node->span);
    CogIrBlockId exit_block = add_block(state, "for.end", node->span);
    if (condition_block == COG_IR_BLOCK_INVALID || body_block == COG_IR_BLOCK_INVALID ||
        post_block == COG_IR_BLOCK_INVALID || exit_block == COG_IR_BLOCK_INVALID)
        return 0;
    if (!set_branch(state, condition_block, node->span))
        return 0;

    int known = 1, value = 1;
    state->block = condition_block;
    if (node->as.for_stmt.condition) {
        CogIrValueId condition = lower_expression(state, node->as.for_stmt.condition);
        if (condition == COG_IR_VALUE_INVALID || !block_is_open(state))
            return 0;
        const_bool_value(state->lower, node->as.for_stmt.condition, &known, &value);
        if (known) {
            if (!set_branch(state, value ? body_block : exit_block, node->span))
                return 0;
        } else if (!set_cond_branch(state, condition, body_block, NULL, 0, exit_block, NULL, 0, node->span)) {
            return 0;
        }
    } else if (!set_branch(state, body_block, node->span)) {
        return 0;
    }

    ExecLoopContext loop = {
        .break_target = exit_block,
        .continue_target = post_block,
        .parent = state->loop,
    };
    state->loop = &loop;
    state->block = body_block;
    if (!lower_statement(state, node->as.for_stmt.body))
        return 0;
    if (block_is_open(state) && !set_branch(state, post_block, node->span))
        return 0;

    state->block = post_block;
    if (node->as.for_stmt.post && !lower_statement_expression(state, node->as.for_stmt.post))
        return 0;
    if (block_is_open(state) && !set_branch(state, condition_block, node->span))
        return 0;
    state->loop = loop.parent;

    state->block = exit_block;
    if (known && value && !loop.has_break)
        return set_unreachable(state, node->span);
    return 1;
}

static int switch_is_exhaustive(CogIrLowerContext *ctx, Node *node, const CogIrConstId *keys, size_t key_count, int has_default)
{
    if (has_default)
        return 1;
    Type *type = node->as.switch_stmt.resolved_type;
    if (!type || key_count == 0)
        return 0;
    if (type->kind == TYPE_BOOL) {
        int has_false = 0, has_true = 0;
        for (size_t i = 0; i < key_count; ++i) {
            const CogIrConstant *key = cog_ir_get_constant(ctx->module, keys[i]);
            if (key && key->kind == COG_IR_CONST_BOOL) {
                if (key->as.boolean) has_true = 1; else has_false = 1;
            }
        }
        return has_true && has_false;
    }
    if (type->kind != TYPE_ENUM || type->enum_member_count <= 0)
        return 0;
    unsigned width = integer_type_width(type);
    if (!width)
        return 0;
    for (int member = 0; member < type->enum_member_count; ++member) {
        uint64_t bits = integer_value_bits(type->enum_members[member].value, width);
        int found = 0;
        for (size_t i = 0; i < key_count; ++i) {
            const CogIrConstant *key = cog_ir_get_constant(ctx->module, keys[i]);
            if (key && key->kind == COG_IR_CONST_INTEGER && key->as.integer_bits == bits) {
                found = 1;
                break;
            }
        }
        if (!found)
            return 0;
    }
    return 1;
}

static int lower_switch_statement(ExecLowerState *state, Node *node)
{
    CogIrValueId selector = lower_expression(state, node->as.switch_stmt.expression);
    if (selector == COG_IR_VALUE_INVALID || !block_is_open(state))
        return 0;

    size_t case_node_count = (size_t)node->as.switch_stmt.cases.count;
    size_t value_case_count = 0;
    int has_default = 0;
    for (size_t i = 0; i < case_node_count; ++i) {
        Node *case_node = node->as.switch_stmt.cases.items[i];
        if (case_node->as.switch_case.is_default) has_default = 1; else value_case_count++;
    }

    CogIrBlockId merge_block = add_block(state, "switch.end", node->span);
    CogIrBlockId *case_blocks = case_node_count ? calloc(case_node_count, sizeof(*case_blocks)) : NULL;
    CogIrSwitchCase *cases = value_case_count ? calloc(value_case_count, sizeof(*cases)) : NULL;
    CogIrConstId *keys = value_case_count ? calloc(value_case_count, sizeof(*keys)) : NULL;
    if (merge_block == COG_IR_BLOCK_INVALID || (case_node_count && !case_blocks) || (value_case_count && (!cases || !keys))) {
        free(case_blocks); free(cases); free(keys);
        lower_error(state->lower, node->span, "out of memory while lowering switch CFG");
        return 0;
    }

    CogIrBlockId default_block = COG_IR_BLOCK_INVALID;
    size_t value_index = 0;
    for (size_t i = 0; i < case_node_count; ++i) {
        Node *case_node = node->as.switch_stmt.cases.items[i];
        case_blocks[i] = add_block(state, case_node->as.switch_case.is_default ? "switch.default" : "switch.case", case_node->span);
        if (case_blocks[i] == COG_IR_BLOCK_INVALID) goto fail;
        if (case_node->as.switch_case.is_default) {
            default_block = case_blocks[i];
        } else {
            ConstValue value;
            if (!semantic_get_constant_value((SemanticContext *)&state->lower->frontend->sem, case_node->as.switch_case.value, &value)) {
                lower_error(state->lower, case_node->span, "switch case lacks checked constant metadata during CogIR lowering");
                goto fail;
            }
            CogIrConstId key = cog_ir_lower_const_value(state->lower, &value);
            if (key == COG_IR_CONST_INVALID) goto fail;
            keys[value_index] = key;
            cases[value_index].key = key;
            cases[value_index].edge.target = case_blocks[i];
            value_index++;
        }
    }

    int exhaustive = switch_is_exhaustive(state->lower, node, keys, value_case_count, has_default);
    CogIrBlockId unmatched_block = COG_IR_BLOCK_INVALID;
    if (!has_default && exhaustive) {
        unmatched_block = add_block(state, "switch.unmatched", node->span);
        if (unmatched_block == COG_IR_BLOCK_INVALID) goto fail;
        default_block = unmatched_block;
    } else if (!has_default) {
        default_block = merge_block;
    }

    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_SWITCH;
    term.span = node->span;
    term.as.switch_term.value = selector;
    term.as.switch_term.cases = cases;
    term.as.switch_term.case_count = value_case_count;
    term.as.switch_term.default_edge.target = default_block;
    if (!cog_ir_set_terminator(state->lower->module, state->function, state->block, &term)) {
        lower_error(state->lower, node->span, "failed to emit CogIR switch terminator");
        goto fail;
    }

    if (unmatched_block != COG_IR_BLOCK_INVALID) {
        state->block = unmatched_block;
        if (!set_unreachable(state, node->span)) goto fail;
    }

    int has_continuing_path = !has_default && !exhaustive;
    for (size_t i = 0; i < case_node_count; ++i) {
        Node *case_node = node->as.switch_stmt.cases.items[i];
        state->block = case_blocks[i];
        if (!lower_statement(state, case_node->as.switch_case.body)) goto fail;
        if (block_is_open(state)) {
            has_continuing_path = 1;
            if (!set_branch(state, merge_block, case_node->span)) goto fail;
        }
    }

    state->block = merge_block;
    if (!has_continuing_path && !set_unreachable(state, node->span)) goto fail;
    free(case_blocks); free(cases); free(keys);
    return 1;

fail:
    free(case_blocks); free(cases); free(keys);
    return 0;
}

static int lower_statement(ExecLowerState *state, Node *node)
{
    if (!node)
        return 1;
    switch (node->type) {
        case NODE_VAR_DECL:
            return lower_variable_declaration(state, node);
        case NODE_VAR_DECL_GROUP:
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++) {
                if (!lower_variable_declaration(state, node->as.var_decl_group.declarations.items[i]))
                    return 0;
            }
            return 1;
        case NODE_ASSIGN:
            return lower_assignment_statement(state, node);
        case NODE_COMPOUND_ASSIGN:
            return lower_compound_assignment_statement(state, node);
        case NODE_INC_DEC:
            return lower_inc_dec_statement(state, node);
        case NODE_EXPR_STMT:
            return lower_statement_expression(state, node->as.expr_stmt.expr);
        case NODE_RETURN:
            return lower_return_statement(state, node);
        case NODE_BLOCK:
            return lower_block_statements(state, node);
        case NODE_IF:
            return lower_if_statement(state, node);
        case NODE_WHILE:
            return lower_while_statement(state, node);
        case NODE_FOR:
            return lower_for_statement(state, node);
        case NODE_BREAK:
            return lower_break_statement(state, node);
        case NODE_CONTINUE:
            return lower_continue_statement(state, node);
        case NODE_SWITCH:
            return lower_switch_statement(state, node);
        case NODE_CONST_DECL:
        case NODE_FUNC_DECL:
        case NODE_STRUCT_DECL:
        case NODE_ENUM_DECL:
            return 1;
        default:
            lower_error(state->lower, node->span, "statement is outside the current CogIR executable-lowering slice");
            return 0;
    }
}

static int bind_function_parameters(ExecLowerState *state, Node *function_node)
{
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    if (!function || (size_t)function_node->as.func_decl.params.count != function->parameter_count) {
        lower_error(state->lower, function_node->span, "function parameter metadata mismatch during CogIR lowering");
        return 0;
    }

    for (int i = 0; i < function_node->as.func_decl.params.count; ++i) {
        Node *param = function_node->as.func_decl.params.items[i];
        SemDeclInfo *info = semantic_get_decl_info(sem, param);
        CogIrLowerDeclBinding *binding = info ? get_decl_binding_mut(state->lower, info->id) : NULL;
        if (!info || !binding || binding->kind != COG_IR_LOWER_DECL_PARAMETER_PENDING) {
            lower_error(state->lower, param->span, "function parameter has no pending CogIR binding");
            return 0;
        }
        CogIrSlotId slot = cog_ir_add_slot(
            state->lower->module,
            state->function,
            COG_IR_SLOT_SOURCE_PARAMETER,
            (size_t)i,
            param->as.param_decl.name,
            param->span,
            binding->type
        );
        if (slot == COG_IR_SLOT_INVALID) {
            lower_error(state->lower, param->span, "failed to allocate CogIR parameter slot");
            return 0;
        }
        CogIrValueId incoming = function->parameters[i];
        const CogIrType *parameter_type = cog_ir_get_type(
            state->lower->module, binding->type);
        int needs_value_abi = parameter_type &&
            parameter_type->kind == COG_IR_TYPE_FUNCTION &&
            parameter_type->as.function.abi == COG_IR_ABI_C;
        CogIrAbiTypeId parameter_abi = needs_value_abi && info->abi_type
            ? cog_ir_lower_abi_type(state->lower, info->abi_type)
            : COG_IR_ABI_TYPE_INVALID;
        if (needs_value_abi && (!info->abi_type || parameter_abi == COG_IR_ABI_TYPE_INVALID)) {
            lower_error(state->lower, param->span,
                        "missing normalized cfn ABI metadata during CogIR lowering");
            return 0;
        }
        if (!annotate_value_abi(state, incoming, parameter_abi, param->span) ||
            !annotate_slot_abi(state, slot, parameter_abi, param->span))
            return 0;

        binding->kind = COG_IR_LOWER_DECL_PARAMETER;
        binding->as.parameter.function = state->function;
        binding->as.parameter.slot = slot;
        binding->as.parameter.incoming_value = incoming;
        binding->as.parameter.index = (uint32_t)i;

        CogIrValueId address = emit_local_address(state, slot, binding->type, param->span);
        if (address == COG_IR_VALUE_INVALID || !emit_store(state, address, incoming, 0, param->span))
            return 0;
    }
    return 1;
}

static int finish_void_function_if_needed(ExecLowerState *state, SourceSpan span)
{
    const CogIrFunction *function = cog_ir_get_function(state->lower->module, state->function);
    const CogIrBlock *block = function ? cog_ir_get_block(function, state->block) : NULL;
    const CogIrType *function_type = function ? cog_ir_get_type(state->lower->module, function->type) : NULL;
    const CogIrType *result_type = function_type ? cog_ir_get_type(state->lower->module, function_type->as.function.result_type) : NULL;
    if (!function || !block || !function_type || !result_type)
        return 0;
    if (block->terminator.kind != COG_IR_TERMINATOR_NONE)
        return 1;
    if (result_type->kind != COG_IR_TYPE_VOID) {
        lower_error(state->lower, span, "non-void function reached end without a lowered return terminator");
        return 0;
    }
    CogIrTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = COG_IR_TERMINATOR_RET;
    term.span = span;
    return cog_ir_set_terminator(state->lower->module, state->function, state->block, &term);
}

static int lower_function_body(CogIrLowerContext *ctx, SemDeclInfo *info)
{
    Node *node = info->node;
    CogIrLowerDeclBinding *binding = get_decl_binding_mut(ctx, info->id);
    if (!binding || binding->kind != COG_IR_LOWER_DECL_FUNCTION || !node->as.func_decl.body)
        return 1;
    if (!cog_ir_begin_function_definition(ctx->module, binding->as.function)) {
        lower_error(ctx, node->span, "failed to begin CogIR function definition");
        return 0;
    }
    CogIrBlockId entry = cog_ir_add_block(
        ctx->module,
        binding->as.function,
        string_view_from_cstr("entry"),
        node->as.func_decl.body->span
    );
    if (entry == COG_IR_BLOCK_INVALID) {
        lower_error(ctx, node->span, "failed to create CogIR entry block");
        return 0;
    }
    ExecLowerState state = { .lower = ctx, .function = binding->as.function, .block = entry };
    if (!bind_function_parameters(&state, node) || !lower_block_statements(&state, node->as.func_decl.body))
        return 0;
    return finish_void_function_if_needed(&state, node->as.func_decl.body->span);
}

static int node_is_runtime_module_item(Node *node)
{
    if (!node)
        return 0;
    switch (node->type) {
        case NODE_VAR_DECL:
            return node->as.var_decl.initializer != NULL;
        case NODE_VAR_DECL_GROUP:
            for (int i = 0; i < node->as.var_decl_group.declarations.count; i++) {
                Node *decl = node->as.var_decl_group.declarations.items[i];
                if (decl->as.var_decl.initializer)
                    return 1;
            }
            return 0;
        case NODE_ASSIGN:
        case NODE_COMPOUND_ASSIGN:
        case NODE_INC_DEC:
        case NODE_EXPR_STMT:
        case NODE_BLOCK:
        case NODE_IF:
        case NODE_WHILE:
        case NODE_FOR:
        case NODE_SWITCH:
            return 1;
        default:
            return 0;
    }
}

static int lower_global_initializer(ExecLowerState *state, Node *node)
{
    SemanticContext *sem = (SemanticContext *)&state->lower->frontend->sem;
    SemDeclInfo *info = semantic_get_decl_info(sem, node);
    CogIrLowerDeclBinding *binding = info ? get_decl_binding_mut(state->lower, info->id) : NULL;
    if (!info || !binding || binding->kind != COG_IR_LOWER_DECL_GLOBAL) {
        lower_error(state->lower, node->span, "global initializer has no CogIR global binding");
        return 0;
    }
    if (!node->as.var_decl.initializer)
        return 1;
    const CogIrGlobal *global = cog_ir_get_global(state->lower->module, binding->as.global);
    CogIrValueId address = COG_IR_VALUE_INVALID;
    CogIrValueId value = COG_IR_VALUE_INVALID;
    if (expression_may_create_cfg(state, node->as.var_decl.initializer)) {
        value = lower_expression(state, node->as.var_decl.initializer);
        if (value != COG_IR_VALUE_INVALID)
            address = emit_global_address(state, binding->as.global, binding->type,
                                          global ? global->is_readonly : 0, node->span);
    } else {
        address = emit_global_address(state, binding->as.global, binding->type,
                                      global ? global->is_readonly : 0, node->span);
        if (address != COG_IR_VALUE_INVALID)
            value = lower_expression(state, node->as.var_decl.initializer);
    }
    if (address == COG_IR_VALUE_INVALID || value == COG_IR_VALUE_INVALID)
        return 0;
    return emit_store(state, address, value, 0, node->span);
}

static int lower_module_item(ExecLowerState *state, Node *node)
{
    if (!node)
        return 1;
    if (node->type == NODE_VAR_DECL)
        return lower_global_initializer(state, node);
    return lower_statement(state, node);
}

static int lower_module_init(CogIrLowerContext *ctx)
{
    Node *program = ctx->frontend->program;
    if (!program || program->type != NODE_PROGRAM) {
        lower_error(ctx, source_span_invalid(), "frontend program root is not a program node");
        return 0;
    }
    int needs_init = 0;
    for (int i = 0; i < program->as.program.statements.count; ++i)
        if (node_is_runtime_module_item(program->as.program.statements.items[i])) {
            needs_init = 1;
            break;
        }
    if (!needs_init)
        return 1;

    CogIrTypeId void_type = cog_ir_type_void(ctx->module);
    CogIrTypeId fn_type = cog_ir_type_function(
        ctx->module,
        void_type,
        NULL,
        0,
        COG_IR_ABI_COGLET,
        COG_IR_CALL_DEFAULT,
        0
    );
    CogIrFunctionId function = cog_ir_add_function(
        ctx->module,
        string_view_from_cstr("__coglet_module_init"),
        program->span,
        fn_type,
        COG_IR_FUNCTION_DEFINITION,
        COG_IR_LINKAGE_INTERNAL,
        1,
        NULL
    );
    if (function == COG_IR_FUNCTION_INVALID || !cog_ir_set_init_function(ctx->module, function)) {
        lower_error(ctx, program->span, "failed to create CogIR module initializer");
        return 0;
    }
    CogIrBlockId entry = cog_ir_add_block(ctx->module, function, string_view_from_cstr("entry"), program->span);
    if (entry == COG_IR_BLOCK_INVALID) {
        lower_error(ctx, program->span, "failed to create module initializer entry block");
        return 0;
    }
    ExecLowerState state = { .lower = ctx, .function = function, .block = entry };
    for (int i = 0; i < program->as.program.statements.count; ++i) {
        Node *item = program->as.program.statements.items[i];
        if (!node_is_runtime_module_item(item))
            continue;
        if (!lower_module_item(&state, item))
            return 0;
    }
    return finish_void_function_if_needed(&state, program->span);
}

int cog_ir_lower_executable(CogIrLowerContext *ctx)
{
    if (!ctx || ctx->failed || !ctx->metadata_prepared || ctx->executable_lowered ||
        !ctx->frontend || !ctx->module || ctx->module->is_frozen || ctx->module->sources.count == 0)
        return 0;
    SemanticContext *sem = (SemanticContext *)&ctx->frontend->sem;
    for (SemDeclId id = 0; id < ctx->decl_binding_count; ++id) {
        SemDeclInfo *info = semantic_get_decl_info_by_id(sem, id);
        if (!info || !info->node)
            return 0;
        if (info->is_generic_template)
            continue;
        if (info->node->type == NODE_FUNC_DECL && info->node->as.func_decl.body) {
            if (!lower_function_body(ctx, info))
                return 0;
        }
    }
    if (!lower_module_init(ctx))
        return 0;
    if (!ctx->failed)
        ctx->executable_lowered = 1;
    return !ctx->failed;
}

int cog_ir_lower_program(CogIrLowerContext *ctx)
{
    return cog_ir_lower_prepare_metadata(ctx) && cog_ir_lower_executable(ctx);
}
