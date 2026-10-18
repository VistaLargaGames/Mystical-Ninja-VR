# Used when FreeType was already added with add_subdirectory for the Quest build.
if(NOT TARGET Freetype::Freetype)
    message(FATAL_ERROR "Freetype::Freetype is not available")
endif()
set(Freetype_FOUND TRUE)
set(FREETYPE_FOUND TRUE)
