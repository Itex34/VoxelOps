#pragma once

#include <string>

namespace NetValidation {

std::string NormalizeIdentity(std::string identity);
bool IsValidIdentity(const std::string &identity);
std::string NormalizeDisplayName(std::string name);

} // namespace NetValidation
