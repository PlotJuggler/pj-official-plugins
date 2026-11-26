#pragma once

// Classifies and decodes one line of `candump` output, in either of its two shapes. The grammar follows
// upstream linux-can/can-utils (candump.c, lib.c snprintf_canframe / snprintf_long_canframe):
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
//   NEITHER a timestamp NOR a leading interface token -- recognized at the
//   parseLine() dispatch level too, in both stdout and log-file output.
//
// parseLogLine / parseScreenLine share only the prefix (optional leading space, timestamp, interface); the frame
// body grammars differ too much to fold into one function.
//
// Perf-critical (~1e7-line files): string_view + a private hex lookup table only, no <regex>, no <sstream>, no
// locale-dependent floating-point parsing (the timestamp is parsed as integer seconds + a nanosecond-scaled fraction).

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace candump_detail {

/// Parses `text` as a hex uint32 without a "0x" prefix. Returns false (leaving `out` untouched) on empty input, a
/// non-hex character, or overflow past 32 bits. Also used by csv_dictionary's ARUS-CSV "ID" column.
bool parseHexU32(std::string_view text, std::uint32_t& out);

/// What one non-blank line turned out to be. Every kind other than kMalformed is COUNTED by the caller, never
/// silently dropped; kData is the only kind carrying decodable bytes.
enum class LineKind {
  kData,         ///< classic CAN data frame (payload 0-8 bytes) -- decodable
  kRtr,          ///< RTR frame -- recognized, no payload to decode
  kFd,           ///< CAN FD frame -- recognized, not decoded (out of scope)
  kXl,           ///< CAN XL frame -- recognized, not decoded (out of scope)
  kError,        ///< error frame (CAN_ERR_FLAG set on the id) -- recognized, not decoded
  kErrorDetail,  ///< a `-e` TAB-continuation detail line for the PRECEDING error frame
  kDropCount,    ///< a "DROPCOUNT:" notification line -- no timestamp/interface, not a frame
  kWallClockTs,  ///< `-tA`-style wall-clock timestamp -- recognized, unsupported
  kMalformed,    ///< line did not match any known shape
};

/// Which per-frame grammar produced a non-kMalformed ParsedLine. kUnknown for lines that never reach a grammar body
/// (bad shared prefix, kWallClockTs, kDropCount, kErrorDetail) or fail in it.
enum class Grammar {
  kUnknown,
  kLog,
  kScreen,
};

/// One parsed line. Fields beyond `kind`/`has_timestamp` are only meaningful for the kinds that use them.
struct ParsedLine {
  LineKind kind = LineKind::kMalformed;

  /// Set whenever `kind` was decided by one grammar's body parser; kUnknown otherwise.
  Grammar grammar = Grammar::kUnknown;

  /// True when a leading "(...)" token was found (numeric or not). A frame line without one is kMalformed (it
  /// cannot be placed on the timeline); kDropCount/kErrorDetail never have one, which is expected.
  bool has_timestamp = false;
  /// Valid iff has_timestamp && kind != kWallClockTs.
  std::int64_t ts_seconds = 0;
  /// Fractional part scaled to nanoseconds [0, 1'000'000'000); valid like ts_seconds.
  std::int64_t ts_fraction_ns = 0;

  /// Any run of non-space characters ("vcan0.1", "can-eth0"). For kDropCount it comes from the quoted socket name
  /// in the message. Empty for kMalformed/kErrorDetail.
  std::string interface;

  /// Valid for kData, kRtr, kFd, kXl, kError.
  std::uint32_t can_id = 0;
  bool extended = false;  ///< 29-bit id (8 hex digits in log format)

  /// Only populated for kData (0-8 bytes).
  std::vector<std::uint8_t> data;

  /// RX/TX direction, when the line carries one (`-x` in log format; the screen format's RX/TX column).
  bool direction_known = false;
  bool is_tx = false;

  /// kDropCount only: the "dropped %u CAN frames" count, 0 if the message did not parse (still kDropCount).
  std::uint64_t dropped_count = 0;
};

/// Does not recognize DROPCOUNT/error-detail lines -- see parseLine().
ParsedLine parseLogLine(std::string_view line);

/// Does not recognize DROPCOUNT/error-detail lines -- see parseLine().
ParsedLine parseScreenLine(std::string_view line);

/// Entry point for callers. Recognizes the two shapes outside both grammars first (TAB-prefixed error-detail
/// continuation, "DROPCOUNT:" notification), then parses the shared prefix ONCE and tries the log-format body
/// (upstream's own precedence) before the screen-format body. The format is detected per line.
ParsedLine parseLine(std::string_view line);

/// seconds * 1e9 + fraction, saturating on overflow. Absolute-vs-relative semantics are TimeMode's job.
std::int64_t rawTimestampNs(const ParsedLine& line);

/// How to turn each line's raw timestamp into absolute nanoseconds.
enum class TimeMode {
  kAbsolute,           ///< raw ns IS the absolute (epoch) timestamp
  kRelativeMonotonic,  ///< `-tz`-style: raw ns is already a non-decreasing offset since start
  kRelativeDelta,      ///< `-td`-style: raw ns is the delta since the PREVIOUS frame; accumulate
};

/// True when `line` carries a numeric "(s.frac)" timestamp, whatever its kind (including an unparsable body behind
/// a valid prefix). Wall-clock (`-t a`) timestamps are not numeric; DROPCOUNT / `-e` detail lines have none.
[[nodiscard]] bool hasNumericTimestamp(const ParsedLine& line);

/// Turns each line's raw timestamp into its position on the import timeline. Must be fed EVERY parsed line of the
/// file, in order, not only the ones that become records: `candump -t d` prints a delta on every received frame
/// regardless of type, so the running sum must include RTR/error/FD/XL and unparsable lines or every later frame
/// lands early. Returns nullopt for lines without a numeric timestamp, otherwise the raw ns (absolute/monotonic
/// mode) or the running sum of deltas (delta mode).
class TimelineClock {
 public:
  explicit TimelineClock(TimeMode mode) : mode_(mode) {}

  std::optional<std::int64_t> advance(const ParsedLine& line);

 private:
  TimeMode mode_;
  std::int64_t cumulative_ns_ = 0;
};

/// Below this many integer seconds, a timestamp is assumed to be relative
/// (an absolute Unix time this small is September 2001 or earlier).
constexpr std::int64_t kAbsoluteThresholdSeconds = 1'000'000'000;

struct TimeModeDetection {
  TimeMode mode = TimeMode::kAbsolute;
  /// False if no scanned line carried a numeric timestamp; the caller should report "not a candump capture"
  /// rather than import zero-length data.
  bool saw_numeric_timestamp = false;
};

/// Decides a TimeMode from the raw timestamps of up to `max_lines` non-blank lines: first numeric timestamp
/// >= kAbsoluteThresholdSeconds -> kAbsolute; otherwise kRelativeMonotonic if the scanned timestamps are
/// non-decreasing, else kRelativeDelta. Leaves `stream` at EOF or partway through; callers that need the data
/// must seek back. A separate bounded pass, not a second full parse of a multi-gigabyte file.
TimeModeDetection detectTimeMode(std::istream& stream, std::uint64_t max_lines);

/// Line cap shared by the dialog's preview and the source's time-mode pre-pass.
constexpr std::uint64_t kScanCap = 200000;

/// Per-interface counts collected by prescanCandump(), in first-seen order.
struct InterfacePrescanStats {
  std::string interface;
  std::uint64_t frames = 0;
  std::size_t distinct_ids = 0;
};

/// Counts of line kinds that are recognized but not decoded, plus the two shapes (DROPCOUNT, `-e` detail) that
/// carry no frame. The append*() helpers give the dialog preview and the source's post-import summary identical
/// wording; the RTR/FD/XL/error breakdown is reported differently by each (dialog: combined unsupported();
/// source: one line per kind), so those fields are exposed directly.
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

/// Everything the dialog's summary needs from one bounded forward scan, so lines are never re-parsed just to
/// learn the grammar or the time mode.
struct PrescanResult {
  std::uint64_t lines_scanned = 0;
  bool truncated = false;  ///< true if `max_lines` was hit before EOF
  std::uint64_t malformed = 0;
  std::uint64_t log_shaped = 0;
  std::uint64_t screen_shaped = 0;
  RecognizedCounters recognized;
  std::vector<InterfacePrescanStats> interfaces;
  TimeModeDetection time_mode;

  /// Raw nanoseconds of the FIRST numeric timestamp seen (the one time_mode is decided from).
  bool have_first_timestamp = false;
  std::int64_t first_timestamp_ns = 0;
};

/// One pass over up to `max_lines` non-blank lines, parsing each with parseLine(). The TimeMode decision applies
/// detectTimeMode()'s rule to the lines already parsed. Leaves `stream` at EOF or partway through.
PrescanResult prescanCandump(std::istream& stream, std::uint64_t max_lines);

}  // namespace candump_detail
