# Defines a static `lua_static` target by fetching the official Lua 5.5
# source release. Idempotent.
if(TARGET lua_static)
    return()
endif()

include(FetchCache)
include(FetchContent)

# 官方站偶尔会返回 5xx，CI 是全新环境每次都现下，撞上就直接构建失败。
# 备用地址取官方镜像，包结构和主站一致（源码在同名的 src/ 子目录下）。
# GitHub 上的 tag 包把源码放在仓库根目录，换过去会找不到文件，不能用。
FetchContent_Declare(
    lua
    URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
        https://www.tecgraf.puc-rio.br/lua/ftp/lua-5.5.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    TLS_VERIFY OFF
)
FetchContent_MakeAvailable(lua)

# The Lua tarball ships only Makefiles, so we build a static library here.
file(GLOB LUA_CORE_SOURCES "${lua_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM LUA_CORE_SOURCES
    "${lua_SOURCE_DIR}/src/lua.c"
    "${lua_SOURCE_DIR}/src/luac.c")

add_library(lua_static STATIC ${LUA_CORE_SOURCES})
target_include_directories(lua_static PUBLIC "${lua_SOURCE_DIR}/src")
set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)
