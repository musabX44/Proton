#include <string.h>
#include <stdio.h>
#include "lexer.h"
#include "common.h"

typedef struct {
    const char* start;
    const char* current;
    int line;
} Lexer;

Lexer lexer;

void initLexer(const char* source) {
    lexer.start = source;
    lexer.current = source;
    lexer.line = 1;
}

LexerState saveLexerState(void) {
    LexerState state;
    state.start = lexer.start;
    state.current = lexer.current;
    state.line = lexer.line;
    return state;
}

void restoreLexerState(LexerState state) {
    lexer.start = state.start;
    lexer.current = state.current;
    lexer.line = state.line;
}

static bool isAtEnd(void) { return *lexer.current == '\0'; }

static char advance(void) {
    lexer.current++;
    return lexer.current[-1];
}

static char peek(void) { return *lexer.current; }
static char peekNext(void) {
    if (isAtEnd()) return '\0';
    return lexer.current[1];
}

static bool match(char expected) {
    if (isAtEnd()) return false;
    if (*lexer.current != expected) return false;
    lexer.current++;
    return true;
}

static Token makeToken(TokenType type) {
    Token token;
    token.type = type;
    token.start = lexer.start;
    token.length = (int)(lexer.current - lexer.start);
    token.line = lexer.line;
    return token;
}

static Token errorToken(const char* message) {
    Token token;
    token.type = TOKEN_ERROR;
    token.start = message;
    token.length = (int)strlen(message);
    token.line = lexer.line;
    return token;
}

static void skipWhitespace(void) {
    for (;;) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                lexer.line++;
                advance();
                break;
            case '/':
                if (peekNext() == '/') {
                    while (peek() != '\n' && !isAtEnd()) advance();
                } else if (peekNext() == '*') {
                    advance(); advance();
                    while (!(peek() == '*' && peekNext() == '/') && !isAtEnd()) {
                        if (peek() == '\n') lexer.line++;
                        advance();
                    }
                    if (!isAtEnd()) { advance(); advance(); }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }
static bool isAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

typedef struct { const char* word; TokenType type; } Keyword;

static const Keyword keywords[] = {
    {"use", TOKEN_USE}, {"var", TOKEN_VAR}, {"const", TOKEN_CONST},
    {"fn", TOKEN_FN}, {"return", TOKEN_RETURN},
    {"if", TOKEN_IF}, {"else", TOKEN_ELSE},
    {"switch", TOKEN_SWITCH}, {"case", TOKEN_CASE}, {"default", TOKEN_DEFAULT},
    {"while", TOKEN_WHILE}, {"for", TOKEN_FOR},
    {"break", TOKEN_BREAK}, {"continue", TOKEN_CONTINUE},
    {"struct", TOKEN_STRUCT}, {"enum", TOKEN_ENUM},
    {"new", TOKEN_NEW}, {"delete", TOKEN_DELETE}, {"defer", TOKEN_DEFER},
    {"true", TOKEN_TRUE}, {"false", TOKEN_FALSE},
    {"sizeof", TOKEN_SIZEOF}, {"typeof", TOKEN_TYPEOF},
    {"assert", TOKEN_ASSERT}, {"panic", TOKEN_PANIC},
    {"as", TOKEN_AS}, {"private", TOKEN_PRIVATE},
    {"bool", TOKEN_TYPE_NAME}, {"char", TOKEN_TYPE_NAME},
    {"string", TOKEN_TYPE_NAME}, {"byte", TOKEN_TYPE_NAME},
    {"short", TOKEN_TYPE_NAME}, {"int", TOKEN_TYPE_NAME},
    {"long", TOKEN_TYPE_NAME}, {"float", TOKEN_TYPE_NAME},
    {"double", TOKEN_TYPE_NAME}, {"void", TOKEN_TYPE_NAME},
    {"int8", TOKEN_TYPE_NAME}, {"int16", TOKEN_TYPE_NAME},
    {"int32", TOKEN_TYPE_NAME}, {"int64", TOKEN_TYPE_NAME},
    {"uint", TOKEN_TYPE_NAME}, {"uint8", TOKEN_TYPE_NAME},
    {"uint16", TOKEN_TYPE_NAME}, {"uint32", TOKEN_TYPE_NAME},
    {"uint64", TOKEN_TYPE_NAME},
    {"float32", TOKEN_TYPE_NAME}, {"float64", TOKEN_TYPE_NAME},
    {"decimal", TOKEN_TYPE_NAME},
    {NULL, TOKEN_EOF}
};

static TokenType identifierType(void) {
    int length = (int)(lexer.current - lexer.start);
    for (int i = 0; keywords[i].word != NULL; i++) {
        if ((int)strlen(keywords[i].word) == length &&
            memcmp(lexer.start, keywords[i].word, length) == 0) {
            return keywords[i].type;
        }
    }
    return TOKEN_IDENTIFIER;
}

static Token identifier(void) {
    while (isAlpha(peek()) || isDigit(peek())) advance();
    return makeToken(identifierType());
}

static Token number(void) {
    while (isDigit(peek())) advance();
    if (peek() == '.' && isDigit(peekNext())) {
        advance();
        while (isDigit(peek())) advance();
    }
    // Scientific notation: e/E, optionally followed by + or -, then digits.
    if ((peek() == 'e' || peek() == 'E') &&
        (isDigit(peekNext()) ||
         ((peekNext() == '+' || peekNext() == '-') && isDigit(lexer.current[2])))) {
        advance(); // consume 'e'/'E'
        if (peek() == '+' || peek() == '-') advance();
        while (isDigit(peek())) advance();
    }
    return makeToken(TOKEN_NUMBER);
}

static Token tripleString(void) {
    // assumes opening """ already consumed
    while (!isAtEnd()) {
        if (peek() == '"' && peekNext() == '"' && lexer.current[2] == '"') {
            Token tok = makeToken(TOKEN_STRING);
            advance(); advance(); advance();
            return tok;
        }
        if (peek() == '\n') lexer.line++;
        advance();
    }
    return errorToken("Unterminated multi-line string.");
}

static Token string(void) {
    // check for triple-quote
    if (peek() == '"' && peekNext() == '"') {
        advance(); advance(); // consume the extra two quotes
        lexer.start = lexer.current; // content starts after """
        return tripleString();
    }
    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') lexer.line++;
        if (peek() == '\\' && peekNext() != '\0') advance();
        advance();
    }
    if (isAtEnd()) return errorToken("Unterminated string.");
    Token tok = makeToken(TOKEN_STRING);
    tok.start = lexer.start + 1;
    tok.length = (int)(lexer.current - lexer.start - 1);
    advance(); // closing quote
    return tok;
}

Token scanToken(void) {
    skipWhitespace();
    lexer.start = lexer.current;

    if (isAtEnd()) return makeToken(TOKEN_EOF);

    char c = advance();
    if (isAlpha(c)) return identifier();
    if (isDigit(c)) return number();

    switch (c) {
        case '(': return makeToken(TOKEN_LPAREN);
        case ')': return makeToken(TOKEN_RPAREN);
        case '{': return makeToken(TOKEN_LBRACE);
        case '}': return makeToken(TOKEN_RBRACE);
        case '[': return makeToken(TOKEN_LBRACKET);
        case ']': return makeToken(TOKEN_RBRACKET);
        case ',': return makeToken(TOKEN_COMMA);
        case '.': return makeToken(TOKEN_DOT);
        case ';': return makeToken(TOKEN_SEMICOLON);
        case ':':
            if (match(':')) return makeToken(TOKEN_COLON_COLON);
            return makeToken(TOKEN_COLON);
        case '?': return makeToken(TOKEN_QUESTION);
        case '&':
            if (match('&')) return makeToken(TOKEN_AND_AND);
            return makeToken(TOKEN_AMP);
        case '|':
            if (match('|')) return makeToken(TOKEN_OR_OR);
            return makeToken(TOKEN_PIPE);
        case '^':
            return makeToken(TOKEN_CARET);
        case '~':
            return makeToken(TOKEN_TILDE);
        case '+':
            if (match('+')) return makeToken(TOKEN_PLUS_PLUS);
            return makeToken(TOKEN_PLUS);
        case '-':
            if (match('-')) return makeToken(TOKEN_MINUS_MINUS);
            return makeToken(TOKEN_MINUS);
        case '*': return makeToken(TOKEN_STAR);
        case '/': return makeToken(TOKEN_SLASH);
        case '%': return makeToken(TOKEN_PERCENT);
        case '!':
            return makeToken(match('=') ? TOKEN_NOT_EQUAL : TOKEN_BANG);
        case '=':
            return makeToken(match('=') ? TOKEN_EQUAL : TOKEN_ASSIGN);
        case '<':
            if (match('=')) return makeToken(TOKEN_LESS_EQUAL);
            if (match('<')) return makeToken(TOKEN_LSHIFT);
            return makeToken(TOKEN_LESS);
        case '>':
            if (match('=')) return makeToken(TOKEN_GREATER_EQUAL);
            if (match('>')) return makeToken(TOKEN_RSHIFT);
            return makeToken(TOKEN_GREATER);
        case '"': return string();
    }

    return errorToken("Unexpected character.");
}
