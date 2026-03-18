# -----------------------------------------
# Stage 1: Build the Go binary
# -----------------------------------------
FROM golang:1.21-alpine AS builder

# Set working directory inside the container
WORKDIR /app

# Copy the Go source code
COPY main.go .

# Initialize a dummy go module and build the static binary
# CGO_ENABLED=0 ensures a fully static binary without external C dependencies
RUN go mod init logfwd && \
    CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -a -installsuffix cgo -o logfwd main.go

# -----------------------------------------
# Stage 2: Create the minimal final image
# -----------------------------------------
FROM alpine:3.19

# Add certificates in case you ever switch to TLS/HTTPS
RUN apk --no-cache add ca-certificates tzdata

# Copy the compiled binary from the builder stage
COPY --from=builder /app/logfwd /usr/local/bin/logfwd

# Create the directory for the default socket path
RUN mkdir -p /var/run && chmod 777 /var/run

# Set default environment variables (can be overridden in k8s deployment)
ENV NODE_NAME="unknown" \
    POD_NAME="unknown" \
    POD_NAMESPACE="unknown" \
    SOCKET_PATH="/var/run/logfwd-socket" \
    SINK_STDOUT="true" \
    SINK_FLUENT=""

# Run the binary
ENTRYPOINT ["/usr/local/bin/logfwd"]
