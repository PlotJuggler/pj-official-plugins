#!/usr/bin/env bash
# Publish two H.264 test cameras to mediamtx via RTSP (paths cam0, cam1).
# No hardware needed (test patterns). Pass a v4l2 device as $1 to make cam0
# a real webcam:  ./publish_cameras.sh /dev/video0
set -euo pipefail
SERVER="${SERVER:-rtsp://127.0.0.1:8554}"
DEVICE="${1:-}"

# Supervise the publishers: if either dies (encoder error, server gone), kill
# the sibling and exit with the failing child's status instead of lingering
# half-alive on a bare `wait`.
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT INT TERM
supervise() {
  local status=0
  wait -n || status=$?
  kill "${pids[@]}" 2>/dev/null || true
  exit "$status"
}

if command -v ffmpeg >/dev/null 2>&1; then
  if [[ -n "$DEVICE" ]]; then
    ffmpeg -loglevel warning -f v4l2 -i "$DEVICE" \
      -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -f rtsp "$SERVER/cam0" &
    pids+=("$!")
  else
    ffmpeg -loglevel warning -re -f lavfi -i "testsrc=size=640x480:rate=30" \
      -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -f rtsp "$SERVER/cam0" &
    pids+=("$!")
  fi
  ffmpeg -loglevel warning -re -f lavfi -i "testsrc2=size=640x480:rate=30" \
    -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -f rtsp "$SERVER/cam1" &
  pids+=("$!")
  supervise
elif command -v gst-launch-1.0 >/dev/null 2>&1; then
  # rtspclientsink is in the gstreamer1.0-rtsp package.
  gst-launch-1.0 videotestsrc is-live=true \
    ! video/x-raw,width=640,height=480,framerate=30/1 ! videoconvert \
    ! x264enc tune=zerolatency key-int-max=30 ! h264parse \
    ! rtspclientsink location="$SERVER/cam0" &
  pids+=("$!")
  gst-launch-1.0 videotestsrc is-live=true pattern=ball \
    ! video/x-raw,width=640,height=480,framerate=30/1 ! videoconvert \
    ! x264enc tune=zerolatency key-int-max=30 ! h264parse \
    ! rtspclientsink location="$SERVER/cam1" &
  pids+=("$!")
  supervise
else
  echo "need ffmpeg or gst-launch-1.0" >&2
  exit 1
fi
