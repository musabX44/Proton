#ifndef PROTON_VM_H
#define PROTON_VM_H

#include "common.h"
#include "chunk.h"
#include "object.h"
#include "table.h"
#include "region.h"

#define FRAMES_MAX 256
#define STACK_MAX (FRAMES_MAX * UINT8_COUNT)

// net::connect/send/recv/close (see vm.c's "outbound socket handles"
// section): a small fixed table of open client sockets, indexed by a
// small integer handle the script holds instead of a raw fd. This is
// deliberately outbound-only -- entries are only ever created by
// net::connect() dialing out to a host:port the script provides; there
// is no bind/listen/accept anywhere in this table's lifecycle, so it
// can't be used to open a listening port. Capacity is small and fixed
// (no malloc/realloc growth) matching this VM's general allocation
// style elsewhere.
#define NET_SOCKETS_MAX 64

typedef struct {
    ObjFunction* function;
    uint8_t* ip;
    Value* slots;
    // Faz 1 LAM: this call frame's own region. Created when the frame is
    // pushed (OP_CALL / interpretSource's initial frame), destroyed
    // unconditionally when the frame is popped (OP_RETURN). Runtime
    // string allocations (concatenation, to-string coercion) that happen
    // while this frame is executing are carved out of this region.
    Region* region;
    // Loop-scope rewind checkpoint (see OP_LOOP in vm.c and
    // regionRewind's doc comment). Per-frame because each frame has its
    // own region and its own independent loop nesting/iteration
    // sequence -- a checkpoint captured in one frame must never be
    // applied to another frame's region. Only meaningful while this
    // frame is inside a loop body; harmless (rewinds to itself, a no-op)
    // when OP_LOOP hasn't run yet in this frame since region == fresh.
    RegionCheckpoint loopCheckpoint;
} CallFrame;

typedef struct {
    CallFrame frames[FRAMES_MAX];
    int frameCount;

    Value stack[STACK_MAX];
    Value* stackTop;

    Table globals;

    // sys::args() support: extra command-line arguments passed after the
    // script path (argv[2..] from main), exposed to Proton read-only.
    // Not owned -- these point directly into main()'s argv, which outlives
    // the VM for the process's whole lifetime, so no copying/freeing needed.
    int scriptArgc;
    const char** scriptArgv;

    // net::connect's open outbound socket handles. netSockets[i] == -1
    // means slot i is free; otherwise it holds a live fd. Handles handed
    // to scripts are simply the table index.
    int netSockets[NET_SOCKETS_MAX];
} VM;

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

extern VM vm;

void initVM(void);
void freeVM(void);
void vmDefineGlobal(ObjString* name, Value value);
InterpretResult interpretSource(const char* source);

#endif
