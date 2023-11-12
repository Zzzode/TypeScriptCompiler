add_mlir_unittest(MLIRGenTests
  TypeToString.cpp
)
get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)

target_include_directories(MLIRGenTests
    PRIVATE
    ${ROOT_DIR}/tsc/tsc-new-parser
    )

target_link_libraries(MLIRGenTests
  PRIVATE
  ${dialect_libs}
  MLIRTypeScript
)
