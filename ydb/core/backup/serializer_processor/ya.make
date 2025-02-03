LIBRARY()

SRCS(
    processor.cpp
    zstd_processor.cpp
)

PEERDIR(
    contrib/libs/zstd
)

END()
