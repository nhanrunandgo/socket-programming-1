# Reliable UDP File Transfer Protocol

This project implements a **Reliable File Transfer Protocol** over **UDP** using C++. It simulates reliable data transmission features typically found in TCP (like ACKs, timeouts, retransmissions, and flow control) on top of the connectionless UDP protocol.

## 🌟 Special Features & Highlights

The project is designed with performance and reliability in mind, featuring:

### 1. Reliability Layer over UDP
-   **ACK Mechanism:** Implements a custom acknowledgment system (`REPLY:Seq#:ACK`) to ensure packets are received.
-   **Timeout & Retransmission:** Both client and server run dedicated background threads (`timeout_checker_thread` / `resend_packet_thread`) to monitor and retransmit lost packets after a specified timeout (`ACK_TIMEOUT` / `SENDING_TIMEOUT`), ensuring data delivery even in unstable networks.
-   **Sequence Numbers:** Uses sequence numbers to track packets and manage state for multiple clients.

### 2. High-Performance File Transfer
-   **Chunk-Based Transfer:** Files are split into fixed-size chunks (default 1024 bytes), allowing for efficient transmission and memory management.
-   **Multi-Threaded Download:** The client uses multiple threads (default 4) to download different parts of a file simultaneously, significantly speeding up the transfer process.
-   **CRC32 Data Integrity:** Every packet includes a CRC32 checksum to detect data corruption during transmission. Corrupted packets are discarded and recompute/retransmitted.

### 3. Advanced Client-Server Architecture
-   **Resumable/Selective Downloads:** The client tracks which chunks have been received (`ThreadTracker`), preventing redownload of existing data and allowing disjoint chunks to be assembled correctly.
-   **Metadata Handshake:** Before transfer, the client requests file metadata (`REQUEST_METADATA`) to understand file size and chunk count, ensuring proper buffer allocation.
-   **Dynamic Server Listing:** The server scans its hosted `files/` directory and updates the available file list (`server_files.txt`) for clients to query.

## 📂 Directory Structure

```text
udp-file-transfer/
├── client/
│   ├── client.cpp          # Main client implementation (Multi-threaded download logic)
│   ├── client.h            # Client headers, structs (Metadata, ReceivedChunk), and constants
│   └── input.txt           # Configuration file listing files the client calls to download
├── server/
│   ├── files/              # Hosting directory (place files here to be downloaded)
│   ├── server.cpp          # Main server implementation (Request handling, UDP socket logic)
│   ├── server.h            # Server headers, structs (Connected_device), and constants
│   └── server_files.txt    # generated cache of available files on server
└── README.md               # Project documentation
```

## 🚀 Getting Started

### Prerequisites
-   **C++ Compiler** (g++ recommended) supports C++11 or higher.
-   **OS:** Designed for Linux/Unix environments (uses `<arpa/inet.h>`, `<unistd.h>`, `<sys/socket.h>`), but includes some Windows compatibility headers.

### Compilation

**Server:**
```bash
g++ server/server.cpp -o server_app -lpthread
```

**Client:**
```bash
g++ client/client.cpp -o client_app -lpthread
```

### Usage

1.  **Start the Server:**
    ```bash
    ./server_app
    ```
    The server listens on port `12345`.

2.  **Start the Client:**
    ```bash
    ./client_app
    ```
    -   Enter the Server IP (default `127.0.0.1`).
    -   The client will fetch the list of available files.
    -   Edit `client/input.txt` to specify files you want to download if needed (or follow on-screen prompts if implemented).

## ⚙️ Configuration
Key constants can be tweaked in `client.h` and `server.h`:
-   `BUFFER_SIZE`: Packet size limit (default 4096).
-   `CHUNK_SIZE`: Size of file chunks (default 1024).
-   `NUM_DOWNLOAD_THREADS`: Number of concurrent download threads (default 4).
-   `ACK_TIMEOUT`: Time before retransmitting a packet (default 200ms).
