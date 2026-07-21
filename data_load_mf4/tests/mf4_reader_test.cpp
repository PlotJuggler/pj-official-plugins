#include "../mf4_reader.hpp"

#include <gtest/gtest.h>
#include <mdf/canconfigadapter.h>
#include <mdf/canmessage.h>
#include <mdf/ichannel.h>
#include <mdf/ichannelgroup.h>
#include <mdf/idatagroup.h>
#include <mdf/iheader.h>
#include <mdf/mdffactory.h>
#include <mdf/mdfwriter.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace {

using mf4_detail::Mf4Reader;
using mf4_detail::SampleValue;

constexpr std::uint64_t kStartNs = 1'700'000'000'000'000'000ULL;  // 2023-11-14, arbitrary
constexpr std::uint64_t kPeriodNs = 10'000'000ULL;                // 10 ms -> 100 Hz

std::string tempPath(const std::string& stem) {
  const auto p = std::filesystem::temp_directory_path() / ("mf4rtest_" + stem + ".mf4");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  return p.string();
}

mdf::IChannel* addChannel(
    mdf::IChannelGroup* cg, const std::string& name, mdf::ChannelDataType dtype, std::size_t bytes,
    const std::string& unit = "") {
  auto* ch = mdf::MdfWriter::CreateChannel(cg);
  ch->Name(name);
  ch->Type(mdf::ChannelType::FixedLength);
  ch->DataType(dtype);
  ch->DataBytes(bytes);
  if (!unit.empty()) {
    ch->Unit(unit);
  }
  return ch;
}

mdf::IChannel* addMaster(mdf::IChannelGroup* cg) {
  auto* ch = mdf::MdfWriter::CreateChannel(cg);
  ch->Name("Time");
  ch->Type(mdf::ChannelType::Master);
  ch->DataType(mdf::ChannelDataType::FloatLe);
  ch->DataBytes(8);
  ch->Unit("s");
  return ch;
}

// Writes one channel group (double master + caller-defined channels) with `n`
// samples spaced by kPeriodNs, then finalizes.
void writeSingleGroup(
    const std::string& path, std::size_t n,
    const std::function<std::vector<mdf::IChannel*>(mdf::IChannelGroup*)>& make_channels,
    const std::function<void(const std::vector<mdf::IChannel*>&, std::size_t)>& set_values) {
  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::Mdf4Basic);
  ASSERT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kStartNs);
  auto* dg = writer->CreateDataGroup();
  auto* cg = mdf::MdfWriter::CreateChannelGroup(dg);
  addMaster(cg);
  const auto channels = make_channels(cg);
  ASSERT_TRUE(writer->InitMeasurement());
  writer->StartMeasurement(kStartNs);
  for (std::size_t i = 0; i < n; ++i) {
    set_values(channels, i);
    writer->SaveSample(*cg, kStartNs + static_cast<std::uint64_t>(i) * kPeriodNs);
  }
  writer->StopMeasurement(kStartNs + static_cast<std::uint64_t>(n) * kPeriodNs);
  writer->FinalizeMeasurement();
}

struct Row {
  std::int64_t ts = 0;
  std::vector<SampleValue> values;
};

std::vector<Row> readAll(Mf4Reader& reader, std::size_t group_index) {
  std::vector<Row> rows;
  const auto status = reader.readGroup(group_index, [&](std::int64_t ts, const std::vector<SampleValue>& vals) {
    rows.push_back(Row{ts, vals});
    return true;
  });
  EXPECT_TRUE(status.has_value()) << (status ? "" : status.error());
  return rows;
}

TEST(Mf4Reader, SingleScalarGroup) {
  const std::string path = tempPath("single");
  writeSingleGroup(
      path, 5,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{addChannel(cg, "speed", mdf::ChannelDataType::FloatLe, 8, "m/s")};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) {
        ch[0]->SetChannelValue(static_cast<double>(i) * 2.0);
      });

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  ASSERT_EQ(reader.groups().size(), 1u);
  EXPECT_TRUE(reader.finalized());
  EXPECT_GT(reader.startTimeNs(), 0);

  const auto& g = reader.groups()[0];
  EXPECT_TRUE(g.has_master);
  EXPECT_EQ(g.channels.size(), 2u);  // master + speed
  EXPECT_EQ(reader.valueChannelNames(0), (std::vector<std::string>{"speed"}));

  const auto rows = readAll(reader, 0);
  ASSERT_EQ(rows.size(), 5u);
  // Absolute epoch: first sample sits exactly at the header start time.
  EXPECT_EQ(rows[0].ts, static_cast<std::int64_t>(kStartNs));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(rows[i].ts - rows[0].ts, static_cast<std::int64_t>(i) * static_cast<std::int64_t>(kPeriodNs));
    ASSERT_EQ(rows[i].values.size(), 1u);
    EXPECT_TRUE(rows[i].values[0].valid);
    EXPECT_DOUBLE_EQ(rows[i].values[0].number, static_cast<double>(i) * 2.0);
  }
}

TEST(Mf4Reader, StringChannel) {
  const std::string path = tempPath("string");
  writeSingleGroup(
      path, 3,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{addChannel(cg, "label", mdf::ChannelDataType::StringAscii, 16)};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) {
        ch[0]->SetChannelValue(std::string("v") + std::to_string(i));
      });

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  EXPECT_EQ(reader.valueChannelNames(0), (std::vector<std::string>{"label"}));

  const auto rows = readAll(reader, 0);
  ASSERT_EQ(rows.size(), 3u);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    ASSERT_EQ(rows[i].values.size(), 1u);
    EXPECT_EQ(rows[i].values[0].type, PJ::PrimitiveType::kString);
    EXPECT_EQ(rows[i].values[0].text, std::string("v") + std::to_string(i));
  }
}

TEST(Mf4Reader, DuplicateChannelNamesAreDisambiguated) {
  const std::string path = tempPath("dup");
  writeSingleGroup(
      path, 2,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{
            addChannel(cg, "temp", mdf::ChannelDataType::FloatLe, 8),
            addChannel(cg, "temp", mdf::ChannelDataType::FloatLe, 8)};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) {
        ch[0]->SetChannelValue(static_cast<double>(i));
        ch[1]->SetChannelValue(static_cast<double>(i) * 100.0);
      });

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  // Both "temp" channels are surfaced; the second is disambiguated.
  EXPECT_EQ(reader.valueChannelNames(0), (std::vector<std::string>{"temp", "temp#1"}));

  const auto rows = readAll(reader, 0);
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_EQ(rows[1].values.size(), 2u);
  EXPECT_DOUBLE_EQ(rows[1].values[0].number, 1.0);
  EXPECT_DOUBLE_EQ(rows[1].values[1].number, 100.0);
}

TEST(Mf4Reader, TwoGroupsHaveIndependentTimelines) {
  const std::string path = tempPath("twogroups");
  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::Mdf4Basic);
  ASSERT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kStartNs);

  auto* dg1 = writer->CreateDataGroup();
  auto* cg1 = mdf::MdfWriter::CreateChannelGroup(dg1);
  addMaster(cg1);
  auto* speed = addChannel(cg1, "speed", mdf::ChannelDataType::FloatLe, 8);

  auto* dg2 = writer->CreateDataGroup();
  auto* cg2 = mdf::MdfWriter::CreateChannelGroup(dg2);
  addMaster(cg2);
  auto* rpm = addChannel(cg2, "rpm", mdf::ChannelDataType::FloatLe, 8);

  ASSERT_TRUE(writer->InitMeasurement());
  writer->StartMeasurement(kStartNs);
  for (std::size_t i = 0; i < 3; ++i) {
    speed->SetChannelValue(static_cast<double>(i) + 10.0);
    writer->SaveSample(*cg1, kStartNs + static_cast<std::uint64_t>(i) * kPeriodNs);
  }
  for (std::size_t i = 0; i < 2; ++i) {
    rpm->SetChannelValue(static_cast<double>(i) + 100.0);
    writer->SaveSample(*cg2, kStartNs + static_cast<std::uint64_t>(i) * kPeriodNs);
  }
  writer->StopMeasurement(kStartNs + 3 * kPeriodNs);
  writer->FinalizeMeasurement();

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  ASSERT_EQ(reader.groups().size(), 2u);

  const auto find = [&](const std::string& field) -> std::size_t {
    for (std::size_t gi = 0; gi < reader.groups().size(); ++gi) {
      const auto names = reader.valueChannelNames(gi);
      if (!names.empty() && names[0] == field) {
        return gi;
      }
    }
    ADD_FAILURE() << "group with field " << field << " not found";
    return 0;
  };

  const auto speed_rows = readAll(reader, find("speed"));
  const auto rpm_rows = readAll(reader, find("rpm"));
  ASSERT_EQ(speed_rows.size(), 3u);
  ASSERT_EQ(rpm_rows.size(), 2u);
  EXPECT_DOUBLE_EQ(speed_rows[0].values[0].number, 10.0);
  EXPECT_DOUBLE_EQ(rpm_rows[0].values[0].number, 100.0);
}

TEST(Mf4Reader, GroupWithoutMasterIsRejected) {
  const std::string path = tempPath("nomaster");
  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::Mdf4Basic);
  ASSERT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kStartNs);
  auto* dg = writer->CreateDataGroup();
  auto* cg = mdf::MdfWriter::CreateChannelGroup(dg);
  auto* a = addChannel(cg, "a", mdf::ChannelDataType::FloatLe, 8);
  auto* b = addChannel(cg, "b", mdf::ChannelDataType::FloatLe, 8);
  ASSERT_TRUE(writer->InitMeasurement());
  writer->StartMeasurement(kStartNs);
  for (std::size_t i = 0; i < 2; ++i) {
    a->SetChannelValue(static_cast<double>(i));
    b->SetChannelValue(static_cast<double>(i));
    writer->SaveSample(*cg, kStartNs + static_cast<std::uint64_t>(i) * kPeriodNs);
  }
  writer->StopMeasurement(kStartNs + 2 * kPeriodNs);
  writer->FinalizeMeasurement();

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  ASSERT_EQ(reader.groups().size(), 1u);
  EXPECT_FALSE(reader.groups()[0].has_master);
  // readGroup must reject a masterless group rather than emit misaligned rows.
  const auto status = reader.readGroup(0, [](std::int64_t, const std::vector<SampleValue>&) { return true; });
  EXPECT_FALSE(status.has_value());
}

TEST(Mf4Reader, UnsupportedChannelIsSkippedWithoutCrash) {
  const std::string path = tempPath("unsupported");
  writeSingleGroup(
      path, 2,
      [](mdf::IChannelGroup* cg) {
        auto* ok = addChannel(cg, "ok", mdf::ChannelDataType::FloatLe, 8);
        // Complex is unsupported in v1; mdflib returns a null observer for it,
        // which the reader must skip (not dereference).
        addChannel(cg, "cplx", mdf::ChannelDataType::ComplexLe, 16);
        return std::vector<mdf::IChannel*>{ok};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) {
        ch[0]->SetChannelValue(static_cast<double>(i) + 7.0);
      });

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  ASSERT_EQ(reader.groups().size(), 1u);
  EXPECT_EQ(reader.groups()[0].channels.size(), 3u);  // master + ok + cplx
  EXPECT_EQ(reader.valueChannelNames(0), (std::vector<std::string>{"ok"}));

  const auto rows = readAll(reader, 0);  // must not crash on the null observer
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_EQ(rows[0].values.size(), 1u);
  EXPECT_DOUBLE_EQ(rows[1].values[0].number, 8.0);
}

// A truncated sample-data block must never fabricate rows. mdflib swallows a
// short read (ReadData still returns true), leaving the missing tail invalid —
// the reader must skip those samples and count them, not emit the
// header-declared row count as nulls all stamped at the file start time.
TEST(Mf4Reader, TruncatedDataBlockSkipsInvalidRowsInsteadOfFabricating) {
  const std::string path = tempPath("truncated");
  writeSingleGroup(
      path, 512,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{addChannel(cg, "speed", mdf::ChannelDataType::FloatLe, 8)};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) { ch[0]->SetChannelValue(static_cast<double>(i)); });

  // Chop into the sample data; the metadata at the file front stays intact.
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  ASSERT_FALSE(ec);
  std::filesystem::resize_file(path, size / 2, ec);
  ASSERT_FALSE(ec);

  Mf4Reader reader;
  const auto open_status = reader.open(path);
  if (!open_status.has_value()) {
    GTEST_SKIP() << "metadata clipped by truncation on this mdflib layout: " << open_status.error();
  }
  std::size_t rows = 0;
  mf4_detail::ReadGroupStats stats;
  const auto status = reader.readGroup(
      0,
      [&](std::int64_t, const std::vector<SampleValue>&) {
        ++rows;
        return true;
      },
      &stats);
  if (!status.has_value()) {
    // Also acceptable: mdflib surfaced the short read as a hard error.
    EXPECT_EQ(rows, 0u);
    return;
  }
  EXPECT_LT(rows, 512u) << "rows missing from the file must not be fabricated";
  EXPECT_GT(stats.skipped_invalid_time, 0u) << "skipped samples must be counted";
  EXPECT_EQ(rows + stats.skipped_invalid_time, 512u);
}

// A hostile header-declared sample count must be rejected before mdflib
// pre-sizes observer buffers from it (NofSamples x channels x element size —
// an allocation DoS from a tiny file). Poke cg_cycle_count to 2^60.
TEST(Mf4Reader, ImplausibleSampleCountIsRejected) {
  const std::string path = tempPath("hugecount");
  writeSingleGroup(
      path, 5,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{addChannel(cg, "speed", mdf::ChannelDataType::FloatLe, 8)};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) { ch[0]->SetChannelValue(static_cast<double>(i)); });

  // Locate the CG block and overwrite cg_cycle_count (header 24 bytes +
  // link_count links + cg_record_id) with an absurd value.
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.is_open());
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::string marker = "##CG";
  const auto it = std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end());
  ASSERT_NE(it, bytes.end());
  const auto cg_pos = static_cast<std::streamoff>(std::distance(bytes.begin(), it));
  std::uint64_t link_count = 0;
  std::memcpy(&link_count, bytes.data() + cg_pos + 16, sizeof(link_count));
  const std::streamoff cycle_pos = cg_pos + 24 + static_cast<std::streamoff>(8 * link_count) + 8;
  const std::uint64_t huge = 1ULL << 60;
  file.seekp(cycle_pos);
  file.write(reinterpret_cast<const char*>(&huge), sizeof(huge));
  file.close();

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  const auto status = reader.readGroup(0, [](std::int64_t, const std::vector<SampleValue>&) { return true; });
  EXPECT_FALSE(status.has_value()) << "implausible sample count must fail cleanly, not allocate";
}

// Cancel must be able to interrupt a group mid-read: a single huge channel
// group would otherwise import to completion after the user hits Cancel.
TEST(Mf4Reader, ReadGroupStopsEarlyWhenCallbackReturnsFalse) {
  const std::string path = tempPath("earlystop");
  writeSingleGroup(
      path, 100,
      [](mdf::IChannelGroup* cg) {
        return std::vector<mdf::IChannel*>{addChannel(cg, "speed", mdf::ChannelDataType::FloatLe, 8)};
      },
      [](const std::vector<mdf::IChannel*>& ch, std::size_t i) { ch[0]->SetChannelValue(static_cast<double>(i)); });

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  std::size_t rows = 0;
  const auto status = reader.readGroup(0, [&](std::int64_t, const std::vector<SampleValue>&) {
    ++rows;
    return rows < 10;  // stop after the 10th row
  });
  EXPECT_TRUE(status.has_value()) << (status ? "" : status.error());
  EXPECT_EQ(rows, 10u);
}

// Writes a CAN bus-log MF4 with frames on two bus channels into one
// CAN_DataFrame channel group. Returns the file path.
std::string writeCanBusFile(const std::string& stem) {
  const auto p = std::filesystem::temp_directory_path() / ("mf4rtest_" + stem + ".mf4");
  std::error_code ec;
  std::filesystem::remove(p, ec);
  const std::string path = p.string();

  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::MdfBusLogger);
  EXPECT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kStartNs);
  writer->BusType(mdf::MdfBusType::CAN);
  writer->StorageType(mdf::MdfStorageType::FixedLengthStorage);
  writer->MaxLength(8);
  mdf::CanConfigAdapter config(*writer);
  auto* dg = writer->CreateDataGroup();
  EXPECT_NE(dg, nullptr);
  config.CreateConfig(*dg);
  writer->PreTrigTime(0.0);
  writer->CompressData(false);
  auto* data_frame_cg = dg->GetChannelGroup("CAN_DataFrame");
  EXPECT_NE(data_frame_cg, nullptr);
  EXPECT_TRUE(writer->InitMeasurement());
  writer->StartMeasurement(kStartNs);
  for (std::uint64_t i = 0; i < 3; ++i) {
    mdf::CanMessage msg;
    msg.MessageId(0x100);
    msg.BusChannel(1);
    msg.DataBytes({1, 2, 3, 4, 5, 6, 7, 8});
    writer->SaveCanMessage(*data_frame_cg, kStartNs + i * kPeriodNs, msg);
  }
  for (std::uint64_t i = 0; i < 2; ++i) {
    mdf::CanMessage msg;
    msg.MessageId(0x18FEF100);
    msg.ExtendedId(true);
    msg.BusChannel(2);
    msg.DataBytes({9, 9, 9, 9, 9, 9, 9, 9});
    writer->SaveCanMessage(*data_frame_cg, kStartNs + (3 + i) * kPeriodNs, msg);
  }
  writer->StopMeasurement(kStartNs + 6 * kPeriodNs);
  writer->FinalizeMeasurement();
  return path;
}

// Frames from different physical buses must be distinguishable downstream:
// the reader surfaces CanMessage::BusChannel() with every frame, otherwise a
// dual-bus log merges same-id traffic into one series.
TEST(Mf4Reader, CanGroupFramesCarryBusChannel) {
  const std::string path = writeCanBusFile("canbus");

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  constexpr int kCanBusType = 2;  // mdf::BusType::CAN
  std::size_t can_gi = std::numeric_limits<std::size_t>::max();
  for (std::size_t gi = 0; gi < reader.groups().size(); ++gi) {
    if (reader.groups()[gi].bus_type == kCanBusType && reader.groups()[gi].sample_count > 0) {
      can_gi = gi;
      break;
    }
  }
  ASSERT_NE(can_gi, std::numeric_limits<std::size_t>::max());

  struct Frame {
    std::uint16_t channel;
    std::uint32_t id;
    bool extended;
  };
  std::vector<Frame> frames;
  const auto status = reader.readCanGroup(
      can_gi, [&](std::int64_t /*ts*/, std::uint16_t bus_channel, std::uint32_t id, bool extended,
                  const std::vector<std::uint8_t>& /*data*/) {
        frames.push_back({bus_channel, id, extended});
        return true;
      });
  ASSERT_TRUE(status.has_value()) << (status ? "" : status.error());
  ASSERT_EQ(frames.size(), 5u);
  EXPECT_EQ(frames[0].channel, 1u);
  EXPECT_EQ(frames[0].id, 0x100u);
  EXPECT_FALSE(frames[0].extended);
  EXPECT_EQ(frames[3].channel, 2u);
  EXPECT_EQ(frames[3].id, 0x18FEF100u);
  EXPECT_TRUE(frames[3].extended);
}

// Same early-stop contract for CAN groups: a CANedge log is typically one huge
// data group, so cancel must work inside it, not just between groups.
TEST(Mf4Reader, ReadCanGroupStopsEarlyWhenCallbackReturnsFalse) {
  const std::string path = writeCanBusFile("canstop");

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  constexpr int kCanBusType = 2;
  std::size_t can_gi = std::numeric_limits<std::size_t>::max();
  for (std::size_t gi = 0; gi < reader.groups().size(); ++gi) {
    if (reader.groups()[gi].bus_type == kCanBusType && reader.groups()[gi].sample_count > 0) {
      can_gi = gi;
      break;
    }
  }
  ASSERT_NE(can_gi, std::numeric_limits<std::size_t>::max());

  std::size_t frames = 0;
  const auto status = reader.readCanGroup(
      can_gi, [&](std::int64_t, std::uint16_t, std::uint32_t, bool, const std::vector<std::uint8_t>&) {
        ++frames;
        return frames < 2;  // stop after the 2nd frame
      });
  EXPECT_TRUE(status.has_value()) << (status ? "" : status.error());
  EXPECT_EQ(frames, 2u);
}

TEST(Mf4Reader, ReadsMdf3File) {
  // Older MDF v3 (.mf3) via the same mdflib reader API. mdflib supports v3;
  // this guards that the version-agnostic Mf4Reader path handles it too.
  const auto p = std::filesystem::temp_directory_path() / "mf4rtest_v3.mf3";
  std::error_code ec;
  std::filesystem::remove(p, ec);
  const std::string path = p.string();

  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::Mdf3Basic);
  ASSERT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kStartNs);
  auto* dg = writer->CreateDataGroup();
  auto* cg = mdf::MdfWriter::CreateChannelGroup(dg);
  addMaster(cg);
  auto* speed = addChannel(cg, "speed", mdf::ChannelDataType::FloatLe, 8, "m/s");
  ASSERT_TRUE(writer->InitMeasurement());
  writer->StartMeasurement(kStartNs);
  for (std::size_t i = 0; i < 5; ++i) {
    speed->SetChannelValue(static_cast<double>(i) * 3.0);
    writer->SaveSample(*cg, kStartNs + static_cast<std::uint64_t>(i) * kPeriodNs);
  }
  writer->StopMeasurement(kStartNs + 5 * kPeriodNs);
  writer->FinalizeMeasurement();

  Mf4Reader reader;
  ASSERT_TRUE(reader.open(path).has_value());
  ASSERT_EQ(reader.groups().size(), 1u);
  EXPECT_EQ(reader.valueChannelNames(0), (std::vector<std::string>{"speed"}));
  const auto rows = readAll(reader, 0);
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_DOUBLE_EQ(rows[2].values[0].number, 6.0);
  EXPECT_EQ(rows[4].ts - rows[0].ts, 4 * static_cast<std::int64_t>(kPeriodNs));
}

}  // namespace
