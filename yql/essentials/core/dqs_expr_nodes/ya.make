LIBRARY()

SRCS(
    dqs_expr_nodes.h
)

PEERDIR(
    yql/essentials/core/expr_nodes
)

SRCDIR(
    yql/essentials/core/expr_nodes_gen
)

RUN_PY3_PROGRAM(
    yql/essentials/core/expr_nodes_gen/gen
    yql_expr_nodes_gen.jnj
    dqs_expr_nodes.json
    dqs_expr_nodes.gen.h
    dqs_expr_nodes.decl.inl.h
    dqs_expr_nodes.defs.inl.h
    NDq
    IN yql_expr_nodes_gen.jnj
    IN dqs_expr_nodes.json
    OUT dqs_expr_nodes.gen.h
    OUT dqs_expr_nodes.decl.inl.h
    OUT dqs_expr_nodes.defs.inl.h
    OUTPUT_INCLUDES
    ${ARCADIA_ROOT}/yql/essentials/core/expr_nodes_gen/yql_expr_nodes_gen.h
    ${ARCADIA_ROOT}/util/generic/hash_set.h
)

END()
