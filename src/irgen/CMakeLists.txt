add_library(of_irgen STATIC
  IRGen.cpp)

# The only module that may depend on BOTH the frontend and the IR: lowering is
# its entire job (architectural_design.md section 3).
target_link_libraries(of_irgen PUBLIC of_frontend of_ir optiforge_options)
