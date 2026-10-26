// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

#include "session_text.hpp"  // kFallbackFeaturesHeader

namespace assistant_agent {

// The "PlotJuggler features" block used when the host has no
// pj.feature_catalog.v1 service (a host older than SDK 0.39.0) or its
// document cannot be used (featuresDigest, features_digest.hpp). Without it
// the model answers product questions from its PlotJuggler 3 training.
//
// Two rules shape the text. It never says what is installed: this build
// cannot know, so every plugin line is conditional. And it lists only routes
// the PlotJuggler 4.1 code confirms (docs/benchmarks/features/fixtures/
// README.md); the drag-and-drop gestures still to be checked in the app are
// left out. Labels are the live UI strings: menu actions end in three ASCII
// dots, and only "Extensions Marketplace…" uses the ellipsis character.
//
// No blank line anywhere inside: stripCatalogPrefix (session_text.hpp) finds
// the end of the block by the first blank line.
inline constexpr std::string_view kFallbackFeaturesBody =
    "Built in:\n"
    "- Open a data file: File → Open Data File... (or Sources panel → File → Load a data file). Which formats open "
    "depends on the installed data loaders.\n"
    "- Live streaming: Sources panel → Stream → choose the source in the list → + (Start streaming).\n"
    "- Record streams to MCAP: while streaming, the record button in Sources panel → Stream saves the subscribed "
    "topics to MCAP files; the folder is set in File → Preferences... → Recording.\n"
    "- Cloud sources: Sources panel → Cloud → choose the service in the list → + (Open cloud browser); needs a "
    "cloud extension.\n"
    "- Layouts: File → Save Layout... / Load Layout... save the workspace to a file and load it again.\n"
    "- Extensions Marketplace: File → Extensions Marketplace… browses, installs, updates, enables or disables "
    "extensions; enabling or disabling takes effect after a restart. Installed list: Help → Installed Extensions.\n"
    "- Datasets panel: lists the loaded datasets, topics and series. Filter... filters by name; right-click a "
    "dataset for Info, Merge, Reload, Replace or Remove.\n"
    "- Custom Topics: Custom Topics panel → + (Add custom topic) opens the Transform Editor (Luau or Python "
    "functions).\n"
    "- 2D view (camera images and video): Empty plot area: click the 2D button (image icon).\n"
    "- 3D view (point clouds, meshes, robot models, transforms, markers): Empty plot area: click the 3D button (cube "
    "icon).\n"
    "- State Transitions (discrete states over time): Empty plot area: click the State Transitions button.\n"
    "- Playback: Timeline bar → Start/Stop player (Space), Loop, Speed, Step size.\n"
    "- Source timeline: Toggle bottom panel → Align sources, Merge the selected datasets, Reset all timeline "
    "changes.\n"
    "- Splits: plot toolbar (shown when you hover the top edge of a plot) → Split horizontally / Split vertically.\n"
    "- Plot filters: Right-click a plot → Apply Filter...\n"
    "- Plot markers: Toggle right panel → Curves → Dataset markers / Global markers.\n"
    "- Preferences: File → Preferences... (Appearance, Plotting, Scene 2D, Scene 3D, Plugins, Recording).\n"
    "- Diagnostics: Title bar bell icon (Diagnostics).\n"
    "Known limitations:\n"
    "- PlotJuggler 4 only receives and displays data; it cannot publish messages or send commands back to a robot "
    "or device.\n"
    "- Loaded data cannot be edited: individual samples cannot be changed or deleted. Only whole datasets can be "
    "removed (Datasets panel → right-click a dataset → Remove).\n"
    "- PlotJuggler cannot record a video of its window or screen. Its only recording feature saves live streams to "
    "MCAP files.\n"
    "- There is no option to save a plot or view as an image file; Copy in a plot's right-click menu copies the "
    "plot configuration, not a picture.\n"
    "- There is no Excel (.xlsx) export. The CSV/Parquet Exporter extension writes CSV or Parquet files, which "
    "spreadsheet programs can open.\n"
    "Key plugins (this reference cannot tell which are installed):\n"
    "- MQTT Subscriber: if installed, it appears under Sources panel → Stream; otherwise install it from File → "
    "Extensions Marketplace….\n"
    "- ROS 2 Topic Subscriber (live ROS 2 topics): if installed, it appears under Sources panel → Stream; otherwise "
    "install it from File → Extensions Marketplace….\n"
    "- MCAP Loader (MCAP files and ROS 2 bags), CSV Loader, ULog Loader (PX4 logs): if installed, they open through "
    "File → Open Data File...; otherwise install them from File → Extensions Marketplace….\n"
    "- MP4 Loader (video next to the plots, shown in a 2D view): if installed, it opens .mp4 files through File → "
    "Open Data File...; otherwise install it from File → Extensions Marketplace….\n"
    "- Fast Fourier Transform (frequency spectrum), Quaternion to RPY (roll, pitch, yaw), CSV/Parquet Exporter: if "
    "installed, each appears in the Toolbox menu under that name; otherwise install it from File → Extensions "
    "Marketplace….\n"
    "- Transform Editor (new series and 2D/3D objects computed from others with Luau or Python): if installed, it "
    "opens from Custom Topics panel → + (Add custom topic); otherwise install it from File → Extensions Marketplace….\n"
    "Plugins are installed from File → Extensions Marketplace…; a plugin not listed here may still be distributed "
    "outside the Marketplace.";

// The fallback block, headed like a host listing so the model reads one
// format. `note` (one line, optional) says why the host's own listing was not
// used when it exists but could not be read.
[[nodiscard]] inline std::string fallbackFeatures(std::string_view note = {}) {
  std::string out(kFallbackFeaturesHeader);
  out += '\n';
  out += "Reference built into this assistant for PlotJuggler 4.1: it may be incomplete or differ from your build.\n";
  if (!note.empty()) {
    out += note;
    out += '\n';
  }
  out += kFallbackFeaturesBody;
  return out;
}

}  // namespace assistant_agent
