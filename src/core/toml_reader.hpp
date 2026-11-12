// Strict TOML table reader: typed getters plus unknown-key detection.
// Only included from .cpp files (keeps toml++ out of most translation units).
#pragma once

#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

#include "gpuoc/result.hpp"

namespace gpuoc {

class TomlTableReader {
 public:
  TomlTableReader(const toml::table* tbl, std::string path) : tbl_(tbl), path_(std::move(path)) {}

  [[nodiscard]] bool present() const { return tbl_ != nullptr; }
  [[nodiscard]] const std::string& path() const { return path_; }

  Result<void> get_bool(std::string_view key, bool& out) { return get_impl<bool>(key, out, "a boolean"); }
  Result<void> get_string(std::string_view key, std::string& out) { return get_impl<std::string>(key, out, "a string"); }

  Result<void> get_int(std::string_view key, int& out, long long min, long long max) {
    known_.insert(std::string(key));
    if (!tbl_) return success();
    const toml::node* n = tbl_->get(key);
    if (!n) return success();
    auto v = n->value<int64_t>();
    if (!v || !n->is_integer()) return type_error(key, "an integer");
    if (*v < min || *v > max)
      return make_error(ErrorCode::ParseError,
                        std::format("{}.{} = {} is out of range [{}, {}]", path_, key, *v, min, max));
    out = static_cast<int>(*v);
    return success();
  }

  Result<void> get_opt_int(std::string_view key, std::optional<int>& out, long long min, long long max) {
    known_.insert(std::string(key));
    if (!tbl_ || !tbl_->get(key)) return success();
    int v = 0;
    GPUOC_TRY(get_int(key, v, min, max));
    out = v;
    return success();
  }

  Result<void> get_double(std::string_view key, double& out, double min, double max) {
    known_.insert(std::string(key));
    if (!tbl_) return success();
    const toml::node* n = tbl_->get(key);
    if (!n) return success();
    std::optional<double> v;
    if (n->is_floating_point()) v = n->value<double>();
    if (n->is_integer()) v = static_cast<double>(*n->value<int64_t>());
    if (!v) return type_error(key, "a number");
    if (!(*v >= min && *v <= max))
      return make_error(ErrorCode::ParseError,
                        std::format("{}.{} = {} is out of range [{}, {}]", path_, key, *v, min, max));
    out = *v;
    return success();
  }

  const toml::table* sub_table(std::string_view key) {
    known_.insert(std::string(key));
    if (!tbl_) return nullptr;
    const toml::node* n = tbl_->get(key);
    return n ? n->as_table() : nullptr;
  }
  const toml::array* array(std::string_view key) {
    known_.insert(std::string(key));
    if (!tbl_) return nullptr;
    const toml::node* n = tbl_->get(key);
    return n ? n->as_array() : nullptr;
  }
  void mark_known(std::string_view key) { known_.insert(std::string(key)); }

  // Error if the table contains keys that were never asked for.
  Result<void> finish() const {
    if (!tbl_) return success();
    for (const auto& [k, v] : *tbl_) {
      (void)v;
      if (!known_.count(std::string(k.str())))
        return make_error(ErrorCode::ParseError, std::format("unknown key '{}' in [{}]", k.str(), path_));
    }
    return success();
  }

 private:
  template <class T>
  Result<void> get_impl(std::string_view key, T& out, const char* what) {
    known_.insert(std::string(key));
    if (!tbl_) return success();
    const toml::node* n = tbl_->get(key);
    if (!n) return success();
    auto v = n->value_exact<T>();
    if (!v) return type_error(key, what);
    out = *v;
    return success();
  }
  Result<void> type_error(std::string_view key, const char* what) const {
    return make_error(ErrorCode::ParseError, std::format("{}.{} must be {}", path_, key, what));
  }

  const toml::table* tbl_;
  std::string path_;
  std::set<std::string> known_;
};

inline Result<toml::table> parse_toml(std::string_view text, const std::string& source) {
  toml::parse_result r = toml::parse(text, std::string_view(source));
  if (!r) {
    const auto& e = r.error();
    return make_error(ErrorCode::ParseError,
                      std::format("{}:{}:{}: {}", source, e.source().begin.line, e.source().begin.column,
                                  std::string(e.description())));
  }
  return std::move(r).table();
}

}  // namespace gpuoc
