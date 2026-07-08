# cmake/XPlaneSDK.cmake
# Locates X-Plane SDK headers and libraries.
# Looks in: third_party/SDK, env XPLANE_SDK_PATH, or user-supplied -DXPLANE_SDK_PATH=...

cmake_minimum_required(VERSION 3.15)

set(_SDK_SEARCH_PATHS
    "${CMAKE_SOURCE_DIR}/third_party/SDK"
    "$ENV{XPLANE_SDK_PATH}"
    "${XPLANE_SDK_PATH}"
)

find_path(XPLANE_SDK_INCLUDE_DIR
    NAMES "XPLM/XPLMPlugin.h"
    PATHS ${_SDK_SEARCH_PATHS}
    PATH_SUFFIXES "CHeaders"
    NO_DEFAULT_PATH
)

if(NOT XPLANE_SDK_INCLUDE_DIR)
    message(FATAL_ERROR
        "[CoPilots] X-Plane SDK headers not found.\n"
        "Download the SDK from https://developer.x-plane.com/sdk/plugin-sdk-downloads/\n"
        "and extract it to: ${CMAKE_SOURCE_DIR}/third_party/SDK/\n"
        "Expected path: third_party/SDK/CHeaders/XPLM/XPLMPlugin.h\n"
        "Or set -DXPLANE_SDK_PATH=/path/to/sdk"
    )
endif()

message(STATUS "[CoPilots] X-Plane SDK headers: ${XPLANE_SDK_INCLUDE_DIR}")

# Libraries (only needed on Windows and Linux; macOS links via -framework)
if(WIN32)
    find_library(XPLM_LIBRARY
        NAMES XPLM_64
        PATHS ${_SDK_SEARCH_PATHS}
        PATH_SUFFIXES "Libraries/Win"
        NO_DEFAULT_PATH
    )
    find_library(XPWIDGETS_LIBRARY
        NAMES XPWidgets_64
        PATHS ${_SDK_SEARCH_PATHS}
        PATH_SUFFIXES "Libraries/Win"
        NO_DEFAULT_PATH
    )
    if(NOT XPLM_LIBRARY OR NOT XPWIDGETS_LIBRARY)
        message(FATAL_ERROR
            "[CoPilots] X-Plane SDK .lib files not found in Libraries/Win.\n"
            "Make sure you extracted the full SDK package."
        )
    endif()
    set(XPLANE_SDK_LIBRARIES ${XPLM_LIBRARY} ${XPWIDGETS_LIBRARY})
elseif(APPLE)
    set(XPLANE_SDK_LIBRARIES
        "-framework XPLM"
        "-framework XPWidgets"
        "-F${XPLANE_SDK_INCLUDE_DIR}/../Libraries/Mac"
    )
else()
    # Linux — no import libs needed; symbols resolved at runtime by X-Plane
    set(XPLANE_SDK_LIBRARIES "")
endif()

# Convenience function to apply SDK settings to a target
function(target_link_xplane_sdk TARGET)
    target_include_directories(${TARGET} PRIVATE
        "${XPLANE_SDK_INCLUDE_DIR}/XPLM"
        "${XPLANE_SDK_INCLUDE_DIR}/XPWidgets"
        "${XPLANE_SDK_INCLUDE_DIR}"
    )
    target_compile_definitions(${TARGET} PRIVATE
        XPLM200 XPLM210 XPLM300 XPLM301 XPLM302 XPLM303
        $<$<PLATFORM_ID:Windows>:IBM=1>
        $<$<PLATFORM_ID:Linux>:LIN=1>
        $<$<PLATFORM_ID:Darwin>:APL=1>
    )
    if(XPLANE_SDK_LIBRARIES)
        target_link_libraries(${TARGET} PRIVATE ${XPLANE_SDK_LIBRARIES})
    endif()
endfunction()
