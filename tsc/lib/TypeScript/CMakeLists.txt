message(STATUS "lib TypeScript")

set_Options()

include_directories(
        ${ROOT_DIR}/tsc/tsc-new-parser
)

link_directories(
        ${ROOT_DIR}/tsc/tsc-new-parser
)

add_mlir_dialect_library(MLIRTypeScript
        TypeScriptDialect.cpp
        TypeScriptOps.cpp
        TypeScriptDialectTranslation.cpp
        AsyncDialectTranslation.cpp
        DiagnosticHelper.cpp
        MLIRGen.cpp
        LowerToAffineLoops.cpp
        LowerToLLVM.cpp
        RelocateConstantPass.cpp
        GCPass.cpp

        ADDITIONAL_HEADER_DIRS
        ${ROOT_DIR}/tsc/tsc-new-parser

        DEPENDS
        MLIRTypeScriptOpsIncGen

        LINK_LIBS PUBLIC
        tsc-new-parser-lib
        MLIRIR
)
