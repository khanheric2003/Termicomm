cmake_minimum_required(VERSION 3.14)
project(Termicomm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)


include(FetchContent)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

find_package(PkgConfig REQUIRED)

pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

find_package(SQLite3 REQUIRED) 

pkg_check_modules(OPUS REQUIRED opus)

add_executable(Termicomm src/main.cpp)

target_link_libraries(Termicomm PRIVATE 
    ftxui::screen 
    ftxui::dom 
    ftxui::component
    SQLite::SQLite3    
    ${OPUS_LIBRARIES}
    ${PORTAUDIO_LIBRARIES}
)

target_include_directories(Termicomm PRIVATE 
    ${OPUS_INCLUDE_DIRS} 
    ${PORTAUDIO_INCLUDE_DIRS}
)