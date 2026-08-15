// -std=c11 hides POSIX-only declarations (popen/pclose, and access() from
// unistd.h) unless a feature-test macro opts back in. Defined before any
// system header is included, per POSIX convention.
#define _POSIX_C_SOURCE 200809L
// asprintf is a GNU extension, not part of POSIX -- opt in separately so
// -std=c11 doesn't hide its declaration in stdio.h.
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "vm.h"
#include "compiler.h"
#include "object.h"

VM vm;

static void resetStack(void) {
    vm.stackTop = vm.stack;
    vm.frameCount = 0;
}

void initVM(void) {
    resetStack();
    initTable(&vm.globals);
    vm.scriptArgc = 0;
    vm.scriptArgv = NULL;
    for (int i = 0; i < NET_SOCKETS_MAX; i++) vm.netSockets[i] = -1;
}

void freeVM(void) {
    freeTable(&vm.globals);
    freeObjects();
    // Close any outbound sockets the script left open at program exit
    // (net::close is best-effort from the script's perspective, but the
    // VM should never leak live fds past its own lifetime).
    for (int i = 0; i < NET_SOCKETS_MAX; i++) {
        if (vm.netSockets[i] != -1) {
            close(vm.netSockets[i]);
            vm.netSockets[i] = -1;
        }
    }
    // Release pooled region blocks/structs back to the system allocator
    // now that the VM is shutting down and nothing will reuse them.
    regionPoolDrain();
}

void vmDefineGlobal(ObjString* name, Value value) {
    tableSet(&vm.globals, name, value);
}

static void push(Value value) {
    *vm.stackTop = value;
    vm.stackTop++;
}

static Value pop(void) {
    vm.stackTop--;
    return *vm.stackTop;
}

static Value peek(int distance) {
    return vm.stackTop[-1 - distance];
}

static int currentLine(CallFrame* frame) {
    size_t instruction = frame->ip - frame->function->chunk.code - 1;
    return frame->function->chunk.lines[instruction];
}

static void runtimeError(const char* format, ...) {
    // Note: on this error path, live regions for frames still on the
    // call stack are not explicitly destroyed here (resetStack() below
    // just zeroes frameCount). This matches the pre-existing behavior of
    // this interpreter on error: main.c's runFile() calls exit(70) right
    // after INTERPRET_RUNTIME_ERROR is returned, so the OS reclaims the
    // memory. If interpretSource() is ever driven from a long-running
    // host process (e.g. a REPL or embedded use) instead of a one-shot
    // CLI, this would need to walk vm.frames[0..frameCount-1] and
    // regionDestroy() each one.
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm.frameCount - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        ObjFunction* function = frame->function;
        int line = currentLine(frame);
        fprintf(stderr, "[line %d] in %s\n", line,
                function->name != NULL ? function->name->chars : "<script>");
    }
    resetStack();
}

static bool isFalsey(Value value) {
    return IS_NIL(value) || (IS_BOOL(value) && !AS_BOOL(value));
}

static bool isIntegerInRange(double n, double lo, double hi) {
    return n == floor(n) && n >= lo && n <= hi;
}

static const char* valueTypeName(Value value) {
    switch (value.type) {
        case VAL_NIL: return "nil";
        case VAL_BOOL: return "bool";
        case VAL_NUMBER: return "number";
        case VAL_ERROR: return "error";
        case VAL_OBJ:
            if (IS_STRING(value)) return "string";
            if (IS_FUNCTION(value)) return "function";
            if (IS_LIST(value)) return "list";
            if (IS_MAP(value)) return "map";
            return "obj";
        default: return "?";
    }
}

static ObjString* toStringValue(Region* region, Value value) {
    if (IS_STRING(value)) return AS_STRING(value);
    char buf[64];
    if (IS_NUMBER(value)) {
        if (NUM_KIND(value) == NUM_I64) {
            snprintf(buf, sizeof(buf), "%lld", (long long)AS_I64(value));
        } else if (NUM_KIND(value) == NUM_U64) {
            snprintf(buf, sizeof(buf), "%llu", (unsigned long long)AS_U64(value));
        } else {
            double n = AS_NUMBER(value);
            if (n == (long long)n) snprintf(buf, sizeof(buf), "%lld", (long long)n);
            else snprintf(buf, sizeof(buf), "%g", n);
        }
    } else if (IS_BOOL(value)) {
        snprintf(buf, sizeof(buf), "%s", AS_BOOL(value) ? "true" : "false");
    } else {
        snprintf(buf, sizeof(buf), "nil");
    }
    // Region-scoped, not interned: this is a fresh throwaway string
    // derived from a non-string value, not a source-code literal, so
    // there's no interning benefit and it should die with its call frame.
    char* heapChars = malloc(strlen(buf) + 1);
    memcpy(heapChars, buf, strlen(buf) + 1);
    return regionTakeString(region, heapChars, (int)strlen(buf));
}

static void concatenate(Region* region) {
    ObjString* b = toStringValue(region, peek(0));
    ObjString* a = toStringValue(region, peek(1));

    int length = a->length + b->length;
    char* chars = malloc(length + 1);
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    // Concatenation results are also runtime-local temporaries: region
    // scoped, not interned. (If both operands happen to be interned
    // literals, e.g. "a" + "b", we still don't intern the *result* --
    // only source-literal text is worth interning; concatenation results
    // are typically distinct on every call anyway.)
    ObjString* result = regionTakeString(region, chars, length);
    pop();
    pop();
    push(OBJ_VAL(result));
}

// ---------------------------------------------------------------------
// net::get / net::post / net::request -- minimal synchronous HTTP/1.1
// client over a raw BSD socket. Deliberately plain-HTTP only: no TLS,
// since adding one means pulling in an external crypto library
// (OpenSSL/mbedTLS/etc.), which would break the "gcc + libc only" build
// this whole VM otherwise relies on. https:// URLs fail with a clear
// error rather than silently downgrading or hanging.
//
// This client is intentionally request-only: it connects out to a URL
// the script provides and speaks HTTP, nothing else. There is no
// exposed raw socket handle, no listen/accept, and no way to target an
// arbitrary host:port outside of an HTTP request -- i.e. no primitives
// generic enough to build a port scanner or a bind/reverse shell out of.
//
// On success returns true and sets *outStatus, *outBody/*outLen, and
// *outHeaders (an already-allocated ObjMap the caller passes in, or
// NULL to skip header capture). On failure returns false and sets
// *outErr to a malloc'd message (caller must free it) or leaves it NULL
// for a generic fallback message.
static bool protonHttpRequestEx(const char* method, const char* url, const char* body,
                                 const char* extraHeaders, int timeoutMs,
                                 int* outStatus, char** outBody, size_t* outLen,
                                 ObjMap* outHeaders, char** outErr) {
    *outBody = NULL;
    *outLen = 0;
    *outErr = NULL;
    if (outStatus) *outStatus = 0;
    if (timeoutMs <= 0) timeoutMs = 10000;

    const char* rest = url;
    if (strncmp(rest, "https://", 8) == 0) {
        *outErr = strdup("net:: yalnizca http:// destekler (TLS yok)");
        return false;
    }
    if (strncmp(rest, "http://", 7) == 0) {
        rest += 7;
    }
    if (*rest == '\0') {
        *outErr = strdup("Gecersiz URL");
        return false;
    }

    // Split into host[:port] and path.
    char hostbuf[256];
    int hi = 0;
    while (*rest && *rest != '/' && *rest != ':' && hi < (int)sizeof(hostbuf) - 1) {
        hostbuf[hi++] = *rest++;
    }
    hostbuf[hi] = '\0';
    if (hi == 0) {
        *outErr = strdup("Gecersiz URL");
        return false;
    }

    int port = 80;
    if (*rest == ':') {
        rest++;
        port = 0;
        while (*rest >= '0' && *rest <= '9') {
            port = port * 10 + (*rest - '0');
            rest++;
        }
        if (port <= 0 || port > 65535) {
            *outErr = strdup("Gecersiz port");
            return false;
        }
    }

    const char* path = (*rest == '\0') ? "/" : rest;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    int gaiResult = getaddrinfo(hostbuf, portStr, &hints, &res);
    if (gaiResult != 0 || res == NULL) {
        *outErr = strdup("Sunucu adresi cozulemedi (DNS)");
        return false;
    }

    int sock = -1;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        // Bounded connect/IO timeout so a dead/unreachable host doesn't
        // hang the whole interpreter forever. Caller-configurable (via
        // net::request's "timeout" option), clamped to a sane ceiling so
        // a script can't accidentally (or deliberately) stall the VM for
        // an unbounded amount of time.
        int clampedMs = timeoutMs;
        if (clampedMs > 60000) clampedMs = 60000;
        struct timeval tv;
        tv.tv_sec = clampedMs / 1000;
        tv.tv_usec = (clampedMs % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        *outErr = strdup("Baglanti kurulamadi");
        return false;
    }

    // Build the request. Content-Length only makes sense (and is only
    // sent) when there's a body -- GET/HEAD-style requests always have
    // an empty body here. extraHeaders (if any) is a caller-supplied
    // block of already-formatted "Name: value\r\n" lines, inserted
    // before the blank line that separates headers from body.
    char* reqBuf;
    int reqLen;
    const char* hdrs = extraHeaders ? extraHeaders : "";
    if (body != NULL) {
        size_t bodyLen = strlen(body);
        reqLen = asprintf(&reqBuf,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "%s"
            "\r\n"
            "%s",
            method, path, hostbuf, bodyLen, hdrs, body);
    } else {
        reqLen = asprintf(&reqBuf,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "%s"
            "\r\n",
            method, path, hostbuf, hdrs);
    }
    if (reqLen < 0) {
        close(sock);
        *outErr = strdup("Istek olusturulamadi");
        return false;
    }

    size_t sent = 0;
    while (sent < (size_t)reqLen) {
        ssize_t n = send(sock, reqBuf + sent, (size_t)reqLen - sent, 0);
        if (n <= 0) {
            free(reqBuf);
            close(sock);
            *outErr = strdup("Istek gonderilemedi");
            return false;
        }
        sent += (size_t)n;
    }
    free(reqBuf);

    // Read the full response.
    size_t cap = 8192;
    size_t len = 0;
    char* raw = (char*)malloc(cap);
    char chunk[4096];
    ssize_t n;
    while ((n = recv(sock, chunk, sizeof(chunk), 0)) > 0) {
        if (len + (size_t)n + 1 > cap) {
            size_t newCap = cap * 2;
            while (newCap < len + (size_t)n + 1) newCap *= 2;
            char* grown = (char*)realloc(raw, newCap);
            raw = grown;
            cap = newCap;
        }
        memcpy(raw + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    close(sock);
    raw[len] = '\0';

    if (len == 0) {
        free(raw);
        *outErr = strdup("Sunucudan yanit alinamadi");
        return false;
    }

    // Split headers from body on the first blank line (\r\n\r\n, with a
    // \n\n fallback for lenient servers).
    char* headerEnd = strstr(raw, "\r\n\r\n");
    size_t headerLen = 4;
    if (headerEnd == NULL) {
        headerEnd = strstr(raw, "\n\n");
        headerLen = 2;
    }
    if (headerEnd == NULL) {
        // No discernible header/body split -- return the raw payload
        // as-is rather than failing outright.
        *outBody = raw;
        *outLen = len;
        return true;
    }

    // Parse the status line ("HTTP/1.1 200 OK\r\n...") for the numeric
    // status code, and each subsequent "Name: value" header line into
    // outHeaders (if the caller wants them -- net::get/post pass NULL
    // and skip this).
    if (outStatus || outHeaders) {
        char* headBuf = (char*)malloc((size_t)(headerEnd - raw) + 1);
        memcpy(headBuf, raw, (size_t)(headerEnd - raw));
        headBuf[headerEnd - raw] = '\0';

        char* line = strtok(headBuf, "\r\n");
        if (line != NULL) {
            const char* sp = strchr(line, ' ');
            if (sp != NULL && outStatus) {
                *outStatus = atoi(sp + 1);
            }
            line = strtok(NULL, "\r\n");
        }
        while (line != NULL) {
            if (outHeaders) {
                char* colon = strchr(line, ':');
                if (colon != NULL) {
                    *colon = '\0';
                    char* value = colon + 1;
                    while (*value == ' ') value++;
                    ObjString* keyStr = copyString(line, (int)strlen(line));
                    ObjString* valStr = copyString(value, (int)strlen(value));
                    tableSet(&outHeaders->table, keyStr, OBJ_VAL(valStr));
                }
            }
            line = strtok(NULL, "\r\n");
        }
        free(headBuf);
    }

    char* bodyStart = headerEnd + headerLen;
    size_t bodyLen = len - (size_t)(bodyStart - raw);
    char* result = (char*)malloc(bodyLen + 1);
    memcpy(result, bodyStart, bodyLen);
    result[bodyLen] = '\0';
    free(raw);

    *outBody = result;
    *outLen = bodyLen;
    return true;
}

// Back-compat wrapper for the original net::get/post signature (no
// status/header capture, default timeout).
static bool protonHttpRequest(const char* method, const char* url, const char* body,
                               char** outBody, size_t* outLen, char** outErr) {
    return protonHttpRequestEx(method, url, body, NULL, 10000, NULL, outBody, outLen, NULL, outErr);
}

// ---------------------------------------------------------------------
// net::resolve -- DNS-only lookup (getaddrinfo, no socket/connect). This
// is deliberately read-only network *information*, not a connectivity
// primitive: it answers "what IP does this name map to" and nothing
// else, so it can't be used to talk to arbitrary ports the way a raw
// connect() would.
//
// Returns a malloc'd string with the first resolved address (caller
// takes ownership) or NULL with *outErr set on failure.
static char* protonNetResolve(const char* hostname, char** outErr) {
    *outErr = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    int gaiResult = getaddrinfo(hostname, NULL, &hints, &res);
    if (gaiResult != 0 || res == NULL) {
        *outErr = strdup("Sunucu adresi cozulemedi (DNS)");
        return NULL;
    }
    char buf[INET6_ADDRSTRLEN];
    const void* addr;
    if (res->ai_family == AF_INET) {
        addr = &((struct sockaddr_in*)res->ai_addr)->sin_addr;
    } else {
        addr = &((struct sockaddr_in6*)res->ai_addr)->sin6_addr;
    }
    inet_ntop(res->ai_family, addr, buf, sizeof(buf));
    freeaddrinfo(res);
    return strdup(buf);
}

// ---------------------------------------------------------------------
// net::connect / net::send / net::recv / net::close -- an OUTBOUND-ONLY
// TCP/UDP socket API. The script gets a small integer handle (an index
// into vm.netSockets, see vm.h), never a raw fd, and every entry in that
// table is created exclusively by dialing *out* to a host:port the
// script supplies. There is deliberately no bind(), no listen(), no
// accept() anywhere in this file -- a script can open client
// connections (to build a custom protocol client, a database driver, an
// IoT device talker, a P2P node's outbound leg, etc.) but can never open
// a listening socket of its own or accept an inbound connection through
// this API. (net::serve, above, remains the only way to accept inbound
// traffic, and it owns its own listening socket entirely inside the VM
// -- the script never touches it directly.)
//
// This intentionally mirrors protonHttpRequestEx's connect logic
// (bounded, clamped SO_RCVTIMEO/SO_SNDTIMEO so a dead host can't hang
// the interpreter) but skips all HTTP framing -- callers speak whatever
// protocol they want over send/recv.
static int protonNetAllocSlot(void) {
    for (int i = 0; i < NET_SOCKETS_MAX; i++) {
        if (vm.netSockets[i] == -1) return i;
    }
    return -1;
}

static bool protonNetConnect(const char* host, int port, const char* protocol,
                              int* outHandle, char** outErr) {
    *outErr = NULL;
    if (port <= 0 || port > 65535) {
        *outErr = strdup("Gecersiz port numarasi");
        return false;
    }
    bool isUdp = (strcmp(protocol, "udp") == 0);
    if (!isUdp && strcmp(protocol, "tcp") != 0) {
        *outErr = strdup("Protokol 'tcp' veya 'udp' olmali");
        return false;
    }

    int slot = protonNetAllocSlot();
    if (slot == -1) {
        *outErr = strdup("Ac\xC4\xB1k soket limitine ulasildi");
        return false;
    }

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = isUdp ? SOCK_DGRAM : SOCK_STREAM;
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, portStr, &hints, &res) != 0 || res == NULL) {
        *outErr = strdup("Sunucu adresi cozulemedi (DNS)");
        return false;
    }

    int sock = -1;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        // Same bounded-timeout rationale as protonHttpRequestEx: a
        // dead/unreachable/slow-loris host must not be able to stall the
        // whole single-threaded interpreter indefinitely.
        struct timeval tv;
        tv.tv_sec = 15;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);

    if (sock < 0) {
        *outErr = strdup("Baglanti kurulamadi");
        return false;
    }

    vm.netSockets[slot] = sock;
    *outHandle = slot;
    return true;
}

static bool protonNetHandleValid(int handle) {
    return handle >= 0 && handle < NET_SOCKETS_MAX && vm.netSockets[handle] != -1;
}

static bool protonNetSend(int handle, const char* data, size_t len, long* outSent, char** outErr) {
    *outErr = NULL;
    if (!protonNetHandleValid(handle)) {
        *outErr = strdup("Gecersiz veya kapali soket handle'i");
        return false;
    }
    ssize_t sent = send(vm.netSockets[handle], data, len, 0);
    if (sent < 0) {
        *outErr = strdup("Veri gonderilemedi");
        return false;
    }
    *outSent = (long)sent;
    return true;
}

// Caller-owned buffer of size maxBytes; returns actual bytes read via
// *outLen (0 on a clean orderly close, same as a plain read()/recv()).
static bool protonNetRecv(int handle, char* buf, size_t maxBytes, size_t* outLen, char** outErr) {
    *outErr = NULL;
    if (!protonNetHandleValid(handle)) {
        *outErr = strdup("Gecersiz veya kapali soket handle'i");
        return false;
    }
    ssize_t got = recv(vm.netSockets[handle], buf, maxBytes, 0);
    if (got < 0) {
        *outErr = strdup("Veri alinamadi (zaman asimi veya baglanti hatasi)");
        return false;
    }
    *outLen = (size_t)got;
    return true;
}

static void protonNetClose(int handle) {
    if (handle >= 0 && handle < NET_SOCKETS_MAX && vm.netSockets[handle] != -1) {
        close(vm.netSockets[handle]);
        vm.netSockets[handle] = -1;
    }
    // Invalid/already-closed handle: silent no-op, matching how e.g.
    // freeing an already-freed resource is often made idempotent rather
    // than an error, and scripts shouldn't need try/catch ceremony just
    // to close something defensively.
}

// ---------------------------------------------------------------------
// net::ping -- a reachability check, not a raw-socket primitive. It
// times a single TCP connect-and-close against the given host/port
// (defaulting to 80 if none is meaningful) and reports the elapsed time
// or -1 on failure/timeout. There is no way to send or receive
// arbitrary payloads through it, and no handle is exposed to the
// script -- it answers one question ("is this reachable, how fast") and
// closes the socket itself before returning.
static double protonNetPing(const char* host, int timeoutMs) {
    if (timeoutMs <= 0) timeoutMs = 2000;
    if (timeoutMs > 30000) timeoutMs = 30000;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = NULL;
    // Port 80 is used purely as a generic TCP reachability probe target
    // (most hosts have *something* listening, and even a refused
    // connection at the TCP layer confirms the host itself answered).
    if (getaddrinfo(host, "80", &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int sock = -1;
    bool connected = false;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) {
            connected = true;
            break;
        }
        // ECONNREFUSED still means the host answered (just nothing on
        // port 80) -- treat that as "reachable" too, same as ping would
        // treat a host that responds at all.
        if (errno == ECONNREFUSED) {
            connected = true;
            break;
        }
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    clock_gettime(CLOCK_MONOTONIC, &end);

    if (sock >= 0) close(sock);
    if (!connected) return -1;

    double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
    return ms;
}

// ---------------------------------------------------------------------
// net::urlEncode / net::urlDecode -- standard percent-encoding, pure
// string transforms with no I/O at all.
static char* protonUrlEncode(const char* str) {
    size_t len = strlen(str);
    char* out = (char*)malloc(len * 3 + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            snprintf(out + o, 4, "%%%02X", c);
            o += 3;
        }
    }
    out[o] = '\0';
    return out;
}

static char* protonUrlDecode(const char* str) {
    size_t len = strlen(str);
    char* out = (char*)malloc(len + 1);
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len && isxdigit((unsigned char)str[i + 1]) && isxdigit((unsigned char)str[i + 2])) {
            char hex[3] = { str[i + 1], str[i + 2], '\0' };
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else if (str[i] == '+') {
            out[o++] = ' ';
        } else {
            out[o++] = str[i];
        }
    }
    out[o] = '\0';
    return out;
}

// Called when a value is about to outlive the region it was allocated
// in -- currently: a regional string being returned from `frame` via
// OP_RETURN. Rather than always promoting to permanent (interned)
// storage (Faz 1's conservative fallback), this walks one step up the
// region chain: if the returning frame's region has an enclosing region
// (i.e. this isn't the outermost/main frame), the string is bump-copied
// into that enclosing region instead. It keeps riding the chain that way,
// call after call, only actually reaching permanent storage if it
// escapes all the way out of main -- so "return a fresh unique string
// from a function called in a loop" no longer grows permanent memory
// per-call, only (at most) once it reaches the outermost frame.
static Value promoteEscapingValue(Region* returningRegion, Value result) {
    if (IS_STRING(result) && AS_STRING(result)->isRegional &&
        AS_STRING(result)->region == returningRegion) {
        // Only promote if the string still belongs to the region being
        // destroyed right now -- mirrors the `list->region ==
        // returningRegion` guard below, now possible for strings too
        // thanks to ObjString::region (previously strings had no way to
        // tell "regional in general" from "regional in *this specific*
        // region", so this check didn't exist and every regional string
        // was unconditionally promoted here, even one already relocated
        // to an enclosing region by an earlier OP_RETURN in the chain).
        return promoteRegionalValue(returningRegion->enclosing, result);
    }
    // Lists (README's "Ömür (lifetime) kısıtı" note): same escape hazard
    // as regional strings above, previously with no promotion path at
    // all -- a list returned from a function was a guaranteed dangling
    // pointer once its owning frame's region was destroyed. Mirrors the
    // string case exactly: ride the region chain one step at a time
    // (regionCopyList into the caller's region) rather than always
    // jumping straight to permanent storage, only falling back to
    // permanentCopyList if this is the outermost frame. `list->region ==
    // returningRegion` guards against re-promoting a list that's already
    // been relocated to an *enclosing* region by a previous OP_RETURN in
    // the chain (e.g. a list stored in a local var and returned again
    // one level further up) -- such a list no longer belongs to the
    // region being destroyed right now, so it needs no further work.
    if (IS_LIST(result)) {
        ObjList* list = AS_LIST(result);
        if (list->region == returningRegion) {
            Region* dest = returningRegion->enclosing;
            if (dest != NULL) {
                return OBJ_VAL(regionCopyList(dest, list));
            }
            return OBJ_VAL(permanentCopyList(list));
        }
    }
    return result;
}

// Reusable, side-effect-free version of the OP_CHECK_TYPE range logic,
// used by OP_BUILD_LIST / OP_SET_INDEX to validate list element types
// without needing a value already sitting on top of the VM stack (unlike
// OP_CHECK_TYPE, which peeks the stack and re-tags in place). Kept in
// sync with the switch in the OP_CHECK_TYPE case below.
static bool valueMatchesType(Value v, ProtonType type) {
    if (type == PTYPE_NONE || type == PTYPE_VOID) return true;
    switch (type) {
        case PTYPE_BOOL: return IS_BOOL(v);
        case PTYPE_STRING: return IS_STRING(v);
        case PTYPE_CHAR:
            return (IS_STRING(v) && AS_STRING(v)->length == 1) ||
                   (IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 0x10FFFF));
        case PTYPE_BYTE:
        case PTYPE_UINT8: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 255);
        case PTYPE_INT8: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -128, 127);
        case PTYPE_INT16:
        case PTYPE_SHORT: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -32768, 32767);
        case PTYPE_UINT16: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 65535);
        case PTYPE_INT32:
        case PTYPE_INT: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -2147483648.0, 2147483647.0);
        case PTYPE_UINT32:
        case PTYPE_UINT: return IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 4294967295.0);
        case PTYPE_INT64:
        case PTYPE_LONG:
            return IS_NUMBER(v) && (NUM_KIND(v) == NUM_I64 ||
                   isIntegerInRange(AS_NUMBER(v), -9007199254740991.0, 9007199254740991.0));
        case PTYPE_UINT64:
            return IS_NUMBER(v) && (NUM_KIND(v) == NUM_U64 ||
                   (NUM_KIND(v) != NUM_I64 && isIntegerInRange(AS_NUMBER(v), 0, 9007199254740991.0)) ||
                   (NUM_KIND(v) == NUM_I64 && AS_I64(v) >= 0));
        case PTYPE_FLOAT32:
        case PTYPE_FLOAT:
        case PTYPE_FLOAT64:
        case PTYPE_DOUBLE:
        case PTYPE_DECIMAL: return IS_NUMBER(v);
        default: return true;
    }
}

// Forward declaration: callProtonFunction (below run()) needs to invoke
// run() re-entrantly; run() itself is defined further down since it's
// the large bytecode dispatch loop.
static InterpretResult run(int stopDepth);
static bool protonNetServe(int port, ObjFunction* handler, char** outErr);

// stopDepth: run() returns INTERPRET_OK once vm.frameCount drops to (not
// below) this depth after an OP_RETURN, instead of always stopping only
// at 0. This makes run() re-entrant: a native function (e.g.
// net::serve's request handler dispatch) can push one extra CallFrame
// for a Proton function and call run(vm.frameCount - 1) to execute just
// that call and return here with the result left on the stack, without
// disturbing or terminating the outer interpreter loop that's already
// in progress. The original top-level entry point (interpretSource)
// passes stopDepth=0, preserving the original "run everything, stop
// only when main returns" behavior.
static InterpretResult run(int stopDepth) {
    CallFrame* frame = &vm.frames[vm.frameCount - 1];

#define READ_BYTE() (*frame->ip++)
#define READ_SHORT() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONSTANT() (frame->function->chunk.constants.values[READ_BYTE()])

    for (;;) {
        uint8_t instruction = READ_BYTE();
        switch (instruction) {
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(constant);
                break;
            }
            case OP_NIL: push(NIL_VAL); break;
            case OP_TRUE: push(BOOL_VAL(true)); break;
            case OP_FALSE: push(BOOL_VAL(false)); break;
            case OP_POP: pop(); break;

            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                push(frame->slots[slot]);
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                frame->slots[slot] = peek(0);
                break;
            }
            case OP_GET_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value value;
                if (!tableGet(&vm.globals, name, &value)) {
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(value);
                break;
            }
            case OP_SET_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                // vm.globals lives for the whole program, so a regional
                // value assigned into it must be promoted to permanent
                // storage now, or it dangles the moment its owning frame
                // returns -- same hazard/fix as OP_SET_INDEX/OP_BUILD_MAP.
                Value value = promoteRegionalValue(NULL, peek(0));
                vm.stackTop[-1] = value;
                if (tableSet(&vm.globals, name, value)) {
                    tableDelete(&vm.globals, name);
                    runtimeError("Undefined variable '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString* name = AS_STRING(READ_CONSTANT());
                Value value = promoteRegionalValue(NULL, peek(0));
                tableSet(&vm.globals, name, value);
                pop();
                break;
            }

            case OP_EQUAL: { Value b = pop(); Value a = pop(); push(BOOL_VAL(valuesEqual(a, b))); break; }
            case OP_NOT_EQUAL: { Value b = pop(); Value a = pop(); push(BOOL_VAL(!valuesEqual(a, b))); break; }

            case OP_GREATER:
            case OP_GREATER_EQUAL:
            case OP_LESS:
            case OP_LESS_EQUAL: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop());
                double a = AS_NUMBER(pop());
                bool result;
                switch (instruction) {
                    case OP_GREATER: result = a > b; break;
                    case OP_GREATER_EQUAL: result = a >= b; break;
                    case OP_LESS: result = a < b; break;
                    default: result = a <= b; break;
                }
                push(BOOL_VAL(result));
                break;
            }

            case OP_ADD: {
                if (IS_NUMBER(peek(0)) && IS_NUMBER(peek(1))) {
                    double b = AS_NUMBER(pop());
                    double a = AS_NUMBER(pop());
                    push(NUMBER_VAL(a + b));
                } else {
                    concatenate(frame->region);
                }
                break;
            }
            case OP_SUBTRACT: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop()); double a = AS_NUMBER(pop());
                push(NUMBER_VAL(a - b));
                break;
            }
            case OP_MULTIPLY: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop()); double a = AS_NUMBER(pop());
                push(NUMBER_VAL(a * b));
                break;
            }
            case OP_DIVIDE: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop()); double a = AS_NUMBER(pop());
                if (b == 0) { runtimeError("Division by zero."); return INTERPRET_RUNTIME_ERROR; }
                push(NUMBER_VAL(a / b));
                break;
            }
            case OP_MODULO: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                double b = AS_NUMBER(pop()); double a = AS_NUMBER(pop());
                if (b == 0) { runtimeError("Division by zero (modulo)."); return INTERPRET_RUNTIME_ERROR; }
                push(NUMBER_VAL(fmod(a, b)));
                break;
            }
            case OP_NOT: push(BOOL_VAL(isFalsey(pop()))); break;
            case OP_NEGATE: {
                if (!IS_NUMBER(peek(0))) { runtimeError("Operand must be a number."); return INTERPRET_RUNTIME_ERROR; }
                push(NUMBER_VAL(-AS_NUMBER(pop())));
                break;
            }

            // Bitwise operators. Operands are coerced to a 64-bit signed
            // integer view of the number (truncating any fractional part,
            // same as C's implicit double->integer conversion), and the
            // result is pushed back as an integer-kind Number so it prints
            // and compares like the exact-integer path used elsewhere
            // (see NUM_I64 in value.h) rather than losing precision through
            // a double round-trip.
            case OP_BIT_AND: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                int64_t b = (int64_t)AS_NUMBER(pop());
                int64_t a = (int64_t)AS_NUMBER(pop());
                push(INT64_VAL(a & b));
                break;
            }
            case OP_BIT_OR: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                int64_t b = (int64_t)AS_NUMBER(pop());
                int64_t a = (int64_t)AS_NUMBER(pop());
                push(INT64_VAL(a | b));
                break;
            }
            case OP_BIT_XOR: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                int64_t b = (int64_t)AS_NUMBER(pop());
                int64_t a = (int64_t)AS_NUMBER(pop());
                push(INT64_VAL(a ^ b));
                break;
            }
            case OP_BIT_NOT: {
                if (!IS_NUMBER(peek(0))) { runtimeError("Operand must be a number."); return INTERPRET_RUNTIME_ERROR; }
                int64_t a = (int64_t)AS_NUMBER(pop());
                push(INT64_VAL(~a));
                break;
            }
            case OP_SHL: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                int64_t b = (int64_t)AS_NUMBER(pop());
                int64_t a = (int64_t)AS_NUMBER(pop());
                if (b < 0 || b >= 64) { runtimeError("Shift amount out of range (0-63)."); return INTERPRET_RUNTIME_ERROR; }
                push(INT64_VAL(a << b));
                break;
            }
            case OP_SHR: {
                if (!IS_NUMBER(peek(0)) || !IS_NUMBER(peek(1))) {
                    runtimeError("Operands must be numbers."); return INTERPRET_RUNTIME_ERROR;
                }
                int64_t b = (int64_t)AS_NUMBER(pop());
                int64_t a = (int64_t)AS_NUMBER(pop());
                if (b < 0 || b >= 64) { runtimeError("Shift amount out of range (0-63)."); return INTERPRET_RUNTIME_ERROR; }
                push(INT64_VAL(a >> b));
                break;
            }

            case OP_PRINT: {
                uint8_t argCount = READ_BYTE();
                // values are on stack in left-to-right order; print in order
                Value* args = vm.stackTop - argCount;
                for (int i = 0; i < argCount; i++) {
                    printValue(args[i]);
                }
                printf("\n");
                for (int i = 0; i < argCount; i++) pop();
                break;
            }

            case OP_READ_LINE: {
                char buf[1024];
                if (fgets(buf, sizeof(buf), stdin) == NULL) {
                    push(NIL_VAL);
                    break;
                }
                size_t len = strlen(buf);
                while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
                char* end;
                double num = strtod(buf, &end);
                if (end != buf && *end == '\0' && len > 0) {
                    push(NUMBER_VAL(num));
                } else {
                    push(OBJ_VAL(copyString(buf, (int)len)));
                }
                break;
            }

            case OP_JUMP: {
                uint16_t offset = READ_SHORT();
                frame->ip += offset;
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset = READ_SHORT();
                if (isFalsey(peek(0))) frame->ip += offset;
                break;
            }
            case OP_LOOP: {
                uint16_t offset = READ_SHORT();
                frame->ip -= offset;
                // Loop-scope region rewind (see regionRewind's doc
                // comment for the tradeoff this accepts). OP_LOOP marks
                // "one iteration just finished, jumping back to the
                // top" for every while/for/continue in this VM, so this
                // is the one point where we know an iteration's
                // temporaries (string concat results, to-string
                // coercions) are done being used *for the common case*
                // where nothing from this iteration was stashed
                // somewhere that outlives the loop. Resetting the
                // frame's region bump pointer here keeps a tight loop
                // that builds lots of short-lived strings/lists from
                // growing frame->region without bound across
                // iterations, instead of only being reclaimed once the
                // whole function returns.
                //
                // noRewind: the compiler statically proved (see
                // markLoopEscapeIfLocalPredatesLoop / LoopCtx.escapes in
                // compiler.c) that this loop's body assigns a value into
                // a local declared *before* the loop -- e.g. `saved =
                // s;` where `s` is loop-produced and `saved` is read
                // after the loop ends. Rewinding in that case would
                // silently corrupt `saved` once a later iteration
                // reused the same freed memory (this was Proton 11's
                // documented "Döngü-Scope Rewind" unsoundness). When
                // this flag is set we skip the rewind for this
                // back-edge, trading the steady-state memory win for
                // correctness in exactly the loops where escape is
                // possible; loops with no such assignment are
                // unaffected and keep the full rewind benefit.
                uint8_t noRewind = READ_BYTE();
                if (!noRewind) {
                    regionRewind(frame->region, frame->loopCheckpoint);
                }
                frame->loopCheckpoint = regionCheckpoint(frame->region);
                break;
            }

            case OP_CALL: {
                uint8_t nameConstIdx = READ_BYTE();
                uint8_t argCount = READ_BYTE();
                ObjString* name = AS_STRING(frame->function->chunk.constants.values[nameConstIdx]);
                Value callee;
                if (!tableGet(&vm.globals, name, &callee) || !IS_FUNCTION(callee)) {
                    runtimeError("Undefined function '%s'.", name->chars);
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjFunction* function = AS_FUNCTION(callee);
                if (function->arity != argCount) {
                    runtimeError("Function '%s' expects %d arguments but got %d.",
                                  name->chars, function->arity, argCount);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm.frameCount == FRAMES_MAX) {
                    runtimeError("Stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame* newFrame = &vm.frames[vm.frameCount++];
                newFrame->function = function;
                newFrame->ip = function->chunk.code;
                newFrame->slots = vm.stackTop - argCount;
                // Faz 1 LAM: every call frame owns its own region for
                // runtime-local string temporaries. Created here, torn
                // down unconditionally in OP_RETURN below. Linked to the
                // caller's region (frame->region) so an escaping value can
                // be promoted one step up the chain instead of straight to
                // permanent storage -- see promoteEscapingValue.
                newFrame->region = regionCreate(frame->region);
                newFrame->loopCheckpoint = regionCheckpoint(newFrame->region);
                frame = newFrame;
                break;
            }

            case OP_CALL_VALUE: {
                uint8_t argCount = READ_BYTE();
                Value callee = peek(argCount);
                if (!IS_FUNCTION(callee)) {
                    runtimeError("Can only call functions.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjFunction* function = AS_FUNCTION(callee);
                if (function->arity != argCount) {
                    runtimeError("Function '%s' expects %d arguments but got %d.",
                                  function->name ? function->name->chars : "?",
                                  function->arity, argCount);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm.frameCount == FRAMES_MAX) {
                    runtimeError("Stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Slide the arguments down by one to cover the callee
                // slot -- OP_CALL's newFrame->slots = stackTop - argCount
                // assumes slot 0 is the first argument, not the callee
                // itself, so the callee Value must not remain on the
                // stack under the args the way OP_GET_LOCAL/params expect.
                Value* calleeSlot = vm.stackTop - argCount - 1;
                for (int i = 0; i < argCount; i++) {
                    calleeSlot[i] = calleeSlot[i + 1];
                }
                vm.stackTop--;
                CallFrame* newFrame = &vm.frames[vm.frameCount++];
                newFrame->function = function;
                newFrame->ip = function->chunk.code;
                newFrame->slots = vm.stackTop - argCount;
                newFrame->region = regionCreate(frame->region);
                newFrame->loopCheckpoint = regionCheckpoint(newFrame->region);
                frame = newFrame;
                break;
            }

            case OP_RETURN: {
                Value result = pop();
                // If the returned value is a string that lives in the
                // frame we're about to tear down, it must be relocated
                // before regionDestroy runs below, or the caller would
                // receive a dangling pointer. promoteEscapingValue walks
                // one step up the region chain (to the caller's region)
                // instead of jumping straight to permanent storage -- see
                // its comment for why this bounds the "return a fresh
                // string in a loop" leak instead of eliminating it only
                // in the no-escape case.
                Region* returningRegion = frame->region;
                result = promoteEscapingValue(returningRegion, result);
                vm.frameCount--;
                if (vm.frameCount == stopDepth) {
                    if (stopDepth == 0) {
                        pop(); // discard leftover script value if any
                    } else {
                        push(result); // caller (native re-entrant call) wants this
                    }
                    regionDestroy(returningRegion);
                    return INTERPRET_OK;
                }
                vm.stackTop = frame->slots;
                push(result);
                regionDestroy(returningRegion);
                frame = &vm.frames[vm.frameCount - 1];
                break;
            }

            case OP_CHECK_TYPE: {
                ProtonType type = (ProtonType)READ_BYTE();
                Value v = peek(0);
                bool ok;
                switch (type) {
                    case PTYPE_BOOL: ok = IS_BOOL(v); break;
                    case PTYPE_STRING: ok = IS_STRING(v); break;
                    case PTYPE_CHAR:
                        ok = (IS_STRING(v) && AS_STRING(v)->length == 1) ||
                             (IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 0x10FFFF));
                        break;
                    case PTYPE_BYTE:
                    case PTYPE_UINT8: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 255); break;
                    case PTYPE_INT8: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -128, 127); break;
                    case PTYPE_INT16:
                    case PTYPE_SHORT: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -32768, 32767); break;
                    case PTYPE_UINT16: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 65535); break;
                    case PTYPE_INT32:
                    case PTYPE_INT: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), -2147483648.0, 2147483647.0); break;
                    case PTYPE_UINT32:
                    case PTYPE_UINT: ok = IS_NUMBER(v) && isIntegerInRange(AS_NUMBER(v), 0, 4294967295.0); break;
                    case PTYPE_INT64:
                    case PTYPE_LONG: {
                        // True signed 64-bit range. A value already tagged
                        // NUM_I64 is trivially in range (it came from an
                        // exact int64 source); NUM_U64 or NUM_F64 values are
                        // checked against the double-safe-integer bound,
                        // since a double can't represent the full int64
                        // range exactly and we can't validate what we can't
                        // represent -- literals/arithmetic beyond +-2^53-1
                        // that aren't already NUM_I64 remain a known
                        // limitation (see README).
                        ok = IS_NUMBER(v) && (NUM_KIND(v) == NUM_I64 ||
                             isIntegerInRange(AS_NUMBER(v), -9007199254740991.0, 9007199254740991.0));
                        break;
                    }
                    case PTYPE_UINT64: {
                        ok = IS_NUMBER(v) && (NUM_KIND(v) == NUM_U64 ||
                             (NUM_KIND(v) != NUM_I64 && isIntegerInRange(AS_NUMBER(v), 0, 9007199254740991.0)) ||
                             (NUM_KIND(v) == NUM_I64 && AS_I64(v) >= 0));
                        break;
                    }
                    case PTYPE_FLOAT32:
                    case PTYPE_FLOAT:
                    case PTYPE_FLOAT64:
                    case PTYPE_DOUBLE:
                    case PTYPE_DECIMAL: ok = IS_NUMBER(v); break;
                    default: ok = true; break;
                }
                if (!ok) {
                    runtimeError("Type mismatch: expected '%s', got %s.",
                                  protonTypeName(type), valueTypeName(v));
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Re-tag int64/uint64-checked values so they carry an exact
                // 64-bit representation from this point on (assignment,
                // storage, later arithmetic-via-AS_NUMBER still works since
                // AS_NUMBER reads any kind, but equality/printing now stay
                // exact instead of clipping to +-2^53-1).
                if (type == PTYPE_INT64 || type == PTYPE_LONG) {
                    if (NUM_KIND(v) != NUM_I64) {
                        vm.stackTop[-1] = INT64_VAL((int64_t)AS_NUMBER(v));
                    }
                } else if (type == PTYPE_UINT64) {
                    if (NUM_KIND(v) != NUM_U64) {
                        vm.stackTop[-1] = UINT64_VAL((uint64_t)AS_NUMBER(v));
                    }
                }
                break;
            }

            case OP_BUILD_LIST: {
                uint8_t elemCount = READ_BYTE();
                ProtonType elemType = (ProtonType)READ_BYTE();
                ObjList* list = newList(frame->region, elemType);
                Value* elems = vm.stackTop - elemCount;
                for (int i = 0; i < elemCount; i++) {
                    if (elemType != PTYPE_NONE && !valueMatchesType(elems[i], elemType)) {
                        runtimeError("List element %d: expected '%s', got %s.",
                                      i, protonTypeName(elemType), valueTypeName(elems[i]));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    appendList(list, elems[i]); // amortized O(1); already right-sized in practice
                }
                vm.stackTop -= elemCount;
                push(OBJ_VAL(list));
                break;
            }

            case OP_GET_INDEX: {
                Value indexVal = pop();
                Value targetVal = pop();
                if (IS_STRING(targetVal)) {
                    if (!IS_NUMBER(indexVal) || !isIntegerInRange(AS_NUMBER(indexVal), 0, (double)INT32_MAX)) {
                        runtimeError("String index must be a non-negative integer.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjString* str = AS_STRING(targetVal);
                    int index = (int)AS_NUMBER(indexVal);
                    if (index < 0 || index >= str->length) {
                        runtimeError("String index %d out of bounds (length %d).", index, str->length);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    // s[i] yields a fresh, single-character string. It's
                    // region-scoped like any other runtime-produced string
                    // (LAM rules -- see README), NOT a view/slice into the
                    // original: mirrors OP_CHAR_FROM_CODE's allocation shape.
                    char* buf = (char*)malloc(2);
                    buf[0] = str->chars[index];
                    buf[1] = '\0';
                    ObjString* result = regionTakeString(frame->region, buf, 1);
                    push(OBJ_VAL(result));
                    break;
                }
                if (IS_MAP(targetVal)) {
                    if (!IS_STRING(indexVal)) {
                        runtimeError("Map index must be a string.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjMap* map = AS_MAP(targetVal);
                    Value out;
                    // O(1) average-case tableGet -- same hash table used
                    // for globals/string interning, no new traversal.
                    if (!tableGet(&map->table, AS_STRING(indexVal), &out)) {
                        runtimeError("Undefined map key '%s'.", AS_CSTRING(indexVal));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(out);
                    break;
                }
                if (!IS_LIST(targetVal)) {
                    runtimeError("Cannot index a value of type %s.", valueTypeName(targetVal));
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!IS_NUMBER(indexVal) || !isIntegerInRange(AS_NUMBER(indexVal), 0, (double)INT32_MAX)) {
                    runtimeError("List index must be a non-negative integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjList* list = AS_LIST(targetVal);
                int index = (int)AS_NUMBER(indexVal);
                if (index < 0 || index >= list->count) {
                    runtimeError("List index %d out of bounds (length %d).", index, list->count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(list->items[index]); // O(1) direct pointer-arithmetic access
                break;
            }

            case OP_SET_INDEX: {
                Value value = pop();
                Value indexVal = pop();
                Value targetVal = pop();
                if (IS_STRING(targetVal)) {
                    runtimeError("Strings are immutable: cannot assign to s[i].");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (IS_MAP(targetVal)) {
                    if (!IS_STRING(indexVal)) {
                        runtimeError("Map index must be a string.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    ObjMap* map = AS_MAP(targetVal);
                    // ObjMap is always permanently allocated (see object.h),
                    // so any regional value written into it must be
                    // promoted straight to permanent storage first --
                    // otherwise, if `value` is a still-region-owned string
                    // or list from the *current* frame, the map would end
                    // up holding a pointer into a region that gets torn
                    // down at this frame's OP_RETURN, long before the map
                    // itself dies. This is the escape path noted in the
                    // README's "Ömür (lifetime)" section: writing a local
                    // regional value into a container that outlives the
                    // current frame, via assignment rather than `return`.
                    value = promoteRegionalValue(NULL, value);
                    // O(1) average-case tableSet insert-or-overwrite.
                    tableSet(&map->table, AS_STRING(indexVal), value);
                    push(value);
                    break;
                }
                if (!IS_LIST(targetVal)) {
                    runtimeError("Cannot index a value of type %s.", valueTypeName(targetVal));
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!IS_NUMBER(indexVal) || !isIntegerInRange(AS_NUMBER(indexVal), 0, (double)INT32_MAX)) {
                    runtimeError("List index must be a non-negative integer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjList* list = AS_LIST(targetVal);
                int index = (int)AS_NUMBER(indexVal);
                if (index < 0 || index >= list->count) {
                    runtimeError("List index %d out of bounds (length %d).", index, list->count);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (list->elemType != PTYPE_NONE && !valueMatchesType(value, list->elemType)) {
                    runtimeError("List element assignment: expected '%s', got %s.",
                                  protonTypeName(list->elemType), valueTypeName(value));
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Same hazard as the map case above, but the destination
                // region varies: a regional (or permanent, region==NULL)
                // list's own `region` field tells us exactly where `value`
                // needs to live. promoteRegionalValue is a no-op if it's
                // already there (e.g. assigning an element already owned
                // by this same list's region back into itself).
                value = promoteRegionalValue(list->region, value);
                list->items[index] = value; // O(1) direct pointer-arithmetic store
                push(value);
                break;
            }

            case OP_LEN: {
                Value v = pop();
                if (IS_STRING(v)) {
                    push(NUMBER_VAL(AS_STRING(v)->length)); // O(1): length is stored metadata
                    break;
                }
                if (IS_MAP(v)) {
                    push(NUMBER_VAL(AS_MAP(v)->table.count)); // O(1): count is stored metadata, no traversal
                    break;
                }
                if (!IS_LIST(v)) {
                    runtimeError("len() expects a string, list, or map, got %s.", valueTypeName(v));
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(NUMBER_VAL(AS_LIST(v)->count)); // O(1): count is stored metadata, no traversal
                break;
            }

            case OP_LIST_PUSH: {
                Value value = pop();
                Value targetVal = pop();
                if (!IS_LIST(targetVal)) {
                    runtimeError("push() expects a list as its first argument, got %s.", valueTypeName(targetVal));
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjList* list = AS_LIST(targetVal);
                if (list->elemType != PTYPE_NONE && !valueMatchesType(value, list->elemType)) {
                    runtimeError("push(): expected element of type '%s', got %s.",
                                  protonTypeName(list->elemType), valueTypeName(value));
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Same escape hazard as OP_SET_INDEX: a regional value from
                // the *current* frame being written into a list that may be
                // owned by an outer frame's (or the permanent) region must
                // be promoted to the list's own region first.
                value = promoteRegionalValue(list->region, value);
                appendList(list, value); // amortized O(1)
                push(OBJ_VAL(list)); // push(list, value) yields the (same, now-longer) list
                break;
            }

            case OP_LIST_COPY: {
                Value v = pop();
                if (!IS_LIST(v)) {
                    runtimeError("listCopy() expects a list, got %s.", valueTypeName(v));
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjList* src = AS_LIST(v);
                // Independent copy owned by the *current* frame's region
                // (or permanent storage if this frame has none), so
                // mutating the copy never aliases the original -- the
                // exact gap `var b = a;` leaves open for lists (Value
                // assignment just copies the ObjList pointer).
                ObjList* copy = frame->region != NULL
                    ? regionCopyList(frame->region, src)
                    : permanentCopyList(src);
                push(OBJ_VAL(copy));
                break;
            }

            case OP_BUILD_MAP: {
                uint8_t pairCount = READ_BYTE();
                ObjMap* map = newMap();
                Value* pairs = vm.stackTop - (pairCount * 2);
                for (int i = 0; i < pairCount; i++) {
                    Value key = pairs[i * 2];
                    Value val = pairs[i * 2 + 1];
                    // Compiler only ever emits string-constant keys for map
                    // literals (see compiler.c: mapLiteral), so this is not
                    // a user-reachable runtime error path, but guarded
                    // defensively for robustness.
                    if (!IS_STRING(key)) {
                        runtimeError("Map keys must be strings.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    // ObjMap is always permanently allocated (object.h),
                    // so a regional value (e.g. a runtime-built string
                    // used as a map literal value) must be promoted to
                    // permanent storage now -- same hazard/fix as
                    // OP_SET_INDEX above.
                    val = promoteRegionalValue(NULL, val);
                    tableSet(&map->table, AS_STRING(key), val); // O(1) amortized
                }
                vm.stackTop -= (pairCount * 2);
                push(OBJ_VAL(map));
                break;
            }

            case OP_TRY: {
                // '?' operator (postfix). If the value just computed is an
                // error, unwind: pop it, tear down the *current* frame's
                // region (its locals/temporaries are gone the same way
                // OP_RETURN tears them down), pop the frame itself, and
                // push the error onto the caller's stack in place of
                // whatever return value that call site expected. If the
                // caller frame is itself the outermost script frame (no
                // caller left), this behaves like an uncaught error
                // propagating out of main() -- report it and halt, same
                // exit path as any other runtime error.
                if (IS_ERROR(peek(0))) {
                    Value err = pop();
                    Region* returningRegion = frame->region;
                    vm.frameCount--;
                    if (vm.frameCount == 0) {
                        regionDestroy(returningRegion);
                        runtimeError("Uncaught error: %s", AS_ERROR_CSTRING(err));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vm.stackTop = frame->slots;
                    push(err);
                    regionDestroy(returningRegion);
                    frame = &vm.frames[vm.frameCount - 1];
                }
                // If not an error, fall through: value stays on the stack
                // untouched, execution continues normally.
                break;
            }

            case OP_FS_READ: {
                Value pathVal = pop();
                if (!IS_STRING(pathVal)) {
                    runtimeError("fs_read() expects a string path.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                FILE* fp = fopen(AS_CSTRING(pathVal), "rb");
                if (fp == NULL) {
                    push(ERROR_VAL("Dosya okunamadi"));
                    break;
                }
                fseek(fp, 0L, SEEK_END);
                long size = ftell(fp);
                if (size < 0) {
                    fclose(fp);
                    push(ERROR_VAL("Dosya okunamadi"));
                    break;
                }
                rewind(fp);
                char* buf = (char*)malloc((size_t)size + 1);
                size_t bytesRead = fread(buf, 1, (size_t)size, fp);
                fclose(fp);
                buf[bytesRead] = '\0';
                // Region-scoped: this is a runtime-local temporary tied to
                // the currently executing frame, same treatment as
                // concatenation results (see concatenate()/toStringValue).
                ObjString* result = regionTakeString(frame->region, buf, (int)bytesRead);
                push(OBJ_VAL(result));
                break;
            }

            case OP_FS_WRITE: {
                Value contentVal = pop();
                Value pathVal = pop();
                if (!IS_STRING(pathVal) || !IS_STRING(contentVal)) {
                    runtimeError("fs_write() expects (string path, string content).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                FILE* fp = fopen(AS_CSTRING(pathVal), "w");
                if (fp == NULL) {
                    push(ERROR_VAL("Dosyaya yazilamadi"));
                    break;
                }
                ObjString* content = AS_STRING(contentVal);
                fwrite(content->chars, 1, (size_t)content->length, fp);
                fclose(fp);
                push(NIL_VAL);
                break;
            }

            case OP_FS_EXISTS: {
                Value pathVal = pop();
                if (!IS_STRING(pathVal)) {
                    runtimeError("fs_exists() expects a string path.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                // O(1) existence check via the POSIX access() syscall --
                // no directory traversal from Proton's side, just a single
                // stat-class kernel call.
                bool exists = access(AS_CSTRING(pathVal), F_OK) == 0;
                push(BOOL_VAL(exists));
                break;
            }

            case OP_SYS_EXEC: {
                Value cmdVal = pop();
                if (!IS_STRING(cmdVal)) {
                    runtimeError("sys_exec() expects a string command.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                FILE* pipe = popen(AS_CSTRING(cmdVal), "r");
                if (pipe == NULL) {
                    push(ERROR_VAL("Komut calistirilamadi"));
                    break;
                }
                size_t cap = 4096;
                size_t len = 0;
                char* buf = (char*)malloc(cap);
                size_t n;
                char chunk[1024];
                while ((n = fread(chunk, 1, sizeof(chunk), pipe)) > 0) {
                    if (len + n + 1 > cap) {
                        size_t newCap = cap * 2;
                        while (newCap < len + n + 1) newCap *= 2;
                        char* grown = (char*)malloc(newCap);
                        memcpy(grown, buf, len);
                        free(buf);
                        buf = grown;
                        cap = newCap;
                    }
                    memcpy(buf + len, chunk, n);
                    len += n;
                }
                pclose(pipe);
                buf[len] = '\0';
                // Region-scoped runtime-local temporary, same as fs_read.
                ObjString* result = regionTakeString(frame->region, buf, (int)len);
                push(OBJ_VAL(result));
                break;
            }

            case OP_ASSERT: {
                Value cond = pop();
                if (isFalsey(cond)) {
                    runtimeError("Assertion failed.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }

            case OP_PANIC: {
                uint8_t argCount = READ_BYTE();
                Value* args = vm.stackTop - argCount;
                fprintf(stderr, "panic: ");
                for (int i = 0; i < argCount; i++) {
                    ObjString* s = toStringValue(frame->region, args[i]);
                    fprintf(stderr, "%s", s->chars);
                }
                fprintf(stderr, "\n");
                CallFrame* f = &vm.frames[vm.frameCount - 1];
                fprintf(stderr, "[line %d] in %s\n", currentLine(f),
                        f->function->name ? f->function->name->chars : "<script>");
                return INTERPRET_RUNTIME_ERROR;
            }

            case OP_CHAR_CODE: {
                Value strVal = pop();
                if (!IS_STRING(strVal)) {
                    runtimeError("char_code() expects a 1-character string.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjString* s = AS_STRING(strVal);
                if (s->length != 1) {
                    runtimeError("char_code() expects a string of length 1, got length %d.", s->length);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(INT64_VAL((int64_t)(unsigned char)s->chars[0]));
                break;
            }

            case OP_CHAR_FROM_CODE: {
                Value codeVal = pop();
                if (!IS_NUMBER(codeVal)) {
                    runtimeError("char_from_code() expects an integer code point.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double d = AS_NUMBER(codeVal);
                int code = (int)d;
                if (code < 0 || code > 255) {
                    runtimeError("char_from_code() expects a code point in range 0-255, got %d.", code);
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* buf = (char*)malloc(2);
                buf[0] = (char)code;
                buf[1] = '\0';
                ObjString* result = regionTakeString(frame->region, buf, 1);
                push(OBJ_VAL(result));
                break;
            }

            case OP_TIME_NOW: {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                int64_t millis = (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
                push(INT64_VAL(millis));
                break;
            }

            case OP_TIME_TICKS: {
                // CLOCK_MONOTONIC: immune to wall-clock adjustments (NTP
                // steps, DST, manual changes), the right clock for
                // measuring elapsed durations / profiling.
                struct timespec ts;
                clock_gettime(CLOCK_MONOTONIC, &ts);
                int64_t micros = (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
                push(INT64_VAL(micros));
                break;
            }

            case OP_TIME_CLOCK: {
                // Process CPU time (user+system), not wall time -- matches
                // ANSI C clock()'s traditional meaning.
                double seconds = (double)clock() / (double)CLOCKS_PER_SEC;
                push(NUMBER_VAL(seconds));
                break;
            }

            case OP_TIME_SLEEP: {
                Value msVal = pop();
                if (!IS_NUMBER(msVal)) {
                    runtimeError("time::sleep() expects a numeric millisecond duration.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double ms = AS_NUMBER(msVal);
                if (ms > 0) {
                    struct timespec req;
                    req.tv_sec = (time_t)(ms / 1000.0);
                    req.tv_nsec = (long)(((int64_t)ms % 1000) * 1000000L +
                                          (ms - (double)(int64_t)ms) * 1000000.0);
                    // Restart on EINTR so a stray signal doesn't cut the
                    // sleep short.
                    while (nanosleep(&req, &req) == -1 && errno == EINTR) { }
                }
                push(NIL_VAL);
                break;
            }

            case OP_TIME_FORMAT: {
                Value fmtVal = pop();
                Value tsVal = pop();
                if (!IS_STRING(fmtVal) || !IS_NUMBER(tsVal)) {
                    runtimeError("time::format() expects (int timestampMs, string fmt).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                int64_t millis = (NUM_KIND(tsVal) == NUM_I64) ? AS_I64(tsVal) : (int64_t)AS_NUMBER(tsVal);
                time_t secs = (time_t)(millis / 1000);
                struct tm tmVal;
                gmtime_r(&secs, &tmVal); // UTC -- deterministic, no local-timezone dependency
                char buf[256];
                size_t written = strftime(buf, sizeof(buf), AS_CSTRING(fmtVal), &tmVal);
                if (written == 0 && AS_CSTRING(fmtVal)[0] != '\0') {
                    // strftime returns 0 both for "empty result" and
                    // "buffer too small"; a non-empty format producing
                    // nothing almost always means the latter here.
                    push(ERROR_VAL("Zaman formati gecersiz veya cok uzun"));
                    break;
                }
                ObjString* result = regionCopyString(frame->region, buf, (int)written);
                push(OBJ_VAL(result));
                break;
            }

            case OP_TIME_PARSE: {
                Value fmtVal = pop();
                Value strVal = pop();
                if (!IS_STRING(fmtVal) || !IS_STRING(strVal)) {
                    runtimeError("time::parse() expects (string dateStr, string fmt).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                struct tm tmVal;
                memset(&tmVal, 0, sizeof(tmVal));
                char* end = strptime(AS_CSTRING(strVal), AS_CSTRING(fmtVal), &tmVal);
                if (end == NULL) {
                    push(ERROR_VAL("Tarih ayristirilamadi"));
                    break;
                }
                time_t secs = timegm(&tmVal); // interpret as UTC, matching time::format's gmtime_r
                if (secs == (time_t)-1) {
                    push(ERROR_VAL("Tarih ayristirilamadi"));
                    break;
                }
                push(INT64_VAL((int64_t)secs * 1000));
                break;
            }

            case OP_SYS_ENV: {
                Value nameVal = pop();
                if (!IS_STRING(nameVal)) {
                    runtimeError("sys::env() expects a string variable name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                const char* value = getenv(AS_CSTRING(nameVal));
                if (value == NULL) {
                    push(NIL_VAL);
                } else {
                    int len = (int)strlen(value);
                    ObjString* result = regionCopyString(frame->region, value, len);
                    push(OBJ_VAL(result));
                }
                break;
            }

            case OP_SYS_ARGS: {
                ObjList* list = newList(frame->region, PTYPE_STRING);
                for (int i = 0; i < vm.scriptArgc; i++) {
                    const char* arg = vm.scriptArgv[i];
                    int len = (int)strlen(arg);
                    ObjString* s = regionCopyString(frame->region, arg, len);
                    appendList(list, OBJ_VAL(s));
                }
                push(OBJ_VAL(list));
                break;
            }

            case OP_SYS_SETENV: {
                Value valueVal = pop();
                Value nameVal = pop();
                if (!IS_STRING(nameVal) || !IS_STRING(valueVal)) {
                    runtimeError("sys::setenv() expects two string arguments (name, value).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (setenv(AS_CSTRING(nameVal), AS_CSTRING(valueVal), 1) != 0) {
                    push(ERROR_VAL("Ortam degiskeni ayarlanamadi"));
                    break;
                }
                push(NIL_VAL);
                break;
            }

            case OP_SYS_EXIT: {
                Value codeVal = pop();
                if (!IS_NUMBER(codeVal)) {
                    runtimeError("sys::exit() expects an integer exit code.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                fflush(stdout);
                fflush(stderr);
                exit((int)AS_NUMBER(codeVal));
                // unreachable
                break;
            }

            case OP_SYS_PID: {
                push(NUMBER_VAL((double)getpid()));
                break;
            }

            case OP_SYS_PPID: {
                push(NUMBER_VAL((double)getppid()));
                break;
            }

            case OP_NET_GET: {
                Value urlVal = pop();
                if (!IS_STRING(urlVal)) {
                    runtimeError("net::get() expects a string URL.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* body = NULL;
                size_t bodyLen = 0;
                char* errMsg = NULL;
                bool ok = protonHttpRequest("GET", AS_CSTRING(urlVal), NULL, &body, &bodyLen, &errMsg);
                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "Istek basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                ObjString* result = regionTakeString(frame->region, body, (int)bodyLen);
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_POST: {
                Value bodyVal = pop();
                Value urlVal = pop();
                if (!IS_STRING(urlVal) || !IS_STRING(bodyVal)) {
                    runtimeError("net::post() expects (string url, string body).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* respBody = NULL;
                size_t respLen = 0;
                char* errMsg = NULL;
                bool ok = protonHttpRequest("POST", AS_CSTRING(urlVal), AS_CSTRING(bodyVal), &respBody, &respLen, &errMsg);
                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "Istek basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                ObjString* result = regionTakeString(frame->region, respBody, (int)respLen);
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_REQUEST: {
                Value optsVal = pop();
                if (!IS_MAP(optsVal)) {
                    runtimeError("net::request() expects a Map of options.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                ObjMap* opts = AS_MAP(optsVal);

                Value v;
                const char* url = NULL;
                {
                    ObjString* k = copyString("url", 3);
                    if (tableGet(&opts->table, k, &v) && IS_STRING(v)) url = AS_CSTRING(v);
                }
                if (url == NULL) {
                    runtimeError("net::request() options must include a string 'url'.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* method = "GET";
                {
                    ObjString* k = copyString("method", 6);
                    if (tableGet(&opts->table, k, &v) && IS_STRING(v)) method = AS_CSTRING(v);
                }

                const char* body = NULL;
                {
                    ObjString* k = copyString("body", 4);
                    if (tableGet(&opts->table, k, &v) && IS_STRING(v)) body = AS_CSTRING(v);
                }

                int timeoutMs = 10000;
                {
                    ObjString* k = copyString("timeout", 7);
                    if (tableGet(&opts->table, k, &v) && IS_NUMBER(v)) timeoutMs = (int)AS_NUMBER(v);
                }

                // Optional "headers" sub-map -> serialized "Name: value\r\n" lines.
                char* headerBlock = NULL;
                {
                    ObjString* k = copyString("headers", 7);
                    if (tableGet(&opts->table, k, &v) && IS_MAP(v)) {
                        ObjMap* hmap = AS_MAP(v);
                        size_t cap = 256;
                        size_t used = 0;
                        headerBlock = (char*)malloc(cap);
                        headerBlock[0] = '\0';
                        for (int i = 0; i < hmap->table.capacity; i++) {
                            Entry* e = &hmap->table.entries[i];
                            if (e->key == NULL) continue;
                            if (!IS_STRING(e->value)) continue;
                            const char* hv = AS_CSTRING(e->value);
                            size_t need = e->key->length + strlen(hv) + 4; // "K: V\r\n"
                            if (used + need + 1 > cap) {
                                while (used + need + 1 > cap) cap *= 2;
                                headerBlock = (char*)realloc(headerBlock, cap);
                            }
                            used += (size_t)snprintf(headerBlock + used, cap - used, "%s: %s\r\n", e->key->chars, hv);
                        }
                    }
                }

                int status = 0;
                char* respBody = NULL;
                size_t respLen = 0;
                char* errMsg = NULL;
                ObjMap* respHeaders = newMap();

                bool ok = protonHttpRequestEx(method, url, body, headerBlock, timeoutMs,
                                               &status, &respBody, &respLen, respHeaders, &errMsg);
                if (headerBlock) free(headerBlock);

                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "Istek basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }

                ObjString* bodyStr = regionTakeString(frame->region, respBody, (int)respLen);
                ObjMap* result = newMap();
                tableSet(&result->table, copyString("status", 6), NUMBER_VAL(status));
                tableSet(&result->table, copyString("body", 4), OBJ_VAL(bodyStr));
                tableSet(&result->table, copyString("headers", 7), OBJ_VAL(respHeaders));
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_RESOLVE: {
                Value hostVal = pop();
                if (!IS_STRING(hostVal)) {
                    runtimeError("net::resolve() expects a string hostname.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* errMsg = NULL;
                char* ip = protonNetResolve(AS_CSTRING(hostVal), &errMsg);
                if (ip == NULL) {
                    push(ERROR_VAL(errMsg ? errMsg : "DNS cozumlemesi basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                ObjString* result = regionTakeString(frame->region, ip, (int)strlen(ip));
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_PING: {
                Value timeoutVal = pop();
                Value hostVal = pop();
                if (!IS_STRING(hostVal) || !IS_NUMBER(timeoutVal)) {
                    runtimeError("net::ping() expects (string host, number timeoutMs).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                double ms = protonNetPing(AS_CSTRING(hostVal), (int)AS_NUMBER(timeoutVal));
                push(NUMBER_VAL(ms));
                break;
            }

            case OP_NET_URLENCODE: {
                Value strVal = pop();
                if (!IS_STRING(strVal)) {
                    runtimeError("net::urlEncode() expects a string.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* enc = protonUrlEncode(AS_CSTRING(strVal));
                ObjString* result = regionTakeString(frame->region, enc, (int)strlen(enc));
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_URLDECODE: {
                Value strVal = pop();
                if (!IS_STRING(strVal)) {
                    runtimeError("net::urlDecode() expects a string.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* dec = protonUrlDecode(AS_CSTRING(strVal));
                ObjString* result = regionTakeString(frame->region, dec, (int)strlen(dec));
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_SERVE: {
                Value handlerVal = pop();
                Value portVal = pop();
                if (!IS_NUMBER(portVal) || !IS_FUNCTION(handlerVal)) {
                    runtimeError("net::serve() expects (number port, function handler).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                char* errMsg = NULL;
                bool ok = protonNetServe((int)AS_NUMBER(portVal), AS_FUNCTION(handlerVal), &errMsg);
                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "net::serve basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                push(NIL_VAL); // unreachable: protonNetServe only returns on setup failure above
                break;
            }

            case OP_NET_CONNECT: {
                Value protoVal = pop();
                Value portVal = pop();
                Value hostVal = pop();
                if (!IS_STRING(hostVal) || !IS_NUMBER(portVal) || !IS_STRING(protoVal)) {
                    runtimeError("net::connect() expects (string host, number port, string protocol).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                int handle;
                char* errMsg = NULL;
                bool ok = protonNetConnect(AS_CSTRING(hostVal), (int)AS_NUMBER(portVal),
                                            AS_CSTRING(protoVal), &handle, &errMsg);
                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "net::connect basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                push(NUMBER_VAL(handle));
                break;
            }

            case OP_NET_SEND: {
                Value dataVal = pop();
                Value handleVal = pop();
                if (!IS_NUMBER(handleVal) || !IS_STRING(dataVal)) {
                    runtimeError("net::send() expects (number handle, string data).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                long sent;
                char* errMsg = NULL;
                ObjString* dataStr = AS_STRING(dataVal);
                bool ok = protonNetSend((int)AS_NUMBER(handleVal), dataStr->chars, (size_t)dataStr->length, &sent, &errMsg);
                if (!ok) {
                    push(ERROR_VAL(errMsg ? errMsg : "net::send basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                push(NUMBER_VAL((double)sent));
                break;
            }

            case OP_NET_RECV: {
                Value maxVal = pop();
                Value handleVal = pop();
                if (!IS_NUMBER(handleVal) || !IS_NUMBER(maxVal)) {
                    runtimeError("net::recv() expects (number handle, number maxBytes).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                long maxBytes = (long)AS_NUMBER(maxVal);
                // Clamp to a sane range: at least 1, and bounded above so
                // a script can't ask for a multi-gigabyte single-shot
                // buffer and blow up the process's memory in one call.
                if (maxBytes < 1) maxBytes = 1;
                if (maxBytes > (1 << 20)) maxBytes = (1 << 20); // 1 MiB ceiling per call
                char* buf = malloc((size_t)maxBytes);
                size_t gotLen = 0;
                char* errMsg = NULL;
                bool ok = protonNetRecv((int)AS_NUMBER(handleVal), buf, (size_t)maxBytes, &gotLen, &errMsg);
                if (!ok) {
                    free(buf);
                    push(ERROR_VAL(errMsg ? errMsg : "net::recv basarisiz"));
                    if (errMsg) free(errMsg);
                    break;
                }
                ObjString* result = regionTakeString(frame->region, buf, (int)gotLen);
                // regionTakeString takes ownership of buf and frees it
                // itself after copying gotLen bytes into the region --
                // do NOT free(buf) here (that would be a double free).
                // The trailing maxBytes-gotLen slack in buf (if any) is
                // simply never read; regionTakeString only copies
                // `length` (gotLen) bytes out of it before freeing.
                push(OBJ_VAL(result));
                break;
            }

            case OP_NET_CLOSE: {
                Value handleVal = pop();
                if (!IS_NUMBER(handleVal)) {
                    runtimeError("net::close() expects (number handle).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                protonNetClose((int)AS_NUMBER(handleVal));
                push(NIL_VAL);
                break;
            }

            case OP_HALT:
                return INTERPRET_OK;
        }
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_CONSTANT
}

// ---------------------------------------------------------------------
// callProtonFunction -- invokes a script-defined function value with a
// single Value argument, re-entrantly, from native C code (used by
// net::serve to dispatch each incoming request to its handler
// function). Mirrors OP_CALL's frame-push logic exactly, but is driven
// from C instead of from bytecode, and uses run()'s stopDepth parameter
// to return here (with the callee's return value pushed on the stack)
// instead of terminating the whole program the way a top-level return
// would.
//
// Takes the handler as an ObjFunction* directly (a first-class function
// value) rather than looking it up by name -- net::serve's handler
// argument is now an arbitrary expression (a bare fn name, a variable
// holding a function value, etc.), evaluated once at net::serve() call
// time, same as any other argument.
//
// Returns true and leaves the callee's return value in *outResult on
// success. Returns false with *outErr set (malloc'd, caller frees) if
// the handler has the wrong arity or its execution hits a runtime error.
static bool callProtonFunction(ObjFunction* function, Value arg, Value* outResult, char** outErr) {
    *outErr = NULL;
    if (function->arity != 1) {
        *outErr = strdup("Handler fonksiyonu tam olarak 1 parametre almali");
        return false;
    }
    if (vm.frameCount == FRAMES_MAX) {
        *outErr = strdup("Stack overflow (handler cagrisi)");
        return false;
    }

    int stopDepth = vm.frameCount;
    push(arg);
    CallFrame* newFrame = &vm.frames[vm.frameCount++];
    newFrame->function = function;
    newFrame->ip = function->chunk.code;
    newFrame->slots = vm.stackTop - 1;
    newFrame->region = regionCreate(vm.frames[stopDepth - 1].region);
    newFrame->loopCheckpoint = regionCheckpoint(newFrame->region);

    InterpretResult result = run(stopDepth);
    if (result != INTERPRET_OK) {
        *outErr = strdup("Handler calisirken hata olustu");
        return false;
    }

    *outResult = pop();
    return true;
}

// ---------------------------------------------------------------------
// net::serve -- an HTTP server, but not a raw-socket API: the VM owns
// the listening socket, the accept loop, and all HTTP framing. The
// script never sees a socket handle; it only ever gets a parsed request
// map in and returns a response map out. This is deliberately narrower
// than a general bind/listen primitive -- there's no way to speak
// anything other than HTTP/1.1 through it, and no way to open a raw
// listening socket on an arbitrary protocol.
//
// Blocking, single-threaded, one connection at a time -- adequate for
// local development/testing servers and simple REST APIs, not a
// production-grade concurrent server.
static bool protonNetServe(int port, ObjFunction* handler, char** outErr) {
    *outErr = NULL;
    if (port <= 0 || port > 65535) {
        *outErr = strdup("Gecersiz port numarasi");
        return false;
    }

    int serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock < 0) {
        *outErr = strdup("Sunucu soketi olusturulamadi");
        return false;
    }
    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(serverSock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(serverSock);
        *outErr = strdup("Port'a bind edilemedi (kullanimda olabilir)");
        return false;
    }
    if (listen(serverSock, 16) < 0) {
        close(serverSock);
        *outErr = strdup("Dinleme baslatilamadi");
        return false;
    }

    fprintf(stderr, "net::serve: listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientSock = accept(serverSock, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientSock < 0) continue;

        // Bounded read/write so one slow/stuck client can't hang the
        // whole server forever.
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // Read the request (headers + body, using Content-Length once
        // headers are in) -- same incremental-recv-until-we-have-enough
        // approach as protonHttpRequestEx's response reader.
        size_t cap = 8192;
        size_t len = 0;
        char* raw = (char*)malloc(cap);
        char chunk[4096];
        char* headerEnd = NULL;
        size_t contentLength = 0;
        bool haveContentLength = false;

        for (;;) {
            ssize_t n = recv(clientSock, chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            if (len + (size_t)n + 1 > cap) {
                size_t newCap = cap * 2;
                while (newCap < len + (size_t)n + 1) newCap *= 2;
                raw = (char*)realloc(raw, newCap);
                cap = newCap;
            }
            memcpy(raw + len, chunk, (size_t)n);
            len += (size_t)n;
            raw[len] = '\0';

            if (headerEnd == NULL) {
                headerEnd = strstr(raw, "\r\n\r\n");
                if (headerEnd != NULL) {
                    char* cl = strcasestr(raw, "Content-Length:");
                    if (cl != NULL && cl < headerEnd) {
                        contentLength = (size_t)atol(cl + 15);
                        haveContentLength = true;
                    }
                }
            }
            if (headerEnd != NULL) {
                size_t bodySoFar = len - (size_t)(headerEnd + 4 - raw);
                if (!haveContentLength || bodySoFar >= contentLength) break;
            }
        }

        if (len == 0 || headerEnd == NULL) {
            free(raw);
            close(clientSock);
            continue;
        }

        // Parse request line: "METHOD /path HTTP/1.1"
        char methodBuf[16] = {0};
        char pathBuf[2048] = {0};
        sscanf(raw, "%15s %2047s", methodBuf, pathBuf);

        char* bodyStart = headerEnd + 4;
        size_t bodyLen = len - (size_t)(bodyStart - raw);

        // Build the request map for the handler.
        ObjMap* reqMap = newMap();
        tableSet(&reqMap->table, copyString("method", 6),
                  OBJ_VAL(copyString(methodBuf, (int)strlen(methodBuf))));
        tableSet(&reqMap->table, copyString("path", 4),
                  OBJ_VAL(copyString(pathBuf, (int)strlen(pathBuf))));
        tableSet(&reqMap->table, copyString("body", 4),
                  OBJ_VAL(copyString(bodyStart, (int)bodyLen)));

        ObjMap* reqHeaders = newMap();
        {
            char* headBuf = (char*)malloc((size_t)(headerEnd - raw) + 1);
            memcpy(headBuf, raw, (size_t)(headerEnd - raw));
            headBuf[headerEnd - raw] = '\0';
            char* line = strtok(headBuf, "\r\n");
            line = strtok(NULL, "\r\n"); // skip request line
            while (line != NULL) {
                char* colon = strchr(line, ':');
                if (colon != NULL) {
                    *colon = '\0';
                    char* value = colon + 1;
                    while (*value == ' ') value++;
                    tableSet(&reqHeaders->table, copyString(line, (int)strlen(line)),
                              OBJ_VAL(copyString(value, (int)strlen(value))));
                }
                line = strtok(NULL, "\r\n");
            }
            free(headBuf);
        }
        tableSet(&reqMap->table, copyString("headers", 7), OBJ_VAL(reqHeaders));
        free(raw);

        // Dispatch to the script handler.
        Value result;
        char* callErr = NULL;
        int status = 500;
        const char* respBody = "Internal Server Error";
        const char* respBodyAlloc = NULL; // set if we need to free() after sending

        if (callProtonFunction(handler, OBJ_VAL(reqMap), &result, &callErr)) {
            if (IS_MAP(result)) {
                ObjMap* respMap = AS_MAP(result);
                Value v;
                status = 200;
                if (tableGet(&respMap->table, copyString("status", 6), &v) && IS_NUMBER(v)) {
                    status = (int)AS_NUMBER(v);
                }
                if (tableGet(&respMap->table, copyString("body", 4), &v) && IS_STRING(v)) {
                    respBody = AS_CSTRING(v);
                } else {
                    respBody = "";
                }
            } else {
                status = 500;
                respBody = "Handler bir map dondurmedi";
            }
        } else {
            fprintf(stderr, "net::serve: handler error: %s\n", callErr ? callErr : "?");
            if (callErr) free(callErr);
        }

        const char* statusText = (status == 200) ? "OK"
                                 : (status == 404) ? "Not Found"
                                 : (status == 400) ? "Bad Request"
                                 : (status >= 500) ? "Internal Server Error"
                                 : "OK";

        char* respBuf;
        int respLen = asprintf(&respBuf,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            status, statusText, strlen(respBody), respBody);
        if (respLen > 0) {
            size_t sent = 0;
            while (sent < (size_t)respLen) {
                ssize_t n = send(clientSock, respBuf + sent, (size_t)respLen - sent, 0);
                if (n <= 0) break;
                sent += (size_t)n;
            }
            free(respBuf);
        }
        (void)respBodyAlloc;
        close(clientSock);
    }

    close(serverSock); // unreachable in practice (infinite loop above), kept for completeness
    return true;
}

InterpretResult interpretSource(const char* source) {
    if (!compileProgram(source)) {
        return INTERPRET_COMPILE_ERROR;
    }

    ObjString* mainName = copyString("main", 4);
    Value mainVal;
    if (!tableGet(&vm.globals, mainName, &mainVal) || !IS_FUNCTION(mainVal)) {
        fprintf(stderr, "Error: no 'fn main()' entry point found.\n");
        return INTERPRET_COMPILE_ERROR;
    }
    ObjFunction* mainFn = AS_FUNCTION(mainVal);
    if (mainFn->arity != 0) {
        fprintf(stderr, "Error: 'main' must take no arguments.\n");
        return INTERPRET_COMPILE_ERROR;
    }

    resetStack();
    CallFrame* frame = &vm.frames[vm.frameCount++];
    frame->function = mainFn;
    frame->ip = mainFn->chunk.code;
    frame->slots = vm.stack;
    frame->region = regionCreate(NULL); // outermost frame: no enclosing region to escape into
    frame->loopCheckpoint = regionCheckpoint(frame->region);

    return run(0);
}
