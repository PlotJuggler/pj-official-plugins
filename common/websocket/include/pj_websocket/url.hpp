#pragma once

/// @file url.hpp
/// @brief The server URL a websocket streamer's dialog takes in one line, and the copy of it that
///  may be remembered.
///   - normalizeUrl(): what the user typed -> the URL to dial, or why it is not one.
///   - cleanUrl(): a URL without its credentials, the only form a streamer stores in settings or
///     shows in a status line or message.

#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <pj_base/expected.hpp>
#include <pj_base/sdk/text_utils.hpp>
#include <string>
#include <string_view>

namespace PJ::WebSocket {

namespace url_detail {

[[nodiscard]] inline std::string_view trim(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

/// Checks `host_port` (`host[:port]`, userinfo already refused); `has_port` reports whether it
/// names a port.
[[nodiscard]] inline Status checkHostPort(std::string_view host_port, bool& has_port) {
  std::string_view host = host_port;
  std::string_view port;
  has_port = false;
  if (host_port.starts_with('[')) {
    const auto close = host_port.find(']');
    if (close == std::string_view::npos) {
      return unexpected(std::string("Missing ']' after the IPv6 address"));
    }
    host = host_port.substr(0, close + 1);
    const std::string_view rest = host_port.substr(close + 1);
    if (!rest.empty()) {
      if (!rest.starts_with(':')) {
        return unexpected(std::string("Unexpected text after the IPv6 address"));
      }
      has_port = true;
      port = rest.substr(1);
    }
  } else if (const auto colon = host_port.find(':'); colon != std::string_view::npos) {
    if (host_port.find(':', colon + 1) != std::string_view::npos) {
      return unexpected(std::string("Put an IPv6 address in brackets, e.g. [::1]"));
    }
    host = host_port.substr(0, colon);
    has_port = true;
    port = host_port.substr(colon + 1);
  }
  if (host.empty() || host == "[]") {
    return unexpected(std::string("The URL has no host"));
  }
  if (has_port && !PJ::sdk::parsePort(port)) {
    return unexpected("Invalid port \"" + std::string(port) + "\": use 1-65535");
  }
  return okStatus();
}

}  // namespace url_detail

/// A URL cut into `scheme://` (optional), authority (`[userinfo@]host[:port]`) and the rest
/// (path, query, fragment, starting at the first '/', '?' or '#'). Views into the split text.
struct UrlParts {
  std::optional<std::string_view> scheme;  ///< without "://"; nullopt for a bare host
  std::string_view authority;
  std::string_view rest;
};

/// Splits `url`. A leading "xxx://" is a scheme only when xxx is not empty and has none of
/// "/?#@": in `robot/x?next=ws://a` the "ws://" belongs to the query.
[[nodiscard]] inline UrlParts splitUrl(std::string_view url) {
  UrlParts parts;
  if (const auto sep = url.find("://");
      sep != std::string_view::npos && sep > 0 && url.substr(0, sep).find_first_of("/?#@") == std::string_view::npos) {
    parts.scheme = url.substr(0, sep);
    url.remove_prefix(sep + 3);
  }
  const auto authority_end = url.find_first_of("/?#");
  parts.authority = url.substr(0, authority_end);
  parts.rest = authority_end == std::string_view::npos ? std::string_view{} : url.substr(authority_end);
  return parts;
}

/// The URL to dial for `text`, a full `ws://` / `wss://` URL or a bare `host[:port][/path][?query]`.
///  - Surrounding whitespace is ignored; the scheme is lowercased; everything else is kept as
///    typed.
///  - Without a scheme, `ws://` is prepended and, without a port, `:default_port` is appended to
///    the host: `robot` -> `ws://robot:9090`, `robot/bridge` -> `ws://robot:9090/bridge`.
///  - A URL with a scheme but no port is kept as is (the scheme's own port 80 / 443).
/// Fails, with a message for the status line, on empty text, another scheme, whitespace inside,
/// credentials (`user:pass@`: IXWebSocket parses and drops them and browsers do not send them),
/// a fragment ('#', which RFC 6455 forbids in a WebSocket URI), a missing host, an unbracketed
/// IPv6 address or a port outside 1..65535.
[[nodiscard]] inline Expected<std::string> normalizeUrl(std::string_view text, uint16_t default_port) {
  const std::string_view trimmed = url_detail::trim(text);
  if (trimmed.empty()) {
    return unexpected(std::string("Enter the server URL, e.g. ws://127.0.0.1:") + std::to_string(default_port));
  }
  for (const char c : trimmed) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0 || std::iscntrl(static_cast<unsigned char>(c)) != 0) {
      return unexpected(std::string("The URL cannot contain spaces"));
    }
  }
  const UrlParts parts = splitUrl(trimmed);
  std::string scheme = "ws";
  if (parts.scheme) {
    scheme = PJ::sdk::lowerAscii(std::string(*parts.scheme));
    if (scheme != "ws" && scheme != "wss") {
      return unexpected("Use a ws:// or wss:// URL, not " + scheme + "://");
    }
  }
  if (parts.authority.find('@') != std::string_view::npos) {
    return unexpected(
        std::string("Credentials in the URL are not supported: neither PlotJuggler nor browsers send them."));
  }
  if (parts.rest.find('#') != std::string_view::npos) {
    return unexpected(std::string("A WebSocket URL cannot have a fragment ('#')"));
  }
  bool has_port = false;
  if (auto status = url_detail::checkHostPort(parts.authority, has_port); !status) {
    return unexpected(status.error());
  }
  std::string out = scheme + "://";
  out += parts.authority;
  if (!parts.scheme && !has_port) {
    out += ':' + std::to_string(default_port);
  }
  out += parts.rest;
  return out;
}

/// Whether a query parameter named `name` may carry a secret: the name, percent-decoded and
/// compared ignoring case, '_' and '-', CONTAINS password, passwd, passphrase, secret, token,
/// apikey, accesskey, authkey, privatekey or credential (PlotJuggler's MCP redaction rule and
/// common URL names), or IS one of the short names key, auth, authorization, sig, signature, pwd,
/// pw, pass, session, jwt, x-amz-signature. The short names match only exactly, so `keyframe`,
/// `passive`, `pwm` or `session_id` are kept.
[[nodiscard]] inline bool isSecretQueryName(std::string_view name) {
  std::string folded;
  for (size_t i = 0; i < name.size(); ++i) {
    char c = name[i];
    if (c == '%' && i + 2 < name.size() && std::isxdigit(static_cast<unsigned char>(name[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(name[i + 2])) != 0) {
      c = static_cast<char>(std::stoi(std::string(name.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    }
    if (c != '_' && c != '-') {
      folded += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  static constexpr std::array<std::string_view, 10> kFragments = {"password",   "passwd",    "passphrase", "secret",
                                                                  "token",      "apikey",    "accesskey",  "authkey",
                                                                  "privatekey", "credential"};
  static constexpr std::array<std::string_view, 11> kExact = {
      "key", "auth", "authorization", "sig", "signature", "pwd", "pw", "pass", "session", "jwt", "xamzsignature"};
  for (const auto fragment : kFragments) {
    if (folded.find(fragment) != std::string::npos) {
      return true;
    }
  }
  for (const auto exact : kExact) {
    if (folded == exact) {
      return true;
    }
  }
  return false;
}

/// `url` without credentials: the userinfo (`user[:password]@`) and every query parameter whose
/// name isSecretQueryName() are removed; everything else (scheme, host, port, path, the other
/// parameters in their order with the separator ('&' or ';') that preceded each, the fragment)
/// is kept byte for byte. A query left empty loses its '?'. A secret inside the path cannot be
/// told apart from an ordinary path and is kept.
[[nodiscard]] inline std::string cleanUrl(std::string_view url) {
  const UrlParts parts = splitUrl(url);
  std::string out;
  if (parts.scheme) {
    out = std::string(*parts.scheme) + "://";
  }
  const auto at = parts.authority.rfind('@');
  out += at == std::string_view::npos ? parts.authority : parts.authority.substr(at + 1);
  const std::string_view rest = parts.rest;
  const auto query = rest.find('?');
  if (query == std::string_view::npos) {
    return out + std::string(rest);
  }
  out += rest.substr(0, query);
  const auto fragment = rest.find('#', query);
  const std::string_view params =
      rest.substr(query + 1, fragment == std::string_view::npos ? std::string_view::npos : fragment - query - 1);
  std::string kept;
  bool any = false;
  size_t begin = 0;
  char separator = '&';  // before the first parameter; never written
  while (begin <= params.size()) {
    const auto end = params.find_first_of("&;", begin);
    const std::string_view param =
        params.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
    if (!isSecretQueryName(param.substr(0, param.find('=')))) {
      if (any) {
        kept += separator;
      }
      kept += param;
      any = true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    separator = params[end];
    begin = end + 1;
  }
  if (any) {
    out += '?' + kept;
  }
  if (fragment != std::string_view::npos) {
    out += rest.substr(fragment);
  }
  return out;
}

}  // namespace PJ::WebSocket
