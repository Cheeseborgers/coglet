#ifndef COGLET_SEMANTIC_H
#define COGLET_SEMANTIC_H

#include "ast.h"
#include "diagnostic.h"
#include "semantic_info.h"
#include "target_info.h"

typedef enum BuiltinKind {
    /*
     * Used by every ordinary user-defined symbol.
     *
     * A symbol with kind SYMBOL_BUILTIN must never carry this value.
     */
    BUILTIN_NONE,

    BUILTIN_WRAPPING_ADD,
    BUILTIN_WRAPPING_SUB,
    BUILTIN_WRAPPING_MUL,
    BUILTIN_WRAPPING_NEG,
} BuiltinKind;

typedef enum {
    SYMBOL_VARIABLE,  // variables
    SYMBOL_FUNCTION,  // functions
    SYMBOL_TYPE,       // struct names, typedefs later // TODO: make separations for typedefs
    SYMBOL_CONSTANT,


    /*
     * A compiler-provided operation resolved through the ordinary
     * lexical scope mechanism.
     *
     * Builtins are not ordinary functions because their signatures
     * may depend on their argument types.
     */
    SYMBOL_BUILTIN,
} SymbolKind;

typedef enum {
    VARIABLE_STORAGE_NONE,
    VARIABLE_STORAGE_GLOBAL,
    VARIABLE_STORAGE_LOCAL,
    VARIABLE_STORAGE_PARAMETER,
} VariableStorage;

#define INVALID_VARIABLE_ID ((size_t)-1)
#define INVALID_FLOW_OWNER_ID ((size_t)-1)

typedef struct Symbol {
    StringView name;

    SymbolKind kind;

    /*
    * Stable compiler identity for a language-provided builtin.
    *
    * This is BUILTIN_NONE for every symbol whose kind is not
    * SYMBOL_BUILTIN.
    */
    BuiltinKind builtin_kind;

    Type *type;

    /*
     * Source declaration and stable semantic identity for ordinary declared
     * symbols. Compiler-provided aliases/builtins have declaration == NULL and
     * declaration_id == INVALID_SEM_DECL_ID.
     */
    Node *declaration;
    SemDeclId declaration_id;

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

typedef struct SemanticModule SemanticModule;
typedef struct GenericSpecialization GenericSpecialization;
typedef struct GenericStructSpecialization GenericStructSpecialization;
typedef struct SliceTypeIntern SliceTypeIntern;
typedef struct StructMethodBinding StructMethodBinding;
typedef struct StructOperatorBinding StructOperatorBinding;

typedef struct SemanticImportBinding {
    SemanticModule *module;
    /* Empty for canonical qualification; otherwise file-local alias. */
    StringView alias;
} SemanticImportBinding;

typedef struct SemanticSourceModule {
    SourceFileId source_id;
    SemanticModule *module;
    SemanticImportBinding *imports;
    size_t import_count;
    size_t import_capacity;
    Node *module_decl;
    int saw_import_directive;
    int saw_non_directive;
} SemanticSourceModule;

struct SemanticModule {
    StringView name; /* empty for the root namespace */
    Scope *scope;
    int is_root;
    SemanticModule *next;
};

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

    TargetInfo target;

    SourceManager *sources;
    DiagnosticList diagnostics;

    int had_error;
    int error_count;

    Scope *current_scope;

    /* Frontend-only module/import state. Modules are erased before CogIR. */
    Scope *builtin_scope;
    SemanticModule *modules;
    SemanticModule *root_module;
    SemanticModule *current_module;
    SemanticSourceModule *source_modules;
    size_t source_module_count;
    SourceFileId current_source_id;

    int loop_depth;
    LoopFlowContext *current_loop;

    int function_depth;

    /*
    * Flow-owner IDs are unique for the complete semantic check.
    * Variable IDs restart from zero for each function.
    */
    size_t next_flow_owner_id;
    size_t next_variable_id;
    SemDeclId next_declaration_id;

    FlowState flow;

    Type *type_s8;
    Type *type_s16;
    Type *type_s32;
    Type *type_s64;

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

    SemDeclInfo *decl_infos;
    SemExprInfo *expr_infos;

    /* Frontend-only monomorphization cache/state. */
    GenericSpecialization *generic_specializations;
    GenericSpecialization *active_generic_specialization;
    GenericStructSpecialization *generic_struct_specializations;
    GenericStructSpecialization *active_generic_struct_specialization;

    /* Frontend-only methods attached to concrete nominal struct types. */
    StructMethodBinding *struct_methods;

    /* Frontend-only explicit operator mappings attached to struct methods. */
    StructOperatorBinding *struct_operators;

    /* Canonical structural slice types, frontend-only. */
    SliceTypeIntern *slice_types;
} SemanticContext;

void semantic_check(
    Node *program,
    SemanticContext *ctx,
    const TargetInfo *target,
    SourceManager *sources
);
SemDeclInfo *semantic_get_decl_info(SemanticContext *ctx, Node *node);
SemDeclInfo *semantic_get_decl_info_by_id(SemanticContext *ctx, SemDeclId id);
SemExprInfo *semantic_get_expr_info(SemanticContext *ctx, Node *node);

/*
 * Returns the type an expression has at its actual use site after semantic
 * contextualization. This is contextual_type when an implicit adaptation was
 * selected, otherwise the expression's intrinsic SemExprInfo.type.
 */
Type *semantic_get_effective_expr_type(SemanticContext *ctx, Node *node);

/* Formats a checked semantic type for diagnostics/frozen debug names. */
void semantic_format_type_name(Type *type, char *buffer, size_t buffer_size);

/*
 * Returns a previously checked compile-time value without re-running semantic
 * evaluation or lexical name lookup. `node` may be an expression, a constant
 * declaration, or an enum-member declaration. The returned value is normalized
 * to the expression's recorded contextual type when one exists.
 */
int semantic_get_constant_value(
    SemanticContext *ctx,
    Node *node,
    ConstValue *out
);

#endif
