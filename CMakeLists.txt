cmake_minimum_required(VERSION 3.31)
project(GG)

set(CMAKE_CXX_STANDARD 17)

add_executable(GG main.cpp
        source/GG.cpp source/GG.h
        source/lexer/Lexer.cpp source/lexer/Lexer.h
        source/lexer/Token.cpp
        source/lexer/Token.h
)
