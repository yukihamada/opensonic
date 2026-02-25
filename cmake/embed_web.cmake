# embed_web.cmake — Generate C header with embedded web files
#
# Usage: cmake -P embed_web.cmake <web_dir> <output_header>
#
# Generates a header with:
#   static const uint8_t web_<name>_data[] = { ... };
#   static const WebFile embedded_web_files[] = { ... };

set(WEB_DIR "${CMAKE_ARGV3}")
set(OUTPUT "${CMAKE_ARGV4}")

if(NOT WEB_DIR OR NOT OUTPUT)
    message(FATAL_ERROR "Usage: cmake -P embed_web.cmake <web_dir> <output_header>")
endif()

# Collect web files
file(GLOB WEB_FILES "${WEB_DIR}/*")

set(HEADER "// Auto-generated — do not edit\n")
string(APPEND HEADER "#pragma once\n")
string(APPEND HEADER "#include <cstdint>\n")
string(APPEND HEADER "#include <cstddef>\n\n")

set(FILE_ENTRIES "")
set(FILE_COUNT 0)

foreach(FILEPATH ${WEB_FILES})
    get_filename_component(FILENAME ${FILEPATH} NAME)

    # Determine MIME type
    if(FILENAME MATCHES "\\.html$")
        set(MIME "text/html")
    elseif(FILENAME MATCHES "\\.js$")
        set(MIME "application/javascript")
    elseif(FILENAME MATCHES "\\.css$")
        set(MIME "text/css")
    elseif(FILENAME MATCHES "\\.json$")
        set(MIME "application/json")
    elseif(FILENAME MATCHES "\\.png$")
        set(MIME "image/png")
    elseif(FILENAME MATCHES "\\.svg$")
        set(MIME "image/svg+xml")
    else()
        set(MIME "application/octet-stream")
    endif()

    # Sanitize variable name
    string(REGEX REPLACE "[^a-zA-Z0-9]" "_" VARNAME "${FILENAME}")

    # Read file as hex
    file(READ ${FILEPATH} FILE_CONTENT HEX)
    string(LENGTH "${FILE_CONTENT}" HEX_LEN)
    math(EXPR BYTE_COUNT "${HEX_LEN} / 2")

    # Convert hex pairs to C array
    set(C_BYTES "")
    set(LINE "")
    set(LINE_COUNT 0)
    math(EXPR LAST_IDX "${HEX_LEN} - 1")
    set(IDX 0)
    while(IDX LESS HEX_LEN)
        string(SUBSTRING "${FILE_CONTENT}" ${IDX} 2 BYTE)
        string(APPEND LINE "0x${BYTE},")
        math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
        if(LINE_COUNT GREATER_EQUAL 16)
            string(APPEND C_BYTES "\n    ${LINE}")
            set(LINE "")
            set(LINE_COUNT 0)
        endif()
        math(EXPR IDX "${IDX} + 2")
    endwhile()
    if(LINE_COUNT GREATER 0)
        string(APPEND C_BYTES "\n    ${LINE}")
    endif()

    string(APPEND HEADER "static const uint8_t web_${VARNAME}_data[] = {${C_BYTES}\n}")
    string(APPEND HEADER ";\n\n")

    string(APPEND FILE_ENTRIES "    {\"/${FILENAME}\", \"${MIME}\", web_${VARNAME}_data, ${BYTE_COUNT}},\n")
    math(EXPR FILE_COUNT "${FILE_COUNT} + 1")
endforeach()

string(APPEND HEADER "struct EmbeddedWebFile {\n")
string(APPEND HEADER "    const char* path;\n")
string(APPEND HEADER "    const char* mime_type;\n")
string(APPEND HEADER "    const uint8_t* data;\n")
string(APPEND HEADER "    size_t size;\n")
string(APPEND HEADER "};\n\n")

string(APPEND HEADER "static const EmbeddedWebFile embedded_web_files[] = {\n")
string(APPEND HEADER "${FILE_ENTRIES}")
string(APPEND HEADER "};\n\n")
string(APPEND HEADER "static const size_t embedded_web_file_count = ${FILE_COUNT};\n")

file(WRITE "${OUTPUT}" "${HEADER}")
message(STATUS "Embedded ${FILE_COUNT} web files into ${OUTPUT}")
