cmake_minimum_required(VERSION 3.26)
project(GG)

set(CMAKE_CXX_STANDARD 23)

# ---- Parallel build default ----
# `cmake --build` reads the CMAKE_BUILD_PARALLEL_LEVEL *environment* variable to
# decide how many compile jobs to run concurrently. Default it to the number of
# logical cores unless the user already set it, so plain `cmake --build` is
# parallel without needing an explicit -j/--parallel flag.
# Note: this only takes effect for build invocations made *after* this configure
# step runs in the same environment; passing --parallel on the command line, or
# using the Ninja generator (parallel by default), are the most reliable options.
if(NOT DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    cmake_host_system_information(RESULT GG_NPROC QUERY NUMBER_OF_LOGICAL_CORES)
    set(ENV{CMAKE_BUILD_PARALLEL_LEVEL} ${GG_NPROC})
    message(STATUS "GG: defaulting CMAKE_BUILD_PARALLEL_LEVEL to ${GG_NPROC} logical cores")
endif()

# ---- Compiler diagnostics ----
# Applied to every translation unit in this project (not to Catch2, which is a
# separate CMake target compiled with its own flags).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(GG_WARNING_FLAGS
        -Wall                   # broad baseline set
        -Wextra                 # extra useful checks
        -Wpedantic              # strict ISO conformance
        -Wshadow                # local variable shadows outer one
        -Wnon-virtual-dtor      # base class with virtual functions but non-virtual dtor
        -Wold-style-cast        # C-style casts
        -Wcast-align            # pointer cast increases required alignment
        -Woverloaded-virtual    # derived-class hides base virtual
        -Wnull-dereference      # statically-detected null dereference (GCC 6+)
        -Wdouble-promotion      # implicit float→double widening
        -Wimplicit-fallthrough  # switch case falls through without annotation
    )
endif()

# ---- Sanitizers (opt-in, Debug builds recommended) ----
# Enable with:  cmake -DGG_SANITIZERS=ON ...
# On MinGW-w64 GCC 13 both ASan and UBSan are supported.
# Make sure the MinGW bin directory is on PATH at runtime so the ASan DLL is found.
option(GG_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviourSanitizer" OFF)
if(GG_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(GG_SANITIZER_FLAGS
        -fsanitize=address,undefined
        -fno-sanitize-recover=all   # treat UB as a fatal error rather than continuing
        -fno-omit-frame-pointer     # keep frame pointers for readable stack traces
    )
endif()

# ---- Source files shared between the main executable and the test suite ----
set(GG_SOURCES
        source/lexer/Lexer.cpp  source/lexer/Lexer.h
        source/lexer/Token.cpp  source/lexer/Token.h
        source/parser/Parser.cpp  source/parser/Parser.h
        source/parser/Ast.h
        source/parser/AstPrinter.h  source/parser/AstPrinter.cpp
        source/semantic/Type.h        source/semantic/Type.cpp
        source/semantic/SymbolTable.h  source/semantic/SymbolTable.cpp
        source/semantic/SemanticAnalyzer.h
        source/semantic/SemanticAnalyzer.cpp
        source/semantic/SemanticAnalyzer_Stmts.cpp
        source/semantic/SemanticAnalyzer_Exprs.cpp
        source/semantic/SemanticAnalyzer_Escape.cpp
        source/codegen/IR.h
        source/codegen/CodeGen.h
        source/codegen/CodeGen.cpp
        source/codegen/CodeGen_Functions.cpp
        source/codegen/CodeGen_Stmts.cpp
        source/codegen/CodeGen_Exprs.cpp
        source/codegen/CodeGen_Classes.cpp
        source/codegen/CodeGen_Emit.cpp
        source/codegen/CodeGen_Debug.cpp
        source/codegen/IRPrinter.h    source/codegen/IRPrinter.cpp
        source/ImportResolver.h       source/ImportResolver.cpp
)

# ---- Main executable ----
add_executable(GG main.cpp
        source/GG.cpp  source/GG.h
        ${GG_SOURCES}
)
target_compile_options(GG PRIVATE ${GG_WARNING_FLAGS} ${GG_SANITIZER_FLAGS})
target_link_options(GG    PRIVATE ${GG_SANITIZER_FLAGS})

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
        tests/import_tests.cpp
        tests/stdlib_tests.cpp
        tests/array_tests.cpp
        tests/class_tests.cpp
        tests/enum_tests.cpp
        tests/static_tests.cpp
        tests/const_tests.cpp
        tests/implicit_this_tests.cpp
        tests/overload_tests.cpp
        tests/cast_tests.cpp
        tests/trait_tests.cpp
        tests/generic_bounds_tests.cpp
        tests/return_slot_tests.cpp
        tests/refcount_tests.cpp
        tests/default_param_tests.cpp
        tests/switch_tests.cpp
        tests/lambda_tests.cpp
        tests/debug_tests.cpp
        tests/escape_tests.cpp
        tests/ref_tests.cpp
        tests/brace_init_tests.cpp
        ${GG_SOURCES}
)
target_compile_options(GG_tests PRIVATE ${GG_WARNING_FLAGS} ${GG_SANITIZER_FLAGS})
target_link_options(GG_tests    PRIVATE ${GG_SANITIZER_FLAGS})
target_link_libraries(GG_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(GG_tests PRIVATE ${CMAKE_SOURCE_DIR})
# Expose the repo root so stdlib_tests.cpp can locate the stdlib/ directory.
target_compile_definitions(GG_tests PRIVATE GG_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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
