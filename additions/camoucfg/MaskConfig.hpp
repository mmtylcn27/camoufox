/*
Helper to extract values from the CAMOU_CONFIG environment variable(s).
Written by daijro.
*/
#pragma once
#include "simdjson.h"
#include <string>
#include <tuple>
#include <optional>
#include <mutex>
#include "mozilla/glue/Debug.h"
#include <cstdlib>
#include <cstdio>
#include <variant>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <cctype>
#include "mozilla/RWLock.h"
#include <unordered_map>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace MaskConfig {

// Function to get the value of an environment variable as a UTF-8 string.
inline std::optional<std::string> get_env_utf8(const std::string& name) {
#ifdef _WIN32
  std::wstring wName(name.begin(), name.end());
  DWORD size = GetEnvironmentVariableW(wName.c_str(), nullptr, 0);
  if (size == 0) return std::nullopt;  // Environment variable not found

  std::vector<wchar_t> buffer(size);
  DWORD result = GetEnvironmentVariableW(wName.c_str(), buffer.data(), size);
  if (result == 0 || result >= size) return std::nullopt;
  std::wstring wValue(buffer.data(), result);

  // Convert UTF-16 to UTF-8 using WideCharToMultiByte
  int utf8Size = WideCharToMultiByte(CP_UTF8, 0, wValue.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (utf8Size == 0) return std::nullopt;
  
  std::vector<char> utf8Buffer(utf8Size);
  WideCharToMultiByte(CP_UTF8, 0, wValue.c_str(), -1, utf8Buffer.data(), utf8Size, nullptr, nullptr);
  return std::string(utf8Buffer.data());
#else
  const char* value = std::getenv(name.c_str());
  if (!value) return std::nullopt;
  return std::string(value);
#endif
}


// Parse CAMOU_CONFIG (and CAMOU_CONFIG_1..N) exactly once, keep it alive forever.
// NOTE: Returned dom::element references internal data owned by the static parser/tape.
inline const simdjson::dom::element& GetJson() {
  static std::once_flag initFlag;

  static simdjson::padded_string jsonPadded;
  static simdjson::dom::parser parser;
  static simdjson::dom::element root;

  std::call_once(initFlag, []() {
    std::vector<std::string> parts;
    int index = 1;
    size_t totalSize = 0;

    while (true) {
      std::string envName = "CAMOU_CONFIG_" + std::to_string(index);
      auto partial = get_env_utf8(envName);
      if (!partial) break;
      totalSize += partial->size();
      parts.push_back(std::move(*partial));
      ++index;
    }

    std::string jsonString;
    jsonString.reserve(totalSize);
    for (const auto& p : parts) jsonString += p;

    if (jsonString.empty()) {
      // Fallback to single CAMOU_CONFIG
      auto original = get_env_utf8("CAMOU_CONFIG");
      if (original) jsonString = *original;
    }

    if (jsonString.empty()) {
      jsonPadded = simdjson::padded_string(std::string_view("{}"));
      (void)parser.parse(jsonPadded).get(root);
      return;
    }

    jsonPadded = simdjson::padded_string(std::move(jsonString));

    auto err = parser.parse(jsonPadded).get(root);
    if (err) {
      printf_stderr("ERROR: Invalid JSON passed to CAMOU_CONFIG!\n");
      jsonPadded = simdjson::padded_string(std::string_view("{}"));
      (void)parser.parse(jsonPadded).get(root);
    }
  });

  return root;
}

inline std::optional<std::string> GetString(const std::string& key) {
  const auto& data = GetJson();
  simdjson::dom::object obj;
  if (data.get(obj)) return std::nullopt;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return std::nullopt;

  std::string_view sv;
  if (el.get(sv)) return std::nullopt;
  return std::string(sv);
}

inline std::vector<std::string> GetStringList(const std::string& key) {
  std::vector<std::string> result;
  const auto& data = GetJson();

  simdjson::dom::object obj;
  if (data.get(obj)) return result;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return result;

  simdjson::dom::array arr;
  if (el.get(arr)) return result;

  for (auto v : arr) {
    std::string_view sv;
    if (!v.get(sv)) result.emplace_back(sv);
  }
  return result;
}

inline std::vector<std::string> GetStringListLower(const std::string& key) {
  static mozilla::RWLock cacheLock("GetStringListLowerCacheLock");
  static std::unordered_map<std::string, std::vector<std::string>> cache;

  {
    mozilla::AutoReadLock readLock(cacheLock);
    auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
  }

  auto result = GetStringList(key);
  for (auto& s : result) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  }

  {
    mozilla::AutoWriteLock writeLock(cacheLock);
    auto [it, inserted] = cache.emplace(key, std::move(result));
    return it->second;
  }
}

template <typename T>
inline std::optional<T> GetUintImpl(const std::string& key) {
  const auto& data = GetJson();
  simdjson::dom::object obj;
  if (data.get(obj)) return std::nullopt;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return std::nullopt;

  uint64_t u = 0;
  if (!el.get(u)) {
    if (u > static_cast<uint64_t>(std::numeric_limits<T>::max())) return std::nullopt;
    return static_cast<T>(u);
  }

  // Allow signed integers if non-negative
  int64_t i = 0;
  if (!el.get(i) && i >= 0) {
    if (static_cast<uint64_t>(i) > static_cast<uint64_t>(std::numeric_limits<T>::max())) return std::nullopt;
    return static_cast<T>(i);
  }

  return std::nullopt;
}

inline std::optional<uint64_t> GetUint64(const std::string& key) {
  return GetUintImpl<uint64_t>(key);
}

inline std::optional<uint32_t> GetUint32(const std::string& key) {
  return GetUintImpl<uint32_t>(key);
}

inline std::optional<int32_t> GetInt32(const std::string& key) {
  const auto& data = GetJson();
  simdjson::dom::object obj;
  if (data.get(obj)) return std::nullopt;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return std::nullopt;

  int64_t v = 0;
  if (el.get(v)) return std::nullopt;
  if (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()) return std::nullopt;
  return static_cast<int32_t>(v);
}

inline std::optional<double> GetDouble(const std::string& key) {
  const auto& data = GetJson();
  simdjson::dom::object obj;
  if (data.get(obj)) return std::nullopt;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return std::nullopt;

  double d = 0.0;
  if (!el.get(d)) return d;

  // Allow integer numbers as doubles
  int64_t i = 0;
  if (!el.get(i)) return static_cast<double>(i);

  uint64_t u = 0;
  if (!el.get(u)) return static_cast<double>(u);

  return std::nullopt;
}

inline std::optional<bool> GetBool(const std::string& key) {
  const auto& data = GetJson();
  simdjson::dom::object obj;
  if (data.get(obj)) return std::nullopt;

  simdjson::dom::element el;
  if (obj.at_key(key).get(el)) return std::nullopt;

  bool b = false;
  if (el.get(b)) return std::nullopt;
  return b;
}

inline bool CheckBool(const std::string& key) {
  return GetBool(key).value_or(false);
}

inline std::optional<std::array<uint32_t, 4>> GetRect(
    const std::string& left, const std::string& top, const std::string& width,
    const std::string& height) {
  auto leftOpt = GetUint32(left);
  auto topOpt = GetUint32(top);
  auto widthOpt = GetUint32(width);
  auto heightOpt = GetUint32(height);

  if (!widthOpt.has_value() || !heightOpt.has_value()) {
    if (widthOpt.has_value() ^ heightOpt.has_value())
      printf_stderr(
          "Both %s and %s must be provided. Using default behavior.\n",
          width.c_str(), height.c_str());
    return std::nullopt;
  }

  std::array<uint32_t, 4> result = {
      leftOpt.value_or(0),
      topOpt.value_or(0),
      widthOpt.value(),
      heightOpt.value()
  };

  return result;
}

inline std::optional<std::array<int32_t, 4>> GetInt32Rect(
    const std::string& left, const std::string& top, const std::string& width,
    const std::string& height) {
  if (auto optValue = GetRect(left, top, width, height)) {
    std::array<int32_t, 4> result;
    for (size_t i = 0; i < 4; ++i) {
      if ((*optValue)[i] > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return std::nullopt;  // Overflow would occur
      }
      result[i] = static_cast<int32_t>((*optValue)[i]);
    }
    return result;
  }
  return std::nullopt;
}

// Helpers for WebGL

inline std::optional<simdjson::dom::element> GetNested(const std::string& domain,
                                                       const std::string& keyStr) {
  const auto& root = GetJson();

  simdjson::dom::object rootObj;
  if (root.get(rootObj)) return std::nullopt;

  simdjson::dom::element domainEl;
  if (rootObj.at_key(domain).get(domainEl)) return std::nullopt;

  simdjson::dom::object domainObj;
  if (domainEl.get(domainObj)) return std::nullopt;

  simdjson::dom::element out;
  if (domainObj.at_key(keyStr).get(out)) return std::nullopt;

  return out;
}

template <typename T>
inline std::optional<T> GetAttribute(const std::string& attrib, bool isWebGL2) {
  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:contextAttributes" : "webGl:contextAttributes", attrib);
  if (!value) return std::nullopt;

  if constexpr (std::is_same_v<T, bool>) {
    bool b{};
    if (value->get(b)) return std::nullopt;
    return b;
  } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
    int64_t i{};
    if (value->get(i)) return std::nullopt;
    if (i < std::numeric_limits<T>::min() || i > std::numeric_limits<T>::max())
      return std::nullopt;
    return static_cast<T>(i);
  } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
    uint64_t u{};
    if (value->get(u)) return std::nullopt;
    if (u > std::numeric_limits<T>::max()) return std::nullopt;
    return static_cast<T>(u);
  } else if constexpr (std::is_floating_point_v<T>) {
    double d{};
    if (value->get(d)) return std::nullopt;
    return static_cast<T>(d);
  } else if constexpr (std::is_same_v<T, std::string>) {
    std::string_view sv;
    if (value->get(sv)) return std::nullopt;
    return std::string(sv);
  } else {
    // Unsupported attribute type
    return std::nullopt;
  }
}

inline std::optional<std::variant<int64_t, bool, double, std::string, std::nullptr_t>> GLParam(uint32_t pname, bool isWebGL2) {
  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:parameters" : "webGl:parameters", std::to_string(pname));
  if (!value) return std::nullopt;

  const auto& el = *value;

  if (el.is_null()) return std::nullptr_t();

  // Try string first (most specific)
  std::string_view sv;
  if (!el.get(sv)) return std::string(sv);

  // Try double (includes integers in some parsers)
  double d = 0.0;
  if (!el.get(d)) return d;

  // Try integer
  int64_t i = 0;
  if (!el.get(i)) return i;

  // Try bool last (least specific, as 0/1 can be bool or int)
  bool b = false;
  if (!el.get(b)) return b;

  return std::nullopt;
}

template <typename T>
inline T MParamGL(uint32_t pname, T defaultValue, bool isWebGL2) {
  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:parameters" : "webGl:parameters", std::to_string(pname));
  if (!value) return defaultValue;

  if constexpr (std::is_same_v<T, bool>) {
    bool b{};
    if (value->get(b)) return defaultValue;
    return b;
  } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
    int64_t i{};
    if (value->get(i)) return defaultValue;
    if (i < std::numeric_limits<T>::min() || i > std::numeric_limits<T>::max())
      return defaultValue;
    return static_cast<T>(i);
  } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
    uint64_t u{};
    if (value->get(u)) return defaultValue;
    if (u > std::numeric_limits<T>::max()) return defaultValue;
    return static_cast<T>(u);
  } else if constexpr (std::is_floating_point_v<T>) {
    double d{};
    if (value->get(d)) return defaultValue;
    return static_cast<T>(d);
  } else if constexpr (std::is_same_v<T, std::string>) {
    std::string_view sv;
    if (value->get(sv)) return defaultValue;
    return std::string(sv);
  } else {
    return defaultValue;
  }
}

template <typename T>
inline std::vector<T> MParamGLVector(uint32_t pname, 
                                     const std::vector<T>& defaultValue,
                                     bool isWebGL2) {
  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:parameters" : "webGl:parameters", std::to_string(pname));
  if (!value) return defaultValue;

  simdjson::dom::array arr;
  if (value->get(arr)) return defaultValue;

  std::vector<T> out;
  for (auto v : arr) {
    if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
      int64_t i{};
      if (v.get(i)) return defaultValue;
      if (i < std::numeric_limits<T>::min() || i > std::numeric_limits<T>::max())
        return defaultValue;
      out.push_back(static_cast<T>(i));
    } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
      uint64_t u{};
      if (v.get(u)) return defaultValue;
      if (u > std::numeric_limits<T>::max())
        return defaultValue;
      out.push_back(static_cast<T>(u));
    } else if constexpr (std::is_floating_point_v<T>) {
      double d{};
      if (v.get(d)) return defaultValue;
      out.push_back(static_cast<T>(d));
    } else if constexpr (std::is_same_v<T, bool>) {
      bool b{};
      if (v.get(b)) return defaultValue;
      out.push_back(b);
    } else if constexpr (std::is_same_v<T, std::string>) {
      std::string_view sv;
      if (v.get(sv)) return defaultValue;
      out.emplace_back(sv);
    } else {
      return defaultValue;
    }
  }
  return out;
}

inline std::optional<std::array<int32_t, 3UL>> MShaderData(
    uint32_t shaderType, uint32_t precisionType, bool isWebGL2) {
  std::string valueName =
      std::to_string(shaderType) + "," + std::to_string(precisionType);

  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:shaderPrecisionFormats" : "webGl:shaderPrecisionFormats",
      valueName);
  if (!value) return std::nullopt;

  simdjson::dom::object obj;
  if (value->get(obj)) return std::nullopt;

  simdjson::dom::element eMin, eMax, ePrec;
  if (obj.at_key("rangeMin").get(eMin)) return std::nullopt;
  if (obj.at_key("rangeMax").get(eMax)) return std::nullopt;
  if (obj.at_key("precision").get(ePrec)) return std::nullopt;

  int64_t rmin{}, rmax{}, prec{};
  if (eMin.get(rmin) || eMax.get(rmax) || ePrec.get(prec)) return std::nullopt;

  // Clamp to int32_t range (or reject)
  if (rmin < std::numeric_limits<int32_t>::min() || rmin > std::numeric_limits<int32_t>::max()) return std::nullopt;
  if (rmax < std::numeric_limits<int32_t>::min() || rmax > std::numeric_limits<int32_t>::max()) return std::nullopt;
  if (prec < std::numeric_limits<int32_t>::min() || prec > std::numeric_limits<int32_t>::max()) return std::nullopt;

  return std::array<int32_t, 3UL>{
      static_cast<int32_t>(rmin), static_cast<int32_t>(rmax),
      static_cast<int32_t>(prec)};
}

inline std::optional<
    std::vector<std::tuple<std::string, std::string, std::string, bool, bool>>>
MVoices() {
  const auto& root = GetJson();

  simdjson::dom::object rootObj;
  if (root.get(rootObj)) return std::nullopt;

  simdjson::dom::element voicesEl;
  if (rootObj.at_key("voices").get(voicesEl)) return std::nullopt;

  simdjson::dom::array voicesArr;
  if (voicesEl.get(voicesArr)) return std::nullopt;

  std::vector<std::tuple<std::string, std::string, std::string, bool, bool>> voices;
  voices.reserve(voicesArr.size());

  for (auto voiceEl : voicesArr) {
    simdjson::dom::object vObj;
    if (voiceEl.get(vObj)) continue;

    simdjson::dom::element langEl, nameEl, uriEl, defEl, localEl;
    if (vObj.at_key("lang").get(langEl)) continue;
    if (vObj.at_key("name").get(nameEl)) continue;
    if (vObj.at_key("voiceUri").get(uriEl)) continue;
    if (vObj.at_key("isDefault").get(defEl)) continue;
    if (vObj.at_key("isLocalService").get(localEl)) continue;

    std::string_view lang, name, uri;
    bool isDefault = false, isLocal = false;

    if (langEl.get(lang) || nameEl.get(name) || uriEl.get(uri)) continue;
    if (defEl.get(isDefault) || localEl.get(isLocal)) continue;

    voices.emplace_back(std::string(lang), std::string(name), std::string(uri),
                        isDefault, isLocal);
  }

  return voices;
}

}  // namespace MaskConfig
