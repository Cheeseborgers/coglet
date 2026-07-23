#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

// ===================== forward declarations =====================

static Node *parse_expression(Parser *p);
static Node *parse_expr_statement(Parser *p);
static Node *parse_if_statement(Parser *p);
static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_primary(Parser *p);
static Node *parse_postfix(Parser *p);
static Node *parse_postfix_from(Parser *p, Node *expr);
static Node *parse_unary(Parser *p);
static Node *parse_binary(Parser *p, int min_prec);
static Node *parse_binary_from(Parser *p, Node *left, int min_prec);
static Node *parse_assignment(Parser *p);
static Node *parse_assignment_from(Parser *p, Node *left);
static Type *parse_type(Parser *p);

static Node *parse_decl_or_expr_statement(Parser *p);
static Node *finish_typed_decl(Parser *p, Token name);
static Node *finish_inferred_const_decl(Parser *p, Token name);
static Node *finish_inferred_var_decl(Parser *p, Token name);
static Node *parse_decl_after_name(Parser *p, Token name);
static Node *parse_proc_decl_rest(Parser *p, Token name, int line);

static Node *parse_struct_decl_rest(Parser *p, Token name, int line);
static Node *parse_struct_field(Parser *p);
static Node *finish_struct_init(Parser *p, Token type_name);

static Node *parse_enum_decl_rest(Parser *p, Token name, int line);
static Node *parse_enum_member(Parser *p);

static Node *parse_expression_before_block(Parser *p);
static Node *parse_switch_statement(Parser *p);
static Node *parse_switch_case(Parser *p);
static Node *parse_return_statement(Parser *p);
static Node *parse_while_statement(Parser *p);
static Node *parse_for_statement(Parser *p);

static Node *parse_conversion_expression(Parser *p);
static Node *parse_array_literal(Parser *p);

static int parse_decimal_u64(Token token, uint64_t *out);
static int parse_float_token(Parser *p, Token token, double *out);

// postfix helpers
static Node *finish_call(Parser *p, Node *callee);
static Node *finish_field(Parser *p, Node *object);
static Node *finish_index(Parser *p, Node *object);

static int is_assignable(Node *n);

static void add_diagnostic(Parser *p, Token token, const char *message);

// ===================== precedence =====================

typedef enum {
    PREC_NONE = 0,
    PREC_LOGICAL_OR,
    PREC_LOGICAL_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_BITWISE_OR,
    PREC_BITWISE_XOR,
    PREC_BITWISE_AND,
    PREC_SHIFT,
    PREC_TERM,      // + -
    PREC_FACTOR,    // * / %
} Precedence;

static int get_precedence(TokenType type)
{
    switch (type) {
        case TOK_OR_OR:
            return PREC_LOGICAL_OR;

        case TOK_AND_AND:
            return PREC_LOGICAL_AND;

        case TOK_EQUAL_EQUAL:
        case TOK_BANG_EQUAL:
            return PREC_EQUALITY;

        case TOK_LESS:
        case TOK_LESS_EQUAL:
        case TOK_GREATER:
        case TOK_GREATER_EQUAL:
            return PREC_COMPARISON;

        case TOK_OR:
            return PREC_BITWISE_OR;

        case TOK_XOR:
            return PREC_BITWISE_XOR;

        case TOK_AND:
            return PREC_BITWISE_AND;

        case TOK_SHIFT_LEFT:
        case TOK_SHIFT_RIGHT:
            return PREC_SHIFT;

        case TOK_PLUS:
        case TOK_MINUS:
            return PREC_TERM;

        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:
            return PREC_FACTOR;

        default:
            return PREC_NONE;
    }
}

// ===================== lexer helpers =====================

static void advance(Parser *p) {
    p->previous = p->current;

    for (;;) {
        p->current = lexer_next(&p->lexer);

        if (p->current.type != TOK_ERROR) {
            return;
        }

        const char *message = p->lexer.error_msg;

        if (!message) {
            message = "invalid token";
        }

        add_diagnostic(
            p,
            p->current,
            message
        );
    }
}

static int check(Parser *p, TokenType type) {
    return p->current.type == type;
}

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance(p);
    return 1;
}

static void synchronize(Parser *p)
{
    advance(p);

    while (!check(p, TOK_EOF)) {

        // stop at statement boundaries
        if (p->previous.type == TOK_SEMICOLON)
            return;

        if (check(p, TOK_RBRACE))
            return;

        // or if we hit a new statement keyword
        switch (p->current.type) {
            case TOK_IF:
            case TOK_WHILE:
            case TOK_FOR:
            case TOK_RETURN:
                return;

            default:
                break;
        }

        advance(p);
    }
}

const char *token_debug_display_name(TokenType type)
{
    switch (type) {
        // Special
        case TOK_EOF:
            return "end of file";

        case TOK_ERROR:
            return "invalid token";

        // Literals
        case TOK_NUMBER_INT:
            return "integer";

        case TOK_NUMBER_FLOAT:
            return "floating-point number";

        case TOK_STRING:
            return "string literal";

        case TOK_CHAR:
            return "character literal";

        case TOK_IDENT:
            return "identifier";

        case TOK_TRUE:
            return "'true'";

        case TOK_FALSE:
            return "'false'";

        case TOK_NULL:
            return "'null'";

        // Keywords
        case TOK_IF:
            return "'if'";

        case TOK_ELSE:
            return "'else'";

        case TOK_WHILE:
            return "'while'";

        case TOK_FOR:
            return "'for'";

        case TOK_RETURN:
            return "'return'";

        case TOK_VOID:
            return "'void'";

        case TOK_STRUCT:
            return "'struct'";

        case TOK_ENUM:
            return "'enum'";

        case TOK_BREAK:
            return "'break'";

        case TOK_CONTINUE:
            return "'continue'";

        case TOK_SWITCH:
            return "'switch'";

        case TOK_CASE:
            return "'case'";

        case TOK_DEFAULT:
            return "'default'";

        case TOK_CAST:
            return "'cast'";

        case TOK_TRUNCATE:
            return "'truncate'";

        case TOK_READONLY:
            return "'readonly'";

        // Types
        case TOK_BOOL:
            return "'bool'";

        case TOK_INT_KW:
            return "'int'";

        case TOK_UINT_KW:
            return "'uint'";

        case TOK_I8:
            return "'i8'";

        case TOK_I16:
            return "'i16'";

        case TOK_I32:
            return "'i32'";

        case TOK_I64:
            return "'i64'";

        case TOK_U8:
            return "'u8'";

        case TOK_U16:
            return "'u16'";

        case TOK_U32:
            return "'u32'";

        case TOK_U64:
            return "'u64'";

        case TOK_F32:
            return "'f32'";

        case TOK_F64:
            return "'f64'";

        // Arithmetic operators
        case TOK_PLUS:
            return "'+'";

        case TOK_MINUS:
            return "'-'";

        case TOK_STAR:
            return "'*'";

        case TOK_SLASH:
            return "'/'";

        case TOK_PERCENT:
            return "'%'";

        case TOK_PLUS_PLUS:
            return "'++'";

        case TOK_MINUS_MINUS:
            return "'--'";

        // Compound assignment
        case TOK_PLUS_EQUAL:
            return "'+='";

        case TOK_MINUS_EQUAL:
            return "'-='";

        case TOK_STAR_EQUAL:
            return "'*='";

        case TOK_SLASH_EQUAL:
            return "'/='";

        case TOK_PERCENT_EQUAL:
            return "'%='";

        case TOK_AND_EQUAL:
            return "'&='";

        case TOK_OR_EQUAL:
            return "'|='";

        case TOK_XOR_EQUAL:
            return "'^='";

        case TOK_SHIFT_LEFT_EQUAL:
            return "'<<='";

        case TOK_SHIFT_RIGHT_EQUAL:
            return "'>>='";

        // Equality and comparison
        case TOK_EQUAL:
            return "'='";

        case TOK_EQUAL_EQUAL:
            return "'=='";

        case TOK_BANG:
            return "'!'";

        case TOK_BANG_EQUAL:
            return "'!='";

        case TOK_LESS:
            return "'<'";

        case TOK_LESS_EQUAL:
            return "'<='";

        case TOK_GREATER:
            return "'>'";

        case TOK_GREATER_EQUAL:
            return "'>='";

        // Logical, bitwise, and shift operators
        case TOK_AND_AND:
            return "'&&'";

        case TOK_OR_OR:
            return "'||'";

        case TOK_AND:
            return "'&'";

        case TOK_OR:
            return "'|'";

        case TOK_XOR:
            return "'^'";

        case TOK_TILDE:
            return "'~'";

        case TOK_SHIFT_LEFT:
            return "'<<'";

        case TOK_SHIFT_RIGHT:
            return "'>>'";

        // Punctuation
        case TOK_LPAREN:
            return "'('";

        case TOK_RPAREN:
            return "')'";

        case TOK_LBRACE:
            return "'{'";

        case TOK_RBRACE:
            return "'}'";

        case TOK_LBRACKET:
            return "'['";

        case TOK_RBRACKET:
            return "']'";

        case TOK_SEMICOLON:
            return "';'";

        case TOK_COMMA:
            return "','";

        case TOK_DOT:
            return "'.'";

        case TOK_ARROW:
            return "'->'";

        case TOK_COLON:
            return "':'";

        case TOK_COLON_COLON:
            return "'::'";

        case TOK_COLON_EQUAL:
            return "':='";
    }

    return "<unknown token>";
}

static void add_diagnostic(Parser *p, Token token, const char *message) {

    ParserDiagnosticNode *node = arena_alloc(p->arena, sizeof(*node));

    node->diagnostic.token   = token;
    node->diagnostic.message = arena_strdup_len(p->arena,message, strlen(message));

    node->next = NULL;

    if (p->diagnostics_last) {
        p->diagnostics_last->next = node;
    } else {
        p->diagnostics_first = node;
    }

    p->diagnostics_last = node;
    p->diagnostic_count++;

    p->had_error = 1;
}

static void error_at(Parser *p, Token *tok, const char *msg)
{
    add_diagnostic(p,*tok, msg);
}

// --------------------------------------------------------------
static int consume(Parser *p, TokenType expected)
{
    if (check(p, expected)) {
        advance(p);
        return 1;
    }

    char buffer[128];

    snprintf(buffer,
             sizeof(buffer),
             "expected %s before %s",
             token_debug_display_name(expected),
             token_debug_display_name(p->current.type));

    error_at(p, &p->current, buffer);
    return 0;
}

// ===================== init =====================

void parser_init(Parser *p, const char *filename, const char *source, Arena *arena, Arena *scratch)
{
    lexer_init(&p->lexer, filename, source); // lexer asserts filename

    p->arena   = arena;
    p->scratch = scratch;

    p->had_error   = 0;

    p->diagnostics_first = NULL;
    p->diagnostics_last  = NULL;
    p->diagnostic_count  = 0;

    p->current.type = TOK_EOF;
    p->suppress_struct_init = 0;

    advance(p);
}

// ===================== primary =====================
static Node *parse_primary(Parser *p)
{
    if (match(p, TOK_NUMBER_INT)) {
        Token token = p->previous;
        uint64_t value;

        if (!parse_decimal_u64(token, &value)) {
            error_at(
                p,
                &token,
                "integer literal exceeds u64 range"
            );

            value = 0;
        }

        return ast_new_integer(
            p->arena,
            value,
            token.line
        );
    }

    if (match(p, TOK_NUMBER_FLOAT)) {
        Token token = p->previous;
        double value;

        if (!parse_float_token(p, token, &value)) {
            error_at(
                p,
                &token,
                "floating-point literal is out of range"
            );

            value = 0.0;
        }

        return ast_new_float(
            p->arena,
            value,
            token.line
        );
    }

    /* our token's start/length from the lexer include the quotes
     * (scan_string/scan_char both capture from the opening quote through the closing one),
     * so strip one character off each end when building the node:
     * */
    if (match(p, TOK_STRING)) {
        Token t = p->previous;
        return ast_new_string(p->arena, t.start + 1, t.length - 2, t.line);
    }

    if (match(p, TOK_CHAR)) {
        Token t = p->previous;
        return ast_new_char(p->arena, t.start + 1, t.length - 2, t.line);
    }

    /*
    * Conversion keywords are parsed before identifiers because their
    * first argument is a type rather than an ordinary expression.
    */
    if (check(p, TOK_CAST) || check(p, TOK_TRUNCATE)) {
        return parse_conversion_expression(p);
    }

    if (match(p, TOK_IDENT)) {

        Token t = p->previous;
        if (check(p, TOK_LBRACE) && !p->suppress_struct_init) {
            return finish_struct_init(p, t);
        }

        return ast_new_ident(p->arena, t.start, t.length, t.line);
    }

    if (match(p, TOK_LPAREN)) {
        int saved = p->suppress_struct_init;
        p->suppress_struct_init = 0;

        Node *expr = parse_expression(p);

        p->suppress_struct_init = saved;
        consume(p, TOK_RPAREN);
        return expr;
    }

    if (match(p, TOK_TRUE)) {
        Token t = p->previous;
        return ast_new_bool(p->arena, 1, t.line);
    }

    if (match(p, TOK_FALSE)) {
        Token t = p->previous;
        return ast_new_bool(p->arena, 0, t.line);
    }

    if (match(p, TOK_NULL)) {
        return ast_new_null(p->arena, p->previous.line);
    }

    if (match(p, TOK_LBRACKET)) {
        return parse_array_literal(p);
    }

    error_at(p, &p->current, "expected expression");
    return ast_new_error(p->arena, p->current);
}

// ===================== postfix pipeline =====================
static Node *make_inc_dec(Parser *p, Node *expr, TokenType op, int is_prefix, int line)
{
    if (!is_assignable(expr)) {
        error_at(p, &p->previous, "invalid increment target");
    }

    return ast_new_inc_dec(
        p->arena,
        op,
        expr,
        is_prefix,
        line
    );
}

// Continues postfix parsing (calls, field access, indexing) from an
// already-built expression node. Used both by parse_postfix (starting
// fresh from parse_primary) and by parse_decl_or_expr_statement
// (resuming from an identifier that's already been consumed while
// checking whether it starts a declaration).
static Node *parse_postfix_from(Parser *p, Node *expr)
{
    while (1) {

        // function call: f(...)
        if (match(p, TOK_LPAREN)) {
            expr = finish_call(p, expr);
            continue;
        }

        // field access: a.b
        if (match(p, TOK_DOT)) {
            expr = finish_field(p, expr);
            continue;
        }

        // indexing: a[b]
        if (match(p, TOK_LBRACKET)) {
            expr = finish_index(p, expr);
            continue;
        }

        if (match(p, TOK_PLUS_PLUS)) {
            Token op = p->previous;
            expr = make_inc_dec(p, expr, TOK_PLUS_PLUS, 0, op.line);
            continue;
        }

        if (match(p, TOK_MINUS_MINUS)) {
            Token op = p->previous;
            expr = make_inc_dec(p, expr, TOK_MINUS_MINUS, 0, op.line);
            continue;
        }

        break;
    }

    return expr;
}

static Node *parse_postfix(Parser *p)
{
    return parse_postfix_from(p, parse_primary(p));
}

// ===================== unary =====================

static Node *parse_unary(Parser *p)
{
    if (check(p, TOK_PLUS_PLUS) ||
        check(p, TOK_MINUS_MINUS) ||
        check(p, TOK_MINUS) ||
        check(p, TOK_BANG) ||
        check(p, TOK_TILDE) ||
        check(p, TOK_AND) ||
        check(p, TOK_STAR)) {
        Token op = p->current;
        advance(p);

        Node *rhs = parse_unary(p);

        if (op.type == TOK_PLUS_PLUS || op.type == TOK_MINUS_MINUS) {
            return make_inc_dec(p, rhs, op.type, 1, op.line);
        }

        return ast_new_unary(
            p->arena,
            op.type,
            rhs,
            op.line
        );
    }

    return parse_postfix(p);
}

// ===================== binary (single unified engine) =====================

// Continues binary-operator parsing from an already-built left operand.
// See parse_postfix_from for why this split exists.
static Node *parse_binary_from(Parser *p, Node *left, int min_prec)
{
    while (1) {
        int prec = get_precedence(p->current.type);
        if (prec < min_prec) break;

        Token op = p->current;
        advance(p);

        Node *right = parse_binary(p, prec + 1);

        left = ast_new_binary(
            p->arena,
            op.type,
            left,
            right,
            op.line
        );
    }

    return left;
}

static Node *parse_binary(Parser *p, int min_prec)
{
    return parse_binary_from(p, parse_unary(p), min_prec);
}

static int is_assignable(Node *n)
{
    switch (n->type)
    {
        case NODE_IDENT:
        case NODE_FIELD:
        case NODE_INDEX:
            return 1;

        case NODE_UNARY:
            return n->as.unary.op == TOK_STAR;

        default: return 0;
    }
}

// Continues assignment parsing from an already-built left-hand side.
// See parse_postfix_from for why this split exists.
static Node *parse_assignment_from(Parser *p, Node *left)
{
    TokenType op = p->current.type;

    if (op != TOK_EQUAL &&
      op != TOK_PLUS_EQUAL &&
      op != TOK_MINUS_EQUAL &&
      op != TOK_STAR_EQUAL &&
      op != TOK_SLASH_EQUAL &&
      op != TOK_PERCENT_EQUAL &&
      op != TOK_AND_EQUAL &&
      op != TOK_OR_EQUAL &&
      op != TOK_XOR_EQUAL &&
      op != TOK_SHIFT_LEFT_EQUAL &&
      op != TOK_SHIFT_RIGHT_EQUAL) {
        return left;
    }

    advance(p);

    if (!is_assignable(left)) {
        error_at(
            p,
            &p->previous,
            "invalid assignment target"
        );
    }

    Node *right = parse_assignment(p);

    if (op == TOK_EQUAL) {
        return ast_new_assign(
            p->arena,
            left,
            right,
            left->line
        );
    }

    return ast_new_compound_assign(
        p->arena,
        op,
        left,
        right,
        left->line
    );
}

static Node *parse_assignment(Parser *p) { return parse_assignment_from(p, parse_binary(p, PREC_LOGICAL_OR)); }

// ===================== types =====================

// Parses a type: a base keyword (int/float/char/void/struct Name)
// followed by zero or more '*' for pointer levels. Called wherever a
// type is grammatically expected -- after ':' in a var decl or param,
// or after '->' for a return type. Never called speculatively.
static Type *parse_type(Parser *p)
{
    int has_readonly     = 0;
    Token readonly_token = {0};

    if (match(p, TOK_READONLY)) {
        has_readonly = 1;
        readonly_token = p->previous;
    }

    Type *base = arena_new(p->arena, Type);

    base->array_size = -1;

    if (match(p, TOK_I8)) {
        base->kind = TYPE_I8;
    } else if (match(p, TOK_I16)) {
        base->kind = TYPE_I16;
    } else if (match(p, TOK_I32)) {
        base->kind = TYPE_I32;
    } else if (match(p, TOK_I64) || match(p, TOK_INT_KW)) {
        base->kind = TYPE_I64;
    } else if (match(p, TOK_U8)) {
        base->kind = TYPE_U8;
    } else if (match(p, TOK_U16)) {
        base->kind = TYPE_U16;
    } else if (match(p, TOK_U32)) {
        base->kind = TYPE_U32;
    } else if (match(p, TOK_U64) || match(p, TOK_UINT_KW)) {
        base->kind = TYPE_U64;
    } else if (match(p, TOK_F32)) {
        base->kind = TYPE_F32;
    } else if (match(p, TOK_F64)) {
        base->kind = TYPE_F64;
    } else if (match(p, TOK_BOOL)) {
        base->kind = TYPE_BOOL;
    } else if (match(p, TOK_VOID)) {
        base->kind = TYPE_VOID;
    } else if (match(p, TOK_IDENT)) {
        /*
         * Parsed named type reference.
         *
         * Examples:
         *     Point
         *     Color
         *     TokenKind
         *
         * Semantic analysis later resolves this to TYPE_STRUCT,
         * TYPE_ENUM, or future named type kinds.
         */
        base->kind = TYPE_NAMED;
        base->named_name.data = p->previous.start;
        base->named_name.length = p->previous.length;
    } else {
        error_at(p, &p->current, "expected type");

        /*
         * Return a safe error-ish type so callers don't read
         * uninitialized memory. Parser error state is already set.
         */
        base->kind = TYPE_VOID;
        return base;
    }

    int pointer_count = 0;

    while (match(p, TOK_STAR)) {
        Type *ptr = arena_new(p->arena, Type);

        ptr->kind = TYPE_POINTER;
        ptr->element = base;
        ptr->array_size = -1;

        if (has_readonly && pointer_count == 0) {
            ptr->pointer_access =
                POINTER_ACCESS_READONLY;
        } else {
            ptr->pointer_access =
                POINTER_ACCESS_MUTABLE;
        }

        base = ptr;
        pointer_count++;
    }

    if (has_readonly && pointer_count == 0) {
        error_at(
            p,
            &readonly_token,
            "'readonly' must qualify a pointer type"
        );
    }

    if (match(p, TOK_LBRACKET)) {
        Type *arr = arena_new(p->arena, Type);

        arr->kind = TYPE_ARRAY;
        arr->element = base;
        arr->array_size = -1;

        if (!check(p, TOK_RBRACKET)) {
            if (match(p, TOK_NUMBER_INT)) {
                Token size_token = p->previous;
                uint64_t size;

                if (!parse_decimal_u64(
                        size_token,
                        &size
                    )) {
                    error_at(
                        p,
                        &size_token,
                        "array size exceeds u64 range"
                    );
                } else if (size > INT_MAX) {
                    error_at(
                        p,
                        &size_token,
                        "array size exceeds compiler limit"
                    );
                } else {
                    arr->array_size = (int)size;
                }
            } else {
                error_at(
                    p,
                    &p->current,
                    "expected array size"
                );
            }
        }

        consume(p, TOK_RBRACKET);

        base = arr;
    }

    return base;
}

// =================== variable declarations ==========================

static Node *finish_typed_decl(Parser *p, Token name) {

    int line   = name.line;
    Type *type = parse_type(p);

    // name : type : expr ;   -- typed constant
    if (match(p, TOK_COLON)) {
        Node *value = parse_assignment(p);

        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        return ast_new_const_decl(p->arena, type, name.start, name.length, value, line);
    }

    // name : type ;   or   name : type = expr ;   -- ordinary var decl
    Node *initializer = NULL;
    if (match(p, TOK_EQUAL)) {
        initializer = parse_assignment(p);
    }

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_var_decl(p->arena, type, name.start, name.length, initializer, line);
}

static Node *finish_inferred_const_decl(Parser *p, Token name) {

    int line    = name.line;
    Node *value = parse_assignment(p);   // '::' always requires a value

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_const_decl(p->arena, NULL, name.start, name.length, value, line);
}

static Node *finish_inferred_var_decl(Parser *p, Token name) {

    int  line         = name.line;
    Node *initializer = parse_assignment(p);   // ':=' always requires a value

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_var_decl(p->arena, NULL, name.start, name.length, initializer, line);
}

// ================ end variable declarations ======================

// ===================== proc/function declarations ================

typedef struct PendingParamName {
    Token name;
    struct PendingParamName *next;
} PendingParamName;

static PendingParamName *pending_param_push(Parser *p, PendingParamName **head, PendingParamName **tail, Token token)
{
    PendingParamName *node = arena_new(p->scratch, PendingParamName);

    node->name = token;
    node->next = NULL;

    if (*tail)
        (*tail)->next = node;
    else
        *head = node;

    *tail = node;

    return node;
}

static int parse_parameter_group(Parser *p, Node *func)
{
    ArenaMarker marker = arena_mark(p->scratch);

    PendingParamName *head = NULL;
    PendingParamName *tail = NULL;

    int success = 0;

    if (!consume(p, TOK_IDENT))
        goto cleanup;

    pending_param_push(p, &head, &tail, p->previous);

    while (match(p, TOK_COMMA)) {

        if (!consume(p, TOK_IDENT))
            goto cleanup;

        pending_param_push(p, &head, &tail, p->previous);

        if (check(p, TOK_COLON))
            break;
    }

    if (!consume(p, TOK_COLON))
        goto cleanup;

    Type *type = parse_type(p);

    // One default expression parsed for the whole parameter group:
    //
    // (x, y, z: i32 = 10)
    //
    // group_default_value -> NODE_NUMBER(10)
    //
    Node *group_default_value = NULL;

    if (match(p, TOK_EQUAL)) {
        group_default_value = parse_assignment(p);
    }

    for (PendingParamName *it = head; it; it = it->next) {

        // Each parameter receives its own AST copy.
        //
        // x.default_value -> NODE_NUMBER(10)
        // y.default_value -> NODE_NUMBER(10)
        //
        Node *param_default_value = NULL;

        if (group_default_value) {
            param_default_value = ast_clone(
                p->arena,
                group_default_value
            );
        }

        Node *param = ast_new_func_param_decl(
            p->arena,
            type,
            it->name.start,
            it->name.length,
            param_default_value,
            it->name.line
        );

        nodelist_push(
            p->arena,
            &func->as.func_decl.params,
            param
        );
    }

    success = 1;

    cleanup:
        arena_reset_to(p->scratch, marker);
    return success;
}

static Type *make_void_type(Arena *arena)
{
    Type *type = arena_alloc(arena, sizeof(Type));
    memset(type, 0, sizeof(*type));

    type->kind = TYPE_VOID;
    type->array_size = -1;

    return type;
}

static Node *parse_proc_decl_rest(Parser *p, Token name, int line) {

    consume(p, TOK_LPAREN); // known present from check() in parse_decl_after_name

    Node *func = ast_new_func_decl(p->arena, name.start, name.length, NULL, line);

    if (!check(p, TOK_RPAREN)) {
        do {
            if (!parse_parameter_group(p, func)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }
        } while (match(p, TOK_COMMA));
    }

    consume(p, TOK_RPAREN);

    // Parse the return type
    Type *return_type;
    if (match(p, TOK_ARROW)) {
        return_type = parse_type(p);
    } else {
        return_type = make_void_type(p->arena); // no return type/void
    }
    func->as.func_decl.return_type = return_type;

    if (!check(p, TOK_LBRACE)) {
        error_at(p, &p->current, "expected function body");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    func->as.func_decl.body = parse_block(p);

    return func;
}

// ================== end proc/function parsing ===================

// =================== struct declarations ========================
static Node *parse_struct_decl_rest(Parser *p,Token name,int line) {

    consume(p, TOK_STRUCT);
    consume(p, TOK_LBRACE);

    Node *decl = ast_new_struct_decl(p->arena, name.start,name.length, line);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Node *field = parse_struct_field(p);
        nodelist_push(p->arena, &decl->as.struct_decl.fields, field);
    }

    consume(p, TOK_RBRACE);

    return decl;
}

static Node *parse_struct_field(Parser *p) {

    consume(p, TOK_IDENT);
    Token field = p->previous;
    consume(p, TOK_COLON);
    Type *type = parse_type(p);
    consume(p, TOK_SEMICOLON);
    return ast_new_struct_field_decl(p->arena, type, field.start, field.length, field.line);
}

// Struct initializer: `Point{ x = 5, y = 10 }` (trailing comma allowed).
// `type_name` is the identifier already consumed by the caller.
static Node *finish_struct_init(Parser *p, Token type_name)
{
    consume(p, TOK_LBRACE); // known present from caller's check()

    Node *init = ast_new_struct_init(p->arena, type_name.start, type_name.length, type_name.line);

    if (!check(p, TOK_RBRACE)) {
        while (1) {
            if (!consume(p, TOK_IDENT)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }
            Token field_name = p->previous;

            if (!consume(p, TOK_EQUAL)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            Node *value = parse_assignment(p);

            Node *field = ast_new_field_init(
                p->arena,
                field_name.start,
                field_name.length,
                value,
                field_name.line
            );

            nodelist_push(p->arena, &init->as.struct_init.fields, field);

            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; // trailing comma
        }
    }

    consume(p, TOK_RBRACE);
    return init;
}

// ====================== end struct declarations ======================
// ====================== enum declarations ===========================
static Node *parse_enum_decl_rest(Parser *p, Token name, int line) {
    consume(p, TOK_ENUM);

    Type *backing_type = NULL;

    /*
     * Backing type is optional:
     *
     *     Color :: enum {
     *         Red,
     *     }
     *
     *     Color :: enum(u16) {
     *         Red,
     *     }
     */
    if (match(p, TOK_LPAREN)) {
        backing_type = parse_type(p);

        if (!consume(p, TOK_RPAREN)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
    }

    if (!consume(p, TOK_LBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *decl = ast_new_enum_decl(
        p->arena,
        name.start,
        name.length,
        line
    );

    decl->as.enum_decl.backing_type = backing_type;

    while (!check(p, TOK_RBRACE) &&
           !check(p, TOK_EOF)) {

        Node *member = parse_enum_member(p);

        nodelist_push(
            p->arena,
            &decl->as.enum_decl.members,
            member
        );

        /*
         * Members are comma-separated. A trailing comma is allowed.
         */
        if (!match(p, TOK_COMMA)) {
            if (!check(p, TOK_RBRACE)) {
                error_at(
                    p,
                    &p->current,
                    "expected ',' or '}' after enum member"
                );

                synchronize(p);
            }

            break;
        }
           }

    consume(p, TOK_RBRACE);

    return decl;
}

static Node *parse_enum_member(Parser *p)
{
    if (!consume(p, TOK_IDENT)) {
        return ast_new_error(
            p->arena,
            p->current
        );
    }

    Token name = p->previous;

    Node *member = ast_new_enum_member(
        p->arena,
        name.start,
        name.length,
        name.line
    );

    if (match(p, TOK_EQUAL)) {
        member->as.enum_member.value =
            parse_expression(p);
    }

    return member;
}
// ====================== end enum declarations ========================
static Node *parse_expression_before_block(Parser *p)
{
    int saved_suppress_struct_init =
        p->suppress_struct_init;

    p->suppress_struct_init = 1;

    Node *expr =
        parse_expression(p);

    p->suppress_struct_init =
        saved_suppress_struct_init;

    return expr;
}
// ====================== switch statements ============================
static Node *parse_switch_statement(Parser *p)
{
    Token keyword = p->current;

    consume(p, TOK_SWITCH);

    Node *expression =
        parse_expression_before_block(p);

    if (!consume(p, TOK_LBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *stmt =
        ast_new_switch(
            p->arena,
            expression,
            keyword.line
        );

    while (!check(p, TOK_RBRACE) &&
           !check(p, TOK_EOF)) {

        if (!check(p, TOK_CASE) &&
            !check(p, TOK_DEFAULT)) {

            error_at(
                p,
                &p->current,
                "expected 'case' or 'default' in switch"
            );

            synchronize(p);
            continue;
            }

        Node *case_node =
            parse_switch_case(p);

        nodelist_push(
            p->arena,
            &stmt->as.switch_stmt.cases,
            case_node
        );
           }

    consume(p, TOK_RBRACE);

    return stmt;
}

static Node *parse_switch_case(Parser *p)
{
    if (match(p, TOK_CASE)) {
        Token keyword = p->previous;

        Node *value = parse_expression(p);

        if (!consume(p, TOK_COLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Node *body = parse_statement(p);

        return ast_new_switch_case(
            p->arena,
            value,
            body,
            0,
            keyword.line
        );
    }

    if (match(p, TOK_DEFAULT)) {
        Token keyword = p->previous;

        if (!consume(p, TOK_COLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Node *body = parse_statement(p);

        return ast_new_switch_case(
            p->arena,
            NULL,
            body,
            1,
            keyword.line
        );
    }

    error_at(
        p,
        &p->current,
        "expected 'case' or 'default'"
    );

    return ast_new_error(p->arena, p->current);
}

// ====================== end switch statements ========================
// ====================== declaration dispatching ======================
static Node *parse_decl_after_name(Parser *p, Token name) {

    int line = name.line;

    if (check(p, TOK_LPAREN)) return parse_proc_decl_rest(p, name, line);
    if (check(p, TOK_STRUCT)) return parse_struct_decl_rest(p, name, line);
    if (check(p, TOK_ENUM))   return parse_enum_decl_rest(p, name, line);

    // anything else after '::' is a constant expression
    return finish_inferred_const_decl(p, name);
}

// Entry point for both declarations and identifier-led expression
// statements. This is what parse_statement, parse_for_statement's
// init clause, and parse_program all call instead of dispatching on
// a leading type keyword (there isn't one anymore -- declarations
// are name-first).
static Node *parse_decl_or_expr_statement(Parser *p)
{
    if (check(p, TOK_IDENT)) {
        Token name = p->current;
        advance(p);

        if (match(p, TOK_COLON_EQUAL)) {
            return finish_inferred_var_decl(p, name);
        }

        if (match(p, TOK_COLON_COLON)) {
            return parse_decl_after_name(p, name);
        }

        if (match(p, TOK_COLON)) {
            return finish_typed_decl(p, name);
        }

        Node *base = NULL;

        if (check(p, TOK_LBRACE) &&
            !p->suppress_struct_init) {
            base = finish_struct_init(
                p,
                name
            );
            } else {
                base = ast_new_ident(
                    p->arena,
                    name.start,
                    name.length,
                    name.line
                );
            }

        Node *postfixed = parse_postfix_from(
            p,
            base
        );

        Node *binary = parse_binary_from(
            p,
            postfixed,
            PREC_LOGICAL_OR
        );

        Node *full = parse_assignment_from(
            p,
            binary
        );

        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return ast_new_error(
                p->arena,
                p->current
            );
        }

        return ast_new_expr_stmt(
            p->arena,
            full,
            name.line
        );
    }

    return parse_expr_statement(p);
}

// ======================= end declaration dispatching ==================

// TODO: Ensure we only allow return only inside a function body,
static Node *parse_return_statement(Parser *p) {

    int line = p->previous.line;   // TOK_RETURN already consumed by caller

    Node *value = NULL;
    if (!check(p, TOK_SEMICOLON))
        value = parse_expression(p);

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_return(p->arena, value, line);
}

static Node *parse_while_statement(Parser *p) {

    int line = p->previous.line;

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 1;
    Node *cond = parse_expression(p);
    p->suppress_struct_init = saved;

    Node *body = parse_block(p);
    return ast_new_while(p->arena,cond, body, line);
}

static Node *parse_for_statement(Parser *p) {

    int line   = p->previous.line;
    Node *cond = NULL;
    Node *post = NULL;

    if (!check(p, TOK_LBRACE)) {

        int saved = p->suppress_struct_init;
        p->suppress_struct_init = 1;

        cond = parse_expression(p);

        if (match(p, TOK_COLON))
            post = parse_expression(p);

        p->suppress_struct_init = saved;
    }

    Node *body = parse_statement(p);

    return ast_new_for(p->arena, cond, post, body, line);
}

static Node *parse_statement(Parser *p) {

    if (match(p, TOK_IF))     return parse_if_statement(p);
    if (match(p, TOK_WHILE))  return parse_while_statement(p);
    if (match(p, TOK_FOR))    return parse_for_statement(p);
    if (match(p, TOK_RETURN)) return parse_return_statement(p);
    if (check(p, TOK_SWITCH)) return parse_switch_statement(p);

    if (match(p, TOK_BREAK)) {
        int line = p->previous.line;
        if (!consume(p, TOK_SEMICOLON)) { synchronize(p); return ast_new_error(p->arena, p->current); }
        return ast_new_break(p->arena, line);
    }

    if (match(p, TOK_CONTINUE)) {
        int line = p->previous.line;
        if (!consume(p, TOK_SEMICOLON)) { synchronize(p); return ast_new_error(p->arena, p->current); }
        return ast_new_continue(p->arena, line);
    }

    if (check(p, TOK_LBRACE)) return parse_block(p);

    return parse_decl_or_expr_statement(p);
}

static Node *parse_block(Parser *p) {

    if (!consume(p, TOK_LBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *block = ast_new_block(p->arena, p->previous.line);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        Node *stmt = parse_statement(p);
        nodelist_push(p->arena, &block->as.block.statements, stmt);
    }

    consume(p, TOK_RBRACE);

    return block;
}

// ===================== expression entry =====================
static Node *parse_expression(Parser *p) { return parse_assignment(p); }

// ===================== casts ================================
static Node *parse_conversion_expression(Parser *p) {

    Token keyword = p->current;

    CastKind kind;

    switch (keyword.type) {
        case TOK_CAST:
            kind = CAST_CHECKED;
            break;

        case TOK_TRUNCATE:
            kind = CAST_TRUNCATING;
            break;

        default:
            assert(0 && "conversion parser called without conversion token");
            return ast_new_error(p->arena, keyword);
    }

    advance(p);

    if (!consume(p, TOK_LPAREN)) {
        synchronize(p);

        return ast_new_error(p->arena,p->current);
    }

    Type *target_type = parse_type(p);

    if (!consume(p, TOK_COMMA)) {
        synchronize(p);

        return ast_new_error(p->arena,p->current);
    }

    Node *expression = parse_assignment(p);

    if (!consume(p, TOK_RPAREN)) {
        synchronize(p);

        return ast_new_error(
            p->arena,
            p->current
        );
    }

    return ast_new_cast(
        p->arena,
        kind,
        target_type,
        expression,
        keyword.line
    );
}

static Node *parse_array_literal(Parser *p) {

    Token open = p->previous;

    Node *array = ast_new_array_literal(p->arena, open.line);

    if (!check(p, TOK_RBRACKET)) {
        while (1) {
            Node *element = parse_assignment(p);

            nodelist_push(
                p->arena,
                &array->as.array_literal.elements,
                element
            );

            if (!match(p, TOK_COMMA))
                break;

            /*
             * Allow trailing comma:
             *
             * [1, 2, 3,]
             */
            if (check(p, TOK_RBRACKET))
                break;
        }
    }

    consume(p, TOK_RBRACKET);

    return array;
}

static int parse_decimal_u64(Token token, uint64_t *out)
{
    uint64_t value = 0;

    for (int i = 0; i < token.length; i++) {
        unsigned digit = (unsigned)(token.start[i] - '0');

        if (value > (UINT64_MAX - digit) / 10)
            return 0;

        value = value * 10 + digit;
    }

    *out = value;
    return 1;
}

static int parse_float_token(Parser *p, Token token, double *out)
{
    // TODO: Why we using scratch here? what is the life time?
    char *text = arena_alloc(p->scratch, (size_t)token.length + 1);

    memcpy(text, token.start, (size_t)token.length);
    text[token.length] = '\0';

    char *end    = NULL;
    double value = strtod(text, &end);

    if (!isfinite(value) || end != text + token.length)
        return 0;

    *out = value;
    return 1;
}

// ===================== postfix builders =====================

static Node *finish_call(Parser *p, Node *callee) {

    Node *call = ast_new_call(p->arena, callee, callee->line);

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            Node *arg = parse_expression(p);
            nodelist_push(p->arena, &call->as.call.arguments, arg);
        } while (match(p, TOK_COMMA));
    }

    p->suppress_struct_init = saved;

    consume(p, TOK_RPAREN);
    return call;
}

static Node *finish_field(Parser *p, Node *object) {

    if (!match(p, TOK_IDENT)) {
        error_at(p, &p->current, "expected field name");
        return ast_new_error(p->arena, p->current);
    }

    Token name = p->previous;
    return ast_new_field(p->arena, object, name.start, name.length, object->line);
}

static Node *finish_index(Parser *p, Node *object) {

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 0;

    Node *index = parse_expression(p);

    p->suppress_struct_init = saved;

    consume(p, TOK_RBRACKET);
    return ast_new_index(p->arena, object, index, object->line);
}
// ===================== statements =====================

static Node *parse_expr_statement(Parser *p) {

    int line   = p->current.line;
    Node *expr = parse_assignment(p);

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_expr_stmt(p->arena, expr, line);
}

// TODO: if currently forces braces:
static Node *parse_if_statement(Parser *p) {

    int line          = p->previous.line;

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 1;
    Node *cond              = parse_expression(p);
    p->suppress_struct_init = saved;

    Node *then_branch = parse_block(p);
    Node *else_branch = NULL;

    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF)) else_branch = parse_if_statement(p);
        else                  else_branch = parse_statement(p);
    }

    return ast_new_if(p->arena,cond, then_branch, else_branch, line);
}

// ===================== program =====================
Node *parse_program(Parser *p) {
    Node *program = ast_new_program(p->arena, p->current.line);

    while (!check(p, TOK_EOF)) {
        Node *decl = parse_statement(p);
        nodelist_push(p->arena, &program->as.program.statements, decl);
    }

    return program;
}
