# GG Language Reference

GG is a statically typed, compiled language that emits LLVM IR and links via Clang to
native x86-64 executables. It aims for a familiar C/Java-style syntax with a lightweight
RAII memory model (stack values + refcounted heap references) and zero-overhead generics
via monomorphization.

---

## Table of contents

1. [Types](#1-types)
2. [Variables & declarations](#2-variables--declarations)
3. [Operators](#3-operators)
4. [Type conversions](#4-type-conversions)
5. [Control flow](#5-control-flow)
6. [Functions](#6-functions)
7. [Arrays](#7-arrays)
8. [Classes](#8-classes)
9. [Enums](#9-enums)
10. [Memory model](#10-memory-model)
11. [Generics](#11-generics)
12. [Imports & extern](#12-imports--extern)
13. [Traits & operator overloading](#13-traits--operator-overloading)
14. [Lambdas & callable objects](#14-lambdas--callable-objects)
15. [What GG does NOT support](#15-what-gg-does-not-support)
16. [Debugging (`--debug` / `-g`)](#16-debugging---debug----g)

---

## 1. Types

### Integers

| Type  | Width | Signed |
|-------|-------|--------|
| `i8`  | 8-bit  | yes |
| `i16` | 16-bit | yes |
| `i32` | 32-bit | yes |
| `i64` | 64-bit | yes |
| `u8`  | 8-bit  | no |
| `u16` | 16-bit | no |
| `u32` | 32-bit | no |
| `u64` | 64-bit | no |

Integer literals are untyped at the source level and infer their type from context.
The default numeric type when context provides no constraint is `i32`.

### Floating point

| Type  | Width |
|-------|-------|
| `f32` | 32-bit (single precision) |
| `f64` | 64-bit (double precision) |

Float literals require a decimal point: `1.0`, `3.14`, `-0.5`.

### Other primitives

| Type   | Notes |
|--------|-------|
| `bool` | `true` or `false` |
| `char` | Unicode code point, stored as 32-bit unsigned; single-quote literals `'A'`, `'\n'`, `'\t'` |
| `void` | Return type only — cannot be used as a variable type |

### Pointer types

> ⚠️ **Do not use in application code.**
>
> `ptr` and `ptr<T>` are unsafe, low-level raw pointer types. They exist exclusively
> for two purposes:
>
> 1. **Internal standard-library implementation** — e.g. `Vec`, `String`, and other
>    containers that need a manually managed heap buffer.
> 2. **CRT / OS integration** — `extern` declarations that call into C libraries
>    (e.g. `malloc`, `free`, system calls).
>
> The compiler enforces this by requiring the `--unsafe-ptr` flag for any source file
> that declares a variable, field, parameter, or return type involving `ptr`/`ptr<T>`.
> Without the flag the compiler rejects those declarations outright.
> `extern` declarations are always exempt from this check.
>
> If you think you need a raw pointer in application code, you almost certainly want
> a `Vec<T>` or a class with a `Class&` reference field instead.

| Type       | Notes |
|------------|-------|
| `ptr`      | Opaque raw pointer (alias `ptr<void>`). Maps to LLVM `ptr`. No GG-managed lifetime. |
| `ptr<T>`   | Typed raw pointer (element type `T`). Supports `[]` indexing (one GEP, no bounds check). `ptr<void>` is the same as `ptr`. |

Valid element types for `ptr<T>`: any primitive (`i32`, `f64`, …), `void` (opaque),
object (`ptr<Point>`), or reference (`ptr<Point&>`).

### String literals — the `str` type

A string literal like `"hello"` has type **`str`**: an immutable view over static (read-only) bytes.
It is **safe** (no `--unsafe-ptr` needed) and lowers to a `{ pointer, byte-length }` pair — there is
no allocation or copying. `str` is distinct from the heap-allocated, growable `String` class.

```gg
str s = "café";       // a view over 5 static bytes (é is 2 UTF-8 bytes)
s.len                 // 5  — the byte length (u64), O(1)
s.data                // the underlying pointer (ptr), NUL-terminated, for FFI/CRT calls
```

- `s.len` is the **byte** length (codepoint counting is a `String` operation).
- `s.data` gives a NUL-terminated `ptr`; a `str` also **implicitly decays to `ptr`**, so a literal can
  be passed straight to a `ptr`/`extern` parameter: `puts("hi")` works. (The reverse — a raw `ptr` to
  a `str` — is **not** allowed, since a pointer carries no length.)
- **Indexing** `s[i]` gives the i-th **byte** as a `char` (a code point). It is **bounds-checked** —
  an out-of-range index aborts at runtime (like a fixed-size array). So the digit-parsing idiom and
  byte iteration work:
  ```
  i32 digit = (s[0] as i32) - 48;              // '7' -> 7
  i32 i = 0;
  while (i < (s.len as i32)) { use(s[i]); i = i + 1; }
  ```
  (`s[i] = …` write-through is not allowed — a `str` is an immutable view.)
- Build a heap `String` from a literal with `String("...")` (its constructor takes a `str`).
- v1 limits: no slicing / concatenation / `==` on `str` yet, and `str?` is unsupported — use `String`
  for those.

### Class types

| Form        | Semantics |
|-------------|-----------|
| `ClassName` | Value — stack-allocated (or embedded in a parent). On copy: primitive **and value-object** fields are deep-copied (recursively), reference fields are shared (retained). |
| `ClassName&`| Reference — heap-allocated, intrusive refcount, `new` produces one. Assignment rebinds (release old, retain new). |

---

## 2. Variables & declarations

```gg
// Scalar declaration — zero-initialised
i32 x;

// Scalar with initialiser
i32 y = 42;
f64 pi = 3.14159;
bool flag = true;
char c = 'A';

// Object — stack-allocated, zero-initialised (no constructor call)
Point p;

// Object — stack-allocated, constructor called
Point p(1.0, 2.0);
Point p{1.0, 2.0};       // braces: an alternate delimiter for the same constructor call

// Object — initialised from another value (deep copy via clone helper)
Point q = p;

// Heap reference — `new` allocates and runs the constructor
Point& r = new Point(1.0, 2.0);
Point& r = new Point{1.0, 2.0};   // braces work here too

// Fixed-size array — stack-allocated, zero-initialised
i32[8] arr;

// Typed raw pointer — initialised to null by default
ptr<i32> buf;

// Typed raw pointer with initialiser (from malloc)
ptr<i32> data = malloc(sizeof(i32) * 16);
```

**Scope:** Variables are block-scoped. A new block `{ }` creates a new scope.
Re-declaring the same name in an inner block shadows the outer one.

### Type inference — `var`

`var name = expr;` lets the compiler **deduce** a local's type from its initializer, so you don't
repeat it. Like every binding it is **const by default**; use `mut var` (or `var mut`) for a
reassignable one. An initializer is **required** — there is nothing to infer from otherwise.

```gg
var i = 5;              // i32   (integer literals infer i32)
var f = 2.5;            // f64   (decimals infer f64)
var b = true;           // bool
var name = "gg";        // str  (a string literal; safe — see the `str` type above)

var p = Point(3, 4);    // Point — value object (copied like `Point q = p;`)
var r = new Point(1);   // Point& — owning heap reference (co-owned + released like the explicit form)
var col = Color::GREEN; // an enum

mut var total = 0;      // mutable; `total = total + i;` is allowed
```

`var` is for **locals only** — parameters, fields, and return types are always written explicitly.
You cannot infer from something with no value type: `var x = null;` (annotate the nullable type),
a `void`-returning call, or a lambda literal are all errors.

### Compile-time reflection — `inline for` + `@`-builtins

Reflection in GG is **compile-time only** and costs nothing at runtime — the compiler unrolls it
into ordinary code during compilation. Use it to write generic code over a type's fields.

`inline for (f in @fields(T)) { … }` is a **compile-time loop**: the body is copied once per field
of `T` (it is *not* a runtime loop). Inside it, `f.name` is the field's name as a string, and
`@field(v, f.name)` accesses that field of `v` (readable, and assignable — an lvalue):

```gg
// a generic structural-equality function
fn eqAll<T>(T& a, T& b) -> bool {
    inline for (f in @fields(T)) {
        if (@field(a, f.name) != @field(b, f.name)) { return false; }
    }
    return true;
}
```

For `T = Point { i32 x; i32 y }` this expands, at compile time, into two straight-line comparisons —
no loop, no runtime type data.

You can also iterate an **enum's variants** at compile time. Inside `inline for (v in @variants(E))`,
the binding `v` is the variant's singleton value (compare it with `==`) and `v.name` is its name:

```gg
enum Color { RED, GREEN, BLUE }

// the position of a variant in declaration order
fn ordinal(Color c) -> i32 {
    mut i32 i = 0;
    inline for (v in @variants(Color)) {
        if (c == v) { return i; }
        i = i + 1;
    }
    return 0 - 1;
}
```

Scalar queries fold to compile-time constants (like `sizeof`):

```gg
@typeName(Point)        // "Point"   (a string)
@fieldCount(Point)      // 2         (u64)
@hasField(Point, "x")   // true      (bool)
@offsetOf(Point, "y")   // 4         (u64 — byte offset of the field)
@alignOf(f64)           // 8         (u64 — natural alignment)
@variantCount(Color)    // 3         (u64)
@compileError("msg")    // aborts compilation with a message
```

Type predicates answer with a `bool` (usable directly as an `if` condition):

```gg
@isInteger(i32)         // true
@isFloat(f64)           // true
@isClass(Point)         // true
@isEnum(Color)          // true
@isPrimitive(bool)      // true
@implements(Point, Eq)  // true if Point has an `impl Eq for Point` (user or built-in trait)
```

`inline for` iterates only compile-time-known sequences (`@fields(T)` / `@variants(E)`); the binding
is a compile-time value (usable as `.name`, inside `@field`, or as the variant value — not stored or
passed as a runtime value). `break`/`continue` are not allowed directly inside an `inline for`.
Runtime reflection (asking a value its type, downcasting) does not exist — GG is static-dispatch only.

### Annotations — compile-time metadata

An `annotation` is a user-defined metadata type you attach to declarations and read back through
reflection. It has **no runtime representation** and emits **no code** — it exists purely to drive
compile-time generation (serializers, validators, key remapping, etc.).

Declare one with the `annotation` keyword. Its body holds only compile-time-constant fields
(`str`, primitives, `bool`, `char`, or enums) — no methods:

```gg
annotation Skip {}                       // a marker (no fields)
annotation Rename { str key; }           // one str field
annotation Range  { i32 lo; i32 hi; }    // two int fields
```

Apply one with an `@Name(args)` prefix on a **class, field, method, or enum variant**. Arguments
must be compile-time constants and match the annotation's field list:

```gg
class User {
    @Rename("user_id") mut i32 id;
    @Skip              mut i32 cache;
    @Range(0, 150)     mut i32 age;
}
```

Read annotations inside an `inline for`, or at the type level:

```gg
fn serialize<T>(T& v) -> i32 {
    mut i32 acc = 0;
    inline for (f in @fields(T)) {
        if (!f.has(Skip)) { acc = acc + @field(v, f.name); }   // skip @Skip fields
        if (f.has(Range)) {                                     // read @Range's data
            i32 x = @field(v, f.name);
            if (x < f.get(Range).lo) { return 0 - 1; }
        }
    }
    return acc;
}

@hasAnnotation(User, Skip)   // true — the type or any member carries @Skip
```

- `f.has(Ann)` → `bool`: does the current member carry `Ann`.
- `f.get(Ann).field` → the constant value of that annotation field.
- `@hasAnnotation(T, Ann)` → `bool`: does the type `T` or any of its members carry `Ann`.

A capitalized `@Name` in declaration position is an annotation; a lowercase `@name` (like
`@fields`, `@typeName`, `@compileError`) is a reflection builtin.

**Note:** an `inline for` unrolls unconditionally, so guard every `f.get(Ann)` with `if (f.has(Ann))`.
On a member that lacks `Ann`, an unguarded `f.get(Ann).field` folds to a type-default placeholder
(`0` / `""` / `false`) rather than an error (a `comptime if` that prunes such branches is a future
addition).

### Mutability — `const` by default, `mut` to opt in

Every binding is **immutable by default**. A const variable may be given a value exactly
once (its *defining* assignment), which may be deferred past the declaration line; any
*later* reassignment is a compile error.

```gg
i32 x = 10;
x = 20;             // ERROR: cannot reassign immutable variable 'x'

mut i32 y = 10;     // `mut` makes it reassignable
y = 20;             // OK
y += 5;  y++;       // OK — compound assignment and ++/-- also require `mut`

i32 z;              // deferred init is fine…
z = 3;              // …this is the one allowed defining assignment
z = 4;              // ERROR: already initialised

// Split initialisation across branches is allowed for a const (one path each):
i32 sign;
if (n < 0) { sign = -1; } else { sign = 1; }
```

Because loop counters and accumulators are mutated, they must be `mut`:

```gg
mut i32 total = 0;
for (mut i32 i = 0; i < 10; i++) { total = total + i; }
```

`mut` may be combined with `static` in either order (`mut static` / `static mut`).
Array/pointer **element** writes (`arr[i] = v`) are not gated by `mut` — only the binding
itself is (you cannot rebind the array variable regardless).

---

## 3. Operators

### Arithmetic
```gg
x + y    // addition
x - y    // subtraction
x * y    // multiplication
x / y    // division (integer: truncates toward zero; float: IEEE)
x % y    // remainder (integer only)
-x       // unary negate
```

### Bitwise (integers only)
```gg
x & y    // AND
x | y    // OR
x ^ y    // XOR
~x       // NOT (bitwise complement)
x << n   // left shift
x >> n   // arithmetic right shift (signed types), logical shift (unsigned types)
```

### Logical (booleans)
```gg
x && y   // short-circuit AND
x || y   // short-circuit OR
!x       // NOT
```

### Comparison
```gg
x == y   x != y
x <  y   x <= y
x >  y   x >= y
```

Comparisons return `bool`. Mixed signed/unsigned comparisons follow the types involved.

### Assignment
```gg
x = expr           // simple assignment; evaluates to the assigned value
x += expr          // compound: x = x + expr (also -=, *=, /=, %=, &=, |=, ^=)
x++   x--          // postfix: return old value, then mutate
++x   --x          // prefix: mutate, then return new value
```

### Other
```gg
arr[i]             // subscript (array or ptr<T>)
obj.field          // field read
obj.field = val    // field write
obj.method(args)   // method call
expr as i64        // explicit type cast (see §4)
sizeof(i32)        // size of a type in bytes, returns u64
new Point(1.0)     // heap allocation + constructor → Point&
```

---

## 4. Type conversions

### Numeric literals are *untyped* (they adopt the target type)

A bare numeric literal has no fixed type of its own — it **takes on the type of its context**, and
only falls back to a default (`i32` for integers, `f64` for decimals) when there is no numeric
target. So none of these warn, and no suffix (`5L`, `1.0f`) is needed:

```gg
i64 y = 5;          // 5 is i64
i8  b = 5;          // 5 is i8
u32 n = 5;          // 5 is u32   (no signed→unsigned warning)
f32 f = 1.0;        // 1.0 is f32 (no f64→f32 narrowing warning)
i64 big = 9000000000;   // typed i64 directly — no i32 overflow
var z = 5;          // no target → default i32
```

A literal that does **not fit** its target warns (and still adopts, truncating): `i8 b = 300;`.
A bare literal also adopts the type of a **non-literal numeric neighbour** in a binary expression,
so `big == 6000000000` and `big + 6000000000` (with `big : i64`) treat the literal as `i64`. But two
literals with no such neighbour keep their defaults — `f64 d = 1 / 2;` is integer division (`0.0`),
not `0.5` (write `1.0 / 2` for float division); and a decimal literal never becomes an integer, so
`1.0 / count` stays float division.

### Implicit conversions (with a warning for narrowing)

- Numeric widening is **silently allowed** (e.g. `i32` → `i64`, `f32` → `f64`).
- Numeric **narrowing of a value emits a warning** at compile time but is not an error. (A bare
  literal does not narrow — it adopts the target type per the rule above.)
- In a binary op mixing a **signed and an unsigned** integer, the operation is performed as
  **signed** (unlike C, which picks unsigned). So `-6 / 3u == -2` and `-1 < 1u` is `true`. Mixing
  an integer with a float promotes the integer to the float type.

> **Optional runtime overflow checks.** Compiling with `--overflow-checks` (or `compile.ps1
> -OverflowChecks`) makes integer arithmetic (`+` `-` `*`, `+=` `-=` `*=`, `++`/`--`, and signed
> negation) and out-of-range *implicit* narrowing conversions **trap** (abort) at runtime instead of
> wrapping silently — the Rust-in-debug model. It is **off by default**, so release builds pay
> nothing. An explicit `as` cast is never checked (it is an intentional truncation); division and
> float arithmetic are not checked. Separately, `compile.ps1 -Opt <0|1|2|3|s|z>` selects clang's
> optimization level (default `0`).
- `char` ↔ `u32`: silently interchangeable (both map to the same 32-bit value).
- `bool` → any integer: `false` = 0, `true` = 1.
- Any integer → `bool`: 0 = false, non-zero = true.
- `ptr<T>` ↔ `ptr`: silently interchangeable at the IR level.
- `ClassName&` → `ClassName` (in an assignment or initialiser): performs a **deep copy** (clone).

### Explicit cast: `expr as Type`

```gg
i32 n = 300;
u8  b = n as u8;          // truncate to 8 bits → 44

f64 pi = 3.14;
i32 trunc = pi as i32;    // 3

i32 neg = -1;
u32 bits = neg as u32;    // reinterpret as unsigned
```

Allowed cast directions:
- Integer ↔ integer (any widths)
- Integer ↔ float
- Float ↔ float (any widths)
- Integer → bool, bool → integer

**Cannot cast:** to/from `void`, or between unrelated class types.

### Mutability coercions (`as mut T`)

Mutability travels with **references**. Coercing a read-only (const) reference into a `mut`
binding — at a `mut Point&` initialisation/reassignment, or when passing a const reference to
a `mut Point&` parameter — is allowed but **emits a warning**, because it silently grants
write access to something borrowed read-only:

```gg
Point& b = new Point(1);
mut Point& a = b;              // WARNING: coercing a read-only reference into a 'mut' binding
mut Point& c = b as mut Point&;   // OK — the explicit `as mut` cast acknowledges it, no warning
```

An explicit cast is the only way to silence the warning. Going the safe direction — a `mut`
reference used where a read-only `Point&` is expected — is always silent. Value types are
copied, so their mutability is chosen independently and never triggers this warning.

---

## 5. Control flow

### if / else
```gg
if (x > 0) {
    // ...
} else if (x == 0) {
    // ...
} else {
    // ...
}
```

### while
```gg
while (condition) {
    // ...
    break;      // exit the loop
    continue;   // jump to the next iteration
}
```

### for
```gg
for (i32 i = 0; i < 10; i++) {
    // ...
}
// All three parts are optional: for (;;) { ... } is an infinite loop
```

### return
```gg
return expr;   // exits the current function, returning expr
return;        // for void functions
```

Before returning, the compiler automatically runs destructors for all live
local objects in reverse-declaration order (innermost scope first).

### switch (statement and expression)
Java-style `switch`, **arrow form only** — no fall-through, so no `break` is needed. An arm is
`case L1, L2, … -> body` (one or more labels) or `default -> body`, where `body` is either a single
`expr;` or a `{ … }` block. At most one `default`.

As a **statement** (the arm value, if any, is discarded):
```gg
switch (x) {
    case 1, 2 -> doA();
    case 3    -> { doB(); doC(); }
    default   -> doDefault();     // optional for statements
}
```

As an **expression** (each arm produces the switch's value; a block arm uses `yield`):
```gg
i32 y = switch (n) {
    case 0    -> 10;
    case 1, 2 -> { compute(); yield 20; }
    default   -> 0;
};
```

- **Label comparison** uses the `Eq` trait when the scrutinee's type implements it (dispatching to
  its `eq`), and the language's **default comparison** otherwise — exactly like `==`: enum variants
  and same-class references compare by identity, value objects compare memberwise (structural), and
  primitives compare by value. Labels may be integers, chars, bools, enum variants, or values of an
  `Eq`-implementing class.
- **Exhaustiveness**: a switch *expression* must be exhaustive — either it has a `default` arm, or
  the scrutinee is an enum and every variant is covered:
  ```gg
  i32 code = switch (color) {          // Color { RED, GREEN, BLUE }
      case Color.RED   -> 1;
      case Color.GREEN -> 2;
      case Color.BLUE  -> 3;           // all variants covered → no default needed
  };
  ```
  A switch *statement* never requires `default`.
- **Duplicate labels are an error** when they can be identified at compile time — integer, char,
  bool and string literals (including negated integers), enum variants, and identifier labels.
  Labels that are arbitrary runtime expressions are not checked.
- `yield expr;` supplies the value of a switch-expression block arm; every path through such a
  block must `yield` (or `return`). `yield` is only valid inside a switch expression.
- The scrutinee is evaluated once. A switch expression may produce a primitive, `bool`, `char`,
  enum, or reference — **not** a value object (bind it through a reference instead).

### Ternary conditional — `cond ? a : b`

A C-style conditional expression: `cond` must be a `bool`; the result is `a` when it's true, else `b`.

```
i32 x = y == 5 ? 3 : 2;              // 3 if y == 5, else 2
i32 sign = n > 0 ? 1 : (n < 0 ? -1 : 0);   // right-associative; parenthesize for clarity
print(ok ? "yes" : "no");           // usable anywhere an expression is
```

- It is **right-associative** and binds just below assignment, so `x = c ? a : b` reads as
  `x = (c ? a : b)`.
- Both branches must have a **common type** (like a switch expression). It can yield a primitive,
  `bool`, `char`, `enum`, `str`, or a reference — **not** a value object (use a reference).
- Don't confuse it with **Elvis `a ?: b`** (below), which is null-coalescing, not a boolean choice.

### Tuples — `(T1, T2, …)`

A **tuple** groups two or more values of possibly-different types without declaring a named class —
handy for multiple return values and ad-hoc pairs. A tuple is a **value object** (it lives on the
stack and copies by value); fields are positional and accessed as `._0`, `._1`, …:

```gg
fn divmod(i32 a, i32 b) -> (i32, i32) out {
    out = (a / b, a % b);          // a tuple literal
    return out;
}

(i32, i32) q = divmod(17, 5);
i32 whole = q._0;                  // 3
i32 rest  = q._1;                  // 2

((i32, i32), str) nested = ((1, 2), "hi");
i32 inner = nested._0._1;          // 2  — tuples nest
```

- **Arity ≥ 2.** `(x)` is just a parenthesized expression; `()` is reserved.
- A tuple **literal** takes its type from context (a typed variable, a parameter, a return type), so
  `(1, 2)` needs a known tuple type at the use site — annotate the binding (`var t = (1, 2)` can't
  infer a tuple type).
- Elements are **value or primitive types** in v1 (a class value like `String` is fine and is
  deep-copied in; references `&`/borrows `*`/nullable `?` elements are not yet allowed).
- Because a tuple is an object, a tuple **parameter is passed by reference** like any object — write
  `fn f((i32, i32)* pair)` (a borrow), not a bare value parameter.

### match — destructuring patterns

`match` takes a value apart by **shape**. Unlike `switch` (which compares the scrutinee to label
*values*), a `match` arm is a **pattern** that can bind variables. It comes in a statement form and an
expression form:

```gg
fn swap((i32, i32)* pair) -> (i32, i32) out {
    match pair {
        (a, b) -> { out = (b, a); return out; }     // bind a = pair._0, b = pair._1
    }
    return out;
}

fn classify(Point* p) -> i32 {
    match p {
        Point{ x: 0, y } -> { return 100 + y; }     // x must equal 0; bind y
        Point{ x, y }    -> { return x + y; }        // catch-all (must come AFTER the specific arm)
    }
    return -1;
}

i32 label = match n {                                // expression form
    0 -> 10;
    1 -> 20;
    _ -> n;                                          // `_` = wildcard
};
```

Pattern kinds: **wildcard** `_`, **binding** `name` (matches anything, binds it), **literal**
(`0`, `"hi"`, `true`, an enum variant — compared with `==`), **tuple** `(p0, p1)`, and **struct**
`Class{ field: subpat, field }` (a bare `field` binds that field by name). Patterns **nest**
(`((a, b), c)`, `Point{ x: 0, y }`).

- A **match expression must be exhaustive** — include a wildcard/binding arm (or a pattern that
  matches everything). A match *statement* need not be.
- Arms are tried top to bottom; an arm that can never be reached (it follows a pattern that already
  matches everything) is a **compile error** — put specific patterns before general ones.
- `yield expr;` supplies a match-expression block arm's value (as in `switch`); a bare `return`
  inside a block arm returns from the enclosing function.
- **Deferred** (v1): enum-payload patterns (`Some(x)`), list/collection patterns (`(x:xs)`),
  or-patterns, `if` guards, ranges, and `var (a, b) = …` destructuring bindings.

### Variadic parameters — compile-time type packs

A function can take a variable number of **heterogeneous** arguments via a **type pack**
`<...Ts>` + a `Ts... args` parameter. It's resolved entirely at compile time — each call
monomorphizes a concrete instantiation, so there is no runtime list and no boxing.

```gg
fn count<...Ts>(Ts... args) -> i32 {
    match args {
        ()     -> { return 0; }                    // empty pack — base case
        (x:xs) -> { return 1 + count(xs...); }      // head x, tail-pack xs (spread with `xs...`)
    }
}

count(1, "hi", true)     // 3  — the pack is (i32, str, bool)
```

- A call collects its trailing arguments into a **tuple** (the pack); the callee receives it and takes
  it apart with a compile-time `match args { () -> …; (x:xs) -> … }`: `()` matches the empty pack,
  `(x:xs)` binds the first element to `x` and the rest to a shorter pack `xs`, and `f(…, xs...)`
  recurses on that shorter pack until it bottoms out at `()`.
- The head `x` has each element's **concrete type** at that level, so ordinary overloading picks the
  right operation per element — no trait needed:

```gg
fn weight(i32 x) -> i32 { return 1; }
fn weight(bool b) -> i32 { return 10; }
fn sumW<...Ts>(Ts... args) -> i32 {
    match args { () -> { return 0; }  (x:xs) -> { return weight(x) + sumW(xs...); } }
}
sumW(1, true)            // 11
```

- **Fixed parameters** may precede the pack (`fn f<...Ts>(str fmt, Ts... args)`); the pack is always
  last. Pack element types may be primitives, `str`, and value classes (deep-copied in, like tuple
  elements).
- **v1 limitations** (clean errors): the pack's argument types must be knowable at parse time —
  literals, in-scope variables, or constructor calls; a value produced by another call (`f(g())`) or a
  forward-referenced function isn't inferable (the same limitation as generic type-argument
  inference). Explicit `<…>` type arguments on a variadic are rejected (the pack is always inferred).
  One pack per function. (Homogeneous `i32...` packing into an array, and binding C variadic functions
  like `printf`, are not part of this.)

---

## 6. Functions

Every function and method has a single, uniform signature shape:

```
fn NAME(params) [mut] [ -> RetType [alias] ]
```

- `fn` always leads — before `static`/`private` modifiers (e.g. `fn static dims() -> i32`).
- The **return type follows `->`**. Omitting `-> RetType` means the function returns `void`
  (`fn greet(ptr name) { … }`).
- The optional **return alias** names the result binding. It is **required for object-value
  returns** (which lower to a caller-provided slot) and **optional** for everything else.
- Trailing `mut` (a method that mutates `this`) goes after the parameter list: `fn f() mut -> i32`.
- Constructors (`ClassName(...)`) and destructors (`~ClassName()`) are the only exceptions —
  they do **not** take `fn` and have no return type.

**Return aliases** (`-> RetType alias`) turn the result into a named binding you fill and then
`return;` (or `return alias;` — those are the *only* returns allowed once an alias is declared):
- **Object value** (`-> Point p`): required; the alias is a caller-allocated slot filled in
  place (no copy). Zero-initialised.
- **Primitive** (`-> i32 r`): optional; a zero-initialised local you fill and return.
- **Reference** (`-> Node& r`): optional; a null-initialised local that **must be definitely
  assigned before it is returned** (checked like any other variable, on every path).

Without an alias, use the usual `return <expr>;`.

### Declaration
```gg
// Returns a value
fn add(i32 a, i32 b) -> i32 {
    return a + b;
}

// Returns nothing
fn greet(ptr name) {
    puts(name);
}

// Recursive — all top-level functions are forward-hoisted (order does not matter)
fn fib(i32 n) -> i32 {
    if (n <= 1) { return n; }
    return fib(n - 1) + fib(n - 2);
}
```

### Class parameters
Functions may receive **class references** (`ClassName&`) as parameters — these are
borrows (the caller retains ownership). They may **not** be reassigned inside the function
(the binding is immutable), but their fields can be mutated freely.

Value-typed class parameters (`ClassName` without `&`) are not supported;

```gg
fn bump(Point& p) {
    p.x = p.x + 1;    // OK — mutate the shared object through the borrow
    // p = other;     // ERROR — cannot reassign a reference parameter
}
```

### Passing objects
A `ClassName&` parameter accepts **either** a heap reference **or** a plain value object. A
value object passed where a reference is expected is **borrowed**: the callee receives the
object's address — no copy, no refcount change.

```gg
fn bump(Point& p) { p.x = p.x + 1; }

Point  v = Point(1.0, 2.0);
Point& h = new Point(3.0, 4.0);
bump(v);    // value object borrowed as Point& (address passed; no copy)
bump(h);    // heap reference passed as usual
```

This borrow is only valid **in argument position**. Binding a value object to a reference in
other contexts is rejected, because a stack object has no refcount header and the reference
machinery would corrupt memory:

```gg
Point  v = Point(1.0, 2.0);
Point& r = v;                // ERROR — cannot bind a value object to a reference
```

Consequently a borrowed value object must be treated as a **pure borrow** by the callee: do
not store it in a reference field, return it as a `ClassName&`, or otherwise retain it beyond
the call. (There is no lifetime checker to enforce this — it is your responsibility, as with
raw pointers.)

### Parameter mutability
Parameters are **const by default**, like locals. A primitive parameter that the body
reassigns must be declared `mut`:

```gg
fn countdown(mut i32 n) -> i32 {
    mut i32 steps = 0;
    while (n > 0) { n--; steps++; }   // both `n` and `steps` are `mut`
    return steps;
}
```

Reference parameters come in two flavours (a read-only borrow vs a mutable borrow):

```gg
fn readOnly(Point& p)  { i32 a = p.x; }   // const borrow — may read, may NOT write fields
fn mutate(mut Point& p) { p.x = 5; }       // mutable borrow — may write the object's mut fields
```

- `Point&` (const borrow) — you may read the object and call its methods, but you may **not**
  write its fields (transitive const, see §8). Passing an object to a `Point&` parameter is
  the usual read-only case.
- `mut Point&` (mutable borrow) — you may additionally write the object's `mut` fields.
- Neither form may be **rebound** (`p = other`) — a reference parameter is a borrow, not an
  owning binding, so rebinding it would corrupt the refcount.
- Passing a read-only (const) reference into a `mut` reference parameter is a **const→mut
  coercion** and produces a warning (see §4).

### Default parameter values
A parameter may declare a **default value** with `= expr`, used to fill the argument when the
caller omits it:
```gg
fn make(i32 a = 0, i32 b = 0) -> Point p { p.x = a; p.y = b; }

make();        // a = 0, b = 0
make(5);       // a = 5, b = 0
make(5, 6);    // both explicit
```
- A default may sit on **any** parameter, in any position — `fn f(i32 a = 0, i32 b, i32 c = 0)` is
  fine. A purely **positional** call can only omit a *trailing* run of defaults (binding is
  left-to-right), but a **named** call (below) can skip a defaulted middle parameter.
- Available on **functions, methods, and constructors** (not `extern`).
- A default is **any expression valid in the enclosing scope**, evaluated per-call at the call
  site; it may **not** reference the function's own parameters (or `this`).
- Omitting a required (non-defaulted) parameter is an error.

### Named arguments
Arguments may be passed **by parameter name**, in any order, and mixed with positional ones (the
positional arguments must come first):
```gg
fn span(i32 lo, i32 step = 10, i32 hi) -> i32 { return (hi - lo) * step; }

span(1, 2, 3);            // positional: lo=1, step=2, hi=3
span(hi: 3, lo: 1);       // named, reordered — step defaults to 10
span(1, hi: 3);           // mixed: lo=1 positional, hi=3 named, step defaults
```
- **Names participate in overload resolution** — an argument naming a parameter a candidate doesn't
  have makes that candidate non-viable, so names can select between overloads.
- A **positional argument may not follow a named one** (`f(a: 1, 2)` is an error).
- Errors: naming an **unknown parameter**, naming one **twice** (or once positionally + once by
  name), or leaving a **required** parameter unfilled.
- Arguments are **evaluated in the order written**, then reordered into parameter order — so side
  effects happen left-to-right as they appear at the call site.
- Works on functions, methods, static methods, and `new`. Brace construction (`Point{...}`) is
  positional only, and the callable-object sugar `obj(...)` is positional only — call `obj.call(...)`
  to pass its arguments by name.

### Calling conventions
- Primitive types pass by value.
- `ClassName&` passes the heap pointer by value (a borrow — no extra retain/release at the call site).
- There are **no variadic functions** (use `extern` + C variadics if needed).

### Overloading
Functions may be **overloaded** — several may share a name if they differ in parameter
signature and/or return type:

```gg
fn add(i32 a, i32 b) -> i32        { return a + b; }
fn add(i32 a, i32 b, i32 c) -> i32 { return a + b + c; }
fn add(f64 a, f64 b) -> f64        { return a + b; }

fn make() -> i32 { return 7; }     // differs from…
fn make() -> f64 { return 2.5; }   // …only by return type
```

Resolution is **best-match**: the compiler keeps candidates whose arguments are implicitly
convertible, then picks the one with the lowest total conversion cost (exact match beats a
widening conversion, which beats a narrowing one). Ties are an **ambiguous call** error.

**Return-type overloading** is disambiguated by the **expected type** of the call's context —
a variable's declared type, an assignment target, a `return`, or an explicit cast:

```gg
i32 a = make();          // picks i32 make()
f64 b = make();          // picks f64 make()
i32 c = make() as i32;   // cast selects the i32 overload
make();                  // ERROR: ambiguous — no context to choose; add a cast
```

Rules: two entities with the **same** parameter types **and** the same return type is a
redefinition error; parameter `mut`-ness is not part of the signature. `extern` functions and
`main` cannot be overloaded.

---

## 7. Arrays

### Fixed-size stack arrays
```gg
i32[8]  arr;          // zero-initialised array of 8 i32 values
arr[0] = 10;
arr[7] = 99;
i32 v = arr[3];       // bounds-checked at runtime (aborts on out-of-bounds)
```

Size must be a compile-time constant. Arrays cannot be passed to functions or
returned from them directly

### Typed raw pointer buffers (`ptr<T>`)
```gg
ptr<i32> data = malloc(sizeof(i32) * 64);   // no bounds check on []
data[0] = 42;
i32 x = data[5];
free(data);
```

`ptr<T>` subscripting uses a single GEP (no bounds check). It is the building
block for implementing dynamic containers. See §9 for the full memory model.

---

## 8. Classes

### Declaration
```gg
class Point {
    mut f32 x;          // `mut` fields — writable after construction (e.g. by scale())
    mut f32 y;
    private i32 id;     // const by default: set in the ctor, never written again

    // Constructor — name matches class name, no return type
    Point(f32 x, f32 y) {
        this.x = x;
        this.y = y;
    }

    // Destructor — ~ClassName(), no parameters, no return type (at most one per class)
    ~Point() {
        // cleanup runs automatically at scope exit and before every return
    }

    // Regular method
    fn squaredLen() -> f32 {
        return this.x * this.x + this.y * this.y;
    }

    // Void method — mutates fields, so it is a `mut` method
    fn scale(f32 factor) mut {
        x = x * factor;   // implicit `this` — same as this.x = this.x * factor
        y = y * factor;
    }
}
```

### Fields
A field may be a **primitive**, a **heap reference** (`Point&`), another class **value**
(embedding), a **`ptr`/`ptr<T>`** (unsafe), or an **enum**.

```gg
class Line {
    mut Point start;    // value-object field — Point is embedded contiguously in Line
    mut Point end;
    mut Node& owner;    // reference field — a refcounted heap pointer
    Line(Point& a, Point& b, Node& o) { start = a; end = b; owner = o; }
}
```

- **Value-object fields (embedding)** live *inside* the parent object — no separate heap
  allocation, no refcount for the sub-object; they share the parent's lifetime. When the parent
  is copied, value fields are **deep-copied** (recursively); when it is destroyed, they are
  **destroyed** (recursively). A field type may name a class declared later in the file.
- **Reference fields** are shared and refcounted (copied by pointer + retained on copy, released
  on destruction) — use one when you want shared ownership or a nullable/rebindable link.
- **Value-embedding cycles are rejected** — `class A { B b; }` with `class B { A a; }` is an
  infinite-size object. Break the cycle by making one side a reference (`A& a`).
- Assign a value-object field in the constructor with `this.f = other` (a deep copy); left
  unassigned it is zero-initialised, like a primitive.

### Implicit `this` — members without `this.`
Inside a method you may refer to a field or method of the enclosing class by its **bare
name** — `x` means `this.x`, `inc()` means `this.inc()`. Name resolution gives class members
the **lowest priority**: a local variable, parameter, or free function of the same name
shadows the member.

```gg
class Point {
    mut i32 x;
    i32 y;
    Point(i32 a, i32 b) { x = a; y = b; }   // `x`/`y` are the fields (no local shadows them)
    fn shift(i32 x) -> i32 { return x + y; }        // `x` = the parameter; `y` = the field
}
```

The usual rules still apply through the implicit `this`: a bare write obeys field mutability
(`x = 5` needs `x` to be a `mut` field and the method to be `mut`), and a bare `foo()` call to
a `mut` method requires a `mut` receiver. `this.x` remains valid and is required when a local
deliberately shadows the field.

### Using classes
```gg
// Stack value — zero-initialised (fields = 0), no constructor call
Point zero;

// Stack value — constructor called
Point p(3.0, 4.0);
Point q{3.0, 4.0};        // brace form — identical to the line above

// Field access (read / write)
f32 len = p.x * p.x + p.y * p.y;
p.x = 10.0;

// Method call
f32 sq = p.squaredLen();
p.scale(2.0);

// ── Assignment semantics ──────────────────────────────────────────────────

// value = value  →  clone
// Primitive fields are copied (independent). Reference fields are shared
// (the handle is copied and the target's refcount is incremented — no recursion
// into the graph, so cycles in the reference graph can never cause infinite loops).
Point q = p;           // q.x / q.y are independent of p.x / p.y

// value = ref  →  deref + clone (same rules as value = value)
Point& r = new Point(1.0, 2.0);  // r is a heap reference
Point v = r;           // v is a fresh stack copy; mutating v.x does not affect r.x

// ref = ref  →  rebind (release old target, retain new; no copy)
// The variable being rebound must be `mut` (reassigning a binding).
Point& r2 = new Point(9.0, 9.0);
mut Point& s = r;      // s and r share the same heap object; refcount → 2
s = r2;                // s releases r, binds to r2; r's refcount drops but r stays alive

// clone value to heap  →  new ClassName(value) produces a ClassName&
// new takes the value, deep-copies it into a fresh heap allocation, and returns a ref.
Point& heap = new Point(p);   // heap is an independent heap copy of p

// Heap member access
r.x = 99.0;     // mutates the heap object through a reference
```

### Brace construction (`Type{args}`)
Braces are an alternate delimiter for a **positional constructor call** — `Point{1, 2}` is exactly
`Point(1, 2)` (same overload resolution). They work for stack values, `new`, and constructor
arguments:
```gg
Point p{1, 2};                     // ≡ Point p(1, 2)
Point& r = new Point{1, 2};        // ≡ new Point(1, 2)
f32 d = dist(Point{0, 0});         // as an argument
```
For nested construction, the inner type can be **omitted** — it is deduced from the constructor's
parameter type (like C++'s `Line l{ {0,0}, {1,1} }`):
```gg
class Line { Point start; Point end; Line(Point& a, Point& b) { start = a; end = b; } }
Line l{ {1, 2}, {10, 20} };        // each {…} builds a Point (deduced from the parameter type)
Point p = {7, 8};                  // untyped braces also deduce from the declared/return type
```
This is *not* field-fill: it always runs a constructor, positional only (no named fields), and the
type must be inferable (untyped `{…}` with no class context, or a brace on an enum, is an error).

### Access control
- Members are **public by default** — accessible from anywhere.
- Prefix a member with `private` to restrict it to the class's own methods.
- Accessing a `private` member from outside the class emits a **compile-time warning** but is not an error — the code still compiles and runs.
- **Free functions** may also be `private`: `fn private helper(...) { … }`. This makes the function **file-local** — calling it from a **different source file** (i.e. through an `import`) emits the same **advisory warning**, while calls within the declaring file are silent. As with members, it is warning-level, not enforced. (Not supported on generic free functions in v1.)

### Field mutability — `const` by default, `mut` to opt in
Instance fields are **immutable by default**, just like local variables. A const field may
be assigned only via `this.field = …` inside the class's **own constructor**; writing it
anywhere else (another method, or through an instance `obj.field = …`) is a compile error.
Prefix the field with `mut` to allow writes after construction:

```gg
class Counter {
    mut i32 n;          // writable by methods and from outside
    i32 id;             // const — fixed at construction

    Counter(i32 id) { this.n = 0; this.id = id; }   // both set in the ctor: OK
    fn inc() mut  { this.n = this.n + 1; }         // OK — `mut` method writing a mut field
    // void bad()   { this.n = 0; }                  // ERROR — non-mut method writing a field
    // void reid()  { this.id = 7; }                 // ERROR — id is const
}
```

`static` fields are **const by default** too — `mut static` makes one reassignable, plain
`static` is a named constant (see [Static members](#static-members)). Enum fields are always
immutable (`mut` is not allowed on them).

**Transitive const.** Writing a `mut` field from *outside* the class also requires the
**receiver itself to be mutable** — const-ness is transitive (Rust-like), not just per-field:

```gg
mut Point p(1, 2);   p.x = 5;     // OK — p is a mut binding and x is a mut field
    Point q(1, 2);   q.x = 5;     // ERROR — q is a const binding, even though x is mut
```

The same applies through references: a field is writable through `mut Point&` but not through
a read-only `Point&`. Array and pointer *element* writes (`a[i] = v`) are never gated — only
the binding is.

### Method mutability — `mut` methods (Rust `&mut self`)
A method that mutates the receiver must be marked with a trailing `mut` (after the parameter
list). This is GG's `&mut self` / `&self` distinction:

```gg
class Counter {
    mut i32 n;
    Counter()      { this.n = 0; }
    fn inc() mut { this.n = this.n + 1; }   // mutates `this` → must be `mut`
    fn get() -> i32     { return this.n; }         // read-only → no `mut`
}

mut Counter a;  a.inc();   // OK — `a` is a mut binding
    Counter b;  b.inc();   // ERROR — inc() is a mut method; `b` is a const binding
    Counter c;  c.get();   // OK — get() is read-only, callable on any binding
```

Rules:
- Inside a **non-`mut`** method, `this` is read-only: writing `this.field` or calling a `mut`
  method on `this` is an error.
- A **`mut`** method may write `this`'s `mut` fields and call other `mut` methods on `this`.
- Calling a `mut` method **requires a mutable receiver** (a `mut` binding, or `this` inside a
  `mut` method). Read-only methods can be called on any receiver.
- **Constructors and destructors** are implicitly mut-context (they may mutate `this` and call
  `mut` methods without being marked). `static` and `enum` methods **cannot** be `mut`.

### Rules & restrictions
- **Constructors may be overloaded** — a class may declare several constructors that differ
  in parameter signature (resolved by best-match at `new Point(…)` / `Point p(…)`). Enums keep
  exactly one constructor.
- **Methods may be overloaded** too (instance and static), by signature and/or return type —
  same rules as free functions (see §6).
- **At most one destructor** per class — `~ClassName()` takes no parameters.
- **No inheritance** — classes cannot extend other classes.
- **No virtual methods** — there is no vtable or dynamic dispatch.
- Destructors **cannot be called explicitly** — the compiler injects calls automatically.
- Destructor injection order: **last declared, first destroyed** (LIFO) within a scope.

### Constructors and zero-init
If you declare an object variable without parentheses (`Point p;`), its fields are
zero-initialised and the constructor is **not** called. Always call the constructor
explicitly when the class's invariants require it.

### Static members
Prefix a field or method with `static` to make it **class-level** — shared by all
instances rather than stored per object. Both forms are accessed with the
scope-resolution operator `ClassName::member`, **or** through any instance
(`obj.member`); both resolve to the same shared storage / receiver-less call.

```gg
class Counter {
    mut static i32 count = 0;    // `mut static` — one shared, reassignable slot for the program
    mut static i32 limit;        // zero-initialised if no initialiser given
    static i32 MAX = 1000;       // const static — a named constant (immutable)

    Counter() {
        Counter::count = Counter::count + 1;   // mutate the shared `mut` field
    }

    // Static method — no implicit `this`; cannot touch instance fields.
    fn static howMany() -> i32 {
        return Counter::count;
    }
}

fn main() {
    Counter a;
    Counter b;
    i32 n = Counter::howMany();   // 2  — via the class
    i32 m = a.howMany();          // 2  — via an instance (same call)
    Counter::limit = 100;         // OK — `limit` is a mut static
    i32 cap = Counter::MAX;       // read the constant
    // Counter::MAX = 2000;       // ERROR — MAX is a const static (declare `mut static` to write)
}
```

Rules:
- **Static fields** are real globals, **not** part of the struct layout, and are
  **const by default** like every other binding — write `mut static` to make one
  reassignable, or leave it `static` for a named constant. Writing a non-`mut` static
  (via `Class::field`, an instance, or implicit `this`) is a compile error. An optional
  constant initialiser (`static i32 count = 0;`) runs once before `main`.
- **Static methods** have **no implicit `this`** — using `this` inside one is an error,
  and they cannot read or write instance fields. They may freely access static fields.
- Calling an **instance** method via `ClassName::method(...)` is an error ("not static").
  Calling a **static** method through an instance (`obj.method(...)`) is allowed.
- Enums may **not** declare static members.

### Static local variables (C-style)
Inside a function or method body, a `static` local is a **single persistent global**:
it keeps its value across calls and is initialised exactly once before `main`.

```gg
fn nextId() -> i32 {
    static i32 counter = 0;   // initialised once, before main
    counter = counter + 1;    // persists across calls
    return counter;
}

fn main() {
    i32 a = nextId();   // 1
    i32 b = nextId();   // 2
    i32 c = nextId();   // 3
}
```

Restrictions:
- Only **scalar primitive** types (numeric / `bool` / `char`) are supported.
- The initialiser (if present) must be a **compile-time constant** (literals and
  unary/binary/cast expressions over constants). Without an initialiser the storage is
  zero-initialised.
- Two functions may declare identically named static locals with no conflict — each is
  an independent global.

---

## 9. Enums

GG enums are **Java-style**: each variant is a global singleton object, not an integer.
Variants may carry immutable fields, declare methods and a constructor, and are compared
by **identity**.

### Fieldless enums
```gg
enum Color {
    RED,
    GREEN,
    BLUE
}

fn main() {
    Color c = Color.GREEN;       // variants accessed as Enum.VARIANT
    if (c == Color.GREEN) { }    // identity equality only
    if (c != Color.RED)   { }
}
```

### Enums with fields, a constructor, and methods
```gg
enum Planet {
    MERCURY(3.303, 2.4397),      // variant list comes first, separated by commas
    EARTH(5.976, 6.37814),
    JUPITER(1898.0, 71.492);     // terminated with a semicolon before the body

    f64 mass;                    // fields are immutable after construction
    f64 radius;

    Planet(f64 mass, f64 radius) {
        this.mass = mass;        // fields may only be assigned via `this.field =`
        this.radius = radius;    // inside the constructor
    }

    fn gravity() -> f64 {
        return this.mass / (this.radius * this.radius);
    }
}

fn main() {
    f64 m = Planet.EARTH.mass;          // field read on a variant
    f64 g = Planet.JUPITER.gravity();   // method call on a variant

    Planet p = Planet.MERCURY;          // bind a variant to a variable
    p = Planet.EARTH;                   // rebind
    if (p == Planet.EARTH) { }          // identity comparison
}
```

### Rules & restrictions
- **Variant list first** — comma-separated; if a body (fields/methods) follows, terminate
  the variant list with a semicolon.
- **Each variant arg count must match the constructor arity.** Every declared field must
  be initialised in the constructor.
- **Fields are immutable** — assignable only via `this.field =` inside the constructor;
  there is no external write.
- **Identity equality only** — `==` / `!=` compare the singleton address. No `<`, `>`,
  ordinal, or other operators.
- An enum value is a handle to a singleton (lowers to a pointer); binding/rebinding a
  variable copies the handle (no allocation).
- Enums **cannot** be constructed directly (`Planet(...)`), `new`-ed, given a destructor,
  or declare `static` members.

---

## 10. Memory model

GG has three memory strategies, chosen by the declaration form:

### Stack values (`ClassName`)
```gg
Point p(1.0, 2.0);   // lives on the stack; freed automatically when scope exits
Point q = p;          // clone — see rules below
```
Value assignment (and value initialisation) calls `@ClassName_clone`, which applies
the following rules field by field:

- **Primitive fields** (`i32`, `f32`, `bool`, …) — copied by value. The two objects
  are completely independent for these fields.
- **Reference fields** (`Class&`) — the handle is copied and the target's refcount is
  incremented (`retain`). Both objects share the same heap sub-object; neither owns
  it exclusively.

This strategy is safe with cyclic reference graphs (a cycle in `Class&` fields can
never cause infinite recursion because the clone never follows reference pointers).
It is also cheap: no memo map, no whole-graph traversal.

If you need a fully independent deep copy of a reference-connected graph, do it
explicitly — e.g. build a new graph node by node.

### Heap references (`ClassName&`)
```gg
Point& r = new Point(1.0, 2.0);  // allocates 8-byte header + object on heap
                                  // refcount starts at 1

Point& s = r;    // s and r now share the same heap object; refcount → 2
                 // releasing either one decrements the count

// at scope exit: s released (count → 1), then r released (count → 0 → free)
```

- `new ClassName(args)` allocates a refcounted heap object and calls the constructor.
- `ref = ref` **rebinds**: retains the new target (count++) then releases the old one (count--). No copy is made.
- To create a ref from a value, use `new ClassName(value)` — this clones the value into a fresh heap allocation and returns a `ClassName&`. There is no implicit `ref = value` assignment.
- When the refcount reaches zero, the destructor (if any) is called, then `free`.
- There is **no cycle detection** — circular reference graphs will leak.

### Non-owning borrows (`T*` / `mut T*`)
```gg
Point& owner = new Point(1, 2);   // owning reference (co-owns the heap object) — postfix `&`
Point* b = owner;                 // borrow: a view, takes no ownership, no refcount change
mut Point* m = owner;             // mutable borrow — may write through it
m.x = 9;                          // mutates the same object `owner` points at

fn shift(mut Point* p, i32 dx) { p.x = p.x + dx; }   // borrow parameter
fn sumOf(Point* p) -> i32 { return p.x + p.y; }      // shared-borrow reader
```

A `T*` is a **non-owning borrow** — like a `ClassName&` but without any ownership. The two
reference sigils are symmetric and both **postfix**: `T&` **owns** (co-owns the heap object,
refcounted), `T*` **borrows** (a zero-cost view). A borrow never touches the refcount (no retain on
bind, no release at scope exit, no `+1` on return). Use it for parameters that only look at / mutate
a value, and for returning a borrow of something the caller already keeps alive (e.g. an element of
a container). *(There is no `ref` keyword; and the unsafe raw `ptr` is a separate, internal-only
type — see below.)*

`T` may be a **class** (`Point*`) or a **primitive** (`i32*`). An `i32*` is an lvalue reference
exactly like C++'s `int&`: reading it yields the value (a load through the borrow), and writing
through a `mut i32*` stores into the borrowed location:

```gg
mut i32 n = 5;
mut i32* r = n;
r = r + 10;          // reads through r (→ 5), writes through r → n is now 15

class Vec {
    ptr<i32> data;
    fn at(i32 i) -> i32* { return data[i]; }   // borrow an element, like vector<int>::operator[]
}
mut i32* e = v.at(0);
e = 42;              // writes into the buffer through the borrow
```

- Both an owning `ClassName&` and a stack value object coerce into a `ClassName*` class borrow; a
  primitive lvalue (a variable or an element `a[i]`) coerces into a primitive borrow.
- A borrow **cannot** be converted back into an owning `ClassName&` (it has no ownership to give).
- A borrow **cannot be a class field** — a field must own (`ClassName&`) or embed (a value).
- A primitive borrow must bind an **addressable** value — `i32* x = a + b;` (a temporary) is a
  compile error.
- Returning a borrow is allowed, but passing a **stack value object** to a `T*` parameter that
  returns or stores it is a compile error (the borrow would outlive the value) — pass a heap
  `ClassName&` in that case. This is enforced by escape analysis.
- `ptr` and `void` cannot be borrowed.
- You can assign directly to a returned primitive borrow — `v.at(i) = x;` stores through it, just
  like C++'s `vector<int>::operator[]`. (Storing a whole object through a returned class reference
  is not yet supported.)

### Nullable references (`T?`)
A reference type may be made **nullable** with a postfix `?`, meaning "possibly absent" — Kotlin's
model: `null` exists, but the type system forces you to handle it before use.
```gg
Node&? maybe = find(key);     // owning reference that may be null
Node*? peek = borrowMaybe();  // a nullable borrow
Color? c = null;              // enums are nullable too

if (maybe == null) { return 0; }   // guard clause
i32 v = maybe.v;                   // smart-cast: `maybe` is now non-null here

Node& sure = maybe!!;              // non-null assertion (aborts if null)
Node& orElse = maybe ?: fallback;  // Elvis: `maybe` if non-null, else `fallback`
```
- **What can be nullable.** Owning `Class&?`, borrow `Class*?`, and enum `Color?` cost nothing extra
  — they share the plain pointer representation (`null` is the machine null). A **primitive** (`i32?`,
  `bool?`, …) is also nullable, stored as a small tag+payload value (like Rust's `Option<i32>`); it
  needs `!!` / `?:` / a null check to read, exactly like a nullable reference. A **value object
  (`Point?`) cannot be nullable** — use a reference (`Point&?`).
- **`null`** is assignable only where a `T?` is expected. A non-null `T` widens implicitly to `T?`;
  the reverse (using a `T?` as a `T`) requires unwrapping.
- **`== null` / `!= null`** test for absence.
- **`x!!`** unwraps `T?` → `T`, aborting the program if it is null (the unsafe escape hatch — there
  are no exceptions to catch).
- **`a ?: b`** (Elvis) evaluates to `a` when non-null, otherwise `b`.
- **Smart-casts** are the safe, preferred path: inside `if (x != null) { … }`, and after a guard
  clause `if (x == null) { return; }`, the binding `x` is treated as the non-null `T` with no
  ceremony. Reassigning `x` to `null` (or another nullable) drops the narrowing; reassigning it to a
  non-null value keeps it. (v1 narrows bare local/parameter bindings, not field chains.)
- **`?.`** (safe access/call) short-circuits on null: `x?.field` / `x?.method()` evaluates the
  receiver, and if it's null the whole expression is `null`, otherwise it does the access. The
  result is nullable (so `a?.get()` on a `C&?`, where `get` returns `i32`, yields an `i32?`). A
  `?.` cannot yield a value object — return a reference or a primitive.

### Raw pointers (`ptr` / `ptr<T>`)
```gg
ptr buf = malloc(64);    // opaque — GG tracks no lifetime
free(buf);               // must free manually

ptr<i32> data = malloc(sizeof(i32) * 16);
data[0] = 42;
free(data);              // must free manually
```
`ptr` and `ptr<T>` have **no GG-managed lifetime**. You must call `free` yourself
(usually from a destructor). Use these only at the FFI boundary or inside class
internals (e.g. dynamic buffer in a container class).

### `destroy(place)` — run a value object's destructor in place

`destroy(place)` explicitly runs the destructor of a value object living at `place`. It exists so a
hand-written container can store value objects **by value** in a `ptr<T>` buffer and tear each one
down (the compiler auto-destroys locals and fields, but not elements inside a raw buffer it doesn't
track). Like the raw-pointer types, it is `--unsafe-ptr`-gated and container-internal.

```gg
fn pop() mut -> T out {
    count = count - 1;
    out = data[count];        // clone the element out to the caller (ownership transfer)
    destroy(data[count]);     // then destroy the buffer copy
    return out;
}
```

`destroy` on a primitive or a class with no destructor is a no-op. It **cannot** be used on a plain
local variable (those are destroyed automatically at scope exit — doing so would double-free); use it
only on `ptr<T>` elements and fields the compiler does not manage for you.

### `sizeof`
```gg
u64 n = sizeof(i32);    // 4
u64 m = sizeof(f64);    // 8
u64 k = sizeof(Point);  // sum of field sizes (LLVM struct layout)
```
Returns `u64`. Use it to compute allocation sizes for `malloc`.

---

## 11. Generics

GG generics are **monomorphized at compile time** — each unique type argument
combination produces a separate concrete class or function. There are no runtime
type parameters, no boxing, and no overhead versus hand-written concrete types.

### Generic classes
```gg
class Box<T> {
    T value;
    Box(T v) { this.value = v; }
    fn get() -> T  { return this.value; }
}

class Pair<K, V> {
    K first;
    V second;
    Pair(K a, V b) { this.first = a; this.second = b; }
}
```

Usage:
```gg
Box<i32>& b = new Box<i32>(42);
if (b.get() != 42) { ... }

Pair<i32, f64>& p = new Pair<i32, f64>(1, 3.14);
```

Each distinct instantiation (`Box<i32>`, `Box<f64>`) is compiled as a separate class.

### Generic functions
```gg
fn maxT<T>(T a, T b) -> T {
    if (a > b) { return a; }
    return b;
}

fn firstOf<K, V>(K a, V b) -> K { return a; }
```

Usage:
```gg
i32 m = maxT<i32>(10, 20);
i64 n = maxT<i64>(1000, 2000);
```

### Type-argument deduction

A generic function called **without** the explicit `<…>` deduces its type parameters from the
argument types, when it can:

```gg
class Box { mut i32 v; Box(i32 x) { this.v = x; } }
fn unwrap<T>(T& b) -> i32 { return b.v; }

fn main() -> i32 {
    mut Box b = Box(7);
    return unwrap(b);          // T deduced as Box — same as unwrap<Box>(b)
}
```

Deduction works when a type parameter appears **directly** as a parameter's type (`T`, `T&`, `T*`,
`T?`) and the matching argument's type is known from its syntax — an in-scope variable, a
`Class(...)` constructor call, or `new Class(...)`. It is **all-or-nothing**: if any type parameter
cannot be deduced, the call is an error asking you to write the type arguments explicitly. Cases
that still need explicit `<…>`:

```gg
fn zero<T>() -> T out { }        // T is only in the return type — nothing to deduce from
var z = zero<i32>();             // → must write it

fn echo<T>(T x) -> i32 { ... }
echo(5);                         // error: a bare literal has no syntactic type — write echo<i32>(5)
```

Explicit type arguments always work; deduction is just a convenience for the common case.

### Cross-file generics
Templates defined in imported files are available at use sites:
```gg
import "box_lib.gg";

Box<i32>& b = new Box<i32>(99);   // Box<T> was declared in box_lib.gg
```

### Trait bounds
A type parameter may carry **trait bounds** requiring the concrete type argument to
implement one or more traits (see §13). Use `T: Trait`, or `T: TraitA + TraitB` for
several, and bound each parameter independently:
```gg
trait Comparable { fn compareTo(Self& other) -> i32; }

fn maxOf<T: Comparable>(T& a, T& b) -> T& {
    if (a.compareTo(b) >= 0) { return a; }
    return b;
}

class Wrapper<T: Show + Ord> { T& inner; /* ... */ }
```
Bounds accept **user traits and the built-in operator traits** (`Add`, `Ord`, `Eq`, …).
Dispatch is static — bounds add no runtime cost.

Enforcement happens in **two** places:

1. **At each instantiation site**: `maxOf<Point>` requires `Point` to implement `Comparable`,
   otherwise you get a clear error —
   `type 'Point' does not satisfy bound 'Comparable' required by 'maxOf$Point'`. A primitive
   argument (`maxOf<i32>`) or an unknown trait name in a bound is likewise rejected.
2. **In the body, at the definition**: a bounded parameter is checked against its bounds — a value
   of `T: A + B` may be used only via the methods/operators that `A` or `B` provide. Calling a
   method not in any bound, using an operator whose trait isn't bound, or accessing a field of `T`
   is a compile error *even if the eventual concrete type happens to provide it*. For example, a
   `fn f<T: Show>(T& a) { … a.compareTo(b) … }` is rejected because `Show` declares no `compareTo`.

**Unbounded** type parameters (`<T>` with no trait) are **not** body-checked — they stay
permissive (duck-typed at instantiation), so `fn addT<T>(T a, T b) -> T { return a + b; }` and
`maxT` keep working. Add a bound to opt into definition-time checking. Dispatch remains static —
bounds add no runtime cost.

### Other constraints
- Recursive instantiation (e.g. `Node<Node<T>>`) is supported.
- There are no `where` clauses, associated types, or trait objects (`dyn`).

---

## 12. Imports & extern

### Importing another GG file
```gg
import "std/String.gg";       // the standard library — resolves anywhere
import "helpers/util.gg";     // relative to THIS file's directory
import "../other_module.gg";  // relative paths still work
```
An import path resolves in this order (first match wins):

1. **`std/` prefix** — the reserved name of the **standard library**. `import "std/String.gg"`
   always finds the compiler's stdlib regardless of where the importing file lives on disk, so you
   never write a fragile `../../stdlib/...` chain. The stdlib location is found automatically
   (relative to the `GG` compiler executable); override it with the `GG_STDLIB` environment
   variable.
2. **File-relative** — resolved against the **importing file's** directory (the original behavior;
   every existing relative import keeps working, and a local file wins over a search root).
3. **Search roots** — any directories listed in the `GG_PATH` environment variable (`;`-separated),
   tried in order.

Imported declarations (functions, classes, generic templates) are merged into the program and
become available throughout. Imports are **not** re-exported — if `a.gg` imports `b.gg` and `c.gg`
imports `a.gg`, `c.gg` does not automatically see `b.gg`'s declarations. There is no project manifest
or dependency-versioning system yet.

### Modules & namespaces

By default every declaration lives in one flat global namespace, so two files that each declare
`class Foo` collide. Opt into a **namespace** by declaring a module at the top of a file:

```gg
// file geo.gg
module geo;                       // this file's top-level names live under `geo`
class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { this.x = a; this.y = b; } }
fn origin() -> i32 { return 0; }
```

Reference a module's symbols the **Java way** — load the file, then either import the name to use it
bare, or write it fully-qualified:

```gg
// file app.gg
import "geo.gg";          // load the file (as usual)
import geo.Point;         // bring `Point` into scope unqualified (note: dotted name, no quotes)

fn main() -> i32 {
    Point p(1, 2);        // bare — resolves to geo.Point via the import
    geo.Point q(3, 4);    // fully-qualified — always works once the file is loaded
    return geo.origin();  // qualified free-function call
}
```

- A file with **no** `module` declaration is in the global namespace (unchanged behaviour); all
  existing code keeps working.
- A **fully-qualified** name (`geo.Point`) works anywhere once the defining file is loaded — the
  `import geo.Point;` is only a convenience that lets you drop the prefix.
- If two imported modules both export `Point`, the **bare** name is ambiguous and is a compile
  error — write `geo.Point` / `phys.Point` to disambiguate (Java-style; there is no import alias).
- Two modules may declare the same type name freely: `geo.Point` and `phys.Point` are distinct.
- `import geo.Point;` (a *symbol* import, dotted) is separate from `import "geo.gg";` (a *file* load,
  a string path). A symbol import does **not** load the file — the file must still be `import "…"`-ed
  somewhere in the build.

A class field, method, or local **may** share a name with a module-level function (`fn size()` and a
field `size` coexist fine). The rare exceptions that are rejected — rename one to resolve: a field or
enum variant named identically to a **type** in the same module, and a local named identically to a
top-level **function** when used in a `<` comparison.

**v1 limitations:** module names are single-segment (`module geo;`, not dotted `module a.b;`); there
is no `import geo.*;` wildcard, no import aliasing, and no module-private visibility (a module only
namespaces — all its symbols remain accessible). Don't name a local variable the same as a module.

### Calling C functions (`extern`)
```gg
extern puts(ptr s) -> i32;
extern malloc(u64 size) -> ptr;
extern free(ptr p);
extern sqrt(f64 x) -> f64;
```
`extern` declares a C ABI function without a body. The symbol must be provided at link
time (the Clang step links against libc/libm automatically). `ptr` is used for any
C `void*` parameter or return value.

Standard library modules in `stdlib/` wrap the most commonly needed C functions:

| File              | Contents |
|-------------------|----------|
| `stdlib/io.gg`    | `puts`, `putchar`, `getchar`, `fflush` |
| `stdlib/mem.gg`   | `malloc`, `realloc`, `free`, `memcpy`, `memset`, `memmove` |
| `stdlib/math.gg`  | `sin`, `cos`, `sqrt`, `pow`, `log`, `floor`, `ceil`, … (all `f64`) |
| `stdlib/process.gg` | `exit` |

---

## 13. Traits & operator overloading

GG has no inheritance, so shared contracts across types are expressed with **traits**
(similar to Rust). A trait is a set of method signatures; a type opts in with a separate
`impl Trait for Type { … }` block. Dispatch is **static** — there are no vtables and no
trait objects. Traits can also bound generic type parameters (`<T: Trait>`, see §11).

### Declaring a trait
```gg
trait Describe {
    fn size() -> i32;            // required method — signature only, ends with ';'
    fn merge(Self& other) -> Self&;
}
```
- A trait body contains **method signatures only** (no fields, no constructors).
- `Self` is a type keyword meaning "the implementing type"; it may appear in parameter and
  return positions (including as `Self&`).
- A trailing `mut` marks a self-mutating method, exactly as on a class method
  (`void reset() mut;`).
- **Default (bodied) trait methods are not supported yet** — every trait method must be a
  bare signature ending in `;`. Giving a trait method a `{ … }` body is a compile error.

### Implementing a trait
```gg
class Acc {
    mut i32 n;
    Acc(i32 x) { n = x; }
    fn get() -> i32 { return n; }
}

impl Describe for Acc {
    fn size() -> i32 { return n; }
    fn merge(Acc& other) -> Acc& { return new Acc(n + other.n); }   // Self → Acc
}
```
- An impl's methods simply **become methods on the target type** (mangled `@Acc_size`,
  `@Acc_merge`, …). They may use implicit `this`, call other methods, and read/write fields
  under the usual `mut` rules.
- The target of an `impl` must be a **class** — not a primitive and not an enum.
- The compiler checks conformance: every required method must be provided with a matching
  signature (after `Self` substitution). A missing or mismatched method is an error.
- Impl methods participate in the normal overload machinery — you can overload alongside them.

### Implementing a trait for a generic type

To implement a trait for a generic class, declare the impl's type parameters right after `impl`,
then use them in the target and the method bodies:

```gg
class Buffer<T> {
    ptr<T> data;
    Buffer(u64 cap) { data = malloc(cap * 8); }
}

impl<T> Index for Buffer<T> {          // note the <T> right after `impl`
    fn get(u64 i) -> T       { return data[i]; }
    fn set(u64 i, T v) -> void { data[i] = v; }
}

fn main() -> i32 {
    Buffer<i32>& b = new Buffer<i32>(8);
    b[0] = 7;              // Index::set — T is i32 here
    return b[0];           // Index::get
}
```

- The header form is **`impl<T…> Trait for Class<T…>`**. Each target type argument must be one of
  the impl's own type parameters (e.g. `impl<K, V> Trait for Pair<K, V>`); nested or concrete
  arguments in the target (`impl<T> Trait for Box<Foo<T>>`) are not supported.
- A generic impl is **monomorphized together with the class**: when `Buffer<i32>` is instantiated,
  its `impl<T> Index` is instantiated too (with `T = i32`), producing an ordinary impl on
  `Buffer<i32>`. You never write the impl per concrete type.
- **Forgetting the header is a common mistake:** `impl Index for Buffer<T>` (no `impl<T>`) leaves
  `T` undeclared and errors with a note pointing you to the `impl<T>` form.
- A concrete impl on a single instantiation still works too: `impl Index for Buffer<i32> { … }`.

**Specialization — a concrete impl overrides the blanket one.** A blanket `impl<T> Trait for Box<T>`
and a concrete `impl Trait for Box<i32>` may coexist; the most specific one wins:

```gg
trait Tag { fn tag() -> i32; }
class Box<T> { mut T v; Box(T x) { this.v = x; } }

impl<T> Tag for Box<T>   { fn tag() -> i32 { return 0; } }   // blanket
impl    Tag for Box<i32> { fn tag() -> i32 { return 1; } }   // concrete — used for Box<i32>

// Box<i32> uses the concrete impl (tag() == 1); Box<f64>, Box<Point>, … use the blanket (== 0).
```

The concrete impl is used for exactly that instantiation; the blanket covers all others. Declaration
order doesn't matter. Because GG only has two tiers (a fully-concrete impl vs. a fully-generic one —
there's no partial specialization like `impl<T> for Box<Vec<T>>`), there's never any ambiguity about
which is "more specific."

### Operator overloading

Operators desugar to **named trait methods**. An operator is only overloaded when the
left/receiver operand's class `impl`s the corresponding built-in trait; otherwise the usual
"operands must be numeric" rules apply. The built-in operator traits need **no declaration** —
they are recognised by name.

| Operator(s)            | Trait  | Method to implement          | Result type            |
|------------------------|--------|------------------------------|------------------------|
| `+`                    | `Add`  | `T add(T& rhs)`              | the method's return    |
| `-` (binary)           | `Sub`  | `T sub(T& rhs)`              | the method's return    |
| `*`                    | `Mul`  | `T mul(T& rhs)`              | the method's return    |
| `/`                    | `Div`  | `T div(T& rhs)`              | the method's return    |
| `%`                    | `Rem`  | `T rem(T& rhs)`              | the method's return    |
| `==`, `!=`             | `Eq`   | `bool eq(T& rhs)`            | `bool`                 |
| `<`, `<=`, `>`, `>=`   | `Ord`  | `i32 cmp(T& rhs)`            | `bool`                 |
| `-` (unary)            | `Neg`  | `T neg()`                    | the method's return    |
| `a[i]`                 | `Index`| `E get(I i)` — **or `E& get(I i)`** to return by reference | `E` (or `E&`) |
| `a[i] = v`             | `Index`| `void set(I i, E v)`         | —                      |

- `a == b` calls `a.eq(b)`; `a != b` calls `a.eq(b)` then negates it.
- **Classes have a default `==`/`!=` (no `Eq` impl required); implementing `Eq` overrides it:**
  - **Two references** (`Class&`) compare by **address identity** — same object ⇒ equal, like enums.
  - A **value object** (or a value/reference mix) compares by **memberwise structural equality**:
    fields are compared pairwise — primitives by value, **reference fields by address**, and an
    embedded value-object field by its own `Eq` if it implements one, else structurally. (A value
    has no address identity, so memberwise is the natural default.)
- `a < b` calls `a.cmp(b)` and compares the `i32` result against `0` (`< 0`, `<= 0`, `> 0`,
  `>= 0` for `<`, `<=`, `>`, `>=`).
- Unary `-a` calls `a.neg()`; `a[i]` calls `a.get(i)`; `a[i] = v` calls `a.set(i, v)`.
- **`get` can return a reference, avoiding a copy.** `a[i]` is typed as exactly what `get` returns,
  so declare `fn get(I i) -> E&` (with `E` a class) and `a[i]` yields a *reference* to the element —
  `mut Point& p = a[i]; p.x = 9;` mutates the stored object in place, no copy. Returning `E` (a
  value) copies. For a generic container this falls out of the element type: `Array<Point&>` with
  `fn get(I i) -> T` already returns a `Point&`; `Array<Point>` returns a `Point` by value.
  (A reference is only meaningful for class/reference element types — you can't return a reference
  to a primitive like `i32`, nor a safe reference into an inline value-object element, since GG
  references are refcounted; see the memory model, §10.)

**Operators return objects by value, and take operands by value — with no heap allocation.**
An operator method may return an object *by value* using a return alias (see §6): the result is
written directly into the caller's storage (a hidden result slot), so `Vec2 c = a + b;` involves
no `new` and no heap. And because a value object is **borrowed as a reference** when passed as an
argument (see [Passing objects](#passing-objects)), the operands may themselves be plain stack
values — the receiver and the `Self&` parameter both receive the object's address. The whole
expression stays on the stack:

```gg
class Vec2 {
    mut i32 x; mut i32 y;
    Vec2(i32 a, i32 b) { x = a; y = b; }
    fn sum() -> i32 { return x + y; }
}
impl Add   for Vec2 { fn add(Vec2& r) -> Vec2 out { out.x = x + r.x; out.y = y + r.y; return out; } }
impl Eq    for Vec2 { fn eq(Vec2& r) -> bool  { return x == r.x && y == r.y; } }
impl Ord   for Vec2 { fn cmp(Vec2& r) -> i32 { return sum() - r.sum(); } }
impl Neg   for Vec2 { fn neg() -> Vec2 out     { out.x = 0 - x; out.y = 0 - y; return out; } }
impl Index for Vec2 {
    fn get(i32 i) -> i32          { if (i == 0) { return x; } return y; }
    fn set(i32 i, i32 v) mut { if (i == 0) { x = v; } else { y = v; } }
}

fn main() -> i32 {
    Vec2 a(1, 2);
    Vec2 b(3, 4);
    Vec2 c = a + b;           // (4, 6) — stack operands, stack result, no heap
    bool lt = a < b;          // true  (3 < 7)
    Vec2 n = -a;              // (-1, -2)
    return c[0] + c[1];       // Index get → 10
}
```

An operator may still return a heap reference (`-> Vec2&` with `return new Vec2(...)`) if you
want a heap-allocated result; the value-return form above is simply the allocation-free default.

### The `Clone` trait — a custom deep-copy (copy constructor)

By default, copying a value object is **memberwise**: primitive fields are copied, reference
(`Class&`) fields are retained, and embedded value-object fields are cloned recursively. That is
correct for every ordinary class. But a class that owns a **raw `ptr` buffer** (a hand-written
container like `String` or `Array<T>`) can't be copied memberwise — the raw pointer would be
duplicated, so two objects would share (and both free) one allocation. For those, implement the
built-in **`Clone` trait** to define a proper deep copy:

```gg
impl Clone for String {
    fn clone(String& src) mut {          // `this` = destination, `src` = source
        free(this.data);                 // release this's current buffer (a no-op if empty)
        this.size = src.size;
        this.data = malloc(this.size + 1);
        memcpy(this.data, src.data, this.size + 1);   // own a fresh, independent copy
    }
}
```

- `clone` is a `mut` method taking `Self&` (the source); it deep-copies `src` into `this`.
- It **replaces** the default memberwise copy everywhere a copy happens — a value binding
  (`String b = a;`), a return, a container element write — so `Clone` types copy deeply and safely.
- **Recursion composes**: an `Array<String>`'s clone copies each element through `String`'s clone;
  an `Array<Array<Point>>`'s clone recurses again. You only write the one hop.
- **Contract**: `clone` must release `this`'s old resources first (the `free(this.data)` above).
  This is a no-op on a fresh/empty destination, so the single body works whether the destination is
  brand-new or already holds data.

This is what lets `String` and the generic `Array<T>` be stored by value inside another `Array`
(`Array<String>`, `Array<Array<Point>>`). Without a `Clone` impl, using a buffer-owning value object
as an `Array` element is a compile error that points you here.

---

## 14. Lambdas & callable objects

Any class can be made **callable** by implementing the built-in **`Call` trait** (like C++'s
`operator()`): `obj(args)` desugars to `obj.call(args)`.

```gg
class Adder { i32 n; Adder(i32 a) { n = a; } }
impl Call for Adder { fn call(i32 x) -> i32 { return x + n; } }

Adder a(5);
i32 r = a(10);          // 15 — a(10) means a.call(10)
```

A **lambda** is an anonymous callable written `(params) -> [ReturnType] { body }`:

```gg
(i32 a, i32 b) -> i32 { return a + b; }   // typed return
(i32 x) -> { doSomething(x); }            // omitted return type ⇒ void
() -> i32 { return 42; }                  // no parameters
```

When a lambda is passed to a `Call`-bounded function (below), its parameter types **and** return
type can be **omitted** — they are inferred from the callee's `Call(…)` signature. The parentheses
are optional for a single parameter:

```gg
apply(x -> { return x + 1; }, 41);        // x and the return type inferred as i32
apply((x) -> { return x * 2; }, 21);      // same, parenthesized
apply2((a, b) -> { return a + b; }, 6, 7);// multiple untyped parameters
```

Lambdas **capture** the enclosing local variables, parameters, and instance fields they use, **by
value** (primitives copied, references retained). Capture is implicit; reference bare names (not
`this.field`):

```gg
i32 base = 100;
apply((i32 y) -> i32 { return y + base; }, 5);   // captures `base` → 105
```

### Passing callables to functions

To accept "any callable" a function is generic over a **signature-carrying `Call` bound**
`F: Call(P…) -> R`. The same function accepts a hand-written callable object **or** a lambda literal;
a callable whose signature doesn't match is rejected where it's passed. Because object parameters are
references, the parameter is written `F&` (a lambda/callable value borrows into it):

```gg
fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }

apply<Adder>(Adder(5), 100);                     // callable object → 105
apply((i32 y) -> i32 { return y + 1; }, 41);     // lambda literal   → 42
apply((f64 y) -> f64 { return y; }, 5);          // ERROR: (f64)->f64 does not satisfy Call(i32)->i32
```

### Limitations (this version)

- A lambda is a **non-escaping stack value object** — it can be passed *into* a call but not
  returned, stored in a field, or kept in a container. For escaping behavior, write a callable
  object and manage it as a heap reference.
- A lambda is usable **only as a literal argument** to a generic function with **exactly one
  `Call`-bounded type parameter** (its type is anonymous and unspellable, so it is inferred there).
- **Nested lambdas** are not supported; a lambda captures **bare** field names (not `this.field`)
  and cannot capture a `static` field or use `this` as a whole value.

---

## 15. What GG does NOT support

The following features are **currently absent** from the current implementation. They may be added in the future.
Attempting them will produce a compile error (or will simply not parse).

### Types & values
| Missing feature | Notes |
|-----------------|-------|
| Built-in string type | Strings are C `ptr`; use `extern puts` and pass string literals directly |
| Union / sum types | No tagged unions (enums are Java-style singletons, not sum types — see §9) |
| Tuples | No tuple syntax or destructuring |
| Nullable value objects | `T?` covers references, borrows, enums, and primitives; `Point?` (a value object) is rejected — use `Point&?` |

### Functions & methods
| Missing feature | Notes |
|-----------------|-------|
| Variadic functions | No `...` — use `extern` to call C variadics |
| Escaping / stored lambdas | Lambdas (§14) exist but are non-escaping value objects usable only as a literal argument to a `Call`-bounded generic; no `dyn Call` for heterogeneous storage |
| Multiple return values | Return a class instance instead |

### Classes
| Missing feature | Notes |
|-----------------|-------|
| Inheritance / subclassing | No `extends` or base classes |
| Virtual methods / dynamic dispatch | Traits (§13) are statically dispatched; no vtable, no trait objects (`dyn`) |
| `const`-qualified *types* | There is no `const T` type qualifier; immutability is a property of the *binding* (`mut` opts out), not the type. See §2/§8 for const-by-default. |
| File-scope / internal linkage for statics | Static fields keep external linkage; no `private`-style linkage control |
| Access modifiers beyond `private` | No `public` keyword, no `protected`; `private` is advisory (warning only) |
| Explicit `this` parameter | `this` is implicit; cannot be renamed or captured |
| Copy constructors | Deep copy is automatic via the generated `@ClassName_clone` helper |

### Control flow
| Missing feature | Notes |
|-----------------|-------|
| `match` (pattern matching / binding) | `switch` (§5) exists, but only value/identity labels — no destructuring patterns |
| `do-while` loops | Negate condition and use `while` |
| `goto` | Not present |
| Exception handling (`try`/`catch`/`throw`) | No exception model; errors are return values |

### Memory
| Missing feature | Notes |
|-----------------|-------|
| Garbage collection | Refcounting only; no cycle detection |
| Cycle handling | Circular references (`A& → B&, B& → A&`) will **leak** |
| Pointer arithmetic | `ptr` is opaque; use `ptr<T>` + `[]` for offset access |
| Bounds checking on `ptr<T>` | Only fixed-size `T[N]` arrays are bounds-checked |
| Weak references | `T*` is a non-owning **borrow** (no liveness tracking); a liveness-checked `weak` reference does not yet exist |

### Other
| Missing feature | Notes |
|-----------------|-------|
| Type inference (return / params) | `var` infers **local** variable types; parameter, field, and return types are always written explicitly |
| Runtime reflection (`typeof`, downcast) | No runtime type information — reflection is **compile-time only** (see below) |
| Compile-time evaluation (`constexpr`) | `sizeof(T)` is the only compile-time computation |
| Preprocessor / macros | None — generics handle the primary use case |
| Modules / namespaces | No namespacing; all declarations share a flat global scope |
| String interpolation | No template strings |
| Assertions | Use `if (cond) { exit(1); }` or call `abort()` via `extern` |

## 16. Debugging (`--debug` / `-g`)

GG can emit DWARF debug information so a compiled program is debuggable with standard C/C++
debuggers (**gdb** / **lldb**) — set breakpoints on `.gg` lines, single-step, inspect a backtrace,
and print local variables and struct fields.

```powershell
.\compile.ps1 e2e\class_test.gg -DebugInfo    # GG --debug + clang -g
```

`-DebugInfo` passes `--debug` to the GG compiler (emit LLVM debug metadata) and `-g` to clang (keep
DWARF in the executable). Invoking `GG.exe` directly, the flag is `--debug` (or `-g`).
(The switch is `-DebugInfo`, not `-Debug`: `compile.ps1` is an advanced script, so PowerShell
reserves `-Debug` as a common parameter.)

What you get:
- **Line-level** — a breakpoint on a source line (`break class_test.gg:180`), stepping, and
  backtraces mapped back to `.gg` lines.
- **Variables & types** — `print p`, `print p.x` for locals and parameters; value objects appear as
  structs with named fields (correct byte offsets), primitives with their natural type. References,
  enums, and `ptr` show as pointers (addresses).

Notes / limitations:
- **Off by default.** Without `-DebugInfo`/`--debug` the emitted IR is identical to a release build —
  no runtime or size cost.
- **Single source file.** The debug info uses one compile-unit file (the main source). In a
  multi-file (imported) build, line numbers are attributed against that file — per-file line
  mapping is not yet implemented.
- Debug info is descriptive metadata only; it never changes the generated code.
