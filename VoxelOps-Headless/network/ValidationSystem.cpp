#include "ValidationSystem.hpp"

#include "../../Shared/network/Packets.hpp"

#include <cctype>

namespace {

bool IsValidIdentityChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool IsValidDisplayNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

} // namespace

namespace NetValidation {

std::string NormalizeIdentity(std::string identity) {
    std::string out;
    out.reserve(identity.size());
    for (char c : identity) {
        const char lower = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (IsValidIdentityChar(lower)) {
            out.push_back(lower);
        }
    }
    if (out.size() > kMaxConnectIdentityChars) {
        out.resize(kMaxConnectIdentityChars);
    }
    return out;
}

bool IsValidIdentity(const std::string &identity) {
    if (identity.empty() || identity.size() > kMaxConnectIdentityChars) {
        return false;
    }
    for (char c : identity) {
        if (!IsValidIdentityChar(c)) {
            return false;
        }
    }
    return true;
}

std::string NormalizeDisplayName(std::string name) {
    size_t begin = 0;
    while (begin < name.size() && std::isspace(static_cast<unsigned char>(name[begin])) != 0) {
        ++begin;
    }
    size_t end = name.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1])) != 0) {
        --end;
    }

    std::string out;
    out.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        const char c = name[i];
        if (IsValidDisplayNameChar(c)) {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxConnectUsernameChars) {
        out.resize(kMaxConnectUsernameChars);
    }
    return out;
}

} // namespace NetValidation
