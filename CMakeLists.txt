cmake_minimum_required(VERSION 3.15)
project(CoPilots VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Output directories ─────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")

# ── X-Plane SDK ───────────────────────────────────────────────────────────
include(cmake/XPlaneSDK.cmake)

# ── Third-party dependencies (FetchContent) ───────────────────────────────
include(FetchContent)

# Dear ImGui (docking branch)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# nlohmann/json single-header: download json.hpp into nlohmann/ subdir so that
# #include <nlohmann/json.hpp> resolves correctly.
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_BINARY_DIR}/nlohmann_include")
file(MAKE_DIRECTORY "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann")
if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
    message(STATUS "Downloading nlohmann/json v3.11.3...")
    file(DOWNLOAD
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp"
        SHOW_PROGRESS
    )
endif()

# ── Source files ───────────────────────────────────────────────────────────
set(COPILOTS_SOURCES
    src/plugin.cpp
    src/Log.cpp

    src/config/Config.cpp
    src/config/SmartCopilotCompat.cpp

    src/net/Transport.cpp
    src/net/NetThread.cpp

    src/sync/DatarefRegistry.cpp
    src/sync/SyncEngine.cpp
    src/sync/PhysicsSync.cpp
    src/sync/WeatherSync.cpp
    src/sync/AutoDatarefSync.cpp

    src/session/Session.cpp

    src/ui/XPImguiWindow.cpp
    src/ui/ImguiBackend.cpp
    src/ui/Theme.cpp
    src/ui/ConnectionWindow.cpp
    src/ui/LobbyWindow.cpp
    src/ui/StatusHud.cpp
    src/ui/NotepadWindow.cpp
)

# Dear ImGui source files
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

# ── Plugin target ─────────────────────────────────────────────────────────
add_library(CoPilots SHARED
    ${COPILOTS_SOURCES}
    ${IMGUI_SOURCES}
)

# Include paths
target_include_directories(CoPilots PRIVATE
    src/
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    "${NLOHMANN_JSON_INCLUDE_DIR}"
)

# X-Plane SDK
target_link_xplane_sdk(CoPilots)

# ── Platform-specific settings ─────────────────────────────────────────────
if(WIN32)
    # X-Plane expects win.xpl (a renamed DLL)
    set_target_properties(CoPilots PROPERTIES
        OUTPUT_NAME  "win"
        SUFFIX       ".xpl"
        PREFIX       ""
    )
    target_link_libraries(CoPilots PRIVATE opengl32 ws2_32)
    target_compile_definitions(CoPilots PRIVATE _CRT_SECURE_NO_WARNINGS)

    # Ensure we export the required plugin symbols
    target_sources(CoPilots PRIVATE "${CMAKE_SOURCE_DIR}/cmake/win_exports.def")

elseif(APPLE)
    set_target_properties(CoPilots PROPERTIES
        OUTPUT_NAME  "mac"
        SUFFIX       ".xpl"
        PREFIX       ""
        LINK_FLAGS   "-bundle -undefined dynamic_lookup"
    )
    find_library(OPENGL_LIB OpenGL)
    target_link_libraries(CoPilots PRIVATE ${OPENGL_LIB})

else()
    # Linux
    set_target_properties(CoPilots PROPERTIES
        OUTPUT_NAME  "lin"
        SUFFIX       ".xpl"
        PREFIX       ""
        LINK_FLAGS   "-Wl,--version-script=${CMAKE_SOURCE_DIR}/cmake/lin_exports.sym"
    )
    target_link_libraries(CoPilots PRIVATE GL)
    target_compile_options(CoPilots PRIVATE -fvisibility=hidden)
endif()

# ── Post-build: copy to plugin installation layout ─────────────────────────
# Build output: build/out/win.xpl (Windows)
# Final layout: build/CoPilots/64/win.xpl
# Copy configs/ next to the .xpl parent
add_custom_command(TARGET CoPilots POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_BINARY_DIR}/install/CoPilots/64"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:CoPilots>
        "${CMAKE_BINARY_DIR}/install/CoPilots/64/$<TARGET_FILE_NAME:CoPilots>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/configs"
        "${CMAKE_BINARY_DIR}/install/CoPilots/configs"
    COMMENT "Staging plugin to build/install/CoPilots/"
)

message(STATUS "CoPilots build configured.")
message(STATUS "  After building, copy build/install/CoPilots/ to:")
message(STATUS "  X-Plane 11/Resources/plugins/")
