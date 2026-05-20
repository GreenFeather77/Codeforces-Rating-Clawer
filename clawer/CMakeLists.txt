cmake_minimum_required(VERSION 3.21)
project(clawer C)

set(CMAKE_C_STANDARD 11)

set(CURL_INCLUDE_DIR "C:/msys64/mingw64/include")
set(CURL_LIBRARY "C:/msys64/mingw64/lib/libcurl.dll.a")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CURL_INCLUDE_DIR})

add_executable(cf_tool cf_tool.c cJSON.c)
target_compile_definitions(cf_tool PRIVATE CJSON_HIDE_SYMBOLS)
target_link_libraries(cf_tool
    ${CURL_LIBRARY}
    ws2_32
    winhttp
    bcrypt
    wldap32
    winmm
    advapi32
    crypt32
    normaliz)

# Copy DLLs next to the executable so they can be found at runtime
set(MSYS_BIN "C:/msys64/mingw64/bin")
foreach(dll libcurl-4.dll libbrotlidec.dll libbrotlicommon.dll libidn2-0.dll libnghttp2-14.dll libnghttp3-9.dll libngtcp2-16.dll libngtcp2_crypto_ossl-0.dll libpsl-5.dll libssh2-1.dll libzstd.dll libcrypto-3-x64.dll libssl-3-x64.dll zlib1.dll libiconv-2.dll libintl-8.dll libunistring-5.dll libgcc_s_seh-1.dll libwinpthread-1.dll libstdc++-6.dll)
    add_custom_command(TARGET cf_tool POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MSYS_BIN}/${dll}"
            "$<TARGET_FILE_DIR:cf_tool>"
        COMMENT "Copying ${dll} to build output")
endforeach()
