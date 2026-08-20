add_library(of_support STATIC
  SourceManager.cpp
  Diagnostic.cpp
  PgoControls.cpp)

target_link_libraries(of_support PUBLIC optiforge_options)

# of_support depends on nothing else, by design (architectural_design.md §3).
