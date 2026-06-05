//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "Lexer.h"
#include <iostream>
#include <ranges>
#include <string>
Lexer::Lexer(std::vector<std::string> &paths){

    inputFiles.reserve(paths.size());
    filenames.reserve(paths.size());
    tokensForFiles.resize(paths.size());

    for (const auto& path : paths){
        filenames.push_back(path);
        inputFiles.emplace_back(path);

        std::ifstream& newFile = inputFiles[inputFiles.size() - 1];
        if (!newFile.is_open()) {
            throw CompileError("error: cannot open file: " + path);
        }
    }
}

void Lexer::lex() {
    for (size_t i = 0; i < inputFiles.size(); i++) {
        processFile(inputFiles[i], tokensForFiles[i], filenames[i]);
    }
}

void Lexer::processFile(std::ifstream &file, std::vector<Token>& tokens, const std::string& filename) {

    tokens.reserve(1024);

    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    int    line    = 1;
    size_t current = 0;
    size_t length  = source.size();

    auto peek     = [&]() -> char { return current < length ? source[current] : '\0'; };
    auto peekNext = [&]() -> char { return (current + 1) < length ? source[current + 1] : '\0'; };
    auto advance  = [&]() -> char { return source[current++]; };
    auto match    = [&](char expected) -> bool {
        if (current >= length || source[current] != expected) return false;
        current++;
        return true;
    };

    while (current < length) {
        size_t start = current;
        char c = advance();

        switch (c) {
            // --- single-character tokens ---
            case '(': tokens.emplace_back(TokenType::LEFT_PAREN,    "(", line); break;
            case ')': tokens.emplace_back(TokenType::RIGHT_PAREN,   ")", line); break;
            case '{': tokens.emplace_back(TokenType::LEFT_BRACE,    "{", line); break;
            case '}': tokens.emplace_back(TokenType::RIGHT_BRACE,   "}", line); break;
            case '[': tokens.emplace_back(TokenType::LEFT_BRACKET,  "[", line); break;
            case ']': tokens.emplace_back(TokenType::RIGHT_BRACKET, "]", line); break;
            case ',': tokens.emplace_back(TokenType::COMMA,    ",", line); break;
            case '.': tokens.emplace_back(TokenType::DOT,      ".", line); break;
            case ';': tokens.emplace_back(TokenType::SEMICOLON, ";", line); break;
            case '~': tokens.emplace_back(TokenType::TILDE,    "~", line); break;
            case '?': tokens.emplace_back(TokenType::QUESTION, "?", line); break;
            case ':': tokens.emplace_back(TokenType::COLON,    ":", line); break;

            // --- one-or-two character tokens ---
            case '!': {
                bool eq = match('=');
                tokens.emplace_back(eq ? TokenType::BANG_EQUAL : TokenType::BANG,
                                    eq ? "!=" : "!", line);
                break;
            }
            case '=':
                tokens.emplace_back(match('=') ? TokenType::EQUAL_EQUAL : TokenType::EQUAL,
                                    source.substr(start, current - start), line);
                break;
            case '+': {
                if      (match('+')) tokens.emplace_back(TokenType::INCREMENT,  "++", line);
                else if (match('=')) tokens.emplace_back(TokenType::PLUS_EQUAL, "+=", line);
                else                 tokens.emplace_back(TokenType::PLUS,       "+",  line);
                break;
            }
            case '-': {
                if      (match('-')) tokens.emplace_back(TokenType::DECREMENT,   "--", line);
                else if (match('=')) tokens.emplace_back(TokenType::MINUS_EQUAL, "-=", line);
                else if (match('>')) tokens.emplace_back(TokenType::ARROW,       "->", line);
                else                 tokens.emplace_back(TokenType::MINUS,       "-",  line);
                break;
            }
            case '*': {
                if (match('=')) tokens.emplace_back(TokenType::STAR_EQUAL, "*=", line);
                else            tokens.emplace_back(TokenType::STAR,       "*",  line);
                break;
            }
            case '%': {
                if (match('=')) tokens.emplace_back(TokenType::PERCENT_EQUAL, "%=", line);
                else            tokens.emplace_back(TokenType::PERCENT,       "%",  line);
                break;
            }
            case '^': {
                if (match('=')) tokens.emplace_back(TokenType::CARET_EQUAL, "^=", line);
                else            tokens.emplace_back(TokenType::CARET,       "^",  line);
                break;
            }
            case '&': {
                if      (match('&')) tokens.emplace_back(TokenType::AND,              "&&", line);
                else if (match('=')) tokens.emplace_back(TokenType::AMPERSAND_EQUAL,  "&=", line);
                else                 tokens.emplace_back(TokenType::AMPERSAND,        "&",  line);
                break;
            }
            case '|': {
                if      (match('|')) tokens.emplace_back(TokenType::OR,         "||", line);
                else if (match('=')) tokens.emplace_back(TokenType::PIPE_EQUAL, "|=", line);
                else                 tokens.emplace_back(TokenType::PIPE,       "|",  line);
                break;
            }
            case '<': {
                if      (match('<')) tokens.emplace_back(TokenType::SHIFT_LEFT,  "<<", line);
                else if (match('=')) tokens.emplace_back(TokenType::LESS_EQUAL,  "<=", line);
                else                 tokens.emplace_back(TokenType::LESS,        "<",  line);
                break;
            }
            case '>': {
                if      (match('>')) tokens.emplace_back(TokenType::SHIFT_RIGHT,   ">>", line);
                else if (match('=')) tokens.emplace_back(TokenType::GREATER_EQUAL, ">=", line);
                else                 tokens.emplace_back(TokenType::GREATER,       ">",  line);
                break;
            }

            // --- slash, comments ---
            case '/':
                if (match('/')) {
                    while (peek() != '\n' && current < length) advance();
                } else if (match('*')) {
                    int commentLine = line;
                    bool terminated = false;
                    while (current < length) {
                        if (peek() == '\n') line++;
                        if (peek() == '*' && peekNext() == '/') { advance(); advance(); terminated = true; break; }
                        advance();
                    }
                    if (!terminated) {
                        throw CompileError(filename + ":" + std::to_string(commentLine) + ": error: unterminated block comment");
                    }
                } else if (match('=')) {
                    tokens.emplace_back(TokenType::SLASH_EQUAL, "/=", line);
                } else {
                    tokens.emplace_back(TokenType::SLASH, "/", line);
                }
                break;

            // --- string literals ---
            case '"': {
                while (peek() != '"' && current < length) {
                    if (peek() == '\n') line++;
                    advance();
                }
                if (current >= length) {
                    throw CompileError(filename + ":" + std::to_string(line) + ": error: unterminated string literal");
                }
                advance(); // closing "
                tokens.emplace_back(TokenType::STRING,
                                    source.substr(start + 1, current - start - 2), line);
                break;
            }

            // --- char literals ---
            case '\'': {
                if (peek() == '\'') {
                    throw CompileError(filename + ":" + std::to_string(line) + ": error: empty char literal");
                }
                char ch = advance();
                if (ch == '\\') advance(); // escape sequence (e.g. '\n')
                if (peek() != '\'') {
                    throw CompileError(filename + ":" + std::to_string(line) + ": error: unterminated or multi-character char literal");
                }
                advance(); // closing '
                tokens.emplace_back(TokenType::CHAR,
                                    source.substr(start + 1, current - start - 2), line);
                break;
            }

            // --- whitespace ---
            case ' ': case '\r': case '\t': break;
            case '\n': line++; break;

            default:
                if (isdigit(c)) {
                    while (isdigit(peek())) advance();
                    if (peek() == '.' && isdigit(peekNext())) {
                        advance(); // consume '.'
                        while (isdigit(peek())) advance();
                    }
                    tokens.emplace_back(TokenType::NUMBER,
                                        source.substr(start, current - start), line);
                } else if (isalpha(c) || c == '_') {
                    while (isalnum(peek()) || peek() == '_') advance();
                    std::string text = source.substr(start, current - start);
                    TokenType type = lookupKeyword(text);
                    tokens.emplace_back(type, text, line);
                } else {
                    throw CompileError(filename + ":" + std::to_string(line) + ": error: unexpected character '" + c + "'");
                }
                break;
        }
    }

    tokens.emplace_back(TokenType::END_OF_FILE, "", line);
}
