//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#ifndef GG_IR_PRINTER_H
#define GG_IR_PRINTER_H

#include <ostream>
#include <string>
#include "IR.h"

class IRPrinter {
public:
    void print(const IRModule& module, std::ostream& out);
};


#endif