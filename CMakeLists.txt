cmake_minimum_required(VERSION 3.21)

project(CodeforcesQuery C)

set(CMAKE_C_STANDARD 11)

set(CURL_INCLUDE_DIR "C:/curl/include")
set(CURL_LIBRARY "C:/curl/lib/libcurl.dll.a")

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CURL_INCLUDE_DIR})

add_executable(cf_tool
        codeforces_clawer.c
        cJSON.c
)

set_target_properties(cf_tool PROPERTIES
        OUTPUT_NAME "Codeforces用户查询"
)

target_compile_definitions(cf_tool PRIVATE CJSON_HIDE_SYMBOLS)

target_link_libraries(cf_tool
        ${CURL_LIBRARY}
        ws2_32
)

set(CURL_BIN "C:/curl/bin")


foreach(dll
        libcurl-x64.dll
)

    add_custom_command(TARGET cf_tool POST_BUILD

            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CURL_BIN}/${dll}"
            "$<TARGET_FILE_DIR:cf_tool>"

            COMMENT "Copying ${dll}"

    )

endforeach()