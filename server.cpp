#include <iostream>
#include <vector>
#include <thread>
#include <netinet/in.h>
#include <unistd.h>
#include <sqlite3.h>
#include <mutex>
#include <map>
#include <arpa/inet.h>
#include "include/json.hpp" 
#include "include/base64.hpp"
#include <sys/socket.h>
#include <fstream>
#include <filesystem>
namespace fs = std::filesystem;
using json = nlohmann::json;

struct User { int socket; std::string name; };
std::vector<User> clients;
std::mutex clients_mutex; 

void init_storage() {
    if (!fs::exists("shared_files")) {
        fs::create_directory("shared_files");
    }
}

// --- UDP VOICE RELAY ---
std::map<std::string, sockaddr_in> active_voice_users;
std::mutex voice_mutex;

void udp_audio_relay() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8081); 
    server_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "[VOICE] UDP Bind failed!" << std::endl;
        return;
    }
    
    std::cout << "[VOICE] UDP Audio Relay running on port 8081..." << std::endl;

    char audio_buffer[4096];
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int bytes = recvfrom(udp_sock, audio_buffer, sizeof(audio_buffer), 0, (struct sockaddr*)&client_addr, &client_len);
        if (bytes > 0) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            std::string client_key = std::string(ip_str) + ":" + std::to_string(ntohs(client_addr.sin_port));
            
            {
                std::lock_guard<std::mutex> lock(voice_mutex);
                active_voice_users[client_key] = client_addr;
            }

            std::lock_guard<std::mutex> lock(voice_mutex);
            for (auto const& [key, addr] : active_voice_users) {
                if (key != client_key) {
                    sendto(udp_sock, audio_buffer, bytes, 0, (struct sockaddr*)&addr, sizeof(addr));
                }
            }
        }
    }
}

// DATABASE SCHEMA 
void init_server_db() {
    sqlite3* db;
    sqlite3_open("termicomm_server.db", &db);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS users (username TEXT PRIMARY KEY, password TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS messages (id INTEGER PRIMARY KEY, channel_id INTEGER, author_name TEXT, content TEXT, timestamp DATETIME);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS guilds (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);", 0, 0, 0);
    sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS channels (id INTEGER PRIMARY KEY AUTOINCREMENT, guild_id INTEGER, name TEXT);", 0, 0, 0);
    sqlite3_exec(db, "INSERT OR IGNORE INTO guilds (id, name) VALUES (1, 'General Lobby');", 0, 0, 0);
    sqlite3_exec(db, "INSERT OR IGNORE INTO channels (id, guild_id, name) VALUES (1, 1, 'general');", 0, 0, 0);
    sqlite3_close(db);
}

void handle_client(int client_socket) {
    char buffer[8192]; 
    std::string username = "Unknown";
    bool identified = false;

    auto broadcast = [&](const std::string& msg, int ignore_sock = -1) {
        std::string formatted_msg = msg + "\n";
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (it->socket != ignore_sock) {
                if (send(it->socket, formatted_msg.c_str(), formatted_msg.length(), 0) < 0) {
                    close(it->socket);
                    it = clients.erase(it);
                    continue;
                }
            }
            ++it;
        }
    };

    while (true) {
        memset(buffer, 0, sizeof(buffer));
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            std::cout << "[LOG] User '" << username << "' disconnected." << std::endl;
            json left_msg = {{"op", 5}, {"d", {{"username", username}}}};
            broadcast(left_msg.dump(), client_socket);

            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->socket == client_socket) {
                    clients.erase(it); break;
                }
            }
            break; 
        }

        buffer[bytes_received] = '\0';
        
        try {
            json payload = json::parse(buffer);

            // OP 2
            if (payload["op"] == 2 && !identified) {
                username = payload["d"]["username"];
                std::cout << "[LOGIN] User '" << username << "' identified." << std::endl;
                
                std::vector<std::string> current_users;
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    clients.push_back({client_socket, username});
                    for (auto& u : clients) current_users.push_back(u.name);
                }
                identified = true;

                json sync_users = {{"op", 3}, {"d", current_users}};
                std::string sync_str = sync_users.dump() + "\n";
                send(client_socket, sync_str.c_str(), sync_str.length(), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                json join_msg = {{"op", 4}, {"d", {{"username", username}}}};
                broadcast(join_msg.dump(), client_socket);

                // OP 9
                json tree_msg = {{"op", 9}, {"d", json::array()}};
                sqlite3* db;
                if (sqlite3_open("termicomm_server.db", &db) == SQLITE_OK) {
                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db, "SELECT id, name FROM guilds;", -1, &stmt, 0) == SQLITE_OK) {
                        while(sqlite3_step(stmt) == SQLITE_ROW) {
                            int g_id = sqlite3_column_int(stmt, 0);
                            std::string g_name = (const char*)sqlite3_column_text(stmt, 1);
                            json guild_obj = {{"id", g_id}, {"name", g_name}, {"channels", json::array()}};
                            
                            sqlite3_stmt* stmt2;
                            if (sqlite3_prepare_v2(db, "SELECT id, name FROM channels WHERE guild_id = ?;", -1, &stmt2, 0) == SQLITE_OK) {
                                sqlite3_bind_int(stmt2, 1, g_id);
                                while(sqlite3_step(stmt2) == SQLITE_ROW) {
                                    guild_obj["channels"].push_back({
                                        {"id", sqlite3_column_int(stmt2, 0)},
                                        {"name", (const char*)sqlite3_column_text(stmt2, 1)}
                                    });
                                }
                                sqlite3_finalize(stmt2);
                            }
                            tree_msg["d"].push_back(guild_obj);
                        }
                    }
                    sqlite3_finalize(stmt);
                    
                    std::string tree_str = tree_msg.dump() + "\n";
                    send(client_socket, tree_str.c_str(), tree_str.length(), 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                    sqlite3_stmt* hist_stmt;
                    std::string sql = "SELECT channel_id, author_name, content FROM messages ORDER BY id ASC;";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &hist_stmt, 0) == SQLITE_OK) {
                        while (sqlite3_step(hist_stmt) == SQLITE_ROW) {
                            int ch_id = sqlite3_column_int(hist_stmt, 0);
                            std::string msg_author = (const char*)sqlite3_column_text(hist_stmt, 1);
                            std::string msg_content = (const char*)sqlite3_column_text(hist_stmt, 2);

                            json hist_msg = {
                                {"op", 0}, {"t", "MESSAGE_CREATE"},
                                {"d", {{"content", msg_content}, {"channel_id", ch_id}, {"author", {{"username", msg_author}}}}}
                            };
                            std::string hist_str = hist_msg.dump() + "\n";
                            send(client_socket, hist_str.c_str(), hist_str.length(), 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
                        }
                    }
                    sqlite3_finalize(hist_stmt);
                    sqlite3_close(db);
                }
            }
            
            // OP 0
            else if (payload["op"] == 0) {
                std::string content = payload["d"]["content"];
                int channel_id = payload["d"]["channel_id"];

                sqlite3* db;
                if (sqlite3_open("termicomm_server.db", &db) == SQLITE_OK) {
                    sqlite3_stmt* stmt;
                    std::string sql = "INSERT INTO messages (channel_id, author_name, content, timestamp) VALUES (?, ?, ?, datetime('now'));";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, 0) == SQLITE_OK) {
                        sqlite3_bind_int(stmt, 1, channel_id);
                        sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_STATIC);
                        sqlite3_bind_text(stmt, 3, content.c_str(), -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                    }
                    sqlite3_finalize(stmt);
                    sqlite3_close(db);
                }

                json outbound = {
                    {"op", 0}, {"t", "MESSAGE_CREATE"},
                    {"d", {{"content", content}, {"channel_id", channel_id}, {"author", {{"username", username}}}}}
                };
                broadcast(outbound.dump());
            }

            else if (payload["op"] == 6) {
                bool is_joining = payload["d"]["joining"];
                json voice_msg = {{"op", 6}, {"d", {{"username", username}, {"joining", is_joining}}}};
                broadcast(voice_msg.dump());
            }

            else if (payload["op"] == 7) {
                std::string server_name = payload["d"]["name"];
                int new_guild_id = -1;

                sqlite3* db;
                if (sqlite3_open("termicomm_server.db", &db) == SQLITE_OK) {
                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db, "INSERT INTO guilds (name) VALUES (?);", -1, &stmt, 0) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, server_name.c_str(), -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        new_guild_id = sqlite3_last_insert_rowid(db);
                    }
                    sqlite3_finalize(stmt);
                    sqlite3_close(db);
                }

                if (new_guild_id != -1) {
                    json outbound = {{"op", 7}, {"d", {{"id", new_guild_id}, {"name", server_name}}}};
                    broadcast(outbound.dump());
                }
            }

            else if (payload["op"] == 8) {
                int target_guild_id = payload["d"]["guild_id"];
                std::string channel_name = payload["d"]["name"];
                int new_channel_id = -1;

                sqlite3* db;
                if (sqlite3_open("termicomm_server.db", &db) == SQLITE_OK) {
                    sqlite3_stmt* stmt;
                    if (sqlite3_prepare_v2(db, "INSERT INTO channels (guild_id, name) VALUES (?, ?);", -1, &stmt, 0) == SQLITE_OK) {
                        sqlite3_bind_int(stmt, 1, target_guild_id);
                        sqlite3_bind_text(stmt, 2, channel_name.c_str(), -1, SQLITE_STATIC);
                        sqlite3_step(stmt);
                        new_channel_id = sqlite3_last_insert_rowid(db);
                    }
                    sqlite3_finalize(stmt);
                    sqlite3_close(db);
                }

                if (new_channel_id != -1) {
                    json outbound = {{"op", 8}, {"d", {{"id", new_channel_id}, {"guild_id", target_guild_id}, {"name", channel_name}}}};
                    broadcast(outbound.dump());
                }
            }

            // --- FILE UPLOAD (BASE64 DECODE) ---
            else if (payload["op"] == 10) {
                std::string filename = payload["d"]["filename"];
                std::string encoded_data = payload["d"]["data"];
                int channel_id = payload["d"]["channel_id"];

                init_storage();
                std::string decoded_data = base64_decode(encoded_data); // DECODE TO BINARY

                std::ofstream outfile("shared_files/" + filename, std::ios::binary);
                if (outfile.is_open()) {
                    outfile << decoded_data;
                    outfile.close();

                    json announce = {
                        {"op", 0}, {"t", "MESSAGE_CREATE"},
                        {"d", {
                            {"content", "[FILE UPLOADED]: " + filename}, 
                            {"channel_id", channel_id}, 
                            {"author", {{"username", "SYSTEM"}}}
                        }}
                    };
                    broadcast(announce.dump());
                }
            }

            // --- OP 11: REQUEST FILE LIST ---
            else if (payload["op"] == 11) {
                json file_list = json::array();
                if (fs::exists("shared_files")) {
                    for (const auto& entry : fs::directory_iterator("shared_files")) {
                        file_list.push_back(entry.path().filename().string());
                    }
                }
                json response = {{"op", 11}, {"d", file_list}};
                std::string p = response.dump() + "\n";
                send(client_socket, p.c_str(), p.length(), 0);
            }

            // --- OP 12: REQUEST FILE DOWNLOAD (BASE64 ENCODE) ---
            else if (payload["op"] == 12) {
                std::string filename = payload["d"]["filename"];
                std::ifstream file("shared_files/" + filename, std::ios::binary);
                if (file.is_open()) {
                    std::string raw_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    std::string encoded_content = base64_encode(raw_content); // ENCODE TO BASE64
                    
                    json response = {{"op", 12}, {"d", {{"filename", filename}, {"data", encoded_content}}}};
                    std::string p = response.dump() + "\n";
                    send(client_socket, p.c_str(), p.length(), 0);
                }
            }            

        } catch (json::parse_error& e) {
             std::cerr << "[ERR] Parse Fail: " << e.what() << std::endl;
        }
    }
    close(client_socket);
}

int main() {
    init_server_db();
    init_storage();
    
    // Start UDP Audio Relay
    std::thread(udp_audio_relay).detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{AF_INET, htons(8080), INADDR_ANY};
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);

    std::cout << "Termicomm Server (JSON Gateway) running on port 8080..." << std::endl;
    while (true) {
        int new_socket = accept(server_fd, nullptr, nullptr);
        std::thread(handle_client, new_socket).detach();
    }
    return 0;
}