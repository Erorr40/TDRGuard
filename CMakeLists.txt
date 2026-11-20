cmake_minimum_required(VERSION 3.25)
project(TDRGuard VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static runtime linkage
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_DIR ${imgui_SOURCE_DIR})

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

set(TDRGUARD_SOURCES
    src/main.cpp
    src/GpuMonitor.cpp
    src/DriverRecovery.cpp
    src/EventWatcher.cpp
    src/DashboardUI.cpp
    src/resource.rc
)

add_executable(TDRGuard WIN32 ${TDRGUARD_SOURCES} ${IMGUI_SOURCES})

target_include_directories(TDRGuard PRIVATE
    src/
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(TDRGuard PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
)

target_link_libraries(TDRGuard PRIVATE
    d3d11.lib
    dxgi.lib
    dxguid.lib
    d3dcompiler.lib
    wevtapi.lib
    setupapi.lib
    cfgmgr32.lib
    pdh.lib
    comctl32.lib
    shell32.lib
    user32.lib
    gdi32.lib
    ole32.lib
    advapi32.lib
    shlwapi.lib
    dwmapi.lib
)

if(MSVC)
    target_compile_options(TDRGuard PRIVATE /W4 /WX- /permissive- /utf-8 /Zc:__cplusplus)
    # Suppress imgui warnings
    set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_FLAGS "/W0")
endif()

set_target_properties(TDRGuard PROPERTIES
    OUTPUT_NAME "TDRGuard"
    WIN32_EXECUTABLE TRUE
    LINK_FLAGS "/MANIFEST:NO"
)
