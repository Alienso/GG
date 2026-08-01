#pragma once

#include "Parser.h"


inline bool isTypeKeyword(TokenType t) {
    switch (t) {
        case TokenType::I8:  case TokenType::I16: case TokenType::I32: case TokenType::I64:
        case TokenType::U8:  case TokenType::U16: case TokenType::U32: case TokenType::U64:
        case TokenType::F32: case TokenType::F64:
        case TokenType::BOOL: case TokenType::CHAR_TYPE: case TokenType::VOID: case TokenType::PTR:
        case TokenType::STR:
            return true;
        default:
            return false;
    }
}

// Mangle one type-argument token slice into an LLVM-safe name fragment.
inline std::string argMangle(const std::vector<Token>& arg) {
    std::string s;
    for (const Token& t : arg) {
        if (t.type == TokenType::AMPERSAND) s += ".ref";   // Class&  ->  Class.ref
        else                                s += t.lexeme;
    }
    return s;
}