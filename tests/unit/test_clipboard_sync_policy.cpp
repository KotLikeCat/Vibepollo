/**
 * @file tests/unit/test_clipboard_sync_policy.cpp
 * @brief Test src/clipboard/sync_policy.*
 */
#include "../tests_common.h"
#include "src/clipboard/sync_policy.h"

using clipboard::client_info;
using clipboard::item_type;
using strings = std::vector<std::string>;

TEST(ClipboardSyncPolicy, NotifiesAllReadersForForeignChanges) {
  clipboard::sync_policy policy;
  EXPECT_EQ(policy.recipients(10, {{"a", true}, {"b", true}, {"c", false}}), (strings {"a", "b"}));
}

TEST(ClipboardSyncPolicy, SkipsOriginOfHostWrite) {
  clipboard::sync_policy policy;
  policy.note_host_write(11, "a");
  EXPECT_EQ(policy.recipients(11, {{"a", true}, {"b", true}}), (strings {"b"}));
  EXPECT_EQ(policy.recipients(12, {{"a", true}, {"b", true}}), (strings {"a", "b"}));
}

TEST(ClipboardSyncPolicy, LatestHostWriteWins) {
  clipboard::sync_policy policy;
  policy.note_host_write(11, "a");
  policy.note_host_write(12, "b");
  EXPECT_EQ(policy.recipients(11, {{"a", true}, {"b", true}}), (strings {"a", "b"}));
  EXPECT_EQ(policy.recipients(12, {{"a", true}, {"b", true}}), (strings {"a"}));
}

TEST(ClipboardGreeting, ReportsOnlyNewSessions) {
  clipboard::greeting_tracker tracker;
  EXPECT_EQ(tracker.new_sessions({"a"}), (strings {"a"}));
  EXPECT_EQ(tracker.new_sessions({"a", "b"}), (strings {"b"}));
  EXPECT_TRUE(tracker.new_sessions({"a", "b"}).empty());
  EXPECT_TRUE(tracker.new_sessions({}).empty());
  EXPECT_EQ(tracker.new_sessions({"a"}), (strings {"a"}));
}

TEST(ClipboardFitToLimit, KeepsEverythingUnderLimit) {
  std::vector<clipboard::item> items {{item_type::text, "hello"}, {item_type::png, "png"}};
  EXPECT_TRUE(clipboard::fit_to_limit(items, 1024));
  EXPECT_EQ(items.size(), 2u);
}

TEST(ClipboardFitToLimit, DropsImageFirst) {
  std::vector<clipboard::item> items {{item_type::text, "hello"}, {item_type::png, std::string(1000, 'p')}};
  EXPECT_TRUE(clipboard::fit_to_limit(items, 100));
  ASSERT_EQ(items.size(), 1u);
  EXPECT_EQ(items[0].type, item_type::text);
}

TEST(ClipboardFitToLimit, FailsWhenTextAloneIsTooLarge) {
  std::vector<clipboard::item> items {{item_type::text, std::string(1000, 't')}};
  EXPECT_FALSE(clipboard::fit_to_limit(items, 100));
}
