include_directories("${ROOT_DIR}/3rdParty/gc/include/")

string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LOWERCASE)
if (MSVC)
    message(STATUS "Using GC Lib: ${ROOT_DIR}/3rdParty/gc/x64/${CMAKE_BUILD_TYPE_LOWERCASE}/")

    #  link_directories("${ROOT_DIR}/3rdParty/gc/x64/${CMAKE_BUILD_TYPE_LOWERCASE}/")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GR")
else ()
    message(STATUS "Using GC Lib: ${ROOT_DIR}/3rdParty/gc/${CMAKE_BUILD_TYPE_LOWERCASE}/")

    #  link_directories("${ROOT_DIR}/3rdParty/gc/${CMAKE_BUILD_TYPE_LOWERCASE}/")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti")
endif ()

add_mlir_library(TypeScriptRuntime
        STATIC
        TypeScriptGC.cpp
        gc.cpp
        MemRuntime.cpp
        AsyncRuntime.cpp
        DynamicRuntime.cpp
        mlir_init.cpp

        EXCLUDE_FROM_LIBMLIR

        LINK_LIBS PRIVATE
        gcmt-lib
)
