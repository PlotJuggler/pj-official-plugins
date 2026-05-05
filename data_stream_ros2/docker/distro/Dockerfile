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

# Optional: pull in Qt 6.8.3 (matching pj4/build.sh) plus the X/GL runtime
# libs needed to link and execute pj_app inside the container. This layer
# is only added when --with-pj-app is passed to run-local.sh; the lean CI
# image (WITH_PJ_APP=0) skips it entirely.
ARG WITH_PJ_APP=0
ENV QT_VERSION=6.8.3
ENV QT_DIR=/opt/qt/${QT_VERSION}/gcc_64
RUN if [ "${WITH_PJ_APP}" = "1" ]; then \
      apt-get update -qq \
   && apt-get install -y --no-install-recommends \
        libgl1 libegl1 libfontconfig1 libdbus-1-3 \
        libxkbcommon0 libxkbcommon-x11-0 \
        libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 \
        libxcb-randr0 libxcb-render-util0 libxcb-shape0 libxcb-sync1 \
        libxcb-xfixes0 libxcb-xkb1 \
   && rm -rf /var/lib/apt/lists/* \
   && /opt/conan/bin/pip install --no-cache-dir 'aqtinstall>=3.1' \
   && aqt install-qt linux desktop "${QT_VERSION}" linux_gcc_64 \
        --modules qtcharts qtwebsockets \
        --outputdir /opt/qt ; \
    fi

WORKDIR /workspace
COPY build-distro.sh /usr/local/bin/build-distro.sh
RUN chmod +x /usr/local/bin/build-distro.sh

ENTRYPOINT ["/usr/local/bin/build-distro.sh"]
