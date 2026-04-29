# ROS 2 builder image for the data_stream_ros2 per-distro subscriber.
#
# Parametrized by ROS_DISTRO and BASE_IMAGE so the same Dockerfile covers
# every supported distro. Driven by ../run-local.sh.

ARG BASE_IMAGE=ros:humble-ros-base
FROM ${BASE_IMAGE}

ARG ROS_DISTRO=humble
ENV ROS_DISTRO=${ROS_DISTRO}
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      cmake \
      ninja-build \
      git \
      python3-venv \
      ca-certificates \
      openssh-client \
 && rm -rf /var/lib/apt/lists/*

# Conan in an isolated venv.
RUN python3 -m venv /opt/conan && /opt/conan/bin/pip install --no-cache-dir 'conan>=2.0,<3'
ENV PATH="/opt/conan/bin:${PATH}"

WORKDIR /workspace
COPY build-distro.sh /usr/local/bin/build-distro.sh
RUN chmod +x /usr/local/bin/build-distro.sh

ENTRYPOINT ["/usr/local/bin/build-distro.sh"]
