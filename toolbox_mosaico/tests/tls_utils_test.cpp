/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "tls_utils.h"

#include <gtest/gtest.h>

#include <string>

TEST(TlsUtils, ValidApiKey) {
  EXPECT_TRUE(isValidApiKey("msco_s3l8gcdwuadege3pkhou0k0n2t5omfij_f9010b9e"));
}

TEST(TlsUtils, ApiKeyWrongPrefix) {
  EXPECT_FALSE(isValidApiKey("xxxx_s3l8gcdwuadege3pkhou0k0n2t5omfij_f9010b9e"));
}

TEST(TlsUtils, ApiKeyTooShort) {
  EXPECT_FALSE(isValidApiKey("msco_abc_12345678"));
}

TEST(TlsUtils, ApiKeyEmpty) {
  EXPECT_FALSE(isValidApiKey(""));
}

TEST(TlsUtils, ApiKeyBadFingerprint) {
  EXPECT_FALSE(isValidApiKey("msco_s3l8gcdwuadege3pkhou0k0n2t5omfij_zzzzzzzz"));
}

// --- isPrintableAscii (connect-path control-byte gate, gap #1) ---

TEST(TlsUtils, PrintableAsciiAcceptsTypicalUri) {
  EXPECT_TRUE(isPrintableAscii("grpc+tls://demo.mosaico.dev:6726"));
  EXPECT_TRUE(isPrintableAscii("msco_s3l8gcdwuadege3pkhou0k0n2t5omfij_f9010b9e"));
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
