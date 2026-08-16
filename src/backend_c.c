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
    unsigned char *aggregate_definition_state;

    char **type_definitions;
    size_t type_definition_count;
    size_t type_definition_capacity;
} CBackend;

static int sv_equals(StringView view, const char *text)
{
    size_t length = strlen(text);
    return view.length == length && memcmp(view.data, text, length) == 0;
}

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
    free(backend->aggregate_definition_state);
}

static int init_backend_storage(CBackend *backend)
{
    const CogIrModule *module = backend->module;

    if (module->type_count) {
        backend->type_names = calloc(module->type_count, sizeof(*backend->type_names));
        backend->runtime_alias_state = calloc(module->type_count, 1);
        backend->aggregate_definition_state = calloc(module->type_count, 1);
        if (!backend->type_names || !backend->runtime_alias_state ||
            !backend->aggregate_definition_state)
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
            if (!type->as.aggregate.is_repr_c) {
                backend_error(backend, span, "ordinary Coglet aggregate types are not lowered by the host-C backend yet");
                return NULL;
            }
            return nominal_type_name(backend, type);
        case COG_IR_TYPE_ENUM:
            if (!type->as.enumeration.is_repr_c) {
                backend_error(backend, span, "ordinary Coglet enum types are not lowered by the host-C backend yet");
                return NULL;
            }
            return nominal_type_name(backend, type);
        case COG_IR_TYPE_ARRAY:
            backend_error(backend, span, "standalone CogIR array values are not lowered by the host-C backend yet");
            return NULL;
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

static void emit_aggregate_forward_declarations(CBackend *backend)
{
    int emitted = 0;
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if ((type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION) ||
            !type->as.aggregate.is_repr_c)
            continue;
        const char *name = nominal_type_name(backend, type);
        fprintf(
            backend->out,
            "typedef %s %s %s;\n",
            type->kind == COG_IR_TYPE_UNION ? "union" : "struct",
            name,
            name
        );
        emitted = 1;
    }
    if (emitted)
        fputc('\n', backend->out);
}

static int emit_enum_definitions(CBackend *backend)
{
    int emitted = 0;
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if (type->kind != COG_IR_TYPE_ENUM || !type->as.enumeration.is_repr_c)
            continue;
        const char *backing = abi_type_name(backend, type->as.enumeration.backing_abi_type, type->span);
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

static int prepare_type_aliases(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->type_count; ++i) {
        const CogIrType *type = &backend->module->types[i];
        if ((type->kind == COG_IR_TYPE_STRUCT || type->kind == COG_IR_TYPE_UNION) &&
            type->as.aggregate.is_repr_c && !type->as.aggregate.is_incomplete) {
            for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
                const CogIrAggregateField *field = &type->as.aggregate.fields[f];
                if (!prepare_abi_field_type(backend, field->abi_type, field->span))
                    return 0;
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
        for (size_t s = 0; s < function->slot_count; ++s)
            if (!runtime_type_name(backend, function->slots[s].type, function->slots[s].span))
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
        if (runtime->kind != COG_IR_TYPE_ARRAY || runtime->as.array.length == 0) {
            backend_error(backend, span, "array ABI field has invalid runtime array type");
            return 0;
        }
        char nested[128];
        int written = snprintf(nested, sizeof(nested), "%s[%zu]", name, runtime->as.array.length);
        if (written < 0 || (size_t)written >= sizeof(nested)) {
            backend_error(backend, span, "aggregate field declarator is too long");
            return 0;
        }
        return emit_abi_field_declaration(
            backend,
            abi->element_type,
            runtime->as.array.element_type,
            nested,
            span
        );
    }

    const char *type = abi_type_name(backend, abi_id, span);
    if (!type)
        return 0;
    fprintf(backend->out, "    %s %s;\n", type, name);
    return 1;
}

static int emit_aggregate_definition(CBackend *backend, CogIrTypeId type_id)
{
    const CogIrType *type = cog_ir_get_type(backend->module, type_id);
    if (!type || (type->kind != COG_IR_TYPE_STRUCT && type->kind != COG_IR_TYPE_UNION) ||
        !type->as.aggregate.is_repr_c)
        return 1;

    unsigned char *state = &backend->aggregate_definition_state[type_id];
    if (*state == 2)
        return 1;
    if (type->as.aggregate.is_incomplete) {
        *state = 2;
        return 1;
    }
    if (*state == 1) {
        backend_error(backend, type->span, "recursive #repr(c) by-value aggregate layout reached C lowering");
        return 0;
    }
    *state = 1;

    for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
        const CogIrType *field = cog_ir_get_type(backend->module, type->as.aggregate.fields[f].type);
        while (field && field->kind == COG_IR_TYPE_ARRAY)
            field = cog_ir_get_type(backend->module, field->as.array.element_type);
        if (field && (field->kind == COG_IR_TYPE_STRUCT || field->kind == COG_IR_TYPE_UNION) &&
            field->as.aggregate.is_repr_c) {
            if (!emit_aggregate_definition(backend, field->id))
                return 0;
        }
    }

    const char *name = nominal_type_name(backend, type);
    fprintf(
        backend->out,
        "%s %s {\n",
        type->kind == COG_IR_TYPE_UNION ? "union" : "struct",
        name
    );
    for (size_t f = 0; f < type->as.aggregate.field_count; ++f) {
        char field_name[32];
        snprintf(field_name, sizeof(field_name), "cg_f_%zu", f);
        const CogIrAggregateField *field = &type->as.aggregate.fields[f];
        if (!emit_abi_field_declaration(
                backend, field->abi_type, field->type, field_name, field->span))
            return 0;
    }
    fputs("}", backend->out);
    if (type->as.aggregate.is_packed)
        fputs(" CG_REPR_C_PACKED", backend->out);
    if (type->as.aggregate.explicit_alignment > 0)
        fprintf(backend->out, " CG_REPR_C_ALIGNED(%u)", type->as.aggregate.explicit_alignment);
    fputs(";\n\n", backend->out);
    *state = 2;
    return 1;
}

static int emit_aggregate_definitions(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->type_count; ++i)
        if (!emit_aggregate_definition(backend, (CogIrTypeId)i))
            return 0;
    return !backend->had_error;
}

static int string_global_bytes(
    CBackend *backend,
    const CogIrGlobal *global,
    const unsigned char **unused
) {
    (void)unused;
    const CogIrType *type = cog_ir_get_type(backend->module, global->type);
    const CogIrConstant *init = cog_ir_get_constant(backend->module, global->static_initializer);
    if (!type || type->kind != COG_IR_TYPE_ARRAY ||
        !global->is_compiler_generated || !global->is_readonly ||
        !sv_equals(global->debug_name, ".str") ||
        !init || init->kind != COG_IR_CONST_ARRAY ||
        init->as.aggregate.element_count != type->as.array.length)
        return 0;
    const CogIrType *element = cog_ir_get_type(backend->module, type->as.array.element_type);
    return element && element->kind == COG_IR_TYPE_INTEGER && element->as.integer.bits == 8;
}

static int emit_globals(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->global_count; ++i) {
        const CogIrGlobal *global = &backend->module->globals[i];
        if (!string_global_bytes(backend, global, NULL)) {
            backend_error(backend, global->span, "non-string CogIR globals are not lowered by the host-C backend yet");
            return 0;
        }
        const CogIrType *type = cog_ir_get_type(backend->module, global->type);
        const CogIrConstant *init = cog_ir_get_constant(backend->module, global->static_initializer);
        fprintf(backend->out, "static const char %s[%zu] = {", backend->global_names[i], type->as.array.length);
        for (size_t e = 0; e < init->as.aggregate.element_count; ++e) {
            const CogIrConstant *element = cog_ir_get_constant(backend->module, init->as.aggregate.elements[e]);
            if (!element || element->kind != COG_IR_CONST_INTEGER) {
                backend_error(backend, global->span, "string global has non-integer byte constant");
                return 0;
            }
            fprintf(backend->out, "%s0x%02" PRIx64, e ? ", " : "", element->as.integer_bits & UINT64_C(0xff));
        }
        fputs("};\n", backend->out);
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
    return function->abi.abi == COG_IR_ABI_C
        ? abi_type_name(backend, function->abi.parameter_abi_types[index], function->span)
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

static char *constant_expr(CBackend *backend, CogIrConstId constant_id)
{
    const CogIrConstant *constant = cog_ir_get_constant(backend->module, constant_id);
    if (!constant)
        return NULL;

    switch (constant->kind) {
        case COG_IR_CONST_ZERO: {
            const CogIrType *type = cog_ir_get_type(backend->module, constant->type);
            if (type && (type->kind == COG_IR_TYPE_POINTER ||
                         type->kind == COG_IR_TYPE_OPAQUE_POINTER ||
                         type->kind == COG_IR_TYPE_FUNCTION))
                return copy_printf("NULL");
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
            backend_error(backend, source_span_invalid(), "aggregate constants are not lowered as executable C values yet");
            return NULL;
    }
    return NULL;
}

static int emit_instruction(
    CBackend *backend,
    const CogIrFunction *function,
    const CogIrInstruction *instruction,
    char **exprs,
    CogIrFunctionId *function_refs
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
            function_refs[result] = target;
            return 1;
        }
        case COG_IR_OP_LOCAL_ADDR: {
            CogIrSlotId slot = instruction->as.local_addr.slot;
            if ((size_t)slot >= function->slot_count)
                goto invalid_result;
            if (!set_value_expr(exprs, value_count, result, copy_printf("&cg_s_%u", slot)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_GLOBAL_ADDR: {
            CogIrGlobalId global = instruction->as.global_addr.global;
            if ((size_t)global >= backend->module->global_count)
                goto invalid_result;
            if (!set_value_expr(exprs, value_count, result,
                                copy_printf("&%s", backend->global_names[global])))
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
        case COG_IR_OP_LOAD: {
            const char *address = value_expr(exprs, value_count, instruction->as.load.address);
            const char *type = runtime_type_name(backend, instruction->result_type, instruction->span);
            if (!address || !type)
                goto missing_operand;
            fprintf(backend->out, "    %s cg_v_%u = *%s;\n", type, result, address);
            if (!set_value_expr(exprs, value_count, result, copy_printf("cg_v_%u", result)))
                goto invalid_result;
            return 1;
        }
        case COG_IR_OP_STORE: {
            const char *address = value_expr(exprs, value_count, instruction->as.store.address);
            const char *value = value_expr(exprs, value_count, instruction->as.store.value);
            if (!address || !value)
                goto missing_operand;
            fprintf(backend->out, "    *%s = %s;\n", address, value);
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
        case COG_IR_OP_CALL: {
            const char *callee = value_expr(exprs, value_count, instruction->as.call.callee);
            if (!callee)
                goto missing_operand;
            CogIrFunctionId callee_id = COG_IR_FUNCTION_INVALID;
            if ((size_t)instruction->as.call.callee < value_count)
                callee_id = function_refs[instruction->as.call.callee];
            if (callee_id == COG_IR_FUNCTION_INVALID) {
                backend_error(backend, instruction->span, "indirect CogIR calls are not lowered by the host-C backend yet");
                return 0;
            }

            const CogIrFunction *callee_function = cog_ir_get_function(backend->module, callee_id);
            if (!callee_function)
                goto missing_operand;

            if (instruction->result_type != COG_IR_TYPE_INVALID) {
                const char *result_type = callee_function->abi.abi == COG_IR_ABI_C
                    ? abi_type_name(backend, callee_function->abi.return_abi_type, instruction->span)
                    : runtime_type_name(backend, instruction->result_type, instruction->span);
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
            backend_error(backend, instruction->span, "checked arithmetic CogIR operations are not lowered by the host-C backend yet");
            return 0;

        default:
            backend_error(backend, instruction->span, "CogIR operation is outside the current host-C backend execution subset");
            return 0;
    }

invalid_result:
    backend_error(backend, instruction->span, "invalid CogIR instruction result during C lowering");
    return 0;
missing_operand:
    backend_error(backend, instruction->span, "missing CogIR operand expression during C lowering");
    return 0;
}

static int emit_function_body(CBackend *backend, const CogIrFunction *function)
{
    if (function->kind != COG_IR_FUNCTION_DEFINITION ||
        function->linkage == COG_IR_LINKAGE_EXTERNAL)
        return 1;

    if (function->block_count != 1 || function->entry_block == COG_IR_BLOCK_INVALID) {
        backend_error(backend, function->span, "structured CogIR CFG emission is not implemented by the host-C backend yet");
        return 0;
    }
    const CogIrBlock *block = cog_ir_get_block(function, function->entry_block);
    if (!block || block->parameter_count != 0) {
        backend_error(backend, function->span, "CogIR block parameters are not lowered by the host-C backend yet");
        return 0;
    }

    const char *result = function_result_type(backend, function);
    if (!result)
        return 0;
    const char *macro = c_call_macro_name(function->abi.calling_convention);
    const char *sep = function->abi.calling_convention == COG_IR_CALL_DEFAULT ? "" : " ";
    fprintf(
        backend->out,
        "static %s%s%s %s(",
        result, sep, macro, backend->function_names[function->id]
    );
    emit_function_parameter_list(backend, function, 1);
    if (backend->had_error)
        return 0;
    fputs(")\n{\n", backend->out);

    for (size_t s = 0; s < function->slot_count; ++s) {
        const CogIrSlot *slot = &function->slots[s];
        const char *type = runtime_type_name(backend, slot->type, slot->span);
        if (!type)
            return 0;
        fprintf(backend->out, "    %s cg_s_%zu;\n", type, s);
    }
    if (function->slot_count)
        fputc('\n', backend->out);

    char **exprs = function->value_count ? calloc(function->value_count, sizeof(*exprs)) : NULL;
    CogIrFunctionId *function_refs = function->value_count
        ? malloc(function->value_count * sizeof(*function_refs))
        : NULL;
    if (function->value_count && (!exprs || !function_refs)) {
        free(exprs); free(function_refs);
        backend_error(backend, function->span, "out of memory while lowering CogIR function body to C");
        return 0;
    }
    for (size_t i = 0; i < function->value_count; ++i)
        function_refs[i] = COG_IR_FUNCTION_INVALID;

    for (size_t p = 0; p < function->parameter_count; ++p) {
        CogIrValueId value = function->parameters[p];
        if (!set_value_expr(exprs, function->value_count, value, copy_printf("cg_p_%zu", p))) {
            backend_error(backend, function->span, "invalid CogIR function parameter value");
            goto fail;
        }
    }

    for (size_t i = 0; i < block->instruction_count; ++i) {
        if (!emit_instruction(backend, function, &block->instructions[i], exprs, function_refs))
            goto fail;
    }

    switch (block->terminator.kind) {
        case COG_IR_TERMINATOR_RET:
            if (block->terminator.as.ret.has_value) {
                const char *value = value_expr(
                    exprs, function->value_count, block->terminator.as.ret.value);
                if (!value) {
                    backend_error(backend, block->terminator.span, "return terminator has no C value expression");
                    goto fail;
                }
                fprintf(backend->out, "    return %s;\n", value);
            } else {
                fputs("    return;\n", backend->out);
            }
            break;
        default:
            backend_error(backend, block->terminator.span, "CogIR terminator is outside the current host-C backend execution subset");
            goto fail;
    }

    fputs("}\n\n", backend->out);
    for (size_t i = 0; i < function->value_count; ++i)
        free(exprs[i]);
    free(exprs);
    free(function_refs);
    return 1;

fail:
    for (size_t i = 0; i < function->value_count; ++i)
        free(exprs[i]);
    free(exprs);
    free(function_refs);
    return 0;
}

static int emit_function_bodies(CBackend *backend)
{
    for (size_t i = 0; i < backend->module->function_count; ++i)
        if (!emit_function_body(backend, &backend->module->functions[i]))
            return 0;
    return !backend->had_error;
}

static const CogIrFunction *find_main_function(const CBackend *backend)
{
    for (size_t i = 0; i < backend->module->function_count; ++i) {
        const CogIrFunction *function = &backend->module->functions[i];
        if (sv_equals(function->debug_name, "main"))
            return function;
    }
    return NULL;
}

static int emit_entrypoint(CBackend *backend)
{
    const CogIrFunction *main_function = find_main_function(backend);
    if (!main_function ||
        main_function->linkage != COG_IR_LINKAGE_INTERNAL ||
        main_function->abi.abi != COG_IR_ABI_COGLET) {
        backend_error(backend, source_span_invalid(), "host executable backend requires a top-level Coglet 'main' function");
        return 0;
    }
    const CogIrType *type = cog_ir_get_type(backend->module, main_function->type);
    if (!type || type->kind != COG_IR_TYPE_FUNCTION ||
        type->as.function.parameter_count != 0 ||
        main_function->source_return_c_scalar_kind != COG_IR_C_SCALAR_INT) {
        backend_error(backend, main_function->span, "host executable entry point must have signature 'main::() -> c_int'");
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
    fprintf(backend->out, "    return %s();\n}\n", backend->function_names[main_function->id]);
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
    fputs("#include <stddef.h>\n#include <stdint.h>\n\n", out);
    emit_repr_c_layout_support(&backend);
    emit_c_calling_convention_support(&backend);
    emit_aggregate_forward_declarations(&backend);

    if (!emit_enum_definitions(&backend) || !prepare_type_aliases(&backend))
        goto unsupported;

    for (size_t i = 0; i < backend.type_definition_count; ++i)
        fprintf(out, "%s\n", backend.type_definitions[i]);
    if (backend.type_definition_count)
        fputc('\n', out);

    if (!emit_aggregate_definitions(&backend) ||
        !emit_globals(&backend) ||
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
