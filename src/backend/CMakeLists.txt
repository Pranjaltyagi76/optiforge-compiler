add_library(of_backend STATIC
  MachineIR.cpp
  CodeGen.cpp
  AsmPrinter.cpp)

# The backend sees IR and nothing else -- never of_frontend
# (architectural_design.md section 3, rule 3).
target_link_libraries(of_backend PUBLIC of_ir of_support optiforge_options)
