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
 *     the node was successfully checked but deliberately produces
 *     no value, such as:
 *
 *     - assignment and other mutation statements;
 *     - a compiler builtin identifier used as a resolved call target.
 */

typedef struct Type Type;
typedef struct Symbol Symbol;

/*
 * Stable identity for a successfully resolved source declaration.
 *
 * IDs are unique within one semantic_check() invocation. They are deliberately
 * independent of AST/Symbol pointer values so later lowering stages can build
 * their own tables without using addresses as declaration identity.
 */
typedef size_t SemDeclId;
#define INVALID_SEM_DECL_ID ((SemDeclId)-1)

typedef enum ValueCategory {
    VALUE_CATEGORY_NONE,
    VALUE_CATEGORY_RVALUE,
    VALUE_CATEGORY_LVALUE,
} ValueCategory;

typedef enum ValueAccess {
    VALUE_ACCESS_NONE,
    VALUE_ACCESS_READONLY,
    VALUE_ACCESS_WRITABLE,
} ValueAccess;

typedef struct SemDeclInfo {
    SemDeclId id;
    Node *node;

    /*
     * Lexical symbol when this declaration introduces one. Aggregate members
     * and parameters on declarations without a body may legitimately have no
     * Symbol while still having declaration identity and a resolved type.
     */
    Symbol *symbol;
    Type *type;

    struct SemDeclInfo *next;
} SemDeclInfo;

typedef struct SemExprInfo {
    Node *node;

    Type *type;
    Symbol *symbol;

    ValueCategory value_category;
    ValueAccess value_access;

    /* True when an lvalue denotes volatile-qualified storage. */
    int value_is_volatile;

    struct SemExprInfo *next;
} SemExprInfo;

#endif
