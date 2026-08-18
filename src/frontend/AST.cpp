#include "optiforge/frontend/AST.h"

namespace optiforge {

std::string_view toString(TypeSpec spec) {
  switch (spec) {
    case TypeSpec::Int:
      return "int";
    case TypeSpec::Float:
      return "float";
    case TypeSpec::Bool:
      return "bool";
    case TypeSpec::Void:
      return "void";
  }
  return "<unknown-type>";
}

std::string_view toString(BinaryOp op) {
  switch (op) {
    case BinaryOp::Add:
      return "+";
    case BinaryOp::Sub:
      return "-";
    case BinaryOp::Mul:
      return "*";
    case BinaryOp::Div:
      return "/";
    case BinaryOp::Mod:
      return "%";
    case BinaryOp::Eq:
      return "==";
    case BinaryOp::Ne:
      return "!=";
    case BinaryOp::Lt:
      return "<";
    case BinaryOp::Gt:
      return ">";
    case BinaryOp::Le:
      return "<=";
    case BinaryOp::Ge:
      return ">=";
    case BinaryOp::And:
      return "&&";
    case BinaryOp::Or:
      return "||";
  }
  return "<unknown-op>";
}

std::string_view toString(UnaryOp op) {
  switch (op) {
    case UnaryOp::Neg:
      return "-";
    case UnaryOp::Not:
      return "!";
  }
  return "<unknown-op>";
}

}  // namespace optiforge
