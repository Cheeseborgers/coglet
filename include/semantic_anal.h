#ifndef COGLET_SEMANTIC_H
#define COGLET_SEMANTIC_H

#include "ast.h"

// TODO: Add: an array with unspecified size is accepted with no complaint. Might be intentional (e.g. as a future "flexible array" feature),
// but currently there's no rule either enforcing or rejecting it — worth deciding on purpose rather than by omission.

// TODO: Add:  nothing checks that the return expression's type actually matches the declared -> i32. A return "wrong";
// in that body right now would type-check "wrong"

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
} ConstValueKind;

typedef struct {
    ConstValueKind kind;
    union {
        long long i;
        double f;
        int b;
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

    Type *type_bool;

    int had_error;
    int error_count;

    Scope *current_scope;
    int loop_depth;
    int function_depth;
} SemanticContext;

void semantic_check(Node *program, SemanticContext *ctx);

#endif