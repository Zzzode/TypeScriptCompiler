cmake_minimum_required(VERSION 3.17.3)

project(TypeScriptCompiler LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

set(ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(BUILD_DIR ${ROOT_DIR}/__build)

string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWERCASE)

# Function to download and extract a tar.gz file
function(download_and_extract url path)
    file(DOWNLOAD ${url} ${path}.tar.gz SHOW_PROGRESS)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf ${path}.tar.gz WORKING_DIRECTORY ${ROOT_DIR}/3rdParty/)
endfunction()

# Download and extract libatomic_ops-7.8.0 if it does not exist
if (NOT EXISTS "${ROOT_DIR}/3rdParty/gc/libatomic_ops")
    message("Downloading and extracting libatomic_ops...")
    download_and_extract("https://www.hboehm.info/gc/gc_source/libatomic_ops-7.8.0.tar.gz" "${ROOT_DIR}/3rdParty/libatomic_ops-7.8.0")
    file(COPY "${ROOT_DIR}/3rdParty/libatomic_ops-7.8.0/" DESTINATION "${ROOT_DIR}/3rdParty/gc/libatomic_ops/")
endif ()

# Copy files
file(COPY "${ROOT_DIR}/docs/fix/gc/" DESTINATION "${ROOT_DIR}/3rdParty/gc/")
# Make __build folders
file(MAKE_DIRECTORY "${BUILD_DIR}/${CMAKE_BUILD_TYPE_LOWERCASE}")

add_subdirectory(3rdParty/gc)
add_subdirectory(tsc)