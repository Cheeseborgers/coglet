#ifndef COGLET_TYPES_H
#define COGLET_TYPES_H

#include <stdint.h>

#include "utils/string_view.h"

typedef struct Type Type;

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,

    TYPE_S8,
    TYPE_S16,
    TYPE_S32,
    TYPE_S64,

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
    TYPE_OPAQUE_POINTER,
    TYPE_ARRAY,
    TYPE_SLICE,

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

typedef enum FunctionAbi {
    /* Ordinary Coglet call ABI. */
    FUNCTION_ABI_COGLET,

    /* Native C function-pointer/callback ABI. */
    FUNCTION_ABI_C,
} FunctionAbi;

typedef enum CCallingConvention {
    /* Platform/default C ABI selected by #extern(c), #repr(c), or cfn. */
    C_CALL_DEFAULT,

    /* Explicit native calling-convention contracts. */
    C_CALL_CDECL,
    C_CALL_STDCALL,
    C_CALL_SYSV64,
    C_CALL_WIN64,
} CCallingConvention;

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
    * Pointer, array, and slice types.
    *
    * pointer_access is meaningful when kind == TYPE_POINTER,
    * TYPE_OPAQUE_POINTER, or TYPE_SLICE. Slice access controls mutation
    * through indexing/data, not assignment of the slice value itself.
    */
    Type *element;
    PointerAccess pointer_access;
    int pointer_is_volatile;
    int array_size; /* -1 if unspecified */

    StringView named_module; // TYPE_NAMED optional module qualifier
    StringView named_name;   // TYPE_NAMED

    StringView struct_name;  // TYPE_STRUCT
    StructField *fields;
    int field_count;
    int struct_is_repr_c;
    int struct_repr_c_packed;
    int struct_repr_c_align;
    int struct_is_union;
    int struct_is_incomplete;

    Type **parameters;       // TYPE_FUNCTION
    int parameter_count;
    Type *return_type;
    FunctionAbi function_abi;
    CCallingConvention function_call_conv;
    int function_is_variadic;

    StringView enum_name;    // TYPE_ENUM
    int enum_is_repr_c;
    Type *enum_backing_type;
    EnumMember *enum_members;
    int enum_member_count;
};

#endif
