#define _POSIX_C_SOURCE 200809L

#include "backend_c.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define C_BACKEND_NAME_SIZE 48

typedef struct CBackend {
    FILE *out;
    const CogIrModule *module;
    int had_error;

    char (*type_names)[C_BACKEND_NAME_SIZE];
    char (*abi_names)[C_BACKEND_NAME_SIZE];
    char (*function_names)[C_BACKEND_NAME_SIZE];
    char (*global_names)[C_BACKEND_NAME_SIZE];

    unsigned char *runtime_alias_state;
    unsigned char *abi_alias_state;
    unsigned char *runtime_definition_state;

    char **type_definitions;
    size_t type_definition_count;
    size_t type_definition_capacity;
    size_t temporary_id;
} CBackend;

static const char *source_filename_for_span(const CBackend *backend, SourceSpan span)
{
    if (!backend || !backend->module)
        return "<cogir>";

    const SourceFile *source = source_manager_get(&backend->module->sources, span.file_id);
    return source && source->filename ? source->filename : "<cogir>";
}

static void backend_error(CBackend *backend, SourceSpan span, const char *message)
{
    if (!backend)
        return;

    fprintf(
        stderr,
        "%s:%u: backend error: %s\n",
        source_filename_for_span(backend, span),
        source_span_is_valid(span) ? span.line : 0,
        message
    );
    backend->had_error = 1;
}

static const char *c_call_macro_name(CogIrCallingConvention convention)
{
    switch (convention) {
        case COG_IR_CALL_DEFAULT: return "";
        case COG_IR_CALL_CDECL:   return "CG_CALL_CDECL";
        case COG_IR_CALL_STDCALL: return "CG_CALL_STDCALL";
        case COG_IR_CALL_SYSV64:  return "CG_CALL_SYSV64";
        case COG_IR_CALL_WIN64:   return "CG_CALL_WIN64";
    }
    return "";
}

static int append_type_definition(CBackend *backend, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return 0;
    }

    char *definition = malloc((size_t)length + 1);
    if (!definition) {
        va_end(args);
        return 0;
    }
    vsnprintf(definition, (size_t)length + 1, format, args);
    va_end(args);

    if (backend->type_definition_count == backend->type_definition_capacity) {
        size_t capacity = backend->type_definition_capacity
            ? backend->type_definition_capacity * 2
            : 32;
        char **definitions = realloc(
            backend->type_definitions,
            capacity * sizeof(*definitions)
        );
        if (!definitions) {
            free(definition);
            return 0;
        }
        backend->type_definitions = definitions;
        backend->type_definition_capacity = capacity;
    }

    backend->type_definitions[backend->type_definition_count++] = definition;
    return 1;
}

static void free_backend_storage(CBackend *backend)
{
    if (!backend)
        return;

    for (size_t i = 0; i < backend->type_definition_count; ++i)
        free(backend->type_definitions[i]);
    free(backend->type_definitions);
    free(backend->type_names);
    free(backend->abi_names);
    free(backend->function_names);
    free(backend->global_names);
    free(backend->runtime_alias_state);
    free(backend->abi_alias_state);
    free(backend->runtime_definition_state);
}

static int init_backend_storage(CBackend *backend)
{
    const CogIrModule *module = backend->module;

    if (module->type_count) {
        backend->type_names = calloc(module->type_count, sizeof(*backend->type_names));
        backend->runtime_alias_state = calloc(module->type_count, 1);
        backend->runtime_definition_state = calloc(module->type_count, 1);
        if (!backend->type_names || !backend->runtime_alias_state ||
            !backend->runtime_definition_state)
            return 0;
    }

    if (module->abi_type_count) {
        backend->abi_names = calloc(module->abi_type_count, sizeof(*backend->abi_names));
        backend->abi_alias_state = calloc(module->abi_type_count, 1);
        if (!backend->abi_names || !backend->abi_alias_state)
            return 0;
    }

    if (module->function_count) {
        backend->function_names = calloc(module->function_count, sizeof(*backend->function_names));
        if (!backend->function_names)
            return 0;
        for (size_t i = 0; i < module->function_count; ++i)
            snprintf(backend->function_names[i], C_BACKEND_NAME_SIZE, "cg_fn_%zu", i);
    }

    if (module->global_count) {
        backend->global_names = calloc(module->global_count, sizeof(*backend->global_names));
        if (!backend->global_names)
            return 0;
        for (size_t i = 0; i < module->global_count; ++i)
            snprintf(backend->global_names[i], C_BACKEND_NAME_SIZE, "cg_g_%zu", i);
    }

    return 1;
}

static const char *c_scalar_name(CogIrCScalarKind scalar)
{
    switch (scalar) {
        case COG_IR_C_SCALAR_CHAR:      return "char";
        case COG_IR_C_SCALAR_SCHAR:     return "signed char";
        case COG_IR_C_SCALAR_UCHAR:     return "unsigned char";
        case COG_IR_C_SCALAR_SHORT:     return "short";
        case COG_IR_C_SCALAR_USHORT:    return "unsigned short";
        case COG_IR_C_SCALAR_INT:       return "int";
        case COG_IR_C_SCALAR_UINT:      return "unsigned int";
        case COG_IR_C_SCALAR_LONG:      return "long";
        case COG_IR_C_SCALAR_ULONG:     return "unsigned long";
        case COG_IR_C_SCALAR_LONGLONG:  return "long long";
        case COG_IR_C_SCALAR_ULONGLONG: return "unsigned long long";
        case COG_IR_C_SCALAR_SIZE:      return "size_t";
        case COG_IR_C_SCALAR_BOOL:      return "_Bool";
        case COG_IR_C_SCALAR_FLOAT:     return "float";
        case COG_IR_C_SCALAR_DOUBLE:    return "double";
        case COG_IR_C_SCALAR_NONE:      return NULL;
    }
    return NULL;
}

static const char *runtime_type_name(CBackend *backend, CogIrTypeId type_id, SourceSpan span);
static const char *abi_type_name(CBackend *backend, CogIrAbiTypeId abi_id, SourceSpan span);
static char *constant_expr(CBackend *backend, CogIrConstId constant_id);
static char *constant_storage_initializer(CBackend *backend, CogIrConstId constant_id, int array_value_wrapper);

static const char *nominal_type_name(CBackend *backend, const CogIrType *type)
{
    assert(type);
    assert((size_t)type->id < backend->module->type_count);

    char *name = backend->type_names[type->id];
    if (name[0])
        return name;

    const char *prefix = NULL;
    switch (type->kind) {
        case COG_IR_TYPE_STRUCT: prefix = "cg_struct_"; break;
        case COG_IR_TYPE_UNION:  prefix = "cg_union_"; break;
        case COG_IR_TYPE_ENUM:   prefix = "cg_enum_"; break;
        default: return NULL;
    }
    snprintf(name, C_BACKEND_NAME_SIZE, "%s%u", prefix, type->id);
    return name;
}

static const char *runtime_integer_name(const CogIrType *type)
{
    if (!type || type->kind != COG_IR_TYPE_INTEGER)
        return NULL;

    switch (type->as.integer.bits) {
        case 8:  return type->as.integer.is_signed ? "int8_t" : "uint8_t";
        case 16: return type->as.integer.is_signed ? "int16_t" : "uint16_t";
        case 32: return type->as.integer.is_signed ? "int32_t" : "uint32_t";
        case 64: return type->as.integer.is_signed ? "int64_t" : "uint64_t";
        default: return NULL;
    }
}

static int append_parameter_type_list_runtime(
    CBackend *backend,
    char **buffer,
    size_t *length,
    size_t *capacity,
    const CogIrFunctionType *function,
    SourceSpan span
);

static int append_text(char **buffer, size_t *length, size_t *capacity, const char *text)
{
    size_t text_length = strlen(text);
    size_t required = *length + text_length + 1;
    if (required > *capacity) {
        size_t next = *capacity ? *capacity : 64;
        while (next < required)
            next *= 2;
        char *grown = realloc(*buffer, next);
        if (!grown)
            return 0;
        *buffer = grown;
        *capacity = next;
    }
    memcpy(*buffer + *length, text, text_length + 1);
    *length += text_length;
    return 1;
}

static const char *array_value_type_name(CBackend *backend, const CogIrType *type)
{
    assert(type && type->kind == COG_IR_TYPE_ARRAY);
    assert((size_t)type->id < backend->module->type_count);
    char *name = backend->type_names[type->id];
    if (!name[0])
        snprintf(name, C_BACKEND_NAME_SIZE, "cg_array_%u", type->id);
    return name;
}

static int append_array_dimensions_runtime(
    CBackend *backend,
    CogIrTypeId array_type_id,
    char **buffer,
    size_t *length,
    size_t *capacity,
    CogIrTypeId *out_leaf,
    SourceSpan span
) {
    CogIrTypeId current = array_type_id;
    for (;;) {
        const CogIrType *type = cog_ir_get_type(backend->module, current);
        if (!type) {
            backend_error(backend, span, "invalid array type while generating C declarator");
            return 0;
        }
        if (type->kind != COG_IR_TYPE_ARRAY) {
            *out_leaf = current;
            return 1;
        }
        char dimension[48];
        int written = snprintf(dimension, sizeof(dimension), "[%zu]", type->as.array.length);
        if (written < 0 || (size_t)written >= sizeof(dimension) ||
            !append_text(buffer, length, capacity, dimension)) {
            backend_error(backend, span, "out of memory while generating array declarator");
            return 0;
        }
        current = type->as.array.element_type;
    }
}

static char *runtime_array_declarator(
    CBackend *backend,
    CogIrTypeId array_type_id,
    const char *name,
    int pointer_to_array,
    int is_readonly,
    int is_volatile,
    SourceSpan span
) {
    char *dimensions = NULL;
    size_t dimensions_length = 0, dimensions_capacity = 0;
    CogIrTypeId leaf_id = COG_IR_TYPE_INVALID;
    if (!append_array_dimensions_runtime(
            backend, array_type_id, &dimensions, &dimensions_length,
            &dimensions_capacity, &leaf_id, span)) {
        free(dimensions);
        return NULL;
    }

    const char *leaf = runtime_type_name(backend, leaf_id, span);
    if (!leaf) {
        free(dimensions);
        return NULL;
    }

    char *result = NULL;
    size_t length = 0, capacity = 0;
    int ok = append_text(&result, &length, &capacity, leaf);
    if (ok && is_readonly) ok = append_text(&result, &length, &capacity, " const");
    if (ok && is_volatile) ok = append_text(&result, &length, &capacity, " volatile");
    if (ok) ok = append_text(&result, &length, &capacity, pointer_to_array ? " (*" : " ");
    if (ok) ok = append_text(&result, &length, &capacity, name);
    if (ok && pointer_to_array) ok = append_text(&result, &length, &capacity, ")");
    if (ok) ok = append_text(&result, &length, &capacity, dimensions ? dimensions : "");
    free(dimensions);
    if (!ok) {
        free(result);
        backend_error(backend, span, "out of memory while generating array declarator");
        return NULL;
    }
    return result;
}

static char *abi_array_declarator(
    CBackend *backend,
    CogIrAbiTypeId abi_array_id,
    CogIrTypeId runtime_array_id,
    const char *name,
    int pointer_to_array,
    int is_readonly,
    int is_volatile,
    SourceSpan span
) {
    char *dimensions = NULL;
    size_t dimensions_length = 0, dimensions_capacity = 0;
    CogIrAbiTypeId abi_id = abi_array_id;
    CogIrTypeId runtime_id = runtime_array_id;

    for (;;) {
        const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, abi_id);
        const CogIrType *runtime = cog_ir_get_type(backend->module, runtime_id);
        if (!abi || !runtime) {
            free(dimensions);
            backend_error(backend, span, "invalid ABI array type while generating C declarator");
            return NULL;
        }
        if (abi->kind != COG_IR_ABI_TYPE_ARRAY) {
            if (runtime->kind == COG_IR_TYPE_ARRAY || abi->runtime_type != runtime_id) {
                free(dimensions);
                backend_error(backend, span, "ABI/runtime array shape mismatch during C lowering");
                return NULL;
            }
            break;
        }
        if (runtime->kind != COG_IR_TYPE_ARRAY || abi->runtime_type != runtime_id) {
            free(dimensions);
            backend_error(backend, span, "ABI/runtime array shape mismatch during C lowering");
            return NULL;
        }
        char dimension[48];
        int written = snprintf(dimension, sizeof(dimension), "[%zu]", runtime->as.array.length);
        if (written < 0 || (size_t)written >= sizeof(dimension) ||
            !append_text(&dimensions, &dimensions_length, &dimensions_capacity, dimension)) {
            free(dimensions);
            backend_error(backend, span, "out of memory while generating ABI array declarator");
            return NULL;
        }
        abi_id = abi->element_type;
        runtime_id = runtime->as.array.element_type;
    }

    const char *leaf = abi_type_name(backend, abi_id, span);
    if (!leaf) {
        free(dimensions);
        return NULL;
    }
    char *result = NULL;
    size_t length = 0, capacity = 0;
    int ok = append_text(&result, &length, &capacity, leaf);
    if (ok && is_readonly) ok = append_text(&result, &length, &capacity, " const");
    if (ok && is_volatile) ok = append_text(&result, &length, &capacity, " volatile");
    if (ok) ok = append_text(&result, &length, &capacity, pointer_to_array ? " (*" : " ");
    if (ok) ok = append_text(&result, &length, &capacity, name);
    if (ok && pointer_to_array) ok = append_text(&result, &length, &capacity, ")");
    if (ok) ok = append_text(&result, &length, &capacity, dimensions ? dimensions : "");
    free(dimensions);
    if (!ok) {
        free(result);
        backend_error(backend, span, "out of memory while generating ABI array declarator");
        return NULL;
    }
    return result;
}

static const char *runtime_type_name(CBackend *backend, CogIrTypeId type_id, SourceSpan span)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    if (!type) {
        backend_error(backend, span, "invalid CogIR runtime type during C lowering");
        return NULL;
    }

    switch (type->kind) {
        case COG_IR_TYPE_VOID: return "void";
        case COG_IR_TYPE_BOOL: return "_Bool";
        case COG_IR_TYPE_INTEGER: {
            const char *name = runtime_integer_name(type);
            if (!name)
                backend_error(backend, span, "integer width is not supported by the current host-C backend");
            return name;
        }
        case COG_IR_TYPE_FLOAT:
            if (type->as.floating.bits == 32) return "float";
            if (type->as.floating.bits == 64) return "double";
            backend_error(backend, span, "floating width is not supported by the current host-C backend");
            return NULL;
        case COG_IR_TYPE_STRUCT:
        case COG_IR_TYPE_UNION:
        case COG_IR_TYPE_ENUM:
            return nominal_type_name(backend, type);
        case COG_IR_TYPE_ARRAY:
            return array_value_type_name(backend, type);
        case COG_IR_TYPE_POINTER:
        case COG_IR_TYPE_OPAQUE_POINTER:
        case COG_IR_TYPE_FUNCTION:
            break;
    }

    unsigned char *state = &backend->runtime_alias_state[type_id];
    if (*state == 2)
        return backend->type_names[type_id];
    if (*state == 1) {
        backend_error(backend, span, "recursive runtime C type alias during CogIR lowering");
        return NULL;
    }
    *state = 1;

    char *name = backend->type_names[type_id];
    snprintf(name, C_BACKEND_NAME_SIZE, "cg_rt_%u", type_id);

    if (type->kind == COG_IR_TYPE_POINTER) {
        const CogIrType *pointee_type = cog_ir_get_type(backend->module, type->as.pointer.pointee);
        if (!pointee_type) {
            backend_error(backend, span, "runtime pointer has invalid pointee type");
            return NULL;
        }
        if (pointee_type->kind == COG_IR_TYPE_ARRAY) {
            char *declarator = runtime_array_declarator(
                backend, pointee_type->id, name, 1,
                type->as.pointer.is_readonly, type->as.pointer.is_volatile, span);
            if (!declarator || !append_type_definition(backend, "typedef %s;", declarator)) {
                free(declarator);
                backend_error(backend, span, "out of memory while generating array-pointer alias");
                return NULL;
            }
            free(declarator);
        } else {
            const char *pointee = runtime_type_name(backend, type->as.pointer.pointee, span);
            if (!pointee)
                return NULL;
            if (!append_type_definition(
                    backend,
                    "typedef %s%s%s *%s;",
                    pointee,
                    type->as.pointer.is_readonly ? " const" : "",
                    type->as.pointer.is_volatile ? " volatile" : "",
                    name)) {
                backend_error(backend, span, "out of memory while generating runtime pointer alias");
                return NULL;
            }
        }
    } else if (type->kind == COG_IR_TYPE_OPAQUE_POINTER) {
        if (!append_type_definition(
                backend,
                "typedef void%s%s *%s;",
                type->as.opaque_pointer.is_readonly ? " const" : "",
                type->as.opaque_pointer.is_volatile ? " volatile" : "",
                name)) {
            backend_error(backend, span, "out of memory while generating opaque pointer alias");
            return NULL;
        }
    } else {
        const CogIrFunctionType *function = &type->as.function;
        if (function->abi != COG_IR_ABI_C) {
            backend_error(backend, span, "ordinary Coglet function values are not lowered as native callbacks");
            return NULL;
        }
        const char *result = runtime_type_name(backend, function->result_type, span);
        if (!result)
            return NULL;

        char *params = NULL;
        size_t params_length = 0, params_capacity = 0;
        if (!append_parameter_type_list_runtime(
                backend, &params, &params_length, &params_capacity, function, span)) {
            free(params);
            return NULL;
        }
        const char *macro = c_call_macro_name(function->calling_convention);
        int ok;
        if (function->calling_convention == COG_IR_CALL_DEFAULT) {
            ok = append_type_definition(
                backend, "typedef %s (*%s)(%s);", result, name, params);
        } else {
            ok = append_type_definition(
                backend, "typedef %s (%s *%s)(%s);", result, macro, name, params);
        }
        free(params);
        if (!ok) {
            backend_error(backend, span, "out of memory while generating callback alias");
            return NULL;
        }
    }

    *state = 2;
    return name;
}

static int append_parameter_type_list_runtime(
    CBackend *backend,
    char **buffer,
    size_t *length,
    size_t *capacity,
    const CogIrFunctionType *function,
    SourceSpan span
) {
    if (function->parameter_count == 0 && !function->is_variadic)
        return append_text(buffer, length, capacity, "void");

    for (size_t i = 0; i < function->parameter_count; ++i) {
        const char *type = runtime_type_name(backend, function->parameter_types[i], span);
        if (!type)
            return 0;
        if (i && !append_text(buffer, length, capacity, ", ")) return 0;
        if (!append_text(buffer, length, capacity, type)) return 0;
    }
    if (function->is_variadic) {
        if (function->parameter_count && !append_text(buffer, length, capacity, ", ")) return 0;
        if (!append_text(buffer, length, capacity, "...")) return 0;
    }
    return 1;
}

static int append_parameter_type_list_abi(
    CBackend *backend,
    char **buffer,
    size_t *length,
    size_t *capacity,
    const CogIrAbiType *abi,
    const CogIrFunctionType *runtime,
    SourceSpan span
) {
    if (abi->parameter_count == 0 && !runtime->is_variadic)
        return append_text(buffer, length, capacity, "void");

    for (size_t i = 0; i < abi->parameter_count; ++i) {
        const char *type = abi_type_name(backend, abi->parameter_types[i], span);
        if (!type)
            return 0;
        if (i && !append_text(buffer, length, capacity, ", ")) return 0;
        if (!append_text(buffer, length, capacity, type)) return 0;
    }
    if (runtime->is_variadic) {
        if (abi->parameter_count && !append_text(buffer, length, capacity, ", ")) return 0;
        if (!append_text(buffer, length, capacity, "...")) return 0;
    }
    return 1;
}

static const char *abi_type_name(CBackend *backend, CogIrAbiTypeId abi_id, SourceSpan span)
{
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, abi_id);
    if (!abi) {
        backend_error(backend, span, "invalid CogIR ABI type during C lowering");
        return NULL;
    }

    if (abi->kind == COG_IR_ABI_TYPE_SEMANTIC)
        return runtime_type_name(backend, abi->runtime_type, span);

    if (abi->kind == COG_IR_ABI_TYPE_C_SCALAR) {
        const char *name = c_scalar_name(abi->c_scalar_kind);
        if (!name)
            backend_error(backend, span, "invalid native-C scalar ABI spelling in CogIR");
        return name;
    }

    if (abi->kind == COG_IR_ABI_TYPE_ARRAY) {
        backend_error(backend, span, "array ABI type requires a declarator in the host-C backend");
        return NULL;
    }

    unsigned char *state = &backend->abi_alias_state[abi_id];
    if (*state == 2)
        return backend->abi_names[abi_id];
    if (*state == 1) {
        backend_error(backend, span, "recursive ABI type alias during CogIR lowering");
        return NULL;
    }
    *state = 1;

    char *name = backend->abi_names[abi_id];
    snprintf(name, C_BACKEND_NAME_SIZE, "cg_abi_%u", abi_id);

    const CogIrType *runtime = cog_ir_get_type(backend->module, abi->runtime_type);
    if (!runtime) {
        backend_error(backend, span, "ABI type has invalid runtime type");
        return NULL;
    }

    if (abi->kind == COG_IR_ABI_TYPE_POINTER) {
        if (runtime->kind != COG_IR_TYPE_POINTER) {
            backend_error(backend, span, "pointer ABI type has non-pointer runtime type");
            return NULL;
        }
        const CogIrAbiType *element_abi = cog_ir_get_abi_type(backend->module, abi->element_type);
        const CogIrType *pointee = cog_ir_get_type(backend->module, runtime->as.pointer.pointee);
        if (!element_abi || !pointee) {
            backend_error(backend, span, "pointer ABI type has invalid element type");
            return NULL;
        }
        if (element_abi->kind == COG_IR_ABI_TYPE_ARRAY && pointee->kind == COG_IR_TYPE_ARRAY) {
            char *declarator = abi_array_declarator(
                backend, abi->element_type, pointee->id, name, 1,
                runtime->as.pointer.is_readonly, runtime->as.pointer.is_volatile, span);
            if (!declarator || !append_type_definition(backend, "typedef %s;", declarator)) {
                free(declarator);
                backend_error(backend, span, "out of memory while generating ABI array-pointer alias");
                return NULL;
            }
            free(declarator);
        } else {
            const char *element = abi_type_name(backend, abi->element_type, span);
            if (!element)
                return NULL;
            if (!append_type_definition(
                    backend,
                    "typedef %s%s%s *%s;",
                    element,
                    runtime->as.pointer.is_readonly ? " const" : "",
                    runtime->as.pointer.is_volatile ? " volatile" : "",
                    name)) {
                backend_error(backend, span, "out of memory while generating ABI pointer alias");
                return NULL;
            }
        }
    } else if (abi->kind == COG_IR_ABI_TYPE_OPAQUE_POINTER) {
        if (runtime->kind != COG_IR_TYPE_OPAQUE_POINTER) {
            backend_error(backend, span, "opaque-pointer ABI type has invalid runtime type");
            return NULL;
        }
        if (!append_type_definition(
                backend,
                "typedef void%s%s *%s;",
                runtime->as.opaque_pointer.is_readonly ? " const" : "",
                runtime->as.opaque_pointer.is_volatile ? " volatile" : "",
                name)) {
            backend_error(backend, span, "out of memory while generating ABI opaque pointer alias");
            return NULL;
        }
    } else if (abi->kind == COG_IR_ABI_TYPE_FUNCTION) {
        if (runtime->kind != COG_IR_TYPE_FUNCTION || runtime->as.function.abi != COG_IR_ABI_C) {
            backend_error(backend, span, "callback ABI type has invalid runtime function type");
            return NULL;
        }
        const char *result = abi_type_name(backend, abi->return_type, span);
        if (!result)
            return NULL;
        char *params = NULL;
        size_t params_length = 0, params_capacity = 0;
        if (!append_parameter_type_list_abi(
                backend, &params, &params_length, &params_capacity,
                abi, &runtime->as.function, span)) {
            free(params);
            return NULL;
        }
        const char *macro = c_call_macro_name(runtime->as.function.calling_convention);
        int ok;
        if (runtime->as.function.calling_convention == COG_IR_CALL_DEFAULT) {
            ok = append_type_definition(
                backend, "typedef %s (*%s)(%s);", result, name, params);
        } else {
            ok = append_type_definition(
                backend, "typedef %s (%s *%s)(%s);", result, macro, name, params);
        }
        free(params);
        if (!ok) {
            backend_error(backend, span, "out of memory while generating callback ABI alias");
            return NULL;
        }
    } else {
        backend_error(backend, span, "unsupported CogIR ABI type in host-C backend");
        return NULL;
    }

    *state = 2;
    return name;
}

static const char *value_type_name(
    CBackend *backend,
    const CogIrFunction *function,
    CogIrValueId value_id,
    SourceSpan span
) {
    const CogIrValue *value = cog_ir_get_value(function, value_id);
    if (!value) {
        backend_error(backend, span, "invalid CogIR value type during C lowering");
        return NULL;
    }
    if (value->abi_type != COG_IR_ABI_TYPE_INVALID) {
        const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, value->abi_type);
        /*
         * Address values may carry object-storage ABI metadata for native
         * backends. The host-C backend already gets pointer/array object layout
         * from the C type system, so retaining its historical runtime pointer
         * spelling avoids manufacturing premature pointer-to-array aliases.
         */
        if (abi && abi->kind != COG_IR_ABI_TYPE_POINTER && abi->kind != COG_IR_ABI_TYPE_ARRAY)
            return abi_type_name(backend, value->abi_type, span);
    }
    return runtime_type_name(backend, value->type, span);
}

static void emit_c_string_literal(FILE *out, StringView value)
{
    fputc('"', out);
    for (size_t i = 0; i < value.length; ++i) {
        unsigned char ch = (unsigned char)value.data[i];
        switch (ch) {
            case '\\': fputs("\\\\", out); break;
            case '"':  fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (ch >= 0x20 && ch <= 0x7e)
                    fputc((int)ch, out);
                else
                    fprintf(out, "\\%03o", ch);
                break;
        }
    }
    fputc('"', out);
}

static int uses_calling_convention(const CBackend *backend, CogIrCallingConvention convention)
{
    for (size_t i = 0; i < backend->module->function_count; ++i) {
        const CogIrFunction *function = &backend->module->functions[i];
        if (function->abi.abi == COG_IR_ABI_C &&
            function->abi.calling_convention == convention)
            return 1;
    }
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if (type->kind == COG_IR_TYPE_FUNCTION &&
            type->as.function.abi == COG_IR_ABI_C &&
            type->as.function.calling_convention == convention)
            return 1;
    }
    return 0;
}

static void emit_c_calling_convention_support(CBackend *backend)
{
    if (uses_calling_convention(backend, COG_IR_CALL_CDECL)) {
        fputs(
            "#if defined(_MSC_VER)\n"
            "#define CG_CALL_CDECL __cdecl\n"
            "#elif defined(__GNUC__) || defined(__clang__)\n"
            "#define CG_CALL_CDECL __attribute__((cdecl))\n"
            "#else\n"
            "#define CG_CALL_CDECL\n"
            "#endif\n\n",
            backend->out
        );
    }
    if (uses_calling_convention(backend, COG_IR_CALL_STDCALL)) {
        fputs(
            "#if defined(_MSC_VER) && defined(_M_IX86)\n"
            "#define CG_CALL_STDCALL __stdcall\n"
            "#elif (defined(__GNUC__) || defined(__clang__)) && defined(__i386__)\n"
            "#define CG_CALL_STDCALL __attribute__((stdcall))\n"
            "#elif defined(_WIN64) || defined(__x86_64__) || defined(__amd64__)\n"
            "#error \"Coglet host-C backend: call=stdcall requires 32-bit x86 (or the unified Win64 ABI)\"\n"
            "#define CG_CALL_STDCALL\n"
            "#else\n"
            "#error \"Coglet host-C backend: call=stdcall is unsupported by this native C compiler\"\n"
            "#define CG_CALL_STDCALL\n"
            "#endif\n\n",
            backend->out
        );
    }
    if (uses_calling_convention(backend, COG_IR_CALL_SYSV64)) {
        fputs(
            "#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__amd64__))\n"
            "#define CG_CALL_SYSV64 __attribute__((sysv_abi))\n"
            "#else\n"
            "#error \"Coglet host-C backend: call=sysv64 requires GNU-compatible x86-64 C attributes\"\n"
            "#define CG_CALL_SYSV64\n"
            "#endif\n\n",
            backend->out
        );
    }
    if (uses_calling_convention(backend, COG_IR_CALL_WIN64)) {
        fputs(
            "#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__amd64__))\n"
            "#define CG_CALL_WIN64 __attribute__((ms_abi))\n"
            "#else\n"
            "#error \"Coglet host-C backend: call=win64 requires GNU-compatible x86-64 C attributes\"\n"
            "#define CG_CALL_WIN64\n"
            "#endif\n\n",
            backend->out
        );
    }
}

static int has_repr_c_layout_controls(const CBackend *backend)
{
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if ((type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) &&
            type->as.aggregate.is_repr_c &&
            (type->as.aggregate.is_packed || type->as.aggregate.explicit_alignment > 0))
            return 1;
    }
    return 0;
}

static void emit_repr_c_layout_support(CBackend *backend)
{
    if (!has_repr_c_layout_controls(backend))
        return;
    fputs(
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#define CG_REPR_C_PACKED __attribute__((packed))\n"
        "#define CG_REPR_C_ALIGNED(n) __attribute__((aligned(n)))\n"
        "#else\n"
        "#error \"Coglet #repr(c) packed/aligned layout requires GNU-compatible C attributes in the current host-C backend\"\n"
        "#define CG_REPR_C_PACKED\n"
        "#define CG_REPR_C_ALIGNED(n)\n"
        "#endif\n\n",
        backend->out
    );
}

static void emit_runtime_forward_declarations(CBackend *backend)
{
    int emitted = 0;
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if (type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) {
            const char *name = nominal_type_name(backend, type);
            fprintf(
                backend->out,
                "typedef %s %s %s;\n",
                type->kind == COG_IR_TYPE_UNION ? "union" : "struct",
                name,
                name
            );
            emitted = 1;
        } else if (type->kind == COG_IR_TYPE_ARRAY) {
            const char *name = array_value_type_name(backend, type);
            fprintf(backend->out, "typedef struct %s %s;\n", name, name);
            emitted = 1;
        }
    }
    if (emitted)
        fputc('\n', backend->out);
}

static int emit_enum_definitions(CBackend *backend)
{
    int emitted = 0;
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if (type->kind != COG_IR_TYPE_ENUM)
            continue;
        const char *backing = type->as.enumeration.is_repr_c
            ? abi_type_name(backend, type->as.enumeration.backing_abi_type, type->span)
            : runtime_type_name(backend, type->as.enumeration.backing_type, type->span);
        const char *name = nominal_type_name(backend, type);
        if (!backing || !name)
            return 0;
        fprintf(backend->out, "typedef %s %s;\n", backing, name);
        emitted = 1;
    }
    if (emitted)
        fputc('\n', backend->out);
    return !backend->had_error;
}

static int prepare_abi_field_type(CBackend *backend, CogIrAbiTypeId abi_id, SourceSpan span)
{
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, abi_id);
    if (!abi) {
        backend_error(backend, span, "invalid aggregate field ABI type");
        return 0;
    }
    if (abi->kind == COG_IR_ABI_TYPE_ARRAY)
        return prepare_abi_field_type(backend, abi->element_type, span);
    return abi_type_name(backend, abi_id, span) != NULL;
}

static int prepare_runtime_array_leaf_type(CBackend *backend, CogIrTypeId type_id, SourceSpan span)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    while (type && type->kind == COG_IR_TYPE_ARRAY)
        type = cog_ir_get_type(backend->module, type->as.array.element_type);
    if (!type) {
        backend_error(backend, span, "invalid runtime array element type");
        return 0;
    }
    return runtime_type_name(backend, type->id, span) != NULL;
}

static int prepare_runtime_field_type(CBackend *backend, CogIrTypeId type_id, SourceSpan span)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    if (!type) {
        backend_error(backend, span, "invalid aggregate field runtime type");
        return 0;
    }
    return type->kind == COG_IR_TYPE_ARRAY
        ? prepare_runtime_array_leaf_type(backend, type_id, span)
        : runtime_type_name(backend, type_id, span) != NULL;
}

static int prepare_instruction_runtime_aliases(
    CBackend *backend,
    const CogIrFunction *function
) {
    for (size_t b = 0; b < function->block_count; ++b) {
        const CogIrBlock *block = &function->blocks[b];
        for (size_t i = 0; i < block->instruction_count; ++i) {
            const CogIrInstruction *instruction = &block->instructions[i];
            CogIrTypeId type = COG_IR_TYPE_INVALID;
            if (instruction->op == COG_IR_OP_PTR_REINTERPRET) {
                type = instruction->result_type;
            } else if (instruction->op == COG_IR_OP_LOAD && instruction->as.load.is_volatile) {
                const CogIrValue *address = cog_ir_get_value(function, instruction->as.load.address);
                type = address ? address->type : COG_IR_TYPE_INVALID;
            } else if (instruction->op == COG_IR_OP_STORE && instruction->as.store.is_volatile) {
                const CogIrValue *address = cog_ir_get_value(function, instruction->as.store.address);
                type = address ? address->type : COG_IR_TYPE_INVALID;
            }
            if (type != COG_IR_TYPE_INVALID &&
                !runtime_type_name(backend, type, instruction->span))
                return 0;
        }
    }
    return 1;
}

static int prepare_type_aliases(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if (type->kind == COG_IR_TYPE_ARRAY) {
            if (!prepare_runtime_array_leaf_type(backend, type->id, type->span))
                return 0;
            continue;
        }
        if ((type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) &&
            !type->as.aggregate.is_incomplete) {
            for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
                const CogIrAggregateField *field = &type->as.aggregate.fields[f];
                if (type->as.aggregate.is_repr_c) {
                    if (!prepare_abi_field_type(backend, field->abi_type, field->span))
                        return 0;
                } else if (!prepare_runtime_field_type(backend, field->type, field->span)) {
                    return 0;
                }
            }
        }
    }

    for (size_t i = 0; i < backend->module->function_count; ++i) {
        const CogIrFunction *function = &backend->module->functions[i];
        const CogIrType *type = cog_ir_get_type(backend->module, function->type);
        if (!type || type->kind != COG_IR_TYPE_FUNCTION) {
            backend_error(backend, function->span, "function has invalid CogIR function type");
            return 0;
        }
        if (function->abi.abi == COG_IR_ABI_C) {
            if (!abi_type_name(backend, function->abi.return_abi_type, function->span))
                return 0;
            for (size_t p = 0; p < function->abi.parameter_count; ++p)
                if (!abi_type_name(backend, function->abi.parameter_abi_types[p], function->span))
                    return 0;
        } else {
            if (!runtime_type_name(backend, type->as.function.result_type, function->span))
                return 0;
            for (size_t p = 0; p < type->as.function.parameter_count; ++p)
                if (!runtime_type_name(backend, type->as.function.parameter_types[p], function->span))
                    return 0;
        }
        for (size_t v = 0; v < function->value_count; ++v) {
            const CogIrValue *value = &function->values[v];
            if (value->abi_type != COG_IR_ABI_TYPE_INVALID) {
                const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, value->abi_type);
                /*
                 * Address values may carry pointer/array object-storage ABI
                 * metadata for native backends.  Host-C intentionally uses the
                 * runtime pointer spelling for these values, but preparing that
                 * alias here can require a pointer-to-array typedef before the
                 * aggregate element type is complete.  Defer only those runtime
                 * aliases until after aggregate definitions have been emitted.
                 */
                if (abi &&
                    (abi->kind == COG_IR_ABI_TYPE_POINTER || abi->kind == COG_IR_ABI_TYPE_ARRAY))
                    continue;
                if (!abi_type_name(backend, value->abi_type, value->span))
                    return 0;
            }
        }
        for (size_t s = 0; s < function->slot_count; ++s) {
            const CogIrSlot *slot = &function->slots[s];
            const char *name = slot->abi_type != COG_IR_ABI_TYPE_INVALID
                ? abi_type_name(backend, slot->abi_type, slot->span)
                : runtime_type_name(backend, slot->type, slot->span);
            if (!name)
                return 0;
        }
        if (!prepare_instruction_runtime_aliases(backend, function))
            return 0;
    }
    return !backend->had_error;
}

static int emit_abi_field_declaration(
    CBackend *backend,
    CogIrAbiTypeId abi_id,
    CogIrTypeId runtime_type_id,
    const char *name,
    SourceSpan span
) {
    const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, abi_id);
    const CogIrType *runtime = cog_ir_get_type(backend->module, runtime_type_id);
    if (!abi || !runtime) {
        backend_error(backend, span, "invalid aggregate field type during C lowering");
        return 0;
    }
    if (abi->kind == COG_IR_ABI_TYPE_ARRAY) {
        if (runtime->kind != COG_IR_TYPE_ARRAY) {
            backend_error(backend, span, "array ABI field has invalid runtime array type");
            return 0;
        }
        char *declarator = abi_array_declarator(
            backend, abi_id, runtime_type_id, name, 0, 0, 0, span);
        if (!declarator)
            return 0;
        fprintf(backend->out, "    %s;\n", declarator);
        free(declarator);
        return 1;
    }
    const char *type = abi_type_name(backend, abi_id, span);
    if (!type)
        return 0;
    fprintf(backend->out, "    %s %s;\n", type, name);
    return 1;
}

static int emit_runtime_field_declaration(
    CBackend *backend,
    CogIrTypeId type_id,
    const char *name,
    SourceSpan span
) {
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    if (!type) {
        backend_error(backend, span, "invalid aggregate field runtime type during C lowering");
        return 0;
    }
    if (type->kind == COG_IR_TYPE_ARRAY) {
        char *declarator = runtime_array_declarator(backend, type_id, name, 0, 0, 0, span);
        if (!declarator)
            return 0;
        fprintf(backend->out, "    %s;\n", declarator);
        free(declarator);
        return 1;
    }
    const char *type_name = runtime_type_name(backend, type_id, span);
    if (!type_name)
        return 0;
    fprintf(backend->out, "    %s %s;\n", type_name, name);
    return 1;
}

static const CogIrType *runtime_array_leaf(CBackend *backend, CogIrTypeId type_id)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    while (type && type->kind == COG_IR_TYPE_ARRAY)
        type = cog_ir_get_type(backend->module, type->as.array.element_type);
    return type;
}

static int emit_runtime_definition(CBackend *backend, CogIrTypeId type_id)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    if (!type || (type->kind != COG_IR_TYPE_ARRAY &&
                  type->kind != COG_IR_TYPE_STRUCT &&
                  type->kind != COG_IR_TYPE_UNION))
        return 1;

    unsigned char *state = &backend->runtime_definition_state[type_id];
    if (*state == 2)
        return 1;
    if ((type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) &&
        type->as.aggregate.is_incomplete) {
        *state = 2;
        return 1;
    }
    if (*state == 1) {
        backend_error(backend, type->span, "recursive by-value aggregate/array layout reached C lowering");
        return 0;
    }
    *state = 1;

    if (type->kind == COG_IR_TYPE_ARRAY) {
        const CogIrType *leaf = runtime_array_leaf(backend, type_id);
        if (leaf && (leaf->kind == COG_IR_TYPE_STRUCT || leaf->kind == COG_IR_TYPE_UNION) &&
            !emit_runtime_definition(backend, leaf->id))
            return 0;
        const char *name = array_value_type_name(backend, type);
        char *member = runtime_array_declarator(backend, type_id, "cg_e", 0, 0, 0, type->span);
        if (!member)
            return 0;
        fprintf(backend->out, "struct %s {\n    %s;\n};\n\n", name, member);
        free(member);
        *state = 2;
        return 1;
    }

    for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
        const CogIrType *field = runtime_array_leaf(backend, type->as.aggregate.fields[f].type);
        if (field && (field->kind == COG_IR_TYPE_STRUCT || field->kind == COG_IR_TYPE_UNION) &&
            !emit_runtime_definition(backend, field->id))
            return 0;
    }

    const char *name = nominal_type_name(backend, type);
    fprintf(backend->out, "%s %s {\n", type->kind == COG_IR_TYPE_UNION ? "union" : "struct", name);
    for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
        char field_name[32];
        snprintf(field_name, sizeof(field_name), "cg_f_%zu", f);
        const CogIrAggregateField *field = &type->as.aggregate.fields[f];
        int ok = type->as.aggregate.is_repr_c
            ? emit_abi_field_declaration(backend, field->abi_type, field->type, field_name, field->span)
            : emit_runtime_field_declaration(backend, field->type, field_name, field->span);
        if (!ok)
            return 0;
    }
    fputs("}", backend->out);
    if (type->as.aggregate.is_repr_c && type->as.aggregate.is_packed)
        fputs(" CG_REPR_C_PACKED", backend->out);
    if (type->as.aggregate.is_repr_c && type->as.aggregate.explicit_alignment > 0)
        fprintf(backend->out, " CG_REPR_C_ALIGNED(%u)", type->as.aggregate.explicit_alignment);
    fputs(";\n\n", backend->out);
    *state = 2;
    return 1;
}

static int prepare_late_value_aliases(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->function_count; ++i) {
        const CogIrFunction *function = &backend->module->functions[i];
        for (size_t v = 0; v < function->value_count; ++v) {
            const CogIrValue *value = &function->values[v];
            if (value->abi_type == COG_IR_ABI_TYPE_INVALID)
                continue;
            const CogIrAbiType *abi = cog_ir_get_abi_type(backend->module, value->abi_type);
            if (!abi ||
                (abi->kind != COG_IR_ABI_TYPE_POINTER && abi->kind != COG_IR_ABI_TYPE_ARRAY))
                continue;
            if (!runtime_type_name(backend, value->type, value->span))
                return 0;
        }
    }
    return 1;
}

static int emit_runtime_definitions(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->type_count; ++i)
        if (!emit_runtime_definition(backend, (CogIrTypeId)i))
            return 0;
    return !backend->had_error;
}

static int emit_globals(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->global_count; ++i) {
        const CogIrGlobal *global = &backend->module->globals[i];
        const CogIrType *type = cog_ir_get_type(backend->module, global->type);
        const char *type_name = runtime_type_name(backend, global->type, global->span);
        char *initializer = constant_storage_initializer(
            backend,
            global->static_initializer,
            type && type->kind == COG_IR_TYPE_ARRAY
        );
        if (!type || !type_name || !initializer) {
            free(initializer);
            backend_error(backend, global->span, "CogIR global has no C type or static initializer");
            return 0;
        }
        fprintf(
            backend->out,
            "static %s%s %s = %s;\n",
            global->is_readonly ? "const " : "",
            type_name,
            backend->global_names[i],
            initializer
        );
        free(initializer);
    }
    if (backend->module->global_count)
        fputc('\n', backend->out);
    return !backend->had_error;
}

static const char *function_result_type(CBackend *backend, const CogIrFunction *function)
{
    const CogIrType *type = cog_ir_get_type(backend->module, function->type);
    if (!type || type->kind != COG_IR_TYPE_FUNCTION) {
        backend_error(backend, function->span, "function has invalid runtime function type");
        return NULL;
    }
    return function->abi.abi == COG_IR_ABI_C
        ? abi_type_name(backend, function->abi.return_abi_type, function->span)
        : runtime_type_name(backend, type->as.function.result_type, function->span);
}

static const char *function_parameter_type(
    CBackend *backend,
    const CogIrFunction *function,
    size_t index
) {
    const CogIrType *type = cog_ir_get_type(backend->module, function->type);
    if (!type || type->kind != COG_IR_TYPE_FUNCTION || index >= type->as.function.parameter_count)
        return NULL;
    if (function->abi.abi == COG_IR_ABI_C)
        return abi_type_name(backend, function->abi.parameter_abi_types[index], function->span);

    CogIrValueId value_id = function->parameters[index];
    const CogIrValue *value = cog_ir_get_value(function, value_id);
    return value && value->abi_type != COG_IR_ABI_TYPE_INVALID
        ? abi_type_name(backend, value->abi_type, function->span)
        : runtime_type_name(backend, type->as.function.parameter_types[index], function->span);
}

static void emit_function_parameter_list(CBackend *backend, const CogIrFunction *function, int names)
{
    const CogIrType *type = cog_ir_get_type(backend->module, function->type);
    size_t count = type->as.function.parameter_count;
    if (count == 0 && !type->as.function.is_variadic) {
        fputs("void", backend->out);
        return;
    }
    for (size_t p = 0; p < count; ++p) {
        if (p) fputs(", ", backend->out);
        const char *param = function_parameter_type(backend, function, p);
        if (!param) {
            backend_error(backend, function->span, "invalid function parameter type during C lowering");
            return;
        }
        fprintf(backend->out, names ? "%s cg_p_%zu" : "%s", param, p);
    }
    if (type->as.function.is_variadic) {
        if (count) fputs(", ", backend->out);
        fputs("...", backend->out);
    }
}

static int emit_function_declarations(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->function_count; ++i) {
        const CogIrFunction *function = &backend->module->functions[i];
        const char *result = function_result_type(backend, function);
        if (!result)
            return 0;
        const char *macro = c_call_macro_name(function->abi.calling_convention);
        const char *sep = function->abi.calling_convention == COG_IR_CALL_DEFAULT ? "" : " ";
        if (function->linkage == COG_IR_LINKAGE_EXTERNAL) {
            fprintf(backend->out, "extern %s%s%s %s(", result, sep, macro, backend->function_names[i]);
            emit_function_parameter_list(backend, function, 0);
            if (backend->had_error) return 0;
            fputs(") __asm__(", backend->out);
            emit_c_string_literal(backend->out, function->abi.external_symbol);
            fputs(");\n", backend->out);
        } else {
            fprintf(backend->out, "static %s%s%s %s(", result, sep, macro, backend->function_names[i]);
            emit_function_parameter_list(backend, function, 0);
            if (backend->had_error) return 0;
            fputs(");\n", backend->out);
        }
    }
    fputc('\n', backend->out);
    return !backend->had_error;
}

static char *copy_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return NULL;
    }
    char *text = malloc((size_t)length + 1);
    if (text)
        vsnprintf(text, (size_t)length + 1, format, args);
    va_end(args);
    return text;
}

static int set_value_expr(char **exprs, size_t count, CogIrValueId value, char *expr)
{
    if (value == COG_IR_VALUE_INVALID || (size_t)value >= count || !expr) {
        free(expr);
        return 0;
    }
    free(exprs[value]);
    exprs[value] = expr;
    return 1;
}

static const char *value_expr(char **exprs, size_t count, CogIrValueId value)
{
    if (value == COG_IR_VALUE_INVALID || (size_t)value >= count)
        return NULL;
    return exprs[value];
}

static char *integer_constant_expr(CBackend *backend, const CogIrConstant *constant)
{
    const CogIrType *type = cog_ir_get_type(backend->module, constant->type);
    if (!type)
        return NULL;
    const CogIrType *integer = type;
    const char *cast_type = runtime_type_name(backend, constant->type, source_span_invalid());
    if (type->kind == COG_IR_TYPE_ENUM)
        integer = cog_ir_get_type(backend->module, type->as.enumeration.backing_type);
    if (!integer || integer->kind != COG_IR_TYPE_INTEGER || !cast_type)
        return NULL;

    unsigned bits = integer->as.integer.bits;
    uint64_t mask = bits == 64 ? UINT64_MAX : ((UINT64_C(1) << bits) - 1);
    uint64_t raw = constant->as.integer_bits & mask;
    if (integer->as.integer.is_signed && (raw & (UINT64_C(1) << (bits - 1)))) {
        uint64_t magnitude = ((~raw) & mask) + 1;
        if (magnitude == (UINT64_C(1) << 63))
            return copy_printf("((%s)(-INT64_C(9223372036854775807) - INT64_C(1)))", cast_type);
        return copy_printf("((%s)(-%" PRIu64 "LL))", cast_type, magnitude);
    }
    return copy_printf("((%s)%" PRIu64 "ULL)", cast_type, raw);
}

static char *scalar_constant_expr(CBackend *backend, const CogIrConstant *constant)
{
    const CogIrType *type = cog_ir_get_type(backend->module, constant->type);
    if (!type)
        return NULL;

    switch (constant->kind) {
        case COG_IR_CONST_ZERO:
            if (type->kind == COG_IR_TYPE_POINTER ||
                type->kind == COG_IR_TYPE_OPAQUE_POINTER ||
                type->kind == COG_IR_TYPE_FUNCTION)
                return copy_printf("NULL");
            if (type->kind == COG_IR_TYPE_ARRAY || type->kind == COG_IR_TYPE_STRUCT ||
                type->kind == COG_IR_TYPE_UNION) {
                backend_error(backend, source_span_invalid(), "aggregate zero constant reached scalar C lowering");
                return NULL;
            }
            {
                const char *name = runtime_type_name(backend, constant->type, source_span_invalid());
                return name ? copy_printf("((%s)0)", name) : NULL;
            }
        case COG_IR_CONST_BOOL:
            return copy_printf(constant->as.boolean ? "1" : "0");
        case COG_IR_CONST_INTEGER:
            return integer_constant_expr(backend, constant);
        case COG_IR_CONST_FLOAT32: {
            float value;
            uint32_t bits = constant->as.float32_bits;
            memcpy(&value, &bits, sizeof(value));
            return copy_printf("%af", (double)value);
        }
        case COG_IR_CONST_FLOAT64: {
            double value;
            uint64_t bits = constant->as.float64_bits;
            memcpy(&value, &bits, sizeof(value));
            return copy_printf("%a", value);
        }
        case COG_IR_CONST_NULL:
            return copy_printf("NULL");
        case COG_IR_CONST_ARRAY:
        case COG_IR_CONST_STRUCT:
            backend_error(backend, source_span_invalid(), "aggregate constant reached scalar C lowering");
            return NULL;
    }
    return NULL;
}

static int append_owned_text(
    char **buffer,
    size_t *length,
    size_t *capacity,
    char *owned
) {
    if (!owned)
        return 0;
    int ok = append_text(buffer, length, capacity, owned);
    free(owned);
    return ok;
}

static char *constant_storage_initializer(
    CBackend *backend,
    CogIrConstId constant_id,
    int array_value_wrapper
) {
    const CogIrConstant *constant = cog_ir_get_constant(backend->module, constant_id);
    const CogIrType *type = constant ? cog_ir_get_type(backend->module, constant->type) : NULL;
    if (!constant || !type)
        return NULL;

    if (type->kind != COG_IR_TYPE_ARRAY && type->kind != COG_IR_TYPE_STRUCT &&
        type->kind != COG_IR_TYPE_UNION) {
        if (array_value_wrapper) {
            backend_error(backend, source_span_invalid(), "non-array constant requested as array value initializer");
            return NULL;
        }
        return scalar_constant_expr(backend, constant);
    }

    if (constant->kind == COG_IR_CONST_ZERO) {
        if (type->kind == COG_IR_TYPE_ARRAY && array_value_wrapper)
            return copy_printf("{ .cg_e = {0} }");
        return copy_printf("{0}");
    }

    char *buffer = NULL;
    size_t length = 0, capacity = 0;
    if (type->kind == COG_IR_TYPE_ARRAY) {
        if (constant->kind != COG_IR_CONST_ARRAY ||
            constant->as.aggregate.element_count != type->as.array.length) {
            backend_error(backend, source_span_invalid(), "array constant has invalid C initializer shape");
            return NULL;
        }
        if (array_value_wrapper && !append_text(&buffer, &length, &capacity, "{ .cg_e = "))
            goto oom;
        if (!append_text(&buffer, &length, &capacity, "{"))
            goto oom;
        for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
            if (i && !append_text(&buffer, &length, &capacity, ", "))
                goto oom;
            char *initializer = constant_storage_initializer(
                backend, constant->as.aggregate.elements[i], 0);
            if (!initializer)
                goto fail;
            if (!append_owned_text(&buffer, &length, &capacity, initializer))
                goto oom;
        }
        if (!append_text(&buffer, &length, &capacity, "}"))
            goto oom;
        if (array_value_wrapper && !append_text(&buffer, &length, &capacity, " }"))
            goto oom;
        return buffer;
    }

    if (type->kind == COG_IR_TYPE_UNION) {
        backend_error(backend, source_span_invalid(), "nonzero union constants are not part of CogIR v1");
        return NULL;
    }

    if (constant->kind != COG_IR_CONST_STRUCT ||
        constant->as.aggregate.element_count != type->as.aggregate.field_count) {
        backend_error(backend, source_span_invalid(), "struct constant has invalid C initializer shape");
        return NULL;
    }
    if (!append_text(&buffer, &length, &capacity, "{"))
        goto oom;
    for (size_t i = 0; i < constant->as.aggregate.element_count; ++i) {
        if (i && !append_text(&buffer, &length, &capacity, ", "))
            goto oom;
        char field[48];
        int written = snprintf(field, sizeof(field), ".cg_f_%zu = ", i);
        if (written < 0 || (size_t)written >= sizeof(field) ||
            !append_text(&buffer, &length, &capacity, field))
            goto oom;
        char *initializer = constant_storage_initializer(
            backend, constant->as.aggregate.elements[i], 0);
        if (!initializer)
            goto fail;
        if (!append_owned_text(&buffer, &length, &capacity, initializer))
            goto oom;
    }
    if (!append_text(&buffer, &length, &capacity, "}"))
        goto oom;
    return buffer;

oom:
    backend_error(backend, source_span_invalid(), "out of memory while generating aggregate constant initializer");
fail:
    free(buffer);
    return NULL;
}

static char *constant_expr(CBackend *backend, CogIrConstId constant_id)
{
    const CogIrConstant *constant = cog_ir_get_constant(backend->module, constant_id);
    const CogIrType *type = constant ? cog_ir_get_type(backend->module, constant->type) : NULL;
    if (!constant || !type)
        return NULL;

    if (type->kind != COG_IR_TYPE_ARRAY && type->kind != COG_IR_TYPE_STRUCT &&
        type->kind != COG_IR_TYPE_UNION)
        return scalar_constant_expr(backend, constant);

    const char *name = runtime_type_name(backend, type->id, source_span_invalid());
    char *initializer = constant_storage_initializer(
        backend, constant_id, type->kind == COG_IR_TYPE_ARRAY);
    if (!name || !initializer) {
        free(initializer);
        return NULL;
    }
    char *result = copy_printf("((%s)%s)", name, initializer);
    free(initializer);
    return result;
}

static uint64_t integer_width_mask(unsigned bits)
{
    return bits == 64
        ? UINT64_MAX
        : ((UINT64_C(1) << bits) - UINT64_C(1));
}

static int emit_wrapping_integer_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const CogIrType *type = cog_ir_get_type(backend->module, instruction->result_type);
    if (!type || type->kind != COG_IR_TYPE_INTEGER) {
        backend_error(backend, instruction->span, "wrapping arithmetic requires an integer result type");
        return 0;
    }

    unsigned bits = type->as.integer.bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        backend_error(backend, instruction->span, "wrapping arithmetic integer width is not supported by the host-C backend");
        return 0;
    }

    const char *operand = NULL;
    const char *lhs = NULL;
    const char *rhs = NULL;
    const char *operator_text = NULL;
    switch (instruction->op) {
        case COG_IR_OP_IADD_WRAP:
            operator_text = "+";
            break;
        case COG_IR_OP_ISUB_WRAP:
            operator_text = "-";
            break;
        case COG_IR_OP_IMUL_WRAP:
            operator_text = "*";
            break;
        case COG_IR_OP_INEG_WRAP:
            operand = value_expr(
                exprs,
                function->value_count,
                instruction->as.unary.operand
            );
            break;
        default:
            backend_error(backend, instruction->span, "invalid wrapping arithmetic operation during C lowering");
            return 0;
    }

    if (operator_text) {
        lhs = value_expr(exprs, function->value_count, instruction->as.binary.lhs);
        rhs = value_expr(exprs, function->value_count, instruction->as.binary.rhs);
        if (!lhs || !rhs)
            return 0;
    } else if (!operand) {
        return 0;
    }

    CogIrValueId result = instruction->result;
    uint64_t mask = integer_width_mask(bits);
    if (operator_text) {
        fprintf(
            backend->out,
            "    uint64_t cg_wrap_bits_%u = ((uint64_t)(%s) %s (uint64_t)(%s))",
            result,
            lhs,
            operator_text,
            rhs
        );
    } else {
        fprintf(
            backend->out,
            "    uint64_t cg_wrap_bits_%u = (UINT64_C(0) - (uint64_t)(%s))",
            result,
            operand
        );
    }
    if (bits != 64)
        fprintf(backend->out, " & UINT64_C(0x%" PRIx64 ")", mask);
    fputs(";\n", backend->out);

    const char *result_type = runtime_type_name(backend, instruction->result_type, instruction->span);
    if (!result_type)
        return 0;

    if (!type->as.integer.is_signed) {
        fprintf(
            backend->out,
            "    %s cg_v_%u = (%s)cg_wrap_bits_%u;\n",
            result_type,
            result,
            result_type,
            result
        );
    } else {
        uint64_t sign_bit = UINT64_C(1) << (bits - 1);
        uint64_t low_mask = sign_bit - UINT64_C(1);
        if (bits == 64) {
            fprintf(
                backend->out,
                "    %s cg_v_%u = (cg_wrap_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                "? (INT64_MIN + (int64_t)(cg_wrap_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                ": (int64_t)cg_wrap_bits_%u;\n",
                result_type,
                result,
                result,
                sign_bit,
                result,
                low_mask,
                result
            );
        } else {
            uint64_t min_magnitude = sign_bit;
            fprintf(
                backend->out,
                "    %s cg_v_%u = (cg_wrap_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                "? (%s)(-INT64_C(%" PRIu64 ") + (int64_t)(cg_wrap_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                ": (%s)cg_wrap_bits_%u;\n",
                result_type,
                result,
                result,
                sign_bit,
                result_type,
                min_magnitude,
                result,
                low_mask,
                result_type,
                result
            );
        }
    }

    if (!set_value_expr(
            exprs,
            function->value_count,
            result,
            copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid wrapping arithmetic result during C lowering");
        return 0;
    }
    return 1;
}

static int emit_shift_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const CogIrType *type = cog_ir_get_type(backend->module, instruction->result_type);
    const CogIrValue *count_value = cog_ir_get_value(function, instruction->as.binary.rhs);
    const CogIrType *count_type = count_value
        ? cog_ir_get_type(backend->module, count_value->type)
        : NULL;
    if (!type || type->kind != COG_IR_TYPE_INTEGER ||
        !count_type || count_type->kind != COG_IR_TYPE_INTEGER) {
        backend_error(backend, instruction->span, "shift requires integer operand types");
        return 0;
    }

    unsigned bits = type->as.integer.bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        backend_error(backend, instruction->span, "shift integer width is not supported by the host-C backend");
        return 0;
    }

    const char *lhs = value_expr(exprs, function->value_count, instruction->as.binary.lhs);
    const char *rhs = value_expr(exprs, function->value_count, instruction->as.binary.rhs);
    if (!lhs || !rhs)
        return 0;

    CogIrValueId result = instruction->result;
    if (count_type->as.integer.is_signed) {
        fprintf(backend->out,
                "    if ((%s) < 0 || (uint64_t)(%s) >= UINT64_C(%u)) abort();\n",
                rhs, rhs, bits);
    } else {
        fprintf(backend->out,
                "    if ((uint64_t)(%s) >= UINT64_C(%u)) abort();\n",
                rhs, bits);
    }

    uint64_t mask = integer_width_mask(bits);
    fprintf(backend->out,
            "    uint64_t cg_shift_bits_%u = (uint64_t)(%s)",
            result, lhs);
    if (bits != 64)
        fprintf(backend->out, " & UINT64_C(0x%" PRIx64 ")", mask);
    fputs(";\n", backend->out);

    switch (instruction->op) {
        case COG_IR_OP_SHL_CHECKED_COUNT:
            fprintf(backend->out,
                    "    cg_shift_bits_%u = cg_shift_bits_%u << (unsigned)(%s)",
                    result, result, rhs);
            if (bits != 64)
                fprintf(backend->out, " & UINT64_C(0x%" PRIx64 ")", mask);
            fputs(";\n", backend->out);
            break;

        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
            fprintf(backend->out,
                    "    cg_shift_bits_%u >>= (unsigned)(%s);\n",
                    result, rhs);
            break;

        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT: {
            uint64_t sign_bit = UINT64_C(1) << (bits - 1);
            fprintf(backend->out,
                    "    if ((cg_shift_bits_%u & UINT64_C(0x%" PRIx64 ")) != 0 && (unsigned)(%s) != 0) {\n",
                    result, sign_bit, rhs);
            fprintf(backend->out,
                    "        uint64_t cg_shift_fill_%u = UINT64_C(0x%" PRIx64 ") ^ "
                    "(UINT64_C(0x%" PRIx64 ") >> (unsigned)(%s));\n",
                    result, mask, mask, rhs);
            fprintf(backend->out,
                    "        cg_shift_bits_%u = (cg_shift_bits_%u >> (unsigned)(%s)) | cg_shift_fill_%u;\n",
                    result, result, rhs, result);
            fputs("    } else {\n", backend->out);
            fprintf(backend->out,
                    "        cg_shift_bits_%u >>= (unsigned)(%s);\n",
                    result, rhs);
            fputs("    }\n", backend->out);
            break;
        }

        default:
            backend_error(backend, instruction->span, "invalid shift operation during C lowering");
            return 0;
    }

    const char *result_type = runtime_type_name(backend, instruction->result_type, instruction->span);
    if (!result_type)
        return 0;

    if (!type->as.integer.is_signed) {
        fprintf(backend->out,
                "    %s cg_v_%u = (%s)cg_shift_bits_%u;\n",
                result_type, result, result_type, result);
    } else {
        uint64_t sign_bit = UINT64_C(1) << (bits - 1);
        uint64_t low_mask = sign_bit - UINT64_C(1);
        if (bits == 64) {
            fprintf(backend->out,
                    "    %s cg_v_%u = (cg_shift_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                    "? (INT64_MIN + (int64_t)(cg_shift_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                    ": (int64_t)cg_shift_bits_%u;\n",
                    result_type, result, result, sign_bit, result, low_mask, result);
        } else {
            fprintf(backend->out,
                    "    %s cg_v_%u = (cg_shift_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                    "? (%s)(-INT64_C(%" PRIu64 ") + (int64_t)(cg_shift_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                    ": (%s)cg_shift_bits_%u;\n",
                    result_type, result, result, sign_bit, result_type, sign_bit,
                    result, low_mask, result_type, result);
        }
    }

    if (!set_value_expr(exprs, function->value_count, result, copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid shift result during C lowering");
        return 0;
    }
    return 1;
}

static const char *signed_integer_min_name(unsigned bits)
{
    switch (bits) {
        case 8: return "INT8_MIN";
        case 16: return "INT16_MIN";
        case 32: return "INT32_MIN";
        case 64: return "INT64_MIN";
        default: return NULL;
    }
}

static const char *signed_integer_max_name(unsigned bits)
{
    switch (bits) {
        case 8: return "INT8_MAX";
        case 16: return "INT16_MAX";
        case 32: return "INT32_MAX";
        case 64: return "INT64_MAX";
        default: return NULL;
    }
}

static int emit_checked_integer_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const CogIrType *type = cog_ir_get_type(backend->module, instruction->result_type);
    if (!type || type->kind != COG_IR_TYPE_INTEGER) {
        backend_error(backend, instruction->span, "checked arithmetic requires an integer result type");
        return 0;
    }

    unsigned bits = type->as.integer.bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        backend_error(backend, instruction->span, "checked arithmetic integer width is not supported by the host-C backend");
        return 0;
    }

    const char *result_type = runtime_type_name(backend, instruction->result_type, instruction->span);
    if (!result_type)
        return 0;

    CogIrValueId result = instruction->result;
    const char *lhs = NULL;
    const char *rhs = NULL;
    int is_unary = instruction->op == COG_IR_OP_INEG_CHECKED;
    if (is_unary) {
        lhs = value_expr(exprs, function->value_count, instruction->as.unary.operand);
        if (!lhs)
            return 0;
    } else {
        lhs = value_expr(exprs, function->value_count, instruction->as.binary.lhs);
        rhs = value_expr(exprs, function->value_count, instruction->as.binary.rhs);
        if (!lhs || !rhs)
            return 0;
    }

    if (type->as.integer.is_signed) {
        const char *min_name = signed_integer_min_name(bits);
        const char *max_name = signed_integer_max_name(bits);
        if (!min_name || !max_name)
            return 0;

        fprintf(
            backend->out,
            "    int64_t cg_checked_lhs_%u = (int64_t)(%s);\n",
            result,
            lhs
        );
        if (!is_unary) {
            fprintf(
                backend->out,
                "    int64_t cg_checked_rhs_%u = (int64_t)(%s);\n",
                result,
                rhs
            );
        }

        switch (instruction->op) {
            case COG_IR_OP_IADD_CHECKED:
                fprintf(
                    backend->out,
                    "    if ((cg_checked_rhs_%u > 0 && cg_checked_lhs_%u > (int64_t)%s - cg_checked_rhs_%u) || "
                    "(cg_checked_rhs_%u < 0 && cg_checked_lhs_%u < (int64_t)%s - cg_checked_rhs_%u)) abort();\n"
                    "    int64_t cg_checked_result_%u = cg_checked_lhs_%u + cg_checked_rhs_%u;\n",
                    result, result, max_name, result,
                    result, result, min_name, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_ISUB_CHECKED:
                fprintf(
                    backend->out,
                    "    if ((cg_checked_rhs_%u < 0 && cg_checked_lhs_%u > (int64_t)%s + cg_checked_rhs_%u) || "
                    "(cg_checked_rhs_%u > 0 && cg_checked_lhs_%u < (int64_t)%s + cg_checked_rhs_%u)) abort();\n"
                    "    int64_t cg_checked_result_%u = cg_checked_lhs_%u - cg_checked_rhs_%u;\n",
                    result, result, max_name, result,
                    result, result, min_name, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_IMUL_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_lhs_%u != 0 && cg_checked_rhs_%u != 0) {\n"
                    "        if ((cg_checked_lhs_%u == -1 && cg_checked_rhs_%u == (int64_t)%s) || "
                    "(cg_checked_rhs_%u == -1 && cg_checked_lhs_%u == (int64_t)%s)) abort();\n"
                    "        if (cg_checked_lhs_%u > 0) {\n"
                    "            if (cg_checked_rhs_%u > 0) { if (cg_checked_lhs_%u > (int64_t)%s / cg_checked_rhs_%u) abort(); }\n"
                    "            else { if (cg_checked_rhs_%u < (int64_t)%s / cg_checked_lhs_%u) abort(); }\n"
                    "        } else {\n"
                    "            if (cg_checked_rhs_%u > 0) { if (cg_checked_lhs_%u < (int64_t)%s / cg_checked_rhs_%u) abort(); }\n"
                    "            else { if (cg_checked_lhs_%u < (int64_t)%s / cg_checked_rhs_%u) abort(); }\n"
                    "        }\n"
                    "    }\n"
                    "    int64_t cg_checked_result_%u = cg_checked_lhs_%u * cg_checked_rhs_%u;\n",
                    result, result,
                    result, result, min_name, result, result, min_name,
                    result,
                    result, result, max_name, result,
                    result, min_name, result,
                    result, result, min_name, result,
                    result, max_name, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_IDIV_CHECKED:
            case COG_IR_OP_IREM_CHECKED: {
                const char *operator_text = instruction->op == COG_IR_OP_IDIV_CHECKED ? "/" : "%";
                fprintf(
                    backend->out,
                    "    if (cg_checked_rhs_%u == 0 || "
                    "(cg_checked_lhs_%u == (int64_t)%s && cg_checked_rhs_%u == -1)) abort();\n"
                    "    int64_t cg_checked_result_%u = cg_checked_lhs_%u %s cg_checked_rhs_%u;\n",
                    result,
                    result, min_name, result,
                    result, result, operator_text, result
                );
                break;
            }
            case COG_IR_OP_INEG_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_lhs_%u == (int64_t)%s) abort();\n"
                    "    int64_t cg_checked_result_%u = -cg_checked_lhs_%u;\n",
                    result, min_name,
                    result, result
                );
                break;
            default:
                backend_error(backend, instruction->span, "invalid checked arithmetic operation during C lowering");
                return 0;
        }

        fprintf(
            backend->out,
            "    %s cg_v_%u = (%s)cg_checked_result_%u;\n",
            result_type,
            result,
            result_type,
            result
        );
    } else {
        uint64_t max_value = integer_width_mask(bits);
        fprintf(
            backend->out,
            "    uint64_t cg_checked_lhs_%u = (uint64_t)(%s);\n",
            result,
            lhs
        );
        if (!is_unary) {
            fprintf(
                backend->out,
                "    uint64_t cg_checked_rhs_%u = (uint64_t)(%s);\n",
                result,
                rhs
            );
        }

        switch (instruction->op) {
            case COG_IR_OP_IADD_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_lhs_%u > UINT64_C(0x%" PRIx64 ") - cg_checked_rhs_%u) abort();\n"
                    "    uint64_t cg_checked_result_%u = cg_checked_lhs_%u + cg_checked_rhs_%u;\n",
                    result, max_value, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_ISUB_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_lhs_%u < cg_checked_rhs_%u) abort();\n"
                    "    uint64_t cg_checked_result_%u = cg_checked_lhs_%u - cg_checked_rhs_%u;\n",
                    result, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_IMUL_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_rhs_%u != 0 && cg_checked_lhs_%u > UINT64_C(0x%" PRIx64 ") / cg_checked_rhs_%u) abort();\n"
                    "    uint64_t cg_checked_result_%u = cg_checked_lhs_%u * cg_checked_rhs_%u;\n",
                    result, result, max_value, result,
                    result, result, result
                );
                break;
            case COG_IR_OP_IDIV_CHECKED:
            case COG_IR_OP_IREM_CHECKED: {
                const char *operator_text = instruction->op == COG_IR_OP_IDIV_CHECKED ? "/" : "%";
                fprintf(
                    backend->out,
                    "    if (cg_checked_rhs_%u == 0) abort();\n"
                    "    uint64_t cg_checked_result_%u = cg_checked_lhs_%u %s cg_checked_rhs_%u;\n",
                    result,
                    result, result, operator_text, result
                );
                break;
            }
            case COG_IR_OP_INEG_CHECKED:
                fprintf(
                    backend->out,
                    "    if (cg_checked_lhs_%u != 0) abort();\n"
                    "    uint64_t cg_checked_result_%u = UINT64_C(0);\n",
                    result,
                    result
                );
                break;
            default:
                backend_error(backend, instruction->span, "invalid checked arithmetic operation during C lowering");
                return 0;
        }

        fprintf(
            backend->out,
            "    %s cg_v_%u = (%s)cg_checked_result_%u;\n",
            result_type,
            result,
            result_type,
            result
        );
    }

    if (!set_value_expr(
            exprs,
            function->value_count,
            result,
            copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid checked arithmetic result during C lowering");
        return 0;
    }
    return 1;
}

static const CogIrType *integer_representation_type(
    const CBackend *backend,
    const CogIrType *type
) {
    if (!type)
        return NULL;
    if (type->kind == COG_IR_TYPE_INTEGER)
        return type;
    if (type->kind == COG_IR_TYPE_ENUM)
        return cog_ir_get_type(backend->module, type->as.enumeration.backing_type);
    return NULL;
}

static int emit_checked_cast_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const CogIrValue *source_value = cog_ir_get_value(
        function,
        instruction->as.conversion.operand
    );
    const CogIrType *source = source_value
        ? cog_ir_get_type(backend->module, source_value->type)
        : NULL;
    const CogIrType *target = cog_ir_get_type(backend->module, instruction->result_type);
    const char *operand = value_expr(
        exprs,
        function->value_count,
        instruction->as.conversion.operand
    );
    const char *result_type = runtime_type_name(
        backend,
        instruction->result_type,
        instruction->span
    );
    if (!source_value || !source || !target || !operand || !result_type)
        return 0;

    CogIrValueId result = instruction->result;
    if (source_value->type == instruction->result_type) {
        if (!set_value_expr(exprs, function->value_count, result, copy_printf("%s", operand))) {
            backend_error(backend, instruction->span, "invalid identity checked-cast result during C lowering");
            return 0;
        }
        return 1;
    }

    const CogIrType *source_integer = integer_representation_type(backend, source);
    const CogIrType *target_integer = integer_representation_type(backend, target);

    if (target_integer && target->kind == COG_IR_TYPE_INTEGER) {
        unsigned target_bits = target_integer->as.integer.bits;
        if (target_bits != 8 && target_bits != 16 && target_bits != 32 && target_bits != 64) {
            backend_error(backend, instruction->span, "checked cast integer width is not supported by the host-C backend");
            return 0;
        }

        if (source_integer) {
            if (source_integer->as.integer.is_signed) {
                fprintf(
                    backend->out,
                    "    int64_t cg_cast_signed_%u = (int64_t)(%s);\n",
                    result,
                    operand
                );
                if (target_integer->as.integer.is_signed) {
                    if (target_bits < 64) {
                        const char *min_name = signed_integer_min_name(target_bits);
                        const char *max_name = signed_integer_max_name(target_bits);
                        if (!min_name || !max_name)
                            return 0;
                        fprintf(
                            backend->out,
                            "    if (cg_cast_signed_%u < %s || cg_cast_signed_%u > %s) abort();\n",
                            result,
                            min_name,
                            result,
                            max_name
                        );
                    }
                    fprintf(
                        backend->out,
                        "    %s cg_v_%u = (%s)cg_cast_signed_%u;\n",
                        result_type,
                        result,
                        result_type,
                        result
                    );
                } else {
                    uint64_t max_value = integer_width_mask(target_bits);
                    fprintf(
                        backend->out,
                        "    if (cg_cast_signed_%u < 0",
                        result
                    );
                    if (target_bits < 64) {
                        fprintf(
                            backend->out,
                            " || (uint64_t)cg_cast_signed_%u > UINT64_C(0x%" PRIx64 ")",
                            result,
                            max_value
                        );
                    }
                    fputs(") abort();\n", backend->out);
                    fprintf(
                        backend->out,
                        "    %s cg_v_%u = (%s)(uint64_t)cg_cast_signed_%u;\n",
                        result_type,
                        result,
                        result_type,
                        result
                    );
                }
            } else {
                fprintf(
                    backend->out,
                    "    uint64_t cg_cast_unsigned_%u = (uint64_t)(%s);\n",
                    result,
                    operand
                );
                if (target_integer->as.integer.is_signed) {
                    uint64_t max_value = (UINT64_C(1) << (target_bits - 1)) - UINT64_C(1);
                    fprintf(
                        backend->out,
                        "    if (cg_cast_unsigned_%u > UINT64_C(0x%" PRIx64 ")) abort();\n",
                        result,
                        max_value
                    );
                } else if (target_bits < 64) {
                    uint64_t max_value = integer_width_mask(target_bits);
                    fprintf(
                        backend->out,
                        "    if (cg_cast_unsigned_%u > UINT64_C(0x%" PRIx64 ")) abort();\n",
                        result,
                        max_value
                    );
                }
                fprintf(
                    backend->out,
                    "    %s cg_v_%u = (%s)cg_cast_unsigned_%u;\n",
                    result_type,
                    result,
                    result_type,
                    result
                );
            }
        } else if (source->kind == COG_IR_TYPE_FLOAT) {
            fprintf(
                backend->out,
                "    double cg_cast_float_%u = (double)(%s);\n",
                result,
                operand
            );
            fputs("    if (!isfinite(cg_cast_float_", backend->out);
            fprintf(backend->out, "%u)", result);
            if (target_integer->as.integer.is_signed) {
                if (target_bits == 64) {
                    fprintf(
                        backend->out,
                        " || cg_cast_float_%u < -0x1p63 || cg_cast_float_%u >= 0x1p63",
                        result,
                        result
                    );
                } else {
                    uint64_t sign_bit = UINT64_C(1) << (target_bits - 1);
                    fprintf(
                        backend->out,
                        " || cg_cast_float_%u <= -%" PRIu64 ".0 || cg_cast_float_%u >= %" PRIu64 ".0",
                        result,
                        sign_bit + UINT64_C(1),
                        result,
                        sign_bit
                    );
                }
            } else {
                fprintf(backend->out, " || cg_cast_float_%u <= -1.0", result);
                if (target_bits == 64) {
                    fprintf(backend->out, " || cg_cast_float_%u >= 0x1p64", result);
                } else {
                    uint64_t upper = UINT64_C(1) << target_bits;
                    fprintf(
                        backend->out,
                        " || cg_cast_float_%u >= %" PRIu64 ".0",
                        result,
                        upper
                    );
                }
            }
            fputs(") abort();\n", backend->out);
            fprintf(
                backend->out,
                "    %s cg_v_%u = (%s)cg_cast_float_%u;\n",
                result_type,
                result,
                result_type,
                result
            );
        } else {
            backend_error(backend, instruction->span, "checked cast has unsupported numeric source during C lowering");
            return 0;
        }
    } else if (target->kind == COG_IR_TYPE_FLOAT) {
        if (!source_integer && source->kind != COG_IR_TYPE_FLOAT) {
            backend_error(backend, instruction->span, "checked floating cast has unsupported source during C lowering");
            return 0;
        }
        if (source->kind == COG_IR_TYPE_FLOAT &&
            source->as.floating.bits == 64 && target->as.floating.bits == 32) {
            fprintf(
                backend->out,
                "    double cg_cast_float_%u = (double)(%s);\n"
                "    if (isfinite(cg_cast_float_%u) && "
                "(cg_cast_float_%u > (double)FLT_MAX || cg_cast_float_%u < -(double)FLT_MAX)) abort();\n",
                result,
                operand,
                result,
                result,
                result
            );
            fprintf(
                backend->out,
                "    %s cg_v_%u = (%s)cg_cast_float_%u;\n",
                result_type,
                result,
                result_type,
                result
            );
        } else {
            fprintf(
                backend->out,
                "    %s cg_v_%u = (%s)(%s);\n",
                result_type,
                result,
                result_type,
                operand
            );
        }
    } else {
        backend_error(backend, instruction->span, "checked cast target is outside the current host-C numeric subset");
        return 0;
    }

    if (!set_value_expr(exprs, function->value_count, result, copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid checked-cast result during C lowering");
        return 0;
    }
    return 1;
}

static int emit_truncating_integer_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const CogIrType *target = cog_ir_get_type(backend->module, instruction->result_type);
    const CogIrValue *source_value = cog_ir_get_value(function, instruction->as.conversion.operand);
    const CogIrType *source = source_value
        ? cog_ir_get_type(backend->module, source_value->type)
        : NULL;
    const char *operand = value_expr(exprs, function->value_count, instruction->as.conversion.operand);
    const char *result_type = runtime_type_name(backend, instruction->result_type, instruction->span);
    if (!source || source->kind != COG_IR_TYPE_INTEGER ||
        !target || target->kind != COG_IR_TYPE_INTEGER || !operand || !result_type) {
        backend_error(backend, instruction->span, "integer truncation has invalid source or target during C lowering");
        return 0;
    }

    unsigned bits = target->as.integer.bits;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) {
        backend_error(backend, instruction->span, "integer truncation width is not supported by the host-C backend");
        return 0;
    }

    CogIrValueId result = instruction->result;
    uint64_t mask = integer_width_mask(bits);
    fprintf(
        backend->out,
        "    uint64_t cg_trunc_bits_%u = (uint64_t)(%s)",
        result,
        operand
    );
    if (bits < 64)
        fprintf(backend->out, " & UINT64_C(0x%" PRIx64 ")", mask);
    fputs(";\n", backend->out);

    if (!target->as.integer.is_signed) {
        fprintf(
            backend->out,
            "    %s cg_v_%u = (%s)cg_trunc_bits_%u;\n",
            result_type,
            result,
            result_type,
            result
        );
    } else {
        uint64_t sign_bit = UINT64_C(1) << (bits - 1);
        uint64_t low_mask = sign_bit - UINT64_C(1);
        if (bits == 64) {
            fprintf(
                backend->out,
                "    %s cg_v_%u = (cg_trunc_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                "? (INT64_MIN + (int64_t)(cg_trunc_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                ": (int64_t)cg_trunc_bits_%u;\n",
                result_type,
                result,
                result,
                sign_bit,
                result,
                low_mask,
                result
            );
        } else {
            fprintf(
                backend->out,
                "    %s cg_v_%u = (cg_trunc_bits_%u & UINT64_C(0x%" PRIx64 ")) "
                "? (%s)(-INT64_C(%" PRIu64 ") + (int64_t)(cg_trunc_bits_%u & UINT64_C(0x%" PRIx64 "))) "
                ": (%s)cg_trunc_bits_%u;\n",
                result_type,
                result,
                result,
                sign_bit,
                result_type,
                sign_bit,
                result,
                low_mask,
                result_type,
                result
            );
        }
    }

    if (!set_value_expr(exprs, function->value_count, result, copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid integer truncation result during C lowering");
        return 0;
    }
    return 1;
}

static int emit_pointer_reinterpret_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    const char *operand = value_expr(exprs, function->value_count, instruction->as.conversion.operand);
    const char *result_type = runtime_type_name(backend, instruction->result_type, instruction->span);
    if (!operand || !result_type)
        return 0;

    CogIrValueId result = instruction->result;
    fprintf(
        backend->out,
        "    %s cg_v_%u = (%s)(%s);\n",
        result_type,
        result,
        result_type,
        operand
    );
    if (!set_value_expr(exprs, function->value_count, result, copy_printf("cg_v_%u", result))) {
        backend_error(backend, instruction->span, "invalid pointer reinterpret result during C lowering");
        return 0;
    }
    return 1;
}

static void emit_c_indent(FILE *out, unsigned level)
{
    while (level--)
        fputs("    ", out);
}

static int emit_array_copy_recursive(
    CBackend *backend,
    CogIrTypeId array_type_id,
    const char *destination,
    const char *source,
    unsigned indent
) {
    const CogIrType *type = cog_ir_get_type(backend->module, array_type_id);
    if (!type || type->kind != COG_IR_TYPE_ARRAY) {
        backend_error(backend, source_span_invalid(), "invalid array type during C array copy");
        return 0;
    }

    size_t loop_id = backend->temporary_id++;
    emit_c_indent(backend->out, indent);
    fprintf(
        backend->out,
        "for (size_t cg_ai_%zu = 0; cg_ai_%zu < %zu; ++cg_ai_%zu) {\n",
        loop_id, loop_id, type->as.array.length, loop_id
    );

    char *destination_element = copy_printf("(%s)[cg_ai_%zu]", destination, loop_id);
    char *source_element = copy_printf("(%s)[cg_ai_%zu]", source, loop_id);
    if (!destination_element || !source_element) {
        free(destination_element);
        free(source_element);
        backend_error(backend, source_span_invalid(), "out of memory while generating C array copy");
        return 0;
    }

    const CogIrType *element = cog_ir_get_type(backend->module, type->as.array.element_type);
    int ok = 1;
    if (element && element->kind == COG_IR_TYPE_ARRAY) {
        ok = emit_array_copy_recursive(
            backend,
            element->id,
            destination_element,
            source_element,
            indent + 1
        );
    } else if (element) {
        emit_c_indent(backend->out, indent + 1);
        fprintf(backend->out, "%s = %s;\n", destination_element, source_element);
    } else {
        backend_error(backend, source_span_invalid(), "array copy has invalid element type");
        ok = 0;
    }

    free(destination_element);
    free(source_element);
    emit_c_indent(backend->out, indent);
    fputs("}\n", backend->out);
    return ok;
}

static int emit_array_storage_copy(
    CBackend *backend,
    CogIrTypeId array_type_id,
    const char *destination,
    const char *source,
    int is_volatile
) {
    const CogIrType *type = cog_ir_get_type(backend->module, array_type_id);
    if (!type || type->kind != COG_IR_TYPE_ARRAY) {
        backend_error(backend, source_span_invalid(), "invalid array storage copy type");
        return 0;
    }
    if (is_volatile)
        return emit_array_copy_recursive(backend, array_type_id, destination, source, 1);

    fprintf(
        backend->out,
        "    memcpy((void *)(%s), (const void *)(%s), sizeof(%s));\n",
        destination,
        source,
        destination
    );
    return 1;
}

static int emit_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs
) {
    size_t value_count = function->value_count;
    CogIrValueId result = instruction->result;

    switch (instruction->op) {
        case COG_IR_OP_CONST: {
            char *expr = constant_expr(backend, instruction->as.constant.constant);
            if (!set_value_expr(exprs, value_count, result, expr))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_FUNCTION_REF: {
            CogIrFunctionId target = instruction->as.function_ref.function;
            if ((size_t)target >= backend->module->function_count)
                goto invalid_result;
            if (!set_value_expr(exprs, value_count, result,
                                copy_printf("%s", backend->function_names[target])))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_LOCAL_ADDR: {
            CogIrSlotId slot = instruction->as.local_addr.slot;
            if ((size_t)slot >= function->slot_count)
                goto invalid_result;
            const CogIrType *slot_type = cog_ir_get_type(backend->module, function->slots[slot].type);
            char *address = slot_type && slot_type->kind == COG_IR_TYPE_ARRAY
                ? copy_printf("&cg_s_%u.cg_e", slot)
                : copy_printf("&cg_s_%u", slot);
            if (!set_value_expr(exprs, value_count, result, address))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_GLOBAL_ADDR: {
            CogIrGlobalId global = instruction->as.global_addr.global;
            if ((size_t)global >= backend->module->global_count)
                goto invalid_result;
            const CogIrType *global_type = cog_ir_get_type(backend->module, backend->module->globals[global].type);
            char *address = global_type && global_type->kind == COG_IR_TYPE_ARRAY
                ? copy_printf("&%s.cg_e", backend->global_names[global])
                : copy_printf("&%s", backend->global_names[global]);
            if (!set_value_expr(exprs, value_count, result, address))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_FIELD_ADDR: {
            const char *base = value_expr(exprs, value_count, instruction->as.field_addr.base);
            const CogIrValue *base_value = cog_ir_get_value(function, instruction->as.field_addr.base);
            const CogIrType *base_type = base_value
                ? cog_ir_get_type(backend->module, base_value->type)
                : NULL;
            const CogIrType *aggregate = NULL;
            if (base_type && base_type->kind == COG_IR_TYPE_POINTER)
                aggregate = cog_ir_get_type(backend->module, base_type->as.pointer.pointee);
            if (!base || !aggregate ||
                (aggregate->kind != COG_IR_TYPE_STRUCT && aggregate->kind != COG_IR_TYPE_UNION) ||
                instruction->as.field_addr.field_index >= aggregate->as.aggregate.field_count)
                goto missing_operand;
            if (!set_value_expr(
                    exprs,
                    value_count,
                    result,
                    copy_printf(
                        "&((%s)->cg_f_%u)",
                        base,
                        instruction->as.field_addr.field_index)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_ARRAY_ELEM_ADDR: {
            const char *base = value_expr(exprs, value_count, instruction->as.index_addr.base);
            const char *index = value_expr(exprs, value_count, instruction->as.index_addr.index);
            if (!base || !index)
                goto missing_operand;
            if (!set_value_expr(exprs, value_count, result,
                                copy_printf("&((*%s)[%s])", base, index)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_PTR_INDEX_ADDR: {
            const char *base = value_expr(exprs, value_count, instruction->as.index_addr.base);
            const char *index = value_expr(exprs, value_count, instruction->as.index_addr.index);
            if (!base || !index)
                goto missing_operand;
            if (!set_value_expr(exprs, value_count, result,
                                copy_printf("&((%s)[%s])", base, index)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_LOAD: {
            const char *address = value_expr(exprs, value_count, instruction->as.load.address);
            const char *type = value_type_name(backend, function, result, instruction->span);
            const CogIrType *result_ir_type = cog_ir_get_type(backend->module, instruction->result_type);
            if (!address || !type || !result_ir_type)
                goto missing_operand;

            if (result_ir_type->kind == COG_IR_TYPE_ARRAY) {
                fprintf(backend->out, "    %s cg_v_%u = {0};\n", type, result);
                char *destination = copy_printf("cg_v_%u.cg_e", result);
                char *source = copy_printf("*(%s)", address);
                if (!destination || !source ||
                    !emit_array_storage_copy(
                        backend,
                        result_ir_type->id,
                        destination,
                        source,
                        instruction->as.load.is_volatile)) {
                    free(destination);
                    free(source);
                    goto missing_operand;
                }
                free(destination);
                free(source);
            } else if (instruction->as.load.is_volatile) {
                const CogIrValue *address_value = cog_ir_get_value(function, instruction->as.load.address);
                const char *address_type = address_value
                    ? value_type_name(backend, function, address_value->id, instruction->span)
                    : NULL;
                if (!address_type)
                    goto missing_operand;
                fprintf(
                    backend->out,
                    "    %s cg_v_%u = *((%s)(%s));\n",
                    type,
                    result,
                    address_type,
                    address
                );
            } else {
                fprintf(backend->out, "    %s cg_v_%u = *%s;\n", type, result, address);
            }
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_STORE: {
            const char *address = value_expr(exprs, value_count, instruction->as.store.address);
            const char *value = value_expr(exprs, value_count, instruction->as.store.value);
            const CogIrValue *stored_value = cog_ir_get_value(function, instruction->as.store.value);
            const CogIrType *stored_type = stored_value
                ? cog_ir_get_type(backend->module, stored_value->type)
                : NULL;
            if (!address || !value || !stored_type)
                goto missing_operand;

            if (stored_type->kind == COG_IR_TYPE_ARRAY) {
                char *destination = copy_printf("*(%s)", address);
                char *source = copy_printf("(%s).cg_e", value);
                if (!destination || !source ||
                    !emit_array_storage_copy(
                        backend,
                        stored_type->id,
                        destination,
                        source,
                        instruction->as.store.is_volatile)) {
                    free(destination);
                    free(source);
                    goto missing_operand;
                }
                free(destination);
                free(source);
            } else if (instruction->as.store.is_volatile) {
                const CogIrValue *address_value = cog_ir_get_value(function, instruction->as.store.address);
                const char *address_type = address_value
                    ? value_type_name(backend, function, address_value->id, instruction->span)
                    : NULL;
                if (!address_type)
                    goto missing_operand;
                fprintf(
                    backend->out,
                    "    *((%s)(%s)) = %s;\n",
                    address_type,
                    address,
                    value
                );
            } else {
                fprintf(backend->out, "    *%s = %s;\n", address, value);
            }
            return 1;
        }
        case COG_IR_OP_MAKE_STRUCT: {
            const CogIrType *aggregate = cog_ir_get_type(backend->module, instruction->result_type);
            const char *type = value_type_name(backend, function, result, instruction->span);
            if (!aggregate || aggregate->kind != COG_IR_TYPE_STRUCT || !type)
                goto missing_operand;
            fprintf(backend->out, "    %s cg_v_%u = {0};\n", type, result);
            for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
                const char *value = value_expr(exprs, value_count, instruction->as.aggregate.values[i]);
                const CogIrType *field_type = i < aggregate->as.aggregate.field_count
                    ? cog_ir_get_type(backend->module, aggregate->as.aggregate.fields[i].type)
                    : NULL;
                if (!value || !field_type)
                    goto missing_operand;
                if (field_type->kind == COG_IR_TYPE_ARRAY) {
                    char *destination = copy_printf("cg_v_%u.cg_f_%zu", result, i);
                    char *source = copy_printf("(%s).cg_e", value);
                    if (!destination || !source ||
                        !emit_array_storage_copy(
                            backend, field_type->id, destination, source, 0)) {
                        free(destination);
                        free(source);
                        goto missing_operand;
                    }
                    free(destination);
                    free(source);
                } else {
                    fprintf(backend->out, "    cg_v_%u.cg_f_%zu = %s;\n", result, i, value);
                }
            }
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_MAKE_ARRAY: {
            const CogIrType *array = cog_ir_get_type(backend->module, instruction->result_type);
            const char *type = value_type_name(backend, function, result, instruction->span);
            const CogIrType *element = array && array->kind == COG_IR_TYPE_ARRAY
                ? cog_ir_get_type(backend->module, array->as.array.element_type)
                : NULL;
            if (!array || array->kind != COG_IR_TYPE_ARRAY || !element || !type)
                goto missing_operand;
            fprintf(backend->out, "    %s cg_v_%u = {0};\n", type, result);
            for (size_t i = 0; i < instruction->as.aggregate.value_count; ++i) {
                const char *value = value_expr(exprs, value_count, instruction->as.aggregate.values[i]);
                if (!value)
                    goto missing_operand;
                if (element->kind == COG_IR_TYPE_ARRAY) {
                    char *destination = copy_printf("cg_v_%u.cg_e[%zu]", result, i);
                    char *source = copy_printf("(%s).cg_e", value);
                    if (!destination || !source ||
                        !emit_array_storage_copy(backend, element->id, destination, source, 0)) {
                        free(destination);
                        free(source);
                        goto missing_operand;
                    }
                    free(destination);
                    free(source);
                } else {
                    fprintf(backend->out, "    cg_v_%u.cg_e[%zu] = %s;\n", result, i, value);
                }
            }
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_EXTRACT_FIELD: {
            const char *aggregate_expr = value_expr(exprs, value_count, instruction->as.extract.aggregate);
            const CogIrValue *aggregate_value = cog_ir_get_value(function, instruction->as.extract.aggregate);
            const CogIrType *aggregate = aggregate_value
                ? cog_ir_get_type(backend->module, aggregate_value->type)
                : NULL;
            const CogIrType *result_ir_type = cog_ir_get_type(backend->module, instruction->result_type);
            const char *type = value_type_name(backend, function, result, instruction->span);
            uint32_t index = instruction->as.extract.index;
            if (!aggregate_expr || !aggregate || !result_ir_type || !type ||
                (aggregate->kind != COG_IR_TYPE_STRUCT && aggregate->kind != COG_IR_TYPE_UNION) ||
                index >= aggregate->as.aggregate.field_count)
                goto missing_operand;
            if (result_ir_type->kind == COG_IR_TYPE_ARRAY) {
                fprintf(backend->out, "    %s cg_v_%u = {0};\n", type, result);
                char *destination = copy_printf("cg_v_%u.cg_e", result);
                char *source = copy_printf("(%s).cg_f_%u", aggregate_expr, index);
                if (!destination || !source ||
                    !emit_array_storage_copy(backend, result_ir_type->id, destination, source, 0)) {
                    free(destination);
                    free(source);
                    goto missing_operand;
                }
                free(destination);
                free(source);
            } else {
                fprintf(
                    backend->out,
                    "    %s cg_v_%u = (%s).cg_f_%u;\n",
                    type, result, aggregate_expr, index);
            }
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_EXTRACT_ELEMENT: {
            const char *aggregate_expr = value_expr(exprs, value_count, instruction->as.extract.aggregate);
            const CogIrValue *aggregate_value = cog_ir_get_value(function, instruction->as.extract.aggregate);
            const CogIrType *array = aggregate_value
                ? cog_ir_get_type(backend->module, aggregate_value->type)
                : NULL;
            const CogIrType *result_ir_type = cog_ir_get_type(backend->module, instruction->result_type);
            const char *type = value_type_name(backend, function, result, instruction->span);
            uint32_t index = instruction->as.extract.index;
            if (!aggregate_expr || !array || array->kind != COG_IR_TYPE_ARRAY ||
                !result_ir_type || !type || index >= array->as.array.length)
                goto missing_operand;
            if (result_ir_type->kind == COG_IR_TYPE_ARRAY) {
                fprintf(backend->out, "    %s cg_v_%u = {0};\n", type, result);
                char *destination = copy_printf("cg_v_%u.cg_e", result);
                char *source = copy_printf("(%s).cg_e[%u]", aggregate_expr, index);
                if (!destination || !source ||
                    !emit_array_storage_copy(backend, result_ir_type->id, destination, source, 0)) {
                    free(destination);
                    free(source);
                    goto missing_operand;
                }
                free(destination);
                free(source);
            } else {
                fprintf(
                    backend->out,
                    "    %s cg_v_%u = (%s).cg_e[%u];\n",
                    type, result, aggregate_expr, index);
            }
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_PTR_QUALIFY: {
            const char *operand = value_expr(exprs, value_count, instruction->as.conversion.operand);
            if (!operand)
                goto missing_operand;
            /* C's implicit qualification conversion has exactly the semantics of
             * this IR operation; retaining the source expression also preserves
             * an exact C ABI pointer spelling produced by a direct C call. */
            if (!set_value_expr(exprs, value_count, result, copy_printf("%s", operand)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_C_VARARG_PROMOTE: {
            const char *operand = value_expr(exprs, value_count, instruction->as.conversion.operand);
            const CogIrType *type = cog_ir_get_type(backend->module, instruction->result_type);
            if (!operand || !type)
                goto missing_operand;
            const char *c_type = type->kind == COG_IR_TYPE_FLOAT ? "double" : "int";
            fprintf(backend->out, "    %s cg_v_%u = (%s)(%s);\n", c_type, result, c_type, operand);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_CAST_CHECKED:
            if (!emit_checked_cast_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;
        case COG_IR_OP_INT_TRUNCATE:
            if (!emit_truncating_integer_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;
        case COG_IR_OP_PTR_REINTERPRET:
            if (!emit_pointer_reinterpret_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;
        case COG_IR_OP_ICMP_EQ:
        case COG_IR_OP_ICMP_NE:
        case COG_IR_OP_ICMP_SLT:
        case COG_IR_OP_ICMP_SLE:
        case COG_IR_OP_ICMP_SGT:
        case COG_IR_OP_ICMP_SGE:
        case COG_IR_OP_ICMP_ULT:
        case COG_IR_OP_ICMP_ULE:
        case COG_IR_OP_ICMP_UGT:
        case COG_IR_OP_ICMP_UGE: {
            const char *lhs = value_expr(exprs, value_count, instruction->as.binary.lhs);
            const char *rhs = value_expr(exprs, value_count, instruction->as.binary.rhs);
            if (!lhs || !rhs)
                goto missing_operand;

            const char *operator_text = NULL;
            switch (instruction->op) {
                case COG_IR_OP_ICMP_EQ:  operator_text = "=="; break;
                case COG_IR_OP_ICMP_NE:  operator_text = "!="; break;
                case COG_IR_OP_ICMP_SLT:
                case COG_IR_OP_ICMP_ULT: operator_text = "<"; break;
                case COG_IR_OP_ICMP_SLE:
                case COG_IR_OP_ICMP_ULE: operator_text = "<="; break;
                case COG_IR_OP_ICMP_SGT:
                case COG_IR_OP_ICMP_UGT: operator_text = ">"; break;
                case COG_IR_OP_ICMP_SGE:
                case COG_IR_OP_ICMP_UGE: operator_text = ">="; break;
                default: break;
            }
            if (!operator_text)
                goto invalid_result;

            fprintf(
                backend->out,
                "    _Bool cg_v_%u = (%s) %s (%s);\n",
                result,
                lhs,
                operator_text,
                rhs
            );
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_BIT_AND:
        case COG_IR_OP_BIT_OR:
        case COG_IR_OP_BIT_XOR:
        case COG_IR_OP_FADD:
        case COG_IR_OP_FSUB:
        case COG_IR_OP_FMUL:
        case COG_IR_OP_FDIV: {
            const char *lhs = value_expr(exprs, value_count, instruction->as.binary.lhs);
            const char *rhs = value_expr(exprs, value_count, instruction->as.binary.rhs);
            const char *type = runtime_type_name(backend, instruction->result_type, instruction->span);
            if (!lhs || !rhs || !type)
                goto missing_operand;

            const char *operator_text = NULL;
            switch (instruction->op) {
                case COG_IR_OP_BIT_AND: operator_text = "&"; break;
                case COG_IR_OP_BIT_OR:  operator_text = "|"; break;
                case COG_IR_OP_BIT_XOR: operator_text = "^"; break;
                case COG_IR_OP_FADD:    operator_text = "+"; break;
                case COG_IR_OP_FSUB:    operator_text = "-"; break;
                case COG_IR_OP_FMUL:    operator_text = "*"; break;
                case COG_IR_OP_FDIV:    operator_text = "/"; break;
                default: break;
            }
            if (!operator_text)
                goto invalid_result;

            fprintf(backend->out, "    %s cg_v_%u = (%s) %s (%s);\n",
                    type, result, lhs, operator_text, rhs);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_BIT_NOT:
        case COG_IR_OP_FNEG: {
            const char *operand = value_expr(exprs, value_count, instruction->as.unary.operand);
            const char *type = runtime_type_name(backend, instruction->result_type, instruction->span);
            if (!operand || !type)
                goto missing_operand;
            const char *operator_text = instruction->op == COG_IR_OP_BIT_NOT ? "~" : "-";
            fprintf(backend->out, "    %s cg_v_%u = %s(%s);\n",
                    type, result, operator_text, operand);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_FCMP_EQ:
        case COG_IR_OP_FCMP_NE:
        case COG_IR_OP_FCMP_LT:
        case COG_IR_OP_FCMP_LE:
        case COG_IR_OP_FCMP_GT:
        case COG_IR_OP_FCMP_GE:
        case COG_IR_OP_PTR_EQ:
        case COG_IR_OP_PTR_NE: {
            const char *lhs = value_expr(exprs, value_count, instruction->as.binary.lhs);
            const char *rhs = value_expr(exprs, value_count, instruction->as.binary.rhs);
            if (!lhs || !rhs)
                goto missing_operand;

            const char *operator_text = NULL;
            switch (instruction->op) {
                case COG_IR_OP_FCMP_EQ:
                case COG_IR_OP_PTR_EQ: operator_text = "=="; break;
                case COG_IR_OP_FCMP_NE:
                case COG_IR_OP_PTR_NE: operator_text = "!="; break;
                case COG_IR_OP_FCMP_LT: operator_text = "<"; break;
                case COG_IR_OP_FCMP_LE: operator_text = "<="; break;
                case COG_IR_OP_FCMP_GT: operator_text = ">"; break;
                case COG_IR_OP_FCMP_GE: operator_text = ">="; break;
                default: break;
            }
            if (!operator_text)
                goto invalid_result;

            fprintf(backend->out, "    _Bool cg_v_%u = (%s) %s (%s);\n",
                    result, lhs, operator_text, rhs);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_BOOL_NOT: {
            const char *operand = value_expr(exprs, value_count, instruction->as.unary.operand);
            if (!operand)
                goto missing_operand;
            fprintf(backend->out, "    _Bool cg_v_%u = !(%s);\n", result, operand);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_SHL_CHECKED_COUNT:
        case COG_IR_OP_SHR_SIGNED_CHECKED_COUNT:
        case COG_IR_OP_SHR_UNSIGNED_CHECKED_COUNT:
            if (!emit_shift_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;
        case COG_IR_OP_IADD_WRAP:
        case COG_IR_OP_ISUB_WRAP:
        case COG_IR_OP_IMUL_WRAP:
        case COG_IR_OP_INEG_WRAP:
            if (!emit_wrapping_integer_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;
        case COG_IR_OP_CALL: {
            const char *callee = value_expr(exprs, value_count, instruction->as.call.callee);
            const CogIrValue *callee_value = cog_ir_get_value(function, instruction->as.call.callee);
            const CogIrType *callee_type = callee_value
                ? cog_ir_get_type(backend->module, callee_value->type)
                : NULL;
            if (!callee || !callee_type || callee_type->kind != COG_IR_TYPE_FUNCTION)
                goto missing_operand;
            if (callee_type->as.function.abi == COG_IR_ABI_C) {
                const CogIrAbiType *callee_abi = callee_value->abi_type != COG_IR_ABI_TYPE_INVALID
                    ? cog_ir_get_abi_type(backend->module, callee_value->abi_type)
                    : NULL;
                if (!callee_abi || callee_abi->kind != COG_IR_ABI_TYPE_FUNCTION) {
                    backend_error(backend, instruction->span,
                                  "C function value is missing exact callback ABI metadata");
                    return 0;
                }
            }

            if (instruction->result_type != COG_IR_TYPE_INVALID) {
                const char *result_type = value_type_name(backend, function, result, instruction->span);
                if (!result_type)
                    return 0;
                fprintf(backend->out, "    %s cg_v_%u = %s(", result_type, result, callee);
            } else {
                fprintf(backend->out, "    %s(", callee);
            }
            for (size_t a = 0; a < instruction->as.call.argument_count; ++a) {
                const char *argument = value_expr(exprs, value_count, instruction->as.call.arguments[a]);
                if (!argument)
                    goto missing_operand;
                fprintf(backend->out, "%s%s", a ? ", " : "", argument);
            }
            fputs(");\n", backend->out);
            if (instruction->result_type != COG_IR_TYPE_INVALID &&
                !set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }

        case COG_IR_OP_IADD_CHECKED:
        case COG_IR_OP_ISUB_CHECKED:
        case COG_IR_OP_IMUL_CHECKED:
        case COG_IR_OP_IDIV_CHECKED:
        case COG_IR_OP_IREM_CHECKED:
        case COG_IR_OP_INEG_CHECKED:
            if (!emit_checked_integer_instruction(backend, function, instruction, exprs)) {
                if (!backend->had_error)
                    goto missing_operand;
                return 0;
            }
            return 1;

        default:
            backend_error(backend, instruction->span, "unhandled CogIR operation reached the host-C backend");
            return 0;
    }

invalid_result:
    backend_error(backend, instruction->span, "invalid CogIR instruction result during C lowering");
    return 0;
missing_operand:
    backend_error(backend, instruction->span, "missing CogIR operand expression during C lowering");
    return 0;
}

static int emit_branch_edge(
    CBackend *backend,
    const CogIrFunction *function,
    CogIrBlockId source_block,
    const CogIrBranchEdge *edge,
    char **exprs,
    size_t edge_tag,
    const char *indent
) {
    const CogIrBlock *target = cog_ir_get_block(function, edge->target);
    if (!target || target->parameter_count != edge->argument_count) {
        backend_error(backend, source_span_invalid(), "invalid CogIR branch edge during C lowering");
        return 0;
    }

    for (size_t i = 0; i < edge->argument_count; ++i) {
        const char *argument = value_expr(exprs, function->value_count, edge->arguments[i]);
        const char *type = runtime_type_name(backend, target->parameters[i].type, target->parameters[i].span);
        if (!argument || !type) {
            backend_error(backend, target->parameters[i].span, "branch edge argument has no C value expression");
            return 0;
        }
        fprintf(
            backend->out,
            "%s%s cg_edge_%u_%zu_%zu = %s;\n",
            indent,
            type,
            source_block,
            edge_tag,
            i,
            argument
        );
    }

    for (size_t i = 0; i < edge->argument_count; ++i) {
        const char *parameter = value_expr(exprs, function->value_count, target->parameters[i].value);
        if (!parameter) {
            backend_error(backend, target->parameters[i].span, "branch target parameter has no C storage");
            return 0;
        }
        fprintf(
            backend->out,
            "%s%s = cg_edge_%u_%zu_%zu;\n",
            indent,
            parameter,
            source_block,
            edge_tag,
            i
        );
    }

    fprintf(backend->out, "%sgoto cg_bb_%u;\n", indent, edge->target);
    return 1;
}

static int emit_terminator(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrBlock *block,
    char **exprs
) {
    const CogIrTerminator *terminator = &block->terminator;
    switch (terminator->kind) {
        case COG_IR_TERMINATOR_BR:
            return emit_branch_edge(
                backend,
                function,
                block->id,
                &terminator->as.branch.edge,
                exprs,
                0,
                "    "
            );

        case COG_IR_TERMINATOR_COND_BR: {
            const char *condition = value_expr(
                exprs,
                function->value_count,
                terminator->as.cond_branch.condition
            );
            if (!condition) {
                backend_error(backend, terminator->span, "conditional branch has no C condition expression");
                return 0;
            }
            fprintf(backend->out, "    if (%s) {\n", condition);
            if (!emit_branch_edge(
                    backend,
                    function,
                    block->id,
                    &terminator->as.cond_branch.if_true,
                    exprs,
                    0,
                    "        "))
                return 0;
            fputs("    } else {\n", backend->out);
            if (!emit_branch_edge(
                    backend,
                    function,
                    block->id,
                    &terminator->as.cond_branch.if_false,
                    exprs,
                    1,
                    "        "))
                return 0;
            fputs("    }\n", backend->out);
            return 1;
        }

        case COG_IR_TERMINATOR_SWITCH: {
            const char *value = value_expr(
                exprs,
                function->value_count,
                terminator->as.switch_term.value
            );
            if (!value) {
                backend_error(backend, terminator->span, "switch terminator has no C value expression");
                return 0;
            }

            fprintf(backend->out, "    switch (%s) {\n", value);
            for (size_t i = 0; i < terminator->as.switch_term.case_count; ++i) {
                char *key = constant_expr(backend, terminator->as.switch_term.cases[i].key);
                if (!key) {
                    if (!backend->had_error)
                        backend_error(backend, terminator->span, "switch case has no C constant expression");
                    return 0;
                }
                fprintf(backend->out, "        case %s: {\n", key);
                free(key);
                if (!emit_branch_edge(
                        backend,
                        function,
                        block->id,
                        &terminator->as.switch_term.cases[i].edge,
                        exprs,
                        i,
                        "            "))
                    return 0;
                fputs("        }\n", backend->out);
            }
            fputs("        default: {\n", backend->out);
            if (!emit_branch_edge(
                    backend,
                    function,
                    block->id,
                    &terminator->as.switch_term.default_edge,
                    exprs,
                    terminator->as.switch_term.case_count,
                    "            "))
                return 0;
            fputs("        }\n    }\n", backend->out);
            return 1;
        }

        case COG_IR_TERMINATOR_RET:
            if (terminator->as.ret.has_value) {
                const char *value = value_expr(
                    exprs,
                    function->value_count,
                    terminator->as.ret.value
                );
                if (!value) {
                    backend_error(backend, terminator->span, "return terminator has no C value expression");
                    return 0;
                }
                fprintf(backend->out, "    return %s;\n", value);
            } else {
                fputs("    return;\n", backend->out);
            }
            return 1;

        case COG_IR_TERMINATOR_TRAP:
            fputs("    abort();\n", backend->out);
            return 1;

        case COG_IR_TERMINATOR_UNREACHABLE:
            fputs("    abort(); /* CogIR unreachable */\n", backend->out);
            return 1;

        case COG_IR_TERMINATOR_NONE:
            backend_error(backend, terminator->span, "CogIR block has no terminator during C lowering");
            return 0;
    }
    return 0;
}

static int mark_reachable_edge(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrBranchEdge *edge,
    unsigned char *reachable,
    int *changed,
    SourceSpan span
) {
    if (edge->target == COG_IR_BLOCK_INVALID || (size_t)edge->target >= function->block_count) {
        backend_error(backend, span, "invalid CogIR branch target during C lowering");
        return 0;
    }
    if (!reachable[edge->target]) {
        reachable[edge->target] = 1;
        *changed = 1;
    }
    return 1;
}

static int compute_reachable_blocks(
    CBackend *backend,
    const CogIrFunction *function,
    unsigned char *reachable
) {
    reachable[function->entry_block] = 1;

    int changed;
    do {
        changed = 0;
        for (size_t b = 0; b < function->block_count; ++b) {
            if (!reachable[b])
                continue;

            const CogIrTerminator *terminator = &function->blocks[b].terminator;
            switch (terminator->kind) {
                case COG_IR_TERMINATOR_BR:
                    if (!mark_reachable_edge(
                            backend,
                            function,
                            &terminator->as.branch.edge,
                            reachable,
                            &changed,
                            terminator->span))
                        return 0;
                    break;

                case COG_IR_TERMINATOR_COND_BR:
                    if (!mark_reachable_edge(
                            backend,
                            function,
                            &terminator->as.cond_branch.if_true,
                            reachable,
                            &changed,
                            terminator->span) ||
                        !mark_reachable_edge(
                            backend,
                            function,
                            &terminator->as.cond_branch.if_false,
                            reachable,
                            &changed,
                            terminator->span))
                        return 0;
                    break;

                case COG_IR_TERMINATOR_SWITCH:
                    for (size_t i = 0; i < terminator->as.switch_term.case_count; ++i) {
                        if (!mark_reachable_edge(
                                backend,
                                function,
                                &terminator->as.switch_term.cases[i].edge,
                                reachable,
                                &changed,
                                terminator->span))
                            return 0;
                    }
                    if (!mark_reachable_edge(
                            backend,
                            function,
                            &terminator->as.switch_term.default_edge,
                            reachable,
                            &changed,
                            terminator->span))
                        return 0;
                    break;

                case COG_IR_TERMINATOR_RET:
                case COG_IR_TERMINATOR_TRAP:
                case COG_IR_TERMINATOR_UNREACHABLE:
                    break;

                case COG_IR_TERMINATOR_NONE:
                    backend_error(backend, terminator->span, "CogIR block has no terminator during C lowering");
                    return 0;
            }
        }
    } while (changed);

    return 1;
}

static int emit_function_body(CBackend *backend, const CogIrFunction *function)
{
    if (function->kind != COG_IR_FUNCTION_DEFINITION ||
        function->linkage == COG_IR_LINKAGE_EXTERNAL)
        return 1;

    if (function->block_count == 0 || function->entry_block == COG_IR_BLOCK_INVALID ||
        (size_t)function->entry_block >= function->block_count) {
        backend_error(backend, function->span, "CogIR function has no valid entry block during C lowering");
        return 0;
    }
    const CogIrBlock *entry = cog_ir_get_block(function, function->entry_block);
    if (!entry || entry->parameter_count != 0) {
        backend_error(backend, function->span, "CogIR entry block must not have parameters during C lowering");
        return 0;
    }

    unsigned char *reachable = calloc(function->block_count, sizeof(*reachable));
    if (!reachable) {
        backend_error(backend, function->span, "out of memory while computing CogIR CFG reachability");
        return 0;
    }
    if (!compute_reachable_blocks(backend, function, reachable)) {
        free(reachable);
        return 0;
    }

    const char *result = function_result_type(backend, function);
    if (!result) {
        free(reachable);
        return 0;
    }
    const char *macro = c_call_macro_name(function->abi.calling_convention);
    const char *sep = function->abi.calling_convention == COG_IR_CALL_DEFAULT ? "" : " ";
    fprintf(
        backend->out,
        "static %s%s%s %s(",
        result, sep, macro, backend->function_names[function->id]
    );
    emit_function_parameter_list(backend, function, 1);
    if (backend->had_error) {
        free(reachable);
        return 0;
    }
    fputs(")\n{\n", backend->out);

    for (size_t s = 0; s < function->slot_count; ++s) {
        const CogIrSlot *slot = &function->slots[s];
        const char *type = slot->abi_type != COG_IR_ABI_TYPE_INVALID
            ? abi_type_name(backend, slot->abi_type, slot->span)
            : runtime_type_name(backend, slot->type, slot->span);
        if (!type) {
            free(reachable);
            return 0;
        }
        fprintf(backend->out, "    %s cg_s_%zu;\n", type, s);
    }
    if (function->slot_count)
        fputc('\n', backend->out);

    char **exprs = function->value_count ? calloc(function->value_count, sizeof(*exprs)) : NULL;
    if (function->value_count && !exprs) {
        free(reachable);
        backend_error(backend, function->span, "out of memory while lowering CogIR function body to C");
        return 0;
    }

    for (size_t p = 0; p < function->parameter_count; ++p) {
        CogIrValueId value = function->parameters[p];
        if (!set_value_expr(exprs, function->value_count, value, copy_printf("cg_p_%zu", p))) {
            backend_error(backend, function->span, "invalid CogIR function parameter value");
            goto fail;
        }
    }

    int has_block_parameters = 0;
    for (size_t b = 0; b < function->block_count; ++b) {
        if (!reachable[b])
            continue;
        const CogIrBlock *block = &function->blocks[b];
        for (size_t p = 0; p < block->parameter_count; ++p) {
            const CogIrBlockParam *parameter = &block->parameters[p];
            const char *type = value_type_name(backend, function, parameter->value, parameter->span);
            if (!type)
                goto fail;
            fprintf(backend->out, "    %s cg_bp_%u;\n", type, parameter->value);
            if (!set_value_expr(
                    exprs,
                    function->value_count,
                    parameter->value,
                    copy_printf("cg_bp_%u", parameter->value))) {
                backend_error(backend, parameter->span, "invalid CogIR block parameter value");
                goto fail;
            }
            has_block_parameters = 1;
        }
    }
    if (has_block_parameters)
        fputc('\n', backend->out);

    fprintf(backend->out, "    goto cg_bb_%u;\n\n", function->entry_block);

    for (size_t b = 0; b < function->block_count; ++b) {
        if (!reachable[b])
            continue;
        const CogIrBlock *block = &function->blocks[b];
        fprintf(backend->out, "cg_bb_%u: ;\n", block->id);
        for (size_t i = 0; i < block->instruction_count; ++i) {
            if (!emit_instruction(backend, function, &block->instructions[i], exprs))
                goto fail;
        }
        if (!emit_terminator(backend, function, block, exprs))
            goto fail;
        fputc('\n', backend->out);
    }

    fputs("}\n\n", backend->out);
    for (size_t i = 0; i < function->value_count; ++i)
        free(exprs[i]);
    free(exprs);
    free(reachable);
    return 1;

fail:
    for (size_t i = 0; i < function->value_count; ++i)
        free(exprs[i]);
    free(exprs);
    free(reachable);
    return 0;
}

static int emit_function_bodies(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->function_count; ++i)
        if (!emit_function_body(backend, &backend->module->functions[i]))
            return 0;
    return !backend->had_error;
}

static int emit_entrypoint(CBackend *backend)
{
    const CogIrFunction *main_function =
        cog_ir_get_function(backend->module, backend->module->entry_function);
    if (!main_function) {
        backend_error(backend, source_span_invalid(), "host executable backend requires a top-level Coglet 'main' function");
        return 0;
    }

    fputs("int main(void)\n{\n", backend->out);
    if (backend->module->init_function != COG_IR_FUNCTION_INVALID) {
        if ((size_t)backend->module->init_function >= backend->module->function_count) {
            backend_error(backend, main_function->span, "module init function is invalid during C lowering");
            return 0;
        }
        fprintf(backend->out, "    %s();\n", backend->function_names[backend->module->init_function]);
    }
    fprintf(backend->out, "    int32_t cg_entry_result = %s();\n", backend->function_names[main_function->id]);
    fputs("    return (int)cg_entry_result;\n}\n", backend->out);
    return 1;
}

static CBackendStatus c_backend_emit_stream(FILE *out, const CogIrModule *module)
{
    if (!out || !module) {
        fprintf(stderr, "C backend error: missing CogIR module\n");
        return C_BACKEND_STATUS_UNSUPPORTED;
    }
    if (!cog_ir_module_is_frozen(module)) {
        fprintf(stderr, "C backend error: CogIR module must be frozen before backend emission\n");
        return C_BACKEND_STATUS_UNSUPPORTED;
    }

    TargetInfo host_target = target_info_host();
    if (!target_info_equal(&module->target, &host_target)) {
        fprintf(stderr, "C backend error: host-C backend cannot emit a non-host target\n");
        return C_BACKEND_STATUS_UNSUPPORTED;
    }

    CBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.out = out;
    backend.module = module;
    if (!init_backend_storage(&backend)) {
        fprintf(stderr, "error: out of memory while initializing host-C backend\n");
        free_backend_storage(&backend);
        return C_BACKEND_STATUS_UNSUPPORTED;
    }

    fputs("/* Generated from CogIR by Coglet's host-C backend. */\n", out);
    fputs(
        "#include <float.h>\n"
        "#include <math.h>\n"
        "#include <stddef.h>\n"
        "#include <string.h>\n"
        "#include <stdint.h>\n"
        "#include <stdlib.h>\n\n",
        out
    );
    emit_repr_c_layout_support(&backend);
    emit_c_calling_convention_support(&backend);
    emit_runtime_forward_declarations(&backend);

    if (!emit_enum_definitions(&backend) || !prepare_type_aliases(&backend))
        goto unsupported;

    size_t printed_type_definition_count = backend.type_definition_count;
    for (size_t i = 0; i < printed_type_definition_count; ++i)
        fprintf(out, "%s\n", backend.type_definitions[i]);
    if (printed_type_definition_count)
        fputc('\n', out);

    if (!emit_runtime_definitions(&backend) ||
        !prepare_late_value_aliases(&backend))
        goto unsupported;

    for (size_t i = printed_type_definition_count; i < backend.type_definition_count; ++i)
        fprintf(out, "%s\n", backend.type_definitions[i]);
    if (backend.type_definition_count > printed_type_definition_count)
        fputc('\n', out);

    if (!emit_globals(&backend) ||
        !emit_function_declarations(&backend) ||
        !emit_function_bodies(&backend) ||
        !emit_entrypoint(&backend))
        goto unsupported;

    if (ferror(out)) {
        fprintf(stderr, "error: failed while writing generated C output\n");
        free_backend_storage(&backend);
        return C_BACKEND_STATUS_IO_ERROR;
    }

    free_backend_storage(&backend);
    return C_BACKEND_STATUS_OK;

unsupported:
    free_backend_storage(&backend);
    return C_BACKEND_STATUS_UNSUPPORTED;
}

CBackendStatus c_backend_emit_file(
    const char *output_path,
    const CogIrModule *module
) {
    if (!output_path) {
        fprintf(stderr, "error: no C output path provided\n");
        return C_BACKEND_STATUS_IO_ERROR;
    }

    FILE *out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "error: could not open '%s' for generated C output: %s\n",
                output_path, strerror(errno));
        return C_BACKEND_STATUS_IO_ERROR;
    }

    CBackendStatus status = c_backend_emit_stream(out, module);
    if (fclose(out) != 0 && status == C_BACKEND_STATUS_OK) {
        fprintf(stderr, "error: could not close generated C output '%s': %s\n",
                output_path, strerror(errno));
        return C_BACKEND_STATUS_IO_ERROR;
    }
    return status;
}

CBackendStatus c_backend_build_executable(
    const char *output_path,
    const CogIrModule *module,
    const CBackendLinkOptions *link_options
) {
#if defined(__unix__) || defined(__APPLE__)
    if (!output_path) {
        fprintf(stderr, "error: no executable output path provided\n");
        return C_BACKEND_STATUS_IO_ERROR;
    }

    char temp_path[] = "/tmp/coglet-c-XXXXXX";
    int fd = mkstemp(temp_path);
    if (fd < 0) {
        fprintf(stderr, "error: could not create temporary C file: %s\n", strerror(errno));
        return C_BACKEND_STATUS_IO_ERROR;
    }

    FILE *out = fdopen(fd, "wb");
    if (!out) {
        fprintf(stderr, "error: could not open temporary C stream: %s\n", strerror(errno));
        close(fd);
        unlink(temp_path);
        return C_BACKEND_STATUS_IO_ERROR;
    }

    CBackendStatus emit_status = c_backend_emit_stream(out, module);
    if (fclose(out) != 0 && emit_status == C_BACKEND_STATUS_OK) {
        fprintf(stderr, "error: could not close temporary C output: %s\n", strerror(errno));
        unlink(temp_path);
        return C_BACKEND_STATUS_IO_ERROR;
    }
    if (emit_status != C_BACKEND_STATUS_OK) {
        unlink(temp_path);
        return emit_status;
    }

    int library_dir_count = link_options ? link_options->library_dir_count : 0;
    int library_count = link_options ? link_options->library_count : 0;
    int cc_arg_count = 9 + (library_dir_count * 2) + (library_count * 2);
    char **cc_argv = calloc((size_t)cc_arg_count + 1, sizeof(*cc_argv));
    if (!cc_argv) {
        fprintf(stderr, "error: could not allocate native C compiler arguments\n");
        unlink(temp_path);
        return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }

    int arg = 0;
    cc_argv[arg++] = "cc";
    cc_argv[arg++] = "-std=c99";
    cc_argv[arg++] = "-Wall";
    cc_argv[arg++] = "-Wextra";
    cc_argv[arg++] = "-x";
    cc_argv[arg++] = "c";
    cc_argv[arg++] = temp_path;
    cc_argv[arg++] = "-o";
    cc_argv[arg++] = (char *)output_path;
    for (int i = 0; i < library_dir_count; ++i) {
        cc_argv[arg++] = "-L";
        cc_argv[arg++] = (char *)link_options->library_dirs[i];
    }
    for (int i = 0; i < library_count; ++i) {
        cc_argv[arg++] = "-l";
        cc_argv[arg++] = (char *)link_options->libraries[i];
    }
    cc_argv[arg] = NULL;

    pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "error: could not start native C compiler: %s\n", strerror(errno));
        free(cc_argv);
        unlink(temp_path);
        return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }
    if (child == 0) {
        execvp("cc", cc_argv);
        fprintf(stderr, "error: could not execute native C compiler 'cc': %s\n", strerror(errno));
        _exit(127);
    }

    int wait_status = 0;
    if (waitpid(child, &wait_status, 0) < 0) {
        fprintf(stderr, "error: failed waiting for native C compiler: %s\n", strerror(errno));
        free(cc_argv);
        unlink(temp_path);
        return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }

    free(cc_argv);
    unlink(temp_path);
    if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
        fprintf(stderr, "error: native C compiler/linker failed\n");
        return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
    }
    return C_BACKEND_STATUS_OK;
#else
    (void)output_path;
    (void)module;
    (void)link_options;
    fprintf(stderr, "error: executable host-C backend is not implemented on this host platform\n");
    return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
#endif
}
