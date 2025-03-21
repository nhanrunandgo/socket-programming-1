#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits>

#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define CHUNK_SIZE 4096
#define FILES_DIR "files/"

#define REQUEST_CHUNK "REQUEST_CHUNK:"

#pragma pack(push, 1)
struct Metadata {
    uint64_t file_size;
    uint64_t num_chunks;
    uint64_t chunk_size;
};
#pragma pack(pop)

// Hàm chuyển đổi 64-bit host to network
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32 | htonl(value >> 32));
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
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(SERVER_PORT);

    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Lỗi bind socket" << std::endl;
        close(server_sock);
        return 1;
    }

    std::cout << "UDP Server đang chạy trên cổng " << SERVER_PORT << "...\n";

    while (true) {
        int recv_len = recvfrom(server_sock, buffer, BUFFER_SIZE - 1, 0,
                               (struct sockaddr*)&client_addr, &client_len);

        if (recv_len > 0) {
            buffer[recv_len] = '\0';
            std::cout << "Nhận từ [" << inet_ntoa(client_addr.sin_addr) << ":" 
                      << ntohs(client_addr.sin_port) << "]: " << buffer << std::endl;

            // Xử lý yêu cầu metadata
            if (strncmp(buffer, "REQUEST_METADATA:", 17) == 0) {  // Sửa từ 16 thành 17 để khớp độ dài chuỗi
                char filename[256];
                strncpy(filename, buffer + 17, sizeof(filename) - 1);
                filename[sizeof(filename) - 1] = '\0';

                // Tạo đường dẫn đầy đủ
                char fullpath[512];
                snprintf(fullpath, sizeof(fullpath), "%s%s", FILES_DIR, filename);
                struct stat file_stat;
                Metadata meta = {0};
                int fd = open(fullpath, O_RDONLY);
                
                if (fd != -1 && fstat(fd, &file_stat) == 0) {
                    meta.file_size = file_stat.st_size;
                    meta.chunk_size = CHUNK_SIZE;
                    meta.num_chunks = (file_stat.st_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
                    close(fd);
                } else {
                    meta.file_size = 0;
                    meta.num_chunks = 0;
                    meta.chunk_size = 0;
                }
                
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
                snprintf(reply, sizeof(reply), "REPLY_METADATA:%s:", filename);

                // Copy dữ liệu metadata vào buffer
                size_t header_len = strlen(reply);
                memcpy(reply + header_len, &net_meta, sizeof(net_meta));
                size_t total_len = header_len + sizeof(net_meta);

                sendto(server_sock, reply, total_len, 0,
                      (struct sockaddr*)&client_addr, client_len);
                printf("%s\n", reply);
            }
            // Xử lý yêu cầu chunk REQUEST_CHUNK:filename:10
            else if (strncmp(buffer, REQUEST_CHUNK, strlen(REQUEST_CHUNK)) == 0) {
                char *end_name_colon = strchr(buffer + strlen(REQUEST_CHUNK), ':'); // Kết thúc filename
                if (end_name_colon != NULL) {
                    *end_name_colon = '\0'; 
                    char filename[256];
                    strncpy(filename, buffer + strlen(REQUEST_CHUNK), sizeof(filename) - 1);
                    filename[sizeof(filename) - 1] = '\0';
                    
                    uint64_t chunk_index = std::strtoul(end_name_colon + 1, nullptr, 10);

                    std::cout << "\n--- Phân giải ---\n"
                                << "File: " << filename << "\n"
                                << "Chunk ID: " << chunk_index << "\n"
                                << "----------------\n";
                    
                    // Tạo đường dẫn đầy đủ
                    char fullpath[512];
                    snprintf(fullpath, sizeof(fullpath), "%s%s", FILES_DIR, filename);
                    
                    int fd = open(fullpath, O_RDONLY);
                    if (fd != -1) {
                        struct stat file_stat;
                        if (fstat(fd, &file_stat) == 0) {
                            uint32_t file_size = file_stat.st_size;
                            uint32_t num_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
                            
                            if (chunk_index < num_chunks) {
                                // Tính offset và kích thước chunk
                                off_t offset = static_cast<off_t>(chunk_index) * CHUNK_SIZE;
                                size_t actual_chunk_size = (chunk_index == num_chunks - 1) ?
                                    (file_size % CHUNK_SIZE ? file_size % CHUNK_SIZE : CHUNK_SIZE) : CHUNK_SIZE;
                                
                                // Đọc dữ liệu chunk
                                char chunk_data[CHUNK_SIZE];
                                lseek(fd, offset, SEEK_SET);
                                ssize_t read_len = read(fd, chunk_data, actual_chunk_size);
                                
                                if (read_len == actual_chunk_size) {
                                    // Tạo phản hồi
                                    char reply[BUFFER_SIZE];
                                    snprintf(reply, sizeof(reply), "REPLY_CHUNK:%s:%lu:", filename, chunk_index);
                                    size_t header_len = strlen(reply);
                                    memcpy(reply + header_len, chunk_data, actual_chunk_size);
                                    size_t total_len = header_len + actual_chunk_size;
                                    
                                    sendto(server_sock, reply, total_len, 0,
                                            (struct sockaddr*)&client_addr, client_len);
                                } else {
                                    // Lỗi đọc file
                                    char error_reply[BUFFER_SIZE];
                                    snprintf(error_reply, sizeof(error_reply), "REPLY_CHUNK:%s:%lu:ERROR_READ", filename, chunk_index);
                                    sendto(server_sock, error_reply, strlen(error_reply), 0,
                                            (struct sockaddr*)&client_addr, client_len);
                                }
                            } else {
                                // Chunk index không hợp lệ
                                char error_reply[BUFFER_SIZE];
                                snprintf(error_reply, sizeof(error_reply), "REPLY_CHUNK:%s:%lu:ERROR_INDEX", filename, chunk_index);
                                sendto(server_sock, error_reply, strlen(error_reply), 0,
                                        (struct sockaddr*)&client_addr, client_len);
                            }
                            close(fd);
                        } else {
                            // Không mở được file
                            char error_reply[BUFFER_SIZE];
                            snprintf(error_reply, sizeof(error_reply), "REPLY_CHUNK:%s:%lu:ERROR_OPEN", filename, chunk_index);
                            sendto(server_sock, error_reply, strlen(error_reply), 0,
                                   (struct sockaddr*)&client_addr, client_len);
                        }
                    }
                }
            }
        }
    }

    close(server_sock);
    return 0;
}
