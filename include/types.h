#ifndef COGLET_TYPES_H
#define COGLET_TYPES_H

#include <stdint.h>

#include "utils/string_view.h"

typedef struct Type Type;

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

    TYPE_NAMED, // Used as a placeholder kind until later resolution to struct, enum etc

    TYPE_STRUCT,
    TYPE_ENUM,

    TYPE_FUNCTION,
} TypeKind;

typedef enum {
    NUMBER_LITERAL_INTEGER,
    NUMBER_LITERAL_FLOAT,
} NumberLiteralKind;

typedef struct IntegerValue {
    uint64_t magnitude;
    int is_negative;
} IntegerValue;

typedef struct StructField {
    StringView name;
    Type *type;
} StructField;

typedef struct EnumMember {
    StringView name;
    IntegerValue value;
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
    * Named types.
    */
    StringView named_name;   // TYPE_NAMED

    /*
     * Struct types.
     */
    StringView struct_name;  // TYPE_STRUCT
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
