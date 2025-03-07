# Generated by devtools/yamaker.

PROGRAM()

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
    contrib/libs/llvm14/lib/Bitstream/Reader
    contrib/libs/llvm14/lib/CodeGen
    contrib/libs/llvm14/lib/CodeGen/AsmPrinter
    contrib/libs/llvm14/lib/CodeGen/GlobalISel
    contrib/libs/llvm14/lib/CodeGen/SelectionDAG
    contrib/libs/llvm14/lib/DebugInfo/CodeView
    contrib/libs/llvm14/lib/DebugInfo/DWARF
    contrib/libs/llvm14/lib/Demangle
    contrib/libs/llvm14/lib/ExecutionEngine
    contrib/libs/llvm14/lib/ExecutionEngine/MCJIT
    contrib/libs/llvm14/lib/ExecutionEngine/Orc/Shared
    contrib/libs/llvm14/lib/ExecutionEngine/Orc/TargetProcess
    contrib/libs/llvm14/lib/ExecutionEngine/RuntimeDyld
    contrib/libs/llvm14/lib/IR
    contrib/libs/llvm14/lib/MC
    contrib/libs/llvm14/lib/MC/MCDisassembler
    contrib/libs/llvm14/lib/MC/MCParser
    contrib/libs/llvm14/lib/Object
    contrib/libs/llvm14/lib/ObjectYAML
    contrib/libs/llvm14/lib/ProfileData
    contrib/libs/llvm14/lib/Remarks
    contrib/libs/llvm14/lib/Support
    contrib/libs/llvm14/lib/Target
    contrib/libs/llvm14/lib/Target/X86
    contrib/libs/llvm14/lib/Target/X86/AsmParser
    contrib/libs/llvm14/lib/Target/X86/Disassembler
    contrib/libs/llvm14/lib/Target/X86/MCTargetDesc
    contrib/libs/llvm14/lib/Target/X86/TargetInfo
    contrib/libs/llvm14/lib/TextAPI
    contrib/libs/llvm14/lib/Transforms/CFGuard
    contrib/libs/llvm14/lib/Transforms/Instrumentation
    contrib/libs/llvm14/lib/Transforms/Scalar
    contrib/libs/llvm14/lib/Transforms/Utils
    contrib/libs/llvm14/tools/llvm-exegesis/lib
    contrib/libs/llvm14/tools/llvm-exegesis/lib/X86
)

ADDINCL(
    contrib/libs/llvm14/tools/llvm-exegesis
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DLLVM_EXEGESIS_INITIALIZE_NATIVE_TARGET=InitializeX86ExegesisTarget
)

SRCS(
    llvm-exegesis.cpp
)

END()
