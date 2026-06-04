//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include "IRPrinter.h"

void IRPrinter::print(const IRModule& module, std::ostream& out) {
    // Global string constants
    for (const auto& g : module.globals) {
        out << g << "\n";
    }
    if (!module.globals.empty()) out << "\n";

    // Functions
    for (const auto& fn : module.functions) {
        out << fn.signature << " {\n";

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const BasicBlock& bb = fn.blocks[bi];
            out << bb.label << ":\n";

            // Prepend allocas to the first (entry) block only
            if (bi == 0) {
                for (const auto& alloca : fn.allocas) {
                    out << alloca << "\n";
                }
            }

            for (const auto& instr : bb.instructions) {
                out << instr << "\n";
            }
        }

        out << "}\n\n";
    }
}
