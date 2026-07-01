#include "mf4_reader.hpp"

#include <mdf/ichannel.h>
#include <mdf/ichannelgroup.h>
#include <mdf/idatagroup.h>
#include <mdf/mdffile.h>

#include <cmath>
#include <unordered_set>
#include <utility>

namespace mf4_detail {

namespace {

bool isMaster(mdf::ChannelType type) {
  return type == mdf::ChannelType::Master || type == mdf::ChannelType::VirtualMaster;
}

}  // namespace

PJ::Status Mf4Reader::open(const std::string& path) {
  reader_ = std::make_unique<mdf::MdfReader>(path);
  if (!reader_->IsOk()) {
    return PJ::unexpected(std::string("cannot open MDF file: ") + path);
  }
  if (!reader_->ReadEverythingButData()) {
    return PJ::unexpected(std::string("failed to read MDF metadata: ") + path);
  }
  finalized_ = reader_->IsFinalized();
  start_time_ns_ = static_cast<std::int64_t>(reader_->GetStartTime());

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

PJ::Status Mf4Reader::readGroup(std::size_t group_index, const RowCallback& cb) {
  if (group_index >= groups_.size()) {
    return PJ::unexpected(std::string("mf4: group index out of range"));
  }
  auto* dg = data_groups_[group_index];
  auto* cg = channel_groups_[group_index];

  mdf::ChannelObserverList observers;
  mdf::CreateChannelObserverForChannelGroup(*dg, *cg, observers);
  reader_->ReadData(*dg);

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
    const std::int64_t ts_ns = start_time_ns_ + (t_ok ? static_cast<std::int64_t>(std::llround(t_sec * 1.0e9)) : 0);

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
    cb(ts_ns, row);
  }

  dg->ClearData();
  return PJ::okStatus();
}

}  // namespace mf4_detail
