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
statement      = var_decl | assign_stmt | if_stmt | while_stmt
               | return_stmt | expr_stmt | block ;

var_decl       = type IDENT [ "=" expression ] ";" ;
assign_stmt    = IDENT "=" expression ";" ;
if_stmt        = "if" "(" expression ")" block [ "else" ( block | if_stmt ) ] ;
while_stmt     = "while" "(" expression ")" block ;
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
               | IDENT | IDENT "(" [ arg_list ] ")" | "(" expression ")" ;
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

`int` is **64-bit**. Variables and parameters may not have type `void`.

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

Arrays, strings, structs, pointers, dynamic allocation, `for`, `break`,
`continue`, explicit casts, bitwise operators, compound assignment (`+=`),
increment/decrement (`++`), the ternary operator, global variables, multiple
source files, and separate compilation.

Arrays, `for`, `break`, and `continue` are Phase 13 candidates
(`context/roadmap.md`).

---

## 9. Complete Example

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
