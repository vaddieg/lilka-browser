// Charset sniffing + single-byte legacy encoding -> UTF-8 transcoding.
// Platform independent (tested on host).
#pragma once
#include <cstddef>
#include <string>

namespace lilka {
namespace charset {

/// Normalize a charset name from a header/meta tag ("windows-1251",
/// "CP1251", "koi8-r", "utf-8"...) to a canonical id:
/// "utf8", "cp1251", "koi8r", or "" if unknown/absent.
std::string canonical(const std::string& name);

/// If `src` starts with a UTF-8 BOM, strip it (out = src without BOM).
/// Otherwise out = src (moved when `move` is true).
void stripBom(const std::string& src, std::string& out);

/// Convert a single-byte encoded string to UTF-8. `enc` is a canonical
/// id ("cp1251", "koi8r"). Unknown enc -> out = src unchanged.
void toUtf8(const std::string& src, const char* enc, std::string& out);

/// Decide the source charset of an HTML body:
///  1. explicit `httpCharset` (from Content-Type) if known;
///  2. otherwise sniff `<meta charset=...>` / `<meta http-equiv=Content-Type ...>`
///     in the first ~2 KB;
///  3. otherwise validate UTF-8 and fall back to "cp1251" for bytes that
///     look like legacy Cyrillic.
/// Returns a canonical id or "utf8".
std::string detect(const std::string& html, const std::string& httpCharset);

}  // namespace charset
}  // namespace lilka
