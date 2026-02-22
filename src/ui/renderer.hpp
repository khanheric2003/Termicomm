#define STB_IMAGE_IMPLEMENTATION
#include "../../libs/stb_image.h"
#include <string>
#include <vector>
#include <sstream>

std::string render_image_to_ansi(const std::string& path, int target_width) {
    int width, height, channels;
    unsigned char* img = stbi_load(path.c_str(), &width, &height, &channels, 3);
    if (!img) return "Failed to load image";

    int target_height = (height * target_width / width) / 2;
    std::stringstream ss;

    for (int y = 0; y < target_height * 2; y += 2) {
        for (int x = 0; x < target_width; ++x) {
            int img_x = x * width / target_width;
            int img_y1 = y * height / (target_height * 2);
            int img_y2 = (y + 1) * height / (target_height * 2);

            auto get_pixel = [&](int px, int py) {
                int idx = (py * width + px) * 3;
                return std::vector<int>{img[idx], img[idx+1], img[idx+2]};
            };

            auto p1 = get_pixel(img_x, img_y1);
            auto p2 = get_pixel(img_x, img_y2);

            ss << "\033[38;2;" << p1[0] << ";" << p1[1] << ";" << p1[2] << "m"
               << "\033[48;2;" << p2[0] << ";" << p2[1] << ";" << p2[2] << "m▀";
        }
        ss << "\033[0m\n";
    }
    stbi_image_free(img);
    return ss.str();
}