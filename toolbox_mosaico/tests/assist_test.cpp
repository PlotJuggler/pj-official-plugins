// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
//
// Tests for the metadata-filter assist model (query/assist.h), encoding the
// lua.txt contract point-by-point. Pure logic — no Qt, no Lua engine.
//
// Caret offsets reference the sample clause:
//   robot == "bonirob"
//   0     6  9
//   [robot]=[0,5)  [==]=[6,8)  ["bonirob"]=[9,18)

#include "query/assist.h"

#include "gtest/gtest.h"

namespace {

Schema schema() {
  return Schema{
      {"robot", {"bonirob", "drone"}},
      {"sensor", {"camera", "laser"}},
  };
}

constexpr const char* kClause = "robot == \"bonirob\"";

// ---------------------------------------------------------------------------
// lua.txt 1 — on key select, the Key dropdown shows that key as its title and
// becomes disabled as focus moves on to the Op dropdown.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P1_KeyStaged_TitleSet_KeyDisabled_OpActivates) {
  const AssistView v = computeAssist(/*text=*/"", /*caret=*/0, schema(), /*key=*/"robot", /*op=*/"", /*value=*/"");
  EXPECT_EQ(v.key.title, "robot");  // title becomes the picked key
  EXPECT_FALSE(v.key.enabled);      // ...and the Key dropdown disables
  EXPECT_TRUE(v.op.enabled);        // focus moves on to Op
  EXPECT_FALSE(v.key.replaces);     // ADD mode stages; it does not edit the text
}

// ---------------------------------------------------------------------------
// lua.txt 2 — when building at the end, the Op auto-selects "==", and stays
// enabled so the user can override it.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P2_AtEnd_OpAutoEquals_AndOverridable) {
  const AssistView v = computeAssist("", 0, schema(), "robot", /*op=*/"", "");
  EXPECT_EQ(v.op.title, "==");  // auto-selected default operator
  EXPECT_TRUE(v.op.enabled);    // not disabled -> overridable
}

TEST(LuaAssist, P2_OpOverrideKept) {
  const AssistView v = computeAssist("", 0, schema(), "robot", /*op=*/"~=", "");
  EXPECT_EQ(v.op.title, "~=");  // an explicit pick replaces the "==" default
}

// ---------------------------------------------------------------------------
// lua.txt 3 — a selected value only enters the editor on PLUS; PLUS joins with
// the default ADD operator " and " when a filter is already present.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P3_ValueStaged_EnablesPlus) {
  const AssistView v = computeAssist("", 0, schema(), "robot", "==", /*value=*/"bonirob");
  EXPECT_TRUE(v.plus_enabled);
}

TEST(LuaAssist, P3_Commit_FirstClause_NoConnector) {
  EXPECT_EQ(commitClause(/*existing=*/"", "robot", "==", "bonirob"), "robot == \"bonirob\"");
}

TEST(LuaAssist, P3_Commit_WithExisting_JoinsWithAnd) {
  EXPECT_EQ(
      commitClause(/*existing=*/"robot == \"bonirob\"", "sensor", "==", "camera"),
      "robot == \"bonirob\" and sensor == \"camera\"");
}

TEST(LuaAssist, P3_Commit_WhitespaceExisting_TreatedAsEmpty) {
  EXPECT_EQ(commitClause("   ", "robot", "==", "drone"), "robot == \"drone\"");
}

// ---------------------------------------------------------------------------
// lua.txt 4 — caret ON a key activates the Key dropdown for replacement; the
// other two are inert.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P4_CaretOnKey_KeyReplaces) {
  const AssistView v = computeAssist(kClause, /*caret=*/2, schema(), "", "", "");  // inside "robot"
  EXPECT_TRUE(v.key.enabled);
  EXPECT_TRUE(v.key.replaces);
  EXPECT_EQ(v.key.title, "robot");  // synced to the token (lua.txt 8)
  EXPECT_FALSE(v.op.enabled);
  EXPECT_FALSE(v.value.enabled);
}

// ---------------------------------------------------------------------------
// lua.txt 5 — caret ON an operator activates the Op dropdown for replacement.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P5_CaretOnOp_OpReplaces) {
  const AssistView v = computeAssist(kClause, /*caret=*/7, schema(), "", "", "");  // inside "=="
  EXPECT_TRUE(v.op.enabled);
  EXPECT_TRUE(v.op.replaces);
  EXPECT_EQ(v.op.title, "==");
  EXPECT_FALSE(v.key.enabled);
  EXPECT_FALSE(v.value.enabled);
}

// ---------------------------------------------------------------------------
// lua.txt 6 — caret ON a value activates the Value dropdown for replacement.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P6_CaretOnValue_ValueReplaces) {
  const AssistView v = computeAssist(kClause, /*caret=*/12, schema(), "", "", "");  // inside "bonirob"
  EXPECT_TRUE(v.value.enabled);
  EXPECT_TRUE(v.value.replaces);
  EXPECT_FALSE(v.key.enabled);
  EXPECT_FALSE(v.op.enabled);
}

// ---------------------------------------------------------------------------
// lua.txt 7 — replacements happen without PLUS (PLUS stays disabled in REPLACE
// mode), and the edit is in-place (applyCompletion Replace).
// ---------------------------------------------------------------------------
TEST(LuaAssist, P7_ReplaceNeedsNoPlus_AndEditsInPlace) {
  const AssistView v = computeAssist(kClause, 2, schema(), "", "", "");  // on key
  EXPECT_FALSE(v.plus_enabled);

  const CursorContext ctx = analyze(kClause, 2, schema());
  ASSERT_EQ(ctx.key_action, Action::Replace);
  const EditResult r = applyCompletion(kClause, ctx, ctx.key_action, "sensor");
  EXPECT_EQ(r.text, "sensor == \"bonirob\"");  // "robot" replaced in place, no new clause
}

// ---------------------------------------------------------------------------
// lua.txt 8 — the user may type the next token; the dropdowns synchronise to
// the token under the caret (a freely typed key reflects in the Key dropdown).
// ---------------------------------------------------------------------------
TEST(LuaAssist, P8_TypedKey_SyncsToKeyDropdown) {
  const AssistView v = computeAssist(/*text=*/"sensor", /*caret=*/3, schema(), "", "", "");  // inside typed "sensor"
  EXPECT_TRUE(v.key.replaces);
  EXPECT_EQ(v.key.title, "sensor");
}

// ---------------------------------------------------------------------------
// lua.txt 9 — PLUS is the only entry that inserts a NEW clause: in ADD mode the
// dropdowns STAGE (never replace/edit the text); only commitClause writes.
// ---------------------------------------------------------------------------
TEST(LuaAssist, P9_AddModeStages_PlusIsOnlyInsert) {
  const AssistView v = computeAssist("", 0, schema(), "robot", "==", "bonirob");
  EXPECT_FALSE(v.key.replaces);
  EXPECT_FALSE(v.op.replaces);
  EXPECT_FALSE(v.value.replaces);
  // The staged clause reaches the editor only through commitClause (PLUS):
  EXPECT_EQ(commitClause("", "robot", "==", "bonirob"), "robot == \"bonirob\"");
}

// ---------------------------------------------------------------------------
// Chaining (§2/§9 interaction): the caret at a value's TRAILING edge — exactly
// where it lands after PLUS — is an ADD position, not value-replace, so the next
// clause can be staged. Value-replace requires the caret STRICTLY inside the value.
// ---------------------------------------------------------------------------
TEST(LuaAssist, ValueTrailingEdge_IsAdd_NotReplace) {
  const std::string text = kClause;  // "robot == \"bonirob\"", length 18
  const AssistView v = computeAssist(text, static_cast<int>(text.size()), schema(), "", "", "");
  EXPECT_FALSE(v.value.replaces);  // not editing the just-finished value
  EXPECT_TRUE(v.key.enabled);      // ready to stage a NEW clause
  EXPECT_FALSE(v.key.replaces);
}

// ---------------------------------------------------------------------------
// value_key: the Value dropdown's items follow the staged key (ADD) or the
// clause's key (value-REPLACE).
// ---------------------------------------------------------------------------
TEST(LuaAssist, ValueKey_AddMode_IsStagedKey) {
  const AssistView v = computeAssist("", 0, schema(), "sensor", "==", "");
  EXPECT_EQ(v.value_key, "sensor");
}

TEST(LuaAssist, ValueKey_ReplaceMode_IsClauseKey) {
  const AssistView v = computeAssist(kClause, 12, schema(), "", "", "");  // strictly inside the value
  ASSERT_TRUE(v.value.replaces);
  EXPECT_EQ(v.value_key, "robot");
}

}  // namespace
