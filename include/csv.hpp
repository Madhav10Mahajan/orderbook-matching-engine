// CSV input/output for replay files. Format (docs/01_PROBLEM_STATEMENT.md §6):
//
//   type,id,side,price,qty,tif
//   A,1,S,101.00,5,GTC      add limit (tif optional: GTC / IOC / FOK)
//   M,3,B,,4,               market
//   C,1,,,,                 cancel
//   X,2,,101.50,2,          modify (new price, new remaining qty)
//
// Blank lines, a header line starting with "type", and anything after '#' are ignored.
// Prices are decimal strings converted straight to integer ticks — never via double.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "message.hpp"
#include "types.hpp"

namespace ob {

// "101.37" with 2 decimals -> 10137. Rejects malformed input and prices with more
// decimal places than the tick allows (not on the price grid).
std::optional<Price> parse_price(std::string_view text, int decimals);

// 10137 with 2 decimals -> "101.37"; -5 -> "-0.05".
std::string format_price(Price ticks, int decimals);

enum class ParseResult { Message, Skip, Error };

// Parses one line. On Error, `error` says why.
ParseResult parse_line(std::string_view line, int decimals, Message& out, std::string& error);

std::string to_csv(const Message& m, int decimals);

}  // namespace ob
