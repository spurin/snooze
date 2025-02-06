# -------------------------
# Stage 1: Build
# -------------------------
FROM alpine:latest AS build

RUN apk add --no-cache gcc musl-dev

WORKDIR /app
COPY snooze.c ./

# Compile with static linking, remove unneeded sections
RUN gcc -Os \
        -ffreestanding \
        -fno-asynchronous-unwind-tables \
        -fno-stack-protector \
        -static \
        -s \
        -Wl,--gc-sections \
        -fdata-sections \
        -ffunction-sections \
        -o snooze snooze.c && \
    strip --strip-all snooze

# -------------------------
# Stage 2: Final Image
# -------------------------
FROM scratch

COPY --from=build /app/snooze /snooze

# Expose the default HTTP port (80)
EXPOSE 80

# By default, run on port 80 with the default message.
ENTRYPOINT ["/snooze"]
