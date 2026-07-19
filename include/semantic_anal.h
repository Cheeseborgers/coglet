#ifndef COGLET_SEMANTIC_H
#define COGLET_SEMANTIC_H

#include "ast.h"
#include "semantic_info.h"

typedef enum {
    SYMBOL_VARIABLE,  // variables
    SYMBOL_FUNCTION,  // functions
    SYMBOL_TYPE,       // struct names, typedefs later // TODO: make separations for typedefs
    SYMBOL_CONSTANT,
} SymbolKind;

typedef enum {
    CONST_VALUE_INT,
    CONST_VALUE_FLOAT,
    CONST_VALUE_BOOL,
    CONST_VALUE_NULL,
} ConstValueKind;

typedef struct ConstValue {
    ConstValueKind kind;

    /*
     * Semantic type of this compile-time value.
     *
     * Examples:
     *
     *   123              -> untyped i32
     *   3.14             -> untyped f64
     *   true             -> bool
     *   Color.Red        -> Color
     *   DEFAULT_COLOR    -> Color
     */
    Type *type;

    union {
        IntegerValue integer;
        double floating;
        int boolean;
    } as;
} ConstValue;

typedef struct Symbol {
    StringView name;

    SymbolKind kind;

    ConstValue const_value;   // only meaningful when kind == SYMBOL_CONSTANT

    Type *type;

    struct Symbol *next;
} Symbol;

typedef struct Scope {
    Symbol *symbols;
    struct Scope *parent;
} Scope;

typedef struct {
    Arena *arena;

    int had_error;
    int error_count;

    Scope *current_scope;
    int loop_depth;
    int function_depth;

    Type *type_i8;
    Type *type_i16;
    Type *type_i32;
    Type *type_i64;

    Type *type_u8;
    Type *type_u16;
    Type *type_u32;
    Type *type_u64;

    Type *type_f32;
    Type *type_f64;

    Type *type_bool;
    Type *type_void;
    Type *type_null;

    Type *current_return_type;

    SemExprInfo *expr_infos;
} SemanticContext;

void semantic_check(Node *program, SemanticContext *ctx);
SemExprInfo *semantic_get_expr_info(SemanticContext *ctx, Node *node);

#endif
