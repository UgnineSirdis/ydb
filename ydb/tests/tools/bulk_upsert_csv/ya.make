PROGRAM()

SRCS(
    main.cpp
)

PEERDIR(
    library/cpp/getopt
    ydb/public/api/protos
    ydb/public/sdk/cpp/client/ydb_driver
    ydb/public/sdk/cpp/client/ydb_table
)

END()
