#pragma once

#include <iosfwd>

namespace optiforge::ir {

class Module;

/// Textual IR, the format `--emit=ir` produces.
///
/// Diff-stable by construction: no addresses, no hash-map iteration, and
/// sequential temporary names, so the same input always renders identically
/// (NFR-06). That is what makes golden-file testing possible.
void printModule(const Module& module, std::ostream& out);

/// Graphviz DOT of every function's control-flow graph, for `--emit=cfg`.
/// Render with:  optiforge x.of --emit=cfg | dot -Tpng -o cfg.png
void printCFG(const Module& module, std::ostream& out);

}  // namespace optiforge::ir
