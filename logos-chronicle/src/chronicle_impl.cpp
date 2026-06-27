#include "chronicle_impl.h"

ChronicleImpl::ChronicleImpl() {}
ChronicleImpl::~ChronicleImpl() {}

std::string ChronicleImpl::echo(const std::string& input) {
    return "echo: " + input;
}
