cmake_minimum_required(VERSION 3.16)
project(MDedit CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static MSVC runtime: self-contained exe, no VC++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32)
    message(FATAL_ERROR "MDedit is Windows-only")
endif()

# WebView2 SDK: prefer a local copy under build/packages, else reuse the
# package already restored by the JAMD_lister plugin build.
set(WEBVIEW2_VER "Microsoft.Web.WebView2.1.0.2592.51")
set(WEBVIEW2_SDK_DIR "${CMAKE_BINARY_DIR}/packages/${WEBVIEW2_VER}")
if(NOT EXISTS "${WEBVIEW2_SDK_DIR}/build/native/include/WebView2.h")
    set(_plugin_sdk "B:/ViBE_code/MDLister_WLX-codex/build/packages/${WEBVIEW2_VER}")
    if(EXISTS "${_plugin_sdk}/build/native/include/WebView2.h")
        set(WEBVIEW2_SDK_DIR "${_plugin_sdk}")
    else()
        message(FATAL_ERROR "WebView2 SDK not found. Restore ${WEBVIEW2_VER} into build/packages/.")
    endif()
endif()

include_directories(
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WEBVIEW2_SDK_DIR}/build/native/include"
)

add_executable(MDedit WIN32
    src/main.cpp
    src/resources.rc
)

target_link_libraries(MDedit PRIVATE
    "${WEBVIEW2_SDK_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    user32.lib
    gdi32.lib
    shlwapi.lib
    shell32.lib
    ole32.lib
    oleaut32.lib
    uuid.lib
    comctl32.lib
    dwmapi.lib
    advapi32.lib
    crypt32.lib
    pathcch.lib
)

# App renamed: Just Another MD Editor — JAMD Editor.
set_target_properties(MDedit PROPERTIES OUTPUT_NAME "JAMDedit")

if(MSVC)
    target_compile_options(MDedit PRIVATE /W4 /utf-8)
    target_compile_definitions(MDedit PRIVATE UNICODE _UNICODE WIN32 _WINDOWS)
    # Manifest is embedded via resources.rc; stop the linker adding its own.
    target_link_options(MDedit PRIVATE /MANIFEST:NO)
endif()

# Runtime assets next to the exe: drop-in libraries and color themes.
add_custom_command(TARGET MDedit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/lib" "$<TARGET_FILE_DIR:MDedit>/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/themes" "$<TARGET_FILE_DIR:MDedit>/themes"
)

install(TARGETS MDedit RUNTIME DESTINATION ".")
install(DIRECTORY lib themes DESTINATION ".")
