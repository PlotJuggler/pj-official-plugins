/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tls_utils.h"

#include <gtest/gtest.h>

#include <string>

namespace {

// Assembled from repeated characters so no key-shaped literal is ever committed:
// the body is 32 'a's and the fingerprint 8 '0's, matching the grammar
// (msco_ + 32 chars + _ + 8 hex) without resembling an issued key.
const std::string kBody(32, 'a');
const std::string kFingerprint(8, '0');
const std::string kValidKey = "msco_" + kBody + "_" + kFingerprint;

}  // namespace

TEST(TlsUtils, ValidApiKey) {
  EXPECT_TRUE(isValidApiKey(kValidKey));
}

TEST(TlsUtils, ApiKeyWrongPrefix) {
  EXPECT_FALSE(isValidApiKey("xxxx_" + kBody + "_" + kFingerprint));
}

TEST(TlsUtils, ApiKeyTooShort) {
  EXPECT_FALSE(isValidApiKey("msco_abc_" + kFingerprint));
}

TEST(TlsUtils, ApiKeyEmpty) {
  EXPECT_FALSE(isValidApiKey(""));
}

TEST(TlsUtils, ApiKeyBadFingerprint) {
  EXPECT_FALSE(isValidApiKey("msco_" + kBody + "_" + std::string(8, 'z')));
}

// --- isPrintableAscii (connect-path control-byte gate, gap #1) ---

TEST(TlsUtils, PrintableAsciiAcceptsTypicalUri) {
  EXPECT_TRUE(isPrintableAscii("grpc+tls://demo.mosaico.dev:6726"));
  EXPECT_TRUE(isPrintableAscii(kValidKey));
  EXPECT_TRUE(isPrintableAscii("/home/user/cert.pem"));
}

TEST(TlsUtils, PrintableAsciiAcceptsEmpty) {
  // Empty cert path / api key are valid (optional credentials).
  EXPECT_TRUE(isPrintableAscii(""));
}

TEST(TlsUtils, PrintableAsciiRejectsControlBytes) {
  // CR / LF / NUL / TAB are exactly what gRPC asserts-and-aborts on.
  EXPECT_FALSE(isPrintableAscii("host:6726\r\n"));
  EXPECT_FALSE(isPrintableAscii("host:6726\n"));
  EXPECT_FALSE(isPrintableAscii(std::string("host\0port", 9)));
  EXPECT_FALSE(isPrintableAscii("host\t6726"));
}

TEST(TlsUtils, PrintableAsciiRejectsNonAscii) {
  // High-bit / multibyte UTF-8 bytes are not printable ASCII.
  EXPECT_FALSE(isPrintableAscii("hÃ¶st:6726"));  // contains 0xC3-prefixed bytes
  EXPECT_FALSE(isPrintableAscii("\x7f"));        // DEL is excluded (> 0x7E)
}

TEST(TlsUtils, PrintableAsciiBoundaryBytes) {
  // 0x20 (space) and 0x7E (~) are the inclusive printable bounds.
  EXPECT_TRUE(isPrintableAscii(" ~"));
  EXPECT_FALSE(isPrintableAscii(std::string(1, '\x1f')));  // just below 0x20
}
