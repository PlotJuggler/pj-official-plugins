# Shared CAN/DBC decoder (pj_can_dbc): turns raw CAN frames (id, extended, bytes)
# into named DBC signals. Consumed by data_load_mf4 and data_load_blf.
#
# dbc_parser_cpp (+ the header-only fast_float) is vendored via CPM DOWNLOAD_ONLY
# and built as a static `dbc` lib here; it is PRIVATE-linked and fully hidden
# behind can_decoder's pImpl, so consumers never see a DBC header. Both deps are
# pinned by commit tarball + SHA256. Handles little- and big-endian signals.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/CPM.cmake)

CPMAddPackage(
  NAME fast_float
  URL https://github.com/fastfloat/fast_float/archive/1ea4f27b2aeee2859a1354a3c24cff52a116cad1.tar.gz
  URL_HASH SHA256=06f860e3adb994b9c8523bb53a76a335c4182e27fe5a077246146d283efdc9da
  DOWNLOAD_ONLY YES
)
CPMAddPackage(
  NAME dbc_parser_cpp
  URL https://github.com/LinuxDevon/dbc_parser_cpp/archive/2c8acdbd2d1ea33545f99f9a0d23e10b7f8e0852.tar.gz
  URL_HASH SHA256=91e12166d83c56f1ff945f6c94f14a7fe80e105219111f984645614ef8e09c57
  DOWNLOAD_ONLY YES
)
file(GLOB _dbc_sources CONFIGURE_DEPENDS ${dbc_parser_cpp_SOURCE_DIR}/src/*.cpp)

# Vendored fix: upstream masks unsigned signals with a 32-bit `1 << size`,
# which is UB for size >= 32 (an unsigned 32-bit signal decodes as 0 on x86).
# Rewrite message.cpp with a 64-bit-safe mask and compile the patched copy.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/VendoredPatch.cmake)
set(_dbc_mask_bug "value = value & ((1 << signal.size) - 1); // masking")
set(_dbc_mask_fix "value = (signal.size >= 64) ? value : (value & ((1ULL << signal.size) - 1)); // masking (PJ fix)")
file(READ ${dbc_parser_cpp_SOURCE_DIR}/src/message.cpp _dbc_message_cpp)
pj_apply_vendored_patch(_dbc_message_cpp _dbc_mask_bug _dbc_mask_fix "can_dbc: dbc_parser_cpp unsigned-mask")
set(_dbc_patched_dir ${CMAKE_CURRENT_BINARY_DIR}/dbc_patched)
file(CONFIGURE OUTPUT ${_dbc_patched_dir}/message.cpp CONTENT "${_dbc_message_cpp}" @ONLY)
list(REMOVE_ITEM _dbc_sources ${dbc_parser_cpp_SOURCE_DIR}/src/message.cpp)
list(APPEND _dbc_sources ${_dbc_patched_dir}/message.cpp)

# Vendored fix: dbc.cpp's SG_/BO_ number and whitespace patterns are too
# narrow, and a signal whose factor/offset/min/max does not match is dropped
# SILENTLY (signal_re just fails to match, parse_dbc_messages() skips the
# line) -- this already affects real Vector DBCs (negative factors, float
# min/max like "[-3.4E+38|3.4E+38]") consumed by data_load_blf/data_load_mf4.
#
# 1) floatPattern/scalePattern: one signed-mantissa + optional-exponent number
#    pattern for factor, offset, min AND max (was: factor non-negative only,
#    offset/min/max signed but no exponent support at all). A leading '+' is
#    matched but kept OUTSIDE the capture group, so the captured text is
#    never handed a '+' mantissa sign -- Utils::String::convert_to_double
#    (== fast_float::from_chars) rejects that, same rule as std::from_chars,
#    which would otherwise silently leave the field at its default (0.0).
#    This keeps EXACTLY 2 capturing groups per use (the whole value + one
#    filler group, same as upstream's (whole value, fractional digits)), so
#    SIGNAL_FACTOR_GROUP=7 / SIGNAL_OFFSET_GROUP=9 / SIGNAL_MIN_GROUP=11 /
#    SIGNAL_MAX_GROUP=13 / SIGNAL_UNIT_GROUP=15 / SIGNAL_RECIEVER_GROUP=16
#    below are untouched -- pj_can_dbc_test pins this mapping.
# 2) whiteSpace/message_re: single "\s" separators reject tabs and repeated
#    spaces; split into a mandatory "\s+" between two real tokens and a
#    permissive "\s*" next to punctuation (":", "(", "[", the quote), which
#    also happens to tolerate the punctuation-adjacent spacing some DBC
#    exporters omit.
set(_dbc_float_bug [==[const auto floatPattern = "(-?\\d+\\.?(\\d+)?)"; // Can be negative]==])
set(_dbc_float_fix [==[const auto floatPattern = "\\+?([-]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][+-]?(\\d+))?)"; // PJ fix: signed mantissa (leading-dot too) + optional exponent, uncaptured leading '+'; still exactly 2 capture groups (whole value, exponent digits) like upstream (whole value, fractional digits)]==])
set(_dbc_scale_bug [==[const auto scalePattern = "(\\d+\\.?(\\d+)?)"; // Non negative float]==])
set(_dbc_scale_fix [==[const auto scalePattern = floatPattern; // PJ fix: factor can be negative/exponential too (was non-negative-only); reusing floatPattern keeps capture-group numbering identical]==])
set(_dbc_whitespace_bug [==[const auto whiteSpace = "\\s";]==])
set(_dbc_whitespace_fix [==[const auto whiteSpace = "\\s+"; // PJ fix: allow tabs / repeated spaces between tokens (was exactly one \s)
const auto whiteSpaceOpt = "\\s*"; // PJ fix: allow tabs / repeated / absent spaces next to punctuation]==])
set(_dbc_signal_re_bug [==[	signal_re(std::string("^") + whiteSpace + signalIdentifierPattern + whiteSpace + namePattern + whiteSpace + "\\:" + whiteSpace + bitStartPattern + "\\|"
			  + lengthPattern + "\\@" + byteOrderPattern + signPattern + whiteSpace + offsetScalePattern + whiteSpace + minMaxPattern + whiteSpace + unitPattern
			  + whiteSpace + receiverPattern) {]==])
set(_dbc_signal_re_fix [==[	signal_re(std::string("^") + whiteSpace + signalIdentifierPattern + whiteSpace + namePattern + whiteSpaceOpt + "\\:" + whiteSpaceOpt + bitStartPattern + "\\|"
			  + lengthPattern + "\\@" + byteOrderPattern + signPattern + whiteSpaceOpt + offsetScalePattern + whiteSpaceOpt + minMaxPattern + whiteSpaceOpt + unitPattern
			  + whiteSpaceOpt + receiverPattern) {]==])
set(_dbc_message_re_bug [==[	, message_re("^(BO_)\\s(\\d+)\\s(\\w+)\\:\\s(\\d+)\\s(\\w+|Vector__XXX)")]==])
set(_dbc_message_re_fix [==[	, message_re("^(BO_)\\s+(\\d+)\\s+(\\w+)\\:\\s*(\\d+)\\s+(\\w+|Vector__XXX)")]==])

file(READ ${dbc_parser_cpp_SOURCE_DIR}/src/dbc.cpp _dbc_dbc_cpp)
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_float_bug _dbc_float_fix "can_dbc: dbc_parser_cpp float number pattern")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_scale_bug _dbc_scale_fix "can_dbc: dbc_parser_cpp scale number pattern")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_whitespace_bug _dbc_whitespace_fix "can_dbc: dbc_parser_cpp whitespace patterns")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_signal_re_bug _dbc_signal_re_fix "can_dbc: dbc_parser_cpp signal_re whitespace")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_message_re_bug _dbc_message_re_fix "can_dbc: dbc_parser_cpp message_re whitespace")

# Vendored fix: VAL_ (value tables) parsing rejects tabs/repeated whitespace
# (value_re predates the shared whiteSpace variable) and reads every key as
# non-negative (std::stoul), so a negative key ("-1 \"ERROR\"") throws and
# drops the WHOLE VAL_ line silently. Fix: value_re/description_re tolerate
# flexible whitespace (still 6 capture groups; only 2/3/4 consumed below);
# the key is read with std::stoll and narrowed to uint32_t's bit pattern --
# pj_can_dbc::normalizeValueKey (can_decoder.cpp) restores the sign at load
# time. Out of scope: VAL_TABLE_, an env-var VAL_, a multi-line VAL_, and an
# escaped quote in label text.
set(_dbc_value_re_bug [==[	, value_re("^(VAL_)\\s(\\d+)\\s(\\w+)((?:\\s(\\d+)\\s\"([^\"]*)\")+)\\s;$")]==])
set(_dbc_value_re_fix [==[	, value_re("^(VAL_)\\s+(\\d+)\\s+(\\w+)((?:\\s+(-?\\d+)\\s+\"([^\"]*)\")+)\\s*;\\s*$")]==])
set(_dbc_description_re_bug [==[			std::regex description_re("\\s(\\d+)\\s\"([^\"]*)\"");]==])
set(_dbc_description_re_fix [==[			std::regex description_re("\\s+(-?\\d+)\\s+\"([^\"]*)\"");]==])
set(_dbc_value_number_bug [==[				uint32_t number = static_cast<uint32_t>(std::stoul(desc_match.str(1)));]==])
set(_dbc_value_number_fix [==[				uint32_t number = static_cast<uint32_t>(std::stoll(desc_match.str(1)));  // PJ fix: stoll accepts a leading minus sign (stoul throws out_of_range on it); narrowing a negative long long to uint32_t gives its 32-bit twos-complement bit pattern (sign restored in pj_can_dbc from Signal::is_signed/size)]==])

pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_value_re_bug _dbc_value_re_fix "can_dbc: dbc_parser_cpp value_re whitespace/negative-key")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_description_re_bug _dbc_description_re_fix "can_dbc: dbc_parser_cpp VAL_ description whitespace/negative-key")
pj_apply_vendored_patch(_dbc_dbc_cpp _dbc_value_number_bug _dbc_value_number_fix "can_dbc: dbc_parser_cpp VAL_ negative key cast")
file(CONFIGURE OUTPUT ${_dbc_patched_dir}/dbc.cpp CONTENT "${_dbc_dbc_cpp}" @ONLY)
list(REMOVE_ITEM _dbc_sources ${dbc_parser_cpp_SOURCE_DIR}/src/dbc.cpp)
list(APPEND _dbc_sources ${_dbc_patched_dir}/dbc.cpp)

add_library(dbc STATIC ${_dbc_sources})
set_target_properties(dbc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(dbc PRIVATE cxx_std_20)
target_include_directories(dbc SYSTEM PUBLIC ${dbc_parser_cpp_SOURCE_DIR}/include)
target_include_directories(dbc SYSTEM PRIVATE ${fast_float_SOURCE_DIR}/include)

add_library(pj_can_dbc STATIC can_decoder.cpp can_topic.cpp signal_row.cpp)
# Static lib goes inside SHARED plugin .so files -> must be position-independent.
set_target_properties(pj_can_dbc PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(pj_can_dbc PUBLIC cxx_std_20)
if(DEFINED PJ_WARNING_FLAGS)
  target_compile_options(pj_can_dbc PRIVATE ${PJ_WARNING_FLAGS})
endif()
target_include_directories(pj_can_dbc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# dbc is PRIVATE: can_decoder.hpp hides it behind a pImpl (no dbc header leaks).
target_link_libraries(pj_can_dbc PUBLIC plotjuggler_sdk::base PRIVATE dbc)

# GTest-guarded so this common target can be configured unconditionally.
find_package(GTest QUIET CONFIG)
if(TARGET GTest::gtest_main)
  add_executable(pj_can_dbc_test tests/can_decoder_test.cpp)
  target_compile_features(pj_can_dbc_test PRIVATE cxx_std_20)
  # field_test_helpers.hpp: shared findField() test helper, also used by
  # data_load_blf/data_load_candump's tests.
  target_include_directories(pj_can_dbc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  if(DEFINED PJ_WARNING_FLAGS)
    target_compile_options(pj_can_dbc_test PRIVATE ${PJ_WARNING_FLAGS})
  endif()
  target_link_libraries(pj_can_dbc_test PRIVATE pj_can_dbc GTest::gtest_main)
  add_test(NAME pj_can_dbc_test COMMAND pj_can_dbc_test)
endif()
