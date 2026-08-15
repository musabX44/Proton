# Proton

**A modern systems programming language focused on simplicity, performance, and explicit memory management.**

Proton is a statically typed systems programming language designed to provide low-level control without sacrificing developer ergonomics.

It aims to occupy a practical space between the simplicity of higher-level systems languages and the control traditionally associated with C.

> **Proton 11 — LAM-2**
> **20,000,000 string iterations**
> **~1.9 MB Peak RSS**

---

## Why Proton?

Systems programming often forces developers to choose between different trade-offs:

* **C** provides exceptional control and predictable execution, but leaves many safety responsibilities to the programmer.
* **Go** provides simplicity and productivity, but relies on a garbage-collected runtime.
* **Rust** provides strong compile-time guarantees, but introduces a more complex programming model.

Proton explores another point in this design space:

> **Simple syntax. Static typing. Explicit control. Predictable memory management.**

Proton is not intended to replace every existing systems language. It is an experiment in finding a practical balance between control, safety, simplicity, and performance.

---

## Features

* Static type system
* Native compilation
* Explicit memory management
* LAM-2 lifetime-aware allocation architecture
* No tracing garbage collector
* Low-level programming capabilities
* C-like systems programming workflow
* Modules and standard library
* Networking support
* Extensible compiler architecture
* VM support
* Cross-platform development roadmap

---

## Hello, Proton

```proton
fn main() {
    println("Hello, Proton!");
}
```

A simple Proton program should feel familiar to developers coming from C-like languages while providing a more structured programming model.

---

# LAM-2

**LAM — Lifecycle Allocation Manager**

LAM-2 is Proton's memory-management architecture for handling allocations according to object lifetime and execution context.

Instead of treating every temporary allocation as an independent long-lived heap object, LAM-2 can organize allocations around their expected lifetime.

Conceptually:

```text
Create objects
      │
      ▼
┌───────────────┐
│    LAM-2      │
│   allocation  │
│    region     │
└───────┬───────┘
        │
        ▼
Objects reach end of lifetime
        │
        ▼
Region reclaimed
```

The goal is to reduce unnecessary allocation overhead while keeping memory behavior predictable.

LAM-2 is an active area of development.

---

# Benchmark

One of the current Proton 11 LAM-2 stress tests performs:

```text
20,000,000 string iterations
```

Measured result:

```text
Peak RSS: ~1.9 MB
```

### Important

This result represents **Peak Resident Set Size (RSS)** for the benchmark process.

Benchmark results are hardware-, operating-system-, compiler-, and workload-dependent. They should not be interpreted as a universal performance claim.

The benchmark source and methodology should be kept publicly available so the result can be independently reproduced.

---

# Compiler Architecture

The Proton toolchain is organized around several compilation stages:

```text
Source Code
    │
    ▼
 Lexer
    │
    ▼
 Parser
    │
    ▼
 AST
    │
    ▼
 Type Checking
    │
    ▼
 Intermediate Representation
    │
    ▼
 Code Generation
    │
    ▼
 Executable
```

The architecture is designed to allow the compiler, VM, type system, and runtime components to evolve independently.

---

# Example

A small example:

```proton
fn add(a: int, b: int) -> int {
    return a + b;
}

fn main() {
    let result = add(20, 22);
    println(result);
}
```

More examples can be found in:

```text
examples/
```

---

# Project Structure

```text
proton/
├── compiler/
├── lexer/
├── parser/
├── ast/
├── typechecker/
├── ir/
├── runtime/
├── vm/
├── std/
├── examples/
├── benchmarks/
├── docs/
└── tests/
```

The exact structure may evolve as the compiler develops.

---

# Installation

## Linux

Clone the repository:

```bash
git clone https://github.com/YOUR_USERNAME/proton.git
cd proton
```

Build Proton:

```bash
./build.sh
```

Then verify the compiler:

```bash
./proton --version
```

> Installation commands may change between releases. See the documentation for the current supported build procedure.

---

# Documentation

Documentation is available in:

```text
docs/
```

Topics include:

* Language syntax
* Types
* Functions
* Modules
* Memory management
* LAM-2
* Compiler architecture
* Standard library
* VM
* Networking
* Examples

---

# Development

Proton is actively developed.

If you want to experiment with the compiler:

```bash
git clone https://github.com/YOUR_USERNAME/proton.git
cd proton
```

Build and run the test suite using the instructions in the repository.

---

# Contributing

Contributions are welcome.

Areas where contributions are especially useful:

* Compiler development
* Type system
* Standard library
* Runtime
* VM
* LAM-2
* Documentation
* Testing
* Benchmarking
* Platform support

Before contributing, please read:

```text
CONTRIBUTING.md
```

---

# Roadmap

The Proton roadmap includes:

* [ ] Expanded standard library
* [ ] More complete tooling
* [ ] Improved diagnostics
* [ ] Advanced lifetime analysis
* [ ] Escape analysis
* [ ] Improved LAM-2 optimization
* [ ] Additional target platforms
* [ ] Package management
* [ ] Debugger integration
* [ ] Production-oriented runtime improvements

The roadmap is subject to change as the language evolves.

---

# Language Support

Proton development tools include language-server support for modern editors.

Current tooling includes:

* Syntax highlighting
* Diagnostics
* Language Server Protocol support
* Editor integration

---

# Philosophy

Proton is built around a simple idea:

> **Low-level control should not require unnecessarily complicated language design.**

The project values:

* Predictability
* Explicitness
* Simplicity
* Performance
* Strong tooling
* Reproducibility
* Technical transparency

Proton does not aim to hide the machine.

It aims to make working close to the machine easier.

---

# Status

**Proton 11 — Experimental / Active Development**

Proton is not yet positioned as a drop-in replacement for established systems languages.

The language, compiler, runtime, and memory-management architecture are actively evolving.

Expect breaking changes.

---

# License

Proton is distributed under the license specified in:

```text
LICENSE
```

---

# Community

Discussions, bug reports, feature requests, and contributions are welcome.

If you find a compiler bug, please provide:

1. Proton version
2. Operating system
3. Architecture
4. Minimal reproducible source code
5. Expected behavior
6. Actual behavior

---

## Proton

**Simple enough to learn.
Low-level enough to build with.**

```text
PROTON 11
LAM-2
20,000,000 string iterations
~1.9 MB Peak RSS
```
