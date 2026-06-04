#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Literals
// ============================================================

TEST_CASE("Lexer - integer literal", "[lexer]") {
    auto tokens = lexString("42");
    REQUIRE(tokens[0].type   == TokenType::NUMBER);
    REQUIRE(tokens[0].lexeme == "42");
}

TEST_CASE("Lexer - float literal", "[lexer]") {
    auto tokens = lexString("3.14");
    REQUIRE(tokens[0].type   == TokenType::NUMBER);
    REQUIRE(tokens[0].lexeme == "3.14");
    // The semantic analyser distinguishes int vs float by the presence of '.'
    REQUIRE(tokens[0].lexeme.find('.') != std::string::npos);
}

TEST_CASE("Lexer - string literal strips quotes", "[lexer]") {
    auto tokens = lexString("\"hello\"");
    REQUIRE(tokens[0].type   == TokenType::STRING);
    REQUIRE(tokens[0].lexeme == "hello");
}

TEST_CASE("Lexer - char literal strips quotes", "[lexer]") {
    auto tokens = lexString("'A'");
    REQUIRE(tokens[0].type   == TokenType::CHAR);
    REQUIRE(tokens[0].lexeme == "A");
}

TEST_CASE("Lexer - boolean keywords", "[lexer]") {
    auto tokens = lexString("true false");
    REQUIRE(tokens[0].type == TokenType::TRUE);
    REQUIRE(tokens[1].type == TokenType::FALSE);
}

// ============================================================
// Comments
// ============================================================

TEST_CASE("Lexer - line comment is ignored", "[lexer]") {
    // Only the number before the comment and EOF should be produced
    auto tokens = lexString("99 // this is a comment\n");
    REQUIRE(tokens.size() == 2);
    REQUIRE(tokens[0].type   == TokenType::NUMBER);
    REQUIRE(tokens[0].lexeme == "99");
    REQUIRE(tokens[1].type   == TokenType::END_OF_FILE);
}

TEST_CASE("Lexer - block comment is ignored", "[lexer]") {
    auto tokens = lexString("1 /* multi\nline */ 2");
    REQUIRE(tokens.size() == 3); // 1, 2, EOF
    REQUIRE(tokens[0].lexeme == "1");
    REQUIRE(tokens[1].lexeme == "2");
}

// ============================================================
// Type keywords
// ============================================================

TEST_CASE("Lexer - signed integer type keywords", "[lexer]") {
    auto tokens = lexString("i8 i16 i32 i64");
    REQUIRE(tokens[0].type == TokenType::I8);
    REQUIRE(tokens[1].type == TokenType::I16);
    REQUIRE(tokens[2].type == TokenType::I32);
    REQUIRE(tokens[3].type == TokenType::I64);
}

TEST_CASE("Lexer - unsigned integer type keywords", "[lexer]") {
    auto tokens = lexString("u8 u16 u32 u64");
    REQUIRE(tokens[0].type == TokenType::U8);
    REQUIRE(tokens[1].type == TokenType::U16);
    REQUIRE(tokens[2].type == TokenType::U32);
    REQUIRE(tokens[3].type == TokenType::U64);
}

TEST_CASE("Lexer - float type keywords", "[lexer]") {
    auto tokens = lexString("f32 f64");
    REQUIRE(tokens[0].type == TokenType::F32);
    REQUIRE(tokens[1].type == TokenType::F64);
}

TEST_CASE("Lexer - other type keywords", "[lexer]") {
    auto tokens = lexString("bool char string");
    REQUIRE(tokens[0].type == TokenType::BOOL);
    REQUIRE(tokens[1].type == TokenType::CHAR_TYPE);
    REQUIRE(tokens[2].type == TokenType::STRING_TYPE);
}

// ============================================================
// Control-flow keywords
// ============================================================

TEST_CASE("Lexer - control flow keywords", "[lexer]") {
    auto tokens = lexString("if else while for return");
    REQUIRE(tokens[0].type == TokenType::IF);
    REQUIRE(tokens[1].type == TokenType::ELSE);
    REQUIRE(tokens[2].type == TokenType::WHILE);
    REQUIRE(tokens[3].type == TokenType::FOR);
    REQUIRE(tokens[4].type == TokenType::RETURN);
}

// ============================================================
// Operators
// ============================================================

TEST_CASE("Lexer - arithmetic operators and their compound forms", "[lexer]") {
    auto tokens = lexString("+ ++ += - -- -= * *= / /= % %=");
    REQUIRE(tokens[0].type  == TokenType::PLUS);
    REQUIRE(tokens[1].type  == TokenType::INCREMENT);
    REQUIRE(tokens[2].type  == TokenType::PLUS_EQUAL);
    REQUIRE(tokens[3].type  == TokenType::MINUS);
    REQUIRE(tokens[4].type  == TokenType::DECREMENT);
    REQUIRE(tokens[5].type  == TokenType::MINUS_EQUAL);
    REQUIRE(tokens[6].type  == TokenType::STAR);
    REQUIRE(tokens[7].type  == TokenType::STAR_EQUAL);
    REQUIRE(tokens[8].type  == TokenType::SLASH);
    REQUIRE(tokens[9].type  == TokenType::SLASH_EQUAL);
    REQUIRE(tokens[10].type == TokenType::PERCENT);
    REQUIRE(tokens[11].type == TokenType::PERCENT_EQUAL);
}

TEST_CASE("Lexer - comparison and equality operators", "[lexer]") {
    auto tokens = lexString("< <= > >= == !=");
    REQUIRE(tokens[0].type == TokenType::LESS);
    REQUIRE(tokens[1].type == TokenType::LESS_EQUAL);
    REQUIRE(tokens[2].type == TokenType::GREATER);
    REQUIRE(tokens[3].type == TokenType::GREATER_EQUAL);
    REQUIRE(tokens[4].type == TokenType::EQUAL_EQUAL);
    REQUIRE(tokens[5].type == TokenType::BANG_EQUAL);
}

TEST_CASE("Lexer - logical and bitwise operators", "[lexer]") {
    auto tokens = lexString("&& || & | ^ ~ << >>");
    REQUIRE(tokens[0].type == TokenType::AND);
    REQUIRE(tokens[1].type == TokenType::OR);
    REQUIRE(tokens[2].type == TokenType::AMPERSAND);
    REQUIRE(tokens[3].type == TokenType::PIPE);
    REQUIRE(tokens[4].type == TokenType::CARET);
    REQUIRE(tokens[5].type == TokenType::TILDE);
    REQUIRE(tokens[6].type == TokenType::SHIFT_LEFT);
    REQUIRE(tokens[7].type == TokenType::SHIFT_RIGHT);
}

TEST_CASE("Lexer - arrow operator", "[lexer]") {
    auto tokens = lexString("->");
    REQUIRE(tokens[0].type   == TokenType::ARROW);
    REQUIRE(tokens[0].lexeme == "->");
}

// ============================================================
// Line tracking
// ============================================================

TEST_CASE("Lexer - line numbers are tracked across newlines", "[lexer]") {
    auto tokens = lexString("i32\n\nx");
    REQUIRE(tokens[0].line == 1); // i32
    REQUIRE(tokens[1].line == 3); // x (two newlines later)
}

// ============================================================
// Identifier vs keyword
// ============================================================

TEST_CASE("Lexer - identifier that starts like a keyword is not a keyword", "[lexer]") {
    auto tokens = lexString("i32value");
    // Should be a single IDENTIFIER, not I32 + something
    REQUIRE(tokens[0].type   == TokenType::IDENTIFIER);
    REQUIRE(tokens[0].lexeme == "i32value");
}
