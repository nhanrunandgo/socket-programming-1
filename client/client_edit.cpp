#include "client_edit.h"

/// @brief To use to manage sent packets status
struct PendingPacket {
    std::chrono::steady_clock::time_point send_time;
    int retry_count;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
    bool needs_retry; // Add flag to manage retry
};

static uint32_t crc_table[256];             // CRC32 table (2^8=256)
std::atomic<bool> running{true};           // Flag to control thread
std::mutex packets_mtx;                   // Mutex for syncing
std::condition_variable timeout_cv;      // Condition variable
std::vector<PendingPacket> pending_packets;
struct sockaddr_in server_addr;
socklen_t server_addr_len = sizeof(server_addr);

uint64_t ntohll(uint64_t value) {
    return (((uint64_t)ntohl(value & 0xFFFFFFFF)) << 32) | ntohl(value >> 32); 
} 
   
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32) | htonl(value >> 32); 
} 

void empty_lines(int height = CONSOLE_HEIGHT) {
    while(height--) {
        std::cout << "\n";
    }
}

/// @brief create CRC32 looking table using 0xEDB88320 polynomial
void init_crc_table() {
    uint32_t polynomial = 0xEDB88320;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (size_t j = 0; j < 8; j++) {
            if (c & 1)
                c = polynomial ^ (c >> 1);
            else
                c = c >> 1;
        }
        crc_table[i] = c;
    }
}

/// @brief calculate crc32 checksum for a string data
uint32_t crc32(char* buf, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t index = (crc ^ (uint8_t)buf[i]) & 0xFF;
        crc = crc_table[index] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/// @brief decode and remove checksum to original message, return True if crc checking is similar
bool decode_and_popback(char* message, size_t& len) {
    uint32_t crc_calculated = htonl(crc32(message, len - 4));
    uint32_t crc_received;
    memcpy(&crc_received, message + len - 4, sizeof(crc_received));
    len -= sizeof(crc_received);
    message[len] = '\0';

    std::cerr << "CRC32: " << crc_received <<" vs " << crc_calculated << " " 
                                        << (crc_received == crc_calculated ? "True": "False") << "\n";

    return (crc_received == crc_calculated);
}

void resend_packet_thread(int client_sock) {
    while(running) {
        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> to_remove;

        {   
            std::unique_lock<std::mutex> lock(packets_mtx);

            for (PendingPacket& packet: pending_packets) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - packet.send_time);

                if(elapsed.count() > RETRY_DELAY_MS) {
                    // Resend packet
                    sendto(client_sock, packet.buffer, packet.buffer_len, 0,
                            (sockaddr*)&server_addr, server_addr_len);

                    packet.send_time = now;
                    std::cout << "[RESEND]: " << packet.buffer << "\n"; 
                }
            }
        }

        // Wait with timeout
        std::unique_lock<std::mutex> lock(packets_mtx);
        timeout_cv.wait_for(lock, std::chrono::milliseconds(RETRY_DELAY_MS), [&]{
            return !running.load();
        });
    }
}

Metadata get_metadata(std::string filename) {
    char buffer[BUFFER_SIZE];
    int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    std::string message = REQUEST_METADATA + (std::string)":" + filename;

    if (client_sock < 0) {
        std::cerr << "Lỗi tạo socket" << std::endl;
        exit(0);
    }

    PendingPacket meta_request;
    strcpy(meta_request.buffer, message.c_str());
    meta_request.buffer_len = message.size();
    meta_request.send_time = std::chrono::steady_clock::now();
    pending_packets.push_back(meta_request);    // Bắt đầu vào việc gửi và resend (giả sử đã được gửi lần đầu)

    std::thread timeout_thread(resend_packet_thread, client_sock);
    while(true) {
        // Load from socket...
        size_t recv_len = recvfrom(client_sock, buffer, BUFFER_SIZE - 1, 0,
                                    (struct sockaddr*)&server_addr, &server_addr_len);
        buffer[recv_len] = '\0';

        if(recv_len > 0) {
            message = buffer;
            decode_and_popback(buffer, recv_len);
            
            std::regex pattern(R"(^REPLY:\d+:META:.+$)");   // Pattern of meta reply packet
            if (std::regex_match(message, pattern)) {
                running = false;
                break;
            }
        }
        timeout_cv.notify_one();
    }
    
    std::cerr << "Exit\n";
    // Clean up
    timeout_cv.notify_all();
    timeout_thread.join();
    close(client_sock);
}

void read_console(char* server_ip) {
    std::cout << "Nhập IP server (mặc định 127.0.0.1): ";
    while (std::cin.getline(server_ip, 15)) {
        if (strlen(server_ip) == 0) {
            strcpy(server_ip, "127.0.0.1");
            break;
        }

        if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
            std::cerr << "Địa chỉ IP không hợp lệ" << std::endl;
            continue;
        }

    }
    empty_lines(4);
    std::cout << "Đang lấy danh sách file từ server [" << server_ip << ":" << SERVER_PORT << "]: ...\n";

    get_metadata(SERVER_LIST_FILE);

    empty_lines(2);
    std::cout << "Nhập tên file (để trống để lấy danh sách, 'exit' để thoát):\n";
    
}

int main() {
    char buffer[BUFFER_SIZE];

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVER_PORT);
    
    char server_ip[16];
    
    auto signal_handler = [](int signum) {
        std::cout << "\nĐã nhận tín hiệu Ctrl+C. Đang kết thúc...\n";
        exit(0);
    };
    signal(SIGINT, signal_handler);

    read_console(server_ip);
    

}