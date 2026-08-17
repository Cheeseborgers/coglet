#include "backend_llvm_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DWARF constants consumed by LLVM's C debug-info API. They are kept local to
 * this backend rather than leaking debug-format policy into CogIR. */
enum {
    COG_DW_ATE_ADDRESS = 0x01,
    COG_DW_ATE_BOOLEAN = 0x02,
    COG_DW_ATE_FLOAT = 0x04,
    COG_DW_ATE_SIGNED = 0x05,
    COG_DW_ATE_UNSIGNED = 0x07,

    COG_DW_TAG_ENUMERATION_TYPE = 0x04,
    COG_DW_TAG_STRUCTURE_TYPE = 0x13,
    COG_DW_TAG_UNION_TYPE = 0x17,
    COG_DW_TAG_CONST_TYPE = 0x26,
    COG_DW_TAG_VOLATILE_TYPE = 0x35,
};

static LLVMMetadataRef debug_type(LlvmBackend *backend, CogIrTypeId id);
static LLVMMetadataRef debug_abi_type(LlvmBackend *backend, CogIrAbiTypeId id);

static LLVMMetadataRef debug_file_for_id(LlvmBackend *backend, SourceFileId id)
{
    if (id == SOURCE_FILE_ID_INVALID || (size_t)id >= backend->di_file_count)
        return NULL;
    return backend->di_files[id];
}

static LLVMMetadataRef debug_file_for_span(LlvmBackend *backend, SourceSpan span)
{
    return source_span_is_valid(span) ? debug_file_for_id(backend, span.file_id) : NULL;
}

static void split_source_path(
    const char *path,
    const char **filename,
    size_t *filename_length,
    const char **directory,
    size_t *directory_length
) {
    const char *last_separator = NULL;
    if (!path)
        path = "<unknown>";
    for (const char *cursor = path; *cursor; ++cursor) {
        if (*cursor == '/' || *cursor == '\\')
            last_separator = cursor;
    }

    if (!last_separator) {
        *filename = path;
        *filename_length = strlen(path);
        *directory = ".";
        *directory_length = 1;
        return;
    }

    *filename = last_separator + 1;
    *filename_length = strlen(*filename);
    *directory = path;
    *directory_length = (size_t)(last_separator - path);
    if (*directory_length == 0 && (*path == '/' || *path == '\\'))
        *directory_length = 1;
}

static int create_debug_files(LlvmBackend *backend)
{
    backend->di_file_count = backend->ir->sources.next_id;
    if (!backend->di_file_count) {
        llvm_backend_error(backend, "debug information requires at least one CogIR source file");
        return 0;
    }

    backend->di_files = calloc(backend->di_file_count, sizeof(*backend->di_files));
    if (!backend->di_files) {
        llvm_backend_error(backend, "out of memory creating debug source-file table");
        return 0;
    }

    for (const SourceFile *source = backend->ir->sources.first; source; source = source->next) {
        if ((size_t)source->id >= backend->di_file_count) {
            llvm_backend_error(backend, "CogIR source file id is outside debug source-file table");
            return 0;
        }
        const char *filename = NULL;
        const char *directory = NULL;
        size_t filename_length = 0;
        size_t directory_length = 0;
        split_source_path(
            source->filename,
            &filename,
            &filename_length,
            &directory,
            &directory_length
        );
        backend->di_files[source->id] = LLVMDIBuilderCreateFile(
            backend->di_builder,
            filename,
            filename_length,
            directory,
            directory_length
        );
        if (!backend->di_files[source->id]) {
            llvm_backend_error(backend, "could not create LLVM debug source-file metadata");
            return 0;
        }
    }

    for (size_t i = 0; i < backend->di_file_count; ++i) {
        if (!backend->di_files[i]) {
            llvm_backend_error(backend, "CogIR source file table contains an unresolved id");
            return 0;
        }
    }
    return 1;
}

static LLVMMetadataRef primary_debug_file(LlvmBackend *backend)
{
    if (!backend->ir->sources.first)
        return NULL;
    return debug_file_for_id(backend, backend->ir->sources.first->id);
}

static void add_debug_info_version_flag(LlvmBackend *backend)
{
    LLVMTypeRef i32 = LLVMInt32TypeInContext(backend->context);
    LLVMValueRef version = LLVMConstInt(i32, LLVMDebugMetadataVersion(), 0);
    LLVMAddModuleFlag(
        backend->module,
        LLVMModuleFlagBehaviorWarning,
        "Debug Info Version",
        sizeof("Debug Info Version") - 1,
        LLVMValueAsMetadata(version)
    );
}

int llvm_debug_init(LlvmBackend *backend)
{
    if (!backend->debug_info)
        return 1;

#if COGLET_LLVM_VERSION_MAJOR >= 19
    /* LLVM 19 removed dbg.value/dbg.declare intrinsics from its default debug
     * representation. Select the replacement DbgRecord format explicitly. */
    LLVMSetIsNewDbgInfoFormat(backend->module, 1);
#endif

    backend->di_builder = LLVMCreateDIBuilder(backend->module);
    if (!backend->di_builder) {
        llvm_backend_error(backend, "could not create LLVM debug-info builder");
        return 0;
    }
    if (!create_debug_files(backend))
        return 0;

    LLVMMetadataRef primary_file = primary_debug_file(backend);
    if (!primary_file) {
        llvm_backend_error(backend, "could not resolve primary CogIR source file for debug information");
        return 0;
    }

    const char *producer = "Coglet compiler";
    /* The LLVM C API exposes only its known DWARF language enumeration. C is
     * used solely as the debugger interoperability language code; Coglet type
     * names, source files, source locations, and semantics remain Coglet-owned. */
    backend->di_compile_unit = LLVMDIBuilderCreateCompileUnit(
        backend->di_builder,
        LLVMDWARFSourceLanguageC,
        primary_file,
        producer,
        strlen(producer),
        backend->optimization_level != COG_OPTIMIZATION_LEVEL_0,
        "",
        0,
        0,
        "",
        0,
        LLVMDWARFEmissionFull,
        0,
        1,
        0,
        "",
        0,
        "",
        0
    );
    if (!backend->di_compile_unit) {
        llvm_backend_error(backend, "could not create LLVM debug compile unit");
        return 0;
    }

    backend->di_expression = LLVMDIBuilderCreateExpression(backend->di_builder, NULL, 0);
    backend->di_types = calloc(backend->ir->type_count, sizeof(*backend->di_types));
    backend->di_abi_types = calloc(backend->ir->abi_type_count, sizeof(*backend->di_abi_types));
    backend->di_subprograms = calloc(backend->ir->function_count, sizeof(*backend->di_subprograms));
    if ((backend->ir->type_count && !backend->di_types) ||
        (backend->ir->abi_type_count && !backend->di_abi_types) ||
        (backend->ir->function_count && !backend->di_subprograms) ||
        !backend->di_expression) {
        llvm_backend_error(backend, "out of memory initializing LLVM debug metadata");
        return 0;
    }

    add_debug_info_version_flag(backend);
    return 1;
}

void llvm_debug_finalize(LlvmBackend *backend)
{
    if (backend->debug_info && backend->di_builder)
        LLVMDIBuilderFinalize(backend->di_builder);
}

void llvm_debug_dispose(LlvmBackend *backend)
{
    if (!backend)
        return;
    if (backend->di_builder)
        LLVMDisposeDIBuilder(backend->di_builder);
    free(backend->di_files);
    free(backend->di_types);
    free(backend->di_abi_types);
    free(backend->di_subprograms);
    backend->di_builder = NULL;
    backend->di_files = NULL;
    backend->di_types = NULL;
    backend->di_abi_types = NULL;
    backend->di_subprograms = NULL;
    backend->di_file_count = 0;
}

static uint64_t llvm_type_size_bits(LlvmBackend *backend, LLVMTypeRef type)
{
    return type ? LLVMABISizeOfType(backend->target_data, type) * UINT64_C(8) : 0;
}

static uint32_t llvm_type_align_bits(LlvmBackend *backend, LLVMTypeRef type)
{
    return type ? (uint32_t)(LLVMABIAlignmentOfType(backend->target_data, type) * 8u) : 0;
}

static void builtin_type_name(const CogIrType *type, char *buffer, size_t buffer_size)
{
    if (!string_view_is_empty(type->debug_name)) {
        size_t length = type->debug_name.length < buffer_size - 1
            ? type->debug_name.length : buffer_size - 1;
        memcpy(buffer, type->debug_name.data, length);
        buffer[length] = '\0';
        return;
    }

    switch (type->kind) {
        case COG_IR_TYPE_VOID: snprintf(buffer, buffer_size, "void"); break;
        case COG_IR_TYPE_BOOL: snprintf(buffer, buffer_size, "bool"); break;
        case COG_IR_TYPE_INTEGER:
            snprintf(buffer, buffer_size, "%c%u", type->as.integer.is_signed ? 'i' : 'u', type->as.integer.bits);
            break;
        case COG_IR_TYPE_FLOAT: snprintf(buffer, buffer_size, "f%u", type->as.floating.bits); break;
        case COG_IR_TYPE_OPAQUE_POINTER: snprintf(buffer, buffer_size, "opaque*"); break;
        default: buffer[0] = '\0'; break;
    }
}

static LLVMMetadataRef qualified_pointee_type(LlvmBackend *backend, const CogIrType *type)
{
    LLVMMetadataRef pointee = NULL;
    if (type->kind == COG_IR_TYPE_POINTER)
        pointee = debug_type(backend, type->as.pointer.pointee);
    else
        pointee = LLVMDIBuilderCreateUnspecifiedType(backend->di_builder, "opaque", 6);
    if (!pointee)
        return NULL;

    int is_readonly = type->kind == COG_IR_TYPE_POINTER
        ? type->as.pointer.is_readonly : type->as.opaque_pointer.is_readonly;
    int is_volatile = type->kind == COG_IR_TYPE_POINTER
        ? type->as.pointer.is_volatile : type->as.opaque_pointer.is_volatile;
    if (is_readonly)
        pointee = LLVMDIBuilderCreateQualifiedType(backend->di_builder, COG_DW_TAG_CONST_TYPE, pointee);
    if (is_volatile)
        pointee = LLVMDIBuilderCreateQualifiedType(backend->di_builder, COG_DW_TAG_VOLATILE_TYPE, pointee);
    return pointee;
}

static LLVMMetadataRef debug_subroutine_type(LlvmBackend *backend, const CogIrType *type)
{
    if (!type || type->kind != COG_IR_TYPE_FUNCTION)
        return NULL;

    size_t count = type->as.function.parameter_count + 1;
    LLVMMetadataRef *members = calloc(count, sizeof(*members));
    if (!members) {
        llvm_backend_error(backend, "out of memory creating LLVM debug function type");
        return NULL;
    }

    const CogIrType *result = cog_ir_get_type(backend->ir, type->as.function.result_type);
    if (!result) {
        free(members);
        return NULL;
    }
    members[0] = result->kind == COG_IR_TYPE_VOID
        ? NULL : debug_type(backend, type->as.function.result_type);
    if (result->kind != COG_IR_TYPE_VOID && !members[0]) {
        free(members);
        return NULL;
    }

    for (size_t i = 0; i < type->as.function.parameter_count; ++i) {
        members[i + 1] = debug_type(backend, type->as.function.parameter_types[i]);
        if (!members[i + 1]) {
            free(members);
            return NULL;
        }
    }

    LLVMMetadataRef file = debug_file_for_span(backend, type->span);
    if (!file)
        file = primary_debug_file(backend);
    LLVMMetadataRef result_type = LLVMDIBuilderCreateSubroutineType(
        backend->di_builder,
        file,
        members,
        (unsigned)count,
        LLVMDIFlagZero
    );
    free(members);
    return result_type;
}

static const char *c_scalar_debug_name(CogIrCScalarKind kind)
{
    switch (kind) {
        case COG_IR_C_SCALAR_CHAR: return "c_char";
        case COG_IR_C_SCALAR_SCHAR: return "c_schar";
        case COG_IR_C_SCALAR_UCHAR: return "c_uchar";
        case COG_IR_C_SCALAR_SHORT: return "c_short";
        case COG_IR_C_SCALAR_USHORT: return "c_ushort";
        case COG_IR_C_SCALAR_INT: return "c_int";
        case COG_IR_C_SCALAR_UINT: return "c_uint";
        case COG_IR_C_SCALAR_LONG: return "c_long";
        case COG_IR_C_SCALAR_ULONG: return "c_ulong";
        case COG_IR_C_SCALAR_LONGLONG: return "c_longlong";
        case COG_IR_C_SCALAR_ULONGLONG: return "c_ulonglong";
        case COG_IR_C_SCALAR_SIZE: return "c_size";
        case COG_IR_C_SCALAR_BOOL: return "c_bool";
        case COG_IR_C_SCALAR_FLOAT: return "c_float";
        case COG_IR_C_SCALAR_DOUBLE: return "c_double";
        case COG_IR_C_SCALAR_NONE: return NULL;
    }
    return NULL;
}

static unsigned c_scalar_debug_encoding(const CogIrAbiType *abi, const CogIrType *runtime)
{
    if (abi->c_scalar_kind == COG_IR_C_SCALAR_BOOL)
        return COG_DW_ATE_BOOLEAN;
    if (abi->c_scalar_kind == COG_IR_C_SCALAR_FLOAT ||
        abi->c_scalar_kind == COG_IR_C_SCALAR_DOUBLE)
        return COG_DW_ATE_FLOAT;
    if (runtime->kind == COG_IR_TYPE_INTEGER && runtime->as.integer.is_signed)
        return COG_DW_ATE_SIGNED;
    return COG_DW_ATE_UNSIGNED;
}

static LLVMMetadataRef debug_abi_subroutine_type(LlvmBackend *backend, const CogIrAbiType *abi)
{
    if (!abi || abi->kind != COG_IR_ABI_TYPE_FUNCTION)
        return NULL;

    size_t count = abi->parameter_count + 1;
    LLVMMetadataRef *members = calloc(count, sizeof(*members));
    if (!members) {
        llvm_backend_error(backend, "out of memory creating LLVM C ABI debug function type");
        return NULL;
    }

    const CogIrAbiType *result_abi = cog_ir_get_abi_type(backend->ir, abi->return_type);
    const CogIrType *result_runtime = result_abi
        ? cog_ir_get_type(backend->ir, result_abi->runtime_type) : NULL;
    if (!result_abi || !result_runtime) {
        free(members);
        return NULL;
    }
    members[0] = result_runtime->kind == COG_IR_TYPE_VOID
        ? NULL : debug_abi_type(backend, abi->return_type);
    if (result_runtime->kind != COG_IR_TYPE_VOID && !members[0]) {
        free(members);
        return NULL;
    }

    for (size_t i = 0; i < abi->parameter_count; ++i) {
        members[i + 1] = debug_abi_type(backend, abi->parameter_types[i]);
        if (!members[i + 1]) {
            free(members);
            return NULL;
        }
    }

    LLVMMetadataRef result = LLVMDIBuilderCreateSubroutineType(
        backend->di_builder,
        primary_debug_file(backend),
        members,
        (unsigned)count,
        LLVMDIFlagZero
    );
    free(members);
    return result;
}

static LLVMMetadataRef debug_abi_type(LlvmBackend *backend, CogIrAbiTypeId id)
{
    if (id == COG_IR_ABI_TYPE_INVALID || (size_t)id >= backend->ir->abi_type_count) {
        llvm_backend_error(backend, "invalid CogIR ABI type id in debug metadata");
        return NULL;
    }
    if (backend->di_abi_types[id])
        return backend->di_abi_types[id];

    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->ir, id);
    const CogIrType *runtime = abi ? cog_ir_get_type(backend->ir, abi->runtime_type) : NULL;
    if (!abi || !runtime)
        return NULL;

    LLVMMetadataRef result = NULL;
    switch (abi->kind) {
        case COG_IR_ABI_TYPE_SEMANTIC:
            result = debug_type(backend, abi->runtime_type);
            break;
        case COG_IR_ABI_TYPE_C_SCALAR: {
            const char *name = c_scalar_debug_name(abi->c_scalar_kind);
            LLVMTypeRef storage = llvm_lower_c_object_type(backend, id);
            if (!name || !storage) {
                llvm_backend_error(backend, "invalid native-C scalar ABI spelling in debug metadata");
                return NULL;
            }
            result = LLVMDIBuilderCreateBasicType(
                backend->di_builder,
                name,
                strlen(name),
                llvm_type_size_bits(backend, storage),
                c_scalar_debug_encoding(abi, runtime),
                LLVMDIFlagZero
            );
            break;
        }
        case COG_IR_ABI_TYPE_POINTER:
        case COG_IR_ABI_TYPE_OPAQUE_POINTER: {
            LLVMMetadataRef pointee = abi->kind == COG_IR_ABI_TYPE_POINTER
                ? debug_abi_type(backend, abi->element_type)
                : LLVMDIBuilderCreateUnspecifiedType(backend->di_builder, "opaque", 6);
            LLVMTypeRef pointer = llvm_lower_c_object_type(backend, id);
            if (!pointee || !pointer)
                return NULL;
            if (runtime->kind == COG_IR_TYPE_POINTER || runtime->kind == COG_IR_TYPE_OPAQUE_POINTER) {
                int is_readonly = runtime->kind == COG_IR_TYPE_POINTER
                    ? runtime->as.pointer.is_readonly : runtime->as.opaque_pointer.is_readonly;
                int is_volatile = runtime->kind == COG_IR_TYPE_POINTER
                    ? runtime->as.pointer.is_volatile : runtime->as.opaque_pointer.is_volatile;
                if (is_readonly)
                    pointee = LLVMDIBuilderCreateQualifiedType(
                        backend->di_builder, COG_DW_TAG_CONST_TYPE, pointee);
                if (is_volatile)
                    pointee = LLVMDIBuilderCreateQualifiedType(
                        backend->di_builder, COG_DW_TAG_VOLATILE_TYPE, pointee);
            }
            result = LLVMDIBuilderCreatePointerType(
                backend->di_builder,
                pointee,
                llvm_type_size_bits(backend, pointer),
                llvm_type_align_bits(backend, pointer),
                0,
                "",
                0
            );
            break;
        }
        case COG_IR_ABI_TYPE_ARRAY: {
            if (runtime->kind != COG_IR_TYPE_ARRAY) {
                llvm_backend_error(backend, "C array ABI debug metadata references a non-array runtime type");
                return NULL;
            }
            LLVMMetadataRef element = debug_abi_type(backend, abi->element_type);
            LLVMTypeRef storage = llvm_lower_c_object_type(backend, id);
            LLVMMetadataRef subrange = LLVMDIBuilderGetOrCreateSubrange(
                backend->di_builder, 0, (int64_t)runtime->as.array.length);
            result = element && storage && subrange ? LLVMDIBuilderCreateArrayType(
                backend->di_builder,
                llvm_type_size_bits(backend, storage),
                llvm_type_align_bits(backend, storage),
                element,
                &subrange,
                1
            ) : NULL;
            break;
        }
        case COG_IR_ABI_TYPE_FUNCTION: {
            LLVMMetadataRef subroutine = debug_abi_subroutine_type(backend, abi);
            LLVMTypeRef pointer = llvm_lower_c_object_type(backend, id);
            result = subroutine && pointer ? LLVMDIBuilderCreatePointerType(
                backend->di_builder,
                subroutine,
                llvm_type_size_bits(backend, pointer),
                llvm_type_align_bits(backend, pointer),
                0,
                "",
                0
            ) : NULL;
            break;
        }
    }

    if (!result && !backend->had_error)
        llvm_backend_error(backend, "could not create LLVM C ABI debug type metadata");
    backend->di_abi_types[id] = result;
    return result;
}

static int64_t signed_enum_value(uint64_t bits, unsigned width)
{
    if (!width || width >= 64)
        return (int64_t)bits;
    uint64_t mask = (UINT64_C(1) << width) - 1;
    bits &= mask;
    if (bits & (UINT64_C(1) << (width - 1)))
        bits |= ~mask;
    return (int64_t)bits;
}

static LLVMMetadataRef debug_enum_type(LlvmBackend *backend, const CogIrType *type)
{
    const CogIrType *backing = cog_ir_get_type(backend->ir, type->as.enumeration.backing_type);
    LLVMMetadataRef backing_debug = debug_type(backend, type->as.enumeration.backing_type);
    LLVMTypeRef llvm_backing = llvm_lower_type(backend, type->as.enumeration.backing_type);
    if (!backing || !backing_debug || !llvm_backing)
        return NULL;

    LLVMMetadataRef *members = type->as.enumeration.member_count
        ? calloc(type->as.enumeration.member_count, sizeof(*members)) : NULL;
    if (type->as.enumeration.member_count && !members) {
        llvm_backend_error(backend, "out of memory creating LLVM debug enum members");
        return NULL;
    }
    for (size_t i = 0; i < type->as.enumeration.member_count; ++i) {
        const CogIrEnumMember *member = &type->as.enumeration.members[i];
        int is_unsigned = backing->kind == COG_IR_TYPE_INTEGER && !backing->as.integer.is_signed;
        int64_t value = is_unsigned
            ? (int64_t)member->bits
            : signed_enum_value(member->bits, backing->as.integer.bits);
        members[i] = LLVMDIBuilderCreateEnumerator(
            backend->di_builder,
            member->debug_name.data,
            member->debug_name.length,
            value,
            is_unsigned
        );
    }

    LLVMMetadataRef file = debug_file_for_span(backend, type->span);
    if (!file)
        file = primary_debug_file(backend);
    LLVMMetadataRef result = LLVMDIBuilderCreateEnumerationType(
        backend->di_builder,
        file,
        type->debug_name.data,
        type->debug_name.length,
        file,
        type->span.line,
        llvm_type_size_bits(backend, llvm_backing),
        llvm_type_align_bits(backend, llvm_backing),
        members,
        (unsigned)type->as.enumeration.member_count,
        backing_debug
    );
    free(members);
    return result;
}

static LLVMTypeRef aggregate_storage_type(LlvmBackend *backend, const CogIrType *type)
{
    return llvm_lower_type(backend, type->id);
}

static LLVMTypeRef aggregate_field_storage_type(
    LlvmBackend *backend,
    const CogIrType *aggregate,
    const CogIrAggregateField *field
) {
    if (aggregate->as.aggregate.is_repr_c) {
        if (field->abi_type == COG_IR_ABI_TYPE_INVALID) {
            llvm_backend_error(backend, "#repr(c) debug field is missing frozen C object ABI metadata");
            return NULL;
        }
        return llvm_lower_c_object_type(backend, field->abi_type);
    }
    return llvm_lower_type(backend, field->type);
}

static LLVMMetadataRef debug_aggregate_type(LlvmBackend *backend, const CogIrType *type)
{
    unsigned tag = type->kind == COG_IR_TYPE_STRUCT
        ? COG_DW_TAG_STRUCTURE_TYPE : COG_DW_TAG_UNION_TYPE;
    LLVMMetadataRef file = debug_file_for_span(backend, type->span);
    if (!file)
        file = primary_debug_file(backend);

    if (!type->as.aggregate.is_complete || type->as.aggregate.is_incomplete) {
        return LLVMDIBuilderCreateForwardDecl(
            backend->di_builder,
            tag,
            type->debug_name.data,
            type->debug_name.length,
            file,
            file,
            type->span.line,
            0,
            0,
            0,
            "",
            0
        );
    }

    LLVMTypeRef storage = aggregate_storage_type(backend, type);
    if (!storage)
        return NULL;
    uint64_t size_bits = llvm_type_size_bits(backend, storage);
    uint32_t align_bits = llvm_type_align_bits(backend, storage);

    char unique_id[64];
    int unique_length = snprintf(unique_id, sizeof(unique_id), "coglet.type.%u", type->id);
    if (unique_length < 0 || (size_t)unique_length >= sizeof(unique_id)) {
        llvm_backend_error(backend, "could not create LLVM debug aggregate identity");
        return NULL;
    }

    LLVMMetadataRef temporary = LLVMDIBuilderCreateReplaceableCompositeType(
        backend->di_builder,
        tag,
        type->debug_name.data,
        type->debug_name.length,
        file,
        file,
        type->span.line,
        0,
        size_bits,
        align_bits,
        LLVMDIFlagZero,
        unique_id,
        (size_t)unique_length
    );
    if (!temporary)
        return NULL;
    backend->di_types[type->id] = temporary;

    LLVMMetadataRef *members = type->as.aggregate.field_count
        ? calloc(type->as.aggregate.field_count, sizeof(*members)) : NULL;
    if (type->as.aggregate.field_count && !members) {
        llvm_backend_error(backend, "out of memory creating LLVM debug aggregate members");
        return NULL;
    }

    LLVMTypeRef struct_layout = NULL;
    if (type->kind == COG_IR_TYPE_STRUCT) {
        struct_layout = type->as.aggregate.is_repr_c
            ? llvm_repr_c_inner_type(backend, type->id)
            : storage;
        if (!struct_layout) {
            free(members);
            return NULL;
        }
    }

    for (size_t i = 0; i < type->as.aggregate.field_count; ++i) {
        const CogIrAggregateField *field = &type->as.aggregate.fields[i];
        LLVMMetadataRef field_type = field->abi_type != COG_IR_ABI_TYPE_INVALID
            ? debug_abi_type(backend, field->abi_type)
            : debug_type(backend, field->type);
        LLVMTypeRef field_storage = aggregate_field_storage_type(backend, type, field);
        if (!field_type || !field_storage) {
            free(members);
            return NULL;
        }
        uint64_t offset_bits = 0;
        if (type->kind == COG_IR_TYPE_STRUCT) {
            uint64_t offset = type->as.aggregate.is_repr_c
                ? llvm_repr_c_field_offset(backend, type, i)
                : LLVMOffsetOfElement(backend->target_data, struct_layout, (unsigned)i);
            if (offset == UINT64_MAX) {
                free(members);
                return NULL;
            }
            offset_bits = offset * UINT64_C(8);
        }
        LLVMMetadataRef field_file = debug_file_for_span(backend, field->span);
        if (!field_file)
            field_file = file;
        members[i] = LLVMDIBuilderCreateMemberType(
            backend->di_builder,
            temporary,
            field->debug_name.data,
            field->debug_name.length,
            field_file,
            field->span.line,
            llvm_type_size_bits(backend, field_storage),
            llvm_type_align_bits(backend, field_storage),
            offset_bits,
            LLVMDIFlagZero,
            field_type
        );
        if (!members[i]) {
            free(members);
            return NULL;
        }
    }

    LLVMMetadataRef result = type->kind == COG_IR_TYPE_STRUCT
        ? LLVMDIBuilderCreateStructType(
            backend->di_builder,
            file,
            type->debug_name.data,
            type->debug_name.length,
            file,
            type->span.line,
            size_bits,
            align_bits,
            LLVMDIFlagZero,
            NULL,
            members,
            (unsigned)type->as.aggregate.field_count,
            0,
            NULL,
            unique_id,
            (size_t)unique_length
        )
        : LLVMDIBuilderCreateUnionType(
            backend->di_builder,
            file,
            type->debug_name.data,
            type->debug_name.length,
            file,
            type->span.line,
            size_bits,
            align_bits,
            LLVMDIFlagZero,
            members,
            (unsigned)type->as.aggregate.field_count,
            0,
            unique_id,
            (size_t)unique_length
        );
    free(members);
    if (!result)
        return NULL;

    /* LLVMMetadataReplaceAllUsesWith also deletes the temporary node. */
    LLVMMetadataReplaceAllUsesWith(temporary, result);
    backend->di_types[type->id] = result;
    return result;
}

static LLVMMetadataRef debug_type(LlvmBackend *backend, CogIrTypeId id)
{
    if (id == COG_IR_TYPE_INVALID || (size_t)id >= backend->ir->type_count) {
        llvm_backend_error(backend, "invalid CogIR type id in debug metadata");
        return NULL;
    }
    if (backend->di_types[id])
        return backend->di_types[id];

    const CogIrType *type = cog_ir_get_type(backend->ir, id);
    if (!type)
        return NULL;

    LLVMMetadataRef result = NULL;
    char name[64];
    builtin_type_name(type, name, sizeof(name));

    switch (type->kind) {
        case COG_IR_TYPE_VOID:
            result = LLVMDIBuilderCreateUnspecifiedType(backend->di_builder, "void", 4);
            break;
        case COG_IR_TYPE_BOOL: {
            LLVMTypeRef storage = llvm_lower_type(backend, id);
            result = storage ? LLVMDIBuilderCreateBasicType(
                backend->di_builder,
                name,
                strlen(name),
                llvm_type_size_bits(backend, storage),
                COG_DW_ATE_BOOLEAN,
                LLVMDIFlagZero
            ) : NULL;
            break;
        }
        case COG_IR_TYPE_INTEGER:
            result = LLVMDIBuilderCreateBasicType(
                backend->di_builder,
                name,
                strlen(name),
                type->as.integer.bits,
                type->as.integer.is_signed ? COG_DW_ATE_SIGNED : COG_DW_ATE_UNSIGNED,
                LLVMDIFlagZero
            );
            break;
        case COG_IR_TYPE_FLOAT:
            result = LLVMDIBuilderCreateBasicType(
                backend->di_builder,
                name,
                strlen(name),
                type->as.floating.bits,
                COG_DW_ATE_FLOAT,
                LLVMDIFlagZero
            );
            break;
        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER: {
            LLVMMetadataRef pointee = qualified_pointee_type(backend, type);
            LLVMTypeRef pointer = llvm_lower_type(backend, id);
            result = pointee && pointer ? LLVMDIBuilderCreatePointerType(
                backend->di_builder,
                pointee,
                llvm_type_size_bits(backend, pointer),
                llvm_type_align_bits(backend, pointer),
                0,
                "",
                0
            ) : NULL;
            break;
        }
        case COG_IR_TYPE_ARRAY: {
            LLVMMetadataRef element = debug_type(backend, type->as.array.element_type);
            LLVMTypeRef storage = llvm_lower_type(backend, id);
            LLVMMetadataRef subrange = LLVMDIBuilderGetOrCreateSubrange(
                backend->di_builder,
                0,
                (int64_t)type->as.array.length
            );
            result = element && storage && subrange ? LLVMDIBuilderCreateArrayType(
                backend->di_builder,
                llvm_type_size_bits(backend, storage),
                llvm_type_align_bits(backend, storage),
                element,
                &subrange,
                1
            ) : NULL;
            break;
        }
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
            return debug_aggregate_type(backend, type);
        case COG_IR_TYPE_ENUM:
            result = debug_enum_type(backend, type);
            break;
        case COG_IR_TYPE_FUNCTION: {
            LLVMMetadataRef subroutine = debug_subroutine_type(backend, type);
            LLVMTypeRef pointer = llvm_lower_type(backend, id);
            result = subroutine && pointer ? LLVMDIBuilderCreatePointerType(
                backend->di_builder,
                subroutine,
                llvm_type_size_bits(backend, pointer),
                llvm_type_align_bits(backend, pointer),
                0,
                "",
                0
            ) : NULL;
            break;
        }
    }

    if (!result && !backend->had_error)
        llvm_backend_error(backend, "could not create LLVM debug type metadata");
    backend->di_types[id] = result;
    return result;
}

int llvm_debug_declare_global(LlvmBackend *backend, const CogIrGlobal *global, LLVMValueRef value)
{
    if (!backend->debug_info || global->is_compiler_generated)
        return 1;
    LLVMMetadataRef type = global->abi_type != COG_IR_ABI_TYPE_INVALID
        ? debug_abi_type(backend, global->abi_type)
        : debug_type(backend, global->type);
    LLVMMetadataRef file = debug_file_for_span(backend, global->span);
    if (!type || !file)
        return 0;

    size_t linkage_length = 0;
    const char *linkage = LLVMGetValueName2(value, &linkage_length);
    LLVMTypeRef storage = global->abi_type != COG_IR_ABI_TYPE_INVALID
        ? llvm_lower_c_object_type(backend, global->abi_type)
        : llvm_lower_type(backend, global->type);
    if (!storage)
        return 0;

    LLVMMetadataRef expression = LLVMDIBuilderCreateGlobalVariableExpression(
        backend->di_builder,
        backend->di_compile_unit,
        global->debug_name.data,
        global->debug_name.length,
        linkage,
        linkage_length,
        file,
        global->span.line,
        type,
        global->linkage == COG_IR_LINKAGE_INTERNAL,
        backend->di_expression,
        NULL,
        llvm_type_align_bits(backend, storage)
    );
    if (!expression)
        return 0;
    unsigned dbg_kind = LLVMGetMDKindIDInContext(backend->context, "dbg", 3);
    LLVMGlobalSetMetadata(value, dbg_kind, expression);
    return 1;
}

int llvm_debug_declare_function(LlvmBackend *backend, const CogIrFunction *function, LLVMValueRef value)
{
    if (!backend->debug_info || function->kind != COG_IR_FUNCTION_DEFINITION ||
        function->is_compiler_generated)
        return 1;

    const CogIrType *function_type = cog_ir_get_type(backend->ir, function->type);
    LLVMMetadataRef file = debug_file_for_span(backend, function->span);
    LLVMMetadataRef subroutine = function->abi.abi == COG_IR_ABI_C &&
        function->abi.return_abi_type != COG_IR_ABI_TYPE_INVALID
        ? debug_abi_subroutine_type(backend, &(CogIrAbiType){
            .kind = COG_IR_ABI_TYPE_FUNCTION,
            .return_type = function->abi.return_abi_type,
            .parameter_types = function->abi.parameter_abi_types,
            .parameter_count = function->abi.parameter_count,
        })
        : debug_subroutine_type(backend, function_type);
    if (!function_type || !file || !subroutine)
        return 0;

    size_t linkage_length = 0;
    const char *linkage = LLVMGetValueName2(value, &linkage_length);
    LLVMMetadataRef subprogram = LLVMDIBuilderCreateFunction(
        backend->di_builder,
        file,
        function->debug_name.data,
        function->debug_name.length,
        linkage,
        linkage_length,
        file,
        function->span.line,
        subroutine,
        function->linkage == COG_IR_LINKAGE_INTERNAL,
        1,
        function->span.line,
        LLVMDIFlagPrototyped,
        backend->optimization_level != COG_OPTIMIZATION_LEVEL_0
    );
    if (!subprogram)
        return 0;
    backend->di_subprograms[function->id] = subprogram;
    LLVMSetSubprogram(value, subprogram);
    return 1;
}

static LLVMMetadataRef debug_location_for_slot(
    LlvmBackend *backend,
    const CogIrFunction *function,
    const CogIrSlot *slot
) {
    LLVMMetadataRef scope = backend->di_subprograms[function->id];
    if (!scope)
        return NULL;
    return LLVMDIBuilderCreateDebugLocation(
        backend->context,
        slot->span.line,
        slot->span.column,
        scope,
        NULL
    );
}

int llvm_debug_declare_slots(
    LlvmBackend *backend,
    const CogIrFunction *function,
    const LlvmFunctionState *state
) {
    if (!backend->debug_info || !backend->di_subprograms[function->id])
        return 1;

    LLVMBasicBlockRef entry = state->blocks[function->entry_block];
    for (size_t i = 0; i < function->slot_count; ++i) {
        const CogIrSlot *slot = &function->slots[i];
        if (slot->kind == COG_IR_SLOT_COMPILER_TEMP)
            continue;

        LLVMMetadataRef file = debug_file_for_span(backend, slot->span);
        LLVMMetadataRef type = slot->abi_type != COG_IR_ABI_TYPE_INVALID
            ? debug_abi_type(backend, slot->abi_type)
            : debug_type(backend, slot->type);
        LLVMMetadataRef location = debug_location_for_slot(backend, function, slot);
        if (!file || !type || !location)
            return 0;

        LLVMMetadataRef variable = NULL;
        if (slot->kind == COG_IR_SLOT_SOURCE_PARAMETER) {
            variable = LLVMDIBuilderCreateParameterVariable(
                backend->di_builder,
                backend->di_subprograms[function->id],
                slot->debug_name.data,
                slot->debug_name.length,
                (unsigned)slot->parameter_index + 1,
                file,
                slot->span.line,
                type,
                1,
                LLVMDIFlagZero
            );
        } else {
            LLVMTypeRef storage = slot->abi_type != COG_IR_ABI_TYPE_INVALID
                ? llvm_lower_c_object_type(backend, slot->abi_type)
                : llvm_lower_type(backend, slot->type);
            if (!storage)
                return 0;
            variable = LLVMDIBuilderCreateAutoVariable(
                backend->di_builder,
                backend->di_subprograms[function->id],
                slot->debug_name.data,
                slot->debug_name.length,
                file,
                slot->span.line,
                type,
                1,
                LLVMDIFlagZero,
                llvm_type_align_bits(backend, storage)
            );
        }
        if (!variable)
            return 0;

#if COGLET_LLVM_VERSION_MAJOR >= 19
        if (!LLVMDIBuilderInsertDeclareRecordAtEnd(
                backend->di_builder,
                state->slots[i],
                variable,
                backend->di_expression,
                location,
                entry)) {
            llvm_backend_error(backend, "could not attach LLVM debug variable record");
            return 0;
        }
#else
        if (!LLVMDIBuilderInsertDeclareAtEnd(
                backend->di_builder,
                state->slots[i],
                variable,
                backend->di_expression,
                location,
                entry)) {
            llvm_backend_error(backend, "could not attach LLVM debug variable intrinsic");
            return 0;
        }
#endif
    }
    return 1;
}

void llvm_debug_set_location(LlvmBackend *backend, const CogIrFunction *function, SourceSpan span)
{
    if (!backend->debug_info || !source_span_is_valid(span) ||
        !backend->di_subprograms[function->id]) {
        llvm_debug_clear_location(backend);
        return;
    }
    LLVMMetadataRef location = LLVMDIBuilderCreateDebugLocation(
        backend->context,
        span.line,
        span.column,
        backend->di_subprograms[function->id],
        NULL
    );
    LLVMSetCurrentDebugLocation2(backend->builder, location);
}

void llvm_debug_clear_location(LlvmBackend *backend)
{
    if (backend && backend->builder)
        LLVMSetCurrentDebugLocation2(backend->builder, NULL);
}
