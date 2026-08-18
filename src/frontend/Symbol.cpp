#include "optiforge/frontend/Symbol.h"

#include <string>
#include <utility>

namespace optiforge {

Symbol* Scope::lookupLocal(std::string_view name) const {
  const auto it = index_.find(std::string(name));
  return it != index_.end() ? it->second : nullptr;
}

Symbol* Scope::lookup(std::string_view name) const {
  for (const Scope* scope = this; scope != nullptr; scope = scope->parent_) {
    if (Symbol* found = scope->lookupLocal(name)) {
      return found;
    }
  }
  return nullptr;
}

Symbol* Scope::insert(Symbol symbol) {
  const std::string name = symbol.name;
  if (index_.find(name) != index_.end()) {
    return nullptr;
  }
  owned_.push_back(std::move(symbol));
  Symbol* stored = &owned_.back();
  index_.emplace(name, stored);
  return stored;
}

SymbolTable::SymbolTable() {
  scopes_.emplace_back(nullptr);
  active_.push_back(&scopes_.front());
}

void SymbolTable::pushScope() {
  scopes_.emplace_back(active_.back());
  active_.push_back(&scopes_.back());
}

void SymbolTable::popScope() {
  // The scope object itself stays alive in scopes_; only its position in the
  // active nest is dropped. Symbols declared in it are still referenced by the
  // AST after the scope closes.
  if (active_.size() > 1) {
    active_.pop_back();
  }
}

Symbol* SymbolTable::declare(Symbol symbol) { return active_.back()->insert(std::move(symbol)); }

Symbol* SymbolTable::lookupOuter(std::string_view name) const {
  const Scope* enclosing = active_.back()->parent();
  return enclosing != nullptr ? enclosing->lookup(name) : nullptr;
}

}  // namespace optiforge
