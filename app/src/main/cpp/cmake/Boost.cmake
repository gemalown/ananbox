set(BOOST_VER 1.83.0)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/boost")
    # Use the boost archive from app/src/main/cpp
    set(BOOST_ARCHIVE "${CMAKE_SOURCE_DIR}/boost-minimal.tar.xz")
    
    if(EXISTS "${BOOST_ARCHIVE}")
        message(STATUS "Extracting Boost archive from boost-minimal.tar.xz ...")
        file(ARCHIVE_EXTRACT INPUT "${BOOST_ARCHIVE}"
                DESTINATION "${CMAKE_SOURCE_DIR}")
        # The archive contains boost/ directory directly, no rename needed
    else()
        # Fallback: download full boost from GitHub
        message(STATUS "boost-minimal.tar.xz not found, downloading Boost ${BOOST_VER} from GitHub...")
        file(
                DOWNLOAD "https://github.com/boostorg/boost/releases/download/boost-${BOOST_VER}/boost-${BOOST_VER}.tar.xz" boost-${BOOST_VER}.tar.xz
                EXPECTED_HASH SHA256=c5a0688e1f0c05f354bbd0b32244d36085d9ffc9f932e8a18983a9908096f614
                SHOW_PROGRESS
        )
        file(ARCHIVE_EXTRACT INPUT boost-${BOOST_VER}.tar.xz
                DESTINATION ${CMAKE_SOURCE_DIR}
                )
        file(RENAME "${CMAKE_SOURCE_DIR}/boost-${BOOST_VER}" "${CMAKE_SOURCE_DIR}/boost")
    endif()
endif()

set(BOOST_INCLUDE_LIBRARIES
        algorithm
        crc
        date_time
        dll
        interprocess
        range
        regex
        scope_exit
        signals2
        utility
        uuid
        #locale
        asio
        filesystem
        log
        #log_setup
        serialization
        system
        thread
        program_options
        )

add_subdirectory(boost EXCLUDE_FROM_ALL)