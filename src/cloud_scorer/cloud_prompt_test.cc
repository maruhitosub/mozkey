#include "cloud_scorer/cloud_prompt.h"

#include <string>

#include "testing/gunit.h"

namespace mozc {
namespace cloud_scorer {
namespace {

constexpr char kReadingBegin[] = "\xEE\xB8\x80";       // U+EE00
constexpr char kOutputBegin[] = "\xEE\xB8\x81";        // U+EE01
constexpr char kContextBegin[] = "\xEE\xB8\x82";       // U+EE02
constexpr char kStyleBegin[] = "\xEE\xB8\x85";         // U+EE05
constexpr char kRightContextBegin[] = "\xEE\xB8\x87";  // U+EE07

TEST(CloudPromptTest, ParsesContextAndReading) {
  const std::string prompt = std::string(kContextBegin) + "今日は" +
                             kReadingBegin + "イイテンキ" + kOutputBegin;
  ZenzPromptFields fields;
  ASSERT_TRUE(ParseZenzPrompt(prompt, &fields));
  EXPECT_EQ(fields.left_context, "今日は");
  EXPECT_EQ(fields.reading_katakana, "イイテンキ");
  EXPECT_TRUE(fields.right_context.empty());
}

TEST(CloudPromptTest, ParsesConditionFields) {
  const std::string prompt = std::string(kStyleBegin) + "敬体" +
                             kRightContextBegin + "です" + kContextBegin +
                             kReadingBegin + "キシャ" + kOutputBegin;
  ZenzPromptFields fields;
  ASSERT_TRUE(ParseZenzPrompt(prompt, &fields));
  EXPECT_EQ(fields.style, "敬体");
  EXPECT_EQ(fields.right_context, "です");
  EXPECT_TRUE(fields.left_context.empty());
  EXPECT_EQ(fields.reading_katakana, "キシャ");
}

TEST(CloudPromptTest, RejectsPromptWithoutReading) {
  ZenzPromptFields fields;
  EXPECT_FALSE(ParseZenzPrompt(std::string(kContextBegin) + "abc", &fields));
  EXPECT_FALSE(ParseZenzPrompt(
      std::string(kReadingBegin) + kOutputBegin, &fields));
}

TEST(CloudPromptTest, BuildsEscapedBody) {
  ZenzPromptFields fields;
  fields.left_context = "say \"hi\"\n";
  fields.reading_katakana = "キシャ";
  ChatRequestOptions options;
  options.model = "glm-5.3-flash";
  options.max_tokens = 64;
  options.extra_body_members = "\"temperature\":0";
  const std::string body = BuildChatCompletionsBody(fields, options);
  EXPECT_NE(body.find("\"model\":\"glm-5.3-flash\""), std::string::npos);
  EXPECT_NE(body.find("\"max_tokens\":64"), std::string::npos);
  EXPECT_NE(body.find("say \\\"hi\\\"\\n"), std::string::npos);
  EXPECT_NE(body.find("キシャ"), std::string::npos);
  EXPECT_EQ(body.substr(body.size() - 17), ",\"temperature\":0}");
}

TEST(CloudPromptTest, ExtractsContent) {
  const std::string json =
      R"({"id":"x","choices":[{"index":0,"message":{"role":"assistant",)"
      R"("content":"記者😀"},"finish_reason":"stop"}],)"
      R"("usage":{"prompt_tokens":10,"completion_tokens":2}})";
  std::string text;
  std::string error;
  ASSERT_TRUE(ExtractChatCompletionText(json, &text, &error)) << error;
  EXPECT_EQ(text, "記者\xF0\x9F\x98\x80");
}

TEST(CloudPromptTest, ReportsErrors) {
  std::string text;
  std::string error;
  EXPECT_FALSE(ExtractChatCompletionText("{", &text, &error));
  EXPECT_EQ(error, "invalid_json");
  EXPECT_FALSE(ExtractChatCompletionText(
      R"({"error":{"message":"bad key"}})", &text, &error));
  EXPECT_EQ(error, "api_error");
  EXPECT_FALSE(ExtractChatCompletionText(
      R"({"choices":[{"message":{"content":null}}]})", &text, &error));
  EXPECT_EQ(error, "content_not_found");
  EXPECT_FALSE(ExtractChatCompletionText(
      R"({"choices":[{"message":{"content":""}}]})", &text, &error));
  EXPECT_EQ(error, "empty_content");
}

TEST(CloudPromptTest, CleansOutput) {
  EXPECT_EQ(CleanModelOutput("  \"記者\"  \n説明です", 0), "記者");
  EXPECT_EQ(CleanModelOutput("\n\n汽車", 0), "汽車");
  EXPECT_EQ(CleanModelOutput("「記者」", 0), "「記者」");
  EXPECT_EQ(CleanModelOutput("記者\xEE\xB8\x81", 0), "記者");
  EXPECT_EQ(CleanModelOutput("あいうえお", 3), "あいう");
}

}  // namespace
}  // namespace cloud_scorer
}  // namespace mozc
