#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include "compiler.h"
#include "lexer.h"
#include "chunk.h"
#include "object.h"
#include "vm.h"
#include "common.h"

// ---------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------

typedef struct {
    Token current;
    Token previous;
    bool hadError;
    bool panicMode;
} Parser;

static Parser parser;

// one-token lookahead pushback buffer (for `x++` vs `x = ...` disambiguation)
static Token peekedToken;
static bool hasPeeked = false;

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT, // =
    PREC_OR,         // ||
    PREC_AND,        // &&
    PREC_BIT_OR,     // |
    PREC_BIT_XOR,    // ^
    PREC_BIT_AND,    // &
    PREC_EQUALITY,   // == !=
    PREC_COMPARISON, // < > <= >=
    PREC_SHIFT,      // << >>
    PREC_TERM,       // + -
    PREC_FACTOR,     // * / %
    PREC_UNARY,      // ! - ~
    PREC_CALL,       // ()
    PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);

typedef struct {
    ParseFn prefix;
    ParseFn infix;
    Precedence precedence;
} ParseRule;

typedef struct {
    Token name;
    int depth;
    ProtonType type; // PTYPE_NONE if untyped/unchecked
} Local;

#define MAX_DEFERS_PER_FUNCTION 16

typedef struct {
    char* source; // owned, null-terminated copy of the block's inner text
                   // (between the '{' and matching '}'), independently
                   // re-lexed/re-compiled at each function-exit point
    int line;      // source line, for error messages if recompilation fails
} DeferredBlock;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    Local locals[UINT8_COUNT];
    int localCount;
    int scopeDepth;
    DeferredBlock defers[MAX_DEFERS_PER_FUNCTION];
    int deferCount;
} Compiler;

static Compiler* current = NULL;

#define MAX_BREAKS 64

typedef struct LoopCtx {
    struct LoopCtx* enclosing;
    int continueTarget;
    int localCountAtStart;
    int breakJumps[MAX_BREAKS];
    int breakCount;
    bool isSwitch; // true for switch contexts: 'break' targets this, but
                    // 'continue' must skip past it to an enclosing real loop
    // Static escape tracking for the LAM loop-scope region rewind
    // (see regionRewind's doc comment in region.h and the OP_LOOP case
    // in vm.c). Set to true the moment we compile an assignment whose
    // target is a local declared *before* this loop (slot <
    // localCountAtStart) -- e.g. `saved = s;` where `saved` lives
    // outside the loop and `s` is loop-produced. Any such store means a
    // value from this iteration's region may be read after later
    // iterations have rewound/reused that memory, so every OP_LOOP
    // back-edge belonging to this loop must skip the rewind. Every
    // back-edge (loop's own emitLoop calls, plus every 'continue' that
    // targets this loop) is patched from this single flag once we know
    // its final value -- see emitLoop/patchLoopFlag.
    bool escapes;
    // Every back-edge (OP_LOOP + jump-offset-short + flag-byte) emitted
    // for this loop, recorded so its flag byte can be patched once
    // `escapes` reaches its final value at the end of the loop
    // statement (an assignment proving escape can appear anywhere in
    // the body, including after an earlier 'continue' was compiled).
    int loopExits[MAX_BREAKS];
    int loopExitCount;
} LoopCtx;

static LoopCtx* currentLoop = NULL;

// ---------------------------------------------------------------------
// Error reporting
// ---------------------------------------------------------------------

static void errorAt(Token* token, const char* message) {
    if (parser.panicMode) return;
    parser.panicMode = true;
    fprintf(stderr, "[line %d] Error", token->line);
    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type == TOKEN_ERROR) {
        // nothing extra
    } else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }
    fprintf(stderr, ": %s\n", message);
    parser.hadError = true;
}

static void error(const char* message) { errorAt(&parser.previous, message); }
static void errorAtCurrent(const char* message) { errorAt(&parser.current, message); }

// ---------------------------------------------------------------------
// Token stream helpers
// ---------------------------------------------------------------------

static Token rawScan(void) {
    Token t = scanToken();
    while (t.type == TOKEN_ERROR) {
        errorAtCurrent(t.start);
        t = scanToken();
    }
    return t;
}

static void advance(void) {
    parser.previous = parser.current;
    if (hasPeeked) {
        parser.current = peekedToken;
        hasPeeked = false;
    } else {
        parser.current = rawScan();
    }
}

static Token peekNextToken(void) {
    if (!hasPeeked) {
        peekedToken = rawScan();
        hasPeeked = true;
    }
    return peekedToken;
}

static bool check(TokenType type) { return parser.current.type == type; }

static bool match(TokenType type) {
    if (!check(type)) return false;
    advance();
    return true;
}

static void consume(TokenType type, const char* message) {
    if (parser.current.type == type) {
        advance();
        return;
    }
    errorAtCurrent(message);
}

// ---------------------------------------------------------------------
// Emitting bytecode
// ---------------------------------------------------------------------

static Chunk* currentChunk(void) { return &current->function->chunk; }

static void emitByte(uint8_t byte) {
    writeChunk(currentChunk(), byte, parser.previous.line);
}

static void emitBytes(uint8_t a, uint8_t b) {
    emitByte(a);
    emitByte(b);
}

static int emitJump(uint8_t instruction) {
    emitByte(instruction);
    emitByte(0xff);
    emitByte(0xff);
    return currentChunk()->count - 2;
}

static void patchJump(int offset) {
    int jump = currentChunk()->count - offset - 2;
    if (jump > UINT16_MAX) error("Too much code to jump over.");
    currentChunk()->code[offset] = (jump >> 8) & 0xff;
    currentChunk()->code[offset + 1] = jump & 0xff;
}

// Emits an OP_LOOP back-edge to `loopStart`, plus a trailing flag byte
// (0/1) controlling whether the VM's regionRewind for this back-edge
// runs (see LoopCtx.escapes doc comment above). The flag byte starts
// at 0 and is patched to 1 later via patchLoopFlags() if the loop body
// turns out to contain an escaping store -- every back-edge needs the
// same final value, since any of them can be the one that "finishes"
// the iteration that produced the escaping value.
// Marks every loop context enclosing (and including) the current one
// as "escapes" if `slot` was declared before that loop started, i.e.
// this assignment stores a (possibly loop-produced) value into a
// variable that survives past this loop iteration. Called from every
// local-assignment compile site. Marking every enclosing loop (not
// just the innermost) matters for nested loops: a store from an inner
// loop into a local declared before an *outer* loop still means the
// outer loop's region rewind could later overwrite that data, even if
// the inner loop's own rewind is unrelated.
static void markLoopEscapeIfLocalPredatesLoop(int slot) {
    // Walk every enclosing loop (not just the innermost): an outer
    // loop's localCountAtStart is always <= an inner loop's, but a
    // slot can predate the outer loop while still being >=
    // the inner loop's own localCountAtStart (e.g. declared between
    // the two loops), so each level needs its own check -- don't stop
    // at the first loop where the slot doesn't predate it.
    for (LoopCtx* l = currentLoop; l != NULL; l = l->enclosing) {
        if (slot < l->localCountAtStart) {
            l->escapes = true;
        }
    }
}

static void emitLoop(int loopStart) {
    emitByte(OP_LOOP);
    // +3, not +2: the VM does `frame->ip -= offset` right after reading
    // the 2 offset bytes, i.e. from a position that still has the
    // trailing flag byte (emitted below) ahead of it, plus the 2 offset
    // bytes themselves, between here and loopStart's target. All three
    // trailing bytes (2 offset + 1 flag) must be included so the jump
    // target lands exactly on loopStart's first byte.
    int offset = currentChunk()->count - loopStart + 3;
    if (offset > UINT16_MAX) error("Loop body too large.");
    emitByte((offset >> 8) & 0xff);
    emitByte(offset & 0xff);
    int flagOffset = currentChunk()->count;
    emitByte(0); // placeholder; patched by patchLoopFlags()
    if (currentLoop != NULL) {
        if (currentLoop->loopExitCount >= MAX_BREAKS) {
            error("Too many loop back-edges (break/continue) in one loop.");
        } else {
            currentLoop->loopExits[currentLoop->loopExitCount++] = flagOffset;
        }
    }
}

// Writes the final escapes value into every back-edge flag byte
// recorded for `loop`. Called once the whole loop body has been
// compiled, since an escaping assignment can be textually anywhere in
// the body (including after a 'continue' whose OP_LOOP was already
// emitted).
static void patchLoopFlags(LoopCtx* loop) {
    uint8_t flag = loop->escapes ? 1 : 0;
    for (int i = 0; i < loop->loopExitCount; i++) {
        currentChunk()->code[loop->loopExits[i]] = flag;
    }
}

static int makeConstant(Value value) {
    int constant = addConstant(currentChunk(), value);
    if (constant > UINT8_MAX) {
        error("Too many constants in one chunk.");
        return 0;
    }
    return constant;
}

static void emitConstant(Value value) {
    emitBytes(OP_CONSTANT, (uint8_t)makeConstant(value));
}

// Emits a runtime type check against the value currently on top of the
// stack. Leaves the stack unchanged (peeks, doesn't pop) so it can be
// inserted between "compute value" and "store it" without disturbing
// the rest of the expression machinery.
static void emitTypeCheck(ProtonType type) {
    if (type == PTYPE_NONE || type == PTYPE_VOID) return;
    emitBytes(OP_CHECK_TYPE, (uint8_t)type);
}

static void emitPendingDefers(void);

static ObjFunction* endCompiler(void) {
    emitPendingDefers();
    emitByte(OP_NIL);
    emitByte(OP_RETURN);
    ObjFunction* function = current->function;
    current = current->enclosing;
    return function;
}

static void beginScope(void) { current->scopeDepth++; }

static void endScope(void) {
    current->scopeDepth--;
    while (current->localCount > 0 &&
           current->locals[current->localCount - 1].depth > current->scopeDepth) {
        emitByte(OP_POP);
        current->localCount--;
    }
}

// ---------------------------------------------------------------------
// String escape processing
// ---------------------------------------------------------------------

static char* processEscapes(const char* raw, int rawLen, int* outLen) {
    char* buf = malloc(rawLen + 1);
    int j = 0;
    for (int i = 0; i < rawLen; i++) {
        char c = raw[i];
        if (c == '\\' && i + 1 < rawLen) {
            char n = raw[i + 1];
            switch (n) {
                case 'n': buf[j++] = '\n'; i++; break;
                case 't': buf[j++] = '\t'; i++; break;
                case 'r': buf[j++] = '\r'; i++; break;
                case '"': buf[j++] = '"'; i++; break;
                case '\\': buf[j++] = '\\'; i++; break;
                case '0': buf[j++] = '\0'; i++; break;
                default: buf[j++] = c; break;
            }
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    *outLen = j;
    return buf;
}

// ---------------------------------------------------------------------
// Local variable helpers
// ---------------------------------------------------------------------

static bool identifiersEqual(Token* a, Token* b) {
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(Compiler* compiler, Token* name) {
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        Local* local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            return i;
        }
    }
    return -1;
}

static void addLocal(Token name, ProtonType type) {
    if (current->localCount == UINT8_COUNT) {
        error("Too many local variables in one function.");
        return;
    }
    Local* local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = current->scopeDepth;
    local->type = type;
}

static void declareLocal(Token name, ProtonType type) {
    if (current->scopeDepth == 0) return; // handled separately as global
    for (int i = current->localCount - 1; i >= 0; i--) {
        Local* local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) break;
        if (identifiersEqual(&name, &local->name)) {
            error("Already a variable with this name in this scope.");
        }
    }
    addLocal(name, type);
}

// Returns the declared type of a resolved local slot, or PTYPE_NONE.
static ProtonType localType(Compiler* compiler, int slot) {
    if (slot < 0 || slot >= compiler->localCount) return PTYPE_NONE;
    return compiler->locals[slot].type;
}

static int identifierConstant(Token* name) {
    return makeConstant(OBJ_VAL(copyString(name->start, name->length)));
}

// ---------------------------------------------------------------------
// Forward decls for Pratt parser
// ---------------------------------------------------------------------

static void expression(void);
static void statement(void);
static void parsePrecedence(Precedence precedence);
static ParseRule* getRule(TokenType type);
static ProtonType parseType(void);
static uint8_t argumentList(void);
static ProtonType findGlobalType(Token* name);
static bool isKnownEnumName(Token* name);
static bool isKnownStructName(Token* name);
static int structFieldCount(Token* name);
static Token structFieldNameAt(Token* name, int index);
#define MAX_STRUCT_FIELDS 32
static void topLevelDeclaration(void);
static void mapLiteral(bool canAssign);
static void structLiteral(Token structName);
static void tryOperator(bool canAssign);

// ---------------------------------------------------------------------
// Generics forward decls. This compiler has no AST (single-pass,
// straight to bytecode), so generics are implemented as "capture the raw
// source text of the generic declaration, and on first use with a given
// set of concrete type arguments, textually substitute the type
// parameters and compile the result as an ordinary (mangled-name) fn or
// struct". identifierExpr / parseTypeEx need to call into this before
// its full definition is reached lexically further down in the file,
// hence the forward decls here.
// ---------------------------------------------------------------------
#define MAX_GENERIC_PARAMS 4
static bool isKnownGenericFn(Token* name);
static bool isKnownGenericStruct(Token* name);
static bool tryParseGenericArgsSpeculative(int* outCount, Token outArgs[MAX_GENERIC_PARAMS]);
static const char* compileGenericFnInstantiation(Token* name, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]);
static const char* compileGenericStructInstantiation(Token* name, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]);

// Module namespace forward decls (see useDeclaration/loadStdlibModule,
// far below, for the full module system).
static bool isKnownModuleName(Token* name);
static const char* resolveModulePrefix(Token* name);
static ObjString* mangledGlobalName(Token* name);
static const char* currentModulePrefix(void);
static void registerPrivateMember(ObjString* mangledName);
static bool isPrivateMangledName(const char* chars, int length);

// ---------------------------------------------------------------------
// Expression parse functions
// ---------------------------------------------------------------------

// A literal is a float literal if its text contains a '.', or an 'e'/'E'
// exponent marker; otherwise it's a plain (possibly very large) integer
// and should be read with strtoll instead of strtod so it isn't silently
// rounded to the nearest double during parsing.
static bool isFloatLiteralText(const char* start, int length) {
    for (int i = 0; i < length; i++) {
        char c = start[i];
        if (c == '.' || c == 'e' || c == 'E') return true;
    }
    return false;
}

static void numberLiteral(bool canAssign) {
    (void)canAssign;
    const char* start = parser.previous.start;
    int length = parser.previous.length;

    if (isFloatLiteralText(start, length)) {
        double value = strtod(start, NULL);
        emitConstant(NUMBER_VAL(value));
        return;
    }

    errno = 0;
    long long value = strtoll(start, NULL, 10);
    if (errno == ERANGE) {
        // Too large for int64 -- only representable as uint64 (or it's a
        // genuine overflow, which the uint64 OP_CHECK_TYPE range check
        // will catch later if it doesn't fit there either).
        errno = 0;
        unsigned long long uvalue = strtoull(start, NULL, 10);
        emitConstant(UINT64_VAL(uvalue));
        return;
    }
    if (value < INT32_MIN || value > INT32_MAX) {
        emitConstant(INT64_VAL(value));
    } else {
        emitConstant(INT32_VAL(value));
    }
}

static void stringLiteral(bool canAssign) {
    (void)canAssign;
    int len;
    char* chars = processEscapes(parser.previous.start, parser.previous.length, &len);
    emitConstant(OBJ_VAL(takeString(chars, len)));
}

static void literalKeyword(bool canAssign) {
    (void)canAssign;
    switch (parser.previous.type) {
        case TOKEN_TRUE: emitByte(OP_TRUE); break;
        case TOKEN_FALSE: emitByte(OP_FALSE); break;
        default: return; // unreachable
    }
}

static void grouping(bool canAssign) {
    (void)canAssign;
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after expression.");
}

static void unary(bool canAssign) {
    (void)canAssign;
    TokenType opType = parser.previous.type;
    parsePrecedence(PREC_UNARY);
    switch (opType) {
        case TOKEN_MINUS: emitByte(OP_NEGATE); break;
        case TOKEN_BANG: emitByte(OP_NOT); break;
        case TOKEN_TILDE: emitByte(OP_BIT_NOT); break;
        default: return;
    }
}

static void binary(bool canAssign) {
    (void)canAssign;
    TokenType opType = parser.previous.type;
    ParseRule* rule = getRule(opType);
    parsePrecedence((Precedence)(rule->precedence + 1));
    switch (opType) {
        case TOKEN_PLUS: emitByte(OP_ADD); break;
        case TOKEN_MINUS: emitByte(OP_SUBTRACT); break;
        case TOKEN_STAR: emitByte(OP_MULTIPLY); break;
        case TOKEN_SLASH: emitByte(OP_DIVIDE); break;
        case TOKEN_PERCENT: emitByte(OP_MODULO); break;
        case TOKEN_EQUAL: emitByte(OP_EQUAL); break;
        case TOKEN_NOT_EQUAL: emitByte(OP_NOT_EQUAL); break;
        case TOKEN_GREATER: emitByte(OP_GREATER); break;
        case TOKEN_GREATER_EQUAL: emitByte(OP_GREATER_EQUAL); break;
        case TOKEN_LESS: emitByte(OP_LESS); break;
        case TOKEN_LESS_EQUAL: emitByte(OP_LESS_EQUAL); break;
        case TOKEN_PIPE: emitByte(OP_BIT_OR); break;
        case TOKEN_CARET: emitByte(OP_BIT_XOR); break;
        case TOKEN_AMP: emitByte(OP_BIT_AND); break;
        case TOKEN_LSHIFT: emitByte(OP_SHL); break;
        case TOKEN_RSHIFT: emitByte(OP_SHR); break;
        default: return;
    }
}

static void and_(bool canAssign) {
    (void)canAssign;
    int endJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    parsePrecedence(PREC_AND);
    patchJump(endJump);
}

static void or_(bool canAssign) {
    (void)canAssign;
    int elseJump = emitJump(OP_JUMP_IF_FALSE);
    int endJump = emitJump(OP_JUMP);
    patchJump(elseJump);
    emitByte(OP_POP);
    parsePrecedence(PREC_OR);
    patchJump(endJump);
}

static bool tokenIs(Token* t, const char* text) {
    size_t len = strlen(text);
    return (size_t)t->length == len && memcmp(t->start, text, len) == 0;
}

// Module member access: moduleName::member -> global "moduleName.member"
// (same flat-mangling mechanic as enum member access; a module is just
// another namespace prefix over the flat global table). Since module
// members are almost always functions, this also swallows the call
// parens right here so `math::sqrt(x)` compiles straight to an OP_CALL
// against the mangled name, matching the plain-call path elsewhere. A
// bare `module::value` (no call) also works, reading the mangled global
// directly. Note: the '::' is surface syntax only -- internally members
// are still mangled with '.' (e.g. "math.sqrt"), so this is purely a
// parser-level concern.
//
// Shared between identifierExpr (TOKEN_IDENTIFIER module names, the
// common case) and typeNameExpr (module names that collide with a
// reserved type keyword, e.g. `string`) so the logic lives in one place.
// Returns false without consuming anything if `name` isn't a known
// module or isn't followed by '::' -- caller falls through to whatever
// else `name` might be.
static bool tryModuleAccess(Token name) {
    if (!isKnownModuleName(&name) || !check(TOKEN_COLON_COLON)) return false;

    advance(); // consume '::'
    consume(TOKEN_IDENTIFIER, "Expect member name after '::'.");
    Token member = parser.previous;

    // Resolve the identifier used at the call site (possibly an alias
    // from `use math as m;`) to the module's real mangled-key prefix --
    // `m::sqrt` and `math::sqrt` must reach the same global.
    const char* prefix = resolveModulePrefix(&name);
    int prefixLen = (int)strlen(prefix);
    int mangledLen = prefixLen + 1 + member.length;
    char* mangled = malloc((size_t)mangledLen + 1);
    memcpy(mangled, prefix, (size_t)prefixLen);
    mangled[prefixLen] = '.';
    memcpy(mangled + prefixLen + 1, member.start, member.length);
    mangled[mangledLen] = '\0';

    // Private members are only reachable from inside their own module's
    // body (e.g. a module referring to its own private helper via the
    // qualified form); everywhere else is "outside".
    const char* callerModule = currentModulePrefix();
    bool isSelfAccess = callerModule != NULL &&
        (int)strlen(callerModule) == prefixLen && memcmp(callerModule, prefix, (size_t)prefixLen) == 0;
    if (!isSelfAccess && isPrivateMangledName(mangled, mangledLen)) {
        error("This module member is private.");
    }

    if (check(TOKEN_LPAREN)) {
        advance();
        uint8_t argCount = argumentList();
        int nameConst = makeConstant(OBJ_VAL(copyString(mangled, mangledLen)));
        free(mangled);
        emitByte(OP_CALL);
        emitByte((uint8_t)nameConst);
        emitByte(argCount);
        return true;
    }

    int nameConst = makeConstant(OBJ_VAL(copyString(mangled, mangledLen)));
    free(mangled);
    emitBytes(OP_GET_GLOBAL, (uint8_t)nameConst);
    return true;
}

// Prefix rule for TOKEN_TYPE_NAME used as an expression. A bare type name
// (`int`, `bool`, ...) is never a valid expression on its own -- the only
// legitimate use is as a namespace prefix, for the handful of names that
// are simultaneously reserved primitive-type keywords AND sensible module/
// namespace names (`string` being the motivating case: `string::toUpper(s)`,
// once a stdlib/string.prt module exists; `char::code(...)` today, a native
// built-in with the same collision). Anything else falls through to the
// same "not a valid expression" error a bare type name always produced.
static void typeNameExpr(bool canAssign) {
    (void)canAssign;
    Token name = parser.previous;

    if (tryModuleAccess(name)) return;

    if (tokenIs(&name, "char") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'char::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "code")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'char::code'.");
            expression(); // 1-character string
            consume(TOKEN_RPAREN, "Expect ')' after 'char::code' argument.");
            emitByte(OP_CHAR_CODE);
            return;
        } else if (tokenIs(&member, "fromCode")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'char::fromCode'.");
            expression(); // integer code point (0-255)
            consume(TOKEN_RPAREN, "Expect ')' after 'char::fromCode' argument.");
            emitByte(OP_CHAR_FROM_CODE);
            return;
        } else {
            error("Unknown 'char' member (only 'code', 'fromCode' are supported).");
            return;
        }
    }

    error("Expect expression.");
}

static void identifierExpr(bool canAssign) {
    Token name = parser.previous;

    // Struct literal construction: StructName { field = expr; ... }
    if (isKnownStructName(&name) && check(TOKEN_LBRACE)) {
        structLiteral(name);
        return;
    }

    // Generic struct literal construction: StructName<Type,...> { ... }
    if (isKnownGenericStruct(&name) && check(TOKEN_LESS)) {
        Token typeArgs[MAX_GENERIC_PARAMS];
        int typeArgCount = 0;
        if (tryParseGenericArgsSpeculative(&typeArgCount, typeArgs) && check(TOKEN_LBRACE)) {
            const char* mangledName = compileGenericStructInstantiation(&name, typeArgCount, typeArgs);
            Token mangledTok;
            mangledTok.type = TOKEN_IDENTIFIER;
            mangledTok.start = mangledName;
            mangledTok.length = (int)strlen(mangledName);
            mangledTok.line = name.line;
            structLiteral(mangledTok);
            return;
        }
        // Speculative parse failed / not followed by '{' -- fall through.
        // Parser state was fully restored already.
    }

    // Enum member access: EnumName::Member -> global "EnumName.Member".
    // Read-only (no assignment path) -- matches normal enum semantics.
    if (isKnownEnumName(&name) && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after '::'.");
        Token member = parser.previous;

        int mangledLen = name.length + 1 + member.length;
        char* mangled = malloc((size_t)mangledLen + 1);
        memcpy(mangled, name.start, name.length);
        mangled[name.length] = '.';
        memcpy(mangled + name.length + 1, member.start, member.length);
        mangled[mangledLen] = '\0';
        int nameConst = makeConstant(OBJ_VAL(copyString(mangled, mangledLen)));
        free(mangled);

        emitBytes(OP_GET_GLOBAL, (uint8_t)nameConst);
        return;
    }

    // Module member access: moduleName::member -> global "moduleName.member".
    // Factored into a shared helper (see tryModuleAccess below) because a
    // module name can also be a reserved type-name token (`string`, most
    // notably -- see typeNameExpr), which never reaches identifierExpr at
    // all since it isn't a TOKEN_IDENTIFIER.
    if (tryModuleAccess(name)) return;

    // Special-cased pseudo-namespace: io::out(...) / io::in()
    if (tokenIs(&name, "io") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'io::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "out")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'io::out'.");
            uint8_t argCount = argumentList();
            emitBytes(OP_PRINT, argCount);
            emitByte(OP_NIL); // io::out() is void; keep the expression-leaves-one-value invariant
            return;
        } else if (tokenIs(&member, "in")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'io::in'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'io::in('.");
            emitByte(OP_READ_LINE);
            return;
        } else {
            error("Unknown 'io' member (only 'out' and 'in' are supported).");
            return;
        }
    }

    // Special-cased builtin: len(x) -- O(1), reads ObjList->count directly
    // rather than going through the user-function OP_CALL machinery.
    if (tokenIs(&name, "len") && check(TOKEN_LPAREN)) {
        advance(); // consume '('
        expression();
        consume(TOKEN_RPAREN, "Expect ')' after 'len' argument.");
        emitByte(OP_LEN);
        return;
    }

    // Special-cased builtin: push(list, value) -- appends in place (amortized
    // O(1), same appendList used by array literals) and yields the list
    // itself as the expression's value, mirroring OP_SET_INDEX's
    // "assignment is also an expression" convention.
    if (tokenIs(&name, "push") && check(TOKEN_LPAREN)) {
        advance(); // consume '('
        expression();
        consume(TOKEN_COMMA, "Expect ',' after 'push' list argument.");
        expression();
        consume(TOKEN_RPAREN, "Expect ')' after 'push' value argument.");
        emitByte(OP_LIST_PUSH);
        return;
    }

    // Special-cased builtin: listCopy(list) -- a fresh, independently
    // mutable copy (breaks the aliasing `var b = a;` has for lists, since
    // plain assignment just copies the Value's list pointer).
    if (tokenIs(&name, "listCopy") && check(TOKEN_LPAREN)) {
        advance(); // consume '('
        expression();
        consume(TOKEN_RPAREN, "Expect ')' after 'listCopy' argument.");
        emitByte(OP_LIST_COPY);
        return;
    }

    // Special-cased native built-ins: fixed-arity, dispatched directly to a
    // dedicated opcode (same pattern as 'io' above) rather than through
    // OP_CALL's global-table/arity-checked user-function path, since these
    // are host-provided natives, not Proton fn's or real `use`-loaded
    // modules. Surfaced as pseudo-namespaces (`fs::read(...)`, `sys::exec(...)`,
    // `char::code(...)`, `time::now()`) for the same reason real modules and
    // `io` use '::' -- so every namespaced call in the language looks the
    // same at the call site, instead of native built-ins standing out as
    // flat underscore_names while everything else is Name::member.
    if (tokenIs(&name, "fs") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'fs::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "read")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'fs::read'.");
            expression(); // path
            consume(TOKEN_RPAREN, "Expect ')' after 'fs::read' argument.");
            emitByte(OP_FS_READ);
            return;
        } else if (tokenIs(&member, "write")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'fs::write'.");
            expression(); // path
            consume(TOKEN_COMMA, "Expect ',' after 'fs::write' path argument.");
            expression(); // content
            consume(TOKEN_RPAREN, "Expect ')' after 'fs::write' arguments.");
            emitByte(OP_FS_WRITE);
            return;
        } else if (tokenIs(&member, "exists")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'fs::exists'.");
            expression(); // path
            consume(TOKEN_RPAREN, "Expect ')' after 'fs::exists' argument.");
            emitByte(OP_FS_EXISTS);
            return;
        } else {
            error("Unknown 'fs' member (only 'read', 'write', 'exists' are supported).");
            return;
        }
    }
    if (tokenIs(&name, "sys") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'sys::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "exec")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::exec'.");
            expression(); // command
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::exec' argument.");
            emitByte(OP_SYS_EXEC);
            return;
        } else if (tokenIs(&member, "env")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::env'.");
            expression(); // variable name
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::env' argument.");
            emitByte(OP_SYS_ENV);
            return;
        } else if (tokenIs(&member, "args")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::args'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::args('.");
            emitByte(OP_SYS_ARGS);
            return;
        } else if (tokenIs(&member, "setenv")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::setenv'.");
            expression(); // name
            consume(TOKEN_COMMA, "Expect ',' after 'sys::setenv' name argument.");
            expression(); // value
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::setenv' arguments.");
            emitByte(OP_SYS_SETENV);
            return;
        } else if (tokenIs(&member, "exit")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::exit'.");
            expression(); // code
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::exit' argument.");
            emitByte(OP_SYS_EXIT);
            return;
        } else if (tokenIs(&member, "pid")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::pid'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::pid('.");
            emitByte(OP_SYS_PID);
            return;
        } else if (tokenIs(&member, "ppid")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'sys::ppid'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'sys::ppid('.");
            emitByte(OP_SYS_PPID);
            return;
        } else {
            error("Unknown 'sys' member (only 'exec', 'env', 'args', 'setenv', 'exit', 'pid', 'ppid' are supported).");
            return;
        }
    }
    // Note: 'char' is handled in typeNameExpr, not here -- `char` lexes as
    // TOKEN_TYPE_NAME (it's also the reserved character-type keyword), so
    // it never reaches identifierExpr in the first place.
    if (tokenIs(&name, "time") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'time::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "now")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::now'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'time::now('.");
            emitByte(OP_TIME_NOW);
            return;
        } else if (tokenIs(&member, "ticks")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::ticks'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'time::ticks('.");
            emitByte(OP_TIME_TICKS);
            return;
        } else if (tokenIs(&member, "clock")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::clock'.");
            consume(TOKEN_RPAREN, "Expect ')' after 'time::clock('.");
            emitByte(OP_TIME_CLOCK);
            return;
        } else if (tokenIs(&member, "sleep")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::sleep'.");
            expression(); // ms
            consume(TOKEN_RPAREN, "Expect ')' after 'time::sleep' argument.");
            emitByte(OP_TIME_SLEEP);
            return;
        } else if (tokenIs(&member, "format")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::format'.");
            expression(); // timestamp
            consume(TOKEN_COMMA, "Expect ',' after 'time::format' timestamp argument.");
            expression(); // fmt
            consume(TOKEN_RPAREN, "Expect ')' after 'time::format' arguments.");
            emitByte(OP_TIME_FORMAT);
            return;
        } else if (tokenIs(&member, "parse")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'time::parse'.");
            expression(); // dateStr
            consume(TOKEN_COMMA, "Expect ',' after 'time::parse' dateStr argument.");
            expression(); // fmt
            consume(TOKEN_RPAREN, "Expect ')' after 'time::parse' arguments.");
            emitByte(OP_TIME_PARSE);
            return;
        } else {
            error("Unknown 'time' member (only 'now', 'ticks', 'clock', 'sleep', 'format', 'parse' are supported).");
            return;
        }
    }
    if (tokenIs(&name, "net") && check(TOKEN_COLON_COLON)) {
        advance(); // consume '::'
        consume(TOKEN_IDENTIFIER, "Expect member name after 'net::'.");
        Token member = parser.previous;
        if (tokenIs(&member, "get")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::get'.");
            expression(); // url
            consume(TOKEN_RPAREN, "Expect ')' after 'net::get' argument.");
            emitByte(OP_NET_GET);
            return;
        } else if (tokenIs(&member, "post")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::post'.");
            expression(); // url
            consume(TOKEN_COMMA, "Expect ',' after 'net::post' url argument.");
            expression(); // body
            consume(TOKEN_RPAREN, "Expect ')' after 'net::post' arguments.");
            emitByte(OP_NET_POST);
            return;
        } else if (tokenIs(&member, "request")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::request'.");
            expression(); // options map
            consume(TOKEN_RPAREN, "Expect ')' after 'net::request' argument.");
            emitByte(OP_NET_REQUEST);
            return;
        } else if (tokenIs(&member, "resolve")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::resolve'.");
            expression(); // hostname
            consume(TOKEN_RPAREN, "Expect ')' after 'net::resolve' argument.");
            emitByte(OP_NET_RESOLVE);
            return;
        } else if (tokenIs(&member, "ping")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::ping'.");
            expression(); // host
            consume(TOKEN_COMMA, "Expect ',' after 'net::ping' host argument.");
            expression(); // timeoutMs
            consume(TOKEN_RPAREN, "Expect ')' after 'net::ping' arguments.");
            emitByte(OP_NET_PING);
            return;
        } else if (tokenIs(&member, "urlEncode")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::urlEncode'.");
            expression(); // str
            consume(TOKEN_RPAREN, "Expect ')' after 'net::urlEncode' argument.");
            emitByte(OP_NET_URLENCODE);
            return;
        } else if (tokenIs(&member, "urlDecode")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::urlDecode'.");
            expression(); // str
            consume(TOKEN_RPAREN, "Expect ')' after 'net::urlDecode' argument.");
            emitByte(OP_NET_URLDECODE);
            return;
        } else if (tokenIs(&member, "serve")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::serve'.");
            expression(); // port
            consume(TOKEN_COMMA, "Expect ',' after 'net::serve' port argument.");
            // The handler is now an ordinary expression -- a bare
            // function name (resolves via OP_GET_GLOBAL to the function
            // value, same as any other global read), a local/parameter
            // holding a function value, etc. Proton now has first-class
            // function values (see OP_CALL_VALUE), so net::serve just
            // evaluates this expression once, at net::serve() call time,
            // and the VM re-invokes that function value for every
            // request (see callProtonFunction).
            expression();
            consume(TOKEN_RPAREN, "Expect ')' after 'net::serve' arguments.");
            emitByte(OP_NET_SERVE);
            return;
        } else if (tokenIs(&member, "connect")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::connect'.");
            expression(); // host
            consume(TOKEN_COMMA, "Expect ',' after 'net::connect' host argument.");
            expression(); // port
            consume(TOKEN_COMMA, "Expect ',' after 'net::connect' port argument.");
            expression(); // protocol ("tcp" or "udp")
            consume(TOKEN_RPAREN, "Expect ')' after 'net::connect' arguments.");
            emitByte(OP_NET_CONNECT);
            return;
        } else if (tokenIs(&member, "send")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::send'.");
            expression(); // handle
            consume(TOKEN_COMMA, "Expect ',' after 'net::send' handle argument.");
            expression(); // data
            consume(TOKEN_RPAREN, "Expect ')' after 'net::send' arguments.");
            emitByte(OP_NET_SEND);
            return;
        } else if (tokenIs(&member, "recv")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::recv'.");
            expression(); // handle
            consume(TOKEN_COMMA, "Expect ',' after 'net::recv' handle argument.");
            expression(); // maxBytes
            consume(TOKEN_RPAREN, "Expect ')' after 'net::recv' arguments.");
            emitByte(OP_NET_RECV);
            return;
        } else if (tokenIs(&member, "close")) {
            consume(TOKEN_LPAREN, "Expect '(' after 'net::close'.");
            expression(); // handle
            consume(TOKEN_RPAREN, "Expect ')' after 'net::close' argument.");
            emitByte(OP_NET_CLOSE);
            return;
        } else {
            error("Unknown 'net' member (only 'get', 'post', 'request', 'resolve', 'ping', 'urlEncode', 'urlDecode', 'serve', 'connect', 'send', 'recv', 'close' are supported).");
            return;
        }
    }

    // Generic function call: name<Type,...>(args...). `<` after a known
    // generic function name is ambiguous with `<` the comparison operator
    // (e.g. a variable named the same as a generic fn used in `x < y`
    // never happens since function names aren't valid comparison operands
    // here, but to stay safe this always speculatively parses and backs
    // out cleanly on any mismatch -- see tryParseGenericArgsSpeculative).
    if (isKnownGenericFn(&name) && check(TOKEN_LESS)) {
        Token typeArgs[MAX_GENERIC_PARAMS];
        int typeArgCount = 0;
        if (tryParseGenericArgsSpeculative(&typeArgCount, typeArgs) && check(TOKEN_LPAREN)) {
            const char* mangledName = compileGenericFnInstantiation(&name, typeArgCount, typeArgs);
            advance(); // consume '('
            uint8_t argCount = argumentList();
            int nameConst = makeConstant(OBJ_VAL(copyString(mangledName, (int)strlen(mangledName))));
            emitByte(OP_CALL);
            emitByte((uint8_t)nameConst);
            emitByte(argCount);
            return;
        }
        // Not actually a generic call (speculative parse failed or wasn't
        // followed by '('); fall through to normal handling below. Parser
        // state was fully restored by tryParseGenericArgsSpeculative.
    }

    if (check(TOKEN_LPAREN)) {
        // Could be a call-by-name (global fn) OR a call-through-value
        // (a local/parameter holding a first-class function value, e.g.
        // `fn apply(f: fn, x: int): int { return f(x); }`). Locals take
        // priority: if `name` resolves to a local slot, compile a value
        // call (push the local, then args, then OP_CALL_VALUE) instead
        // of the by-name OP_CALL used for ordinary global function
        // declarations -- this mirrors how a same-named local shadows a
        // global everywhere else in this grammar.
        int localSlot = resolveLocal(current, &name);
        if (localSlot != -1) {
            advance(); // consume '('
            emitBytes(OP_GET_LOCAL, (uint8_t)localSlot);
            uint8_t argCount = argumentList();
            emitByte(OP_CALL_VALUE);
            emitByte(argCount);
            return;
        }
        // function call by name. While compiling a module's own body
        // (activeModulePrefix set), an unqualified call like `min(...)`
        // inside stdlib/math.prt must resolve to the module's own
        // mangled global `"math.min"`, not a bare `"min"` -- since every
        // top-level name the module defines was itself registered under
        // that prefix (see mangledGlobalName / funDeclaration). This
        // mirrors how a source file's own functions can call each other
        // by their plain declared names.
        advance();
        uint8_t argCount = argumentList();
        int nameConst;
        const char* modulePrefix = currentModulePrefix();
        if (modulePrefix != NULL) {
            int prefixLen = (int)strlen(modulePrefix);
            int mangledLen = prefixLen + 1 + name.length;
            char* mangled = malloc((size_t)mangledLen + 1);
            memcpy(mangled, modulePrefix, (size_t)prefixLen);
            mangled[prefixLen] = '.';
            memcpy(mangled + prefixLen + 1, name.start, (size_t)name.length);
            mangled[mangledLen] = '\0';
            nameConst = makeConstant(OBJ_VAL(copyString(mangled, mangledLen)));
            free(mangled);
        } else {
            nameConst = identifierConstant(&name);
        }
        emitByte(OP_CALL);
        emitByte((uint8_t)nameConst);
        emitByte(argCount);
        return;
    }

    int slot = resolveLocal(current, &name);
    if (slot != -1) {
        if (canAssign && match(TOKEN_ASSIGN)) {
            expression();
            emitTypeCheck(localType(current, slot));
            markLoopEscapeIfLocalPredatesLoop(slot);
            emitBytes(OP_SET_LOCAL, (uint8_t)slot);
        } else {
            emitBytes(OP_GET_LOCAL, (uint8_t)slot);
        }
    } else {
        // Same intra-module resolution as the call path above: a bare
        // global reference inside a module's own body (e.g. `PI` used
        // inside math.prt's degToRad) must resolve to that module's own
        // mangled global, not a bare, unprefixed name.
        int nameConst;
        const char* modulePrefix = currentModulePrefix();
        if (modulePrefix != NULL) {
            int prefixLen = (int)strlen(modulePrefix);
            int mangledLen = prefixLen + 1 + name.length;
            char* mangled = malloc((size_t)mangledLen + 1);
            memcpy(mangled, modulePrefix, (size_t)prefixLen);
            mangled[prefixLen] = '.';
            memcpy(mangled + prefixLen + 1, name.start, (size_t)name.length);
            mangled[mangledLen] = '\0';
            nameConst = makeConstant(OBJ_VAL(copyString(mangled, mangledLen)));
            free(mangled);
        } else {
            nameConst = identifierConstant(&name);
        }
        if (canAssign && match(TOKEN_ASSIGN)) {
            expression();
            emitTypeCheck(findGlobalType(&name));
            emitBytes(OP_SET_GLOBAL, (uint8_t)nameConst);
        } else {
            emitBytes(OP_GET_GLOBAL, (uint8_t)nameConst);
        }
    }
}

// ---------------------------------------------------------------------
// Array literals `[e1, e2, ...]` and indexing `expr[index]`
// ---------------------------------------------------------------------

// Tracks the most recently *declared* array-typed local/global's element
// type, so an array literal appearing as that declaration's initializer
// can be compiled with OP_BUILD_LIST carrying the right elemType for
// runtime enforcement. Reset to PTYPE_NONE once consumed. This is a
// simple one-shot side channel (mirrors how emitTypeCheck / rememberGlobalType
// already thread type info from declaration site to init-expression site)
// rather than a general type-inference pass -- sufficient for
// `T name[] = [ ... ];` declarations, which is the only array-literal
// production point in this grammar.
static ProtonType pendingArrayElemType = PTYPE_NONE;

static void setPendingArrayElemType(ProtonType elemType) {
    pendingArrayElemType = elemType;
}

// Prefix rule for TOKEN_LBRACKET: `[ expr, expr, ... ]`
static void arrayLiteral(bool canAssign) {
    (void)canAssign;
    ProtonType elemType = pendingArrayElemType;
    pendingArrayElemType = PTYPE_NONE;

    uint8_t elemCount = 0;
    if (!check(TOKEN_RBRACKET)) {
        do {
            expression();
            if (elemCount == 255) error("Can't have more than 255 elements in an array literal.");
            elemCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACKET, "Expect ']' after array literal elements.");
    emitBytes(OP_BUILD_LIST, elemCount);
    emitByte((uint8_t)elemType);
}

// Infix rule for TOKEN_LBRACKET: `expr[ index ]`, optionally followed by
// `= value` for indexed assignment (`expr[index] = value`). The list/
// index/(value) are compiled left-to-right; OP_GET_INDEX / OP_SET_INDEX
// do the O(1) direct access at runtime.
static void indexExpr(bool canAssign) {
    expression(); // index
    consume(TOKEN_RBRACKET, "Expect ']' after index.");

    if (canAssign && match(TOKEN_ASSIGN)) {
        expression(); // value
        emitByte(OP_SET_INDEX);
    } else {
        emitByte(OP_GET_INDEX);
    }
}

// ---------------------------------------------------------------------
// Map literals `{ "key": value, ... }` -- string keys only, per spec.
// ---------------------------------------------------------------------

// Prefix rule for TOKEN_LBRACE as an *expression*: `{ "k": v, "k2": v2 }`.
// Only string-literal keys are accepted (TOKEN_STRING), matching the
// "sadece string anahtarlara izin ver" requirement. Compiles key and
// value expressions left-to-right onto the stack (key, value, key,
// value, ...) and emits OP_BUILD_MAP with the pair count; the VM builds
// the ObjMap and does the tableSet insertions (see vm.c).
static void mapLiteral(bool canAssign) {
    (void)canAssign;
    uint8_t pairCount = 0;
    if (!check(TOKEN_RBRACE)) {
        do {
            if (check(TOKEN_RBRACE)) break; // allow trailing comma
            if (!check(TOKEN_STRING)) {
                errorAtCurrent("Map keys must be string literals.");
                // best-effort resync: skip token so we don't spin forever
                advance();
            } else {
                advance(); // consume the string token
                int len;
                char* chars = processEscapes(parser.previous.start, parser.previous.length, &len);
                emitConstant(OBJ_VAL(takeString(chars, len)));
            }
            consume(TOKEN_COLON, "Expect ':' after map key.");
            expression(); // value
            if (pairCount == 255) error("Can't have more than 255 pairs in a map literal.");
            pairCount++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RBRACE, "Expect '}' after map literal.");
    emitBytes(OP_BUILD_MAP, pairCount);
}

// ---------------------------------------------------------------------
// Struct literals: Name { field = expr; field = expr; ... }
//
// Compiles to the exact same bytecode shape as a map literal (push key,
// push value, ... ; OP_BUILD_MAP <pairCount>) -- a struct instance IS a
// map at runtime (ObjMap), just with a compile-time-known, fixed field
// set. Any declared field not explicitly assigned in the literal gets an
// implicit `nil`, so every instance of a given struct always has the
// same complete set of keys present (no "undefined map key" surprises
// from a field that was simply omitted).
//
// Called from identifierExpr once it has already consumed the struct's
// name identifier and confirmed the next token is '{' and the name is a
// known struct type.
static void structLiteral(Token structName) {
    consume(TOKEN_LBRACE, "Expect '{' to begin struct literal.");

    bool fieldSet[MAX_STRUCT_FIELDS];
    int declaredCount = structFieldCount(&structName);
    for (int i = 0; i < declaredCount && i < MAX_STRUCT_FIELDS; i++) fieldSet[i] = false;

    int pairCount = 0;
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        consume(TOKEN_IDENTIFIER, "Expect field name in struct literal.");
        Token fieldName = parser.previous;

        int fieldIndex = -1;
        for (int i = 0; i < declaredCount; i++) {
            Token declared = structFieldNameAt(&structName, i);
            if (declared.length == fieldName.length &&
                memcmp(declared.start, fieldName.start, fieldName.length) == 0) {
                fieldIndex = i;
                break;
            }
        }
        if (fieldIndex == -1) {
            error("Unknown field name for this struct type.");
        } else if (fieldIndex < MAX_STRUCT_FIELDS) {
            fieldSet[fieldIndex] = true;
        }

        consume(TOKEN_ASSIGN, "Expect '=' after field name in struct literal.");
        emitConstant(OBJ_VAL(copyString(fieldName.start, fieldName.length)));
        expression(); // field value
        consume(TOKEN_SEMICOLON, "Expect ';' after field value in struct literal.");
        if (pairCount == 255) error("Can't have more than 255 fields in a struct literal.");
        pairCount++;
    }
    consume(TOKEN_RBRACE, "Expect '}' after struct literal.");

    // Fill in any declared field the literal didn't set, as nil.
    for (int i = 0; i < declaredCount && i < MAX_STRUCT_FIELDS; i++) {
        if (!fieldSet[i]) {
            Token declared = structFieldNameAt(&structName, i);
            emitConstant(OBJ_VAL(copyString(declared.start, declared.length)));
            emitByte(OP_NIL);
            if (pairCount == 255) error("Can't have more than 255 fields in a struct literal.");
            pairCount++;
        }
    }

    emitBytes(OP_BUILD_MAP, (uint8_t)pairCount);
}

// Infix rule for TOKEN_DOT: `expr.field`, optionally followed by
// `= value` for field assignment (`expr.field = value`). Field names are
// pushed as string constants and field access reuses OP_GET_INDEX /
// OP_SET_INDEX exactly as map indexing does -- a struct instance is an
// ObjMap, so `.field` and `["field"]` are the same operation at runtime,
// just with the key coming from an unquoted identifier instead of a
// string literal.
static void dotExpr(bool canAssign) {
    consume(TOKEN_IDENTIFIER, "Expect field name after '.'.");
    Token field = parser.previous;
    emitConstant(OBJ_VAL(copyString(field.start, field.length)));

    if (canAssign && match(TOKEN_ASSIGN)) {
        expression();
        emitByte(OP_SET_INDEX);
    } else {
        emitByte(OP_GET_INDEX);
    }
}

// ---------------------------------------------------------------------
// `?` (OP_TRY) postfix operator: `expr?`. Compiles the operand expression
// normally, then emits OP_TRY, which at runtime checks whether the value
// just produced is a VAL_ERROR and, if so, unwinds the current function
// (see vm.c OP_TRY case). Parsed as a very-high-precedence postfix rule
// (PREC_CALL) via the same infix-rule mechanism used for `expr[index]` --
// getRule(TOKEN_QUESTION)->infix is this function; it consumes no further
// tokens (there is nothing to the right of `?`) and just emits the op.
static void tryOperator(bool canAssign) {
    (void)canAssign;
    emitByte(OP_TRY);
}

ParseRule rules[/* TOKEN_EOF+1 is largest, use big enough array */ 96];
static bool rulesInitialized = false;

static void initRules(void) {
    if (rulesInitialized) return;
    rulesInitialized = true;
    for (int i = 0; i < 96; i++) rules[i] = (ParseRule){NULL, NULL, PREC_NONE};

    rules[TOKEN_LPAREN]        = (ParseRule){grouping, NULL,   PREC_NONE};
    rules[TOKEN_MINUS]         = (ParseRule){unary,    binary, PREC_TERM};
    rules[TOKEN_PLUS]          = (ParseRule){NULL,     binary, PREC_TERM};
    rules[TOKEN_SLASH]         = (ParseRule){NULL,     binary, PREC_FACTOR};
    rules[TOKEN_STAR]          = (ParseRule){NULL,     binary, PREC_FACTOR};
    rules[TOKEN_PERCENT]       = (ParseRule){NULL,     binary, PREC_FACTOR};
    rules[TOKEN_BANG]          = (ParseRule){unary,    NULL,   PREC_NONE};
    rules[TOKEN_TILDE]         = (ParseRule){unary,    NULL,   PREC_NONE};
    rules[TOKEN_PIPE]          = (ParseRule){NULL,     binary, PREC_BIT_OR};
    rules[TOKEN_CARET]         = (ParseRule){NULL,     binary, PREC_BIT_XOR};
    rules[TOKEN_AMP]           = (ParseRule){NULL,     binary, PREC_BIT_AND};
    rules[TOKEN_LSHIFT]        = (ParseRule){NULL,     binary, PREC_SHIFT};
    rules[TOKEN_RSHIFT]        = (ParseRule){NULL,     binary, PREC_SHIFT};
    rules[TOKEN_NOT_EQUAL]     = (ParseRule){NULL,     binary, PREC_EQUALITY};
    rules[TOKEN_EQUAL]         = (ParseRule){NULL,     binary, PREC_EQUALITY};
    rules[TOKEN_GREATER]       = (ParseRule){NULL,     binary, PREC_COMPARISON};
    rules[TOKEN_GREATER_EQUAL] = (ParseRule){NULL,     binary, PREC_COMPARISON};
    rules[TOKEN_LESS]          = (ParseRule){NULL,     binary, PREC_COMPARISON};
    rules[TOKEN_LESS_EQUAL]    = (ParseRule){NULL,     binary, PREC_COMPARISON};
    rules[TOKEN_IDENTIFIER]    = (ParseRule){identifierExpr, NULL, PREC_NONE};
    rules[TOKEN_STRING]        = (ParseRule){stringLiteral, NULL, PREC_NONE};
    rules[TOKEN_NUMBER]        = (ParseRule){numberLiteral, NULL, PREC_NONE};
    rules[TOKEN_AND_AND]       = (ParseRule){NULL,     and_,   PREC_AND};
    rules[TOKEN_OR_OR]         = (ParseRule){NULL,     or_,    PREC_OR};
    rules[TOKEN_TRUE]          = (ParseRule){literalKeyword, NULL, PREC_NONE};
    rules[TOKEN_FALSE]         = (ParseRule){literalKeyword, NULL, PREC_NONE};
    rules[TOKEN_LBRACKET]      = (ParseRule){arrayLiteral, indexExpr, PREC_CALL};
    rules[TOKEN_LBRACE]        = (ParseRule){mapLiteral, NULL, PREC_NONE};
    rules[TOKEN_QUESTION]      = (ParseRule){NULL, tryOperator, PREC_CALL};
    rules[TOKEN_DOT]           = (ParseRule){NULL, dotExpr, PREC_CALL};
    rules[TOKEN_TYPE_NAME]     = (ParseRule){typeNameExpr, NULL, PREC_NONE};
}

static ParseRule* getRule(TokenType type) { return &rules[type]; }

static void parsePrecedence(Precedence precedence) {
    advance();
    ParseFn prefixRule = getRule(parser.previous.type)->prefix;
    if (prefixRule == NULL) {
        error("Expect expression.");
        return;
    }
    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(canAssign);

    while (precedence <= getRule(parser.current.type)->precedence) {
        advance();
        ParseFn infixRule = getRule(parser.previous.type)->infix;
        infixRule(canAssign);
    }

    if (canAssign && match(TOKEN_ASSIGN)) {
        error("Invalid assignment target.");
    }
}

static void expression(void) { parsePrecedence(PREC_ASSIGNMENT); }

static uint8_t argumentList(void) {
    uint8_t count = 0;
    if (!check(TOKEN_RPAREN)) {
        do {
            expression();
            if (count == 255) error("Can't have more than 255 arguments.");
            count++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after arguments.");
    return count;
}

// ---------------------------------------------------------------------
// Types (parsed, mostly not semantically enforced yet)
// ---------------------------------------------------------------------

typedef struct { const char* word; ProtonType type; } TypeNameEntry;

static const TypeNameEntry primitiveTypeNames[] = {
    {"bool", PTYPE_BOOL}, {"char", PTYPE_CHAR}, {"string", PTYPE_STRING},
    {"byte", PTYPE_BYTE}, {"short", PTYPE_SHORT}, {"int", PTYPE_INT},
    {"long", PTYPE_LONG}, {"float", PTYPE_FLOAT}, {"double", PTYPE_DOUBLE},
    {"void", PTYPE_VOID},
    {"int8", PTYPE_INT8}, {"int16", PTYPE_INT16}, {"int32", PTYPE_INT32},
    {"int64", PTYPE_INT64}, {"uint", PTYPE_UINT}, {"uint8", PTYPE_UINT8},
    {"uint16", PTYPE_UINT16}, {"uint32", PTYPE_UINT32}, {"uint64", PTYPE_UINT64},
    {"float32", PTYPE_FLOAT32}, {"float64", PTYPE_FLOAT64},
    {"decimal", PTYPE_DECIMAL},
    {NULL, PTYPE_NONE}
};

static ProtonType primitiveTypeFromToken(Token* t) {
    for (int i = 0; primitiveTypeNames[i].word != NULL; i++) {
        if (tokenIs(t, primitiveTypeNames[i].word)) return primitiveTypeNames[i].type;
    }
    return PTYPE_NONE;
}

// Parses a type annotation (after the ':'). Returns the resolved primitive
// type tag so the caller can enforce it, or PTYPE_NONE for anything that
// isn't a checkable primitive yet (custom/struct types, pointers, arrays --
// those are parsed for forward compatibility but not semantically enforced,
// same as before).
// isArrayOut, if non-NULL, receives whether an array suffix `[]` was
// parsed, and *elemTypeOut receives the element's primitive type tag
// (PTYPE_NONE if the element type itself isn't a checkable primitive).
// Pointer suffixes (`*`) still collapse the overall declared type to
// PTYPE_NONE, same as before -- pointers aren't representable in the
// value model yet.
static ProtonType parseTypeEx(bool* isArrayOut, ProtonType* elemTypeOut) {
    if (isArrayOut) *isArrayOut = false;
    if (elemTypeOut) *elemTypeOut = PTYPE_NONE;

    ProtonType type = PTYPE_NONE;
    if (check(TOKEN_TYPE_NAME)) {
        type = primitiveTypeFromToken(&parser.current);
        advance();
    } else if (check(TOKEN_FN)) {
        // `fn` as a type name: a first-class function value parameter/
        // variable (e.g. `fn apply(f: fn, x: int): int`). Unchecked at
        // runtime -- there's no per-signature function type yet, this
        // just lets the parameter declare that it expects *a* function
        // value rather than falling through to "unknown type name" error.
        advance();
    } else if (check(TOKEN_IDENTIFIER)) {
        advance(); // custom/struct type name -- not implemented yet, unchecked
    } else {
        errorAtCurrent("Expect a type name.");
        return PTYPE_NONE;
    }
    bool isPointer = false;
    while (match(TOKEN_STAR)) {
        isPointer = true; // pointer level(s) - parsed, not yet executable
    }
    bool isArray = false;
    if (match(TOKEN_LBRACKET)) {
        consume(TOKEN_RBRACKET, "Expect ']' after '[' in array type.");
        isArray = true;
    }
    if (isArray && !isPointer) {
        // `T[]` is now representable: ObjList carrying elemType == T.
        if (isArrayOut) *isArrayOut = true;
        if (elemTypeOut) *elemTypeOut = type;
        return PTYPE_NONE; // the *declared variable's* own PTYPE stays
                            // "none" (it's a list, not a checkable scalar
                            // primitive) -- element checking happens via
                            // ObjList->elemType instead, see OP_BUILD_LIST.
    }
    // Pointers (and pointer-to-array) aren't representable in the
    // current value model yet, so don't try to enforce a primitive
    // check on them.
    if (isPointer) return PTYPE_NONE;
    return type;
}

static ProtonType parseType(void) {
    return parseTypeEx(NULL, NULL);
}

// ---------------------------------------------------------------------
// Increment / decrement (statement-level only: `x++;` `x--;`)
// ---------------------------------------------------------------------

static void compileIncDec(Token name, bool isInc) {
    int slot = resolveLocal(current, &name);
    if (slot != -1) {
        emitBytes(OP_GET_LOCAL, (uint8_t)slot);
        emitConstant(NUMBER_VAL(1));
        emitByte(isInc ? OP_ADD : OP_SUBTRACT);
        emitTypeCheck(localType(current, slot));
        emitBytes(OP_SET_LOCAL, (uint8_t)slot);
        emitByte(OP_POP);
    } else {
        int nameConst = identifierConstant(&name);
        emitBytes(OP_GET_GLOBAL, (uint8_t)nameConst);
        emitConstant(NUMBER_VAL(1));
        emitByte(isInc ? OP_ADD : OP_SUBTRACT);
        emitTypeCheck(findGlobalType(&name));
        emitBytes(OP_SET_GLOBAL, (uint8_t)nameConst);
        emitByte(OP_POP);
    }
}

// returns true if it consumed an inc/dec statement starting at current token
static bool tryIncDecStatement(bool requireSemicolon) {
    if (!check(TOKEN_IDENTIFIER)) return false;
    Token maybeName = parser.current;
    TokenType nextType = peekNextToken().type;
    if (nextType != TOKEN_PLUS_PLUS && nextType != TOKEN_MINUS_MINUS) return false;

    advance(); // consume identifier -> previous == name
    Token name = parser.previous;
    bool isInc = check(TOKEN_PLUS_PLUS);
    advance(); // consume ++/--
    compileIncDec(name, isInc);
    (void)maybeName;
    if (requireSemicolon) consume(TOKEN_SEMICOLON, "Expect ';' after increment/decrement.");
    return true;
}

// ---------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------

static void expressionStatement(void) {
    expression();
    consume(TOKEN_SEMICOLON, "Expect ';' after expression.");
    emitByte(OP_POP);
}

static void localVarDeclarationBody(void) {
    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    consume(TOKEN_COLON, "Expect ':' after variable name.");
    bool isArray;
    ProtonType elemType;
    ProtonType type = parseTypeEx(&isArray, &elemType);
    if (match(TOKEN_ASSIGN)) {
        if (isArray) setPendingArrayElemType(elemType);
        expression();
        emitTypeCheck(type); // leave value on stack, validate before it becomes the local
    } else {
        emitByte(OP_NIL); // no initializer -> nil, not type-checked (acts as "unset")
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after variable declaration.");
    declareLocal(name, type);
}

static void ifStatement(void) {
    consume(TOKEN_LPAREN, "Expect '(' after 'if'.");
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after condition.");

    int thenJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();

    int elseJump = emitJump(OP_JUMP);
    patchJump(thenJump);
    emitByte(OP_POP);

    if (match(TOKEN_ELSE)) statement();
    patchJump(elseJump);
}

static void whileStatement(void) {
    int loopStart = currentChunk()->count;

    LoopCtx loop;
    loop.enclosing = currentLoop;
    loop.continueTarget = loopStart;
    loop.localCountAtStart = current->localCount;
    loop.breakCount = 0;
    loop.isSwitch = false;
    loop.escapes = false;
    loop.loopExitCount = 0;
    currentLoop = &loop;

    consume(TOKEN_LPAREN, "Expect '(' after 'while'.");
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after condition.");

    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);
    statement();
    emitLoop(loopStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < loop.breakCount; i++) patchJump(loop.breakJumps[i]);
    patchLoopFlags(&loop);
    currentLoop = loop.enclosing;
}

// `for (x in y)`: sugar over index-based iteration through a list.
// Desugars to (conceptually):
//   var __iter: T[] = y;      (hidden, evaluated once)
//   var __idx: int32 = 0;     (hidden loop counter)
//   var __len: int32 = len(__iter); (hidden, evaluated once)
//   while (__idx < __len) {
//       var x = __iter[__idx];   (real user local, re-bound each iteration)
//       ...body...
//       __idx++;
//   }
// `x` is declared PTYPE_NONE (unchecked) since ObjList element types are
// only tracked at runtime (see ObjList->elemType / OP_GET_INDEX, which
// itself performs no static type check on the retrieved element) --
// matching the same unchecked-read behavior any manual `y[i]` already has.
// Only lists are supported (checked at runtime via OP_LEN/OP_GET_INDEX's
// existing IS_LIST guards); maps/strings aren't iterable via this form yet.
static void forInStatement(Token varName) {
    Token hiddenIter;
    hiddenIter.type = TOKEN_IDENTIFIER;
    hiddenIter.start = "";
    hiddenIter.length = 0;
    hiddenIter.line = parser.previous.line;

    expression(); // the iterable expression (`y`)
    consume(TOKEN_RPAREN, "Expect ')' after for-in iterable.");
    addLocal(hiddenIter, PTYPE_NONE);
    int iterSlot = current->localCount - 1;

    Token hiddenIdx;
    hiddenIdx.type = TOKEN_IDENTIFIER;
    hiddenIdx.start = "";
    hiddenIdx.length = 0;
    hiddenIdx.line = parser.previous.line;
    emitConstant(NUMBER_VAL(0));
    addLocal(hiddenIdx, PTYPE_NONE);
    int idxSlot = current->localCount - 1;

    Token hiddenLen;
    hiddenLen.type = TOKEN_IDENTIFIER;
    hiddenLen.start = "";
    hiddenLen.length = 0;
    hiddenLen.line = parser.previous.line;
    emitBytes(OP_GET_LOCAL, (uint8_t)iterSlot);
    emitByte(OP_LEN);
    addLocal(hiddenLen, PTYPE_NONE);
    int lenSlot = current->localCount - 1;

    // loop condition: __idx < __len
    int loopStart = currentChunk()->count;
    emitBytes(OP_GET_LOCAL, (uint8_t)idxSlot);
    emitBytes(OP_GET_LOCAL, (uint8_t)lenSlot);
    emitByte(OP_LESS);
    int exitJump = emitJump(OP_JUMP_IF_FALSE);
    emitByte(OP_POP);

    LoopCtx loop;
    loop.enclosing = currentLoop;
    loop.localCountAtStart = current->localCount;
    loop.breakCount = 0;
    loop.isSwitch = false;
    loop.escapes = false;
    loop.loopExitCount = 0;
    currentLoop = &loop;

    // Body runs in its own scope so `x` is fresh (and poppable) each
    // iteration, same as the user writing `var x = __iter[__idx];`
    // themselves at the top of the loop body.
    beginScope();
    emitBytes(OP_GET_LOCAL, (uint8_t)iterSlot);
    emitBytes(OP_GET_LOCAL, (uint8_t)idxSlot);
    emitByte(OP_GET_INDEX);
    addLocal(varName, PTYPE_NONE);

    // continue inside a for-in must still run the increment before
    // looping, so it can't just jump straight to loopStart (that would
    // skip __idx++). Route it to incrementStart instead.
    int bodyJump = emitJump(OP_JUMP);
    int incrementStart = currentChunk()->count;
    // increment: __idx = __idx + 1
    emitBytes(OP_GET_LOCAL, (uint8_t)idxSlot);
    emitConstant(NUMBER_VAL(1));
    emitByte(OP_ADD);
    emitBytes(OP_SET_LOCAL, (uint8_t)idxSlot);
    emitByte(OP_POP);
    emitLoop(loopStart);
    patchJump(bodyJump);

    loop.continueTarget = incrementStart;
    currentLoop = &loop; // continueStatement reads currentLoop directly

    statement();
    endScope(); // pops `x`

    emitLoop(incrementStart);

    patchJump(exitJump);
    emitByte(OP_POP);

    for (int i = 0; i < loop.breakCount; i++) patchJump(loop.breakJumps[i]);
    patchLoopFlags(&loop);
    currentLoop = loop.enclosing;
}

static void forStatement(void) {
    beginScope();
    consume(TOKEN_LPAREN, "Expect '(' after 'for'.");

    // Lookahead for `for (x in y)`: an identifier immediately followed by
    // the (non-keyword) identifier "in" signals for-in form. Only one
    // token of lookahead is available (peekNextToken), which is exactly
    // enough here since a real C-style for's first clause can never be a
    // bare identifier token followed directly by another identifier
    // named "in" (that's not a valid start of a var-decl, expression
    // statement headed by `;`, or `x++`/`x--`).
    if (check(TOKEN_IDENTIFIER)) {
        Token maybeVarName = parser.current;
        Token savedCurrent = parser.current;
        TokenType nextType = peekNextToken().type;
        Token nextTok = peekedToken;
        if (nextType == TOKEN_IDENTIFIER && tokenIs(&nextTok, "in")) {
            advance(); // consume var name -> previous == name
            Token varName = parser.previous;
            advance(); // consume 'in'
            forInStatement(varName);
            endScope();
            return;
        }
        (void)maybeVarName;
        (void)savedCurrent;
    }

    if (match(TOKEN_SEMICOLON)) {
        // no initializer
    } else if (check(TOKEN_VAR) || check(TOKEN_CONST)) {
        advance();
        localVarDeclarationBody();
    } else {
        expressionStatement();
    }

    int loopStart = currentChunk()->count;
    int exitJump = -1;
    if (!check(TOKEN_SEMICOLON)) {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after loop condition.");
        exitJump = emitJump(OP_JUMP_IF_FALSE);
        emitByte(OP_POP);
    } else {
        advance(); // consume ';'
    }

    // LoopCtx is created before the increment clause (rather than only
    // before the body) so that the increment's own OP_LOOP back-edge to
    // `loopStart` -- which also runs once per iteration -- is tracked
    // by loopExits and gets the same rewind-suppression flag as the
    // body's back-edge. localCountAtStart is already correct here: the
    // increment clause (an expression or x++/x--) never declares locals.
    LoopCtx loop;
    loop.enclosing = currentLoop;
    loop.continueTarget = loopStart;
    loop.localCountAtStart = current->localCount;
    loop.breakCount = 0;
    loop.isSwitch = false;
    loop.escapes = false;
    loop.loopExitCount = 0;
    currentLoop = &loop;

    if (!check(TOKEN_RPAREN)) {
        int bodyJump = emitJump(OP_JUMP);
        int incrementStart = currentChunk()->count;
        if (!tryIncDecStatement(false)) {
            expression();
            emitByte(OP_POP);
        }
        emitLoop(loopStart);
        loopStart = incrementStart;
        loop.continueTarget = incrementStart;
        patchJump(bodyJump);
    }
    consume(TOKEN_RPAREN, "Expect ')' after for clauses.");

    statement();
    emitLoop(loopStart);

    if (exitJump != -1) {
        patchJump(exitJump);
        emitByte(OP_POP);
    }

    for (int i = 0; i < loop.breakCount; i++) patchJump(loop.breakJumps[i]);
    patchLoopFlags(&loop);
    currentLoop = loop.enclosing;

    endScope();
}

// switch (expr) { case c1: stmts... case c2: stmts... default: stmts... }
//
// C-like fallthrough: a case body with no 'break' runs into the next
// case's body. The switch value is evaluated once into a hidden local
// slot. A second hidden 'matched' flag (starts false) tracks whether some
// earlier case has already matched: once true, later case tests are
// skipped and their bodies just execute in order, which gives correct
// fallthrough without re-comparing (and without a later non-equal
// constant incorrectly stopping execution). 'default' always runs when
// reached in source order. 'break' reuses the LoopCtx machinery
// (isSwitch = true) so it jumps to the end of the switch; 'continue'
// inside a switch skips past it to find a real enclosing loop (handled
// in continueStatement).
static void switchStatement(void) {
    consume(TOKEN_LPAREN, "Expect '(' after 'switch'.");

    beginScope();

    Token hiddenVal;
    hiddenVal.type = TOKEN_IDENTIFIER;
    hiddenVal.start = "";
    hiddenVal.length = 0; // can't collide with any real identifier
    hiddenVal.line = parser.previous.line;

    expression();
    consume(TOKEN_RPAREN, "Expect ')' after switch value.");
    addLocal(hiddenVal, PTYPE_NONE);
    int valueSlot = current->localCount - 1;

    Token hiddenMatched;
    hiddenMatched.type = TOKEN_IDENTIFIER;
    hiddenMatched.start = "";
    hiddenMatched.length = 0;
    hiddenMatched.line = parser.previous.line;
    emitByte(OP_FALSE);
    addLocal(hiddenMatched, PTYPE_NONE);
    int matchedSlot = current->localCount - 1;

    consume(TOKEN_LBRACE, "Expect '{' after switch value.");

    LoopCtx loop;
    loop.enclosing = currentLoop;
    loop.continueTarget = -1; // unused: continue skips isSwitch contexts
    loop.localCountAtStart = current->localCount;
    loop.breakCount = 0;
    loop.isSwitch = true;
    currentLoop = &loop;

    bool defaultSeen = false;

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        if (match(TOKEN_CASE)) {
            expression(); // case constant
            consume(TOKEN_COLON, "Expect ':' after case value.");

            // if (matched) goto body;
            // else if (value == caseConst) { matched = true; goto body; }
            // else goto skipBody;
            emitBytes(OP_GET_LOCAL, (uint8_t)matchedSlot);
            int notYetMatchedJump = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP); // pop true
            int enterBodyAlreadyMatched = emitJump(OP_JUMP);

            patchJump(notYetMatchedJump);
            emitByte(OP_POP); // pop false

            emitBytes(OP_GET_LOCAL, (uint8_t)valueSlot);
            emitByte(OP_EQUAL);
            int notEqualJump = emitJump(OP_JUMP_IF_FALSE);
            emitByte(OP_POP); // pop true (comparison)
            emitByte(OP_TRUE);
            emitBytes(OP_SET_LOCAL, (uint8_t)matchedSlot);
            emitByte(OP_POP);
            int enterBodyJustMatched = emitJump(OP_JUMP);

            patchJump(notEqualJump);
            emitByte(OP_POP); // pop false (comparison)
            int skipBody = emitJump(OP_JUMP);

            patchJump(enterBodyAlreadyMatched);
            patchJump(enterBodyJustMatched);
            // -- body executes here --
            while (!check(TOKEN_CASE) && !check(TOKEN_DEFAULT) &&
                   !check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
                statement();
            }
            patchJump(skipBody);
        } else if (match(TOKEN_DEFAULT)) {
            if (defaultSeen) error("Multiple 'default' labels in one switch.");
            defaultSeen = true;
            consume(TOKEN_COLON, "Expect ':' after 'default'.");
            // Mark matched so that, absent a 'break', any case textually
            // following 'default' is entered unconditionally rather than
            // re-tested against the switch value (correct C fallthrough).
            emitByte(OP_TRUE);
            emitBytes(OP_SET_LOCAL, (uint8_t)matchedSlot);
            emitByte(OP_POP);
            while (!check(TOKEN_CASE) && !check(TOKEN_DEFAULT) &&
                   !check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
                statement();
            }
        } else {
            error("Expect 'case' or 'default' inside switch body.");
            advance();
        }
    }

    consume(TOKEN_RBRACE, "Expect '}' after switch body.");

    for (int i = 0; i < loop.breakCount; i++) patchJump(loop.breakJumps[i]);
    currentLoop = loop.enclosing;

    endScope(); // pops hidden 'matched' and switch-value locals
}

static void returnStatement(void) {
    if (match(TOKEN_SEMICOLON)) {
        emitByte(OP_NIL);
        emitPendingDefers();
        emitByte(OP_RETURN);
    } else {
        expression();
        consume(TOKEN_SEMICOLON, "Expect ';' after return value.");
        emitPendingDefers();
        emitByte(OP_RETURN);
    }
}

static void breakStatement(void) {
    if (currentLoop == NULL) {
        error("'break' used outside of a loop.");
        consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
        return;
    }
    int popCount = current->localCount - currentLoop->localCountAtStart;
    for (int i = 0; i < popCount; i++) emitByte(OP_POP);
    if (currentLoop->breakCount >= MAX_BREAKS) {
        error("Too many 'break' statements in one loop.");
    } else {
        currentLoop->breakJumps[currentLoop->breakCount++] = emitJump(OP_JUMP);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after 'break'.");
}

static void continueStatement(void) {
    LoopCtx* target = currentLoop;
    while (target != NULL && target->isSwitch) target = target->enclosing;
    if (target == NULL) {
        error("'continue' used outside of a loop.");
        consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
        return;
    }
    int popCount = current->localCount - target->localCountAtStart;
    for (int i = 0; i < popCount; i++) emitByte(OP_POP);
    emitLoop(target->continueTarget);
    consume(TOKEN_SEMICOLON, "Expect ';' after 'continue'.");
}

static void assertStatement(void) {
    consume(TOKEN_LPAREN, "Expect '(' after 'assert'.");
    expression();
    consume(TOKEN_RPAREN, "Expect ')' after assert condition.");
    consume(TOKEN_SEMICOLON, "Expect ';' after assert statement.");
    emitByte(OP_ASSERT);
}

static void panicStatement(void) {
    consume(TOKEN_LPAREN, "Expect '(' after 'panic'.");
    uint8_t argCount = argumentList();
    consume(TOKEN_SEMICOLON, "Expect ';' after panic statement.");
    emitBytes(OP_PANIC, argCount);
}

static void skipBalancedBraceBlock(void) {
    if (!match(TOKEN_LBRACE)) return;
    int depth = 1;
    while (depth > 0 && !check(TOKEN_EOF)) {
        if (check(TOKEN_LBRACE)) depth++;
        else if (check(TOKEN_RBRACE)) depth--;
        advance();
    }
}

// ---------------------------------------------------------------------
// defer { stmt; stmt; ... }
//
// Braces are mandatory (unlike bare-statement defer in some languages)
// so the end of the deferred block is unambiguous to locate via simple
// brace counting, without duplicating the full statement grammar just to
// find where it ends.
//
// There is no AST in this compiler -- everything compiles directly to
// bytecode in one pass -- so a deferred block can't be "recorded" as a
// tree and re-emitted later. Instead, its raw source text (the bytes
// between '{' and the matching '}') is copied into an owned,
// null-terminated buffer at the 'defer' site (which also validates it
// compiles cleanly there, matching normal error-reporting behavior).
// That buffer is then independently re-lexed and re-compiled, using the
// same save/restore-lexer-state technique loadStdlibModule uses for
// stdlib source files, at every point the function can exit: each
// 'return' statement, and the implicit fallthrough return at the end of
// the function body (see returnStatement / endCompiler). Deferred blocks
// run in LIFO order (most recently deferred runs first), matching the
// usual 'defer' semantics (e.g. Go).
//
// Deferred blocks are function-scoped: they belong to `current`
// (the innermost enclosing function's Compiler), not to a lexical {}
// block, so `defer` inside an `if`/`while` body still fires when the
// *function* returns, not when that inner block ends.
static char* captureBlockSource(int* outLen) {
    // Assumes the '{' has already been matched by the caller and we're
    // positioned right after it (parser.previous == the '{' token).
    const char* blockStart = parser.current.start;
    int depth = 1;
    // Walk tokens (not raw characters) so string/char literals containing
    // brace-like bytes can't confuse the depth count.
    while (depth > 0 && !check(TOKEN_EOF)) {
        if (check(TOKEN_LBRACE)) depth++;
        else if (check(TOKEN_RBRACE)) depth--;
        if (depth == 0) break; // stop before consuming the matching '}'
        advance();
    }
    const char* blockEnd = parser.current.start; // start of the matching '}'
    int len = (int)(blockEnd - blockStart);
    char* copy = malloc((size_t)len + 1);
    memcpy(copy, blockStart, (size_t)len);
    copy[len] = '\0';
    if (outLen) *outLen = len;
    return copy;
}

// Re-lexes and re-compiles a previously captured deferred block's source
// into the current chunk, using an isolated lexer/parser state so it
// doesn't disturb whatever position the real parser is at (same pattern
// as loadStdlibModule). Emitted inline at a function-exit point.
static void emitDeferredBlock(DeferredBlock* block) {
    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;

    initLexer(block->source);
    hasPeeked = false;
    advance(); // prime parser.current with the block's first token

    beginScope();
    while (!check(TOKEN_EOF)) {
        statement();
    }
    endScope();

    restoreLexerState(savedLexer);
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    hasPeeked = savedHasPeeked;
    peekedToken = savedPeeked;
}

// Emits every deferred block registered in the current function, in LIFO
// (most-recently-deferred-first) order. Called right before every
// OP_RETURN this compiler emits.
static void emitPendingDefers(void) {
    for (int i = current->deferCount - 1; i >= 0; i--) {
        emitDeferredBlock(&current->defers[i]);
    }
}

static void deferStatement(void) {
    if (current->enclosing == NULL) {
        // Top-level script: OP_RETURN still exists for it (see
        // compileProgram/endCompiler), so defer is technically well
        // defined there too, but there's no practical reason to support
        // it outside a function body, and disallowing it avoids subtle
        // interactions with 'use' pulling in stdlib top-level code.
        error("'defer' is only allowed inside a function body.");
        skipBalancedBraceBlock();
        match(TOKEN_SEMICOLON);
        return;
    }
    if (!check(TOKEN_LBRACE)) {
        errorAtCurrent("Expect '{' after 'defer' (defer requires a braced block).");
        return;
    }
    advance(); // consume '{'
    int len;
    char* source = captureBlockSource(&len);
    (void)len;
    consume(TOKEN_RBRACE, "Expect '}' to close 'defer' block.");
    match(TOKEN_SEMICOLON); // optional trailing ';' after the block

    if (current->deferCount >= MAX_DEFERS_PER_FUNCTION) {
        error("Too many 'defer' statements in one function.");
        free(source);
        return;
    }
    current->defers[current->deferCount].source = source;
    current->defers[current->deferCount].line = parser.previous.line;
    current->deferCount++;
}

static void notYetSupported(const char* feature) {
    char buf[128];
    snprintf(buf, sizeof(buf), "'%s' is not supported yet in this version of the Proton compiler.", feature);
    errorAtCurrent(buf);
    // best-effort resync: skip to the next '{...}' block or ';'
    while (!check(TOKEN_LBRACE) && !check(TOKEN_SEMICOLON) &&
           !check(TOKEN_EOF)) {
        advance();
    }
    if (check(TOKEN_LBRACE)) skipBalancedBraceBlock();
    else if (check(TOKEN_SEMICOLON)) advance();
}

static void block(void) {
    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        statement();
    }
    consume(TOKEN_RBRACE, "Expect '}' after block.");
}

static void statement(void) {
    if (tryIncDecStatement(true)) return;

    if (match(TOKEN_LBRACE)) {
        beginScope();
        block();
        endScope();
    } else if (match(TOKEN_VAR) || match(TOKEN_CONST)) {
        localVarDeclarationBody();
    } else if (match(TOKEN_IF)) {
        ifStatement();
    } else if (match(TOKEN_WHILE)) {
        whileStatement();
    } else if (match(TOKEN_FOR)) {
        forStatement();
    } else if (match(TOKEN_RETURN)) {
        returnStatement();
    } else if (match(TOKEN_BREAK)) {
        breakStatement();
    } else if (match(TOKEN_CONTINUE)) {
        continueStatement();
    } else if (match(TOKEN_ASSERT)) {
        assertStatement();
    } else if (match(TOKEN_PANIC)) {
        panicStatement();
    } else if (match(TOKEN_SWITCH)) {
        switchStatement();
    } else if (check(TOKEN_STRUCT) || check(TOKEN_ENUM)) {
        notYetSupported("struct/enum");
    } else if (match(TOKEN_DEFER)) {
        deferStatement();
    } else if (check(TOKEN_NEW) || check(TOKEN_DELETE)) {
        notYetSupported("new/delete (pointers & manual memory management)");
    } else if (check(TOKEN_SIZEOF) || check(TOKEN_TYPEOF)) {
        notYetSupported("sizeof/typeof");
    } else {
        expressionStatement();
    }
}

// ---------------------------------------------------------------------
// Top-level declarations
// ---------------------------------------------------------------------

static void initCompilerState(Compiler* compiler, Compiler* enclosing, ObjString* name) {
    compiler->enclosing = enclosing;
    compiler->function = newFunction();
    compiler->function->name = name;
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    compiler->deferCount = 0;
    current = compiler;
}

// ---------------------------------------------------------------------
// Global variable type table (compile-time only). Globals themselves are
// stored in the VM's runtime hash table with no type info attached, so we
// keep a side table here mapping name -> declared type, used to validate
// initializers (checked immediately, at compile time, since global inits
// must be literals) and later reassignments (checked via OP_CHECK_TYPE,
// emitted at the reassignment site in identifierExpr).
// ---------------------------------------------------------------------

#define MAX_GLOBAL_TYPES 256

// NOTE: entries are keyed by the *mangled* global name (e.g. "math.flag",
// or just "flag" at top-level with no active module), not the bare
// declared token text. Two different modules declaring a same-named
// global (e.g. both `nstest_a` and `nstest_b` declaring `flag`) must not
// collide in this table -- storing only the bare token text would let
// the second module's type entry silently overwrite/shadow the first's,
// causing type-checks on one module's global to run against the wrong
// type (this was a real bug: `mod_b.flag: string` got checked against
// `mod_a.flag: int32`'s type because both entries were keyed as bare
// "flag"). Mangling before storing/looking up mirrors exactly how the
// underlying global storage itself is keyed (see mangledGlobalName /
// vmDefineGlobal), so the two stay in lockstep.
typedef struct { char* chars; int length; ProtonType type; } GlobalTypeEntry;
static GlobalTypeEntry globalTypeTable[MAX_GLOBAL_TYPES];
static int globalTypeCount = 0;

typedef struct { const char* start; int length; } EnumNameEntry;
#define MAX_ENUM_NAMES 64
static EnumNameEntry enumNameTable[MAX_ENUM_NAMES];
static int enumNameCount = 0;

static void rememberEnumName(Token* name) {
    if (enumNameCount >= MAX_ENUM_NAMES) return;
    enumNameTable[enumNameCount].start = name->start;
    enumNameTable[enumNameCount].length = name->length;
    enumNameCount++;
}

static bool isKnownEnumName(Token* name) {
    for (int i = 0; i < enumNameCount; i++) {
        if (enumNameTable[i].length == name->length &&
            memcmp(enumNameTable[i].start, name->start, name->length) == 0) {
            return true;
        }
    }
    return false;
}

// (mangledGlobalName is forward-declared earlier in the file, near the
// other Pratt-parser forward decls -- see line ~366.)

static void rememberGlobalType(Token* name, ProtonType type) {
    if (type == PTYPE_NONE) return;
    if (globalTypeCount >= MAX_GLOBAL_TYPES) return; // not fatal, just unchecked past this point
    ObjString* mangled = mangledGlobalName(name);
    // Overwrite an existing entry for the same mangled name in place
    // (e.g. re-declaration), rather than appending a duplicate that
    // findGlobalType would never reach (it returns on first match).
    for (int i = 0; i < globalTypeCount; i++) {
        if (globalTypeTable[i].length == mangled->length &&
            memcmp(globalTypeTable[i].chars, mangled->chars, (size_t)mangled->length) == 0) {
            globalTypeTable[i].type = type;
            return;
        }
    }
    char* owned = malloc((size_t)mangled->length + 1);
    memcpy(owned, mangled->chars, (size_t)mangled->length);
    owned[mangled->length] = '\0';
    globalTypeTable[globalTypeCount].chars = owned;
    globalTypeTable[globalTypeCount].length = mangled->length;
    globalTypeTable[globalTypeCount].type = type;
    globalTypeCount++;
}

static ProtonType findGlobalType(Token* name) {
    ObjString* mangled = mangledGlobalName(name);
    for (int i = 0; i < globalTypeCount; i++) {
        if (globalTypeTable[i].length == mangled->length &&
            memcmp(globalTypeTable[i].chars, mangled->chars, (size_t)mangled->length) == 0) {
            return globalTypeTable[i].type;
        }
    }
    return PTYPE_NONE;
}

static bool isIntegerInRange(double n, double lo, double hi) {
    return n == floor(n) && n >= lo && n <= hi;
}

// Compile-time equivalent of the runtime OP_CHECK_TYPE check, used for
// global initializers (which are folded into Values at compile time
// rather than going through bytecode).
static void checkLiteralAgainstType(Value value, ProtonType type, Token* nameTok) {
    if (type == PTYPE_NONE || type == PTYPE_VOID) return;
    bool ok;
    switch (type) {
        case PTYPE_BOOL: ok = IS_BOOL(value); break;
        case PTYPE_STRING: ok = IS_STRING(value); break;
        case PTYPE_CHAR:
            ok = (IS_STRING(value) && AS_STRING(value)->length == 1) ||
                 (IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), 0, 0x10FFFF));
            break;
        case PTYPE_BYTE:
        case PTYPE_UINT8: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), 0, 255); break;
        case PTYPE_INT8: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), -128, 127); break;
        case PTYPE_INT16:
        case PTYPE_SHORT: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), -32768, 32767); break;
        case PTYPE_UINT16: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), 0, 65535); break;
        case PTYPE_INT32:
        case PTYPE_INT: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), -2147483648.0, 2147483647.0); break;
        case PTYPE_UINT32:
        case PTYPE_UINT: ok = IS_NUMBER(value) && isIntegerInRange(AS_NUMBER(value), 0, 4294967295.0); break;
        case PTYPE_INT64:
        case PTYPE_LONG:
            // NUM_I64 values are exact int64 by construction. A NUM_U64
            // value fits int64 only if it's <= INT64_MAX. NUM_F64 falls
            // back to the double-range check (double-safe-integer bound is
            // the honest limit there, same caveat as the runtime path).
            ok = IS_NUMBER(value) && (
                     NUM_KIND(value) == NUM_I64 ||
                     (NUM_KIND(value) == NUM_U64 && AS_U64(value) <= (uint64_t)INT64_MAX) ||
                     (NUM_KIND(value) == NUM_F64 && isIntegerInRange(AS_NUMBER(value), -9007199254740991.0, 9007199254740991.0)));
            break;
        case PTYPE_UINT64:
            ok = IS_NUMBER(value) && (
                     NUM_KIND(value) == NUM_U64 ||
                     (NUM_KIND(value) == NUM_I64 && AS_I64(value) >= 0) ||
                     (NUM_KIND(value) == NUM_F64 && isIntegerInRange(AS_NUMBER(value), 0, 9007199254740991.0)));
            break;
        case PTYPE_FLOAT32:
        case PTYPE_FLOAT:
        case PTYPE_FLOAT64:
        case PTYPE_DOUBLE:
        case PTYPE_DECIMAL: ok = IS_NUMBER(value); break;
        default: ok = true; break;
    }
    if (!ok) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Global '%.*s' initializer doesn't match declared type '%s'.",
                 nameTok->length, nameTok->start, protonTypeName(type));
        error(buf);
    }
}

static Value constLiteralExpr(void) {
    bool neg = false;
    if (match(TOKEN_MINUS)) neg = true;
    if (match(TOKEN_NUMBER)) {
        const char* start = parser.previous.start;
        int length = parser.previous.length;
        if (isFloatLiteralText(start, length)) {
            double v = strtod(start, NULL);
            return NUMBER_VAL(neg ? -v : v);
        }
        errno = 0;
        long long v = strtoll(start, NULL, 10);
        if (errno == ERANGE && !neg) {
            errno = 0;
            unsigned long long uv = strtoull(start, NULL, 10);
            return UINT64_VAL(uv);
        }
        if (neg) v = -v;
        if (v < INT32_MIN || v > INT32_MAX) {
            return INT64_VAL(v);
        }
        return INT32_VAL(v);
    }
    if (match(TOKEN_STRING)) {
        if (neg) error("Cannot negate a string literal.");
        int len;
        char* chars = processEscapes(parser.previous.start, parser.previous.length, &len);
        return OBJ_VAL(takeString(chars, len));
    }
    if (match(TOKEN_TRUE)) return BOOL_VAL(true);
    if (match(TOKEN_FALSE)) return BOOL_VAL(false);
    error("Expect a constant literal (number, string, or boolean) as a global initializer.");
    return NIL_VAL;
}

// ---------------------------------------------------------------------
// Generics: fn name<T,...>(...) { ... } and struct Name<T,...> { ... }
//
// This compiler has no AST -- source goes straight to bytecode in one
// pass -- so there's no type-parameterized IR to type-check once and
// stamp out at every call site. Instead, a generic declaration's raw
// source text is captured verbatim (same "capture between braces, later
// re-lex it in an isolated parser state" technique already used for
// `defer` blocks and stdlib module loading, see captureBlockSource /
// loadStdlibModule) and stored as a GenericTemplate, WITHOUT compiling
// it -- the type parameter names (T, K, ...) aren't real types, so
// there's nothing valid to emit yet.
//
// The first time a call site or struct literal actually uses the
// generic with a concrete list of type arguments (e.g. `max<int>(3,5)`
// or `Box<string>{ value = "hi"; }`), the type parameter names are
// substituted for the concrete type tokens throughout the captured
// source (whole-token match only, so a local variable named the same as
// a type parameter inside the body is not touched unless it really is
// that identifier -- see substituteGenericParams), the result is
// re-lexed, and compiled as an ordinary fn/struct under a mangled name
// (`max$int`, `Box$string`). Later calls with the same type arguments
// reuse the same compiled instantiation (memoized by mangled name).
// ---------------------------------------------------------------------

#define MAX_GENERIC_TEMPLATES 32
#define MAX_GENERIC_INSTANTIATIONS 128

typedef struct GenericTemplate {
    const char* start;
    int length;
    bool isStruct;
    char* paramNames[MAX_GENERIC_PARAMS];
    int paramNameLens[MAX_GENERIC_PARAMS];
    int paramCount;
    char* source; // captured raw text between the declaration's braces, owned
    int line;
} GenericTemplate;

static GenericTemplate genericTemplates[MAX_GENERIC_TEMPLATES];
static int genericTemplateCount = 0;

// Mangled instantiation names must live at a stable address for as long
// as the compiler runs, since structTable[] entries and OP_CALL constant
// strings end up pointing at (or copying from) this text. Storing each
// mangled name in its own malloc'd slot here (rather than a stack buffer)
// is what makes that safe.
typedef struct {
    char* mangledName;
} GenericInstantiation;

static GenericInstantiation genericInstantiations[MAX_GENERIC_INSTANTIATIONS];
static int genericInstantiationCount = 0;

static GenericTemplate* findGenericTemplate(Token* name) {
    for (int i = 0; i < genericTemplateCount; i++) {
        if (genericTemplates[i].length == name->length &&
            memcmp(genericTemplates[i].start, name->start, (size_t)name->length) == 0) {
            return &genericTemplates[i];
        }
    }
    return NULL;
}

static bool isKnownGenericFn(Token* name) {
    GenericTemplate* t = findGenericTemplate(name);
    return t != NULL && !t->isStruct;
}

static bool isKnownGenericStruct(Token* name) {
    GenericTemplate* t = findGenericTemplate(name);
    return t != NULL && t->isStruct;
}

static const char* findMemoizedInstantiation(const char* mangledName) {
    for (int i = 0; i < genericInstantiationCount; i++) {
        if (strcmp(genericInstantiations[i].mangledName, mangledName) == 0) {
            return genericInstantiations[i].mangledName;
        }
    }
    return NULL;
}

// Stores mangledName (already-built, heap text this function takes
// ownership of) in the instantiation table and returns the table's own
// stable copy -- callers should use the returned pointer, not their own,
// from this point on.
static const char* rememberInstantiation(char* mangledName) {
    if (genericInstantiationCount >= MAX_GENERIC_INSTANTIATIONS) {
        error("Too many generic instantiations.");
        return mangledName;
    }
    genericInstantiations[genericInstantiationCount].mangledName = mangledName;
    genericInstantiationCount++;
    return mangledName;
}

// Builds "name$Type1$Type2..." into a freshly malloc'd buffer.
static char* mangleGenericName(Token* name, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]) {
    int len = name->length;
    for (int i = 0; i < argCount; i++) len += 1 + typeArgs[i].length;
    char* buf = malloc((size_t)len + 1);
    int pos = 0;
    memcpy(buf + pos, name->start, (size_t)name->length); pos += name->length;
    for (int i = 0; i < argCount; i++) {
        buf[pos++] = '$';
        memcpy(buf + pos, typeArgs[i].start, (size_t)typeArgs[i].length);
        pos += typeArgs[i].length;
    }
    buf[pos] = '\0';
    return buf;
}

// Speculatively parses `< Type , Type , ... >` starting at the current
// token (which must be TOKEN_LESS). On success, advances past the `>`
// and fills outArgs/outCount, returning true. On any failure (not a
// clean type-argument list), restores the parser/lexer to exactly where
// it was before this call and returns false -- callers must then fall
// through to normal (non-generic) handling, e.g. treating `<` as the
// comparison operator.
static bool tryParseGenericArgsSpeculative(int* outCount, Token outArgs[MAX_GENERIC_PARAMS]) {
    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;
    bool savedHadError = parser.hadError;
    bool savedPanicMode = parser.panicMode;

    bool ok = true;
    int count = 0;
    advance(); // consume '<'
    if (check(TOKEN_GREATER)) {
        ok = false; // `<>` is not a valid type-argument list
    } else {
        do {
            if (!check(TOKEN_TYPE_NAME) && !check(TOKEN_IDENTIFIER)) { ok = false; break; }
            if (count >= MAX_GENERIC_PARAMS) { ok = false; break; }
            advance();
            outArgs[count++] = parser.previous;
        } while (ok && match(TOKEN_COMMA));
    }
    if (ok && !match(TOKEN_GREATER)) ok = false;

    if (!ok || parser.hadError) {
        // Restore everything -- this was never a generic argument list.
        restoreLexerState(savedLexer);
        parser.current = savedCurrent;
        parser.previous = savedPrevious;
        hasPeeked = savedHasPeeked;
        peekedToken = savedPeeked;
        parser.hadError = savedHadError;
        parser.panicMode = savedPanicMode;
        return false;
    }
    if (outCount) *outCount = count;
    return true;
}

// Re-lexes template->source, copying every token verbatim into `out`
// EXCEPT whole-token matches of a type parameter name, which are replaced
// with the corresponding concrete type argument's text. Whole-token match
// only (never a substring inside a longer identifier), so a template
// parameter named `T` never touches a local variable named `Type` or a
// struct field named `Total`.
static char* substituteGenericParams(GenericTemplate* tmpl, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]) {
    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;

    initLexer(tmpl->source);
    hasPeeked = false;

    size_t cap = strlen(tmpl->source) + 256;
    char* out = malloc(cap);
    size_t outLen = 0;

    for (;;) {
        Token t = scanToken();
        if (t.type == TOKEN_EOF) break;

        const char* replStart = t.start;
        int replLen = t.length;
        if (t.type == TOKEN_IDENTIFIER) {
            for (int i = 0; i < tmpl->paramCount && i < argCount; i++) {
                if (tmpl->paramNameLens[i] == t.length &&
                    memcmp(tmpl->paramNames[i], t.start, (size_t)t.length) == 0) {
                    replStart = typeArgs[i].start;
                    replLen = typeArgs[i].length;
                    break;
                }
            }
        }

        // Grow if needed (+1 for a separating space we may add, +1 for NUL).
        while (outLen + (size_t)replLen + 2 > cap) {
            cap *= 2;
            out = realloc(out, cap);
        }
        // A single space between tokens is always a safe separator here:
        // it can only ever widen a gap the lexer already tolerated (all
        // whitespace is insignificant to this grammar), never fuse two
        // tokens together the way concatenating with no separator could
        // (e.g. `T` followed by `x` becoming `Tx`).
        if (outLen > 0) out[outLen++] = ' ';
        memcpy(out + outLen, replStart, (size_t)replLen);
        outLen += (size_t)replLen;
    }
    out[outLen] = '\0';

    restoreLexerState(savedLexer);
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    hasPeeked = savedHasPeeked;
    peekedToken = savedPeeked;

    return out;
}

// Forward decls for the two out-of-line helpers pulled out of
// funDeclaration / structDeclaration so both the normal (non-generic)
// declaration path and generic instantiation can share them.
static void compileFnBody(ObjString* nameStr);
static void registerStructFromSource(Token nameTok);

static const char* compileGenericFnInstantiation(Token* name, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]) {
    char* mangled = mangleGenericName(name, argCount, typeArgs);
    const char* existing = findMemoizedInstantiation(mangled);
    if (existing != NULL) { free(mangled); return existing; }

    GenericTemplate* tmpl = findGenericTemplate(name);
    if (tmpl == NULL) {
        error("Unknown generic function.");
        return rememberInstantiation(mangled);
    }
    if (argCount != tmpl->paramCount) {
        error("Wrong number of type arguments for generic function.");
        return rememberInstantiation(mangled);
    }

    const char* stableMangled = rememberInstantiation(mangled);
    char* substituted = substituteGenericParams(tmpl, argCount, typeArgs);

    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;

    initLexer(substituted);
    hasPeeked = false;
    advance(); // prime parser.current with the substituted body's first token

    ObjString* nameStr = copyString(stableMangled, (int)strlen(stableMangled));
    compileFnBody(nameStr);

    restoreLexerState(savedLexer);
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    hasPeeked = savedHasPeeked;
    peekedToken = savedPeeked;
    free(substituted);

    return stableMangled;
}

static const char* compileGenericStructInstantiation(Token* name, int argCount, Token typeArgs[MAX_GENERIC_PARAMS]) {
    char* mangled = mangleGenericName(name, argCount, typeArgs);
    const char* existing = findMemoizedInstantiation(mangled);
    if (existing != NULL) { free(mangled); return existing; }

    GenericTemplate* tmpl = findGenericTemplate(name);
    if (tmpl == NULL) {
        error("Unknown generic struct.");
        return rememberInstantiation(mangled);
    }
    if (argCount != tmpl->paramCount) {
        error("Wrong number of type arguments for generic struct.");
        return rememberInstantiation(mangled);
    }

    const char* stableMangled = rememberInstantiation(mangled);
    char* substitutedFields = substituteGenericParams(tmpl, argCount, typeArgs);
    // registerStructFromSource expects to consume() the struct's opening
    // '{' itself (mirroring the non-generic structDeclaration path), but
    // the captured template source is only the field-list text between
    // the braces (see registerGenericStructTemplate's captureBlockSource
    // call) -- so re-wrap it here before re-lexing.
    size_t substLen = strlen(substitutedFields);
    char* substituted = malloc(substLen + 3);
    substituted[0] = '{';
    memcpy(substituted + 1, substitutedFields, substLen);
    substituted[substLen + 1] = '}';
    substituted[substLen + 2] = '\0';
    free(substitutedFields);

    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;

    initLexer(substituted);
    hasPeeked = false;
    advance();

    Token mangledTok;
    mangledTok.type = TOKEN_IDENTIFIER;
    mangledTok.start = stableMangled;
    mangledTok.length = (int)strlen(stableMangled);
    mangledTok.line = name->line;
    registerStructFromSource(mangledTok);

    restoreLexerState(savedLexer);
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    hasPeeked = savedHasPeeked;
    peekedToken = savedPeeked;
    // Intentionally never freed: structTable[]'s field-name tokens (see
    // registerStructFromSource) keep pointers into `substituted` for the
    // rest of compilation, exactly like loadStdlibModule's module source
    // buffer and captureBlockSource's captured defer-block text.

    return stableMangled;
}

static void globalVarDeclaration(bool isPrivate) {
    consume(TOKEN_IDENTIFIER, "Expect variable name.");
    Token name = parser.previous;
    consume(TOKEN_COLON, "Expect ':' after variable name.");
    ProtonType type = parseType();

    Value value = NIL_VAL;
    if (match(TOKEN_ASSIGN)) {
        value = constLiteralExpr();
        checkLiteralAgainstType(value, type, &name);
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after global variable declaration.");
    rememberGlobalType(&name, type);

    if (isPrivate && currentModulePrefix() == NULL) {
        error("'private' can only be used inside a module (a file loaded via 'use').");
    }

    ObjString* nameStr = mangledGlobalName(&name);
    if (isPrivate) registerPrivateMember(nameStr);
    vmDefineGlobal(nameStr, value);
}

// enum Name { A, B, C }              -> A=0, B=1, C=2
// enum Name { A, B = 5, C }          -> A=0, B=5, C=6 (continues from override)
//
// Each member is stored as a VM global named "Name.Member" (the '.' is
// just a literal byte in the hash key -- there's no general namespacing
// mechanism yet, so this piggybacks on the existing flat global table).
// identifierExpr() recognizes `Name.Member` for any name recorded in
// enumNameTable and compiles it straight to OP_GET_GLOBAL against the
// mangled key; member access is read-only (no assignment path is wired
// up for it), matching normal enum semantics.
static void enumDeclaration(void) {
    consume(TOKEN_ENUM, "Expect 'enum'.");
    consume(TOKEN_IDENTIFIER, "Expect enum name.");
    Token enumName = parser.previous;
    rememberEnumName(&enumName);

    consume(TOKEN_LBRACE, "Expect '{' after enum name.");

    int32_t nextValue = 0;
    if (!check(TOKEN_RBRACE)) {
        do {
            if (check(TOKEN_RBRACE)) break; // trailing comma
            consume(TOKEN_IDENTIFIER, "Expect enum member name.");
            Token member = parser.previous;

            int32_t value = nextValue;
            if (match(TOKEN_ASSIGN)) {
                bool neg = match(TOKEN_MINUS);
                consume(TOKEN_NUMBER, "Expect integer constant after '=' in enum member.");
                long v = strtol(parser.previous.start, NULL, 10);
                value = (int32_t)(neg ? -v : v);
            }
            nextValue = value + 1;

            // Build the mangled "EnumName.Member" key.
            int mangledLen = enumName.length + 1 + member.length;
            char* mangled = malloc((size_t)mangledLen + 1);
            memcpy(mangled, enumName.start, enumName.length);
            mangled[enumName.length] = '.';
            memcpy(mangled + enumName.length + 1, member.start, member.length);
            mangled[mangledLen] = '\0';

            ObjString* nameStr = copyString(mangled, mangledLen);
            free(mangled);
            vmDefineGlobal(nameStr, INT32_VAL(value));
        } while (match(TOKEN_COMMA));
    }

    consume(TOKEN_RBRACE, "Expect '}' after enum body.");
    match(TOKEN_SEMICOLON); // optional trailing ';', matching struct-like decls
}

// ---------------------------------------------------------------------
// struct declarations: struct Name { field: type; field: type; ... }
//
// A struct is purely a compile-time shape: it names a fixed set of
// fields. Instances are represented at runtime as an ObjMap (the same
// string-keyed hash table object used for map literals) -- no new value
// representation is needed, since a struct instance's field access is
// identical to a map's string-keyed indexing. This table just remembers,
// per struct name, the declared field names (in order) so struct-literal
// expressions (see structLiteral below) can fill in `nil` for any field
// the literal didn't explicitly set, giving every instance of a given
// struct the same consistent set of keys.
// ---------------------------------------------------------------------

#define MAX_STRUCT_TYPES 32

typedef struct {
    const char* start;
    int length;
} StructFieldName;

typedef struct {
    const char* start;
    int length;
    StructFieldName fields[MAX_STRUCT_FIELDS];
    int fieldCount;
} StructTypeEntry;

static StructTypeEntry structTable[MAX_STRUCT_TYPES];
static int structTypeCount = 0;

static StructTypeEntry* findStructType(Token* name) {
    for (int i = 0; i < structTypeCount; i++) {
        if (structTable[i].length == name->length &&
            memcmp(structTable[i].start, name->start, name->length) == 0) {
            return &structTable[i];
        }
    }
    return NULL;
}

static bool isKnownStructName(Token* name) {
    return findStructType(name) != NULL;
}

static int structFieldCount(Token* name) {
    StructTypeEntry* entry = findStructType(name);
    return entry ? entry->fieldCount : 0;
}

static Token structFieldNameAt(Token* name, int index) {
    StructTypeEntry* entry = findStructType(name);
    Token t;
    t.type = TOKEN_IDENTIFIER;
    t.line = 0;
    if (entry == NULL || index < 0 || index >= entry->fieldCount) {
        t.start = "";
        t.length = 0;
        return t;
    }
    t.start = entry->fields[index].start;
    t.length = entry->fields[index].length;
    return t;
}

// Registers a struct type from '{' onward (the name has already been
// consumed and decided by the caller -- either the real declared name,
// or a generic instantiation's mangled name). Shared by structDeclaration
// and compileGenericStructInstantiation.
static void registerStructFromSource(Token nameTok) {
    if (findStructType(&nameTok) != NULL) {
        error("A struct with this name already exists.");
    }
    if (structTypeCount >= MAX_STRUCT_TYPES) {
        error("Too many struct types declared.");
        return;
    }
    StructTypeEntry* entry = &structTable[structTypeCount++];
    entry->start = nameTok.start;
    entry->length = nameTok.length;
    entry->fieldCount = 0;

    consume(TOKEN_LBRACE, "Expect '{' after struct name.");

    while (!check(TOKEN_RBRACE) && !check(TOKEN_EOF)) {
        consume(TOKEN_IDENTIFIER, "Expect field name.");
        Token fieldName = parser.previous;
        consume(TOKEN_COLON, "Expect ':' after field name.");
        parseType(); // field type -- parsed for documentation/forward
                     // compatibility, not runtime-enforced yet (same
                     // status as function param/return types on custom
                     // names elsewhere in this compiler).
        consume(TOKEN_SEMICOLON, "Expect ';' after field declaration.");

        if (entry->fieldCount >= MAX_STRUCT_FIELDS) {
            error("Too many fields in one struct.");
        } else {
            entry->fields[entry->fieldCount].start = fieldName.start;
            entry->fields[entry->fieldCount].length = fieldName.length;
            entry->fieldCount++;
        }
    }

    consume(TOKEN_RBRACE, "Expect '}' after struct body.");
    match(TOKEN_SEMICOLON); // optional trailing ';'
}

// Registers a new generic struct template: captures `<T,...>` param names
// and the raw source between the struct's braces, WITHOUT compiling it
// (see the generics engine comment above for why). Assumes 'struct Name'
// has already been consumed and parser.current is TOKEN_LESS.
static void registerGenericStructTemplate(Token structName) {
    if (genericTemplateCount >= MAX_GENERIC_TEMPLATES) {
        error("Too many generic templates declared.");
        return;
    }
    GenericTemplate* tmpl = &genericTemplates[genericTemplateCount];
    tmpl->start = structName.start;
    tmpl->length = structName.length;
    tmpl->isStruct = true;
    tmpl->paramCount = 0;
    tmpl->line = structName.line;

    consume(TOKEN_LESS, "Expect '<' to start generic parameter list.");
    do {
        consume(TOKEN_IDENTIFIER, "Expect type parameter name.");
        Token p = parser.previous;
        if (tmpl->paramCount < MAX_GENERIC_PARAMS) {
            tmpl->paramNames[tmpl->paramCount] = malloc((size_t)p.length + 1);
            memcpy(tmpl->paramNames[tmpl->paramCount], p.start, (size_t)p.length);
            tmpl->paramNames[tmpl->paramCount][p.length] = '\0';
            tmpl->paramNameLens[tmpl->paramCount] = p.length;
            tmpl->paramCount++;
        } else {
            error("Too many generic type parameters.");
        }
    } while (match(TOKEN_COMMA));
    consume(TOKEN_GREATER, "Expect '>' after generic parameter list.");

    consume(TOKEN_LBRACE, "Expect '{' after generic struct header.");
    int len;
    char* source = captureBlockSource(&len);
    (void)len;
    consume(TOKEN_RBRACE, "Expect '}' to close generic struct body.");
    match(TOKEN_SEMICOLON);

    tmpl->source = source;
    genericTemplateCount++;
}

static void structDeclaration(void) {
    consume(TOKEN_STRUCT, "Expect 'struct'.");
    consume(TOKEN_IDENTIFIER, "Expect struct name.");
    Token structName = parser.previous;

    if (check(TOKEN_LESS)) {
        registerGenericStructTemplate(structName);
        return;
    }

    registerStructFromSource(structName);
}

// Compiles a function body from '(' onward under the given (already
// decided) global name -- either the real declared name, or a generic
// instantiation's mangled name. Shared by funDeclaration and
// compileGenericFnInstantiation.
static void compileFnBody(ObjString* nameStr) {
    Compiler compiler;
    initCompilerState(&compiler, current, nameStr);
    beginScope();

    ProtonType paramTypes[UINT8_COUNT];
    int paramCount = 0;

    consume(TOKEN_LPAREN, "Expect '(' after function name.");
    if (!check(TOKEN_RPAREN)) {
        do {
            consume(TOKEN_IDENTIFIER, "Expect parameter name.");
            Token paramName = parser.previous;
            consume(TOKEN_COLON, "Expect ':' after parameter name.");
            ProtonType paramType = parseType();
            declareLocal(paramName, paramType);
            if (paramCount < UINT8_COUNT) paramTypes[paramCount++] = paramType;
            current->function->arity++;
        } while (match(TOKEN_COMMA));
    }
    consume(TOKEN_RPAREN, "Expect ')' after parameters.");

    if (match(TOKEN_COLON)) {
        parseType(); // return type, not semantically checked yet
    }

    consume(TOKEN_LBRACE, "Expect '{' before function body.");
    // Validate typed parameters against whatever the caller actually passed,
    // right as the function starts executing (params already sit in their
    // local slots 0..paramCount-1 courtesy of OP_CALL).
    for (int i = 0; i < paramCount; i++) {
        if (paramTypes[i] == PTYPE_NONE) continue;
        emitBytes(OP_GET_LOCAL, (uint8_t)i);
        emitTypeCheck(paramTypes[i]);
        emitByte(OP_POP);
    }
    block();

    ObjFunction* function = endCompiler();
    vmDefineGlobal(nameStr, OBJ_VAL(function));
}

// Registers a new generic function template: captures `<T,...>` param
// names and the raw source of the body's braced block, WITHOUT
// compiling it. Assumes the function name has already been consumed and
// parser.current is TOKEN_LESS.
static void registerGenericFnTemplate(Token nameTok) {
    if (genericTemplateCount >= MAX_GENERIC_TEMPLATES) {
        error("Too many generic templates declared.");
        return;
    }
    GenericTemplate* tmpl = &genericTemplates[genericTemplateCount];
    tmpl->start = nameTok.start;
    tmpl->length = nameTok.length;
    tmpl->isStruct = false;
    tmpl->paramCount = 0;
    tmpl->line = nameTok.line;

    consume(TOKEN_LESS, "Expect '<' to start generic parameter list.");
    do {
        consume(TOKEN_IDENTIFIER, "Expect type parameter name.");
        Token p = parser.previous;
        if (tmpl->paramCount < MAX_GENERIC_PARAMS) {
            tmpl->paramNames[tmpl->paramCount] = malloc((size_t)p.length + 1);
            memcpy(tmpl->paramNames[tmpl->paramCount], p.start, (size_t)p.length);
            tmpl->paramNames[tmpl->paramCount][p.length] = '\0';
            tmpl->paramNameLens[tmpl->paramCount] = p.length;
            tmpl->paramCount++;
        } else {
            error("Too many generic type parameters.");
        }
    } while (match(TOKEN_COMMA));
    consume(TOKEN_GREATER, "Expect '>' after generic parameter list.");

    // Capture from '(' through the matching final '}' as one raw block:
    // params, optional return type, and body all get textually
    // substituted and re-parsed together at instantiation time via
    // compileFnBody, so the captured text needs to include everything
    // from '(' onward, not just the {}-delimited body. Reuse the same
    // token-depth-counting technique as captureBlockSource, but starting
    // depth at 0 and stopping at the '{' that begins depth-tracking, so
    // the capture naturally spans '(...) : type { ... }' as a whole.
    const char* captureStart = parser.current.start;
    int parenDepth = 0;
    int braceDepth = 0;
    bool sawOpenBrace = false;
    for (;;) {
        if (check(TOKEN_EOF)) { error("Unterminated generic function body."); break; }
        if (check(TOKEN_LPAREN)) parenDepth++;
        else if (check(TOKEN_RPAREN)) parenDepth--;
        else if (check(TOKEN_LBRACE)) { braceDepth++; sawOpenBrace = true; }
        else if (check(TOKEN_RBRACE)) {
            braceDepth--;
            if (sawOpenBrace && braceDepth == 0 && parenDepth <= 0) {
                advance(); // consume the final '}' so capture includes it
                break;
            }
        }
        advance();
    }
    const char* captureEnd = parser.previous.start + parser.previous.length;
    int len = (int)(captureEnd - captureStart);
    char* source = malloc((size_t)len + 1);
    memcpy(source, captureStart, (size_t)len);
    source[len] = '\0';
    match(TOKEN_SEMICOLON); // optional trailing ';'

    tmpl->source = source;
    genericTemplateCount++;
}

static void funDeclaration(bool isPrivate) {
    consume(TOKEN_IDENTIFIER, "Expect function name.");
    Token nameTok = parser.previous;

    if (check(TOKEN_LESS)) {
        if (isPrivate) error("'private' is not yet supported on generic functions.");
        registerGenericFnTemplate(nameTok);
        return;
    }

    if (isPrivate && currentModulePrefix() == NULL) {
        error("'private' can only be used inside a module (a file loaded via 'use').");
    }

    ObjString* nameStr = mangledGlobalName(&nameTok);
    if (isPrivate) registerPrivateMember(nameStr);
    compileFnBody(nameStr);
}

// ---------------------------------------------------------------------
// `use` module loading with real namespacing.
//
// `use math;` looks for `stdlib/math.prt` (relative to the current
// working directory) and, if found, compiles its top-level declarations
// right at the `use` site -- but unlike the original flat-paste
// behavior, every top-level fn/var/const name the module defines is
// mangled with a `math.` prefix before being registered as a global
// (`sqrt` -> global name `"math.sqrt"`), using the exact same
// "flat-string-key namespacing" mechanic enum member access already
// established (`EnumName.Member` -> global `"EnumName.Member"`, see
// enumDeclaration/identifierExpr above). `math.sqrt(x)` at the call site
// then compiles straight to an OP_CALL/OP_GET_GLOBAL against that
// mangled key (see the module-member-access branch in identifierExpr).
//
// `activeModulePrefix` is a simple side channel (matches the style of
// `pendingArrayElemType` elsewhere in this file): funDeclaration/
// compileFnBody and globalVarDeclaration check it and, if set, mangle
// the name they're about to hand to vmDefineGlobal/rememberGlobalType.
// It's set for the duration of compiling one module's top-level
// declarations and cleared (restored, to support nested `use` inside a
// module) immediately after.
//
// `private fn`/`private var`/`private const` inside a module's top-level
// body registers the member's mangled name (e.g. "math.helper") in
// privateMemberRegistry; the `moduleName.member` access branch in
// identifierExpr rejects it unless the access happens while compiling
// that same module's own body (self-access is always allowed, matching
// how private members work in most languages). There's still no
// export/visibility control for struct/enum -- only fn/var/const.
//
// `use math as m;` registers an alias binding (display name "m" ->
// real prefix "math") instead of just a name -> itself: every mangled
// global key still uses the real module name ("math.sqrt"), so multiple
// aliases of the same module (or the module loaded once, aliased
// differently at different `use` sites) all resolve to the same
// underlying storage.
//
// A missing module file is now a compile error (`use foo;` with no
// stdlib/foo.prt is a mistake, not a valid "not installed yet"
// declaration) -- with one carve-out: `io` is a compiler-builtin
// pseudo-namespace with no backing file and its own hardcoded dispatch
// (see the `io.out`/`io.in` special case in identifierExpr); `use io;`
// stays a recognized no-op and `io` is deliberately never registered as
// a generic module namespace (doing so would shadow the hardcoded
// dispatch and break `io.out`/`io.in`).
// ---------------------------------------------------------------------

static const char* activeModulePrefix = NULL; // e.g. "math" while compiling stdlib/math.prt's body

// Accessor so identifierExpr (defined earlier in the file, before this
// state exists lexically) can consult it for intra-module calls -- see
// the plain-call-by-name fallback in identifierExpr below.
static const char* currentModulePrefix(void) { return activeModulePrefix; }

// Builds "prefix.name" into an interned ObjString, or just "name" if no
// module prefix is currently active. This is the single choke point
// funDeclaration/compileFnBody and globalVarDeclaration call instead of
// `copyString(name.start, name.length)` directly, so every top-level
// name defined while inside a `use`d module's body picks up its prefix.
static ObjString* mangledGlobalName(Token* name) {
    if (activeModulePrefix == NULL) {
        return copyString(name->start, name->length);
    }
    int prefixLen = (int)strlen(activeModulePrefix);
    int mangledLen = prefixLen + 1 + name->length;
    char* mangled = malloc((size_t)mangledLen + 1);
    memcpy(mangled, activeModulePrefix, (size_t)prefixLen);
    mangled[prefixLen] = '.';
    memcpy(mangled + prefixLen + 1, name->start, (size_t)name->length);
    mangled[mangledLen] = '\0';
    ObjString* result = copyString(mangled, mangledLen);
    free(mangled);
    return result;
}

// Registers `name` (already mangled, e.g. "math.helper") as private if
// a module is currently being compiled. Called from funDeclaration /
// globalVarDeclaration when the `private` modifier was present.
#define MAX_PRIVATE_MEMBERS 128
static char privateMemberRegistry[MAX_PRIVATE_MEMBERS][128];
static int privateMemberCount = 0;

static void registerPrivateMember(ObjString* mangledName) {
    if (privateMemberCount >= MAX_PRIVATE_MEMBERS) {
        error("Too many 'private' module members declared.");
        return;
    }
    snprintf(privateMemberRegistry[privateMemberCount], sizeof(privateMemberRegistry[0]),
             "%.*s", mangledName->length, mangledName->chars);
    privateMemberCount++;
}

static bool isPrivateMangledName(const char* chars, int length) {
    for (int i = 0; i < privateMemberCount; i++) {
        size_t len = strlen(privateMemberRegistry[i]);
        if ((size_t)length == len && memcmp(privateMemberRegistry[i], chars, len) == 0) {
            return true;
        }
    }
    return false;
}

#define MAX_LOADED_MODULES 32
static char loadedModules[MAX_LOADED_MODULES][64];
static int loadedModuleCount = 0;

// Known module namespaces (distinct from loadedModules: this is what
// identifierExpr's `moduleName.member` branch checks to recognize a
// bare identifier as a namespace prefix, not just load-once
// bookkeeping). Each entry maps the identifier used at call sites
// (`alias`, e.g. "m") to the real mangled-key prefix (`prefix`, e.g.
// "math") -- distinct fields so `use math as m;` doesn't require every
// mangled global to be renamed, just the lookup.
#define MAX_MODULE_NAMES 32
typedef struct {
    char alias[64];
    char prefix[64];
} ModuleBinding;
static ModuleBinding moduleRegistry[MAX_MODULE_NAMES];
static int moduleRegistryCount = 0;

static ModuleBinding* findModuleBinding(Token* name) {
    for (int i = 0; i < moduleRegistryCount; i++) {
        size_t len = strlen(moduleRegistry[i].alias);
        if ((size_t)name->length == len && memcmp(moduleRegistry[i].alias, name->start, len) == 0) {
            return &moduleRegistry[i];
        }
    }
    return NULL;
}

static bool isKnownModuleName(Token* name) { return findModuleBinding(name) != NULL; }

// Returns the real mangled-key prefix for the module identifier used at
// a call site (accounting for aliasing). Caller must already know
// `name` is a known module (isKnownModuleName checked).
static const char* resolveModulePrefix(Token* name) {
    ModuleBinding* b = findModuleBinding(name);
    return b != NULL ? b->prefix : NULL;
}

static void registerModuleBinding(const char* alias, const char* prefix) {
    for (int i = 0; i < moduleRegistryCount; i++) {
        if (strcmp(moduleRegistry[i].alias, alias) == 0) {
            // Re-binding the same alias (e.g. `use math;` issued twice) --
            // just refresh the target prefix, don't duplicate the entry.
            snprintf(moduleRegistry[i].prefix, sizeof(moduleRegistry[i].prefix), "%s", prefix);
            return;
        }
    }
    if (moduleRegistryCount < MAX_MODULE_NAMES) {
        snprintf(moduleRegistry[moduleRegistryCount].alias, sizeof(moduleRegistry[0].alias), "%s", alias);
        snprintf(moduleRegistry[moduleRegistryCount].prefix, sizeof(moduleRegistry[0].prefix), "%s", prefix);
        moduleRegistryCount++;
    }
}

static bool moduleAlreadyLoaded(const char* name) {
    for (int i = 0; i < loadedModuleCount; i++) {
        if (strcmp(loadedModules[i], name) == 0) return true;
    }
    return false;
}

static char* readEntireFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) return NULL;
    fseek(file, 0L, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size < 0) { fclose(file); return NULL; }
    char* buffer = malloc((size_t)size + 1);
    size_t bytesRead = fread(buffer, 1, (size_t)size, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

// `nameTok` is the real module name (`math` in `use math as m;`);
// `aliasTok` is the optional `as` identifier, or NULL if none was given.
static void loadStdlibModule(Token* nameTok, Token* aliasTok) {
    char name[64];
    int len = nameTok->length < 63 ? nameTok->length : 63;
    memcpy(name, nameTok->start, (size_t)len);
    name[len] = '\0';

    // `io` is a compiler-builtin pseudo-namespace (see identifierExpr's
    // hardcoded `io.out`/`io.in` dispatch) -- it must never enter
    // moduleRegistry, or `io.out(...)` would try to resolve as a normal
    // mangled-global module call ("io.out") instead of its dedicated
    // opcode, and fail with "Undefined function 'io.out'".
    if (strcmp(name, "io") == 0) {
        if (aliasTok != NULL) errorAt(aliasTok, "'io' is a built-in namespace and cannot be aliased.");
        return;
    }

    char alias[64];
    if (aliasTok != NULL) {
        int aliasLen = aliasTok->length < 63 ? aliasTok->length : 63;
        memcpy(alias, aliasTok->start, (size_t)aliasLen);
        alias[aliasLen] = '\0';
    } else {
        snprintf(alias, sizeof(alias), "%s", name);
    }

    // Bind the alias (or the real name, if unaliased) to this module's
    // real prefix regardless of load status, so repeated/differently
    // aliased `use` of an already-loaded module still works.
    registerModuleBinding(alias, name);

    if (moduleAlreadyLoaded(name)) return;

    char path[256];
    snprintf(path, sizeof(path), "stdlib/%s.prt", name);
    char* source = readEntireFile(path);
    if (source == NULL) {
        // Unlike before, a missing module file is now a real compile
        // error -- silently ignoring it let typos like `use mathh;`
        // compile fine and only fail confusingly at the first call site.
        char msg[192];
        snprintf(msg, sizeof(msg), "Module '%s' not found (expected stdlib/%s.prt).", name, name);
        errorAt(nameTok, msg);
        return;
    }

    // Intentionally never freed: tokens compiled from this buffer (e.g.
    // global-type-table entries, see rememberGlobalType) keep pointers
    // into it for the rest of compilation.
    if (loadedModuleCount < MAX_LOADED_MODULES) {
        snprintf(loadedModules[loadedModuleCount], sizeof(loadedModules[0]), "%s", name);
        loadedModuleCount++;
    }

    LexerState savedLexer = saveLexerState();
    Token savedCurrent = parser.current;
    Token savedPrevious = parser.previous;
    bool savedHasPeeked = hasPeeked;
    Token savedPeeked = peekedToken;
    const char* savedPrefix = activeModulePrefix;

    // `name` is a stack buffer, but it's alive for the rest of this
    // function's body (which is exactly as long as activeModulePrefix
    // needs to stay valid) -- it's restored to savedPrefix before we
    // return, so nothing outlives it.
    activeModulePrefix = name;

    initLexer(source);
    hasPeeked = false;
    advance(); // prime parser.current with the module's first token

    while (!check(TOKEN_EOF)) {
        topLevelDeclaration();
    }

    activeModulePrefix = savedPrefix;
    restoreLexerState(savedLexer);
    parser.current = savedCurrent;
    parser.previous = savedPrevious;
    hasPeeked = savedHasPeeked;
    peekedToken = savedPeeked;
}

static void useDeclaration(void) {
    // A module name is usually a plain identifier (`use math;`), but a few
    // reasonable module names -- `string` chief among them -- are also
    // reserved primitive-type keywords (TOKEN_TYPE_NAME), so both token
    // kinds are accepted here. Module lookup/mangling downstream only
    // ever compares the token's raw text, so nothing else needs to know
    // which kind it was.
    if (!check(TOKEN_IDENTIFIER) && !check(TOKEN_TYPE_NAME)) {
        errorAtCurrent("Expect module name after 'use'.");
    }
    advance();
    Token nameTok = parser.previous;
    while (match(TOKEN_DOT)) {
        consume(TOKEN_IDENTIFIER, "Expect identifier after '.'.");
    }
    bool hasAlias = match(TOKEN_AS);
    Token aliasTok;
    if (hasAlias) {
        consume(TOKEN_IDENTIFIER, "Expect alias name after 'as'.");
        aliasTok = parser.previous;
    }
    consume(TOKEN_SEMICOLON, "Expect ';' after 'use' declaration.");
    loadStdlibModule(&nameTok, hasAlias ? &aliasTok : NULL);
}

static void synchronize(void) {
    parser.panicMode = false;
    while (!check(TOKEN_EOF)) {
        if (parser.previous.type == TOKEN_SEMICOLON) return;
        switch (parser.current.type) {
            case TOKEN_FN:
            case TOKEN_VAR:
            case TOKEN_CONST:
            case TOKEN_USE:
            case TOKEN_STRUCT:
            case TOKEN_ENUM:
                return;
            default:
                break;
        }
        advance();
    }
}

static void topLevelDeclaration(void) {
    bool isPrivate = match(TOKEN_PRIVATE);

    if (match(TOKEN_USE)) {
        if (isPrivate) error("'private' cannot be used with 'use'.");
        useDeclaration();
    } else if (match(TOKEN_FN)) {
        funDeclaration(isPrivate);
    } else if (match(TOKEN_VAR) || match(TOKEN_CONST)) {
        globalVarDeclaration(isPrivate);
    } else if (check(TOKEN_ENUM)) {
        if (isPrivate) error("'private' is not yet supported on 'enum' declarations.");
        enumDeclaration();
    } else if (check(TOKEN_STRUCT)) {
        if (isPrivate) error("'private' is not yet supported on 'struct' declarations.");
        structDeclaration();
    } else if (isPrivate) {
        errorAtCurrent("Expect 'fn', 'var', or 'const' after 'private'.");
        advance();
    } else {
        errorAtCurrent("Expect a top-level declaration ('use', 'fn', 'var', 'const').");
        advance();
    }

    if (parser.panicMode) synchronize();
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

bool compileProgram(const char* source) {
    initRules();
    initLexer(source);
    hasPeeked = false;

    Compiler scriptCompiler;
    ObjString* scriptName = copyString("<script>", 8);
    initCompilerState(&scriptCompiler, NULL, scriptName);

    parser.hadError = false;
    parser.panicMode = false;

    advance();
    while (!match(TOKEN_EOF)) {
        topLevelDeclaration();
    }

    endCompiler();
    return !parser.hadError;
}
