// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

// Text helpers of the Transform Editor's script editing: variable names, a small tokenizer
// that tells code from strings and comments, identifier renaming, the layout of the generated
// on-demand chunk and the mapping of host error lines back to the user's code. Only the editor
// needs them; the shared recipe library (common/derived_recipes) keeps what the assistant also uses.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "derived_recipes/recipes.hpp"

namespace transform_editor::text {

using derived_recipes::BuiltScript;
using derived_recipes::ResolvedEvalInputs;
using derived_recipes::ScriptLayout;

// One local of an on-demand body: the variable the script reads and the `inputs[...]` key it
// is bound from.
struct InputBinding {
  std::string var;
  std::string key;
};

// The lines that bind one local per input ("local cloud = inputs[\"/lidar_top\"]" for Luau,
// "cloud = inputs[\"/lidar_top\"]" for Python), to be put in front of a user body.
[[nodiscard]] std::string buildVariablePrologue(std::string_view language, const std::vector<InputBinding>& bindings);

// The variable name an input gets by default: the leaf of its topic or field path made a valid
// identifier ("/lidar_top" -> "lidar_top", "pose/x" -> "x", "run1:/cloud" -> "cloud"). A name
// in `taken`, a Lua/Python keyword or a name the chunk itself uses gets a "_2", "_3"... suffix
// (a keyword gets "_" first), so the result is never in `taken`.
[[nodiscard]] std::string inferredVariableName(std::string_view topic, const std::set<std::string>& taken);

// "kPointCloud" -> "point cloud", "kSceneEntities" -> "scene", "kFrameTransforms" -> "transforms",
// "number" and "string" unchanged; any other builtin object type is its name split into lower-case
// words. A name that is not a builtin object type stays as it is.
[[nodiscard]] std::string objectTypeLabel(const std::string& type);

// --- code tokens -----------------------------------------------------------

// What a stretch of script text is, for the helpers that must not touch strings or comments.
enum class CodeTokenKind {
  kWord,     // a run of letters, digits and '_' (keywords, names, numbers)
  kString,   // a quoted string; a Luau [[long]] / [=[long]=] string; a Python triple-quoted string
  kComment,  // `-- ...` and `--[[ ... ]]` (Luau), `# ...` (Python); the line break is NOT part of it
  kOther,    // any other single byte (punctuation, whitespace, line breaks)
};

struct CodeToken {
  CodeTokenKind kind = CodeTokenKind::kOther;
  std::size_t begin = 0;
  std::size_t end = 0;  // one past the last byte
};

// Split `text` (language "luau" or "python") into tokens that cover it completely, in order. An
// unterminated string runs to the end of its line (a long string or triple quote, to the end of
// the text).
[[nodiscard]] std::vector<CodeToken> tokenize(std::string_view text, std::string_view language);

// How many values the body returns per sample, read from its `return` statements: the commas at the
// top level of a return's expression list, the most of any return (an early `return nil` does not
// count). 1 when no return has a value, at most 8. Strings, comments and brackets are skipped.
[[nodiscard]] std::size_t returnArity(const std::string& body, const std::string& language);

// `word` as an identifier of `text`: not inside a string or comment, not a field after `.` (or a
// method after `:` in Luau), not a Luau table key (`{ word = 1 }`) and not a Python keyword
// argument (`f(word=1)`).
[[nodiscard]] bool mentionsIdentifier(std::string_view text, std::string_view language, std::string_view word);

// `text` with every identifier use found as in mentionsIdentifier renamed through `renames` (old ->
// new), all at once, so two names can be swapped. Nothing else changes.
[[nodiscard]] std::string renameIdentifiers(
    std::string_view text, std::string_view language, const std::map<std::string, std::string>& renames);

// A name the chunk or the language owns: a keyword of `language` ("luau" or "python"; any other
// value means both), or a name the generated chunk and the standard libraries use ("inputs",
// "params", "math", ...). Such a name cannot be a Var.
[[nodiscard]] bool isReservedScriptName(std::string_view name, std::string_view language = {});

// Why `name` cannot be the Var of an input, "" when it can: an identifier ([A-Za-z_][A-Za-z0-9_]*),
// not reserved in `language`, not one of `other_vars` (the Vars of the other inputs).
[[nodiscard]] std::string varNameError(
    std::string_view name, std::string_view language, const std::vector<std::string>& other_vars);

// `error` with the chunk's line numbers ("script:8:", "filter:3:", "rule:2:" in Luau,
// "<string>(8)" or `File "<string>", line 8` in Python) rewritten as the user sees their code:
// "line 1", "globals line 2", or "line ?" when the line is in neither part (the generated frame).
[[nodiscard]] std::string remapScriptLines(const std::string& error, const ScriptLayout& layout);

// True when `error` is the kind a script raises when it reads a name that is not bound (a nil
// local in Luau, a NameError or a None attribute in Python): the caller then lists the names
// that are. The phrases are pinned by tests.
[[nodiscard]] bool namesUndefinedName(std::string_view language, std::string_view error);

// The on-demand chunk (language "luau" or "python") of the Transform Editor: the alias table for
// `resolved`, one local per binding, the globals, then the body -- the same text
// buildResolvedOnDemandChunk / buildOnDemandChunkPython give for that composed body.
[[nodiscard]] BuiltScript buildOnDemandScript(
    const std::string& body, const std::string& globals, const std::vector<InputBinding>& bindings,
    const ResolvedEvalInputs& resolved, std::string_view language);

// --- on-demand output types ------------------------------------------------

[[nodiscard]] bool isObjectOutputType(const std::string& type);
// Which scene tab shows an output of this type: "2d" for image-like types, "3d" otherwise.
[[nodiscard]] std::string sceneKindForOutputType(const std::string& type);

}  // namespace transform_editor::text
