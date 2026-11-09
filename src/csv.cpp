#include "csv.hpp"

#include <charconv>
#include <cstdlib>
#include <vector>

namespace ob {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

template <class Int>
bool parse_int(std::string_view s, Int& out) {
    if (s.empty()) return false;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

}  // namespace

std::optional<Price> parse_price(std::string_view text, int decimals) {
    text = trim(text);
    if (text.empty()) return std::nullopt;
    bool negative = false;
    if (text.front() == '-' || text.front() == '+') {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    const std::size_t dot = text.find('.');
    const std::string_view whole = text.substr(0, dot);
    std::string_view frac = dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
    if (whole.empty() && frac.empty()) return std::nullopt;
    for (char c : whole) if (c < '0' || c > '9') return std::nullopt;
    for (char c : frac) if (c < '0' || c > '9') return std::nullopt;

    // Extra decimal places are allowed only if they are zeros ("101.500" with 2 decimals).
    while (static_cast<int>(frac.size()) > decimals && frac.back() == '0') frac.remove_suffix(1);
    if (static_cast<int>(frac.size()) > decimals) return std::nullopt;  // off the tick grid

    Price result = 0;
    if (!whole.empty() && !parse_int(whole, result)) return std::nullopt;
    Price frac_value = 0;
    if (!frac.empty() && !parse_int(frac, frac_value)) return std::nullopt;
    for (int i = 0; i < decimals; ++i) result *= 10;
    for (int i = static_cast<int>(frac.size()); i < decimals; ++i) frac_value *= 10;  // "5" -> 50
    result += frac_value;
    return negative ? -result : result;
}

std::string format_price(Price ticks, int decimals) {
    Price scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const bool negative = ticks < 0;
    const Price abs = negative ? -ticks : ticks;
    std::string s = (negative ? "-" : "") + std::to_string(abs / scale);
    if (decimals > 0) {
        std::string frac = std::to_string(abs % scale);
        s += "." + std::string(static_cast<std::size_t>(decimals) - frac.size(), '0') + frac;
    }
    return s;
}

ParseResult parse_line(std::string_view line, int decimals, Message& out, std::string& error) {
    if (auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty() || line.substr(0, 4) == "type") return ParseResult::Skip;

    auto f = split(line, ',');
    while (f.size() < 6) f.emplace_back();
    if (f[0].size() != 1) {
        error = "bad type field";
        return ParseResult::Error;
    }

    Message m;
    if (!parse_int(f[1], m.id)) {
        error = "bad id";
        return ParseResult::Error;
    }

    auto need_side = [&]() -> bool {
        if (f[2] == "B") m.side = Side::Buy;
        else if (f[2] == "S") m.side = Side::Sell;
        else {
            error = "side must be B or S";
            return false;
        }
        return true;
    };
    auto need_price = [&]() -> bool {
        auto p = parse_price(f[3], decimals);
        if (!p) {
            error = "bad price '" + std::string(f[3]) + "'";
            return false;
        }
        m.price = *p;
        return true;
    };
    auto need_qty = [&]() -> bool {
        if (!parse_int(f[4], m.qty)) {
            error = "bad qty";
            return false;
        }
        return true;
    };

    switch (f[0][0]) {
        case 'A':
            m.type = MsgType::Add;
            if (!need_side() || !need_price() || !need_qty()) return ParseResult::Error;
            if (f[5].empty() || f[5] == "GTC") m.tif = TimeInForce::GTC;
            else if (f[5] == "IOC") m.tif = TimeInForce::IOC;
            else if (f[5] == "FOK") m.tif = TimeInForce::FOK;
            else {
                error = "tif must be GTC, IOC or FOK";
                return ParseResult::Error;
            }
            break;
        case 'M':
            m.type = MsgType::Market;
            if (!need_side() || !need_qty()) return ParseResult::Error;
            break;
        case 'C':
            m.type = MsgType::Cancel;
            break;
        case 'X':
            m.type = MsgType::Modify;
            if (!need_price() || !need_qty()) return ParseResult::Error;
            break;
        default:
            error = "unknown message type '" + std::string(f[0]) + "'";
            return ParseResult::Error;
    }
    out = m;
    return ParseResult::Message;
}

std::string to_csv(const Message& m, int decimals) {
    const std::string id = std::to_string(m.id);
    const char* side = m.side == Side::Buy ? "B" : "S";
    switch (m.type) {
        case MsgType::Add: {
            const char* tif = m.tif == TimeInForce::GTC ? "GTC" : (m.tif == TimeInForce::IOC ? "IOC" : "FOK");
            return "A," + id + "," + side + "," + format_price(m.price, decimals) + "," +
                   std::to_string(m.qty) + "," + tif;
        }
        case MsgType::Market: return "M," + id + "," + side + ",," + std::to_string(m.qty) + ",";
        case MsgType::Cancel: return "C," + id + ",,,,";
        case MsgType::Modify:
            return "X," + id + ",," + format_price(m.price, decimals) + "," + std::to_string(m.qty) + ",";
    }
    return "";
}

}  // namespace ob
