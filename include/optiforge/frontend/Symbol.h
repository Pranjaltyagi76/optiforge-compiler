#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "optiforge/support/SourceLocation.h"

namespace optiforge {

class Type;

/// A declared name. AST nodes point at these after semantic analysis, so a
/// Symbol must outlive the analysis pass -- the SymbolTable owns them for the
/// whole compilation.
struct Symbol {
  enum class Kind : std::uint8_t { Variable, Parameter, Function };

  Kind kind = Kind::Variable;
  std::string name;
  /// For a function this is the return type.
  const Type* type = nullptr;
  /// Where the name was declared, for "previous declaration here" notes.
  SourceRange declRange{};

  // Functions only.
  std::vector<const Type*> paramTypes;
  std::vector<std::string> paramNames;
  /// True for names provided by the runtime rather than the source file.
  bool isBuiltin = false;

  bool isFunction() const { return kind == Kind::Function; }
  bool isVariableLike() const { return kind == Kind::Variable || kind == Kind::Parameter; }
};

/// One lexical scope. Owns the symbols declared directly in it.
class Scope {
public:
  explicit Scope(Scope* parent) : parent_(parent) {}

  Scope* parent() const { return parent_; }

  /// Looks in this scope only. Used for duplicate detection.
  Symbol* lookupLocal(std::string_view name) const;

  /// Looks here, then outward through enclosing scopes.
  Symbol* lookup(std::string_view name) const;

  /// Returns the new symbol, or null if the name is already declared in *this*
  /// scope (shadowing an outer scope is allowed and handled by the caller).
  Symbol* insert(Symbol symbol);

private:
  Scope* parent_;
  // Insertion-ordered ownership. A deque keeps Symbol addresses stable as more
  // symbols are added, which matters because AST nodes hold raw pointers here.
  std::deque<Symbol> owned_;
  std::unordered_map<std::string, Symbol*> index_;
};

/// Stack of scopes for one compilation.
///
/// Scopes are never destroyed while the table lives: popping only makes a scope
/// inactive. AST nodes keep pointers to symbols declared in scopes that have
/// already been left, so destroying them at pop time would dangle.
class SymbolTable {
public:
  SymbolTable();

  void pushScope();
  void popScope();

  Scope& current() { return *active_.back(); }
  Scope& global() { return scopes_.front(); }

  /// Declares in the current scope. Null if already declared there.
  Symbol* declare(Symbol symbol);

  Symbol* lookup(std::string_view name) const { return active_.back()->lookup(name); }
  Symbol* lookupLocal(std::string_view name) const {
    return active_.back()->lookupLocal(name);
  }

  /// Innermost enclosing declaration of `name`, ignoring the current scope.
  /// Used to report shadowing.
  Symbol* lookupOuter(std::string_view name) const;

  std::size_t depth() const { return active_.size(); }

private:
  std::deque<Scope> scopes_;      // every scope ever created, in creation order
  std::vector<Scope*> active_;    // the currently open nest
};

}  // namespace optiforge
