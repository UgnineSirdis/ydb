LIBRARY()

SRCS(
    checksum_processor.cpp
    processor.cpp
    zstd_processor.cpp
)

PEERDIR(
    contrib/libs/zstd
    ydb/core/backup/common
)

END()
