#include "../include/lexer.h"

#include <assert.h>
#include <string.h>
#include <ctype.h>

#include "../include/utils/utils.h"

void lexer_init(Lexer *lx, const char *filename, const char *source) {
    assert(filename); // TODO: Add better asserts

    lx->filename = filename;
    lx->source_start = source;
    lx->current = source;
    lx->line = 1;
    lx->column = 1;
    lx->line_start = source;
    lx->error_msg = NULL;
}

static int is_at_end(Lexer *lx) {
    return *lx->current == '\0';
}

static char peek(Lexer *lx) {
    return *lx->current;
}

static char peek_next(Lexer *lx) {
    if (is_at_end(lx)) return '\0';
    return lx->current[1];
}

static char advance(Lexer *lx)
{
    char c = *lx->current++;

    if (c == '\n') {
        lx->line++;
        lx->column = 1;
    } else {
        lx->column++;
    }

    return c;
}

static int match(Lexer *lx, char expected) {
    if (is_at_end(lx)) return 0;
    if (*lx->current != expected) return 0;
    lx->current++;
    return 1;
}

static Token make_token(Lexer *lx, TokenType type, const char *start, int length) {
    Token t;
    t.type = type;
    t.start = start;
    t.length = length;
    t.line = lx->line;
    t.column = lx->column;
    return t;
}

static Token error_token(Lexer *lx, const char *msg) {
    lx->error_msg = msg;
    Token t;
    t.type = TOK_ERROR;
    t.start = msg;
    t.length = (int)strlen(msg);
    t.line = lx->line;
    t.column = lx->column;
    return t;
}

// Skips whitespace, // line comments, and /* block */ comments.
// Line comments and block comments never produce tokens.
static void skip_whitespace_and_comments(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(lx);
        } else if (c == '/' && peek_next(lx) == '/') {
            while (peek(lx) != '\n' && !is_at_end(lx)) advance(lx);
        } else if (c == '/' && peek_next(lx) == '*') {
            advance(lx); advance(lx); // consume "/*"
            while (!is_at_end(lx) &&
                !(peek(lx) == '*' && peek_next(lx) == '/'))
            {
                advance(lx);
            }

            if (is_at_end(lx))
            {
                // TODO: emit an error token.
                lx->error_msg = "unterminated block comment";
                return;
            }
        } else {
            return;
        }
    }
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

typedef struct { const char *word; TokenType type; } Keyword;

static const Keyword keywords[] = {
    {"if", TOK_IF}, {"else", TOK_ELSE}, {"while", TOK_WHILE},
    {"for", TOK_FOR}, {"return", TOK_RETURN},
    {"struct", TOK_STRUCT},
    {"break", TOK_BREAK}, {"continue", TOK_CONTINUE},
    {"void", TOK_VOID},  {"true", TOK_TRUE}, {"false", TOK_FALSE},

    {"i8", TOK_I8}, {"i16", TOK_I16}, {"i32", TOK_I32}, {"i64", TOK_I64},
    {"u8", TOK_U8}, {"u16", TOK_U16}, {"u32", TOK_U32}, {"u64", TOK_U64},
    {"f32", TOK_F32}, {"f64", TOK_F64},
    {"bool", TOK_BOOL},
    {"int", TOK_INT_KW}, {"uint", TOK_UINT_KW},
};
#define NUM_KEYWORDS (sizeof(keywords) / sizeof(keywords[0]))

static TokenType identifier_type(const char *start, int length) {
    for (size_t i = 0; i < NUM_KEYWORDS; i++) {
        size_t kw_len = strlen(keywords[i].word);
        if ((int)kw_len == length && memcmp(start, keywords[i].word, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static Token scan_identifier(Lexer *lx, const char *start) {
    while (is_alpha(peek(lx)) || is_digit(peek(lx))) advance(lx);
    int length = (int)(lx->current - start);
    return make_token(lx, identifier_type(start, length), start, length);
}

// Handles integers (123), floats (3.14), no exponent/hex support yet.
static Token scan_number(Lexer *lx, const char *start) {
    while (is_digit(peek(lx))) advance(lx);

    int is_float = 0;
    if (peek(lx) == '.' && is_digit(peek_next(lx))) {
        is_float = 1;
        advance(lx); // consume '.'
        while (is_digit(peek(lx))) advance(lx);
    }

    int length = (int)(lx->current - start);
    return make_token(lx, is_float ? TOK_NUMBER_FLOAT : TOK_NUMBER_INT, start, length);
}

// Handles "..." with backslash escapes; does NOT interpret the escapes
// here (that's the parser/codegen's job) — just scans past them safely.
static Token scan_string(Lexer *lx, const char *start) {
    while (
        peek(lx) != '"' &&
        !is_at_end(lx)){
        if (peek(lx) == '\\' && peek_next(lx) != '\0') {
            advance(lx); // skip escaped char
        }
        advance(lx);
    }

    if (is_at_end(lx))
        return error_token(lx, "unterminated string");

    advance(lx); // closing quote
    int length = (int)(lx->current - start);
    return make_token(lx, TOK_STRING, start, length);
}

static Token scan_char(Lexer *lx, const char *start)
{
    if (is_at_end(lx))
        return error_token(lx, "unterminated char literal");

    if (peek(lx) == '\\')
    {
        advance(lx); // '\'

        if (is_at_end(lx))
            return error_token(lx, "unterminated char literal");

        advance(lx); // escaped char
    }
    else
    {
        advance(lx); // normal char
    }

    if (peek(lx) != '\'')
        return error_token(lx, "expected closing '\''");

    advance(lx);

    return make_token(
        lx,
        TOK_CHAR,
        start,
        (int)(lx->current - start)
    );
}

Token lexer_next(Lexer *lx) {
    skip_whitespace_and_comments(lx);

    const char *start = lx->current;
    if (is_at_end(lx)) return make_token(lx, TOK_EOF, start, 0);

    char c = advance(lx);

    if (is_alpha(c)) return scan_identifier(lx, start);
    if (is_digit(c)) return scan_number(lx, start);
    if (c == '"') return scan_string(lx, start);
    if (c == '\'') return scan_char(lx, start);

    switch (c) {
        case '(': return make_token(lx, TOK_LPAREN, start, 1);
        case ')': return make_token(lx, TOK_RPAREN, start, 1);
        case '{': return make_token(lx, TOK_LBRACE, start, 1);
        case '}': return make_token(lx, TOK_RBRACE, start, 1);
        case '[': return make_token(lx, TOK_LBRACKET, start, 1);
        case ']': return make_token(lx, TOK_RBRACKET, start, 1);
        case ';': return make_token(lx, TOK_SEMICOLON, start, 1);
        case ',': return make_token(lx, TOK_COMMA, start, 1);
        case '.': return make_token(lx, TOK_DOT, start, 1);

        case '+':
            if (match(lx, '+')) return make_token(lx, TOK_PLUS_PLUS, start, 2);
            if (match(lx, '=')) return make_token(lx, TOK_PLUS_EQUAL, start, 2);
            return make_token(lx, TOK_PLUS, start, 1);
        case '-':
            if (match(lx, '-')) return make_token(lx, TOK_MINUS_MINUS, start, 2);
            if (match(lx, '=')) return make_token(lx, TOK_MINUS_EQUAL, start, 2);
            if (match(lx, '>')) return make_token(lx, TOK_ARROW, start, 2);
            return make_token(lx, TOK_MINUS, start, 1);
        case '*':
            if (match(lx, '=')) return make_token(lx, TOK_STAR_EQUAL, start, 2);
            return make_token(lx, TOK_STAR, start, 1);
        case '/':
            if (match(lx, '=')) return make_token(lx, TOK_SLASH_EQUAL, start, 2);
            return make_token(lx, TOK_SLASH, start, 1);
        case '%':
            return make_token(lx, TOK_PERCENT, start, 1);
        case ':':
            if (match(lx, ':')) return make_token(lx, TOK_COLON_COLON, start, 2);
            if (match(lx, '=')) return make_token(lx, TOK_COLON_EQUAL, start, 2);
            return make_token(lx, TOK_COLON, start, 1);
        case '=':
            if (match(lx, '=')) return make_token(lx, TOK_EQUAL_EQUAL, start, 2);
            return make_token(lx, TOK_EQUAL, start, 1);
        case '!':
            if (match(lx, '=')) return make_token(lx, TOK_BANG_EQUAL, start, 2);
            return make_token(lx, TOK_BANG, start, 1);
        case '<':
            if (match(lx, '=')) return make_token(lx, TOK_LESS_EQUAL, start, 2);
            return make_token(lx, TOK_LESS, start, 1);
        case '>':
            if (match(lx, '=')) return make_token(lx, TOK_GREATER_EQUAL, start, 2);
            return make_token(lx, TOK_GREATER, start, 1);

        case '&':
            if (match(lx, '&')) return make_token(lx, TOK_AND_AND, start, 2);
            return make_token(lx, TOK_AND, start, 1);
        case '|':
            if (match(lx, '|')) return make_token(lx, TOK_OR_OR, start, 2);
            return make_token(lx, TOK_OR, start, 1);

        default:
            return error_token(lx, "unexpected character");
    }
}

const char *token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF:          return "EOF";
        case TOK_ERROR:        return "ERROR";

        case TOK_NUMBER_INT:   return "NUMBER_INT";
        case TOK_NUMBER_FLOAT: return "NUMBER_FLOAT";
        case TOK_STRING: return "STRING";
        case TOK_CHAR:   return "CHAR";
        case TOK_IDENT:  return "IDENT";
        case TOK_TRUE:   return "TRUE";
        case TOK_FALSE:  return "FALSE";

        case TOK_IF:     return "IF";
        case TOK_ELSE:   return "ELSE";
        case TOK_WHILE:  return "WHILE";
        case TOK_FOR:    return "FOR";
        case TOK_RETURN: return "RETURN";
        case TOK_BOOL:   return "BOOL";

        case TOK_I8:  return "I8";
        case TOK_I16: return "I16";
        case TOK_I32: return "I32";
        case TOK_I64: return "I64";
        case TOK_U8:  return "U8";
        case TOK_U16: return "U16";
        case TOK_U32: return "U32";
        case TOK_U64: return "U64";
        case TOK_F32: return "F32";
        case TOK_F64: return "F64";
        case TOK_INT_KW: return "INT_KW";
        case TOK_UINT_KW: return "UINT_KW";

        case TOK_VOID: return "VOID";
        case TOK_STRUCT: return "STRUCT";
        case TOK_BREAK: return "BREAK";
        case TOK_CONTINUE: return "CONTINUE";
        case TOK_PLUS: return "PLUS";
        case TOK_MINUS: return "MINUS";
        case TOK_STAR: return "STAR";
        case TOK_SLASH: return "SLASH";
        case TOK_PERCENT: return "PERCENT";
        case TOK_PLUS_PLUS: return "PLUS_PLUS";
        case TOK_MINUS_MINUS: return "MINUS_MINUS";
        case TOK_PLUS_EQUAL: return "PLUS_EQUAL";
        case TOK_MINUS_EQUAL: return "MINUS_EQUAL";
        case TOK_STAR_EQUAL: return "STAR_EQUAL";
        case TOK_SLASH_EQUAL: return "SLASH_EQUAL";
        case TOK_EQUAL: return "EQUAL";
        case TOK_EQUAL_EQUAL: return "EQUAL_EQUAL";
        case TOK_BANG: return "BANG";
        case TOK_BANG_EQUAL: return "BANG_EQUAL";
        case TOK_LESS: return "LESS";
        case TOK_LESS_EQUAL: return "LESS_EQUAL";
        case TOK_GREATER: return "GREATER";
        case TOK_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOK_AND_AND: return "AND_AND";
        case TOK_OR_OR: return "OR_OR";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_LBRACE: return "LBRACE";
        case TOK_RBRACE: return "RBRACE";
        case TOK_LBRACKET: return "LBRACKET";
        case TOK_RBRACKET: return "RBRACKET";
        case TOK_SEMICOLON: return "SEMICOLON";
        case TOK_COMMA: return "COMMA";
        case TOK_DOT: return "DOT";
        case TOK_ARROW: return "ARROW";
        case TOK_COLON: return "COLON";
        case TOK_COLON_COLON: return "COLON_COLON";
        case TOK_COLON_EQUAL: return "COLON_EQUAL";
    }
    return "UNKNOWN";
}