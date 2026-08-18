add_library(of_frontend STATIC
  Token.cpp
  Lexer.cpp
  AST.cpp
  Type.cpp
  Symbol.cpp
  Sema.cpp
  ASTPrinter.cpp
  Parser.cpp)

# of_frontend may depend on of_support only. It must NOT depend on of_ir:
# lowering is of_irgen's job (architectural_design.md section 3, rule 2).
target_link_libraries(of_frontend PUBLIC of_support optiforge_options)
