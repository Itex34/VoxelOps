#pragma once

#include <string>
#include <string_view>

namespace Shared::NetValidation {

    std::string NormalizeIdentity(std::string_view identity);
    bool IsValidIdentity(std::string_view identity);
    std::string NormalizeDisplayName(std::string_view name);
    bool IsValidDisplayName(std::string_view name);

} // namespace Shared::NetValidation
