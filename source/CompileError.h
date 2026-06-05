#pragma once
#include <stdexcept>
#include <string>

// Thrown by any compilation stage on the first detected error.
// Propagates to GG::run() which catches it, prints the message, and aborts.
class CompileError : public std::runtime_error {
public:
    explicit CompileError(const std::string& message) : std::runtime_error(message) {}
};
