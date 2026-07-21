#include "mf4_reader.hpp"

#include <mdf/canbusobserver.h>
#include <mdf/canmessage.h>
#include <mdf/ichannel.h>
#include <mdf/ichannelgroup.h>
#include <mdf/idatagroup.h>
#include <mdf/mdffile.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace mf4_detail {

namespace {

bool isMaster(mdf::ChannelType type) {
  return type == mdf::ChannelType::Master || type == mdf::ChannelType::VirtualMaster;
}

/// Thrown from the CAN observer callback to abort mdflib's ReadData loop on
/// cancel (std::exception so mdflib's internal catch handles it cleanly).
struct ReadStopRequested : std::exception {
  const char* what() const noexcept override {
    return "mf4: read stopped by consumer";
  }
};

}  // namespace

PJ::Status Mf4Reader::open(const std::string& path) {
  std::error_code ec;
  const auto fs_size = std::filesystem::file_size(path, ec);
  file_size_bytes_ = ec ? 0 : static_cast<std::uint64_t>(fs_size);

  reader_ = std::make_unique<mdf::MdfReader>(path);
  if (!reader_->IsOk()) {
    return PJ::unexpected(std::string("cannot open MDF file: ") + path);
  }
  if (!reader_->ReadEverythingButData()) {
    return PJ::unexpected(std::string("failed to read MDF metadata: ") + path);
  }
  finalized_ = reader_->IsFinalized();
  // GetStartTime() is an unsigned ns epoch; a corrupt header near UINT64_MAX
  // would become a negative start after the cast and then skew (or overflow)
  // every derived timestamp. Clamp implausible values to 0 (Unix epoch).
  const std::uint64_t start_time = reader_->GetStartTime();
  start_time_ns_ = start_time <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                       ? static_cast<std::int64_t>(start_time)
                       : 0;

  const mdf::MdfFile* file = reader_->GetFile();
  if (file == nullptr) {
    return PJ::unexpected(std::string("MDF file has no content: ") + path);
  }

  mdf::DataGroupList data_groups;
  file->DataGroups(data_groups);
  std::size_t dg_index = 0;
  for (auto* dg : data_groups) {
    for (auto* cg : dg->ChannelGroups()) {
      GroupInfo group;
      group.dg_index = dg_index;
      group.name = cg->Name();
      group.sample_count = cg->NofSamples();
      group.bus_type = static_cast<int>(cg->GetBusType());
      for (auto* ch : cg->Channels()) {
        ChannelInfo info;
        info.name = ch->Name();
        info.unit = ch->Unit();
        info.type = mf4TypeToPrimitive(ch->DataType());
        info.is_master = isMaster(ch->Type());
        if (info.is_master) {
          group.has_master = true;
        }
        group.channels.push_back(std::move(info));
      }
      data_groups_.push_back(dg);
      channel_groups_.push_back(cg);
      groups_.push_back(std::move(group));
    }
    ++dg_index;
  }
  return PJ::okStatus();
}

PJ::Status Mf4Reader::checkSampleCount(std::size_t group_index) const {
  // mdflib pre-sizes observer buffers to NofSamples x channels before reading,
  // so a hostile header count (e.g. 2^60 in a tiny file) is an allocation DoS.
  // A ##DZ-compressed group legitimately holds far more logical samples than
  // the file has bytes, so only reject counts beyond a very generous
  // samples-per-byte ceiling (well above any real compression ratio). The
  // try/catch around observer creation is the backstop for what slips through
  // (e.g. a forged channel array size).
  constexpr std::uint64_t kMaxSamplesPerFileByte = 1'000'000;
  const std::uint64_t count = groups_[group_index].sample_count;
  if (file_size_bytes_ > 0 && count / kMaxSamplesPerFileByte > file_size_bytes_) {
    return PJ::unexpected(
        std::string("mf4: channel group declares ") + std::to_string(count) + " samples in a file of " +
        std::to_string(file_size_bytes_) + " bytes — corrupt file?");
  }
  return PJ::okStatus();
}

std::vector<std::string> Mf4Reader::valueChannelNames(std::size_t group_index) const {
  std::vector<std::string> names;
  if (group_index >= groups_.size()) {
    return names;
  }
  std::unordered_set<std::string> used;
  for (const auto& ch : groups_[group_index].channels) {
    if (ch.is_master || ch.type == PJ::PrimitiveType::kUnspecified) {
      continue;
    }
    std::string name = ch.name.empty() ? (std::string("chan") + std::to_string(names.size())) : ch.name;
    if (used.count(name) != 0) {
      const std::string base = name;
      int suffix = 1;
      do {
        name = base + "#" + std::to_string(suffix);
        ++suffix;
      } while (used.count(name) != 0);
    }
    used.insert(name);
    names.push_back(std::move(name));
  }
  return names;
}

PJ::Status Mf4Reader::readGroup(std::size_t group_index, const RowCallback& cb, ReadGroupStats* stats) {
  if (group_index >= groups_.size()) {
    return PJ::unexpected(std::string("mf4: group index out of range"));
  }
  if (auto status = checkSampleCount(group_index); !status) {
    return status;
  }
  auto* dg = data_groups_[group_index];
  auto* cg = channel_groups_[group_index];

  // Observer creation sizes buffers to NofSamples x channel-array-size; a
  // forged array dimension can exhaust memory. Fail cleanly instead of letting
  // bad_alloc/length_error cross the plugin boundary.
  mdf::ChannelObserverList observers;
  try {
    mdf::CreateChannelObserverForChannelGroup(*dg, *cg, observers);
  } catch (const std::exception& e) {
    dg->ClearData();
    return PJ::unexpected(std::string("mf4: cannot allocate channel observers (corrupt file?): ") + e.what());
  }
  if (!reader_->ReadData(*dg)) {
    dg->ClearData();
    return PJ::unexpected(std::string("mf4: failed to read sample data (truncated or corrupt file?)"));
  }

  mdf::IChannelObserver* master = nullptr;
  std::vector<mdf::IChannelObserver*> value_obs;
  std::vector<PJ::PrimitiveType> value_types;
  for (auto& obs : observers) {
    // mdflib returns a null observer for channel data types it cannot decode
    // (e.g. Complex). Those map to kUnspecified and are skipped by
    // valueChannelNames() too, so skipping them here keeps `value_obs` aligned
    // with the field-name list (and avoids a null dereference).
    if (obs == nullptr) {
      continue;
    }
    const mdf::IChannel& ch = obs->Channel();
    if (isMaster(ch.Type())) {
      if (master == nullptr) {
        master = obs.get();
      }
      continue;
    }
    const PJ::PrimitiveType ptype = mf4TypeToPrimitive(ch.DataType());
    if (ptype == PJ::PrimitiveType::kUnspecified) {
      continue;
    }
    value_obs.push_back(obs.get());
    value_types.push_back(ptype);
  }

  if (master == nullptr) {
    dg->ClearData();
    return PJ::unexpected(std::string("mf4: channel group has no master channel"));
  }

  // Absolute time = file HD start + the master channel's relative seconds. This
  // is correct for single-measurement files (the common case: ASAP2, CANedge).
  // KNOWN v1 LIMITATION: for *appended* files with multiple measurements at
  // distinct start times, mdflib rewrites each measurement's master relative to
  // its own StartMeasurement origin while GetStartTime() reflects only the first
  // measurement, so later data groups would be anchored to the first epoch.
  // mdflib exposes no per-DG epoch to correct this; revisit if such files appear.
  const std::uint64_t n = cg->NofSamples();
  std::vector<SampleValue> row(value_obs.size());
  for (std::uint64_t i = 0; i < n; ++i) {
    double t_sec = 0.0;
    const bool t_ok = master->GetEngValue(i, t_sec);
    const auto ts = t_ok ? relativeSecondsToNs(start_time_ns_, t_sec) : std::nullopt;
    if (!ts.has_value()) {
      // No usable timestamp (invalidation bit, a truncated file's missing
      // tail, or a NaN/out-of-range master value) — skip rather than
      // fabricate a sample at the file start time.
      if (stats != nullptr) {
        ++stats->skipped_invalid_time;
      }
      continue;
    }
    const std::int64_t ts_ns = *ts;

    for (std::size_t k = 0; k < value_obs.size(); ++k) {
      row[k].type = value_types[k];
      if (value_types[k] == PJ::PrimitiveType::kString) {
        std::string text;
        row[k].valid = value_obs[k]->GetChannelValue(i, text);
        row[k].text = std::move(text);
      } else {
        double value = 0.0;
        row[k].valid = value_obs[k]->GetEngValue(i, value);
        row[k].number = value;
      }
    }
    if (!cb(ts_ns, row)) {
      break;  // consumer asked to stop (cancel)
    }
  }

  dg->ClearData();
  return PJ::okStatus();
}

PJ::Status Mf4Reader::readCanGroup(std::size_t group_index, const CanFrameCallback& cb, ReadGroupStats* stats) {
  if (group_index >= groups_.size()) {
    return PJ::unexpected(std::string("mf4: group index out of range"));
  }
  if (auto status = checkSampleCount(group_index); !status) {
    return status;
  }
  auto* dg = data_groups_[group_index];
  auto* cg = channel_groups_[group_index];

  std::unique_ptr<mdf::CanBusObserver> observer;
  try {
    observer = std::make_unique<mdf::CanBusObserver>(*dg, *cg);
  } catch (const std::exception& e) {
    return PJ::unexpected(std::string("mf4: cannot allocate CAN observer (corrupt file?): ") + e.what());
  }
  bool stopped = false;
  observer->OnCanMessage = [&](std::uint64_t /*sample*/, const mdf::CanMessage& msg) -> bool {
    const auto ts = relativeSecondsToNs(start_time_ns_, msg.Timestamp());
    if (!ts.has_value()) {
      if (stats != nullptr) {
        ++stats->skipped_invalid_time;
      }
      return true;  // corrupt frame timestamp — skip the frame
    }
    const std::int64_t ts_ns = *ts;
    if (!cb(ts_ns, static_cast<std::uint16_t>(msg.BusChannel()), msg.CanId(), msg.ExtendedId(), msg.DataBytes())) {
      // mdflib's CanBusObserver discards this callback's return value, so a
      // plain `return false` cannot abort ReadData. Throw instead: mdflib
      // catches it inside ReadData (which then returns false) and `stopped`
      // tells us it was a cancel, not a corrupt file.
      stopped = true;
      throw ReadStopRequested{};
    }
    return true;
  };
  const bool ok = reader_->ReadData(*dg);
  dg->ClearData();
  if (!ok && !stopped) {
    return PJ::unexpected(std::string("mf4: failed to read CAN data (truncated or corrupt file?)"));
  }
  return PJ::okStatus();
}

}  // namespace mf4_detail
