# Changelog — data_stream_ros2

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_ros2/`.

## Upcoming

### Changed
- `tf2_msgs/msg/TFMessage` topics are always subscribed: their rows are selected, locked and tooltipped, Deselect All, a filter or a saved selection never drops them, and they are saved with the selection, so a PlotJuggler without demand subscriptions subscribes them too.
- A Select All button (Ctrl+A, follows the filter) and a visible Deselect All button (Ctrl+Shift+A) sit next to the filter in a "Topics" header, like the other streamers.
- The dialog's title is the plugin's name, "ROS 2 Topic Subscriber" (was "Select ROS messages"), as in the source picker.
- The topic table's second column is titled "Type" (was "Datatype"), like the other streamers.
- The parser options read like the Foxglove Bridge dialog's: "Use the timestamp in the message (if present)" (the tooltip names `header.stamp`), then "When an array size exceeds:" with "Clamp" and "Skip" (were "clamp" and "discard"). The saved configuration is unchanged.
- OK and Cancel sit in the footer the other streamers use, and the dialog opens at a similar size (460x560, was 905x641) that can be resized larger.
- The empty topic table shows the hint "Click to select, Click+Drag to multi-select", as the PlotJuggler Bridge does.

## [1.2.1] - 2026-11-11

### Fixed
- RoboStack: with several conda environments on one machine, each loads its own
  distro's payload. PlotJuggler copies bundled plugins into the per-user
  extensions folder, which the environments share; when the running environment
  ships this extension (`<env>/lib/plotjuggler/plugins/ros2-topic-subscriber/`),
  the payload is now loaded from there instead of from that copy.

### Changed
- Emphasised text in the dialog uses DemiBold (600) instead of Bold, matching the app.

## [1.2.0] - 2026-11-11

### Added
- RoboStack payload for lyrical (dist/lyrical-robostack/), next to jazzy and kilted.
- macOS: the proxy detects a RoboStack environment (the executable's environment
  holding `librclcpp.dylib`) and loads its payload; `robostack/build.sh` builds
  macos-arm64 payloads. The marketplace zip is still Linux-only.

### Fixed
- RoboStack: the distro is detected without an activated environment
  (`ROS_DISTRO` unset) also where RoboStack ships `ros2-rclcpp` instead of
  `ros-<distro>-rclcpp` (lyrical; jazzy and kilted on macOS).

## [1.1.3] - 2026-11-05

### Fixed
- Missing optional ROS installations and verified unsourced native ROS library
  paths no longer produce a plugin error at every startup. Starting the subscriber
  reports the setup instruction; broken packages and incompatible runtimes still
  report errors.

## [1.1.2] - 2026-10-01

### Fixed
- Nested action feedback and service interfaces retain their namespace when
  building schemas, avoiding truncated package names and wrong typesupport lookup.

## [1.1.1] - 2026-09-30

### Fixed
- linux-arm64: the RoboStack payloads now build. Their build environment
  used the x86_64 compiler on arm64, which failed to link, so the 1.1.0
  release has no linux-arm64 package. 1.1.1 ships the 1.1.0 changes below
  on both linux-x86_64 and linux-arm64.

## [1.1.0] - 2026-09-22

### Added
- Payloads for ROS 2 Kilted Kaiju and Lyrical Luth, on linux-x86_64 and
  linux-arm64. The proxy auto-selects them like any other distro.
- RoboStack payloads for Jazzy and Kilted (`dist/<distro>-robostack/`),
  built against RoboStack's conda `rclcpp`. The proxy loads them when
  PlotJuggler runs from a RoboStack environment, where the `/opt/ros`
  payloads cannot find their libraries.

### Changed
- Dropped the ROS 2 Iron Irwini payload. Iron reached end of life in
  November 2024; installs that still source it now fall back to whichever
  other supported distro is present, or report that none was detected.

### Fixed
- RoboStack detection looked for `$CONDA_PREFIX/share/<distro>`, which
  RoboStack does not create. The proxy now reads the distro from the
  environment's package records, ahead of `/opt/ros`.

## [1.0.2] - 2026-09-06

### Fixed
- Silence inner library discovery noise in the proxy.
