#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <limits>

#define SERVER_PORT 12345
#define BUFFER_SIZE 4096
#define FILE_LIST "_serverlist.svl"

#pragma pack(push, 1)
struct Metadata {
    uint32_t file_size;
    uint32_t num_chunks;
    uint32_t chunk_size;
};
#pragma pack(pop)

int main() {
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);
    char buffer[BUFFER_SIZE];
    
    int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (client_sock < 0) {
        std::cerr << "Lỗi tạo socket" << std::endl;
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    char server_ip[16];
    std::cout << "Nhập IP server (mặc định 127.0.0.1): ";
    std::cin.getline(server_ip, 15);
    if (strlen(server_ip) == 0) {
        strcpy(server_ip, "127.0.0.1");
    }

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        std::cerr << "Địa chỉ IP không hợp lệ" << std::endl;
        close(client_sock);
        return 1;
    }

    std::cout << "Kết nối đến " << server_ip << ":" << SERVER_PORT << std::endl;
    std::cout << "Nhập tên file (để trống để lấy danh sách, 'exit' để thoát):\n";

    while (true) {
        std::cout << "> ";
        std::cin.getline(buffer, BUFFER_SIZE);
        
        if (strcmp(buffer, "exit") == 0) break;

        char request[BUFFER_SIZE] = "REQUEST_METADATA:";
        char* target_file = buffer;

        // Xử lý đầu vào
        if (strlen(buffer) == 0) {
            strcat(request, FILE_LIST);
        } else {
            if (strncmp(buffer, "REQUEST_METADATA:", 16) != 0) {
                strcat(request, buffer);
            } else {
                strcpy(request, buffer);
            }
        }

        // Gửi yêu cầu
        int sent_len = sendto(client_sock, request, strlen(request), 0,
                             (struct sockaddr*)&server_addr, addr_len);
        if (sent_len < 0) {
            std::cerr << "Lỗi gửi dữ liệu" << std::endl;
            continue;
        }

        std::cout << "Đã gửi yêu cầu cho file: " 
                 << (strlen(buffer) ? buffer : FILE_LIST) << std::endl;

        // Nhận phản hồi
        int recv_len = recvfrom(client_sock, buffer, BUFFER_SIZE, 0,
                               (struct sockaddr*)&server_addr, &addr_len);
        if (recv_len > 0) {
            // Xử lý phản hồi metadata
            if (strncmp(buffer, "REPLY_METADATA:", 14) == 0) {
                char* colon1 = strchr(buffer + 14, ':');
                if (colon1) {
                    *colon1 = '\0';
                    char* filename = buffer + 14;
                    char* meta_start = colon1 + 1;
                    
                    // Kiểm tra độ dài dữ liệu
                    if (recv_len >= (meta_start - buffer) + sizeof(Metadata)) {
                        Metadata net_meta;
                        memcpy(&net_meta, meta_start, sizeof(Metadata));
                        
                        // Chuyển đổi byte order
                        Metadata meta;
                        meta.file_size = ntohl(net_meta.file_size);
                        meta.num_chunks = ntohl(net_meta.num_chunks);
                        meta.chunk_size = ntohl(net_meta.chunk_size);

                        std::cout << "\n--- Metadata ---\n"
                                  << "File: " << filename << "\n"
                                  << "Kích thước: " << meta.file_size << " bytes\n"
                                  << "Số chunk: " << meta.num_chunks << "\n"
                                  << "Kích thước chunk: " << meta.chunk_size << " bytes\n"
                                  << "----------------\n";
                    } else {
                        std::cout << "Dữ liệu metadata không hợp lệ\n";
                    }
                }
            } else {
                std::cout << "Phản hồi từ server: ";
                fwrite(buffer, 1, recv_len, stdout);
                std::cout << std::endl;
            }
        }
    }

    close(client_sock);
    std::cout << "Đã đóng kết nối" << std::endl;
    return 0;
}
