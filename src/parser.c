#include "parser.h"
#include "string_decode.h"

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
static Node *parse_if_expression(Parser *p);
static Node *parse_statement(Parser *p);
static Node *parse_block(Parser *p);
static Node *parse_primary(Parser *p);
static Node *parse_type_layout_query(Parser *p);
static void type_list_push(Arena *arena, TypeList *list, Type *value);
static Node *parse_postfix(Parser *p);
static Node *parse_postfix_from(Parser *p, Node *expr);
static Node *parse_unary(Parser *p);
static Node *parse_binary(Parser *p, int min_prec);
static Node *parse_binary_from(Parser *p, Node *left, int min_prec);
static Node *parse_assignment(Parser *p);
static Node *parse_assignment_from(Parser *p, Node *left);
static Type *parse_type(Parser *p);
static int parse_explicit_type_arguments(Parser *p, TypeList *out, const char *subject);
static Type *make_void_type(Arena *arena);

static Node *parse_decl_or_expr_statement(Parser *p);
static Node *finish_typed_decl(Parser *p, Token name);
static Node *finish_inferred_const_decl(Parser *p, Token name);
static Node *finish_inferred_var_decl(Parser *p, Token name);
static Node *parse_decl_after_name(Parser *p, Token name);
static Node *parse_proc_decl_rest(Parser *p, Token name, SourceSpan span);
static Node *parse_generic_decl_rest(Parser *p, Token name, SourceSpan span);
static Node *parse_attribute_decl(Parser *p);

static Node *parse_struct_decl_rest(Parser *p, Token name, SourceSpan span);
static int parse_struct_operator_block(Parser *p, Node *decl);
static Node *finish_struct_init(Parser *p, Token type_name);

static Node *parse_enum_decl_rest(Parser *p, Token name, SourceSpan span);
static Node *parse_enum_member(Parser *p);

static Node *parse_expression_before_block(Parser *p);
static Node *parse_switch_statement(Parser *p);
static Node *parse_switch_case(Parser *p);
static Node *parse_return_statement(Parser *p);
static Node *parse_defer_statement(Parser *p);
static Node *parse_static_assert_statement(Parser *p);
static Node *parse_asm_statement(Parser *p);
static Node *parse_while_statement(Parser *p);
static Node *parse_for_statement(Parser *p);
static Node *parse_scoped_control_body(Parser *p);

static Node *parse_conversion_expression(Parser *p);
static Node *parse_array_literal(Parser *p);
static Node *parse_zero_array_initializer(Parser *p);

static int parse_integer_u64(Token token, uint64_t *out);
static int parse_float_token(Parser *p, Token token, double *out);
static int token_text_equals(Token token, const char *text);
static int parse_c_calling_convention_token(
    Parser *p, Token token, CCallingConvention *out
);

typedef struct {
    StringView full;
    StringView prefix;
    StringView leaf;
    SourceSpan span;
    int component_count;
} ParsedDottedName;

static ParsedDottedName parse_dotted_name_from_first(Parser *p, Token first);
static int split_dotted_leaf(StringView full, StringView *out_prefix, StringView *out_leaf);
static Node *finish_struct_init_named(
    Parser *p, StringView module_name, StringView type_name, SourceSpan span
);

// postfix helpers
static Node *finish_call(Parser *p, Node *callee);
static Node *finish_generic_call(Parser *p, Node *callee);
static Node *finish_field(Parser *p, Node *object);
static Node *finish_index(Parser *p, Node *object);

static int is_assignable(Node *n);
static int generic_suffix_starts_expression(Parser *p);

static void error_at(Parser *p, Token *tok, const char *msg);
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

static TokenType peek_next_token_type(Parser *p) {
    Lexer lexer = p->lexer;
    Token token = lexer_next(&lexer);
    return token.type;
}

static int match(Parser *p, TokenType type) {
    if (!check(p, type)) return 0;
    advance(p);
    return 1;
}

static int match_generic_greater(Parser *p) {
    if (match(p, TOK_GREATER))
        return 1;

    /*
     * The lexer correctly tokenizes `>>` as a shift operator for expressions.
     * Inside nested generic argument lists, the same bytes are two closing
     * delimiters. Consume the first `>` now and leave a synthetic second
     * `>` as the current token for the enclosing generic list.
     */
    if (check(p, TOK_SHIFT_RIGHT)) {
        Token pair = p->current;
        Token first = pair;
        first.type = TOK_GREATER;
        first.length = 1;

        Token second = pair;
        second.type = TOK_GREATER;
        second.start = pair.start + 1;
        second.length = 1;

        p->previous = first;
        p->current = second;
        return 1;
    }

    return 0;
}

static int consume_generic_greater(Parser *p, const char *message) {
    if (match_generic_greater(p))
        return 1;
    error_at(p, &p->current, message);
    return 0;
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
            case TOK_DEFER:
            case TOK_DISCARD:
            case TOK_STATIC_ASSERT:
            case TOK_RESOURCE:
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

        case TOK_IN:
            return "'in'";

        case TOK_RETURN:
            return "'return'";

        case TOK_DEFER:
            return "'defer'";

        case TOK_DISCARD:
            return "'discard'";

        case TOK_STATIC_ASSERT:
            return "'static_assert'";

        case TOK_VOID:
            return "'void'";

        case TOK_STRUCT:
            return "'struct'";

        case TOK_RESOURCE:
            return "'resource'";

        case TOK_UNION:
            return "'union'";

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

        case TOK_REINTERPRET:
            return "'reinterpret'";

        case TOK_MOVE:
            return "'move'";

        case TOK_READONLY:
            return "'readonly'";

        case TOK_VOLATILE:
            return "'volatile'";

        case TOK_OPAQUE:
            return "'opaque'";

        case TOK_CFN:
            return "'cfn'";

        case TOK_SIZE_OF:
            return "'size_of'";

        case TOK_ALIGN_OF:
            return "'align_of'";

        case TOK_ASM:
            return "'asm'";

        // Types
        case TOK_BOOL:
            return "'bool'";

        case TOK_INT_KW:
            return "'int'";

        case TOK_UINT_KW:
            return "'uint'";

        case TOK_S8:
            return "'s8'";

        case TOK_S16:
            return "'s16'";

        case TOK_S32:
            return "'s32'";

        case TOK_S64:
            return "'s64'";

        case TOK_U8:
            return "'u8'";

        case TOK_U16:
            return "'u16'";

        case TOK_U32:
            return "'u32'";

        case TOK_U64:
            return "'u64'";

        case TOK_ISIZE:
            return "'isize'";

        case TOK_USIZE:
            return "'usize'";

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

        case TOK_ELLIPSIS:
            return "'...'";

        case TOK_ARROW:
            return "'->'";

        case TOK_COLON:
            return "':'";

        case TOK_COLON_COLON:
            return "'::'";

        case TOK_COLON_EQUAL:
            return "':='";

        case TOK_HASH:
            return "'#'";
    }

    return "<unknown token>";
}

static void add_diagnostic(Parser *p, Token token, const char *message) {
    diagnostic_add(
        &p->diagnostics,
        DIAGNOSTIC_ERROR,
        token.type == TOK_ERROR ? DIAGNOSTIC_PHASE_LEXER : DIAGNOSTIC_PHASE_PARSER,
        token.span,
        message
    );

    p->diagnostic_count = p->diagnostics.count;
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

static void parser_init_common(
    Parser *p,
    SourceManager *sources,
    SourceFileId source_id,
    Arena *arena,
    Arena *scratch
) {
    const SourceFile *file = source_manager_get(sources, source_id);
    assert(file);

    lexer_init_with_source_id(
        &p->lexer,
        source_id,
        file->filename,
        file->source
    );

    p->arena = arena;
    p->scratch = scratch;
    p->sources = sources;
    p->source_id = source_id;

    p->had_error = 0;
    diagnostic_list_init(&p->diagnostics, arena);
    p->diagnostic_count = 0;

    memset(&p->current, 0, sizeof(p->current));
    p->current.type = TOK_EOF;
    p->current.span = source_span_invalid();
    p->previous = p->current;
    p->suppress_struct_init = 0;

    advance(p);
}

void parser_init_with_source(
    Parser *p,
    SourceManager *sources,
    SourceFileId source_id,
    Arena *arena,
    Arena *scratch
) {
    assert(p);
    assert(sources);
    parser_init_common(p, sources, source_id, arena, scratch);
}

void parser_init(Parser *p, const char *filename, const char *source, Arena *arena, Arena *scratch)
{
    source_manager_init(&p->local_sources, arena);
    SourceFileId source_id = source_manager_add(&p->local_sources, filename, source);
    parser_init_common(p, &p->local_sources, source_id, arena, scratch);
}

static Node *parse_type_layout_query(Parser *p)
{
    Token keyword = p->current;
    assert(keyword.type == TOK_SIZE_OF || keyword.type == TOK_ALIGN_OF);
    advance(p);

    if (match(p, TOK_COLON_COLON)) {
        error_at(
            p,
            &p->previous,
            keyword.type == TOK_SIZE_OF
                ? "'size_of' uses size_of(T), not generic syntax"
                : "'align_of' uses align_of(T), not generic syntax"
        );
        while (!check(p, TOK_SEMICOLON) &&
               !check(p, TOK_RBRACE) &&
               !check(p, TOK_EOF)) {
            advance(p);
        }
        return ast_new_error(p->arena, keyword);
    }

    if (!consume(p, TOK_LPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (match(p, TOK_RPAREN)) {
        Node *callee = ast_new_ident(
            p->arena,
            keyword.start,
            keyword.length,
            keyword.span
        );
        return ast_new_call(
            p->arena,
            callee,
            source_span_join(keyword.span, p->previous.span)
        );
    }

    Type *queried = parse_type(p);

    if (!consume(p, TOK_RPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *callee = ast_new_ident(
        p->arena,
        keyword.start,
        keyword.length,
        keyword.span
    );
    Node *call = ast_new_call(
        p->arena,
        callee,
        source_span_join(keyword.span, p->previous.span)
    );
    type_list_push(p->arena, &call->as.call.type_arguments, queried);
    return call;
}

static Node *parse_if_expression(Parser *p)
{
    SourceSpan span = p->previous.span;

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 1;
    Node *condition = parse_expression(p);
    p->suppress_struct_init = saved;

    if (!consume(p, TOK_LBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *then_value = parse_expression(p);
    if (!consume(p, TOK_RBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (!consume(p, TOK_ELSE)) {
        error_at(p, &p->current, "if expression requires an else branch");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *else_value = NULL;
    if (match(p, TOK_IF)) {
        else_value = parse_if_expression(p);
    } else {
        if (!consume(p, TOK_LBRACE)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        else_value = parse_expression(p);
        if (!consume(p, TOK_RBRACE)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
    }

    return ast_new_if_expr(
        p->arena,
        condition,
        then_value,
        else_value,
        source_span_join(span, else_value->span)
    );
}

// ===================== primary =====================
static Node *parse_primary(Parser *p)
{
    if (match(p, TOK_IF))
        return parse_if_expression(p);

    if (match(p, TOK_NUMBER_INT)) {

        Token token = p->previous;
        uint64_t value;

        if (!parse_integer_u64(token, &value)) {
            error_at(p, &token,
                "integer literal exceeds u64 range");

            value = 0;
        }

        return ast_new_integer(
            p->arena,
            value,
            token.span
        );
    }

    if (match(p, TOK_NUMBER_FLOAT)) {
        Token token = p->previous;
        double value;

        if (!parse_float_token(p, token, &value)) {
            error_at(p, &token,
                "floating-point literal is out of range");

            value = 0.0;
        }

        return ast_new_float(
            p->arena,
            value,
            token.span
        );
    }

    /* our token's start/length from the lexer include the quotes
     * (scan_string/scan_char both capture from the opening quote through the closing one),
     * so strip one character off each end when building the node:
     * */
    if (match(p, TOK_STRING)) {
        Token t = p->previous;
        return ast_new_string(p->arena, t.start + 1, t.length - 2, t.span);
    }

    if (match(p, TOK_CHAR)) {
        Token t = p->previous;
        return ast_new_char(p->arena, t.start + 1, t.length - 2, t.span);
    }

    if (check(p, TOK_SIZE_OF) || check(p, TOK_ALIGN_OF))
        return parse_type_layout_query(p);

    /*
    * Conversion keywords are parsed before identifiers because their
    * first argument is a type rather than an ordinary expression.
    */
    if (check(p, TOK_CAST) ||
        check(p, TOK_TRUNCATE) ||
        check(p, TOK_REINTERPRET)) {
        return parse_conversion_expression(p);
    }

    if (match(p, TOK_IDENT)) {

        Token t = p->previous;
        if (check(p, TOK_LBRACE) && !p->suppress_struct_init) {
            return finish_struct_init(p, t);
        }

        return ast_new_ident(p->arena, t.start, t.length, t.span);
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
        return ast_new_bool(p->arena, 1, t.span);
    }

    if (match(p, TOK_FALSE)) {
        Token t = p->previous;
        return ast_new_bool(p->arena, 0, t.span);
    }

    if (match(p, TOK_NULL)) {
        return ast_new_null(p->arena, p->previous.span);
    }

    if (match(p, TOK_LBRACKET)) {
        return parse_array_literal(p);
    }

    if (match(p, TOK_LBRACE)) {
        return parse_zero_array_initializer(p);
    }

    error_at(p, &p->current, "expected expression");
    return ast_new_error(p->arena, p->current);
}

// ===================== postfix pipeline =====================
static Node *make_inc_dec(Parser *p, Node *expr, TokenType op, int is_prefix, SourceSpan op_span)
{
    if (!is_assignable(expr)) {
        error_at(p, &p->previous, "invalid increment target");
    }

    return ast_new_inc_dec(
        p->arena,
        op,
        expr,
        is_prefix,
        source_span_join(op_span, expr->span)
    );
}

static void generic_type_parameter_list_push(
    Arena *arena,
    GenericTypeParameterList *list,
    GenericTypeParameter value
) {
    if (list->count >= list->capacity) {
        int capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        GenericTypeParameter *items =
            arena_alloc(arena, sizeof(*items) * (size_t)capacity);
        if (list->items && list->count > 0)
            memcpy(items, list->items, sizeof(*items) * (size_t)list->count);
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
}

static void type_list_push(Arena *arena, TypeList *list, Type *value)
{
    if (list->count >= list->capacity) {
        int capacity = list->capacity == 0 ? 4 : list->capacity * 2;
        Type **items = arena_alloc(arena, sizeof(*items) * (size_t)capacity);
        if (list->items && list->count > 0)
            memcpy(items, list->items, sizeof(*items) * (size_t)list->count);
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
}

// Continues postfix parsing (calls, field access, indexing) from an
// already-built expression node. Used both by parse_postfix (starting
// fresh from parse_primary) and by parse_decl_or_expr_statement
// (resuming from an identifier that's already been consumed while
// checking whether it starts a declaration).
static Node *parse_postfix_from(Parser *p, Node *expr)
{
    while (1) {

        // explicit generic call: f::<T, U>(...)
        if (match(p, TOK_COLON_COLON)) {
            expr = finish_generic_call(p, expr);
            continue;
        }

        // function call: f(...)
        if (match(p, TOK_LPAREN)) {
            expr = finish_call(p, expr);
            continue;
        }

        // field access: a.b
        if (match(p, TOK_DOT)) {
            expr = finish_field(p, expr);

            /*
             * Qualified nominal construction:
             *
             *     math.Pair { x = 1, y = 2 }
             *     std.math.Pair { x = 1, y = 2 }
             *
             * Keep ordinary runtime field access unchanged. Pure identifier
             * chains carry a canonical dotted spelling; the final component
             * is the type name and every preceding component is the module.
             */
            if (check(p, TOK_LBRACE) &&
                !p->suppress_struct_init &&
                expr && expr->type == NODE_FIELD &&
                expr->as.field.dotted_path.length != 0) {
                StringView module_name = string_view_empty();
                StringView type_name = string_view_empty();
                if (split_dotted_leaf(
                        expr->as.field.dotted_path,
                        &module_name,
                        &type_name) &&
                    module_name.length != 0) {
                    expr = finish_struct_init_named(
                        p, module_name, type_name, expr->span);
                }
            }
            continue;
        }

        // indexing: a[b]
        if (match(p, TOK_LBRACKET)) {
            expr = finish_index(p, expr);
            continue;
        }

        if (match(p, TOK_PLUS_PLUS)) {
            Token op = p->previous;
            expr = make_inc_dec(p, expr, TOK_PLUS_PLUS, 0, op.span);
            continue;
        }

        if (match(p, TOK_MINUS_MINUS)) {
            Token op = p->previous;
            expr = make_inc_dec(p, expr, TOK_MINUS_MINUS, 0, op.span);
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
        check(p, TOK_MOVE) ||
        check(p, TOK_AND) ||
        check(p, TOK_STAR)) {
        Token op = p->current;
        advance(p);

        Node *rhs = parse_unary(p);

        if (op.type == TOK_PLUS_PLUS || op.type == TOK_MINUS_MINUS) {
            return make_inc_dec(p, rhs, op.type, 1, op.span);
        }

        return ast_new_unary(
            p->arena,
            op.type,
            rhs,
            source_span_join(op.span, rhs->span)
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
            source_span_join(left->span, right->span)
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
            source_span_join(left->span, right->span)
        );
    }

    return ast_new_compound_assign(
        p->arena,
        op,
        left,
        right,
        source_span_join(left->span, right->span)
    );
}

static Node *parse_assignment(Parser *p) {
    if (match(p, TOK_DISCARD)) {
        Token op = p->previous;
        Node *operand = parse_assignment(p);
        return ast_new_unary(
            p->arena,
            TOK_DISCARD,
            operand,
            source_span_join(op.span, operand->span)
        );
    }

    return parse_assignment_from(p, parse_binary(p, PREC_LOGICAL_OR));
}

// ===================== types =====================

// Parses a type: a base keyword (int/float/char/void/struct Name)
// followed by zero or more '*' for pointer levels. Called wherever a
// type is grammatically expected -- after ':' in a var decl or param,
// or after '->' for a return type. Never called speculatively.
static Type *parse_type(Parser *p)
{
    int has_readonly     = 0;
    int has_volatile     = 0;
    Token readonly_token = {0};
    Token volatile_token = {0};

    /*
     * Pointer qualifiers may appear in either order:
     *
     *     readonly volatile T*
     *     volatile readonly T*
     *
     * They qualify only the first raw-pointer layer, matching the existing
     * `readonly` rule for nested pointers.
     */
    for (;;) {
        if (match(p, TOK_READONLY)) {
            if (has_readonly)
                error_at(p, &p->previous, "duplicate 'readonly' pointer qualifier");
            has_readonly = 1;
            readonly_token = p->previous;
            continue;
        }

        if (match(p, TOK_VOLATILE)) {
            if (has_volatile)
                error_at(p, &p->previous, "duplicate 'volatile' pointer qualifier");
            has_volatile = 1;
            volatile_token = p->previous;
            continue;
        }

        break;
    }

    Type *base = arena_new(p->arena, Type);

    base->array_size = -1;

    int pointer_count = 0;

    if (match(p, TOK_CFN)) {
        /*
         * Native C function-pointer type:
         *
         *     cfn(c_int, opaque*) -> c_int
         *     cfn()                         // returns void
         *
         * Parameter names are intentionally absent in type syntax. The
         * resulting TYPE_FUNCTION is a first-class pointer-sized callback
         * value with FUNCTION_ABI_C.
         */
        base->kind = TYPE_FUNCTION;
        base->function_abi = FUNCTION_ABI_C;
        base->function_call_conv = C_CALL_DEFAULT;

        if (!consume(p, TOK_LPAREN)) {
            return base;
        }

        /*
         * Optional contextual calling-convention option:
         *
         *     cfn(call=win64, c_int) -> c_int
         *     cfn(call=stdcall) -> void
         *
         * `call` remains a valid named type when it is not followed by `=`.
         */
        if (check(p, TOK_IDENT) &&
            token_text_equals(p->current, "call") &&
            peek_next_token_type(p) == TOK_EQUAL) {
            advance(p);
            consume(p, TOK_EQUAL);

            if (!consume(p, TOK_IDENT))
                return base;

            if (!parse_c_calling_convention_token(
                    p, p->previous, &base->function_call_conv)) {
                return base;
            }

            if (!check(p, TOK_RPAREN) && !consume(p, TOK_COMMA))
                return base;
        }

        Type **parameters = NULL;
        int parameter_count = 0;
        int parameter_capacity = 0;

        if (!check(p, TOK_RPAREN)) {
            for (;;) {
                if (match(p, TOK_ELLIPSIS)) {
                    if (parameter_count == 0) {
                        error_at(
                            p,
                            &p->previous,
                            "C variadic function types require at least one fixed parameter"
                        );
                    }

                    base->function_is_variadic = 1;
                    break;
                }

                Type *parameter = parse_type(p);

                if (parameter_count >= parameter_capacity) {
                    int new_capacity = parameter_capacity == 0
                        ? 4
                        : parameter_capacity * 2;
                    Type **grown = arena_alloc(
                        p->arena,
                        sizeof(Type *) * (size_t)new_capacity
                    );

                    if (parameters && parameter_count > 0) {
                        memcpy(
                            grown,
                            parameters,
                            sizeof(Type *) * (size_t)parameter_count
                        );
                    }

                    parameters = grown;
                    parameter_capacity = new_capacity;
                }

                parameters[parameter_count++] = parameter;

                if (!match(p, TOK_COMMA))
                    break;
            }
        }

        consume(p, TOK_RPAREN);

        base->parameters = parameters;
        base->parameter_count = parameter_count;

        if (match(p, TOK_ARROW)) {
            base->return_type = parse_type(p);
        } else {
            base->return_type = make_void_type(p->arena);
        }
    } else if (match(p, TOK_OPAQUE)) {
        /*
         * `opaque*` is a dedicated non-dereferenceable raw pointer kind.
         * There is deliberately no standalone `opaque` value type.
         * Additional stars wrap the opaque pointer normally, so:
         *
         *     opaque*   -> TYPE_OPAQUE_POINTER
         *     opaque**  -> TYPE_POINTER(TYPE_OPAQUE_POINTER)
         *
         * As with ordinary pointers, `readonly` qualifies the first
         * pointer layer and outer pointer layers remain mutable.
         */
        Token opaque_token = p->previous;

        if (!match(p, TOK_STAR)) {
            error_at(
                p,
                &opaque_token,
                "'opaque' must be followed by '*'"
            );

            base->kind = TYPE_VOID;
            return base;
        }

        base->kind = TYPE_OPAQUE_POINTER;
        base->pointer_access = has_readonly
            ? POINTER_ACCESS_READONLY
            : POINTER_ACCESS_MUTABLE;
        base->pointer_is_volatile = has_volatile;
        pointer_count = 1;
    } else if (match(p, TOK_S8)) {
        base->kind = TYPE_S8;
    } else if (match(p, TOK_S16)) {
        base->kind = TYPE_S16;
    } else if (match(p, TOK_S32)) {
        base->kind = TYPE_S32;
    } else if (match(p, TOK_S64) || match(p, TOK_INT_KW)) {
        base->kind = TYPE_S64;
    } else if (match(p, TOK_U8)) {
        base->kind = TYPE_U8;
    } else if (match(p, TOK_U16)) {
        base->kind = TYPE_U16;
    } else if (match(p, TOK_U32)) {
        base->kind = TYPE_U32;
    } else if (match(p, TOK_U64) || match(p, TOK_UINT_KW)) {
        base->kind = TYPE_U64;
    } else if (match(p, TOK_ISIZE) || match(p, TOK_USIZE)) {
        Token alias = p->previous;
        base->kind = TYPE_NAMED;
        base->named_module = string_view_empty();
        base->named_name = string_view(alias.start, (size_t)alias.length);
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
         * Parsed named type reference. Every component before the final type
         * name is the canonical module qualifier:
         *
         *     Point
         *     math.Point
         *     std.math.Point
         */
        ParsedDottedName name = parse_dotted_name_from_first(p, p->previous);
        base->kind = TYPE_NAMED;
        base->named_module = name.prefix;
        base->named_name = name.leaf;

        if (match(p, TOK_COLON_COLON)) {
            TypeList arguments = {0};
            if (parse_explicit_type_arguments(p, &arguments, "generic type")) {
                base->type_arguments = arguments.items;
                base->type_argument_count = arguments.count;
            }
        }
    } else {
        error_at(p, &p->current, "expected type");

        /*
         * Return a safe error-ish type so callers don't read
         * uninitialized memory. Parser error state is already set.
         */
        base->kind = TYPE_VOID;
        return base;
    }

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

        ptr->pointer_is_volatile = has_volatile && pointer_count == 0;

        base = ptr;
        pointer_count++;
    }

    if (match(p, TOK_LBRACKET)) {
        /*
         * Fixed arrays keep the existing suffix spelling `T[N]`.
         * An empty suffix is the first-class non-owning slice type `T[]`.
         *
         * `readonly T[]` removes mutation through slice indexing/data. The
         * qualifier belongs to the slice only when no pointer layer consumed
         * it first, preserving the established nested-pointer qualifier rule.
         */
        if (match(p, TOK_RBRACKET)) {
            Type *slice = arena_new(p->arena, Type);
            slice->kind = TYPE_SLICE;
            slice->element = base;
            slice->array_size = -1;
            slice->pointer_access = has_readonly && pointer_count == 0
                ? POINTER_ACCESS_READONLY
                : POINTER_ACCESS_MUTABLE;

            if (has_volatile && pointer_count == 0) {
                error_at(
                    p,
                    &volatile_token,
                    "'volatile' is not supported on slice types"
                );
            }

            base = slice;
        } else {
            Type *arr = arena_new(p->arena, Type);

            arr->kind = TYPE_ARRAY;
            arr->element = base;
            arr->array_size = -1;

            if (match(p, TOK_NUMBER_INT)) {
                Token size_token = p->previous;
                uint64_t size;

                if (!parse_integer_u64(
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

            consume(p, TOK_RBRACKET);
            base = arr;
        }
    }

    if (has_readonly && pointer_count == 0 && base->kind != TYPE_SLICE) {
        error_at(
            p,
            &readonly_token,
            "'readonly' must qualify a pointer or slice type"
        );
    }

    if (has_volatile && pointer_count == 0 && base->kind != TYPE_SLICE) {
        error_at(
            p,
            &volatile_token,
            "'volatile' must qualify a pointer type"
        );
    }

    return base;
}

// =================== variable declarations ==========================

typedef struct PendingVarName {
    Token name;
    struct PendingVarName *next;
} PendingVarName;

static void pending_var_push(Parser *p, PendingVarName **head, PendingVarName **tail, Token name) {
    PendingVarName *item = arena_alloc(p->arena, sizeof(*item));
    item->name = name;
    item->next = NULL;
    if (*tail)
        (*tail)->next = item;
    else
        *head = item;
    *tail = item;
}

static Node *finish_grouped_typed_var_decl(Parser *p, Token first_name) {
    PendingVarName *head = NULL;
    PendingVarName *tail = NULL;
    pending_var_push(p, &head, &tail, first_name);

    while (match(p, TOK_COMMA)) {
        if (!check(p, TOK_IDENT)) {
            error_at(p, &p->current, "expected variable name after ','");
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        Token name = p->current;
        advance(p);
        pending_var_push(p, &head, &tail, name);
    }

    if (!consume(p, TOK_COLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Type *type = parse_type(p);

    if (match(p, TOK_COLON)) {
        error_at(p, &p->previous, "grouped declarations cannot declare constants");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *initializer = NULL;
    if (match(p, TOK_EQUAL))
        initializer = parse_assignment(p);

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *group = ast_new_var_decl_group(p->arena, first_name.span);
    int index = 0;
    for (PendingVarName *item = head; item; item = item->next, index++) {
        Node *child_init = initializer;
        if (initializer && index > 0)
            child_init = ast_clone(p->arena, initializer);
        Node *decl = ast_new_var_decl(
            p->arena,
            type,
            item->name.start,
            item->name.length,
            child_init,
            item->name.span
        );
        nodelist_push(p->arena, &group->as.var_decl_group.declarations, decl);
    }
    return group;
}

static Node *finish_typed_decl(Parser *p, Token name) {

    SourceSpan span = name.span;
    Type *type = parse_type(p);

    // name : type : expr ;   -- typed constant
    if (match(p, TOK_COLON)) {
        Node *value = parse_assignment(p);

        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        return ast_new_const_decl(p->arena, type, name.start, name.length, value, span);
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

    return ast_new_var_decl(p->arena, type, name.start, name.length, initializer, span);
}

static Node *finish_inferred_const_decl(Parser *p, Token name) {

    SourceSpan span = name.span;
    Node *value = parse_assignment(p);   // '::' always requires a value

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_const_decl(p->arena, NULL, name.start, name.length, value, span);
}

static Node *finish_inferred_var_decl(Parser *p, Token name) {

    SourceSpan span = name.span;
    Node *initializer = parse_assignment(p);   // ':=' always requires a value

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_var_decl(p->arena, NULL, name.start, name.length, initializer, span);
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

    int is_pack = match(p, TOK_ELLIPSIS);
    Type *type = parse_type(p);

    if (is_pack && head && head->next) {
        error_at(p, &p->previous, "variadic generic parameter groups may contain only one name");
        goto cleanup;
    }

    if (match(p, TOK_EQUAL)) {
        error_at(p, &p->previous, "default parameters are not supported");
        /* Consume the old parser-only spelling so one unsupported feature does
         * not cascade into unrelated function-body diagnostics. */
        (void)parse_assignment(p);
    }

    for (PendingParamName *it = head; it; it = it->next) {
        Node *param = ast_new_func_param_decl(
            p->arena,
            type,
            it->name.start,
            it->name.length,
            it->name.span
        );

        param->as.param_decl.is_pack = is_pack;
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

static Node *parse_function_signature_rest(Parser *p, Token name, SourceSpan span) {

    if (!consume(p, TOK_LPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *func = ast_new_func_decl(p->arena, name.start, name.length, NULL, span);

    if (!check(p, TOK_RPAREN)) {
        for (;;) {
            if (match(p, TOK_ELLIPSIS)) {
                if (func->as.func_decl.params.count == 0) {
                    error_at(
                        p,
                        &p->previous,
                        "C variadic declarations require at least one fixed parameter"
                    );
                }

                func->as.func_decl.is_variadic = 1;
                break;
            }

            if (!parse_parameter_group(p, func)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            if (!match(p, TOK_COMMA))
                break;
        }
    }

    if (!consume(p, TOK_RPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (match(p, TOK_ARROW)) {
        func->as.func_decl.return_type = parse_type(p);
    } else {
        func->as.func_decl.return_type = make_void_type(p->arena);
    }

    return func;
}

static int parse_generic_type_parameter_list_after_less(
    Parser *p,
    GenericTypeParameterList *out,
    const char *subject
) {
    assert(out);
    *out = (GenericTypeParameterList){0};

    if (check(p, TOK_GREATER)) {
        char message[128];
        snprintf(message, sizeof(message), "%s requires at least one type parameter", subject);
        error_at(p, &p->current, message);
        advance(p);
        return 0;
    }

    for (;;) {
        int is_pack = match(p, TOK_ELLIPSIS);
        if (!consume(p, TOK_IDENT)) {
            synchronize(p);
            return 0;
        }
        Token parameter = p->previous;
        GenericTypeParameter type_parameter = {
            .name = string_view(parameter.start, (size_t)parameter.length),
            .constraint = string_view_empty(),
            .is_pack = is_pack,
        };

        if (match(p, TOK_COLON)) {
            if (!consume(p, TOK_IDENT)) {
                synchronize(p);
                return 0;
            }
            Token constraint = p->previous;
            type_parameter.constraint =
                string_view(constraint.start, (size_t)constraint.length);
        }

        generic_type_parameter_list_push(p->arena, out, type_parameter);
        if (type_parameter.is_pack) {
            if (match(p, TOK_COMMA)) {
                error_at(p, &p->previous, "variadic generic type parameter must be last");
                while (!check(p, TOK_GREATER) && !check(p, TOK_EOF))
                    advance(p);
                if (check(p, TOK_GREATER))
                    advance(p);
                return 0;
            }
            break;
        }
        if (!match(p, TOK_COMMA))
            break;
    }

    if (!consume_generic_greater(p, "expected '>' after generic type parameters")) {
        synchronize(p);
        return 0;
    }
    return 1;
}

static Node *parse_generic_decl_rest(Parser *p, Token name, SourceSpan span)
{
    if (!consume(p, TOK_LESS))
        return ast_new_error(p->arena, p->current);

    GenericTypeParameterList parameters = {0};
    if (!parse_generic_type_parameter_list_after_less(
            p, &parameters, "generic declaration")) {
        return ast_new_error(p->arena, p->previous);
    }

    if (check(p, TOK_LPAREN)) {
        Node *func = parse_function_signature_rest(p, name, span);
        if (!func || func->type == NODE_ERROR)
            return func;

        func->as.func_decl.type_parameters = parameters;
        if (!check(p, TOK_LBRACE)) {
            error_at(p, &p->current, "expected function body");
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        func->as.func_decl.body = parse_block(p);
        return func;
    }

    if (check(p, TOK_STRUCT) || check(p, TOK_RESOURCE)) {
        Node *decl = parse_struct_decl_rest(p, name, span);
        if (decl && decl->type == NODE_STRUCT_DECL)
            decl->as.struct_decl.type_parameters = parameters;
        return decl;
    }

    if (check(p, TOK_UNION)) {
        error_at(p, &p->current, "generic unions are not supported");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    error_at(p, &p->current, "generic declaration must be a function or struct");
    synchronize(p);
    return ast_new_error(p->arena, p->current);
}

static Node *parse_proc_decl_rest(Parser *p, Token name, SourceSpan span) {

    Node *func = parse_function_signature_rest(p, name, span);

    if (!func || func->type == NODE_ERROR)
        return func;

    if (!check(p, TOK_LBRACE)) {
        error_at(p, &p->current, "expected function body");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    func->as.func_decl.body = parse_block(p);

    return func;
}

static ParsedDottedName parse_dotted_name_from_first(Parser *p, Token first)
{
    ParsedDottedName result;
    memset(&result, 0, sizeof(result));

    result.full.data = first.start;
    result.full.length = (size_t)first.length;
    result.leaf = result.full;
    result.span = first.span;
    result.component_count = 1;

    Token last = first;
    while (match(p, TOK_DOT)) {
        if (!consume(p, TOK_IDENT))
            break;

        Token part = p->previous;
        size_t total = result.full.length + 1 + (size_t)part.length;
        char *joined = arena_alloc(p->arena, total);
        memcpy(joined, result.full.data, result.full.length);
        joined[result.full.length] = '.';
        memcpy(joined + result.full.length + 1, part.start, (size_t)part.length);

        result.full.data = joined;
        result.full.length = total;
        result.leaf.data = joined + total - (size_t)part.length;
        result.leaf.length = (size_t)part.length;
        result.component_count++;
        last = part;
    }

    result.span = source_span_join(first.span, last.span);
    if (result.component_count > 1) {
        result.prefix.data = result.full.data;
        result.prefix.length = result.full.length - result.leaf.length - 1;
    } else {
        result.prefix = string_view_empty();
    }

    return result;
}

static int split_dotted_leaf(
    StringView full,
    StringView *out_prefix,
    StringView *out_leaf
) {
    if (!full.data || full.length == 0)
        return 0;

    for (size_t i = full.length; i > 0; --i) {
        if (full.data[i - 1] != '.')
            continue;

        if (i == 1 || i == full.length)
            return 0;

        if (out_prefix) {
            out_prefix->data = full.data;
            out_prefix->length = i - 1;
        }
        if (out_leaf) {
            out_leaf->data = full.data + i;
            out_leaf->length = full.length - i;
        }
        return 1;
    }

    if (out_prefix)
        *out_prefix = string_view_empty();
    if (out_leaf)
        *out_leaf = full;
    return 1;
}

static int token_text_equals(Token token, const char *text)
{
    size_t length = strlen(text);

    return token.type == TOK_IDENT &&
           token.length == (int)length &&
           memcmp(token.start, text, length) == 0;
}

static int parse_c_calling_convention_token(
    Parser *p,
    Token token,
    CCallingConvention *out
)
{
    if (token_text_equals(token, "cdecl")) {
        *out = C_CALL_CDECL;
        return 1;
    }

    if (token_text_equals(token, "stdcall")) {
        *out = C_CALL_STDCALL;
        return 1;
    }

    if (token_text_equals(token, "sysv64")) {
        *out = C_CALL_SYSV64;
        return 1;
    }

    if (token_text_equals(token, "win64")) {
        *out = C_CALL_WIN64;
        return 1;
    }

    error_at(
        p,
        &token,
        "unsupported C calling convention; expected 'cdecl', 'stdcall', 'sysv64', or 'win64'"
    );
    return 0;
}

/*
 * Parses the first ABI declaration form:
 *
 *     #extern(c)
 *     puts::(s: readonly u8*) -> s32;
 *
 * `extern` and `c` intentionally remain identifiers rather than globally
 * reserved keywords. The `#` introduces declaration metadata.
 */
static int decode_extern_name(Parser *p, Token token, StringView *out)
{
    assert(token.type == TOK_STRING);

    StringView raw = string_view(token.start + 1, (size_t)token.length - 2);
    StringDecodeInfo info = string_analyze(raw);

    if (!info.ok) {
        if (info.invalid_escape) {
            error_at(p, &token, "invalid escape sequence in extern symbol name");
        } else {
            error_at(p, &token, "unterminated escape sequence in extern symbol name");
        }
        return 0;
    }

    if (info.decoded_length == 0) {
        error_at(p, &token, "extern symbol name cannot be empty");
        return 0;
    }

    char *decoded = arena_alloc(p->arena, (size_t)info.decoded_length + 1);
    string_decode_into(raw, decoded);
    decoded[info.decoded_length] = '\0';

    for (int i = 0; i < info.decoded_length; i++) {
        if (decoded[i] == '\0') {
            error_at(p, &token, "extern symbol name cannot contain NUL");
            return 0;
        }
    }

    *out = string_view(decoded, (size_t)info.decoded_length);
    return 1;
}

/*
 * Parses C ABI declarations:
 *
 *     #extern(c)
 *     puts::(s: readonly c_char*) -> c_int;
 *
 *     #extern(c, name="SDL_CreateWindow")
 *     create_window::(...) -> opaque*;
 *
 * `extern`, `c`, and option names intentionally remain identifiers rather
 * than globally reserved keywords. Empty external_name means the Coglet
 * declaration name is also the linker symbol.
 */
static Node *parse_attribute_decl(Parser *p)
{
    StringView external_name = string_view_empty();
    CCallingConvention extern_call_conv = C_CALL_DEFAULT;
    int saw_name = 0;
    int saw_extern_call = 0;

    if (!consume(p, TOK_HASH))
        return ast_new_error(p->arena, p->current);

    if (!consume(p, TOK_IDENT)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Token attribute = p->previous;

    if (token_text_equals(attribute, "discardable")) {
        Node *decl = NULL;

        /*
         * #discardable is declaration policy rather than ABI metadata. Keep
         * it outermost when combined with another declaration attribute:
         *
         *     #discardable
         *     #extern(c)
         *     poll::() -> c_int;
         */
        if (check(p, TOK_HASH)) {
            decl = parse_attribute_decl(p);
        } else {
            if (!consume(p, TOK_IDENT)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            Token name = p->previous;
            if (!consume(p, TOK_COLON_COLON)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            decl = parse_decl_after_name(p, name);
        }

        if (!decl || decl->type == NODE_ERROR)
            return decl;

        if (decl->type != NODE_FUNC_DECL) {
            error_at(p, &attribute, "#discardable applies only to function declarations");
            return ast_new_error(p->arena, attribute);
        }

        if (decl->as.func_decl.is_discardable) {
            error_at(p, &attribute, "duplicate #discardable attribute");
            return ast_new_error(p->arena, attribute);
        }

        decl->as.func_decl.is_discardable = 1;
        return decl;
    }

    if (token_text_equals(attribute, "repr")) {
        int repr_packed = 0;
        int repr_align = 0;
        int saw_packed = 0;
        int saw_align = 0;
        int saw_layout_option = 0;
        int saw_call = 0;
        CCallingConvention repr_call_conv = C_CALL_DEFAULT;

        if (!consume(p, TOK_LPAREN) || !consume(p, TOK_IDENT)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Token abi = p->previous;
        if (!token_text_equals(abi, "c")) {
            error_at(p, &abi, "unsupported representation ABI; expected 'c'");
            synchronize(p);
            return ast_new_error(p->arena, abi);
        }

        while (match(p, TOK_COMMA)) {
            if (!consume(p, TOK_IDENT)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            Token option = p->previous;

            if (token_text_equals(option, "packed")) {
                saw_layout_option = 1;
                if (saw_packed) {
                    error_at(p, &option, "duplicate #repr(c) option 'packed'");
                    synchronize(p);
                    return ast_new_error(p->arena, option);
                }

                saw_packed = 1;
                repr_packed = 1;
                continue;
            }

            if (token_text_equals(option, "align")) {
                saw_layout_option = 1;
                if (saw_align) {
                    error_at(p, &option, "duplicate #repr(c) option 'align'");
                    synchronize(p);
                    return ast_new_error(p->arena, option);
                }

                if (!consume(p, TOK_EQUAL) || !consume(p, TOK_NUMBER_INT)) {
                    synchronize(p);
                    return ast_new_error(p->arena, p->current);
                }

                Token align_token = p->previous;
                uint64_t align_value = 0;

                if (!parse_integer_u64(align_token, &align_value) ||
                    align_value > INT_MAX) {
                    error_at(p, &align_token, "#repr(c) alignment exceeds compiler limit");
                    synchronize(p);
                    return ast_new_error(p->arena, align_token);
                }

                if (align_value == 0) {
                    error_at(p, &align_token, "#repr(c) alignment must be greater than zero");
                    synchronize(p);
                    return ast_new_error(p->arena, align_token);
                }

                saw_align = 1;
                repr_align = (int)align_value;
                continue;
            }

            if (token_text_equals(option, "call")) {
                if (saw_call) {
                    error_at(p, &option, "duplicate #repr(c) option 'call'");
                    synchronize(p);
                    return ast_new_error(p->arena, option);
                }

                if (!consume(p, TOK_EQUAL) || !consume(p, TOK_IDENT)) {
                    synchronize(p);
                    return ast_new_error(p->arena, p->current);
                }

                if (!parse_c_calling_convention_token(
                        p, p->previous, &repr_call_conv)) {
                    synchronize(p);
                    return ast_new_error(p->arena, p->previous);
                }

                saw_call = 1;
                continue;
            }

            error_at(p, &option, "unknown #repr(c) option; expected 'packed', 'align', or 'call'");
            synchronize(p);
            return ast_new_error(p->arena, option);
        }

        if (!consume(p, TOK_RPAREN) || !consume(p, TOK_IDENT)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Token name = p->previous;
        if (!consume(p, TOK_COLON_COLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Node *decl = NULL;

        if (check(p, TOK_STRUCT) || check(p, TOK_UNION)) {
            if (saw_call) {
                error_at(
                    p,
                    &p->current,
                    "#repr(c) option 'call' applies only to function declarations"
                );
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            decl = parse_struct_decl_rest(p, name, name.span);
            if (decl && decl->type == NODE_STRUCT_DECL) {
                decl->as.struct_decl.is_repr_c = 1;
                decl->as.struct_decl.repr_c_packed = repr_packed;
                decl->as.struct_decl.repr_c_align = repr_align;
            }
            return decl;
        }

        if (saw_layout_option) {
            error_at(
                p,
                &p->current,
                "#repr(c) layout options apply only to struct or union declarations"
            );
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        if (saw_call && !check(p, TOK_LPAREN)) {
            error_at(
                p,
                &p->current,
                "#repr(c) option 'call' applies only to function declarations"
            );
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        if (check(p, TOK_ENUM)) {
            decl = parse_enum_decl_rest(p, name, name.span);
            if (decl && decl->type == NODE_ENUM_DECL)
                decl->as.enum_decl.is_repr_c = 1;
            return decl;
        }

        if (check(p, TOK_LPAREN)) {
            decl = parse_proc_decl_rest(p, name, name.span);
            if (decl && decl->type == NODE_FUNC_DECL) {
                decl->as.func_decl.is_repr_c = 1;
                decl->as.func_decl.c_call_conv = repr_call_conv;
            }
            return decl;
        }

        error_at(p, &p->current, "#repr(c) applies only to struct, union, enum, or function declarations");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (!token_text_equals(attribute, "extern")) {
        error_at(p, &attribute, "unknown declaration attribute; expected '#extern(c)', '#repr(c)', or '#discardable'");
        synchronize(p);
        return ast_new_error(p->arena, attribute);
    }

    if (!consume(p, TOK_LPAREN) || !consume(p, TOK_IDENT)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Token abi = p->previous;

    if (!token_text_equals(abi, "c")) {
        error_at(p, &abi, "unsupported extern ABI; expected 'c'");
        synchronize(p);
        return ast_new_error(p->arena, abi);
    }

    while (match(p, TOK_COMMA)) {
        if (!consume(p, TOK_IDENT)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Token option = p->previous;

        if (token_text_equals(option, "name")) {
            if (saw_name) {
                error_at(p, &option, "duplicate #extern(c) option 'name'");
                synchronize(p);
                return ast_new_error(p->arena, option);
            }

            if (!consume(p, TOK_EQUAL) || !consume(p, TOK_STRING)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            if (!decode_extern_name(p, p->previous, &external_name)) {
                synchronize(p);
                return ast_new_error(p->arena, p->previous);
            }

            saw_name = 1;
            continue;
        }

        if (token_text_equals(option, "call")) {
            if (saw_extern_call) {
                error_at(p, &option, "duplicate #extern(c) option 'call'");
                synchronize(p);
                return ast_new_error(p->arena, option);
            }

            if (!consume(p, TOK_EQUAL) || !consume(p, TOK_IDENT)) {
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }

            if (!parse_c_calling_convention_token(
                    p, p->previous, &extern_call_conv)) {
                synchronize(p);
                return ast_new_error(p->arena, p->previous);
            }

            saw_extern_call = 1;
            continue;
        }

        error_at(p, &option, "unknown #extern(c) option; expected 'name' or 'call'");
        synchronize(p);
        return ast_new_error(p->arena, option);
    }

    if (!consume(p, TOK_RPAREN) || !consume(p, TOK_IDENT)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Token name = p->previous;

    if (!consume(p, TOK_COLON_COLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (!check(p, TOK_LPAREN)) {
        error_at(p, &p->current, "#extern(c) applies only to function declarations");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *func = parse_function_signature_rest(p, name, name.span);

    if (!func || func->type == NODE_ERROR)
        return func;

    func->as.func_decl.linkage = FUNCTION_LINKAGE_EXTERN_C;
    func->as.func_decl.external_name = external_name;
    func->as.func_decl.c_call_conv = extern_call_conv;

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return func;
}

// ================== end proc/function parsing ===================

// =================== struct declarations ========================
static void struct_operator_list_push(
    Parser *p,
    StructOperatorDeclList *list,
    StructOperatorDecl value
) {
    if (list->count >= list->capacity) {
        int new_capacity = list->capacity ? list->capacity * 2 : 4;
        StructOperatorDecl *items = arena_alloc(
            p->arena,
            sizeof(*items) * (size_t)new_capacity
        );
        if (list->items && list->count > 0) {
            memcpy(items, list->items, sizeof(*items) * (size_t)list->count);
        }
        list->items = items;
        list->capacity = new_capacity;
    }
    list->items[list->count++] = value;
}

static int parse_struct_operator_block(Parser *p, Node *decl)
{
    assert(decl && decl->type == NODE_STRUCT_DECL);
    if (!consume(p, TOK_LBRACE))
        return 0;

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        int is_unary = 0;
        if (check(p, TOK_IDENT) && token_text_equals(p->current, "unary")) {
            advance(p);
            is_unary = 1;
        }

        Token op_token = p->current;
        TokenType op = op_token.type;
        int valid_binary = op == TOK_PLUS || op == TOK_MINUS ||
            op == TOK_STAR || op == TOK_SLASH;
        int valid_unary = op == TOK_MINUS;
        if ((is_unary && !valid_unary) || (!is_unary && !valid_binary)) {
            error_at(
                p,
                &p->current,
                is_unary
                    ? "expected unary '-' operator mapping"
                    : "expected '+', '-', '*', or '/' operator mapping"
            );
            synchronize(p);
            return 0;
        }
        advance(p);

        if (!consume(p, TOK_EQUAL)) {
            synchronize(p);
            return 0;
        }
        if (!consume(p, TOK_IDENT)) {
            synchronize(p);
            return 0;
        }
        Token method = p->previous;
        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return 0;
        }

        struct_operator_list_push(
            p,
            &decl->as.struct_decl.operators,
            (StructOperatorDecl){
                .op = op,
                .is_unary = is_unary,
                .method_name = string_view(method.start, (size_t)method.length),
                .span = op_token.span,
            }
        );
    }

    return consume(p, TOK_RBRACE);
}

static Node *parse_struct_decl_rest(Parser *p,Token name,SourceSpan span) {

    int is_union = check(p, TOK_UNION);
    int is_resource = check(p, TOK_RESOURCE);

    if (is_union)
        consume(p, TOK_UNION);
    else if (is_resource)
        consume(p, TOK_RESOURCE);
    else
        consume(p, TOK_STRUCT);

    Node *decl = ast_new_struct_decl(p->arena, name.start,name.length, span);
    decl->as.struct_decl.is_union = is_union;
    decl->as.struct_decl.is_resource = is_resource;

    /*
     * A semicolon spells an incomplete named struct. Semantic analysis
     * restricts this form to #repr(c), where it models C APIs that expose
     * only `struct T *` handles without publishing T's layout.
     */
    if (match(p, TOK_SEMICOLON)) {
        decl->as.struct_decl.is_incomplete = 1;
        return decl;
    }

    consume(p, TOK_LBRACE);

    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_HASH)) {
            Node *method = parse_attribute_decl(p);
            if (!method || method->type == NODE_ERROR)
                return method;
            if (method->type != NODE_FUNC_DECL) {
                error_at(p, &p->previous, "struct attributes may only introduce methods");
                return ast_new_error(p->arena, p->previous);
            }
            nodelist_push(p->arena, &decl->as.struct_decl.methods, method);
            continue;
        }

        if (!consume(p, TOK_IDENT)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Token member = p->previous;

        if (token_text_equals(member, "operators") && check(p, TOK_LBRACE)) {
            if (!parse_struct_operator_block(p, decl))
                return ast_new_error(p->arena, p->current);
            continue;
        }

        if (match(p, TOK_COLON)) {
            Type *type = parse_type(p);
            consume(p, TOK_SEMICOLON);
            Node *field = ast_new_struct_field_decl(
                p->arena, type, member.start, member.length, member.span);
            nodelist_push(p->arena, &decl->as.struct_decl.fields, field);
            continue;
        }

        if (match(p, TOK_COLON_COLON)) {
            GenericTypeParameterList method_type_parameters = {0};
            if (match(p, TOK_LESS)) {
                if (!parse_generic_type_parameter_list_after_less(
                        p,
                        &method_type_parameters,
                        "generic method")) {
                    return ast_new_error(p->arena, p->previous);
                }
            }
            if (!check(p, TOK_LPAREN)) {
                error_at(p, &p->current,
                    "struct member declaration must be a field or function");
                synchronize(p);
                return ast_new_error(p->arena, p->current);
            }
            Node *method = parse_proc_decl_rest(p, member, member.span);
            if (method && method->type == NODE_FUNC_DECL)
                method->as.func_decl.type_parameters = method_type_parameters;
            nodelist_push(p->arena, &decl->as.struct_decl.methods, method);
            continue;
        }

        error_at(p, &p->current,
            "expected ':' for a field or '::' for a method");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    consume(p, TOK_RBRACE);

    return decl;
}

// Struct initializer: `Point{ x = 5, y = 10 }` (trailing comma allowed).
static Node *finish_struct_init_named(
    Parser *p,
    StringView module_name,
    StringView type_name,
    SourceSpan span
) {
    consume(p, TOK_LBRACE); // known present from caller's check()

    Node *init = ast_new_struct_init(
        p->arena, type_name.data, (int)type_name.length, span);
    init->as.struct_init.module_name = module_name;

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
                field_name.span
            );

            nodelist_push(p->arena, &init->as.struct_init.fields, field);

            if (!match(p, TOK_COMMA)) break;
            if (check(p, TOK_RBRACE)) break; // trailing comma
        }
    }

    consume(p, TOK_RBRACE);
    return init;
}

// `type_name` is the identifier already consumed by the caller.
static Node *finish_struct_init(Parser *p, Token type_name)
{
    StringView name = { type_name.start, (size_t)type_name.length };
    return finish_struct_init_named(
        p, string_view_empty(), name, type_name.span);
}

// ====================== end struct declarations ======================
// ====================== enum declarations ===========================
static Node *parse_enum_decl_rest(Parser *p, Token name, SourceSpan span) {
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
        span
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
        name.span
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
            keyword.span
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
            keyword.span
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
            keyword.span
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

    SourceSpan span = name.span;

    if (check(p, TOK_LESS))  return parse_generic_decl_rest(p, name, span);
    if (check(p, TOK_LPAREN)) return parse_proc_decl_rest(p, name, span);
    if (check(p, TOK_STRUCT) || check(p, TOK_RESOURCE) || check(p, TOK_UNION))
        return parse_struct_decl_rest(p, name, span);
    if (check(p, TOK_ENUM))   return parse_enum_decl_rest(p, name, span);

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

        if (check(p, TOK_COMMA)) {
            return finish_grouped_typed_var_decl(p, name);
        }

        if (match(p, TOK_COLON_EQUAL)) {
            return finish_inferred_var_decl(p, name);
        }

        if (match(p, TOK_COLON_COLON)) {
            if (check(p, TOK_LESS) && generic_suffix_starts_expression(p)) {
                Node *base = ast_new_ident(
                    p->arena,
                    name.start,
                    name.length,
                    name.span
                );
                base = finish_generic_call(p, base);
                Node *postfixed = parse_postfix_from(p, base);
                Node *binary = parse_binary_from(p, postfixed, PREC_LOGICAL_OR);
                Node *full = parse_assignment_from(p, binary);
                if (!consume(p, TOK_SEMICOLON)) {
                    synchronize(p);
                    return ast_new_error(p->arena, p->current);
                }
                return ast_new_expr_stmt(p->arena, full, name.span);
            }
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
                    name.span
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
            name.span
        );
    }

    return parse_expr_statement(p);
}

// ======================= end declaration dispatching ==================

/* Parsing is context-free here; semantic analysis diagnoses `return` outside
 * a function so recovery and source provenance stay centralized there. */
static Node *parse_return_statement(Parser *p) {

    SourceSpan span = p->previous.span;   // TOK_RETURN already consumed by caller

    Node *value = NULL;
    if (!check(p, TOK_SEMICOLON))
        value = parse_expression(p);

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_return(p->arena, value, span);
}

static Node *parse_asm_statement(Parser *p)
{
    SourceSpan span = p->previous.span;
    int is_volatile = match(p, TOK_VOLATILE);

    if (!consume(p, TOK_LPAREN))
        return ast_new_error(p->arena, p->current);

    if (!consume(p, TOK_STRING)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }
    Token template_token = p->previous;
    StringView template_text = string_view(
        template_token.start + 1,
        (size_t)template_token.length - 2
    );

    if (!consume(p, TOK_COLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (!consume(p, TOK_STRING)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }
    Token output_constraint_token = p->previous;
    StringView output_constraint = string_view(
        output_constraint_token.start + 1,
        (size_t)output_constraint_token.length - 2
    );

    if (!consume(p, TOK_LPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }
    Node *output = parse_assignment(p);
    if (!output || !consume(p, TOK_RPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    StringView input_constraint = {0};
    Node *input = NULL;

    if (match(p, TOK_COLON)) {
        if (!consume(p, TOK_STRING)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        Token input_constraint_token = p->previous;
        input_constraint = string_view(
            input_constraint_token.start + 1,
            (size_t)input_constraint_token.length - 2
        );

        if (!consume(p, TOK_LPAREN)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        input = parse_assignment(p);
        if (!input || !consume(p, TOK_RPAREN)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
    }

    if (!consume(p, TOK_RPAREN) || !consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_asm(
        p->arena,
        template_text,
        output_constraint,
        output,
        input_constraint,
        input,
        is_volatile,
        source_span_join(span, p->previous.span)
    );
}

static Node *parse_static_assert_statement(Parser *p) {
    SourceSpan span = p->previous.span; /* TOK_STATIC_ASSERT already consumed */

    if (!consume(p, TOK_LPAREN)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    if (check(p, TOK_RPAREN) || check(p, TOK_COMMA)) {
        error_at(p, &p->current, "static_assert requires a condition expression");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *condition = parse_expression(p);
    Node *message = NULL;

    if (match(p, TOK_COMMA)) {
        if (!consume(p, TOK_STRING)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }

        Token t = p->previous;
        message = ast_new_string(
            p->arena,
            t.start + 1,
            t.length - 2,
            t.span
        );
    }

    if (!consume(p, TOK_RPAREN) || !consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_static_assert(
        p->arena,
        condition,
        message,
        source_span_join(span, p->previous.span)
    );
}

static Node *parse_defer_statement(Parser *p) {
    SourceSpan span = p->previous.span; /* TOK_DEFER already consumed */

    Node *statement = NULL;
    if (check(p, TOK_LBRACE)) {
        statement = parse_block(p);
    } else {
        Node *expr = parse_expression(p);
        if (!consume(p, TOK_SEMICOLON)) {
            synchronize(p);
            return ast_new_error(p->arena, p->current);
        }
        statement = ast_new_expr_stmt(p->arena, expr, expr ? expr->span : span);
    }

    return ast_new_defer(
        p->arena,
        statement,
        statement ? source_span_join(span, statement->span) : span
    );
}

static Node *parse_scoped_control_body(Parser *p)
{
    Node *statement = parse_statement(p);

    /*
     * An unbraced control-flow body is still a lexical scope. Normalizing it
     * into a block keeps scope/flow/lowering semantics identical to the braced
     * spelling instead of teaching later phases about two body forms.
     */
    if (statement->type == NODE_BLOCK)
        return statement;

    Node *block = ast_new_block(p->arena, statement->span);
    nodelist_push(p->arena, &block->as.block.statements, statement);
    return block;
}

static Node *parse_while_statement(Parser *p) {

    SourceSpan span = p->previous.span;

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 1;
    Node *cond = parse_expression(p);
    p->suppress_struct_init = saved;

    Node *body = parse_scoped_control_body(p);
    return ast_new_while(p->arena, cond, body, source_span_join(span, body->span));
}

static int parenthesized_for_has_top_level_semicolon(Parser *p)
{
    Lexer lexer = p->lexer;
    Token token = p->current;
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;

    for (;;) {
        switch (token.type) {
            case TOK_EOF:
                return 0;
            case TOK_LPAREN:
                paren_depth++;
                break;
            case TOK_RPAREN:
                if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0)
                    return 0;
                if (paren_depth > 0)
                    paren_depth--;
                break;
            case TOK_LBRACKET:
                bracket_depth++;
                break;
            case TOK_RBRACKET:
                if (bracket_depth > 0)
                    bracket_depth--;
                break;
            case TOK_LBRACE:
                brace_depth++;
                break;
            case TOK_RBRACE:
                if (brace_depth > 0)
                    brace_depth--;
                break;
            case TOK_SEMICOLON:
                if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0)
                    return 1;
                break;
            default:
                break;
        }

        token = lexer_next(&lexer);
    }
}

static int for_initializer_is_allowed(Node *node)
{
    return node &&
        (node->type == NODE_VAR_DECL ||
         node->type == NODE_VAR_DECL_GROUP ||
         node->type == NODE_CONST_DECL ||
         node->type == NODE_EXPR_STMT);
}

static Node *parse_c_style_for_statement(Parser *p, SourceSpan span)
{
    Node *init = NULL;
    Node *cond = NULL;
    Node *post = NULL;

    if (match(p, TOK_SEMICOLON)) {
        /* Empty initializer. */
    } else {
        Token init_start = p->current;
        init = parse_decl_or_expr_statement(p);
        if (init->type != NODE_ERROR && !for_initializer_is_allowed(init)) {
            error_at(p, &init_start,
                "for initializer must be a local value declaration or expression statement");
        }
    }

    if (!check(p, TOK_SEMICOLON))
        cond = parse_expression(p);

    if (!consume(p, TOK_SEMICOLON))
        return ast_new_error(p->arena, p->current);

    if (!check(p, TOK_RPAREN))
        post = parse_expression(p);

    if (!consume(p, TOK_RPAREN))
        return ast_new_error(p->arena, p->current);

    Node *body = parse_scoped_control_body(p);
    SourceSpan loop_span = source_span_join(span, body->span);
    Node *loop = ast_new_for(p->arena, cond, post, body, loop_span);

    if (!init)
        return loop;

    /*
     * The initializer's lifetime covers the condition, body, and post clause
     * but ends with the loop. A synthetic block expresses that using Coglet's
     * existing lexical-scope invariant and requires no special CogIR loop form.
     */
    Node *scope = ast_new_block(p->arena, loop_span);
    nodelist_push(p->arena, &scope->as.block.statements, init);
    nodelist_push(p->arena, &scope->as.block.statements, loop);
    return scope;
}

static Node *parse_for_statement(Parser *p) {

    SourceSpan span = p->previous.span;
    Node *cond = NULL;
    Node *post = NULL;

    if (match(p, TOK_LPAREN)) {
        if (check(p, TOK_IDENT) && peek_next_token_type(p) == TOK_IN) {
            Token item = p->current;
            advance(p);
            advance(p); /* in */
            if (!consume(p, TOK_IDENT)) {
                error_at(p, &p->current, "expected generic pack name after 'in'");
                return ast_new_error(p->arena, p->current);
            }
            Token pack = p->previous;
            if (!consume(p, TOK_RPAREN))
                return ast_new_error(p->arena, p->current);
            Node *body = parse_scoped_control_body(p);
            return ast_new_pack_for(
                p->arena,
                string_view(item.start, (size_t)item.length),
                ast_new_ident(p->arena, pack.start, pack.length, pack.span),
                body,
                source_span_join(span, body->span));
        }

        int c_style = parenthesized_for_has_top_level_semicolon(p);

        if (c_style)
            return parse_c_style_for_statement(p, span);

        int saved = p->suppress_struct_init;
        p->suppress_struct_init = 1;

        if (!check(p, TOK_RPAREN)) {
            cond = parse_expression(p);
            if (match(p, TOK_COLON))
                post = parse_expression(p);
        }

        p->suppress_struct_init = saved;

        if (!consume(p, TOK_RPAREN))
            return ast_new_error(p->arena, p->current);
    } else if (!check(p, TOK_LBRACE)) {
        int saved = p->suppress_struct_init;
        p->suppress_struct_init = 1;

        cond = parse_expression(p);

        if (match(p, TOK_COLON))
            post = parse_expression(p);

        p->suppress_struct_init = saved;
    }

    Node *body = parse_scoped_control_body(p);

    return ast_new_for(p->arena, cond, post, body, source_span_join(span, body->span));
}

static Node *parse_statement(Parser *p) {

    if (check(p, TOK_HASH))   return parse_attribute_decl(p);
    if (match(p, TOK_IF))     return parse_if_statement(p);
    if (match(p, TOK_WHILE))  return parse_while_statement(p);
    if (match(p, TOK_FOR))    return parse_for_statement(p);
    if (match(p, TOK_RETURN)) return parse_return_statement(p);
    if (match(p, TOK_DEFER))  return parse_defer_statement(p);
    if (match(p, TOK_STATIC_ASSERT)) return parse_static_assert_statement(p);
    if (match(p, TOK_ASM)) return parse_asm_statement(p);
    if (check(p, TOK_SWITCH)) return parse_switch_statement(p);

    if (match(p, TOK_BREAK)) {
        SourceSpan span = p->previous.span;
        if (!consume(p, TOK_SEMICOLON)) { synchronize(p); return ast_new_error(p->arena, p->current); }
        return ast_new_break(p->arena, span);
    }

    if (match(p, TOK_CONTINUE)) {
        SourceSpan span = p->previous.span;
        if (!consume(p, TOK_SEMICOLON)) { synchronize(p); return ast_new_error(p->arena, p->current); }
        return ast_new_continue(p->arena, span);
    }

    if (check(p, TOK_LBRACE)) return parse_block(p);

    return parse_decl_or_expr_statement(p);
}

static Node *parse_block(Parser *p) {

    if (!consume(p, TOK_LBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Node *block = ast_new_block(p->arena, p->previous.span);

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

        case TOK_REINTERPRET:
            kind = CAST_REINTERPRET;
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
        source_span_join(keyword.span, expression->span)
    );
}

static Node *parse_zero_array_initializer(Parser *p) {

    Token open = p->previous;

    if (!match(p, TOK_NUMBER_INT)) {
        error_at(p, &p->current, "zero initializer must be spelled '{0}'");
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    Token zero = p->previous;
    if (zero.length != 1 || zero.start[0] != '0') {
        error_at(p, &zero, "zero initializer must be spelled '{0}'");
    }

    if (!consume(p, TOK_RBRACE)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_zero_array_initializer(
        p->arena,
        source_span_join(open.span, p->previous.span)
    );
}

static Node *parse_array_literal(Parser *p) {

    Token open = p->previous;

    Node *array = ast_new_array_literal(p->arena, open.span);

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

/*
 * Returns the numeric value of an ASCII integer digit, or -1 when
 * the character is not a supported digit.
 *
 * The lexer has already validated the spelling, but keeping this
 * conversion defensive prevents this parser helper from silently
 * accepting malformed tokens.
 */
static int integer_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return 10 + c - 'a';

    if (c >= 'A' && c <= 'F')
        return 10 + c - 'A';

    return -1;
}

/*
 * Converts a syntactically valid integer token into its exact u64
 * magnitude.
 *
 * Supported forms:
 *
 *     123
 *     0xff
 *     0b1010
 *     0o755
 *
 * Leading-zero literals without a radix prefix remain decimal:
 *
 *     0755 -> decimal 755
 */
static int parse_integer_u64(Token token, uint64_t *out) {

    if (!out || token.length <= 0)
        return 0;

    unsigned radix = 10;
    int digit_start = 0;

    if (token.length >= 2 &&
        token.start[0] == '0') {
        switch (token.start[1]) {
            case 'x':
            case 'X':
                radix = 16;
                digit_start = 2;
                break;

            case 'b':
            case 'B':
                radix = 2;
                digit_start = 2;
                break;

            case 'o':
            case 'O':
                radix = 8;
                digit_start = 2;
                break;

            default:
                break;
        }
    }

    /*
     * This should already be prevented by the lexer, but reject a
     * prefix without digits defensively.
     */
    if (digit_start >= token.length)
        return 0;

    uint64_t value = 0;

    for (int i = digit_start;
         i < token.length;
         i++) {
        int parsed_digit =
            integer_digit_value(token.start[i]);

        if (parsed_digit < 0 ||
            (unsigned)parsed_digit >= radix) {
            return 0;
        }

        uint64_t digit =
            (uint64_t)parsed_digit;

        /*
         * Check:
         *
         *     value * radix + digit <= UINT64_MAX
         *
         * before performing either operation.
         */
        if (value >
            (UINT64_MAX - digit) / radix) {
            return 0;
        }

        value =
            value * radix + digit;
    }

    *out = value;
    return 1;
}

static int parse_float_token(Parser *p, Token token, double *out)
{
    ArenaMarker marker = arena_mark(p->scratch);
    char *text = arena_alloc(p->scratch, (size_t)token.length + 1);

    memcpy(text, token.start, (size_t)token.length);
    text[token.length] = '\0';

    char *end    = NULL;
    double value = strtod(text, &end);
    int valid = isfinite(value) && end == text + token.length;

    arena_reset_to(p->scratch, marker);

    if (!valid)
        return 0;

    *out = value;
    return 1;
}

// ===================== postfix builders =====================

static Node *finish_call(Parser *p, Node *callee) {

    Node *call = ast_new_call(p->arena, callee, callee->span);

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 0;

    if (!check(p, TOK_RPAREN)) {
        do {
            Node *arg = parse_expression(p);
            if (match(p, TOK_ELLIPSIS))
                arg = ast_new_pack_expansion(p->arena, arg, arg->span);
            nodelist_push(p->arena, &call->as.call.arguments, arg);
        } while (match(p, TOK_COMMA));
    }

    p->suppress_struct_init = saved;

    consume(p, TOK_RPAREN);
    return call;
}

static int parse_explicit_type_arguments(Parser *p, TypeList *out, const char *subject)
{
    assert(out);
    *out = (TypeList){0};

    if (!consume(p, TOK_LESS)) {
        char message[128];
        snprintf(message, sizeof(message), "expected '<' after '::' in %s", subject);
        error_at(p, &p->current, message);
        return 0;
    }

    if (check(p, TOK_GREATER)) {
        char message[128];
        snprintf(message, sizeof(message), "%s requires at least one type argument", subject);
        error_at(p, &p->current, message);
        advance(p);
        return 0;
    }

    for (;;) {
        type_list_push(p->arena, out, parse_type(p));
        if (!match(p, TOK_COMMA))
            break;
    }

    return consume_generic_greater(p, "expected '>' after generic type arguments");
}

static int generic_suffix_starts_expression(Parser *p)
{
    if (!p || p->current.type != TOK_LESS)
        return 0;

    /*
     * At statement start, both a generic declaration and an explicit generic
     * call begin with `name::<...>`. Look past the generic argument list and,
     * when necessary, the following parenthesized list before deciding which
     * grammar to enter. A declaration is uniquely identified by `struct` /
     * `resource`, or by a function parameter list followed by `->` / `{`.
     * Everything else is an expression application. This keeps ordinary
     * expression-level shift tokenization unchanged and avoids reparsing AST.
     */
    Lexer lookahead = p->lexer;
    int generic_depth = 1;
    while (generic_depth > 0) {
        Token token = lexer_next(&lookahead);
        if (token.type == TOK_EOF || token.type == TOK_ERROR)
            return 0;
        if (token.type == TOK_LESS) {
            generic_depth++;
        } else if (token.type == TOK_GREATER) {
            generic_depth--;
        } else if (token.type == TOK_SHIFT_RIGHT) {
            generic_depth -= 2;
        }
    }

    Token after_generic = lexer_next(&lookahead);
    if (after_generic.type == TOK_DOT || after_generic.type == TOK_LBRACE)
        return 1;

    if (after_generic.type == TOK_STRUCT || after_generic.type == TOK_RESOURCE)
        return 0;

    if (after_generic.type != TOK_LPAREN)
        return 0;

    int paren_depth = 1;
    while (paren_depth > 0) {
        Token token = lexer_next(&lookahead);
        if (token.type == TOK_EOF || token.type == TOK_ERROR)
            return 0;
        if (token.type == TOK_LPAREN)
            paren_depth++;
        else if (token.type == TOK_RPAREN)
            paren_depth--;
    }

    Token after_parens = lexer_next(&lookahead);
    return after_parens.type != TOK_ARROW &&
           after_parens.type != TOK_LBRACE;
}

static int generic_callee_dotted_name(
    Node *callee,
    StringView *out_module,
    StringView *out_name
) {
    if (!callee || !out_module || !out_name)
        return 0;

    if (callee->type == NODE_IDENT) {
        *out_module = string_view_empty();
        *out_name = string_view(callee->as.ident.data, callee->as.ident.length);
        return 1;
    }

    if (callee->type == NODE_FIELD && callee->as.field.dotted_path.length != 0)
        return split_dotted_leaf(callee->as.field.dotted_path, out_module, out_name);

    return 0;
}

static Node *finish_generic_call(Parser *p, Node *callee)
{
    TypeList arguments = {0};
    if (!parse_explicit_type_arguments(p, &arguments, "generic application"))
        return ast_new_error(p->arena, p->current);

    if (match(p, TOK_LPAREN)) {
        Node *call = finish_call(p, callee);
        call->as.call.type_arguments = arguments;
        return call;
    }

    if (check(p, TOK_LBRACE) && !p->suppress_struct_init) {
        StringView module_name = string_view_empty();
        StringView type_name = string_view_empty();
        if (generic_callee_dotted_name(callee, &module_name, &type_name)) {
            Node *init = finish_struct_init_named(
                p, module_name, type_name, callee->span);
            init->as.struct_init.type_arguments = arguments;
            return init;
        }
    }

    if (check(p, TOK_DOT)) {
        StringView module_name = string_view_empty();
        StringView type_name = string_view_empty();
        if (generic_callee_dotted_name(callee, &module_name, &type_name)) {
            Type *source_type = arena_new(p->arena, Type);
            memset(source_type, 0, sizeof(*source_type));
            source_type->kind = TYPE_NAMED;
            source_type->array_size = -1;
            source_type->named_module = module_name;
            source_type->named_name = type_name;
            source_type->type_arguments = arguments.items;
            source_type->type_argument_count = arguments.count;
            return ast_new_type_ref(p->arena, source_type, callee->span);
        }
    }

    error_at(
        p,
        &p->current,
        "generic type arguments must be followed by a call or struct initializer"
    );
    return ast_new_error(p->arena, p->current);
}

static Node *finish_field(Parser *p, Node *object) {

    if (!match(p, TOK_IDENT)) {
        error_at(p, &p->current, "expected field name");
        return ast_new_error(p->arena, p->current);
    }

    Token name = p->previous;
    return ast_new_field(p->arena, object, name.start, name.length, name.span);
}

static Node *finish_index(Parser *p, Node *object) {

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 0;

    Node *index = parse_expression(p);

    p->suppress_struct_init = saved;

    consume(p, TOK_RBRACKET);
    return ast_new_index(p->arena, object, index, source_span_join(object->span, index->span));
}
// ===================== statements =====================

static Node *parse_expr_statement(Parser *p) {

    SourceSpan span = p->current.span;
    Node *expr = parse_assignment(p);

    if (!consume(p, TOK_SEMICOLON)) {
        synchronize(p);
        return ast_new_error(p->arena, p->current);
    }

    return ast_new_expr_stmt(p->arena, expr, source_span_join(span, expr->span));
}

static int node_is_exportable_declaration(const Node *node)
{
    if (!node)
        return 0;

    switch (node->type) {
        case NODE_VAR_DECL:
        case NODE_VAR_DECL_GROUP:
        case NODE_FUNC_DECL:
        case NODE_STRUCT_DECL:
        case NODE_ENUM_DECL:
        case NODE_CONST_DECL:
            return 1;
        default:
            return 0;
    }
}

static void mark_exported_declaration(Node *node)
{
    assert(node_is_exportable_declaration(node));
    node->is_exported = 1;

    if (node->type == NODE_VAR_DECL_GROUP) {
        for (int i = 0; i < node->as.var_decl_group.declarations.count; i++)
            node->as.var_decl_group.declarations.items[i]->is_exported = 1;
    }
}

static Node *parse_if_statement(Parser *p) {

    SourceSpan span = p->previous.span;

    int saved = p->suppress_struct_init;
    p->suppress_struct_init = 1;
    Node *cond              = parse_expression(p);
    p->suppress_struct_init = saved;

    Node *then_branch = parse_scoped_control_body(p);
    Node *else_branch = NULL;

    if (match(p, TOK_ELSE)) {
        if (match(p, TOK_IF))
            else_branch = parse_if_statement(p);
        else
            else_branch = parse_scoped_control_body(p);
    }

    return ast_new_if(p->arena, cond, then_branch, else_branch, source_span_join(span, else_branch ? else_branch->span : then_branch->span));
}

// ===================== program =====================
Node *parse_program(Parser *p) {
    Node *program = ast_new_program(p->arena, p->current.span);

    while (!check(p, TOK_EOF)) {
        Node *decl = NULL;

        /*
         * `module` and `import` remain ordinary identifiers outside this
         * top-level directive shape. This avoids consuming useful names as
         * global lexer keywords.
         */
        if (check(p, TOK_IDENT) &&
            (token_text_equals(p->current, "module") ||
             token_text_equals(p->current, "import")) &&
            peek_next_token_type(p) == TOK_IDENT) {
            Token keyword = p->current;
            int is_module = token_text_equals(keyword, "module");
            advance(p);

            if (!consume(p, TOK_IDENT)) {
                decl = ast_new_error(p->arena, p->current);
            } else {
                ParsedDottedName name =
                    parse_dotted_name_from_first(p, p->previous);
                StringView import_alias = string_view_empty();
                SourceSpan directive_span = source_span_join(keyword.span, name.span);

                if (!is_module && check(p, TOK_IDENT) &&
                    token_text_equals(p->current, "as")) {
                    advance(p);
                    if (!consume(p, TOK_IDENT)) {
                        synchronize(p);
                        decl = ast_new_error(p->arena, p->current);
                    } else {
                        import_alias = string_view(
                            p->previous.start,
                            (size_t)p->previous.length
                        );
                        directive_span = source_span_join(directive_span, p->previous.span);
                    }
                }

                if (!decl && !consume(p, TOK_SEMICOLON)) {
                    synchronize(p);
                    decl = ast_new_error(p->arena, p->current);
                } else if (!decl && is_module) {
                    decl = ast_new_module_decl(
                        p->arena, name.full.data, (int)name.full.length,
                        directive_span);
                } else if (!decl) {
                    decl = ast_new_import_decl(
                        p->arena, name.full.data, (int)name.full.length,
                        import_alias.data, (int)import_alias.length,
                        directive_span);
                }
            }
        } else if (check(p, TOK_IDENT) &&
                   token_text_equals(p->current, "export") &&
                   (peek_next_token_type(p) == TOK_IDENT ||
                    peek_next_token_type(p) == TOK_HASH)) {
            Token keyword = p->current;
            advance(p);

            decl = parse_statement(p);
            if (!node_is_exportable_declaration(decl)) {
                error_at(
                    p,
                    &keyword,
                    "export must prefix a top-level declaration"
                );
            } else {
                mark_exported_declaration(decl);
                decl->span = source_span_join(keyword.span, decl->span);
            }
        } else {
            decl = parse_statement(p);
        }

        nodelist_push(p->arena, &program->as.program.statements, decl);
    }

    return program;
}
