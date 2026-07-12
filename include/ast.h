#ifndef COGLET_AST_H
#define COGLET_AST_H

#include "lexer.h"
#include "types.h"
#include "utils/arena.h"
#include "utils/string_view.h"

typedef enum {
    NODE_NUMBER,       // a literal like 3 or 3.14
    NODE_IDENT,
    NODE_STRING,
    NODE_CHAR,
    NODE_BOOL,

    NODE_UNARY,        // <op> operand, e.g. -x
    NODE_BINARY,       // left <op> right
    NODE_INC_DEC,      // ++x, --x, x++, x--

    NODE_BLOCK,
    NODE_ASSIGN,       // =

    NODE_IF,

    NODE_EXPR_STMT,    // an expression used as a statement: `1 + 2;`

    NODE_CALL,         // function calls
    NODE_FIELD,
    NODE_INDEX,

    NODE_PROGRAM,      // the whole file: a list of statements

    NODE_VAR_DECL,

    NODE_FUNC_DECL,
    NODE_FUNC_PARAM_DECL,

    NODE_STRUCT_DECL,
    NODE_STRUCT_FIELD_DECL,

    NODE_STRUCT_INIT,      // Point{ x = 5, y = 10 }
    NODE_FIELD_INIT,       // one `x = 5` inside a struct init

    NODE_CONST_DECL,       // PI :: 3.14159;  or  PI: f64 : 3.14159;

    NODE_RETURN,
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

struct Node {
    NodeType type;
    int line;   // always set during parsing -- needed for error messages

    union {
        struct {
            double value;
            int is_float;       // 1 if this came from TOK_NUMBER_FLOAT, 0 otherwise
        } number;

        StringView ident;
        StringView string_literal;
        StringView char_literal;

        struct {
            int value;
        } boolean;

        struct {
            TokenType op;   // currently just TOK_MINUS
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
            Node *condition;
            Node *then_branch;
            Node *else_branch; // NULL if no else
        } if_stmt;

        struct {
            Node *expr;
        } expr_stmt;

        struct {
            NodeList statements;
        } block;

        struct {
            Node *callee;
            NodeList arguments;
        } call;

        struct {
            Node *object;
            StringView name;
        } field;

        struct {
            Node *object;
            Node *index;
        } index;

        struct {
            Token token;
        } error;

        struct {
            NodeList statements;
        } program;

        struct {
            Type *var_type;
            StringView name;
            Node *initializer;   // NULL if none
        } var_decl;

        struct {
            Type *var_type;
            StringView name;
            Node *default_value;   // NULL if none
        } param_decl;

        struct {
            Type *var_type;
            StringView name;
        } struct_field_decl;

        struct {
            StringView name;
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
            StringView name;
            NodeList params;      // list of NODE_FUNC_PARAM_DECL
            Type *return_type;
            Node *body;           // NODE_BLOCK
            Type *resolved_type;  // semantic TYPE_FUNCTION, NULL if declaration failed
        } func_decl;

        struct {
            StringView name;
            NodeList fields;      // list of NODE_STRUCT_FIELD_DECL
        } struct_decl;

        struct {
            Type *const_type;    // NULL for inferred (::); non-NULL for typed (: type :)
            StringView name;
            Node *value;         // required -- never NULL
        } const_decl;
    } as;
};

// Constructors allocate from the given arena -- callers never free
// individual nodes.
Node *ast_new_number(Arena *arena, double value, int is_float, int line);
Node *ast_new_ident(Arena *arena, const char *start, int length, int line);
Node *ast_new_string(Arena *arena, const char *start, int length, int line);
Node *ast_new_char(Arena *arena, const char *start, int length, int line);
Node *ast_new_bool(Arena *arena, int value, int line);
Node *ast_new_unary(Arena *arena, TokenType op, Node *operand, int line);
Node *ast_new_inc_dec(Arena *arena, TokenType op, Node *target, int is_prefix, int line);
Node *ast_new_binary(Arena *arena, TokenType op, Node *left, Node *right, int line);
Node *ast_new_assign(Arena *arena,Node *target,Node *value,int line);
Node *ast_new_if(Arena *arena, Node *cond, Node *then_b, Node *else_b, int line);
Node *ast_new_expr_stmt(Arena *arena, Node *expr, int line);
Node *ast_new_block(Arena *arena, int line);
Node *ast_new_call(Arena *arena, Node *callee, int line);
Node *ast_new_field(Arena *arena, Node *object, const char *name, int length, int line );
Node *ast_new_index(Arena *arena,Node *object, Node *index, int line);
Node *ast_new_error(Arena *arena, Token token);
Node *ast_new_program(Arena *arena, int line);
Node *ast_new_var_decl(Arena *arena, Type *type, const char *name, int length, Node *initializer, int line);
Node *ast_new_struct_field_decl(Arena *arena, Type *type, const char *name, int length, int line);
Node *ast_new_func_param_decl(Arena *arena, Type *type, const char *name, int length, Node *default_value, int line);
Node *ast_new_return(Arena *arena, Node *value, int line);
Node *ast_new_while(Arena *arena, Node *cond, Node *body, int line);
Node *ast_new_for(Arena *arena, Node *cond, Node *post, Node *body, int line);
Node *ast_new_break(Arena *arena, int line);
Node *ast_new_continue(Arena *arena, int line);
Node *ast_new_func_decl(Arena *arena, const char *name, int name_length, Type *return_type, int line);
Node *ast_new_struct_decl(Arena *arena, const char *name, int name_length, int line);
Node *ast_new_struct_init(Arena *arena, const char *name, int name_length, int line);
Node *ast_new_field_init(Arena *arena, const char *name, int name_length, Node *value, int line);
Node *ast_new_const_decl(Arena *arena, Type *type, const char *name, int name_length, Node *value, int line);

// TODO: Allow cloning of all ast node types
Node *ast_clone(Arena *arena, const Node *node);

void nodelist_push(Arena *arena, NodeList *list, Node *node);

#endif
