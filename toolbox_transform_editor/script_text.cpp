// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#include "script_text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <pj_base/builtin/builtin_object.hpp>

namespace transform_editor::text {

using derived_recipes::luaStringEscape;

std::string buildVariablePrologue(std::string_view language, const std::vector<InputBinding>& bindings) {
  const bool python = language == "python";
  std::string out;
  for (const auto& binding : bindings) {
    out += std::string(python ? "" : "local ") + binding.var + " = inputs[\"" + luaStringEscape(binding.key) + "\"]\n";
  }
  return out;
}

std::string inferredVariableName(std::string_view topic, const std::set<std::string>& taken) {
  std::string leaf(topic);
  while (!leaf.empty() && leaf.back() == '/') {
    leaf.pop_back();
  }
  if (const std::size_t slash = leaf.rfind('/'); slash != std::string::npos) {
    leaf = leaf.substr(slash + 1);
  } else if (const std::size_t colon = leaf.rfind(':'); colon != std::string::npos) {
    leaf = leaf.substr(colon + 1);
  }
  std::string name;
  for (const char ch : leaf) {
    const bool ok = std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    const char mapped = ok ? ch : '_';
    if (mapped == '_' && !name.empty() && name.back() == '_') {
      continue;  // one underscore for a run of separators
    }
    name.push_back(mapped);
  }
  while (!name.empty() && name.back() == '_') {
    name.pop_back();
  }
  if (name.empty()) {
    name = "input";
  } else if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    name = "input_" + name;
  }
  if (isReservedScriptName(name)) {
    name += "_";
  }
  std::string candidate = name;
  for (int n = 2; taken.count(candidate) != 0; ++n) {
    candidate = name + "_" + std::to_string(n);
  }
  return candidate;
}

namespace {

bool isWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// The index after the string whose opening quote is at `at`.
std::size_t skipQuoted(std::string_view text, std::size_t at, bool python) {
  const char quote = text[at];
  const std::size_t n = text.size();
  if (python && text.compare(at, 3, std::string(3, quote)) == 0) {
    const std::size_t close = text.find(std::string(3, quote), at + 3);
    return close == std::string_view::npos ? n : close + 3;
  }
  std::size_t k = at + 1;
  while (k < n && text[k] != quote && text[k] != '\n') {
    k += text[k] == '\\' ? 2 : 1;
  }
  return std::min(k + 1, n);
}

// `at` is on a '[': the index after a [[...]] / [=[...]=] block, or `at` when there is none.
std::size_t skipLuaLong(std::string_view text, std::size_t at) {
  const std::size_t n = text.size();
  std::size_t k = at + 1;
  std::size_t level = 0;
  while (k < n && text[k] == '=') {
    ++level;
    ++k;
  }
  if (k >= n || text[k] != '[') {
    return at;
  }
  const std::size_t close = text.find("]" + std::string(level, '=') + "]", k + 1);
  return close == std::string_view::npos ? n : close + level + 2;
}

}  // namespace

std::vector<CodeToken> tokenize(std::string_view text, std::string_view language) {
  const bool python = language == "python";
  const std::size_t n = text.size();
  std::vector<CodeToken> out;
  std::size_t i = 0;
  while (i < n) {
    const char c = text[i];
    CodeToken token{CodeTokenKind::kOther, i, i + 1};
    if (c == '"' || c == '\'') {
      token = {CodeTokenKind::kString, i, skipQuoted(text, i, python)};
    } else if (python && c == '#') {
      std::size_t end = i;
      while (end < n && text[end] != '\n') {
        ++end;
      }
      token = {CodeTokenKind::kComment, i, end};
    } else if (!python && c == '-' && i + 1 < n && text[i + 1] == '-') {
      const std::size_t after = i + 2 < n && text[i + 2] == '[' ? skipLuaLong(text, i + 2) : i + 2;
      std::size_t end = after;
      if (after == i + 2) {
        while (end < n && text[end] != '\n') {
          ++end;
        }
      }
      token = {CodeTokenKind::kComment, i, end};
    } else if (!python && c == '[' && skipLuaLong(text, i) != i) {
      token = {CodeTokenKind::kString, i, skipLuaLong(text, i)};
    } else if (isWordChar(c)) {
      std::size_t end = i;
      while (end < n && isWordChar(text[end])) {
        ++end;
      }
      token = {CodeTokenKind::kWord, i, end};
    }
    out.push_back(token);
    i = token.end;
  }
  return out;
}

std::size_t returnArity(const std::string& body, const std::string& language) {
  const bool python = language == "python";
  const std::vector<CodeToken> tokens = tokenize(body, language);
  const std::string_view text = body;
  std::size_t best = 0;
  std::size_t t = 0;
  while (t < tokens.size()) {
    const CodeToken& head = tokens[t];
    if (head.kind != CodeTokenKind::kWord || text.substr(head.begin, head.end - head.begin) != "return") {
      ++t;
      continue;
    }
    // The expression list after `return`.
    int depth = 0;
    std::size_t commas = 0;
    bool any = false;
    char last = '\0';  // the last significant byte: a Lua list may continue on the next line after a ','
    std::size_t k = t + 1;
    for (; k < tokens.size(); ++k) {
      const CodeToken& tok = tokens[k];
      if (tok.kind == CodeTokenKind::kComment) {
        if (python && depth == 0) {
          break;
        }
        continue;
      }
      if (tok.kind == CodeTokenKind::kString) {
        any = true;
        last = '"';
        continue;
      }
      if (tok.kind == CodeTokenKind::kWord) {
        const std::string_view word = text.substr(tok.begin, tok.end - tok.begin);
        if (depth == 0 && !python && (word == "end" || word == "else" || word == "elseif" || word == "until")) {
          break;
        }
        any = true;
        last = 'w';
        continue;
      }
      const char d = text[tok.begin];
      if (d == '(' || d == '[' || d == '{') {
        any = true;
        ++depth;
        last = d;
      } else if (d == ')' || d == ']' || d == '}') {
        --depth;
        last = d;
      } else if (depth == 0 && d == ';') {
        break;
      } else if (depth == 0 && d == '\n') {
        if (python || last != ',') {
          break;
        }
      } else if (depth == 0 && d == ',') {
        ++commas;
        last = d;
      } else if (std::isspace(static_cast<unsigned char>(d)) == 0) {
        any = true;
        last = d;
      }
    }
    if (any) {
      best = std::max(best, commas + 1);
    }
    t = std::max(k, t + 1);
  }
  return std::clamp<std::size_t>(best, 1, 8);
}

namespace {

// Calls fn(word_begin, word_end) for every identifier use of `text`: words that are not strings or
// comments, not a field after `.` (or a method after `:` in Luau), not a Luau table key and not a
// Python keyword argument.
void forEachIdentifierUse(
    std::string_view text, std::string_view language, const std::function<void(std::size_t, std::size_t)>& fn) {
  const bool python = language == "python";
  const std::vector<CodeToken> tokens = tokenize(text, language);
  // Neighbouring significant (non-blank, non-comment) tokens of tokens[i].
  const auto significant = [&](std::size_t from, int step) -> const CodeToken* {
    for (std::ptrdiff_t k = static_cast<std::ptrdiff_t>(from) + step;
         k >= 0 && k < static_cast<std::ptrdiff_t>(tokens.size()); k += step) {
      const CodeToken& tok = tokens[static_cast<std::size_t>(k)];
      if (tok.kind == CodeTokenKind::kComment ||
          (tok.kind == CodeTokenKind::kOther && std::isspace(static_cast<unsigned char>(text[tok.begin])) != 0)) {
        continue;
      }
      return &tok;
    }
    return nullptr;
  };
  const auto is_punct = [&](const CodeToken* tok, char c) {
    return tok != nullptr && tok->kind == CodeTokenKind::kOther && text[tok->begin] == c;
  };
  std::vector<char> brackets;  // the open brackets, innermost last
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const CodeToken& tok = tokens[i];
    if (tok.kind == CodeTokenKind::kOther) {
      const char c = text[tok.begin];
      if (c == '(' || c == '[' || c == '{') {
        brackets.push_back(c);
      } else if ((c == ')' || c == ']' || c == '}') && !brackets.empty()) {
        brackets.pop_back();
      }
      continue;
    }
    if (tok.kind != CodeTokenKind::kWord) {
      continue;
    }
    const CodeToken* prev = significant(i, -1);
    const CodeToken* next = significant(i, +1);
    // A field or a method, not a variable. But `a .. b` (Luau concatenation) puts a name after TWO dots:
    // the tokenizer emits each '.' on its own, so a dot directly preceded by another dot is not a field access.
    const bool concat = !python && is_punct(prev, '.') && prev > tokens.data() &&
                        (prev - 1)->kind == CodeTokenKind::kOther && text[(prev - 1)->begin] == '.';
    if ((is_punct(prev, '.') && !concat) || (!python && is_punct(prev, ':'))) {
      continue;
    }
    // `name =` (not `==`) as a key of a table constructor (Luau) or a keyword argument (Python).
    if (is_punct(next, '=')) {
      const CodeToken* after = significant(static_cast<std::size_t>(next - tokens.data()), +1);
      const bool single = !is_punct(after, '=') || after->begin != next->end;
      const char inner = brackets.empty() ? '\0' : brackets.back();
      const bool key_position =
          python ? inner == '(' : (inner == '{' && (is_punct(prev, '{') || is_punct(prev, ',') || is_punct(prev, ';')));
      if (single && key_position) {
        continue;
      }
    }
    fn(tok.begin, tok.end);
  }
}

}  // namespace

bool mentionsIdentifier(std::string_view text, std::string_view language, std::string_view word) {
  bool found = false;
  forEachIdentifierUse(text, language, [&](std::size_t begin, std::size_t end) {
    found = found || text.substr(begin, end - begin) == word;
  });
  return found;
}

std::string renameIdentifiers(
    std::string_view text, std::string_view language, const std::map<std::string, std::string>& renames) {
  std::string out;
  out.reserve(text.size());
  std::size_t copied = 0;
  forEachIdentifierUse(text, language, [&](std::size_t begin, std::size_t end) {
    const auto it = renames.find(std::string(text.substr(begin, end - begin)));
    if (it == renames.end()) {
      return;
    }
    out.append(text.substr(copied, begin - copied));
    out += it->second;
    copied = end;
  });
  out.append(text.substr(copied));
  return out;
}

bool isReservedScriptName(std::string_view name, std::string_view language) {
  static const std::set<std::string, std::less<>> kLuau = {
      "and", "break", "continue", "do",  "else", "elseif", "end",    "export", "false", "for",   "function", "if",
      "in",  "local", "nil",      "not", "or",   "repeat", "return", "then",   "true",  "until", "while",    "type"};
  static const std::set<std::string, std::less<>> kPython = {
      "False",    "None",   "True",  "and",  "as",     "assert",   "async",   "await", "break", "class",
      "continue", "def",    "del",   "elif", "else",   "except",   "finally", "for",   "from",  "global",
      "if",       "import", "in",    "is",   "lambda", "nonlocal", "not",     "or",    "pass",  "raise",
      "return",   "try",    "while", "with", "yield",  "match",    "case"};
  // names the generated chunk and the standard libraries use
  static const std::set<std::string, std::less<>> kShared = {
      "inputs",   "params", "pj",    "math",  "string", "table", "print",  "pairs",     "ipairs", "tostring",
      "tonumber", "select", "error", "os",    "bit32",  "utf8",  "buffer", "coroutine", "vector", "len",
      "range",    "str",    "int",   "float", "list",   "dict",  "min",    "max",       "abs"};
  if (kShared.count(name) != 0) {
    return true;
  }
  const bool luau = language != "python";
  const bool python = language != "luau";
  return (luau && kLuau.count(name) != 0) || (python && kPython.count(name) != 0);
}

std::string varNameError(std::string_view name, std::string_view language, const std::vector<std::string>& other_vars) {
  const std::string shown(name);
  const bool identifier = !name.empty() && std::isdigit(static_cast<unsigned char>(name.front())) == 0 &&
                          std::all_of(name.begin(), name.end(), [](char c) { return isWordChar(c); });
  if (!identifier) {
    return "'" + shown + "' is not a valid name: use letters, digits and _, and do not start with a digit.";
  }
  if (isReservedScriptName(name, language)) {
    return "'" + shown + "' is reserved by the language or by the script; pick another name.";
  }
  if (std::find(other_vars.begin(), other_vars.end(), shown) != other_vars.end()) {
    return "'" + shown + "' is already the name of another input.";
  }
  return {};
}

BuiltScript buildOnDemandScript(
    const std::string& body, const std::string& globals, const std::vector<InputBinding>& bindings,
    const ResolvedEvalInputs& resolved, std::string_view language) {
  const bool python = language == "python";
  const std::string prologue = buildVariablePrologue(language, bindings);
  const std::string user = globals.empty() ? body : globals + "\n" + body;
  const std::string chunk_body = prologue + user;
  BuiltScript out = python ? derived_recipes::buildOnDemandChunkPython(chunk_body, resolved)
                           : derived_recipes::buildResolvedOnDemandChunk(chunk_body, resolved);
  // The user's text follows the variable prologue, one line per line break; the layout the builder
  // reports is that of the whole chunk_body.
  int line = out.layout.body_first_line + static_cast<int>(std::count(prologue.begin(), prologue.end(), '\n'));
  out.layout = {};
  if (!globals.empty()) {
    out.layout.globals_first_line = line;
    out.layout.globals_lines = derived_recipes::physicalLines(globals);
    line += out.layout.globals_lines;
  }
  out.layout.body_first_line = line;
  out.layout.body_lines = derived_recipes::physicalLines(body);
  return out;
}

namespace {

std::string userLine(long chunk_line, const ScriptLayout& layout) {
  const auto within = [&](int first, int count) {
    return count > 0 && chunk_line >= first && chunk_line < first + count;
  };
  if (within(layout.body_first_line, layout.body_lines)) {
    return "line " + std::to_string(chunk_line - layout.body_first_line + 1);
  }
  if (within(layout.globals_first_line, layout.globals_lines)) {
    return "globals line " + std::to_string(chunk_line - layout.globals_first_line + 1);
  }
  return "line ?";
}

// The digits at `at`, or 0 digits.
std::size_t digitsAt(const std::string& text, std::size_t at) {
  std::size_t end = at;
  while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])) != 0) {
    ++end;
  }
  return end - at;
}

}  // namespace

std::string remapScriptLines(const std::string& error, const ScriptLayout& layout) {
  // Matches `<chunk>:<n>:` at `i` (Luau); returns the index of the closing colon, or 0.
  const auto luau_at = [&](std::size_t i, long& number) -> std::size_t {
    if (i != 0 && isWordChar(error[i - 1])) {
      return 0;
    }
    for (const std::string_view chunk : {"script", "filter", "rule"}) {
      if (error.compare(i, chunk.size(), chunk) != 0 || error.compare(i + chunk.size(), 1, ":") != 0) {
        continue;
      }
      const std::size_t first = i + chunk.size() + 1;
      const std::size_t digits = digitsAt(error, first);
      if (digits != 0 && digits <= 9 && error.compare(first + digits, 1, ":") == 0) {
        number = std::stol(error.substr(first, digits));
        return first + digits;
      }
    }
    return 0;
  };
  // Matches `<string>(<n>)` or `"<string>", line <n>` at `i` (Python); returns the index after it, or 0.
  const auto python_at = [&](std::size_t i, long& number) -> std::size_t {
    std::size_t first = 0;
    std::string_view close;
    if (error.compare(i, 9, "<string>(") == 0) {
      first = i + 9;
      close = ")";
    } else if (error.compare(i, 17, "\"<string>\", line ") == 0) {
      first = i + 17;
    } else {
      return 0;
    }
    const std::size_t digits = digitsAt(error, first);
    if (digits == 0 || digits > 9 || error.compare(first + digits, close.size(), close) != 0) {
      return 0;
    }
    number = std::stol(error.substr(first, digits));
    return first + digits + close.size();
  };
  // Matches `(<string>, line <n>)` at `i` (a Python SyntaxError from ast.parse); returns the index after it, or 0.
  const auto syntax_error_at = [&](std::size_t i, long& number) -> std::size_t {
    constexpr std::string_view kOpen = "(<string>, line ";
    if (error.compare(i, kOpen.size(), kOpen) != 0) {
      return 0;
    }
    const std::size_t first = i + kOpen.size();
    const std::size_t digits = digitsAt(error, first);
    if (digits == 0 || digits > 9 || error.compare(first + digits, 1, ")") != 0) {
      return 0;
    }
    number = std::stol(error.substr(first, digits));
    return first + digits + 1;
  };
  std::string out;
  std::size_t i = 0;
  while (i < error.size()) {
    long number = 0;
    if (const std::size_t syntax_end = syntax_error_at(i, number); syntax_end != 0) {
      out += "(" + userLine(number, layout) + ")";
      i = syntax_end;
    } else if (const std::size_t colon = luau_at(i, number); colon != 0) {
      out += userLine(number, layout);
      i = colon;  // the closing colon stays
    } else if (const std::size_t after = python_at(i, number); after != 0) {
      out += userLine(number, layout);
      i = after;
    } else {
      out.push_back(error[i++]);
    }
  }
  return out;
}

bool namesUndefinedName(std::string_view language, std::string_view error) {
  static constexpr std::string_view kLuau[] = {
      "attempt to index nil", "attempt to index a nil value", "attempt to call a nil value"};
  static constexpr std::string_view kPython[] = {"NameError: name '", "'NoneType' object has no attribute"};
  const bool python = language == "python";
  const auto matches = [&](const auto& phrases) {
    return std::any_of(std::begin(phrases), std::end(phrases), [&](std::string_view phrase) {
      return error.find(phrase) != std::string_view::npos;
    });
  };
  return python ? matches(kPython) : matches(kLuau);
}

std::string objectTypeLabel(const std::string& type) {
  if (type == "kSceneEntities") {
    return "scene";
  }
  if (type == "kFrameTransforms") {
    return "transforms";
  }
  // "kOccupancyGrid" -> "occupancy grid": only a builtin object type is split into words.
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  if (!parsed || *parsed == PJ::sdk::BuiltinObjectType::kNone || type.size() < 2 || type.front() != 'k') {
    return type;
  }
  std::string label;
  for (std::size_t i = 1; i < type.size(); ++i) {
    const unsigned char ch = static_cast<unsigned char>(type[i]);
    if (std::isupper(ch) != 0) {
      if (i > 1) {
        label.push_back(' ');
      }
      label.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      label.push_back(type[i]);
    }
  }
  return label;
}

bool isObjectOutputType(const std::string& type) {
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  return parsed && *parsed != PJ::sdk::BuiltinObjectType::kNone;
}

std::string sceneKindForOutputType(const std::string& type) {
  const auto parsed = PJ::sdk::parseBuiltinObjectType(type);
  if (parsed &&
      (*parsed == PJ::sdk::BuiltinObjectType::kImage || *parsed == PJ::sdk::BuiltinObjectType::kImageAnnotations ||
       *parsed == PJ::sdk::BuiltinObjectType::kDepthImage || *parsed == PJ::sdk::BuiltinObjectType::kVideoFrame)) {
    return "2d";
  }
  return "3d";
}

}  // namespace transform_editor::text
