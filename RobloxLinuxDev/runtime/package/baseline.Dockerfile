FROM ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config python3 binutils curl ca-certificates \
    libgtk-3-dev libwebkit2gtk-4.1-dev libjson-glib-dev libwayland-dev wayland-protocols \
    libxcursor-dev libx11-dev libegl-dev libgl-dev libxkbcommon-dev libasound2-dev \
    libpulse-dev libudev-dev libavcodec-dev libavutil-dev libswscale-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-libav gstreamer1.0-pipewire \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fL --retry 3 -o /tmp/sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.2/SDL3-3.4.2.tar.gz \
    && echo 'ef39a2e3f9a8a78296c40da701967dd1b0d0d6e267e483863ce70f8a03b4050c  /tmp/sdl.tar.gz' | sha256sum -c - \
    && tar -xf /tmp/sdl.tar.gz -C /tmp \
    && cmake -S /tmp/SDL3-3.4.2 -B /tmp/sdl-build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib/x86_64-linux-gnu \
       -DCMAKE_C_FLAGS='-march=x86-64 -mtune=generic' -DSDL_X11=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    && cmake --build /tmp/sdl-build -j 8 && cmake --install /tmp/sdl-build \
    && rm -rf /tmp/SDL3-3.4.2 /tmp/sdl-build /tmp/sdl.tar.gz
RUN apt-get update && apt-get install -y --no-install-recommends libncurses-dev \
    && rm -rf /var/lib/apt/lists/*
RUN curl -fL --retry 3 -o /tmp/libedit.tar.gz https://thrysoee.dk/editline/libedit-20260512-3.1.tar.gz \
    && echo '432d5e7ea8b0116dd39f2eca7bc11d0eed77faa6b77ea526ace89907c23ea4a0  /tmp/libedit.tar.gz' | sha256sum -c - \
    && tar -xf /tmp/libedit.tar.gz -C /tmp && cd /tmp/libedit-20260512-3.1 \
    && CFLAGS='-O2 -march=x86-64 -mtune=generic' ./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu \
    && make -j 8 && make install \
    && install -Dm644 COPYING /usr/share/licenses/libedit/COPYING \
    && rm -rf /tmp/libedit-20260512-3.1 /tmp/libedit.tar.gz
RUN apt-get update && apt-get install -y --no-install-recommends libfuse2t64 libxkbfile1 libglu1-mesa libgif7 \
    && rm -rf /var/lib/apt/lists/*
RUN apt-get update && apt-get install -y --no-install-recommends vulkan-tools mesa-vulkan-drivers \
    && rm -rf /var/lib/apt/lists/*
