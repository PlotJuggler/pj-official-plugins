#pragma once

#include <string>
#include <string_view>
#include <system_error>

namespace pj::parser_arrow {

/// Prefix a diagnostic for the parser-facing error contract.
[[nodiscard]] inline std::string parserError(std::string_view message) {
  return "parser_arrow: " + std::string(message);
}

/// Convert an errno-style nanoarrow result into a parser-facing diagnostic.
[[nodiscard]] inline std::string nanoarrowError(int result, const char* message) {
  if (message != nullptr && message[0] != '\0') {
    return parserError(message);
  }
  return parserError(std::error_code(result, std::generic_category()).message());
}

}  // namespace pj::parser_arrow
