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
#include <mutex>
#include <condition_variable>
#include <map>
#include <set>
#include <filesystem>
#include <csignal>
#include <numeric>
#include <atomic>
#include <errno.h> // For errno
#include <cstdint> // For uint32_t

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/types.h>
#include <sys/stat.h>
#endif

#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define FILE_LIST_FILE "server_files.txt"
#define INPUT_FILE "input.txt"
#define NUM_DOWNLOAD_THREADS 4
#define DOWNLOADS_DIR "downloads/"
#define MAX_RETRIES 3
#define RETRY_DELAY_MS 1000
#define MAX_FILENAME_LENGTH 256

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

// Hàm đọc danh sách file mới từ input.txt
std::vector<std::string> get_new_files();

// Hàm gửi ACK (có thể cần điều chỉnh hoặc loại bỏ tùy thuộc vào server)
void send_ack(int sock, const sockaddr_in& server_addr, socklen_t addr_len, uint64_t seq_num);

// Hàm gửi yêu cầu metadata đến server
bool request_metadata(int sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, Metadata& metadata, uint64_t seq_num);

// Hàm gửi yêu cầu chunk và nhận dữ liệu
bool download_chunk(int sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, uint64_t chunk_id, std::vector<char>& data, uint64_t seq_num);

// Hàm worker cho mỗi thread download
void download_worker(int client_sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, uint64_t start_chunk, uint64_t end_chunk, std::map<uint64_t, std::vector<char>>& received_chunks, std::atomic<uint64_t>& chunks_received_count, uint64_t total_chunks, std::atomic<uint64_t>& next_seq_num, std::atomic<uint64_t>& thread_chunks_received, uint64_t total_thread_chunks);

// CRC-32 function declaration
uint32_t calculate_crc32(const char* data, size_t length);

#endif // CLIENT_H