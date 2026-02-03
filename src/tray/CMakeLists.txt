# Tray application - independent system tray for Lemonade Server
# This is a standalone application that communicates with lemonade-server via HTTP API

# Platform-specific sources
set(PLATFORM_SOURCES "")
set(PLATFORM_LIBS "")
set(PLATFORM_INCLUDE_DIRS "")

if(WIN32)
    set(PLATFORM_SOURCES
        platform/windows_tray.cpp
    )
    set(PLATFORM_LIBS
        user32
        shell32
        ole32
        oleaut32
        comctl32
    )

elseif(APPLE)
    set(PLATFORM_SOURCES
        platform/macos_tray.mm
    )

    find_library(COCOA_LIBRARY Cocoa REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)

    set(PLATFORM_LIBS
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
    )

    set_source_files_properties(
        platform/macos_tray.mm
        PROPERTIES
        COMPILE_FLAGS "-x objective-c++"
    )

elseif(UNIX)  # Linux
    # Find GTK3, AppIndicator (try both ayatana and original), and libnotify
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 gtk+-3.0)

    # Try ayatana-appindicator first (modern), then fall back to original
    pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
    if(NOT APPINDICATOR_FOUND)
        pkg_check_modules(APPINDICATOR appindicator3-0.1)
    endif()

    pkg_check_modules(LIBNOTIFY libnotify)

    if(NOT GTK3_FOUND OR NOT APPINDICATOR_FOUND OR NOT LIBNOTIFY_FOUND)
        message(WARNING "=== Tray app dependencies not found ===")
        if(NOT GTK3_FOUND)
            message(WARNING "  Missing: gtk+-3.0 (install libgtk-3-dev)")
        endif()
        if(NOT APPINDICATOR_FOUND)
            message(WARNING "  Missing: appindicator3 (install libayatana-appindicator3-dev or libappindicator3-dev)")
        endif()
        if(NOT LIBNOTIFY_FOUND)
            message(WARNING "  Missing: libnotify (install libnotify-dev)")
        endif()
        message(WARNING "Skipping lemonade-server-tray build")
        message(WARNING "=========================================")
        return()
    endif()

    set(PLATFORM_SOURCES
        platform/linux_tray.cpp
    )

    set(PLATFORM_INCLUDE_DIRS
        ${GTK3_INCLUDE_DIRS}
        ${APPINDICATOR_INCLUDE_DIRS}
        ${LIBNOTIFY_INCLUDE_DIRS}
    )

    set(PLATFORM_LIBS
        ${GTK3_LIBRARIES}
        ${APPINDICATOR_LIBRARIES}
        ${LIBNOTIFY_LIBRARIES}
    )

    # Add compile flags
    add_compile_options(${GTK3_CFLAGS_OTHER})
endif()

# Common sources
set(COMMON_SOURCES
    main.cpp
    tray_app.cpp
    server_client.cpp
    platform/tray_factory.cpp
)

# Create executable
add_executable(lemonade-server-tray
    ${COMMON_SOURCES}
    ${PLATFORM_SOURCES}
)

# Include directories
target_include_directories(lemonade-server-tray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src/cpp/include
    ${CMAKE_BINARY_DIR}/include
    ${PLATFORM_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(lemonade-server-tray PRIVATE
    nlohmann_json::nlohmann_json
    ${PLATFORM_LIBS}
)

# Link httplib
if(USE_SYSTEM_HTTPLIB)
    target_link_libraries(lemonade-server-tray PRIVATE cpp-httplib)
else()
    target_link_libraries(lemonade-server-tray PRIVATE httplib::httplib)
endif()

if(USE_SYSTEM_HTTPLIB AND HTTPLIB_INCLUDE_DIRS)
    target_include_directories(lemonade-server-tray PRIVATE ${HTTPLIB_INCLUDE_DIRS})
endif()

# Link CLI11
if(USE_SYSTEM_CLI11)
    target_include_directories(lemonade-server-tray PRIVATE ${CLI11_INCLUDE_DIRS})
else()
    target_link_libraries(lemonade-server-tray PRIVATE CLI11::CLI11)
endif()

# Set output directory
set_target_properties(lemonade-server-tray PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
if(WIN32)
    set_target_properties(lemonade-server-tray PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# Platform-specific settings
if(WIN32)
    target_compile_definitions(lemonade-server-tray PRIVATE
        UNICODE
        _UNICODE
    )
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(lemonade-server-tray PRIVATE
        Threads::Threads
    )
endif()

# Install target
install(TARGETS lemonade-server-tray
    RUNTIME DESTINATION bin
)

# Create symlink in standard bin path only if not installing to /usr
if(UNIX AND NOT APPLE AND NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                ${CMAKE_INSTALL_PREFIX}/bin/lemonade-server-tray
                \"\$ENV{DESTDIR}/usr/bin/lemonade-server-tray\"
        )
    ")
endif()

# Print configuration summary
message(STATUS "=== Lemonade Tray Application Configuration ===")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
if(WIN32)
    message(STATUS "Windows tray support: YES")
elseif(APPLE)
    message(STATUS "macOS tray support: STUB (not implemented)")
elseif(UNIX)
    message(STATUS "Linux tray support: YES (AppIndicator)")
    message(STATUS "  GTK3: ${GTK3_VERSION}")
    message(STATUS "  AppIndicator: ${APPINDICATOR_VERSION}")
    message(STATUS "  libnotify: ${LIBNOTIFY_VERSION}")
endif()
message(STATUS "===============================================")
