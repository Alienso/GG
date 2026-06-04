//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "GG.h"
#include "parser/AstPrinter.h"

#include <filesystem>
#include <fstream>
#include <iostream>

GG::GG(std::vector<std::string>& inputPaths) : paths(inputPaths), lexer(inputPaths) {

}

void GG::run() {
    lexer.lex();

    namespace fs = std::filesystem;
    const fs::path buildDir = fs::current_path() / "build";
    std::error_code errorCode;
    fs::create_directories(buildDir, errorCode);
    if (errorCode) {
        std::cerr << "Error: cannot create build directory: " << errorCode.message() << "\n";
        return;
    }

    for (size_t fileIndex = 0; fileIndex < lexer.tokens().size(); ++fileIndex) {
        const std::string stem    = fs::path(paths[fileIndex]).stem().string();
        const fs::path    astPath = buildDir / (stem + ".ast");
        const fs::path    irPath  = buildDir / (stem + ".ll");

        std::ofstream astFile(astPath);
        std::ofstream irFile(irPath);

        if (!astFile) { std::cerr << "Error: cannot open " << astPath << " for writing\n"; continue; }
        if (!irFile)  { std::cerr << "Error: cannot open " << irPath  << " for writing\n"; continue; }

        Program ast = parser.parse(lexer.tokens()[fileIndex]);

        AstPrinter astPrinter;
        astPrinter.print(ast, astFile);

        SemanticResult result = semanticAnalyzer.analyze(ast);
        if (!result.hadError) {
            IRModule ir = codeGenerator.generate(ast, result.typeMap);
            IRPrinter irPrinter;
            irPrinter.print(ir, irFile);
        }
    }
}
