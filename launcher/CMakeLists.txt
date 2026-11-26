# CHDLauncher: the menu that installs and starts the game. It holds no game
# code, so it is built once by the maintainer and shipped in the release.
#   build_launcher.bat   (uses the portable llvm-mingw in cache/toolchain)
cmake_minimum_required(VERSION 3.20)
project(CHDLauncher CXX RC)

add_executable(CHDLauncher WIN32 launcher.cpp launcher.rc)
target_compile_features(CHDLauncher PRIVATE cxx_std_17)
target_compile_definitions(CHDLauncher PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(CHDLauncher PRIVATE d2d1 dwrite dwmapi ole32 shell32 uuid user32)
if(MINGW)
    target_link_options(CHDLauncher PRIVATE -municode -static)
endif()
