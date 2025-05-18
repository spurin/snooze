# -------------------------
# Stage 1: Build
# -------------------------
FROM alpine:latest AS build

# Install necessary build dependencies
RUN apk add --no-cache \
    cmake \
    make \
    gcc \
    musl-dev

WORKDIR /app

# Copy source files and CMakeLists.txt
COPY snooze.c CMakeLists.txt ./

# Create build directory and compile with CMake
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_EXE_LINKER_FLAGS="-static" .. && \
    make -j$(nproc) && \
    strip --strip-all snooze

# -------------------------
# Stage 2: Final Image
# -------------------------
FROM scratch

# Copy the compiled binary from the build stage
COPY --from=build /app/build/snooze /snooze

# Expose the default HTTP port (80)
EXPOSE 80

# By default, run on port 80 with the default message.
ENTRYPOINT ["/snooze"]

