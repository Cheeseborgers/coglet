#define _POSIX_C_SOURCE 200809L

#include "backend_c.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "string_decode.h"
#include "utils/string_view.h"

#define C_BACKEND_MAX_FUNCTIONS 512
#define C_BACKEND_MAX_STRUCTS 256
#define C_BACKEND_MAX_ENUMS 256
#define C_BACKEND_MAX_TYPE_ALIASES 1024
#define C_BACKEND_NAME_SIZE 48
#define C_BACKEND_TYPE_DEF_SIZE 1024

typedef struct CFunction {
    Node *node;
    char generated_name[C_BACKEND_NAME_SIZE];
} CFunction;

typedef struct CTypeAlias {
    const Type *type;
    char name[C_BACKEND_NAME_SIZE];
    char definition[C_BACKEND_TYPE_DEF_SIZE];
} CTypeAlias;

typedef struct CStruct {
    Node *node;
    const Type *type;
    char generated_name[C_BACKEND_NAME_SIZE];
    unsigned char definition_state;
} CStruct;

typedef struct CEnum {
    Node *node;
    const Type *type;
    char generated_name[C_BACKEND_NAME_SIZE];
} CEnum;

typedef struct CBackend {
    FILE *out;
    const char *source_filename;
    Node *program;
    SemanticContext *sem;

    CFunction functions[C_BACKEND_MAX_FUNCTIONS];
    int function_count;

    CStruct structs[C_BACKEND_MAX_STRUCTS];
    int struct_count;

    CEnum enums[C_BACKEND_MAX_ENUMS];
    int enum_count;

    CTypeAlias type_aliases[C_BACKEND_MAX_TYPE_ALIASES];
    int type_alias_count;

    Node *current_function;
    int had_error;
} CBackend;

static int sv_equals(StringView view, const char *text)
{
    size_t length = strlen(text);
    return view.length == length && memcmp(view.data, text, length) == 0;
}

static void backend_error(CBackend *backend, const Node *node, const char *message)
{
    if (!backend) return;

    fprintf(
        stderr,
        "%s:%d: backend error: %s\n",
        backend->source_filename ? backend->source_filename : "<input>",
        node ? node->line : 0,
        message
    );

    backend->had_error = 1;
}

static const CTypeAlias *find_type_alias(CBackend *backend, const Type *type)
{
    for (int i = 0; i < backend->type_alias_count; i++) {
        if (backend->type_aliases[i].type == type)
            return &backend->type_aliases[i];
    }

    return NULL;
}

static const CStruct *find_c_struct_by_name(CBackend *backend, StringView name)
{
    for (int i = 0; i < backend->struct_count; i++) {
        StringView candidate = backend->structs[i].node->as.struct_decl.name;
        if (candidate.length == name.length &&
            memcmp(candidate.data, name.data, name.length) == 0)
            return &backend->structs[i];
    }

    return NULL;
}

static const CStruct *find_c_struct_by_type(CBackend *backend, const Type *type)
{
    for (int i = 0; i < backend->struct_count; i++) {
        if (backend->structs[i].type == type)
            return &backend->structs[i];
    }

    return NULL;
}

static const CEnum *find_c_enum_by_name(CBackend *backend, StringView name)
{
    for (int i = 0; i < backend->enum_count; i++) {
        StringView candidate = backend->enums[i].node->as.enum_decl.name;
        if (candidate.length == name.length &&
            memcmp(candidate.data, name.data, name.length) == 0)
            return &backend->enums[i];
    }

    return NULL;
}

static const CEnum *find_c_enum_by_type(CBackend *backend, const Type *type)
{
    for (int i = 0; i < backend->enum_count; i++) {
        if (backend->enums[i].type == type)
            return &backend->enums[i];
    }

    return NULL;
}

static const char *base_c_type_name(const Type *type)
{
    if (!type) return NULL;

    switch (type->kind) {
        case TYPE_VOID: return "void";
        case TYPE_BOOL: return "_Bool";

        case TYPE_I8:  return "int8_t";
        case TYPE_I16: return "int16_t";
        case TYPE_I32: return "int32_t";
        case TYPE_I64: return "int64_t";

        case TYPE_U8:  return "uint8_t";
        case TYPE_U16: return "uint16_t";
        case TYPE_U32: return "uint32_t";
        case TYPE_U64: return "uint64_t";

        case TYPE_F32: return "float";
        case TYPE_F64: return "double";

        case TYPE_NAMED:
            if (sv_equals(type->named_name, "c_char"))      return "char";
            if (sv_equals(type->named_name, "c_schar"))     return "signed char";
            if (sv_equals(type->named_name, "c_uchar"))     return "unsigned char";
            if (sv_equals(type->named_name, "c_short"))     return "short";
            if (sv_equals(type->named_name, "c_ushort"))    return "unsigned short";
            if (sv_equals(type->named_name, "c_int"))       return "int";
            if (sv_equals(type->named_name, "c_uint"))      return "unsigned int";
            if (sv_equals(type->named_name, "c_long"))      return "long";
            if (sv_equals(type->named_name, "c_ulong"))     return "unsigned long";
            if (sv_equals(type->named_name, "c_longlong"))  return "long long";
            if (sv_equals(type->named_name, "c_ulonglong")) return "unsigned long long";
            if (sv_equals(type->named_name, "c_size"))      return "size_t";
            if (sv_equals(type->named_name, "c_bool"))      return "_Bool";
            if (sv_equals(type->named_name, "c_float"))     return "float";
            if (sv_equals(type->named_name, "c_double"))    return "double";
            return NULL;

        case TYPE_UNTYPED_INT:
        case TYPE_UNTYPED_FLOAT:
        case TYPE_NULL:
        case TYPE_POINTER:
        case TYPE_OPAQUE_POINTER:
        case TYPE_ARRAY:
        case TYPE_STRUCT:
        case TYPE_ENUM:
        case TYPE_FUNCTION:
            return NULL;
    }

    return NULL;
}

static const char *register_c_type(CBackend *backend, const Type *type, const Node *owner)
{
    const char *base = base_c_type_name(type);
    if (base) return base;

    if (!type) {
        backend_error(backend, owner, "missing type during C lowering");
        return NULL;
    }

    if (type->kind == TYPE_NAMED) {
        const CStruct *structure = find_c_struct_by_name(backend, type->named_name);
        if (structure) return structure->generated_name;

        const CEnum *enumeration = find_c_enum_by_name(backend, type->named_name);
        if (enumeration) return enumeration->generated_name;
    }

    if (type->kind == TYPE_STRUCT) {
        const CStruct *structure = find_c_struct_by_type(backend, type);
        if (structure) return structure->generated_name;
    }

    if (type->kind == TYPE_ENUM) {
        const CEnum *enumeration = find_c_enum_by_type(backend, type);
        if (enumeration) return enumeration->generated_name;
    }

    if (type->kind == TYPE_FUNCTION) {
        if (type->function_abi != FUNCTION_ABI_C) {
            backend_error(
                backend,
                owner,
                "ordinary Coglet function types cannot be lowered as native C callbacks"
            );
            return NULL;
        }

        const CTypeAlias *existing_function = find_type_alias(backend, type);
        if (existing_function) return existing_function->name;

        const char *return_name = register_c_type(backend, type->return_type, owner);
        if (!return_name) return NULL;

        /* Register nested parameter types before allocating this alias. */
        for (int i = 0; i < type->parameter_count; i++) {
            if (!register_c_type(backend, type->parameters[i], owner))
                return NULL;
        }

        if (backend->type_alias_count >= C_BACKEND_MAX_TYPE_ALIASES) {
            backend_error(backend, owner, "too many generated C callback types");
            return NULL;
        }

        int index = backend->type_alias_count;
        CTypeAlias *alias = &backend->type_aliases[backend->type_alias_count++];
        memset(alias, 0, sizeof(*alias));
        alias->type = type;
        snprintf(alias->name, sizeof(alias->name), "cg_type_%d", index);

        char return_copy[C_BACKEND_NAME_SIZE];
        char alias_copy[C_BACKEND_NAME_SIZE];

        if (strlen(return_name) >= sizeof(return_copy) ||
            strlen(alias->name) >= sizeof(alias_copy)) {
            backend_error(backend, owner, "generated C callback type name is too long");
            return NULL;
        }

        strcpy(return_copy, return_name);
        strcpy(alias_copy, alias->name);

        int written = snprintf(
            alias->definition,
            sizeof(alias->definition),
            "typedef %s (*%s)(",
            return_copy,
            alias_copy
        );

        if (written < 0 || (size_t)written >= sizeof(alias->definition)) {
            backend_error(backend, owner, "generated C callback typedef is too long");
            return NULL;
        }

        size_t used = (size_t)written;

        if (type->parameter_count == 0) {
            written = snprintf(
                alias->definition + used,
                sizeof(alias->definition) - used,
                "void"
            );
            if (written < 0 || (size_t)written >= sizeof(alias->definition) - used) {
                backend_error(backend, owner, "generated C callback typedef is too long");
                return NULL;
            }
            used += (size_t)written;
        } else {
            for (int i = 0; i < type->parameter_count; i++) {
                const char *parameter_name =
                    register_c_type(backend, type->parameters[i], owner);
                if (!parameter_name) return NULL;

                written = snprintf(
                    alias->definition + used,
                    sizeof(alias->definition) - used,
                    "%s%s",
                    i > 0 ? ", " : "",
                    parameter_name
                );

                if (written < 0 || (size_t)written >= sizeof(alias->definition) - used) {
                    backend_error(backend, owner, "generated C callback typedef is too long");
                    return NULL;
                }
                used += (size_t)written;
            }
        }

        written = snprintf(
            alias->definition + used,
            sizeof(alias->definition) - used,
            ");"
        );
        if (written < 0 || (size_t)written >= sizeof(alias->definition) - used) {
            backend_error(backend, owner, "generated C callback typedef is too long");
            return NULL;
        }

        return alias->name;
    }

    if (type->kind != TYPE_POINTER && type->kind != TYPE_OPAQUE_POINTER) {
        backend_error(
            backend,
            owner,
            "type is not supported by the current host-C backend subset"
        );
        return NULL;
    }

    const CTypeAlias *existing = find_type_alias(backend, type);
    if (existing) return existing->name;

    if (backend->type_alias_count >= C_BACKEND_MAX_TYPE_ALIASES) {
        backend_error(backend, owner, "too many generated C pointer types");
        return NULL;
    }

    char element_name[C_BACKEND_NAME_SIZE];
    element_name[0] = '\0';

    if (type->kind == TYPE_POINTER) {
        const char *resolved_element = register_c_type(backend, type->element, owner);
        if (!resolved_element) return NULL;

        if (strlen(resolved_element) >= sizeof(element_name)) {
            backend_error(backend, owner, "generated C element type name is too long");
            return NULL;
        }

        strcpy(element_name, resolved_element);
    }

    int index = backend->type_alias_count;
    CTypeAlias *alias = &backend->type_aliases[backend->type_alias_count++];
    memset(alias, 0, sizeof(*alias));
    alias->type = type;

    snprintf(alias->name, sizeof(alias->name), "cg_type_%d", index);

    char alias_name[C_BACKEND_NAME_SIZE];
    strcpy(alias_name, alias->name);

    if (type->kind == TYPE_OPAQUE_POINTER) {
        snprintf(
            alias->definition,
            sizeof(alias->definition),
            "typedef %svoid *%s;",
            type->pointer_access == POINTER_ACCESS_READONLY ? "const " : "",
            alias_name
        );
    } else {
        snprintf(
            alias->definition,
            sizeof(alias->definition),
            "typedef %s%s *%s;",
            element_name,
            type->pointer_access == POINTER_ACCESS_READONLY ? " const" : "",
            alias_name
        );
    }

    return alias->name;
}

static CFunction *find_function(CBackend *backend, StringView name)
{
    for (int i = 0; i < backend->function_count; i++) {
        Node *func = backend->functions[i].node;
        StringView candidate = func->as.func_decl.name;

        if (candidate.length == name.length &&
            memcmp(candidate.data, name.data, name.length) == 0) {
            return &backend->functions[i];
        }
    }

    return NULL;
}

static int collect_enums(CBackend *backend)
{
    if (!backend->program || backend->program->type != NODE_PROGRAM) {
        backend_error(backend, backend->program, "expected program node");
        return 0;
    }

    NodeList statements = backend->program->as.program.statements;

    for (int i = 0; i < statements.count; i++) {
        Node *node = statements.items[i];
        if (!node || node->type != NODE_ENUM_DECL || !node->as.enum_decl.is_repr_c)
            continue;

        if (backend->enum_count >= C_BACKEND_MAX_ENUMS) {
            backend_error(backend, node, "too many #repr(c) enums for current C backend");
            return 0;
        }

        if (!node->as.enum_decl.resolved_type) {
            backend_error(backend, node, "missing resolved #repr(c) enum type during C lowering");
            return 0;
        }

        CEnum *enumeration = &backend->enums[backend->enum_count];
        memset(enumeration, 0, sizeof(*enumeration));
        enumeration->node = node;
        enumeration->type = node->as.enum_decl.resolved_type;
        snprintf(
            enumeration->generated_name,
            sizeof(enumeration->generated_name),
            "cg_enum_%d",
            backend->enum_count
        );
        backend->enum_count++;
    }

    return !backend->had_error;
}

static int collect_structs(CBackend *backend)
{
    if (!backend->program || backend->program->type != NODE_PROGRAM) {
        backend_error(backend, backend->program, "expected program node");
        return 0;
    }

    NodeList statements = backend->program->as.program.statements;

    for (int i = 0; i < statements.count; i++) {
        Node *node = statements.items[i];
        if (!node || node->type != NODE_STRUCT_DECL || !node->as.struct_decl.is_repr_c)
            continue;

        if (backend->struct_count >= C_BACKEND_MAX_STRUCTS) {
            backend_error(backend, node, "too many #repr(c) structs for current C backend");
            return 0;
        }

        if (!node->as.struct_decl.resolved_type) {
            backend_error(backend, node, "missing resolved #repr(c) struct type during C lowering");
            return 0;
        }

        CStruct *structure = &backend->structs[backend->struct_count];
        memset(structure, 0, sizeof(*structure));
        structure->node = node;
        structure->type = node->as.struct_decl.resolved_type;
        snprintf(
            structure->generated_name,
            sizeof(structure->generated_name),
            "cg_struct_%d",
            backend->struct_count
        );
        backend->struct_count++;
    }

    return !backend->had_error;
}

static int collect_functions(CBackend *backend)
{
    if (!backend->program || backend->program->type != NODE_PROGRAM) {
        backend_error(backend, backend->program, "expected program node");
        return 0;
    }

    NodeList statements = backend->program->as.program.statements;

    for (int i = 0; i < statements.count; i++) {
        Node *node = statements.items[i];
        if (!node || node->type != NODE_FUNC_DECL)
            continue;

        if (backend->function_count >= C_BACKEND_MAX_FUNCTIONS) {
            backend_error(backend, node, "too many functions for current C backend");
            return 0;
        }

        CFunction *function = &backend->functions[backend->function_count];
        memset(function, 0, sizeof(*function));
        function->node = node;
        snprintf(
            function->generated_name,
            sizeof(function->generated_name),
            "cg_fn_%d",
            backend->function_count
        );

        backend->function_count++;
    }

    return !backend->had_error;
}

static int prepare_struct_field_type(CBackend *backend, const Type *type, const Node *owner)
{
    if (!type) {
        backend_error(backend, owner, "missing struct field type during C lowering");
        return 0;
    }

    if (type->kind == TYPE_ARRAY) {
        if (type->array_size <= 0 || !type->element) {
            backend_error(backend, owner, "invalid #repr(c) array field during C lowering");
            return 0;
        }

        return prepare_struct_field_type(backend, type->element, owner);
    }

    return register_c_type(backend, type, owner) != NULL;
}

static int prepare_struct_types(CBackend *backend)
{
    for (int i = 0; i < backend->struct_count; i++) {
        Node *decl = backend->structs[i].node;

        for (int f = 0; f < decl->as.struct_decl.fields.count; f++) {
            Node *field = decl->as.struct_decl.fields.items[f];
            if (!prepare_struct_field_type(backend, field->as.struct_field_decl.var_type, field))
                return 0;
        }
    }

    return !backend->had_error;
}

static int prepare_function_types(CBackend *backend)
{
    for (int i = 0; i < backend->function_count; i++) {
        Node *func = backend->functions[i].node;

        if (!register_c_type(backend, func->as.func_decl.return_type, func))
            return 0;

        for (int p = 0; p < func->as.func_decl.params.count; p++) {
            Node *param = func->as.func_decl.params.items[p];
            if (!register_c_type(backend, param->as.param_decl.var_type, param))
                return 0;
        }
    }

    return !backend->had_error;
}

static void emit_c_string_literal(FILE *out, StringView value)
{
    fputc('"', out);

    for (size_t i = 0; i < value.length; i++) {
        unsigned char ch = (unsigned char)value.data[i];

        switch (ch) {
            case '\\': fputs("\\\\", out); break;
            case '"':  fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (ch >= 32 && ch <= 126) {
                    fputc((int)ch, out);
                } else {
                    fprintf(out, "\\%03o", (unsigned int)ch);
                }
                break;
        }
    }

    fputc('"', out);
}

static int emit_expression(CBackend *backend, Node *node);

static int emit_integer_value(CBackend *backend, const Node *owner, IntegerValue value)
{
    if (value.is_negative) {
        if (value.magnitude > ((uint64_t)INT64_MAX + 1u)) {
            backend_error(backend, owner, "negative enum value exceeds host-C backend integer range");
            return 0;
        }

        if (value.magnitude == ((uint64_t)INT64_MAX + 1u)) {
            fputs("(-9223372036854775807LL - 1LL)", backend->out);
            return 1;
        }

        fprintf(backend->out, "(-%" PRIu64 "LL)", value.magnitude);
        return 1;
    }

    fprintf(backend->out, "%" PRIu64 "ULL", value.magnitude);
    return 1;
}

static int emit_enum_member(CBackend *backend, Node *node)
{
    SemExprInfo *info = semantic_get_expr_info(backend->sem, node);
    if (!info || !info->type || info->type->kind != TYPE_ENUM)
        return 0;

    Type *enum_type = info->type;
    for (int i = 0; i < enum_type->enum_member_count; i++) {
        EnumMember *member = &enum_type->enum_members[i];
        if (member->name.length == node->as.field.name.length &&
            memcmp(member->name.data, node->as.field.name.data, member->name.length) == 0) {
            const CEnum *enumeration = find_c_enum_by_type(backend, enum_type);
            if (!enumeration) {
                backend_error(backend, node, "enum member belongs to a non-#repr(c) enum in C lowering");
                return 0;
            }

            fprintf(backend->out, "((%s)", enumeration->generated_name);
            if (!emit_integer_value(backend, node, member->value))
                return 0;
            fputc(')', backend->out);
            return 1;
        }
    }

    backend_error(backend, node, "could not resolve enum member during C lowering");
    return 0;
}

static int emit_integer_literal(CBackend *backend, Node *node)
{
    uint64_t value = node->as.number.value.integer;

    if (value > (uint64_t)INT64_MAX) {
        backend_error(
            backend,
            node,
            "integer literal exceeds the first host-C backend literal subset"
        );
        return 0;
    }

    fprintf(backend->out, "%" PRIu64, value);
    return 1;
}

static int emit_float_literal(CBackend *backend, Node *node)
{
    /*
     * `%a` is a C99 hexadecimal floating literal representation. It round-
     * trips the AST's host-double value exactly and avoids locale-sensitive
     * decimal formatting. Contextual conversion to a C `float` parameter or
     * return type performs the same binary32 rounding required by Coglet f32.
     */
    fprintf(backend->out, "%a", node->as.number.value.floating);
    return 1;
}

static int emit_parameter_identifier(CBackend *backend, Node *node)
{
    if (!backend->current_function)
        return 0;

    for (int i = 0; i < backend->current_function->as.func_decl.params.count; i++) {
        Node *param = backend->current_function->as.func_decl.params.items[i];
        StringView name = param->as.param_decl.name;

        if (name.length == node->as.ident.length &&
            memcmp(name.data, node->as.ident.data, name.length) == 0) {
            fprintf(backend->out, "cg_p_%d", i);
            return 1;
        }
    }

    return 0;
}

static int emit_call(CBackend *backend, Node *node)
{
    Node *callee = node->as.call.callee;

    if (!callee || callee->type != NODE_IDENT) {
        backend_error(
            backend,
            node,
            "current host-C backend supports only direct calls to named functions"
        );
        return 0;
    }

    CFunction *function = find_function(backend, callee->as.ident);
    if (function) {
        fputs(function->generated_name, backend->out);
    } else if (!emit_parameter_identifier(backend, callee)) {
        backend_error(backend, node, "could not resolve call target during C lowering");
        return 0;
    }

    fputc('(', backend->out);

    for (int i = 0; i < node->as.call.arguments.count; i++) {
        if (i > 0) fputs(", ", backend->out);
        if (!emit_expression(backend, node->as.call.arguments.items[i]))
            return 0;
    }

    fputc(')', backend->out);
    return 1;
}

static int emit_expression(CBackend *backend, Node *node)
{
    if (!node) {
        backend_error(backend, backend->current_function, "missing expression during C lowering");
        return 0;
    }

    switch (node->type) {
        case NODE_NUMBER:
            if (node->as.number.kind == NUMBER_LITERAL_INTEGER)
                return emit_integer_literal(backend, node);

            if (node->as.number.kind == NUMBER_LITERAL_FLOAT)
                return emit_float_literal(backend, node);

            backend_error(backend, node, "unknown numeric literal kind during C lowering");
            return 0;

        case NODE_BOOL:
            fputs(node->as.boolean.value ? "1" : "0", backend->out);
            return 1;

        case NODE_NULL:
            fputs("NULL", backend->out);
            return 1;

        case NODE_IDENT:
            if (emit_parameter_identifier(backend, node))
                return 1;

            {
                CFunction *function = find_function(backend, node->as.ident);
                if (function) {
                    fputs(function->generated_name, backend->out);
                    return 1;
                }
            }

            backend_error(
                backend,
                node,
                "current host-C backend only lowers parameter and function identifiers in value expressions"
            );
            return 0;

        case NODE_UNARY:
            if (node->as.unary.op == TOK_MINUS &&
                node->as.unary.operand &&
                node->as.unary.operand->type == NODE_NUMBER) {
                fputs("(-", backend->out);

                if (node->as.unary.operand->as.number.kind == NUMBER_LITERAL_INTEGER) {
                    if (!emit_integer_literal(backend, node->as.unary.operand))
                        return 0;
                } else if (node->as.unary.operand->as.number.kind == NUMBER_LITERAL_FLOAT) {
                    if (!emit_float_literal(backend, node->as.unary.operand))
                        return 0;
                } else {
                    backend_error(backend, node, "unknown numeric literal kind during C lowering");
                    return 0;
                }

                fputc(')', backend->out);
                return 1;
            }

            backend_error(
                backend,
                node,
                "runtime unary operations are not lowered yet; checked integer semantics must be preserved"
            );
            return 0;

        case NODE_CALL:
            return emit_call(backend, node);

        case NODE_STRING: {
            StringDecodeInfo info =
                string_analyze(node->as.string_literal);

            if (!info.ok) {
                backend_error(
                    backend,
                    node,
                    "invalid string literal reached C lowering after semantic checking"
                );
                return 0;
            }

            size_t decoded_size =
                info.decoded_length > 0
                    ? (size_t)info.decoded_length
                    : 1;

            char *decoded = malloc(decoded_size);
            if (!decoded) {
                backend_error(backend, node, "out of memory while lowering string literal");
                return 0;
            }

            StringDecodeInfo decoded_info =
                string_decode_into(node->as.string_literal, decoded);

            if (!decoded_info.ok) {
                free(decoded);
                backend_error(
                    backend,
                    node,
                    "invalid string literal reached C lowering after semantic checking"
                );
                return 0;
            }

            emit_c_string_literal(
                backend->out,
                (StringView){
                    .data = decoded,
                    .length = (size_t)decoded_info.decoded_length,
                }
            );

            free(decoded);
            return 1;
        }

        case NODE_FIELD:
            if (emit_enum_member(backend, node))
                return 1;

            if (!backend->had_error)
                backend_error(backend, node, "current host-C backend only lowers enum-member field expressions");
            return 0;

        case NODE_CAST:
            backend_error(
                backend,
                node,
                "cast lowering is not implemented by the first host-C backend subset"
            );
            return 0;

        case NODE_BINARY:
            backend_error(
                backend,
                node,
                "runtime binary operations are not lowered yet; checked arithmetic cannot use raw C operators"
            );
            return 0;

        default:
            backend_error(
                backend,
                node,
                "expression is not supported by the current host-C backend subset"
            );
            return 0;
    }
}

static int emit_statement(CBackend *backend, Node *node)
{
    if (!node) return 1;

    switch (node->type) {
        case NODE_RETURN:
            fputs("    return", backend->out);
            if (node->as.return_stmt.value) {
                fputc(' ', backend->out);
                if (!emit_expression(backend, node->as.return_stmt.value))
                    return 0;
            }
            fputs(";\n", backend->out);
            return 1;

        case NODE_EXPR_STMT:
            fputs("    ", backend->out);
            if (!emit_expression(backend, node->as.expr_stmt.expr))
                return 0;
            fputs(";\n", backend->out);
            return 1;

        default:
            backend_error(
                backend,
                node,
                "statement is not supported by the current host-C backend subset"
            );
            return 0;
    }
}

static int emit_block(CBackend *backend, Node *block)
{
    if (!block || block->type != NODE_BLOCK) {
        backend_error(backend, block, "expected function body block during C lowering");
        return 0;
    }

    for (int i = 0; i < block->as.block.statements.count; i++) {
        if (!emit_statement(backend, block->as.block.statements.items[i]))
            return 0;
    }

    return 1;
}

static const char *function_return_type(CBackend *backend, Node *func)
{
    return register_c_type(backend, func->as.func_decl.return_type, func);
}

static const char *parameter_type(CBackend *backend, Node *param)
{
    return register_c_type(backend, param->as.param_decl.var_type, param);
}

static void emit_parameter_type_list(CBackend *backend, Node *func, int with_names)
{
    int count = func->as.func_decl.params.count;

    if (count == 0) {
        fputs("void", backend->out);
        return;
    }

    for (int i = 0; i < count; i++) {
        Node *param = func->as.func_decl.params.items[i];
        const char *type_name = parameter_type(backend, param);

        if (i > 0) fputs(", ", backend->out);
        fputs(type_name ? type_name : "void", backend->out);

        if (with_names)
            fprintf(backend->out, " cg_p_%d", i);
    }
}

static int emit_enum_definitions(CBackend *backend)
{
    for (int i = 0; i < backend->enum_count; i++) {
        CEnum *enumeration = &backend->enums[i];
        Node *decl = enumeration->node;
        const char *backing = base_c_type_name(decl->as.enum_decl.backing_type);

        if (!backing) {
            backend_error(backend, decl, "invalid #repr(c) enum backing type during C lowering");
            return 0;
        }

        /*
         * C99 cannot portably spell a fixed-underlying enum. Lower the ABI
         * type to the exact requested native C integer representation while
         * Coglet retains closed-enum validity in semantic analysis.
         */
        fprintf(backend->out, "typedef %s %s;\n", backing, enumeration->generated_name);
    }

    if (backend->enum_count > 0)
        fputc('\n', backend->out);

    return !backend->had_error;
}

static void emit_struct_forward_declarations(CBackend *backend)
{
    for (int i = 0; i < backend->struct_count; i++) {
        const char *name = backend->structs[i].generated_name;
        fprintf(backend->out, "typedef struct %s %s;\n", name, name);
    }

    if (backend->struct_count > 0)
        fputc('\n', backend->out);
}

static int emit_struct_field_declaration(
    CBackend *backend,
    const Type *type,
    const Node *field,
    int field_index
) {
    if (!type) {
        backend_error(backend, field, "missing struct field type during C lowering");
        return 0;
    }

    if (type->kind == TYPE_ARRAY) {
        if (type->array_size <= 0 || !type->element) {
            backend_error(backend, field, "invalid #repr(c) array field during C lowering");
            return 0;
        }

        const char *element_type = register_c_type(backend, type->element, field);
        if (!element_type) return 0;

        fprintf(
            backend->out,
            "    %s cg_f_%d[%d];\n",
            element_type,
            field_index,
            type->array_size
        );
        return 1;
    }

    const char *type_name = register_c_type(backend, type, field);
    if (!type_name) return 0;

    fprintf(backend->out, "    %s cg_f_%d;\n", type_name, field_index);
    return 1;
}

static int emit_struct_definition(CBackend *backend, int index)
{
    CStruct *structure = &backend->structs[index];

    if (structure->definition_state == 2)
        return 1;

    if (structure->definition_state == 1) {
        backend_error(
            backend,
            structure->node,
            "recursive #repr(c) by-value struct layout reached C lowering"
        );
        return 0;
    }

    structure->definition_state = 1;

    /*
     * C permits pointers to forward-declared structs, but a struct embedded
     * by value must already be complete. Emit inline aggregate dependencies
     * first regardless of Coglet source declaration order.
     */
    for (int f = 0; f < structure->type->field_count; f++) {
        const Type *field_type = structure->type->fields[f].type;

        while (field_type && field_type->kind == TYPE_ARRAY)
            field_type = field_type->element;

        if (!field_type || field_type->kind != TYPE_STRUCT)
            continue;

        const CStruct *dependency = find_c_struct_by_type(backend, field_type);
        if (!dependency) {
            backend_error(
                backend,
                structure->node,
                "missing #repr(c) struct dependency during C lowering"
            );
            return 0;
        }

        int dependency_index = (int)(dependency - backend->structs);
        if (!emit_struct_definition(backend, dependency_index))
            return 0;
    }

    Node *decl = structure->node;
    fprintf(backend->out, "struct %s {\n", structure->generated_name);

    for (int f = 0; f < decl->as.struct_decl.fields.count; f++) {
        Node *field = decl->as.struct_decl.fields.items[f];
        if (!emit_struct_field_declaration(
                backend,
                field->as.struct_field_decl.var_type,
                field,
                f)) {
            return 0;
        }
    }

    fputs("};\n\n", backend->out);
    structure->definition_state = 2;
    return 1;
}

static int emit_struct_definitions(CBackend *backend)
{
    for (int i = 0; i < backend->struct_count; i++) {
        if (!emit_struct_definition(backend, i))
            return 0;
    }

    return !backend->had_error;
}

static StringView external_symbol_name(Node *func)
{
    if (!string_view_is_empty(func->as.func_decl.external_name))
        return func->as.func_decl.external_name;

    return func->as.func_decl.name;
}

static int emit_function_declarations(CBackend *backend)
{
    for (int i = 0; i < backend->function_count; i++) {
        CFunction *entry = &backend->functions[i];
        Node *func = entry->node;
        const char *return_type = function_return_type(backend, func);
        if (!return_type) return 0;

        if (func->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C) {
            fprintf(backend->out, "extern %s %s(", return_type, entry->generated_name);
            emit_parameter_type_list(backend, func, 0);
            fputs(") __asm__(", backend->out);
            emit_c_string_literal(backend->out, external_symbol_name(func));
            fputs(");\n", backend->out);
        } else {
            fprintf(backend->out, "static %s %s(", return_type, entry->generated_name);
            emit_parameter_type_list(backend, func, 0);
            fputs(");\n", backend->out);
        }
    }

    fputc('\n', backend->out);
    return !backend->had_error;
}

static int emit_function_bodies(CBackend *backend)
{
    for (int i = 0; i < backend->function_count; i++) {
        CFunction *entry = &backend->functions[i];
        Node *func = entry->node;

        if (func->as.func_decl.linkage == FUNCTION_LINKAGE_EXTERN_C)
            continue;

        const char *return_type = function_return_type(backend, func);
        if (!return_type) return 0;

        backend->current_function = func;

        fprintf(backend->out, "static %s %s(", return_type, entry->generated_name);
        emit_parameter_type_list(backend, func, 1);
        fputs(")\n{\n", backend->out);

        if (!emit_block(backend, func->as.func_decl.body))
            return 0;

        fputs("}\n\n", backend->out);
        backend->current_function = NULL;
    }

    return !backend->had_error;
}

static int source_type_is_c_int(const Type *type)
{
    return type && type->kind == TYPE_NAMED && sv_equals(type->named_name, "c_int");
}

static int emit_entrypoint(CBackend *backend)
{
    StringView main_name = string_view_from_cstr("main");
    CFunction *main_function = find_function(backend, main_name);

    if (!main_function ||
        main_function->node->as.func_decl.linkage != FUNCTION_LINKAGE_COGLET) {
        backend_error(
            backend,
            backend->program,
            "host executable backend requires a top-level Coglet 'main' function"
        );
        return 0;
    }

    Node *func = main_function->node;

    if (func->as.func_decl.params.count != 0 ||
        !source_type_is_c_int(func->as.func_decl.return_type)) {
        backend_error(
            backend,
            func,
            "host executable entry point must have signature 'main::() -> c_int'"
        );
        return 0;
    }

    fputs("int main(void)\n{\n    return ", backend->out);
    fputs(main_function->generated_name, backend->out);
    fputs("();\n}\n", backend->out);

    return 1;
}

static CBackendStatus c_backend_emit_stream(
    FILE *out,
    const char *source_filename,
    Node *program,
    SemanticContext *sem
) {
    CBackend backend;
    memset(&backend, 0, sizeof(backend));

    backend.out = out;
    backend.source_filename = source_filename;
    backend.program = program;
    backend.sem = sem;

    if (!collect_enums(&backend) ||
        !collect_structs(&backend) ||
        !collect_functions(&backend) ||
        !prepare_struct_types(&backend) ||
        !prepare_function_types(&backend))
        return C_BACKEND_STATUS_UNSUPPORTED;

    fputs("/* Generated by Coglet's initial host-C backend. */\n", out);
    fputs("#include <stddef.h>\n#include <stdint.h>\n\n", out);

    emit_struct_forward_declarations(&backend);

    if (!emit_enum_definitions(&backend))
        return C_BACKEND_STATUS_UNSUPPORTED;

    for (int i = 0; i < backend.type_alias_count; i++) {
        fputs(backend.type_aliases[i].definition, out);
        fputc('\n', out);
    }

    if (backend.type_alias_count > 0)
        fputc('\n', out);

    if (!emit_struct_definitions(&backend) ||
        !emit_function_declarations(&backend) ||
        !emit_function_bodies(&backend) ||
        !emit_entrypoint(&backend)) {
        return C_BACKEND_STATUS_UNSUPPORTED;
    }

    if (ferror(out)) {
        fprintf(stderr, "error: failed while writing generated C output\n");
        return C_BACKEND_STATUS_IO_ERROR;
    }

    return C_BACKEND_STATUS_OK;
}

CBackendStatus c_backend_emit_file(
    const char *output_path,
    const char *source_filename,
    Node *program,
    SemanticContext *sem
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

    CBackendStatus status = c_backend_emit_stream(
        out,
        source_filename,
        program,
        sem
    );

    if (fclose(out) != 0 && status == C_BACKEND_STATUS_OK) {
        fprintf(stderr, "error: could not close generated C output '%s': %s\n",
                output_path, strerror(errno));
        return C_BACKEND_STATUS_IO_ERROR;
    }

    return status;
}

CBackendStatus c_backend_build_executable(
    const char *output_path,
    const char *source_filename,
    Node *program,
    SemanticContext *sem,
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

    CBackendStatus emit_status = c_backend_emit_stream(
        out,
        source_filename,
        program,
        sem
    );

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

    /*
     * Build the complete compiler argv before fork(). Each -L/-l entry is
     * passed as a separate argv pair. No shell is involved, so paths and
     * library names are not subject to shell expansion or command
     * interpretation.
     *
     * Keep libraries after the generated source/output options: static
     * archive resolution is order-sensitive on common Unix linkers.
     */
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

    for (int i = 0; i < library_dir_count; i++) {
        cc_argv[arg++] = "-L";
        cc_argv[arg++] = (char *)link_options->library_dirs[i];
    }

    for (int i = 0; i < library_count; i++) {
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
    (void)source_filename;
    (void)program;
    (void)sem;
    (void)link_options;
    fprintf(stderr, "error: executable host-C backend is not implemented on this host platform\n");
    return C_BACKEND_STATUS_TOOLCHAIN_ERROR;
#endif
}
