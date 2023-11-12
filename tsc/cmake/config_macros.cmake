macro(set_Options)

    if (MSVC)
        # enable RTTI
        SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /GR")
    else ()
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-switch -Wno-unused-function -Wno-unused-result -Wno-unused-variable -Wno-sign-compare -Wno-implicit-fallthrough -Wno-parentheses -Wno-type-limits")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-const-variable -Wno-unused-private-field -Wmacro-redefined")
        else ()
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-subobject-linkage -Wno-unused-but-set-variable")
        endif ()
        # enable RTTI and exceptions
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -frtti -fexceptions")
    endif ()
endmacro()

macro(set_Options_With_FS)

    set_Options()

    if (NOT MSVC AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"))
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -lstdc++fs")
        link_libraries(stdc++fs)
    endif ()

endmacro()