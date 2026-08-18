# The target runtime. Compiled for the platform OptiForge emits code for, and
# linked into compiled programs -- never into the compiler itself.
add_library(ofrt STATIC ofrt/ofrt.c)

set_target_properties(ofrt PROPERTIES
  OUTPUT_NAME ofrt
  # The driver looks here relative to its own location; see deployment.md 5.
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/optiforge")

# No optiforge_options here on purpose: those are host C++ flags.
target_compile_options(ofrt PRIVATE -O2)
