#include "cog_ir_lower.h"

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
        case TYPE_I8: result = cog_ir_type_integer(ctx->module, 8, 1); break;
        case TYPE_I16: result = cog_ir_type_integer(ctx->module, 16, 1); break;
        case TYPE_I32: result = cog_ir_type_integer(ctx->module, 32, 1); break;
        case TYPE_I64: result = cog_ir_type_integer(ctx->module, 64, 1); break;
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
        case TYPE_I8: case TYPE_U8: return 8;
        case TYPE_I16: case TYPE_U16: return 16;
        case TYPE_I32: case TYPE_U32: return 32;
        case TYPE_I64: case TYPE_U64: return 64;
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
        if (!info || !info->node || !info->type) {
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
        if (!info || !info->node || !info->type) return 0;
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
                    CogIrGlobalId global = cog_ir_add_global(ctx->module, info->node->as.var_decl.name, info->node->span,
                        type, COG_IR_LINKAGE_INTERNAL, 0, 0, zero);
                    if (zero == COG_IR_CONST_INVALID || global == COG_IR_GLOBAL_INVALID) { lower_error(ctx, info->node->span, "failed to lower global storage metadata to CogIR"); return 0; }
                    binding->kind = COG_IR_LOWER_DECL_GLOBAL; binding->as.global = global;
                } else binding->kind = COG_IR_LOWER_DECL_LOCAL_PENDING;
                break;
            }
            case NODE_CONST_DECL: {
                ConstValue value; if (!semantic_get_constant_value(sem, info->node, &value)) { lower_error(ctx, info->node->span, "missing checked constant declaration value during CogIR lowering"); return 0; }
                CogIrConstId constant = cog_ir_lower_const_value(ctx, &value); if (constant == COG_IR_CONST_INVALID) return 0;
                binding->kind = COG_IR_LOWER_DECL_CONSTANT; binding->type = cog_ir_lower_type(ctx, value.type); binding->as.constant = constant; break;
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
    if (!ctx || !ctx->frontend || !ctx->module || ctx->failed || ctx->module->is_frozen || ctx->module->sources.count != 0)
        return 0;
    if (!cog_ir_module_copy_sources(ctx->module, &ctx->frontend->sources)) {
        lower_error(ctx, source_span_invalid(), "failed to copy frontend source provenance into CogIR"); return 0;
    }
    if (!predeclare_nominal_types(ctx) || !define_nominal_types(ctx) || !lower_remaining_declarations(ctx)) return 0;
    for (size_t i = 0; i < ctx->decl_binding_count; ++i) {
        if (ctx->decl_bindings[i].kind == COG_IR_LOWER_DECL_NONE || ctx->decl_bindings[i].type == COG_IR_TYPE_INVALID) {
            lower_error(ctx, source_span_invalid(), "CogIR declaration map is incomplete after metadata lowering"); return 0;
        }
    }
    return !ctx->failed;
}
