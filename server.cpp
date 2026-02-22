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

using json = nlohmann::json;

struct User { int socket; std::string name; };
std::vector<User> clients;
std::mutex clients_mutex; 

// --- UDP VOICE RELAY ---
std::map<std::string, sockaddr_in> active_voice_users;
std::mutex voice_mutex;

void udp_audio_relay() {
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8081); 
    server_addr.sin_addr.s_addr = INADDR_ANY;

    bind(udp_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    std::cout << "[VOICE] UDP Audio Relay running on port 8081..." << std::endl;

    char audio_buffer[4096];
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);

    while (true) {
        int bytes = recvfrom(udp_sock, audio_buffer, sizeof(audio_buffer), 0, (struct sockaddr*)&client_addr, &client_len);
        if (bytes > 0) {
            std::string client_key = std::string(inet_ntoa(client_addr.sin_addr)) + ":" + std::to_string(ntohs(client_addr.sin_port));
            
            {
                std::lock_guard<std::mutex> lock(voice_mutex);
                if (active_voice_users.find(client_key) == active_voice_users.end()) {
                    active_voice_users[client_key] = client_addr;
                }
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
    char buffer[2048];
    std::string username = "Unknown";
    bool identified = false;

    auto broadcast = [&](const std::string& msg, int ignore_sock = -1) {
        std::lock_guard<std::mutex> lock(clients_mutex);
        for (auto it = clients.begin(); it != clients.end(); ) {
            if (it->socket != ignore_sock) {
                if (send(it->socket, msg.c_str(), msg.length(), 0) < 0) {
                    close(it->socket);
                    it = clients.erase(it);
                    continue;
                }
            }
            ++it;
        }
    };

    while (true) {
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received <= 0) {
            std::cout << "[LOG] User '" << username << "' disconnected." << std::endl;
            json left_msg = {{"op", 5}, {"d", {{"username", username}}}};
            broadcast(left_msg.dump(), client_socket);

            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto it = clients.begin(); it != clients.end(); ++it) {
                if (it->socket == client_socket) {
                    clients.erase(it);
                    break;
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
                std::string sync_str = sync_users.dump();
                send(client_socket, sync_str.c_str(), sync_str.length(), 0);

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
                    
                    // Send Tree
                    std::string tree_str = tree_msg.dump();
                    send(client_socket, tree_str.c_str(), tree_str.length(), 0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));

                    // 4. Send Message History
                    sqlite3_stmt* hist_stmt;
                    std::string sql = "SELECT channel_id, author_name, content FROM messages ORDER BY id ASC;";
                    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &hist_stmt, 0) == SQLITE_OK) {
                        while (sqlite3_step(hist_stmt) == SQLITE_ROW) {
                            int ch_id = sqlite3_column_int(hist_stmt, 0);
                            std::string msg_author = (const char*)sqlite3_column_text(hist_stmt, 1);
                            std::string msg_content = (const char*)sqlite3_column_text(hist_stmt, 2);

                            json hist_msg = {
                                {"op", 0},
                                {"t", "MESSAGE_CREATE"},
                                {"d", {{"content", msg_content}, {"channel_id", ch_id}, {"author", {{"username", msg_author}}}}}
                            };
                            std::string hist_str = hist_msg.dump();
                            send(client_socket, hist_str.c_str(), hist_str.length(), 0);
                            std::this_thread::sleep_for(std::chrono::milliseconds(2)); 
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

            // --- OP 6: VOICE TOGGLE ---
            else if (payload["op"] == 6) {
                bool is_joining = payload["d"]["joining"];
                json voice_msg = {{"op", 6}, {"d", {{"username", username}, {"joining", is_joining}}}};
                broadcast(voice_msg.dump());
            }

            // --- OP 7: CREATE SERVER ---
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

            // --- OP 8: CREATE CHANNEL ---
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

        } catch (json::parse_error& e) {}
    }
    close(client_socket);
}

int main() {
    init_server_db();

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