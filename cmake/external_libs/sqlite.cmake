
set(sqlite_USE_STATIC_LIBS ON)
set(sqlite_CXXFLAGS)
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(sqlite_CFLAGS "-fstack-protector-all -Wno-uninitialized -Wno-unused-parameter -fPIC -D_FORTIFY_SOURCE=2 -O2")
else()
    set(sqlite_CFLAGS "-fstack-protector-all -Wno-maybe-uninitialized -Wno-unused-parameter -fPIC -D_FORTIFY_SOURCE=2 -O2")
endif()
set(sqlite_LDFLAGS "-Wl,-z,relro,-z,now,-z,noexecstack")

mindspore_add_pkg(sqlite
        VER 3.31.1
        LIBS sqlite3
        URL https://github.com/sqlite/sqlite/archive/version-3.31.1.tar.gz
        MD5 5f4e7b4016c15f4fb5855615279819da
        PATCHES ${CMAKE_SOURCE_DIR}/third_party/patch/sqlite/sqlite.patch001
        CONFIGURE_COMMAND ./configure --enable-shared=no --disable-tcl --disable-editline --enable-json1)
include_directories(${sqlite_INC})
add_library(mindspore::sqlite ALIAS sqlite::sqlite3)