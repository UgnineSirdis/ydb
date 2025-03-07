# Generated by devtools/yamaker.

LIBRARY()

SUBSCRIBER(g:cpp-contrib)

VERSION(14.0.6)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm14
    contrib/libs/llvm14/include
    contrib/libs/llvm14/lib/Analysis
    contrib/libs/llvm14/lib/BinaryFormat
    contrib/libs/llvm14/lib/Bitcode/Reader
    contrib/libs/llvm14/lib/Bitcode/Writer
    contrib/libs/llvm14/lib/CodeGen
    contrib/libs/llvm14/lib/Extensions
    contrib/libs/llvm14/lib/IR
    contrib/libs/llvm14/lib/Linker
    contrib/libs/llvm14/lib/MC
    contrib/libs/llvm14/lib/Object
    contrib/libs/llvm14/lib/Passes
    contrib/libs/llvm14/lib/Remarks
    contrib/libs/llvm14/lib/Support
    contrib/libs/llvm14/lib/Target
    contrib/libs/llvm14/lib/Transforms/AggressiveInstCombine
    contrib/libs/llvm14/lib/Transforms/IPO
    contrib/libs/llvm14/lib/Transforms/InstCombine
    contrib/libs/llvm14/lib/Transforms/Instrumentation
    contrib/libs/llvm14/lib/Transforms/ObjCARC
    contrib/libs/llvm14/lib/Transforms/Scalar
    contrib/libs/llvm14/lib/Transforms/Utils
    contrib/libs/llvm14/tools/polly/lib
    contrib/libs/llvm14/tools/polly/lib/External/isl
    contrib/libs/llvm14/tools/polly/lib/External/ppcg
)

ADDINCL(
    contrib/libs/llvm14/lib/LTO
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    LTO.cpp
    LTOBackend.cpp
    LTOCodeGenerator.cpp
    LTOModule.cpp
    SummaryBasedOptimizations.cpp
    ThinLTOCodeGenerator.cpp
    UpdateCompilerUsed.cpp
)

END()
