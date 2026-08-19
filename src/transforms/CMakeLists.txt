add_library(of_transforms STATIC
  Mem2Reg.cpp
  SSADestruction.cpp
  ScalarPasses.cpp
  SCCP.cpp
  GVN.cpp
  LICM.cpp
  SimplifyCFG.cpp
  Inline.cpp)

# Transforms mutate the IR and consume analyses; they never reference one
# another (architectural_design.md section 3, rule 5).
target_link_libraries(of_transforms PUBLIC of_passes of_analysis of_ir of_support optiforge_options)
