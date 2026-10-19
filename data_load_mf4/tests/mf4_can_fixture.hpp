#pragma once

// Shared MF4 fixture writer: tests/mf4_reader_test.cpp reads the file back,
// tests/mf4_layout_paths_test.cpp imports it through the built plugin.

#include <gtest/gtest.h>
#include <mdf/canconfigadapter.h>
#include <mdf/canmessage.h>
#include <mdf/ichannelgroup.h>
#include <mdf/idatagroup.h>
#include <mdf/iheader.h>
#include <mdf/mdffactory.h>
#include <mdf/mdfwriter.h>

#include <cstdint>
#include <string>

namespace mf4_test {

inline constexpr std::uint64_t kFixtureStartNs = 1'700'000'000'000'000'000ULL;  // 2023-11-14, arbitrary
inline constexpr std::uint64_t kFixturePeriodNs = 10'000'000ULL;                // 10 ms -> 100 Hz

/// Writes a CAN bus-log MF4 with frames on two bus channels into one
/// CAN_DataFrame channel group: 3 frames of id 0x100 on bus 1, 2 frames of
/// extended id 0x18FEF100 on bus 2.
inline void writeCanBusMf4(const std::string& path) {
  auto writer = mdf::MdfFactory::CreateMdfWriter(mdf::MdfWriterType::MdfBusLogger);
  EXPECT_TRUE(writer->Init(path));
  writer->Header()->StartTime(kFixtureStartNs);
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
  writer->StartMeasurement(kFixtureStartNs);
  for (std::uint64_t i = 0; i < 3; ++i) {
    mdf::CanMessage msg;
    msg.MessageId(0x100);
    msg.BusChannel(1);
    msg.DataBytes({1, 2, 3, 4, 5, 6, 7, 8});
    writer->SaveCanMessage(*data_frame_cg, kFixtureStartNs + i * kFixturePeriodNs, msg);
  }
  for (std::uint64_t i = 0; i < 2; ++i) {
    mdf::CanMessage msg;
    msg.MessageId(0x18FEF100);
    msg.ExtendedId(true);
    msg.BusChannel(2);
    msg.DataBytes({9, 9, 9, 9, 9, 9, 9, 9});
    writer->SaveCanMessage(*data_frame_cg, kFixtureStartNs + (3 + i) * kFixturePeriodNs, msg);
  }
  writer->StopMeasurement(kFixtureStartNs + 6 * kFixturePeriodNs);
  writer->FinalizeMeasurement();
}

}  // namespace mf4_test
