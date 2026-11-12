// File-system helpers. Paths are UTF-8 internally; converted to UTF-16 at the
// Windows API boundary.
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "gpuoc/result.hpp"

namespace gpuoc::platform {

#ifdef _WIN32
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);
#endif

std::filesystem::path u8path(const std::string& utf8);
std::string path_utf8(const std::filesystem::path& p);

std::FILE* fopen_utf8(const std::string& path, const char* mode);

Result<std::string> read_file(const std::filesystem::path& p);
// Write via a temporary file and rename, so readers never see a partial file.
Result<void> write_file_atomic(const std::filesystem::path& p, const std::string& contents);
// Append one line and flush it to disk before returning.
Result<void> append_line_durable(const std::filesystem::path& p, const std::string& line);
Result<void> ensure_dir(const std::filesystem::path& p);

std::string getenv_utf8(const char* name);
void setenv_utf8(const char* name, const std::string& value);  // empty value unsets

}  // namespace gpuoc::platform
