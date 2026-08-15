# Proton Language Guide — Training, Learning, Reinforcement

This document is written to sit alongside `README.md` (which focuses on
architecture + changelog), **for someone who wants to learn the language
from scratch**. In order: setup, language fundamentals, the type
system, control flow, functions, structs/enums, arrays and maps, error
handling, the memory model (LAM), native built-ins, and a complete API
reference for all `stdlib/` modules. Every section has working,
copy-paste examples; all of them have been tested against the actual
files in this repo.

---

## Table of Contents

1. [Setup and first run](#1-setup-and-first-run)
2. [Language fundamentals](#2-language-fundamentals)
3. [The type system](#3-the-type-system)
4. [Control flow](#4-control-flow)
5. [Functions](#5-functions)
6. [Struct and enum](#6-struct-and-enum)
7. [Arrays (Lists) and Maps](#7-arrays-lists-and-maps)
8. [Error values and the `?` operator](#8-error-values-and-the--operator)
9. [The memory model (LAM) — when does it matter?](#9-the-memory-model-lam--when-does-it-matter)
10. [Native built-ins (built into the language core)](#10-native-built-ins-built-into-the-language-core)
11. [The module system (`use`)](#11-the-module-system-use)
12. [`stdlib/string` — full API reference](#12-stdlibstring--full-api-reference)
13. [`stdlib/collections` — full API reference](#13-stdlibcollections--full-api-reference)
14. [`stdlib/os` — full API reference](#14-stdlibos--full-api-reference)
15. [`stdlib/math` — full API reference](#15-stdlibmath--full-api-reference)
16. [`stdlib/random` — full API reference](#16-stdlibrandom--full-api-reference)
17. [`stdlib/ml` — full API reference](#17-stdlibml--full-api-reference)
18. [`net::` — networking library](#18-net--networking-library)
19. [Common mistakes and pitfalls](#19-common-mistakes-and-pitfalls)
20. [Exercises](#20-exercises)
21. [Quick command/opcode reference card](#21-quick-commandopcode-reference-card)

---

## 1. Setup and first run

```sh
make                      # builds with gcc, produces the proton6/proton executable
./proton examples/hello.prt
```

`examples/hello.prt`:

```proton
fn main() {
    io::out("Hello, World!");
}
```

Every Proton program starts from a `main()` function (like C/Rust/Go).
The file extension is `.prt`. `io::out(...)` takes a variable number of
arguments, prints them all in sequence, and adds a newline at the end —
think of it like `println!`/`console.log`.

To run your own script:

```sh
./proton path/to/script.prt [extra arguments...]
```

Extra arguments are accessed from within the script via `sys::args()` or
`os::args()` (see [Section 10](#10-native-built-ins-built-into-the-language-core)
and [Section 14](#14-stdlibos--full-api-reference)).

---

## 2. Language fundamentals

### Comments

```proton
// single-line comment
```

### Variables: `var` / `const`

```proton
var age: int = 25;        // reassignable
const PI_ISH: float = 3.14; // not reassignable

age = 26;                 // OK
// PI_ISH = 3.15;         // compile error
```

**Important difference — global vs local:**

- **Global** `var`/`const` (defined outside a function) can only be
  initialized with a **literal**: `const MAX: int = 100;` works but
  `const MAX: int = compute();` doesn't. The compiler writes global
  initial values directly into the VM's global table at compile time;
  it doesn't run a "script body".
- **Local** `var`/`const` (inside a function body) can be initialized
  with any expression: like `var total: int = sumTo(10) + 5;`.

### Is a type annotation always required?

Yes — a type annotation is mandatory in `var`/`const` declarations and
function parameters (there's no inferred syntax like `var x = 5;`).

### Strings

```proton
var s: string = "hello";
var multi: string = """
multi-line
string
""";
var withEscape: string = "line1\nline2\t\"quoted\"\\";
```

Concatenation (`+`) works:

```proton
var full: string = "hello" + " " + "world";
```

Character indexing also works (`s[i]`, returns a new single-character
string — see [Section 12](#12-stdlibstring--full-api-reference)):

```proton
io::out("hello"[1]);  // "e"
```

### `io::` — input/output

```proton
io::out("value:", 42, " and ", true);  // prints in sequence + \n
var line = io::in();                    // reads a line from stdin;
                                         // returns a number if convertible,
                                         // otherwise a string
```

`io` is **not** a real `use`-loaded module — it's a pseudo-namespace
built into the compiler, no need to write `use io;`.

### `assert` and `panic`

```proton
assert(1 + 1 == 2);          // stops with a runtime error if the condition is false
panic("unexpected state");   // unconditional, immediate program termination
```

---

## 3. The type system

Proton has a "logical" type system: **at runtime every number is stored
as a `double`** (int64/uint64 are the exception — stored with full
64-bit precision, see below), but the declared type for every
`var`/`const`/parameter **is checked**: if you assign an out-of-range
value, you get a meaningful error.

### Full type list

| Type | Description | Legacy alias |
|---|---|---|
| `bool` | `true`/`false` | |
| `char` | single character / code point | |
| `string` | text | |
| `byte` | 0-255 | |
| `int8` | -128..127 | |
| `int16` | | `short` |
| `int32` | | `int` |
| `int64` | full 64-bit precision | `long` |
| `uint` | unsigned | |
| `uint8` | 0-255 | |
| `uint16` | | |
| `uint32` | | |
| `uint64` | full 64-bit precision | |
| `float32` | | `float` |
| `float64` | | `double` |
| `decimal` | | |
| `fn` | first-class function value (not checked) | |
| `T[]` | array/list (see Section 7) | |

```proton
var a: int8 = 120;
a = 200; // PANIC: int8 range is -128..127, 200 is out of range
```

### `int64`/`uint64` — NumKind

Since normal numbers are stored as `double`, they lose precision beyond
±2^53-1. Variables declared with the `int64`/`uint64` types get past
this limitation in terms of **storage** (represented with full 64-bit
precision, real `INT64_MAX`/`UINT64_MAX` can be compared and printed
correctly):

```proton
const bigI: int64 = 9223372036854775807; // INT64_MAX -- stored with full precision
var a: int64 = 9223372036854775807;
var b: int64 = 9223372036854775806;
io::out(a != b); // true -- if this were a double, the two might be indistinguishable
```

The restriction is only **post-arithmetic**: the result of `+ - * /` is
still reduced to double, meaning directly assigning an arithmetic
operation's result back into an `int64` variable can lead to precision
loss/type mismatch if the result exceeds 2^53-1. Simple `var`/`const`
assignment and comparison operations are safe.

### Generics

```proton
fn max<T>(a: T, b: T): T {
    if (a > b) { return a; }
    return b;
}

fn main() {
    io::out(max<int>(3, 5));          // 5
    io::out(max<float64>(1.5, 2.5));  // 2.5
}
```

Generic structs also exist:

```proton
struct Box<T> {
    value: T;
}

fn main() {
    var b: Box = Box<int>{ value = 42; };
    io::out(b.value); // 42
}
```

**Important:** Generic type arguments are **not inferred**, they must
be written explicitly at every call site: `max<int>(3, 5)`, not
`max(3, 5)`.

---

## 4. Control flow

### `if` / `else`

```proton
if (age >= 18) {
    io::out("Adult");
} else if (age >= 13) {
    io::out("Teenager");
} else {
    io::out("Child");
}
```

### `while`

```proton
var n: int = 5;
while (n > 0) {
    io::out("countdown: ", n);
    n--;
}
```

### `for` (classic three-part)

```proton
for (var i: int = 0; i < 10; i++) {
    if (i == 4) { continue; }
    if (i > 8) { break; }
    io::out(i);
}
```

### `for ... in` (iterating over an array)

```proton
var xs: int[] = [10, 20, 30];
for (x in xs) {
    io::out(x);
}
```

### `switch` / `case` / `default`

**Warning: there's C-style fall-through.** If a `case` has no `break`
at its end, execution **keeps falling through** to the next case
(unlike Swift/Rust, it doesn't stop automatically):

```proton
switch (day) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
        io::out("Weekday");
        break;
    case 6:
    case 7:
        io::out("Weekend");
        break;
    default:
        io::out("Invalid day");
}
```

### `defer`

Runs right before every `return` in a function body. Multiple `defer`s
process in **LIFO** order (the most recently defined runs first):

```proton
fn demo(): void {
    defer { io::out("1st defer (written last, runs first)"); }
    defer { io::out("2nd defer"); }
    io::out("body running");
    return;
}
// output order: "body running", "2nd defer", "1st defer"
```

---

## 5. Functions

```proton
fn add(a: int, b: int): int {
    return a + b;
}
```

- A return type is required (including `void`, even if the body doesn't
  use `return;`/`return;`).
- Recursion works freely (functions are called by name, looked up in
  the global table — forward references aren't a problem).

```proton
fn factorial(n: int): int {
    if (n <= 1) { return 1; }
    return n * factorial(n - 1);
}
```

### First-class function values

Functions can be assigned to a variable, passed as a parameter,
reassigned. The `fn` type name is used to say "this holds a function
value" (not checked, just expresses intent):

```proton
fn square(x: int): int { return x * x; }
fn cube(x: int): int { return x * x * x; }

fn apply(f: fn, x: int): int {
    return f(x);
}

fn main() {
    io::out(apply(square, 5));   // 25
    var g: fn = square;
    io::out(g(7));               // 49
    g = cube;
    io::out(g(4));               // 64
}
```

**Deliberately missing: closure/upvalue capture.** A function value
only points to code + a name — it doesn't capture any local variable
from the scope it was defined in. Callbacks you pass to
`collections::map/filter/reduce/find` must therefore be **stateless**
(they can only use their own parameters).

---

## 6. Struct and enum

These can only be defined at the **top level** (not inside a function
body).

### `struct`

```proton
struct Point {
    x: int;
    y: int;
}

fn main() {
    var p: Point = Point{ x = 3; y = 4; };
    io::out(p.x, ",", p.y);
}
```

Generic struct:

```proton
struct Pair<K, V> {
    key: K;
    val: V;
}

fn main() {
    var p: Pair = Pair<string, int>{ key = "age"; val = 30; };
    io::out(p.key, "=", p.val);
}
```

### `enum`

```proton
enum Color { Red, Green, Blue }              // 0, 1, 2 automatically
enum StatusCode { OK = 200, NotFound = 404 } // manual values

fn main() {
    io::out(Color::Green);       // 1
    io::out(StatusCode::OK);     // 200
}
```

Member access is read-only (no assignment).

---

## 7. Arrays (Lists) and Maps

### Arrays

```proton
var nums: int[] = [1, 2, 3, 4];
io::out(nums[1]);    // 2
nums[1] = 99;         // index assignment also returns a value as an expression
io::out(len(nums));   // 4
```

**Arrays are born fixed-size with the `[...]` literal** — `OP_SET_INDEX`
can only write to an existing index, it can't grow the list.
`var b: T[] = a;` does **not copy** the list, it gives a second
reference to the same list (an alias):

```proton
var a: int[] = [1, 2, 3];
var b: int[] = a;
b[0] = 99;
io::out(a[0]); // 99 -- a changed too! (same ObjList)
```

There are two native built-ins for dynamic growth and true copying:

```proton
var xs: float64[] = [];
push(xs, 1.0);
push(xs, 2.0);
io::out(len(xs));  // 2

var a: float64[] = [1.0, 2.0];
var b: float64[] = listCopy(a); // independent copy
b[0] = 99.0;
io::out(a[0]);  // 1 (a unchanged)
```

### Maps

```proton
var m: map = {"a": 1, "b": 2};
io::out(m["a"]);   // 1
m["c"] = 3;         // adds a new key
io::out(len(m));    // 3
```

Only **string literal** keys are accepted (there's no
expression-keyed map like `{x: 1}`). `map` isn't a special keyword —
it's accepted as an unrecognized type name and passes through
unchecked.

### Is it safe to return an array/map from a function?

Yes. LAM (see Section 9) handles this automatically — if a function
`return`s a list it produced in its own region, it's automatically
copied into the caller's region as it escapes, no dangling pointer
risk:

```proton
fn makeList(): int[] {
    var xs: int[] = [1, 2, 3];
    return xs; // safe
}
```

---

## 8. Error values and the `?` operator

Proton has **no** exceptions/try-catch. Instead, there's a Rust/Go-style
"error value + short-circuit operator":

- Some native built-ins (`fs::read`, `sys::exec`, `net::get`, ...)
  return an **error value** (`VAL_ERROR`) instead of a normal value
  when they fail.
- **`expr?` (postfix `?`)**: runs `expr`, and if the result is an
  error, **it early-exits the current function** and propagates the
  error to the caller, as if it were a `return` value. If it's not an
  error, execution continues normally. It propagates in a chain — if
  it's not caught by `main`, the program stops with
  "Uncaught error: ...".

```proton
fn readConfig(): string {
    var content: string = fs::read("config.txt")?; // returns early here if there's an error
    return content;
}

fn main() {
    var cfg: string = readConfig()?; // propagates here too; if the file doesn't exist,
                                       // stops with "Uncaught error: ..."
    io::out(cfg);
}
```

Right now there's no way to programmatically inspect an error and
recover — `?` only provides the binary of "continue if no error" /
"throw upward if there's an error".

`os.prt`, `collections.prt`, `string.prt` in this repo's stdlib **don't
produce** error values (pure Proton, they stop with panic instead) —
where you'll actually encounter error values is the `fs::`,
`sys::exec`, and `net::` natives.

---

## 9. The memory model (LAM) — when does it matter?

Proton has no GC; instead it has **region-based deterministic memory
management** (LAM = Lifetime Allocation Model). Every function call
gets its own "region" (bump-pointer arena); the region is destroyed
when the function returns.

**You don't need to know this for everyday use** — the compiler/VM
automatically handles these two things:

1. It's safe for a function to `return` a value (like concatenation, an
   array literal) that it produced in its own region — it's
   automatically "promoted" into the caller's region.
2. Writing the currently running frame's own regional value into a
   list/map/global coming from an outer scope (`OP_SET_INDEX`,
   `OP_BUILD_MAP`, global assignment) is also automatically promoted
   to the right region.

**When does it matter?** When profiling performance/memory. Temporary
strings/lists that you produce (but **don't** return — consumed within
their own frame) by calling the same function millions of times get
cleaned up immediately along with their regions — memory stays
constant. But if you're **producing and returning a unique string on
every call**, this can still "leak" into permanent memory (a known
Phase 1 limitation — there's no escape analysis yet). You don't need to
think about this in everyday code; just keep it in mind if you're
returning millions of unique strings in tight loops.

---

## 10. Native built-ins (built into the language core)

These are **not** user functions defined with `fn` — they're
fixed-arity calls specially recognized by the compiler, going directly
to a bytecode opcode. They don't require `use`.

| Call | Description |
|---|---|
| `len(x)` | array/map/string length, O(1) |
| `push(list, value)` | appends to the end of the list, returns the list |
| `listCopy(list)` | returns an independent copy |
| `char::code(s)` | single-character string → code (int, 0-255) |
| `char::fromCode(n)` | code (0-255) → single-character string |
| `fs::read(path)` | reads a file, returns a string (error → `VAL_ERROR`) |
| `fs::write(path, content)` | writes to a file (overwriting), returns `nil` |
| `fs::exists(path)` | `bool`, O(1) `access()` check |
| `sys::exec(cmd)` | runs the command in a subshell, returns stdout |
| `sys::env(name)` | reads an environment variable, `nil` if not present |
| `sys::setenv(name, value)` | **[new]** sets this process's environment variable |
| `sys::args()` | extra CLI arguments, `string[]` |
| `sys::exit(code)` | **[new]** immediately terminates the process with `code`, doesn't return |
| `sys::pid()` | **[new]** this process's pid, `int64` |
| `sys::ppid()` | **[new]** the parent process's pid, `int64` |
| `time::now()` | Unix time, ms (`int64`) |
| `time::ticks()` | monotonic microsecond counter (`int64`) |
| `time::clock()` | process CPU time, seconds (`float64`) |
| `time::sleep(ms)` | sleeps for `ms` milliseconds |
| `time::format(ts, fmt)` | timestamp → formatted string |
| `time::parse(dateStr, fmt)` | string → timestamp (`int64` ms), error → `VAL_ERROR` |

> The four marked **[new]** were added in this repo this session
> (`OP_SYS_SETENV`, `OP_SYS_EXIT`, `OP_SYS_PID`, `OP_SYS_PPID`) —
> `stdlib/os.prt`'s `set_env/exit/pid/parent_pid` functions wrap these.

The `net::` family is separate and large, covered in
[Section 18](#18-net--networking-library).

---

## 11. The module system (`use`)

```proton
use math;              // loads stdlib/math.prt, accessed with the math:: prefix
use math as m;         // alias -- now only m:: is valid, not math::
```

- The compiler looks for the `stdlib/<name>.prt` file, compiles
  top-level definitions with a global name mangled with the `<name>.`
  prefix. Real dotted access works at the call site, like
  `math::sqrt(2.0)`.
- If the same module is `use`d more than once it's not reloaded
  (idempotent).
- A nonexistent module (`use mathh;`) gives a compile error.
- **`private fn`/`private var`/`private const`**: blocks
  `module.member` access from outside the module; access from within
  the module itself remains free.
- `io` is not a module, `use io;` isn't needed.

If you want to add your own module: create a `stdlib/<name>.prt` file,
define top-level `fn`/`var`/`const`, load it with `use <name>;`.

---

## 12. `stdlib/string` — full API reference

Loaded with `use string;`. All functions are called with the `string::`
prefix. Source: `stdlib/string.prt` (219 lines, pure Proton). Test:
`examples/test_string_stdlib.prt`.

Proton strings are **immutable**; every function returns a new string
(or list of strings), none mutate in place. Character access is built
on `s[i]` (native `OP_GET_INDEX`) and `char::code`/`char::fromCode`.

| Signature | Description | Example |
|---|---|---|
| `string::len(s: string): int` | length (a thin wrapper around native `len()`) | `string::len("hello")` → `5` |
| `string::upper(s: string): string` | converts to uppercase (a-z only) | `string::upper("Hi")` → `"HI"` |
| `string::lower(s: string): string` | converts to lowercase (A-Z only) | `string::lower("Hi")` → `"hi"` |
| `string::trim(s: string): string` | strips leading/trailing whitespace/tab/CR/LF | `string::trim("  hi  ")` → `"hi"` |
| `string::split(s: string, sep: string): string[]` | splits by separator; splits character by character if `sep=""` | `string::split("a,b,c", ",")` → `["a","b","c"]` |
| `string::join(list: string[], sep: string): string` | joins the list with a separator | `string::join(["a","b"], "-")` → `"a-b"` |
| `string::replace(s, old, replacement)` | replaces every occurrence of `old` | `string::replace("aXaXa","X","-")` → `"a-a-a"` |
| `string::contains(s, sub): bool` | whether it contains a substring | `string::contains("hello","ell")` → `true` |
| `string::starts_with(s, prefix): bool` | prefix check | `string::starts_with("hello","he")` → `true` |
| `string::ends_with(s, suffix): bool` | suffix check | `string::ends_with("hello","lo")` → `true` |
| `string::substring(s, start, end): string` | the `[start,end)` range, bounds are clamped | `string::substring("hello",1,3)` → `"el"` |
| `string::repeat(s, n): string` | repeats `s` `n` times | `string::repeat("ab",3)` → `"ababab"` |
| `string::reverse(s): string` | reverses it | `string::reverse("abc")` → `"cba"` |
| `string::char_at(s, index): string` | returns a single character (a wrapper around `s[index]`) | `string::char_at("hello",1)` → `"e"` |
| `string::indexOf(s, sub): int` | index of first occurrence, `-1` if not found | `string::indexOf("hello world","world")` → `6` |

**Note:** `replace`'s third parameter is named `replacement`, **not**
`new` — `new` is a reserved word in the language (reserved for future
`new`/`delete`).

Full example:

```proton
use string;

fn main() {
    var s: string = "  Hello, World!  ";
    io::out(string::trim(s));                          // "Hello, World!"
    io::out(string::upper(string::trim(s)));            // "HELLO, WORLD!"
    io::out(string::split("a,b,,c", ","));               // [a, b, , c]
    io::out(string::join(["x","y","z"], "/"));            // x/y/z
    io::out(string::contains("proton lang", "lang"));    // true
    io::out(string::replace("foo bar foo", "foo", "baz")); // baz bar baz
}
```

---

## 13. `stdlib/collections` — full API reference

Loaded with `use collections;`. Source: `stdlib/collections.prt` (148
lines, pure Proton). Test: `examples/test_collections_stdlib.prt`.

**Type note:** Proton generics require explicit instantiation and
array types are single-level (`T[]`, no `T[][]`). So that every
function can work with any element type, the collection
parameter/return is typed as `list[]` (an array whose element type
isn't checked); callbacks are ordinary first-class `fn` values (see
Section 5 — **must be stateless**, no closures).

| Signature | Description | Example |
|---|---|---|
| `collections::map(list: list[], f: fn): list[]` | applies `f` to every element, returns a new list | `collections::map(xs, double)` |
| `collections::filter(list: list[], f: fn): list[]` | keeps elements where `f(elem)` is true | `collections::filter(xs, isEven)` |
| `collections::reduce(list: list[], f: fn, initial: list[]): list[]` | left fold: `acc = f(acc, elem)` | see below |
| `collections::find(list: list[], f: fn): list[]` | returns the first matching element, `nil` if none | `collections::find(xs, isEven)` |
| `collections::contains(list: list[], value: list[]): bool` | linear search | `collections::contains(xs, 9)` |
| `collections::reverse(list: list[]): list[]` | a new reversed list | |
| `collections::sort(list: list[]): list[]` | ascending sort (insertion sort, on a copy) | |
| `collections::unique(list: list[]): list[]` | removes duplicates, preserves order | |
| `collections::flatten(list: list[]): list[]` | flattens one level | `collections::flatten([[1,2],[3]])` → `[1,2,3]` |
| `collections::range(start: int, end: int, step: int): int[]` | the `[start,end)` range, `step` can be negative | `collections::range(0,10,2)` → `[0,2,4,6,8]` |

Full example:

```proton
use collections;

fn isEven(x: int): bool { return x % 2 == 0; }
fn timesTwo(x: int): int { return x * 2; }

fn main() {
    var xs: int[] = [5, 3, 1, 4, 1, 5, 9, 2, 6];

    io::out(collections::map(xs, timesTwo));      // [10,6,2,8,2,10,18,4,12]
    io::out(collections::filter(xs, isEven));      // [4,2,6]
    io::out(collections::find(xs, isEven));        // 4
    io::out(collections::sort(xs));                 // [1,1,2,3,4,5,5,6,9]
    io::out(collections::unique(xs));               // [5,3,1,4,9,2,6]
    io::out(collections::range(0, 10, 2));          // [0,2,4,6,8]
}
```

**How does `sort` work?** Insertion sort, built on Proton's native
`<`/`>` operators — meaning it works on any comparable type (numbers,
strings), but it **doesn't accept** a custom comparator function (the
language doesn't support operator overloading/passing a comparator
yet).

---

## 14. `stdlib/os` — full API reference

Loaded with `use os;`. Source: `stdlib/os.prt` (119 lines, pure Proton +
native `sys::*` wrappers). Test: `examples/test_os_stdlib.prt`.

| Signature | Description | How it works |
|---|---|---|
| `os::platform(): string` | `"Linux"`, `"Darwin"`, ... | `sys::exec("uname -s")` |
| `os::arch(): string` | `"x86_64"`, `"arm64"`, ... | `sys::exec("uname -m")` |
| `os::cpu_count(): int` | logical CPU count | `sys::exec("nproc")` + manual decimal parse |
| `os::hostname(): string` | machine name | `sys::exec("hostname")` |
| `os::cwd(): string` | working directory | `sys::exec("pwd")` |
| `os::home(): string` | home directory | `sys::exec("echo $HOME")` |
| `os::env(name: string): string` | reads an environment variable | `sys::env(name)` (native) |
| `os::args(): string[]` | extra CLI arguments | `sys::args()` (native) |
| `os::set_env(name, value): void` | **[new]** sets this process's environment variable | `sys::setenv` (native, `setenv()`) |
| `os::exit(code: int): void` | **[new]** immediately terminates the process, doesn't return | `sys::exit` (native, `exit()`) |
| `os::pid(): int` | **[new]** this process's pid | `sys::pid` (native, `getpid()`) |
| `os::parent_pid(): int` | **[new]** the parent process's pid | `sys::ppid` (native, `getppid()`) |

**Why are some functions `sys::exec`, others direct natives?**
`sys::exec` always runs in a **separate child subshell** (`popen`) —
doing an `export FOO=bar` inside that subshell never affects the
calling Proton process, and reading a pid there would give the
subshell's own pid, not the process's real pid. That's why
`set_env`/`exit`/`pid`/`parent_pid` rely on four new native opcodes
added to the VM this session (`OP_SYS_SETENV`, `OP_SYS_EXIT`,
`OP_SYS_PID`, `OP_SYS_PPID` — see Section 10); the others (`platform`,
`arch`, ...) still run POSIX commands via `sys::exec` and parse the
output, because this information is already read-only/informational
and running in a subshell isn't a problem for it.

Full example:

```proton
use os;

fn main() {
    io::out("platform:", os::platform(), os::arch());
    io::out("cpu_count:", os::cpu_count());
    io::out("pid:", os::pid(), " parent_pid:", os::parent_pid());

    os::set_env("MY_VAR", "42");
    io::out("MY_VAR =", os::env("MY_VAR"));

    io::out("exiting now, with code 3");
    os::exit(3);
    io::out("this never runs");
}
```

---

## 15. `stdlib/math` — full API reference

Loaded with `use math;`. Source: `stdlib/math.prt` (136 lines, pure
Proton). Example: `examples/stdlib_demo.prt`, `examples/stdlib_edge_test.prt`.

| Signature | Description |
|---|---|
| `math::PI` / `math::E` | `const float64` constants |
| `math::abs(x: float64): float64` | absolute value |
| `math::absInt(x: int64): int64` | integer absolute value |
| `math::min(a, b): float64` / `math::max(a, b): float64` | |
| `math::clamp(x, lo, hi): float64` | clamps to the `[lo,hi]` range |
| `math::lerp(a, b, t): float64` | linear interpolation |
| `math::degToRad(deg): float64` / `math::radToDeg(rad): float64` | |
| `math::pow(base: float64, exp: int32): float64` | integer power (exponent can be negative) |
| `math::sqrt(x): float64` | Newton-Raphson; **panics** if `x<0` |
| `math::sin(x)` / `math::cos(x)` / `math::tan(x)` | Taylor series + angle reduction |
| `math::floor(x)` / `math::ceil(x)` / `math::round(x)` | |

```proton
use math;

fn main() {
    io::out("sqrt(2) =", math::sqrt(2.0));
    io::out("sin(PI/2) =", math::sin(math::PI / 2.0));
    io::out("pow(2,10) =", math::pow(2.0, 10));
}
```

**Warning:** calling `math::sqrt(-1.0)` stops the program with a
**panic** (a negative root is not a catchable error value, it's a
deliberately hard failure).

---

## 16. `stdlib/random` — full API reference

Loaded with `use random;`. Source: `stdlib/random.prt` (43 lines, pure
Proton). LCG-based (linear congruential generator) — even though
Proton has bitwise operators (see Section 21), this module was
historically written using only multiplication/addition/modulo;
**not cryptographically secure.**

| Signature | Description |
|---|---|
| `random::seed(value: uint32): void` | sets the generator's state |
| `random::next(): uint32` | a raw 32-bit value |
| `random::nextFloat(): float64` | in the `[0, 1)` range |
| `random::nextInt(min: int32, max: int32): int32` | in the `[min, max]` range (both ends inclusive) |
| `random::nextBool(p: float64): bool` | `true` with probability `p` |

```proton
use random;

fn main() {
    random::seed(42); // for deterministic/repeatable results
    for (var i: int32 = 0; i < 3; i++) {
        io::out(random::nextInt(1, 100));
    }
}
```

---

## 17. `stdlib/ml` — full API reference

Loaded with `use ml;` (internally does `use math;`, no extra `use`
needed). Source: `stdlib/ml.prt` (529 lines, pure Proton). Example:
`examples/ml_demo.prt`.

**Data representation:** Vector = `float64[]`. Matrix = `list[]` (a
list where each element is a `float64[]` row — there's no
`float64[][]` syntax).

### Vector statistics
`sum(xs)`, `mean(xs)`, `variance(xs)`/`sampleVariance(xs)`,
`stddev(xs)`/`sampleStddev(xs)`, `vecMin(xs)`, `vecMax(xs)`,
`correlation(xs, ys)` (Pearson).

### Vector algebra
`vecAdd(a,b)`, `vecSub(a,b)`, `vecScale(v,s)`, `dot(a,b)`, `norm(v)`.

### Matrix algebra
`zerosMatrix(rows,cols)`, `matRows(m)`, `matCols(m)`, `matAdd(a,b)`,
`matScale(m,s)`, `matTranspose(m)`, `matMul(a,b)`, `matVecMul(m,v)`.

### Activation functions
`sigmoid(x)`, `relu(x)`, `tanh(x)`, `sigmoidVec(xs)`, `reluVec(xs)`.

### Regression
`linRegFit(xs, ys)` → `LinRegModel{ slope; intercept; }` (closed form),
`linRegPredict(model, x)`, `linRegRSquared(model, xs, ys)`;
`gdFit(xs, ys, learningRate, epochs)` (batch gradient descent,
multivariate), `gdPredict`, `mse`.

### K-means
`nearestCentroid(point, centroids)`,
`kMeansFit(points, initialCentroids, iterations)`.

```proton
use ml;

fn main() {
    var xs: float64[] = [1.0, 2.0, 3.0, 4.0, 5.0];
    var ys: float64[] = [2.0, 4.0, 6.0, 8.0, 10.0];
    var model: LinRegModel = ml::linRegFit(xs, ys);
    io::out("slope=", model.slope, " intercept=", model.intercept);
    io::out("R^2=", ml::linRegRSquared(model, xs, ys));
}
```

---

## 18. `net::` — networking library

This is **not** a `stdlib/*.prt` module — it's a native `net::`
namespace built into the compiler (no need to write `use net;`).
Synchronous, plain-HTTP (no TLS).

| Call | Description |
|---|---|
| `net::get(url)` | GET, response body (`string`), error → `VAL_ERROR` |
| `net::post(url, body)` | POST, response body |
| `net::request(options: map)` | flexible request: `url`(required), `method`, `body`, `timeout`(ms, default 10000), `headers`(map). Returns: `{status, body, headers}` |
| `net::resolve(hostname)` | DNS lookup → first IP |
| `net::ping(host, timeoutMs)` | TCP connect to port 80, duration (ms) or `-1` |
| `net::urlEncode(str)` / `net::urlDecode(str)` | percent-encoding |
| `net::serve(port, handler)` | blocking HTTP/1.1 server; `handler` is a `fn` value |
| `net::connect(host, port, protocol)` | outbound TCP/UDP, returns a `handle` (int) |
| `net::send(handle, data)` | sends bytes |
| `net::recv(handle, maxBytes)` | reads (clamped between 1 and 1 MiB) |
| `net::close(handle)` | closes it, a no-op if already closed |

**Deliberately missing:** `bind`/`listen`/`accept` — a script can never
open a listening socket (the only exception is `net::serve`, but that
too is under the VM's own control).

```proton
fn appHandler(req: map) {
    return { "status": 200, "body": "{\"ok\": true}" };
}

fn main() {
    net::serve(8080, appHandler);
}
```

---

## 19. Common mistakes and pitfalls

1. **Expecting to initialize a global `var`/`const` with a function
   call.**
   ```proton
   const X: int = compute(); // ERROR -- global init can only be a literal
   ```
   Fix: use `var`/`const` inside `main()`, or initialize the global with
   a literal first and assign it later inside `main`.

2. **Expecting `var b = a;` to copy an array.**
   ```proton
   var b: int[] = a; // alias! not a copy
   ```
   Fix: use `listCopy(a)`.

3. **Naming `replace`'s third parameter `new`.** `new` is a reserved
   word, which is why `stdlib/string.prt` uses the name `replacement`
   instead. You also can't use type names like `new`, `double`, `char`,
   `string`, `int` as variable/parameter names in your own code.

4. **Forgetting `break` in `switch`.** Fall-through is C-like, it
   doesn't stop automatically.

5. **Expecting closures in `collections::` callbacks.** Callbacks are
   stateless first-class functions — they can't "capture" an outside
   variable. If you need to, pass the value as a parameter (like the
   `initial` argument of `reduce`).

6. **Forgetting that calls like `math::sqrt(-1.0)` panic instead of
   returning an error value.** The `?` operator is useless here —
   `math::sqrt` calls a real `panic()`, not `VAL_ERROR`.

7. **Forgetting to write the type argument in generic calls.**
   `max(3,5)` doesn't work, you need `max<int>(3,5)`.

8. **Trying to set an environment variable with `sys::exec`.**
   `sys::exec("export X=1")` doesn't work (stays in the subshell,
   doesn't reflect on the main process) — use `sys::setenv`/
   `os::set_env` instead.

---

## 20. Exercises

The following are designed to reinforce different layers of the
language; in increasing order of difficulty. Each one can be solved
using real functions from `stdlib/`.

1. **Warm-up:** Write a program using `io::out`, `var`, `if/else` that
   prints whether a number is odd or even.
2. **Loops:** Use `for` to compute the sum of numbers from 1 to 100
   divisible by 3 or 5.
3. **Functions + recursion:** Write a recursive `fn` that computes the
   nth Fibonacci term, verify a few known values with `assert`.
4. **`string::`:** Get a sentence from the user with `io::in()`, print
   the word count (`string::split` + `len`), and whether it's a
   palindrome (`string::reverse` + `==`).
5. **`collections::`:** Using a chain of `collections::filter` +
   `collections::map` on an `int[]`, do "first select the even numbers,
   then square them".
6. **`collections::reduce`:** Use `reduce` to compute the sum and (in
   a separate call) the maximum of a `float64[]` — without looking at
   `ml::sum`.
7. **Struct + generics:** Design your own `Stack<T>` struct (with a
   `T[]` field), write two functions like `push`/`pop` (using native
   `push` and array indexing).
8. **Error values:** Write to a file with `fs::write`, then read it
   back with `fs::read(...)?` and `io::out` it; deliberately break the
   file path and observe the error propagating all the way to `main`.
9. **`os::` + `sys::`:** Write a "system info" script that prints
   `os::platform()`, `os::cpu_count()`, `os::pid()`, then sets a
   variable with `os::set_env` and reads it back with `os::env`.
10. **`ml::`:** Run `ml::linRegFit` on a synthetic `xs`/`ys` dataset you
    generate yourself (with `random::nextFloat`), print `R²`.

---

## 21. Quick command/opcode reference card

This table summarizes every pseudo-namespace and fixed-arity built-in
**specially recognized** by the compiler (meaning, not a user `fn`).

| Namespace/call | Members |
|---|---|
| `io::` | `out(...)`, `in()` |
| `char::` | `code(s)`, `fromCode(n)` |
| `fs::` | `read(path)`, `write(path,content)`, `exists(path)` |
| `sys::` | `exec(cmd)`, `env(name)`, `args()`, `setenv(name,value)`, `exit(code)`, `pid()`, `ppid()` |
| `time::` | `now()`, `ticks()`, `clock()`, `sleep(ms)`, `format(ts,fmt)`, `parse(dateStr,fmt)` |
| `net::` | `get`, `post`, `request`, `resolve`, `ping`, `urlEncode`, `urlDecode`, `serve`, `connect`, `send`, `recv`, `close` |
| free (namespace-less) | `len(x)`, `push(list,v)`, `listCopy(list)`, `assert(expr)`, `panic(msg)` |

And the `use`-loaded, pure-Proton stdlib modules (`stdlib/*.prt`):

| Module | `use` | Notable members |
|---|---|---|
| `math` | `use math;` | `PI`, `E`, `sqrt`, `sin/cos/tan`, `pow`, `clamp`, `floor/ceil/round` |
| `random` | `use random;` | `seed`, `next`, `nextFloat`, `nextInt`, `nextBool` |
| `ml` | `use ml;` | vector/matrix statistics, regression, k-means |
| `string` | `use string;` | `upper/lower/trim/split/join/replace/contains/substring/...` |
| `collections` | `use collections;` | `map/filter/reduce/find/sort/unique/flatten/range` |
| `os` | `use os;` | `platform/arch/cpu_count/hostname/cwd/home/env/args/set_env/exit/pid/parent_pid` |

**Operator precedence order (low to high):**
`||` < `&&` < `|` < `^` < `&` < equality (`== !=`) < comparison
(`< <= > >=`) < shift (`<< >>`) < add/subtract (`+ -`) < multiply/
divide/mod (`* / %`) < unary (`! - ~`) < postfix (`?`, `++`, `--`, `[]`,
`()`, `.`).

---

*This document doesn't repeat `README.md`'s technical/architecture-
focused content — it references the "LAM", "NumKind", and changelog
sections there. The two should be read together: this document answers
"how do I write it", `README.md` answers "how does it work".*
