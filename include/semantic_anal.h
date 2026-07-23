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
    VARIABLE_STORAGE_NONE,
    VARIABLE_STORAGE_GLOBAL,
    VARIABLE_STORAGE_LOCAL,
    VARIABLE_STORAGE_PARAMETER,
} VariableStorage;

#define INVALID_VARIABLE_ID ((size_t)-1)
#define INVALID_FLOW_OWNER_ID ((size_t)-1)

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

    VariableStorage variable_storage;

    /*
     * Function flow state that owns variable_id.
     *
     * Only locals and parameters have flow ownership.
     * Globals and non-variable symbols use INVALID_FLOW_OWNER_ID.
     */
    size_t flow_owner_id;
    size_t variable_id;

    struct Symbol *next;
} Symbol;

typedef struct Scope {
    Symbol *symbols;

    /*
     * Number of flow-state slots that were active when this scope
     * was entered. Scope exit restores the flow state to this mark.
     */
    size_t flow_count_mark;

    struct Scope *parent;
} Scope;

typedef struct {
    /*
     * Identifies the function whose local-variable slots are stored
     * in this flow state.
     */
    size_t owner_id;

    unsigned char *initialized;
    size_t count;
    size_t capacity;
    int reachable;
} FlowState;

typedef struct LoopFlowContext LoopFlowContext;

typedef struct {
    Arena *arena;

    int had_error;
    int error_count;

    Scope *current_scope;

    int loop_depth;
    LoopFlowContext *current_loop;

    int function_depth;

    /*
    * Flow-owner IDs are unique for the complete semantic check.
    * Variable IDs restart from zero for each function.
    */
    size_t next_flow_owner_id;
    size_t next_variable_id;

    FlowState flow;

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
