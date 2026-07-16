// semantic_info.h
#ifndef COGLET_SEMANTIC_INFO_H
#define COGLET_SEMANTIC_INFO_H

#include "ast.h"

/*
 * Semantic-info states:
 *
 * No entry:
 *     the node was not successfully checked.
 *
 * type != NULL and category is LVALUE/RVALUE:
 *     the node produces a value.
 *
 * type == NULL and category == NONE:
 *     the node was successfully checked but deliberately
 *     produces no value, such as a mutation statement.
 */

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