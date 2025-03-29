#include "client.h"

uint64_t ntohll(uint64_t value) {
    return (((uint64_t)ntohl(value & 0xFFFFFFFF)) << 32) | ntohl(value >> 32); 
} 
   
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl(value & 0xFFFFFFFF)) << 32) | htonl(value >> 32); 
} 

// Biến toàn cục để theo dõi các file đã được xử lý
std::set<std::string> processed_files;
std::mutex processed_files_mutex;

// Hàm đọc danh sách file mới từ input.txt
std::vector<std::string> get_new_files() {
    std::vector<std::string> new_files;
    std::ifstream input_file(INPUT_FILE);

    if (!input_file.is_open()) {
        std::cerr << "Không thể mở file input.txt\n";
        return new_files;
    }
    std::string line;
    while (std::getline(input_file, line)) {
        std::string filename = line;
        // Không cần trim ở đây, để nguyên khoảng trắng
        if (!filename.empty()) {
            std::lock_guard<std::mutex> lock(processed_files_mutex);
            if (processed_files.find(filename) == processed_files.end()) {
                new_files.push_back(filename);
                processed_files.insert(filename);
            }
        }
    }
    input_file.close();
    return new_files;
}

// Hàm gửi ACK (có thể cần điều chỉnh hoặc loại bỏ tùy thuộc vào server)
void send_ack(int sock, const sockaddr_in& server_addr, socklen_t addr_len, uint64_t seq_num) {
    char ack_buffer[BUFFER_SIZE];
    snprintf(ack_buffer, sizeof(ack_buffer), "REPLY:%lu:ACK", seq_num);
    sendto(sock, ack_buffer, strlen(ack_buffer), 0, (const sockaddr*)&server_addr, addr_len);
    std::cout << "Đã gửi ACK #" << seq_num << " đến server\n";
}

// Hàm gửi yêu cầu metadata đến server
bool request_metadata(int sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, Metadata& metadata, uint64_t seq_num) {
    char request_buffer[BUFFER_SIZE];
    snprintf(request_buffer, sizeof(request_buffer), "REQUEST_METADATA:%s", filename.c_str());
    if (sendto(sock, request_buffer, strlen(request_buffer), 0, (const sockaddr*)&server_addr, addr_len) < 0) {
        std::cerr << "Lỗi gửi yêu cầu metadata cho " << filename << "\n";
        return false;
    }
    // std::cout << "Đã gửi yêu cầu metadata cho " << filename << " đến server.\n";

    char buffer[BUFFER_SIZE];
    int recv_len = recvfrom(sock, buffer, BUFFER_SIZE, 0, (sockaddr*)&server_addr, &addr_len);
    if (recv_len < 0) {
        std::cerr << "Lỗi nhận phản hồi metadata cho " << filename << "\n";
        return false;
    }
    buffer[recv_len] = '\0';

    if (strncmp(buffer, REPLY, strlen(REPLY)) == 0) {
        char* token = strtok(buffer, ":"); // REPLY
        token = strtok(NULL, ":");          // Seq Num
        if (token != NULL) {
            uint64_t received_seq_num = std::stoull(token);
            token = strtok(NULL, ":");          // Command (META)
            if (token != NULL && strcmp(token, "META") == 0) {
                token = strtok(NULL, ":");      // Filename
                if (token != NULL && strcmp(token, filename.c_str()) == 0) {
                    Metadata net_meta;
                    size_t data_len = recv_len - (token - buffer + strlen(token) + 1);
                    if (data_len == sizeof(Metadata)) {
                        memcpy(&net_meta, token + strlen(token) + 1, sizeof(Metadata));
                        metadata.file_size = ntohll(net_meta.file_size);
                        metadata.num_chunks = ntohll(net_meta.num_chunks);
                        metadata.chunk_size = ntohll(net_meta.chunk_size);
                        std::cout << "\n--- Metadata ---\n"
                                  << "File: " << filename << "\n"
                                  << "Kích thước: " << metadata.file_size << " bytes\n"
                                  << "Số chunk: " << metadata.num_chunks << "\n"
                                  << "Kích thước chunk: " << metadata.chunk_size << " bytes\n"
                                  << "----------------\n";
                        send_ack(sock, server_addr, addr_len, received_seq_num);
                        return true;
                    } else {
                        std::cerr << "Kích thước metadata không hợp lệ\n";
                        return false;
                    }
                } else {
                    std::cerr << "Tên file trong metadata không khớp: \"" << (token ? token : "NULL") << "\" != \"" << filename << "\"\n";
                    return false;
                }
            } else if (token != NULL && strcmp(token, "ERROR") == 0) {
                token = strtok(NULL, ":");
                if (token != NULL) {
                    std::cerr << "Lỗi từ server khi yêu cầu metadata cho \"" << filename << "\": " << token << "\n";
                } else {
                    std::cerr << "Lỗi không xác định từ server khi yêu cầu metadata cho \"" << filename << "\"\n";
                }
                return false;
            } else {
                std::cerr << "Phản hồi không hợp lệ từ server cho metadata của \"" << filename << "\": " << buffer << "\n";
                return false;
            }
        } else {
            std::cerr << "Phản hồi REPLY không đúng định dạng (thiếu sequence number)\n";
            return false;
        }
    } else {
        std::cerr << "Phản hồi không mong đợi từ server cho metadata của \"" << filename << "\": " << buffer << "\n";
        return false;
    }
}

// Hàm gửi yêu cầu chunk và nhận dữ liệu
bool download_chunk(int sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, uint64_t chunk_id, std::vector<char>& data, uint64_t seq_num) {
    char request_buffer[BUFFER_SIZE];
    uint64_t current_seq = seq_num++;
    snprintf(request_buffer, sizeof(request_buffer), "REQUEST_CHUNK:%s:%lu", filename.c_str(), chunk_id);
    // std::cout << "[CLIENT] Thread đang gửi yêu cầu chunk: " << request_buffer << " (Seq: " << current_seq << ")\n";
    for (int i = 0; i < MAX_RETRIES; ++i) {
        if (sendto(sock, request_buffer, strlen(request_buffer), 0, (const sockaddr*)&server_addr, addr_len) < 0) {
            std::cerr << "Lỗi gửi yêu cầu chunk " << chunk_id << " của \"" << filename << "\" (lần " << i + 1 << ")\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
            continue;
        }

        char buffer[BUFFER_SIZE];
        int recv_len = recvfrom(sock, buffer, BUFFER_SIZE, 0, (sockaddr*)&server_addr, &addr_len);
        if (recv_len > 0) {
            buffer[recv_len] = '\0';
            if (strncmp(buffer, REPLY, strlen(REPLY)) == 0) {
                char* token = strtok(buffer, ":"); // REPLY
                token = strtok(NULL, ":");          // Seq Num
                if (token != NULL) {
                    uint64_t received_seq_num = std::stoull(token);
                    token = strtok(NULL, ":"); // Command
                    if (token != NULL && strcmp(token, "CHUNK") == 0) {
                        token = strtok(NULL, ":"); // Filename
                        if (token != NULL && strcmp(token, filename.c_str()) == 0) {
                            token = strtok(NULL, ":"); // Chunk ID
                            if (token != NULL) {
                                uint64_t received_chunk_id = std::stoull(token);
                                if (received_chunk_id == chunk_id) {
                                    data.assign(token + strlen(token) + 1, buffer + recv_len);
                                    send_ack(sock, server_addr, addr_len, received_seq_num);
                                    return true;
                                } else {
                                    std::cerr << "ID chunk không khớp trong phản hồi cho \"" << filename << "\" chunk " << chunk_id << "\n";
                                }
                            } else {
                                std::cerr << "Thiếu ID chunk trong phản hồi cho \"" << filename << "\" chunk " << chunk_id << "\n";
                            }
                        } else {
                            std::cerr << "Tên file không khớp trong phản hồi chunk cho \"" << filename << "\" chunk " << chunk_id << "\n";
                        }
                    } else if (token != NULL && strcmp(token, "ERROR") == 0) {
                        token = strtok(NULL, ":");
                        if (token != NULL) {
                            std::cerr << "Lỗi từ server khi yêu cầu chunk " << chunk_id << " của \"" << filename << "\": " << token << "\n";
                        } else {
                            std::cerr << "Lỗi không xác định từ server khi yêu cầu chunk " << chunk_id << " của \"" << filename << "\"\n";
                        }
                        return false;
                    } else {
                        std::cerr << "Phản hồi không mong đợi từ server cho chunk " << chunk_id << " của \"" << filename << "\": " << buffer << "\n";
                        return false;
                    }
                } else {
                    std::cerr << "Phản hồi REPLY không đúng định dạng (thiếu sequence number)\n";
                    return false;
                }
            } else {
                std::cerr << "Phản hồi không mong đợi từ server cho chunk " << chunk_id << " của \"" << filename << "\": " << buffer << "\n";
                return false;
            }
        } else {
            std::cerr << "Không nhận được phản hồi cho chunk " << chunk_id << " của \"" << filename << "\" (lần " << i + 1 << ")\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
    }
    std::cerr << "Không thể tải xuống chunk " << chunk_id << " của \"" << filename << "\" sau nhiều lần thử lại.\n";
    return false;
}

std::mutex received_chunks_mutex;

// Hàm worker cho mỗi thread download
void download_worker(int client_sock, const sockaddr_in& server_addr, socklen_t addr_len, const std::string& filename, uint64_t start_chunk, uint64_t end_chunk, std::map<uint64_t, std::vector<char>>& received_chunks, std::atomic<uint64_t>& chunks_received_count, uint64_t total_chunks, std::atomic<uint64_t>& next_seq_num, std::atomic<uint64_t>& thread_chunks_received, uint64_t total_thread_chunks) {
    for (uint64_t i = start_chunk; i < end_chunk; ++i) {
        std::vector<char> chunk_data;
        uint64_t current_seq = next_seq_num++;
        if (download_chunk(client_sock, server_addr, addr_len, filename, i, chunk_data, current_seq)) {
            std::lock_guard<std::mutex> lock(received_chunks_mutex);
            received_chunks[i] = chunk_data;
            chunks_received_count++;
            thread_chunks_received++;
        }
    }
}

int main() {
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(server_addr);

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

#ifdef _WIN32
    if (_mkdir(DOWNLOADS_DIR) == -1) {
        if (errno != EEXIST) {
            std::cerr << "Lỗi tạo thư mục " << DOWNLOADS_DIR << std::endl;
            return 1;
        }
    }
#else
    struct stat st = {0};
    if (stat(DOWNLOADS_DIR, &st) == -1) {
        if (mkdir(DOWNLOADS_DIR, 0777) == -1) {
            std::cerr << "Lỗi tạo thư mục " << DOWNLOADS_DIR << std::endl;
            return 1;
        }
    }
#endif

    auto signal_handler = [](int signum) {
        std::cout << "\nĐã nhận tín hiệu Ctrl+C. Đang kết thúc...\n";
        exit(0);
    };
    signal(SIGINT, signal_handler);

    std::atomic<uint64_t> next_global_seq_num(1);

    // Lấy danh sách file từ server và hiển thị (sử dụng một sequence number)
    Metadata file_list_metadata;
    if (request_metadata(client_sock, server_addr, addr_len, FILE_LIST_FILE, file_list_metadata, next_global_seq_num++)) {
        std::cout << "[CLIENT] Đã nhận metadata cho server_files.txt. Số chunk: " << file_list_metadata.num_chunks << "\n";
        uint64_t num_list_chunks = file_list_metadata.num_chunks;
        std::cout << "[CLIENT] Chuẩn bị tải " << num_list_chunks << " chunk với " << NUM_DOWNLOAD_THREADS << " threads.\n";
        std::map<uint64_t, std::vector<char>> file_list_chunks;
        std::atomic<uint64_t> list_chunks_received(0);
        std::vector<std::thread> list_threads;
        uint64_t list_chunks_per_thread = (num_list_chunks + NUM_DOWNLOAD_THREADS - 1) / NUM_DOWNLOAD_THREADS;
        std::atomic<uint64_t> next_list_seq_num = next_global_seq_num.load();

        std::vector<int> list_sockets(NUM_DOWNLOAD_THREADS);
        for (int i = 0; i < NUM_DOWNLOAD_THREADS; ++i) {
            list_sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
            if (list_sockets[i] < 0) {
                std::cerr << "Lỗi tạo socket download danh sách file thread " << i << "\n";
                for (int j = 0; j < i; ++j) close(list_sockets[j]);
                goto request_command;
            }
            uint64_t start_chunk = i * list_chunks_per_thread;
            uint64_t end_chunk = std::min((i + 1) * list_chunks_per_thread, num_list_chunks);
            if (start_chunk < end_chunk) {
                list_threads.emplace_back(
                    [&](int sock, uint64_t start, uint64_t end, std::map<uint64_t, std::vector<char>>& chunks, std::atomic<uint64_t>& received_count, const std::string& file, std::atomic<uint64_t>& seq_num) {
                        for (uint64_t j = start; j < end; ++j) {
                            std::vector<char> chunk_data;
                            uint64_t current_seq = seq_num++;
                            if (download_chunk(sock, server_addr, addr_len, file, j, chunk_data, current_seq)) {
                                std::lock_guard<std::mutex> lock(received_chunks_mutex);
                                chunks[j] = chunk_data;
                                received_count++;
                            }
                        }
                    },
                    list_sockets[i], start_chunk, end_chunk, std::ref(file_list_chunks), std::ref(list_chunks_received), FILE_LIST_FILE, std::ref(next_list_seq_num));
            } else {
                list_sockets[i] = -1;
            }
        }
        for (auto& thread : list_threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        for (int sock : list_sockets) {
            if (sock != -1) close(sock);
        }
        next_global_seq_num = next_list_seq_num.load();

        if (list_chunks_received == num_list_chunks) {
            std::stringstream ss_file_list;
            for (uint64_t i = 0; i < num_list_chunks; ++i) {
                auto it = file_list_chunks.find(i);
                if (it != file_list_chunks.end()) {
                    ss_file_list.write(it->second.data(), it->second.size());
                } else {
                    std::cerr << "Lỗi: Thiếu chunk " << i << " của danh sách file từ server.\n";
                    goto request_command;
                }
            }
            std::cout << "\nDanh sách file có sẵn trên server:\n";
            std::string line;
            while (std::getline(ss_file_list, line)) {
                std::cout << "- " << line << "\n"; // In cả dòng, giữ nguyên khoảng trắng
            }
            std::cout << std::endl;
        } else {
            std::cerr << "Không thể tải đầy đủ danh sách file từ server.\n";
        }
    } else {
        std::cerr << "[CLIENT] Không thể lấy metadata cho server_files.txt.\n";
        return 1;
    }

request_command:
    std::string command;
    std::cout << "Nhập lệnh (ví dụ: REQUEST_METADATA:File1.zip, hoặc 'exit' để thoát): ";
    while (std::getline(std::cin, command)) {
        if (command == "exit") {
            break;
        }
        if (command.find("REQUEST_METADATA:") == 0) {
            std::string filename = command.substr(strlen("REQUEST_METADATA:"));
            if (!filename.empty()) {
                // Thêm filename vào input.txt
                std::ofstream input_file(INPUT_FILE, std::ios::app); // Mở để append
                if (input_file.is_open()) {
                    input_file << filename << "\n";
                    input_file.close();
                    std::cout << "Đã thêm \"" << filename << "\" vào input.txt\n";
                } else {
                    std::cerr << "Lỗi khi mở input.txt để ghi\n";
                }
            } else {
                std::cerr << "Lệnh REQUEST_METADATA không hợp lệ\n";
            }
        } else {
            std::cerr << "Lệnh không hợp lệ\n";
        }

        // Sau khi (có thể) đã thêm file vào input.txt, tiến hành đọc và tải
        std::vector<std::string> new_files_to_download = get_new_files();
        for (const auto& filename : new_files_to_download) {
            std::cout << "Bắt đầu xử lý file: " << filename << "\n";
            Metadata metadata;
            int metadata_sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (metadata_sock < 0) {
                std::cerr << "Lỗi tạo socket cho metadata\n";
                continue;
            }
            if (!request_metadata(metadata_sock, server_addr, addr_len, filename, metadata, next_global_seq_num++)) {
                close(metadata_sock);
                std::cerr << "Không thể lấy metadata cho \"" << filename << "\". Bỏ qua file này.\n";
                continue;
            }
            close(metadata_sock);

            uint64_t num_chunks = metadata.num_chunks;
            std::map<uint64_t, std::vector<char>> received_chunks;
            std::vector<std::thread> download_threads;
            std::atomic<uint64_t> chunks_received_count(0);
            bool download_failed = false;

            uint64_t chunks_per_thread = (num_chunks + NUM_DOWNLOAD_THREADS - 1) / NUM_DOWNLOAD_THREADS;
            std::vector<int> download_sockets(NUM_DOWNLOAD_THREADS);
            std::atomic<uint64_t> next_download_seq_num = next_global_seq_num.load();
            std::vector<std::atomic<uint64_t>> thread_received_counts(NUM_DOWNLOAD_THREADS);
            std::vector<uint64_t> total_thread_chunks(NUM_DOWNLOAD_THREADS);

            for (int i = 0; i < NUM_DOWNLOAD_THREADS; ++i) {
                download_sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
                if (download_sockets[i] < 0) {
                    std::cerr << "Lỗi tạo socket download thread " << i << " cho \"" << filename << "\"\n";
                    for (int j = 0; j < i; ++j) close(download_sockets[j]);
                    download_failed = true;
                    break;
                }
                uint64_t start_chunk = i * chunks_per_thread;
                uint64_t end_chunk = std::min((i + 1) * chunks_per_thread, num_chunks);
                total_thread_chunks[i] = end_chunk - start_chunk;
                thread_received_counts[i] = 0;
                if (start_chunk < end_chunk) {
                    download_threads.emplace_back(download_worker, download_sockets[i], std::ref(server_addr), addr_len, filename, start_chunk, end_chunk, std::ref(received_chunks), std::ref(chunks_received_count), num_chunks, std::ref(next_download_seq_num), std::ref(thread_received_counts[i]), total_thread_chunks[i]);
                } else {
                    download_sockets[i] = -1;
                }
            }
            next_global_seq_num = next_download_seq_num.load();

            if (!download_failed) {
                while (chunks_received_count < num_chunks) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));

                    // Di chuyển con trỏ về đầu dòng để ghi đè
                    std::cout << "\r";

                    // Hiển thị tiến độ của từng thread
                    std::cout << "\n";
                    for (int i = 0; i < NUM_DOWNLOAD_THREADS; ++i) {
                        if (download_sockets[i] != -1 && total_thread_chunks[i] > 0) {
                            uint64_t received = thread_received_counts[i].load();
                            double progress = (static_cast<double>(received) / total_thread_chunks[i]) * 100.0;
                            std::cout << "Downloading \"" << filename << "\" part " << i + 1 << " .... " << static_cast<int>(progress) << "%\t\n";
                        } else if (download_sockets[i] != -1) {
                            std::cout << "Downloading \"" << filename << "\" part " << i + 1 << " .... 0%\t\n";
                        }
                    }
                    std::cout.flush();

                    // Nếu bạn muốn hiển thị tổng tiến độ trên dòng riêng
                    uint64_t received_total = chunks_received_count.load();
                    double progress_total = (static_cast<double>(received_total) / num_chunks) * 100.0;
                    std::cout << "\nTổng tiến độ \"" << filename << "\": " << static_cast<int>(progress_total) << "%\r\n\n";
                    std::cout.flush();
                }
                std::cout << "\nĐã tải xong \"" << filename << "\" (" << metadata.file_size << " bytes, " << num_chunks << " chunks)\n";

                for (size_t i = 0; i < download_threads.size(); ++i) {
                    if (download_threads[i].joinable()) {
                        download_threads[i].join();
                    }
                    if (download_sockets[i] != -1) {
                        close(download_sockets[i]);
                    }
                }

                // Nối các chunk lại thành file hoàn chỉnh
                std::string output_path = DOWNLOADS_DIR + filename;
                std::ofstream output_file(output_path, std::ios::binary);
                bool write_failed = false;
                if (output_file.is_open()) {
                    for (uint64_t i = 0; i < num_chunks; ++i) {
                        auto it = received_chunks.find(i);
                        if (it != received_chunks.end()) {
                            output_file.write(it->second.data(), it->second.size());
                        } else {
                            std::cerr << "Lỗi: Thiếu chunk " << i << " của file \"" << filename << "\"\n";
                            output_file.close();
                            std::filesystem::remove(output_path);
                            write_failed = true;
                            break;
                        }
                    }
                    if (!write_failed) {
                        output_file.close();
                        if (std::filesystem::file_size(output_path) == metadata.file_size) {
                            std::cout << "Đã lưu file \"" << filename << "\" thành công tại " << output_path << "\n";
                        } else {
                            std::cerr << "Lỗi: Kích thước file sau khi tải không khớp với metadata của \"" << filename << "\"\n";
                            std::filesystem::remove(output_path);
                        }
                    }
                } else {
                    std::cerr << "Không thể mở file để ghi: " << output_path << "\n";
                }
            } else {
                std::cerr << "Không thể tạo đủ socket để tải \"" << filename << "\". Bỏ qua file này.\n";
                for (int sock : download_sockets) {
                    if (sock != -1) close(sock);
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(5)); // Kiểm tra file input.txt mỗi 5 giây
    }

    close(client_sock);
    std::cout << "Đã đóng kết nối" << std::endl;
    return 0;
}