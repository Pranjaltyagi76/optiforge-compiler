#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace optiforge::ir {

class Module;
class Function;

/// Structural validation of the IR.
///
/// Runs after IR generation and after every pass in debug builds. Its purpose
/// is to turn "some later stage crashed mysteriously" into "pass X produced
/// invalid IR", which is the difference between a ten-minute bug and a two-day
/// one (architectural_design.md section 7.4).
class Verifier {
public:
  /// Human-readable problems found. Empty means the IR is well-formed.
  const std::vector<std::string>& errors() const { return errors_; }

  bool verify(const Module& module);
  bool verify(const Function& function);

  /// Writes every error, one per line, prefixed with the offending location.
  void printErrors(std::ostream& out) const;

private:
  void checkFunction(const Function& function);
  void report(const std::string& context, const std::string& message);

  std::vector<std::string> errors_;
};

}  // namespace optiforge::ir
