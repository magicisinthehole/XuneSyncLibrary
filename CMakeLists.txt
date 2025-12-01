cmake_minimum_required(VERSION 3.10)
project(zune_wireless)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# AFTL library configuration
set(AFTL_ROOT "${CMAKE_SOURCE_DIR}/vendor/android-file-transfer-linux")
set(AFTL_BUILD_DIR "${CMAKE_BINARY_DIR}/vendor/android-file-transfer-linux-build")
set(AFTL_LIBRARY "${AFTL_BUILD_DIR}/libmtp-ng-static.a")

# Check if AFTL is already built
if(NOT EXISTS "${AFTL_LIBRARY}")
    message(STATUS "AFTL library not found, building it first...")

    # Create build directory
    file(MAKE_DIRECTORY ${AFTL_BUILD_DIR})

    # Configure AFTL
    execute_process(
        COMMAND ${CMAKE_COMMAND}
            -S ${AFTL_ROOT}
            -B ${AFTL_BUILD_DIR}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DBUILD_SHARED_LIB=OFF
            -DBUILD_QT_UI=OFF
            -DBUILD_PYTHON=OFF
            -DBUILD_FUSE=OFF
            -DBUILD_MTPZ=ON
        RESULT_VARIABLE AFTL_CMAKE_RESULT
    )

    if(NOT AFTL_CMAKE_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure AFTL library")
    endif()

    # Build AFTL
    execute_process(
        COMMAND ${CMAKE_COMMAND} --build ${AFTL_BUILD_DIR} --target mtp-ng-static
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

# Find TagLib (required by AFTL Metadata)
find_library(TAGLIB_LIBRARY NAMES tag PATHS /opt/homebrew/lib /usr/local/lib)
find_path(TAGLIB_INCLUDE_DIR NAMES taglib/tag.h PATHS /opt/homebrew/include /usr/local/include)

if(NOT TAGLIB_LIBRARY OR NOT TAGLIB_INCLUDE_DIR)
    message(FATAL_ERROR "TagLib not found. Install with: brew install taglib")
endif()

message(STATUS "TagLib library: ${TAGLIB_LIBRARY}")
message(STATUS "TagLib include: ${TAGLIB_INCLUDE_DIR}")

# macOS specific settings
if(APPLE)
    find_library(CORE_LIBRARY CoreFoundation)
    find_library(IOKIT_LIBRARY IOKit)
    set(PLATFORM_LIBRARIES ${IOKIT_LIBRARY} ${CORE_LIBRARY})
else()
    set(PLATFORM_LIBRARIES)
endif()

# AFTL include directories
set(AFTL_INCLUDE_DIRS
    ${AFTL_ROOT}
    ${AFTL_ROOT}/mtp
    ${AFTL_ROOT}/mtp/backend/posix
    ${AFTL_ROOT}/cli
)

if(APPLE)
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/darwin)
else()
    list(APPEND AFTL_INCLUDE_DIRS ${AFTL_ROOT}/mtp/backend/linux)
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

target_compile_options(aft-cli-static PRIVATE
    -Wall
    -Wno-unused-parameter
)

# Main library
add_library(zune_wireless SHARED
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(zune_wireless PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${TAGLIB_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(zune_wireless
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

# Compiler flags
target_compile_options(zune_wireless PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    target_compile_options(zune_wireless PRIVATE -g)
endif()

# Automatically copy library to Xune app runtime directories after build (if parent project exists)
set(XUNE_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/Xune.App/bin/${CMAKE_BUILD_TYPE}/net8.0/runtimes")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../src/Xune.App")
    add_custom_command(TARGET zune_wireless POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XUNE_RUNTIME_DIR}/osx/native"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${XUNE_RUNTIME_DIR}/osx-arm64/native"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:zune_wireless>" "${XUNE_RUNTIME_DIR}/osx/native/libzune_wireless.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:zune_wireless>" "${XUNE_RUNTIME_DIR}/osx-arm64/native/libzune_wireless.dylib"
        COMMENT "Copying libzune_wireless.dylib to Xune app runtime directories"
        VERBATIM
    )
    message(STATUS "Xune parent project detected - will auto-copy library after build")
else()
    message(STATUS "Standalone build - library will not be auto-copied to parent project")
endif()

# Test executable for library JSON export
add_executable(test_library_json
    tests/test_library_json.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_library_json PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(test_library_json
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_library_json PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable for zmdb fast library extraction
add_executable(test_zmdb_fast
    tests/test_zmdb_fast.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_zmdb_fast PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(test_zmdb_fast
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_zmdb_fast PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable for unified zmdb extractor (file or device)
add_executable(test_zmdb_extractor
    tests/test_zmdb_extractor.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_zmdb_extractor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
)

target_link_libraries(test_zmdb_extractor
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_zmdb_extractor PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable for HTTP interceptor
add_executable(test_http_interceptor
    tests/test_http_interceptor.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_http_interceptor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(test_http_interceptor
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_http_interceptor PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable for rating iteration
add_executable(test_rating
    tests/test_rating.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp
    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_rating PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(test_rating
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_rating PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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

target_compile_options(test_tcp_flow_controller PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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

target_compile_options(test_tcp_connection_manager PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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

target_compile_options(test_dns_handler PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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

target_compile_options(test_ccp_handler PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Test executable for HTTP monitor (boot-time traffic capture)
add_executable(test_http_monitor_boot
    tests/test_http_monitor_boot.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(test_http_monitor_boot PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(test_http_monitor_boot
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(test_http_monitor_boot PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Artist Images Tool CLI
add_executable(artist_images_tool_cli
    tools/artist_images_tool_cli.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(artist_images_tool_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(artist_images_tool_cli
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(artist_images_tool_cli PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Audiobook Test CLI
add_executable(audiobook_test_cli
    tools/audiobook_test_cli.cpp
    lib/src/ZuneDevice.cpp
    lib/src/NetworkManager.cpp
    lib/src/LibraryManager.cpp

    lib/src/ZMDBLibraryExtractor.cpp
    lib/src/zmdb/ZMDBParserBase.cpp
    lib/src/zmdb/ZMDBUtils.cpp
    lib/src/zmdb/ZuneHDParser.cpp
    lib/src/zmdb/ZuneClassicParser.cpp
    lib/src/zmdb/ZMDBParserFactory.cpp
    lib/src/ptpip_client.cpp
    lib/src/ssdp_discovery.cpp
    lib/src/zune_wireless_api.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
    lib/src/protocols/handlers/CCPHandler.cpp
    lib/src/protocols/handlers/DNSHandler.cpp
    lib/src/protocols/tcp/TCPConnectionManager.cpp
    lib/src/protocols/tcp/TCPFlowController.cpp
    lib/src/protocols/tcp/TCPStreamReassembler.cpp
    lib/src/protocols/tcp/RTOManager.cpp
)

target_include_directories(audiobook_test_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AFTL_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${TAGLIB_INCLUDE_DIR}
)

target_link_libraries(audiobook_test_cli
    aft-cli-static
    ${AFTL_LIBRARY}
    ${TAGLIB_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${CURL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBRARIES}
)

target_compile_options(audiobook_test_cli PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

# Install
install(TARGETS zune_wireless
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
message(STATUS "")

# Test executable for network stack
add_executable(test_network_stack
    tests/test_network_stack.cpp
    lib/src/protocols/http/ZuneHTTPInterceptor.cpp
    lib/src/protocols/ppp/PPPParser.cpp
    lib/src/protocols/ppp/PPPFrameBuilder.cpp
    lib/src/protocols/http/HTTPParser.cpp
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
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

target_compile_options(test_network_stack PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)

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
    lib/src/protocols/http/StaticModeHandler.cpp
    lib/src/protocols/http/ProxyModeHandler.cpp
    lib/src/protocols/http/HttpClient.cpp
    lib/src/protocols/http/HybridModeHandler.cpp
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

target_compile_options(test_http_interceptor_integration PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter
)
