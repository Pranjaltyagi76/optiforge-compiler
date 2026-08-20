add_library(of_backend STATIC
  MachineIR.cpp
  CodeGen.cpp
  RegAlloc.cpp
  Layout.cpp
  AsmPrinter.cpp)

# The backend sees IR and the analyses, and never of_frontend
# (architectural_design.md section 3, rule 3). of_analysis arrives in Phase 8:
# the register allocator builds its interference graph from liveness rather
# than deriving live ranges a second time, badly.
target_link_libraries(of_backend PUBLIC of_ir of_analysis of_support optiforge_options)
