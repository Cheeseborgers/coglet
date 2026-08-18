#ifndef COGLET_AST_H
#define COGLET_AST_H

#include "lexer.h"
#include "types.h"
#include "utils/arena.h"
#include "utils/string_view.h"

typedef enum FunctionLinkage {
    /* Ordinary Coglet function with a Coglet body. */
    FUNCTION_LINKAGE_COGLET,

    /* Declaration of a function provided by the C ABI/linker. */
    FUNCTION_LINKAGE_EXTERN_C,
} FunctionLinkage;

typedef enum CastKind {
    /*
     * Value-preserving checked conversion:
     *
     *     cast(TargetType, expression)
     */
    CAST_CHECKED,

    /*
     * Fixed-width low-bit integer conversion:
     *
     *     truncate(TargetType, expression)
     */
    CAST_TRUNCATING,

    /*
     * Unchecked raw-pointer representation conversion:
     *
     *     reinterpret(TargetPointerType, expression)
     *
     * This may cross between typed and opaque raw pointers but may
     * never recover mutable access from a readonly pointer.
     */
    CAST_REINTERPRET,
} CastKind;

typedef enum {
    NODE_NUMBER,       // a literal like 3 or 3.14
    NODE_IDENT,
    NODE_STRING,
    NODE_CHAR,
    NODE_BOOL,
    NODE_NULL,
    NODE_CAST,

    NODE_UNARY,        // <op> operand, e.g. -x
    NODE_BINARY,       // left <op> right
    NODE_INC_DEC,      // ++x, --x, x++, x--

    NODE_BLOCK,
    NODE_ASSIGN,          // =
    NODE_COMPOUND_ASSIGN, // +=, -=. *=, /= %=

    NODE_EXPR_STMT,    // an expression used as a statement: `1 + 2;`

    NODE_CALL,         // function calls
    NODE_FIELD,
    NODE_INDEX,
    NODE_TYPE_REF,     // type used as an associated-member qualifier

    NODE_PROGRAM,      // the whole compilation unit: a list of statements
    NODE_MODULE_DECL,  // top-level `module name;` file metadata
    NODE_IMPORT_DECL,  // top-level `import name;` file metadata

    NODE_VAR_DECL,
    NODE_VAR_DECL_GROUP,

    NODE_FUNC_DECL,
    NODE_FUNC_PARAM_DECL,

    NODE_STRUCT_DECL,
    NODE_STRUCT_FIELD_DECL,

    NODE_ENUM_DECL,
    NODE_ENUM_MEMBER,

    NODE_STRUCT_INIT,      // Point{ x = 5, y = 10 }
    NODE_FIELD_INIT,       // one `x = 5` inside a struct init

    NODE_CONST_DECL,       // PI :: 3.14159;  or  PI: f64 : 3.14159;

    NODE_ARRAY_LITERAL,

    NODE_IF,
    NODE_SWITCH,
    NODE_SWITCH_CASE,
    NODE_RETURN,
    NODE_DEFER,
    NODE_WHILE,
    NODE_FOR,
    NODE_BREAK,
    NODE_CONTINUE,

    NODE_ERROR
} NodeType;

typedef struct Node Node;

typedef struct {
    Node **items;
    int count;
    int capacity;
} NodeList;

typedef struct GenericTypeParameter {
    StringView name;

    /*
     * Optional closed builtin constraint spelling (for example `ordered`).
     * Empty means unconstrained. Constraints are frontend-only and disappear
     * before concrete specialization lowering.
     */
    StringView constraint;
} GenericTypeParameter;

typedef struct {
    GenericTypeParameter *items;
    int count;
    int capacity;
} GenericTypeParameterList;

typedef struct StructOperatorDecl {
    TokenType op;
    int is_unary;
    StringView method_name;
    SourceSpan span;
} StructOperatorDecl;

typedef struct {
    StructOperatorDecl *items;
    int count;
    int capacity;
} StructOperatorDeclList;

typedef struct {
    Type **items;
    int count;
    int capacity;
} TypeList;

struct Node {
    NodeType type;

    /*
     * Top-level module visibility marker set by the contextual `export`
     * declaration prefix. It is meaningful only for source declarations;
     * semantic analysis rejects exported declarations in the root namespace.
     */
    int is_exported;

    /*
     * Canonical source provenance for diagnostics and later IR lowering.
     * `line` is retained temporarily as a compatibility cache while existing
     * debug/backend code migrates to SourceSpan.
     */
    SourceSpan span;
    int line;

    union {
        struct {
            NumberLiteralKind kind;
            union {
                uint64_t integer;
                double floating;
            } value;
        } number;

        StringView ident;
        StringView string_literal;
        StringView char_literal;

        struct {
            int value;
        } boolean;

        struct {
            CastKind kind;
            Type *target_type;
            Node *expression;
        } cast_expr;

        struct {
            /*
             * Prefix unary operators:
             *     -x
             *     !x
             *     &x
             *     *p
             */
            TokenType op;
            Node *operand;
        } unary;

        struct {
            TokenType op;      // TOK_PLUS_PLUS or TOK_MINUS_MINUS
            Node *target;
            int is_prefix;     // 1 for ++x / --x, 0 for x++ / x--
        } inc_dec;

        struct {
            TokenType op;   // TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH
            Node *left;
            Node *right;
        } binary;

        struct {
            Node *target;
            Node *value;
        } assign;           // Currently '=' only

        struct {
            TokenType op;
            Node *target;
            Node *value;
        } compound_assign;  // +=, -=. *=, /=

        struct {
            Node *condition;
            Node *then_branch;
            Node *else_branch; // NULL if no else
        } if_stmt;

        struct {
            Node *expr;
        } expr_stmt;

        struct {
            Node *statement; /* expression statement or block, executed at scope exit */
        } defer_stmt;

        struct {
            NodeList statements;
        } block;

        struct {
            Node *callee;
            TypeList type_arguments; /* explicit `::<...>` arguments; empty when inferred */
            NodeList arguments;
        } call;

        struct {
            Node *object;
            StringView name;

            /*
             * Canonical dotted spelling when this field expression is a pure
             * identifier chain (for example `std.io.print` or `state.pair.x`).
             * Empty for general runtime field expressions such as `(*p).x`.
             *
             * This is frontend-only syntax metadata used to recognize the
             * longest visible module prefix without encoding module names in
             * CogIR or backends.
             */
            StringView dotted_path;
        } field;

        struct {
            Node *object;
            Node *index;
        } index;

        struct {
            Type *source_type;
        } type_ref;

        struct {
            Token token;
        } error;

        struct {
            NodeList statements;
        } program;

        struct {
            StringView name;
        } module_decl;

        struct {
            StringView name;
            /* Optional file-local qualifier from `import name as alias;`. */
            StringView alias;
        } import_decl;

        struct {
            Type *var_type;
            StringView name;
            Node *initializer;   // NULL if none
        } var_decl;

        struct {
            NodeList declarations;  // NODE_VAR_DECL children, evaluated left-to-right
        } var_decl_group;

        struct {
            Type *var_type;
            StringView name;
        } param_decl;

        struct {
            Type *var_type;
            StringView name;
        } struct_field_decl;

        struct {
            StringView module_name; // empty for unqualified construction
            StringView name;
            TypeList type_arguments; // explicit generic type arguments, empty for ordinary structs
            NodeList fields;   // list of NODE_FIELD_INIT
        } struct_init;

        struct {
            StringView name;
            Node *value;
        } field_init;

        struct {
            Node *value;         // NULL for bare `return;`
        } return_stmt;

        struct {
            Node *condition;
            Node *body;
        } while_stmt;

        struct {
            Node *condition;     // NULL-able
            Node *post;          // NULL-able
            Node *body;
        } for_stmt;

        // break/continue need no payload -- `line` on the Node itself is enough

        struct {
            Node *expression;
            NodeList cases;
            Type *resolved_type;
        } switch_stmt;

        struct {
            Node *value;      // NULL for default
            Node *body;
            int is_default;
        } switch_case;

        struct {
            StringView name;
            GenericTypeParameterList type_parameters; /* source generic parameters; empty for ordinary/concrete functions */
            NodeList params;      // list of NODE_FUNC_PARAM_DECL
            Type *return_type;
            Node *body;           // NODE_BLOCK
            Type *resolved_type;  // semantic TYPE_FUNCTION, NULL if declaration failed
            FunctionLinkage linkage;

            /*
             * Explicit native C callback/calling-convention contract for a
             * Coglet-defined function:
             *
             *     #repr(c)
             *     callback::(value: c_int) -> c_int { ... }
             */
            int is_repr_c;

            /*
             * Optional explicit native C calling convention. C_CALL_DEFAULT
             * means the platform/default C ABI. Meaningful only for
             * #extern(c) or #repr(c) functions.
             */
            CCallingConvention c_call_conv;

            /* C-ABI variadic declaration: fixed parameters followed by `...`. */
            int is_variadic;

            /*
             * Optional external linker symbol for FUNCTION_LINKAGE_EXTERN_C.
             * Empty means the Coglet declaration name is used unchanged.
             */
            StringView external_name;
        } func_decl;

        struct {
            StringView name;
            GenericTypeParameterList type_parameters; /* source generic parameters; empty for ordinary/concrete structs */
            NodeList fields;      // list of NODE_STRUCT_FIELD_DECL
            NodeList methods;     // NODE_FUNC_DECL namespace members; never part of layout
            StructOperatorDeclList operators; /* frontend-only mappings from source operators to methods */

            /*
             * Explicit native C aggregate representation contract:
             *
             *     #repr(c)
             *     Point::struct { ... }
             */
            int is_repr_c;
            int is_resource;

            /*
             * Optional native-C layout controls for represented aggregates:
             *
             *     #repr(c, packed)
             *     #repr(c, align=16)
             *     #repr(c, packed, align=8)
             *
             * repr_c_align == 0 means no explicit minimum alignment.
             */
            int repr_c_packed;
            int repr_c_align;

            /*
             * `union` shares the aggregate field representation with structs
             * but has native C union layout when this flag is set.
             */
            int is_union;

            /*
             * Incomplete native-C struct declaration:
             *
             *     #repr(c)
             *     SDL_Window::struct;
             *
             * The type may only be used behind raw pointers until a layout is
             * provided by C; Coglet never defines or accesses its fields.
             * Incomplete unions are not part of the current C ABI subset.
             */
            int is_incomplete;
            Type *resolved_type;  // semantic TYPE_STRUCT, NULL if declaration failed
        } struct_decl;

        struct {
            StringView name;
            Type *backing_type;
            NodeList members;

            /*
             * Explicit C ABI representation contract:
             *
             *     #repr(c)
             *     Mode::enum(c_int) { ... }
             */
            int is_repr_c;
            Type *resolved_type;
        } enum_decl;

        struct {
            StringView name;
            Node *value;     // NULL if implicit
            IntegerValue resolved_value;
        } enum_member;

        struct {
            Type *const_type;    // NULL for inferred (::); non-NULL for typed (: type :)
            StringView name;
            Node *value;         // required -- never NULL
        } const_decl;

        struct {
            NodeList elements;   // values: s32[3] = [1, 2, 3];
            int is_zero_initializer; // contextual `{0}` aggregate-zero spelling
        } array_literal;

    } as;
};

// Constructors allocate from the given arena -- callers never free
// individual nodes.
Node *ast_new_integer(Arena *arena, uint64_t value, SourceSpan span);
Node *ast_new_float(Arena *arena, double value, SourceSpan span);
Node *ast_new_ident(Arena *arena, const char *start, int length, SourceSpan span);
Node *ast_new_compound_assign(Arena *arena, TokenType op, Node *target, Node *value, SourceSpan span);
Node *ast_new_string(Arena *arena, const char *start, int length, SourceSpan span);
Node *ast_new_char(Arena *arena, const char *start, int length, SourceSpan span);
Node *ast_new_null(Arena *arena, SourceSpan span);
Node *ast_new_bool(Arena *arena, int value, SourceSpan span);
Node *ast_new_cast(Arena *arena, CastKind kind, Type *target_type, Node *expression, SourceSpan span);
Node *ast_new_unary(Arena *arena, TokenType op, Node *operand, SourceSpan span);
Node *ast_new_inc_dec(Arena *arena, TokenType op, Node *target, int is_prefix, SourceSpan span);
Node *ast_new_binary(Arena *arena, TokenType op, Node *left, Node *right, SourceSpan span);
Node *ast_new_assign(Arena *arena,Node *target,Node *value,SourceSpan span);
Node *ast_new_if(Arena *arena, Node *cond, Node *then_b, Node *else_b, SourceSpan span);
Node *ast_new_expr_stmt(Arena *arena, Node *expr, SourceSpan span);
Node *ast_new_block(Arena *arena, SourceSpan span);
Node *ast_new_call(Arena *arena, Node *callee, SourceSpan span);
Node *ast_new_field(Arena *arena, Node *object, const char *name, int length, SourceSpan span );
Node *ast_new_index(Arena *arena,Node *object, Node *index, SourceSpan span);
Node *ast_new_error(Arena *arena, Token token);
Node *ast_new_program(Arena *arena, SourceSpan span);
Node *ast_new_module_decl(Arena *arena, const char *name, int length, SourceSpan span);
Node *ast_new_import_decl(Arena *arena, const char *name, int length, const char *alias, int alias_length, SourceSpan span);
Node *ast_new_var_decl(Arena *arena, Type *type, const char *name, int length, Node *initializer, SourceSpan span);
Node *ast_new_var_decl_group(Arena *arena, SourceSpan span);
Node *ast_new_struct_field_decl(Arena *arena, Type *type, const char *name, int length, SourceSpan span);
Node *ast_new_type_ref(Arena *arena, Type *source_type, SourceSpan span);
Node *ast_new_func_param_decl(Arena *arena, Type *type, const char *name, int length, SourceSpan span);
Node *ast_new_return(Arena *arena, Node *value, SourceSpan span);
Node *ast_new_defer(Arena *arena, Node *statement, SourceSpan span);
Node *ast_new_while(Arena *arena, Node *cond, Node *body, SourceSpan span);
Node *ast_new_for(Arena *arena, Node *cond, Node *post, Node *body, SourceSpan span);
Node *ast_new_break(Arena *arena, SourceSpan span);
Node *ast_new_continue(Arena *arena, SourceSpan span);
Node *ast_new_switch(Arena *arena, Node *expression, SourceSpan span);
Node *ast_new_switch_case(Arena *arena, Node *value, Node *body, int is_default, SourceSpan span);
Node *ast_new_func_decl(Arena *arena, const char *name, int name_length, Type *return_type, SourceSpan span);
Node *ast_new_struct_decl(Arena *arena, const char *name, int name_length, SourceSpan span);
Node *ast_new_struct_init(Arena *arena, const char *name, int name_length, SourceSpan span);
Node *ast_new_enum_decl(Arena *arena, const char *name, int name_length, SourceSpan span);
Node *ast_new_enum_member(Arena *arena, const char *name, int name_length, SourceSpan span);
Node *ast_new_field_init(Arena *arena, const char *name, int name_length, Node *value, SourceSpan span);
Node *ast_new_const_decl(Arena *arena, Type *type, const char *name, int name_length, Node *value, SourceSpan span);
Node *ast_new_array_literal(Arena *arena, SourceSpan span);
Node *ast_new_zero_array_initializer(Arena *arena, SourceSpan span);

Node *ast_clone(Arena *arena, const Node *node);

void nodelist_push(Arena *arena, NodeList *list, Node *node);

#endif
