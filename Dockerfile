# sdcpp-serve: image generation on the CPU with stable-diffusion.cpp, see README.md

FROM ubuntu:24.04 AS source

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates \
  && rm -rf /var/lib/apt/lists/*

# stable-diffusion.cpp release master-995-fd21ecf
ARG SDCPP_COMMIT=fd21ecfceef2665d2c5f2db8536e3202d864fd30

WORKDIR /sd.cpp

RUN git init -q . \
  && git remote add origin https://github.com/leejet/stable-diffusion.cpp \
  && git fetch -q --depth 1 origin ${SDCPP_COMMIT} \
  && git checkout -q FETCH_HEAD \
  && git submodule update -q --init --depth 1 ggml

FROM source AS build

ARG TARGETARCH

# CPU only
# On x86 every CPU variant is built, the best one is picked at runtime
RUN if [ "$TARGETARCH" = amd64 ]; then \
      CPU_ARGS="-DGGML_NATIVE=OFF -DGGML_BACKEND_DL=ON -DGGML_CPU_ALL_VARIANTS=ON"; \
    else \
      CPU_ARGS="-DGGML_NATIVE=ON"; \
    fi \
  && cmake -B build $CPU_ARGS \
    -DSD_SERVER_BUILD_FRONTEND=OFF \
    -DSD_BUILD_SHARED_LIBS=ON \
    -DSD_BUILD_SHARED_GGML_LIB=ON \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
  && cmake --build build --parallel "$(nproc)"

# the license files in stable-diffusion.cpp's source: its own and of what it builds in
RUN mkdir /licenses \
  && cp LICENSE /licenses/stable-diffusion.cpp.txt \
  && cp ggml/LICENSE /licenses/ggml.txt \
  && cp thirdparty/utf8proc/LICENSE.md /licenses/utf8proc.md \
  && cp thirdparty/oniguruma/COPYING /licenses/oniguruma.txt \
  && cp thirdparty/LICENSE.darts_clone.txt /licenses/darts-clone.txt

# needs only the thirdparty headers, so it compiles in parallel with build
FROM source AS server

COPY server.cpp /server/

RUN g++ -std=c++17 -O2 -Wall -Wextra -s -isystem thirdparty \
    -o /server/sdcpp-serve /server/server.cpp -pthread

# only the runtime libraries, no shell, non-root
# its glibc must not be older than the build stage's (Ubuntu 24.04)
FROM gcr.io/distroless/cc-debian13:nonroot

COPY --from=build /sd.cpp/build/bin /opt/sd.cpp
COPY --from=build /licenses /usr/share/doc/sdcpp-serve/third-party
COPY --chmod=644 LICENSE /usr/share/doc/sdcpp-serve/
COPY --from=server /server/sdcpp-serve /usr/local/bin/

EXPOSE 8081

ENTRYPOINT [ "/usr/local/bin/sdcpp-serve" ]
