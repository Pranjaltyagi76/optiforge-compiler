add_library(of_analysis STATIC
  Dominators.cpp
  LoopInfo.cpp
  Liveness.cpp
  AnalysisPrinter.cpp
  SSAVerifier.cpp)

# Analyses read the IR and never mutate it (architectural_design.md section 3,
# rule 4). The one mutation that lives here, insertLoopPreheaders, is named and
# documented as a transform rather than an analysis.
# of_profile arrives in Phase 10, for ProfileAnalysis -- the binding that makes
# a loaded profile queryable by a pass. The profile model itself stays free of
# any IR dependency (rule 6); only the binding knows about both.
target_link_libraries(of_analysis PUBLIC of_ir of_profile of_support optiforge_options)
