#include "optiforge/support/PgoControls.h"

namespace optiforge {

namespace {

/// The one table. Adding a decision means adding a row here and nothing else,
/// which is what keeps `--help`, the usage error and the attribution harness
/// from drifting apart from what the compiler actually implements.
struct Decision {
  std::string_view name;
  bool PgoControls::*field;
};

constexpr Decision kDecisions[] = {
    {"inline", &PgoControls::inlining},   {"unroll", &PgoControls::unrolling},
    {"regalloc", &PgoControls::regalloc}, {"layout", &PgoControls::layout},
    {"cold-size", &PgoControls::coldSize},
};

}  // namespace

bool disablePgoDecision(std::string_view name, PgoControls& controls) {
  for (const Decision& decision : kDecisions) {
    if (decision.name == name) {
      controls.*decision.field = false;
      return true;
    }
  }
  return false;
}

std::vector<std::string_view> disabledPgoDecisionNames(const PgoControls& controls) {
  std::vector<std::string_view> names;
  for (const Decision& decision : kDecisions) {
    if (!(controls.*decision.field)) {
      names.push_back(decision.name);
    }
  }
  return names;
}

std::vector<std::string_view> pgoDecisionNames() {
  std::vector<std::string_view> names;
  names.reserve(sizeof(kDecisions) / sizeof(kDecisions[0]));
  for (const Decision& decision : kDecisions) {
    names.push_back(decision.name);
  }
  return names;
}

}  // namespace optiforge
