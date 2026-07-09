#ifndef COGLET_LEXER_H
#define COGLET_LEXER_H

typedef enum {
    TOK_EOF = 0,
    TOK_ERROR,

    // literals
    TOK_NUMBER_INT,     // 123
    TOK_NUMBER_FLOAT,   // 3.14
    TOK_STRING,         // "hello"
    TOK_CHAR,           // 'a'
    TOK_IDENT,          // foo

    // keywords
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR, TOK_RETURN,
    TOK_VOID, TOK_STRUCT, TOK_BREAK, TOK_CONTINUE,

    // types
    TOK_I8, TOK_I16, TOK_I32, TOK_I64,
    TOK_U8, TOK_U16, TOK_U32, TOK_U64,
    TOK_F32, TOK_F64,
    TOK_BOOL,
    TOK_INT_KW, TOK_UINT_KW,           // pointer-width aliases

    // operators
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_PLUS_PLUS, TOK_MINUS_MINUS,
    TOK_PLUS_EQUAL, TOK_MINUS_EQUAL, TOK_STAR_EQUAL, TOK_SLASH_EQUAL,
    TOK_EQUAL, TOK_EQUAL_EQUAL, TOK_BANG, TOK_BANG_EQUAL,
    TOK_LESS, TOK_LESS_EQUAL, TOK_GREATER, TOK_GREATER_EQUAL,
    TOK_OR_OR, TOK_AND_AND,
    TOK_AND, TOK_OR,

    // punctuation
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMICOLON, TOK_COMMA, TOK_DOT, TOK_ARROW,
    TOK_COLON, TOK_COLON_COLON, TOK_COLON_EQUAL,

} TokenType;

// Tokens do NOT own their text: `start` points into the original
// source buffer, which must stay alive for the whole compile.
typedef struct {
    TokenType type;

    const char *start;
    int length;

    int line;
    int column;
} Token;

typedef struct {
    const char *filename;      // for error reporting
    const char *source_start;  // kept for error reporting
    const char *current;
    int line;
    int column;
    const char *line_start;
    const char *error_msg;     // set when type == TOK_ERROR
} Lexer;

void lexer_init(Lexer *lx, const char *filename, const char *source);
Token lexer_next(Lexer *lx);
const char *token_type_name(TokenType type); // for printing/tests

#endif