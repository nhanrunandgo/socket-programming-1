#ifndef CLIENT_H
#define CLIENT_H

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <map>
#include <filesystem>
#include <csignal>
#include <atomic>
#include <netinet/in.h>

/** COMMANDS **/
#define REQUEST_METADATA "REQUEST_METADATA"     // Structure: REQUEST_METADATA:filename
#define REQUEST_CHUNK "REQUEST_CHUNK"           // Structure: REQUEST_CHUNK:filename:chunk_id

/*
Structure: REPLY:Seq#:Command
Including:
Server -> Client
- REPLY:0:CHUNK:filename:id:data
- REPLY:0:META:filename:data
- REPLY:0:ERROR:BAD REQUEST

Client -> Server
- REPLY:0:ACK
*/
#define REPLY "REPLY"
#define BAD_REQUEST "ERROR:BAD REQUEST\0"
#define CHUNK_CMD "CHUNK"
#define META_CMD "META"
#define ACK_CMD "ACK"
#define REQUEST_FILE_LIST "GET_FILE_LIST"

/** DEFINITIONS **/
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 1024
#define MAX_FILENAME_LENGTH 256
#define INPUT_FILE "input.txt"
#define DOWNLOADED_FILES_LOG "downloaded.log"
#define NUM_DOWNLOAD_THREADS 4
#define MAX_RETRIES 3
#define ACK_TIMEOUT 2000 // milliseconds

/*-------------------Structures-------------------*/
#pragma pack(push, 1) // No padding activated
/// @brief To use when handling reply meta of a file
struct Metadata {
    uint64_t file_size;     // Size of file
    uint64_t num_chunk;      // Number of chunk
    uint64_t chunk_size;    // Chunk size
};
#pragma pack(pop) // Release padding (normal mode)

/// @brief Represents a chunk of a file being downloaded
struct FileChunk {
    std::vector<char> data;
    bool received;
    std::chrono::steady_clock::time_point last_received_time;
};

/// @brief Represents the download progress of a file
struct FileDownloadInfo {
    std::string filename;
    uint64_t file_size;
    uint64_t num_chunks;
    std::vector<FileChunk> chunks;
    std::vector<bool> requested_chunks;
    std::atomic<uint32_t> received_chunks;
    std::unique_ptr<std::mutex> progress_mutex;

    // Default constructor
    FileDownloadInfo() : file_size(0), num_chunks(0), received_chunks(0) {}

    // Constructor nhận các đối số khởi tạo
    FileDownloadInfo(std::string filename, uint64_t file_size, uint64_t num_chunks,
                     std::vector<FileChunk> chunks, std::vector<bool> requested_chunks,
                     uint32_t received_chunks, std::unique_ptr<std::mutex> progress_mutex)
        : filename(std::move(filename)), file_size(file_size), num_chunks(num_chunks),
          chunks(std::move(chunks)), requested_chunks(std::move(requested_chunks)),
          received_chunks(received_chunks), progress_mutex(std::move(progress_mutex)) {}

    // Move constructor
    FileDownloadInfo(FileDownloadInfo&& other) noexcept
        : filename(std::move(other.filename)),
          file_size(other.file_size),
          num_chunks(other.num_chunks),
          chunks(std::move(other.chunks)),
          requested_chunks(std::move(other.requested_chunks)),
          received_chunks(other.received_chunks.load()),
          progress_mutex(std::move(other.progress_mutex)) {}

    // Move assignment operator
    FileDownloadInfo& operator=(FileDownloadInfo&& other) noexcept {
        if (this != &other) {
            filename = std::move(other.filename);
            file_size = other.file_size;
            num_chunks = other.num_chunks;
            chunks = std::move(other.chunks);
            requested_chunks = std::move(other.requested_chunks);
            received_chunks = other.received_chunks.load();
            progress_mutex = std::move(other.progress_mutex);
        }
        return *this;
    }

    // Vô hiệu hóa copy constructor và copy assignment operator
    FileDownloadInfo(const FileDownloadInfo&) = delete;
    FileDownloadInfo& operator=(const FileDownloadInfo&) = delete;
};

/*-------------------Global variables-------------------*/
std::unordered_map<std::string, FileDownloadInfo> downloading_files;
std::mutex downloading_files_mutex;
std::condition_variable downloading_files_cv;
std::atomic<bool> running{true};
std::mutex received_ack_mutex;
std::condition_variable received_ack_cv;
std::map<uint64_t, std::chrono::steady_clock::time_point> sent_packets;
std::mutex sent_packets_mutex;
std::map<uint64_t, std::pair<sockaddr_in, size_t>> pending_sends;
std::mutex pending_sends_mutex;
std::atomic<uint64_t> next_sequence_number{0};
std::unordered_map<std::string, bool> processed_files;
std::mutex processed_files_mutex;

/*-------------------Functions-------------------*/
uint64_t generate_sequence_number();
void send_packet(int sock, const sockaddr_in& server_addr, const char* buffer, size_t len);
void send_request_metadata(int sock, const sockaddr_in& server_addr, const std::string& filename);
void send_request_chunk(int sock, const sockaddr_in& server_addr, const std::string& filename, uint64_t chunk_id);
void receive_reply(int sock);
void process_metadata_reply(const char* buffer, size_t len);
void process_chunk_reply(const char* buffer, size_t len);
void handle_download(const std::string& filename);
void download_chunk(int sock, const sockaddr_in& server_addr, const std::string& filename, uint64_t chunk_id);
void monitor_download_progress();
void read_input_file();
void load_processed_files();
void save_processed_files(const std::string& filename);
void assemble_file(const FileDownloadInfo& download_info);
void signal_handler(int signum);
void resend_timeout_packets(int sock, const sockaddr_in& server_addr);

#endif // CLIENT_H