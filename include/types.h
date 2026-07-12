#ifndef COGLET_TYPES_H
#define COGLET_TYPES_H

#include "utils/string_view.h"

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,

    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,

    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,

    TYPE_F32,
    TYPE_F64,

    TYPE_POINTER,
    TYPE_ARRAY,

    TYPE_STRUCT,
    TYPE_ENUM,

    TYPE_FUNCTION,
} TypeKind;

typedef struct Type Type;

typedef struct StructField {
    StringView name;
    Type *type;
} StructField;

typedef struct EnumMember {
    StringView name;
    long long value;
} EnumMember;

struct Type {
    TypeKind kind;

    /*
     * True for numeric literals and untyped constant expressions that
     * may adapt to a concrete numeric type.
     */
    int is_untyped;

    /*
     * Pointer and array types.
     */
    Type *element;
    int array_size; /* -1 if unspecified */

    /*
     * Struct types.
     */
    StringView struct_name;
    StructField *fields;
    int field_count;

    /*
     * Function types.
     */
    Type **parameters;
    int parameter_count;
    Type *return_type;

    /*
     * Enum types.
     */
    StringView enum_name;
    Type *enum_backing_type;
    EnumMember *enum_members;
    int enum_member_count;
};

#endif