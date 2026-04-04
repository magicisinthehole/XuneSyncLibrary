cmake_minimum_required(VERSION 3.10)
project(xune_sync_library)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build options
option(ZUNE_BUILD_TESTS "Build test executables" OFF)
option(ZUNE_BUILD_TOOLS "Build tool executables" OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Build tests: ${ZUNE_BUILD_TESTS}")
message(STATUS "Build tools: ${ZUNE_BUILD_TOOLS}")

# AFTL library configuration
set(AFTL_ROOT "${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux")
set(AFTL_BUILD_DIR "${CMAKE_BINARY_DIR}/vendor/android-file-transfer-linux-build")
if(WIN32)
    set(AFTL_LIBRARY "${AFTL_BUILD_DIR}/${CMAKE_BUILD_TYPE}/mtp-ng-static.lib")
else()
    set(AFTL_LIBRARY "${AFTL_BUILD_DIR}/libmtp-ng-static.a")
endif()

# On Linux, use libusb backend for IOKit parity (synchronous blocking transfers)
if(UNIX AND NOT APPLE)
    if(EXISTS "/opt/libusb/lib/pkgconfig")
        set(ENV{PKG_CONFIG_PATH} "/opt/libusb/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
    endif()
endif()

# Check if AFTL is already built
if(NOT EXISTS "${AFTL_LIBRARY}")
    message(STATUS "AFTL library not found, building it first...")

    # Create build directory
    file(MAKE_DIRECTORY ${AFTL_BUILD_DIR})

    # Configure AFTL
    set(AFTL_CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DBUILD_SHARED_LIB=OFF
        -DBUILD_QT_UI=OFF
        -DBUILD_PYTHON=OFF
        -DBUILD_FUSE=OFF
        -DBUILD_MTPZ=ON
    )
    if(UNIX AND NOT APPLE)
        list(APPEND AFTL_CMAKE_ARGS -DUSB_BACKEND_LIBUSB=ON)
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        list(APPEND AFTL_CMAKE_ARGS -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES})
    endif()
    if(CMAKE_PREFIX_PATH)
        list(APPEND AFTL_CMAKE_ARGS -DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH})
    endif()
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND AFTL_CMAKE_ARGS -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE})
    endif()

    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${AFTL_ROOT}
            -B ${AFTL_BUILD_DIR}
            ${AFTL_CMAKE_ARGS}
        RESULT_VARIABLE AFTL_CMAKE_RESULT
    )

    if(NOT AFTL_CMAKE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure AFTL library")
    endif()

    # Build AFTL
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${AFTL_BUILD_DIR} --target mtp-ng-static --config ${CMAKE_BUILD_TYPE}
        RESULT_VARIABLE AFTL_BUILD_RESULT
    )

    if(NOT AFTL_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to build AFTL library")
    endif()

    message(STATUS "AFTL library built successfully")
else()
    message(STATUS "Using existing AFTL library at ${AFTL_LIBRARY}")
endif()

# Find dependencies
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# Linux: prefer static libcrypto to eliminate runtime OpenSSL version dependency
if(UNIX AND NOT APPLE)
    find_library(OPENSSL_CRYPTO_STATIC NAMES libcrypto.a
        PATHS ${OPENSSL_ROOT_DIR}/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib)
    if(OPENSSL_CRYPTO_STATIC)
        message(STATUS "Using static libcrypto: ${OPENSSL_CRYPTO_STATIC}")
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_CRYPTO_STATIC})
    endif()
endif()

# Find TagLib (required by AFTL Metadata)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(TAGLIB QUIET taglib)
    if(TAGLIB_FOUND)
        link_directories(${TAGLIB_LIBRARY_DIRS})
    endif()
endif()

if(NOT TAGLIB_FOUND)
    find_library(TAGLIB_LIBRARIES NAMES tag
        PATHS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu)
    find_path(TAGLIB_INCLUDE_DIRS NAMES taglib/tag.h
        PATHS /opt/homebrew/include /usr/local/include /usr/include)
    if(TAGLIB_LIBRARIES AND TAGLIB_INCLUDE_DIRS)
        set(TAGLIB_FOUND TRUE)
    endif()
endif()

if(NOT TAGLIB_FOUND)
    if(APPLE)
        message(FATAL_ERROR "TagLib not found. Install with: brew install taglib")
    elseif(WIN32)
        message(FATAL_ERROR "TagLib not found. Install with vcpkg: vcpkg install taglib")
    else()
        message(FATAL_ERROR "TagLib not found. Build TagLib 2 from source: https://taglib.org/releases/")
    endif()
endif()

message(STATUS "TagLib library: ${TAGLIB_LIBRARIES}")
message(STATUS "TagLib include: ${TAGLIB_INCLUDE_DIRS}")

# Platform specific settings
if(APPLE)
    find_library(CORE_LIBRARY CoreFoundation)
    find_library(IOKIT_LIBRARY IOKit)
    set(PLATFORM_LIBRARIES ${IOKIT_LIBRARY} ${CORE_LIBRARY})
elseif(WIN32)
    set(PLATFORM_LIBRARIES setupapi winusb urlmon shlwapi ws2_32 cfgmgr32)
else()
    set(PLATFORM_LIBRARIES)
endif()

# ── Shared macros ─────────────────────────────────────────────────────────
macro(xune_target_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W3)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    endif()
endmacro()

# AFTL include directories
set(AFTL_INCLUDE_DIRS
    ${AFTL_ROOT}
    ${AFTL_ROOT}/mtp
    ${AFTL_ROOT}/cli
)
if(NOT WIN32)
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/posix)
endif()

if(APPLE)
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/darwin)
elseif(WIN32)
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/windows)
elseif(UNIX)
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/libusb)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    list(APPEND AFTL_INCLUDE_DIRS ${LIBUSB_INCLUDE_DIRS})
endif()

# CLI library sources (needed for cli::Session)
set(CLI_SOURCES
    ${AFTL_ROOT}/cli/Command.cpp
    ${AFTL_ROOT}/cli/CommandLineStub.cpp
    ${AFTL_ROOT}/cli/Session.cpp
    ${AFTL_ROOT}/cli/Tokenizer.cpp
    ${AFTL_ROOT}/cli/arg_lexer.l.cpp
)

# Build CLI library as static library
add_library(aft-cli-static STATIC ${CLI_SOURCES})

target_include_directories(aft-cli-static PUBLIC
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

if(MSVC)
    target_compile_options(aft-cli-static PRIVATE /W3)
    target_compile_definitions(aft-cli-static PRIVATE YY_NO_UNISTD_H NOMINMAX)
else()
    target_compile_options(aft-cli-static PRIVATE -Wall -Wno-unused-parameter)
endif()

# Main library
add_library(xune_sync SHARED
    lib/src/ZuneDevice.cpp
    lib/src/ZuneDeviceIdentification.cpp
    lib/src/NetworkManager.cpp

    lib/src/ZuneMtpReader.cpp
    lib/src/ZuneMtpWriter.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/xune_sync_api.cpp
    lib/src/xune_driver_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/MetadataRequestHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

# Windows-only driver query sources (SetupAPI, no libwdi dependency)
if(WIN32)
    target_sources(xune_sync PRIVATE lib/src/driver_manager.cpp)
endif()

target_include_directories(xune_sync PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${TAGLIB_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(xune_sync
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARIES}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

if(WIN32)
    target_compile_definitions(xune_sync PRIVATE ZUNE_WIRELESS_EXPORTS)
endif()

# Hide all symbols by default — only XUNE_SYNC_API functions are exported.
# C_VISIBILITY_PRESET hidden handles symbols from our own source files.
# --exclude-libs,ALL handles symbols from statically linked archives (OpenSSL,
# libusb, etc.) that would otherwise leak into the global table and crash when
# the system loads its own version (e.g., libssl calling our EC_GROUP_order_bits).
set_target_properties(xune_sync PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
if(UNIX AND NOT APPLE)
    target_link_options(xune_sync PRIVATE -Wl,--exclude-libs,ALL)
endif()

# Link libusb for Linux USB backend (static in Docker, dynamic for local dev)
if(UNIX AND NOT APPLE)
    find_library(LIBUSB_STATIC NAMES libusb-1.0.a
        PATHS /opt/libusb/lib NO_DEFAULT_PATH)
    if(LIBUSB_STATIC)
        target_link_libraries(xune_sync ${LIBUSB_STATIC})
        find_library(UDEV_LIBRARY udev)
        if(UDEV_LIBRARY)
            target_link_libraries(xune_sync ${UDEV_LIBRARY})
        endif()
    else()
        target_link_libraries(xune_sync ${LIBUSB_LIBRARIES})
    endif()
endif()

# macOS: use @loader_path for hardened runtime compatibility
if(APPLE)
    set_target_properties(xune_sync PROPERTIES
        MACOSX_RPATH OFF
        INSTALL_NAME_DIR "@loader_path"
    )
elseif(UNIX)
    # RPATH $ORIGIN so bundled deps (TagLib) are found next to the .so
    set_target_properties(xune_sync PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

# Compiler flags
xune_target_warnings(xune_sync)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    target_compile_options(xune_sync PRIVATE -g)
endif()

# Automatically copy library to Xune.Infrastructure.Devices runtime directories after build
# This is the canonical location - .NET project system handles copying to output
set(XUNE_DEVICES_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/Xune.Infrastructure.Devices/runtimes")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/Xune.Infrastructure.Devices")
    # Determine the runtime identifier based on platform and architecture
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
            set(RUNTIME_ID "win-x64")
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(RUNTIME_ID "win-arm64")
        else()
            set(RUNTIME_ID "win-x86")
        endif()
    elseif(APPLE)
        # CMAKE_OSX_ARCHITECTURES is the target arch (cross-compilation);
        # CMAKE_SYSTEM_PROCESSOR is the host arch (native build).
        if(CMAKE_OSX_ARCHITECTURES)
            set(_target_arch "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(_target_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(_target_arch MATCHES "arm64|aarch64")
            set(RUNTIME_ID "osx-arm64")
        else()
            set(RUNTIME_ID "osx-x64")
        endif()
    elseif(UNIX)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(RUNTIME_ID "linux-arm64")
        else()
            set(RUNTIME_ID "linux-x64")
        endif()
    endif()

    set(ZW_RUNTIME_NATIVE "${XUNE_DEVICES_RUNTIME_DIR}/${RUNTIME_ID}/native")
    add_custom_command(TARGET xune_sync POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZW_RUNTIME_NATIVE}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:xune_sync>" "${ZW_RUNTIME_NATIVE}/$<TARGET_FILE_NAME:xune_sync>"
        COMMENT "Copying $<TARGET_FILE_NAME:xune_sync> to Xune.Infrastructure.Devices/runtimes/${RUNTIME_ID}/native/"
        VERBATIM
    )

    # Bundle non-system dependencies into the runtime directory
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_library(TAGLIB_SO_PATH NAMES tag REQUIRED)
        get_filename_component(TAGLIB_SO_REALPATH "${TAGLIB_SO_PATH}" REALPATH)
        get_filename_component(TAGLIB_SO_FILENAME "${TAGLIB_SO_REALPATH}" NAME)
        # Derive SONAME (e.g. libtag.so.2 from libtag.so.2.0.2)
        string(REGEX MATCH "libtag\\.so\\.[0-9]+" TAGLIB_SONAME "${TAGLIB_SO_FILENAME}")
        if(NOT TAGLIB_SONAME)
            set(TAGLIB_SONAME "libtag.so.2")
        endif()
        message(STATUS "Bundling TagLib: ${TAGLIB_SO_REALPATH} (SONAME: ${TAGLIB_SONAME})")
        add_custom_command(TARGET xune_sync POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${TAGLIB_SO_REALPATH}" "${ZW_RUNTIME_NATIVE}/${TAGLIB_SO_FILENAME}"
            COMMAND ${CMAKE_COMMAND} -E create_symlink "${TAGLIB_SO_FILENAME}" "${ZW_RUNTIME_NATIVE}/${TAGLIB_SONAME}"
            COMMENT "Bundling TagLib (${TAGLIB_SONAME}) for Flatpak/AppImage compatibility"
        )
    elseif(APPLE)
        # Resolve actual dylib paths (pkg-config TAGLIB_LIBRARIES may be just "tag")
        # CMAKE_PREFIX_PATH takes priority (cross-compilation), then standard Homebrew locations
        if(CMAKE_PREFIX_PATH)
            set(_taglib_search_paths "${CMAKE_PREFIX_PATH}/lib")
        else()
            set(_taglib_search_paths /opt/homebrew/lib /usr/local/lib)
        endif()
        find_library(TAGLIB_DYLIB_PATH NAMES tag PATHS ${_taglib_search_paths} NO_DEFAULT_PATH)
        if(NOT TAGLIB_DYLIB_PATH)
            set(TAGLIB_DYLIB_PATH "${TAGLIB_LIBRARIES}")
        endif()
        # Get the original install name from the dylib for the -change command
        execute_process(
            COMMAND otool -D "${TAGLIB_DYLIB_PATH}"
            OUTPUT_VARIABLE TAGLIB_INSTALL_NAME_OUTPUT OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX REPLACE ".*\n" "" TAGLIB_ORIG_ID "${TAGLIB_INSTALL_NAME_OUTPUT}")
        execute_process(
            COMMAND otool -D "${OPENSSL_CRYPTO_LIBRARY}"
            OUTPUT_VARIABLE OPENSSL_INSTALL_NAME_OUTPUT OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        string(REGEX REPLACE ".*\n" "" OPENSSL_ORIG_ID "${OPENSSL_INSTALL_NAME_OUTPUT}")
        message(STATUS "Bundling TagLib: ${TAGLIB_DYLIB_PATH} (id: ${TAGLIB_ORIG_ID})")
        message(STATUS "Bundling OpenSSL: ${OPENSSL_CRYPTO_LIBRARY} (id: ${OPENSSL_ORIG_ID})")

        add_custom_command(TARGET xune_sync POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${TAGLIB_DYLIB_PATH}" "${ZW_RUNTIME_NATIVE}/libtag.2.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy "${OPENSSL_CRYPTO_LIBRARY}" "${ZW_RUNTIME_NATIVE}/libcrypto.3.dylib"
            COMMAND install_name_tool -id "@loader_path/libtag.2.dylib" "${ZW_RUNTIME_NATIVE}/libtag.2.dylib"
            COMMAND install_name_tool -id "@loader_path/libcrypto.3.dylib" "${ZW_RUNTIME_NATIVE}/libcrypto.3.dylib"
            COMMAND install_name_tool -change "${TAGLIB_ORIG_ID}" "@loader_path/libtag.2.dylib"
                "${ZW_RUNTIME_NATIVE}/$<TARGET_FILE_NAME:xune_sync>"
            COMMAND install_name_tool -change "${OPENSSL_ORIG_ID}" "@loader_path/libcrypto.3.dylib"
                "${ZW_RUNTIME_NATIVE}/$<TARGET_FILE_NAME:xune_sync>"
            # Fix xune_sync own install name
            COMMAND install_name_tool -id "@loader_path/libxune_sync.dylib"
                "${ZW_RUNTIME_NATIVE}/$<TARGET_FILE_NAME:xune_sync>"
            # Re-sign ad-hoc (install_name_tool invalidates signatures)
            COMMAND codesign --force --sign - "${ZW_RUNTIME_NATIVE}/libtag.2.dylib"
            COMMAND codesign --force --sign - "${ZW_RUNTIME_NATIVE}/libcrypto.3.dylib"
            COMMAND codesign --force --sign - "${ZW_RUNTIME_NATIVE}/$<TARGET_FILE_NAME:xune_sync>"
            COMMENT "Bundling taglib/openssl and fixing install names for hardened runtime"
        )
    elseif(WIN32)
        # Bundle vcpkg dynamic dependencies (OpenSSL, curl)
        set(_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(_crypto_dll "libcrypto-3-arm64.dll")
        else()
            set(_crypto_dll "libcrypto-3-x64.dll")
        endif()
        foreach(_dll "${_crypto_dll}" "libcurl.dll")
            if(EXISTS "${_vcpkg_bin}/${_dll}")
                add_custom_command(TARGET xune_sync POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy "${_vcpkg_bin}/${_dll}" "${ZW_RUNTIME_NATIVE}/"
                    COMMENT "Bundling ${_dll} to ${ZW_RUNTIME_NATIVE}/"
                )
            endif()
        endforeach()
    endif()
    message(STATUS "Xune parent project detected - will auto-copy library to Xune.Infrastructure.Devices/runtimes/${RUNTIME_ID}/native/")
else()
    message(STATUS "Standalone build - library will not be auto-copied to parent project")
endif()

# ── Shared source list for tests and tools ────────────────────────────────
# Tests and tools compile sources directly (not linking xune_sync shared lib)
set(XUNE_CORE_SOURCES
    lib/src/ZuneDevice.cpp
    lib/src/ZuneDeviceIdentification.cpp
    lib/src/NetworkManager.cpp
    lib/src/ZuneMtpReader.cpp
    lib/src/ZuneMtpWriter.cpp
    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/xune_sync_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/MetadataRequestHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)
if(WIN32)
    list(APPEND XUNE_CORE_SOURCES lib/src/driver_manager.cpp)
endif()

set(XUNE_CORE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

set(XUNE_CORE_LIBS
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARIES}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

# =============================================================================
# Test Executables (only built when ZUNE_BUILD_TESTS=ON)
# =============================================================================
if(ZUNE_BUILD_TESTS)

# Test executable for library JSON export
add_executable(test_library_json tests/test_library_json.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_library_json PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(test_library_json ${XUNE_CORE_LIBS})
xune_target_warnings(test_library_json)

# Test executable for zmdb fast library extraction
add_executable(test_zmdb_fast tests/test_zmdb_fast.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_zmdb_fast PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(test_zmdb_fast ${XUNE_CORE_LIBS})
xune_target_warnings(test_zmdb_fast)

# Test executable for unified zmdb extractor (file or device)
add_executable(test_zmdb_extractor tests/test_zmdb_extractor.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_zmdb_extractor PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(test_zmdb_extractor ${XUNE_CORE_LIBS})
xune_target_warnings(test_zmdb_extractor)

# Test executable for HTTP interceptor
add_executable(test_http_interceptor tests/test_http_interceptor.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_http_interceptor PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(test_http_interceptor ${XUNE_CORE_LIBS})
xune_target_warnings(test_http_interceptor)

# Test executable for TCPFlowController (Phase 5.3)
add_executable(test_tcp_flow_controller
    tests/test_tcp_flow_controller.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp  # Phase 2: RTO integration
    lib/src/protocols/ppp/PPPParser.cpp  # For ByteArray type
)

target_include_directories(test_tcp_flow_controller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
)

target_link_libraries(test_tcp_flow_controller
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_tcp_flow_controller)

# Test executable for TCPConnectionManager (Phase 5.2)
add_executable(test_tcp_connection_manager
    tests/test_tcp_connection_manager.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp  # Phase 2: RTO integration
    lib/src/protocols/ppp/PPPParser.cpp
)

target_include_directories(test_tcp_connection_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
)

target_link_libraries(test_tcp_connection_manager
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_tcp_connection_manager)

# Test executable for DNSHandler (Phase 5.2)
add_executable(test_dns_handler
    tests/test_dns_handler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/ppp/PPPParser.cpp
)

target_include_directories(test_dns_handler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
)

target_link_libraries(test_dns_handler
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_dns_handler)

# Test executable for CCPHandler (Phase 5.2)
add_executable(test_ccp_handler
    tests/test_ccp_handler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/ppp/PPPParser.cpp
)

target_include_directories(test_ccp_handler PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
)

target_link_libraries(test_ccp_handler
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_ccp_handler)

# Test executable for HTTP monitor (boot-time traffic capture)
add_executable(test_http_monitor_boot tests/test_http_monitor_boot.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_http_monitor_boot PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(test_http_monitor_boot ${XUNE_CORE_LIBS})
xune_target_warnings(test_http_monitor_boot)

# Test executable for rating iteration
add_executable(test_rating tests/test_rating.cpp ${XUNE_CORE_SOURCES})
target_include_directories(test_rating PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(test_rating ${XUNE_CORE_LIBS})
xune_target_warnings(test_rating)

# Test executable for network stack
add_executable(test_network_stack
    tests/test_network_stack.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/MetadataRequestHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_network_stack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(test_network_stack
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_network_stack)

# Test executable for HTTP interceptor integration (Phase 5.4)
add_executable(test_http_interceptor_integration
    tests/test_http_interceptor_integration.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/MetadataRequestHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
)

target_include_directories(test_http_interceptor_integration PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(test_http_interceptor_integration
    aft-cli-static
    ${AFTL_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(test_http_interceptor_integration)

endif() # ZUNE_BUILD_TESTS

# =============================================================================
# Tool Executables (only built when ZUNE_BUILD_TOOLS=ON)
# =============================================================================
if(ZUNE_BUILD_TOOLS)

# Device Info Dump CLI (reads raw MTP device properties)
add_executable(device_info_dump_cli tools/device_info_dump_cli.cpp)
target_include_directories(device_info_dump_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux/mtp/backend/posix
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux/mtp/backend/darwin
)
target_link_libraries(device_info_dump_cli ${AFTL_LIBRARY} ${CMAKE_THREAD_LIBS_INIT} ${PLATFORM_LIBRARIES})

# Artist Images Tool CLI
add_executable(artist_images_tool_cli tools/artist_images_tool_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(artist_images_tool_cli PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(artist_images_tool_cli ${XUNE_CORE_LIBS})
xune_target_warnings(artist_images_tool_cli)

# Playlist Test CLI
add_executable(playlist_test_cli tools/playlist_test_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(playlist_test_cli PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(playlist_test_cli ${XUNE_CORE_LIBS})
xune_target_warnings(playlist_test_cli)

# Audiobook Test CLI
add_executable(audiobook_test_cli tools/audiobook_test_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(audiobook_test_cli PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(audiobook_test_cli ${XUNE_CORE_LIBS})
xune_target_warnings(audiobook_test_cli)

# Sync Partnership Test CLI
add_executable(sync_partnership_test_cli tools/sync_partnership_test_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(sync_partnership_test_cli PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(sync_partnership_test_cli ${XUNE_CORE_LIBS})
xune_target_warnings(sync_partnership_test_cli)

# Erase Content Test CLI
add_executable(erase_content_test_cli tools/erase_content_test_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(erase_content_test_cli PRIVATE ${XUNE_CORE_INCLUDES})
target_link_libraries(erase_content_test_cli ${XUNE_CORE_LIBS})
xune_target_warnings(erase_content_test_cli)

# Upload Test CLI
add_executable(upload_test_cli tools/upload_test_cli.cpp ${XUNE_CORE_SOURCES})
target_include_directories(upload_test_cli PRIVATE ${XUNE_CORE_INCLUDES} ${TAGLIB_INCLUDE_DIRS})
target_link_libraries(upload_test_cli ${XUNE_CORE_LIBS})
xune_target_warnings(upload_test_cli)

# XNA Deploy Test CLI
add_executable(xna_deploy_test_cli tools/xna_deploy_test_cli.cpp)

target_include_directories(xna_deploy_test_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux/mtp/backend/posix
    ${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux/mtp/backend/darwin
    ${OPENSSL_INCLUDE_DIR}
)

target_link_libraries(xna_deploy_test_cli
    ${AFTL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

xune_target_warnings(xna_deploy_test_cli)

endif() # ZUNE_BUILD_TOOLS

# =============================================================================
# Windows: Elevated driver setup helper (requires libwdi)
# =============================================================================
if(WIN32)
    set(LIBWDI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libwdi")

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
        set(LIBWDI_ARCH "x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(LIBWDI_ARCH "arm64")
    else()
        set(LIBWDI_ARCH "x86")
    endif()

    set(LIBWDI_LIB "${LIBWDI_DIR}/lib/${LIBWDI_ARCH}/libwdi.lib")
    if(EXISTS "${LIBWDI_LIB}")
        add_executable(xune-driver-setup
            tools/xune-driver-setup/main.cpp
        )
        target_include_directories(xune-driver-setup PRIVATE
            "${LIBWDI_DIR}/include"
        )
        target_link_libraries(xune-driver-setup
            "${LIBWDI_LIB}"
            setupapi cfgmgr32 newdev ole32 advapi32 shlwapi shell32
        )
        target_compile_definitions(xune-driver-setup PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
        )
        # UAC requireAdministrator manifest
        if(MSVC)
            set_target_properties(xune-driver-setup PROPERTIES
                LINK_FLAGS "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
            )
        endif()
        # Copy to .NET runtime directory alongside xune_sync.dll
        if(EXISTS "${XUNE_DEVICES_RUNTIME_DIR}")
            add_custom_command(TARGET xune-driver-setup POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:xune-driver-setup>
                    "${XUNE_DEVICES_RUNTIME_DIR}/${RUNTIME_ID}/native/"
                COMMENT "Copying xune-driver-setup.exe to .NET runtime directory"
            )
        endif()
        message(STATUS "  Driver setup helper: enabled (libwdi ${LIBWDI_ARCH})")
    else()
        message(STATUS "  Driver setup helper: disabled (libwdi not found at ${LIBWDI_LIB})")
    endif()
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS xune_sync
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Print configuration summary
message(STATUS "")
message(STATUS "Configuration Summary:")
message(STATUS "  Project: ${PROJECT_NAME}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  AFTL library: ${AFTL_LIBRARY}")
message(STATUS "  OpenSSL: ${OPENSSL_CRYPTO_LIBRARY}")
message(STATUS "  Platform libraries: ${PLATFORM_LIBRARIES}")
message(STATUS "  Build tests: ${ZUNE_BUILD_TESTS}")
message(STATUS "  Build tools: ${ZUNE_BUILD_TOOLS}")
message(STATUS "")
