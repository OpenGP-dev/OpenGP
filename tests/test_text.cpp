// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the games' text on the console (runtime/os/printf.h).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <string>

#include "os/printf.h"

TEST_CASE("sjis: ASCII is unchanged") {
  CHECK(os::sjis_to_utf8("") == "");
  CHECK(os::sjis_to_utf8("PCB ID : 1 OK\n") == "PCB ID : 1 OK\n");
}

TEST_CASE("sjis: Japanese becomes UTF-8") {
  // The media board's "OK" in GP 2's network library: "正常終了" (completed normally).
  CHECK(os::sjis_to_utf8("AC_MEDIACMD_OK : \x90\xb3\x8f\xed\x8fI\x97\xb9") ==
        "AC_MEDIACMD_OK : \xe6\xad\xa3\xe5\xb8\xb8\xe7\xb5\x82\xe4\xba\x86");
  // Half-width katakana are one byte in Shift JIS and three in UTF-8: "ｿｹｯﾄ" (socket).
  CHECK(os::sjis_to_utf8("\xbf\xb9\xaf\xc4") == "\xef\xbd\xbf\xef\xbd\xb9\xef\xbd\xaf\xef\xbe\x84");
}

TEST_CASE("sjis: bytes that are not Shift JIS come back unchanged") {
  // A lead byte at the end, with nothing after it.
  CHECK(os::sjis_to_utf8("abc\x90") == "abc\x90");
}
