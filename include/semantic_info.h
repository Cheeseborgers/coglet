// semantic_info.h
#ifndef COGLET_SEMANTIC_INFO_H
#define COGLET_SEMANTIC_INFO_H

#include "ast.h"

typedef struct Type Type;
typedef struct Symbol Symbol;

typedef enum ValueCategory {
    VALUE_CATEGORY_NONE,
    VALUE_CATEGORY_RVALUE,
    VALUE_CATEGORY_LVALUE,
} ValueCategory;

typedef struct SemExprInfo {
    Node *node;

    Type *type;
    Symbol *symbol;

    ValueCategory value_category;

    struct SemExprInfo *next;
} SemExprInfo;

#endif