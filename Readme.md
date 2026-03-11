
# liblogtap

This project provides a Linux library which may be used to tap into
logs of containers and to analyze, transform or redirect the output.

## Introduction
The liblogtap library gets (pre)loaded using the environment variable
LD_PRELOAD to get layered between the app and the Linux kernel. There
it overwrites the central funtions write() and writev(), tapping into
the stdout and stderr streams and writing copies of the data into a
file or a socket. If the latter resides on a shared volume, another
process may read from file or socket and process the data according
to your needs.

## Use cases
A use case for this library may be the situation where your company
_is_ gathering all logs centrally using a common produkt like the ELK
stack or something similar, but where that doesn't fit your needs,
or access or changes needed are way to bureaucratic.

Another use case would be to provide central logging to an existing
sink like elastic, flume or hbase, using a customized image with
fluentd or similar to enrich and gather the tapped data.

This library puts you back in control, allowing you to decide for yourself
how and where you want to use the logs, regardless of whether it's a
self-written application or a container with third-party software.


(to be continued)

