#
#---- Dockerfile to build the liblogtap demo image ----
#
# This file is part of https://github.com/katalyticIT/liblogtap and is
# licensed under GPL 3.0. See LICENSE file for details.
#

# --- Stage 1: Compiling the C library ---
FROM gcc:latest AS builder

WORKDIR /build
COPY    src/liblogtap.c  .
RUN     gcc -fPIC -shared -o liblogtap.so liblogtap.c -ldl -lpthread


# --- Stage 2: Building the final sidecar image ---
FROM python:3.11-slim

WORKDIR /app

# Copy the library from the first build stage
COPY --from=builder /build/liblogtap.so /usr/local/lib/liblogtap.so

# Copy python sources for the clock app in the tapped
# container and the log-reader in the sidecar
COPY src/*.py ./

# add executable flag
RUN ls *.py && chmod +x *.py

# default command for sidecar
CMD ["python3", "-u", "log_sink.py"]

