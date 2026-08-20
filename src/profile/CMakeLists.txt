# The .prof reader, the profile model, and the hot/warm/cold classifier.
#
# Deliberately does NOT depend on of_ir (architectural_design.md section 3,
# rule 6). The profile model is plain data: keeping it that way is what lets the
# format be unit-tested standalone, and what would let the reader be shared with
# libofprof if the format ever needed a writer on this side.
add_library(of_profile STATIC
  Profile.cpp
  Report.cpp)

target_link_libraries(of_profile PUBLIC of_support optiforge_options)
