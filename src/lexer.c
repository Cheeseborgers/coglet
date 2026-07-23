#include "lexer.h"

#include <assert.h>
#include <string.h>

#include "utils/utils.h"

void lexer_init(Lexer *lx, const char *filename, const char *source) {
    assert(filename);

    lx->filename     = filename;
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
    advance(lx);
    return 1;
}

static Token make_token(TokenType type, const char *start,int length, int line, int column) {
    Token t;
    t.type = type;
    t.start = start;
    t.length = length;
    t.line = line;
    t.column = column;
    return t;
}

static Token error_token(
    Lexer *lx,
    const char *start,
    int length,
    int line,
    int column,
    const char *message
) {
    lx->error_msg = message;

    return make_token(
        TOK_ERROR,
        start,
        length,
        line,
        column
    );
}

// Skips whitespace, // line comments, and /* block */ comments.
// Line comments and block comments never produce tokens.
// 1 = whitespace/comments skipped successfully
// 0 = lexical error was produced
static int skip_whitespace_and_comments(Lexer *lx, Token *error) {

    for (;;) {
        char c = peek(lx);

        if (c == ' ' ||
            c == '\t' ||
            c == '\r' ||
            c == '\n') {
            advance(lx);
            continue;
            }

        if (c == '/' && peek_next(lx) == '/') {
            while (peek(lx) != '\n' && !is_at_end(lx)) {
                advance(lx);
            }

            continue;
        }

        if (c == '/' && peek_next(lx) == '*') {
            const char *comment_start = lx->current;
            int comment_line = lx->line;
            int comment_column = lx->column;

            advance(lx);
            advance(lx);

            while (!is_at_end(lx) && !(peek(lx) == '*' && peek_next(lx) == '/')) {
                advance(lx);
            }

            if (is_at_end(lx)) {
                *error = error_token(
                    lx,
                    comment_start,
                    2,
                    comment_line,
                    comment_column,
                    "unterminated block comment"
                );

                return 0;
            }

            advance(lx);
            advance(lx);
            continue;
        }

        return 1;
    }
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

typedef struct {
    const char *word;
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"if", TOK_IF},
    {"else", TOK_ELSE},
    {"while", TOK_WHILE},
    {"for", TOK_FOR},
    {"return", TOK_RETURN},
    {"struct", TOK_STRUCT},
    {"break", TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"enum", TOK_ENUM},
    {"switch", TOK_SWITCH},
    {"case", TOK_CASE},
    {"default", TOK_DEFAULT},
    {"void", TOK_VOID},
    {"true", TOK_TRUE},
    {"false", TOK_FALSE},
    {"cast", TOK_CAST},
    {"truncate", TOK_TRUNCATE},
    {"readonly", TOK_READONLY},
    {"null", TOK_NULL},

    {"i8", TOK_I8},
    {"i16", TOK_I16},
    {"i32", TOK_I32},
    {"i64", TOK_I64},
    {"u8", TOK_U8},
    {"u16", TOK_U16},
    {"u32", TOK_U32},
    {"u64", TOK_U64},
    {"f32", TOK_F32},
    {"f64", TOK_F64},
    {"bool", TOK_BOOL},
    {"int", TOK_INT_KW},
    {"uint", TOK_UINT_KW},
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

static Token scan_identifier(Lexer *lx, const char *start, int start_line, int start_column) {
    while (is_alpha(peek(lx)) || is_digit(peek(lx)))
        advance(lx);

    int length = (int)(lx->current - start);

    return make_token(
        identifier_type(start, length),
        start,
        length,
        start_line,
        start_column
    );
}

/*
 * Scans decimal integer and floating-point literals.
 *
 * Supported forms:
 *
 *     123
 *     3.14
 *     1e3
 *     1E3
 *     1e+3
 *     1e-3
 *     1.25e4
 *
 * A decimal point must be followed by a digit. An exponent must
 * contain at least one digit after its optional sign.
 */
static Token scan_number(
    Lexer *lx,
    const char *start,
    int start_line,
    int start_column
) {
    while (is_digit(peek(lx)))
        advance(lx);

    int is_float = 0;

    /*
     * Fractional component.
     *
     * Preserve the existing rule that a decimal point belongs to
     * the number only when followed by a digit.
     */
    if (peek(lx) == '.' &&
        is_digit(peek_next(lx))) {
        is_float = 1;

        advance(lx);

        while (is_digit(peek(lx)))
            advance(lx);
        }

    /*
     * Optional decimal exponent.
     *
     * Exponent notation always produces a floating-point token,
     * including forms without a decimal point such as 1e3.
     */
    if (peek(lx) == 'e' || peek(lx) == 'E') {

        is_float = 1;

        advance(lx);

        if (peek(lx) == '+' || peek(lx) == '-')
            advance(lx);


        if (!is_digit(peek(lx))) {

            int length = (int)(lx->current - start);

            return error_token(
                lx,
                start,
                length,
                start_line,
                start_column,
                "expected digits after exponent");
        }

        while (is_digit(peek(lx)))
            advance(lx);
        }

    int length = (int)(lx->current - start);

    return make_token(
        is_float
            ? TOK_NUMBER_FLOAT
            : TOK_NUMBER_INT,
        start,
        length,
        start_line,
        start_column
    );
}

// Handles "..." with backslash escapes; does NOT interpret the escapes
// here (that's the parser/codegen's job) — just scans past them safely.
static Token scan_string(Lexer *lx, const char *start, int start_line, int start_column) {

    while (peek(lx) != '"' && !is_at_end(lx)) {
        if (peek(lx) == '\\' && peek_next(lx) != '\0') {
            advance(lx); // skip escaped char
        }

        advance(lx);
    }

    if (is_at_end(lx)) {
        return error_token(
            lx,
            start,
            1,
            start_line,
            start_column,
            "unterminated string"
        );
    }

    advance(lx); // closing quote

    return make_token(
        TOK_STRING,
        start,
        (int)(lx->current - start),
        start_line,
        start_column
    );
}

static Token scan_char(Lexer *lx, const char *start, int start_line, int start_column)
{
    if (is_at_end(lx)) {
        return error_token(
            lx,
            start,
            1,
            start_line,
            start_column,
            "unterminated character literal"
        );
    }

    if (peek(lx) == '\\')
    {
        advance(lx); // '\'

        if (is_at_end(lx)) {
            return error_token(
                lx,
                start,
                1,
                start_line,
                start_column,
                "unterminated character literal"
            );
        }

        advance(lx); // escaped char
    }
    else
    {
        advance(lx); // normal char
    }

    if (peek(lx) != '\'') {
        return error_token(
            lx,
            start,
            1,
            start_line,
            start_column,
            "expected closing quote for character literal"
        );
    }

    advance(lx);

    return make_token(
        TOK_CHAR,
        start,
        (int)(lx->current - start),
        start_line,
        start_column
    );
}

Token lexer_next(Lexer *lx) {

    Token comment_error;

    if (!skip_whitespace_and_comments(lx, &comment_error)) {
        return comment_error;
    }

    const char *start = lx->current;
    int start_line    = lx->line;
    int start_column  = lx->column;

    if (is_at_end(lx)) {
        return make_token(
            TOK_EOF,
            start,
            0,
            start_line,
            start_column
        );
    }

    char c = advance(lx);

    if (is_alpha(c)) {
        return scan_identifier(
            lx,
            start,
            start_line,
            start_column
        );
    }

    if (is_digit(c)) {
        return scan_number(
            lx,
            start,
            start_line,
            start_column
        );
    }

    if (c == '"') {
        return scan_string(
            lx,
            start,
            start_line,
            start_column
        );
    }

    if (c == '\'') {
        return scan_char(
            lx,
            start,
            start_line,
            start_column
        );
    }

    switch (c) {
        case '(':
            return make_token(
                TOK_LPAREN,
                start,
                1,
                start_line,
                start_column
            );

        case ')':
            return make_token(
                TOK_RPAREN,
                start,
                1,
                start_line,
                start_column
            );

        case '{':
            return make_token(
                TOK_LBRACE,
                start,
                1,
                start_line,
                start_column
            );

        case '}':
            return make_token(
                TOK_RBRACE,
                start,
                1,
                start_line,
                start_column
            );

        case '[':
            return make_token(
                TOK_LBRACKET,
                start,
                1,
                start_line,
                start_column
            );

        case ']':
            return make_token(
                TOK_RBRACKET,
                start,
                1,
                start_line,
                start_column
            );

        case ';':
            return make_token(
                TOK_SEMICOLON,
                start,
                1,
                start_line,
                start_column
            );

        case ',':
            return make_token(
                TOK_COMMA,
                start,
                1,
                start_line,
                start_column
            );

        case '.':
            return make_token(
                TOK_DOT,
                start,
                1,
                start_line,
                start_column
            );

        case '+':
            if (match(lx, '+')) {
                return make_token(
                    TOK_PLUS_PLUS,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_PLUS_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_PLUS,
                start,
                1,
                start_line,
                start_column
            );

        case '-':
            if (match(lx, '-')) {
                return make_token(
                    TOK_MINUS_MINUS,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_MINUS_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '>')) {
                return make_token(
                    TOK_ARROW,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_MINUS,
                start,
                1,
                start_line,
                start_column
            );

        case '*':
            if (match(lx, '=')) {
                return make_token(
                    TOK_STAR_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_STAR,
                start,
                1,
                start_line,
                start_column
            );

        case '/':
            if (match(lx, '=')) {
                return make_token(
                    TOK_SLASH_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_SLASH,
                start,
                1,
                start_line,
                start_column
            );

        case '%':
            if (match(lx, '=')) {
                return make_token(
                    TOK_PERCENT_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_PERCENT,
                start,
                1,
                start_line,
                start_column
            );

        case ':':
            if (match(lx, ':')) {
                return make_token(
                    TOK_COLON_COLON,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_COLON_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_COLON,
                start,
                1,
                start_line,
                start_column
            );

        case '=':
            if (match(lx, '=')) {
                return make_token(
                    TOK_EQUAL_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_EQUAL,
                start,
                1,
                start_line,
                start_column
            );

        case '!':
            if (match(lx, '=')) {
                return make_token(
                    TOK_BANG_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_BANG,
                start,
                1,
                start_line,
                start_column
            );

        case '<':
            if (match(lx, '<')) {
                if (match(lx, '=')) {
                    return make_token(
                        TOK_SHIFT_LEFT_EQUAL,
                        start,
                        3,
                        start_line,
                        start_column
                    );
                }

                return make_token(
                    TOK_SHIFT_LEFT,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_LESS_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_LESS,
                start,
                1,
                start_line,
                start_column
            );

        case '>':
            if (match(lx, '>')) {
                if (match(lx, '=')) {
                    return make_token(
                        TOK_SHIFT_RIGHT_EQUAL,
                        start,
                        3,
                        start_line,
                        start_column
                    );
                }

                return make_token(
                    TOK_SHIFT_RIGHT,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_GREATER_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_GREATER,
                start,
                1,
                start_line,
                start_column
            );

        case '&':
            if (match(lx, '&')) {
                return make_token(
                    TOK_AND_AND,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_AND_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_AND,
                start,
                1,
                start_line,
                start_column
            );

        case '|':
            if (match(lx, '|')) {
                return make_token(
                    TOK_OR_OR,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            if (match(lx, '=')) {
                return make_token(
                    TOK_OR_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_OR,
                start,
                1,
                start_line,
                start_column
            );

        case '^':
            if (match(lx, '=')) {
                return make_token(
                    TOK_XOR_EQUAL,
                    start,
                    2,
                    start_line,
                    start_column
                );
            }

            return make_token(
                TOK_XOR,
                start,
                1,
                start_line,
                start_column
            );

        case '~':
            return make_token(
                TOK_TILDE,
                start,
                1,
                start_line,
                start_column
            );

        default:
            return error_token(
                lx,
                start,
                1,
                start_line,
                start_column,
                "unexpected character"
            );
    }
}

const char *token_type_name(TokenType type) {

    switch (type) {
        case TOK_EOF: return "EOF";
        case TOK_ERROR: return "ERROR";
        case TOK_NUMBER_INT: return "NUMBER_INT";
        case TOK_NUMBER_FLOAT: return "NUMBER_FLOAT";
        case TOK_STRING: return "STRING";
        case TOK_CHAR: return "CHAR";
        case TOK_IDENT: return "IDENT";
        case TOK_TRUE: return "TRUE";
        case TOK_FALSE: return "FALSE";
        case TOK_NULL: return "NULL";
        case TOK_IF: return "IF";
        case TOK_ELSE: return "ELSE";
        case TOK_WHILE: return "WHILE";
        case TOK_FOR: return "FOR";
        case TOK_RETURN: return "RETURN";
        case TOK_VOID: return "VOID";
        case TOK_STRUCT: return "STRUCT";
        case TOK_BREAK: return "BREAK";
        case TOK_CONTINUE: return "CONTINUE";
        case TOK_ENUM: return "ENUM";
        case TOK_SWITCH: return "SWITCH";
        case TOK_CASE: return "CASE";
        case TOK_DEFAULT: return "DEFAULT";
        case TOK_CAST: return "CAST";
        case TOK_TRUNCATE: return "TRUNCATE";
        case TOK_READONLY: return "READONLY";
        case TOK_I8: return "I8";

        case TOK_I16:
            return "I16";

        case TOK_I32:
            return "I32";

        case TOK_I64:
            return "I64";

        case TOK_U8:
            return "U8";

        case TOK_U16:
            return "U16";

        case TOK_U32:
            return "U32";

        case TOK_U64:
            return "U64";

        case TOK_F32:
            return "F32";

        case TOK_F64:
            return "F64";

        case TOK_BOOL:
            return "BOOL";

        case TOK_INT_KW:
            return "INT_KW";

        case TOK_UINT_KW:
            return "UINT_KW";

        case TOK_PLUS:
            return "PLUS";

        case TOK_MINUS:
            return "MINUS";

        case TOK_STAR:
            return "STAR";

        case TOK_SLASH:
            return "SLASH";

        case TOK_PERCENT:
            return "PERCENT";

        case TOK_PLUS_PLUS:
            return "PLUS_PLUS";

        case TOK_MINUS_MINUS:
            return "MINUS_MINUS";

        case TOK_PLUS_EQUAL:
            return "PLUS_EQUAL";

        case TOK_MINUS_EQUAL:
            return "MINUS_EQUAL";

        case TOK_STAR_EQUAL:
            return "STAR_EQUAL";

        case TOK_SLASH_EQUAL:
            return "SLASH_EQUAL";

        case TOK_PERCENT_EQUAL:
            return "PERCENT_EQUAL";

        case TOK_EQUAL:
            return "EQUAL";

        case TOK_EQUAL_EQUAL:
            return "EQUAL_EQUAL";

        case TOK_BANG:
            return "BANG";

        case TOK_BANG_EQUAL:
            return "BANG_EQUAL";

        case TOK_LESS:
            return "LESS";

        case TOK_LESS_EQUAL:
            return "LESS_EQUAL";

        case TOK_GREATER:
            return "GREATER";

        case TOK_GREATER_EQUAL:
            return "GREATER_EQUAL";

        case TOK_SHIFT_LEFT:
            return "SHIFT_LEFT";

        case TOK_SHIFT_RIGHT:
            return "SHIFT_RIGHT";

        case TOK_SHIFT_LEFT_EQUAL:
            return "SHIFT_LEFT_EQUAL";

        case TOK_SHIFT_RIGHT_EQUAL:
            return "SHIFT_RIGHT_EQUAL";

        case TOK_AND_AND:
            return "AND_AND";

        case TOK_OR_OR:
            return "OR_OR";

        case TOK_AND:
            return "AND";

        case TOK_OR:
            return "OR";

        case TOK_XOR:
            return "XOR";

        case TOK_TILDE:
            return "TILDE";

        case TOK_AND_EQUAL:
            return "AND_EQUAL";

        case TOK_OR_EQUAL:
            return "OR_EQUAL";

        case TOK_XOR_EQUAL:
            return "XOR_EQUAL";

        case TOK_LPAREN:
            return "LPAREN";

        case TOK_RPAREN:
            return "RPAREN";

        case TOK_LBRACE:
            return "LBRACE";

        case TOK_RBRACE:
            return "RBRACE";

        case TOK_LBRACKET:
            return "LBRACKET";

        case TOK_RBRACKET:
            return "RBRACKET";

        case TOK_SEMICOLON:
            return "SEMICOLON";

        case TOK_COMMA:
            return "COMMA";

        case TOK_DOT:
            return "DOT";

        case TOK_ARROW:
            return "ARROW";

        case TOK_COLON:
            return "COLON";

        case TOK_COLON_COLON:
            return "COLON_COLON";

        case TOK_COLON_EQUAL:
            return "COLON_EQUAL";
    }

    return "UNKNOWN";
}
