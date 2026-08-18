#include "ir/cog_ir.h"

#include <assert.h>
#include <string.h>

#define COG_IR_ARENA_BLOCK_SIZE (64u * 1024u)

static int module_mutable(const CogIrModule *module)
{
    return module && module->arena && !module->is_frozen;
}

static void *grow_array(
    Arena *arena,
    void *old_data,
    size_t old_count,
    size_t element_size,
    size_t *capacity
) {
    if (old_count < *capacity)
        return old_data;

    size_t new_capacity = *capacity ? *capacity * 2u : 8u;
    void *new_data = arena_zalloc(arena, new_capacity * element_size);

    if (old_data && old_count)
        memcpy(new_data, old_data, old_count * element_size);

    *capacity = new_capacity;
    return new_data;
}

static StringView copy_string_view(CogIrModule *module, StringView value)
{
    if (!value.data || value.length == 0)
        return string_view_empty();

    return string_view(
        arena_strdup_len(module->arena, value.data, value.length),
        value.length
    );
}

static CogIrTypeId *copy_type_ids(
    CogIrModule *module,
    const CogIrTypeId *ids,
    size_t count
) {
    if (!count)
        return NULL;

    CogIrTypeId *copy = arena_alloc(module->arena, count * sizeof(*copy));
    memcpy(copy, ids, count * sizeof(*copy));
    return copy;
}

static CogIrAbiTypeId *copy_abi_type_ids(
    CogIrModule *module,
    const CogIrAbiTypeId *ids,
    size_t count
) {
    if (!count)
        return NULL;

    CogIrAbiTypeId *copy = arena_alloc(module->arena, count * sizeof(*copy));
    memcpy(copy, ids, count * sizeof(*copy));
    return copy;
}

static CogIrConstId *copy_const_ids(
    CogIrModule *module,
    const CogIrConstId *ids,
    size_t count
) {
    if (!count)
        return NULL;

    CogIrConstId *copy = arena_alloc(module->arena, count * sizeof(*copy));
    memcpy(copy, ids, count * sizeof(*copy));
    return copy;
}

static CogIrValueId *copy_value_ids(
    CogIrModule *module,
    const CogIrValueId *ids,
    size_t count
) {
    if (!count)
        return NULL;

    CogIrValueId *copy = arena_alloc(module->arena, count * sizeof(*copy));
    memcpy(copy, ids, count * sizeof(*copy));
    return copy;
}

static int same_type_ids(const CogIrTypeId *a, const CogIrTypeId *b, size_t count)
{
    if (count == 0)
        return 1;
    return memcmp(a, b, count * sizeof(*a)) == 0;
}

static int same_abi_type_ids(const CogIrAbiTypeId *a, const CogIrAbiTypeId *b, size_t count)
{
    if (count == 0)
        return 1;
    return memcmp(a, b, count * sizeof(*a)) == 0;
}

void cog_ir_module_init(CogIrModule *module, const TargetInfo *target)
{
    assert(module);
    assert(target);

    memset(module, 0, sizeof(*module));
    module->arena = arena_create(COG_IR_ARENA_BLOCK_SIZE);
    module->target = *target;
    source_manager_init(&module->sources, module->arena);
    module->entry_function = COG_IR_FUNCTION_INVALID;
    module->init_function = COG_IR_FUNCTION_INVALID;
}

void cog_ir_module_destroy(CogIrModule *module)
{
    if (!module)
        return;

    Arena *arena = module->arena;
    memset(module, 0, sizeof(*module));
    module->entry_function = COG_IR_FUNCTION_INVALID;
    module->init_function = COG_IR_FUNCTION_INVALID;

    if (arena)
        arena_destroy(arena);
}

int cog_ir_module_add_source(
    CogIrModule *module,
    const char *filename,
    const char *source,
    SourceFileId *out_id
) {
    if (!module_mutable(module) || !filename || !source)
        return 0;

    char *source_copy = arena_strdup_len(module->arena, source, strlen(source));
    SourceFileId id = source_manager_add(&module->sources, filename, source_copy);

    if (out_id)
        *out_id = id;

    return 1;
}

int cog_ir_module_copy_sources(CogIrModule *module, const SourceManager *sources)
{
    if (!module_mutable(module) || !sources)
        return 0;

    if (module->sources.count != 0)
        return 0;

    for (const SourceFile *file = sources->first; file; file = file->next) {
        SourceFileId copied = SOURCE_FILE_ID_INVALID;
        if (!cog_ir_module_add_source(module, file->filename, file->source, &copied))
            return 0;
        if (copied != file->id)
            return 0;
    }

    return 1;
}

static int ir_string_view_starts_with_cstr(StringView value, const char *prefix)
{
    size_t length = strlen(prefix);
    return value.length >= length && memcmp(value.data, prefix, length) == 0;
}

static unsigned derive_runtime_requirements(const CogIrModule *module)
{
    unsigned requirements = COG_IR_RUNTIME_REQUIREMENT_NONE;
    if (!module)
        return requirements;

    for (size_t i = 0; i < module->function_count; ++i) {
        const CogIrFunction *function = &module->functions[i];
        if (function->linkage != COG_IR_LINKAGE_EXTERNAL ||
            function->abi.abi != COG_IR_ABI_C ||
            !ir_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_")) {
            continue;
        }

        if (ir_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_io_"))
            requirements |= COG_IR_RUNTIME_REQUIREMENT_IO;
        else if (ir_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_math_"))
            requirements |= COG_IR_RUNTIME_REQUIREMENT_MATH;
        else if (ir_string_view_starts_with_cstr(function->abi.external_symbol, "coglet_rt_mem_"))
            requirements |= COG_IR_RUNTIME_REQUIREMENT_MEMORY;
        else
            requirements |= COG_IR_RUNTIME_REQUIREMENT_UNKNOWN;
    }
    return requirements;
}

void cog_ir_module_freeze(CogIrModule *module)
{
    if (!module || module->is_frozen)
        return;

    module->runtime_requirements = derive_runtime_requirements(module);
    module->is_frozen = 1;
}

int cog_ir_module_is_frozen(const CogIrModule *module)
{
    return module && module->is_frozen;
}

unsigned cog_ir_module_runtime_requirements(const CogIrModule *module)
{
    return module && module->is_frozen
        ? module->runtime_requirements
        : COG_IR_RUNTIME_REQUIREMENT_NONE;
}

const CogIrType *cog_ir_get_type(const CogIrModule *module, CogIrTypeId id)
{
    if (!module || id == COG_IR_TYPE_INVALID || (size_t)id >= module->type_count)
        return NULL;
    return &module->types[id];
}

const CogIrAbiType *cog_ir_get_abi_type(const CogIrModule *module, CogIrAbiTypeId id)
{
    if (!module || id == COG_IR_ABI_TYPE_INVALID || (size_t)id >= module->abi_type_count)
        return NULL;
    return &module->abi_types[id];
}

const CogIrConstant *cog_ir_get_constant(const CogIrModule *module, CogIrConstId id)
{
    if (!module || id == COG_IR_CONST_INVALID || (size_t)id >= module->constant_count)
        return NULL;
    return &module->constants[id];
}

const CogIrGlobal *cog_ir_get_global(const CogIrModule *module, CogIrGlobalId id)
{
    if (!module || id == COG_IR_GLOBAL_INVALID || (size_t)id >= module->global_count)
        return NULL;
    return &module->globals[id];
}

const CogIrFunction *cog_ir_get_function(const CogIrModule *module, CogIrFunctionId id)
{
    if (!module || id == COG_IR_FUNCTION_INVALID || (size_t)id >= module->function_count)
        return NULL;
    return &module->functions[id];
}

const CogIrBlock *cog_ir_get_block(const CogIrFunction *function, CogIrBlockId id)
{
    if (!function || id == COG_IR_BLOCK_INVALID || (size_t)id >= function->block_count)
        return NULL;
    return &function->blocks[id];
}

const CogIrValue *cog_ir_get_value(const CogIrFunction *function, CogIrValueId id)
{
    if (!function || id == COG_IR_VALUE_INVALID || (size_t)id >= function->value_count)
        return NULL;
    return &function->values[id];
}

const CogIrSlot *cog_ir_get_slot(const CogIrFunction *function, CogIrSlotId id)
{
    if (!function || id == COG_IR_SLOT_INVALID || (size_t)id >= function->slot_count)
        return NULL;
    return &function->slots[id];
}

static CogIrTypeId append_type(CogIrModule *module, const CogIrType *input)
{
    if (!module_mutable(module) || module->type_count >= COG_IR_ID_INVALID)
        return COG_IR_TYPE_INVALID;

    module->types = grow_array(
        module->arena,
        module->types,
        module->type_count,
        sizeof(*module->types),
        &module->type_capacity
    );

    CogIrTypeId id = (CogIrTypeId)module->type_count;
    CogIrType *type = &module->types[module->type_count++];
    *type = *input;
    type->id = id;
    return id;
}

CogIrTypeId cog_ir_type_void(CogIrModule *module)
{
    if (!module_mutable(module))
        return COG_IR_TYPE_INVALID;

    for (size_t i = 0; i < module->type_count; ++i)
        if (module->types[i].kind == COG_IR_TYPE_VOID)
            return (CogIrTypeId)i;

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_VOID;
    type.span = source_span_invalid();
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_bool(CogIrModule *module)
{
    if (!module_mutable(module))
        return COG_IR_TYPE_INVALID;

    for (size_t i = 0; i < module->type_count; ++i)
        if (module->types[i].kind == COG_IR_TYPE_BOOL)
            return (CogIrTypeId)i;

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_BOOL;
    type.span = source_span_invalid();
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_integer(CogIrModule *module, unsigned bits, int is_signed)
{
    if (!module_mutable(module) || (bits != 8 && bits != 16 && bits != 32 && bits != 64))
        return COG_IR_TYPE_INVALID;

    is_signed = !!is_signed;
    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_INTEGER &&
            type->as.integer.bits == bits &&
            type->as.integer.is_signed == is_signed)
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_INTEGER;
    type.span = source_span_invalid();
    type.as.integer.bits = bits;
    type.as.integer.is_signed = is_signed;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_float(CogIrModule *module, unsigned bits)
{
    if (!module_mutable(module) || (bits != 32 && bits != 64))
        return COG_IR_TYPE_INVALID;

    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_FLOAT && type->as.floating.bits == bits)
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_FLOAT;
    type.span = source_span_invalid();
    type.as.floating.bits = bits;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_pointer(
    CogIrModule *module,
    CogIrTypeId pointee,
    int is_readonly,
    int is_volatile
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, pointee))
        return COG_IR_TYPE_INVALID;

    is_readonly = !!is_readonly;
    is_volatile = !!is_volatile;

    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_POINTER &&
            type->as.pointer.pointee == pointee &&
            type->as.pointer.is_readonly == is_readonly &&
            type->as.pointer.is_volatile == is_volatile)
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_POINTER;
    type.span = source_span_invalid();
    type.as.pointer.pointee = pointee;
    type.as.pointer.is_readonly = is_readonly;
    type.as.pointer.is_volatile = is_volatile;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_opaque_pointer(CogIrModule *module, int is_readonly, int is_volatile)
{
    if (!module_mutable(module))
        return COG_IR_TYPE_INVALID;

    is_readonly = !!is_readonly;
    is_volatile = !!is_volatile;

    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_OPAQUE_POINTER &&
            type->as.opaque_pointer.is_readonly == is_readonly &&
            type->as.opaque_pointer.is_volatile == is_volatile)
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_OPAQUE_POINTER;
    type.span = source_span_invalid();
    type.as.opaque_pointer.is_readonly = is_readonly;
    type.as.opaque_pointer.is_volatile = is_volatile;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_array(CogIrModule *module, CogIrTypeId element_type, size_t length)
{
    if (!module_mutable(module) || !cog_ir_get_type(module, element_type))
        return COG_IR_TYPE_INVALID;

    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_ARRAY &&
            type->as.array.element_type == element_type &&
            type->as.array.length == length)
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_ARRAY;
    type.span = source_span_invalid();
    type.as.array.element_type = element_type;
    type.as.array.length = length;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_type_function(
    CogIrModule *module,
    CogIrTypeId result_type,
    const CogIrTypeId *parameter_types,
    size_t parameter_count,
    CogIrAbiRepresentation abi,
    CogIrCallingConvention calling_convention,
    int is_variadic
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, result_type))
        return COG_IR_TYPE_INVALID;
    if (parameter_count && !parameter_types)
        return COG_IR_TYPE_INVALID;
    for (size_t i = 0; i < parameter_count; ++i)
        if (!cog_ir_get_type(module, parameter_types[i]))
            return COG_IR_TYPE_INVALID;

    is_variadic = !!is_variadic;
    for (size_t i = 0; i < module->type_count; ++i) {
        const CogIrType *type = &module->types[i];
        if (type->kind == COG_IR_TYPE_FUNCTION &&
            type->as.function.result_type == result_type &&
            type->as.function.parameter_count == parameter_count &&
            type->as.function.abi == abi &&
            type->as.function.calling_convention == calling_convention &&
            type->as.function.is_variadic == is_variadic &&
            same_type_ids(type->as.function.parameter_types, parameter_types, parameter_count))
            return (CogIrTypeId)i;
    }

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_TYPE_FUNCTION;
    type.span = source_span_invalid();
    type.as.function.result_type = result_type;
    type.as.function.parameter_types = copy_type_ids(module, parameter_types, parameter_count);
    type.as.function.parameter_count = parameter_count;
    type.as.function.abi = abi;
    type.as.function.calling_convention = calling_convention;
    type.as.function.is_variadic = is_variadic;
    return append_type(module, &type);
}

CogIrTypeId cog_ir_declare_nominal_type(
    CogIrModule *module,
    CogIrTypeKind kind,
    StringView debug_name,
    SourceSpan span
) {
    if (!module_mutable(module) ||
        (kind != COG_IR_TYPE_STRUCT && kind != COG_IR_TYPE_UNION && kind != COG_IR_TYPE_ENUM))
        return COG_IR_TYPE_INVALID;

    CogIrType type;
    memset(&type, 0, sizeof(type));
    type.kind = kind;
    type.debug_name = copy_string_view(module, debug_name);
    type.span = span;
    if (kind == COG_IR_TYPE_ENUM) {
        type.as.enumeration.backing_type = COG_IR_TYPE_INVALID;
        type.as.enumeration.backing_abi_type = COG_IR_ABI_TYPE_INVALID;
    }
    return append_type(module, &type);
}

int cog_ir_mark_incomplete_aggregate_type(CogIrModule *module, CogIrTypeId type_id)
{
    if (!module_mutable(module))
        return 0;

    CogIrType *type = (CogIrType *)cog_ir_get_type(module, type_id);
    if (!type || (type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION) ||
        type->as.aggregate.is_complete || type->as.aggregate.is_incomplete)
        return 0;

    type->as.aggregate.is_incomplete = 1;
    type->as.aggregate.is_repr_c = 1;
    return 1;
}

int cog_ir_define_aggregate_type(
    CogIrModule *module,
    CogIrTypeId type_id,
    const CogIrAggregateField *fields,
    size_t field_count,
    int is_repr_c,
    int is_packed,
    unsigned explicit_alignment
) {
    if (!module_mutable(module) || (field_count && !fields))
        return 0;

    CogIrType *type = (CogIrType *)cog_ir_get_type(module, type_id);
    if (!type || (type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION) ||
        type->as.aggregate.is_complete || type->as.aggregate.is_incomplete)
        return 0;

    CogIrAggregateField *copy = NULL;
    if (field_count) {
        copy = arena_zalloc(module->arena, field_count * sizeof(*copy));
        for (size_t i = 0; i < field_count; ++i) {
            if (!cog_ir_get_type(module, fields[i].type))
                return 0;
            copy[i] = fields[i];
            copy[i].debug_name = copy_string_view(module, fields[i].debug_name);
        }
    }

    type->as.aggregate.is_complete = 1;
    type->as.aggregate.is_incomplete = 0;
    type->as.aggregate.fields = copy;
    type->as.aggregate.field_count = field_count;
    type->as.aggregate.is_repr_c = !!is_repr_c;
    type->as.aggregate.is_packed = !!is_packed;
    type->as.aggregate.explicit_alignment = explicit_alignment;
    return 1;
}

int cog_ir_define_enum_type(
    CogIrModule *module,
    CogIrTypeId type_id,
    CogIrTypeId backing_type,
    const CogIrEnumMember *members,
    size_t member_count,
    int is_repr_c,
    CogIrAbiTypeId backing_abi_type
) {
    if (!module_mutable(module) || (member_count && !members))
        return 0;

    CogIrType *type = (CogIrType *)cog_ir_get_type(module, type_id);
    const CogIrType *backing = cog_ir_get_type(module, backing_type);
    if (!type || type->kind != COG_IR_TYPE_ENUM ||
        type->as.enumeration.backing_type != COG_IR_TYPE_INVALID ||
        !backing || backing->kind != COG_IR_TYPE_INTEGER)
        return 0;

    CogIrEnumMember *copy = NULL;
    if (member_count) {
        copy = arena_zalloc(module->arena, member_count * sizeof(*copy));
        for (size_t i = 0; i < member_count; ++i) {
            copy[i] = members[i];
            copy[i].debug_name = copy_string_view(module, members[i].debug_name);
        }
    }

    if (is_repr_c && !cog_ir_get_abi_type(module, backing_abi_type))
        return 0;

    type->as.enumeration.backing_type = backing_type;
    type->as.enumeration.members = copy;
    type->as.enumeration.member_count = member_count;
    type->as.enumeration.is_repr_c = !!is_repr_c;
    type->as.enumeration.backing_abi_type = backing_abi_type;
    return 1;
}

static CogIrAbiTypeId append_abi_type(CogIrModule *module, const CogIrAbiType *input)
{
    if (!module_mutable(module) || module->abi_type_count >= COG_IR_ID_INVALID)
        return COG_IR_ABI_TYPE_INVALID;

    module->abi_types = grow_array(
        module->arena,
        module->abi_types,
        module->abi_type_count,
        sizeof(*module->abi_types),
        &module->abi_type_capacity
    );

    CogIrAbiTypeId id = (CogIrAbiTypeId)module->abi_type_count;
    CogIrAbiType *type = &module->abi_types[module->abi_type_count++];
    *type = *input;
    type->id = id;
    return id;
}

static CogIrAbiTypeId intern_abi_type(CogIrModule *module, const CogIrAbiType *candidate)
{
    for (size_t i = 0; i < module->abi_type_count; ++i) {
        const CogIrAbiType *type = &module->abi_types[i];
        if (type->kind != candidate->kind ||
            type->runtime_type != candidate->runtime_type ||
            type->c_scalar_kind != candidate->c_scalar_kind ||
            type->element_type != candidate->element_type ||
            type->parameter_count != candidate->parameter_count ||
            type->return_type != candidate->return_type)
            continue;
        if (!same_abi_type_ids(type->parameter_types, candidate->parameter_types, candidate->parameter_count))
            continue;
        return (CogIrAbiTypeId)i;
    }

    CogIrAbiType copy = *candidate;
    copy.parameter_types = copy_abi_type_ids(module, candidate->parameter_types, candidate->parameter_count);
    return append_abi_type(module, &copy);
}

CogIrAbiTypeId cog_ir_abi_type_semantic(CogIrModule *module, CogIrTypeId runtime_type)
{
    if (!module_mutable(module) || !cog_ir_get_type(module, runtime_type))
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_SEMANTIC;
    type.runtime_type = runtime_type;
    type.element_type = COG_IR_ABI_TYPE_INVALID;
    type.return_type = COG_IR_ABI_TYPE_INVALID;
    return intern_abi_type(module, &type);
}

CogIrAbiTypeId cog_ir_abi_type_c_scalar(
    CogIrModule *module,
    CogIrTypeId runtime_type,
    CogIrCScalarKind scalar
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, runtime_type) || scalar == COG_IR_C_SCALAR_NONE)
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_C_SCALAR;
    type.runtime_type = runtime_type;
    type.c_scalar_kind = scalar;
    type.element_type = COG_IR_ABI_TYPE_INVALID;
    type.return_type = COG_IR_ABI_TYPE_INVALID;
    return intern_abi_type(module, &type);
}

CogIrAbiTypeId cog_ir_abi_type_pointer(
    CogIrModule *module,
    CogIrTypeId runtime_type,
    CogIrAbiTypeId element_type
) {
    const CogIrType *runtime = cog_ir_get_type(module, runtime_type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_POINTER ||
        !cog_ir_get_abi_type(module, element_type))
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_POINTER;
    type.runtime_type = runtime_type;
    type.element_type = element_type;
    type.return_type = COG_IR_ABI_TYPE_INVALID;
    return intern_abi_type(module, &type);
}

CogIrAbiTypeId cog_ir_abi_type_opaque_pointer(CogIrModule *module, CogIrTypeId runtime_type)
{
    const CogIrType *runtime = cog_ir_get_type(module, runtime_type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_OPAQUE_POINTER)
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_OPAQUE_POINTER;
    type.runtime_type = runtime_type;
    type.element_type = COG_IR_ABI_TYPE_INVALID;
    type.return_type = COG_IR_ABI_TYPE_INVALID;
    return intern_abi_type(module, &type);
}

CogIrAbiTypeId cog_ir_abi_type_array(
    CogIrModule *module,
    CogIrTypeId runtime_type,
    CogIrAbiTypeId element_type
) {
    const CogIrType *runtime = cog_ir_get_type(module, runtime_type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_ARRAY ||
        !cog_ir_get_abi_type(module, element_type))
        return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_ARRAY;
    type.runtime_type = runtime_type;
    type.element_type = element_type;
    type.return_type = COG_IR_ABI_TYPE_INVALID;
    return intern_abi_type(module, &type);
}

CogIrAbiTypeId cog_ir_abi_type_function(
    CogIrModule *module,
    CogIrTypeId runtime_type,
    CogIrAbiTypeId return_type,
    const CogIrAbiTypeId *parameter_types,
    size_t parameter_count
) {
    const CogIrType *runtime = cog_ir_get_type(module, runtime_type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_FUNCTION ||
        !cog_ir_get_abi_type(module, return_type) ||
        parameter_count != runtime->as.function.parameter_count ||
        (parameter_count && !parameter_types))
        return COG_IR_ABI_TYPE_INVALID;

    for (size_t i = 0; i < parameter_count; ++i)
        if (!cog_ir_get_abi_type(module, parameter_types[i]))
            return COG_IR_ABI_TYPE_INVALID;

    CogIrAbiType type;
    memset(&type, 0, sizeof(type));
    type.kind = COG_IR_ABI_TYPE_FUNCTION;
    type.runtime_type = runtime_type;
    type.element_type = COG_IR_ABI_TYPE_INVALID;
    type.parameter_types = (CogIrAbiTypeId *)parameter_types;
    type.parameter_count = parameter_count;
    type.return_type = return_type;
    return intern_abi_type(module, &type);
}

static CogIrConstId append_constant(CogIrModule *module, const CogIrConstant *input)
{
    if (!module_mutable(module) || module->constant_count >= COG_IR_ID_INVALID)
        return COG_IR_CONST_INVALID;

    module->constants = grow_array(
        module->arena,
        module->constants,
        module->constant_count,
        sizeof(*module->constants),
        &module->constant_capacity
    );

    CogIrConstId id = (CogIrConstId)module->constant_count;
    CogIrConstant *constant = &module->constants[module->constant_count++];
    *constant = *input;
    constant->id = id;
    return id;
}

static CogIrConstId intern_scalar_constant(CogIrModule *module, const CogIrConstant *candidate)
{
    for (size_t i = 0; i < module->constant_count; ++i) {
        const CogIrConstant *constant = &module->constants[i];
        if (constant->kind != candidate->kind || constant->type != candidate->type)
            continue;

        switch (candidate->kind) {
            case COG_IR_CONST_ZERO:
            case COG_IR_CONST_NULL:
                return (CogIrConstId)i;
            case COG_IR_CONST_BOOL:
                if (constant->as.boolean == candidate->as.boolean) return (CogIrConstId)i;
                break;
            case COG_IR_CONST_INTEGER:
                if (constant->as.integer_bits == candidate->as.integer_bits) return (CogIrConstId)i;
                break;
            case COG_IR_CONST_FLOAT32:
                if (constant->as.float32_bits == candidate->as.float32_bits) return (CogIrConstId)i;
                break;
            case COG_IR_CONST_FLOAT64:
                if (constant->as.float64_bits == candidate->as.float64_bits) return (CogIrConstId)i;
                break;
            case COG_IR_CONST_ARRAY:
            case COG_IR_CONST_STRUCT:
                break;
        }
    }
    return append_constant(module, candidate);
}

CogIrConstId cog_ir_const_zero(CogIrModule *module, CogIrTypeId type)
{
    if (!module_mutable(module) || !cog_ir_get_type(module, type))
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_ZERO;
    constant.type = type;
    return intern_scalar_constant(module, &constant);
}

CogIrConstId cog_ir_const_bool(CogIrModule *module, CogIrTypeId type, int value)
{
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_BOOL)
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_BOOL;
    constant.type = type;
    constant.as.boolean = !!value;
    return intern_scalar_constant(module, &constant);
}

CogIrConstId cog_ir_const_integer(CogIrModule *module, CogIrTypeId type, uint64_t bits)
{
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!module_mutable(module) || !runtime ||
        (runtime->kind != COG_IR_TYPE_INTEGER && runtime->kind != COG_IR_TYPE_ENUM))
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_INTEGER;
    constant.type = type;
    constant.as.integer_bits = bits;
    return intern_scalar_constant(module, &constant);
}

CogIrConstId cog_ir_const_float32(CogIrModule *module, CogIrTypeId type, uint32_t bits)
{
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_FLOAT || runtime->as.floating.bits != 32)
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_FLOAT32;
    constant.type = type;
    constant.as.float32_bits = bits;
    return intern_scalar_constant(module, &constant);
}

CogIrConstId cog_ir_const_float64(CogIrModule *module, CogIrTypeId type, uint64_t bits)
{
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!module_mutable(module) || !runtime || runtime->kind != COG_IR_TYPE_FLOAT || runtime->as.floating.bits != 64)
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_FLOAT64;
    constant.type = type;
    constant.as.float64_bits = bits;
    return intern_scalar_constant(module, &constant);
}

CogIrConstId cog_ir_const_null(CogIrModule *module, CogIrTypeId type)
{
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!module_mutable(module) || !runtime ||
        (runtime->kind != COG_IR_TYPE_POINTER &&
         runtime->kind != COG_IR_TYPE_OPAQUE_POINTER &&
         runtime->kind != COG_IR_TYPE_FUNCTION))
        return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = COG_IR_CONST_NULL;
    constant.type = type;
    return intern_scalar_constant(module, &constant);
}

static CogIrConstId add_aggregate_constant(
    CogIrModule *module,
    CogIrConstKind kind,
    CogIrTypeId type,
    const CogIrConstId *elements,
    size_t element_count
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, type) || (element_count && !elements))
        return COG_IR_CONST_INVALID;
    for (size_t i = 0; i < element_count; ++i)
        if (!cog_ir_get_constant(module, elements[i]))
            return COG_IR_CONST_INVALID;

    CogIrConstant constant;
    memset(&constant, 0, sizeof(constant));
    constant.kind = kind;
    constant.type = type;
    constant.as.aggregate.elements = copy_const_ids(module, elements, element_count);
    constant.as.aggregate.element_count = element_count;
    return append_constant(module, &constant);
}

CogIrConstId cog_ir_const_array(
    CogIrModule *module,
    CogIrTypeId type,
    const CogIrConstId *elements,
    size_t element_count
) {
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!runtime || runtime->kind != COG_IR_TYPE_ARRAY)
        return COG_IR_CONST_INVALID;
    return add_aggregate_constant(module, COG_IR_CONST_ARRAY, type, elements, element_count);
}

CogIrConstId cog_ir_const_struct(
    CogIrModule *module,
    CogIrTypeId type,
    const CogIrConstId *fields,
    size_t field_count
) {
    const CogIrType *runtime = cog_ir_get_type(module, type);
    if (!runtime || runtime->kind != COG_IR_TYPE_STRUCT)
        return COG_IR_CONST_INVALID;
    return add_aggregate_constant(module, COG_IR_CONST_STRUCT, type, fields, field_count);
}

CogIrGlobalId cog_ir_add_global(
    CogIrModule *module,
    StringView debug_name,
    SourceSpan span,
    CogIrTypeId type,
    CogIrAbiTypeId abi_type,
    CogIrLinkage linkage,
    int is_compiler_generated,
    int is_readonly,
    CogIrConstId static_initializer
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, type) ||
        (abi_type != COG_IR_ABI_TYPE_INVALID && !cog_ir_get_abi_type(module, abi_type)) ||
        !cog_ir_get_constant(module, static_initializer) ||
        module->global_count >= COG_IR_ID_INVALID)
        return COG_IR_GLOBAL_INVALID;

    module->globals = grow_array(
        module->arena,
        module->globals,
        module->global_count,
        sizeof(*module->globals),
        &module->global_capacity
    );

    CogIrGlobalId id = (CogIrGlobalId)module->global_count;
    CogIrGlobal *global = &module->globals[module->global_count++];
    memset(global, 0, sizeof(*global));
    global->id = id;
    global->debug_name = copy_string_view(module, debug_name);
    global->span = span;
    global->type = type;
    global->abi_type = abi_type;
    global->linkage = linkage;
    global->is_compiler_generated = !!is_compiler_generated;
    global->is_readonly = !!is_readonly;
    global->static_initializer = static_initializer;
    return id;
}

static CogIrFunction *get_function_mut(CogIrModule *module, CogIrFunctionId id)
{
    return (CogIrFunction *)cog_ir_get_function(module, id);
}

static CogIrBlock *get_block_mut(CogIrFunction *function, CogIrBlockId id)
{
    return (CogIrBlock *)cog_ir_get_block(function, id);
}

static CogIrValueId append_value(
    CogIrModule *module,
    CogIrFunction *function,
    CogIrTypeId type,
    CogIrValueKind kind,
    CogIrBlockId block,
    size_t ordinal,
    SourceSpan span
) {
    if (!cog_ir_get_type(module, type) || function->value_count >= COG_IR_ID_INVALID)
        return COG_IR_VALUE_INVALID;

    function->values = grow_array(
        module->arena,
        function->values,
        function->value_count,
        sizeof(*function->values),
        &function->value_capacity
    );

    CogIrValueId id = (CogIrValueId)function->value_count;
    CogIrValue *value = &function->values[function->value_count++];
    memset(value, 0, sizeof(*value));
    value->id = id;
    value->type = type;
    value->abi_type = COG_IR_ABI_TYPE_INVALID;
    value->kind = kind;
    value->block = block;
    value->ordinal = ordinal;
    value->span = span;
    return id;
}

CogIrFunctionId cog_ir_add_function(
    CogIrModule *module,
    StringView debug_name,
    SourceSpan span,
    CogIrTypeId function_type,
    CogIrFunctionKind kind,
    CogIrLinkage linkage,
    int is_compiler_generated,
    const CogIrFunctionAbi *abi
) {
    const CogIrType *type = cog_ir_get_type(module, function_type);
    if (!module_mutable(module) || !type || type->kind != COG_IR_TYPE_FUNCTION ||
        module->function_count >= COG_IR_ID_INVALID)
        return COG_IR_FUNCTION_INVALID;

    if (abi) {
        if (abi->abi != type->as.function.abi ||
            abi->calling_convention != type->as.function.calling_convention ||
            !!abi->is_variadic != !!type->as.function.is_variadic ||
            abi->parameter_count != type->as.function.parameter_count ||
            (abi->parameter_count && !abi->parameter_abi_types))
            return COG_IR_FUNCTION_INVALID;
    }

    module->functions = grow_array(
        module->arena,
        module->functions,
        module->function_count,
        sizeof(*module->functions),
        &module->function_capacity
    );

    CogIrFunctionId id = (CogIrFunctionId)module->function_count;
    CogIrFunction *function = &module->functions[module->function_count++];
    memset(function, 0, sizeof(*function));
    function->id = id;
    function->debug_name = copy_string_view(module, debug_name);
    function->span = span;
    function->type = function_type;
    function->kind = kind;
    function->linkage = linkage;
    function->is_compiler_generated = !!is_compiler_generated;
    function->entry_block = COG_IR_BLOCK_INVALID;

    function->abi.abi = type->as.function.abi;
    function->abi.calling_convention = type->as.function.calling_convention;
    function->abi.is_variadic = type->as.function.is_variadic;
    function->abi.return_abi_type = COG_IR_ABI_TYPE_INVALID;

    if (abi) {
        function->abi = *abi;
        function->abi.external_symbol = copy_string_view(module, abi->external_symbol);
        function->abi.parameter_abi_types = copy_abi_type_ids(
            module,
            abi->parameter_abi_types,
            abi->parameter_count
        );
    }

    function->parameter_count = type->as.function.parameter_count;
    if (function->parameter_count) {
        function->parameters = arena_alloc(
            module->arena,
            function->parameter_count * sizeof(*function->parameters)
        );

        for (size_t i = 0; i < function->parameter_count; ++i) {
            CogIrValueId value = append_value(
                module,
                function,
                type->as.function.parameter_types[i],
                COG_IR_VALUE_FUNCTION_PARAMETER,
                COG_IR_BLOCK_INVALID,
                i,
                span
            );
            if (value == COG_IR_VALUE_INVALID)
                return COG_IR_FUNCTION_INVALID;
            function->parameters[i] = value;
            if (abi && i < abi->parameter_count) {
                const CogIrAbiType *parameter_abi = cog_ir_get_abi_type(
                    module, abi->parameter_abi_types[i]);
                if (parameter_abi && parameter_abi->kind == COG_IR_ABI_TYPE_FUNCTION)
                    function->values[value].abi_type = abi->parameter_abi_types[i];
            }
        }
    }

    return id;
}

int cog_ir_begin_function_definition(CogIrModule *module, CogIrFunctionId function_id)
{
    if (!module_mutable(module))
        return 0;

    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || function->kind != COG_IR_FUNCTION_DECLARATION ||
        function->linkage != COG_IR_LINKAGE_INTERNAL ||
        function->block_count != 0 || function->slot_count != 0 ||
        function->entry_block != COG_IR_BLOCK_INVALID)
        return 0;

    function->kind = COG_IR_FUNCTION_DEFINITION;
    return 1;
}

int cog_ir_set_entry_function(CogIrModule *module, CogIrFunctionId function)
{
    if (!module_mutable(module) || !cog_ir_get_function(module, function))
        return 0;
    module->entry_function = function;
    return 1;
}

int cog_ir_set_init_function(CogIrModule *module, CogIrFunctionId function)
{
    if (!module_mutable(module) || !cog_ir_get_function(module, function))
        return 0;
    module->init_function = function;
    return 1;
}

CogIrSlotId cog_ir_add_slot(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrSlotKind kind,
    size_t parameter_index,
    StringView debug_name,
    SourceSpan span,
    CogIrTypeId type
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, type) ||
        kind < COG_IR_SLOT_SOURCE_LOCAL || kind > COG_IR_SLOT_COMPILER_TEMP ||
        (kind == COG_IR_SLOT_SOURCE_PARAMETER) !=
            (parameter_index != COG_IR_PARAMETER_INDEX_INVALID))
        return COG_IR_SLOT_INVALID;

    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || function->kind != COG_IR_FUNCTION_DEFINITION || function->slot_count >= COG_IR_ID_INVALID)
        return COG_IR_SLOT_INVALID;

    function->slots = grow_array(
        module->arena,
        function->slots,
        function->slot_count,
        sizeof(*function->slots),
        &function->slot_capacity
    );

    CogIrSlotId id = (CogIrSlotId)function->slot_count;
    CogIrSlot *slot = &function->slots[function->slot_count++];
    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->kind = kind;
    slot->parameter_index = parameter_index;
    slot->debug_name = copy_string_view(module, debug_name);
    slot->span = span;
    slot->type = type;
    slot->abi_type = COG_IR_ABI_TYPE_INVALID;
    return id;
}

int cog_ir_set_slot_abi_type(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrSlotId slot_id,
    CogIrAbiTypeId abi_type
) {
    if (!module_mutable(module))
        return 0;
    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || slot_id == COG_IR_SLOT_INVALID || (size_t)slot_id >= function->slot_count)
        return 0;
    const CogIrAbiType *abi = cog_ir_get_abi_type(module, abi_type);
    CogIrSlot *slot = &function->slots[slot_id];
    if (!abi || abi->runtime_type != slot->type)
        return 0;
    slot->abi_type = abi_type;
    return 1;
}

int cog_ir_set_value_abi_type(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrValueId value_id,
    CogIrAbiTypeId abi_type
) {
    if (!module_mutable(module))
        return 0;
    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || value_id == COG_IR_VALUE_INVALID || (size_t)value_id >= function->value_count)
        return 0;
    const CogIrAbiType *abi = cog_ir_get_abi_type(module, abi_type);
    CogIrValue *value = &function->values[value_id];
    if (!abi || abi->runtime_type != value->type)
        return 0;
    value->abi_type = abi_type;
    return 1;
}

int cog_ir_mark_value_discarded(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrValueId value_id
) {
    if (!module_mutable(module))
        return 0;

    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || value_id == COG_IR_VALUE_INVALID ||
        (size_t)value_id >= function->value_count)
        return 0;

    CogIrValue *value = &function->values[value_id];
    if (value->kind != COG_IR_VALUE_INSTRUCTION ||
        value->block == COG_IR_BLOCK_INVALID ||
        (size_t)value->block >= function->block_count)
        return 0;

    CogIrBlock *block = &function->blocks[value->block];
    if (value->ordinal >= block->instruction_count)
        return 0;

    CogIrInstruction *instruction = &block->instructions[value->ordinal];
    if (instruction->result != value_id ||
        instruction->result_type == COG_IR_TYPE_INVALID)
        return 0;

    instruction->result_is_discarded = 1;
    return 1;
}

CogIrBlockId cog_ir_add_block(
    CogIrModule *module,
    CogIrFunctionId function_id,
    StringView debug_name,
    SourceSpan span
) {
    if (!module_mutable(module))
        return COG_IR_BLOCK_INVALID;

    CogIrFunction *function = get_function_mut(module, function_id);
    if (!function || function->kind != COG_IR_FUNCTION_DEFINITION || function->block_count >= COG_IR_ID_INVALID)
        return COG_IR_BLOCK_INVALID;

    function->blocks = grow_array(
        module->arena,
        function->blocks,
        function->block_count,
        sizeof(*function->blocks),
        &function->block_capacity
    );

    CogIrBlockId id = (CogIrBlockId)function->block_count;
    CogIrBlock *block = &function->blocks[function->block_count++];
    memset(block, 0, sizeof(*block));
    block->id = id;
    block->debug_name = copy_string_view(module, debug_name);
    block->span = span;
    block->terminator.kind = COG_IR_TERMINATOR_NONE;
    block->terminator.span = source_span_invalid();

    if (function->entry_block == COG_IR_BLOCK_INVALID)
        function->entry_block = id;

    return id;
}

CogIrValueId cog_ir_add_block_parameter(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrBlockId block_id,
    CogIrTypeId type,
    StringView debug_name,
    SourceSpan span
) {
    if (!module_mutable(module) || !cog_ir_get_type(module, type))
        return COG_IR_VALUE_INVALID;

    CogIrFunction *function = get_function_mut(module, function_id);
    CogIrBlock *block = get_block_mut(function, block_id);
    if (!function || !block)
        return COG_IR_VALUE_INVALID;

    block->parameters = grow_array(
        module->arena,
        block->parameters,
        block->parameter_count,
        sizeof(*block->parameters),
        &block->parameter_capacity
    );

    size_t ordinal = block->parameter_count;
    CogIrValueId value = append_value(
        module,
        function,
        type,
        COG_IR_VALUE_BLOCK_PARAMETER,
        block_id,
        ordinal,
        span
    );
    if (value == COG_IR_VALUE_INVALID)
        return COG_IR_VALUE_INVALID;

    CogIrBlockParam *param = &block->parameters[block->parameter_count++];
    param->value = value;
    param->type = type;
    param->debug_name = copy_string_view(module, debug_name);
    param->span = span;
    return value;
}

static void copy_instruction_payload(CogIrModule *module, CogIrInstruction *instruction)
{
    if (instruction->op == COG_IR_OP_CALL && instruction->as.call.argument_count) {
        instruction->as.call.arguments = copy_value_ids(
            module,
            instruction->as.call.arguments,
            instruction->as.call.argument_count
        );
    } else if ((instruction->op == COG_IR_OP_MAKE_STRUCT || instruction->op == COG_IR_OP_MAKE_ARRAY) &&
               instruction->as.aggregate.value_count) {
        instruction->as.aggregate.values = copy_value_ids(
            module,
            instruction->as.aggregate.values,
            instruction->as.aggregate.value_count
        );
    }
}

int cog_ir_emit(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrBlockId block_id,
    const CogIrInstruction *input,
    CogIrValueId *out_result
) {
    if (out_result)
        *out_result = COG_IR_VALUE_INVALID;

    if (!module_mutable(module) || !input)
        return 0;

    CogIrFunction *function = get_function_mut(module, function_id);
    CogIrBlock *block = get_block_mut(function, block_id);
    if (!function || !block || block->terminator.kind != COG_IR_TERMINATOR_NONE)
        return 0;

    block->instructions = grow_array(
        module->arena,
        block->instructions,
        block->instruction_count,
        sizeof(*block->instructions),
        &block->instruction_capacity
    );

    size_t ordinal = block->instruction_count;
    CogIrInstruction *instruction = &block->instructions[block->instruction_count++];
    *instruction = *input;
    copy_instruction_payload(module, instruction);
    instruction->result = COG_IR_VALUE_INVALID;

    if (input->result_type != COG_IR_TYPE_INVALID) {
        if (!cog_ir_get_type(module, input->result_type)) {
            block->instruction_count--;
            return 0;
        }
        CogIrValueId value = append_value(
            module,
            function,
            input->result_type,
            COG_IR_VALUE_INSTRUCTION,
            block_id,
            ordinal,
            input->span
        );
        if (value == COG_IR_VALUE_INVALID) {
            block->instruction_count--;
            return 0;
        }
        instruction->result = value;
        if (out_result)
            *out_result = value;
    }

    return 1;
}

static CogIrBranchEdge copy_edge(CogIrModule *module, CogIrBranchEdge edge)
{
    CogIrBranchEdge copy = edge;
    copy.arguments = copy_value_ids(module, edge.arguments, edge.argument_count);
    return copy;
}

int cog_ir_set_terminator(
    CogIrModule *module,
    CogIrFunctionId function_id,
    CogIrBlockId block_id,
    const CogIrTerminator *input
) {
    if (!module_mutable(module) || !input || input->kind == COG_IR_TERMINATOR_NONE)
        return 0;

    CogIrFunction *function = get_function_mut(module, function_id);
    CogIrBlock *block = get_block_mut(function, block_id);
    if (!function || !block || block->terminator.kind != COG_IR_TERMINATOR_NONE)
        return 0;

    block->terminator = *input;
    switch (input->kind) {
        case COG_IR_TERMINATOR_BR:
            block->terminator.as.branch.edge = copy_edge(module, input->as.branch.edge);
            break;
        case COG_IR_TERMINATOR_COND_BR:
            block->terminator.as.cond_branch.if_true = copy_edge(module, input->as.cond_branch.if_true);
            block->terminator.as.cond_branch.if_false = copy_edge(module, input->as.cond_branch.if_false);
            break;
        case COG_IR_TERMINATOR_SWITCH:
            block->terminator.as.switch_term.default_edge = copy_edge(module, input->as.switch_term.default_edge);
            if (input->as.switch_term.case_count) {
                if (!input->as.switch_term.cases)
                    return 0;
                CogIrSwitchCase *cases = arena_zalloc(
                    module->arena,
                    input->as.switch_term.case_count * sizeof(*cases)
                );
                for (size_t i = 0; i < input->as.switch_term.case_count; ++i) {
                    cases[i] = input->as.switch_term.cases[i];
                    cases[i].edge = copy_edge(module, input->as.switch_term.cases[i].edge);
                }
                block->terminator.as.switch_term.cases = cases;
            }
            break;
        case COG_IR_TERMINATOR_RET:
        case COG_IR_TERMINATOR_TRAP:
        case COG_IR_TERMINATOR_UNREACHABLE:
        case COG_IR_TERMINATOR_NONE:
            break;
    }

    return 1;
}

const char *cog_ir_type_kind_name(CogIrTypeKind kind)
{
    switch (kind) {
        case COG_IR_TYPE_VOID: return "void";
        case COG_IR_TYPE_BOOL: return "bool";
        case COG_IR_TYPE_INTEGER: return "integer";
        case COG_IR_TYPE_FLOAT: return "float";
        case COG_IR_TYPE_POINTER: return "pointer";
        case COG_IR_TYPE_OPAQUE_POINTER: return "opaque-pointer";
        case COG_IR_TYPE_ARRAY: return "array";
        case COG_IR_TYPE_STRUCT: return "struct";
        case COG_IR_TYPE_UNION: return "union";
        case COG_IR_TYPE_ENUM: return "enum";
        case COG_IR_TYPE_FUNCTION: return "function";
    }
    return "unknown";
}

const char *cog_ir_op_name(CogIrOp op)
{
    static const char *names[] = {
        "const", "function_ref", "local_addr", "global_addr", "field_addr",
        "array_elem_addr", "ptr_index_addr", "load", "store", "make_struct",
        "make_array", "extract_field", "extract_element", "iadd.checked",
        "isub.checked", "imul.checked", "idiv.checked", "irem.checked",
        "ineg.checked", "iadd.wrap", "isub.wrap", "imul.wrap", "ineg.wrap",
        "bit.and", "bit.or", "bit.xor", "bit.not", "shl.checked_count",
        "shr.signed.checked_count", "shr.unsigned.checked_count", "fadd", "fsub",
        "fmul", "fdiv", "fneg", "icmp.eq", "icmp.ne", "icmp.slt", "icmp.sle",
        "icmp.sgt", "icmp.sge", "icmp.ult", "icmp.ule", "icmp.ugt", "icmp.uge",
        "fcmp.eq", "fcmp.ne", "fcmp.lt", "fcmp.le", "fcmp.gt", "fcmp.ge",
        "ptr.eq", "ptr.ne", "bool.not", "cast.checked", "int.truncate",
        "ptr.reinterpret", "ptr.qualify", "size_of", "align_of",
        "c.vararg.promote", "call"
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    if ((size_t)op >= count)
        return "unknown-op";
    return names[op];
}
