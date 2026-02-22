#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <arpa/inet.h> 
#include <sys/socket.h>
#include <unistd.h>
#include <mutex>
#include <algorithm>
#include <map>
#include "../include/json.hpp" 
#include <portaudio.h>

using namespace ftxui;
using json = nlohmann::json;

std::mutex chat_mutex;

// SSO
void save_session(std::string user, std::string ip) {
    std::ofstream ofs(".termicomm_session");
    ofs << user << "\n" << ip;
}

bool load_session(std::string& user, std::string& ip) {
    std::ifstream ifs(".termicomm_session");
    if (!ifs.is_open()) return false;
    std::getline(ifs, user);
    std::getline(ifs, ip);
    return true;
}

// Audio set up
#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 512

bool is_mic_active = false;

void start_voice_chat(std::string target_ip) {
    Pa_Initialize();
    is_mic_active = true;

    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8081);
    inet_pton(AF_INET, target_ip.c_str(), &server_addr.sin_addr);

    PaStream *input_stream, *output_stream;
    Pa_OpenDefaultStream(&input_stream, 1, 0, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, nullptr, nullptr);
    Pa_OpenDefaultStream(&output_stream, 0, 1, paFloat32, SAMPLE_RATE, FRAMES_PER_BUFFER, nullptr, nullptr);
    
    Pa_StartStream(input_stream);
    Pa_StartStream(output_stream);

    std::thread recorder([=]() {
        float buffer[FRAMES_PER_BUFFER];
        while (is_mic_active) {
            Pa_ReadStream(input_stream, buffer, FRAMES_PER_BUFFER);
            sendto(udp_sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&server_addr, sizeof(server_addr));
        }
    });
    recorder.detach();

    std::thread player([=]() {
        float buffer[FRAMES_PER_BUFFER];
        while (is_mic_active) {
            int bytes = recv(udp_sock, buffer, sizeof(buffer), 0);
            if (bytes > 0) {
                Pa_WriteStream(output_stream, buffer, FRAMES_PER_BUFFER);
            }
        }
    });
    player.detach();
}

void stop_voice_chat() {
    is_mic_active = false;
    Pa_Terminate();
}


struct Channel { int id; std::string name; };
struct Server { int id; std::string name; std::vector<Channel> channels; };

int main(int argc, char* argv[]) {
    std::string target_ip, username, password;
    
    if (argc == 4) {
        target_ip = argv[1];
        username = argv[2];
        password = argv[3];
        save_session(username, target_ip);
    } 
    else if (load_session(username, target_ip)) {
        if (argc > 1) target_ip = argv[1]; 
    }
    else {
        std::cout << "Usage: ./Termicomm [IP] [USER] [PASS]" << std::endl;
        return 1;
    }

    // Socket Connections
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, target_ip.c_str(), &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed! Ensure termicomm_server is running." << std::endl;
        return -1;
    }

    // Small delay debug
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    json identify_payload = {{"op", 2}, {"d", {{"username", username}, {"password", password}}}};
    std::string id_str = identify_payload.dump() + "\n";
    send(sock, id_str.c_str(), id_str.length(), 0);

    // Debug
    std::cout << "[DEBUG] Identifying as: " << username << std::endl;
    // State 
    std::vector<Server> discord_tree;
    int selected_server = 0;
    int selected_channel = 0;
    int previous_server = -1; 

    std::vector<std::string> server_names;
    std::vector<std::string> channel_names;

    std::map<int, std::vector<std::string>> chat_histories;
    std::vector<std::string> online_users;
    std::vector<std::string> voice_users;
    bool in_voice = false;    
    std::string input_content;
    int scroll_offset = 0; 

    std::string new_server_input;
    std::string new_channel_input;

    // UI 
    auto server_menu = Menu(&server_names, &selected_server);
    auto channel_menu = Menu(&channel_names, &selected_channel);
    auto input_box = Input(&input_content, "Type message...");
    auto new_server_box = Input(&new_server_input, "+ Add Server...");
    auto new_channel_box = Input(&new_channel_input, "+ Add Channel...");

    auto new_server_handler = CatchEvent(new_server_box, [&](Event e) {
        if (e == Event::Return && !new_server_input.empty()) {
            json req = {{"op", 7}, {"d", {{"name", new_server_input}}}};
            std::string payload = req.dump() + "\n";
            send(sock, payload.c_str(), payload.length(), 0);
            new_server_input.clear();
            return true;
        }
        return false;
    });

    auto new_channel_handler = CatchEvent(new_channel_box, [&](Event e) {
        if (e == Event::Return && !new_channel_input.empty() && !discord_tree.empty()) {
            json req = {{"op", 8}, {"d", {{"guild_id", discord_tree[selected_server].id}, {"name", new_channel_input}}}};
            std::string payload = req.dump() + "\n";
            send(sock, payload.c_str(), payload.length(), 0);
            new_channel_input.clear();
            return true;
        }
        return false;
    });

    auto chat_handler = CatchEvent(input_box, [&](Event event) {
        if (event == Event::Return && !input_content.empty()) {
            
            if (input_content.find("/voice") == 0) {
                in_voice = !in_voice;
                if (in_voice) start_voice_chat(target_ip);
                else stop_voice_chat();

                json voice_out = {{"op", 6}, {"d", {{"joining", in_voice}}}};
                std::string v_payload = voice_out.dump() + "\n";
                send(sock, v_payload.c_str(), v_payload.length(), 0);
                input_content.clear();
                return true;
            }

            if (!discord_tree.empty() && !discord_tree[selected_server].channels.empty()) {
                int active_channel_id = discord_tree[selected_server].channels[selected_channel].id;
                json outbound = {
                    {"op", 0}, {"t", "MESSAGE_CREATE"},
                    {"d", {{"content", input_content}, {"channel_id", active_channel_id}}}
                };
                std::string payload = outbound.dump() + "\n";
                send(sock, payload.c_str(), payload.length(), 0);
            }
            
            input_content.clear();
            scroll_offset = 0;
            return true;
        }
        
        if (event == Event::ArrowUp) { scroll_offset++; return true; }
        if (event == Event::ArrowDown) { scroll_offset--; return true; }
        return false;
    });

    auto main_container = Container::Horizontal({
        Container::Vertical({
            server_menu,
            new_server_handler,
            channel_menu,
            new_channel_handler
        }),
        chat_handler
    });

    auto screen = ScreenInteractive::Fullscreen();

    // Listener Thread
    std::thread listener([&]() {
        char buffer[8192]; // Buffer increased for network bridge packets
        while (true) {
            int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes > 0) {
                buffer[bytes] = '\0';
                
                std::string data(buffer);
                size_t pos = 0;
                while ((pos = data.find('\n')) != std::string::npos) {
                    std::string line = data.substr(0, pos);
                    data.erase(0, pos + 1);

                    try {
                        if (line.empty() || line[0] != '{') continue; // Skip garbage
                        json incoming = json::parse(line);
                        std::lock_guard<std::mutex> lock(chat_mutex);

                        if (incoming["op"] == 0) {
                            std::string author = incoming["d"]["author"]["username"];
                            std::string content = incoming["d"]["content"];
                            int ch_id = incoming["d"]["channel_id"];
                            chat_histories[ch_id].push_back(author + ": " + content);
                            
                            if (!discord_tree.empty() && !discord_tree[selected_server].channels.empty()) {
                                if (ch_id == discord_tree[selected_server].channels[selected_channel].id) scroll_offset = 0;
                            }
                        }
                        else if (incoming["op"] == 3) { 
                            online_users.clear();
                            for (auto& u : incoming["d"]) online_users.push_back(u);
                        }
                        else if (incoming["op"] == 4) { 
                            online_users.push_back(incoming["d"]["username"]);
                        }
                        else if (incoming["op"] == 5) { 
                            std::string left_user = incoming["d"]["username"];
                            online_users.erase(std::remove(online_users.begin(), online_users.end(), left_user), online_users.end());
                            voice_users.erase(std::remove(voice_users.begin(), voice_users.end(), left_user), voice_users.end());
                        }
                        else if (incoming["op"] == 6) { 
                            std::string v_user = incoming["d"]["username"];
                            if (incoming["d"]["joining"]) {
                                if (std::find(voice_users.begin(), voice_users.end(), v_user) == voice_users.end()) voice_users.push_back(v_user);
                            } else {
                                voice_users.erase(std::remove(voice_users.begin(), voice_users.end(), v_user), voice_users.end());
                            }
                        }
                        else if (incoming["op"] == 7) { 
                            discord_tree.push_back({incoming["d"]["id"], incoming["d"]["name"], {}});
                        }
                        else if (incoming["op"] == 8) { 
                            int g_id = incoming["d"]["guild_id"];
                            for (auto& s : discord_tree) {
                                if (s.id == g_id) {
                                    s.channels.push_back({incoming["d"]["id"], incoming["d"]["name"]});
                                    break;
                                }
                            }
                        }
                        else if (incoming["op"] == 9) { 
                            discord_tree.clear();
                            for (auto& g : incoming["d"]) {
                                Server new_server{g["id"], g["name"], {}};
                                for (auto& c : g["channels"]) new_server.channels.push_back({c["id"], c["name"]});
                                discord_tree.push_back(new_server);
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[JSON ERR] Caught mangled packet: " << e.what() << std::endl;
                        continue; 
                    }
                }
            } else {
                break; 
            }
        }
    });
    listener.detach();

    std::thread ui_refresher([&]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            screen.PostEvent(Event::Custom);
        }
    });
    ui_refresher.detach();

    // --- RENDERER ---
    auto renderer = Renderer(main_container, [&] {

        std::lock_guard<std::mutex> lock(chat_mutex);
        Elements message_list;
        Elements user_elements;
        
        for (const auto& u : online_users) user_elements.push_back(text(" " + u) | color(u == username ? Color::Green : Color::White));
        
        Elements voice_elements;
        for (const auto& vu : voice_users) voice_elements.push_back(text(" 🔊 " + vu) | dim);

        server_names.clear();
        for (const auto& s : discord_tree) server_names.push_back(s.name);

        if (discord_tree.empty()) {
            return hbox({text("Waiting for server sync...") | center});
        }

        // Prevent Segfaults if vectors change
        if (selected_server >= (int)discord_tree.size()) selected_server = std::max(0, (int)discord_tree.size() - 1);

        if (selected_server != previous_server) {
            channel_names.clear();
            for (const auto& c : discord_tree[selected_server].channels) channel_names.push_back("# " + c.name);
            selected_channel = 0;
            scroll_offset = 0;
            previous_server = selected_server;
        } else {
            // Keep channels updated if new ones are added
            channel_names.clear();
            for (const auto& c : discord_tree[selected_server].channels) channel_names.push_back("# " + c.name);
        }

        std::string current_s_name = discord_tree[selected_server].name;
        std::string current_c_name = "No Channels";
        
        if (!discord_tree[selected_server].channels.empty()) {
            if (selected_channel >= (int)discord_tree[selected_server].channels.size()) selected_channel = std::max(0, (int)discord_tree[selected_server].channels.size() - 1);
            int active_channel_id = discord_tree[selected_server].channels[selected_channel].id;
            current_c_name = "#" + discord_tree[selected_server].channels[selected_channel].name;
            
            auto& current_chat = chat_histories[active_channel_id]; 
            // User can adjust it
            int max_lines_on_screen = 30; 
            int total_msgs = current_chat.size();
            
            if (scroll_offset < 0) scroll_offset = 0;
            int max_scroll = std::max(0, total_msgs - max_lines_on_screen);
            if (scroll_offset > max_scroll) scroll_offset = max_scroll;

            int start_idx = std::max(0, total_msgs - max_lines_on_screen - scroll_offset);
            int end_idx = std::min(total_msgs, start_idx + max_lines_on_screen);

            if (total_msgs < max_lines_on_screen) message_list.push_back(filler());
            for (int i = start_idx; i < end_idx; ++i) message_list.push_back(text(current_chat[i]));
        } else {
            message_list.push_back(filler());
            message_list.push_back(text("Create a channel to start chatting!") | center | dim);
        }

        return hbox({
            // LEFT SIDEBAR
            vbox({ 
                text(" SERVERS ") | bold | center, separator(), 
                server_menu->Render(), separator(), new_server_handler->Render(),
                separator(),
                text(" CHANNELS ") | bold | center, separator(), 
                channel_menu->Render() | flex, separator(), new_channel_handler->Render()
            }) | border | size(WIDTH, EQUAL, 30),
            
            // MIDDLE CHAT
            vbox({
                text(" termicomm // " + current_s_name + " // " + (discord_tree[selected_server].channels.empty() ? "None" : discord_tree[selected_server].channels[selected_channel].name)) | bold | color(Color::Cyan),
                separator(),
                vbox(std::move(message_list)) | flex, 
                separator(),
                hbox({ text(" > ") | bold, chat_handler->Render() })
            }) | flex | border,

            // RIGHT SIDEBAR
            vbox({
                text(" ONLINE (" + std::to_string(online_users.size()) + ") ") | bold | center, separator(), vbox(std::move(user_elements)), filler(),
                text(" VOICE (" + std::to_string(voice_users.size()) + ") ") | bold | center, separator(), vbox(std::move(voice_elements))
            }) | border | size(WIDTH, EQUAL, 20)
        });
    });

    screen.Loop(renderer);
    close(sock);
    return 0;
}