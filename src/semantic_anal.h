#ifndef COG_SEMANTIC_H
#define COG_SEMANTIC_H

#include "ast.h"

typedef enum {
    SYMBOL_VARIABLE,  // variables
    SYMBOL_FUNCTION,  // functions
    SYMBOL_TYPE       // struct names, typedefs later
} SymbolKind;

typedef struct Symbol {
    const char *name;
    int length;

    SymbolKind kind;

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
} SemanticContext;

void semantic_check(Node *program, SemanticContext *ctx);

#endif