cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBJBIG2_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_jbig2.c
)

SET(LIBJBIG2_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libjbig2dec.a
)

SET(LIBJBIG2_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libjbig2
        "${LIBJBIG2_SRC}"
        "${LIBJBIG2_LIBS}"
        []
        ""
        "${LIBJBIG2_INC}"
        ""
        "1")
