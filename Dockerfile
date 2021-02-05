FROM ubuntu:focal
ARG DEBIAN_FRONTEND=noninteractive

RUN \
  mkdir -p /usr/src/x11pulsemux/src && \
  apt update && \
  apt install -y cmake gcc g++ pkg-config make \
  ffmpeg libavutil-dev libpostproc-dev libswresample-dev \
  libavcodec-dev libavfilter-dev libavformat-dev \
  libavutil-dev libpostproc-dev libswresample-dev \
  libswscale-dev libavdevice-dev libuv1-dev \
  xvfb pulseaudio curl && \
  curl -o /tmp/chrome.deb https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb && \
  cd /tmp && apt install -y ./chrome.deb

COPY CMakeLists.txt /usr/src/x11pulsemux
COPY src /usr/src/x11pulsemux/src

RUN \
  cd /usr/src/x11pulsemux && \
  mkdir build && \
  cd build && \
  cmake .. && \
  make
