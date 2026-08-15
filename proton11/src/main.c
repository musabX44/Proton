#include <stdio.h>
#include <stdlib.h>
#include "vm.h"

static char* readFile(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char* buffer = malloc(fileSize + 1);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        exit(74);
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    buffer[bytesRead] = '\0';
    fclose(file);
    return buffer;
}

static void runFile(const char* path) {
    char* source = readFile(path);
    InterpretResult result = interpretSource(source);
    free(source);

    if (result == INTERPRET_COMPILE_ERROR) exit(65);
    if (result == INTERPRET_RUNTIME_ERROR) exit(70);
}

int main(int argc, const char* argv[]) {
    initVM();

    if (argc >= 2) {
        // Anything after the script path is exposed to Proton via
        // sys::args() -- argv[2..argc-1].
        vm.scriptArgc = argc - 2;
        vm.scriptArgv = (argc > 2) ? &argv[2] : NULL;
        runFile(argv[1]);
    } else {
        fprintf(stderr, "Usage: proton <path.prt> [args...]\n");
        freeVM();
        exit(64);
    }

    freeVM();
    return 0;
}
