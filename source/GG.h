//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#ifndef GG_GG_H
#define GG_GG_H

#include "ImportResolver.h"
#include "semantic/SemanticAnalyzer.h"
#include "codegen/CodeGen.h"
#include "codegen/IRPrinter.h"
#include "CompilerOptions.h"

#include <vector>
#include <string>


class GG {
public:
    explicit GG(std::vector<std::string>& paths, CompilerOptions options = {},
                ModuleSearchConfig moduleConfig = {});
    int run();   // process exit code: 0 on success, non-zero if any file failed to compile
private:
    std::vector<std::string> paths;
    SemanticAnalyzer         semanticAnalyzer;
    CodeGen                  codeGenerator;
    CompilerOptions          options;
    ModuleSearchConfig       moduleConfig;   // stdlib / search-root import resolution
};


#endif //GG_GG_H
