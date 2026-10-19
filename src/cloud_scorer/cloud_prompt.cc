#include "cloud_scorer/cloud_prompt.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mozc {
namespace cloud_scorer {
namespace {

constexpr char32_t kZenzReadingBegin = 0xEE00;
constexpr char32_t kZenzOutputBegin = 0xEE01;
constexpr char32_t kZenzContextBegin = 0xEE02;
constexpr char32_t kZenzProfileBegin = 0xEE03;
constexpr char32_t kZenzTopicBegin = 0xEE04;
constexpr char32_t kZenzStyleBegin = 0xEE05;
constexpr char32_t kZenzSettingsBegin = 0xEE06;
constexpr char32_t kZenzRightContextBegin = 0xEE07;

constexpr size_t kMaxJsonDepth = 32;

bool IsZenzControl(char32_t cp) { return 0xEE00 <= cp && cp <= 0xEE0F; }

// Decodes one UTF-8 code point.  Invalid bytes decode as U+FFFD and advance by
// one byte so callers always make progress.
char32_t DecodeUtf8(std::string_view s, size_t* pos) {
  const unsigned char c = static_cast<unsigned char>(s[*pos]);
  size_t len = 0;
  char32_t cp = 0;
  if (c < 0x80) {
    ++*pos;
    return c;
  } else if ((c & 0xE0) == 0xC0) {
    len = 2;
    cp = c & 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    len = 3;
    cp = c & 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    len = 4;
    cp = c & 0x07;
  } else {
    ++*pos;
    return 0xFFFD;
  }
  if (*pos + len > s.size()) {
    ++*pos;
    return 0xFFFD;
  }
  for (size_t i = 1; i < len; ++i) {
    const unsigned char cc = static_cast<unsigned char>(s[*pos + i]);
    if ((cc & 0xC0) != 0x80) {
      ++*pos;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cc & 0x3F);
  }
  *pos += len;
  return cp;
}

void AppendUtf8(char32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Minimal JSON reader.  Only what is needed to walk a chat completion response.
struct JsonValue {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  Type type = Type::kNull;
  std::string string_value;
  std::vector<std::unique_ptr<JsonValue>> array;
  std::map<std::string, std::unique_ptr<JsonValue>> object;

  const JsonValue* Get(const std::string& key) const {
    if (type != Type::kObject) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : it->second.get();
  }
  const JsonValue* At(size_t index) const {
    if (type != Type::kArray || index >= array.size()) return nullptr;
    return array[index].get();
  }
};

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : s_(input) {}

  std::unique_ptr<JsonValue> Parse() {
    auto value = ParseValue(0);
    SkipWhitespace();
    if (value == nullptr || pos_ != s_.size()) return nullptr;
    return value;
  }

 private:
  void SkipWhitespace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' ||
                                s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool Consume(std::string_view literal) {
    if (s_.substr(pos_, literal.size()) != literal) return false;
    pos_ += literal.size();
    return true;
  }

  std::unique_ptr<JsonValue> ParseValue(size_t depth) {
    if (depth > kMaxJsonDepth) return nullptr;
    SkipWhitespace();
    if (pos_ >= s_.size()) return nullptr;
    auto value = std::make_unique<JsonValue>();
    const char c = s_[pos_];
    if (c == '{') {
      ++pos_;
      value->type = JsonValue::Type::kObject;
      SkipWhitespace();
      if (pos_ < s_.size() && s_[pos_] == '}') {
        ++pos_;
        return value;
      }
      while (true) {
        SkipWhitespace();
        std::string key;
        if (!ParseString(&key)) return nullptr;
        SkipWhitespace();
        if (!Consume(":")) return nullptr;
        auto member = ParseValue(depth + 1);
        if (member == nullptr) return nullptr;
        value->object[key] = std::move(member);
        SkipWhitespace();
        if (Consume(",")) continue;
        if (Consume("}")) return value;
        return nullptr;
      }
    }
    if (c == '[') {
      ++pos_;
      value->type = JsonValue::Type::kArray;
      SkipWhitespace();
      if (pos_ < s_.size() && s_[pos_] == ']') {
        ++pos_;
        return value;
      }
      while (true) {
        auto element = ParseValue(depth + 1);
        if (element == nullptr) return nullptr;
        value->array.push_back(std::move(element));
        SkipWhitespace();
        if (Consume(",")) continue;
        if (Consume("]")) return value;
        return nullptr;
      }
    }
    if (c == '"') {
      value->type = JsonValue::Type::kString;
      if (!ParseString(&value->string_value)) return nullptr;
      return value;
    }
    if (Consume("true") || Consume("false")) {
      value->type = JsonValue::Type::kBool;
      return value;
    }
    if (Consume("null")) {
      return value;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      value->type = JsonValue::Type::kNumber;
      while (pos_ < s_.size()) {
        const char d = s_[pos_];
        if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' ||
            d == 'e' || d == 'E') {
          ++pos_;
        } else {
          break;
        }
      }
      return value;
    }
    return nullptr;
  }

  bool ParseHex4(char32_t* out) {
    if (pos_ + 4 > s_.size()) return false;
    char32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      const char h = s_[pos_++];
      v <<= 4;
      if (h >= '0' && h <= '9') {
        v |= h - '0';
      } else if (h >= 'a' && h <= 'f') {
        v |= h - 'a' + 10;
      } else if (h >= 'A' && h <= 'F') {
        v |= h - 'A' + 10;
      } else {
        return false;
      }
    }
    *out = v;
    return true;
  }

  bool ParseString(std::string* out) {
    if (!Consume("\"")) return false;
    out->clear();
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out->push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) return false;
      const char e = s_[pos_++];
      switch (e) {
        case '"': out->push_back('"'); break;
        case '\\': out->push_back('\\'); break;
        case '/': out->push_back('/'); break;
        case 'b': out->push_back('\b'); break;
        case 'f': out->push_back('\f'); break;
        case 'n': out->push_back('\n'); break;
        case 'r': out->push_back('\r'); break;
        case 't': out->push_back('\t'); break;
        case 'u': {
          char32_t cp = 0;
          if (!ParseHex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            char32_t low = 0;
            if (!Consume("\\u") || !ParseHex4(&low) || low < 0xDC00 ||
                low > 0xDFFF) {
              return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false;
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  std::string_view s_;
  size_t pos_ = 0;
};

constexpr char kSystemPrompt[] =
    "あなたは日本語入力システムのかな漢字変換エンジンです。"
    "ユーザーが入力中の「読み」(カタカナ)を、前後の文脈に最も自然につながる"
    "日本語の表記に変換してください。\n"
    "規則:\n"
    "- 出力は変換後の文字列だけにする。説明・引用符・前置き・改行を付けない。\n"
    "- 読みの音を変えない。語の追加・省略・言い換え・要約をしない。\n"
    "- 直前・直後の文脈そのものは出力に含めない。\n"
    "- 文脈に合うなら、ひらがな・カタカナ・英数字・記号もそのまま使う。";

void AppendSection(const char* label, const std::string& text,
                   std::string* out) {
  if (text.empty()) return;
  out->append("[");
  out->append(label);
  out->append("]\n");
  out->append(text);
  out->append("\n");
}

}  // namespace

bool ParseZenzPrompt(std::string_view prompt, ZenzPromptFields* fields) {
  *fields = ZenzPromptFields();
  std::string* target = nullptr;
  bool saw_reading = false;
  size_t pos = 0;
  while (pos < prompt.size()) {
    const char32_t cp = DecodeUtf8(prompt, &pos);
    if (!IsZenzControl(cp)) {
      if (target != nullptr) AppendUtf8(cp, target);
      continue;
    }
    switch (cp) {
      case kZenzReadingBegin:
        target = &fields->reading_katakana;
        saw_reading = true;
        break;
      case kZenzOutputBegin:
        return saw_reading && !fields->reading_katakana.empty();
      case kZenzContextBegin: target = &fields->left_context; break;
      case kZenzProfileBegin: target = &fields->profile; break;
      case kZenzTopicBegin: target = &fields->topic; break;
      case kZenzStyleBegin: target = &fields->style; break;
      case kZenzSettingsBegin: target = &fields->settings; break;
      case kZenzRightContextBegin: target = &fields->right_context; break;
      default: target = nullptr; break;
    }
  }
  return saw_reading && !fields->reading_katakana.empty();
}

std::string JsonEscape(std::string_view input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char ch : input) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (c < 0x20) {
          static constexpr char kHex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::string BuildChatCompletionsBody(const ZenzPromptFields& fields,
                                     const ChatRequestOptions& options) {
  std::string user;
  AppendSection("ユーザー情報", fields.profile, &user);
  AppendSection("話題", fields.topic, &user);
  AppendSection("文体", fields.style, &user);
  AppendSection("補足", fields.settings, &user);
  AppendSection("直前の文脈", fields.left_context, &user);
  AppendSection("直後の文脈", fields.right_context, &user);
  AppendSection("読み", fields.reading_katakana, &user);

  std::string body = "{\"model\":\"";
  body += JsonEscape(options.model);
  body += "\",\"max_tokens\":";
  body += std::to_string(options.max_tokens);
  body += ",\"stream\":false,\"messages\":[{\"role\":\"system\",\"content\":\"";
  body += JsonEscape(kSystemPrompt);
  body += "\"},{\"role\":\"user\",\"content\":\"";
  body += JsonEscape(user);
  body += "\"}]";
  if (!options.extra_body_members.empty()) {
    body += ",";
    body += options.extra_body_members;
  }
  body += "}";
  return body;
}

bool ExtractChatCompletionText(std::string_view json, std::string* text,
                               std::string* error) {
  text->clear();
  error->clear();
  JsonParser parser(json);
  const std::unique_ptr<JsonValue> root = parser.Parse();
  if (root == nullptr) {
    *error = "invalid_json";
    return false;
  }
  if (root->Get("error") != nullptr) {
    *error = "api_error";
    return false;
  }
  const JsonValue* choices = root->Get("choices");
  const JsonValue* first = choices == nullptr ? nullptr : choices->At(0);
  const JsonValue* message = first == nullptr ? nullptr : first->Get("message");
  const JsonValue* content =
      message == nullptr ? nullptr : message->Get("content");
  if (content == nullptr || content->type != JsonValue::Type::kString) {
    *error = "content_not_found";
    return false;
  }
  if (content->string_value.empty()) {
    *error = "empty_content";
    return false;
  }
  *text = content->string_value;
  return true;
}

std::string CleanModelOutput(std::string_view text, uint32_t max_chars) {
  // Decode, dropping zenz control code points and C0 controls other than
  // newlines, which terminate the result.
  std::u32string cps;
  size_t pos = 0;
  while (pos < text.size()) {
    const char32_t cp = DecodeUtf8(text, &pos);
    if (cp == '\n' || cp == '\r') {
      if (!cps.empty()) break;
      continue;
    }
    if (IsZenzControl(cp) || cp < 0x20 || cp == 0x7F) continue;
    cps.push_back(cp);
  }

  auto is_space = [](char32_t cp) {
    return cp == ' ' || cp == 0x3000 || cp == '\t';
  };
  while (!cps.empty() && is_space(cps.front())) cps.erase(cps.begin());
  while (!cps.empty() && is_space(cps.back())) cps.pop_back();

  // Strip one layer of wrapping ASCII quotes the model may add.  Japanese
  // brackets are kept because the user may have typed them; the session-side
  // validator rejects outputs whose symbols differ from the reading.
  static constexpr char32_t kPairs[][2] = {
      {'"', '"'}, {'\'', '\''}, {'`', '`'}, {0x201C, 0x201D},  // “”
  };
  if (cps.size() >= 2) {
    for (const auto& pair : kPairs) {
      if (cps.front() == pair[0] && cps.back() == pair[1]) {
        cps = cps.substr(1, cps.size() - 2);
        break;
      }
    }
  }

  if (max_chars > 0 && cps.size() > max_chars) cps.resize(max_chars);

  std::string out;
  for (const char32_t cp : cps) AppendUtf8(cp, &out);
  return out;
}

}  // namespace cloud_scorer
}  // namespace mozc
