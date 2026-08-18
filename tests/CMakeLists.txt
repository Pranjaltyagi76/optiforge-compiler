# --- Unit tests -------------------------------------------------------------
add_executable(of_unit_tests unit/test_support.cpp)
target_link_libraries(of_unit_tests PRIVATE of_support optiforge_options)
target_include_directories(of_unit_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/unit")

add_test(NAME unit COMMAND of_unit_tests)

# --- Golden-file tests ------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  add_test(NAME golden
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/run_golden.py"
            --optiforge "$<TARGET_FILE:optiforge>"
            --dir "${CMAKE_CURRENT_SOURCE_DIR}/golden")
else()
  message(WARNING
    "Python 3 not found; golden-file tests will be skipped. "
    "Unit tests still run.")
endif()
