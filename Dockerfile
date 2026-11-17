#
#---- Dockerfile to build the liblogtap demo image ----
#
# This file is part of https://github.com/katalyticIT/liblogtap and is
# licensed under GPL 3.0. See LICENSE file for details.
#

#---- set images
ARG BUILDER_IMG=docker.io/gcc
ARG BUILDER_TAG=latest
ARG RUNTIME_IMG=docker.io/python
ARG RUNTIME_TAG=3.16-rc-alpine3.24


# --- Stage 1: Compiling the C library ---
FROM $BUILDER_IMG:$BUILDER_TAG  AS builder

WORKDIR /build
COPY    src/liblogtap.c  .
RUN     gcc -Wall -Wextra -fPIC -o liblogtap.so liblogtap.c -shared -ldl -lpthread


# --- Stage 2: Building the final sidecar image ---
FROM $RUNTIME_IMG:$RUNTIME_TAG

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

