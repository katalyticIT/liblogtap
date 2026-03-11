
#
# **** log aggregator for sidecar in liblogtap demo ****
#
# reads over a socket in a shared volume the log output from the main
# container, enriches it and writes it to stdout as single line JSON.
#
# A next step would be e.g. to add a fluentd client or a similar tool
# for central aggregation of logs.
#
# ----
#
# This file is part of https://github.com/katalyticIT/liblogtap and is
# licensed under GPL 3.0. See LICENSE file for details.
#

import socket
import os
import json
import datetime

# Path to socket in shared volume
SOCKET_PATH = "/var/run/sidecar-logging.sock"

# These values may be set by k8s
POD_NAME = os.getenv("POD_NAME", "unknown-pod")
POD_NAMESPACE = os.getenv("POD_NAMESPACE", "default")

def start_json_logging_sink():
    if os.path.exists(SOCKET_PATH):
        os.remove(SOCKET_PATH)

    # connect to socket
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(SOCKET_PATH)
    os.chmod(SOCKET_PATH, 0o666)  # set permission so that the library in the
    server.listen(10)             # main app container may write to the socket

    buffer = {}

    # say hello
    print(f"JSON Aggregator active. Metadata: {POD_NAMESPACE}/{POD_NAME}")

    try:
        while True:
            conn, _ = server.accept()
            try:
                while True:
                    data = conn.recv(8192)  # read from socket
                    if not data: break

                    # split data into lines
                    text  = data.decode('utf-8', errors='replace')
                    lines = text.split('\n')

                    if conn in buffer:
                        lines[0] = buffer[conn] + lines[0]

                    if not text.endswith('\n'):
                        buffer[conn] = lines.pop()
                    else:
                        if conn in buffer: del buffer[conn]

                    # process line-by-line
                    for line in lines:
                        if not line.strip(): continue

                        # Create structured log object
                        log_entry = {
                            "timestamp": datetime.datetime.utcnow().isoformat() + "Z",
                            "pod_name":  POD_NAME,
                            "namespace": POD_NAMESPACE,
                            "message":   line.strip(),
                            "stream":    "stdout",
                            "source":    "interceptor-hook"
                        }

                        # Simple heuristics: if the msg looks like JSON, then try to parse it
                        if line.strip().startswith('{') and line.strip().endswith('}'):
                            try:
                                nested_json = json.loads(line)
                                log_entry["data"] = nested_json
                            except:
                                pass

                        # print as single JSON line (important for tools like Filebeat or Fluentd)
                        print(json.dumps(log_entry))

                #-- end(while)

            except Exception as e:
                pass 
            finally:
                if conn in buffer: del buffer[conn]
                conn.close()
    finally:
        if os.path.exists(SOCKET_PATH):
            os.remove(SOCKET_PATH)

if __name__ == "__main__":
    start_json_logging_sink()
