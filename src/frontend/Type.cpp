#include "optiforge/frontend/Type.h"

#include "optiforge/frontend/AST.h"

namespace optiforge {

std::string_view Type::name() const {
  switch (kind_) {
    case Kind::Int:
      return "int";
    case Kind::Float:
      return "float";
    case Kind::Bool:
      return "bool";
    case Kind::Void:
      return "void";
  }
  return "<unknown>";
}

unsigned Type::sizeInBytes() const {
  switch (kind_) {
    case Kind::Int:
      return 8;
    case Kind::Float:
      return 8;
    case Kind::Bool:
      return 1;
    case Kind::Void:
      return 0;
  }
  return 0;
}

const Type* Type::get(Kind kind) {
  // Function-local statics: initialized once, in a fixed order, with no
  // dependency on translation-unit initialization order.
  static const Type kInt{Kind::Int};
  static const Type kFloat{Kind::Float};
  static const Type kBool{Kind::Bool};
  static const Type kVoid{Kind::Void};

  switch (kind) {
    case Kind::Int:
      return &kInt;
    case Kind::Float:
      return &kFloat;
    case Kind::Bool:
      return &kBool;
    case Kind::Void:
      return &kVoid;
  }
  return &kVoid;
}

const Type* typeFromSpec(TypeSpec spec) {
  switch (spec) {
    case TypeSpec::Int:
      return Type::getInt();
    case TypeSpec::Float:
      return Type::getFloat();
    case TypeSpec::Bool:
      return Type::getBool();
    case TypeSpec::Void:
      return Type::getVoid();
  }
  return Type::getVoid();
}

bool isAssignable(const Type* target, const Type* source) {
  if (target == nullptr || source == nullptr) {
    return true;  // an earlier error already reported; do not cascade
  }
  if (target == source) {
    return true;
  }
  // The single implicit widening conversion. Everything else, including
  // float -> int and any conversion involving bool, is an error.
  return target->isFloat() && source->isInt();
}

const Type* promoteArithmetic(const Type* lhs, const Type* rhs) {
  if (lhs == nullptr || rhs == nullptr) {
    return nullptr;
  }
  if (!lhs->isNumeric() || !rhs->isNumeric()) {
    return nullptr;
  }
  if (lhs->isFloat() || rhs->isFloat()) {
    return Type::getFloat();
  }
  return Type::getInt();
}

}  // namespace optiforge
