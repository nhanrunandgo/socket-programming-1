#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>
#include "socket.h"

#pragma pack(push, 1)
struct Metadata {
    uint64_t file_size;     // Kích thước file
    uint64_t num_chunks;    // Số lượng chunk
    uint64_t chunk_size;    // Kích thước chunk
};  // Cấu trúc meta của file

#pragma pack(pop)   // Đóng gói packet

// Hàm chuyển đổi 64-bit host to network (đưa Little/Big Endian -> Big Endian (Chuẩn chung của network))
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32 | htonl(value >> 32));
}

/// @brief Hàm chạy khi client gửi sai cú pháp, lỗi file hoặc gói tin bị lỗi. Hàm gửi trả gói tin REPLY_CHUNK
/// @param server_sock 
/// @param client_addr 
/// @param client_len 
void handle_wrong_command(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len) {
    char error_reply[BUFFER_SIZE];
    snprintf(error_reply, sizeof(error_reply), "%sERROR_COMMAND", REPLY);
    sendto(server_sock, error_reply, strlen(error_reply), 0,
           (struct sockaddr*)&client_addr, client_len);
}

void handle_metadata_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    char filename[256];
                strncpy(filename, buffer + strlen(REQUEST_METADATA), sizeof(filename) - 1); // Trích xuất filename từ buffer
                filename[sizeof(filename) - 1] = '\0';

                // Tạo đường dẫn đầy đủ
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s%s", FILES_DIR, filename); // Ghép thành path dạng: files/filename

                struct stat file_stat;  // Struct để lấy thông tin file (file size...)
                Metadata meta = {0};
                int fd = open(fullpath, O_RDONLY);
                
                // Nếu file tồn tại
                if (fd != -1 && fstat(fd, &file_stat) == 0) {  
                    meta.file_size = file_stat.st_size;
                    meta.chunk_size = CHUNK_SIZE;   // Lấy chunk size mặc định
                    meta.num_chunks = (file_stat.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
                    close(fd);
                } 
                // File lỗi hoặc không tồn tại thì báo lỗi
                else {
                    char error_reply[BUFFER_SIZE];
                    snprintf(error_reply, sizeof(error_reply), "%sERROR_METADATA:%s:ERROR_OPEN", REPLY, filename);
                    sendto(server_sock, error_reply, strlen(error_reply), 0,
                            (struct sockaddr*)&client_addr, client_len);
                    return;
                }
                
                //Debug
                std::cout << "\n--- Metadata ---\n"
                          << "File: " << filename << "\n"
                          << "Kích thước: " << meta.file_size << " bytes\n"
                          << "Số chunk: " << meta.num_chunks << "\n"
                          << "Kích thước chunk: " << meta.chunk_size << " bytes\n"
                          << "----------------\n";
                
                // Tạo bản sao đã chuyển đổi byte order
                Metadata net_meta;
                net_meta.file_size = htonll(meta.file_size);    // Hàm tự định nghĩa cho 64-bit
                net_meta.num_chunks = htonll(meta.num_chunks);
                net_meta.chunk_size = htonll(meta.chunk_size); 

                // Đóng gói dữ liệu
                char reply[BUFFER_SIZE];
                snprintf(reply, sizeof(reply), "%s%s:", REPLY_METADATA, filename);

                // Copy dữ liệu metadata vào buffer
                size_t header_len = strlen(reply);
                memcpy(reply + header_len, &net_meta, sizeof(net_meta));
                size_t total_len = header_len + sizeof(net_meta);

                sendto(server_sock, reply, total_len, 0,
                      (struct sockaddr*)&client_addr, client_len);
                printf("%s\n", reply);
}

void handle_chunk_request(int server_sock, struct sockaddr_in &client_addr, socklen_t &client_len, char* buffer) {
    char *end_name_colon = strchr(buffer + strlen(REQUEST_CHUNK), ':'); // Kết thúc filename

    if (end_name_colon != NULL) {
        *end_name_colon = '\0'; 
        char filename[MAX_FILE_LENGTH];
        strncpy(filename, buffer + strlen(REQUEST_CHUNK), sizeof(filename) - 1);
        filename[sizeof(filename) - 1] = '\0';
        
        uint64_t chunk_index = std::strtoul(end_name_colon + 1, nullptr, 10);

        //Debug
        std::cout << "\n--- Phân giải ---\n"
                    << "File: " << filename << "\n"
                    << "Chunk ID: " << chunk_index << "\n"
                    << "----------------\n";
        
        // Tạo đường dẫn đầy đủ (files/filename)
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s%s", FILES_DIR, filename);
        
        // Tiến hành đọc file
        int fd = open(fullpath, O_RDONLY);

        // File mở được
        if (fd != -1) {
            struct stat file_stat;
            // Lấy được thông tin của file (file size...)
            if (fstat(fd, &file_stat) == 0) {
                uint32_t file_size = file_stat.st_size;
                uint32_t num_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
                
                // Nếu chunk_id trong request nằm trong phạm vi file
                if (chunk_index < num_chunks) {
                    // Tính offset và kích thước thật của chunk 
                    off_t offset = static_cast<off_t>(chunk_index) * CHUNK_SIZE;
                    size_t actual_chunk_size = (chunk_index == num_chunks - 1) ?
                        (file_size % CHUNK_SIZE ? file_size % CHUNK_SIZE : CHUNK_SIZE) : CHUNK_SIZE; 
                    
                    // Đọc dữ liệu chunk
                    char chunk_data[CHUNK_SIZE];
                    lseek(fd, offset, SEEK_SET);
                    ssize_t read_len = read(fd, chunk_data, actual_chunk_size);
                    
                    if (read_len == actual_chunk_size) {
                        // Tạo gói tin phản hồi REPLY_CHUNK (REPLY_CHUNK:filename:id:data)
                        char reply[BUFFER_SIZE];
                        snprintf(reply, sizeof(reply), "%s%s:%lu:", REPLY_CHUNK, filename, chunk_index);
                        size_t header_len = strlen(reply);
                        memcpy(reply + header_len, chunk_data, actual_chunk_size);
                        size_t total_len = header_len + actual_chunk_size;
                        
                        sendto(server_sock, reply, total_len, 0,
                                (struct sockaddr*)&client_addr, client_len);
                    } else {
                        // Lỗi đọc file
                        char error_reply[BUFFER_SIZE];
                        snprintf(error_reply, sizeof(error_reply), "%sERROR_CHUNK:%s:%lu:ERROR_READ", REPLY, filename, chunk_index);
                        sendto(server_sock, error_reply, strlen(error_reply), 0,
                                (struct sockaddr*)&client_addr, client_len);
                    }
                } else {
                    // Chunk_id không hợp lệ (nằm ngoài phạm vi file)
                    char error_reply[BUFFER_SIZE];
                    snprintf(error_reply, sizeof(error_reply), "%sERROR_CHUNK:%s:%lu:ERROR_INDEX", REPLY, filename, chunk_index);
                    sendto(server_sock, error_reply, strlen(error_reply), 0,
                            (struct sockaddr*)&client_addr, client_len);
                }
                close(fd);
            } else {
                // Không mở được file
                char error_reply[BUFFER_SIZE];
                snprintf(error_reply, sizeof(error_reply), "%sERROR_CHUNK:%s:%lu:ERROR_OPEN", REPLY, filename, chunk_index);
                sendto(server_sock, error_reply, strlen(error_reply), 0,
                        (struct sockaddr*)&client_addr, client_len);
            }
        }
        else {
            // Không mở được file
            char error_reply[BUFFER_SIZE];
            snprintf(error_reply, sizeof(error_reply), "%sERROR_CHUNK:%s:%lu:ERROR_OPEN", REPLY, filename, chunk_index);
            sendto(server_sock, error_reply, strlen(error_reply), 0,
                    (struct sockaddr*)&client_addr, client_len);
        }
    }
    else {
        handle_wrong_command(server_sock, client_addr, client_len);
    }
}

int main() {
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[BUFFER_SIZE];

    // Tạo socket UDP
    int server_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sock < 0) {
        std::cerr << "Lỗi tạo socket" << std::endl;
        return 1;
    }

    // Cấu hình địa chỉ server
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;   // IP address
    server_addr.sin_port = htons(SERVER_PORT);  // Port

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Lỗi bind socket" << std::endl;
        close(server_sock);
        return 404;
    }

    std::cout << "UDP Server đang chạy trên cổng " << SERVER_PORT << "...\n";

    while (true) {
        // Đọc từ socket
        int recv_len = recvfrom(server_sock, buffer, BUFFER_SIZE - 1, 0,
                               (struct sockaddr*)&client_addr, &client_len);
        
        // Có dữ liệu đến
        if (recv_len > 0) {
            buffer[recv_len] = '\0'; // Ngắt buffer để đảm bảo an toàn khi đọc

            // Debug
            std::cout << "\n>>> Nhận từ [" << inet_ntoa(client_addr.sin_addr) << ":" 
                      << ntohs(client_addr.sin_port) << "]: " << buffer << "\n";

            // Xử lý yêu cầu metadata (REQUEST_METADATA:filename)
            if (strncmp(buffer, REQUEST_METADATA, strlen(REQUEST_METADATA)) == 0) {
                handle_metadata_request(server_sock, client_addr, client_len, buffer);
                continue;
            }
            // Xử lý yêu cầu chunk với định dạng: [REQUEST_CHUNK:filename:chunk_number]
            else if (strncmp(buffer, REQUEST_CHUNK, strlen(REQUEST_CHUNK)) == 0) {
                handle_chunk_request(server_sock, client_addr, client_len, buffer);
                continue;
            }
        }
        // Không mở được file hoặc sai cú pháp gửi
        handle_wrong_command(server_sock, client_addr, client_len);
    }

    close(server_sock);
    return 0;
}
