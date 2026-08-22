# The OptiForge Language

> **Status:** complete for the Phase 0–13 feature set. Anything marked *not
> supported* is deliberately out of scope (see `context/requirement.md` §14).
> Source files use the `.of` extension.

---

## 1. Lexical Structure

### Comments

```
// line comment, runs to end of line
/* block comment, may span lines */
```

Block comments **do not nest**: the first `*/` closes the comment.

### Identifiers

`[A-Za-z_][A-Za-z0-9_]*`. Case-sensitive.

### Keywords

Reserved, and never usable as identifiers:

```
fn  int  float  bool  void  if  else  while  return  true  false
```

### Literals

| Kind | Examples | Notes |
|---|---|---|
| Integer | `0` `42` `9223372036854775807` | Decimal only. Must fit in a signed 64-bit `int`. |
| Float | `3.14` `0.5` `1e3` `2.5e-2` `1E+2` | A digit is required on both sides of `.`; an exponent needs at least one digit. |
| Boolean | `true` `false` | |

Rejected, each with a specific diagnostic: `1.2.3`, `123abc`, `0x10`, `1e`,
`1e+`, `1.`, and any integer too large for `int`.

### Operators and punctuation

```
+  -  *  /  %        arithmetic
!                    logical negation
== != <  >  <= >=    comparison
&& ||                logical
=                    assignment (a statement, not an expression)
( ) { } , ; ->       punctuation
```

Lexing is maximal-munch: `<=` is one token, never `<` then `=`. A lone `&` or
`|` is an error — the language has no bitwise operators.

Strings are not supported.

---

## 2. Grammar (EBNF)

```ebnf
program        = { function_decl } ;
function_decl  = "fn" IDENT "(" [ param_list ] ")" [ "->" type ] block ;
param_list     = param { "," param } ;
param          = type IDENT ;
type           = "int" | "float" | "bool" | "void" ;

block          = "{" { statement } "}" ;
statement      = var_decl | assign_stmt | if_stmt | while_stmt | for_stmt
               | break_stmt | continue_stmt | return_stmt | expr_stmt | block ;

var_decl       = type IDENT [ "[" INT_LIT "]" ] [ "=" expression ] ";" ;
assign_stmt    = IDENT [ "[" expression "]" ] "=" expression ";" ;
if_stmt        = "if" "(" expression ")" block [ "else" ( block | if_stmt ) ] ;
while_stmt     = "while" "(" expression ")" block ;
for_stmt       = "for" "(" [ for_init ] ";" [ expression ] ";" [ for_step ] ")" block ;
for_init       = var_decl_nosemi | assign_nosemi ;
for_step       = assign_nosemi ;
break_stmt     = "break" ";" ;
continue_stmt  = "continue" ";" ;
return_stmt    = "return" [ expression ] ";" ;
expr_stmt      = expression ";" ;

expression     = logical_or ;
logical_or     = logical_and { "||" logical_and } ;
logical_and    = equality   { "&&" equality } ;
equality       = relational { ( "==" | "!=" ) relational } ;
relational     = additive   { ( "<" | ">" | "<=" | ">=" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" | "%" ) unary } ;
unary          = [ "-" | "!" ] unary | primary ;
primary        = INT_LIT | FLOAT_LIT | "true" | "false"
               | IDENT | IDENT "[" expression "]"
               | IDENT "(" [ arg_list ] ")" | "(" expression ")" ;
arg_list       = expression { "," expression } ;
```

### Precedence

Loosest to tightest; **all binary operators are left-associative**.

| Level | Operators |
|---|---|
| 1 | `\|\|` |
| 2 | `&&` |
| 3 | `==` `!=` |
| 4 | `<` `>` `<=` `>=` |
| 5 | `+` `-` |
| 6 | `*` `/` `%` |
| 7 | unary `-` `!` |
| 8 | `a[i]` indexing, `f(...)` call |

### Nesting limit

Expressions and blocks may nest to depth 1000. Beyond that the compiler
reports an error rather than exhausting its stack.

---

## 3. Types

| Type | Representation | Size |
|---|---|---|
| `int` | signed two's-complement | 8 bytes |
| `float` | IEEE-754 double | 8 bytes |
| `bool` | `true` / `false` | 1 byte |
| `void` | no value; function return type only | 0 |
| `T[N]` | `N` consecutive elements of `T`, local only (§9) | 8 × N |

`int` is **64-bit**. Variables and parameters may not have type `void`, and
parameters may not have an array type.

### Conversions

The **only** implicit conversion is `int` → `float`. It applies to
initialization, assignment, argument passing, and return values.

| From → To | Implicit? |
|---|---|
| `int` → `float` | **yes** |
| `float` → `int` | no |
| `int` → `bool` | **no** — deliberate |
| `bool` → `int` | no |
| anything → `void` | no |

There are no explicit casts. `if (x)` where `x` is an `int` is an error; write
`if (x != 0)`. This keeps conditions unambiguous and removes a whole class of
C bugs.

### Operator typing

| Operator | Operands | Result |
|---|---|---|
| `+ - * /` | both numeric | `float` if either is `float`, else `int` |
| `%` | both `int` | `int` |
| `< > <= >=` | both numeric | `bool` |
| `== !=` | both numeric, **or** both `bool` | `bool` |
| `&& \|\|` | both `bool` | `bool` |
| unary `-` | numeric | same as operand |
| unary `!` | `bool` | `bool` |

`&&` and `||` short-circuit.

---

## 4. Declarations and Scope

Variables must be declared before use. A declaration's initializer is analyzed
*before* the name enters scope, so `int x = x;` is an error rather than a
self-reference.

Scopes nest by block. A function body shares the scope of its parameters, so a
local with a parameter's name is a **redeclaration error**, not shadowing.
Shadowing a variable from an enclosing block is legal but **warns**.

```of
fn f(int a) -> int {
    int a = 1;       // error: redeclaration of 'a'
    {
        int b = 2;
        { int b = 3; }   // warning: shadows an outer declaration
    }
    return a;
}
```

---

## 5. Functions

```of
fn add(int a, int b) -> int {
    return a + b;
}

fn greet() {          // no "->" means the return type is void
    print_int(1);
}
```

- Functions may be declared in any order; forward references and mutual
  recursion work.
- Every function name must be unique across the program.
- A non-`void` function must return a value on **every** path. An `if` without
  an `else` never satisfies this, and neither does a loop — the compiler does
  not attempt to prove `while (true)` runs forever.
- A `void` function may `return;` or fall off the end, but may not return a
  value.

### Entry point

Every program must define exactly:

```of
fn main() -> int { ... }
```

### Built-in functions

Provided by the runtime; no declaration or import needed.

| Signature | Effect |
|---|---|
| `print_int(int) -> void` | prints an integer and a newline |
| `print_float(float) -> void` | prints a float and a newline |
| `print_bool(bool) -> void` | prints `true` or `false` and a newline |

Redefining one is an error.

---

## 6. Statements

```of
int x = 10;          // declaration, initializer optional
x = x + 1;           // assignment (statement, not an expression)
f(x);                // expression statement
if (x > 0) { } else if (x < 0) { } else { }
while (x < 100) { x = x + 1; }
return x;            // or bare `return;` in a void function
{ /* nested block */ }
```

Conditions must have type `bool` exactly. Braces are **required** on every
`if`, `else`, and `while` body — there is no single-statement form, so the
dangling-else problem does not arise.

---

## 7. Diagnostics Catalogue

Every message below has at least one test that triggers it (requirement QA-03).

### Lexical

- `unexpected character '<c>'`
- `expected '&&'; a single '&' is not an operator in this language`
- `expected '||'; a single '|' is not an operator in this language`
- `string literals are not supported`
- `unterminated block comment`
- `invalid number literal '<text>'`
- `expected a digit after '.' in floating-point literal`
- `exponent has no digits`
- `integer literal '<text>' is too large for type 'int'`

### Syntax

- `expected <token> <context>`
- `expected an expression`
- `expected a type name ('int', 'float', 'bool', or 'void')`
- `expected 'fn' to begin a function declaration`
- `input nests too deeply (limit 1000)`

### Semantic

- `use of undeclared variable '<name>'`
- `use of undeclared function '<name>'`
- `'<name>' is not a function`
- `function '<name>' cannot be used as a value; did you mean to call it?`
- `redeclaration of '<name>'` *(+ note: previous declaration is here)*
- `redefinition of function '<name>'` *(+ note: previous definition is here)*
- `redefinition of built-in function '<name>'`
- `variable '<name>' cannot have type 'void'`
- `parameter '<name>' cannot have type 'void'`
- `cannot initialize a variable of type 'X' with a value of type 'Y'`
- `cannot assign a value of type 'X' to variable '<name>' of type 'Y'`
- `cannot assign to function '<name>'`
- `invalid operands to binary operator '<op>' ('X' and 'Y')`
- `operator '%' requires integer operands, ...`
- `operator '-' requires a numeric operand, ...`
- `operator '!' requires a 'bool' operand, ...`
- `condition of '<if|while>' must have type 'bool', but has type 'X'`
  *(+ note suggesting `!= 0`)*
- `function '<name>' expects N arguments, but M were provided`
- `cannot pass a value of type 'X' as parameter '<name>' of type 'Y'`
- `cannot return a value of type 'X' from a function returning 'Y'`
- `non-void function '<name>' must return a value of type 'X'`
- `void function '<name>' cannot return a value`
- `not all control paths in function '<name>' return a value`
- `program has no 'main' function`
- `'main' must take no parameters and return 'int'`

### Warnings

- `declaration of '<name>' shadows an outer declaration`
  *(+ note: previous declaration is here)*

Pass `-Werror` to turn warnings into errors; the diagnostic is then printed as
an error and the compiler exits non-zero.

---

## 8. Not Supported

Strings, structs, pointers, dynamic allocation, explicit casts, bitwise
operators, compound assignment (`+=`), increment/decrement (`++`), the ternary
operator, global variables, multiple source files, and separate compilation.

Arrays, `for`, `break` and `continue` all arrived in Phase 13; see §9 and §10.

---

## 9. Arrays

Added in Phase 13. Fixed-length, local, one-dimensional.

```of
int  counts[16];       // sixteen ints, uninitialized
bool seen[100];
float xs[8];

counts[0] = 1;
counts[i + 1] = counts[i] * 2;
int total = counts[3];
```

### The rules

| Rule | Why |
|---|---|
| The length is an **integer literal**, not an expression | There are no named constants to fold, so a constant expression would have nothing to be for |
| The length must be **positive** | A zero-length array has no valid index |
| **No initializer**: `int a[3] = 0;` is an error | There is no aggregate initializer syntax, so the meaning would have to be invented |
| Elements are **not zero-initialized** | Unlike a scalar, which is. See below. |
| The index must be `int` | `a[true]` is an error, as `if (x)` is |
| An array name is **not a value** — `int b = a;`, `f(a)` and `return a;` are all errors | There are no pointers for it to decay to |
| **No whole-array assignment**: `a = b;` is an error | Aggregate copy is not implemented |
| Arrays are **local only** — no array parameters, no array returns | Follows from an array name not being a value |
| **No bounds checking** | `a[99]` on `int a[3]` reads or writes other stack memory. This is the one place the language is memory-unsafe, and it is stated rather than hidden. |

### Two consequences worth knowing

**Elements start with whatever was on the stack.** Every scalar declaration
emits a zero store, because otherwise `mem2reg` would have to invent a value
for an unwritten local and could invent a different one at different
optimization levels — which would break the differential suite silently. An
array is never promoted (its address is taken by every use), so that
divergence cannot arise, and zeroing would instead cost a loop of `length`
stores in every function that declares one. **Read an element before writing it
and the value is undefined.**

**Two dimensions are written by hand.** There are no arrays of arrays, so a
matrix is one flat block:

```of
m[row * size + col] = value;
```

`bench/programs/matmul.of` is the worked example.

### Sizing

Arrays live in the stack frame, and the stack is 1 MB. Every element occupies
**eight bytes regardless of type**, so `bool[50000]` costs 400 KB, not 50 KB —
the frame does not pack, because packing would make element addressing
disagree with the slot layout everything else uses.

A frame over 4 KB is probed a page at a time in the prologue, which the
platform requires; that is handled by the compiler and costs one call.

---

## 10. Loops and Loop Control

`while` has been there since Phase 1. `for`, `break` and `continue` arrived in
Phase 13.

```of
for (int i = 0; i < 10; i = i + 1) {
    if (i == 3) { continue; }   // runs the step, then re-tests
    if (i == 7) { break; }      // leaves the loop
    total = total + i;
}

for (; i < n;) { ... }          // any clause may be omitted
for (;;) { ... break; ... }     // an omitted condition is true
```

### The rules

| Rule | Notes |
|---|---|
| The init clause is a declaration or an assignment | `for (int i = 0; ...)` or `for (i = 0; ...)` |
| The step clause is an **assignment only** | There is no `++` or `+=` to write there instead |
| Any clause may be omitted | An omitted condition means `true` |
| The header has **its own scope** | `for (int i = ...)` does not leak `i`, and two loops in a row may both use it |
| `break` and `continue` bind to the **innermost** enclosing loop | There are no labels |
| `break` or `continue` outside any loop is an error | Caught in semantic analysis |

### `continue` in a `for` runs the step

Worth stating because it is the one place a plausible implementation is wrong.
`for` is **not** sugar for `while`. The obvious desugaring —

```of
{ init; while (cond) { body; step; } }
```

— makes `continue` jump to the condition and skip the step, so
`for (i = 0; i < n; i = i + 1) { continue; }` would never advance and would hang
rather than misbehave visibly. The step therefore gets a basic block of its own,
which is where `continue` branches to; `docs/ir.md` §7 shows the shape, and
`tests/e2e/for_break_continue.of` is the regression test.

---

## 11. Complete Example

```of
// Sums 0..n-1 and prints the result.

fn sum(int n) -> int {
    int total = 0;
    int i = 0;
    while (i < n) {
        total = total + i;
        i = i + 1;
    }
    return total;
}

fn main() -> int {
    print_int(sum(100));   // 4950
    return 0;
}
```

```bash
optiforge examples/sum.of --emit=ast
```
