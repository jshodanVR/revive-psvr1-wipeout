if (NOT DEFINED FFMPEG_CORE_NAME)
	set(FFMPEG_CORE_NAME ffmpeg)
endif()

add_library(${FFMPEG_CORE_NAME} INTERFACE)

target_include_directories(${FFMPEG_CORE_NAME} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# Detect OS
if (WIN32 AND NOT MINGW)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-windows")
elseif (APPLE)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-macos")
elseif (UNIX)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-linux")
elseif (MINGW)
	set(FFMPEG_PREBUILTS_NAME "ffmpeg-mingw")
else ()
	message(FATAL_ERROR "Unsupported OS.")
endif ()

# Detect Architecture
if(NOT DEFINED ARCHITECTURE)
	message(FATAL_ERROR "ARCHITECTURE variable is not set up")
elseif (ARCHITECTURE STREQUAL "x86_64")
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-x64.zip")
elseif(ARCHITECTURE STREQUAL "arm64")
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-arm64.zip")
elseif(APPLE AND "x86_64" IN_LIST ARCHITECTURE AND "arm64" IN_LIST ARCHITECTURE) # macOS universal
	set(FFMPEG_PREBUILTS_NAME "${FFMPEG_PREBUILTS_NAME}-universal.zip")
else ()
	message(FATAL_ERROR "Unsupported architecture.")
endif()

# Vendored dependency revision: never infer it from the parent repository.
set(FFMPEG_GIT_SHA 94dde08)
if(NOT WIN32 OR MINGW OR NOT ARCHITECTURE STREQUAL "x86_64")
    message(FATAL_ERROR "This standalone Statik distribution supports Windows x64 only.")
endif()

# Download prebuilt ffmpeg
set(FFMPEG_ZIP_PATH "${CMAKE_BINARY_DIR}/externals/ffmpeg-${FFMPEG_GIT_SHA}.zip")
if(NOT EXISTS "${FFMPEG_ZIP_PATH}")
	message(STATUS "Downloading FFMPEG prebuilts...")
	file(DOWNLOAD https://github.com/shadps4-emu/ext-ffmpeg-core/releases/download/${FFMPEG_GIT_SHA}/${FFMPEG_PREBUILTS_NAME}
			"${FFMPEG_ZIP_PATH}" SHOW_PROGRESS EXPECTED_HASH SHA256=5038087e7798a0b129dbb4ff4a0caf4b6995aff91f5bf424096f65840385661d
		STATUS FILE_STATUS)
	list(GET FILE_STATUS 0 STATUS_CODE)
	if (NOT STATUS_CODE EQUAL 0)
		file(REMOVE "${FFMPEG_ZIP_PATH}") # CMake create 0 byte file even if URL is invalid. So need to delete it.
		message(FATAL_ERROR "No FFMPEG prebuilt found with corresponding commit SHA (${FFMPEG_GIT_SHA})")
	endif()
endif()

file(SHA256 "${FFMPEG_ZIP_PATH}" FFMPEG_ARCHIVE_HASH)
if(NOT FFMPEG_ARCHIVE_HASH STREQUAL "5038087e7798a0b129dbb4ff4a0caf4b6995aff91f5bf424096f65840385661d")
    message(FATAL_ERROR "FFmpeg archive checksum mismatch.")
endif()

set(FFMPEG_LIB_PATH "${CMAKE_BINARY_DIR}/externals/ffmpeg-${FFMPEG_GIT_SHA}/lib")
if(NOT EXISTS "${FFMPEG_LIB_PATH}")
	file(MAKE_DIRECTORY "${FFMPEG_LIB_PATH}")
	execute_process(COMMAND ${CMAKE_COMMAND} -E tar xzf "${FFMPEG_ZIP_PATH}"
		WORKING_DIRECTORY "${FFMPEG_LIB_PATH}")
endif()

set(LIB_PREFIX "lib")
set(LIB_EXT "a")
if (WIN32 AND NOT MINGW)
	set(LIB_PREFIX "")
	set(LIB_EXT "lib")
	target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
		"psapi;strmiids;uuid;oleaut32;shlwapi;ws2_32;ole32;user32;bcrypt")
elseif (APPLE)
	target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
		"-framework CoreServices" "-framework CoreFoundation" "-framework AudioUnit"
		"-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMedia"
		"-framework VideoToolbox" "-framework CoreVideo" "-framework Security")
endif ()

target_link_libraries(${FFMPEG_CORE_NAME} INTERFACE
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avformat.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avcodec.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}swscale.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avutil.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}avfilter.${LIB_EXT}"
	"${FFMPEG_LIB_PATH}/${LIB_PREFIX}swresample.${LIB_EXT}")
