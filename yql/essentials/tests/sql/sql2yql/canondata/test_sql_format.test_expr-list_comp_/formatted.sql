/* postgres can not */
SELECT
    AsTuple(
        AsList(1, 2) < AsList(1u, 3u),
        AsList(1, 2) < AsList(1u, 2u),
        AsList(1, 2) < AsList(1u, 1u),
        AsList(1, 2) < AsList(2u, 1u),
        AsList(1, 2) < AsList(2u, 3u),
        AsList(1, 2 / 1) < AsList(1u, 3u),
        AsList(1, 2 / 1) < AsList(1u, 2u),
        AsList(1, 2 / 1) < AsList(1u, 1u),
        AsList(1, 2 / 0) < AsList(2u, 3u),
        AsList(1, 2 / 0) < AsList(1u, 3u),
    )
;

SELECT
    AsTuple(
        AsList(1, 2) <= AsList(1u, 3u),
        AsList(1, 2) <= AsList(1u, 2u),
        AsList(1, 2) <= AsList(1u, 1u),
        AsList(1, 2) <= AsList(2u, 1u),
        AsList(1, 2) <= AsList(2u, 3u),
        AsList(1, 2 / 1) <= AsList(1u, 3u),
        AsList(1, 2 / 1) <= AsList(1u, 2u),
        AsList(1, 2 / 1) <= AsList(1u, 1u),
        AsList(1, 2 / 0) <= AsList(2u, 3u),
        AsList(1, 2 / 0) <= AsList(1u, 3u),
    )
;

SELECT
    AsTuple(
        AsList(1, 2) > AsList(1u, 3u),
        AsList(1, 2) > AsList(1u, 2u),
        AsList(1, 2) > AsList(1u, 1u),
        AsList(1, 2) > AsList(2u, 1u),
        AsList(1, 2) > AsList(2u, 3u),
        AsList(1, 2 / 1) > AsList(1u, 3u),
        AsList(1, 2 / 1) > AsList(1u, 2u),
        AsList(1, 2 / 1) > AsList(1u, 1u),
        AsList(1, 2 / 0) > AsList(2u, 3u),
        AsList(1, 2 / 0) > AsList(1u, 3u),
    )
;

SELECT
    AsTuple(
        AsList(1, 2) >= AsList(1u, 3u),
        AsList(1, 2) >= AsList(1u, 2u),
        AsList(1, 2) >= AsList(1u, 1u),
        AsList(1, 2) >= AsList(2u, 1u),
        AsList(1, 2) >= AsList(2u, 3u),
        AsList(1, 2 / 1) >= AsList(1u, 3u),
        AsList(1, 2 / 1) >= AsList(1u, 2u),
        AsList(1, 2 / 1) >= AsList(1u, 1u),
        AsList(1, 2 / 0) >= AsList(2u, 3u),
        AsList(1, 2 / 0) >= AsList(1u, 3u),
    )
;

SELECT
    AsTuple(
        AsList(1, 2, 3) == AsList(1u, 2u),
        AsList(1 / 1, 2 / 1) == AsList(1u, 2u),
        AsList(1 / 1, 2 / 0) == AsList(1u, 2u)
    )
;

SELECT
    AsTuple(
        AsList(1, 2) < AsList(1u, 2u, 3u),
        AsList(1, 2) <= AsList(1u, 2u, 3u),
        AsList(1, 2) > AsList(1u, 2u, 3u),
        AsList(1, 2) >= AsList(1u, 2u, 3u),
        AsList(1, 2, 3) < AsList(1u, 2u),
        AsList(1, 2, 3) <= AsList(1u, 2u),
        AsList(1, 2, 3) > AsList(1u, 2u),
        AsList(1, 2, 3) >= AsList(1u, 2u),
    )
;
