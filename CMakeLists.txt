cmake_minimum_required (VERSION 3.5)
project (x11pulsemux)

set (CMAKE_CXX_FLAGS "--std=gnu++11 ${CMAKE_CXX_FLAGS}")
set (CMAKE_C_FLAGS "--std=gnu99 ${CMAKE_C_FLAGS}")

file (GLOB SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc")

find_package (PkgConfig)
pkg_check_modules (LIBAVCODEC REQUIRED libavcodec)
pkg_check_modules (LIBAVUTIL REQUIRED libavutil)
pkg_check_modules (LIBAVFORMAT REQUIRED libavformat)
pkg_check_modules (LIBAVDEVICE REQUIRED libavdevice)
pkg_check_modules (LIBAVFILTER REQUIRED libavfilter)
pkg_check_modules (LIBSWSCALE REQUIRED libswscale)
pkg_check_modules (LIBSWRESAMPLE REQUIRED libswresample)
pkg_check_modules (LIBUV REQUIRED libuv)

# Repair broken -framework flags from pkg_check_modules
if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  function(repair_framework_flags BAD_FLAG)
    string(REPLACE "-framework;" "-framework " GOOD_FLAG "${BAD_FLAG}")
    set(REPAIRED_FLAG "${GOOD_FLAG}" PARENT_SCOPE)
  endfunction()
  # "-framework;Security;-framework;CoreFoundation" to
  # "-framework Security;-framework CoreFoundation"
  repair_framework_flags("${LIBAVCODEC_LDFLAGS}")
  set(LIBAVCODEC_LDFLAGS "${REPAIRED_FLAG}")
  repair_framework_flags("${LIBAVFORMAT_LDFLAGS}")
  set(LIBAVFORMAT_LDFLAGS "${REPAIRED_FLAG}")
  repair_framework_flags("${LIBAVFILTER_LDFLAGS}")
  set(LIBAVFILTER_LDFLAGS "${REPAIRED_FLAG}")
endif()

link_libraries (${LIBAVCODEC_LDFLAGS})
link_libraries (${LIBAVUTIL_LDFLAGS})
link_libraries (${LIBAVFORMAT_LDFLAGS})
link_libraries (${LIBAVDEVICE_LDFLAGS})
link_libraries (${LIBAVFILTER_LDFLAGS})
link_libraries (${LIBSWSCALE_LDFLAGS})
link_libraries (${LIBSWRESAMPLE_LDFLAGS})
link_libraries (${LIBUV_LDFLAGS})

include_directories (
  ${LIBAVCODEC_INCLUDE_DIRS}
  ${LIBAVUTIL_INCLUDE_DIRS}
  ${LIBAVFORMAT_INCLUDE_DIRS}
  ${LIBAVDEVICE_INCLUDE_DIRS}
  ${LIBAVFILTER_INCLUDE_DIRS}
  ${LIBSWSCALE_INCLUDE_DIRS}
  ${LIBSWRESAMPLE_INCLUDE_DIRS}
  ${LIBUV_INCLUDE_DIRS}
)

add_executable (x11pulsemux ${SOURCES})
