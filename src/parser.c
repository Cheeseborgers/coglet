#include "../include/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static Node *finish_typed_var_decl(Parser *p, Token name);
static Node *finish_inferred_var_decl(Parser *p, Token name);
static Node *parse_decl_after_name(Parser *p, Token name);
static Node *parse_proc_decl_rest(Parser *p, Token name, int line);
static Node *parse_struct_decl_rest(Parser *p, Token name, int line);
static Node *parse_struct_field(Parser *p);

static Node *parse_return_statement(Parser *p);
static Node *parse_while_statement(Parser *p);
static Node *parse_for_statement(Parser *p);

// postfix helpers
static Node *finish_call(Parser *p, Node *callee);
static Node *finish_field(Parser *p, Node *object);
static Node *finish_index(Parser *p, Node *object);

static int is_assignable(Node *n);

// ===================== precedence =====================

typedef enum {
    PREC_NONE = 0,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,     // + -
    PREC_FACTOR,   // * / %
} Precedence;

// simple precedence table
static int get_precedence(TokenType t)
{
    switch (t) {
        case TOK_OR_OR:
            return PREC_OR;

        case TOK_AND_AND:
            return PREC_AND;

        case TOK_EQUAL_EQUAL:
        case TOK_BANG_EQUAL:
            return PREC_EQUALITY;

        case TOK_LESS:
        case TOK_LESS_EQUAL:
        case TOK_GREATER:
        case TOK_GREATER_EQUAL:
            return PREC_COMPARISON;

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
    p->current = lexer_next(&p->lexer);
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

const char *token_debug_display_name(TokenType t)
{
    switch (t)
    {
        // Special
        case TOK_EOF:             return "end of file";
        case TOK_ERROR:           return "invalid token";

        // Literals
        case TOK_NUMBER_INT:      return "integer";
        case TOK_NUMBER_FLOAT:    return "floating-point number";
        case TOK_STRING:          return "string literal";
        case TOK_CHAR:            return "character literal";
        case TOK_IDENT:           return "identifier";

        // Keywords
        case TOK_IF:              return "'if'";
        case TOK_ELSE:            return "'else'";
        case TOK_WHILE:           return "'while'";
        case TOK_FOR:             return "'for'";
        case TOK_RETURN:          return "'return'";
        case TOK_VOID:            return "'void'";
        case TOK_STRUCT:          return "'struct'";
        case TOK_BREAK:           return "'break'";
        case TOK_CONTINUE:        return "'continue'";
        case TOK_BOOL:            return "'bool'";
        case TOK_INT_KW:          return "'int'";
        case TOK_UINT_KW:         return "'uint'";
        case TOK_I8:              return "'i8'";
        case TOK_I16:             return "'i16'";
        case TOK_I32:             return "'i32'";
        case TOK_I64:             return "'i64'";
        case TOK_U8:              return "'u8'";
        case TOK_U16:             return "'u16'";
        case TOK_U32:             return "'u32'";
        case TOK_U64:             return "'u64'";
        case TOK_F32:             return "'f32'";
        case TOK_F64:             return "'f64'";

        // Operators
        case TOK_PLUS:            return "'+'";
        case TOK_MINUS:           return "'-'";
        case TOK_STAR:            return "'*'";
        case TOK_SLASH:           return "'/'";
        case TOK_PERCENT:         return "'%'";

        case TOK_PLUS_PLUS:       return "'++'";
        case TOK_MINUS_MINUS:     return "'--'";

        case TOK_PLUS_EQUAL:      return "'+='";
        case TOK_MINUS_EQUAL:     return "'-='";
        case TOK_STAR_EQUAL:      return "'*='";
        case TOK_SLASH_EQUAL:     return "'/='";

        case TOK_EQUAL:           return "'='";
        case TOK_EQUAL_EQUAL:     return "'=='";

        case TOK_BANG:            return "'!'";
        case TOK_BANG_EQUAL:      return "'!='";

        case TOK_LESS:            return "'<'";
        case TOK_LESS_EQUAL:      return "'<='";
        case TOK_GREATER:         return "'>'";
        case TOK_GREATER_EQUAL:   return "'>='";

        case TOK_AND_AND:         return "'&&'";
        case TOK_OR_OR:           return "'||'";

        case TOK_AND:             return "'&'";
        case TOK_OR:              return "'|'";

        // Punctuation
        case TOK_LPAREN:          return "'('";
        case TOK_RPAREN:          return "')'";
        case TOK_LBRACE:          return "'{'";
        case TOK_RBRACE:          return "'}'";
        case TOK_LBRACKET:        return "'['";
        case TOK_RBRACKET:        return "']'";
        case TOK_SEMICOLON:       return "';'";
        case TOK_COMMA:           return "','";
        case TOK_DOT:             return "'.'";
        case TOK_ARROW:           return "'->'";
        case TOK_COLON:           return "':'";
        case TOK_COLON_COLON:     return "'::'";
        case TOK_COLON_EQUAL:     return "':='";

        default:
            return "<unknown token>";
    }
}

static void add_diagnostic(Parser *p, Token token, const char *message)
{
    if (p->diagnostic_count >= p->diagnostic_capacity - 1) {
        fprintf(stderr, "WARNING: parser diagnostics full\n");
        return;
    }

    Diagnostic *d = &p->diagnostics[p->diagnostic_count++];

    d->token = token;
    d->message = message;

    p->had_error = 1;
    p->error_count++;
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

    p->arena = arena;
    p->scratch = scratch;

    p->had_error = 0;
    p->error_count = 0;
    p->current.type = TOK_EOF;
    p->diagnostic_capacity = 100;

    p->diagnostics = arena_alloc(arena, sizeof(Diagnostic) * p->diagnostic_capacity);
    advance(p);
}

// ===================== primary =====================
static Node *parse_primary(Parser *p)
{
    if (match(p, TOK_NUMBER_INT)) {
        Token t = p->previous;
        return ast_new_number(p->arena, strtod(t.start, NULL), 0, t.line);
    }

    if (match(p, TOK_NUMBER_FLOAT)) {
        Token t = p->previous;
        return ast_new_number(p->arena, strtod(t.start, NULL), 1, t.line);
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

    if (match(p, TOK_IDENT)) {
        Token t = p->previous;
        return ast_new_ident(p->arena, t.start, t.length, t.line);
    }

    if (match(p, TOK_LPAREN)) {
        Node *expr = parse_expression(p);
        consume(p, TOK_RPAREN);
        return expr;
    }

    error_at(p, &p->current, "expected expression");
    return ast_new_error(p->arena, p->current);
}

// ===================== postfix pipeline =====================
static Node *make_inc_dec(Parser *p, Node *expr, TokenType op)
{
    if (!is_assignable(expr)) {
        error_at(p, &p->previous, "invalid increment target");
    }

    return ast_new_assign(
        p->arena,
        expr,
        ast_new_binary(
            p->arena,
            op == TOK_PLUS_PLUS ? TOK_PLUS : TOK_MINUS,
            expr,
            ast_new_number(p->arena, 1, 0, expr->line),   // 0 = not a float literal
            expr->line
        ),
        expr->line
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
            expr = make_inc_dec(p, expr, TOK_PLUS_PLUS);
            continue;
        }

        if (match(p, TOK_MINUS_MINUS)) {
            expr = make_inc_dec(p, expr, TOK_MINUS_MINUS);
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
    if (check(p, TOK_PLUS_PLUS) || check(p, TOK_MINUS_MINUS) || check(p, TOK_MINUS) || check(p, TOK_BANG)) {
        Token op = p->current;
        advance(p);

        Node *rhs = parse_unary(p);

        if (op.type == TOK_PLUS_PLUS ||
           op.type == TOK_MINUS_MINUS)
        {
            return make_inc_dec(p, rhs, op.type);
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
        op != TOK_SLASH_EQUAL)
    {
        return left;
    }

    advance(p);

    if (!is_assignable(left))
        error_at(p, &p->previous, "invalid assignment target");

    Node *right = parse_assignment(p);

    if (op == TOK_EQUAL) return ast_new_assign(p->arena, left, right, left->line);

    TokenType binary_op;

    switch (op) {
        case TOK_PLUS_EQUAL:  binary_op = TOK_PLUS; break;
        case TOK_MINUS_EQUAL: binary_op = TOK_MINUS; break;
        case TOK_STAR_EQUAL:  binary_op = TOK_STAR; break;
        case TOK_SLASH_EQUAL: binary_op = TOK_SLASH; break;
        default: return left;
    }

    Node *combined = ast_new_binary(p->arena, binary_op, left, right, left->line);
    return ast_new_assign(p->arena, left, combined, left->line);
}

static Node *parse_assignment(Parser *p) { return parse_assignment_from(p, parse_binary(p, PREC_OR)); }

// ===================== types =====================

// Parses a type: a base keyword (int/float/char/void/struct Name)
// followed by zero or more '*' for pointer levels. Called wherever a
// type is grammatically expected -- after ':' in a var decl or param,
// or after '->' for a return type. Never called speculatively.
static Type *parse_type(Parser *p)
{
    Type *base = arena_alloc(p->arena, sizeof(Type));
    base->element            = NULL;
    base->array_size         = -1;
    base->struct_name        = NULL;
    base->struct_name_length = 0;

    if (match(p, TOK_I8))       base->kind = TYPE_I8;
    else if (match(p, TOK_I16)) base->kind = TYPE_I16;
    else if (match(p, TOK_I32)) base->kind = TYPE_I32;
    else if (match(p, TOK_I64) || match(p, TOK_INT_KW)) base->kind = TYPE_I64;
    else if (match(p, TOK_U8))  base->kind = TYPE_U8;
    else if (match(p, TOK_U16)) base->kind = TYPE_U16;
    else if (match(p, TOK_U32)) base->kind = TYPE_U32;
    else if (match(p, TOK_U64) || match(p, TOK_UINT_KW)) base->kind = TYPE_U64;
    else if (match(p, TOK_F32))  base->kind = TYPE_F32;
    else if (match(p, TOK_F64))  base->kind = TYPE_F64;
    else if (match(p, TOK_BOOL)) base->kind = TYPE_BOOL;
    else if (match(p, TOK_VOID)) base->kind = TYPE_VOID;
    else if (match(p, TOK_IDENT)) {
        base->kind               = TYPE_STRUCT;
        base->struct_name        = p->previous.start;
        base->struct_name_length = p->previous.length;
    } else {
        error_at(p, &p->current, "expected type");
        return base;
    }

    while (match(p, TOK_STAR)) {

        Type *ptr = arena_alloc(p->arena, sizeof(Type));
        ptr->kind               = TYPE_POINTER;
        ptr->element            = base;
        ptr->array_size         = -1;
        ptr->struct_name        = NULL;
        ptr->struct_name_length = 0;
        base                    = ptr;
    }

    if (match(p, TOK_LBRACKET)) {

        Type *arr = arena_alloc(p->arena, sizeof(Type));
        arr->kind               = TYPE_ARRAY;
        arr->element            = base;
        arr->array_size         = -1;
        arr->struct_name        = NULL;
        arr->struct_name_length = 0;

        if (!check(p, TOK_RBRACKET)) {
            if (match(p, TOK_NUMBER_INT)) {
                arr->array_size = (int)strtod(p->previous.start, NULL);
            } else {
                error_at(p, &p->current, "expected array size");
            }
        }
        consume(p, TOK_RBRACKET);
        base = arr;
    }

    return base;
}

// =================== variable declarations ==========================

static Node *finish_typed_var_decl(Parser *p, Token name) {

    int line   = name.line;
    Type *type = parse_type(p);

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

typedef struct PendingParam {
    Token name;
    struct PendingParam *next;
} PendingParam;

static PendingParam *pending_param_push(Parser *p, PendingParam **head, PendingParam **tail, Token token)
{
    PendingParam *node = arena_alloc(
        p->scratch,
        sizeof(PendingParam)
    );

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

    PendingParam *head = NULL;
    PendingParam *tail = NULL;

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

    // Optional default value shared by the whole parameter group
    Node *default_value = NULL;

    if (match(p, TOK_EQUAL)) {
        default_value = parse_assignment(p);
    }

    for (PendingParam *it = head; it; it = it->next) {

        Node *initializer = NULL;

        if (default_value) {
            initializer = ast_clone(p->arena, default_value);
        }

        Node *param = ast_new_var_decl(
            p->arena,
            type,
            it->name.start,
            it->name.length,
            initializer,
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

    *type = (Type){
        .kind = TYPE_VOID,
        .element = NULL,
        .array_size = -1,
        .struct_name = NULL,
        .struct_name_length = 0
    };

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

    Node *decl =ast_new_struct_decl(p->arena, name.start,name.length, line);

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
    return ast_new_var_decl(p->arena, type, field.start, field.length, NULL, field.line);
}

// ====================== end struct declarations ======================

// ====================== declaration dispatching ======================

static Node *parse_decl_after_name(Parser *p, Token name) {

    int line = name.line;

    if (check(p, TOK_LPAREN)) return parse_proc_decl_rest(p, name, line);
    if (check(p, TOK_STRUCT)) return parse_struct_decl_rest(p, name, line);

    error_at(p, &p->current, "expected '(' or 'struct' after '::'");

    synchronize(p);
    return ast_new_error(p->arena, p->current);
}

// Entry point for both declarations and identifier-led expression
// statements. This is what parse_statement, parse_for_statement's
// init clause, and parse_program all call instead of dispatching on
// a leading type keyword (there isn't one anymore -- declarations
// are name-first).
static Node *parse_decl_or_expr_statement(Parser *p) {

    if (check(p, TOK_IDENT)) {

        Token name = p->current;
        advance(p);   // consume the identifier -- shared prefix either way

        if (match(p, TOK_COLON_EQUAL)) return finish_inferred_var_decl(p, name);
        if (match(p, TOK_COLON_COLON)) return parse_decl_after_name(p, name);
        if (match(p, TOK_COLON))       return finish_typed_var_decl(p, name);

        // Not a declaration -- resume ordinary expression parsing from
        // this identifier, then finish out as an expression statement.
        Node *base      = ast_new_ident(p->arena, name.start, name.length, name.line);
        Node *postfixed = parse_postfix_from(p, base);
        Node *bin       = parse_binary_from(p, postfixed, PREC_OR);
        Node *full      = parse_assignment_from(p, bin);

        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        return ast_new_expr_stmt(p->arena, full, name.line);
    }

    // Doesn't start with an identifier at all (e.g. a number, string,
    // parenthesized expression, or unary op) -- ordinary expression
    // statement, no declaration possible.
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

    int line   = p->previous.line;
    Node *cond = parse_expression(p);
    Node *body = parse_block(p);
    return ast_new_while(p->arena,cond, body, line);
}

static Node *parse_for_statement(Parser *p) {

    int line   = p->previous.line;
    Node *cond = NULL;
    Node *post = NULL;

    if (!check(p, TOK_LBRACE)) {
        cond = parse_expression(p);

        if (match(p, TOK_COLON))
            post = parse_expression(p);
    }

    Node *body = parse_statement(p);

    return ast_new_for(p->arena, cond, post, body, line);
}

static Node *parse_statement(Parser *p) {

    if (match(p, TOK_IF))     return parse_if_statement(p);
    if (match(p, TOK_WHILE))  return parse_while_statement(p);
    if (match(p, TOK_FOR))    return parse_for_statement(p);
    if (match(p, TOK_RETURN)) return parse_return_statement(p);

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

// ===================== postfix builders =====================

static Node *finish_call(Parser *p, Node *callee) {

    Node *call = ast_new_call(p->arena, callee, callee->line);

    if (!check(p, TOK_RPAREN)) {
        do {
            Node *arg = parse_expression(p);
            nodelist_push(p->arena, &call->as.call.arguments, arg);
        } while (match(p, TOK_COMMA));
    }

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

    Node *index = parse_expression(p);
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
    Node *cond        = parse_expression(p);
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