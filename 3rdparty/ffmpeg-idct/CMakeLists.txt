if(ARCH_ARM64)
	enable_language(C ASM)
	add_library(pcsx2-ffmpeg-idct STATIC
		libavcodec/aarch64/simple_idct_neon.S
	)
	target_include_directories(pcsx2-ffmpeg-idct PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
else()
	add_library(pcsx2-ffmpeg-idct INTERFACE)
endif()

add_library(FFmpegIDCT::FFmpegIDCT ALIAS pcsx2-ffmpeg-idct)
