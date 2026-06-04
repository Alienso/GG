//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "GG.h"
#include "parser/AstPrinter.h"

#include <filesystem>
#include <fstream>
#include <iostream>

GG::GG(std::vector<std::string>& paths) : paths_(paths), lexer(paths) {

}

void GG::run() {
    lexer.lex();

    // Create the build directory in the current working directory once.
    namespace fs = std::filesystem;
    const fs::path buildDir = fs::current_path() / "build";
    std::error_code ec;
    fs::create_directories(buildDir, ec);
    if (ec) {
        std::cerr << "Error: cannot create build directory: " << ec.message() << "\n";
        return;
    }

    for (size_t fi = 0; fi < lexer.tokensForFiles.size(); ++fi) {
        const auto& fileTokens = lexer.tokensForFiles[fi];

        const std::string stem = fs::path(paths_[fi]).stem().string();
        std::ofstream astFile(buildDir / (stem + ".ast"));
        std::ofstream irFile (buildDir / (stem + ".ll"));

        if (!astFile) {
            std::cerr << "Error: cannot open " << (buildDir / (stem + ".ast")).string() << " for writing\n";
            continue;
        }

        if (!irFile) {
            std::cerr << "Error: cannot open " << (buildDir / (stem + ".ll")).string() << " for writing\n";
            continue;
        }

        Program ast = parser.parse(fileTokens);
        AstPrinter astPrinter;
        astPrinter.print(ast, astFile);

        SemanticResult result = semanticAnalyzer.analyze(ast);
        if (!result.hadError) {
            IRModule ir = codeGen.generate(ast, result.typeMap);
            IRPrinter irPrinter;
            irPrinter.print(ir, irFile);
        }
    }
}
