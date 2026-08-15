#ifndef PROTON_COMPILER_H
#define PROTON_COMPILER_H

#include <stdbool.h>

// Compiles the whole program. Registers all top-level fn's and
// literal-initialized var/const globals directly into the VM's global
// table (via vmDefineGlobal). Returns true on success.
bool compileProgram(const char* source);

#endif
