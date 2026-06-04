//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "IRPrinter.h"

void IRPrinter::print(const IRModule& module, std::ostream& out) {
    // Global string constants
    for (const auto& global : module.globals)
        out << global << "\n";
    if (!module.globals.empty()) out << "\n";

    // Functions
    for (const auto& irFunction : module.functions) {
        out << irFunction.signature << " {\n";

        bool isEntryBlock = true;
        for (const auto& basicBlock : irFunction.blocks) {
            out << basicBlock.label << ":\n";

            // Prepend allocas to the first (entry) block only
            if (isEntryBlock) {
                for (const auto& alloca : irFunction.allocas)
                    out << alloca << "\n";
                isEntryBlock = false;
            }

            for (const auto& instruction : basicBlock.instructions)
                out << instruction << "\n";
        }

        out << "}\n\n";
    }
}
