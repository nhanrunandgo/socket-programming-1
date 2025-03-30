#include <iostream>
#include <cstring>
#include <unistd.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>
#include <time.h>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <unordered_map>
#include "server.h"

/*-------------------Structures-------------------*/
#pragma pack(push, 1)         // No padding activated
/// @brief To use when handling request meta of a file
struct Metadata {
    uint64_t file_size;      // Size of file
    uint64_t num_chunk;      // Number of chunk
    uint64_t chunk_size;     // Chunk size
};
#pragma pack(pop)             // Release padding (normal mode)

/// @brief To use to save and track (IP, port) pairs connected to this socket
struct Connected_device {
    in_addr_t IP_addr;       // IP address
    in_port_t port;           // Port number
    uint64_t *seq_number;     // Current ACK sequence number
};

/// @brief To use to manage sent packets status
struct PendingPacket {
    std::chrono::steady_clock::time_point send_time;
    int retry_count;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
    sockaddr_in client_addr;
    bool needs_retry; // Add flag to manage retry
};

/*-------------------Global variables-------------------*/
time_t last_reload = INT16_MIN; // -INF
std::map<std::pair<in_addr_t, in_port_t>, uint64_t> connected_device; // (IP, port) => ACK
std::atomic<bool> running{true};           // Flag to control thread
std::mutex packets_mtx;                   // Mutex for syncing
std::condition_variable timeout_cv;       // Condition variable
std::unordered_map<uint64_t, PendingPacket> pending_packets;

/*-------------------Functions-------------------*/
/// @brief Convert from host order (Little endian/Big endian) to network order (Big endian)
uint64_t htonll(uint64_t value);
/// @brief Update list of files to download
void update_list();
/// @brief Return true if we need to refresh list
bool isTimeout();
/// @brief Convert from host order (Little endian/Big endian) to network order (Big endian)
void handle_fullname_getter(char* fullpath, char* &filename);
/// @brief Handle metadata requests (REQUEST_METADATA:filename)
void handle_metadata_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer);
/// @brief Handle chunk requests (REQUEST_CHUNK:filename:chunk_number)
void handle_chunk_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer);
/// @brief Handle all replies to clients
void handle_reply_to_client(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer, size_t buffer_len);
/// @brief Handle ACK reply from client
void handle_reply_from_client(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer, size_t buffer_len);
/// @brief
void timeout_checker_thread(int server_sock);

int main() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(sockaddr_in);
    char buffer[BUFFER_SIZE];

    // Create UDP socket
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock_fd < 0) {// Error creating socket
        std::cout << "Error initializing socket\n";
        return 404;
    }

    // Configure server IP/port
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   // IP address
    server_addr.sin_port = htons(SERVER_PORT); // Port

    // Bind socket with IP/port
    if (bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "Error binding socket\n";
        close(sock_fd);
        return 404;
    }

    std::cout << "UDP Server is running on port: " << SERVER_PORT << "...\n";

    // Start timeout thread
    std::thread timeout_thread(timeout_checker_thread, sock_fd);

    while(true) {
        // Load from socket...
        int recv_len = recvfrom(sock_fd, buffer, BUFFER_SIZE - 1, 0,
                                    (struct sockaddr*)&client_addr, &client_len);

        // Any incoming data, solve it
        if(recv_len > 0) {
            buffer[recv_len] = '\0'; // Add to make sure the data has ending point

            // Debug
            std::cout << "\n>>> Received from [" << inet_ntoa(client_addr.sin_addr) << ":"
                      << ntohs(client_addr.sin_port) << "]: " << buffer << "\n";

            // Handle all replies (Commonly ACK replies)
            if (strncmp(buffer, REPLY, strlen(REPLY)) == 0) {
                handle_reply_from_client(sock_fd, client_addr, client_len, buffer, recv_len);
            }

            // Handle metadata requests (REQUEST_METADATA:filename)
            else if (strncmp(buffer, REQUEST_METADATA, strlen(REQUEST_METADATA)) == 0) {
                handle_metadata_request(sock_fd, client_addr, client_len, buffer);
            }

            // Handle chunk requests (REQUEST_CHUNK:filename:chunk_number)
            else if (strncmp(buffer, REQUEST_CHUNK, strlen(REQUEST_CHUNK)) == 0) {
                handle_chunk_request(sock_fd, client_addr, client_len, buffer);
            }
            else {
                handle_reply_to_client(sock_fd, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
            }
        }
        timeout_cv.notify_one();
    }

    // Clean up
    running = false;
    timeout_cv.notify_all();
    timeout_thread.join();
    close(sock_fd);
    return 0;
}
/// @brief Update list of files to download
void update_list() {
    std::cout << "Database is old. Reloading...\n";
    DIR *dir = opendir(DOWNLOAD_DIR);

    // Unable to open this path
    if (dir == 0) {
        std::cout << "Error opening path: " << DOWNLOAD_DIR << "\n";
        return;
    }

    struct dirent *entry;
    struct stat file_stat;

    // Write to file
    FILE* file = fopen(DOWNLOAD_LIST, "w");
    if (file == NULL) {
        std::cout << "Error: Unable to create/open file " << DOWNLOAD_LIST << "\n";
        exit(0);
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", DOWNLOAD_DIR, entry->d_name);

        if (stat(full_path, &file_stat) == -1) {
            fprintf(stderr, "Error reading %s: %s\n", entry->d_name, strerror(errno));
            continue;
        }

        printf("%-30s %-15ld\n", entry->d_name, file_stat.st_size);        // Print to screen
        fprintf(file, "%s %ld\n", entry->d_name, file_stat.st_size);        // Write to file
    }
    fclose(file);
    closedir(dir);
}

/// @brief Return true if we need to refresh list
bool isTimeout() {
    time_t current_time = time(NULL); // Get current time

    if (difftime(current_time, last_reload) >= RELOAD_INTERVAL) {
        last_reload = current_time;
        return true;
    }
    return false;
}

/// @brief Convert from host order (Little endian/Big endian) to network order (Big endian)
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32 | htonl(value >> 32));
}

void handle_fullname_getter(char* fullpath, char* &filename) {
    // Special request: List file
    if (strncmp(filename, DOWNLOAD_LIST, strlen(DOWNLOAD_LIST)) == 0) {
        // If data is old, update it first
        if (isTimeout()) {
            update_list();
        }
        // Special file then get that file in the main directory
        strcpy(fullpath, filename);
    }
    // Normal file then go to specific download directory to get file
    else {
        sprintf(fullpath, "%s/%s", DOWNLOAD_DIR, filename);
    }
    std::cerr << fullpath << "\n";
}

/// @brief Handle metadata requests (REQUEST_METADATA:filename)
void handle_metadata_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    std::cout << "[SERVER] Entering handle_metadata_request\n";
    char fullpath[MAX_FILE_LENGTH * 2];
    char* token = strtok(buffer, ":"); // Initialize token splitter

    // Get filename
    token = strtok(NULL, ":");

    // No name detected (wrong structure)
    if (token == NULL) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    char* filename = token;

    handle_fullname_getter(fullpath, filename);

    struct stat file_stat; // Get file information (eg. file size...)
    Metadata meta = {0};
    int fd = open(fullpath, O_RDONLY);

    // If file is exists then calculate
    if (fd != -1 && fstat(fd, &file_stat) == 0) {
        meta.file_size = file_stat.st_size;
        meta.chunk_size = CHUNK_SIZE;    // Lấy chunk size mặc định
        meta.num_chunk = (file_stat.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
        close(fd);
    }
    // If unable to open file
    else {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    //Debug
    std::cout << "\n--- Metadata ---\n"
              << "File: " << filename << "\n"
              << "Size: " << meta.file_size << " bytes\n"
              << "Chunk count: " << meta.num_chunk << "\n"
              << "Chunk size: " << meta.chunk_size << " bytes\n"
              << "----------------\n";

    // Make a metadata copy with network order (big endian)
    Metadata net_meta;
    net_meta.file_size = htonll(meta.file_size);      // Hàm tự định nghĩa cho 64-bit
    net_meta.num_chunk = htonll(meta.num_chunk);
    net_meta.chunk_size = htonll(meta.chunk_size);

    // Make a reply message
    char message[BUFFER_SIZE];
    snprintf(message, sizeof(message), "META:%s:", filename); // Header

    // Copy data to message
    size_t header_len = strlen(message);
    if (header_len + sizeof(net_meta) <= BUFFER_SIZE) {
        memcpy(message + header_len, &net_meta, sizeof(net_meta));
        size_t total_len = header_len + sizeof(net_meta);
        // Chú ý: Hàm handle_reply_to_client đã tự thêm header REPLY và sequence number
        handle_reply_to_client(server_sock, client_addr, client_len, message, total_len);
        printf("Debug in meta func: Header: %s, Metadata size: %zu bytes\n", message, sizeof(net_meta));
        return;
    } else {
        std::cerr << "[SERVER] Metadata phản hồi quá lớn.\n";
        handle_reply_to_client(server_sock, client_addr, client_len, INTERNAL_ERROR, strlen(INTERNAL_ERROR));
        return;
    }
}

/// @brief Handle chunk requests (REQUEST_CHUNK:filename:chunk_number)
void handle_chunk_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    char filename[MAX_FILE_LENGTH];
    char fullpath[MAX_FILE_LENGTH * 2];
    char reply_message[BUFFER_SIZE];
    struct stat file_stat;
    char* tok = strtok(buffer, ":"); // Initialize token splitter

    tok = strtok(NULL, ":");      // Filename
    // Name does not exist
    if(tok == NULL) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }
    strncpy(filename, tok, sizeof(filename) - 1);
    handle_fullname_getter(fullpath, tok);

    //Debug
    std::cout << "\n--- REQUEST ---\n"
              << "File: " << filename << "\n";

    tok = strtok(NULL, ":");      // Chunk ID
    // Chunk ID does not exist
    if(tok == NULL) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }
    uint64_t chunk_index = std::strtoul(tok, nullptr, 10);

    //Debug
    std::cout<< "Chunk ID: " << chunk_index << "\n"
             << "----------------\n";

    // Read file
    int fd = open(fullpath, O_RDONLY);

    // File do not open
    if (fd == -1) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    // Unable to read file information
    if (fstat(fd, &file_stat) != 0) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    uint32_t file_size = file_stat.st_size;
    uint32_t num_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // If request chunk ID exceeded accepted range
    if (chunk_index >= num_chunks) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    // Calculate offset and actual chunk size
    off_t offset = static_cast<off_t>(chunk_index) * CHUNK_SIZE;
    size_t actual_chunk_size = (chunk_index == num_chunks - 1) ?
                               (file_size % CHUNK_SIZE ? file_size % CHUNK_SIZE : CHUNK_SIZE) : CHUNK_SIZE;

    // Read chunk data from file
    char chunk_data[CHUNK_SIZE];
    lseek(fd, offset, SEEK_SET);
    ssize_t read_len = read(fd, chunk_data, actual_chunk_size);

    // File has been changed or modified (smaller than expect)
    if (read_len != actual_chunk_size) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }

    // Create a reply
    snprintf(reply_message, sizeof(reply_message), "CHUNK:%s:%lu:", filename, chunk_index);
    size_t header_len = strlen(reply_message);
    memcpy(reply_message + header_len, chunk_data, actual_chunk_size);
    size_t total_len = header_len + actual_chunk_size;
    handle_reply_to_client(server_sock, client_addr, client_len, reply_message, total_len);
}

/** TIMEOUT THREAD **/
void timeout_checker_thread(int server_sock) {
    while(running) {
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> to_remove;

        {
            std::unique_lock<std::mutex> lock(packets_mtx);

            for(auto& [seq_num, packet] : pending_packets) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - packet.send_time);

                if(elapsed.count() > ACK_TIMEOUT) {
                    if(packet.retry_count < MAX_RETRIES) {
                        // Resend packet
                        sendto(server_sock, packet.buffer, packet.buffer_len, 0,
                               (sockaddr*)&packet.client_addr, sizeof(packet.client_addr));

                        packet.retry_count++;
                        packet.send_time = now;
                        std::cout << "[RETRY] Seq " << seq_num << " (attempt "
                                  << packet.retry_count << ")\n";
                    } else {
                        to_remove.push_back(seq_num);
                        std::cout << "[DROP] Seq " << seq_num << " (max retries)\n";
                    }
                }
            }

            // Remove expired packets
            for(auto seq : to_remove) {
                pending_packets.erase(seq);
            }
        }

        // Wait with timeout
        std::unique_lock<std::mutex> lock(packets_mtx);
        timeout_cv.wait_for(lock, std::chrono::milliseconds(500), [&]{
            return !running.load();
        });
    }
}

/** HANDLER FUNCTIONS **/
void handle_reply_to_client(int server_sock, sockaddr_in &client_addr,
                            socklen_t &client_len, char* buffer, size_t buffer_len) {
    std::lock_guard<std::mutex> lock(packets_mtx);

    // Get sequence number
    auto key = std::make_pair(client_addr.sin_addr.s_addr, client_addr.sin_port);
    uint64_t& seq_num = connected_device[key];
    uint64_t current_seq = seq_num++;

    // Build message
    char message[BUFFER_SIZE];
    snprintf(message, sizeof(message), "%s:%lu:", REPLY, current_seq);
    size_t header_len = strlen(message);
    memcpy(message + header_len, buffer, buffer_len);
    size_t total_len = header_len + buffer_len;

    // Save to pending
    PendingPacket packet {
        .send_time = std::chrono::steady_clock::now(),
        .retry_count = 0,
        .buffer_len = total_len,
        .client_addr = client_addr
    };
    memcpy(packet.buffer, message, total_len);

    pending_packets[current_seq] = packet;

    // Initial send
    sendto(server_sock, message, total_len, 0,
           (sockaddr*)&client_addr, client_len);

    std::cout << "[SEND] Seq " << current_seq << " to "
              << inet_ntoa(client_addr.sin_addr) << "\n";
    timeout_cv.notify_one();
}

void handle_reply_from_client(int server_sock, sockaddr_in &client_addr,
                                    socklen_t &client_len, char* buffer, size_t buffer_len) {
    char* seq_start = strchr(buffer, ':') + 1;
    uint64_t seq_num = std::stoull(seq_start);

    std::lock_guard<std::mutex> lock(packets_mtx);
    if(pending_packets.erase(seq_num)) {
        std::cout << "[ACK] Seq " << seq_num << " from "
                  << inet_ntoa(client_addr.sin_addr) << "\n";
    }
}