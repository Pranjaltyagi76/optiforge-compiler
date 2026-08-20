#include "optiforge/transforms/ProfileAnnotate.h"

#include "optiforge/ir/BasicBlock.h"
#include "optiforge/ir/Function.h"
#include "optiforge/ir/Module.h"
#include "optiforge/profile/Profile.h"

namespace optiforge::transforms {

std::size_t annotateBlockCounts(ir::Module& module, const profile::ProfileData& profile) {
  if (!profile.isValid()) {
    return 0;
  }

  std::size_t annotated = 0;
  for (const auto& function : module.functions()) {
    if (function->isDeclaration()) {
      continue;
    }
    for (const auto& block : function->blocks()) {
      const std::uint64_t count = profile.blockCount(function->name(), block->label());
      if (count == 0 && profile.blockHeat(function->name(), block->label()) ==
                            profile::Heat::Unknown) {
        // The profile says nothing about this block -- it did not exist when the
        // profile was collected. Leave whatever is there: a block cloned by
        // unrolling has already inherited its original's count, and overwriting
        // that with zero would tell the backend the hot loop is dead.
        continue;
      }
      block->executionCount = count;
      ++annotated;
    }
  }
  return annotated;
}

}  // namespace optiforge::transforms
