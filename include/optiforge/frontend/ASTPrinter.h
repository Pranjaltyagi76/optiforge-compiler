#pragma once

#include <iosfwd>
#include <vector>

#include "optiforge/frontend/AST.h"
#include "optiforge/frontend/Token.h"

namespace optiforge {

/// Renders a token stream for `--emit=tokens`.
///
/// One token per line, columns aligned, no box drawing or other non-ASCII:
/// golden tests compare this byte-for-byte across platforms.
void printTokens(const std::vector<Token>& tokens, std::ostream& out);

/// Renders an AST for `--emit=ast` as an indented tree.
///
/// Child roles are labelled ("cond:", "then:", "else:") so the shape stays
/// readable without needing the grammar open alongside. Phase 2 will extend
/// each expression line with its resolved type.
void printAST(const Program& program, std::ostream& out);

}  // namespace optiforge
