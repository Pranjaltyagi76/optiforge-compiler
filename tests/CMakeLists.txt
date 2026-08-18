# --- Unit tests -------------------------------------------------------------
add_executable(of_unit_tests
  unit/TestMain.cpp
  unit/test_support.cpp
  unit/test_lexer.cpp
  unit/test_parser.cpp
  unit/test_sema.cpp
  unit/test_ir.cpp
  unit/test_irgen.cpp
  unit/test_codegen.cpp
  unit/test_analysis.cpp)
target_link_libraries(of_unit_tests PRIVATE
  of_target_x86_64 of_backend of_analysis of_irgen of_ir of_frontend of_support optiforge_options)
target_include_directories(of_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")

add_test(NAME unit COMMAND of_unit_tests)
# A parser bug can manifest as an infinite loop; fail fast rather than hang CI.
set_tests_properties(unit PROPERTIES TIMEOUT 60)

# --- Golden-file tests ------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  add_test(NAME golden
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/run_golden.py"
            --optiforge "$<TARGET_FILE:optiforge>"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/golden"
            --workdir "${CMAKE_SOURCE_DIR}")
  # --- End-to-end tests -----------------------------------------------------
  #
  # Run at every optimization level. The same program must produce the same
  # output at each, so this is also the differential test category (QA-05) --
  # the tests that will cover every optimization pass added in Phase 7 without
  # anyone writing a new case.
  add_test(NAME e2e
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/run_e2e.py"
            --optiforge "$<TARGET_FILE:optiforge>"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/e2e"
            "--levels=-O0,-O1,-O2")
  set_tests_properties(e2e PROPERTIES TIMEOUT 300)
else()
  message(WARNING
    "Python 3 not found; golden-file and end-to-end tests will be skipped. "
    "Unit tests still run.")
endif()
