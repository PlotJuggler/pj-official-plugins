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
| `--with-pj-app` | (modifier) After the distro build, also build `pj_app` from `pj4` and assemble a single-distro test extension layout | `<pj4>/build/Release/pj_app/pj_app` and `build_ros2_<distro>/Release/test_extensions/ros2-topic-subscriber/` |

## Examples

    cd data_stream_ros2/docker
    ./run-local.sh --distro humble
    ./run-local.sh --distro all
    ./run-local.sh --proxy
    ./run-local.sh --bundle
    ./run-local.sh --distro humble --with-pj-app --pj4 /path/to/pj4

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

## `--with-pj-app` (smoke-testing a distro in a clean environment)

Distro mode only. Requires `--pj4 <path>` because the build artifacts land
in `<pj4>/build/`.

When set, the distro builder image is extended with Qt 6.8.3 (via
`aqtinstall`, mirroring `pj4/build.sh`) plus the X/GL runtime libs needed
by Qt apps. After the plugin `.so` is built, the container also configures
`pj4` and builds the `pj_app` target. `pj4`'s own `CMakeLists.txt` already
forces `PJ_BUILD_TESTS`, `PJ_BUILD_PORTED_PLUGINS` and
`PJ_BUILD_PARQUET_IMPORT_EXAMPLE` internally, so this script does not pass
them on the cmake command line.

Image tags are namespaced (`pj-ros2-builder:<distro>-with-pj-app`) so the
heavier image never replaces the lean one used by CI. The plain-build path
(without the flag) is unchanged in every respect.

The single `build/` directory under `<pj4>` is intentional — `pj4` is
ROS-agnostic. If you switch between toolchains (humble/iron use GCC 11,
jazzy/rolling use GCC 13) the script aborts with a hint to
`rm -rf <pj4>/build/`. It does not auto-delete.

### Test extension layout

`pj_app` discovers extensions by scanning every subdirectory of its
extensions root (default `~/.local/share/PlotJuggler4/extensions/`). The
script assembles a marketplace-style layout next to the plugin build:

    <plugins>/build_ros2_<distro>/Release/test_extensions/
      ros2-topic-subscriber/                                ← one extension dir
        libros2_stream_plugin.so                            ← proxy entrypoint
        manifest.json                                        ← from data_stream_ros2/
        dist/<distro>/libros2_stream_plugin-<distro>.so     ← per-distro inner

To run `pj_app` inside the same image, bind-mount the assembled
`test_extensions/` as the container user's extensions dir and forward X11:

    xhost +local:root
    docker run --rm -it \
      --net=host \
      -e DISPLAY=$DISPLAY \
      -e QT_X11_NO_MITSHM=1 \
      -v /tmp/.X11-unix:/tmp/.X11-unix:ro \
      -v <plugins>:/workspace \
      -v <pj4>:/pj4 \
      -v <plugins>/build_ros2_<distro>/Release/test_extensions:/root/.local/share/PlotJuggler4/extensions \
      --entrypoint bash \
      pj-ros2-builder:<distro>-with-pj-app

    # inside the container
    source /opt/ros/<distro>/setup.bash
    export LD_LIBRARY_PATH=${QT_DIR}/lib:${LD_LIBRARY_PATH}
    /pj4/build/Release/pj_app/pj_app

For headless validation, replace the X11 flags with
`-e QT_QPA_PLATFORM=offscreen`.

## Supported distros

See [`distros.env`](./distros.env) — single source of truth shared by the
local helper and CI workflows.

## Windows

Linux only today. A Windows variant (Chocolatey-based ROS 2 installs) is a
follow-up.
