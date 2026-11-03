#pragma once

// Portable dictionary (DBC / CSV) paths for the loaders that keep per-channel
// dictionaries in their config (data_load_mf4, data_load_blf, data_load_candump).
//
// Why: a layout normally keeps the data file and its dictionaries in one
// folder. Absolute dictionary paths stop working when that folder is moved and
// silently point at the OLD folder's files when it is copied. So a dictionary
// that lies under the data file's directory is saved relative to it, and on
// load a relative path is resolved against the data file that is open now.
//
// Layout replay: PlotJuggler rewrites "filepath" to the file it actually opens
// and, when that replaces a different absolute path, passes the replaced path
// as the host-private key kSavedFilepathKey. Configs saved before this feature
// hold absolute dictionary paths; those that lie under the folder that moved
// (e.g. a dbc/ folder next to data/) are rebased onto its new location.
// Plugins read that key in loadConfig() and never write it from saveConfig().
//
// Containment is LEXICAL (no symlink resolution): "/run2/x" is not under "/run", and "/run/../other/x" is not
// under "/run". The only filesystem access is resolveAuxPath checking whether a rebased candidate exists.
// A string containing "://" is a URL, never a filesystem path: it is never
// rewritten, and a data file given as a URL has no directory.

#include <pj_base/expected.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pj_can_dbc {

/// Host-private (layout replay): the absolute data-file path the host replaced. Read it; never write it from
/// saveConfig.
inline constexpr char kSavedFilepathKey[] = "__pj_saved_filepath";

/// Plugin-owned: the data file's directory when the config was saved. Lets settings reused for another data
/// file (the host's per-plugin pre-fill) still find dictionaries saved relative to the previous one.
inline constexpr char kAuxBaseDirKey[] = "aux_base_dir";

/// The config values resolveAuxPath needs, read once per loadConfig.
struct AuxPathContext {
  std::string data_file;        ///< "filepath": the data file loaded now
  std::string saved_data_file;  ///< kSavedFilepathKey, or empty
  std::string base_dir;         ///< kAuxBaseDirKey, or empty
};

/// Config form: relative to data_file's directory ('/' separators) when aux_path is absolute and lies
/// lexically under it; otherwise aux_path byte-for-byte.
std::string toConfigPath(std::string_view aux_path, std::string_view data_file);

/// kAuxBaseDirKey value: data_file's lexically normal directory ('/' separators); empty when data_file is
/// empty, relative or contains "://".
std::string auxBaseDir(std::string_view data_file);

/// Path to open for a stored path: relative -> against the current data dir (or the pre-fill base_dir when no
/// saved_data_file); absolute under the folder that moved (the common trailing part of saved_data_file and
/// data_file) -> the rebased path if that file exists, else the stored file if it exists, else the rebased path;
/// everything else (outside it, "://", empty data_file) -> unchanged.
std::string resolveAuxPath(std::string_view stored, const AuxPathContext& context);

/// toConfigPath applied to every entry.
std::vector<std::string> toConfigPaths(const std::vector<std::string>& paths, std::string_view data_file);

/// resolveAuxPath applied to every entry.
std::vector<std::string> resolveAuxPaths(const std::vector<std::string>& stored, const AuxPathContext& context);

/// The whole text of the regular file at the UTF-8 `path`, opened once. Errors: "<kind> file not found: <path>"
/// (not a regular file) or "cannot read <kind> file: <path>" (open or read failure).
PJ::Expected<std::string> readAuxFile(const std::string& path, std::string_view kind);

}  // namespace pj_can_dbc
