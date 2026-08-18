add_library(of_target_x86_64 STATIC
  X86Target.cpp)

target_link_libraries(of_target_x86_64 PUBLIC of_backend optiforge_options)
