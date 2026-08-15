# Proton — C interpreter (v11, stack-based VM)

Single-pass Pratt parser + register-free stack-based bytecode VM.
Architecture close to the `clox` (Crafting Interpreters) style: source code
compiles directly to bytecode (no separate AST layer), then the VM runs
that bytecode.

**Proton 11 note:** This release adds three performance optimizations to
the VM's internal memory management without changing the language's
syntax or semantics (region pooling, lazy block allocation, loop-scope
region rewind). The last optimization is guarded by a lightweight static
escape analysis in the compiler — see the
**"✅ Proton 11: Loop-Scope Rewind — Fixed with Static Escape Analysis"**
section below for details.

## Build

```sh
make
./proton examples/hello.prt
./proton examples/factorial.prt
```

## Architecture

```
src/lexer.c     -> turns source text into tokens (comments, multi-line
                    strings, escape characters supported)
src/compiler.c  -> single-pass Pratt parser; emits bytecode directly
src/chunk.c     -> bytecode + constants pool
src/value.c     -> Value type (nil / bool / number / obj)
src/object.c    -> heap objects: ObjString (interned), ObjFunction
src/table.c     -> open-addressing hash table
src/vm.c        -> bytecode interpreter (call frames, stack)
src/main.c      -> CLI entry point
```

### Design decisions

- **Functions are called by name.** `OP_CALL` looks the name up in the
  global table at runtime. This means forward references (a function
  calling another function defined after it) and recursion just work,
  with no need for a separate "forward declaration" mechanism.
- **Global `var`/`const` can only take a literal initial value**
  (like `const MAX_USERS: int = 100;`). The reason: instead of running a
  "script" function that executes global bytecode, the compiler writes
  these values directly into the VM's global table at compile time. If
  needed later, a top-level script chunk could be added to lift this
  restriction.
- **Numbers (int/float) are stored as `double` at runtime.** Type
  annotations (`: int`, `: float`, etc.) are parsed and are now
  **enforced** too: every primitive type (`int8`..`uint64`, `float32/64`,
  `byte`, `char`, `bool`, `string`, ...) is validated at declaration/
  assignment/parameter points via `OP_CHECK_TYPE` (at compile time for
  global literal inits). Since the runtime representation is still a
  single `double`, this is a "logical" type system — actually getting the
  memory-size/performance difference (e.g. real 1-byte `uint8` storage)
  would require adding separate tags to `Value`.
- **`break`/`continue`** stay stack-balanced by having the compiler's
  loop-context stack automatically inject `OP_POP` based on how many
  local variables have been opened since entering the loop.

## LAM (Lifetime Allocation Model) — Phase 1

Region-based deterministic memory management instead of a GC. Phase 1
scope was deliberately kept narrow: **only strings produced at
runtime** (results of concatenation, number/bool → string conversion)
are regional; string literals (written in source code) are still
interned and remain permanent, as before.

### Architecture

- **Granularity: per-call-frame, not per-lexical-block.** Each
  `CallFrame` carries its own `Region` (`src/region.c`: a bump-pointer
  arena, block-chained). `OP_CALL` creates the region, `OP_RETURN`
  unconditionally destroys it when the frame is popped. This choice is
  deliberate: wanting a separate region per block (each `{ }` entry/exit)
  would require escape analysis to guarantee correct cleanup at every
  `return`/`break`/`continue` point — Phase 1 doesn't have that, so it
  uses the one lifetime boundary the VM already fully controls (the call
  frame).
- **Regions are completely disjoint from string interning.**
  `copyString`/`takeString` (literals) always live on the permanent heap,
  not tied to any region — this lets two different scopes safely share
  the same interned string. `regionTakeString` (new), on the other hand,
  never enters the intern table; the `ObjString.isRegional` flag marks
  this.

### Known limitation: string escape as a function return value

Phase 1 has no escape analysis. If a function `return`s a string it
produced in its own region (like `return a + b;`), the return value is
**permanently interned** before that region is destroyed in `OP_RETURN`
(`vm.c`, `OP_RETURN`) — otherwise the caller would get a dangling
pointer. This is correct but a conservative solution: when a function
that always returns a **new, unique** string every time is called many
times (e.g. a function in a loop like `build(i)` that produces a
different string each call), this promotion still grows permanent memory
— because the only way to know which caller will still hold onto that
string is escape analysis, and that's Phase 2.

Temporary strings a function consumes but does **not** return (e.g. the
intermediate result of `describe(n-1)`, if it stays within its own frame
and never escapes) don't have this problem — they get cleaned up along
with their regions. The problem only shows up in the "produce a
different string on every call and return it" pattern.

### Measurement

`examples/lam_stress_unique.prt` (200k calls, a unique string on every
call, none of them returned — all consumed within their own frame):

| | Peak RSS |
|---|---|
| Before Phase 1 | 72 MB (334 MB at 1M iterations) |
| After Phase 1 | 2.09 MB (still 2.09 MB at 1M iterations, constant) |

### Left for Phase 2

- Escape analysis: detecting whether a string escapes via `return`,
  by being assigned to a global, or by being placed into a struct field
  (once structs are added) — and leaving it in the region if it doesn't
  escape.
- Per-lexical-block granularity (destroying a block's own region as soon
  as a `{ }` block ends, before the function returns).
- Once arrays/structs are added, region/heap separation for those too.

### ✅ Proton 11: Loop-Scope Rewind — Fixed with Static Escape Analysis

As a performance/memory optimization, Proton 11 rewinds that frame's
region's bump pointer **back to the position at the start of that
iteration** on every `OP_LOOP` (when a `while`/`for`/`continue` round
completes and control returns to the top) (`regionRewind`, see
`src/region.c` and the `OP_LOOP` case in `src/vm.c`). This lets memory
from code that produces many short-lived strings/lists in tight loops be
reclaimed **on every iteration** instead of waiting until the function
returns — on a 3-million-iteration string-concat benchmark it brought
peak RSS down from 392 MB to 2.1 MB.

**This optimization is now guarded by a lightweight static escape
analysis in the compiler.** While compiling a loop body, the compiler
(`src/compiler.c`) tracks every assignment to a local variable that was
defined **before** the loop (`markLoopEscapeIfLocalPredatesLoop`,
`LoopCtx.escapes`). If such an assignment is detected — exactly the
"produce inside the loop → assign to an outer variable inside the loop"
pattern — every back-edge belonging to that loop (`OP_LOOP` — the body
itself, `continue`, and the `for` loop's increment expression) gets
flagged. When the VM sees this flag it **skips** `regionRewind` at that
back-edge; the region is only cleaned up on function return
(`OP_RETURN`), just like Phase 1's original (rewind-less) behavior. If
the flag isn't set (no value produced inside the loop is assigned
outside the loop), rewind keeps working unconditionally as before and
the full performance/memory gain is preserved.

This analysis is **conservative**: it only checks whether "the target
local's slot was defined before the start of the loop it's inside (or
that contains it)" — it doesn't track whether the value is actually read
after the loop. False positives (unnecessarily disabling rewind) are
possible but always on the safe side; there are no false negatives
(silently wrong results). `OP_SET_GLOBAL` and `OP_SET_INDEX`
(map/list element assignment) were already always promoted to permanent
storage through a separate mechanism (`promoteRegionalValue`), so the
escape analysis only focuses on local-to-local assignment.

Live verification: `examples/lam_rewind_unsound_demo.prt` now produces
the correct result:

```
saved (should still be item-2) = item-2
```

For loops without escapes (e.g. `examples/lam_stress_test.prt`, 200k
calls, no value escapes the loop) peak RSS is still ~2.1 MB — the
rewind optimization keeps running at full performance in this case, with
no regression.



## NumKind — int64/uint64 full precision

`Value`'s number field is now tagged as `{NumKind, union{f64,i64,u64}}`
rather than a plain `double`. `NUM_F64` is the default for untagged/
float numbers (identical to prior behavior); once a value passes through
`OP_CHECK_TYPE` with an `int64`/`uint64` type, it's marked as
`NUM_I64`/`NUM_U64` and stored with its true 64-bit representation. This
means `int64`/`uint64` variables can now be compared and printed
correctly even beyond double's exact-integer limit (±2^53-1). Arithmetic
(`+ - * /`, comparison operators) is still reduced to double via
`AS_NUMBER()` — meaning mixed/int64 arithmetic is still promoted to
double; true native 64-bit arithmetic is a future step.

## Currently working features

- **`use <name>;` is a real module/namespace system.** The compiler
  looks for the `stdlib/<name>.prt` file (relative to the working
  directory) and compiles the file's top-level definitions under a
  global name **mangled with the `<name>.` prefix** (`sqrt` →
  `math.sqrt`). Real dotted access also works at the call site:
  `math.sqrt(2.0)`, `math.PI` — it's not flattened into plain names. If
  the same module is `use`d more than once it isn't reloaded
  (idempotent). `io` is the exception: a file-less pseudo-namespace
  built into the compiler (`io.out`/`io.in` are specially recognized),
  no need to write `use io;`.
  - **A nonexistent module is now a compile error.** Something like
    `use mathh;` (a typo) now stops with a meaningful error
    ("Module 'mathh' not found...") when `stdlib/mathh.prt` can't be
    found — previously this was silently swallowed and only led to a
    vague error at the first call site (`mathh.foo()`).
  - **Visibility control: `private fn` / `private var` / `private const`.**
    Marking a module's top-level `fn`/`var`/`const` as `private` blocks
    access from outside as `module.member` (compile error); access from
    **within the module itself** (by bare name or qualified with its own
    prefix) remains free. `stdlib/math.prt`'s internal helper
    `_reduceAngle` is now genuinely `private` — see `stdlib/math.prt`.
    Not yet available for `struct`/`enum`, only for `fn`/`var`/`const`;
    `private` is only meaningful inside a module body, and errors if used
    in a top-level script.
  - **Import alias: `use math as m;`.** Can be accessed at the call site
    with a shortened name like `m.sqrt(2.0)`; the mangled global key
    still uses the module's real name (`math.sqrt`), meaning the same
    module loaded with different aliases at different `use` points still
    points to the same storage. Once an alias is given, the module's
    real name (`math`) is no longer recognized as a namespace — only the
    alias is valid (similar to Python's `import x as y` semantics).
- **There are three modules under `stdlib/` written in pure Proton**:
  `math.prt` (`PI`, `E`, `abs`, `absInt`, `min`, `max`, `clamp`, `lerp`,
  `pow`, `sqrt` [Newton-Raphson], `sin/cos/tan` [Taylor series + angle
  reduction, internal helper `_reduceAngle` is now `private`],
  `floor/ceil/round`, `degToRad/radToDeg`), `random.prt` (LCG-based
  `seed/next/nextFloat/nextInt/nextBool` — since Proton has no bitwise
  operators, it's an LCG based on multiplication/addition/modulo rather
  than xorshift), and `ml.prt` (statistics + linear algebra + simple ML
  — see the "`ml::` Library" section below). All calls are dotted:
  `math::sqrt(2.0)`, `random::nextInt(1, 100)`, `ml::mean(xs)`. Example
  usage: `examples/stdlib_demo.prt`, `examples/ml_demo.prt`.
- `var` / `const` (global: literal init only; local: any expression)
- **Primitive types and type checking**: `bool`, `char`, `string`, `byte`,
  `int8/16/32/64`, `uint`, `uint8/16/32/64`, `float32/64`, `decimal`, and
  legacy aliases (`short`→int16, `int`→int32, `long`→int64,
  `float`→float32, `double`→float64). Numbers are still stored as
  `double` at runtime, but each `var`/`const`/parameter's declared type
  is checked, either **at compile time** (global literal inits) or **at
  runtime** (`OP_CHECK_TYPE`, local init, every reassignment, `++`/`--`,
  and function parameters). If a value out of range is assigned (like
  `var a: int8 = 200;`), it stops with a meaningful error. `int64`/
  `uint64` now have full 64-bit precision for **storage** (see the
  "NumKind" section above); the restriction is only **post-arithmetic**:
  the result of `+ - * /` is still reduced to double, so directly
  assigning the result of an arithmetic operation back into an `int64`
  variable can produce a type error.
- `fn`, `return`, recursion
- `if` / `else`
- `while`, `for` (classic three-part), `break`, `continue`
- `switch` / `case` / `default` — **C-style fall-through**: if a case
  has no `break` at its end, execution keeps falling through to the next
  case/default (unlike Swift/Rust, it doesn't stop automatically).
- `struct Name { field: type; ... }` — can only be defined at the
  **top level**; defining a `struct`/`enum` inside a function body is
  still rejected. Represented at runtime via `ObjMap` (no separate
  instance type). Generic structs also work: `Box<T>{ value = 42; }`.
- `enum Name { A, B, C }` (auto-incrementing values) and
  `enum Name { A, B = 5 }` (manual/mixed values) — member access is
  `Name.Member`, read-only (no assignment). This too can only be defined
  at the top level.
- `defer { ... }` — only inside a function body; multiple `defer` blocks
  in a function run in **LIFO** order (the most recently defined runs
  first), each executed right before every `return`.
- **Generics**: `fn max<T>(...)` and `struct Box<T>` — a meaningful
  compile/runtime error if the number of type arguments is wrong or
  there's a type mismatch; nested generic calls are supported.
- **Error values + the `?` operator**: see the "Error values" section
  below.
- Arithmetic: `+ - * / %`, comparison: `== != < <= > >=`, logical:
  `&& || !` (with short-circuit)
- `++` / `--` (only as a standalone statement: `age--;`, `i++`)
- `io.out(...)` (variable number of arguments, prints them in sequence +
  a newline)
- `io.in()` (reads a line from stdin; returns a number if it can be
  converted to one, otherwise a string)
- `assert(expr);`, `panic(...);`
- String literals: normal `"..."`, escapes (`\n`, `\t`, `\"`, `\\`), and
  multi-line `""" ... """`
- `string + string` concatenation

## Arrays (Lists) and Maps

The features described in this section exist in the code but weren't
documented in a previous README revision.

- **The `T[]` array type and the `[...]` literal** now work:

  ```
  var nums: int[] = [1, 2, 3, 4];
  io.out(nums[1]);   // 2
  nums[1] = 99;      // index assignment also returns a value as an expression
  io.out(len(nums)); // 4
  ```

  The element type (`int[]` -> `int`) is stored as `ObjList->elemType`
  and is checked with `OP_CHECK_TYPE` during literal construction /
  `OP_SET_INDEX` (like other primitive types).
- **Map literal `{ "key": value, ... }`** — only string literal keys are
  allowed (there's no expression-keyed map like `{ x: 1 }`). The same
  `[]` indexing syntax also applies to maps:

  ```
  var m: map = {"a": 1, "b": 2};
  io.out(m["a"]);   // 1
  m["c"] = 3;       // adds a new key
  io.out(len(m));   // 3
  ```

  There is NO special `map` keyword for the map type itself; the `map`
  above is simply accepted as an "unrecognized type name" and passes
  through unchecked (just like struct types that don't exist yet). The
  map value itself still works fine, there's just no separate "this is a
  map" type check at compile time.
- **`len(x)`** returns the element count for both arrays and maps; O(1),
  no traversal.
- **Lifetime — returning arrays/lists from a function is now safe.**
  `ObjList` is still born **region-scoped** under LAM (see the section
  above), but an analogue of the `promoteEscapingValue` mechanism used
  for strings is now also applied to arrays (`regionCopyList` /
  `permanentCopyList`, `object.c`): before `OP_RETURN` destroys the
  region owning a returned list, it copies the list (and every element
  inside it — recursively, including nested lists and runtime-produced
  strings) into the parent (caller's) region. Just like with strings,
  this cascades: the value stays wherever region it gets moved to, all
  the way up to the outermost frame, and only falls into permanent/
  malloc'd storage if it escapes even that outermost frame (a rare
  case). So now code like:

  ```
  fn makeList(): int[] {
      var xs: int[] = [1, 2, 3];
      return xs;    // now safe: xs is copied into the caller's region as soon as it returns
  }
  ```

  both compiles and runs correctly and predictably — no dangling
  pointer risk. Maps never had this problem in the first place
  (`ObjMap` is permanent/malloc'd, see `newMap()`); now there's an
  equivalent guarantee for arrays/lists too, just via a different
  mechanism (region-chain promotion, instead of malloc). Note: this only
  covers escaping via `return` — writing a regional string/list a
  function produced in its own frame into a list from an outer scope
  (e.g. one passed in from outside) via `OP_SET_INDEX`, without
  returning it (as would also happen with struct fields), is still a
  separate, unhandled escape path — that falls under full escape
  analysis (Phase 2).

### `push(list, value)` and `listCopy(list)`

Array/list literals (`[...]`) are born fixed-size: `OP_SET_INDEX` can
only write to an **existing** index, it can't grow the list. Likewise,
`var b: T[] = a;` doesn't copy a list, it gives a second reference
pointing to the same `ObjList` (a `Value` carries a plain pointer) —
meaning `b[0] = 99;` also changes `a[0]`. These two limits made it
impossible to write dynamically growing data structures (accumulating
matrix rows, building a weight vector in gradient descent, etc.), so two
new native built-ins were added (in the same pattern as `len(x)`: calls
specially recognized by the compiler that go straight to an opcode, not
user `fn`s):

- **`push(list, value)`** — appends `value` to the end of `list` (uses
  the existing `appendList` C function — amortized O(1), grows 2x within
  the region into a new array when capacity is full), and returns **the
  same list** as the expression's value (consistent with
  `OP_SET_INDEX`'s "assignment is also an expression" rule). `value` is
  promoted into `list`'s own region if needed (see the "Lifetime"
  section above — the same hazard as in `OP_SET_INDEX`).
  ```
  var xs: float64[] = [];
  push(xs, 1.0);
  push(xs, 2.0);
  io::out(len(xs));   // 2
  ```
- **`listCopy(list)`** — returns an independent, separately mutable copy
  of `list` (in the caller frame's region, via `regionCopyList` — nested
  list/string elements are also promoted recursively). This fully closes
  the aliasing gap left by `var b: T[] = a;`.
  ```
  var a: float64[] = [1.0, 2.0];
  var b: float64[] = listCopy(a);
  b[0] = 99.0;
  io::out(a[0]);   // 1  (a is unchanged)
  io::out(b[0]);   // 99
  ```

Without these two, the fixed-size/aliasing constraints would have made
any stdlib module producing dynamically-sized vectors/matrices (like
`ml::`) impossible; almost every function in `stdlib/ml.prt` relies on
these two built-ins.

## Error values (`VAL_ERROR`) and the `?` operator

There's a simple "recoverable error" mechanism — not exceptions/
try-catch, a Rust/Go-style "error value + short-circuit operator":

- Native (host-provided) built-ins like `fs_read(path)`,
  `fs_write(path, content)`, `fs_exists(path)`, `sys_exec(cmd)` return
  an **error value** (`VAL_ERROR`) instead of a normal value when they
  fail.
- **`expr?` (postfix `?` operator, `OP_TRY`)**: compiles `expr` normally,
  then checks the result at runtime — if it's an error, **it early-exits
  the current function** (destroying its own region) and propagates the
  error to the caller's stack, as if it were a `return` value. If it's
  not an error, the value stays on the stack as-is and execution
  continues normally. It propagates in a chain: `?` carries the error
  all the way up to the outermost (`main`) call; if it's not caught
  there, the program terminates with "Uncaught error: ...".

  ```
  fn readConfig(): string {
      var content: string = fs_read("config.txt")?; // if there's an error, the function
                                                       // returns early here, carrying the error
      return content;
  }

  fn main() {
      var cfg: string = readConfig()?; // if the file doesn't exist, this propagates too and
                                         // stops with "Uncaught error: Could not read file"
      io.out(cfg);
  }
  ```

- Right now there's NO real `try { } catch (e) { }` block — the only way
  to "catch" an error would be a conditional type check
  (something like `if (IS_ERROR...)`) that doesn't exist yet at the
  language level in Proton. So there's no way yet to programmatically
  inspect an error and recover; `?` only provides the binary of either
  "continue if no error" or "throw upward if there's an error (ending up
  uncaught eventually)".
- **Interaction with type checking**: if the result of `expr?` is being
  assigned to a typed `var` (`string` in the example above), type
  checking runs normally on the non-error success path; but an error
  value never substitutes for a typed primitive — if you try to assign
  directly to a typed `var` without `?`, `OP_CHECK_TYPE` rejects it with
  "expected 'X', got error".

## Native (host) built-ins: `fs_*` / `sys_exec`

Besides `io.out` / `io.in`, there are a few more fixed-arity native
functions, going straight to an opcode, for filesystem and command
execution (these aren't user functions defined with `fn`, they're
built-ins specially recognized by the compiler, like `len(x)`):

- **`fs_read(path: string): string`** — reads the file, returns the
  content as a string; returns an error value if the file can't be
  opened (see the `?` section above). The content read belongs to the
  currently running frame's region (LAM string rules apply — see the
  LAM section above).
- **`fs_write(path: string, content: string): nil`** — writes to the
  file (overwriting an existing one), returns an error value on failure,
  `nil` on success.
- **`fs_exists(path: string): bool`** — O(1) existence check via
  `access()`.
- **`sys_exec(cmd: string): string`** — runs the command via `popen`,
  returns the captured stdout as a string; returns an error value if the
  command can't be started.

These are deliberately flat, namespace-less names — there's no dotted
access like `fs.read(...)`; these are natives directly built into the
compiler, like `len(x)`, not user modules. (`io.` is the same way —
also not a user module, a separate special case built into the
compiler.) Dotted access already works for real user modules (loaded
via `use math;`) — see the `use` section above.

## Not yet supported (roadmap)

These are recognized by the parser and rejected with a clear "not yet
supported" compile error (no crash, a meaningful error message with a
line number):

- Pointers (`int*`, `&`, `*ptr`), `new` / `delete`
- `sizeof`, `typeof`
- Character indexing on `string` values (`s[i]`) — rejected by the
  runtime, not the parser ("Cannot index a value of type string")
- Defining a `struct`/`enum` **inside** a function body (local) — both
  fully work at the top level, see below

`struct`, `enum`, `switch`/`case`/`default`, and `defer` now **fully
work at the top level/inside functions** — see the "Currently working
features" section above (a previous revision of this README still
listed these on the roadmap, which was outdated). Arrays (`int[]`,
`[1, 2, 3]` literal init) and maps (`{"k": v}`) also gained support —
see the "Arrays (Lists) and Maps" section above. Similarly, the
`fs_read/fs_write/fs_exists/sys_exec` native built-ins and error values
(`VAL_ERROR`) + the `?` operator now also work — see the relevant
sections above.

The remaining items (pointers, `sizeof`/`typeof`, string indexing)
touch the type system and the memory model, so they were deliberately
left for the next phase.

### Why there are no `string`/`time`/`path`/`process`/`thread`/`net` stdlib modules

`math` and `random` could be written in pure Proton because both only
need arithmetic + loops + functions — things the language already has.
`fs` is no longer on this list: `fs_read/fs_write/fs_exists` were added
directly as built-ins on the C side (see the "Native (host) built-ins"
section above) — there's still no real `stdlib/fs.prt` (pure Proton),
but the lack of a syscall is no longer a blocker. The rest are
**currently impossible**, not due to a missing library but due to a
missing core language feature:

- **`string`**: character indexing on `string` values with `s[i]`
  doesn't exist yet (the `[]` operator currently only works for
  `ObjList`/`ObjMap`), so almost nothing can be written — like `length`,
  `substring`, `toUpper` — beyond concatenation and `len(s)` (these two
  already work).
- **`time`/`process`/`net`**: running processes/commands via `sys_exec`
  is now possible (see above), but there's still no host/syscall
  binding for `time` (reading the clock) or `net` (sockets) — these
  need to be added as new built-ins on the C side, not in pure Proton.
- **`thread`**: the VM is single-threaded, the call frame stack is built
  on shared global state; real thread support would require touching
  the VM itself.
- **`path`**: theoretically reducible to string operations, meaning it
  too is waiting on `s[i]` string indexing support.

### Suggested next steps

Arrays (`ObjList`) and maps (`ObjMap`) are now implemented (see the
relevant sections above), so item 1 on the old list is complete. The
priority that came along with it, array/list escape analysis, is now
also implemented (see the "Lifetime" note above and the "Remaining"
section below) — `promoteEscapingValue` now also works for `ObjList`,
the blocker in front of collections like `Stack`/`Queue` is gone
(collections themselves still haven't been written, see "Remaining"):

1. ~~Structs~~ — **done.** `struct Name { field: type; }` at the top
   level and generic `struct Box<T>` work, runtime representation via
   `ObjMap` (no separate `ObjInstance` type). Remaining: `sizeof` is
   still unsupported, and local (inside-function) struct/enum
   definitions are still rejected.
2. ~~switch~~ — **done.** Compiles to bytecode as expected, as syntactic
   sugar, without needing a new opcode. Note: it has C-style
   fall-through semantics (`break` required), which remains a design
   decision.
3. **Pointers / `new` / `delete`** — still not done. Requires a real
   heap model and a GC or arena allocator decision. Since structs now
   exist (the heap object model via `ObjMap` is already in place), this
   is no longer the only thing blocking it — the actual decision is
   still pending.
4. **String indexing (`s[i]`)** — still not done; this is the only
   thing blocking the `string`/`path` stdlib modules; the `[]`
   infrastructure that already exists for arrays (`OP_GET_INDEX`) can
   be used as a model.
5. The bootstrap goal (writing the Proton VM in Proton itself) requires
   this C interpreter to first reach a full feature set.

### Remaining — status of work done this session

1. **Cross-module call fix within a module — verified.** Mangling with
   `currentModulePrefix()` had been added to the bare-call path in
   `identifierExpr` (`compiler.c`); both this and the bare global
   variable *read* side (the same function's `OP_GET_GLOBAL` branch —
   e.g. `degToRad` in `math.prt` using `PI`) were verified by rebuilding
   and re-running `examples/stdlib_demo.prt` and
   `examples/stdlib_edge_test.prt`; both give the expected output
   (including the `math.sqrt(-1.0)` panic at the end of the edge test,
   as the file itself expects).
2. **Generics tested end-to-end.** `examples/generics_fn_test.prt` and
   `examples/generics_struct_test.prt` were compiled and run to verify
   they produce the correct result. Ad-hoc edge-case tests were also
   done: a wrong number of type arguments (`max<int,int>(...)`) is
   rejected at compile time with a meaningful error; nested generic
   calls (`max<int>(max<int>(1,7), max<int>(4,2))`) produce the correct
   result; a type mismatch (`identity<int>("not a number")`) is stopped
   at runtime with a meaningful error; defining a local variable with
   the same name as a module (`math`) safely works without breaking
   `module.member` access (dot-access always resolves as a module, a
   bare name always as a local).
3. **ObjList escape/promotion fix completed.** `promoteEscapingValue`
   in `vm.c` now also works for `ObjList`, alongside `ObjString`:
   `regionCopyList` added to `object.c` (moving into a parent/caller's
   region, with the exact same cascading logic as `regionCopyString`
   for strings) and `permanentCopyList` (falling into permanent/malloc'd
   storage if it escapes even the outermost frame — the previously
   unreachable `OBJ_LIST` branch in `freeObjects()` now has a real
   function too). Promotion also recursively covers the list's elements
   (including nested lists and runtime-produced regional strings).
   Tested: single-level return-escape, resilience against heavy
   region-churn after return (data verified unbroken via `assert`),
   escape of lists containing regional strings, and true nested
   list-in-list escape (recursive promotion) — all produced correct
   results, no crashes. This removes the blocker in front of
   collections like `Stack`/`Queue`; collections themselves (outside
   this session's scope) still haven't been written.
4. **Test coverage is still not complete.** Beyond what's noted in the
   items above, there's no systematic test suite — tests were written
   and run by hand this session with ad-hoc `/tmp` files, not added to
   `examples/` as a permanent regression test.

5. **`OP_SET_INDEX`/`OP_BUILD_MAP`/`OP_SET_GLOBAL`/`OP_DEFINE_GLOBAL`
   escape path — fixed.** The gap noted in a previous session (writing
   the current frame's own not-yet-promoted regional value into a
   list/map coming from an outer scope via `OP_SET_INDEX`) has now been
   addressed: `ObjString` gained the same `region` field as `ObjList`
   (`object.h`/`object.c`), and the special-purpose `promoteListElement`
   function used in list-element promotion was turned into a
   general-purpose, public `promoteRegionalValue(Region* dest, Value v)`
   (see `object.h`). This function is now called at all four write
   points:
   - `OP_SET_INDEX` → map branch: `promoteRegionalValue(NULL, value)`
     (since `ObjMap` is always permanent/malloc'd, the destination is
     always permanent storage).
   - `OP_SET_INDEX` → list branch: `promoteRegionalValue(list->region, value)`
     (the destination is that list's own region — no copy happens if
     it's already in the right region).
   - `OP_BUILD_MAP` (map literal values) → `promoteRegionalValue(NULL, val)`.
   - `OP_SET_GLOBAL` / `OP_DEFINE_GLOBAL` → `promoteRegionalValue(NULL, value)`
     (`vm.globals` lives for the program's lifetime).

   This was verified under ASan/UBSan by running both the existing
   `examples/` suite and dedicated escape tests targeting these three
   write paths (list, map, global) (loops that produce a regional
   string inside a function and write it 500 times in a row into an
   outer list/map/global) — no use-after-free / heap-buffer-overflow
   detected, results consistent.

6. **README accuracy sweep — done this session.** The README was
   compared line by line against the code itself (relevant
   `compiler.c` sections via `grep`/`sed`) and verified at runtime with
   ad-hoc test files (`/tmp/*.prt`). Result: the document's
   "Not yet supported" section had gone stale — `struct`, `enum`,
   `switch`/`case`/`default`, and `defer` were **already fully working**
   in the code but were still listed on the roadmap. These were moved
   to "Currently working features"; only genuinely unsupported items
   remain on the roadmap: pointers/`new`/`delete`, `sizeof`/`typeof`,
   string indexing (`s[i]`), and local (inside-function) `struct`/`enum`
   definitions. Also, the int64/uint64 note under "working features" was
   outdated in a way that contradicted the NumKind section (it still
   said "limited to ±2^53-1") — fixed: storage has full 64-bit
   precision, only post-arithmetic falls back to double.

7. **Module system expanded — added this session.** Real dotted access
   (`math.sqrt(x)`) had already been added in an earlier session, but
   this README still said "not a real module system" (another
   inconsistency between code and docs) — this was fixed, and three
   real gaps were also closed:
   - A nonexistent module (`use mathh;`) is now rejected with a
     meaningful compile error (`readEntireFile` returning NULL is no
     longer silently swallowed); only `io` (file-less, built into the
     compiler as a pseudo-namespace) is exempt from this rule.
   - **`private fn`/`private var`/`private const`** (new
     `TOKEN_PRIVATE` keyword) — registers the mangled name in
     `privateMemberRegistry`; the `moduleName.member` access branch in
     `identifierExpr` rejects it if the calling module
     (`currentModulePrefix()`) isn't the same as the target module (not
     a self-access). `stdlib/math.prt`'s internal helper
     `_reduceAngle` is now genuinely `private` (verified that external
     access is tested and rejected).
   - **Import alias** (`use math as m;`, new `TOKEN_AS` keyword) —
     `moduleRegistry` was converted from a plain name list into a list
     of `{alias, prefix}` pairs (`ModuleBinding`), so `m.sqrt` at the
     call site resolves to the mangled key `math.sqrt`.
   - As a side effect, a real bug was found and fixed: when `use io;`
     was written, `io` would get registered as a regular module,
     shadowing `io.out`'s hardcoded dispatch and breaking with
     "Undefined function 'io.out'" — `loadStdlibModule` now never
     registers `io` in `moduleRegistry`.
   - The meaningless `use stdlib;` lines in `examples/hello.prt`,
     `examples/factorial.prt`, `examples/features.prt` were cleaned up
     (they wouldn't compile under the new "missing module → error"
     rule; `io.out` never needed a `use` anyway). The entire
     `examples/` suite was re-run to verify there was no regression.

## The `ml::` Library

A statistics + linear algebra + simple ML module written in pure Proton
(`stdlib/ml.prt`, loaded via `use ml;`). Depends on `math::sqrt` (the
module does `use math;` internally, the caller doesn't need to also
write `use math;`).

**Data representation:** Vector → `float64[]`. Matrix → `list[]` (a
list where each element is a `float64[]` row). Proton's type
annotations only support single-level `T[]`, there's no `float64[][]`
syntax — so matrices are declared as `list[]` (an array whose element
type isn't checked); individual rows (`m[r]`) are still plain
`float64[]`, indexable normally like `m[r][c]`.

- **Vector statistics:** `sum`, `mean`, `variance`/`sampleVariance`,
  `stddev`/`sampleStddev`, `vecMin`, `vecMax`, `correlation` (Pearson).
- **Vector algebra:** `vecAdd`, `vecSub`, `vecScale`, `dot`, `norm`.
- **Matrix algebra:** `zerosMatrix(rows, cols)`, `matRows`, `matCols`,
  `matAdd`, `matScale`, `matTranspose`, `matMul` (rows x inner times
  inner x cols), `matVecMul`.
- **Activation functions:** `sigmoid`, `relu`, `tanh` (since Proton has
  no native `exp()`, `tanh`/`sigmoid` rely on a private/`private`
  `_exp` helper written with a fixed-iteration Taylor series), and
  `sigmoidVec`/`reluVec` that operate on vectors.
- **Simple linear regression (single feature):** `linRegFit(xs, ys)`
  returns a `LinRegModel { slope; intercept; }` struct via closed-form
  least squares; `linRegPredict(model, x)`,
  `linRegRSquared(model, xs, ys)`.
- **Multivariate linear regression:** `gdFit(xs, ys, learningRate, epochs)`
  returns a weight vector (`float64[]`) via batch gradient descent
  (`xs` is a `list[]`, each row a sample's feature vector — a constant-1
  column should be added to `xs` if a bias term is wanted);
  `gdPredict`, `mse`.
- **K-means clustering:** `nearestCentroid(point, centroids)`,
  `kMeansFit(points, initialCentroids, iterations)` — runs Lloyd's
  algorithm for a fixed number of iterations (no dynamic convergence
  check, deterministic); if a cluster gets no points in a round, it
  keeps its old centroid instead of dividing by zero.

Example:
```
use ml;

fn main() {
    var xs: float64[] = [1.0, 2.0, 3.0, 4.0, 5.0];
    var ys: float64[] = [2.0, 4.0, 6.0, 8.0, 10.0];
    var model: LinRegModel = ml::linRegFit(xs, ys);
    io::out("slope=", model.slope, " intercept=", model.intercept);
    io::out("R^2=", ml::linRegRSquared(model, xs, ys));

    var a: list[] = [[1.0, 2.0], [3.0, 4.0]];
    var b: list[] = [[5.0, 6.0], [7.0, 8.0]];
    var c: list[] = ml::matMul(a, b);
}
```

Full example: `examples/ml_demo.prt`.

## The `net::` Library

A synchronous, plain-HTTP (no TLS) client library, now bundled with an
**outbound-only** raw TCP/UDP socket API (see below). Still
deliberately **missing**: `bind`/`listen`/`accept` or similar
*listening* socket primitives — these were deliberately kept out of
scope since they're the basic building blocks needed to build a port
scanner / arbitrary backdoor server. `net::serve` is the sole exception
to this rule: the VM itself opens and manages the listening socket, the
script only ever sees parsed HTTP request/response maps, never a socket
handle.

- `net::get(url)` -- sends a GET request, returns the response body
  (`string`), `VAL_ERROR` on failure.
- `net::post(url, body)` -- sends a POST request, returns the response
  body.
- `net::request(options)` -- a flexible request: `options` is a `map`
  accepting `"url"` (required), `"method"` (default `"GET"`), `"body"`,
  `"timeout"` (ms, default 10000, capped at 60000), and `"headers"`
  (a string->string sub-`map`) keys. Returns a `map`:
  `{ "status": 200, "body": "...", "headers": {...} }`.
- `net::resolve(hostname)` -- a DNS lookup, returns the first resolved
  IP address as a `string`. Doesn't open/connect a socket.
- `net::ping(host, timeoutMs)` -- measures reachability with a single
  TCP connect attempt to port 80; returns the elapsed time in ms
  (`float`) on success, `-1` otherwise. Even `ECONNREFUSED` counts as
  "reachable" (it means the host responded).
- `net::urlEncode(str)` / `net::urlDecode(str)` -- percent-encoding, no
  network access, pure string transformation.
- `net::serve(port, handler)` -- a built-in, single-threaded blocking
  HTTP/1.1 server. The VM manages the listening socket, the accept
  loop, and HTTP framing (request parsing, Content-Length, writing the
  response) itself; the script never sees a raw socket handle -- it
  just gets `handler` called for each request with a parsed `req` map
  (`method`, `path`, `body`, `headers`) and sends the returned map
  (`status`, `body`) as the response. `handler` is now an ordinary
  **expression**: a bare function name (`net::serve(8080, appHandler)`)
  or a variable/parameter holding a function value
  (`net::serve(8080, h)`) -- Proton now supports first-class function
  values (see below). Suitable for local dev/simple REST API scenarios;
  it's not a production-grade concurrent server.

### Outbound-only socket API: `net::connect` / `send` / `recv` / `close`

To write a general-purpose client/protocol layer (a custom DB driver,
an IoT protocol, a P2P client side, etc.), scripts can now open an
outbound TCP/UDP socket and do send/recv over a raw byte stream. The
value given to the script is a small integer **handle** (an index into
the VM's internal `netSockets` table), not a real file descriptor (fd).

- `net::connect(host, port, protocol)` -- `protocol` must be `"tcp"` or
  `"udp"`. Returns a `handle` (`number`) on success, `VAL_ERROR`
  otherwise. Connect/send/recv operations are subject to a 15-second
  timeout (so a frozen/unreachable host can't lock up the whole
  interpreter forever).
- `net::send(handle, data)` -- sends `data` (`string`), returns the
  number of bytes sent (`number`), `VAL_ERROR` on failure.
- `net::recv(handle, maxBytes)` -- reads at most `maxBytes` (clamped
  between 1 and 1 MiB) bytes, returns as a `string` (an empty string if
  the other side closed the connection cleanly), `VAL_ERROR` on
  failure.
- `net::close(handle)` -- closes the socket, returns `nil`. An invalid
  or already-closed handle is a silent no-op (doesn't throw an error).

**Deliberately missing:** `bind`, `listen`, `accept` -- scripts can
never open a listening socket or accept an incoming connection with
this API; they can only connect outward. The number of open sockets is
limited by a fixed, small table (`NET_SOCKETS_MAX = 64`).

Example (TCP client):
```
fn main() {
    var h: int = net::connect("example.com", 80, "tcp");
    net::send(h, "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    var resp: string = net::recv(h, 4096);
    io::out(resp);
    net::close(h);
}
```

Example server (handler can be held in a variable):
```
fn appHandler(req: map) {
    return {
        "status": 200,
        "body": "{\"message\": \"Proton 6 HTTP Server Running!\"}"
    };
}

fn main() {
    var h: fn = appHandler;
    net::serve(8080, h);
}
```

Example:
```
var opts: map = { "method": "GET", "url": "http://example.com/", "timeout": 5000 };
var res: map = net::request(opts);
io::out(res["status"], " ", len(res["body"]));
```

## First-class function values

Functions can now be carried around like ordinary `Value`s: assigned to
a variable, passed as a parameter to another function (e.g.
`net::serve`'s `handler` argument), reassigned. `fn` can be used as a
type name to indicate that a parameter or local "will hold a function
value" (unchecked, but expresses syntactic intent):

```
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
    io::out(g(4));                // 64
}
```

**Deliberately missing: closure / upvalue capture.** A function value
only points to code + a name; it doesn't capture/carry any local
variable from the scope it was defined in (stateless first-class
functions). Real closure support was deliberately left out of this
stage since it would create serious design tension with the current
region-based memory model (each call frame destroying its own region on
return).

## Bitwise operators

`&` (AND), `|` (OR), `^` (XOR), `~` (unary NOT), `<<` (left shift), `>>`
(right shift). Operands are truncated to a 64-bit signed integer
(`int64_t`) (same behavior as C's double->int conversion), the result
is returned as a number of `NUM_I64` kind. Precedence order is similar
to C's: `|` < `^` < `&` < equality/comparison < `<<`/`>>` < `+`/`-` <
`*`/`/`/`%`. A runtime error is thrown if the shift amount is outside
the 0-63 range.

```
var a: int = 12;
var b: int = 10;
io::out(a & b);   // 8
io::out(a | b);   // 14
io::out(a ^ b);   // 6
io::out(~a);      // -13
io::out(a << 2);  // 48
io::out(a >> 2);  // 3
```
