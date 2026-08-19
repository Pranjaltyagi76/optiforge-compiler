add_library(of_passes STATIC
  PassManager.cpp)

target_link_libraries(of_passes PUBLIC of_analysis of_ir of_support optiforge_options)
