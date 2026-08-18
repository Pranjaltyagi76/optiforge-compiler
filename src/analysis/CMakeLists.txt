add_library(of_analysis STATIC
  Dominators.cpp
  LoopInfo.cpp
  Liveness.cpp
  AnalysisPrinter.cpp
  SSAVerifier.cpp)

# Analyses read the IR and never mutate it (architectural_design.md section 3,
# rule 4). The one mutation that lives here, insertLoopPreheaders, is named and
# documented as a transform rather than an analysis.
target_link_libraries(of_analysis PUBLIC of_ir of_support optiforge_options)
