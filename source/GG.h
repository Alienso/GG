//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#ifndef GG_GG_H
#define GG_GG_H

#include "ImportResolver.h"
#include "semantic/SemanticAnalyzer.h"
#include "codegen/CodeGen.h"
#include "codegen/IRPrinter.h"

#include <vector>
#include <string>


class GG {
public:
    explicit GG(std::vector<std::string>& paths);
    void run();
private:
    std::vector<std::string> paths;
    SemanticAnalyzer         semanticAnalyzer;
    CodeGen                  codeGenerator;
};


#endif //GG_GG_H
