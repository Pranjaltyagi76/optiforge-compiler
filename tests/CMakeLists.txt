# --- Unit tests -------------------------------------------------------------
add_executable(of_unit_tests
  unit/TestMain.cpp
  unit/test_support.cpp
  unit/test_lexer.cpp
  unit/test_parser.cpp
  unit/test_sema.cpp)
target_link_libraries(of_unit_tests PRIVATE of_frontend of_support optiforge_options)
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
else()
  message(WARNING
    "Python 3 not found; golden-file tests will be skipped. "
    "Unit tests still run.")
endif()
