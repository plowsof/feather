find_path(POLYSEED_INCLUDE_DIR polyseed.h)
message(STATUS "POLYSEED PATH ${POLYSEED_INCLUDE_DIR}")

find_library(POLYSEED_LIBRARY polyseed)
message(STATUS "POLYSEED LIBARY ${POLYSEED_LIBRARY}")