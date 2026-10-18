# 第三方依赖统一在这里声明 imported target。
# 业务 target 只链接 cm::mysql（后续阶段再加 cm::hiredis / cm::ffmpeg / cm::sdl2 / cm::gtest），
# 不直接写 -l 或绝对路径。

function(cm_find_mysql)
    if(TARGET cm::mysql)
        return()
    endif()

    set(_mysql_include_hint "")
    set(_mysql_library_hint "")

    find_program(MYSQL_CONFIG_EXECUTABLE NAMES mysql_config mariadb_config)
    if(MYSQL_CONFIG_EXECUTABLE)
        execute_process(
            COMMAND ${MYSQL_CONFIG_EXECUTABLE} --include
            OUTPUT_VARIABLE _mysql_include_flags
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        string(REPLACE "-I" "" _mysql_include_flags "${_mysql_include_flags}")
        separate_arguments(_mysql_include_hint UNIX_COMMAND "${_mysql_include_flags}")

        execute_process(
            COMMAND ${MYSQL_CONFIG_EXECUTABLE} --libs
            OUTPUT_VARIABLE _mysql_libs
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        string(REGEX MATCHALL "-L[^ ]+" _mysql_lib_dirs "${_mysql_libs}")
        string(REPLACE "-L" "" _mysql_library_hint "${_mysql_lib_dirs}")
    endif()

    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql.h
        HINTS ${_mysql_include_hint}
        PATH_SUFFIXES mysql mariadb)
    find_library(MYSQL_LIBRARY
        NAMES mysqlclient mariadb
        HINTS ${_mysql_library_hint})

    if(NOT MYSQL_INCLUDE_DIR OR NOT MYSQL_LIBRARY)
        message(FATAL_ERROR
            "libmysqlclient not found. Install it (Ubuntu: apt-get install libmysqlclient-dev) "
            "or set MYSQL_INCLUDE_DIR / MYSQL_LIBRARY explicitly.")
    endif()

    add_library(cm::mysql UNKNOWN IMPORTED)
    set_target_properties(cm::mysql PROPERTIES
        IMPORTED_LOCATION "${MYSQL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MYSQL_INCLUDE_DIR}")
endfunction()

function(cm_find_hiredis)
    if(TARGET cm::hiredis)
        return()
    endif()

    find_library(HIREDIS_LIB hiredis)
    find_path(HIREDIS_INC hiredis/hiredis.h)

    if(NOT HIREDIS_LIB OR NOT HIREDIS_INC)
        message(FATAL_ERROR
            "hiredis not found. Install it (Ubuntu: apt-get install libhiredis-dev) "
            "or set HIREDIS_LIB / HIREDIS_INC explicitly.")
    endif()

    add_library(cm::hiredis UNKNOWN IMPORTED)
    set_target_properties(cm::hiredis PROPERTIES
        IMPORTED_LOCATION "${HIREDIS_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${HIREDIS_INC}")
endfunction()
