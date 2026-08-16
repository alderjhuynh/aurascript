# Aurascript

A small toy language written in C whose programs read like English sentences.
An `.ausc` source file is compiled to bytecode and executed on a stack-based
virtual machine.

```text
create a variable called x as an integer with a value of 1.
repeat while the variable x is less than 10:
    write out the variable x
stop repeating.
```

## Building

```sh
make
```

Requires a C11 compiler (the default is `cc`). The binary is `./aurascript`.
`make clean` removes build artifacts.

## Usage

```text
usage: aurascript [--ast | --bytecode [out.txt]] <script.ausc>

  (no flag)            compile the script to bytecode and run it in the VM
  --ast                print the parsed AST
  --bytecode [f]       compile to bytecode and disassemble to f (default stdout)
```

## The language

Aurascript is case-insensitive. Statement terminators (`.`) are optional.
Comments start with `#` or `//` and run to the end of the line.

There are four variable types: `integer`, `double`, `char` and `string`.

| Statement | Meaning |
| --- | --- |
| `create a variable called NAME as an integer [with a value of EXPR].` | declare an integer (0 if no initial value) |
| `create a variable called NAME as a double [with a value of EXPR].` | declare a double (`0` if no initial value) |
| `create a variable called NAME as a char [with a value of EXPR].` | declare a char (`'\0'` if no initial value) |
| `create a variable called NAME as a string [with a value of EXPR].` | declare a string (`""` if no initial value) |
| `create a variable called NAME as an array of TYPE with a size of EXPR.` | declare an array of `EXPR` elements, each a `TYPE` default value |
| `assign the variable called NAME the value of EXPR.` | assign to a variable |
| `assign the variable called NAME at the index EXPR the value of EXPR2.` | assign to one element of an array |
| `add the variable SRC to the variable DST.` | `DST += SRC` (integers, doubles, or string concatenation) |
| `subtract the variable SRC from the variable DST.` | `DST -= SRC` (integers, doubles) |
| `multiply the variable SRC by the variable DST.` | `DST *= SRC` (integers, doubles) |
| `divide the variable SRC by the variable DST.` | `DST /= SRC` (integers, doubles) |
| `modulo the variable SRC by the variable DST.` | `DST %= SRC` (integers, doubles) |
| `write out the variable NAME.` | print a variable |
| `write out the variable NAME at the index EXPR.` | print one element of an array |
| `write out the string "TEXT".` | print a literal |
| `write out the char 'X'.` | print a character literal |
| `write out ITEM and ITEM ...` | print several items on the same line |
| `read the input into the variable called NAME.` | read a line from stdin into a variable |
| `read the input into the variable called NAME at the index EXPR.` | read a line from stdin into one array element |
| `wait for EXPR seconds.` | pause for `EXPR` seconds |
| `wait for EXPR milliseconds.` | pause for `EXPR` milliseconds |
| `repeat while CONDITION: ... stop repeating.` | while loop |
| `when CONDITION STATEMENT [otherwise STATEMENT]` | conditional statement (optional else, single-statement bodies) |
| `when CONDITION: STATEMENTS [otherwise: STATEMENTS] stop when.` | conditional statement (optional else, block bodies) |
| `create a function called NAME [with a parameter called P as a TYPE (and ...)*]: ... stop the function.` | define a function |
| `call the function called NAME [with the value of EXPR (and the value of EXPR ...)*].` | call a function, discarding its result |
| `return [the value of EXPR].` | leave a function, pushing `EXPR` as its result (0 when omitted) |

A `write out` statement can list any number of items joined by `and` (each
still introduced by `the`); they print adjacent on one line with a single
trailing newline, e.g. `write out the variable x and the string "!"` prints
`1!`. The items may be variables, string literals, and char literals mixed
freely.

`EXPR` is a number literal (integer or floating-point), the constant `pi`, a
string literal `"..."`, a character literal `'x'`, a variable name, an array
element (`the variable NAME at the index EXPR`), or a function call:
`call the function called NAME with the value of EXPR and ...`. String
literals support the escapes `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\0`, `\e`
(escape, byte 0x1b) and `\xHH` (exactly two hex digits, e.g. `\x1b` for
escape); character literals support the same except `\xHH`.
`CONDITION` is `the variable NAME is less than EXPR`, `... less than or equal to
EXPR`, `... greater than EXPR`, `... greater than or equal to EXPR`, `... equal
to EXPR`, or `... not equal to EXPR`. Conditions can be combined with `and` and
`or` (and binds tighter than or): e.g. `the variable x is greater than 3 and
the variable y is less than 10`. Strings compare lexicographically; numbers
(and characters, by code point) compare numerically.

Arithmetic follows the same English shape as `add` — the second named variable
is always the destination:

* `add the variable SRC to the variable DST` → `DST += SRC`
* `subtract the variable SRC from the variable DST` → `DST -= SRC`
* `multiply the variable SRC by the variable DST` → `DST *= SRC`
* `divide the variable SRC by the variable DST` → `DST /= SRC`
* `modulo the variable SRC by the variable DST` → `DST %= SRC`

`add` is the only operator that also concatenates two string variables.
Division truncates for integers and produces a `double` when either operand is
a double; dividing (or taking modulo) by zero is a runtime error.

## Math

The constant `pi` is a keyword usable anywhere a numeric expression is, but it
is a double, so it must be stored into a `double` variable to be written out
(`write out` only prints variables, not expressions):

```text
create a variable called p as a double with a value of pi
```

`sin`, `cos` and `sqrt` are built-in math functions, called with the same
syntax as user functions and taking radians. They accept any numeric argument
and always yield a double, so store the result in a double variable:

```text
create a variable called s as a double with a value of call the function called sin with the value of pi
create a variable called r as a double with a value of call the function called sqrt with the value of 2
```

A user-defined function with the same name shadows the built-in. Taking the
square root of a negative number is a runtime error.

`read the input into the variable called NAME.` blocks until a line arrives on
stdin and converts it to the variable's type: a string variable takes the whole
line, an integer or double variable parses the whole line (a malformed number
is a runtime error), and a char variable takes the first character. Reading
past end-of-input is a runtime error.

`wait for EXPR seconds.` and `wait for EXPR milliseconds.` pause the VM for the
given amount, where `EXPR` is a number literal or a numeric variable (integer
or double), so the delay can be fixed at compile time or decided at runtime.
Negative delays are a runtime error; fractional seconds are honored.

Conversions happen when storing a value into a variable:

* `integer` accepts integers, characters (their code point) and doubles
  (truncated toward zero: `3.7` becomes `3`, `-2.5` becomes `-2`).
* `double` accepts any number (integers are promoted).
* `char` accepts a number, a character, or a single-character string.
* `string` accepts anything: numbers become their decimal text, a character
  becomes a one-character string, and strings are stored as-is.

Note that strings and numbers are not interchangeable: assigning a string
variable to an integer variable, or using a string as a number, is a compile
error. Assigning a *number literal* to a string variable is allowed and stores
its decimal text.

## Arrays

An array is declared with a fixed size, computed at runtime from `EXPR`:

```text
create a variable called scores as an array of integer with a size of 5
create a variable called names as an array of string with a size of 3
```

Each element starts at its type's default value (`0`, `0.0`, `'\0'` or `""`).
The element type can be any of the four variable types; the size may be a
literal or any integer-valued expression. A negative size is a runtime error.

Elements are read and written with `at the index EXPR`, which works anywhere
a variable name does — expressions, assignments, `write out`, conditions,
arithmetic, `read the input`, and function arguments:

```text
assign the variable called scores at the index 0 the value of 10
write out the variable scores at the index 0
add the variable scores at the index 0 to the variable scores at the index 1
when the variable scores at the index 1 is greater than 10 write out the string "big"
```

The index must be an integer-valued expression (`0`, a char literal, an
integer or double variable, or a function call; doubles are truncated).
It is bounds-checked at runtime, and the store applies the same conversion
rules as scalars. Using an array variable without an index (or indexing a
scalar) is a compile error. Arrays may be declared inside functions, where
each call gets its own copy.

## Functions

A function is declared with `create a function called NAME`, an optional list
of `with a parameter called P as a TYPE` clauses joined by `and`, a `:`, a
statement body, and `stop the function.`:

```text
create a function called scale with a parameter called amount as an integer and a parameter called factor as an integer:
    create a variable called result as an integer with a value of amount
    multiply the variable factor by the variable result
    return the value of result
stop the function.
```

Each call runs the body in a fresh frame: parameters and any variables
declared inside the body are local to that call, so the same name may be used
in different functions (or recursively) without interference. A function can
also read and write outer-scope variables — anything declared at the top
level of the program, including arrays and their elements — by name; a local
declared with the same name shadows the outer one for that call. A function is
called with `call the function called NAME with the value of EXPR and ...`
— as a statement it discards the result, and as an expression it yields it:

```text
create a variable called doubled as an integer with a value of call the function called scale with the value of 21 and the value of 2
assign the variable called answer the value of call the function called scale with the value of doubled and the value of 3
```

`return the value of EXPR.` leaves the function, pushing `EXPR` as its
result; `return.` returns 0, as does running off the end of the body. The
result type is not declared, so using it where a specific type is expected is
checked when the call executes. Arguments are converted to each parameter's
declared type (a number or char to a string parameter becomes its text, a
string to a char parameter must be one character, and so on); arity and
obvious type mismatches are compile errors, as is `return` outside a
function. Functions may be called before they are defined, and functions can
call other functions or themselves.

## Example

```text
create a variable called x as an integer.
assign the variable called x the value of 1.
create a variable called y as an integer with a value of 0.

repeat while the variable y is less than 10:
    add the variable x to the variable y.
    write out the variable y
    when the variable y is equal to 5 write out the string "hello"
stop repeating.
```

```sh
$ ./aurascript examples/basic.ausc
1
2
3
4
5
hello
6
7
8
9
10
```

The `when` statement can also use a block body that mirrors the loop's
`repeat while ... stop repeating` pattern, closed by `stop when`:

```text
when the variable x is less than 10:
    write out the variable x
    add the variable y to the variable x
otherwise:
    write out the string "x is large"
stop when.
```

## How it works

1. **Lexer** turns the source into a stream of tokens. English keywords map to
   single tokens; anything else becomes an identifier.
2. **Parser** builds a statement/expression AST (`src/ast.h`).
3. **Compiler** lowers the AST to a flat bytecode stream. Variables are
   resolved to slot indices, string literals are collected into a constant
   pool, and loops/conditionals become relative jumps that are patched once
   the target addresses are known. Type errors are reported here. Function
   bodies are compiled after the main program (which ends in `halt`), one
   contiguous block per function, each with its own table of parameters
   followed by locals; a name that is not the function's own param or local
   resolves to the top-level table, so functions can touch outer-scope
   variables and arrays. `call`/`ret` manage the frames at run time.
4. **VM** is a stack machine that executes the bytecode, with a call stack of
   frames — one per active function call. Stack values are tagged as integer,
   double, char or string; `store_str` converts a numeric value to its decimal
   text and a char to a one-character string. Array variables hold a block of
   element values; `load_index`/`store_index` access them through a runtime
   bounds check. Comparisons dispatch on the value types at runtime (strings
   use `strcmp`). Output goes to stdout.

### Bytecode

Instructions are a little-endian byte stream with fixed-width operands
(`u32` index, `i64` immediate, or `i32` relative jump offset). A `u32 var`
operand is a variable reference: its top 2 bits are the frame depth (0 = the
current frame, 1 = the top-level global frame) and the low 30 bits are the
slot index. Functions fall back to the global frame for names that are not
their own params or locals.

| Opcode | Operands | Effect |
| --- | --- | --- |
| `halt` | — | stop execution |
| `push_num` | `i64` | push an integer literal |
| `push_double` | `f64` | push a floating-point literal |
| `push_str` | `u32` | push a string literal |
| `push_char` | `i64` | push a character literal |
| `load_var` | `u32` | push a copy of a variable's value |
| `store_int` | `u32` | pop a number, store as an integer variable |
| `store_double` | `u32` | pop a number, store as a double variable |
| `store_char` | `u32` | pop a number/single-char, store as a char variable |
| `store_str` | `u32` | pop any value, store as a string variable |
| `add` | `u32` `u32` | `dst += src` (integer variables) |
| `add_double` | `u32` `u32` | `dst += src` (double variables) |
| `concat` | `u32` `u32` | `dst = dst + src` (string concatenation) |
| `sub` / `sub_double` | `u32` `u32` | `dst -= src` (integer / double variables) |
| `mul` / `mul_double` | `u32` `u32` | `dst *= src` (integer / double variables) |
| `div` / `div_double` | `u32` `u32` | `dst /= src` (integer / double variables) |
| `mod` / `mod_double` | `u32` `u32` | `dst %= src` (integer / double variables) |
| `print_var` | `u32` | print a variable (no newline) |
| `print_str` | `u32` | print a string literal (no newline) |
| `print_char` | `i64` | print a character literal (no newline) |
| `print_nl` | — | print a newline (ends a `write out` statement) |
| `read_line` | `u32` | read a line of stdin into a variable |
| `sleep` | `u32` | pop a number; wait that long (0 = seconds, 1 = milliseconds) |
| `cmp_lt` / `cmp_gt` / `cmp_eq` / `cmp_le` / `cmp_ge` / `cmp_ne` | — | pop right, pop left, push `left <op> right` |
| `and` / `or` | — | pop right, pop left, push `left && right` / `left \|\| right` |
| `jmp` | `i32` | unconditional relative jump |
| `jmp_if_false` | `i32` | pop an int; jump when false |
| `call` | `u32` `u32` | call function (func index, arg count); args on the stack, pushes one result |
| `ret` | — | pop a value; return to the caller, leaving the value on the stack |
| `pop` | — | pop and discard a value |
| `make_array` | `u32` `u32` | pop a size; allocate that many element slots for the variable (var index, element type) |
| `load_index` | `u32` | pop an index; push a copy of the array element at that index |
| `store_index` | `u32` `u32` | pop an index, pop a value; store the value as that element (var index, element type) |
| `read_index` | `u32` | pop an index; read a line of stdin into that element |
| `print_stk` | — | pop a value; print it (no newline) |
| `add_stk` / `sub_stk` / `mul_stk` / `div_stk` / `mod_stk` | — | pop right, pop left, push `left <op> right` (numeric, or concatenation for `add_stk` on two strings) |
| `sin` / `cos` / `sqrt` | — | pop a number, push the trig/square-root result (radians) |

Use `aurascript --bytecode script.ausc` to see the compiled instructions:

```text
; variables
;   [0] x : integer
;   [1] y : integer
; strings
;   [0] "hello"
; code
0000: push_num    1
0009: store_int   [0] x
0014: push_num    0
0023: store_int   [1] y
0028: load_var    [1] y
0033: push_num    10
0042: cmp_lt
0043: jmp_if_false-> 94
0048: add         [1] y += [0] x
0057: print_var   [1] y
0062: print_nl
0063: load_var    [1] y
0068: push_num    5
0077: cmp_eq
0078: jmp_if_false-> 89
0083: print_str   [0] "hello"
0088: print_nl
0089: jmp         -> 28
0094: halt
```

## Project layout

| File | Role |
| --- | --- |
| `src/main.c` | CLI entry point, wires the pipeline together |
| `src/token.c/h` | token types and names |
| `src/lexer.c/h` | source text → tokens |
| `src/parser.c/h` | tokens → AST |
| `src/ast.c/h` | AST structures, constructors, pretty-printer |
| `src/bytecode.c/h` | AST → bytecode compiler + disassembler |
| `src/vm.c/h` | the bytecode VM |

This is a toy compiler, not a language server: on a syntax, compile, or VM
error it prints a diagnostic to stderr and exits with status 1.