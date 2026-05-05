# `data_stream_ros2` — ROS 2 Topic Subscriber

A PlotJuggler 4.x marketplace extension that subscribes to ROS 2 topics in
real time and streams the decoded fields into PlotJuggler. Linux-only
today (`linux-x86_64`, `linux-arm64`).

## What makes this plugin different

A single shared library cannot link against more than one ROS 2
distribution at once: `rclcpp` ABI, type support libraries and DDS
implementations all change between distros. Distributing one binary per
distro inside a single marketplace extension requires a load-time
dispatcher.

This extension is therefore split into two artifacts:

- **Proxy** (`libros2_stream_plugin.so`) — the entry point advertised by
  `manifest.json`. No dependency on `rclcpp`, `<dlfcn.h>` only. At first
  call into the plugin's vtable it detects the ROS 2 distribution
  installed on the user's machine (via `ROS_DISTRO`, `/opt/ros/*`, or an
  override env var) and `dlopen`s the matching per-distro binary.

- **Per-distro inner** (`libros2_stream_plugin-<distro>.so`) — the actual
  subscriber, compiled against one specific distro's `rclcpp`. One inner
  per supported distro (see [`docker/distros.env`](docker/distros.env)).
  Inner libraries deliberately rename their vtable getters
  (`PJ_ros2_inner_get_*`) so PlotJuggler's recursive plugin scanner does
  not register every distro as a top-level plugin — only the proxy is
  visible to the scanner.

```
~/.local/share/PlotJuggler4/extensions/ros2-topic-subscriber/
  manifest.json
  libros2_stream_plugin.so                ← proxy (entry point)
  dist/
    humble/libros2_stream_plugin-humble.so
    iron/libros2_stream_plugin-iron.so
    jazzy/libros2_stream_plugin-jazzy.so
    rolling/libros2_stream_plugin-rolling.so
```

Source layout in this repository:

    data_stream_ros2/
      proxy/src/                ← distro-agnostic dispatcher (no rclcpp)
      distro/                   ← per-distro subscriber (links rclcpp)
        src/
        datastream_ros2.ui
        package.xml
      docker/                   ← build helpers (see "Docker" below)
      manifest.json             ← marketplace manifest
      CMakeLists.txt            ← proxy always; inner only with PJ_BUILD_ROS2_DISTRO=ON

## Building

The proxy and the per-distro inner are different CMake targets, gated on
`PJ_BUILD_ROS2_DISTRO`:

    # Proxy only — runs anywhere, no ROS required
    cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2

    # Per-distro inner — must be invoked inside a sourced ROS 2 environment
    source /opt/ros/<distro>/setup.bash
    cmake ... -DPJ_BUILD_PLUGIN=data_stream_ros2 -DPJ_BUILD_ROS2_DISTRO=ON

The active distro is exposed to the inner as a compile definition
(`ROS_DISTRO_HUMBLE`, `ROS_DISTRO_JAZZY`, …) so source files can branch on
API differences between distros (e.g. the rosbag2 → rclcpp typesupport
migration).

In practice you will rarely run those commands by hand — the Docker
helpers under [`docker/`](docker/) wrap them.

## Docker — the two roles

This plugin's `docker/` directory exists because two very different
audiences need a reproducible ROS 2 build environment:

### Role 1 — Reproducible release builds (CI and packagers)

The marketplace artifact must contain the proxy plus one inner per
supported distro, all built reproducibly against the canonical ROS 2
images and bundled with the layout PlotJuggler expects. The Docker
images guarantee that:

- The proxy is built in a **plain Ubuntu 22.04** image with no ROS
  installed, so it physically cannot pick up `librclcpp` symbols and the
  resulting binary uses an old-enough glibc to run on any host.
- Each inner is built in an `osrf/ros:<distro>-desktop` image with that
  distro's `rclcpp` available on `CMAKE_PREFIX_PATH`.
- A single `--bundle` invocation iterates every entry in `distros.env`,
  produces every inner, copies in the proxy, and assembles the
  marketplace zip.

CI on `linux-x86_64` and `linux-arm64` calls the same `run-local.sh`
entry point that developers use locally, so the local artifact is
bit-for-bit the one shipped from CI.

### Role 2 — End-to-end smoke testing (ROS developers)

Beyond producing release artifacts, the same `docker/distro/` image can
spin up `pj_app` itself inside a ROS-aware container, with a known-good
ROS 2 distro already sourced and the freshly-built plugin pre-installed
in the right marketplace layout. This is what `--with-pj-app` enables.

Concretely, `./run-local.sh --distro <distro> --with-pj-app --pj4 <path>`:

- Extends the per-distro builder image with Qt 6.8.3 (via `aqtinstall`,
  matching `pj4/build.sh`) plus the GL/X runtime libs Qt apps need. The
  Qt layer is gated by an `ARG`, so the lean CI image used by Role 1 is
  unchanged.
- Builds the plugin against `/opt/ros/<distro>` exactly as in Role 1.
- Builds `pj_app` from your `pj4` super-repo into `<pj4>/build/` so a
  fresh container start does not have to recompile the host app.
- Assembles a single-distro marketplace layout under
  `build_ros2_<distro>/Release/test_extensions/ros2-topic-subscriber/`
  ready to be bind-mounted as the container's PlotJuggler 4 extensions
  directory.

The intended flow for a ROS developer is then to launch the resulting
image, source `setup.bash` for the chosen distro, and run `pj_app` with
real ROS topics on the network — without polluting the host or worrying
about which Qt, glibc or ROS version it has installed.

End-to-end recipe and headless variants are documented in
[`docker/README.md`](docker/README.md).

## Testing the plugin without Docker

Everything Docker does can be reproduced on a host that already has a
working ROS 2 install: source the distro's `setup.bash`, build the plugin
with `-DPJ_BUILD_ROS2_DISTRO=ON`, and copy the resulting `.so` files into
`~/.local/share/PlotJuggler4/extensions/ros2-topic-subscriber/` following
the layout above. Docker is a convenience, not a requirement.

## Supported distros and platforms

| | linux-x86_64 | linux-arm64 |
|--|--|--|
| humble (GCC 11) | ✅ | ✅ |
| iron (GCC 11) | ✅ | ✅ |
| jazzy (GCC 13) | ✅ | ✅ |
| rolling (GCC 13) | ✅ | ✅ |

Single source of truth: [`docker/distros.env`](docker/distros.env).

Windows is a follow-up — Chocolatey-based ROS 2 installs require a
different bootstrap path and are not covered yet.
