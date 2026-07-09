#ifndef COGLET_TYPES_H
#define COGLET_TYPES_H

typedef enum {
    TYPE_VOID,
    TYPE_BOOL,

    TYPE_I8, TYPE_I16, TYPE_I32, TYPE_I64,
    TYPE_U8, TYPE_U16, TYPE_U32, TYPE_U64,

    TYPE_F32, TYPE_F64,

    TYPE_POINTER,
    TYPE_ARRAY,

    TYPE_STRUCT,

    TYPE_FUNCTION,

} TypeKind;

typedef struct Type Type;

typedef struct StructField {
    const char *name;
    int length;
    Type *type;
} StructField;

struct Type {
    TypeKind kind;

    Type *element;        // TYPE_POINTER / TYPE_ARRAY

    int array_size;              // -1 if unspecified

    const char *struct_name;     // TYPE_STRUCT
    int struct_name_length;

    StructField *fields;
    int field_count;

    // function types
    Type **parameters;
    int parameter_count;
    Type *return_type;
};

#endif