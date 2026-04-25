#include "ValidationSystem.hpp"

#include "../../Shared/network/IdentityValidation.hpp"

namespace NetValidation {

std::string NormalizeIdentity(std::string identity) {
    return Shared::NetValidation::NormalizeIdentity(identity);
}

bool IsValidIdentity(const std::string &identity) {
    return Shared::NetValidation::IsValidIdentity(identity);
}

std::string NormalizeDisplayName(std::string name) {
    return Shared::NetValidation::NormalizeDisplayName(name);
}

} // namespace NetValidation
