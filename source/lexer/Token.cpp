//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "Token.h"

#include <utility>
#include <unordered_map>

static const std::unordered_map<std::string, TokenType> keywords = {
    // Signed integer types
    {"i8",     TokenType::I8},
    {"i16",    TokenType::I16},
    {"i32",    TokenType::I32},
    {"i64",    TokenType::I64},

    // Unsigned integer types
    {"u8",     TokenType::U8},
    {"u16",    TokenType::U16},
    {"u32",    TokenType::U32},
    {"u64",    TokenType::U64},

    // Floating point types
    {"f32",    TokenType::F32},
    {"f64",    TokenType::F64},

    // Other types
    {"bool",   TokenType::BOOL},
    {"char",   TokenType::CHAR_TYPE},
    {"void",   TokenType::VOID},
    {"ptr",    TokenType::PTR},

    // Keywords
    {"extern",   TokenType::EXTERN},
    {"import",   TokenType::IMPORT},
    {"return",   TokenType::RETURN},
    {"break",    TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"if",     TokenType::IF},
    {"else",   TokenType::ELSE},
    {"for",    TokenType::FOR},
    {"while",  TokenType::WHILE},
    {"class",  TokenType::CLASS},
    {"enum",   TokenType::ENUM},
    {"true",   TokenType::TRUE},
    {"false",  TokenType::FALSE},
    {"var",    TokenType::VAR},
    {"super",  TokenType::SUPER},
    {"this",    TokenType::THIS},
    {"private", TokenType::PRIVATE},
    {"as",      TokenType::AS},
    {"new",     TokenType::NEW},
    {"sizeof",  TokenType::SIZEOF},
    {"static",  TokenType::STATIC},
    {"mut",     TokenType::MUT},
    {"trait",   TokenType::TRAIT},
    {"impl",    TokenType::IMPL},
    {"Self",    TokenType::SELF},
    {"fn",      TokenType::FN},
    {"ref",     TokenType::REF},
    {"switch",  TokenType::SWITCH},
    {"case",    TokenType::CASE},
    {"default", TokenType::DEFAULT},
    {"yield",   TokenType::YIELD}
};

TokenType lookupKeyword(const std::string& text) {
    auto it = keywords.find(text);
    return it != keywords.end() ? it->second : TokenType::IDENTIFIER;
}

Token::Token(TokenType tokenType, std::string lex, int ln) : type(tokenType), lexeme(std::move(lex)), line(ln) {}
