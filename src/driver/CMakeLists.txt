add_executable(optiforge
  main.cpp
  Options.cpp
  Toolchain.cpp)

target_link_libraries(optiforge PRIVATE
  of_target_x86_64 of_backend of_analysis of_irgen of_ir of_frontend of_support optiforge_options)

# The compiler cannot be run until the target runtime it links programs against
# has been built.
add_dependencies(optiforge ofrt)
