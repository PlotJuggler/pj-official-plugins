#pragma once

// candump_parser: classifies and decodes one line of `candump` output, in
// either of its two shapes. Grammar verified directly against upstream
// linux-can/can-utils sources (candump.c ~L836-921, lib.c
// snprintf_canframe ~L318-440 and snprintf_long_canframe ~L442-660):
//
//   - "log" format (`candump -l ... > file.log`, snprintf_canframe, sep=0):
//     "(ts) iface ID#DATA", with "##<flag>DATA" for CAN FD, "#R[dlc]" for
//     RTR, "%02X%03X#%02X:%02X:%08X#<data>" (vcid+prio#flags:sdt:af#data)
//     for CAN XL, and " R"/" T" appended by `-x` for direction.
//   - "screen" format (interactive candump output, snprintf_long_canframe):
//     " (ts) iface  [RX|TX <flag> <flag>]  ID <dlc-marker>  b0 b1 ..." --
//     LEADING SPACE before the timestamp (unlike log format); ID is
//     right-padded with blanks (3 extra for classic/FD, EFF/error always
//     8 hex digits, possibly 5-space SFF-indented once an EFF frame was
//     seen); <dlc-marker> is "[N]" (1 decimal digit, classic, N=0-8),
//     "{H}" (1 hex digit, classic LEN8_DLC view), "[NN]" (2 digits, CAN FD),
//     or "[NNNN]" (4 digits, CAN XL, followed by a
//     "(vcid|flags:sdt:af)" group instead of plain data); RTR shows the
//     classic "[N]" marker followed by the two words "remote request"
//     instead of data bytes. `-x` inserts "  RX"/"  TX" plus a 3-character
//     flag pair (one of "- -"/"B -"/"- E"/"B E" for CC/FD, "- -"/"S -"/
//     "- R"/"S R" for XL -- note the embedded space, so it tokenizes as TWO
//     whitespace-separated symbols) between the interface and the id.
//     Error frames append "   ERRORFRAME"; with `-e`, further detail lines
//     follow on SEPARATE physical lines, each starting with a literal TAB
//     (snprintf_can_error_frame, sep="\n\t") -- handled at the parseLine()
//     dispatch level, not by either grammar, since they carry no id/data of
//     their own (see kErrorDetail).
//
//   "DROPCOUNT: dropped %u CAN frame%s on '%s' socket (total drops %u)" has
//   NEITHER a timestamp NOR a leading interface token (candump.c ~L840-845)
//   -- recognized at the parseLine() dispatch level too, in both stdout and
//   log-file output.
//
// Two distinct entry points (parseLogLine / parseScreenLine) share only the
// common prefix (an optional leading space, then the timestamp, then the
// interface) — the frame body grammar differs enough between the two shapes
// that folding them into one function would trade clarity for a few dozen
// shared lines.
//
// Perf-critical (measured against ~1e7-line files): string_view + a private
// hex lookup table only, no <regex>, no <sstream>, no locale-dependent
// floating-point parsing of the timestamp (parsed by structure, as integer
// seconds + a nanosecond-scaled fraction).

#include <cstdint>
#include <istream>
#include <string>
#include <string_view>
#include <vector>

namespace candump_detail {

/// Parses `text` as a hex uint32 (no "0x"/"0X" prefix -- strip that first if
/// present). Returns false (leaving `out` untouched) on empty input, a
/// non-hex character, or overflow past 32 bits. The one hex-digit-run parser
/// shared by this parser's own id/data tokens and csv_dictionary's ARUS-CSV
/// "ID" column.
bool parseHexU32(std::string_view text, std::uint32_t& out);

/// What one non-blank line turned out to be. Every kind other than
/// kMalformed is COUNTED by the caller, never silently dropped; kData is the
/// only kind carrying signal-decodable bytes.
enum class LineKind {
  kData,         ///< classic CAN data frame (payload 0-8 bytes) -- decodable
  kRtr,          ///< RTR frame -- recognized, no payload to decode
  kFd,           ///< CAN FD frame -- recognized; dbc_parser_cpp rejects payloads > 8 bytes
  kXl,           ///< CAN XL frame -- recognized, not decoded (out of scope)
  kError,        ///< error frame (CAN_ERR_FLAG set on the id) -- recognized, not decoded
  kErrorDetail,  ///< a `-e` TAB-continuation detail line for the PRECEDING error frame
  kDropCount,    ///< a "DROPCOUNT:" notification line -- no timestamp/interface, not a frame
  kWallClockTs,  ///< `-tA`-style wall-clock timestamp -- recognized, unsupported in v0.1.0
  kMalformed,    ///< line did not match any known shape
};

/// Which per-frame grammar (see the file banner) produced a non-kMalformed
/// ParsedLine. kUnknown for lines that never reach either grammar's body
/// (kMalformed-from-the-shared-prefix, kWallClockTs, kDropCount,
/// kErrorDetail) or that reach one grammar's body but fail there too.
enum class Grammar {
  kUnknown,
  kLog,
  kScreen,
};

/// One parsed line. Fields beyond `kind`/`has_timestamp` are only meaningful
/// for the kinds that use them (documented per-field below).
struct ParsedLine {
  LineKind kind = LineKind::kMalformed;

  /// Set by parseLine()/parseLogLine()/parseScreenLine() whenever `kind` was
  /// decided by one grammar's body parser (i.e. kind != kMalformed and the
  /// shared prefix parsed). kUnknown otherwise.
  Grammar grammar = Grammar::kUnknown;

  /// True when a leading "(...)" token was found at all (numeric or not).
  /// A frame line with no timestamp token is always kMalformed: it cannot
  /// be placed on the timeline. kDropCount/kErrorDetail never have one --
  /// that is expected, not an error (see the grammar note above).
  bool has_timestamp = false;
  /// Integer seconds from the timestamp token. Valid iff has_timestamp &&
  /// kind != kWallClockTs.
  std::int64_t ts_seconds = 0;
  /// Fractional part of the timestamp, scaled to nanoseconds [0, 1'000'000'000).
  /// Valid under the same condition as ts_seconds.
  std::int64_t ts_fraction_ns = 0;

  /// Interface name: any run of non-space characters (dots and dashes are
  /// valid, e.g. "vcan0.1", "can-eth0"). For kDropCount this is extracted
  /// from the quoted socket name in the message instead of a leading token.
  /// Empty if the line has no interface (only possible before kind is
  /// decided, i.e. kMalformed/kErrorDetail).
  std::string interface;

  /// Valid for kData, kRtr, kFd, kXl, kError.
  std::uint32_t can_id = 0;
  bool extended = false;  ///< 29-bit id (8 hex digits in log format)

  /// Payload bytes. Only ever populated for kind == kData (0-8 bytes).
  std::vector<std::uint8_t> data;

  /// RX/TX direction, when the line's format carries one (`-x` in log
  /// format; screen format's RX/TX column). Unknown otherwise.
  bool direction_known = false;
  bool is_tx = false;

  /// kDropCount only: the "dropped %u CAN frames" count, when the message
  /// text parsed cleanly (0 if it did not -- the line is still kDropCount,
  /// just without a usable count).
  std::uint64_t dropped_count = 0;
};

/// Parses one line of `candump -l` log output. Does not recognize
/// DROPCOUNT/error-detail lines -- see parseLine().
ParsedLine parseLogLine(std::string_view line);

/// Parses one line of interactive candump screen output. Does not recognize
/// DROPCOUNT/error-detail lines -- see parseLine().
ParsedLine parseScreenLine(std::string_view line);

/// Dispatch used by callers. Recognizes the two shapes that are NOT part of
/// either per-frame grammar first (a literal-TAB-prefixed error-detail
/// continuation; a "DROPCOUNT:"-prefixed notification), then parses the
/// shared timestamp+interface prefix ONCE and tries the log-format body
/// first (matching upstream's own hardcoded precedence), falling back to
/// the screen-format body off that SAME parsed prefix -- unlike calling
/// parseLogLine()/parseScreenLine() independently, the prefix is never
/// parsed twice. Per-line format detection (a capture file is normally
/// homogeneous, but nothing requires it to be).
ParsedLine parseLine(std::string_view line);

/// Combines a parsed line's structural (seconds, fraction) timestamp into raw
/// nanoseconds (seconds * 1e9 + fraction), saturating on overflow. Does not
/// resolve absolute-vs-relative semantics -- see TimeMode.
std::int64_t rawTimestampNs(const ParsedLine& line);

/// How to turn each line's raw timestamp into the absolute nanoseconds
/// appendRecord() wants.
enum class TimeMode {
  kAbsolute,           ///< raw ns IS the absolute (epoch) timestamp
  kRelativeMonotonic,  ///< `-tz`-style: raw ns is already a non-decreasing offset since start
  kRelativeDelta,      ///< `-td`-style: raw ns is the delta since the PREVIOUS frame; accumulate
};

/// Below this many integer seconds, a timestamp is assumed to be relative
/// (an absolute Unix time this small is September 2001 or earlier).
constexpr std::int64_t kAbsoluteThresholdSeconds = 1'000'000'000;

struct TimeModeDetection {
  TimeMode mode = TimeMode::kAbsolute;
  /// False if no line in the scanned window carried a numeric ("(s.frac)")
  /// timestamp at all -- the caller should report a clear "not a candump
  /// capture" error rather than silently importing zero-length data.
  bool saw_numeric_timestamp = false;
};

/// Scans up to `max_lines` non-blank lines from `stream` (fewer if the
/// stream runs out first) and decides a TimeMode from the raw timestamps
/// alone: the first numeric timestamp >= kAbsoluteThresholdSeconds ->
/// kAbsolute; otherwise kRelativeMonotonic if the scanned timestamps are
/// non-decreasing, else kRelativeDelta. Leaves `stream` at EOF or partway
/// through -- callers needing the data itself must re-open/seek back to the
/// start; this is intentionally a SEPARATE small pass (bounded by
/// max_lines), not a second full parse of a multi-gigabyte file.
TimeModeDetection detectTimeMode(std::istream& stream, std::uint64_t max_lines);

/// The one bounded-scan line cap shared by the dialog's preview (summary +
/// interface table) and the source's own time-mode pre-pass -- was two
/// separately-maintained "200000" literals.
constexpr std::uint64_t kScanCap = 200000;

/// Per-interface counts collected by prescanCandump(), in first-seen order.
struct InterfacePrescanStats {
  std::string interface;
  std::uint64_t frames = 0;
  std::size_t distinct_ids = 0;
};

/// Counts of the line kinds that are "recognized but not decoded into
/// signals", plus the two shapes (DROPCOUNT, `-e` detail) that carry no
/// frame at all. Shared between CandumpDialog's preview summary and
/// CandumpSource's post-import summary so their text stays in sync --
/// appendDropCount()/appendErrorDetail()/appendWallClock() are used
/// verbatim by both; the RTR/FD/XL/error breakdown is reported differently
/// by each caller (dialog: one combined "recognized-but-unsupported" count
/// via unsupported(); source: one line per kind), so those four fields are
/// exposed directly rather than through a shared appender.
struct RecognizedCounters {
  std::uint64_t rtr = 0;
  std::uint64_t fd = 0;
  std::uint64_t xl = 0;
  std::uint64_t error = 0;
  std::uint64_t error_detail = 0;
  std::uint64_t dropcount = 0;
  std::uint64_t dropped_frames_total = 0;
  std::uint64_t wall_clock = 0;

  /// Total recognized-but-not-decoded frames (RTR + FD + XL + error).
  [[nodiscard]] std::uint64_t unsupported() const {
    return rtr + fd + xl + error;
  }

  void appendDropCount(std::string& out) const;
  void appendErrorDetail(std::string& out) const;
  void appendWallClock(std::string& out) const;
};

/// Result of one bounded forward scan of a candump capture: everything the
/// dialog's file-picker summary needs (interface/grammar/malformed counts,
/// the recognized-but-unsupported breakdown, the inferred TimeMode) in a
/// single pass, so the dialog never re-parses lines just to learn the
/// grammar or the time mode.
struct PrescanResult {
  std::uint64_t lines_scanned = 0;
  bool truncated = false;  ///< true if `max_lines` was hit before EOF
  std::uint64_t malformed = 0;
  std::uint64_t log_shaped = 0;
  std::uint64_t screen_shaped = 0;
  RecognizedCounters recognized;
  std::vector<InterfacePrescanStats> interfaces;
  TimeModeDetection time_mode;

  /// Raw nanoseconds of the FIRST numeric timestamp seen (same value
  /// time_mode's decision is based on) -- the dialog's "range starts at"
  /// preview needs the actual instant, not just the coarse TimeMode.
  bool have_first_timestamp = false;
  std::int64_t first_timestamp_ns = 0;
};

/// Scans up to `max_lines` non-blank lines from `stream` ONCE, fully parsing
/// each (parseLine()) to derive: per-interface frame/distinct-id counts, the
/// log-vs-screen shape breakdown (from ParsedLine::grammar -- no second
/// parseLogLine()/parseScreenLine() attempt), malformed/RTR/FD/XL/error/
/// DROPCOUNT/`-e`-detail counts, and the same TimeMode decision
/// detectTimeMode() makes (from the identical first-numeric-timestamp +
/// monotonicity rule, applied to the lines already parsed here instead of a
/// second scan). Leaves `stream` at EOF or partway through, like
/// detectTimeMode().
PrescanResult prescanCandump(std::istream& stream, std::uint64_t max_lines);

}  // namespace candump_detail
