# `data_stream_ros2` — Docker build helpers

Build the `data_stream_ros2` marketplace extension end-to-end using Docker.
The extension has a proxy + per-distro split:

- A distro-agnostic **proxy** `.so` that detects the ROS 2 distribution
  installed on the user's machine at load time and `dlopen`s the matching
  per-distro binary.
- A **per-distro** `.so` per supported ROS 2 distribution (`humble`, `iron`,
  `jazzy`, `rolling`), linked against that distro's `rclcpp`.

These images cover two roles:

1. **Reproducible builds** of the proxy and per-distro artifacts on a
   developer machine, regardless of the host's ROS install (or absence).
2. **Release packaging**: assembling the proxy + every per-distro `.so`
   into the marketplace zip layout consumed by PlotJuggler 4.x.

## Layout

    data_stream_ros2/
      docker/
        distro/                   # ROS-aware builder image
          Dockerfile
          build-distro.sh
        proxy/                    # Plain Ubuntu builder image (no ROS overlay)
          Dockerfile
          build-proxy.sh
        distros.env               # Single source of truth for supported distros
        run-local.sh              # Top-level entry point
        README.md

## Why two Docker images

The proxy `.so` must NOT depend on `librclcpp` — that is the whole point of
the dispatch design. Building it inside any `osrf/ros:*-desktop` image would
risk pulling ROS symbols transitively through `CMAKE_PREFIX_PATH` or system
libraries. A plain Ubuntu 22.04 image with no ROS installed enforces the
constraint by construction.

Ubuntu 22.04 (and not a newer release) is chosen on purpose: the resulting
proxy binary uses an older glibc and runs on any host distro the user might
have installed (including 24.04+).

## Modes

| Flag | Action | Output |
|------|--------|--------|
| `--distro <distro>` | One per-distro build against `/opt/ros/<distro>` | `build_ros2_<distro>/Release/bin/libros2_stream_plugin-<distro>.so` |
| `--distro all` | Iterates every entry in `distros.env` | one `build_ros2_<distro>/…` per distro |
| `--proxy` | Builds the proxy in plain Ubuntu 22.04 | `build_ros2_proxy/Release/bin/libros2_stream_plugin.so` |
| `--bundle` | Every per-distro build + proxy + assembled tree + marketplace zip | see "Bundle layout" below |

## Examples

    cd data_stream_ros2/docker
    ./run-local.sh --distro humble
    ./run-local.sh --distro all
    ./run-local.sh --proxy
    ./run-local.sh --bundle

## Bundle layout

After `--bundle`, under the `pj-official-plugins` root:

    dist_ros2/
      libros2_stream_plugin.so                      ← entry point referenced by manifest.json
      manifest.json                                  ← copied from data_stream_ros2/
      dist/
        humble/libros2_stream_plugin-humble.so
        iron/libros2_stream_plugin-iron.so
        jazzy/libros2_stream_plugin-jazzy.so
        rolling/libros2_stream_plugin-rolling.so

    ros2-topic-subscriber-linux-x86_64.zip           ← marketplace artifact

## `plotjuggler_core` resolution

The plugin's CMake fetches `plotjuggler_core` via CPM. Two modes:

- **Default**: CPM clones from GitHub at build time. Requires the container
  to have outbound network access. Inside the container the SSH URL
  declared in CMake is rewritten to HTTPS so no SSH agent is needed.
- **`--core <path>`**: bind-mounts an existing local checkout at `/core`
  and passes `-DCPM_plotjuggler_core_SOURCE=/core` to CMake. Useful for
  offline builds and to avoid re-cloning.

## Supported distros

See [`distros.env`](./distros.env) — single source of truth shared by the
local helper and CI workflows.

## Windows

Linux only today. A Windows variant (Chocolatey-based ROS 2 installs) is a
follow-up.
