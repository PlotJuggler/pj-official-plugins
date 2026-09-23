# Changelog — data_stream_ros2

One entry per released version (newest first). Introduced at the version
below; for earlier releases see the git history of `data_stream_ros2/`.

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
