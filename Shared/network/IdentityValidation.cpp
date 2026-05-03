#include "IdentityValidation.hpp"

#include "Packets.hpp"

#include <cctype>

namespace {
    bool IsValidIdentityChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    bool IsValidDisplayNameChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    }

    std::string TrimAscii(std::string_view value) {
        size_t begin = 0;
        while (begin < value.size() &&
               std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
            ++begin;
        }
        size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
            --end;
        }
        return std::string(value.substr(begin, end - begin));
    }
} // namespace

namespace Shared::NetValidation {

    std::string NormalizeIdentity(std::string_view identity) {
        std::string trimmed = TrimAscii(identity);
        std::string out;
        out.reserve(trimmed.size());
        for (char c : trimmed) {
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

    bool IsValidIdentity(std::string_view identity) {
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

    std::string NormalizeDisplayName(std::string_view name) {
        std::string trimmed = TrimAscii(name);
        std::string out;
        out.reserve(trimmed.size());
        for (char c : trimmed) {
            if (IsValidDisplayNameChar(c)) {
                out.push_back(c);
            }
        }
        if (out.size() > kMaxConnectUsernameChars) {
            out.resize(kMaxConnectUsernameChars);
        }
        return out;
    }

    bool IsValidDisplayName(std::string_view name) {
        if (name.empty() || name.size() > kMaxConnectUsernameChars) {
            return false;
        }
        for (char c : name) {
            if (!IsValidDisplayNameChar(c)) {
                return false;
            }
        }
        return true;
    }

} // namespace Shared::NetValidation
