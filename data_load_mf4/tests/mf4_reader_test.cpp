#include "../mf4_reader.hpp"

#include <gtest/gtest.h>
#include <mdf/ichannel.h>
#include <mdf/ichannelgroup.h>
#include <mdf/idatagroup.h>
#include <mdf/iheader.h>
#include <mdf/mdffactory.h>
#include <mdf/mdfwriter.h>

#include <cstdint>
#include <filesystem>
#include <functional>
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
  const auto status = reader.readGroup(
      group_index, [&](std::int64_t ts, const std::vector<SampleValue>& vals) { rows.push_back(Row{ts, vals}); });
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
  const auto status = reader.readGroup(0, [](std::int64_t, const std::vector<SampleValue>&) {});
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

}  // namespace
