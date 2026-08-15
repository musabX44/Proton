#ifndef PROTON_LEXER_H
#define PROTON_LEXER_H

typedef enum {
    // literals
    TOKEN_IDENTIFIER, TOKEN_NUMBER, TOKEN_STRING,

    // punctuation
    TOKEN_LPAREN, TOKEN_RPAREN, TOKEN_LBRACE, TOKEN_RBRACE,
    TOKEN_LBRACKET, TOKEN_RBRACKET,
    TOKEN_COMMA, TOKEN_DOT, TOKEN_SEMICOLON, TOKEN_COLON, TOKEN_COLON_COLON,

    // operators
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_STAR, TOKEN_SLASH, TOKEN_PERCENT,
    TOKEN_PLUS_PLUS, TOKEN_MINUS_MINUS,
    TOKEN_ASSIGN,
    TOKEN_EQUAL, TOKEN_NOT_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL,
    TOKEN_AND_AND, TOKEN_OR_OR, TOKEN_BANG,
    TOKEN_AMP,   // & (address-of, parsed but not executed yet; also binary bitwise-AND)
    TOKEN_STAR_PTR, // reuse TOKEN_STAR for pointer deref/type too
    TOKEN_QUESTION, // ? (postfix "try" operator -- emits OP_TRY)
    TOKEN_PIPE,      // | (bitwise OR)
    TOKEN_CARET,     // ^ (bitwise XOR)
    TOKEN_TILDE,     // ~ (bitwise NOT, unary)
    TOKEN_LSHIFT,    // <<
    TOKEN_RSHIFT,    // >>

    // keywords
    TOKEN_USE, TOKEN_VAR, TOKEN_CONST, TOKEN_FN, TOKEN_RETURN,
    TOKEN_IF, TOKEN_ELSE, TOKEN_SWITCH, TOKEN_CASE, TOKEN_DEFAULT,
    TOKEN_WHILE, TOKEN_FOR, TOKEN_BREAK, TOKEN_CONTINUE,
    TOKEN_STRUCT, TOKEN_ENUM, TOKEN_NEW, TOKEN_DELETE, TOKEN_DEFER,
    TOKEN_TRUE, TOKEN_FALSE, TOKEN_SIZEOF, TOKEN_TYPEOF,
    TOKEN_ASSERT, TOKEN_PANIC,
    TOKEN_AS,      // 'use math as m;' module import aliasing
    TOKEN_PRIVATE, // 'private fn/var/const' module visibility control

    // built-in type names
    TOKEN_TYPE_NAME,

    TOKEN_ERROR, TOKEN_EOF
} TokenType;

typedef struct {
    TokenType type;
    const char* start;
    int length;
    int line;
} Token;

// Snapshot of the lexer's scan position, used to pause scanning one source
// buffer, scan another (e.g. an included stdlib module), then resume.
typedef struct {
    const char* start;
    const char* current;
    int line;
} LexerState;

void initLexer(const char* source);
Token scanToken(void);
LexerState saveLexerState(void);
void restoreLexerState(LexerState state);

#endif
