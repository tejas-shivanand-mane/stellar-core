#pragma once

#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace stellar
{
struct ShabdizOperation
{
    bool write;
    std::string key, value;
};
using ShabdizBatch = std::vector<ShabdizOperation>;

// Validate the existing client wire format without deserialize/serialize
// round trips or stringstreams. Keep parsed operations for execution; this
// does not change transaction semantics or skip validation of remote data.
inline std::optional<ShabdizBatch>
parseShabdizBatch(std::string_view data, size_t batchSize)
{
    if (data.empty() || data.size() > 90000 || !batchSize)
        return {};
    auto number = [](std::string_view s) {
        if (s.empty() || (s.size() > 1 && s.front() == '0'))
            return false;
        uint64_t value;
        auto result = std::from_chars(s.data(), s.data() + s.size(), value);
        return result.ec == std::errc{} && result.ptr == s.data() + s.size();
    };
    auto token = [](std::string_view& s) {
        size_t start = 0;
        while (start < s.size() &&
               std::isspace(static_cast<unsigned char>(s[start])))
            ++start;
        size_t end = start;
        while (end < s.size() &&
               !std::isspace(static_cast<unsigned char>(s[end])))
            ++end;
        auto word = s.substr(start, end - start);
        s.remove_prefix(end);
        return word;
    };
    ShabdizBatch result;
    result.reserve(batchSize);
    while (!data.empty())
    {
        auto separator = data.find('|');
        auto txn = data.substr(0, separator);
        size_t a = txn.find(':'), b = txn.find(':', a == txn.npos ? 0 : a + 1);
        size_t c = txn.find(':', b == txn.npos ? 0 : b + 1);
        if (a == txn.npos || b == txn.npos || c == txn.npos ||
            txn.find(':', c + 1) != txn.npos || !number(txn.substr(0, a)) ||
            !number(txn.substr(b + 1, c - b - 1)))
            return {};
        auto payload = txn.substr(a + 1, b - a - 1);
        auto op = token(payload), key = token(payload);
        bool write = op == "UPDATE" || op == "RMW";
        if ((!write && op != "READ") || key.empty())
            return {};
        auto value = write ? token(payload) : std::string_view{};
        if ((write && value.empty()) || !token(payload).empty())
            return {};
        result.push_back({write, std::string(key), std::string(value)});
        if (result.size() > batchSize)
            return {};
        if (separator == data.npos)
            break;
        data.remove_prefix(separator + 1);
        if (data.empty())
            return {};
    }
    if (result.size() != batchSize)
        return {};
    return result;
}
} // namespace stellar
