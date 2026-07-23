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

    /*
    * Numeric constants that have not yet been fixed to a concrete
    * storage type. Their exact value lives in ConstValue.
    */
    TYPE_UNTYPED_INT,
    TYPE_UNTYPED_FLOAT,

    /*
    * Contextual null-pointer literal.
    *
    * This is an internal expression type, not a type users can write
    * and not a concrete type suitable for inferred storage.
    */
    TYPE_NULL,

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

typedef enum PointerAccess {
    POINTER_ACCESS_MUTABLE,
    POINTER_ACCESS_READONLY,
} PointerAccess;

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
    * Pointer and array types.
    *
    * pointer_access is meaningful only when kind == TYPE_POINTER.
    */
    Type *element;
    PointerAccess pointer_access;
    int array_size; /* -1 if unspecified */

    StringView named_name;   // TYPE_NAMED

    StringView struct_name;  // TYPE_STRUCT
    StructField *fields;
    int field_count;

    Type **parameters;       // TYPE_FUNCTION
    int parameter_count;
    Type *return_type;

    StringView enum_name;    // TYPE_ENUM
    Type *enum_backing_type;
    EnumMember *enum_members;
    int enum_member_count;
};

#endif
