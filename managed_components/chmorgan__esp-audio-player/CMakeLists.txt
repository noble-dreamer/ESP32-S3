
set(srcs
    "audio_player.cpp"
)

set(includes
    "include"
)

set(requires "")

if(CONFIG_AUDIO_PLAYER_ENABLE_MP3)
    list(APPEND srcs "audio_mp3.cpp")
endif()

# TODO: move inside of the 'if(CONFIG_AUDIO_PLAYER_ENABLE_MP3)' when everything builds correctly
list(APPEND requires "esp-libhelix-mp3")

if(CONFIG_AUDIO_PLAYER_ENABLE_WAV)
    list(APPEND srcs "audio_wav.cpp")
endif()

if(CONFIG_AUDIO_PLAYER_ENABLE_FLAC)
    list(APPEND srcs "audio_flac.cpp")
    
    # Directly compile FLAC decoder source files and include path
    set(FLAC_COMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../components/flac")
    list(APPEND srcs 
        "${FLAC_COMP_DIR}/flacdecoder.c"
        "${FLAC_COMP_DIR}/bitstreamf.c"
        "${FLAC_COMP_DIR}/tables.c"
    )
    list(APPEND includes 
        "${FLAC_COMP_DIR}"
    )
endif()

idf_component_register(SRCS "${srcs}"
                       REQUIRES "${requires}"
                       INCLUDE_DIRS "${includes}"
                       REQUIRES driver
)
