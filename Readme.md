
# liblogtap

This project provides a Linux library which may be used to tap into
logs of containers and to analyze, transform or redirect the output.
It's capable of intercepting stdout, stderr and up to one custom
logfile.

## Introduction
The liblogtap library gets (pre)loaded using the environment variable
*LD_PRELOAD* to get inserted _between_ the app and the Linux kernel. There
it overwrites the central funtions _write()_ and _writev()_, tapping into
the stdout and stderr streams and writing copies of the data into a
file or a socket. If the latter resides on a shared volume, another
process may read from file or socket and process the data according
to your needs.

Functions to open files are intercepted, too, according to the need
to find the filehandle of the custom logfile to tap into, if this
option is set.

## Use cases
A use case for this library may be the situation where your company
_is_ gathering all logs centrally using a common produkt like the ELK
stack or something similar, but where that doesn't fit your needs,
or access to that facility or changes needed are way to bureaucratic.

Another use case would be to provide central logging to an existing
sink like elastic, flume or hbase, using a sidecar running a customized
image with fluentd or similar to gather and maybe enrich the tapped data
before sending it to a central sink.

Modern apps should follow the 12 factors pattern, once they're packaged
into a docker image, but there's still some around which don't, writing
their logs into the containers filesystem (or an attached volume). To
fix this, liblogtap is able to intercept the data of a given logfile
and instead write it into a socket, where the sidecar can pick it up,
forwarding it into the place you want. An environment variable may be
set to suppress the writing to the original logfile, reducing disk i/o
and keeping the containers filesystem clean.

In summary, this library puts you back in control, allowing you to decide
how and where you want to use the logs, regardless of whether it's a
self-written application or a container with third-party software.

## Building library and docker image

You may use the Makefile and just run _make_ to create the library, or compile
it manually with the following command:
```
gcc -fPIC -shared -o local/liblogtap.so src/liblogtap.c -ldl -lpthread
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


## Controlling the library with environment variables.

The behaviour of the liblogtap library can be controlled through a couple of
environment variables:

| environment variable  | What's it for? |
| --------------------  | -------------- |
| LLT_PASSIV_ON_START   | Time in ms which the library stays passive on startup. This is to address race conditions and dead loops while e.g. the dynamic linker still loads libraries. |
| LLT_TAP_INTO          | Determines which log stream to tap into; 0=none, 1=stdout, 2=stderr, 3=both. Default=0. |
| LLT_SUPPRESS_STDOUT   | Set to "true" to suppress the original logging on the main process. Default=false. |
| LLT_DEBUG_MODE        | Set to "true" if you want output of the library itself. Default=false. |
| LLT_TARGET_RECONNECT  | Number of seconds to wait until trying to reconnect to the socket, e.g. if the sidecar gets restart and the socket is temporarily unavailable. This is to ensure the main app won't get stuck waiting on the logging. Default=30.|
| LLT_TARGET            | "file:/path/to/file" or "socket:/path/to/socket". Default=file:/tmp/liblogtap.log. |
| LLT_TAP_FILE          | Custom file inside the container to tap into, e.g. something like /var/log/nginx.log. There's still containers around which don't follow the 12 factors. Default=none.|
| LLT_SUPPRESS_TAP_FILE | Tap logfile and write to own file/socket, but don't write to intended log. Useful to reduce disk i/o and keep disk space footprint of the container small. Default=false. |

## AI notice

Parts of the library source code and the log_sink python script were created using AI (Google Gemini).



