
# Extended liblogtap demo

## Long story short

This demo shows how liblogtap intercepts logfiles inside a container
without modifying them. A special sidecar is picking up the logs
via a Unix socket and forwards them as JSON record to a fluentd
daemon which may process, enrich and forward it furthermore.

## Why this setup

Originally it was tried to setup a sidecar with fluentd inside
it, configured to read directly from the Unix socket and then
forward it to some central sink.

Unfortunately, the socket source seems to await some kind of
structured data (presumably MessagePack) and is uncapablel of
processing plain text. It was therefore decided to proceed
another way.

In many k8s clusters, there's a fluentd daemonset running to
pickup the logs of all pods, sending them to a central sink.
So a simple logforwarder was designed and programmed in Go.
Reading from the socket, it picks up the logs and packages
them in a JSON record, enriched by some metadata. It then
writes it to a) stdout, b) a fluentd tcp sink or c) both.

### Writing to stdout

The simplest way to go. While the legacy app may write into
its default logfile and thus leaving stdout blank, the sidecar
can write it there, making it possible for an existing fluentd
to pick it up and forward it. Simple, gets the job done.
And always good for debugging.

### Writing to a fluentd TCP sink

Omitting stdout and instead forward the logs to fluentd via
internal network reduces disk i/o: the sidecar doesn't need to
write it, fluentd doesn't have to read it.

## Configuration

Use the following environment variables to configure the
intercepting sidecar:

* SOCKET_PATH : path to the socket which the sidecar has to create and listen to. Default = "/var/run/logfwd-socket".
* SINK_STDOUT : Set to "true" if you want to see the logs onstdout. Default = "false
* SINK_FLUENT : Define the network path to the fluentd TCP sink here, e.g. "my-fluentd-svc:24224". Default: not set, means logs are not forwarded over TCP.

## Metadata

Set the following environment variables in your deployment to let the
logfwd binary enrich the JSON records:

* NODE_NAME: should be valueFrom.fieldRef.fieldPath: spec.nodeName
* POD_NAME:  should be valueFrom.fieldRef.fieldPath: metadata.name
* POD_NAMESPACE: should be valueFrom.fieldRef.fieldPath: metadata.namespace

## AI notice

The Dockerfile and the code of the logfwd binary (main.go) were created
using AI (Gemini v3).

