// test_utils.cpp
//
// Tests for the inline utility functions in utils.h:
//   JsonStr         — JSON string escaping
//   WstrToUtf8      — wide → UTF-8
//   Utf8ToWstr      — UTF-8 → wide

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <stdexcept>
#include <cstdio>

#include "../Source/theaudioapp/utils.h"

// ── Test framework ────────────────────────────────────────────────────────────

static int g_passed = 0, g_failed = 0;

static void check(bool cond, const char* expr, int line) {
    if (!cond) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expr);
}
#define CHECK(x) check((x), #x, __LINE__)

static void runTest(const char* name, void(*fn)()) {
    try {
        fn();
        printf("  PASS  %s\n", name);
        g_passed++;
    } catch (const std::exception& e) {
        printf("  FAIL  %s  [%s]\n", name, e.what());
        g_failed++;
    }
}
#define RUN(fn) runTest(#fn, fn)

// ── JsonStr tests ─────────────────────────────────────────────────────────────

static void jsonstr_plain_string() {
    CHECK(JsonStr("hello") == "\"hello\"");
}

static void jsonstr_empty_string() {
    CHECK(JsonStr("") == "\"\"");
}

static void jsonstr_escapes_double_quote() {
    CHECK(JsonStr("say \"hi\"") == "\"say \\\"hi\\\"\"");
}

static void jsonstr_escapes_backslash() {
    CHECK(JsonStr("C:\\path\\to\\file") == "\"C:\\\\path\\\\to\\\\file\"");
}

static void jsonstr_escapes_both() {
    CHECK(JsonStr("\"C:\\x\"") == "\"\\\"C:\\\\x\\\"\"");
}

static void jsonstr_does_not_escape_forward_slash() {
    CHECK(JsonStr("a/b") == "\"a/b\"");
}

static void jsonstr_does_not_escape_newline_chars() {
    // JsonStr only escapes " and \; control chars pass through unchanged.
    // This is intentional — sessions names from Windows will never contain raw \n.
    std::string in = "ab\ncd";
    std::string out = JsonStr(in);
    CHECK(out == "\"ab\ncd\"");
}

// ── WstrToUtf8 / Utf8ToWstr round-trip tests ─────────────────────────────────

static void wstr_utf8_roundtrip_ascii() {
    std::wstring original = L"Discord";
    CHECK(Utf8ToWstr(WstrToUtf8(original)) == original);
}

static void wstr_utf8_roundtrip_empty() {
    CHECK(WstrToUtf8(L"") == "");
    CHECK(Utf8ToWstr("") == L"");
}

static void wstr_utf8_roundtrip_unicode() {
    // U+30B2 U+30FC U+30E0 = katakana "game" — verifies multi-byte encoding
    std::wstring original = { (wchar_t)0x30B2, (wchar_t)0x30FC, (wchar_t)0x30E0 };
    CHECK(Utf8ToWstr(WstrToUtf8(original)) == original);
}

static void wstr_utf8_roundtrip_special_ascii() {
    std::wstring original = L"C:\\Program Files\\app.exe";
    CHECK(Utf8ToWstr(WstrToUtf8(original)) == original);
}

static void utf8_to_wstr_known_value() {
    // U+00E9 = LATIN SMALL LETTER E WITH ACUTE, encoded as \xc3\xa9 in UTF-8
    std::string utf8 = "caf\xc3\xa9";
    std::wstring wide = Utf8ToWstr(utf8);
    std::wstring expected = { L'c', L'a', L'f', (wchar_t)0x00E9 };
    CHECK(wide == expected);
}

static void wstr_to_utf8_known_value() {
    std::wstring wide = { L'c', L'a', L'f', (wchar_t)0x00E9 };
    CHECK(WstrToUtf8(wide) == "caf\xc3\xa9");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    printf("=== utils tests ===\n");
    RUN(jsonstr_plain_string);
    RUN(jsonstr_empty_string);
    RUN(jsonstr_escapes_double_quote);
    RUN(jsonstr_escapes_backslash);
    RUN(jsonstr_escapes_both);
    RUN(jsonstr_does_not_escape_forward_slash);
    RUN(jsonstr_does_not_escape_newline_chars);
    RUN(wstr_utf8_roundtrip_ascii);
    RUN(wstr_utf8_roundtrip_empty);
    RUN(wstr_utf8_roundtrip_unicode);
    RUN(wstr_utf8_roundtrip_special_ascii);
    RUN(utf8_to_wstr_known_value);
    RUN(wstr_to_utf8_known_value);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
