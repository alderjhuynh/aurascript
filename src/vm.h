#ifndef VM_H
#define VM_H

#include "bytecode.h"

/* Runs compiled bytecode on the stack-based VM. Program output is printed to
 * stdout. On an internal error (bad opcode, stack underflow, ...), prints a
 * diagnostic to stderr and exits(1). */
void vm_run(Bytecode *bc);

#endif