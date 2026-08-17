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
 * Normalized compile-time value produced by semantic constant evaluation.
 *
 * The payload is independent of source spelling. `type` is the semantic type
 * of the value before any use-site contextual conversion; callers should use
 * semantic_get_constant_value() to obtain the effective value selected for the
 * checked use-site.
 */
typedef enum ConstValueKind {
    CONST_VALUE_INT,
    CONST_VALUE_FLOAT,
    CONST_VALUE_BOOL,
    CONST_VALUE_NULL,
} ConstValueKind;

typedef struct ConstValue {
    ConstValueKind kind;
    Type *type;

    union {
        IntegerValue integer;
        double floating;
        int boolean;
    } as;
} ConstValue;

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

/*
 * Implicit/contextual conversion selected by semantic analysis for one
 * expression use-site.
 *
 * `SemExprInfo.type` remains the expression's intrinsic semantic type. When a
 * parent context requires a different concrete representation,
 * `contextual_type` records that destination and `contextual_conversion`
 * explains why the adaptation is legal.
 *
 * This is deliberately narrower than explicit casts. A NODE_CAST carries its
 * conversion in the AST and therefore normally has SEM_CONTEXT_CONVERSION_NONE.
 */
typedef enum SemContextConversionKind {
    SEM_CONTEXT_CONVERSION_NONE,

    /* adaptable untyped-int -> concrete integer */
    SEM_CONTEXT_CONVERSION_INT_MATERIALIZE,

    /* adaptable untyped-int -> concrete floating-point */
    SEM_CONTEXT_CONVERSION_INT_TO_FLOAT_MATERIALIZE,

    /* adaptable untyped-float -> concrete floating-point */
    SEM_CONTEXT_CONVERSION_FLOAT_MATERIALIZE,

    /* dedicated null literal/value -> concrete nullable raw pointer/cfn */
    SEM_CONTEXT_CONVERSION_NULL_TO_POINTER,

    /* matching raw pointer with only monotonic immediate qualifier addition */
    SEM_CONTEXT_CONVERSION_POINTER_QUALIFICATION,

    /* direct string literal admitted at the narrow readonly c_char* C boundary */
    SEM_CONTEXT_CONVERSION_C_STRING_TO_POINTER,
} SemContextConversionKind;

typedef enum SemAbiRepresentation {
    SEM_ABI_REPR_COGLET,
    SEM_ABI_REPR_C,
} SemAbiRepresentation;

typedef enum SemFunctionLinkage {
    /* Definition emitted by the current Coglet compilation unit. */
    SEM_FUNCTION_LINKAGE_INTERNAL,

    /* Declaration whose definition is supplied by the native linker/ABI. */
    SEM_FUNCTION_LINKAGE_EXTERNAL,
} SemFunctionLinkage;

typedef enum SemAggregateKind {
    SEM_AGGREGATE_STRUCT,
    SEM_AGGREGATE_UNION,
} SemAggregateKind;

/*
 * Exact source-level native-C scalar spelling retained at an ABI boundary.
 *
 * Semantic type resolution intentionally erases these aliases to Coglet's
 * concrete runtime scalar types. Keeping the spelling here lets later lowering
 * distinguish, for example, source `c_int` from source `i32` without consulting
 * the AST again.
 */
typedef enum SemCScalarKind {
    SEM_C_SCALAR_NONE,

    SEM_C_SCALAR_CHAR,
    SEM_C_SCALAR_SCHAR,
    SEM_C_SCALAR_UCHAR,
    SEM_C_SCALAR_SHORT,
    SEM_C_SCALAR_USHORT,
    SEM_C_SCALAR_INT,
    SEM_C_SCALAR_UINT,
    SEM_C_SCALAR_LONG,
    SEM_C_SCALAR_ULONG,
    SEM_C_SCALAR_LONGLONG,
    SEM_C_SCALAR_ULONGLONG,
    SEM_C_SCALAR_SIZE,
    SEM_C_SCALAR_BOOL,
    SEM_C_SCALAR_FLOAT,
    SEM_C_SCALAR_DOUBLE,
} SemCScalarKind;

typedef enum SemAbiTypeKind {
    /* No source-level native-C scalar override; use semantic_type. */
    SEM_ABI_TYPE_SEMANTIC,

    /* Exact `c_*` scalar spelling selected by c_scalar_kind. */
    SEM_ABI_TYPE_C_SCALAR,

    SEM_ABI_TYPE_POINTER,
    SEM_ABI_TYPE_OPAQUE_POINTER,
    SEM_ABI_TYPE_ARRAY,
    SEM_ABI_TYPE_FUNCTION,
} SemAbiTypeKind;

typedef struct SemAbiType SemAbiType;

/*
 * Normalized type spelling for a C ABI surface.
 *
 * semantic_type is always the already-resolved runtime semantic type. The
 * recursive shape exists only where source spelling matters to the native ABI;
 * it is not a second semantic type system.
 */
struct SemAbiType {
    SemAbiTypeKind kind;
    Type *semantic_type;

    SemCScalarKind c_scalar_kind;

    SemAbiType *element;

    SemAbiType **parameters;
    int parameter_count;
    SemAbiType *return_type;
};

typedef enum SemDeclAbiKind {
    SEM_DECL_ABI_NONE,
    SEM_DECL_ABI_FUNCTION,
    SEM_DECL_ABI_AGGREGATE,
    SEM_DECL_ABI_ENUM,
} SemDeclAbiKind;

typedef struct SemFunctionAbiInfo {
    FunctionAbi abi;
    SemFunctionLinkage linkage;
    CCallingConvention c_call_conv;
    int is_variadic;


    /*
     * Effective native linker symbol for an external declaration. This is
     * normalized to the Coglet function name when #extern(c) omits name=.
     * Internal definitions leave it empty.
     */
    StringView external_symbol;

    /* Exact C-facing return spelling when abi == FUNCTION_ABI_C. */
    SemAbiType *return_abi_type;
} SemFunctionAbiInfo;

typedef struct SemAggregateAbiInfo {
    SemAbiRepresentation representation;
    SemAggregateKind aggregate_kind;
    int is_incomplete;
    int is_packed;
    unsigned explicit_alignment;
} SemAggregateAbiInfo;

typedef struct SemEnumAbiInfo {
    SemAbiRepresentation representation;

    /* Exact C-facing backing spelling for #repr(c) enums. */
    SemAbiType *backing_abi_type;
} SemEnumAbiInfo;

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

    /*
     * Exact C-facing type spelling for declarations whose native-C spelling
     * must survive semantic type canonicalization. This includes direct C ABI
     * surfaces and first-class cfn values stored in Coglet parameters/locals.
     * NULL when runtime semantic type identity is sufficient.
     */
    SemAbiType *abi_type;

    SemDeclAbiKind abi_kind;
    union {
        SemFunctionAbiInfo function;
        SemAggregateAbiInfo aggregate;
        SemEnumAbiInfo enumeration;
    } abi;

    /*
     * Compile-time value for constant-like declarations. This is populated for
     * constant declarations and enum members, including implicit enum values.
     */
    int has_constant_value;
    ConstValue constant_value;

    /*
     * Frontend-only declaration-check state. Top-level constants are
     * predeclared so imported module constants are independent of physical
     * input order; these flags guard lazy dependency evaluation and cycles.
     * Backends do not consume this state.
     */
    int semantic_check_started;
    int semantic_check_complete;

    /* True only for the validated root-namespace `main::() -> i32`. */
    int is_executable_entry;

    struct SemDeclInfo *next;
} SemDeclInfo;

typedef struct SemExprInfo {
    Node *node;

    Type *type;
    Symbol *symbol;

    /*
     * Concrete type selected by an enclosing semantic context when it differs
     * from the expression's intrinsic type. NULL when no implicit/contextual
     * adaptation occurs.
     *
     * Future lowering should use semantic_get_effective_expr_type() rather
     * than re-deriving these decisions from AST syntax.
     */
    Type *contextual_type;
    SemContextConversionKind contextual_conversion;

    ValueCategory value_category;
    ValueAccess value_access;

    /* True when an lvalue denotes volatile-qualified storage. */
    int value_is_volatile;

    /*
     * Intrinsic compile-time value cached while semantic checking still has
     * lexical scope available. Retrieval applies any recorded contextual
     * conversion without re-running name lookup or constant evaluation.
     */
    int has_constant_value;
    ConstValue constant_value;

    struct SemExprInfo *next;
} SemExprInfo;

#endif
