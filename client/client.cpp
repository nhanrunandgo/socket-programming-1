#include "client.h"

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
std::chrono::_V2::steady_clock::time_point last_seen = std::chrono::steady_clock::now();

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

    return (crc_received == crc_calculated);
}

/// @brief Hàm gửi ACK (có thể cần điều chỉnh hoặc loại bỏ tùy thuộc vào server)
void send_ack(int sock, uint64_t seq_num) {
    char ack_buffer[BUFFER_SIZE];
    snprintf(ack_buffer, sizeof(ack_buffer), "REPLY:%lu:ACK", seq_num);
    
    sendto(sock, ack_buffer, strlen(ack_buffer), 0, (const sockaddr*)&server_addr, server_addr_len);
    //std::cout << "Đã gửi ACK #" << seq_num << " đến server\n";
}

void resend_packet_thread(int client_sock) {
    while(running) {
        auto now = std::chrono::steady_clock::now();

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

struct Metadata get_metadata(std::string filename) {
    running = true;
    std::cout << "Metadata " << filename << "\n";
    Metadata file_downloading;
    char buffer[BUFFER_SIZE];
    size_t recv_len;
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
    pending_packets.clear();                    // Empty pending queue to send only this meta packet
    pending_packets.push_back(meta_request);    // Supposed we sent the first time and then push to wait for resend

    std::thread timeout_thread(resend_packet_thread, client_sock);
    while(true) {
        // Load from socket...
        recv_len = recvfrom(client_sock, buffer, BUFFER_SIZE - 1, 0,
                                    (struct sockaddr*)&server_addr, &server_addr_len);
        buffer[recv_len] = '\0';

        if(recv_len > 0) {
            decode_and_popback(buffer, recv_len);
            message = buffer;
            std::cout << "[RECEIVED]: " << message << "\n";
            
            // List file metadata pattern
            std::string pattern = R"(^REPLY:\d+:META:)" + filename + R"(:.*$)"; 

            std::regex regex_pattern(pattern);   
            if (std::regex_match(message, regex_pattern)) {   // If matches then check data
                char *token = strtok(buffer, ":");  // REPLY
                token = strtok(NULL, ":");          // seq#
                uint64_t seq_num = std::stoull(token);
                token = strtok(NULL, ":");          // META
                token = strtok(NULL, ":");          // filename

                size_t data_len = recv_len - ((size_t)(token - &buffer[0]) + strlen(token) + 1);
                Metadata net_meta;

                if (data_len == sizeof(Metadata)) {            // If data matches then parse and break the loop
                    memcpy(&net_meta, token + strlen(token) + 1, sizeof(Metadata));
                    file_downloading.file_size = ntohll(net_meta.file_size);
                    file_downloading.num_chunks = ntohll(net_meta.num_chunks);
                    file_downloading.chunk_size = ntohll(net_meta.chunk_size);
                    std::cout << "\n--- Metadata ---\n"
                            << "File: " << filename << "\n"
                            << "Kích thước: " << file_downloading.file_size << " bytes\n"
                            << "Số chunk: " << file_downloading.num_chunks << "\n"
                            << "Kích thước chunk: " << file_downloading.chunk_size << " bytes\n"
                            << "----------------\n";

                    send_ack(client_sock, seq_num);
                    running = false;
                    break;
                }
            }
        }
        timeout_cv.notify_one();
    }

    // Clean up
    timeout_cv.notify_all();
    timeout_thread.join();
    close(client_sock);
    return file_downloading;
}

std::string uint64_to_string_converter(uint64_t num) {
    std::string tmp = "";
    while(num > 0) {
        tmp += (char)(num % 10 + '0');
        num /= 10;
    }
    if (tmp.size() == 0) {
        tmp = "0";
    }
    std::reverse(tmp.begin(), tmp.end());
    return tmp;
}

int create_socket() {
    size_t recv_len;
    int client_sock = socket(AF_INET, SOCK_DGRAM, 0);
    

    if (client_sock < 0) {
        std::cerr << "Lỗi tạo socket" << std::endl;
        exit(0);
    }

    // Đặt socket ở chế độ non-blocking
    fcntl(client_sock, F_SETFL, O_NONBLOCK);

    return client_sock;
}

void createFileWithSize(std::string filename, uint64_t size) {
    filename = DOWNLOADS_DIR + filename;
    std::lock_guard<std::mutex> lock(packets_mtx);

    // Mở file với chế độ ghi và tạo mới
    std::ofstream out(filename, std::ios::binary);

    if (!out) {
        std::cerr << "Không thể mở file để ghi!" << std::endl;
        return;
    }

    // Ghi đủ dữ liệu (khoảng trắng) để đạt được kích thước mong muốn
    char filler = '\0';  // Ký tự để điền vào file
    for (uint64_t i = 0; i < size; ++i) {
        out.put(filler);  // Ghi ký tự filler vào file
    }

    out.close();
    std::cout << "File đã được tạo với kích thước: " << size << " bytes." << std::endl;
}

void overwriteAtChunk(std::string filename, uint64_t chunk_id, uint64_t chunk_size, char* data, size_t data_len) {
    filename = DOWNLOADS_DIR + filename;
    // Mở file ở chế độ ghi nhị phân và cho phép ghi đè
    std::lock_guard<std::mutex> lock(packets_mtx); 
    std::fstream file(filename, std::ios::in | std::ios::out | std::ios::binary);

    if (!file) {
        std::cerr << "Không thể mở file để ghi!" << std::endl;
        return;
    }

    uint64_t offset = chunk_id * chunk_size;

    // Di chuyển con trỏ đến vị trí offset
    file.seekp(offset, std::ios::beg);

    // Ghi dữ liệu tại offset
    file.write(data, data_len);

    file.close();
}

void thread_chunk(int thread_part, int client_sock, std::string filename, uint64_t start_chunk, uint64_t end_chunk, struct ThreadTracker& tracker, struct Metadata& metadata) {
    char buffer[BUFFER_SIZE];
    fd_set readfds;
    struct timeval timeout;

    for (uint64_t chunk_id = start_chunk; chunk_id < end_chunk; chunk_id++) {
        tracker.downloading_chunk.insert(chunk_id);
    }

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(client_sock, &readfds);

        // Thiết lập timeout (ví dụ: 1000ms)
        timeout.tv_sec = SENDING_TIMEOUT / 1000;
        timeout.tv_usec = (SENDING_TIMEOUT % 1000) * 1000;

        // Chờ sự kiện hoặc timeout
        int activity = select(client_sock + 1, &readfds, nullptr, nullptr, &timeout);

        if (activity < 0) {
            perror("select error");
            break;
        }

        // Xử lý nhận dữ liệu nếu có
        if (FD_ISSET(client_sock, &readfds)) {
            struct sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            size_t recv_len = recvfrom(client_sock, buffer, sizeof(buffer), 0,
                                            (struct sockaddr*)&clientAddr, &addrLen);

            if (recv_len > BUFFER_SIZE) {
                std::cout << "Lỗi tràn bộ nhớ!!!";
                exit(0);
            }

            if (recv_len > 0) {
                decode_and_popback(buffer, recv_len);
                std::string message(buffer, recv_len); // Chuyển buffer sang string
                std::vector<std::string> parts;
                size_t pos = 0;
                int erasedCount;
                uint64_t seq_num;

                // Tách các phần của message
                for (int i = 0; i < 5; ++i) { // REPLY, seq, CHUNK, filename, chunk_id
                    size_t colon_pos = message.find(':', pos);
                    if (colon_pos == std::string::npos) {
                        std::cerr << "Invalid message format: missing fields!" << std::endl;
                        break;
                    }
                    parts.push_back(message.substr(pos, colon_pos - pos));
                    pos = colon_pos + 1;
                }

                if (parts.size() != 5){    // Gói tin lỗi
                    continue;
                }

                // Lấy phần dữ liệu sau chunk_id
                std::string data_part = message.substr(pos);
                size_t data_len = data_part.size();

                // Kiểm tra chunk_id hợp lệ
                uint64_t chunk_id;
                try {
                    seq_num = std::stoull(parts[1]);
                    chunk_id = std::stoull(parts[4]);
                    send_ack(client_sock, seq_num);
                } catch (const std::exception& e) { // Gói tin lỗi
                    std::cout << e.what() << "\n";
                    continue;
                }
                
                erasedCount = tracker.downloading_chunk.erase(chunk_id);
                
                // Ghi dữ liệu vào file
                if (data_len > 0 && erasedCount > 0 && parts[0] == REPLY && parts[2] == "CHUNK" && parts[3] == filename) {
                    overwriteAtChunk(filename, chunk_id, metadata.chunk_size, data_part.data(), data_len);
                    //std::cout << "[RECEIVED]: REPLY:" << parts[1] << ":CHUNK:" << filename << ":" << chunk_id << ":\n";
                }
            }
        }
        
        // Tải hết rồi thì thoát
        if (tracker.downloading_chunk.empty()) {
            break;
        }

        // Xử lý gửi request nếu đang trống
        if (activity == 0) {
            std::string header = REQUEST_CHUNK + (std::string)":" + filename + (std::string)":";   // Request chunk header
            int token_num = TOKEN_LIMIT;

            for (uint64_t number: tracker.downloading_chunk) {
                if (token_num > 0)
                    token_num--;
                else
                    break;
                
                std::string message = header + uint64_to_string_converter(number);
                sendto(client_sock, message.c_str(), message.size(), 0,
                                (const sockaddr*)&server_addr, server_addr_len);
            }
        }
    }

    close(client_sock);
}

void download_file(std::string filename) {      // Data gets from file_downloading metadata
    struct Metadata metadata = get_metadata(filename);
    struct ThreadTracker download_tracker[4];
    std::vector<std::thread> threads;

    createFileWithSize(filename, metadata.file_size);   // Fulfill file with dummy bytes
    auto now = std::chrono::steady_clock::now();
    
    uint64_t socket_quantity = NUM_DOWNLOAD_THREADS;
    uint64_t chunks_per_thread = (metadata.num_chunks + NUM_DOWNLOAD_THREADS - 1) / NUM_DOWNLOAD_THREADS;

    //std::cout << metadata.num_chunks << "\n";
    for (int sock_id = 0; sock_id < socket_quantity; sock_id++) {
        uint64_t start_chunk = sock_id * chunks_per_thread;
        uint64_t end_chunk = std::min(chunks_per_thread * (sock_id + 1), metadata.num_chunks);
        download_tracker[sock_id].total_chunk = end_chunk - start_chunk;

        int socket_fd = create_socket();
        threads.emplace_back(thread_chunk, sock_id + 1, socket_fd, filename, start_chunk, end_chunk, std::ref(download_tracker[sock_id]), std::ref(metadata));
    }

    uint64_t downloading_state = 1;
    while (downloading_state) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_seen);

        if (elapsed.count() > REFRESH_CONSOLE) {
            empty_lines(5);
            downloading_state = 0;
            
            for (int sock_id = 0; sock_id < socket_quantity; sock_id++)
            {
                downloading_state |= download_tracker[sock_id].downloading_chunk.size();
                std::cout << "Downloading " << filename << " part " << sock_id + 1 << " .... " << 
                            100 - (download_tracker[sock_id].downloading_chunk.size() * 100 / std::max(1UL, download_tracker[sock_id].total_chunk)) << "%\n";
            }
            last_seen = now;
        }
    }
    
    // Đợi tất cả luồng kết thúc
    for (auto& t : threads) {
        t.join();
    }
    std::cout << "Downloading " << filename <<" done.\n";
}

std::string byte_name_converter(float bytes) {
    const std::string name[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    
    int level = 0;
    while (bytes >= 1024 && level < 5) {  // Không đi quá TB (5 cấp độ)
        bytes /= 1024;
        level++;
    }

    std::ostringstream res;
    res << std::fixed << std::setprecision(2) << bytes << name[level];  // Định dạng số với 2 chữ số thập phân
    return res.str();  // Trả về chuỗi kết quả
}

void read_list() {
    std::string filename = (std::string)DOWNLOADS_DIR + SERVER_LIST_FILE;
    std::ifstream file(filename);  // Mở file để đọc (thay đổi tên file nếu cần)
    if (!file.is_open()) {
        std::cerr << "Không thể lấy danh sách từ server!" << std::endl;
        exit(0);
    }

    std::cout << "--------Danh sách file có thể tải:---------\n";

    std::string line;
    while (std::getline(file, line)) {
        // Tìm vị trí của dấu cách cuối cùng
        size_t pos = line.find_last_of(' ');

        // Kiểm tra nếu có ít nhất 1 dấu cách
        if (pos != std::string::npos) {
            // Tách tên file (trước dấu cách cuối cùng) và kích thước (sau dấu cách cuối cùng)
            std::string filename = line.substr(0, pos);
            std::string size_str = line.substr(pos + 1);

            // Chuyển kích thước từ chuỗi sang uint64_t
            uint64_t size = 0;
            std::istringstream(size_str) >> size;
            
            // In kết quả
            std::cout << filename << "\t " << byte_name_converter(size) << "\n";
        }
    }
    std::cout << "----------------------------------\n";

    file.close();  // Đóng file
}

void download_and_checkfile() {
    DIR *dir = opendir(DOWNLOADS_DIR);
    std::ifstream file(CLIENT_LIST_FILE);  // Mở file để đọc (thay đổi tên file nếu cần)

    if (!file.is_open()) {
        std::cerr << "Lỗi đọc file input.txt!" << std::endl;
        exit(0);
    }

    // Unable to open this path
    if (dir == 0) {
        std::cout << "Error opening path: " << DOWNLOADS_DIR << "\n";
        exit(0);
    }

    struct dirent *entry;
    std::unordered_map<std::string, bool> f;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        f[(std::string) entry->d_name] = true;
    }
    closedir(dir);

    std::string filename;
    while (std::getline(file, filename)) {  // Quét từng file trong danh sách
        if (f[filename] == false && filename.size()) // Chưa có file này
        {
            file.close();
            download_file(filename);
            std::cout << "\nRefreshing...\n";
            return;
        }
    }

    file.close();
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
        else break;

    }
    empty_lines(0);
    std::cout << "Đang lấy danh sách file từ server [" << server_ip << ":" << SERVER_PORT << "]: ...\n";

    download_file(SERVER_LIST_FILE);

    empty_lines();
    read_list();
    std::cout << "Chỉnh sửa danh sách file cần tải trong input.txt. Nhấn Enter để bắt đầu tải... :";
    std::string dummy;
    getline(std::cin, dummy);   // Đợi đến khi enter là nhảy
    std::cout << "\nRefreshing...\n";

    while(true) {
        download_and_checkfile();
    }    
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
    return 0;
}