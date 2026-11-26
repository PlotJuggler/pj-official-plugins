// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace assistant_agent {

// The panel is single-turn: Send is enabled only in Idle, Cancel only otherwise.
enum class TurnState {
  Idle,
  WaitingForLlm,
};

// One rendered message in the transcript. `Tool` rows carry a collapsed summary of
// the model's tool call and result (the full payload would leak internal JSON).
struct ChatMessage {
  enum class Role { User, Assistant, System, Tool };
  Role role;
  std::string text;
};

// Pure, Qt-free transcript + turn-state model, mutated on the GUI thread only.
class ChatSession {
 public:
  void addUser(std::string text) {
    messages_.push_back({ChatMessage::Role::User, std::move(text)});
  }
  void addAssistant(std::string text) {
    messages_.push_back({ChatMessage::Role::Assistant, std::move(text)});
  }
  // Grows the current assistant reply; streamed pieces must not each become a
  // speaker-tagged message.
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
    frozen_markdown_.clear();
    frozen_markdown_count_ = 0;
    state_ = TurnState::Idle;
  }

  // Speaker labels get their own paragraph so a body can start with a heading,
  // list or fence. User and assistant bodies stay Markdown; system and tool text
  // is escaped (status text, not authored prose).
  [[nodiscard]] std::string renderMarkdown() const {
    if (frozen_markdown_count_ > messages_.size()) {
      frozen_markdown_.clear();
      frozen_markdown_count_ = 0;
    }
    while (frozen_markdown_count_ + 1 < messages_.size()) {
      frozen_markdown_ += renderMarkdownMessage(messages_[frozen_markdown_count_]);
      ++frozen_markdown_count_;
    }
    if (messages_.empty()) {
      return {};
    }
    return frozen_markdown_ + renderMarkdownMessage(messages_.back());
  }

  // No full stop: the panel appends the token counts, and "Ready. - 19.8k" reads
  // as two fragments.
  [[nodiscard]] std::string statusText() const {
    switch (state_) {
      case TurnState::Idle:
        return "Ready";
      case TurnState::WaitingForLlm:
        return "Thinking…";
    }
    return {};
  }

 private:
  static const char* markdownSpeakerTag(ChatMessage::Role role) {
    switch (role) {
      case ChatMessage::Role::User:
        return "**You:**";
      case ChatMessage::Role::Assistant:
        return "**Assistant:**";
      case ChatMessage::Role::System:
        return "**System:**";
      case ChatMessage::Role::Tool:
        return "**Tool:**";
    }
    return "";
  }

  static std::string escapeMarkdownLiteral(const std::string& text) {
    std::string out;
    out.reserve(text.size() * 2);
    for (char ch : text) {
      const auto byte = static_cast<unsigned char>(ch);
      if (byte < 128 && std::ispunct(byte) != 0) {
        out.push_back('\\');
      }
      out.push_back(ch);
    }
    return out;
  }

  static std::string closeOpenFence(std::string text) {
    char open_marker = '\0';
    std::size_t open_length = 0;
    std::size_t line_start = 0;
    while (line_start <= text.size()) {
      const std::size_t line_end = text.find('\n', line_start);
      const std::size_t end = line_end == std::string::npos ? text.size() : line_end;
      std::size_t pos = line_start;
      std::size_t indent = 0;
      while (pos < end && text[pos] == ' ' && indent < 4) {
        ++pos;
        ++indent;
      }
      if (indent <= 3 && pos < end && (text[pos] == '`' || text[pos] == '~')) {
        const char marker = text[pos];
        std::size_t run_end = pos;
        while (run_end < end && text[run_end] == marker) {
          ++run_end;
        }
        const std::size_t run_length = run_end - pos;
        if (open_marker == '\0' && run_length >= 3) {
          const bool valid_info = marker != '`' || text.find('`', run_end) >= end;
          if (valid_info) {
            open_marker = marker;
            open_length = run_length;
          }
        } else if (marker == open_marker && run_length >= open_length) {
          bool only_space_follows = true;
          for (std::size_t i = run_end; i < end; ++i) {
            if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
              only_space_follows = false;
              break;
            }
          }
          if (only_space_follows) {
            open_marker = '\0';
            open_length = 0;
          }
        }
      }
      if (line_end == std::string::npos) {
        break;
      }
      line_start = line_end + 1;
    }
    if (open_marker != '\0') {
      if (!text.empty() && text.back() != '\n') {
        text.push_back('\n');
      }
      text.append(open_length, open_marker);
    }
    return text;
  }

  static std::string renderMarkdownMessage(const ChatMessage& message) {
    std::string body = message.text;
    if (message.role == ChatMessage::Role::System || message.role == ChatMessage::Role::Tool) {
      body = escapeMarkdownLiteral(body);
    } else {
      body = closeOpenFence(std::move(body));
    }
    std::string out = markdownSpeakerTag(message.role);
    out += "\n\n";
    out += body;
    out += "\n\n";
    return out;
  }

  std::vector<ChatMessage> messages_;
  // Rendered Markdown of messages_[0, frozen_markdown_count_), the ones that can no
  // longer change; mutable because renderMarkdown() is const.
  mutable std::string frozen_markdown_;
  mutable std::size_t frozen_markdown_count_ = 0;
  TurnState state_ = TurnState::Idle;
};

}  // namespace assistant_agent
