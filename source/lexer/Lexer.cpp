//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "Lexer.h"
#include <iostream>
#include <string>

Lexer::Lexer(std::vector<std::string> &paths){

    inputFiles.reserve(paths.size());

    for (const auto& path : paths){
        inputFiles.emplace_back(path);

        std::ifstream& newFile = inputFiles[inputFiles.size() - 1];
        if (!newFile.is_open()) {
            std::cout << "Could not open file: " << path << '\n';
            exit(1);
        }
    }
}

void Lexer::lex() {
    for (size_t i = 0; i < inputFiles.size(); i++) {
        processFile(inputFiles[i], tokensForFiles[i]);
    }
}

#define ADD_TOKEN(x) tokens.emplace_back(x, lineStr.substr(start, current) , line)

void Lexer::processFile(std::ifstream &file, std::vector<Token>& tokens) {

    tokens.reserve(1024);

    int line = 0;
    std::string lineStr;

    for(int i = 0; std::getline(file, lineStr); i++) {

        int start = 0;
        int current = 0;

        for (char c : lineStr) {

            switch (c) {
                case '(': ADD_TOKEN(TokenType::LEFT_PAREN); break;
                case ')': ADD_TOKEN(TokenType::RIGHT_PAREN); break;
                case '{': ADD_TOKEN(TokenType::LEFT_BRACE); break;
                case '}': ADD_TOKEN(TokenType::RIGHT_BRACE); break;
                case ',': ADD_TOKEN(TokenType::COMMA); break;
                case '.': ADD_TOKEN(TokenType::DOT); break;
                case '-': ADD_TOKEN(TokenType::MINUS); break;
                case '+': ADD_TOKEN(TokenType::PLUS); break;
                case ';': ADD_TOKEN(TokenType::SEMICOLON); break;
                case '*': ADD_TOKEN(TokenType::STAR); break;
                default:
                    std::cerr << "Unexpected character " << c << " at line " << line << " position " << current << '\n';
                    std::cerr << lineStr << '\n';
                    break;
            }

        }

    }

}