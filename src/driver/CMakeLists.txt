add_executable(optiforge
  main.cpp
  Options.cpp)

target_link_libraries(optiforge PRIVATE of_irgen of_ir of_frontend of_support optiforge_options)
