// client.h
#ifndef CLIENT_H
#define CLIENT_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <limits>
#include <thread>
#include <dirent.h>
#include <mutex>
#include <condition_variable>
#include <map>
#include <set>
#include <filesystem>
#include <csignal>
#include <numeric>
#include <atomic>
#include <unordered_map>
#include <regex>
#include <set>
#include <fcntl.h>
#include <cstdio>
#include <errno.h> // For errno
#include <iostream>
#include <stdexcept>  // Để sử dụng std::runtime_error

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/types.h>
#include <sys/stat.h>
#endif

#define CONSOLE_HEIGHT 25
#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define TOKEN_LIMIT 200 // tokens per second
#define SENDING_TIMEOUT 200
#define REFRESH_CONSOLE 1000
#define SERVER_LIST_FILE "server_files.txt"
#define CLIENT_LIST_FILE "input.txt"
#define NUM_DOWNLOAD_THREADS 4
#define DOWNLOADS_DIR "downloads/"
#define MAX_RETRIES 3
#define RETRY_DELAY_MS 200
#define MAX_FILENAME_LENGTH 256

#define REQUEST_METADATA "REQUEST_METADATA"
#define REQUEST_CHUNK "REQUEST_CHUNK"
#define REPLY "REPLY"

#pragma pack(push, 1)
struct Metadata {
    uint64_t file_size;
    uint64_t num_chunks;
    uint64_t chunk_size;
};

struct ReceivedChunk {
    uint64_t id;
    std::vector<char> data;

    bool operator<(const ReceivedChunk& other) const {
        return id < other.id;
    }
};

struct ThreadTracker {
    uint64_t total_chunk = 0;
    uint64_t downloaded_chunk = 0;
    std::set<uint64_t> downloading_chunk;
};

struct AckPacket {
    char type; // 'A' for ACK
    uint64_t seq_num;
};
#pragma pack(pop)

uint64_t ntohll(uint64_t value);
uint64_t htonll(uint64_t value);

// Biến toàn cục để theo dõi các file đã được xử lý
extern std::set<std::string> processed_files;
extern std::mutex processed_files_mutex;


#endif // CLIENT_H