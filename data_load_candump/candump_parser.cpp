#include "candump_parser.hpp"

#include <array>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace candump_detail {

namespace {

/// CAN_ERR_FLAG (SocketCAN): set on the id candump prints for an error frame.
constexpr std::uint32_t kErrFlag = 0x2000'0000u;
/// 29-bit extended-id mask (matches blf_frames.cpp's kCanIdMask).
constexpr std::uint32_t kExtIdMask = 0x1FFF'FFFFu;
constexpr int kMaxClassicBytes = 8;

/// "DROPCOUNT:" recognized both by parseLine()'s dispatch and by
/// parseDropCount() itself (which re-derives its own start position from it).
constexpr std::string_view kDropCountPrefix = "DROPCOUNT:";

/// Hex digit lookup: -1 for non-hex bytes. Avoids isxdigit()'s
/// locale-dependent behavior and the associated branch-per-call overhead.
constexpr std::array<std::int8_t, 256> makeHexTable() {
  std::array<std::int8_t, 256> table{};
  for (auto& v : table) {
    v = -1;
  }
  for (char c = '0'; c <= '9'; ++c) {
    table[static_cast<unsigned char>(c)] = static_cast<std::int8_t>(c - '0');
  }
  for (char c = 'a'; c <= 'f'; ++c) {
    table[static_cast<unsigned char>(c)] = static_cast<std::int8_t>(c - 'a' + 10);
  }
  for (char c = 'A'; c <= 'F'; ++c) {
    table[static_cast<unsigned char>(c)] = static_cast<std::int8_t>(c - 'A' + 10);
  }
  return table;
}
constexpr std::array<std::int8_t, 256> kHexTable = makeHexTable();

bool isHexChar(char c) {
  return kHexTable[static_cast<unsigned char>(c)] >= 0;
}

/// Parses a run of hex-digit PAIRS ("DEADBEEF") into bytes. Returns false on
/// an odd digit count or a non-hex character.
bool parseHexBytes(std::string_view text, std::vector<std::uint8_t>& out) {
  if (text.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) {
    const std::int8_t hi = kHexTable[static_cast<unsigned char>(text[i])];
    const std::int8_t lo = kHexTable[static_cast<unsigned char>(text[i + 1])];
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

/// Parses one 2-hex-digit byte token ("A5"). Shared by parseScreenBody's two
/// classic-data loops (bracket-DLC-counted and brace-until-non-byte).
std::optional<std::uint8_t> parseByteToken(std::string_view tok) {
  if (tok.size() != 2 || !isHexChar(tok[0]) || !isHexChar(tok[1])) {
    return std::nullopt;
  }
  std::uint32_t value = 0;
  parseHexU32(tok, value);
  return static_cast<std::uint8_t>(value);
}

std::size_t skipSpaces(std::string_view s, std::size_t pos) {
  while (pos < s.size() && s[pos] == ' ') {
    ++pos;
  }
  return pos;
}

/// Next run of non-space characters starting at `pos`. Advances `pos` past
/// it (and any trailing spaces). Empty token if `pos` is already at the end.
std::string_view nextToken(std::string_view s, std::size_t& pos) {
  pos = skipSpaces(s, pos);
  const std::size_t start = pos;
  while (pos < s.size() && s[pos] != ' ') {
    ++pos;
  }
  const std::string_view tok = s.substr(start, pos - start);
  pos = skipSpaces(s, pos);
  return tok;
}

/// Parses the leading "(...)" timestamp token. `pos` lands just after the
/// closing paren (and any trailing spaces) on success.
struct TsToken {
  bool present = false;
  bool numeric = false;
  std::int64_t seconds = 0;
  std::int64_t fraction_ns = 0;
};

TsToken parseTsToken(std::string_view s, std::size_t& pos) {
  TsToken t;
  if (pos >= s.size() || s[pos] != '(') {
    return t;
  }
  t.present = true;
  const std::size_t close = s.find(')', pos + 1);
  if (close == std::string_view::npos) {
    pos = s.size();
    return t;  // unterminated -- not numeric, caller treats as malformed
  }
  const std::string_view inner = s.substr(pos + 1, close - pos - 1);
  pos = skipSpaces(s, close + 1);

  std::size_t i = 0;
  bool neg = false;
  if (i < inner.size() && (inner[i] == '+' || inner[i] == '-')) {
    neg = inner[i] == '-';
    ++i;
  }
  const std::size_t int_start = i;
  while (i < inner.size() && inner[i] >= '0' && inner[i] <= '9') {
    ++i;
  }
  if (i == int_start) {
    return t;  // no digits at all: e.g. `-tA`'s "2024-01-15 10:23:45.123456"
  }
  std::int64_t seconds = 0;
  for (std::size_t k = int_start; k < i; ++k) {
    seconds = seconds * 10 + (inner[k] - '0');
  }

  std::int64_t frac_ns = 0;
  if (i < inner.size()) {
    if (inner[i] != '.') {
      return t;  // trailing non-fraction garbage: not a plain numeric timestamp
    }
    ++i;
    const std::size_t frac_start = i;
    while (i < inner.size() && inner[i] >= '0' && inner[i] <= '9') {
      ++i;
    }
    if (i != inner.size()) {
      return t;  // trailing garbage after the fraction
    }
    const std::size_t digits = i - frac_start;
    std::int64_t frac_val = 0;
    for (std::size_t k = frac_start; k < i; ++k) {
      frac_val = frac_val * 10 + (inner[k] - '0');
    }
    // Scale to nanoseconds by structure (candump prints 6 digits by default,
    // 9 with -tN): pad on the right if narrower, truncate if wider.
    if (digits <= 9) {
      for (std::size_t k = digits; k < 9; ++k) {
        frac_val *= 10;
      }
    } else {
      for (std::size_t k = digits; k > 9; --k) {
        frac_val /= 10;
      }
    }
    frac_ns = frac_val;
  }

  t.numeric = true;
  t.seconds = neg ? -seconds : seconds;
  t.fraction_ns = frac_ns;
  return t;
}

/// Classifies an already-hex-parsed id (log format's fixed-width id, or
/// screen format's id token) as error/extended/standard, matching candump's
/// own field widths: exactly 3 hex digits -> standard; exactly 8 -> extended
/// or (if CAN_ERR_FLAG is set) an error frame. Other widths are accepted
/// defensively (some producers don't zero-pad) and classified by value.
/// Returns nullopt when `hex` is not a valid (<=8-digit) hex token.
struct IdClass {
  bool is_error = false;
  bool extended = false;
  std::uint32_t id = 0;
};

std::optional<IdClass> classifyId(std::string_view hex) {
  std::uint32_t raw = 0;
  if (!parseHexU32(hex, raw)) {
    return std::nullopt;
  }
  IdClass c;
  if (hex.size() >= 8 && (raw & kErrFlag) != 0) {
    c.is_error = true;
    c.id = raw;
    return c;
  }
  c.extended = hex.size() > 3 || raw > 0x7FFu;
  c.id = raw & kExtIdMask;
  return c;
}

/// Common prefix shared by both grammars: an optional leading space (screen
/// format's " (ts) ..." always has one, via candump.c's
/// `sprintf(afrbuf, " %s", ...)`; log format's never does -- tolerating it
/// either way costs nothing), the timestamp, then the interface. Returns
/// true and advances `pos` past the interface when the line should continue
/// into the format-specific frame-body parse; returns false when `out` is
/// already fully decided (kMalformed / kWallClockTs / no timestamp at all).
/// DROPCOUNT/error-detail lines never reach here -- they carry no timestamp
/// or leading-interface token at all (see parseLine()).
bool parseCommonPrefix(std::string_view line, ParsedLine& out, std::size_t& pos) {
  pos = skipSpaces(line, 0);
  const TsToken ts = parseTsToken(line, pos);
  out.has_timestamp = ts.present;
  if (!ts.present) {
    out.kind = LineKind::kMalformed;
    return false;
  }
  if (!ts.numeric) {
    out.kind = LineKind::kWallClockTs;
    return false;
  }
  out.ts_seconds = ts.seconds;
  out.ts_fraction_ns = ts.fraction_ns;

  const std::string_view iface = nextToken(line, pos);
  if (iface.empty()) {
    out.kind = LineKind::kMalformed;
    return false;
  }
  out.interface = std::string(iface);
  return true;
}

/// Finishes a log-format line (candump -l) whose shared prefix (timestamp +
/// interface) has already been parsed into `out`; `pos` is just past the
/// interface token.
void parseLogBody(std::string_view line, ParsedLine& out, std::size_t pos) {
  const std::string_view body = nextToken(line, pos);
  const std::size_t hash = body.find('#');
  if (hash == std::string_view::npos || hash == 0) {
    out.kind = LineKind::kMalformed;
    return;
  }
  const std::string_view id_hex = body.substr(0, hash);
  std::string_view rest = body.substr(hash + 1);

  const auto id_class = classifyId(id_hex);
  if (!id_class) {
    out.kind = LineKind::kMalformed;
    return;
  }
  out.can_id = id_class->id;
  out.extended = id_class->extended;

  if (id_class->is_error) {
    out.kind = LineKind::kError;
  } else if (!rest.empty() && rest.front() == '#') {
    out.kind = LineKind::kFd;  // "##<flag><data>"
  } else if (!rest.empty() && (rest.front() == 'R' || rest.front() == 'r')) {
    out.kind = LineKind::kRtr;  // "R", "R<requested-dlc-hex-digit>", or with a "_<hex>" len8_dlc suffix
  } else if (rest.find(':') != std::string_view::npos) {
    // CAN XL: id_hex is actually "%02X%03X" (vcid+prio, 5 hex digits), and
    // `rest` is "%02X:%02X:%08X#<data>" (flags:sdt:af#data) --
    // lib.c snprintf_canframe ~L343-346.
    out.kind = LineKind::kXl;
  } else {
    // A classic 8-byte frame may carry an optional raw len8_dlc suffix
    // "_<hex-digit>" after the data (lib.c ~L428-435, CC_DLC_DELIM='_').
    std::string_view data_hex = rest;
    const std::size_t underscore = rest.find('_');
    if (underscore != std::string_view::npos) {
      if (underscore + 2 != rest.size() || !isHexChar(rest[underscore + 1])) {
        out.kind = LineKind::kMalformed;
        return;
      }
      data_hex = rest.substr(0, underscore);
    }
    std::vector<std::uint8_t> data;
    if (!parseHexBytes(data_hex, data) || data.size() > static_cast<std::size_t>(kMaxClassicBytes)) {
      out.kind = LineKind::kMalformed;
      return;
    }
    out.kind = LineKind::kData;
    out.data = std::move(data);
  }

  // `-x` direction suffix: a trailing " R" or " T" token (RX/TX), distinct
  // from RTR's un-spaced "#R" handled above.
  const std::string_view dir = nextToken(line, pos);
  if (dir == "R") {
    out.direction_known = true;
    out.is_tx = false;
  } else if (dir == "T") {
    out.direction_known = true;
    out.is_tx = true;
  }
}

/// Finishes an interactive-screen-output line whose shared prefix
/// (timestamp + interface) has already been parsed into `out`; `pos` is
/// just past the interface token.
void parseScreenBody(std::string_view line, ParsedLine& out, std::size_t pos) {
  // `-x`: "  RX"/"  TX" then ALWAYS a 3-character flag pair (candump.c
  // ~L897-909; the shapes are in the file banner above). Its embedded space
  // makes it tokenize as two symbols; their value carries no information we
  // need (FD/XL/error are already determined structurally below), so both
  // are simply consumed.
  {
    std::size_t peek_pos = pos;
    const std::string_view tok = nextToken(line, peek_pos);
    if (tok == "RX" || tok == "TX") {
      out.direction_known = true;
      out.is_tx = (tok == "TX");
      pos = peek_pos;
      nextToken(line, pos);  // flag symbol 1 (e.g. "-"/"B"/"S")
      nextToken(line, pos);  // flag symbol 2 (e.g. "-"/"E"/"R")
    }
  }

  const std::string_view id_hex = nextToken(line, pos);
  if (id_hex.empty()) {
    out.kind = LineKind::kMalformed;
    return;
  }
  const auto id_class = classifyId(id_hex);
  if (!id_class) {
    out.kind = LineKind::kMalformed;
    return;
  }
  out.can_id = id_class->id;
  out.extended = id_class->extended;

  // Error frames (8 hex digits, CAN_ERR_FLAG set) short-circuit here exactly
  // as upstream's snprintf_long_canframe does: the DLC/data columns that
  // follow ("...   ERRORFRAME", optionally more TAB-continuation lines from
  // `-e`) carry no decodable payload, so there is nothing left to parse.
  if (id_class->is_error) {
    out.kind = LineKind::kError;
    return;
  }

  // The DLC/length marker's shape (not its value) tells classic/FD/XL apart
  // -- lib.c always emits a fixed digit count per kind (shapes in the file
  // banner above). A 3-digit id at this point is either a classic/FD CAN-ID
  // or a CAN XL frame's priority field -- indistinguishable except by what
  // follows.
  const std::string_view marker = nextToken(line, pos);
  bool marker_ok = marker.size() >= 3;
  const bool is_brace = marker_ok && marker.front() == '{' && marker.back() == '}';
  const bool is_bracket = marker_ok && marker.front() == '[' && marker.back() == ']';
  if (!is_brace && !is_bracket) {
    out.kind = LineKind::kMalformed;
    return;
  }
  const std::string_view digits = marker.substr(1, marker.size() - 2);
  // "{H}" carries one HEX digit (raw len8_dlc code, up to 'F'); "[N...]"
  // carries decimal digits only.
  for (char c : digits) {
    if ((is_brace && !isHexChar(c)) || (is_bracket && (c < '0' || c > '9'))) {
      marker_ok = false;
      break;
    }
  }
  if (!marker_ok || digits.empty() || (is_brace && digits.size() != 1)) {
    out.kind = LineKind::kMalformed;
    return;
  }

  if (is_bracket && digits.size() == 4) {
    // CAN XL: "123 [0004] (00|11:22:12345678) AA BB ..." -- the parenthesized
    // "(vcid|flags:sdt:af)" group follows as one more whitespace-delimited
    // token (no embedded spaces in that format string).
    const std::string_view xl_group = nextToken(line, pos);
    if (xl_group.size() < 2 || xl_group.front() != '(' || xl_group.back() != ')') {
      out.kind = LineKind::kMalformed;
      return;
    }
    out.kind = LineKind::kXl;
    return;
  }
  if (is_bracket && digits.size() == 2) {
    out.kind = LineKind::kFd;  // CAN FD: "[NN]" -- payload not decoded (out of scope)
    return;
  }
  if (digits.size() != 1) {
    out.kind = LineKind::kMalformed;  // no real candump output produces any other width
    return;
  }

  // RTR: the classic "[N]"/"{H}" marker is followed by the two words
  // "remote request" instead of data bytes (lib.c ~L579-582).
  {
    std::size_t peek_pos = pos;
    if (nextToken(line, peek_pos) == "remote") {
      const std::string_view request = nextToken(line, peek_pos);
      if (request == "request") {
        out.kind = LineKind::kRtr;
        return;
      }
    }
  }

  // Classic data. "[N]" (bracket) reliably gives the byte COUNT (lib.c
  // literally does `buf[offset+2] = len + '0'`), so a byte token that fails
  // to parse there is a real error. "{H}" (brace, LEN8_DLC) does NOT: H can
  // be a raw len8_dlc code (9-F) unrelated to how many bytes are actually
  // present, so its payload length is derived from bytes present instead.
  std::vector<std::uint8_t> data;
  if (is_bracket) {
    const int dlc = digits[0] - '0';
    data.reserve(static_cast<std::size_t>(dlc));
    for (int i = 0; i < dlc; ++i) {
      const std::string_view byte_tok = nextToken(line, pos);
      const auto byte_val = parseByteToken(byte_tok);
      if (!byte_val) {
        out.kind = LineKind::kMalformed;
        return;
      }
      data.push_back(*byte_val);
    }
  } else {
    while (data.size() < static_cast<std::size_t>(kMaxClassicBytes)) {
      std::size_t peek_pos = pos;
      const std::string_view byte_tok = nextToken(line, peek_pos);
      const auto byte_val = parseByteToken(byte_tok);
      if (!byte_val) {
        break;
      }
      data.push_back(*byte_val);
      pos = peek_pos;
    }
  }
  out.kind = LineKind::kData;
  out.data = std::move(data);
}

/// "DROPCOUNT: dropped %u CAN frame%s on '%s' socket (total drops %u)"
/// (candump.c ~L840-845, both stdout logfrmt and the log file get the exact
/// same text -- no timestamp, no leading interface token). Best-effort:
/// a message shape that does not parse still classifies as kDropCount with
/// dropped_count left at 0 and interface empty, never kMalformed.
ParsedLine parseDropCount(std::string_view line) {
  ParsedLine out;
  out.kind = LineKind::kDropCount;

  std::size_t pos = kDropCountPrefix.size();
  pos = skipSpaces(line, pos);
  constexpr std::string_view kDropped = "dropped ";
  if (line.substr(pos, kDropped.size()) == kDropped) {
    pos += kDropped.size();
    std::uint64_t count = 0;
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
      count = count * 10 + static_cast<std::uint64_t>(line[pos] - '0');
      ++pos;
    }
    // `count` is already 0 (ParsedLine's default) when no digit was seen at
    // all, so assigning it unconditionally is exactly equivalent to gating
    // on "did we see at least one digit".
    out.dropped_count = count;
  }

  const std::size_t open_quote = line.find('\'', pos);
  if (open_quote != std::string_view::npos) {
    const std::size_t close_quote = line.find('\'', open_quote + 1);
    if (close_quote != std::string_view::npos) {
      out.interface = std::string(line.substr(open_quote + 1, close_quote - open_quote - 1));
    }
  }
  return out;
}

/// Raw (seconds, fraction) -> nanoseconds, saturating on overflow. Shared by
/// the public rawTimestampNs(const ParsedLine&) and the lightweight
/// timestamp-prefix-only path below (detectTimeMode/prescanCandump's
/// bookkeeping never needs a full ParsedLine to compute this).
std::int64_t combineRawNs(std::int64_t seconds, std::int64_t fraction_ns) {
  constexpr std::int64_t kNsPerSecond = 1'000'000'000;
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  if (seconds > 0 && seconds > (kMax - fraction_ns) / kNsPerSecond) {
    return kMax;
  }
  if (seconds < 0 && seconds < (kMin - fraction_ns) / kNsPerSecond) {
    return kMin;
  }
  return seconds * kNsPerSecond + fraction_ns;
}

/// Result of parsing ONLY the leading "(ts) iface" prefix -- everything
/// detectTimeMode()'s bookkeeping needs, without paying for the frame-body
/// parse (hex bytes, DLC markers, ...).
struct TimestampPrefix {
  bool has_timestamp = false;
  bool is_wall_clock = false;  ///< present but non-numeric (`-tA`)
  std::int64_t seconds = 0;
  std::int64_t fraction_ns = 0;
};

TimestampPrefix parseTimestampPrefix(std::string_view line) {
  TimestampPrefix result;
  std::size_t pos = skipSpaces(line, 0);
  const TsToken ts = parseTsToken(line, pos);
  result.has_timestamp = ts.present;
  if (!ts.present) {
    return result;
  }
  if (!ts.numeric) {
    result.is_wall_clock = true;
    return result;
  }
  result.seconds = ts.seconds;
  result.fraction_ns = ts.fraction_ns;
  return result;
}

/// Shared by detectTimeMode() and prescanCandump(): both accumulate
/// "saw a numeric timestamp at all", "the first one's whole-seconds value",
/// and "are they non-decreasing" the same way, then apply this one rule.
TimeModeDetection decideTimeMode(bool saw_numeric_timestamp, std::int64_t first_seconds, bool monotonic) {
  TimeModeDetection result;
  result.saw_numeric_timestamp = saw_numeric_timestamp;
  if (!saw_numeric_timestamp) {
    return result;
  }
  if (first_seconds >= kAbsoluteThresholdSeconds) {
    result.mode = TimeMode::kAbsolute;
  } else if (monotonic) {
    result.mode = TimeMode::kRelativeMonotonic;
  } else {
    result.mode = TimeMode::kRelativeDelta;
  }
  return result;
}

}  // namespace

bool parseHexU32(std::string_view text, std::uint32_t& out) {
  if (text.empty() || text.size() > 8) {
    return false;
  }
  std::uint32_t value = 0;
  for (char c : text) {
    const std::int8_t nibble = kHexTable[static_cast<unsigned char>(c)];
    if (nibble < 0) {
      return false;
    }
    value = (value << 4) | static_cast<std::uint32_t>(nibble);
  }
  out = value;
  return true;
}

ParsedLine parseLogLine(std::string_view line) {
  ParsedLine out;
  std::size_t pos = 0;
  if (!parseCommonPrefix(line, out, pos)) {
    return out;
  }
  parseLogBody(line, out, pos);
  if (out.kind != LineKind::kMalformed) {
    out.grammar = Grammar::kLog;
  }
  return out;
}

ParsedLine parseScreenLine(std::string_view line) {
  ParsedLine out;
  std::size_t pos = 0;
  if (!parseCommonPrefix(line, out, pos)) {
    return out;
  }
  parseScreenBody(line, out, pos);
  if (out.kind != LineKind::kMalformed) {
    out.grammar = Grammar::kScreen;
  }
  return out;
}

ParsedLine parseLine(std::string_view line) {
  if (!line.empty() && line.front() == '\t') {
    ParsedLine detail;
    detail.kind = LineKind::kErrorDetail;
    return detail;
  }
  if (line.substr(0, kDropCountPrefix.size()) == kDropCountPrefix) {
    return parseDropCount(line);
  }

  ParsedLine out;
  std::size_t pos = 0;
  if (!parseCommonPrefix(line, out, pos)) {
    return out;  // kMalformed or kWallClockTs -- neither grammar's body ever runs
  }

  // Try log format first (the common case for `-l` captures, and upstream's
  // own hardcoded order) directly off the prefix just parsed above -- no
  // second parseCommonPrefix call. Only on failure do we retry as screen,
  // reusing the SAME already-parsed prefix (has_timestamp/ts_*/interface)
  // instead of parsing it again from scratch.
  const std::size_t body_pos = pos;
  parseLogBody(line, out, pos);
  if (out.kind != LineKind::kMalformed) {
    out.grammar = Grammar::kLog;
    return out;
  }

  ParsedLine screen_out;
  screen_out.has_timestamp = out.has_timestamp;
  screen_out.ts_seconds = out.ts_seconds;
  screen_out.ts_fraction_ns = out.ts_fraction_ns;
  screen_out.interface = out.interface;
  pos = body_pos;
  parseScreenBody(line, screen_out, pos);
  if (screen_out.kind != LineKind::kMalformed) {
    screen_out.grammar = Grammar::kScreen;
    return screen_out;
  }
  return out;  // both failed: report the log-format attempt's classification
}

std::int64_t rawTimestampNs(const ParsedLine& line) {
  return combineRawNs(line.ts_seconds, line.ts_fraction_ns);
}

TimeModeDetection detectTimeMode(std::istream& stream, std::uint64_t max_lines) {
  std::string line;
  bool have_prev = false;
  std::int64_t prev_ns = 0;
  std::int64_t first_seconds = 0;
  bool monotonic = true;
  bool saw_numeric_timestamp = false;

  for (std::uint64_t count = 0; count < max_lines && std::getline(stream, line); ++count) {
    if (line.empty()) {
      continue;
    }
    const TimestampPrefix prefix = parseTimestampPrefix(line);
    if (!prefix.has_timestamp || prefix.is_wall_clock) {
      continue;
    }
    saw_numeric_timestamp = true;
    if (!have_prev) {
      first_seconds = prefix.seconds;
    }
    const std::int64_t ns = combineRawNs(prefix.seconds, prefix.fraction_ns);
    if (have_prev && ns < prev_ns) {
      monotonic = false;
    }
    prev_ns = ns;
    have_prev = true;
  }

  return decideTimeMode(saw_numeric_timestamp, first_seconds, monotonic);
}

void RecognizedCounters::appendDropCount(std::string& out) const {
  if (dropcount == 0) {
    return;
  }
  out += "; " + std::to_string(dropcount) + " DROPCOUNT notification(s)";
  if (dropped_frames_total > 0) {
    out += " (" + std::to_string(dropped_frames_total) + " frame(s) dropped total)";
  }
}

void RecognizedCounters::appendErrorDetail(std::string& out) const {
  if (error_detail == 0) {
    return;
  }
  out += "; " + std::to_string(error_detail) + " `-e` error-detail line(s)";
}

void RecognizedCounters::appendWallClock(std::string& out) const {
  if (wall_clock == 0) {
    return;
  }
  out += "; " + std::to_string(wall_clock) + " wall-clock (`-t a`) line(s), unsupported";
}

PrescanResult prescanCandump(std::istream& stream, std::uint64_t max_lines) {
  PrescanResult result;

  std::unordered_map<std::string, std::size_t> index_of;
  std::vector<std::unordered_set<std::uint32_t>> id_sets;  // parallel to result.interfaces

  bool have_prev_ts = false;
  std::int64_t prev_ts_ns = 0;
  std::int64_t first_seconds = 0;
  bool monotonic = true;
  bool saw_numeric_timestamp = false;

  std::string line;
  while (result.lines_scanned < max_lines && std::getline(stream, line)) {
    if (line.empty()) {
      continue;
    }
    ++result.lines_scanned;

    // parseLine() handles the two shapes that sit OUTSIDE either per-frame
    // grammar (a "DROPCOUNT:" notification -- no timestamp/interface at
    // all; a `-e` TAB-continuation detail line) before trying log/screen.
    const ParsedLine parsed = parseLine(line);

    if (parsed.kind == LineKind::kMalformed) {
      ++result.malformed;
      continue;
    }
    if (parsed.kind == LineKind::kErrorDetail) {
      ++result.recognized.error_detail;
      continue;
    }
    if (parsed.kind == LineKind::kWallClockTs) {
      ++result.recognized.wall_clock;
      continue;
    }
    if (parsed.kind == LineKind::kDropCount) {
      ++result.recognized.dropcount;
      result.recognized.dropped_frames_total += parsed.dropped_count;
      continue;
    }

    // Format-label bookkeeping: parseLine() already recorded which grammar
    // matched, so this never re-parses the line.
    if (parsed.grammar == Grammar::kLog) {
      ++result.log_shaped;
    } else if (parsed.grammar == Grammar::kScreen) {
      ++result.screen_shaped;
    }

    if (parsed.has_timestamp) {
      saw_numeric_timestamp = true;
      const std::int64_t ns = rawTimestampNs(parsed);
      if (!have_prev_ts) {
        first_seconds = parsed.ts_seconds;
        result.have_first_timestamp = true;
        result.first_timestamp_ns = ns;
      }
      if (have_prev_ts && ns < prev_ts_ns) {
        monotonic = false;
      }
      prev_ts_ns = ns;
      have_prev_ts = true;
    }

    switch (parsed.kind) {
      case LineKind::kRtr:
        ++result.recognized.rtr;
        break;
      case LineKind::kFd:
        ++result.recognized.fd;
        break;
      case LineKind::kXl:
        ++result.recognized.xl;
        break;
      case LineKind::kError:
        ++result.recognized.error;
        break;
      default:
        break;
    }

    if (parsed.interface.empty()) {
      continue;
    }
    std::size_t idx;
    if (const auto it = index_of.find(parsed.interface); it != index_of.end()) {
      idx = it->second;
    } else {
      idx = result.interfaces.size();
      index_of.emplace(parsed.interface, idx);
      result.interfaces.push_back(InterfacePrescanStats{parsed.interface, 0, 0});
      id_sets.emplace_back();
    }
    ++result.interfaces[idx].frames;
    id_sets[idx].insert(parsed.can_id);
  }

  for (std::size_t i = 0; i < result.interfaces.size(); ++i) {
    result.interfaces[i].distinct_ids = id_sets[i].size();
  }

  result.truncated = result.lines_scanned >= max_lines;
  result.time_mode = decideTimeMode(saw_numeric_timestamp, first_seconds, monotonic);
  return result;
}

}  // namespace candump_detail
