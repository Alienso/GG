cmake_minimum_required(VERSION 3.26)
project(GG)

set(CMAKE_CXX_STANDARD 23)

# ---- Source files shared between the main executable and the test suite ----
set(GG_SOURCES
        source/lexer/Lexer.cpp  source/lexer/Lexer.h
        source/lexer/Token.cpp  source/lexer/Token.h
        source/parser/Parser.cpp  source/parser/Parser.h
        source/parser/Ast.h
        source/parser/AstPrinter.h  source/parser/AstPrinter.cpp
        source/semantic/Type.h        source/semantic/Type.cpp
        source/semantic/SymbolTable.h  source/semantic/SymbolTable.cpp
        source/semantic/SemanticAnalyzer.h  source/semantic/SemanticAnalyzer.cpp
        source/codegen/IR.h
        source/codegen/CodeGen.h      source/codegen/CodeGen.cpp
        source/codegen/IRPrinter.h    source/codegen/IRPrinter.cpp
)

# ---- Main executable ----
add_executable(GG main.cpp
        source/GG.cpp  source/GG.h
        ${GG_SOURCES}
)

# ---- Catch2 (downloaded and built automatically on first configure) ----
include(FetchContent)
FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.15.0
)
FetchContent_MakeAvailable(Catch2)

# ---- Test executable ----
enable_testing()

add_executable(GG_tests
        tests/lexer_tests.cpp
        tests/parser_tests.cpp
        tests/semantic_tests.cpp
        tests/codegen_tests.cpp
        ${GG_SOURCES}
)
target_link_libraries(GG_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(GG_tests PRIVATE ${CMAKE_SOURCE_DIR})

# Wire Catch2 tests into CTest so "Run All Tests" works in CLion.
# On MinGW, pass the compiler's bin directory as a DLL search path so the
# test executable can find libstdc++ and libgcc at discovery time.
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
if(MINGW)
    get_filename_component(MINGW_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    catch_discover_tests(GG_tests DL_PATHS "${MINGW_BIN_DIR}")
else()
    catch_discover_tests(GG_tests)
endif()
