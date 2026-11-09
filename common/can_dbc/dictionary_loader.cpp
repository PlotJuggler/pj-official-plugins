#include <filesystem>
#include <pj_base/sdk/text_utils.hpp>
#include <pj_can_dbc/csv_dictionary.hpp>
#include <pj_can_dbc/dictionary_loader.hpp>
#include <pj_can_dbc/dictionary_paths.hpp>

namespace pj_can_dbc {

PJ::Status loadDictionary(CanDecoder& decoder, const std::string& path, std::string& warnings) {
  if (PJ::sdk::lowerAscii(std::filesystem::path(path).extension().string()) != ".csv") {
    return decoder.loadDbcFile(path);
  }
  auto csv_text = readAuxFile(path, "dictionary");
  if (!csv_text) {
    return PJ::unexpected(csv_text.error());
  }
  std::string dbc_text;
  std::string csv_warnings;
  PJ::Status status = arusCsvToDbc(*csv_text, dbc_text, csv_warnings);
  if (status) {
    status = decoder.loadDbcString(dbc_text);
  }
  if (!csv_warnings.empty()) {
    warnings += path + ": " + csv_warnings;
  }
  return status;
}

}  // namespace pj_can_dbc
