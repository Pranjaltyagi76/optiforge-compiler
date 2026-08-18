add_library(of_ir STATIC
  IR.cpp
  IRBuilder.cpp
  Verifier.cpp
  Printer.cpp)

# of_ir must NOT depend on of_frontend: the IR has to be constructible without
# a source language (architectural_design.md section 3, rule 1). of_irgen is the
# only module that knows both.
target_link_libraries(of_ir PUBLIC of_support optiforge_options)
