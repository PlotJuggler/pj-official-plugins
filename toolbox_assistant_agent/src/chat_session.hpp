// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace assistant_agent {

// Where a conversation turn is in its lifecycle. The panel is single-turn:
// the user sends one message, the backend thinks (WaitingForLlm), optionally
// runs tools (ExecutingTool), then returns to Idle. The input box + Send are
// only enabled in Idle; Cancel is only enabled otherwise.
enum class TurnState {
  Idle,           // ready to accept a new user message
  WaitingForLlm,  // request in flight to the backend
  ExecutingTool,  // a tool call is running (on the GUI thread via GuiExecutor)
};

// One rendered line of the transcript. `Tool` rows carry the model's tool call
// and its result, collapsed to a short summary in the view (the full payload
// would blow the QPlainTextEdit and leak internal JSON to the user).
struct ChatMessage {
  enum class Role { User, Assistant, System, Tool };
  Role role;
  std::string text;
};

// Pure, Qt-free transcript + turn-state model. Mutated on the GUI thread only;
// the dialog owns one instance and renders it into the panel's QPlainTextEdit
// on every change. Kept free of threading and host types so it is exhaustively
// unit-testable in isolation.
class ChatSession {
 public:
  void addUser(std::string text) {
    messages_.push_back({ChatMessage::Role::User, std::move(text)});
  }
  void addAssistant(std::string text) {
    messages_.push_back({ChatMessage::Role::Assistant, std::move(text)});
  }
  // Grow the assistant's current reply instead of starting a new one. Backends
  // that stream deliver a reply in many small pieces; without this each piece
  // would become its own speaker-tagged turn and the transcript would read as
  // one word per line.
  void appendAssistant(const std::string& text) {
    if (!messages_.empty() && messages_.back().role == ChatMessage::Role::Assistant) {
      messages_.back().text += text;
      return;
    }
    messages_.push_back({ChatMessage::Role::Assistant, text});
  }
  void addSystem(std::string text) {
    messages_.push_back({ChatMessage::Role::System, std::move(text)});
  }
  void addTool(std::string text) {
    messages_.push_back({ChatMessage::Role::Tool, std::move(text)});
  }

  [[nodiscard]] const std::vector<ChatMessage>& messages() const {
    return messages_;
  }
  [[nodiscard]] TurnState state() const {
    return state_;
  }
  void setState(TurnState state) {
    state_ = state;
  }
  [[nodiscard]] bool busy() const {
    return state_ != TurnState::Idle;
  }

  void clear() {
    messages_.clear();
    frozen_.clear();
    frozen_count_ = 0;
    state_ = TurnState::Idle;
  }

  // Render the whole transcript to a single plain-text block for the panel's
  // read-only QPlainTextEdit. Each row is prefixed with a speaker tag; blank
  // lines separate turns so a long exchange stays scannable.
  [[nodiscard]] std::string render() const {
    // Only the last message can still change (appendAssistant grows it); every
    // earlier one is final. So keep their rendered text and rebuild just the
    // tail. Without this a streaming reply re-renders the entire history once
    // per chunk, which is quadratic over a long conversation.
    if (frozen_count_ > messages_.size()) {  // cleared/rewound — start over
      frozen_.clear();
      frozen_count_ = 0;
    }
    while (frozen_count_ + 1 < messages_.size()) {
      const auto& m = messages_[frozen_count_];
      frozen_ += speakerTag(m.role);
      frozen_ += m.text;
      frozen_ += "\n\n";
      ++frozen_count_;
    }
    if (messages_.empty()) {
      return {};
    }
    const auto& last = messages_.back();
    std::string out;
    out.reserve(frozen_.size() + last.text.size() + 16);
    out += frozen_;
    out += speakerTag(last.role);
    out += last.text;
    out += "\n\n";
    return out;
  }

  // A one-line status suitable for the panel's status label, reflecting the
  // turn state (and the busy hint the user needs while a request is in flight).
  //
  // No full stop: the panel appends the turn's token counts after this, and
  // "Ready. - 19.8k" reads as two fragments rather than one line.
  [[nodiscard]] std::string statusText() const {
    switch (state_) {
      case TurnState::Idle:
        return "Ready";
      case TurnState::WaitingForLlm:
        return "Thinking…";
      case TurnState::ExecutingTool:
        return "Running a tool…";
    }
    return {};
  }

 private:
  static const char* speakerTag(ChatMessage::Role role) {
    switch (role) {
      case ChatMessage::Role::User:
        return "You: ";
      case ChatMessage::Role::Assistant:
        return "Assistant: ";
      case ChatMessage::Role::System:
        return "• ";
      case ChatMessage::Role::Tool:
        return "  ↳ ";
    }
    return "";
  }

  std::vector<ChatMessage> messages_;
  // Rendered text of messages_[0, frozen_count_) — the ones that can no longer
  // change. Mutable because render() is a const observer that maintains it.
  mutable std::string frozen_;
  mutable std::size_t frozen_count_ = 0;
  TurnState state_ = TurnState::Idle;
};

}  // namespace assistant_agent
