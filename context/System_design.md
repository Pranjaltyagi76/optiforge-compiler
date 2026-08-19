# OptiForge — System Design

> **Doc version:** 1.0 · **Created:** 2026-08-18 · **Scope:** component internals — data structures, class designs, algorithms, formats, and control flow.
> For module boundaries and architectural decisions see `architectural_design.md`.

---

## Table of Contents

1. [Support Layer](#1-support-layer)
2. [Lexer Design](#2-lexer-design)
3. [Parser & AST Design](#3-parser--ast-design)
4. [Type System & Semantic Analysis](#4-type-system--semantic-analysis)
5. [IR Design](#5-ir-design)
6. [IR Generation](#6-ir-generation)
7. [Analysis Framework](#7-analysis-framework)
8. [SSA Construction & Destruction](#8-ssa-construction--destruction)
9. [Pass Infrastructure](#9-pass-infrastructure)
10. [Optimization Pass Designs](#10-optimization-pass-designs)
11. [Backend Design](#11-backend-design)
12. [Register Allocation](#12-register-allocation)
13. [Instrumentation Design](#13-instrumentation-design)
14. [Profile Runtime & Format](#14-profile-runtime--format)
15. [Hot-Path Detection](#15-hot-path-detection)
16. [PGO Pass Designs](#16-pgo-pass-designs)
17. [Driver & CLI](#17-driver--cli)
18. [Testing Design](#18-testing-design)

> Code below is **design sketch, not final API**. It fixes shape and responsibility; signatures will be refined during implementation.

---

## 1. Support Layer

### 1.1 SourceManager & SourceLocation

```cpp
struct SourceLocation {
    uint32_t fileId;
    uint32_t line;    // 1-based
    uint32_t col;     // 1-based
    static SourceLocation invalid();
    bool isValid() const;
};

struct SourceRange { SourceLocation begin, end; };

class SourceManager {
public:
    uint32_t   addFile(std::string path, std::string contents);
    StringRef  getLine(uint32_t fileId, uint32_t line) const;   // for caret rendering
    StringRef  getPath(uint32_t fileId) const;
    uint64_t   contentHash(uint32_t fileId) const;              // FNV-1a — used in .prof header
private:
    struct Entry { std::string path, contents; std::vector<uint32_t> lineStarts; uint64_t hash; };
    std::vector<Entry> files_;
};
```

`contentHash` is the mechanism behind profile staleness detection (ADR-06). It is computed once per file at load.

### 1.2 DiagnosticEngine

```cpp
enum class DiagSeverity { Note, Warning, Error, Fatal };

class DiagnosticEngine {
public:
    DiagBuilder report(SourceLocation loc, DiagSeverity sev, std::string_view fmt);
    unsigned errorCount()   const;
    unsigned warningCount() const;
    bool     hadError()     const;
    void setWarningsAsErrors(bool);
private:
    SourceManager& sm_;
    unsigned errors_ = 0, warnings_ = 0;
};
```

Rendered output format (fixed, tested by golden files):

```
examples/bad.of:7:9: error: use of undeclared variable 'y'
    y = x + 5;
    ^
examples/bad.of:12:16: error: cannot assign 'float' to variable 'n' of type 'int'
    int n = 3.14;
            ~~~~
2 errors generated.
```

### 1.3 Arena Allocator

```cpp
class Arena {
public:
    void* allocate(size_t bytes, size_t align);
    template <class T, class... Args> T* create(Args&&... args);  // trivially destructible T only
    void reset();
private:
    std::vector<std::unique_ptr<std::byte[]>> blocks_;
    std::byte* cur_ = nullptr; std::byte* end_ = nullptr;
    static constexpr size_t kBlockSize = 64 * 1024;
};
```

AST and IR nodes are arena-allocated. Nodes must be trivially destructible or explicitly registered for destruction; the design prefers the former (use arena-allocated spans and interned strings rather than `std::string`/`std::vector` members where practical).

---

## 2. Lexer Design

### 2.1 Token

```cpp
enum class TokenKind : uint8_t {
    // literals & identifiers
    Identifier, IntLiteral, FloatLiteral,
    // keywords
    KwInt, KwFloat, KwBool, KwVoid, KwFn, KwIf, KwElse, KwWhile,
    KwReturn, KwTrue, KwFalse,
    // operators
    Plus, Minus, Star, Slash, Percent, Bang,
    Assign, EqEq, NotEq, Less, Greater, LessEq, GreaterEq,
    AmpAmp, PipePipe,
    // punctuation
    LParen, RParen, LBrace, RBrace, Comma, Semicolon, Arrow,
    // control
    EndOfFile, Error
};

struct Token {
    TokenKind      kind;
    SourceLocation loc;
    StringRef      lexeme;      // interned; points into the source buffer
    union { int64_t intVal; double floatVal; };
};
```

### 2.2 Algorithm

Single-pass, single-character lookahead, no backtracking.

```
loop:
  skip whitespace and comments (// to EOL, /* */ nesting NOT supported — documented)
  record start location
  switch on first character:
    alpha or '_'  -> scan identifier, then keyword lookup in a static perfect map
    digit         -> scan number: integer digits; if '.' follows a digit, switch to float;
                     optional exponent; reject '1.2.3', '1e', trailing garbage
    operator char -> maximal munch: try the two-char operator first ('==','!=','<=','>=','&&','||','->'),
                     else the one-char form
    '"'           -> reserved for strings (out of scope) — emit Error token
    EOF           -> emit EndOfFile, stop
    otherwise     -> diagnostic "unexpected character", emit Error, advance one char, continue
```

**Error recovery:** the lexer never stops on the first bad character. It emits an `Error` token, reports, and continues — so a single run reports every lexical problem. An unterminated block comment reports at the comment's opening location and consumes to EOF.

**Keyword lookup:** a `constexpr` sorted array with binary search, or a small hand-rolled hash. Avoids a per-token `std::map` allocation.

---

## 3. Parser & AST Design

### 3.1 AST Hierarchy

```cpp
class Node {
public:
    enum class Kind : uint8_t {
        // Declarations
        Program, FunctionDecl, ParamDecl, VarDecl,
        // Statements
        Block, ExprStmt, AssignStmt, IfStmt, WhileStmt, ReturnStmt,
        // Expressions
        BinaryExpr, UnaryExpr, CallExpr, VarRefExpr,
        IntLiteral, FloatLiteral, BoolLiteral,
    };
    Kind kind() const;
    SourceRange range() const;
protected:
    Kind kind_; SourceRange range_;
};

class Expr : public Node {
public:
    Type* type() const;                 // null until semantic analysis
    void  setType(Type*);
private:
    Type* type_ = nullptr;
};

class BinaryExpr final : public Expr {
    BinaryOp op_;                       // Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Gt, Le, Ge, And, Or
    Expr *lhs_, *rhs_;                  // arena pointers
};

class FunctionDecl final : public Node {
    StringRef            name_;
    std::span<ParamDecl*> params_;
    Type*                returnType_;
    Block*               body_;
    FunctionSymbol*      symbol_ = nullptr;   // filled by sema
};
```

Traversal uses a `Visitor` with `visit(Node*)` dispatching on `kind()`. A CRTP `RecursiveASTVisitor` provides default traversal so each consumer overrides only the nodes it cares about.

### 3.2 Grammar (EBNF, authoritative copy lives in `docs/language.md`)

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
equality       = relational  { ( "==" | "!=" ) relational } ;
relational     = additive    { ( "<" | ">" | "<=" | ">=" ) additive } ;
additive       = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative = unary { ( "*" | "/" | "%" ) unary } ;
unary          = [ "-" | "!" ] unary | primary ;
primary        = INT_LIT | FLOAT_LIT | "true" | "false"
               | IDENT | IDENT "(" [ arg_list ] ")" | "(" expression ")" ;
arg_list       = expression { "," expression } ;
```

### 3.3 Parser Strategy

Recursive descent, one token of lookahead, with **precedence climbing** for the binary-expression cascade so the eight precedence levels above collapse into one function:

```cpp
Expr* Parser::parseBinaryExpr(int minPrec) {
    Expr* lhs = parseUnaryExpr();
    while (true) {
        int prec = precedenceOf(peek().kind);
        if (prec < minPrec) break;
        Token op = advance();
        Expr* rhs = parseBinaryExpr(prec + 1);   // +1 = left associative
        lhs = arena.create<BinaryExpr>(toBinaryOp(op.kind), lhs, rhs, spanOf(lhs, rhs));
    }
    return lhs;
}
```

**Error recovery — panic mode.** On an unexpected token: report once, then `synchronize()` — discard tokens until a `;`, a `}`, or a token that can begin a statement. A `panicMode_` flag suppresses cascading diagnostics until one token is successfully consumed, preventing a single typo from producing twenty errors.

---

## 4. Type System & Semantic Analysis

### 4.1 Types

```cpp
class Type {
public:
    enum class Kind { Int, Float, Bool, Void };
    Kind kind() const;
    unsigned sizeInBytes() const;      // Int=8, Float=8, Bool=1, Void=0
    bool isNumeric() const;
    const char* name() const;
    // Canonical singletons — types are compared by pointer identity
    static Type* getInt(); static Type* getFloat();
    static Type* getBool(); static Type* getVoid();
};
```

Types are interned singletons, so type equality is a pointer comparison. This stays true when arrays are added later (an `ArrayType` cache keyed by element type and length).

### 4.2 Conversion Rules (decided — LANG/FE-29)

| From → To | Implicit? | Notes |
|---|---|---|
| `int` → `float` | **yes** | Inserts an `SIToFP` IR instruction |
| `float` → `int` | no | Error: "cannot implicitly convert 'float' to 'int'" |
| `bool` → `int` | no | Error — keeps conditions unambiguous |
| `int` → `bool` | no | **Deliberate.** `if (x)` is an error; `if (x != 0)` is required |
| anything → `void` | no | Error |

Binary arithmetic: if either operand is `float`, the other is promoted, and the result is `float`. `%` requires both operands `int`. Comparisons yield `bool`. `&&`/`||` require `bool` operands and yield `bool`.

### 4.3 Symbol Table

```cpp
struct Symbol {
    enum class Kind { Variable, Parameter, Function };
    Kind kind; StringRef name; Type* type; SourceLocation declLoc;
};

struct FunctionSymbol : Symbol {
    std::vector<Type*> paramTypes;
    Type* returnType;
    bool  defined = false;
};

class Scope {
    Scope* parent_;
    llvm_like::StringMap<Symbol*> symbols_;   // insertion-ordered for determinism
public:
    Symbol* lookupLocal(StringRef) const;     // this scope only — duplicate detection
    Symbol* lookup(StringRef) const;          // walks up the parent chain
    bool    insert(Symbol*);                  // false if already present locally
};
```

Shadowing: an inner scope **may** shadow an outer variable; a warning is emitted (`-Wshadow`, on by default). Redeclaration **in the same scope** is an error.

### 4.4 Semantic Analysis Algorithm

Two passes over the program, which is what makes mutual recursion work (FE-27):

```
Pass 1 — Collect signatures:
    for each FunctionDecl:
        build FunctionSymbol (name, param types, return type)
        error if the name is already declared
        insert into global scope
    verify a 'main' exists with the required signature

Pass 2 — Check bodies:
    for each FunctionDecl:
        push function scope; insert parameters
        currentFunction_ = this function
        visit body:
            VarDecl      : check initializer type, insert symbol (duplicate check first)
            AssignStmt   : resolve target, check assignability
            BinaryExpr   : check operand types, apply promotion, set result type
            UnaryExpr    : '-' requires numeric; '!' requires bool
            CallExpr     : resolve callee, check arity then argument types pairwise,
                           set result type = callee return type
            VarRefExpr   : resolve or error "use of undeclared variable"
            IfStmt/While : condition must be exactly 'bool'
            ReturnStmt   : type must match currentFunction_->returnType
                           (missing value in non-void = error, value in void = error)
        run returnsOnAllPaths() for non-void functions (FE/LANG-47)
        pop scope
```

`returnsOnAllPaths()` is a small structural recursion: a `Block` returns on all paths if any statement does; an `IfStmt` does only if it has an `else` and **both** branches do; a `WhileStmt` never guarantees a return (the condition may be false initially); a `ReturnStmt` does.

---

## 5. IR Design

### 5.1 Value Model

```cpp
class Value {
public:
    enum class VKind { Constant, Argument, Instruction, BasicBlock, Function, Undef };
    VKind     valueKind() const;
    Type*     type() const;
    StringRef name() const;              // for printing; deterministic (%t0, %t1, ...)
    // Use list — the backbone of DCE, CSE, and copy propagation
    iterator_range<Use*> uses();
    unsigned  useCount() const;
    void      replaceAllUsesWith(Value* newVal);
};

struct Use {           // intrusive doubly-linked node
    Value*       value;    // the used value
    Instruction* user;     // the instruction using it
    unsigned     operandNo;
    Use *prev, *next;      // links in value->uses()
};
```

`replaceAllUsesWith` is the single most-used primitive in the optimizer — constant folding, CSE, and copy propagation are all "compute a better value, then RAUW."

### 5.2 Instruction Set

Defined via an X-macro list (`ir/Instruction.def`) so printer, parser, verifier, and instruction selector stay in sync:

```
// OPCODE(Name, Mnemonic, NumOperands, HasResult, IsTerminator)
OPCODE(Add,     "add",    2, true,  false)
OPCODE(Sub,     "sub",    2, true,  false)
OPCODE(Mul,     "mul",    2, true,  false)
OPCODE(SDiv,    "sdiv",   2, true,  false)
OPCODE(SRem,    "srem",   2, true,  false)
OPCODE(FAdd,    "fadd",   2, true,  false)
OPCODE(FSub,    "fsub",   2, true,  false)
OPCODE(FMul,    "fmul",   2, true,  false)
OPCODE(FDiv,    "fdiv",   2, true,  false)
OPCODE(Neg,     "neg",    1, true,  false)
OPCODE(Not,     "not",    1, true,  false)
OPCODE(Shl,     "shl",    2, true,  false)   // produced by strength reduction
OPCODE(AShr,    "ashr",   2, true,  false)
OPCODE(And,     "and",    2, true,  false)
OPCODE(ICmp,    "icmp",   2, true,  false)   // carries a predicate
OPCODE(FCmp,    "fcmp",   2, true,  false)
OPCODE(SIToFP,  "sitofp", 1, true,  false)
OPCODE(Alloca,  "alloca", 0, true,  false)
OPCODE(Load,    "load",   1, true,  false)
OPCODE(Store,   "store",  2, false, false)
OPCODE(Br,      "br",     0, false, true)    // unconditional; successor in the block
OPCODE(CondBr,  "condbr", 1, false, true)
OPCODE(Call,    "call",   -1, true, false)   // variadic operand count
OPCODE(Ret,     "ret",    -1, false, true)
OPCODE(Phi,     "phi",    -1, true, false)
OPCODE(Copy,    "copy",   1, true,  false)   // produced by SSA destruction
```

### 5.3 Container Hierarchy

```cpp
class Module {
    std::vector<Function*> functions_;
    StringMap<Function*>   byName_;
    SourceManager*         sm_;
    uint64_t               sourceHash_;   // stamped into .prof
};

class Function : public Value {
    StringRef                  name_;
    Type*                      returnType_;
    std::vector<Argument*>     args_;
    std::vector<BasicBlock*>   blocks_;    // blocks_[0] is the entry block
    unsigned                   nextTempId_ = 0;   // deterministic %t naming
    unsigned                   nextBlockId_ = 0;  // deterministic block naming (IR-11)
};

class BasicBlock : public Value {
    StringRef                  label_;     // "entry", "if.then", "while.body", ...
    Function*                  parent_;
    IntrusiveList<Instruction> insts_;
    std::vector<BasicBlock*>   preds_;     // maintained incrementally
    // successors are derived from the terminator, never stored separately
public:
    Instruction* terminator() const;                  // null = malformed
    std::span<BasicBlock*> successors() const;
    uint64_t executionCount = 0;                      // filled by ProfileData, 0 = unknown
};
```

**Design note — successors are derived, predecessors are stored.** Successors come from the terminator, so they cannot go stale. Predecessors must be stored (there is no reverse pointer) and are updated by a single choke point: any code that changes a terminator goes through `BasicBlock::setTerminator()`, which fixes up the predecessor lists. This eliminates the most common CFG-corruption bug.

### 5.4 Deterministic Naming (requirement IR-11)

Block labels are `<semantic-prefix>.<counter>` where the counter is per-function and assigned in creation order during IRGen: `entry`, `if.then.1`, `if.else.2`, `if.end.3`, `while.cond.4`, `while.body.5`, `while.end.6`. Passes that create blocks use the same allocator. Passes that delete blocks **do not renumber**. This makes block identity stable enough for profile matching across a recompile of unchanged source.

### 5.5 Textual IR (`--emit=ir`)

```
module "examples/sum.of" hash=0x9f2a1c04e77b3d18

fn @sum(i64 %n) -> i64 {
entry:
  %total.addr = alloca i64
  %i.addr     = alloca i64
  store i64 0, %total.addr
  store i64 0, %i.addr
  br while.cond.1

while.cond.1:                                   ; preds = entry, while.body.2
  %t0 = load i64, %i.addr
  %t1 = icmp slt i64 %t0, %n
  condbr %t1, while.body.2, while.end.3

while.body.2:                                   ; preds = while.cond.1
  %t2 = load i64, %total.addr
  %t3 = load i64, %i.addr
  %t4 = add i64 %t2, %t3
  store i64 %t4, %total.addr
  %t5 = load i64, %i.addr
  %t6 = add i64 %t5, 1
  store i64 %t6, %i.addr
  br while.cond.1

while.end.3:                                    ; preds = while.cond.1
  %t7 = load i64, %total.addr
  ret i64 %t7
}
```

After `mem2reg` the same function becomes:

```
fn @sum(i64 %n) -> i64 {
entry:
  br while.cond.1

while.cond.1:                                   ; preds = entry, while.body.2
  %total.1 = phi i64 [ 0, entry ], [ %total.2, while.body.2 ]
  %i.1     = phi i64 [ 0, entry ], [ %i.2, while.body.2 ]
  %t1      = icmp slt i64 %i.1, %n
  condbr %t1, while.body.2, while.end.3

while.body.2:                                   ; preds = while.cond.1
  %total.2 = add i64 %total.1, %i.1
  %i.2     = add i64 %i.1, 1
  br while.cond.1

while.end.3:                                    ; preds = while.cond.1
  ret i64 %total.1
}
```

When a profile is loaded, the printer annotates blocks: `while.body.2:   ; preds = ...  ; count = 50000000 (HOT)`.

### 5.5 IR Verifier

Checked after every pass in debug builds:

| Check | Failure means |
|---|---|
| Every block has exactly one terminator, as its last instruction | Malformed CFG construction |
| Entry block has no predecessors | A back edge into entry — breaks dominance |
| Every block is reachable from entry (or is pruned) | Dead block left behind |
| Predecessor lists match the terminators that reference them | CFG bookkeeping bug |
| Operand types match the opcode's signature | Type-unsafe lowering |
| Phi operand count equals predecessor count, in the same order | Phi update bug — the classic SSA crash source |
| Phi nodes appear only at the top of a block | Invalid SSA |
| In SSA mode: exactly one definition per value; every use is dominated by its definition | Broken SSA |
| No use of a value from a deleted instruction | Dangling use — memory corruption waiting to happen |

---

## 6. IR Generation

### 6.1 Lowering Rules

| AST construct | IR produced |
|---|---|
| `int x = e;` | `%x.addr = alloca i64` in the entry block; `store <e>, %x.addr` |
| `x = e;` | `store <e>, %x.addr` |
| Variable reference | `%t = load i64, %x.addr` |
| `a + b` | `%t = add <a>, <b>` (with `SIToFP` inserted on the narrower side if mixed) |
| `a < b` | `%t = icmp slt <a>, <b>` |
| `f(a, b)` | `%t = call @f(<a>, <b>)` |
| `if (c) T else E` | see below |
| `while (c) B` | see below |
| `return e;` | `ret <e>` |

**All `alloca`s are emitted in the entry block**, regardless of where the declaration appears. This is essential: `mem2reg` only promotes entry-block allocas, and an alloca inside a loop body would otherwise grow the stack every iteration.

### 6.2 Control-Flow Lowering

```
if (cond) { T } else { E }:
    %c = <lower cond>
    condbr %c, if.then.N, if.else.M
  if.then.N:   <lower T>;  br if.end.K
  if.else.M:   <lower E>;  br if.end.K
  if.end.K:    ...
  (if either branch already terminated — e.g. it returns — its br is omitted)

while (cond) { B }:
    br while.cond.N
  while.cond.N: %c = <lower cond>; condbr %c, while.body.M, while.end.K
  while.body.M: <lower B>; br while.cond.N        ; the back edge
  while.end.K:  ...
```

The back edge `while.body.M → while.cond.N` is precisely what loop detection finds in §7.4, which is what makes LICM, unrolling, and loop profiling possible. Getting this shape right in Phase 3 pays off in Phases 5, 7, and 11.

**Short-circuit `&&` / `||`** lower to additional blocks:

```
a && b   ->   %ra = <a>; condbr %ra, and.rhs.N, and.end.M
              and.rhs.N: %rb = <b>; br and.end.M
              and.end.M: %r = phi [ false, pred ], [ %rb, and.rhs.N ]
```

### 6.3 IRGen State

```cpp
class IRGenerator {
    ir::Module*    module_;
    ir::Function*  curFn_    = nullptr;
    ir::BasicBlock* curBlock_ = nullptr;
    DenseMap<Symbol*, ir::Value*> varAddrs_;   // Symbol -> its alloca
    IRBuilder builder_;                        // insertion point + convenience creators
};
```

`IRBuilder` centralizes instruction creation, insertion-point tracking, and constant folding of trivially-constant operands (so `2+3` never reaches the optimizer).

---

## 7. Analysis Framework

### 7.1 AnalysisManager

```cpp
class AnalysisManager {
public:
    template <class A> typename A::Result& get(ir::Function& F);       // compute or fetch
    template <class A> typename A::Result* getCached(ir::Function& F); // null if absent
    void invalidate(ir::Function& F, AnalysisSet preserved);
    void invalidateAll(ir::Function& F);
private:
    DenseMap<std::pair<AnalysisID, ir::Function*>, std::unique_ptr<AnalysisResult>> cache_;
};
```

Every analysis declares a static `ID()`. `ProfileData` is registered here like any other (ADR-04) — but it is *module*-scoped and never invalidated, since the profile describes an external run and no pass can change it.

### 7.2 Dominator Tree

Algorithm: **iterative dataflow (Cooper–Harvey–Kennedy)**. Chosen over Lengauer–Tarjan because it is roughly 30 lines, easy to verify by hand, and near-linear in practice on the CFG sizes we compile.

```
compute reverse-postorder(entry)
idom[entry] = entry;  idom[all others] = undefined
repeat until no change:
  for each block b in RPO, b != entry:
      newIdom = first processed predecessor of b
      for each other predecessor p of b with idom[p] defined:
          newIdom = intersect(p, newIdom)
      idom[b] = newIdom

intersect(a, b):
  while a != b:
      while rpoNum[a] > rpoNum[b]: a = idom[a]
      while rpoNum[b] > rpoNum[a]: b = idom[b]
  return a
```

Result exposes `idom(BB)`, `dominates(A, B)`, `children(BB)`, and a dominator-tree preorder walk (used by SSA renaming and GVN).

### 7.3 Dominance Frontiers

Cytron's algorithm, computed from the dominator tree:

```
for each block b with |preds(b)| >= 2:
    for each predecessor p of b:
        runner = p
        while runner != idom[b]:
            DF[runner].insert(b)
            runner = idom[runner]
```

This is the only input `mem2reg` needs beyond the dominator tree.

### 7.4 Loop Detection

```
1. Find back edges: edge (n -> h) where h dominates n.
2. For each back edge, the natural loop is {h} plus all blocks that reach n
   without passing through h — collected by a reverse BFS from n, stopping at h.
3. Loops sharing a header are merged.
4. Nest loops by containment to build a loop forest.
5. For each loop, ensure a preheader: a block whose only successor is the header
   and which is the target of every non-back-edge entry. Create one if absent.
```

```cpp
class Loop {
    BasicBlock* header_; BasicBlock* preheader_; BasicBlock* latch_;
    SmallVector<BasicBlock*> blocks_, exitBlocks_;
    Loop* parent_; SmallVector<Loop*> subLoops_;
    unsigned depth_;
    // Filled by ProfileData when available:
    uint64_t entryCount_ = 0;       // times the loop was entered
    uint64_t iterationCount_ = 0;   // total iterations across all entries
    double   avgTripCount() const { return entryCount_ ? double(iterationCount_)/entryCount_ : 0; }
};
```

`avgTripCount()` is what drives the PGO unroller in §16.2. Guaranteeing a preheader is what makes LICM's hoist target unambiguous.

### 7.5 Liveness Analysis

Backward dataflow to a fixed point over the reverse-postorder-reversed block order:

```
live_out[B] = union over S in succ(B) of live_in[S]
live_in[B]  = use[B] union (live_out[B] minus def[B])
```

Phi nodes are handled specially: a phi's operand is live at the **end of the corresponding predecessor**, not at the top of the phi's own block. Getting this wrong is the classic source of register-allocator miscompiles, so the design isolates it in one helper (`addPhiOperandLiveness`) with a dedicated unit test.

### 7.6 Generic Dataflow Driver

```cpp
template <class Domain, Direction Dir>
class DataflowAnalysis {
    virtual Domain boundary() const = 0;
    virtual Domain meet(const Domain&, const Domain&) const = 0;
    virtual Domain transfer(BasicBlock&, const Domain& in) const = 0;
public:
    DenseMap<BasicBlock*, Domain> run(Function&);   // worklist to fixed point
};
```

Reaching definitions, liveness, and available expressions are each ~50 lines on top of this (requirement AN-11).

---

## 8. SSA Construction & Destruction

### 8.1 mem2reg (construction)

```
Step 1 — Find promotable allocas:
    an alloca in the entry block whose every use is a direct Load or Store
    (never passed as an operand elsewhere — no address taken).

Step 2 — Phi insertion (per promotable variable v):
    W = { blocks containing a Store to v }
    while W not empty:
        b = W.pop()
        for each d in DF[b]:
            if d has no phi for v:
                insert phi for v at the top of d
                if d has no Store to v: W.push(d)

Step 3 — Renaming (dominator-tree preorder DFS, with a stack per variable):
    on entering block b:
        for each phi for v in b:      push(v, phi)
        for each instruction:
            Load  of v  -> replaceAllUsesWith(top(v)); erase
            Store to v  -> push(v, storedValue); erase
        for each successor s: fill in s's phi operand for predecessor b with top(v)
    recurse into dominator-tree children
    on leaving b: pop everything b pushed
    finally: erase the alloca
```

### 8.2 SSA Destruction

Run immediately before register allocation. Phi nodes are not machine instructions, so each phi becomes copies in the predecessor blocks:

```
for each phi   %x = phi [ %a, P1 ], [ %b, P2 ]:
    in P1 (before its terminator): copy %x_reg <- %a
    in P2 (before its terminator): copy %x_reg <- %b
    replace all uses of %x with %x_reg; erase the phi
```

Two classic hazards, both handled explicitly:

- **Critical edges** (a predecessor with multiple successors feeding a block with multiple predecessors): the copy has nowhere correct to go. **Fix:** split every critical edge before destruction, inserting an empty block for the copy.
- **The swap problem / lost copies** (`%a = phi[%b], %b = phi[%a]` in the same block — parallel copies with a cycle): naive sequential copies corrupt one value. **Fix:** treat each block's phi group as a *parallel copy* and sequentialize it properly, breaking cycles with one temporary.

These two are the highest-risk correctness items in the middle-end and each gets a dedicated regression test.

---

## 9. Pass Infrastructure

```cpp
class Pass {
public:
    virtual StringRef name() const = 0;
    virtual AnalysisSet requiredAnalyses() const { return {}; }
    virtual AnalysisSet preservedAnalyses() const { return {}; }
    virtual ~Pass() = default;
};

class FunctionPass : public Pass {
public:
    virtual bool run(ir::Function&, AnalysisManager&) = 0;   // true = IR changed
};

class ModulePass : public Pass {
public:
    virtual bool run(ir::Module&, AnalysisManager&) = 0;
};

class PassManager {
public:
    void addPass(std::unique_ptr<Pass>);
    bool run(ir::Module&, AnalysisManager&);
    void enableVerifyEach(bool);
    void enablePrintAfterEach(bool);
private:
    std::vector<std::unique_ptr<Pass>> passes_;
};
```

**Contract:** a pass returning `true` triggers invalidation of every analysis not in its `preservedAnalyses()`. A pass that mutates the CFG but claims to preserve `DominatorTree` is a correctness bug; `--verify-analyses` catches it by recomputing and comparing.

Passes register themselves at static-initialization time into a `PassRegistry`, so pipelines are built from names and `--print-after=<name>` works for any pass without a lookup table.

---

## 10. Optimization Pass Designs

### 10.1 Constant Folding
Peephole over instructions with constant operands. Also folds algebraic identities: `x+0`, `x*1`, `x*0`, `x-x`, `x/1`, `x&0`. Implemented inside `IRBuilder` too, so folding happens at creation time as well as as a pass.

### 10.2 Sparse Conditional Constant Propagation (SCCP)
Preferred over separate constant propagation because it folds constants **and** prunes unreachable branches simultaneously, catching cases neither does alone.

Lattice per value: `Undefined ⊑ Constant(c) ⊑ Overdefined`. Two worklists (CFG edges, SSA values). Only blocks proven reachable are processed, so a branch on a constant marks one successor unreachable and everything downstream stays `Undefined` rather than being conservatively `Overdefined`. Afterwards: replace constant values, replace constant branches with unconditional ones, delete unreachable blocks.

### 10.3 Dead Code Elimination
Mark-and-sweep over SSA use lists. Seed the live set with instructions that have side effects (`Store`, `Call`, `Ret`, `Br`, `CondBr`). Propagate liveness backwards through operands. Sweep everything unmarked. On SSA with maintained use lists this is one linear pass and needs no dataflow at all.

### 10.4 Copy Propagation
For each `%b = copy %a`: `RAUW(%b, %a)`, erase. After `mem2reg` most copies come from SSA destruction, so this runs both in the middle-end and after phi elimination.

### 10.5 CSE / GVN
Dominator-tree-scoped value numbering. Walk the dominator tree in preorder with a scoped hash table keyed by `(opcode, operand value numbers, predicate)`. On a hit where the earlier definition dominates the current one: `RAUW` and erase. Scoped insertion/removal on entering/leaving each dominator-tree node keeps the "dominates" requirement automatic rather than requiring an explicit check.

Commutative operations sort their operands by value number before hashing, so `a+b` and `b+a` hash identically.

### 10.6 Strength Reduction
Peephole over the IR:

| Pattern | Replacement |
|---|---|
| `mul %x, 2^k` | `shl %x, k` |
| `sdiv %x, 2^k` | `ashr` with a sign-correction sequence |
| `srem %x, 2^k` | `and` plus sign correction |
| `mul %x, 2` | `add %x, %x` |
| `mul %x, 3` | `shl %x,1` then `add` (when the target says it is cheaper) |

Profitability comes from `TargetInfo::instructionCost()` — this is the one middle-end pass that legitimately consults the target, and it does so through the interface, not by including target headers.

### 10.7 LICM
For each loop, innermost-first:

```
for each block b in the loop, in dominator-tree order:
    for each instruction i in b:
        if i has no side effects and is safe to speculate,
           and every operand is defined outside the loop or is already hoisted,
           and (i dominates all loop exits OR i cannot fault):
              move i to the loop preheader
```

The dominates-all-exits condition is what prevents hoisting a division that would fault on an iteration the original program never reached. Division and modulo are excluded from speculative hoisting unless the divisor is a known non-zero constant.

### 10.8 SimplifyCFG
Merge a block into its single predecessor when that predecessor has a single successor; delete empty blocks whose only instruction is an unconditional branch (redirecting predecessors and fixing phis); fold `condbr` on a constant into `br`; remove unreachable blocks. Runs repeatedly because each simplification can enable another.

### 10.9 Inlining (static heuristic)
Bottom-up over the call graph. Inline when: the callee is not recursive, the callee's instruction count is below the threshold (default 50 at `-O2`), and the call site is not in a cold block. Mechanics: clone the callee's blocks into the caller with a value map, map arguments to actual operands, split the call's block at the call site, redirect `Ret` to the continuation block (with a phi if there are multiple returns).

---

## 11. Backend Design

### 11.1 Machine IR

```cpp
struct MachineOperand {
    enum class Kind { VirtualReg, PhysicalReg, Immediate, FrameIndex, GlobalLabel, BasicBlockRef };
    Kind kind; union { unsigned reg; int64_t imm; int frameIdx; StringRef label; };
    bool isDef = false;
};

struct MachineInstr {
    X86Opcode opcode;                      // MOV64rr, ADD64rr, IMUL64rri, CMP64rr, JMP, JE, CALL, RET, ...
    SmallVector<MachineOperand, 3> ops;
    SourceLocation loc;                    // for --emit=asm comments
};

struct MachineBasicBlock {
    StringRef label; std::vector<MachineInstr> insts;
    std::vector<MachineBasicBlock*> preds, succs;
    uint64_t executionCount = 0;           // carried from ProfileData for layout
};
```

### 11.2 Instruction Selection

Phase 4 uses **macro expansion**: each IR instruction maps to a fixed template, operands assumed to be in virtual registers.

| IR | x86-64 (virtual registers) |
|---|---|
| `%d = add %a, %b` | `mov %d, %a` ; `add %d, %b` |
| `%d = sub %a, %b` | `mov %d, %a` ; `sub %d, %b` |
| `%d = mul %a, %b` | `mov %d, %a` ; `imul %d, %b` |
| `%d = sdiv %a, %b` | `mov rax, %a` ; `cqo` ; `idiv %b` ; `mov %d, rax` |
| `%d = srem %a, %b` | `mov rax, %a` ; `cqo` ; `idiv %b` ; `mov %d, rdx` |
| `%d = icmp slt %a, %b` | `cmp %a, %b` ; `setl %d8` ; `movzx %d, %d8` |
| `condbr %c, T, F` | `test %c, %c` ; `jne T` ; `jmp F` (the `jmp` is dropped if `F` follows) |
| `%d = call @f(%a, %b)` | `mov rdi, %a` ; `mov rsi, %b` ; `call f` ; `mov %d, rax` |
| `ret %a` | `mov rax, %a` ; epilogue ; `ret` |

`sdiv`/`srem` are the awkward cases — `idiv` has fixed `rax`/`rdx` operands, which constrains the register allocator. These are modeled with explicit physical-register constraints so the allocator handles them rather than the selector special-casing them.

A later improvement (Phase 13) is pattern-matching selection that fuses `%t = mul %i, 8; %a = add %base, %t` into a single addressing mode.

### 11.3 Block Layout

Default: reverse postorder, which naturally places loop bodies contiguously.
With a profile (PGO-09): order blocks so the **most frequent successor falls through**, and sink blocks with zero or near-zero counts to the end of the function. This improves branch prediction and instruction-cache density and is one of the cheapest, most reliable PGO wins.

### 11.4 Prologue / Epilogue

```
prologue:  push rbp
           mov  rbp, rsp
           push <callee-saved GPRs actually used>   ; before the frame, see below
           sub  rsp, <frameSize>
           movups <callee-saved XMMs>, <frame slot> ; SSE has no push

epilogue:  movups <frame slot>, <callee-saved XMMs>
           lea  rsp, [rbp - 8*N]                    ; N = GPRs pushed
           pop  <the same GPRs, reverse order>
           pop  rbp
           ret
```

**Corrected in Phase 8**, from a sketch that put the pushes after `sub rsp`.
Three facts the implementation had to respect and the sketch did not:

- The pushes go **before** the frame, so local slots start at `rbp - 8*N` rather
  than at `rbp`. Putting them after would leave the pushed registers sitting on
  top of the outgoing-argument area.
- `frameSize` gains 8 bytes when N is odd. `rsp` must be 16-byte aligned at
  every call, and `push rbp` plus N pushes plus the frame is what gets it there;
  the frame is the only part still free to fix the parity.
- SSE callee-saved registers have no push. They get frame slots, written
  **after** `sub rsp` — Windows x64 has no red zone, so nothing may be stored
  below `rsp`.

Frame size is known only after register allocation decides which values live in memory and which callee-saved registers are used, so prologue/epilogue insertion runs **after** allocation.

### 11.5 Calling Convention (System V AMD64 — pending ADR-10)

| Role | Registers |
|---|---|
| Integer arguments 1–6 | `rdi rsi rdx rcx r8 r9` |
| Float arguments 1–8 | `xmm0`–`xmm7` |
| Integer return | `rax` |
| Float return | `xmm0` |
| Caller-saved | `rax rcx rdx rsi rdi r8-r11`, all `xmm` |
| Callee-saved | `rbx rbp r12-r15` |
| Stack alignment at `call` | 16 bytes |

All of these live in `TargetInfo` so a Microsoft-x64 target (`rcx rdx r8 r9`, 32-byte shadow space, different callee-saved set) is a data change rather than a code change.

---

## 12. Register Allocation

### 12.1 Naive Allocator (Phase 4, retained permanently — ADR-08)

Every virtual register gets a stack slot. For each instruction: load operands into scratch registers (`r10`, `r11`), execute, store the result back. Correct, slow, and completely predictable — which is exactly what is wanted when debugging the rest of the backend.

### 12.2 Graph-Coloring Allocator (Phase 8)

Chaitin–Briggs with coalescing:

```
1. Build      : liveness -> live ranges -> interference graph
                (two vregs interfere if one is live at the other's definition point)
2. Coalesce   : merge move-related nodes when the merged node is provably colorable
                (Briggs' conservative test: fewer than K neighbors of significant degree)
3. Simplify   : repeatedly remove nodes with degree < K, pushing them on a stack
4. Freeze     : if stuck, give up on coalescing a low-degree move-related node
5. Spill      : if still stuck, pick a spill candidate by cost, remove it optimistically
6. Select     : pop the stack, assign colors; an uncolorable node becomes an actual spill
7. Rewrite    : insert spill stores and reloads, then repeat from step 1 (usually 1-2 iterations)
```

**Step 7 does not exist in the implementation, and does not need to.** The
rewrite-and-repeat loop is there because spill code introduces new short live
ranges the graph has to account for. Here the code generator already computes
with values that live in memory — that is all the naive allocator ever did — so
"spill" means only "assign no register", and the reloads use the reserved
scratch registers, which are not allocatable and so add no live range at all.
One pass is exact rather than approximate.

**Spill cost heuristic** — the hook where PGO enters the backend:

```
staticCost(v)  = sum over uses and defs of  10^loopDepth(block)  /  degree(v)
profileCost(v) = sum over uses and defs of  blockExecCount(block) / degree(v)
```

Without a profile, static loop depth is a guess: it assumes every loop runs about ten times and every branch is taken half the time. With a profile, the cost is the *measured* number of executions. A loop the compiler statically assumes is hot but which the profile shows runs twice will stop stealing registers from the loop that actually runs fifty million times. This is a small code change with a disproportionate effect, and it is a good candidate for the first PGO win to implement (PGO-08).

**K = 8 integer registers** (`r11 rbx rsi rdi r12-r15`) and **10 SSE**
(`xmm6`–`xmm15`), not the 14 this section originally assumed.

The difference is a decision taken in Phase 8 and worth recording rather than
quietly correcting. The original figure counted `rax` and `rdx` as "constrained
around `idiv` but still allocatable elsewhere", which is true and is what a
production allocator does — by modelling the constraint as a pre-coloured node
and letting colouring work around it. Doing that means every fixed-register
instruction (`idiv`, variable shifts, every argument register at every call,
the return register) becomes a set of pre-coloured nodes and interference edges,
and each one is a chance to be wrong in a way that miscompiles silently.

Instead the target keeps those registers out of the allocatable pool entirely:
`rcx rdx r8 r9` carry arguments, `rax` returns and is scratch, `r10` is the
second scratch, `rsp rbp` are the frame. The allocator then needs no
pre-colouring at all. Six registers bought for an allocator whose correctness
argument fits on a page — and, measured, still 63% less memory traffic than the
naive allocator (metric BE-04).

`r11` is listed first in the pool because it is the only caller-saved register
in it: handing it out before a callee-saved one saves a push and a pop. It is
also the one register a value spanning a call may never use.

---

## 13. Instrumentation Design

Runs late (ADR-05), after optimization, immediately before codegen.

### 13.1 What Gets Counted

| Counter | Placement | Purpose |
|---|---|---|
| Function entry | First instruction of the entry block | Call counts (PROF-02) |
| Basic block | First non-phi instruction of each block | Block frequencies (PROF-03) |
| Branch edges | Both successors of each `condbr` | Branch bias (PROF-04) |
| Loop entry | Loop preheader | Loop entry count (PROF-05) |
| Loop iteration | Loop header | Total iterations (PROF-05) |

Branch edge counting is placed in the successor blocks when the edge is not critical; critical edges are split first so each edge has a unique home block.

### 13.2 Counter Storage

One global array per module, emitted as a BSS symbol:

```
  .bss
  .globl __ofprof_counters
__ofprof_counters:  .zero  8 * NUM_COUNTERS
```

Increment code, inserted directly (no call, so the hot path stays cheap):

```asm
  incq __ofprof_counters(%rip)        ; for counter index 0
  incq __ofprof_counters+8(%rip)      ; index 1, etc.
```

A single `incq` to a static address is roughly 1–2 cycles amortized and does not clobber any allocatable register or require a call sequence. This is what keeps overhead within the 40% target (NFR-09).

### 13.3 Counter Metadata

The compiler emits a parallel metadata table describing what each index means, so `libofprof` can write a symbolic profile without any compiler code inside the target program:

```
  .section .rodata
__ofprof_names:
  .quad  NUM_COUNTERS
  .quad  __ofprof_src_hash
  .asciz "FUNCTION\0main"
  .asciz "BLOCK\0compute\0while.body.5"
  .asciz "BRANCH\0compute\0while.cond.4\0taken"
  ...
```

This design keeps `libofprof` a fixed, generic library — it walks the metadata table and the counter array in parallel and writes the file. No generated code is needed to describe the program to itself.

### 13.4 Counter Index Assignment

Assigned in a deterministic walk order: functions in module order, blocks in reverse postorder, counters within a block in a fixed kind order. Combined with deterministic block naming (§5.4), this makes indices reproducible — though the *matching* on the PGO side is done by name, not index, so an index shift is harmless (ADR-06).

---

## 14. Profile Runtime & Format

### 14.1 libofprof

```c
/* runtime/ofprof/ofprof.c  — plain C, no dependency on compiler code */
extern uint64_t __ofprof_counters[];
extern const char __ofprof_names[];
extern const uint64_t __ofprof_num_counters;
extern const uint64_t __ofprof_src_hash;

static void ofprof_dump(void);                       /* registered with atexit */

__attribute__((constructor))
static void ofprof_init(void) { atexit(ofprof_dump); }

static void ofprof_dump(void) {
    const char* path = getenv("OPTIFORGE_PROFILE_OUT");
    if (!path) path = __ofprof_default_path;         /* emitted by the compiler */
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "ofprof: cannot write %s\n", path); return; }
    /* write header, then walk names + counters in parallel */
    fclose(f);
}
```

`atexit` registration via a constructor means the program source needs no changes at all (PROF-07). Programs that terminate abnormally lose their profile — documented, with a `ofprof_flush()` escape hatch available.

### 14.2 `.prof` Format (authoritative spec: `docs/profile-format.md`)

Line-oriented text (ADR-07). Blank lines and `#` comments ignored.

```
OPTIFORGE_PROFILE 1
SOURCE examples/compute.of
SRCHASH 0x9f2a1c04e77b3d18
OPTLEVEL 2
COMPILER optiforge-0.1.0
RUNS 1
TOTAL_SAMPLES 50502511

FUNCTION main 1
FUNCTION compute 500000
FUNCTION helper 10

BLOCK compute entry 500000
BLOCK compute while.cond.4 50500000
BLOCK compute while.body.5 50000000
BLOCK compute while.end.6 500000

BRANCH compute while.cond.4 taken=50000000 not_taken=500000

LOOP compute while.cond.4 entries=500000 iterations=50000000

TIME compute 1842.55
```

**Grammar:**

```ebnf
profile     = header { record } ;
header      = "OPTIFORGE_PROFILE" version NL
              "SOURCE" path NL "SRCHASH" hex NL "OPTLEVEL" int NL
              "COMPILER" string NL [ "RUNS" int NL ] [ "TOTAL_SAMPLES" int NL ] ;
record      = func_rec | block_rec | branch_rec | loop_rec | time_rec ;
func_rec    = "FUNCTION" ident count NL ;
block_rec   = "BLOCK"    ident ident count NL ;             (* function, block *)
branch_rec  = "BRANCH"   ident ident "taken=" count "not_taken=" count NL ;
loop_rec    = "LOOP"     ident ident "entries=" count "iterations=" count NL ;
time_rec    = "TIME"     ident float NL ;                   (* milliseconds *)
```

### 14.3 Validation on Load

| Condition | Action |
|---|---|
| Unknown version | Error: refuse the profile, compile without it, warn |
| `SRCHASH` mismatch | **Warning**, then attempt name-based partial matching; report the match rate |
| Match rate below 50% | Warning: "profile appears stale, PGO effectiveness will be limited" |
| Unknown record type | Ignore that line, warn once (forward compatibility) |
| Malformed line | Ignore that line, warn, continue |
| Flow conservation violated (block counts inconsistent with edges) | Warn, use counts as advisory only |
| Missing file | Error naming the path; compile without a profile |

Every row ends in a correct binary. That is requirement PGO-11 and it is designed in, not tested in.

### 14.4 ProfileData (the analysis)

```cpp
class ProfileData {
public:
    enum class Heat { Cold, Warm, Hot, Unknown };

    uint64_t functionCount(StringRef fn) const;
    uint64_t blockCount(StringRef fn, StringRef bb) const;
    double   branchProbability(StringRef fn, StringRef bb) const;  // P(taken), NaN if unknown
    double   loopTripCount(StringRef fn, StringRef header) const;
    Heat     heatOf(const ir::Function&) const;
    Heat     heatOf(const ir::BasicBlock&) const;
    bool     isValid() const;
    double   matchRate() const;      // fraction of IR entities found in the profile
};
```

Passes obtain it via `AM.getCached<ProfileData>()`, which returns `nullptr` when no profile was supplied — the fallback path every PGO pass must handle.

---

## 15. Hot-Path Detection

### 15.1 Classification

Absolute thresholds do not transfer between programs, so classification is **relative**, with an absolute floor:

```
totalBlockExecutions = sum of all block counts
Sort blocks by count, descending.

HOT   : blocks in the smallest prefix whose cumulative count reaches 80% of the total
        AND whose own count >= 1000 (the floor stops trivial programs producing
        a "hot" block that ran four times)
COLD  : count == 0, or count < 0.01% of the maximum block count
WARM  : everything else
```

Functions inherit heat from their entry block count, measured against total function entries. Loops are hot if their header is hot; the trip count then decides the unroll factor separately.

`--hot-threshold=<percent>` overrides the 80% cumulative figure (PGO-04).

### 15.2 Report Output (`--profile-report`)

```
OptiForge Profile Report — examples/compute.of
Profile: compute.prof   (match rate: 100%,  1 run,  50,502,511 samples)

HOT FUNCTIONS
  compute            calls =    500,000    (99.9% of executions)   [HOT]

WARM FUNCTIONS
  main               calls =          1                            [WARM]

COLD FUNCTIONS
  helper             calls =         10    (0.00002%)              [COLD]

HOT LOOPS
  compute:while.cond.4     entries = 500,000    iterations = 50,000,000
                           avg trip count = 100.0                  [HOT]
                           -> unroll candidate (factor 4)

BIASED BRANCHES  (>90% one-sided)
  compute:while.cond.4     taken 98.99%  /  not-taken 1.01%
                           -> layout: place while.body.5 as fall-through

SUMMARY
  2 hot blocks, 1 warm block, 3 cold blocks (1 never executed)
  Suggested PGO actions: 1 inline, 1 unroll, 1 layout change
```

This report is a deliverable in its own right (PROF/PGO-05) — it is the clearest demonstration that profiling works, and it is producible at Phase 10, before any PGO pass exists.

---

## 16. PGO Pass Designs

Each pass states its behaviour **with** and **without** a profile. The "without" column is not a degradation path bolted on afterwards; it is the pass's normal, always-tested behaviour.

### 16.1 PGO Inlining

| | Without profile | With profile |
|---|---|---|
| Size budget | 50 instructions | 50 baseline; **250** for hot call sites; **0** for cold ones |
| Recursion | never inlined | hot self-recursive functions may be unrolled one level |
| Ordering | call-graph bottom-up | hot call sites considered first, so the budget goes where it matters |

The gain is not merely the eliminated call overhead — inlining a hot callee exposes its body to the caller's constants, which then feeds SCCP, CSE, and LICM. This is why `sccp` is scheduled again immediately after inlining in the `-O2` pipeline.

### 16.2 PGO Loop Unrolling

```
for each loop L:
    trip = profile ? profileTripCount(L) : staticTripCountOrUnknown(L)
    if L is not HOT:                 skip (cold loops only cost code size)
    if trip is unknown:              conservative unroll by 2, only if the body is tiny
    if trip <= 4 and known constant: fully unroll
    if trip is large:                factor = clamp(nextPow2(trip / 8), 2, 8)
                                     bounded by bodySize * factor <= 400 instructions
    emit remainder loop when trip is not a multiple of the factor
```

Without a profile the unroller is guessing at both "is it hot" and "how many iterations." With one, it knows both. This is the single clearest illustration of the project's thesis and should be the showcase example in the final report.

### 16.3 Profile-Weighted Register Allocation
Swaps `staticCost` for `profileCost` in §12.2. Roughly ten lines of change for a measurable win — implement it first among the PGO passes.

### 16.4 Profile-Guided Block Layout
Greedy chain construction: start from the entry block, repeatedly append the highest-count successor not already placed, and emit remaining chains ordered by count with zero-count blocks last. Then invert conditional branches where doing so makes the hot successor the fall-through.

### 16.5 Cold Code Size Mode
Blocks classified `Cold` skip unrolling and inlining entirely and prefer smaller encodings. Cold *functions* are compiled at an effective `-O1`. The gain is instruction-cache pressure, not direct execution time — worth measuring separately, and worth reporting honestly if the effect is small on benchmarks of our size.

---

## 17. Driver & CLI

### 17.1 Compilation Sequence

```
1. Parse CLI options; validate their combinations
   (e.g. --profile with --use-profile is an error: instrument or use, not both)
2. Read source; register with SourceManager; compute the source hash
3. Lex           -> tokens          [--emit=tokens stops here]
4. Parse         -> AST             [--emit=ast (untyped) stops here]
5. Sema          -> typed AST       [--emit=ast (typed) stops here]
   -> abort if DiagnosticEngine::hadError()
6. IRGen         -> ir::Module      [--emit=ir stops here at -O0]
7. Verify IR
8. If --use-profile: read, validate, build ProfileData, register with AnalysisManager
9. Build the pass pipeline for the requested -O level (profile-aware variant if step 8 ran)
10. Run the pipeline                [--emit=ir / --emit=cfg stops here]
11. If --profile: run InstrumentationPass; verify
12. SSA destruction; critical edge splitting
13. Instruction selection -> MachineFunctions
14. Register allocation
15. Block layout (profile-aware if available)
16. Prologue/epilogue insertion
17. Emit assembly                   [--emit=asm stops here]
18. Invoke the assembler and linker; link libofrt, plus libofprof if instrumented
19. Report warnings; exit with the appropriate code
```

### 17.2 Option Surface

```
optiforge <input.of> [options]

  -o <file>              Output path (default: a.out / a.exe)
  -O0 -O1 -O2            Optimization level (default: -O0)
  --emit=<stage>         tokens | ast | ir | cfg | asm | obj
  --profile              Build an instrumented binary that writes a .prof on exit
  --profile-out=<file>   Default .prof path baked into the instrumented binary
  --use-profile=<file>   Enable profile-guided optimization
  --profile-report=<f>   Print the hot-path report for a .prof and exit
  --hot-threshold=<pct>  Cumulative percentage defining "hot" (default 80)
  --regalloc=<naive|graph>   Register allocator (default: graph)
  --print-after=<pass>   Dump IR after the named pass
  --print-after-all      Dump IR after every pass
  --verify-each          Run the IR verifier after every pass
  --verify-analyses      Recompute analyses and compare against the cache
  --pgo-remarks          Explain every profile-guided decision
  --time-passes          Per-pass timing
  -W<name> / -Wno-<name> Warning control;  -Werror
  --help  --version
```

### 17.3 Exit Codes

| Code | Meaning |
|---|---|
| 0 | Success (warnings permitted) |
| 1 | Compilation error in user code |
| 2 | Invalid command-line usage |
| 3 | Environment error (missing file, assembler not found) |
| 70 | Internal compiler error |

---

## 18. Testing Design

### 18.1 Golden-File Tests

```
tests/parser/expr_precedence.of          # input
tests/parser/expr_precedence.ast.txt     # expected --emit=ast output
```

The runner compiles with the appropriate `--emit`, diffs against the expected file, and prints a unified diff on mismatch. `tools/update-goldens.py` regenerates expectations — with the rule that a regenerated golden must be *read* before it is committed, since blind regeneration is how golden tests stop catching anything.

### 18.2 Negative Tests

```
// tests/sema/undeclared_var.of
// EXPECT-ERROR: 3:5: error: use of undeclared variable 'y'
int main() -> int {
    y = 5;
    return 0;
}
```

The runner extracts `EXPECT-ERROR` / `EXPECT-WARNING` comments and requires an exact match on the set of diagnostics — no missing ones, no extra ones.

### 18.3 End-to-End Tests

```
// tests/e2e/fib.of
// EXPECT-OUTPUT: 832040
// EXPECT-EXIT: 0
```

### 18.4 Differential Tests (the highest-value category)

For every program in `tests/e2e/` and `examples/`, the runner compiles at `-O0`, `-O1`, `-O2`, `--profile`, and `-O2 --use-profile`, runs all five, and requires byte-identical stdout and exit codes. Every new pass is instantly covered by every existing program, so this suite strengthens automatically as the compiler grows.

### 18.5 PGO Integration Test

```
1. optiforge bench.of -O2 --profile -o inst
2. ./inst <fixed workload>                    -> bench.prof
3. Assert bench.prof exists and parses
4. Assert the report names the expected hot function and hot loop
5. optiforge bench.of -O2 --use-profile=bench.prof -o pgo
6. Assert ./pgo output == ./inst output == ./a.out output
7. Assert the PGO build applied the expected decisions (via --pgo-remarks)
8. Record timings for the benchmark table
```

Step 7 matters as much as step 6: a PGO build that is correct but made no decisions at all would silently pass a correctness-only test. Asserting on the *remarks* is what detects a profile that stopped matching.

### 18.6 Benchmark Harness

```
bench/harness/run.py --configs O0,O1,O2,PGO --reps 10 --report metrics/results/<date>.md
```

Reports median, minimum, and interquartile range per configuration, plus generated code size, compile time, and instrumentation overhead — with CPU model, core count, frequency governor, and compiler version recorded in the header so results remain interpretable later.
