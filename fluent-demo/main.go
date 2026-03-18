package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

// LogRecord defines the required JSON structure
type LogRecord struct {
	Datetime     string `json:"datetime"`
	NodeName     string `json:"nodename"`
	PodNamespace string `json:"pod_namespace"`
	PodName      string `json:"pod_name"`
	Msg          string `json:"msg"`
}

// Config holds the environment variables
type Config struct {
	NodeName     string
	PodName      string
	PodNamespace string
	SocketPath   string
	SinkStdout   bool
	SinkFluent   string
}

func main() {
	// 1. Load configuration with default values
	cfg := Config{
		NodeName:     getEnvOrDefault("NODE_NAME", "unknown"),
		PodName:      getEnvOrDefault("POD_NAME", "unknown"),
		PodNamespace: getEnvOrDefault("POD_NAMESPACE", "unknown"),
		SocketPath:   getEnvOrDefault("SOCKET_PATH", "/var/run/logfwd-socket"),
		SinkStdout:   strings.ToLower(getEnvOrDefault("SINK_STDOUT", "false")) == "true",
		SinkFluent:   getEnvOrDefault("SINK_FLUENT", ""),
	}

	log.Printf("Starting logfwd. Configuration loaded: %+v\n", cfg)

	// Channel to decouple reading from network sending
	// Buffer of 5000 lines to handle bursts
	logChan := make(chan []byte, 5000)

	// 2. Start TCP worker for Fluentd if configured
	if cfg.SinkFluent != "" {
		go fluentTCPWorker(cfg.SinkFluent, logChan)
	}

	// 3. Setup Unix Socket
	// Remove socket if it already exists from a previous crashed run
	if err := os.RemoveAll(cfg.SocketPath); err != nil {
		log.Fatalf("Failed to remove existing socket: %v", err)
	}

	listener, err := net.Listen("unix", cfg.SocketPath)
	if err != nil {
		log.Fatalf("Failed to listen on unix socket: %v", err)
	}
	defer listener.Close()

	// Make socket writable for the application container
	if err := os.Chmod(cfg.SocketPath, 0666); err != nil {
		log.Fatalf("Failed to change socket permissions: %v", err)
	}
	log.Printf("Listening on unix socket: %s", cfg.SocketPath)

	// 4. Handle graceful shutdown (cleanup socket file)
	go handleShutdown(listener, cfg.SocketPath)

	// 5. Main accept loop
	for {
		conn, err := listener.Accept()
		if err != nil {
			log.Printf("Error accepting connection: %v", err)
			continue
		}
		// Handle each connection in a new goroutine
		go handleClient(conn, cfg, logChan)
	}
}

// handleClient reads plain text lines from the unix socket
func handleClient(conn net.Conn, cfg Config, logChan chan<- []byte) {
	defer conn.Close()

	scanner := bufio.NewScanner(conn)
	for scanner.Scan() {
		text := scanner.Text()

		// Build the record
		record := LogRecord{
			Datetime:     time.Now().UTC().Format(time.RFC3339Nano),
			NodeName:     cfg.NodeName,
			PodNamespace: cfg.PodNamespace,
			PodName:      cfg.PodName,
			Msg:          text,
		}

		// Convert to JSON
		jsonBytes, err := json.Marshal(record)
		if err != nil {
			log.Printf("Failed to marshal json: %v", err)
			continue
		}

		// Passive mode: do nothing if neither sink is configured
		if !cfg.SinkStdout && cfg.SinkFluent == "" {
			continue
		}

		// Output to stdout
		if cfg.SinkStdout {
			// Print standard string format, adding a newline
			fmt.Println(string(jsonBytes))
		}

		// Output to Fluentd via channel
		if cfg.SinkFluent != "" {
			// Append newline for NDJSON stream over TCP
			payload := append(jsonBytes, '\n')
			select {
			case logChan <- payload:
				// Successfully sent to buffer
			default:
				// Buffer is full (Fluentd might be down).
				// We drop the log to prevent blocking the app container.
				log.Println("Warning: Fluentd buffer full, dropping log line")
			}
		}
	}

	if err := scanner.Err(); err != nil {
		log.Printf("Error reading from socket connection: %v", err)
	}
}

// fluentTCPWorker maintains a persistent connection to Fluentd
func fluentTCPWorker(target string, logChan <-chan []byte) {
	var conn net.Conn
	var err error

	for {
		// Attempt to connect if not connected
		if conn == nil {
			conn, err = net.DialTimeout("tcp", target, 5*time.Second)
			if err != nil {
				log.Printf("Failed to connect to fluentd at %s, retrying in 5s...", target)
				time.Sleep(5 * time.Second)
				continue
			}
			log.Printf("Successfully connected to fluentd at %s", target)
		}

		// Read from channel and write to network
		msg := <-logChan
		_, err = conn.Write(msg)
		if err != nil {
			log.Printf("Failed to write to fluentd, dropping connection: %v", err)
			conn.Close()
			conn = nil
			// We lost one message here, but we will reconnect on the next loop
		}
	}
}

// getEnvOrDefault returns the env var value or a fallback
func getEnvOrDefault(key, fallback string) string {
	if value, exists := os.LookupEnv(key); exists && value != "" {
		return value
	}
	return fallback
}

// handleShutdown ensures the socket file is deleted when the pod terminates
func handleShutdown(listener net.Listener, socketPath string) {
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)
	<-sigs
	log.Println("Shutting down logfwd...")
	listener.Close()
	os.RemoveAll(socketPath)
	os.Exit(0)
}
