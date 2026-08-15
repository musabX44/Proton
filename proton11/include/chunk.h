#ifndef PROTON_CHUNK_H
#define PROTON_CHUNK_H

#include "common.h"
#include "value.h"

typedef enum {
    OP_CONSTANT,
    OP_NIL,
    OP_TRUE,
    OP_FALSE,
    OP_POP,

    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_DEFINE_GLOBAL,

    OP_EQUAL,
    OP_NOT_EQUAL,
    OP_GREATER,
    OP_GREATER_EQUAL,
    OP_LESS,
    OP_LESS_EQUAL,

    OP_ADD,
    OP_SUBTRACT,
    OP_MULTIPLY,
    OP_DIVIDE,
    OP_MODULO,
    OP_NOT,
    OP_NEGATE,

    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_SHL,
    OP_SHR,

    OP_PRINT,       // operand: arg count, pops that many values and prints concatenated
    OP_READ_LINE,   // reads a line from stdin, pushes number if parseable else string
    OP_JUMP,
    OP_JUMP_IF_FALSE,
    OP_LOOP,
    OP_CALL,        // operand1: name constant index (1 byte), operand2: arg count (1 byte)
    OP_CALL_VALUE,  // operand: arg count (1 byte). Callee (a function Value)
                     // sits on the stack below the args, unlike OP_CALL
                     // which looks the function up by name at the call
                     // site. Used for calling through a variable/parameter
                     // that holds a function value (first-class functions,
                     // no closures/captured state -- see ObjFunction).
    OP_RETURN,
    OP_RETURN_VOID,
    OP_ASSERT,
    OP_PANIC,       // operand: arg count (message parts)
    OP_CHECK_TYPE,  // operand: ProtonType tag (1 byte); peeks top of stack, validates, does not pop
    OP_BUILD_LIST,  // operand1: element count (1 byte), operand2: ProtonType elem type tag (1 byte);
                     // pops that many values (left-to-right on stack), pushes one ObjList
    OP_GET_INDEX,   // pops index, pops list/map, pushes list[index] or map[key] -- O(1) direct access
    OP_SET_INDEX,   // pops value, pops index, pops list/map, pushes value (assignment is an expression) --
                     // O(1) direct store; enforces list->elemType if set (maps are untyped)
    OP_LEN,         // pops a list or map, pushes its element count -- O(1), reads ->count / ->table.count directly
    OP_LIST_PUSH,   // pops value, pops list; appends value to the list (amortized O(1), mutates in place) and pushes the same list back
    OP_LIST_COPY,   // pops list; pushes a fresh shallow-ish copy (elements promoted like return-escape) owned by the current frame's region
    OP_BUILD_MAP,   // operand: pair count (1 byte); pops pairCount*2 values (key,value,key,value,... left-to-right),
                     // pushes one ObjMap built via tableSet -- O(1) per pair (amortized)
    OP_TRY,         // '?' operator. Peeks top of stack: if IS_ERROR, pops it, tears down the current
                     // frame's region, pops the frame, and pushes the error onto the caller's stack in
                     // place of a return value (stack unwinding). If not an error, falls through (no-op).

    // Native I/O / system calls. Each is special-cased at the compiler
    // level (identifierExpr, same pattern as `len(x)`) rather than routed
    // through OP_CALL's user-function/arity-checked global-table lookup,
    // since these are fixed-arity built-ins, not user-defined fn's.
    OP_FS_READ,     // pops path (string); pushes file contents as string, or VAL_ERROR on failure
    OP_FS_WRITE,    // pops content (string), pops path (string); pushes nil
    OP_FS_EXISTS,   // pops path (string); pushes bool -- O(1) access() check
    OP_SYS_EXEC,    // pops command (string); pushes captured stdout as string
    OP_SYS_ENV,     // pops name (string); pushes value as string, or nil if unset
    OP_SYS_ARGS,    // pushes ObjList of string, the process's argv[1..] -- no operand, no pop
    OP_SYS_SETENV,  // pops value (string), pops name (string); sets this process's env var; pushes nil
    OP_SYS_EXIT,    // pops code (int); flushes stdout/stderr and terminates the process immediately -- never returns
    OP_SYS_PID,     // pushes this process's pid as int64 -- no operand, no pop
    OP_SYS_PPID,    // pushes this process's parent pid as int64 -- no operand, no pop

    OP_CHAR_CODE,      // pops a 1-character string; pushes its byte value as int64 (0-255)
    OP_CHAR_FROM_CODE, // pops an integer (0-255); pushes a 1-character string

    OP_TIME_NOW,    // pushes current Unix time in milliseconds as int64 -- no operand, no pop
    OP_TIME_TICKS,  // pushes a monotonic microsecond counter as int64 -- no operand, no pop
    OP_TIME_CLOCK,  // pushes process CPU time in seconds as float64 -- no operand, no pop
    OP_TIME_SLEEP,  // pops ms (number); sleeps that many milliseconds; pushes nil
    OP_TIME_FORMAT, // pops fmt (string), pops timestamp (int ms); pushes formatted string
    OP_TIME_PARSE,  // pops fmt (string), pops dateStr (string); pushes int64 ms, or VAL_ERROR on failure

    OP_NET_GET,     // pops url (string); pushes response body as string, or VAL_ERROR on failure
    OP_NET_POST,    // pops body (string), pops url (string); pushes response body as string, or VAL_ERROR
    OP_NET_REQUEST, // pops options (map); pushes {status,body,headers} map, or VAL_ERROR
    OP_NET_RESOLVE, // pops hostname (string); pushes resolved IP as string, or VAL_ERROR
    OP_NET_PING,    // pops timeoutMs (number), pops host (string); pushes elapsed ms (number), or -1
    OP_NET_URLENCODE, // pops string; pushes percent-encoded string
    OP_NET_URLDECODE, // pops string; pushes percent-decoded string
    OP_NET_SERVE,   // pops handler (function value), pops port (number); blocks serving HTTP forever, or pushes VAL_ERROR on setup failure

    // Outbound-only socket handles (see VM.netSockets in vm.h). No
    // bind/listen/accept exist anywhere -- these can only dial *out* to
    // a host:port the script provides.
    OP_NET_CONNECT, // pops protocol (string "tcp"/"udp"), pops port (number), pops host (string); pushes handle (number) or VAL_ERROR
    OP_NET_SEND,    // pops data (string), pops handle (number); pushes bytes-sent (number) or VAL_ERROR
    OP_NET_RECV,    // pops maxBytes (number), pops handle (number); pushes received data (string, "" on clean close) or VAL_ERROR
    OP_NET_CLOSE,   // pops handle (number); pushes nil (invalid/already-closed handle is a silent no-op)

    OP_HALT
} OpCode;

typedef struct {
    int count;
    int capacity;
    uint8_t* code;
    int* lines;
    ValueArray constants;
} Chunk;

void initChunk(Chunk* chunk);
void freeChunk(Chunk* chunk);
void writeChunk(Chunk* chunk, uint8_t byte, int line);
int addConstant(Chunk* chunk, Value value);

#endif
