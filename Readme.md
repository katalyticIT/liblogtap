
# liblogtap

This project provides a Linux library which may be used to tap into
logs of containers and to analyze, transform or redirect the output.
It's capable of intercepting stdout, stderr and up to one custom
logfile.

If you need to capture the log file of a containered legacy app while
reducing disk i/o and saving disk space, liblogtap is your friend.

## Introduction

The liblogtap library gets (pre)loaded using the environment variable
*LD_PRELOAD* to get inserted _between_ the app and the Linux kernel.
There it overwrites central funtions like _write()_ and _writev()_,
tapping into the stdout/stderr streams or even a custom logfile and
writing copies of the data into a configured file or a socket.

If the latter resides on a shared volume, another process may read
from file or socket and process the data according to your needs,
e.g. enhance or filter it and/or forward it to a central sink.

![One of the 12 factors: Logs should go to stdout.](img/12factors_dodekaeder_datacenter.png)

## Use cases

Modern apps should follow the 12 factors pattern, once they're packaged
into a docker image, but there's still some around which don't, writing
their logs into the containers filesystem (or an attached volume).
Often it's a legacy application which nobody dares to touch or it's
a third party tool of which the image may not or cannot be modified.
To fix this, liblogtap is able to intercept the data of a given logfile
and instead write it into a socket, where the sidecar can pick it up,
forwarding it *into the place you want*.

*But why shouldn't I just use a sidecar which is tailing the log file
on a shared volume?*

This is also a viable path. You'll find some examples how to do that
in the [kubernetes documentation](https://kubernetes.io/docs/concepts/cluster-administration/logging/#streaming-sidecar-container).
But in these scenarios you double the disk i/o (app writes and sidecar
reads the log) and store large log files in the shared volume, means
valuable disk space on the node gets blocked.

With liblogtab, an environment variable may be set to **suppress** the
writing to the original logfile, **reducing disk i/o**
and **keeping the containers filesystem clean**.

In the folder [fluent-demo](fluent-demo/) you'll find an example of how to
setup a sidecar that picks up the logs of a legacy app and sends it
directly via TCP to a fluentd daemonset within the cluster, which means, should
the fluentd daemonset forward the logs directly to a central sink such as
ELK, not one byte of the logs is ever written to the cluster's hard drives.

Best thing is: **no modification of app, code or image is necessary!**

Another use case for this library could be the situation where your
company *is* gathering all logs centrally using a common produkt like
the ELK stack or something similar, but where that doesn't fit your
needs, or getting access to or changes needed are way to bureaucratic.

Based on the liblogtap pattern, you could build your custom log
gathering client to route the logs to a central sink like elastic,
flume or hbase, using a sidecar running a customized image with
fluentd or similar.

In summary, this library puts you back in control, allowing you to decide
*how and where* you want to use the logs, regardless of whether it's a
self-written application or a container with third-party software.

## Cases in which it won't work

The trick liblogtap relies on is LD_PRELOAD which places it between
app and kernel where it can intercept glibc system calls. That means
there are some cases in which the trick won't work, e.g.:

* if the binary is linked statically, like often seen with go binaries,
* if there's no glibc, like with alpine images which uses musl instead.


## Building library and docker image

You may use the Makefile and just run _make clean && make_ to create the library, or compile
it manually with the following command:

```
gcc -Wall -Wextra -fPIC -o local/liblogtap.so  -shared -ldl -lpthread
```

To build the demo image containing the python scripts and the library
simply run:

```
docker build -t katalytic/liblogtap-demo:1.0 .
```


## docker-compose

Use the file docker-compose.yaml for running a demo showing how liblogtap works.

The image gets used for both containers containing the following active components:
* clock.py - simulating the main app, creating simple log output,
* liblogtap.so - the library that taps into stdout and redirects it,
* log_sink.py - reading the redirected log in the sidecar.

The trick is to *preload* liblogtap.so in the main container, using
the environment variable LD_PRELOAD, before the script "clock.py"
even starts.

The library intercepts the system calls _write()_ and _writev()_ and writes copies of the
intercepted data to a Unix socket in a shared volume. This keeps the
communication in memory and without the overhead of e.g. a network
socket.

The sidecar container on the other side runs the script "log_sink.py"
which reads the tapped logs and enriches them before writing them as
single line JSON to stdout.

The following lines show an example output:

```
$ docker-compose  up -d
Creating network "liblogtap_default" with the default driver
Creating sidecar ... done
Creating main-app ... done
$ docker logs main-app 
[liblogtap DEBUG PID:1] liblogtap active (debug mode) - injected PYTHONUNBUFFERED=1
[liblogtap DEBUG PID:1] Successfully connected to target socket.
13:47:35
13:47:37
13:47:39
$ docker logs  sidecar 
JSON Aggregator active. Metadata: docker-compose/sidecar
{"timestamp": "2026-03-11T13:47:35.097063Z", "pod_name": "sidecar", "namespace": "docker-compose", "message": "13:47:35", "stream": "stdout", "source": "interceptor-hook"}
{"timestamp": "2026-03-11T13:47:37.097196Z", "pod_name": "sidecar", "namespace": "docker-compose", "message": "13:47:37", "stream": "stdout", "source": "interceptor-hook"}
{"timestamp": "2026-03-11T13:47:39.097485Z", "pod_name": "sidecar", "namespace": "docker-compose", "message": "13:47:39", "stream": "stdout", "source": "interceptor-hook"}
$ docker-compose down
Stopping main-app ... done
Stopping sidecar  ... done
Removing main-app ... done
Removing sidecar  ... done
Removing network liblogtap_default
$ 
```

## Kubernetes deployment

Use the file _deplyoment.yaml_ for a show case which demonstrates the capabilities
of liblogtap in a kubernetes environment. The demo deployment shows

* how to use an init container injecting liblogtap.so into an application container **without modifying it**,
* where the library intercepts the applications output and
* reroutes it to a sidecar which is reading and processing the logs.

An quick and easy way to try it is by deploying it on an online k8s
playgound like killercoda.com.

For more information see the documentation inside the file deployment.yaml.

## Extended kubernetes deployment with fluentd sink

There's an extended demo deployment with a custom go binary to pick up
the logs and send it to a fluentd TCP sink. Read more about it in the
[fluent-demo/](fluent-demo/) folder.

## Controlling the library with environment variables.

The behaviour of the liblogtap library can be controlled through a couple of
environment variables:

| environment variable  | What's it for? |
| --------------------  | -------------- |
| LLT_PASSIV_ON_START   | Time in ms which the library stays passive on startup. This is to address race conditions and dead loops while e.g. the dynamic linker still loads libraries. |
| LLT_TAP_INTO          | Determines which log stream to tap into; 0=none, 1=stdout, 2=stderr, 3=both. Default=0. |
| LLT_SUPPRESS_STDOUT   | Set to "true" to suppress the original logging on the main process. Default=false. |
| LLT_DEBUG_MODE        | Set to "true" if you want output of the library itself. Default=false. |
| LLT_TARGET_1STCONNECT | Interval in seconds to wait before the very first connection attempt to the target. This gives the sidecar time to start up and create the socket. Default is 5.|
| LLT_TARGET_RECONNECT  | Number of seconds to wait until trying to reconnect to the socket, e.g. if the sidecar gets restart and the socket is temporarily unavailable. This is to ensure the main app won't get stuck waiting on the logging. Default=30.|
| LLT_TARGET            | "file:/path/to/file" or "socket:/path/to/socket". Default=file:/tmp/liblogtap.log. |
| LLT_TAP_FILE          | Custom file inside the container to tap into, e.g. something like /var/log/nginx.log. There's still containers around which don't follow the 12 factors. Default=none.|
| LLT_SUPPRESS_TAP_FILE | Tap logfile and write to own file/socket, but don't write to intended log (which the application won't notice). Useful to reduce disk i/o and keep disk space footprint of the container small. Default=false. |

## AI notice

Parts of the library source code and the log_sink python script were created using AI (Google Gemini).



