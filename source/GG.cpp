//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "GG.h"
#include "CompileError.h"
#include "parser/AstPrinter.h"

#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

GG::GG(std::vector<std::string>& inputPaths, CompilerOptions opts) : paths(inputPaths), options(opts) {}

int GG::run() {
    const fs::path buildDir = fs::current_path() / "build";
    std::error_code errorCode;
    fs::create_directories(buildDir, errorCode);
    if (errorCode) {
        std::cerr << "Error: cannot create build directory: " << errorCode.message() << "\n";
        return 1;
    }

    int exitCode = 0;
    for (const std::string& filePath : paths) {
        try {
            // Run the entire front end *before* creating any output file, so a failed
            // compile never leaves a stale or empty .ll behind for the linker to pick up
            // (which previously surfaced as a baffling `undefined symbol: WinMain`).
            ImportResolver resolver;
            Program ast = resolver.resolve(filePath);

            SemanticResult result = semanticAnalyzer.analyze(ast, filePath, options);
            if (result.hadError) { exitCode = 1; continue; }   // errors already reported

            IRModule ir = codeGenerator.generate(ast, result, options);

            const std::string stem    = fs::path(filePath).stem().string();
            const fs::path    astPath = buildDir / (stem + ".ast");
            const fs::path    irPath  = buildDir / (stem + ".ll");
            std::ofstream astFile(astPath);
            std::ofstream irFile(irPath);
            if (!astFile) { std::cerr << "Error: cannot open " << astPath << " for writing\n"; exitCode = 1; continue; }
            if (!irFile)  { std::cerr << "Error: cannot open " << irPath  << " for writing\n"; exitCode = 1; continue; }

            AstPrinter astPrinter;
            astPrinter.print(ast, astFile);
            IRPrinter irPrinter;
            irPrinter.print(ir, irFile);
        } catch (const CompileError& e) {
            std::cerr << e.what() << '\n';   // the single print of a thrown compile error
            exitCode = 1;
        }
    }
    return exitCode;
}
