# Proxy builder image for the data_stream_ros2 proxy.
#
# Plain Ubuntu 22.04, no ROS overlay. The proxy .so must NOT depend on
# librclcpp — it only links against libdl/libstdc++/libc — so it has to
# be built in an environment that does not pull ROS deps via
# CMAKE_PREFIX_PATH or otherwise. 22.04 chosen on purpose: the resulting
# binary uses an older glibc and runs on any host distro the user might
# have installed (including 24.04+).

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      ninja-build \
      git \
      openssh-client \
      python3-venv \
      ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir 'conan>=2.0,<3'
ENV PATH="/opt/conan/bin:${PATH}"

WORKDIR /workspace
COPY build-proxy.sh /usr/local/bin/build-proxy.sh
RUN chmod +x /usr/local/bin/build-proxy.sh

ENTRYPOINT ["/usr/local/bin/build-proxy.sh"]
