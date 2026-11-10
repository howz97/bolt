/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/Likely.h>
#include <sonic/sonic.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "bolt/core/QueryConfig.h"
#include "bolt/expression/StringWriter.h"
#include "bolt/functions/Macros.h"
#include "bolt/functions/lib/JsonUtil.h"
#include "bolt/functions/prestosql/json/SIMDJsonUtil.h"
#include "bolt/type/Conversions.h"

namespace bytedance::bolt::functions::sparksql {

namespace detail {
inline bool isWhitespace(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
};

// Normalizes the JSON path to be Spark-compatible.
//
// Rules applied:
// 1. Removes single quotes in bracket notation (e.g., "$['a']" -> "$[a]").
// 2. Removes spaces after dots (e.g., "$. a" -> "$.a").
// 3. Removes trailing spaces after root symbol (e.g., "$ " -> "$").
// 4. Invalid cases return "-1":
//    - Empty path or path not starting with '$'.
//    - Space between ($ or ]) and dot (e.g., "$ .a").
//    - Space between [ and field name (e.g., "$[' a']").
//    - Consecutive dots (e.g., "$..a").
//    - Dot at the end (e.g., "$.a. ").
class JsonPathNormalizer {
 public:
  // The state transitions: kAfterDollar --> kAfterDot <--> kToken
  //                            |               /|\            |
  //                            |                |             |
  //                            |----> kInSquareBrackets <-----|
  enum class State { kAfterDollar, kAfterDot, kToken, kInSquareBrackets };

  std::string normalize(StringView jsonPath) {
    // First, remove single quotes for bracket notation
    std::string path = removeSingleQuotes(jsonPath);
    if (path.empty() || path[0] != '$') {
      return "-1";
    }
    std::string normalized;
    normalized.reserve(path.size() - 1);

    // Initialize state.
    state_ = State::kAfterDollar;
    for (size_t i = 1; i < path.size(); ++i) {
      const char c = path[i];
      if (isWhitespace(c)) {
        if (state_ == State::kToken || state_ == State::kInSquareBrackets) {
          // Spaces within tokens and quare brackets are preserved.
          normalized.push_back(c);
        }
        continue;
      }
      switch (state_) {
        case State::kAfterDollar: {
          if (!tryConvertToAfterDot(c, path[i - 1])) {
            return "-1";
          }
          // i + 1 is safe because removeSingleQuotes ensures at least one
          // char (']') after.
          if (!tryConvertToInSquareBrackets(c, path[i + 1])) {
            return "-1";
          }
          break;
        }
        case State::kAfterDot: {
          if (c == '.') {
            // Consecutive dots are invalid.
            return "-1";
          }
          state_ = State::kToken;
          break;
        }
        case State::kToken: {
          if (c == '.') {
            state_ = State::kAfterDot;
          } else if (!tryConvertToInSquareBrackets(c, path[i + 1])) {
            return "-1";
          }
          break;
        }
        case State::kInSquareBrackets: {
          if (!tryConvertToAfterDot(c, path[i - 1])) {
            return "-1";
          }
          break;
        }
      }
      normalized.push_back(c);
    }

    if (state_ == State::kAfterDot) {
      // Trailing dot is invalid.
      return "-1";
    }

    return normalized;
  }

 private:
  // Spark's json path requires field name surrounded by single quotes if it is
  // specified in "[]". But simdjson lib requires not. This method just removes
  // such single quotes to adapt to simdjson lib, e.g., converts "['a']['b']" to
  // "[a][b]".
  std::string removeSingleQuotes(StringView jsonPath) {
    std::string result(jsonPath.data(), jsonPath.size());
    size_t pairEnd = 0;
    while (true) {
      auto pairBegin = result.find("['", pairEnd);
      if (pairBegin == std::string::npos) {
        break;
      }
      pairEnd = result.find(']', pairBegin);
      // If expected pattern, like ['a'], is not found.
      if (pairEnd == std::string::npos || result[pairEnd - 1] != '\'') {
        return "-1";
      }
      result.erase(pairEnd - 1, 1);
      result.erase(pairBegin + 1, 1);
      pairEnd -= 2;
    }
    return result;
  }

  // Try to convert state to kAfterDot if currentChar is '.'.
  bool tryConvertToAfterDot(char currentChar, char previousChar) {
    if (currentChar == '.') {
      if (isWhitespace(previousChar) && state_ != State::kToken) {
        // Spaces before '.' are invalid.
        return false;
      }
      state_ = State::kAfterDot;
    }
    return true;
  }

  // Try to convert state to kInSquareBrackets if currentChar is '['.
  bool tryConvertToInSquareBrackets(char currentChar, char nextChar) {
    if (currentChar == '[') {
      if (isWhitespace(nextChar)) {
        // Spaces between '[' and field are invalid.
        return false;
      }
      state_ = State::kInSquareBrackets;
    }
    return true;
  }

  State state_{State::kAfterDollar};
};

// Appends the raw JSON object/array slice `raw` to `out`, rewriting characters
// in the Unicode supplementary planes (code points >= U+10000, encoded as
// 4-byte UTF-8) into '\\uXXXX\\uXXXX' UTF-16 surrogate-pair escapes. All other
// bytes (ASCII, 2-/3-byte BMP sequences, existing escapes) are appended
// verbatim, in bulk runs. When `raw` has no supplementary-plane character it is
// appended directly with no extra allocation.
//
// Spark's get_json_object re-serializes an object/array result through a
// Jackson JsonGenerator, which escapes supplementary-plane characters as
// surrogate-pair '\\u' escapes while leaving BMP characters (e.g. CJK) literal.
// The raw simdjson slice keeps them literal, so this aligns Velox with Spark.
// See https://github.com/facebookincubator/velox/issues/17923.
void appendWithSupplementaryEscapes(
    std::string_view raw,
    exec::StringWriter<false>& out);

// Normalizes the JSON path to be Spark-compatible.
//
// Rules applied:
//  Removes spaces after dots outside bracket keys (e.g., "$. a" -> "$.a").
//  Removes trailing spaces after root symbol (e.g., "$ " -> "$").
inline std::string normalizeJsonPath(StringView jsonPath) {
  std::string path(jsonPath.data(), jsonPath.size());
  if (path.empty() || path[0] != '$') {
    return path;
  }

  if (std::all_of(path.begin() + 1, path.end(), isWhitespace)) {
    return "$";
  }

  std::string normalized{"$"};
  normalized.reserve(path.size());
  bool afterDot = false;
  bool inBrackets = false;
  bool escaped = false;
  char quote = '\0';

  for (size_t i = 1; i < path.size(); ++i) {
    const char c = path[i];

    if (inBrackets) {
      normalized.push_back(c);
      if (quote != '\0') {
        if (escaped) {
          escaped = false;
        } else if (c == '\\') {
          escaped = true;
        } else if (c == quote) {
          quote = '\0';
        }
      } else if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == ']') {
        inBrackets = false;
      }
      continue;
    }

    if (afterDot) {
      if (isWhitespace(c)) {
        // Removes whitespace after dot
        continue;
      }
      afterDot = false;
    }

    if (c == '[') {
      inBrackets = true;
    } else if (c == '.') {
      afterDot = true;
    }
    normalized.push_back(c);
  }

  return normalized;
}

} // namespace detail

/// Parses a JSON string and returns the value at the specified path.
/// Simdjson On-Demand API is used to parse JSON string.
/// get_json_object(jsonString, path) -> value
template <typename T>
struct GetJsonObjectFunction {
  BOLT_DEFINE_FUNCTION_TYPES(T);

  // ASCII input always produces ASCII result.
  static constexpr bool is_default_ascii_behavior = true;

  FOLLY_ALWAYS_INLINE void initialize(
      const std::vector<TypePtr>& /*inputTypes*/,
      const core::QueryConfig& config,
      const arg_type<Varchar>* /*json*/,
      const arg_type<Varchar>* jsonPath) {
    throwExceptionWhenEncounterBadJson_ =
        config.throwExceptionWhenEncounterBadJson();
    useDOMParserInGetJsonObject_ = config.useDOMParserInGetJsonObject();
    getJsonObjectEscapeEmoji_ = config.getJsonObjectEscapeEmoji();
    if (!throwExceptionWhenEncounterBadJson_ && !useDOMParserInGetJsonObject_ &&
        getJsonObjectEscapeEmoji_ && config.useSonicJson()) {
      useSonic_ = true;
    }
    if (jsonPath != nullptr && checkJsonPath(*jsonPath)) {
      jsonPath_ = pathNormalizer_.normalize(*jsonPath);
    }
  }

  FOLLY_ALWAYS_INLINE bool call(
      out_type<Varchar>& result,
      const arg_type<Varchar>& json,
      const arg_type<Varchar>& jsonPath) {
    // Spark requires the first char in jsonPath is '$'.
    if (!checkJsonPath(jsonPath)) {
      return false;
    }
    // Check if json has invalid escape sequence.
    if (hasInvalidEscapedChar(json.data(), json.size())) {
      return false;
    }

    const auto formattedSimdPath = jsonPath_.has_value()
        ? jsonPath_.value()
        : pathNormalizer_.normalize(jsonPath);
    const bool hasWildcard = formattedSimdPath.find("[*]") != std::string::npos;

    if (formattedSimdPath.empty() || (!hasWildcard && useSonic_)) {
      const std::string formattedSonic =
          detail::normalizeJsonPath(StringView(jsonPath));
      auto ret = sonic_json::GetByJsonPathOnDemand(
          std::string_view(json), formattedSonic);
      // Treat no-match and parse errors as no result. Sonic may return a
      // partial result together with a parse error, but Spark's get_json_object
      // returns NULL for malformed JSON.
      if (std::get<1>(ret) != sonic_json::kErrorNone) {
        return false;
      }
      result = std::get<0>(ret);
      return true;
    }

    simdjson::dom::parser domParser;
    auto domJson = domParser.parse(json);
    if (hasWildcard) {
      std::vector<simdjson::dom::element> matches;
      auto err = domJson.at_path_with_wildcard(formattedSimdPath).get(matches);
      if (err) {
        if (throwExceptionWhenEncounterBadJson_) {
          std::stringstream ss;
          ss << "Parse json error, detail=" << err;
          BOLT_USER_FAIL(ss.str());
        }
        return false;
      }
      if (matches.empty()) {
        return false;
      }
      const bool wrapInArray = matches.size() > 1;
      if (wrapInArray) {
        result.append("[");
      }
      for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) {
          result.append(",");
        }
        const auto serialized = simdjson::to_string(matches[i]);
        detail::appendWithSupplementaryEscapes(serialized, result);
      }
      if (wrapInArray) {
        result.append("]");
      }
      return true;
    }

    simdjson::dom::element jsonDoc;
    auto err = domJson.get(jsonDoc);
    if (err) {
      if (throwExceptionWhenEncounterBadJson_) {
        std::stringstream ss;
        ss << "Parse json error, detail=" << err;
        BOLT_USER_FAIL(ss.str());
      }
      return false;
    }

    simdjson::dom::element value;
    if (jsonDoc.at_path(formattedSimdPath).get(value)) {
      return false;
    }
    return extractDomResult(value, result);
  }

 private:
  FOLLY_ALWAYS_INLINE bool checkJsonPath(StringView jsonPath) {
    // Spark requires the first char in jsonPath is '$'.
    return std::string_view{jsonPath}.starts_with('$');
  }

  bool extractDomResult(
      const simdjson::dom::element& value,
      out_type<Varchar>& result) {
    switch (value.type()) {
      case simdjson::dom::element_type::STRING: {
        std::string_view stringValue;
        if (value.get_string().get(stringValue)) {
          return false;
        }
        result.append(stringValue);
        return true;
      }
      case simdjson::dom::element_type::INT64: {
        int64_t number;
        if (value.get_int64().get(number)) {
          return false;
        }
        result.append(std::to_string(number));
        return true;
      }
      case simdjson::dom::element_type::UINT64: {
        uint64_t number;
        if (value.get_uint64().get(number)) {
          return false;
        }
        result.append(std::to_string(number));
        return true;
      }
      case simdjson::dom::element_type::DOUBLE: {
        double number;
        if (value.get_double().get(number)) {
          return false;
        }
        result.append(
            util::Converter<TypeKind::VARCHAR>::cast(number, nullptr));
        return true;
      }
      case simdjson::dom::element_type::BOOL: {
        bool boolean;
        if (value.get_bool().get(boolean)) {
          return false;
        }
        result.append(boolean ? "true" : "false");
        return true;
      }
      case simdjson::dom::element_type::ARRAY:
      case simdjson::dom::element_type::OBJECT: {
        const auto serialized = simdjson::to_string(value);
        detail::appendWithSupplementaryEscapes(serialized, result);
        return true;
      }
      case simdjson::dom::element_type::NULL_VALUE:
        return false;
    }
    return false;
  }

  // Checks for invalid escape sequences in JSON string.
  // Note: We only search for '\' which is ASCII (0x5C) and cannot appear
  // as a continuation byte in valid UTF-8 (continuation bytes are 0x80-0xBF).
  // So we can safely scan byte-by-byte regardless of encoding.
  // See the valid escape sequences in Jackson's JSON parser:
  // https://github.com/FasterXML/jackson-core/blob/jackson-core-2.19.2/src/main/java/com/fasterxml/jackson/core/json/ReaderBasedJsonParser.java#L2648
  FOLLY_ALWAYS_INLINE bool hasInvalidEscapedChar(
      const char* json,
      size_t size) {
    const char* end = json + size;
    const char* pos = json;

    while ((pos = static_cast<const char*>(std::memchr(
                pos, '\\', static_cast<size_t>(end - pos)))) != nullptr) {
      const auto remaining = static_cast<size_t>(end - pos);
      if (FOLLY_UNLIKELY(remaining < 2)) {
        return false; // Incomplete escape at end, let parser handle it.
      }

      switch (pos[1]) {
        case '"':
          [[fallthrough]];
        case '\\':
          [[fallthrough]];
        case '/':
          [[fallthrough]];
        case 'b':
          [[fallthrough]];
        case 'f':
          [[fallthrough]];
        case 'n':
          [[fallthrough]];
        case 'r':
          [[fallthrough]];
        case 't':
          pos += 2;
          break;
        case 'u':
          // Validate \uXXXX.
          if (FOLLY_UNLIKELY(remaining < 6) || !isHexDigit(pos[2]) ||
              !isHexDigit(pos[3]) || !isHexDigit(pos[4]) ||
              !isHexDigit(pos[5])) {
            return true;
          }
          pos += 6;
          break;
        default:
          return true; // Invalid escape character.
      }
    }
    return false;
  }

  static FOLLY_ALWAYS_INLINE bool isHexDigit(char c) {
    auto uc = static_cast<unsigned char>(c);
    return (uc - '0' < 10U) || ((uc | 0x20) - 'a' < 6U);
  }

  bool throwExceptionWhenEncounterBadJson_;
  bool useDOMParserInGetJsonObject_;
  bool getJsonObjectEscapeEmoji_;
  bool useSonic_;

  // Used for constant json path.
  std::optional<std::string> jsonPath_;
  detail::JsonPathNormalizer pathNormalizer_;
};

} // namespace bytedance::bolt::functions::sparksql
