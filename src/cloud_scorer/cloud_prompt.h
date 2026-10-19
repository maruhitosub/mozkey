#ifndef MOZC_CLOUD_SCORER_CLOUD_PROMPT_H_
#define MOZC_CLOUD_SCORER_CLOUD_PROMPT_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mozc {
namespace cloud_scorer {

// Fields decoded from a ZNZ1 zenz prompt.  The zenz prompt is a sequence of
// (U+EE0x marker, text) segments terminated by U+EE00 reading U+EE01.
struct ZenzPromptFields {
  std::string left_context;
  std::string right_context;
  std::string profile;
  std::string topic;
  std::string style;
  std::string settings;
  std::string reading_katakana;
};

// Returns false when the prompt has no reading segment.
bool ParseZenzPrompt(std::string_view prompt, ZenzPromptFields* fields);

struct ChatRequestOptions {
  std::string model;
  int max_tokens = 128;
  // Optional raw JSON object members appended to the request body, e.g.
  // "\"reasoning_effort\":\"none\"".  Must not start or end with a comma.
  std::string extra_body_members;
};

// Builds an OpenAI-compatible /chat/completions request body.
std::string BuildChatCompletionsBody(const ZenzPromptFields& fields,
                                     const ChatRequestOptions& options);

// Extracts choices[0].message.content from a /chat/completions response.
// On failure, |error| receives a short ASCII reason.
bool ExtractChatCompletionText(std::string_view json, std::string* text,
                               std::string* error);

// Normalizes model output into a single conversion string: trims whitespace,
// keeps only the first line, strips wrapping quotes/brackets and zenz control
// code points, and truncates to |max_chars| code points.
std::string CleanModelOutput(std::string_view text, uint32_t max_chars);

// Escapes UTF-8 text for a JSON string literal (without surrounding quotes).
std::string JsonEscape(std::string_view input);

}  // namespace cloud_scorer
}  // namespace mozc

#endif  // MOZC_CLOUD_SCORER_CLOUD_PROMPT_H_
