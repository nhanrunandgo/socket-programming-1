#include <iostream>
#include <cstring>
#include <unistd.h>
#include <dirent.h>         // Directory tasks
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>
#include <time.h>
#include <vector>
#include <map>
#include "socket.h"

/*-------------------Structures-------------------*/
#pragma pack(push, 1)       // No padding activated
/// @brief To use when handling request meta of a file
struct Metadata {
    uint64_t file_size;     // Size of file
    uint64_t num_chunk;     // Number of chunk
    uint64_t chunk_size;    // Chunk size
};
#pragma pack(pop)           // Release padding (normal mode)

/// @brief To use to save and track (IP, port) pairs connected to this socket
struct Connected_device {
    in_addr_t IP_addr;      // IP address
    in_port_t port;         // Port number
    uint64_t *seq_number;    // Current ACK sequence number
};

/*-------------------Global variables-------------------*/
time_t last_reload = INT16_MIN; // -INF
std::map<std::pair<in_addr_t, in_port_t>, uint64_t> connected_device; // (IP, port) => ACK 

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
    server_addr.sin_port = htons(SERVER_PORT);  // Port

    // Bind socket with IP/port
    if (bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "Error binding socket\n";
        close(sock_fd);
        return 404;
    }

    std::cout << "UDP Server is running on port: " << SERVER_PORT << "...\n";

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
                std::cout << "ACK\n";
                handle_reply_from_client(sock_fd, client_addr, client_len, buffer, recv_len);
                continue;
            }

            // Handle metadata requests (REQUEST_METADATA:filename)
            if (strncmp(buffer, REQUEST_METADATA, strlen(REQUEST_METADATA)) == 0) {
                std::cout << "META\n";
                handle_metadata_request(sock_fd, client_addr, client_len, buffer);
                continue;
            }

            // Handle chunk requests (REQUEST_CHUNK:filename:chunk_number)
            if (strncmp(buffer, REQUEST_CHUNK, strlen(REQUEST_CHUNK)) == 0) {
                std::cout << "CHUNK\n";
                handle_chunk_request(sock_fd, client_addr, client_len, buffer);
                continue;
            }

            handle_reply_to_client(sock_fd, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        }
    }
}

/// @brief Update list of files to download
void update_list() { 
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

        printf("%-30s %-15ld\n", entry->d_name, file_stat.st_size);     // Print to screen
        fprintf(file, "%s %ld\n", entry->d_name, file_stat.st_size);     // Write to file
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
        sprintf(fullpath, "%s%s", DOWNLOAD_DIR, filename);
    }
}

/// @brief Handle metadata requests (REQUEST_METADATA:filename)
void handle_metadata_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    char fullpath[MAX_FILE_LENGTH * 2];
    char* filename = strtok(buffer, ":"); // Initialize token splitter

    // Get filename
    filename = strtok(NULL, ":");

    // No name detected (wrong structure)
    if (filename == NULL) {
        handle_reply_to_client(server_sock, client_addr, client_len, BAD_REQUEST, strlen(BAD_REQUEST));
        return;
    }
    
    handle_fullname_getter(fullpath, filename);
    
    struct stat file_stat;  // Get file information (eg. file size...)
    Metadata meta = {0};
    int fd = open(fullpath, O_RDONLY);

    // If file is exists then calculate 
    if (fd != -1 && fstat(fd, &file_stat) == 0) {  
        meta.file_size = file_stat.st_size;
        meta.chunk_size = CHUNK_SIZE;   // Lấy chunk size mặc định
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
    net_meta.file_size = htonll(meta.file_size);    // Hàm tự định nghĩa cho 64-bit
    net_meta.num_chunk = htonll(meta.num_chunk);
    net_meta.chunk_size = htonll(meta.chunk_size); 

    // Make a reply message
    char message[BUFFER_SIZE];
    snprintf(message, sizeof(message), "META:%s:", filename); // Header

    // Copy data to message
    size_t header_len = strlen(message);
    memcpy(message + header_len, &net_meta, sizeof(net_meta));
    size_t total_len = header_len + sizeof(net_meta) + 1;
    message[total_len - 1] = '\0';

    printf("Debug in meta func: %s\n", message);
    handle_reply_to_client(server_sock, client_addr, client_len, message, total_len);
}

/// @brief Handle chunk requests (REQUEST_CHUNK:filename:chunk_number)
void handle_chunk_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    char filename[MAX_FILE_LENGTH];
    char fullpath[MAX_FILE_LENGTH * 2];
    char reply_message[BUFFER_SIZE];
    struct stat file_stat;
    char* tok = strtok(buffer, ":"); // Initialize token splitter
    
    tok = strtok(NULL, ":");    // Filename
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

    tok = strtok(NULL, ":");    // Chunk ID
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
    size_t total_len = header_len + actual_chunk_size + 1;
    reply_message[total_len - 1] = '\0';
    handle_reply_to_client(server_sock, client_addr, client_len, reply_message, total_len);
}

/// @brief Handle all replies to clients
void handle_reply_to_client(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer, size_t buffer_len) {
    char message[BUFFER_SIZE];
    
    Connected_device client;
    client.IP_addr = client_addr.sin_addr.s_addr;
    client.port = client_addr.sin_port;
    client.seq_number = &connected_device[{client.IP_addr, client.port}];

    snprintf(message, sizeof(message), "%s:%lu:", REPLY, *client.seq_number++);
    
    size_t header_len = strlen(message);
    memcpy(message + header_len, buffer, buffer_len);
    size_t total_len = header_len + buffer_len + 1;
    message[total_len - 1] = '\0';

    printf("Debug in reply func: %s\n", buffer);

    sendto(server_sock, message, strlen(message), 0,
                (struct sockaddr*)&client_addr, client_len);
}

/// @brief Handle ACK reply from client
void handle_reply_from_client(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer, size_t buffer_len) {
    
}