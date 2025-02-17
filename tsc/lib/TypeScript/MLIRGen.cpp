// TODO: it seems in Jit mode, LLVM Engine can resolve external references from loading DLLs

#define DEBUG_TYPE "mlir"

#ifdef GC_ENABLE
#define ADD_GC_ATTRIBUTE true
#endif

#include "TypeScript/MLIRGen.h"
#include "TypeScript/Config.h"
#include "TypeScript/TypeScriptDialect.h"
#include "TypeScript/TypeScriptOps.h"
#include "TypeScript/DiagnosticHelper.h"

#include "TypeScript/MLIRLogic/MLIRCodeLogic.h"
#include "TypeScript/MLIRLogic/MLIRGenContext.h"
#include "TypeScript/MLIRLogic/MLIRNamespaceGuard.h"
#include "TypeScript/MLIRLogic/MLIRTypeHelper.h"
#include "TypeScript/MLIRLogic/MLIRValueGuard.h"

#include "TypeScript/MLIRLogic/TypeOfOpHelper.h"

#include "TypeScript/MLIRLogic/MLIRRTTIHelperVC.h"
#include "TypeScript/VisitorAST.h"

#include "TypeScript/DOM.h"
#include "TypeScript/Defines.h"

// parser includes
#include "dump.h"
#include "file_helper.h"
#include "node_factory.h"
#include "parser.h"
#include "utilities.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/Diagnostics.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Types.h"
#include "mlir/IR/Verifier.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMTypes.h"
#include "mlir/Dialect/DLTI/DLTI.h"
#include "mlir/IR/Diagnostics.h"
#ifdef ENABLE_ASYNC
#include "mlir/Dialect/Async/IR/Async.h"
#endif

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/BinaryFormat/Dwarf.h"
//#include "llvm/IR/DebugInfoMetadata.h"


#include <algorithm>
#include <iterator>
#include <numeric>

using namespace ::typescript;
using namespace ts;
namespace mlir_ts = mlir::typescript;

using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::makeArrayRef;
using llvm::ScopedHashTableScope;
using llvm::SmallVector;
using llvm::StringRef;
using llvm::Twine;

// TODO: optimize of amount of calls to detect return types and if it is was calculated before then do not run it all
// the time

SourceMgrDiagnosticHandlerEx::SourceMgrDiagnosticHandlerEx(llvm::SourceMgr &mgr, mlir::MLIRContext *ctx) : mlir::SourceMgrDiagnosticHandler(mgr, ctx)
{
}

void SourceMgrDiagnosticHandlerEx::emit(mlir::Diagnostic &diag)
{
    emitDiagnostic(diag);
}

namespace
{

enum class IsGeneric
{
    False,
    True
};

enum class TypeProvided
{
    No,
    Yes
};

enum class DisposeDepth
{
    CurrentScope,
    LoopScope,
    FullStack
};

typedef std::tuple<mlir::Type, mlir::Value, TypeProvided> TypeValueInitType;
typedef std::function<TypeValueInitType(mlir::Location, const GenContext &)> TypeValueInitFuncType;

/// Implementation of a simple MLIR emission from the TypeScript AST.
///
/// This will emit operations that are specific to the TypeScript language, preserving
/// the semantics of the language and (hopefully) allow to perform accurate
/// analysis and transformation based on these high level semantics.
class MLIRGenImpl
{
  public:
    MLIRGenImpl(const mlir::MLIRContext &context, const llvm::StringRef &fileNameParam,
                const llvm::StringRef &pathParam, const llvm::SourceMgr &sourceMgr, CompileOptions &compileOptions)
        : builder(&const_cast<mlir::MLIRContext &>(context)), 
          mth(&const_cast<mlir::MLIRContext &>(context), 
            std::bind(&MLIRGenImpl::getClassInfoByFullName, this, std::placeholders::_1), 
            std::bind(&MLIRGenImpl::getGenericClassInfoByFullName, this, std::placeholders::_1), 
            std::bind(&MLIRGenImpl::getInterfaceInfoByFullName, this, std::placeholders::_1), 
            std::bind(&MLIRGenImpl::getGenericInterfaceInfoByFullName, this, std::placeholders::_1)),
          compileOptions(compileOptions), 
          declarationMode(false),
          tempEntryBlock(nullptr),
          sourceMgr(const_cast<llvm::SourceMgr &>(sourceMgr)),
          sourceMgrHandler(const_cast<llvm::SourceMgr &>(sourceMgr), &const_cast<mlir::MLIRContext &>(context)),
          mainSourceFileName(fileNameParam),
          path(pathParam)
    {
        rootNamespace = currentNamespace = std::make_shared<NamespaceInfo>();
        const_cast<llvm::SourceMgr &>(sourceMgr).setIncludeDirs({pathParam.str()});
    }

    mlir::LogicalResult report(SourceFile module, const std::vector<SourceFile> &includeFiles)
    {
        // output diag info
        auto hasAnyError = false;
        auto fileName = convertWideToUTF8(module->fileName);
        for (auto diag : module->parseDiagnostics)
        {
            hasAnyError |= diag.category == DiagnosticCategory::Error;
            if (diag.category == DiagnosticCategory::Error)
            {
                emitError(loc2(module, fileName, diag.start, diag.length), convertWideToUTF8(diag.messageText));
            }
            else
            {
                emitWarning(loc2(module, fileName, diag.start, diag.length), convertWideToUTF8(diag.messageText));
            }
        }

        for (auto incFile : includeFiles)
        {
            auto fileName = convertWideToUTF8(incFile->fileName);
            for (auto diag : incFile->parseDiagnostics)
            {
                hasAnyError |= diag.category == DiagnosticCategory::Error;
                if (diag.category == DiagnosticCategory::Error)
                {
                    emitError(loc2(incFile, fileName, diag.start, diag.length), convertWideToUTF8(diag.messageText));
                }
                else
                {
                    emitWarning(loc2(incFile, fileName, diag.start, diag.length), convertWideToUTF8(diag.messageText));
                }
            }
        }

        return hasAnyError ? mlir::failure() : mlir::success();
    }

    std::pair<SourceFile, std::vector<SourceFile>> loadMainSourceFile()
    {
        const auto *sourceBuf = sourceMgr.getMemoryBuffer(sourceMgr.getMainFileID());
        auto sourceFileLoc = mlir::FileLineColLoc::get(builder.getContext(),
                    sourceBuf->getBufferIdentifier(), /*line=*/0, /*column=*/0);
        return loadSourceBuf(sourceFileLoc, sourceBuf, true);
    }    

    std::pair<SourceFile, std::vector<SourceFile>> loadSourceBuf(mlir::Location location, const llvm::MemoryBuffer *sourceBuf, bool isMain = false)
    {
        std::vector<SourceFile> includeFiles;
        std::vector<string> filesToProcess;

        Parser parser;
        auto sourceFile = parser.parseSourceFile(stows(mainSourceFileName.str()), stows(sourceBuf->getBuffer().str()), ScriptTarget::Latest);

        // add default lib
        if (isMain)
        {
            if (sourceFile->hasNoDefaultLib)
            {
                compileOptions.noDefaultLib = true;
            }

            if (!compileOptions.noDefaultLib)
            {
                filesToProcess.push_back(S("jslib/lib.d.ts"));
            }
        }

        for (auto refFile : sourceFile->referencedFiles)
        {
            filesToProcess.push_back(refFile.fileName);
        }

        while (filesToProcess.size() > 0)
        {
            string includeFileName = filesToProcess.back();
            SmallString<256> fullPath;
            auto includeFileNameUtf8 = convertWideToUTF8(includeFileName);
            sys::path::append(fullPath, includeFileNameUtf8);

            filesToProcess.pop_back();

            std::string actualFilePath;
            auto id = sourceMgr.AddIncludeFile(std::string(fullPath), SMLoc(), actualFilePath);
            if (!id)
            {
                emitError(location, "can't open file: ") << fullPath;
                continue;
            }

            const auto *sourceBuf = sourceMgr.getMemoryBuffer(id);

            Parser parser;
            auto includeFile =
                parser.parseSourceFile(ConvertUTF8toWide(actualFilePath), stows(sourceBuf->getBuffer().str()), ScriptTarget::Latest);
            for (auto refFile : includeFile->referencedFiles)
            {
                filesToProcess.push_back(refFile.fileName);
            }

            includeFiles.push_back(includeFile);
        }

        std::reverse(includeFiles.begin(), includeFiles.end());

        return {sourceFile, includeFiles};
    }

    mlir::LogicalResult showMessages(SourceFile module, std::vector<SourceFile> includeFiles)
    {
        mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
            sourceMgrHandler.emit(diag);
        });

        if (mlir::failed(report(module, includeFiles)))
        {
            return mlir::failure();
        }

        return mlir::success();
    }

    mlir::ModuleOp mlirGenSourceFile(SourceFile module, std::vector<SourceFile> includeFiles)
    {
        if (mlir::failed(showMessages(module, includeFiles)))
        {
            return nullptr;
        }        

        if (mlir::failed(mlirGenCodeGenInit(module)))
        {
            return nullptr;
        }

        SymbolTableScopeT varScope(symbolTable);
        llvm::ScopedHashTableScope<StringRef, NamespaceInfo::TypePtr> fullNamespacesMapScope(fullNamespacesMap);
        llvm::ScopedHashTableScope<StringRef, VariableDeclarationDOM::TypePtr> fullNameGlobalsMapScope(
            fullNameGlobalsMap);
        llvm::ScopedHashTableScope<StringRef, GenericFunctionInfo::TypePtr> fullNameGenericFunctionsMapScope(
            fullNameGenericFunctionsMap);
        llvm::ScopedHashTableScope<StringRef, ClassInfo::TypePtr> fullNameClassesMapScope(fullNameClassesMap);
        llvm::ScopedHashTableScope<StringRef, GenericClassInfo::TypePtr> fullNameGenericClassesMapScope(
            fullNameGenericClassesMap);
        llvm::ScopedHashTableScope<StringRef, InterfaceInfo::TypePtr> fullNameInterfacesMapScope(fullNameInterfacesMap);
        llvm::ScopedHashTableScope<StringRef, GenericInterfaceInfo::TypePtr> fullNameGenericInterfacesMapScope(
            fullNameGenericInterfacesMap);

        if (mlir::succeeded(mlirDiscoverAllDependencies(module, includeFiles)) &&
            mlir::succeeded(mlirCodeGenModule(module, includeFiles)))
        {
            return theModule;
        }

        return nullptr;
    }

  private:
    mlir::LogicalResult mlirGenCodeGenInit(SourceFile module)
    {
        sourceFile = module;

        auto location = loc(module);
        if (compileOptions.generateDebugInfo)
        {
            auto isOptimized = false;

            // TODO: in file location helper
            SmallString<256> FullName(mainSourceFileName);
            sys::path::remove_filename(FullName);
            auto file = mlir::LLVM::DIFileAttr::get(builder.getContext(), sys::path::filename(mainSourceFileName), FullName);

            // CU
            unsigned sourceLanguage = llvm::dwarf::DW_LANG_C; 
            auto producer = builder.getStringAttr("TypeScript Native Compiler");
            auto emissionKind = mlir::LLVM::DIEmissionKind::Full;
            auto compileUnit = mlir::LLVM::DICompileUnitAttr::get(builder.getContext(), sourceLanguage, file, producer, isOptimized, emissionKind);        

            auto fusedLocWithCU = mlir::FusedLoc::get(
                    builder.getContext(), {location}, compileUnit);                

            location = fusedLocWithCU;
        }

        // We create an empty MLIR module and codegen functions one at a time and
        // add them to the module.
        theModule = mlir::ModuleOp::create(location, mainSourceFileName);

        if (!compileOptions.moduleTargetTriple.empty())
        {
            theModule->setAttr(
                mlir::LLVM::LLVMDialect::getTargetTripleAttrName(), 
                builder.getStringAttr(compileOptions.moduleTargetTriple));

            // DataLayout for IndexType
            auto indexSize = mlir::DataLayoutEntryAttr::get(builder.getIndexType(), builder.getI32IntegerAttr(compileOptions.sizeBits));
            theModule->setAttr("dlti.dl_spec", mlir::DataLayoutSpecAttr::get(builder.getContext(), {indexSize}));
        }

        builder.setInsertionPointToStart(theModule.getBody());

        return mlir::success();
    }

    mlir::LogicalResult createDeclarationExportGlobalVar(const GenContext &genContext)
    {
        if (!declExports.rdbuf()->in_avail())
        {
            return mlir::success();
        }

        auto declText = convertWideToUTF8(declExports.str());

        LLVM_DEBUG(llvm::dbgs() << "\n!! export declaration: \n" << declText << "\n";);

        auto typeWithInit = [&](mlir::Location location, const GenContext &genContext) {
            auto litValue = V(mlirGenStringValue(location, declText, true));
            return std::make_tuple(litValue.getType(), litValue, TypeProvided::No);            
        };

        VariableClass varClass = VariableType::Var;
        varClass.isExport = true;
        registerVariable(mlir::UnknownLoc::get(builder.getContext()), SHARED_LIB_DECLARATIONS, true, varClass, typeWithInit, genContext);
        return mlir::success();
    }

    int processStatements(NodeArray<Statement> statements,
                          mlir::SmallVector<std::unique_ptr<mlir::Diagnostic>> &postponedMessages,
                          const GenContext &genContext)
    {
        auto notResolved = 0;
        do
        {
            // clear previous errors
            postponedMessages.clear();

            // main cycles
            auto noErrorLocation = true;
            mlir::Location errorLocation = mlir::UnknownLoc::get(builder.getContext());
            auto lastTimeNotResolved = notResolved;
            notResolved = 0;
            for (auto &statement : statements)
            {
                if (statement->processed)
                {
                    continue;
                }

                if (failed(mlirGen(statement, genContext)))
                {
                    emitError(loc(statement), "failed statement");

                    notResolved++;
                    if (noErrorLocation)
                    {
                        errorLocation = loc(statement);
                        noErrorLocation = false;
                    }
                }
                else
                {
                    statement->processed = true;
                }
            }

            if (lastTimeNotResolved > 0 && lastTimeNotResolved == notResolved)
            {
                break;
            }

        } while (notResolved > 0);

        return notResolved;
    }

    mlir::LogicalResult outputDiagnostics(mlir::SmallVector<std::unique_ptr<mlir::Diagnostic>> &postponedMessages,
                                          int notResolved)
    {
        // print errors
        if (notResolved)
        {
            printDiagnostics(sourceMgrHandler, postponedMessages, compileOptions.disableWarnings);
        }

        postponedMessages.clear();

        // we return error when we can't generate code
        if (notResolved)
        {
            return mlir::failure();
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirDiscoverAllDependencies(SourceFile module, std::vector<SourceFile> includeFiles = {})
    {
        mlir::SmallVector<std::unique_ptr<mlir::Diagnostic>> postponedMessages;
        mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
            postponedMessages.emplace_back(new mlir::Diagnostic(std::move(diag)));
        });

        llvm::ScopedHashTableScope<StringRef, VariableDeclarationDOM::TypePtr> fullNameGlobalsMapScope(
            fullNameGlobalsMap);

        // Process of discovery here
        GenContext genContextPartial{};
        genContextPartial.allowPartialResolve = true;
        genContextPartial.dummyRun = true;
        // TODO: no need to clean up here as whole module will be removed
        //genContextPartial.cleanUps = new mlir::SmallVector<mlir::Block *>();
        //genContextPartial.cleanUpOps = new mlir::SmallVector<mlir::Operation *>();

        for (auto includeFile : includeFiles)
        {
            MLIRValueGuard<llvm::StringRef> vgFileName(mainSourceFileName); 
            auto fileNameUtf8 = convertWideToUTF8(includeFile->fileName);
            mainSourceFileName = fileNameUtf8;

            if (failed(mlirGen(includeFile->statements, genContextPartial)))
            {
                outputDiagnostics(postponedMessages, 1);
                return mlir::failure();
            }
        }

        auto notResolved = processStatements(module->statements, postponedMessages, genContextPartial);

        genContextPartial.clean();

        // clean up
        theModule.getBody()->clear();

        // clear state
        for (auto &statement : module->statements)
        {
            statement->processed = false;
        }

        if (failed(outputDiagnostics(postponedMessages, notResolved)))
        {
            return mlir::failure();
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirCodeGenModule(SourceFile module, std::vector<SourceFile> includeFiles = {},
                                          bool validate = true)
    {
        mlir::SmallVector<std::unique_ptr<mlir::Diagnostic>> postponedWarningsMessages;
        mlir::SmallVector<std::unique_ptr<mlir::Diagnostic>> postponedMessages;
        mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
            if (diag.getSeverity() == mlir::DiagnosticSeverity::Error)
            {
                postponedMessages.emplace_back(new mlir::Diagnostic(std::move(diag)));
            }
            else
            {
                postponedWarningsMessages.emplace_back(new mlir::Diagnostic(std::move(diag)));
            }
        });

        // Process generating here
        declExports.str(S(""));
        declExports.clear();
        exports.str(S(""));
        exports.clear();
        GenContext genContext{};

        for (auto includeFile : includeFiles)
        {
            MLIRValueGuard<llvm::StringRef> vgFileName(mainSourceFileName); 
            auto fileNameUtf8 = convertWideToUTF8(includeFile->fileName);
            mainSourceFileName = fileNameUtf8;

            if (failed(mlirGen(includeFile->statements, genContext)))
            {
                outputDiagnostics(postponedMessages, 1);
                return mlir::failure();
            }
        }

        auto notResolved = processStatements(module->statements, postponedMessages, genContext);
        if (failed(outputDiagnostics(postponedMessages, notResolved)))
        {
            return mlir::failure();
        }
       
        // exports
        createDeclarationExportGlobalVar(genContext);

        clearTempModule();

        // Verify the module after we have finished constructing it, this will check
        // the structural properties of the IR and invoke any specific verifiers we
        // have on the TypeScript operations.
        if (validate && failed(mlir::verify(theModule)))
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! broken module: \n" << theModule << "\n";);

            theModule.emitError("module verification error");

            // to show all errors now
            outputDiagnostics(postponedMessages, 1);
            return mlir::failure();
        }

        printDiagnostics(sourceMgrHandler, postponedWarningsMessages, compileOptions.disableWarnings);

        return mlir::success();
    }

    bool registerNamespace(llvm::StringRef namePtr, bool isFunctionNamespace = false)
    {
        if (isFunctionNamespace)
        {
            std::string res;
            res += ".f_";
            res += namePtr;
            namePtr = StringRef(res).copy(stringAllocator);
        }
        else
        {
            namePtr = StringRef(namePtr).copy(stringAllocator);
        }

        auto fullNamePtr = getFullNamespaceName(namePtr);
        auto &namespacesMap = getNamespaceMap();
        auto it = namespacesMap.find(namePtr);
        if (it == namespacesMap.end())
        {
            auto newNamespacePtr = std::make_shared<NamespaceInfo>();
            newNamespacePtr->name = namePtr;
            newNamespacePtr->fullName = fullNamePtr;
            newNamespacePtr->namespaceType = getNamespaceType(fullNamePtr);
            newNamespacePtr->parentNamespace = currentNamespace;
            newNamespacePtr->isFunctionNamespace = isFunctionNamespace;

            namespacesMap.insert({namePtr, newNamespacePtr});
            if (!isFunctionNamespace && !fullNamespacesMap.count(fullNamePtr))
            {
                // TODO: full investigation needed, if i register function namespace as full namespace, it will fail
                // running
                fullNamespacesMap.insert(fullNamePtr, newNamespacePtr);
            }

            currentNamespace = newNamespacePtr;
        }
        else
        {
            currentNamespace = it->getValue();
            return false;
        }

        return true;
    }

    mlir::LogicalResult exitNamespace()
    {
        // TODO: it will increase reference count, investigate how to fix it
        currentNamespace = currentNamespace->parentNamespace;
        return mlir::success();
    }

    mlir::LogicalResult mlirGenNamespace(ModuleDeclaration moduleDeclarationAST, const GenContext &genContext)
    {
        auto location = loc(moduleDeclarationAST);

        auto namespaceName = MLIRHelper::getName(moduleDeclarationAST->name, stringAllocator);
        auto namePtr = namespaceName;

        MLIRNamespaceGuard nsGuard(currentNamespace);
        registerNamespace(namePtr);

        return mlirGenBody(moduleDeclarationAST->body, genContext);
    }

    mlir::LogicalResult mlirGen(ModuleDeclaration moduleDeclarationAST, const GenContext &genContext)
    {
#ifdef MODULE_AS_NAMESPACE
        return mlirGenNamespace(moduleDeclarationAST, genContext);
#else
        auto isNamespace = (moduleDeclarationAST->flags & NodeFlags::Namespace) == NodeFlags::Namespace;
        auto isNestedNamespace =
            (moduleDeclarationAST->flags & NodeFlags::NestedNamespace) == NodeFlags::NestedNamespace;
        if (isNamespace || isNestedNamespace)
        {
            return mlirGenNamespace(moduleDeclarationAST, genContext);
        }

        auto location = loc(moduleDeclarationAST);

        auto moduleName = MLIRHelper::getName(moduleDeclarationAST->name);

        auto moduleOp = builder.create<mlir::ModuleOp>(location, StringRef(moduleName));

        builder.setInsertionPointToStart(&moduleOp.getBody().front());

        // save module theModule
        auto parentModule = theModule;
        theModule = moduleOp;

        GenContext moduleGenContext{};
        auto result = mlirGenBody(moduleDeclarationAST->body, moduleGenContext);
        auto result = V(result);

        // restore
        theModule = parentModule;

        builder.setInsertionPointAfter(moduleOp);

        return result;
#endif
    }

    mlir::LogicalResult mlirGenInclude(mlir::Location location, StringRef filePath, const GenContext &genContext)
    {
        MLIRValueGuard<bool> vg(declarationMode);
        declarationMode = true;

        auto [importSource, importIncludeFiles] = loadIncludeFile(location, filePath);
        if (!importSource)
        {
            return mlir::failure();
        }

        if (mlir::failed(showMessages(importSource, importIncludeFiles)))
        {
            return mlir::failure();
        }          

        if (mlir::succeeded(mlirDiscoverAllDependencies(importSource, importIncludeFiles)) &&
            mlir::succeeded(mlirCodeGenModule(importSource, importIncludeFiles, false)))
        {
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGenImportSharedLib(mlir::Location location, StringRef filePath, bool dynamic, const GenContext &genContext)
    {
        // TODO: ...
        std::string errMsg;
        auto dynLib = llvm::sys::DynamicLibrary::getPermanentLibrary(filePath.str().c_str(), &errMsg);
        if (!dynLib.isValid())
        {
            emitError(location, errMsg);
            return mlir::failure();
        }

        // load library
        auto fullInitGlobalFuncName = getFullNamespaceName(MLIRHelper::getAnonymousName(location, ".ll"));

        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);

            // create global construct
            auto funcType = getFunctionType({}, {}, false);

            if (mlir::failed(mlirGenFunctionBody(location, fullInitGlobalFuncName, funcType,
                [&](const GenContext &genContext) {
                    auto litValue = mlirGenStringValue(location, filePath.str());
                    auto strVal = cast(location, getStringType(), litValue, genContext);
                    builder.create<mlir_ts::LoadLibraryPermanentlyOp>(location, mth.getI32Type(), strVal);
                    return mlir::success();
                }, genContext)))
            {
                return mlir::failure();
            }
        }

        builder.create<mlir_ts::GlobalConstructorOp>(location, mlir::FlatSymbolRefAttr::get(builder.getContext(), fullInitGlobalFuncName));

        // TODO: for now, we have code in TS to load methods from DLL/Shared libs
        if (auto addrOfDeclText = dynLib.getAddressOfSymbol(SHARED_LIB_DECLARATIONS))
        {
            std::string result;
            // process shared lib declarations
            auto dataPtr = *(const char**)addrOfDeclText;
            if (dynamic)
            {
                // TODO: use option variable instead of "this hack"
                result = MLIRHelper::replaceAll(dataPtr, "@dllimport", "@dllimport('.')");
                dataPtr = result.c_str();
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! Shared lib import: \n" << dataPtr << "\n";);

            auto importData = ConvertUTF8toWide(dataPtr);
            if (mlir::failed(parsePartialStatements(importData, genContext, false)))
            {
                //assert(false);
                return mlir::failure();
            }            
        }
        else
        {
            emitWarning(location, "missing information about shared library. (reference " SHARED_LIB_DECLARATIONS " is missing)");
        }

        return mlir::success();
    }    

    mlir::LogicalResult mlirGen(ImportDeclaration importDeclarationAST, const GenContext &genContext)
    {
        auto location = loc(importDeclarationAST);

        auto result = mlirGen(importDeclarationAST->moduleSpecifier, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto modulePath = V(result);

        auto constantOp = modulePath.getDefiningOp<mlir_ts::ConstantOp>();
        assert(constantOp);
        auto valueAttr = constantOp.getValueAttr().cast<mlir::StringAttr>();

        auto stringVal = valueAttr.getValue();

        std::string fullPath;
        fullPath += stringVal;
#ifdef WIN_LOADSHAREDLIBS
#endif        
#ifdef LINUX_LOADSHAREDLIBS
        // rebuild file path
        auto fileName = sys::path::filename(stringVal);
        auto path = stringVal.substr(0, stringVal.size() - fileName.size());
        fullPath = path;
        fullPath += "lib";
        fullPath += fileName;
#endif

        if (sys::path::extension(fullPath) == "")
        {
#ifdef WIN_LOADSHAREDLIBS
            fullPath += ".dll";
#endif
#ifdef LINUX_LOADSHAREDLIBS
            fullPath += ".so";
#endif
        }

        if (sys::fs::exists(fullPath))
        {
            //auto dynamic = MLIRHelper::hasDecorator(importDeclarationAST, "dynamic");
            auto dynamic = !MLIRHelper::hasDecorator(importDeclarationAST, "static");

            // this is shared lib.
            return mlirGenImportSharedLib(location, fullPath, dynamic, genContext);    
        }

        return mlirGenInclude(location, stringVal, genContext);
    }

    boolean isStatement(SyntaxKind kind)
    {
        switch (kind)
        {
        case SyntaxKind::FunctionDeclaration:
        case SyntaxKind::ExpressionStatement:
        case SyntaxKind::VariableStatement:
        case SyntaxKind::IfStatement:
        case SyntaxKind::ReturnStatement:
        case SyntaxKind::LabeledStatement:
        case SyntaxKind::DoStatement:
        case SyntaxKind::WhileStatement:
        case SyntaxKind::ForStatement:
        case SyntaxKind::ForInStatement:
        case SyntaxKind::ForOfStatement:
        case SyntaxKind::ContinueStatement:
        case SyntaxKind::BreakStatement:
        case SyntaxKind::SwitchStatement:
        case SyntaxKind::ThrowStatement:
        case SyntaxKind::TryStatement:
        case SyntaxKind::TypeAliasDeclaration:
        case SyntaxKind::Block:
        case SyntaxKind::EnumDeclaration:
        case SyntaxKind::ClassDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::ImportEqualsDeclaration:
        case SyntaxKind::ImportDeclaration:
        case SyntaxKind::ModuleDeclaration:
        case SyntaxKind::DebuggerStatement:
        case SyntaxKind::EmptyStatement:
            return true;
        default:
            return false;
        }
    }

    mlir::LogicalResult mlirGenBody(Node body, const GenContext &genContext)
    {
        auto kind = (SyntaxKind)body;
        if (kind == SyntaxKind::Block)
        {
            return mlirGen(body.as<Block>(), genContext);
        }

        if (kind == SyntaxKind::ModuleBlock)
        {
            return mlirGen(body.as<ModuleBlock>(), genContext);
        }

        if (isStatement(body))
        {
            return mlirGen(body.as<Statement>(), genContext);
        }

        if (body.is<Expression>())
        {
            auto result = mlirGen(body.as<Expression>(), genContext);
            EXIT_IF_FAILED(result)
            auto resultValue = V(result);
            if (resultValue)
            {
                return mlirGenReturnValue(loc(body), resultValue, false, genContext);
            }

            builder.create<mlir_ts::ReturnOp>(loc(body));
            return mlir::success();
        }

        llvm_unreachable("unknown body type");
    }

    void clearState(NodeArray<Statement> statements)
    {
        for (auto &statement : statements)
        {
            statement->processed = false;
        }
    }

    mlir::LogicalResult mlirGen(NodeArray<Statement> statements, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto notResolved = 0;
        do
        {
            auto noErrorLocation = true;
            mlir::Location errorLocation = mlir::UnknownLoc::get(builder.getContext());
            auto lastTimeNotResolved = notResolved;
            notResolved = 0;
            for (auto &statement : statements)
            {
                if (statement->processed)
                {
                    continue;
                }

                if (failed(mlirGen(statement, genContext)))
                {
                    if (noErrorLocation)
                    {
                        errorLocation = loc(statement);
                        noErrorLocation = false;
                    }

                    notResolved++;
                }
                else
                {
                    statement->processed = true;
                }
            }

            // repeat if not all resolved
            if (lastTimeNotResolved > 0 && lastTimeNotResolved == notResolved)
            {
                // class can depends on other class declarations
                emitError(errorLocation, "can't resolve dependencies in namespace");
                return mlir::failure();
            }
        } while (notResolved > 0);

        // clear up state
        for (auto &statement : statements)
        {
            statement->processed = false;
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(NodeArray<Statement> statements, std::function<bool(Statement)> filter,
                                const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto notResolved = 0;
        do
        {
            auto noErrorLocation = true;
            mlir::Location errorLocation = mlir::UnknownLoc::get(builder.getContext());
            auto lastTimeNotResolved = notResolved;
            notResolved = 0;
            for (auto &statement : statements)
            {
                if (statement->processed)
                {
                    continue;
                }

                if (!filter(statement))
                {
                    continue;
                }

                if (failed(mlirGen(statement, genContext)))
                {
                    if (noErrorLocation)
                    {
                        errorLocation = loc(statement);
                        noErrorLocation = false;
                    }

                    notResolved++;
                }
                else
                {
                    statement->processed = true;
                }
            }

            // repeat if not all resolved
            if (lastTimeNotResolved > 0 && lastTimeNotResolved == notResolved)
            {
                // class can depends on other class declarations
                emitError(errorLocation, "can't resolve dependencies in namespace");
                return mlir::failure();
            }
        } while (notResolved > 0);

        // clear up state
        for (auto &statement : statements)
        {
            statement->processed = false;
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(ModuleBlock moduleBlockAST, const GenContext &genContext)
    {
        return mlirGen(moduleBlockAST->statements, genContext);
    }

    static bool processIfDeclaration(Statement statement)
    {
        switch ((SyntaxKind)statement)
        {
        case SyntaxKind::FunctionDeclaration:
        case SyntaxKind::ClassDeclaration:
        case SyntaxKind::InterfaceDeclaration:
        case SyntaxKind::EnumDeclaration:
            return true;
        }

        return false;
    }

    mlir::LogicalResult mlirGen(Block blockAST, const GenContext &genContext)
    {
        auto location = loc(blockAST);

        SymbolTableScopeT varScope(symbolTable);
        GenContext genContextUsing(genContext);
        genContextUsing.parentBlockContext = &genContext;

        auto usingVars = std::make_unique<SmallVector<ts::VariableDeclarationDOM::TypePtr>>();
        genContextUsing.usingVars = usingVars.get();

        if (genContextUsing.generatedStatements.size() > 0)
        {
            // we need to process it only once (to prevent it processing in nested functions with body)
            NodeArray<Statement> generatedStatements;
            std::copy(genContextUsing.generatedStatements.begin(), genContextUsing.generatedStatements.end(),
                      std::back_inserter(generatedStatements));

            // clean up
            genContextUsing.generatedStatements.clear();

            // auto generated code
            for (auto statement : generatedStatements)
            {
                if (failed(mlirGen(statement, genContextUsing)))
                {
                    return mlir::failure();
                }
            }
        }

        for (auto statement : blockAST->statements)
        {
            if (statement->processed)
            {
                continue;
            }

            if (failed(mlirGen(statement, genContextUsing)))
            {
                // now try to process all internal declarations
                // process all declrations
                if (mlir::failed(mlirGen(blockAST->statements, processIfDeclaration, genContextUsing)))
                {
                    return mlir::failure();
                }

                // try to process it again
                if (failed(mlirGen(statement, genContextUsing)))
                {
                    return mlir::failure();
                }
            }

            statement->processed = true;
        }

        // we need to call dispose for those which are in "using"
        EXIT_IF_FAILED(mlirGenDisposable(location, DisposeDepth::CurrentScope, {}, &genContextUsing));

        // clear states to be able to run second time
        clearState(blockAST->statements);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenDisposable(mlir::Location location, DisposeDepth disposeDepth, std::string loopLabel, const GenContext* genContext)
    {
        if (genContext->usingVars != nullptr)
        {
            for (auto vi : *genContext->usingVars)
            {
                auto varInTable = symbolTable.lookup(vi->getName());
                auto callResult = mlirGenCallThisMethod(location, varInTable.first, SYMBOL_DISPOSE, undefined, {}, *genContext);
                EXIT_IF_FAILED(callResult);            
            }

            // remove when used
            if (disposeDepth == DisposeDepth::CurrentScope)
            {
                const_cast<GenContext *>(genContext)->usingVars = nullptr;
            }

            auto continueIntoDepth = disposeDepth == DisposeDepth::FullStack
                    || disposeDepth == DisposeDepth::LoopScope && genContext->isLoop && genContext->loopLabel != loopLabel;
            if (continueIntoDepth)
            {
                EXIT_IF_FAILED(mlirGenDisposable(location, disposeDepth, {}, genContext->parentBlockContext));
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(Statement statementAST, const GenContext &genContext)
    {
        auto kind = (SyntaxKind)statementAST;
        if (kind == SyntaxKind::FunctionDeclaration)
        {
            return mlirGen(statementAST.as<FunctionDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::ExpressionStatement)
        {
            return mlirGen(statementAST.as<ExpressionStatement>(), genContext);
        }
        else if (kind == SyntaxKind::VariableStatement)
        {
            return mlirGen(statementAST.as<VariableStatement>(), genContext);
        }
        else if (kind == SyntaxKind::IfStatement)
        {
            return mlirGen(statementAST.as<IfStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ReturnStatement)
        {
            return mlirGen(statementAST.as<ReturnStatement>(), genContext);
        }
        else if (kind == SyntaxKind::LabeledStatement)
        {
            return mlirGen(statementAST.as<LabeledStatement>(), genContext);
        }
        else if (kind == SyntaxKind::DoStatement)
        {
            return mlirGen(statementAST.as<DoStatement>(), genContext);
        }
        else if (kind == SyntaxKind::WhileStatement)
        {
            return mlirGen(statementAST.as<WhileStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ForStatement)
        {
            return mlirGen(statementAST.as<ForStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ForInStatement)
        {
            return mlirGen(statementAST.as<ForInStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ForOfStatement)
        {
            return mlirGen(statementAST.as<ForOfStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ContinueStatement)
        {
            return mlirGen(statementAST.as<ContinueStatement>(), genContext);
        }
        else if (kind == SyntaxKind::BreakStatement)
        {
            return mlirGen(statementAST.as<BreakStatement>(), genContext);
        }
        else if (kind == SyntaxKind::SwitchStatement)
        {
            return mlirGen(statementAST.as<SwitchStatement>(), genContext);
        }
        else if (kind == SyntaxKind::ThrowStatement)
        {
            return mlirGen(statementAST.as<ThrowStatement>(), genContext);
        }
        else if (kind == SyntaxKind::TryStatement)
        {
            return mlirGen(statementAST.as<TryStatement>(), genContext);
        }
        else if (kind == SyntaxKind::TypeAliasDeclaration)
        {
            // declaration
            return mlirGen(statementAST.as<TypeAliasDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::Block)
        {
            return mlirGen(statementAST.as<Block>(), genContext);
        }
        else if (kind == SyntaxKind::EnumDeclaration)
        {
            // declaration
            return mlirGen(statementAST.as<EnumDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::ClassDeclaration)
        {
            // declaration
            return mlirGen(statementAST.as<ClassDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::InterfaceDeclaration) 
        {
            // declaration
            return mlirGen(statementAST.as<InterfaceDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::ImportEqualsDeclaration)
        {
            // declaration
            return mlirGen(statementAST.as<ImportEqualsDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::ImportDeclaration)
        {
            // declaration
            return mlirGen(statementAST.as<ImportDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::ModuleDeclaration)
        {
            return mlirGen(statementAST.as<ModuleDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::DebuggerStatement)
        {
            return mlirGen(statementAST.as<DebuggerStatement>(), genContext);
        }
        else if (kind == SyntaxKind::EmptyStatement ||
                 kind == SyntaxKind::Unknown /*TODO: temp solution to treat null statements as empty*/)
        {
            return mlir::success();
        }

        llvm_unreachable("unknown statement type");
    }

    mlir::LogicalResult mlirGen(ExpressionStatement expressionStatementAST, const GenContext &genContext)
    {
        auto result = mlirGen(expressionStatementAST->expression, genContext);
        EXIT_IF_FAILED(result)
        return mlir::success();
    }

    ValueOrLogicalResult mlirGen(Expression expressionAST, const GenContext &genContext)
    {
        auto kind = (SyntaxKind)expressionAST;
        if (kind == SyntaxKind::Identifier)
        {
            return mlirGen(expressionAST.as<Identifier>(), genContext);
        }
        else if (kind == SyntaxKind::PropertyAccessExpression)
        {
            return mlirGen(expressionAST.as<PropertyAccessExpression>(), genContext);
        }
        else if (kind == SyntaxKind::CallExpression)
        {
            return mlirGen(expressionAST.as<CallExpression>(), genContext);
        }
        else if (kind == SyntaxKind::NumericLiteral)
        {
            return mlirGen(expressionAST.as<NumericLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::StringLiteral)
        {
            return mlirGen(expressionAST.as<ts::StringLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::NoSubstitutionTemplateLiteral)
        {
            return mlirGen(expressionAST.as<NoSubstitutionTemplateLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::BigIntLiteral)
        {
            return mlirGen(expressionAST.as<BigIntLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::NullKeyword)
        {
            return mlirGen(expressionAST.as<NullLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::TrueKeyword)
        {
            return mlirGen(expressionAST.as<TrueLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::FalseKeyword)
        {
            return mlirGen(expressionAST.as<FalseLiteral>(), genContext);
        }
        else if (kind == SyntaxKind::ArrayLiteralExpression)
        {
            return mlirGen(expressionAST.as<ArrayLiteralExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ObjectLiteralExpression)
        {
            return mlirGen(expressionAST.as<ObjectLiteralExpression>(), genContext);
        }
        else if (kind == SyntaxKind::SpreadElement)
        {
            return mlirGen(expressionAST.as<SpreadElement>(), genContext);
        }
        else if (kind == SyntaxKind::BinaryExpression)
        {
            return mlirGen(expressionAST.as<BinaryExpression>(), genContext);
        }
        else if (kind == SyntaxKind::PrefixUnaryExpression)
        {
            return mlirGen(expressionAST.as<PrefixUnaryExpression>(), genContext);
        }
        else if (kind == SyntaxKind::PostfixUnaryExpression)
        {
            return mlirGen(expressionAST.as<PostfixUnaryExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ParenthesizedExpression)
        {
            return mlirGen(expressionAST.as<ParenthesizedExpression>(), genContext);
        }
        else if (kind == SyntaxKind::TypeOfExpression)
        {
            return mlirGen(expressionAST.as<TypeOfExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ConditionalExpression)
        {
            return mlirGen(expressionAST.as<ConditionalExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ElementAccessExpression)
        {
            return mlirGen(expressionAST.as<ElementAccessExpression>(), genContext);
        }
        else if (kind == SyntaxKind::FunctionExpression)
        {
            return mlirGen(expressionAST.as<FunctionExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ArrowFunction)
        {
            return mlirGen(expressionAST.as<ArrowFunction>(), genContext);
        }
        else if (kind == SyntaxKind::TypeAssertionExpression)
        {
            return mlirGen(expressionAST.as<TypeAssertion>(), genContext);
        }
        else if (kind == SyntaxKind::AsExpression)
        {
            return mlirGen(expressionAST.as<AsExpression>(), genContext);
        }
        else if (kind == SyntaxKind::TemplateExpression)
        {
            return mlirGen(expressionAST.as<TemplateLiteralLikeNode>(), genContext);
        }
        else if (kind == SyntaxKind::TaggedTemplateExpression)
        {
            return mlirGen(expressionAST.as<TaggedTemplateExpression>(), genContext);
        }
        else if (kind == SyntaxKind::NewExpression)
        {
            return mlirGen(expressionAST.as<NewExpression>(), genContext);
        }
        else if (kind == SyntaxKind::DeleteExpression)
        {
            mlirGen(expressionAST.as<DeleteExpression>(), genContext);
            return mlir::success();
        }
        else if (kind == SyntaxKind::ThisKeyword)
        {
            if ((expressionAST->internalFlags & InternalFlags::ThisArgAlias) == InternalFlags::ThisArgAlias)
            {
                return mlirGen(loc(expressionAST), THIS_ALIAS, genContext);
            }

            return mlirGen(loc(expressionAST), THIS_NAME, genContext);
        }
        else if (kind == SyntaxKind::SuperKeyword)
        {
            return mlirGen(loc(expressionAST), SUPER_NAME, genContext);
        }
        else if (kind == SyntaxKind::VoidExpression)
        {
            return mlirGen(expressionAST.as<VoidExpression>(), genContext);
        }
        else if (kind == SyntaxKind::YieldExpression)
        {
            return mlirGen(expressionAST.as<YieldExpression>(), genContext);
        }
        else if (kind == SyntaxKind::AwaitExpression)
        {
            return mlirGen(expressionAST.as<AwaitExpression>(), genContext);
        }
        else if (kind == SyntaxKind::NonNullExpression)
        {
            return mlirGen(expressionAST.as<NonNullExpression>(), genContext);
        }
        else if (kind == SyntaxKind::ClassExpression)
        {
            return mlirGen(expressionAST.as<ClassExpression>(), genContext);
        }
        else if (kind == SyntaxKind::OmittedExpression)
        {
            return mlirGen(expressionAST.as<OmittedExpression>(), genContext);
        }
        else if (kind == SyntaxKind::Unknown /*TODO: temp solution to treat null expr as empty expr*/)
        {
            return mlir::success();
        }

        llvm_unreachable("unknown expression");
    }

    void inferType(mlir::Location location, mlir::Type templateType, mlir::Type concreteType, StringMap<mlir::Type> &results, const GenContext &genContext)
    {
        auto currentTemplateType = templateType;
        auto currentType = concreteType;

        LLVM_DEBUG(llvm::dbgs() << "\n!! inferring \n\ttemplate type: " << templateType << ", \n\ttype: " << concreteType
                                << "\n";);

        if (!currentTemplateType || !currentType)
        {
            // nothing todo here
            return;
        }                                

        if (currentTemplateType == currentType)
        {
            // nothing todo here
            return;
        }

        if (auto namedGenType = currentTemplateType.dyn_cast<mlir_ts::NamedGenericType>())
        {
            // merge if exists

            auto name = namedGenType.getName().getValue();
            auto existType = results.lookup(name);
            if (existType)
            {
                auto merged = false;
                currentType = mth.mergeType(existType, currentType, merged);

                LLVM_DEBUG(llvm::dbgs() << "\n!! result type: " << currentType << "\n";);
                results[name] = currentType;
            }
            else
            {
                // TODO: when u use literal type to validate extends u need to use original type
                // currentType = mth.wideStorageType(currentType);
                LLVM_DEBUG(llvm::dbgs() << "\n!! type: " << name << " = " << currentType << "\n";);
                results.insert({name, currentType});
            }

            assert(results.lookup(name) == currentType);

            return;
        }

        // class -> class
        if (auto tempClass = currentTemplateType.dyn_cast<mlir_ts::ClassType>())
        {
            if (auto typeClass = concreteType.dyn_cast<mlir_ts::ClassType>())
            {
                auto typeClassInfo = getClassInfoByFullName(typeClass.getName().getValue());
                if (auto tempClassInfo = getClassInfoByFullName(tempClass.getName().getValue()))
                {
                    for (auto &templateParam : tempClassInfo->typeParamsWithArgs)
                    {
                        auto name = templateParam.getValue().first->getName();
                        auto found = typeClassInfo->typeParamsWithArgs.find(name);
                        if (found != typeClassInfo->typeParamsWithArgs.end())
                        {
                            // TODO: convert GenericType -> AnyGenericType,  and NamedGenericType -> GenericType, and
                            // add 2 type Parameters to it Constrain, Default
                            currentTemplateType = templateParam.getValue().second;
                            currentType = found->getValue().second;

                            inferType(location, currentTemplateType, currentType, results, genContext);
                        }
                    }

                    return;
                }
                else if (auto tempGenericClassInfo = getGenericClassInfoByFullName(tempClass.getName().getValue()))
                {
                    for (auto &templateParam : tempGenericClassInfo->typeParams)
                    {
                        auto name = templateParam->getName();
                        auto found = typeClassInfo->typeParamsWithArgs.find(name);
                        if (found != typeClassInfo->typeParamsWithArgs.end())
                        {
                            currentTemplateType = getNamedGenericType(found->getValue().first->getName());
                            currentType = found->getValue().second;

                            inferType(location, currentTemplateType, currentType, results, genContext);
                        }
                    }

                    return;
                }
            }
        }

        // interface -> interface
        if (auto tempInterface = currentTemplateType.dyn_cast<mlir_ts::InterfaceType>())
        {
            if (auto typeInterface = concreteType.dyn_cast<mlir_ts::InterfaceType>())
            {
                auto typeInterfaceInfo = getInterfaceInfoByFullName(typeInterface.getName().getValue());
                if (auto tempInterfaceInfo = getInterfaceInfoByFullName(tempInterface.getName().getValue()))
                {
                    for (auto &templateParam : tempInterfaceInfo->typeParamsWithArgs)
                    {
                        auto name = templateParam.getValue().first->getName();
                        auto found = typeInterfaceInfo->typeParamsWithArgs.find(name);
                        if (found != typeInterfaceInfo->typeParamsWithArgs.end())
                        {
                            // TODO: convert GenericType -> AnyGenericType,  and NamedGenericType -> GenericType, and
                            // add 2 type Parameters to it Constrain, Default
                            currentTemplateType = templateParam.getValue().second;
                            currentType = found->getValue().second;

                            inferType(location, currentTemplateType, currentType, results, genContext);
                        }
                    }

                    return;
                }
                else if (auto tempGenericInterfaceInfo = getGenericInterfaceInfoByFullName(tempInterface.getName().getValue()))
                {
                    for (auto &templateParam : tempGenericInterfaceInfo->typeParams)
                    {
                        auto name = templateParam->getName();
                        auto found = typeInterfaceInfo->typeParamsWithArgs.find(name);
                        if (found != typeInterfaceInfo->typeParamsWithArgs.end())
                        {
                            currentTemplateType = getNamedGenericType(found->getValue().first->getName());
                            currentType = found->getValue().second;

                            inferType(location, currentTemplateType, currentType, results, genContext);
                        }
                    }

                    return;
                }
            }
        }

        // array -> array
        if (auto tempArray = currentTemplateType.dyn_cast<mlir_ts::ArrayType>())
        {
            if (auto typeArray = concreteType.dyn_cast<mlir_ts::ArrayType>())
            {
                currentTemplateType = tempArray.getElementType();
                currentType = typeArray.getElementType();
                inferType(location, currentTemplateType, currentType, results, genContext);
                return;
            }

            if (auto typeArray = concreteType.dyn_cast<mlir_ts::ConstArrayType>())
            {
                currentTemplateType = tempArray.getElementType();
                currentType = typeArray.getElementType();
                inferType(location, currentTemplateType, currentType, results, genContext);
                return;
            }
        }

        // TODO: finish it
        // tuple -> tuple
        if (auto tempTuple = currentTemplateType.dyn_cast<mlir_ts::TupleType>())
        {
            if (auto typeTuple = concreteType.dyn_cast<mlir_ts::TupleType>())
            {
                for (auto tempFieldInfo : tempTuple.getFields())
                {
                    currentTemplateType = tempFieldInfo.type;
                    auto index = typeTuple.getIndex(tempFieldInfo.id);
                    if (index >= 0)
                    {
                        currentType = typeTuple.getFieldInfo(index).type;
                        inferType(location, currentTemplateType, currentType, results, genContext);
                    }
                    else
                    {
                        return;
                    }
                }

                return;
            }

            if (auto typeTuple = concreteType.dyn_cast<mlir_ts::ConstTupleType>())
            {
                for (auto tempFieldInfo : tempTuple.getFields())
                {
                    currentTemplateType = tempFieldInfo.type;
                    auto index = typeTuple.getIndex(tempFieldInfo.id);
                    if (index >= 0)
                    {
                        currentType = typeTuple.getFieldInfo(index).type;
                        inferType(location, currentTemplateType, currentType, results, genContext);
                    }
                    else
                    {
                        return;
                    }
                }

                return;
            }
        }        

        // optional -> optional
        if (auto tempOpt = currentTemplateType.dyn_cast<mlir_ts::OptionalType>())
        {
            if (auto typeOpt = concreteType.dyn_cast<mlir_ts::OptionalType>())
            {
                currentTemplateType = tempOpt.getElementType();
                currentType = typeOpt.getElementType();
                inferType(location, currentTemplateType, currentType, results, genContext);
                return;
            }

            // optional -> value
            currentTemplateType = tempOpt.getElementType();
            currentType = concreteType;
            inferType(location, currentTemplateType, currentType, results, genContext);
            return;
        }

        // lambda -> lambda
        if (mth.isAnyFunctionType(currentTemplateType) && mth.isAnyFunctionType(concreteType))
        {
            auto tempfuncType = mth.getParamsFromFuncRef(currentTemplateType);
            if (tempfuncType.size() > 0)
            {
                auto funcType = mth.getParamsFromFuncRef(concreteType);
                if (funcType.size() > 0)
                {
                    inferTypeFuncType(location, tempfuncType, funcType, results, genContext);

                    // lambda(return) -> lambda(return)
                    auto tempfuncRetType = mth.getReturnsFromFuncRef(currentTemplateType);
                    if (tempfuncRetType.size() > 0)
                    {
                        auto funcRetType = mth.getReturnsFromFuncRef(concreteType);
                        if (funcRetType.size() > 0)
                        {
                            inferTypeFuncType(location, tempfuncRetType, funcRetType, results, genContext);
                        }
                    }

                    return;
                }
            }
        }

        // union -> union
        if (auto tempUnionType = currentTemplateType.dyn_cast<mlir_ts::UnionType>())
        {
            if (auto typeUnionType = concreteType.dyn_cast<mlir_ts::UnionType>())
            {
                auto types = typeUnionType.getTypes();
                if (types.size() != tempUnionType.getTypes().size())
                {
                    return;
                }

                auto index = -1;
                for (auto tempSubType : tempUnionType.getTypes())
                {
                    index++;
                    auto typeSubType = types[index];

                    currentTemplateType = tempSubType;
                    currentType = typeSubType;
                    inferType(location, currentTemplateType, currentType, results, genContext);
                }

                return;
            }
            else 
            {
                // TODO: review how to call functions such as: "function* Map<T, R>(a: T[] | Iterable<T>, f: (i: T) => R) { ... }"
                // special case when UnionType is used in generic method
                for (auto tempSubType : tempUnionType.getTypes())
                {
                    currentTemplateType = tempSubType;
                    currentType = concreteType;

                    auto count = results.size();
                    inferType(location, currentTemplateType, currentType, results, genContext);
                    if (count < results.size())
                    {
                        return;
                    }
                }

                return;
            }
        }

        // conditional type
        if (auto templateCondType = currentTemplateType.dyn_cast<mlir_ts::ConditionalType>())
        {
            currentTemplateType = templateCondType.getTrueType();
            inferType(location, currentTemplateType, currentType, results, genContext);
            currentTemplateType = templateCondType.getFalseType();
            inferType(location, currentTemplateType, currentType, results, genContext);
        }

        // typeref -> type
        if (auto tempTypeRefType = currentTemplateType.dyn_cast<mlir_ts::TypeReferenceType>())
        {
            currentTemplateType = getTypeByTypeReference(location, tempTypeRefType, genContext);
            inferType(location, currentTemplateType, currentType, results, genContext);
        }
    }

    void inferTypeFuncType(mlir::Location location, mlir::ArrayRef<mlir::Type> tempfuncType, mlir::ArrayRef<mlir::Type> funcType,
                           StringMap<mlir::Type> &results, const GenContext &genContext)
    {
        if (tempfuncType.size() != funcType.size())
        {
            return;
        }

        for (auto paramIndex = 0; paramIndex < tempfuncType.size(); paramIndex++)
        {
            auto currentTemplateType = tempfuncType[paramIndex];
            auto currentType = funcType[paramIndex];
            inferType(location, currentTemplateType, currentType, results, genContext);
        }
    }

    bool isDelayedInstantiationForSpeecializedArrowFunctionReference(mlir::Value arrowFunctionRefValue)
    {
        auto currValue = arrowFunctionRefValue;
        if (auto createBoundFunctionOp = currValue.getDefiningOp<mlir_ts::CreateBoundFunctionOp>())
        {
            currValue = createBoundFunctionOp.getFunc();
        }

        if (auto symbolOp = currValue.getDefiningOp<mlir_ts::SymbolRefOp>())
        {
            return symbolOp->hasAttrOfType<mlir::BoolAttr>(GENERIC_ATTR_NAME);
        }

        return false;
    }

    mlir::Type instantiateSpecializedFunctionTypeHelper(mlir::Location location, mlir::Value functionRefValue,
                                                        mlir::Type recieverType, bool discoverReturnType,
                                                        const GenContext &genContext)
    {
        auto currValue = functionRefValue;
        if (auto createBoundFunctionOp = currValue.getDefiningOp<mlir_ts::CreateBoundFunctionOp>())
        {
            currValue = createBoundFunctionOp.getFunc();
        }

        if (auto symbolOp = currValue.getDefiningOp<mlir_ts::SymbolRefOp>())
        {
            auto functionName = symbolOp.getIdentifier();

            // it is not generic arrow function
            auto functionGenericTypeInfo = getGenericFunctionInfoByFullName(functionName);

            MLIRNamespaceGuard nsGuard(currentNamespace);
            currentNamespace = functionGenericTypeInfo->elementNamespace;

            return instantiateSpecializedFunctionTypeHelper(location, functionGenericTypeInfo->functionDeclaration,
                                                            recieverType, discoverReturnType, genContext);
        }

        llvm_unreachable("not implemented");
    }

    mlir::Type instantiateSpecializedFunctionTypeHelper(mlir::Location location, FunctionLikeDeclarationBase funcDecl,
                                                        mlir::Type recieverType, bool discoverReturnType,
                                                        const GenContext &genContext)
    {
        GenContext funcGenContext(genContext);
        funcGenContext.receiverFuncType = recieverType;

        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(theModule.getBody());

        auto [result, funcOp] = getFuncArgTypesOfGenericMethod(funcDecl, {}, discoverReturnType, funcGenContext);
        if (mlir::failed(result))
        {
            if (!genContext.dummyRun)
            {
                emitError(location) << "can't instantiate specialized arrow function.";
            }

            return mlir::Type();
        }

        return funcOp->getFuncType();
    }

    mlir::LogicalResult instantiateSpecializedArrowFunctionHelper(mlir::Location location,
                                                                  mlir::Value arrowFunctionRefValue,
                                                                  mlir::Type recieverType, const GenContext &genContext)
    {
        auto currValue = arrowFunctionRefValue;
        auto createBoundFunctionOp = currValue.getDefiningOp<mlir_ts::CreateBoundFunctionOp>();
        if (createBoundFunctionOp)
        {
            currValue = createBoundFunctionOp.getFunc();
        }

        auto symbolOp = currValue.getDefiningOp<mlir_ts::SymbolRefOp>();
        assert(symbolOp);
        auto arrowFunctionName = symbolOp.getIdentifier();

        // it is not generic arrow function
        auto arrowFunctionGenericTypeInfo = getGenericFunctionInfoByFullName(arrowFunctionName);

        GenContext arrowFuncGenContext(genContext);
        arrowFuncGenContext.receiverFuncType = recieverType;

        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(theModule.getBody());

            MLIRNamespaceGuard nsGuard(currentNamespace);
            currentNamespace = arrowFunctionGenericTypeInfo->elementNamespace;

            auto [result, arrowFuncOp, arrowFuncName, isGeneric] =
                mlirGenFunctionLikeDeclaration(arrowFunctionGenericTypeInfo->functionDeclaration, arrowFuncGenContext);
            if (mlir::failed(result))
            {
                emitError(location) << "can't instantiate specialized arrow function.";
                return mlir::failure();
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! fixing arrow func: " << arrowFuncName << " type: ["
                                    << arrowFuncOp.getFunctionType() << "\n";);

            // fix symbolref
            currValue.setType(arrowFuncOp.getFunctionType());

            if (createBoundFunctionOp)
            {
                // fix create bound if any
                TypeSwitch<mlir::Type>(createBoundFunctionOp.getType())
                    .template Case<mlir_ts::BoundFunctionType>([&](auto boundFunc) {
                        arrowFunctionRefValue.setType(getBoundFunctionType(arrowFuncOp.getFunctionType()));
                    })
                    .template Case<mlir_ts::HybridFunctionType>([&](auto hybridFuncType) {
                        arrowFunctionRefValue.setType(
                            mlir_ts::HybridFunctionType::get(builder.getContext(), arrowFuncOp.getFunctionType()));
                    })
                    .Default([&](auto type) { llvm_unreachable("not implemented"); });
            }

            symbolOp->removeAttr(GENERIC_ATTR_NAME);
        }

        return mlir::success();
    }

    mlir::LogicalResult appendInferredTypes(mlir::Location location,
                                            llvm::SmallVector<TypeParameterDOM::TypePtr> &typeParams,
                                            StringMap<mlir::Type> &inferredTypes, IsGeneric &anyNamedGenericType,
                                            GenContext &genericTypeGenContext,
                                            bool arrayMerge = false)
    {
        for (auto &pair : inferredTypes)
        {
            // find typeParam
            auto typeParamName = pair.getKey();
            auto inferredType = pair.getValue();
            auto found = std::find_if(typeParams.begin(), typeParams.end(),
                                      [&](auto &paramItem) { return paramItem->getName() == typeParamName; });
            if (found == typeParams.end())
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! can't find : " << typeParamName << " in type params: " << "\n";);
                LLVM_DEBUG(for (auto typeParam : typeParams) llvm::dbgs() << "\t!! type param: " << typeParam->getName() << "\n";);

                //return mlir::failure();
                // just ignore it
                continue;
            }

            auto typeParam = (*found);

            auto [result, hasAnyNamedGenericType] =
                zipTypeParameterWithArgument(location, genericTypeGenContext.typeParamsWithArgs, typeParam,
                                             inferredType, false, genericTypeGenContext, true, arrayMerge);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            if (hasAnyNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasAnyNamedGenericType;
            }
        }

        return mlir::success();
    }

    std::pair<mlir::LogicalResult, bool> resolveGenericParamFromFunctionCall(mlir::Location location, mlir::Type paramType, mlir::Value argOp, int paramIndex,
        GenericFunctionInfo::TypePtr functionGenericTypeInfo, IsGeneric &anyNamedGenericType,  GenContext &genericTypeGenContext)
    {
        if (paramType == argOp.getType())
        {
            return {mlir::success(), true};
        }

        StringMap<mlir::Type> inferredTypes;
        inferType(location, paramType, argOp.getType(), inferredTypes, genericTypeGenContext);
        if (mlir::failed(appendInferredTypes(location, functionGenericTypeInfo->typeParams, inferredTypes, anyNamedGenericType,
                                                genericTypeGenContext)))
        {
            return {mlir::failure(), true};
        }

        if (isDelayedInstantiationForSpeecializedArrowFunctionReference(argOp))
        {
            GenContext typeGenContext(genericTypeGenContext);
            typeGenContext.dummyRun = true;
            auto recreatedFuncType = instantiateSpecializedFunctionTypeHelper(
                location, functionGenericTypeInfo->functionDeclaration, mlir::Type(), false,
                typeGenContext);
            if (!recreatedFuncType)
            {
                // next param
                return {mlir::failure(), true};
            }

            LLVM_DEBUG(llvm::dbgs()
                            << "\n!! instantiate specialized  type function: '"
                            << functionGenericTypeInfo->name << "' type: " << recreatedFuncType << "\n";);

            auto recreatedParamType = mth.getParamFromFuncRef(recreatedFuncType, paramIndex);

            LLVM_DEBUG(llvm::dbgs()
                            << "\n!! param type for arrow func[" << paramIndex << "]: " << recreatedParamType << "\n";);

            auto newArrowFuncType = instantiateSpecializedFunctionTypeHelper(location, argOp, recreatedParamType,
                                                                                true, genericTypeGenContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! instantiate specialized arrow type function: "
                                    << newArrowFuncType << "\n";);

            if (!newArrowFuncType)
            {
                return {mlir::failure(), false};
            }

            // infer second type when ArrowType is fully built
            StringMap<mlir::Type> inferredTypes;
            inferType(location, paramType, newArrowFuncType, inferredTypes, genericTypeGenContext);
            if (mlir::failed(appendInferredTypes(location, functionGenericTypeInfo->typeParams, inferredTypes, anyNamedGenericType,
                                                    genericTypeGenContext)))
            {
                return {mlir::failure(), false};
            }
        }

        return {mlir::success(), true};
    }

    mlir::LogicalResult resolveGenericParamsFromFunctionCall(mlir::Location location,
                                                             GenericFunctionInfo::TypePtr functionGenericTypeInfo,
                                                             NodeArray<TypeNode> typeArguments,
                                                             bool skipThisParam,
                                                             IsGeneric &anyNamedGenericType,
                                                             GenContext &genericTypeGenContext)
    {
        // add provided type arguments, ignoring defaults
        auto typeParams = functionGenericTypeInfo->typeParams;
        if (typeArguments)
        {
            auto [result, hasAnyNamedGenericType] = zipTypeParametersWithArgumentsNoDefaults(
                location, typeParams, typeArguments, genericTypeGenContext.typeParamsWithArgs, genericTypeGenContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            if (hasAnyNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasAnyNamedGenericType;
            }
        }

        // TODO: investigate, in [...].reduce, lambda function does not have funcOp, why?
        auto funcOp = functionGenericTypeInfo->funcOp;
        assert(funcOp);
        if (funcOp)
        {
            // TODO: we have func params.
            for (auto paramInfo : funcOp->getParams())
            {
                paramInfo->processed = false;
            }

            auto callOpsCount = genericTypeGenContext.callOperands.size();
            auto totalProcessed = 0;
            do
            {
                auto paramIndex = -1;
                auto processed = 0;
                auto skipCount = skipThisParam ? 1 : 0;
                for (auto paramInfo : funcOp->getParams())
                {
                    if (skipCount-- > 0)
                    {
                        processed++;
                        continue;
                    }

                    paramIndex++;
                    if (paramInfo->processed)
                    {
                        continue;
                    }

                    auto paramType = paramInfo->getType();

                    if (callOpsCount <= paramIndex)
                    {
                        // there is no more ops
                        if (paramInfo->getIsOptional() || paramType.isa<mlir_ts::OptionalType>())
                        {
                            processed++;
                            continue;
                        }

                        break;
                    }

                    auto argOp = genericTypeGenContext.callOperands[paramIndex];

                    LLVM_DEBUG(llvm::dbgs()
                        << "\n!! resolving param for generic function: '"
                        << functionGenericTypeInfo->name << "'\n\t parameter #" << paramIndex << " type: [ " << paramType << " ] \n\t argument type: [ " << argOp.getType() << " ]\n";);

                    if (!paramInfo->getIsMultiArgsParam())
                    {
                        auto [result, cont] = resolveGenericParamFromFunctionCall(
                            location, paramType, argOp, paramIndex, functionGenericTypeInfo, anyNamedGenericType, genericTypeGenContext);
                        if (mlir::succeeded(result))
                        {
                            paramInfo->processed = true;
                            processed++;
                        }
                        else if (!cont)
                        {
                            return mlir::failure();
                        }
                    }
                    else
                    {
                        auto anyFailed = false;
                        struct ArrayInfo arrayInfo{};
                        for (auto varArgIndex = paramIndex; varArgIndex < callOpsCount; varArgIndex++)
                        {
                            auto argOp = genericTypeGenContext.callOperands[varArgIndex];

                            accumulateArrayItemType(argOp.getType(), arrayInfo);                            
                        }

                        mlir::Type arrayType = getArrayType(arrayInfo.accumulatedArrayElementType);

                        StringMap<mlir::Type> inferredTypes;
                        inferType(location, paramType, arrayType, inferredTypes, genericTypeGenContext);
                        if (mlir::failed(appendInferredTypes(location, functionGenericTypeInfo->typeParams, inferredTypes, anyNamedGenericType,
                                                                genericTypeGenContext, true)))
                        {
                            return mlir::failure();
                        }                        

                        if (!anyFailed)
                        {
                            paramInfo->processed = true;
                            processed++;
                        }
                    }
                }

                if (processed == 0)
                {
                    emitError(location) << "not all types could be inferred";
                    return mlir::failure();
                }

                totalProcessed += processed;

                if (totalProcessed == funcOp->getParams().size())
                {
                    break;
                }
            } while (true);
        }

        // add default params if not provided
        auto [resultDefArg, hasNamedGenericType] = zipTypeParametersWithDefaultArguments(
            location, typeParams, typeArguments, genericTypeGenContext.typeParamsWithArgs, genericTypeGenContext);
        if (mlir::failed(resultDefArg))
        {
            return mlir::failure();
        }

        if (hasNamedGenericType == IsGeneric::True)
        {
            anyNamedGenericType = hasNamedGenericType;
        }

        // TODO: check if all typeParams are there
        if (genericTypeGenContext.typeParamsWithArgs.size() < typeParams.size())
        {
            // no resolve needed, this type without param
            emitError(location) << "not all types could be inferred";
            return mlir::failure();
        }

        return mlir::success();
    }

    std::tuple<mlir::LogicalResult, mlir_ts::FunctionType, std::string> instantiateSpecializedFunctionType(
        mlir::Location location, StringRef name, NodeArray<TypeNode> typeArguments, bool skipThisParam, const GenContext &genContext)
    {
        auto functionGenericTypeInfo = getGenericFunctionInfoByFullName(name);
        if (functionGenericTypeInfo)
        {
            if (functionGenericTypeInfo->functionDeclaration == SyntaxKind::ArrowFunction 
                || functionGenericTypeInfo->functionDeclaration == SyntaxKind::FunctionExpression)
            {
                // we need to avoid wrong redeclaration of arrow functions (when thisType is provided it will add THIS parameter as first)
                const_cast<GenContext &>(genContext).thisType = nullptr;
            }

            MLIRNamespaceGuard ng(currentNamespace);
            currentNamespace = functionGenericTypeInfo->elementNamespace;

            auto anyNamedGenericType = IsGeneric::False;

            // step 1, add type arguments first
            GenContext genericTypeGenContext(genContext);
            auto typeParams = functionGenericTypeInfo->typeParams;
            if (typeArguments && typeParams.size() == typeArguments.size())
            {
                // create typeParamsWithArgs from typeArguments
                auto [result, hasAnyNamedGenericType] = zipTypeParametersWithArguments(
                    location, typeParams, typeArguments, genericTypeGenContext.typeParamsWithArgs, genContext);
                if (mlir::failed(result))
                {
                    return {mlir::failure(), mlir_ts::FunctionType(), ""};
                }

                if (hasAnyNamedGenericType == IsGeneric::True)
                {
                    anyNamedGenericType = hasAnyNamedGenericType;
                }
            }
            else if (genericTypeGenContext.callOperands.size() > 0 ||
                     functionGenericTypeInfo->functionDeclaration->parameters.size() > 0)
            {
                auto result =
                    resolveGenericParamsFromFunctionCall(location, functionGenericTypeInfo, typeArguments,
                                                         skipThisParam, anyNamedGenericType, genericTypeGenContext);
                if (mlir::failed(result))
                {
                    return {mlir::failure(), mlir_ts::FunctionType(), ""};
                }
            }
            else
            {
                llvm_unreachable("not implemented");
            }

            // we need to wide all types when initializing function

            for (auto &typeParam : genericTypeGenContext.typeParamsWithArgs)
            {
                auto name = std::get<0>(typeParam.getValue())->getName();
                auto type = std::get<1>(typeParam.getValue());
                auto widenType = mth.wideStorageType(type);
                genericTypeGenContext.typeParamsWithArgs[name] =
                    std::make_pair(std::get<0>(typeParam.getValue()), widenType);
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! instantiate specialized function: " << functionGenericTypeInfo->name
                                    << " ";
                       for (auto &typeParam
                            : genericTypeGenContext.typeParamsWithArgs) llvm::dbgs()
                       << " param: " << std::get<0>(typeParam.getValue())->getName()
                       << " type: " << std::get<1>(typeParam.getValue());
                       llvm::dbgs() << "\n";);

            LLVM_DEBUG(llvm::dbgs() << "\n!! type alias: ";
                       for (auto &typeAlias
                            : genericTypeGenContext.typeAliasMap) llvm::dbgs()
                       << " name: " << typeAlias.getKey() << " type: " << typeAlias.getValue();
                       llvm::dbgs() << "\n";);

            // revalidate all types
            if (anyNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = IsGeneric::False;
                for (auto &typeParamWithArg : genericTypeGenContext.typeParamsWithArgs)
                {
                    if (mth.isGenericType(std::get<1>(typeParamWithArg.second)))
                    {
                        anyNamedGenericType = IsGeneric::True;
                    }
                }
            }

            if (anyNamedGenericType == IsGeneric::False)
            {
                if (functionGenericTypeInfo->processing)
                {
                    auto [fullName, name] =
                        getNameOfFunction(functionGenericTypeInfo->functionDeclaration, genericTypeGenContext);

                    auto funcType = lookupFunctionTypeMap(fullName);
                    if (funcType)
                    {
                        return {mlir::success(), funcType, fullName};
                    }

                    return {mlir::failure(), mlir_ts::FunctionType(), ""};
                }

                // create new instance of function with TypeArguments
                functionGenericTypeInfo->processing = true;
                auto [result, funcOp, funcName, isGeneric] =
                    mlirGenFunctionLikeDeclaration(functionGenericTypeInfo->functionDeclaration, genericTypeGenContext);
                functionGenericTypeInfo->processing = false;
                if (mlir::failed(result))
                {
                    return {mlir::failure(), mlir_ts::FunctionType(), ""};
                }

                functionGenericTypeInfo->processed = true;

                // instatiate all ArrowFunctions which are not yet instantiated
                auto opIndex = -1;
                for (auto op : genContext.callOperands)
                {
                    opIndex++;
                    if (isDelayedInstantiationForSpeecializedArrowFunctionReference(op))
                    {
                        LLVM_DEBUG(llvm::dbgs() << "\n!! delayed arrow func instantiation for func type: "
                                                << funcOp.getFunctionType() << "\n";);
                        auto result = instantiateSpecializedArrowFunctionHelper(
                            location, op, funcOp.getFunctionType().getInput(opIndex), genContext);
                        if (mlir::failed(result))
                        {
                            return {mlir::failure(), mlir_ts::FunctionType(), ""};
                        }
                    }
                }

                return {mlir::success(), funcOp.getFunctionType(), funcOp.getName().str()};
            }

            emitError(location) << "can't instantiate specialized function [" << name << "].";
            return {mlir::failure(), mlir_ts::FunctionType(), ""};
        }

        emitError(location) << "can't find generic [" << name << "] function.";
        return {mlir::failure(), mlir_ts::FunctionType(), ""};
    }

    std::pair<mlir::LogicalResult, FunctionPrototypeDOM::TypePtr> getFuncArgTypesOfGenericMethod(
        FunctionLikeDeclarationBase functionLikeDeclarationAST, ArrayRef<TypeParameterDOM::TypePtr> typeParams,
        bool discoverReturnType, const GenContext &genContext)
    {
        GenContext funcGenContext(genContext);
        funcGenContext.discoverParamsOnly = !discoverReturnType;

        // we need to map generic parameters to generic types to be able to resolve function parameters which
        // are not generic
        for (auto typeParam : typeParams)
        {
            funcGenContext.typeAliasMap.insert({typeParam->getName(), getNamedGenericType(typeParam->getName())});
        }

        auto [funcOp, funcProto, result, isGenericType] =
            mlirGenFunctionPrototype(functionLikeDeclarationAST, funcGenContext);
        if (mlir::failed(result) || !funcOp)
        {
            return {mlir::failure(), {}};
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! func name: " << funcProto->getName()
                                << ", Op type (resolving from operands): " << funcOp.getFunctionType() << "\n";);

        LLVM_DEBUG(llvm::dbgs() << "\n!! func args: "; auto index = 0; for (auto paramInfo
                                                                            : funcProto->getParams()) {
            llvm::dbgs() << "\n_ " << paramInfo->getName() << ": " << paramInfo->getType() << " = (" << index << ") ";
            if (genContext.callOperands.size() > index)
                llvm::dbgs() << genContext.callOperands[index];
            llvm::dbgs() << "\n";
            index++;
        });

        return {mlir::success(), funcProto};
    }

    std::pair<mlir::LogicalResult, mlir::Type> instantiateSpecializedClassType(mlir::Location location,
                                                                               mlir_ts::ClassType genericClassType,
                                                                               NodeArray<TypeNode> typeArguments,
                                                                               const GenContext &genContext,
                                                                               bool allowNamedGenerics = false)
    {
        auto fullNameGenericClassTypeName = genericClassType.getName().getValue();
        auto genericClassInfo = getGenericClassInfoByFullName(fullNameGenericClassTypeName);
        if (genericClassInfo)
        {
            MLIRNamespaceGuard ng(currentNamespace);
            currentNamespace = genericClassInfo->elementNamespace;

            GenContext genericTypeGenContext(genContext);
            auto typeParams = genericClassInfo->typeParams;
            auto [result, hasAnyNamedGenericType] = zipTypeParametersWithArguments(
                location, typeParams, typeArguments, genericTypeGenContext.typeParamsWithArgs, genContext);
            if (mlir::failed(result) || (hasAnyNamedGenericType == IsGeneric::True && !allowNamedGenerics))
            {
                return {mlir::failure(), mlir::Type()};
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! instantiate specialized class: " << fullNameGenericClassTypeName << " ";
                       for (auto &typeParam
                            : genericTypeGenContext.typeParamsWithArgs) llvm::dbgs()
                       << " param: " << std::get<0>(typeParam.getValue())->getName()
                       << " type: " << std::get<1>(typeParam.getValue());
                       llvm::dbgs() << "\n";);

            LLVM_DEBUG(llvm::dbgs() << "\n!! type alias: ";
                       for (auto &typeAlias
                            : genericTypeGenContext.typeAliasMap) llvm::dbgs()
                       << " name: " << typeAlias.getKey() << " type: " << typeAlias.getValue();
                       llvm::dbgs() << "\n";);

            // create new instance of interface with TypeArguments
            if (mlir::failed(std::get<0>(mlirGen(genericClassInfo->classDeclaration, genericTypeGenContext))))
            {
                return {mlir::failure(), mlir::Type()};
            }

            // get instance of generic interface type
            auto specType = getSpecializationClassType(genericClassInfo, genericTypeGenContext);
            return {mlir::success(), specType};
        }

        // can't find generic instance
        return {mlir::success(), mlir::Type()};
    }

    std::pair<mlir::LogicalResult, mlir::Type> instantiateSpecializedInterfaceType(
        mlir::Location location, mlir_ts::InterfaceType genericInterfaceType, NodeArray<TypeNode> typeArguments,
        const GenContext &genContext, bool allowNamedGenerics = false)
    {
        auto fullNameGenericInterfaceTypeName = genericInterfaceType.getName().getValue();
        auto genericInterfaceInfo = getGenericInterfaceInfoByFullName(fullNameGenericInterfaceTypeName);
        if (genericInterfaceInfo)
        {
            MLIRNamespaceGuard ng(currentNamespace);
            currentNamespace = genericInterfaceInfo->elementNamespace;

            GenContext genericTypeGenContext(genContext);
            auto typeParams = genericInterfaceInfo->typeParams;
            auto [result, hasAnyNamedGenericType] = zipTypeParametersWithArguments(
                location, typeParams, typeArguments, genericTypeGenContext.typeParamsWithArgs, genContext);
            if (mlir::failed(result) || (hasAnyNamedGenericType == IsGeneric::True && !allowNamedGenerics))
            {
                return {mlir::failure(), mlir::Type()};
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! instantiate specialized interface: " << fullNameGenericInterfaceTypeName
                                    << " ";
                       for (auto &typeParam
                            : genericTypeGenContext.typeParamsWithArgs) llvm::dbgs()
                       << " param: " << std::get<0>(typeParam.getValue())->getName()
                       << " type: " << std::get<1>(typeParam.getValue());
                       llvm::dbgs() << "\n";);

            LLVM_DEBUG(llvm::dbgs() << "\n!! type alias: ";
                       for (auto &typeAlias
                            : genericTypeGenContext.typeAliasMap) llvm::dbgs()
                       << " name: " << typeAlias.getKey() << " type: " << typeAlias.getValue();
                       llvm::dbgs() << "\n";);

            // create new instance of interface with TypeArguments
            if (mlir::failed(mlirGen(genericInterfaceInfo->interfaceDeclaration, genericTypeGenContext)))
            {
                // return mlir::Type();
                // type can't be resolved, so return generic base type
                //return {mlir::success(), genericInterfaceInfo->interfaceType};
                return {mlir::failure(), mlir::Type()};
            }

            // get instance of generic interface type
            auto specType = getSpecializationInterfaceType(genericInterfaceInfo, genericTypeGenContext);
            return {mlir::success(), specType};
        }

        // can't find generic instance
        return {mlir::success(), mlir::Type()};
    }

    ValueOrLogicalResult mlirGenSpecialized(mlir::Location location, mlir::Value genResult,
                                            NodeArray<TypeNode> typeArguments, const GenContext &genContext)
    {
        // in case it is generic arrow function
        auto currValue = genResult;

        // in case of this.generic_func<T>();
        if (auto extensFuncRef = currValue.getDefiningOp<mlir_ts::CreateExtensionFunctionOp>())
        {
            currValue = extensFuncRef.getFunc();

            SmallVector<mlir::Value, 4> operands;
            operands.push_back(extensFuncRef.getThisVal());
            operands.append(genContext.callOperands.begin(), genContext.callOperands.end());

            GenContext specGenContext(genContext);
            specGenContext.callOperands = operands;

            auto newFuncRefOrLogicResult = mlirGenSpecialized(location, currValue, typeArguments, specGenContext);
            EXIT_IF_FAILED(newFuncRefOrLogicResult)
            if (newFuncRefOrLogicResult && currValue != newFuncRefOrLogicResult)
            {
                mlir::Value newFuncRefValue = newFuncRefOrLogicResult;

                // special case to work with interfaces
                // TODO: finish it, bug
                auto thisRef = extensFuncRef.getThisVal();
                auto funcType = newFuncRefValue.getType().cast<mlir_ts::FunctionType>();

                mlir::Value newExtensionFuncVal = builder.create<mlir_ts::CreateExtensionFunctionOp>(
                                location, getExtensionFunctionType(funcType), thisRef, newFuncRefValue);

                extensFuncRef.erase();

                return newExtensionFuncVal;
            }
            else
            {
                return genResult;
            }
        }

        if (currValue.getDefiningOp()->hasAttrOfType<mlir::BoolAttr>(GENERIC_ATTR_NAME))
        {
            // create new function instance
            GenContext initSpecGenContext(genContext);
            initSpecGenContext.forceDiscover = true;
            initSpecGenContext.thisType = mlir::Type();

            auto skipThisParam = false;
            mlir::Value thisValue;
            StringRef funcName;
            if (auto symbolOp = currValue.getDefiningOp<mlir_ts::SymbolRefOp>())
            {
                funcName = symbolOp.getIdentifierAttr().getValue();
            }
            else if (auto thisSymbolOp = currValue.getDefiningOp<mlir_ts::ThisSymbolRefOp>())
            {
                funcName = thisSymbolOp.getIdentifierAttr().getValue();
                skipThisParam = true;
                thisValue = thisSymbolOp.getThisVal();
                initSpecGenContext.thisType = thisValue.getType();
            }
            else
            {
                llvm_unreachable("not implemented");
            }

            auto [result, funcType, funcSymbolName] =
                instantiateSpecializedFunctionType(location, funcName, typeArguments, skipThisParam, initSpecGenContext);
            if (mlir::failed(result))
            {
                emitError(location) << "can't instantiate function. '" << funcName
                                    << "' not all generic types can be identified";
                return mlir::failure();
            }

            return resolveFunctionWithCapture(location, StringRef(funcSymbolName), funcType, thisValue, false, genContext);
        }

        if (auto classOp = genResult.getDefiningOp<mlir_ts::ClassRefOp>())
        {
            auto classType = classOp.getType();
            auto [result, specType] = instantiateSpecializedClassType(location, classType, typeArguments, genContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            if (auto specClassType = specType.dyn_cast_or_null<mlir_ts::ClassType>())
            {
                return V(builder.create<mlir_ts::ClassRefOp>(
                    location, specClassType,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), specClassType.getName().getValue())));
            }

            return genResult;
        }

        if (auto ifaceOp = genResult.getDefiningOp<mlir_ts::InterfaceRefOp>())
        {
            auto interfaceType = ifaceOp.getType();
            auto [result, specType] =
                instantiateSpecializedInterfaceType(location, interfaceType, typeArguments, genContext);
            if (auto specInterfaceType = specType.dyn_cast_or_null<mlir_ts::InterfaceType>())
            {
                return V(builder.create<mlir_ts::InterfaceRefOp>(
                    location, specInterfaceType,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), specInterfaceType.getName().getValue())));
            }

            return genResult;
        }

        return genResult;
    }

    ValueOrLogicalResult mlirGen(Expression expression, NodeArray<TypeNode> typeArguments, const GenContext &genContext)
    {
        auto result = mlirGen(expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto genResult = V(result);
        if (typeArguments.size() == 0)
        {
            return genResult;
        }

        auto location = loc(expression);

        return mlirGenSpecialized(location, genResult, typeArguments, genContext);
    }

    ValueOrLogicalResult mlirGen(ExpressionWithTypeArguments expressionWithTypeArgumentsAST,
                                 const GenContext &genContext)
    {
        return mlirGen(expressionWithTypeArgumentsAST->expression, expressionWithTypeArgumentsAST->typeArguments,
                       genContext);
    }

    ValueOrLogicalResult registerVariableInThisContext(mlir::Location location, StringRef name, mlir::Type type,
                                                       const GenContext &genContext)
    {
        if (genContext.passResult)
        {

            // create new type with added field
            genContext.passResult->extraFieldsInThisContext.push_back({MLIRHelper::TupleFieldName(name, builder.getContext()), type});
            return mlir::Value();
        }

        // resolve object property

        NodeFactory nf(NodeFactoryFlags::None);
        // load this.<var name>
        auto _this = nf.createToken(SyntaxKind::ThisKeyword);
        auto _name = nf.createIdentifier(stows(std::string(name)));
        auto _this_name = nf.createPropertyAccessExpression(_this, _name);

        auto result = mlirGen(_this_name, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto thisVarValue = V(result);

        assert(thisVarValue);

        MLIRCodeLogic mcl(builder);
        auto thisVarValueRef = mcl.GetReferenceOfLoadOp(thisVarValue);

        assert(thisVarValueRef);

        return V(thisVarValueRef);
    }

    bool isConstValue(mlir::Value init)
    {
        if (!init)
        {
            return false;
        }

        if (init.getType().isa<mlir_ts::ConstArrayType>() || init.getType().isa<mlir_ts::ConstTupleType>())
        {
            return true;
        }

        auto defOp = init.getDefiningOp();
        if (isa<mlir_ts::ConstantOp>(defOp) || isa<mlir_ts::UndefOp>(defOp) || isa<mlir_ts::NullOp>(defOp))
        {
            return true;
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! is it const? : " << init << "\n";);

        return false;
    }

    struct VariableDeclarationInfo
    {
        VariableDeclarationInfo() : variableName(), fullName(), initial(), type(), storage(), globalOp(), varClass(),
            scope{VariableScope::Local}, isFullName{false}, isGlobal{false}, isConst{false}, isExternal{false}, isExport{false}, isImport{false}, deleted{false} 
        {
        };

        VariableDeclarationInfo(
            TypeValueInitFuncType func_, 
            std::function<StringRef(StringRef)> getFullNamespaceName_) : VariableDeclarationInfo() 
        {
            getFullNamespaceName = getFullNamespaceName_;
            func = func_;
        }

        void setName(StringRef name_)
        {
            variableName = name_;
            fullName = name_;

            // I think it is only making it worst
            if (!isFullName && isGlobal)
                fullName = getFullNamespaceName(name_);
        }        

        void setType(mlir::Type type_)
        {
            type = type_;
        }             

        void setInitial(mlir::Value initial_)
        {
            initial = initial_;
        }             

        void setIsTypeProvided(TypeProvided typeProvided_)
        {
            typeProvided = typeProvided_;
        }

        void setExternal(bool isExternal_)
        {
            isExternal = isExternal_;
        }        

        void setStorage(mlir::Value storage_)
        {
            storage = storage_;
        }   

        void detectFlags(bool isFullName_, VariableClass varClass_, const GenContext &genContext)
        {
            varClass = varClass_;
            isFullName = isFullName_;
            
            if (isFullName_ || !genContext.funcOp)
            {
                scope = VariableScope::Global;
            }

            isGlobal = scope == VariableScope::Global || varClass == VariableType::Var;
            isConst = (varClass == VariableType::Const || varClass == VariableType::ConstRef) &&
                       !genContext.allocateVarsOutsideOfOperation && !genContext.allocateVarsInContextThis;
            isExternal = varClass == VariableType::External;
            isExport = varClass.isExport;
            isImport = varClass.isImport;
            isAppendingLinkage = varClass.isAppendingLinkage;
        }

        mlir::LogicalResult processConstRef(mlir::Location location, mlir::OpBuilder &builder, const GenContext &genContext)
        {
            if (mlir::failed(getVariableTypeAndInit(location, genContext)))
            {
                return mlir::failure();
            }

            if (varClass == VariableType::ConstRef)
            {
                MLIRCodeLogic mcl(builder);
                if (auto possibleInit = mcl.GetReferenceOfLoadOp(initial))
                {
                    setInitial(possibleInit);
                }
                else
                {
                    // convert ConstRef to Const again as this is const object (it seems)
                    varClass = VariableType::Const;
                }
            }

            return mlir::success();
        }

        mlir::LogicalResult getVariableTypeAndInit(mlir::Location location, const GenContext &genContext)
        {
            auto [type, init, typeProvided] = func(location, genContext);
            if (!type)
            {
                if (!genContext.allowPartialResolve)
                {
                    emitError(location) << "Can't resolve variable '" << variableName << "' type";
                }

                return mlir::failure();
            }

            if (type.isa<mlir_ts::VoidType>())
            {
                emitError(location) << "variable '" << variableName << "' can't be 'void' type";
                return mlir::failure();
            }

            if (type.isa<mlir_ts::NeverType>())
            {
                emitError(location) << "variable '" << variableName << "' can't be 'never' type";
                return mlir::failure();
            }

            assert(type);
            setType(type);
            setInitial(init);
            setIsTypeProvided(typeProvided);

            return mlir::success();
        }    

        VariableDeclarationDOM::TypePtr createVariableDeclaration(mlir::Location location, const GenContext &genContext)
        {
            auto varDecl = std::make_shared<VariableDeclarationDOM>(fullName, type, location);
            if (!isConst || varClass == VariableType::ConstRef)
            {
                varDecl->setReadWriteAccess();
                // TODO: HACK: to mark var as local and ignore when capturing
                if (varClass == VariableType::ConstRef)
                {
                    varDecl->setIgnoreCapturing();
                }
            }

            varDecl->setUsing(varClass.isUsing);

            return varDecl;
        }

        void printDebugInfo()
        {
            LLVM_DEBUG(dbgs() << "\n!! variable = " << fullName << " type: " << type << "\n";);
        }

        TypeValueInitFuncType func;
        std::function<StringRef(StringRef)> getFullNamespaceName;

        StringRef variableName;
        StringRef fullName;
        mlir::Value initial;
        TypeProvided typeProvided;
        mlir::Type type;
        mlir::Value storage;
        mlir_ts::GlobalOp globalOp;

        VariableClass varClass;
        VariableScope scope;
        bool isFullName;
        bool isGlobal;
        bool isConst;
        bool isExternal;
        bool isExport;
        bool isImport;
        bool isAppendingLinkage;
        bool deleted;
    };

    mlir::LogicalResult adjustLocalVariableType(mlir::Location location, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        auto type = variableDeclarationInfo.type;

        // if it is Optional type, we need to set to undefined                
        if (type.isa<mlir_ts::OptionalType>() && !variableDeclarationInfo.initial)
        {                    
            CAST_A(castedValue, location, type, getUndefined(location), genContext);
            variableDeclarationInfo.setInitial(castedValue);
        }

        if (variableDeclarationInfo.isConst)
        {
            return mlir::success();
        }

        auto actualType = variableDeclarationInfo.typeProvided == TypeProvided::Yes ? type : mth.wideStorageType(type);

        // this is 'let', if 'let' is func, it should be HybridFunction
        if (auto funcType = actualType.dyn_cast<mlir_ts::FunctionType>())
        {
            actualType = mlir_ts::HybridFunctionType::get(builder.getContext(), funcType);
        }

        if (variableDeclarationInfo.initial && actualType != type)
        {
            CAST_A(castedValue, location, actualType, variableDeclarationInfo.initial, genContext);
            variableDeclarationInfo.setInitial(castedValue);
        }

        variableDeclarationInfo.setType(actualType);

        return mlir::success();
    }

    mlir::LogicalResult adjustGlobalVariableType(mlir::Location location, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        if (variableDeclarationInfo.isConst)
        {
            return mlir::success();
        }

        auto type = variableDeclarationInfo.type;

        auto actualType = variableDeclarationInfo.typeProvided == TypeProvided::Yes ? type : mth.wideStorageType(type);

        variableDeclarationInfo.setType(actualType);

        return mlir::success();
    }    
   
    mlir::LogicalResult createLocalVariable(mlir::Location location, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        if (mlir::failed(variableDeclarationInfo.getVariableTypeAndInit(location, genContext)))
        {
            return mlir::failure();
        }

        if (mlir::failed(adjustLocalVariableType(location, variableDeclarationInfo, genContext)))
        {
            return mlir::failure();
        }

        // scope to restore inserting point
        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);
            if (genContext.allocateVarsOutsideOfOperation)
            {
                builder.setInsertionPoint(genContext.currentOperation);
            }

            if (genContext.allocateVarsInContextThis)
            {
                auto varValueInThisContext = registerVariableInThisContext(location, variableDeclarationInfo.variableName, variableDeclarationInfo.type, genContext);
                variableDeclarationInfo.setStorage(varValueInThisContext);
            }

            if (!variableDeclarationInfo.storage)
            {
                // default case
                auto varOpValue = builder.create<mlir_ts::VariableOp>(
                    location, mlir_ts::RefType::get(variableDeclarationInfo.type),
                    genContext.allocateVarsOutsideOfOperation ? mlir::Value() : variableDeclarationInfo.initial,
                    builder.getBoolAttr(false));

                variableDeclarationInfo.setStorage(varOpValue);
            }
        }

        // init must be in its normal place
        if ((genContext.allocateVarsInContextThis || genContext.allocateVarsOutsideOfOperation) 
            && variableDeclarationInfo.initial 
            && variableDeclarationInfo.storage)
        {
            builder.create<mlir_ts::StoreOp>(location, variableDeclarationInfo.initial, variableDeclarationInfo.storage);
        }

        return mlir::success();
    }    

    mlir::LogicalResult createGlobalVariableInitialization(mlir::Location location, mlir_ts::GlobalOp globalOp, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        mlir::OpBuilder::InsertionGuard insertGuard(builder);

        auto &region = globalOp.getInitializerRegion();
        auto *block = builder.createBlock(&region);

        builder.setInsertionPoint(block, block->begin());

        GenContext genContextWithNameReceiver(genContext);
        if (variableDeclarationInfo.isConst)
        {
            genContextWithNameReceiver.receiverName = variableDeclarationInfo.fullName;
        }

        if (mlir::failed(variableDeclarationInfo.getVariableTypeAndInit(location, genContextWithNameReceiver)))
        {
            return mlir::failure();
        }

        if (mlir::failed(adjustGlobalVariableType(location, variableDeclarationInfo, genContext)))
        {
            return mlir::failure();
        }        

        globalOp.setTypeAttr(mlir::TypeAttr::get(variableDeclarationInfo.type));
        /*
        if (variableDeclarationInfo.isExport)
        {
            addGlobalToExport(variableDeclarationInfo.variableName, variableDeclarationInfo.type, genContext);
        }
        */

        if (!variableDeclarationInfo.initial)
        {
            variableDeclarationInfo.initial = builder.create<mlir_ts::UndefOp>(location, variableDeclarationInfo.type);
        }

        builder.create<mlir_ts::GlobalResultOp>(location, mlir::ValueRange{variableDeclarationInfo.initial});

        return mlir::success();
    }    

    mlir::LogicalResult createGlobalVariableUndefinedInitialization(mlir::Location location, mlir_ts::GlobalOp globalOp, struct VariableDeclarationInfo &variableDeclarationInfo)
    {
        // we need to put undefined into GlobalOp
        mlir::OpBuilder::InsertionGuard insertGuard(builder);

        auto &region = globalOp.getInitializerRegion();
        auto *block = builder.createBlock(&region);

        builder.setInsertionPoint(block, block->begin());

        auto undefVal = builder.create<mlir_ts::UndefOp>(location, variableDeclarationInfo.type);
        builder.create<mlir_ts::GlobalResultOp>(location, mlir::ValueRange{undefVal});

        return mlir::success();
    }

    mlir::LogicalResult createGlobalVariable(mlir::Location location, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        // generate only for real pass
        mlir_ts::GlobalOp globalOp;
        // get constant
        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);
            builder.setInsertionPointToStart(theModule.getBody());
            // find last string
            auto lastUse = [&](mlir::Operation *op) {
                if (auto globalOp = dyn_cast<mlir_ts::GlobalOp>(op))
                {
                    builder.setInsertionPointAfter(globalOp);
                }
            };

            theModule.getBody()->walk(lastUse);

            SmallVector<mlir::NamedAttribute> attrs;
            if (variableDeclarationInfo.isExternal)
            {
                attrs.push_back({builder.getStringAttr("Linkage"), builder.getStringAttr("External")});
            }
            else if (variableDeclarationInfo.isAppendingLinkage)
            {
                attrs.push_back({builder.getStringAttr("Linkage"), builder.getStringAttr("Appending")});
            }

            // add modifiers
            if (variableDeclarationInfo.isExport)
            {
                attrs.push_back({mlir::StringAttr::get(builder.getContext(), "export"), mlir::UnitAttr::get(builder.getContext())});
            }            

            if (variableDeclarationInfo.isImport)
            {
                attrs.push_back({mlir::StringAttr::get(builder.getContext(), "import"), mlir::UnitAttr::get(builder.getContext())});
            }  

            globalOp = builder.create<mlir_ts::GlobalOp>(
                location, builder.getNoneType(), variableDeclarationInfo.isConst, variableDeclarationInfo.fullName, mlir::Attribute(), attrs);                

            variableDeclarationInfo.globalOp = globalOp;

            if (genContext.dummyRun && genContext.cleanUpOps)
            {
                genContext.cleanUpOps->push_back(globalOp);
            }

            if (variableDeclarationInfo.scope == VariableScope::Global)
            {
                if (variableDeclarationInfo.isExternal)
                {
                    if (mlir::failed(variableDeclarationInfo.getVariableTypeAndInit(location, genContext)))
                    {
                        return mlir::failure();
                    }

                    if (mlir::failed(adjustGlobalVariableType(location, variableDeclarationInfo, genContext)))
                    {
                        return mlir::failure();
                    }                      

                    globalOp.setTypeAttr(mlir::TypeAttr::get(variableDeclarationInfo.type));
                }
                else
                {
                    createGlobalVariableInitialization(location, globalOp, variableDeclarationInfo, genContext);
                }

                return mlir::success();
            }
        }

        // it is not global scope (for example 'var' in function)
        if (mlir::failed(variableDeclarationInfo.getVariableTypeAndInit(location, genContext))) 
        {
            return mlir::failure();
        }

        if (mlir::failed(adjustGlobalVariableType(location, variableDeclarationInfo, genContext)))
        {
            return mlir::failure();
        }  

        globalOp.setTypeAttr(mlir::TypeAttr::get(variableDeclarationInfo.type));
        if (variableDeclarationInfo.isExternal)
        {
            // all is done here
            return mlir::success();
        }

        if (variableDeclarationInfo.initial)
        {
            // save value
            auto address = builder.create<mlir_ts::AddressOfOp>(
                location, mlir_ts::RefType::get(variableDeclarationInfo.type), variableDeclarationInfo.fullName, mlir::IntegerAttr());
            builder.create<mlir_ts::StoreOp>(location, variableDeclarationInfo.initial, address);
        }

        return createGlobalVariableUndefinedInitialization(location, globalOp, variableDeclarationInfo);
    }    

    mlir::LogicalResult isGlobalConstLambda(mlir::Location location, struct VariableDeclarationInfo &variableDeclarationInfo, const GenContext &genContext)
    {
        if (variableDeclarationInfo.isConst 
            && variableDeclarationInfo.initial 
            && mth.isAnyFunctionType(variableDeclarationInfo.type))
        {
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult registerVariableDeclaration(mlir::Location location, VariableDeclarationDOM::TypePtr variableDeclaration, struct VariableDeclarationInfo &variableDeclarationInfo, bool showWarnings, const GenContext &genContext)
    {
        if (variableDeclarationInfo.deleted)
        {
            return mlir::success();
        }
        else if (!variableDeclarationInfo.isGlobal)
        {
            if (mlir::failed(declare(
                location, 
                variableDeclaration, 
                variableDeclarationInfo.storage 
                    ? variableDeclarationInfo.storage 
                    : variableDeclarationInfo.initial, 
                genContext, 
                showWarnings)))
            {
                return mlir::failure();
            }
        }
        else if (variableDeclarationInfo.isFullName)
        {
            fullNameGlobalsMap.insert(variableDeclarationInfo.fullName, variableDeclaration);
        }
        else
        {
            getGlobalsMap().insert({variableDeclarationInfo.variableName, variableDeclaration});
        }

        return mlir::success();
    }

    mlir::Type registerVariable(mlir::Location location, StringRef name, bool isFullName, VariableClass varClass,
                                TypeValueInitFuncType func, const GenContext &genContext, bool showWarnings = false)
    {
        struct VariableDeclarationInfo variableDeclarationInfo(
            func, std::bind(&MLIRGenImpl::getGlobalsFullNamespaceName, this, std::placeholders::_1));

        variableDeclarationInfo.detectFlags(isFullName, varClass, genContext);
        variableDeclarationInfo.setName(name);

        if (declarationMode)
            variableDeclarationInfo.setExternal(true);

        if (!variableDeclarationInfo.isGlobal)
        {
            if (variableDeclarationInfo.isConst)
                variableDeclarationInfo.processConstRef(location, builder, genContext);
            else
                createLocalVariable(location, variableDeclarationInfo, genContext);
        }
        else
        {
            createGlobalVariable(location, variableDeclarationInfo, genContext);

            if (mlir::succeeded(isGlobalConstLambda(location, variableDeclarationInfo, genContext)))
            {
                variableDeclarationInfo.globalOp->erase();
                variableDeclarationInfo.deleted = true;
            }
        }

        if (!variableDeclarationInfo.type)
        {
            emitError(location) << "type of variable '" << variableDeclarationInfo.variableName << "' is not valid";
            return variableDeclarationInfo.type;
        }

#ifndef NDEBUG
        variableDeclarationInfo.printDebugInfo();
#endif

        auto varDecl = variableDeclarationInfo.createVariableDeclaration(location, genContext);
        if (genContext.usingVars != nullptr && varDecl->getUsing())
        {
            genContext.usingVars->push_back(varDecl);
        }

        registerVariableDeclaration(location, varDecl, variableDeclarationInfo, showWarnings, genContext);
        return varDecl->getType();
    }

    // TODO: to support '...' u need to use 'processOperandSpreadElement' and instead of "index" param use "next" logic
    ValueOrLogicalResult processDeclarationArrayBindingPatternSubPath(mlir::Location location, int index, mlir::Type type, mlir::Value init, const GenContext &genContext)
    {
        MLIRPropertyAccessCodeLogic cl(builder, location, init, builder.getI32IntegerAttr(index));
        mlir::Value subInit =
            TypeSwitch<mlir::Type, mlir::Value>(type)
                .template Case<mlir_ts::ConstTupleType>(
                    [&](auto constTupleType) { return cl.Tuple(constTupleType, true); })
                .template Case<mlir_ts::TupleType>([&](auto tupleType) { return cl.Tuple(tupleType, true); })
                .template Case<mlir_ts::ConstArrayType>([&](auto constArrayType) {
                    // TODO: unify it with ElementAccess
                    auto constIndex = builder.create<mlir_ts::ConstantOp>(location, builder.getI32Type(),
                                                                        builder.getI32IntegerAttr(index));
                    auto elemRef = builder.create<mlir_ts::ElementRefOp>(
                        location, mlir_ts::RefType::get(constArrayType.getElementType()), init, constIndex);
                    return builder.create<mlir_ts::LoadOp>(location, constArrayType.getElementType(), elemRef);
                })
                .template Case<mlir_ts::ArrayType>([&](auto arrayType) {
                    // TODO: unify it with ElementAccess
                    auto constIndex = builder.create<mlir_ts::ConstantOp>(location, builder.getI32Type(),
                                                                        builder.getI32IntegerAttr(index));
                    auto elemRef = builder.create<mlir_ts::ElementRefOp>(
                        location, mlir_ts::RefType::get(arrayType.getElementType()), init, constIndex);
                    return builder.create<mlir_ts::LoadOp>(location, arrayType.getElementType(), elemRef);
                })
                .Default([&](auto type) { llvm_unreachable("not implemented"); return mlir::Value(); });

        if (!subInit)
        {
            return mlir::failure();
        }

        return subInit; 
    }

    mlir::LogicalResult processDeclarationArrayBindingPattern(mlir::Location location, ArrayBindingPattern arrayBindingPattern,
                                               VariableClass varClass,
                                               TypeValueInitFuncType func,
                                               const GenContext &genContext)
    {
        auto [typeRef, initRef, typeProvidedRef] = func(location, genContext);
        mlir::Type type = typeRef;
        mlir::Value init = initRef;
        //TypeProvided typeProvided = typeProvidedRef;

        auto index = 0;
        for (auto arrayBindingElement : arrayBindingPattern->elements)
        {
            auto subValueFunc = [&](mlir::Location location, const GenContext &genContext) { 
                auto result = processDeclarationArrayBindingPatternSubPath(location, index, type, init, genContext);
                if (result.failed_or_no_value()) 
                {
                    return std::make_tuple(mlir::Type(), mlir::Value(), TypeProvided::No); 
                }

                auto value = V(result);
                return std::make_tuple(value.getType(), value, TypeProvided::No); 
            };

            if (mlir::failed(processDeclaration(
                    arrayBindingElement.as<BindingElement>(), varClass, subValueFunc, genContext)))
            {
                return mlir::failure();
            }

            index++;
        }

        return mlir::success();
    }

    mlir::Attribute getFieldNameFromBindingElement(BindingElement objectBindingElement)
    {
        mlir::Attribute fieldName;
        if (objectBindingElement->propertyName == SyntaxKind::NumericLiteral)
        {
            fieldName = getNumericLiteralAttribute(objectBindingElement->propertyName);
        }
        else
        {
            auto propertyName = MLIRHelper::getName(objectBindingElement->propertyName);
            if (propertyName.empty())
            {
                propertyName = MLIRHelper::getName(objectBindingElement->name);
            }

            if (!propertyName.empty())
            {
                fieldName = MLIRHelper::TupleFieldName(propertyName, builder.getContext());
            }
        }

        return fieldName;
    }

    ValueOrLogicalResult processDeclarationObjectBindingPatternSubPath(
        mlir::Location location, BindingElement objectBindingElement, mlir::Type type, mlir::Value init, const GenContext &genContext)
    {
        auto fieldName = getFieldNameFromBindingElement(objectBindingElement);
        auto isNumericAccess = fieldName.isa<mlir::IntegerAttr>();

        LLVM_DEBUG(llvm::dbgs() << "ObjectBindingPattern:\n\t" << init << "\n\tprop: " << fieldName << "\n");

        mlir::Value subInit;
        mlir::Type subInitType;

        mlir::Value value;
        if (isNumericAccess)
        {
            MLIRPropertyAccessCodeLogic cl(builder, location, init, fieldName);
            if (auto tupleType = dyn_cast<mlir_ts::TupleType>(type))
            {
                value = cl.Tuple(tupleType, true);
            }
            else if (auto constTupleType = dyn_cast<mlir_ts::ConstTupleType>(type))
            {
                value = cl.Tuple(constTupleType, true);
            }
        }
        else
        {
            auto result = mlirGenPropertyAccessExpression(location, init, fieldName, false, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            value = V(result);
        }

        if (!value)
        {
            return mlir::failure();
        }

        if (objectBindingElement->initializer)
        {
            auto tupleType = type.cast<mlir_ts::TupleType>();
            auto subType = tupleType.getFieldInfo(tupleType.getIndex(fieldName)).type.cast<mlir_ts::OptionalType>().getElementType();
            auto res = optionalValueOrDefault(location, subType, value, objectBindingElement->initializer, genContext);
            subInit = V(res);
            subInitType = subInit.getType();                    
        }
        else
        {
            subInit = value;
            subInitType = subInit.getType();
        }

        assert(subInit);

        return subInit; 
    }

    ValueOrLogicalResult processDeclarationObjectBindingPatternSubPathSpread(
        mlir::Location location, ObjectBindingPattern objectBindingPattern, mlir::Type type, mlir::Value init, const GenContext &genContext)
    {
        mlir::Value subInit;
        mlir::Type subInitType;

        SmallVector<mlir::Attribute> names;

        // take all used fields
        for (auto objectBindingElement : objectBindingPattern->elements)
        {
            auto isSpreadBinding = !!objectBindingElement->dotDotDotToken;
            if (isSpreadBinding)
            {
                continue;
            }

            auto fieldId = getFieldNameFromBindingElement(objectBindingElement);
            names.push_back(fieldId);
        }                

        // filter all fields
        llvm::SmallVector<mlir_ts::FieldInfo> tupleFields;
        llvm::SmallVector<mlir_ts::FieldInfo> destTupleFields;
        if (mlir::succeeded(mth.getFields(init.getType(), tupleFields)))
        {
            for (auto fieldInfo : tupleFields)
            {
                if (std::find_if(names.begin(), names.end(), [&] (auto& item) { return item == fieldInfo.id; }) == names.end())
                {
                    // filter;
                    destTupleFields.push_back(fieldInfo);
                }
            }
        }

        // create object
        subInitType = getTupleType(destTupleFields);
        CAST(subInit, location, subInitType, init, genContext);

        assert(subInit);

        return subInit; 
    }

    mlir::LogicalResult processDeclarationObjectBindingPattern(mlir::Location location, ObjectBindingPattern objectBindingPattern,
                                                VariableClass varClass,
                                                TypeValueInitFuncType func,
                                                const GenContext &genContext)
    {
        auto [typeRef, initRef, typeProvidedRef] = func(location, genContext);
        mlir::Type type = typeRef;
        mlir::Value init = initRef;
        //TypeProvided typeProvided = typeProvidedRef;

        auto index = 0;
        for (auto objectBindingElement : objectBindingPattern->elements)
        {
            auto subValueFunc = [&] (mlir::Location location, const GenContext &genContext) {

                auto isSpreadBinding = !!objectBindingElement->dotDotDotToken;
                auto result = isSpreadBinding 
                    ? processDeclarationObjectBindingPatternSubPathSpread(location, objectBindingPattern, type, init, genContext)
                    : processDeclarationObjectBindingPatternSubPath(location, objectBindingElement, type, init, genContext);
                if (result.failed_or_no_value()) 
                {
                    return std::make_tuple(mlir::Type(), mlir::Value(), TypeProvided::No); 
                }                    

                auto value = V(result);
                return std::make_tuple(value.getType(), value, TypeProvided::No); 
            };

            // nested obj, objectBindingElement->propertyName -> name
            if (objectBindingElement->name == SyntaxKind::ObjectBindingPattern)
            {
                auto objectBindingPattern = objectBindingElement->name.as<ObjectBindingPattern>();

                return processDeclarationObjectBindingPattern(
                    location, objectBindingPattern, varClass, subValueFunc, genContext);
            }

            if (mlir::failed(processDeclaration(
                    objectBindingElement, varClass, subValueFunc, genContext)))
            { 
                return mlir::failure();
            }

            index++;
        }

        return mlir::success();;
    }

    mlir::LogicalResult processDeclarationName(DeclarationName name, VariableClass varClass,
                            TypeValueInitFuncType func, const GenContext &genContext, bool showWarnings = false)
    {
        auto location = loc(name);

        if (name == SyntaxKind::ArrayBindingPattern)
        {
            auto arrayBindingPattern = name.as<ArrayBindingPattern>();
            if (mlir::failed(processDeclarationArrayBindingPattern(location, arrayBindingPattern, varClass, func, genContext)))
            {
                return mlir::failure();
            }
        }
        else if (name == SyntaxKind::ObjectBindingPattern)
        {
            auto objectBindingPattern = name.as<ObjectBindingPattern>();
            if (mlir::failed(processDeclarationObjectBindingPattern(location, objectBindingPattern, varClass, func, genContext)))
            {
                return mlir::failure();
            }
        }
        else
        {
            // name
            auto nameStr = MLIRHelper::getName(name);

            // register
            return !!registerVariable(location, nameStr, false, varClass, func, genContext, showWarnings) ? mlir::success() : mlir::failure();
        }

        return mlir::success();       
    }

    mlir::LogicalResult processDeclaration(NamedDeclaration item, VariableClass varClass,
                            TypeValueInitFuncType func, const GenContext &genContext, bool showWarnings = false)
    {
        if (item == SyntaxKind::OmittedExpression)
        {
            return mlir::success();
        }

        return processDeclarationName(item->name, varClass, func, genContext, showWarnings);
    }

    template <typename ItemTy>
    TypeValueInitType getTypeOnly(ItemTy item, mlir::Type defaultType, const GenContext &genContext)
    {
        // type
        auto typeProvided = TypeProvided::No;
        mlir::Type type = defaultType;
        if (item->type)
        {
            type = getType(item->type, genContext);
            typeProvided = TypeProvided::Yes;
        }

        return std::make_tuple(type, mlir::Value(), typeProvided);
    }

    template <typename ItemTy>
    std::tuple<mlir::Type, bool, bool> evaluateTypeAndInit(ItemTy item, const GenContext &genContext)
    {
        // type
        auto hasInit = false;
        auto typeProvided = false;
        mlir::Type type;
        if (item->type)
        {
            type = getType(item->type, genContext);
            typeProvided = true;
        }

        // init
        if (auto initializer = item->initializer)
        {
            hasInit = true;
            auto initType = evaluate(initializer, genContext);
            if (initType && !type)
            {
                type = initType;
            }
        }

        return std::make_tuple(type, hasInit, typeProvided);
    }

    template <typename ItemTy>
    std::tuple<mlir::Type, mlir::Value, TypeProvided> getTypeAndInit(ItemTy item, const GenContext &genContext)
    {
        // type
        auto typeProvided = TypeProvided::No;
        mlir::Type type;
        if (item->type)
        {
            type = getType(item->type, genContext);
            if (!type || VALIDATE_FUNC_BOOL(type))
            {
                return {mlir::Type(), mlir::Value(), TypeProvided::No};
            }

            typeProvided = TypeProvided::Yes;
        }

        // init
        mlir::Value init;
        if (auto initializer = item->initializer)
        {
            GenContext genContextWithTypeReceiver(genContext);
            genContextWithTypeReceiver.clearReceiverTypes();
            if (type)
            {
                genContextWithTypeReceiver.receiverType = type;
                LLVM_DEBUG(dbgs() << "\n!! variable receiverType " << type << "\n");
            }

            auto result = mlirGen(initializer, genContextWithTypeReceiver);
            if (result.failed())
            {
                return {mlir::Type(), mlir::Value(), TypeProvided::No};
            }

            init = V(result);
            if (init)
            {
                if (!type)
                {
                    type = init.getType();
                }
                else if (type != init.getType())
                {
                    auto result = cast(loc(initializer), type, init, genContext);
                    if (result.failed())
                    {
                        return {mlir::Type(), mlir::Value(), TypeProvided::No};
                    }

                    init = V(result);
                }
            }
        }

#ifdef ANY_AS_DEFAULT
        if (!type)
        {
            type = getAnyType();
        }
#endif

        return std::make_tuple(type, init, typeProvided);
    }

    mlir::LogicalResult mlirGen(VariableDeclaration item, VariableClass varClass, const GenContext &genContext)
    {
        auto location = loc(item);

        auto isExternal = varClass == VariableType::External;
        if (declarationMode)
        {
            isExternal = true;
        }

#ifndef ANY_AS_DEFAULT
        if (mth.isNoneType(item->type) && !item->initializer && !isExternal)
        {
            auto name = MLIRHelper::getName(item->name);
            emitError(loc(item)) << "type of variable '" << name
                                 << "' is not provided, variable must have type or initializer";
            return mlir::failure();
        }
#endif

        auto initFunc = [&](mlir::Location location, const GenContext &genContext) {
            if (declarationMode)
            {
                auto [t, b, p] = evaluateTypeAndInit(item, genContext);
                return std::make_tuple(t, mlir::Value(), p ? TypeProvided::Yes : TypeProvided::No);
            }

            return getTypeAndInit(item, genContext);
        };

        auto valClassItem = varClass;
        if ((item->internalFlags & InternalFlags::ForceConst) == InternalFlags::ForceConst)
        {
            valClassItem = VariableType::Const;
        }

        if ((item->internalFlags & InternalFlags::ForceConstRef) == InternalFlags::ForceConstRef)
        {
            valClassItem = VariableType::ConstRef;
        }

        if (!genContext.funcOp && (item->name == SyntaxKind::ObjectBindingPattern || item->name == SyntaxKind::ArrayBindingPattern))
        {
            auto fullInitGlobalFuncName = getFullNamespaceName(MLIRHelper::getAnonymousName(location, ".gc"));

            {
                mlir::OpBuilder::InsertionGuard insertGuard(builder);

                // create global construct
                valClassItem = VariableType::Var;

                auto funcType = getFunctionType({}, {}, false);

                if (mlir::failed(mlirGenFunctionBody(location, fullInitGlobalFuncName, funcType,
                    [&](const GenContext &genContext) {
                        return processDeclaration(item, valClassItem, initFunc, genContext, true);
                    }, genContext)))
                {
                    return mlir::failure();
                }
            }

            builder.create<mlir_ts::GlobalConstructorOp>(location, mlir::FlatSymbolRefAttr::get(builder.getContext(), fullInitGlobalFuncName));
        }
        else if (mlir::failed(processDeclaration(item, valClassItem, initFunc, genContext, true)))
        {
            return mlir::failure();
        }

        return mlir::success();
    }

    auto getExportModifier(Node node) -> boolean
    {
        if (compileOptions.exportOpt == ExportAll)
        {
            return true;
        }

        if (compileOptions.exportOpt == IgnoreAll)
        {
            return false;
        }

        return hasModifier(node, SyntaxKind::ExportKeyword);
    }

    mlir::LogicalResult mlirGen(VariableDeclarationList variableDeclarationListAST, const GenContext &genContext)
    {
        auto isLet = (variableDeclarationListAST->flags & NodeFlags::Let) == NodeFlags::Let;
        auto isConst = (variableDeclarationListAST->flags & NodeFlags::Const) == NodeFlags::Const;
        auto isUsing = (variableDeclarationListAST->flags & NodeFlags::Using) == NodeFlags::Using;
        auto isExternal = (variableDeclarationListAST->flags & NodeFlags::Ambient) == NodeFlags::Ambient;
        VariableClass varClass = isExternal ? VariableType::External
                        : isLet    ? VariableType::Let
                        : isConst || isUsing ? VariableType::Const
                                   : VariableType::Var;

        varClass.isUsing = isUsing;

        if (variableDeclarationListAST->parent)
        {
            varClass.isExport = getExportModifier(variableDeclarationListAST->parent);
            if (varClass.isExport)
            {
                addDeclarationToExport(variableDeclarationListAST->parent, "@dllimport\n");
            }
        }

        for (auto &item : variableDeclarationListAST->declarations)
        {
            if (mlir::failed(mlirGen(item, varClass, genContext)))
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(VariableStatement variableStatementAST, const GenContext &genContext)
    {
        // we need it for support "export" keyword
        variableStatementAST->declarationList->parent = variableStatementAST;
        return mlirGen(variableStatementAST->declarationList, genContext);
    }

    mlir::LogicalResult mlirGenParameterBindingElement(BindingElement objectBindingElement, SmallVector<mlir_ts::FieldInfo> &fieldInfos, const GenContext &genContext)
    {
        auto fieldId = getFieldNameFromBindingElement(objectBindingElement);
        if (!fieldId)
        {
            auto genName = MLIRHelper::getAnonymousName(loc_check(objectBindingElement), ".be");
            fieldId = MLIRHelper::TupleFieldName(genName, builder.getContext());
        }

        mlir::Type fieldType;

        if (objectBindingElement->initializer)
        {
            auto evalType = evaluate(objectBindingElement->initializer, genContext);
            auto widenType = mth.wideStorageType(evalType);

            // if it has initializer - it should have optional type to support default values
            fieldType = getOptionalType(widenType);
        }
        else if (objectBindingElement->name == SyntaxKind::ObjectBindingPattern || objectBindingElement->name == SyntaxKind::ArrayBindingPattern)
        {
            fieldType = mlirGenParameterObjectOrArrayBinding(objectBindingElement->name, genContext);
        }
        else
        {
            emitError(loc(objectBindingElement)) << "can't resolve type for binding pattern '"
                                                << fieldId << "', provide default initializer";
            return mlir::failure();
        }

        LLVM_DEBUG(dbgs() << "\n!! property " << fieldId << " mapped to type " << fieldType << "");

        fieldInfos.push_back({fieldId, fieldType});

        return mlir::success();
    }

    mlir::Type mlirGenParameterObjectOrArrayBinding(Node name, const GenContext &genContext)
    {
        // TODO: put it into function to support recursive call
        if (name == SyntaxKind::ObjectBindingPattern)
        {
            SmallVector<mlir_ts::FieldInfo> fieldInfos;

            // we need to construct object type
            auto objectBindingPattern = name.as<ObjectBindingPattern>();
            auto index = 0;
            for (auto objectBindingElement : objectBindingPattern->elements)
            {
                mlirGenParameterBindingElement(objectBindingElement, fieldInfos, genContext);
                index++;
            }

            return getTupleType(fieldInfos);
        } 
        else if (name == SyntaxKind::ArrayBindingPattern)
        {
            SmallVector<mlir_ts::FieldInfo> fieldInfos;

            // we need to construct object type
            auto arrayBindingPattern = name.as<ArrayBindingPattern>();
            auto index = 0;
            for (auto arrayBindingElement : arrayBindingPattern->elements)
            {
                if (arrayBindingElement == SyntaxKind::OmittedExpression)
                {
                    index++;
                    continue;
                }

                auto objectBindingElement = arrayBindingElement.as<BindingElement>();
                mlirGenParameterBindingElement(objectBindingElement, fieldInfos, genContext);
                index++;
            }

            return getTupleType(fieldInfos);
        }        

        return mlir::Type();
    }

    std::tuple<mlir::LogicalResult, bool, std::vector<std::shared_ptr<FunctionParamDOM>>> mlirGenParameters(
        SignatureDeclarationBase parametersContextAST, const GenContext &genContext)
    {
        auto isGenericTypes = false;
        std::vector<std::shared_ptr<FunctionParamDOM>> params;

        SyntaxKind kind = parametersContextAST;
        // add this param
        auto isStatic = hasModifier(parametersContextAST, SyntaxKind::StaticKeyword);
        if (!isStatic &&
            (kind == SyntaxKind::MethodDeclaration || kind == SyntaxKind::Constructor ||
             kind == SyntaxKind::GetAccessor || kind == SyntaxKind::SetAccessor))
        {
            params.push_back(
                std::make_shared<FunctionParamDOM>(THIS_NAME, genContext.thisType, loc(parametersContextAST)));
        }

        if (!isStatic && genContext.thisType && !!parametersContextAST->parent &&
            (kind == SyntaxKind::FunctionExpression ||
             kind == SyntaxKind::ArrowFunction))
        {            
            // TODO: this is very tricky code, if we rediscover function again and if by any chance thisType is not null, it will append thisType to lambda which very wrong code
            params.push_back(
                std::make_shared<FunctionParamDOM>(THIS_NAME, genContext.thisType, loc(parametersContextAST)));
        }

        if (parametersContextAST->parent.is<InterfaceDeclaration>())
        {
            params.push_back(std::make_shared<FunctionParamDOM>(THIS_NAME, getOpaqueType(), loc(parametersContextAST)));
        }

        auto formalParams = parametersContextAST->parameters;
        auto index = 0;
        for (auto arg : formalParams)
        {
            mlir::StringRef namePtr;
            namePtr = MLIRHelper::getName(arg->name, stringAllocator);
            if (namePtr.empty())
            {
                std::stringstream ss;
                ss << "arg" << index;
                namePtr = mlir::StringRef(ss.str()).copy(stringAllocator);
            }

            auto isBindingPattern = arg->name == SyntaxKind::ObjectBindingPattern || arg->name == SyntaxKind::ArrayBindingPattern;

            mlir::Type type;
            auto isMultiArgs = !!arg->dotDotDotToken;
            auto isOptional = !!arg->questionToken;
            auto typeParameter = arg->type;
            if (typeParameter)
            {
                type = getType(typeParameter, genContext);
            }

            // process init value
            auto initializer = arg->initializer;
            if (initializer)
            {
                auto evalType = evaluate(initializer, genContext);
                if (evalType)
                {
                    evalType = mth.wideStorageType(evalType);

                    // TODO: set type if not provided
                    isOptional = true;
                    if (mth.isNoneType(type))
                    {
                        type = evalType;
                    }
                }
            }

            if (mth.isNoneType(type) && genContext.receiverFuncType && mth.isAnyFunctionType(genContext.receiverFuncType))
            {
                type = mth.getParamFromFuncRef(genContext.receiverFuncType, index);

                LLVM_DEBUG(dbgs() << "\n!! param " << namePtr << " mapped to type " << type << "");

                isGenericTypes |= mth.isGenericType(type);
            }

            // in case of binding
            if (mth.isNoneType(type) && isBindingPattern)
            {
                type = mlirGenParameterObjectOrArrayBinding(arg->name, genContext);
                LLVM_DEBUG(dbgs() << "\n!! binding param " << namePtr << " is type " << type << "");
            }

            if (mth.isNoneType(type))
            {
                if (!typeParameter && !initializer)
                {
#ifndef ANY_AS_DEFAULT
                    if (!genContext.allowPartialResolve && !genContext.dummyRun)
                    {
                        auto funcName = MLIRHelper::getName(parametersContextAST->name);
                        emitError(loc(arg))
                            << "type of parameter '" << namePtr
                            << "' is not provided, parameter must have type or initializer, function: " << funcName;
                    }
                    return {mlir::failure(), isGenericTypes, params};
#else
                    emitWarning(loc(parametersContextAST)) << "type for parameter '" << namePtr << "' is any";
                    type = getAnyType();
#endif
                }
                else
                {
                    emitError(loc(typeParameter)) << "can't resolve type for parameter '" << namePtr << "'";
                    return {mlir::failure(), isGenericTypes, params};
                }
            }

            if (type.isa<mlir_ts::VoidType>())
            {
                emitError(loc(typeParameter), "'Void' can't be used as parameter type");
                return {mlir::failure(), isGenericTypes, params};
            }

            if (type.isa<mlir_ts::NeverType>())
            {
                emitError(loc(typeParameter), "'Never' can't be used as parameter type");
                return {mlir::failure(), isGenericTypes, params};
            }

            if (isBindingPattern)
            {
                params.push_back(
                    std::make_shared<FunctionParamDOM>(
                        namePtr, type, loc(arg), isOptional, isMultiArgs, initializer, arg->name));
            }
            else
            {
                params.push_back(
                    std::make_shared<FunctionParamDOM>(
                        namePtr, type, loc(arg), isOptional, isMultiArgs, initializer));
            }

            index++;
        }

        return {mlir::success(), isGenericTypes, params};
    }

    std::tuple<std::string, std::string> getNameOfFunction(SignatureDeclarationBase signatureDeclarationBaseAST,
                                                           const GenContext &genContext)
    {
        auto name = getNameWithArguments(signatureDeclarationBaseAST, genContext);
        std::string objectOwnerName;
        if (signatureDeclarationBaseAST->parent == SyntaxKind::ClassDeclaration ||
            signatureDeclarationBaseAST->parent == SyntaxKind::ClassExpression)
        {
            objectOwnerName =
                getNameWithArguments(signatureDeclarationBaseAST->parent.as<ClassDeclaration>(), genContext);
        }
        else if (signatureDeclarationBaseAST->parent == SyntaxKind::InterfaceDeclaration)
        {
            objectOwnerName =
                getNameWithArguments(signatureDeclarationBaseAST->parent.as<InterfaceDeclaration>(), genContext);
        }
        else if (genContext.funcOp)
        {
            auto funcName = const_cast<GenContext &>(genContext).funcOp.getSymName().str();
            objectOwnerName = funcName;
        }

        if (signatureDeclarationBaseAST == SyntaxKind::MethodDeclaration)
        {
            if (!genContext.thisType || !genContext.thisType.isa<mlir_ts::ObjectType>())
            {
                // class method name
                name = objectOwnerName + "." + name;
            }
            else
            {
                name = MLIRHelper::getAnonymousName(loc_check(signatureDeclarationBaseAST), ".md");
            }
        }
        // TODO: for new () interfaces
        else if (signatureDeclarationBaseAST == SyntaxKind::MethodSignature || signatureDeclarationBaseAST == SyntaxKind::ConstructSignature)
        {
            // class method name
            name = objectOwnerName + "." + name;
        }
        else if (signatureDeclarationBaseAST == SyntaxKind::GetAccessor)
        {
            // class method name
            name = objectOwnerName + ".get_" + name;
        }
        else if (signatureDeclarationBaseAST == SyntaxKind::SetAccessor)
        {
            // class method name
            name = objectOwnerName + ".set_" + name;
        }
        else if (signatureDeclarationBaseAST == SyntaxKind::Constructor)
        {
            // class method name
            auto isStatic = hasModifier(signatureDeclarationBaseAST, SyntaxKind::StaticKeyword);
            if (isStatic)
            {
                name = objectOwnerName + "." + STATIC_NAME + "_" + name;
            }
            else
            {
                name = objectOwnerName + "." + name;
            }
        }

        auto fullName = getFullNamespaceName(name).str();
        return std::make_tuple(fullName, name);
    }

    // TODO: review it, seems doing work which mlirGenFunctionPrototype will overwrite anyway
    std::tuple<FunctionPrototypeDOM::TypePtr, mlir_ts::FunctionType, SmallVector<mlir::Type>>
    mlirGenFunctionSignaturePrototype(SignatureDeclarationBase signatureDeclarationBaseAST, bool defaultVoid,
                                      const GenContext &genContext)
    {
        auto [fullName, name] = getNameOfFunction(signatureDeclarationBaseAST, genContext);

        registerNamespace(name, true);

        mlir_ts::FunctionType funcType;
        auto [result, isGenericType, params] = mlirGenParameters(signatureDeclarationBaseAST, genContext);

        exitNamespace();

        if (mlir::failed(result))
        {
            return std::make_tuple(FunctionPrototypeDOM::TypePtr(nullptr), funcType, SmallVector<mlir::Type>{});
        }

        SmallVector<mlir::Type> argTypes;
        auto argNumber = 0;
        auto isMultiArgs = false;

        // auto isAsync = hasModifier(signatureDeclarationBaseAST, SyntaxKind::AsyncKeyword);

        for (const auto &param : params)
        {
            auto paramType = param->getType();
            if (mth.isNoneType(paramType))
            {
                return std::make_tuple(FunctionPrototypeDOM::TypePtr(nullptr), funcType, SmallVector<mlir::Type>{});
            }

            if (param->getIsOptional() && !paramType.isa<mlir_ts::OptionalType>())
            {
                argTypes.push_back(getOptionalType(paramType));
            }
            else
            {
                argTypes.push_back(paramType);
            }

            isMultiArgs |= param->getIsMultiArgsParam();

            argNumber++;
        }

        auto funcProto = std::make_shared<FunctionPrototypeDOM>(fullName, params);

        funcProto->setNameWithoutNamespace(name);
        funcProto->setIsGeneric(isGenericType);

        // check if function already discovered
        auto funcIt = getFunctionMap().find(name);
        if (funcIt != getFunctionMap().end())
        {
            auto cachedFuncType = funcIt->second.getFunctionType();
            if (cachedFuncType.getNumResults() > 0)
            {
                auto returnType = cachedFuncType.getResult(0);
                funcProto->setReturnType(returnType);
            }

            funcType = cachedFuncType;
        }
        else if (auto typeParameter = signatureDeclarationBaseAST->type)
        {
            GenContext paramsGenContext(genContext);
            paramsGenContext.funcProto = funcProto;

            auto returnType = getType(typeParameter, paramsGenContext);
            if (!returnType)
            {
                return std::make_tuple(FunctionPrototypeDOM::TypePtr(nullptr), funcType, SmallVector<mlir::Type>{});
            }

            funcProto->setReturnType(returnType);

            funcType = getFunctionType(argTypes, returnType, isMultiArgs);
        }
        else if (defaultVoid)
        {
            auto returnType = getVoidType();
            funcProto->setReturnType(returnType);

            funcType = getFunctionType(argTypes, returnType, isMultiArgs);
        }

        return std::make_tuple(funcProto, funcType, argTypes);
    }

    bool isFuncAttr(StringRef name)
    {
        static llvm::StringMap<bool> funcAttrs {
            {"noinline", true },
            {"optnone", true },
            {DLL_IMPORT, true },
            {DLL_EXPORT, true },
        };

        return funcAttrs[name];        
    }

    std::tuple<mlir_ts::FuncOp, FunctionPrototypeDOM::TypePtr, mlir::LogicalResult, bool> mlirGenFunctionPrototype(
        FunctionLikeDeclarationBase functionLikeDeclarationBaseAST, const GenContext &genContext)
    {
        auto location = loc(functionLikeDeclarationBaseAST);

        mlir_ts::FuncOp funcOp;

        auto [funcProto, funcType, argTypes] =
            mlirGenFunctionSignaturePrototype(functionLikeDeclarationBaseAST, false, genContext);
        if (!funcProto)
        {
            return std::make_tuple(funcOp, funcProto, mlir::failure(), false);
        }

        auto fullName = funcProto->getName();

        mlir_ts::FunctionType functionDiscovered;
        auto funcTypeIt = getFunctionTypeMap().find(fullName);
        if (funcTypeIt != getFunctionTypeMap().end())
        {
            functionDiscovered = (*funcTypeIt).second;
        }        

        // discover type & args
        // seems we need to discover it all the time due to captured vars
        if (!funcType || genContext.forceDiscover || !functionDiscovered)
        {
            if (mlir::succeeded(discoverFunctionReturnTypeAndCapturedVars(functionLikeDeclarationBaseAST, fullName,
                                                                          argTypes, funcProto, genContext)))
            {
                if (!genContext.forceDiscover && funcType && funcType.getNumResults() > 0)
                {
                    funcProto->setReturnType(funcType.getResult(0));
                }
                else if (auto typeParameter = functionLikeDeclarationBaseAST->type)
                {
                    // rewrite ret type with actual value in case of specialized generic
                    auto returnType = getType(typeParameter, genContext);
                    funcProto->setReturnType(returnType);
                }
                else if (genContext.receiverFuncType)
                {
                    // rewrite ret type with actual value
                    auto &argTypeDestFuncType = genContext.receiverFuncType;
                    auto retTypeFromReceiver = mth.isAnyFunctionType(argTypeDestFuncType) 
                        ? mth.getReturnTypeFromFuncRef(argTypeDestFuncType)
                        : mlir::Type();
                    if (retTypeFromReceiver && !mth.isNoneType(retTypeFromReceiver))
                    {
                        funcProto->setReturnType(retTypeFromReceiver);
                        LLVM_DEBUG(llvm::dbgs()
                                       << "\n!! set return type from receiver: " << retTypeFromReceiver << "\n";);
                    }
                }

                // create funcType
                if (funcProto->getReturnType())
                {
                    funcType = getFunctionType(argTypes, funcProto->getReturnType(), funcProto->isMultiArgs());
                }
                else
                {
                    // no return type
                    funcType = getFunctionType(argTypes, std::nullopt, funcProto->isMultiArgs());
                }
            }
            else
            {
                // false result
                return std::make_tuple(funcOp, funcProto, mlir::failure(), false);
            }
        }
        else if (functionDiscovered)
        {
            funcType = functionDiscovered;
        }

        // we need it, when we run rediscovery second time
        if (!funcProto->getHasExtraFields())
        {
            funcProto->setHasExtraFields(existLocalVarsInThisContextMap(funcProto->getName()));
        }

        SmallVector<mlir::NamedAttribute> attrs;
#ifdef ADD_GC_ATTRIBUTE
        attrs.push_back({builder.getIdentifier(TS_GC_ATTRIBUTE), mlir::UnitAttr::get(builder.getContext())});
#endif
        // add decorations, "noinline, optnone"

        MLIRHelper::iterateDecorators(functionLikeDeclarationBaseAST, [&](std::string name, SmallVector<std::string> args) {
            if (isFuncAttr(name))
            {
                attrs.push_back({mlir::StringAttr::get(builder.getContext(), name), mlir::UnitAttr::get(builder.getContext())});
            }
        });

        // add modifiers
        auto dllExport = getExportModifier(functionLikeDeclarationBaseAST)
            || ((functionLikeDeclarationBaseAST->internalFlags & InternalFlags::DllExport) == InternalFlags::DllExport);
        if (dllExport)
        {
            attrs.push_back({mlir::StringAttr::get(builder.getContext(), "export"), mlir::UnitAttr::get(builder.getContext())});
            if (functionLikeDeclarationBaseAST == SyntaxKind::FunctionDeclaration
                || functionLikeDeclarationBaseAST == SyntaxKind::ArrowFunction)
            {
                //addDeclarationToExport(funcProto->getName(), funcType, genContext);
                addFunctionDeclarationToExport(functionLikeDeclarationBaseAST);
            }
        }

        auto dllImport = ((functionLikeDeclarationBaseAST->internalFlags & InternalFlags::DllImport) == InternalFlags::DllImport);
        if (dllImport)
        {
            attrs.push_back({mlir::StringAttr::get(builder.getContext(), "import"), mlir::UnitAttr::get(builder.getContext())});
        }

        auto it = getCaptureVarsMap().find(funcProto->getName());
        auto hasCapturedVars = funcProto->getHasCapturedVars() || (it != getCaptureVarsMap().end());
        if (hasCapturedVars)
        {
            // important set when it is discovered and in process second type
            funcProto->setHasCapturedVars(true);
            funcOp = mlir_ts::FuncOp::create(location, fullName, funcType, attrs);
        }
        else
        {
            funcOp = mlir_ts::FuncOp::create(location, fullName, funcType, attrs);
        }

        funcProto->setFuncType(funcType);

        if (!funcProto->getIsGeneric())
        {
            auto funcTypeIt = getFunctionTypeMap().find(fullName);
            if (funcTypeIt != getFunctionTypeMap().end())
            {
                getFunctionTypeMap().erase(funcTypeIt);
            }

            getFunctionTypeMap().insert({fullName, funcType});

            LLVM_DEBUG(llvm::dbgs() << "\n!! register func name: " << fullName << ", type: " << funcType << "\n";);
        }

        return std::make_tuple(funcOp, funcProto, mlir::success(), funcProto->getIsGeneric());
    }

    mlir::LogicalResult discoverFunctionReturnTypeAndCapturedVars(
        FunctionLikeDeclarationBase functionLikeDeclarationBaseAST, StringRef name, SmallVector<mlir::Type> &argTypes,
        const FunctionPrototypeDOM::TypePtr &funcProto, const GenContext &genContext)
    {
        if (funcProto->getDiscovered())
        {
            return mlir::failure();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! discovering 'ret type' & 'captured vars' for : " << name << "\n";);

        mlir::OpBuilder::InsertionGuard guard(builder);

        auto partialDeclFuncType = getFunctionType(argTypes, std::nullopt, false);
        auto dummyFuncOp = mlir_ts::FuncOp::create(loc(functionLikeDeclarationBaseAST), name, partialDeclFuncType);

        {
            // simulate scope
            SymbolTableScopeT varScope(symbolTable);

            llvm::ScopedHashTableScope<StringRef, VariableDeclarationDOM::TypePtr> 
                fullNameGlobalsMapScope(fullNameGlobalsMap);

            GenContext genContextWithPassResult{};
            genContextWithPassResult.funcOp = dummyFuncOp;
            genContextWithPassResult.thisType = genContext.thisType;
            genContextWithPassResult.allowPartialResolve = true;
            genContextWithPassResult.dummyRun = true;
            genContextWithPassResult.cleanUps = new SmallVector<mlir::Block *>();
            genContextWithPassResult.cleanUpOps = new SmallVector<mlir::Operation *>();
            genContextWithPassResult.passResult = new PassResult();
            genContextWithPassResult.state = new int(1);
            genContextWithPassResult.allocateVarsInContextThis =
                (functionLikeDeclarationBaseAST->internalFlags & InternalFlags::VarsInObjectContext) ==
                InternalFlags::VarsInObjectContext;
            genContextWithPassResult.discoverParamsOnly = genContext.discoverParamsOnly;
            genContextWithPassResult.typeAliasMap = genContext.typeAliasMap;
            genContextWithPassResult.typeParamsWithArgs = genContext.typeParamsWithArgs;

            registerNamespace(funcProto->getNameWithoutNamespace(), true);

            if (succeeded(mlirGenFunctionBody(functionLikeDeclarationBaseAST, dummyFuncOp, funcProto,
                                              genContextWithPassResult)))
            {
                exitNamespace();

                auto &passResult = genContextWithPassResult.passResult;
                if (passResult->functionReturnTypeShouldBeProvided 
                    && mth.isNoneType(passResult->functionReturnType))
                {
                    // has return value but type is not provided yet
                    genContextWithPassResult.clean();
                    return mlir::failure();
                }

                funcProto->setDiscovered(true);
                auto discoveredType = passResult->functionReturnType;
                if (discoveredType && discoveredType != funcProto->getReturnType())
                {
                    // TODO: do we need to convert it here? maybe send it as const object?

                    funcProto->setReturnType(mth.convertConstArrayTypeToArrayType(discoveredType));
                    LLVM_DEBUG(llvm::dbgs()
                                   << "\n!! ret type: " << funcProto->getReturnType() << ", name: " << name << "\n";);
                }

                // if we have captured parameters, add first param to send lambda's type(class)
                if (passResult->outerVariables.size() > 0)
                {
                    MLIRCodeLogic mcl(builder);
                    auto isObjectType =
                        genContext.thisType != nullptr && genContext.thisType.isa<mlir_ts::ObjectType>();
                    if (!isObjectType)
                    {
                        argTypes.insert(argTypes.begin(), mcl.CaptureType(passResult->outerVariables));
                    }

                    getCaptureVarsMap().insert({name, passResult->outerVariables});
                    funcProto->setHasCapturedVars(true);

                    LLVM_DEBUG(llvm::dbgs() << "\n!! has captured vars, name: " << name << "\n";);

                    LLVM_DEBUG(for (auto& var : passResult->outerVariables)
                    {
                        llvm::dbgs() << "\n!! ...captured var - name: " << var.second->getName() << ", type: " << var.second->getType() << "\n";
                    });
                }

                if (passResult->extraFieldsInThisContext.size() > 0)
                {
                    getLocalVarsInThisContextMap().insert({name, passResult->extraFieldsInThisContext});

                    funcProto->setHasExtraFields(true);
                }

                genContextWithPassResult.clean();
                return mlir::success();
            }
            else
            {
                exitNamespace();

                genContextWithPassResult.clean();
                return mlir::failure();
            }
        }
    }

    mlir::LogicalResult mlirGen(FunctionDeclaration functionDeclarationAST, const GenContext &genContext)
    {
        auto funcGenContext = GenContext(genContext);
        funcGenContext.clearScopeVars();
        // declaring function which is nested and object should not have this context (unless it is part of object declaration)
        if (!functionDeclarationAST->parent && funcGenContext.thisType != nullptr)
        {
            funcGenContext.thisType = nullptr;
        }

        mlir::OpBuilder::InsertionGuard guard(builder);
        auto res = mlirGenFunctionLikeDeclaration(functionDeclarationAST, funcGenContext);
        return std::get<0>(res);
    }

    ValueOrLogicalResult mlirGen(FunctionExpression functionExpressionAST, const GenContext &genContext)
    {
        auto location = loc(functionExpressionAST);
        mlir_ts::FuncOp funcOp;
        std::string funcName;
        bool isGeneric;

        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(theModule.getBody());

            // provide name for it
            auto funcGenContext = GenContext(genContext);
            funcGenContext.clearScopeVars();
            funcGenContext.thisType = nullptr;

            auto [result, funcOpRet, funcNameRet, isGenericRet] =
                mlirGenFunctionLikeDeclaration(functionExpressionAST, funcGenContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            funcOp = funcOpRet;
            funcName = funcNameRet;
            isGeneric = isGenericRet;
        }

        // if funcOp is null, means lambda is generic]
        if (!funcOp)
        {
            // return reference to generic method
            if (getGenericFunctionMap().count(funcName))
            {
                auto genericFunctionInfo = getGenericFunctionMap().lookup(funcName);
                // info: it will not take any capture now
                return resolveFunctionWithCapture(location, genericFunctionInfo->name, genericFunctionInfo->funcType,
                                                  mlir::Value(), true, genContext);
            }
            else
            {
                emitError(location) << "can't find generic function: " << funcName;
                return mlir::failure();
            }
        }

        return resolveFunctionWithCapture(location, funcOp.getName(), funcOp.getFunctionType(), mlir::Value(), false, genContext);
    }

    ValueOrLogicalResult mlirGen(ArrowFunction arrowFunctionAST, const GenContext &genContext)
    {
        auto location = loc(arrowFunctionAST);
        mlir_ts::FuncOp funcOp;
        std::string funcName;
        bool isGeneric;

        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(theModule.getBody());

            // provide name for it
            auto allowFuncGenContext = GenContext(genContext);
            allowFuncGenContext.clearScopeVars();
            // if we set it to value we will not capture 'this' references
            allowFuncGenContext.thisType = nullptr;
            auto [result, funcOpRet, funcNameRet, isGenericRet] =
                mlirGenFunctionLikeDeclaration(arrowFunctionAST, allowFuncGenContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            funcOp = funcOpRet;
            funcName = funcNameRet;
            isGeneric = isGenericRet;
        }

        // if funcOp is null, means lambda is generic]
        if (!funcOp)
        {
            // return reference to generic method
            if (getGenericFunctionMap().count(funcName))
            {
                auto genericFunctionInfo = getGenericFunctionMap().lookup(funcName);
                // info: it will not take any capture now
                return resolveFunctionWithCapture(location, genericFunctionInfo->name, genericFunctionInfo->funcType,
                                                  mlir::Value(), true, genContext);
            }
            else
            {
                emitError(location) << "can't find generic function: " << funcName;
                return mlir::failure();
            }
        }

        assert(funcOp);

        return resolveFunctionWithCapture(location, funcOp.getName(), funcOp.getFunctionType(), mlir::Value(), isGeneric, genContext);
    }

    std::tuple<mlir::LogicalResult, mlir_ts::FuncOp, std::string, bool> mlirGenFunctionGenerator(
        FunctionLikeDeclarationBase functionLikeDeclarationBaseAST, const GenContext &genContext)
    {
        auto location = loc(functionLikeDeclarationBaseAST);

        auto fixThisReference = functionLikeDeclarationBaseAST == SyntaxKind::MethodDeclaration;
        if (functionLikeDeclarationBaseAST->parameters.size() > 0)
        {
            auto nameNode = functionLikeDeclarationBaseAST->parameters.front()->name;
            if ((SyntaxKind)nameNode == SyntaxKind::Identifier)
            {
                auto ident = nameNode.as<Identifier>();
                if (ident->escapedText == S(THIS_NAME))
                {
                    fixThisReference = true;
                }
            }
        }
        
        NodeFactory nf(NodeFactoryFlags::None);

        auto stepIdent = nf.createIdentifier(S(GENERATOR_STEP));

        // create return object
        NodeArray<ObjectLiteralElementLike> generatorObjectProperties;

        // add step field
        auto stepProp = nf.createPropertyAssignment(stepIdent, nf.createNumericLiteral(S("0"), TokenFlags::None));
        generatorObjectProperties.push_back(stepProp);

        // create body of next method
        NodeArray<Statement> nextStatements;

        // add main switcher
        auto stepAccess = nf.createPropertyAccessExpression(nf.createToken(SyntaxKind::ThisKeyword), stepIdent);

        // call stateswitch
        auto callStat = nf.createExpressionStatement(
            nf.createCallExpression(nf.createIdentifier(S(GENERATOR_SWITCHSTATE)), undefined, {stepAccess}));

        nextStatements.push_back(callStat);

        // add function body to statements to first step
        if (functionLikeDeclarationBaseAST->body == SyntaxKind::Block)
        {
            // process every statement
            auto block = functionLikeDeclarationBaseAST->body.as<Block>();
            for (auto statement : block->statements)
            {
                nextStatements.push_back(statement);
            }
        }
        else
        {
            nextStatements.push_back(functionLikeDeclarationBaseAST->body);
        }

        // add next statements
        // add default return with empty
        nextStatements.push_back(
            nf.createReturnStatement(getYieldReturnObject(nf, location, nf.createIdentifier(S(UNDEFINED_NAME)), true)));

        // create next body
        auto nextBody = nf.createBlock(nextStatements, /*multiLine*/ false);

        // create method next in object
        auto nextMethodDecl =
            nf.createMethodDeclaration(undefined, undefined, nf.createIdentifier(S(ITERATOR_NEXT)), undefined,
                                       undefined, undefined, undefined, nextBody);
        nextMethodDecl->internalFlags |= InternalFlags::VarsInObjectContext;

        // copy location info, to fix issue with names of anonymous functions
        nextMethodDecl->pos = functionLikeDeclarationBaseAST->pos;
        nextMethodDecl->_end = functionLikeDeclarationBaseAST->_end;

        if (fixThisReference)
        {
            FilterVisitorSkipFuncsAST<Node> visitor(SyntaxKind::ThisKeyword, [&](auto thisNode) {
                thisNode->internalFlags |= InternalFlags::ThisArgAlias;
            });

            for (auto it = begin(nextStatements) + 1; it != end(nextStatements); ++it)
            {
                visitor.visit(*it);
            }
        }

        generatorObjectProperties.push_back(nextMethodDecl);

        auto generatorObject = nf.createObjectLiteralExpression(generatorObjectProperties, false);

        // copy location info, to fix issue with names of anonymous functions
        generatorObject->pos = functionLikeDeclarationBaseAST->pos;
        generatorObject->_end = functionLikeDeclarationBaseAST->_end;

        // generator body
        NodeArray<Statement> generatorStatements;

        // TODO: this is hack, adding this as thisArg alias
        if (fixThisReference)
        {
            // TODO: this is temp hack, add this alias as thisArg, 
            NodeArray<VariableDeclaration> _thisArgDeclarations;
            auto _thisArg = nf.createIdentifier(S(THIS_ALIAS));
            _thisArgDeclarations.push_back(nf.createVariableDeclaration(_thisArg, undefined, undefined, nf.createToken(SyntaxKind::ThisKeyword)));
            auto _thisArgList = nf.createVariableDeclarationList(_thisArgDeclarations, NodeFlags::Const);

            generatorStatements.push_back(nf.createVariableStatement(undefined, _thisArgList));
        }

        // step 1, add return object
        auto retStat = nf.createReturnStatement(generatorObject);
        generatorStatements.push_back(retStat);

        auto body = nf.createBlock(generatorStatements, /*multiLine*/ false);

        if (functionLikeDeclarationBaseAST == SyntaxKind::MethodDeclaration)
        {
            auto methodOp = nf.createMethodDeclaration(
                functionLikeDeclarationBaseAST->modifiers, undefined,
                functionLikeDeclarationBaseAST->name, undefined, functionLikeDeclarationBaseAST->typeParameters,
                functionLikeDeclarationBaseAST->parameters, functionLikeDeclarationBaseAST->type, body);

            // copy location info, to fix issue with names of anonymous functions
            methodOp->pos = functionLikeDeclarationBaseAST->pos;
            methodOp->_end = functionLikeDeclarationBaseAST->_end;        

            //LLVM_DEBUG(printDebug(methodOp););

            auto genMethodOp = mlirGenFunctionLikeDeclaration(methodOp, genContext);
            return genMethodOp;            
        }
        else
        {
            auto funcOp = nf.createFunctionDeclaration(
                functionLikeDeclarationBaseAST->modifiers, undefined,
                functionLikeDeclarationBaseAST->name, functionLikeDeclarationBaseAST->typeParameters,
                functionLikeDeclarationBaseAST->parameters, functionLikeDeclarationBaseAST->type, body);

            // copy location info, to fix issue with names of anonymous functions
            funcOp->pos = functionLikeDeclarationBaseAST->pos;
            funcOp->_end = functionLikeDeclarationBaseAST->_end;        

            //LLVM_DEBUG(printDebug(funcOp););

            auto genFuncOp = mlirGenFunctionLikeDeclaration(funcOp, genContext);
            return genFuncOp;
        }
    }

    std::pair<mlir::LogicalResult, std::string> registerGenericFunctionLike(
        FunctionLikeDeclarationBase functionLikeDeclarationBaseAST, bool ignoreFunctionArgsDetection,
        const GenContext &genContext)
    {
        auto [fullName, name] = getNameOfFunction(functionLikeDeclarationBaseAST, genContext);
        if (name.empty())
        {
            return {mlir::failure(), name};
        }

        if (existGenericFunctionMap(name))
        {
            return {mlir::success(), name};
        }

        llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
        if (mlir::failed(
                processTypeParameters(functionLikeDeclarationBaseAST->typeParameters, typeParameters, genContext)))
        {
            return {mlir::failure(), name};
        }

        // register class
        auto namePtr = StringRef(name).copy(stringAllocator);
        auto fullNamePtr = StringRef(fullName).copy(stringAllocator);
        GenericFunctionInfo::TypePtr newGenericFunctionPtr = std::make_shared<GenericFunctionInfo>();
        newGenericFunctionPtr->name = fullNamePtr;
        newGenericFunctionPtr->typeParams = typeParameters;
        newGenericFunctionPtr->functionDeclaration = functionLikeDeclarationBaseAST;
        newGenericFunctionPtr->elementNamespace = currentNamespace;

        // TODO: review it, ignore in case of ArrowFunction,
        if (!ignoreFunctionArgsDetection)
        {
            auto [result, funcOp] =
                getFuncArgTypesOfGenericMethod(functionLikeDeclarationBaseAST, typeParameters, false, genContext);
            if (mlir::failed(result))
            {
                return {mlir::failure(), name};
            }

            newGenericFunctionPtr->funcOp = funcOp;
            newGenericFunctionPtr->funcType = funcOp->getFuncType();

            LLVM_DEBUG(llvm::dbgs() << "\n!! registered generic function: " << name
                                    << ", type: " << funcOp->getFuncType() << "\n";);
        }

        getGenericFunctionMap().insert({namePtr, newGenericFunctionPtr});
        fullNameGenericFunctionsMap.insert(fullNamePtr, newGenericFunctionPtr);

        return {mlir::success(), name};
    }

    bool registerFunctionOp(FunctionPrototypeDOM::TypePtr funcProto, mlir_ts::FuncOp funcOp)
    {
        auto name = funcProto->getNameWithoutNamespace();
        if (!getFunctionMap().count(name))
        {
            getFunctionMap().insert({name, funcOp});

            LLVM_DEBUG(llvm::dbgs() << "\n!! reg. func: " << name << " type:" << funcOp.getFunctionType() << " function name: " << funcProto->getName()
                                    << " num inputs:" << funcOp.getFunctionType().cast<mlir_ts::FunctionType>().getNumInputs()
                                    << "\n";);

            return true;
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! re-reg. func: " << name << " type:" << funcOp.getFunctionType() << " function name: " << funcProto->getName()
                                << " num inputs:" << funcOp.getFunctionType().cast<mlir_ts::FunctionType>().getNumInputs()
                                << "\n";);

        return false;
    }

    std::tuple<mlir::LogicalResult, mlir_ts::FuncOp, std::string, bool> mlirGenFunctionLikeDeclaration(
        FunctionLikeDeclarationBase functionLikeDeclarationBaseAST, const GenContext &genContext)
    {
        auto isGenericFunction = functionLikeDeclarationBaseAST->typeParameters.size() > 0;
        if (isGenericFunction && genContext.typeParamsWithArgs.size() == 0)
        {
            auto [result, name] = registerGenericFunctionLike(functionLikeDeclarationBaseAST, false, genContext);
            return {result, mlir_ts::FuncOp(), name, false};
        }

        // check if it is generator
        if (functionLikeDeclarationBaseAST->asteriskToken)
        {
            // this is generator, let's generate other function out of it
            return mlirGenFunctionGenerator(functionLikeDeclarationBaseAST, genContext);
        }

        // do not process generic functions more then 1 time
        if (isGenericFunction && genContext.typeParamsWithArgs.size() > 0)
        {
            auto [fullFunctionName, functionName] = getNameOfFunction(functionLikeDeclarationBaseAST, genContext);

            auto funcOp = lookupFunctionMap(functionName);
            if (funcOp && theModule.lookupSymbol(functionName))
            {
                return {mlir::success(), funcOp, functionName, false};
            }
        }

        // go to root
        mlir::OpBuilder::InsertPoint savePoint;
        if (isGenericFunction)
        {
            savePoint = builder.saveInsertionPoint();
            builder.setInsertionPointToStart(theModule.getBody());
        }

        auto location = loc(functionLikeDeclarationBaseAST);

        auto [funcOp, funcProto, result, isGeneric] =
            mlirGenFunctionPrototype(functionLikeDeclarationBaseAST, genContext);
        if (mlir::failed(result))
        {
            // in case of ArrowFunction without params and receiver is generic function as well
            return {result, funcOp, "", false};
        }

        if (mlir::succeeded(result) && isGeneric)
        {
            auto [result, name] = registerGenericFunctionLike(functionLikeDeclarationBaseAST, true, genContext);
            return {result, funcOp, funcProto->getName().str(), isGeneric};
        }

        // check decorator for class
        auto dynamicImport = false;
        MLIRHelper::iterateDecorators(functionLikeDeclarationBaseAST, [&](std::string name, SmallVector<std::string> args) {
            if (name == DLL_IMPORT && args.size() > 0)
            {
                dynamicImport = true;
            }
        });

        if (dynamicImport)
        {
            // TODO: we do not need to register funcOp as we need to reference global variables
            auto result = mlirGenFunctionLikeDeclarationDynamicImport(location, funcOp, funcProto->getNameWithoutNamespace(), genContext);
            return {result, funcOp, funcProto->getName().str(), false};
        }

        auto funcGenContext = GenContext(genContext);
        funcGenContext.clearScopeVars();
        funcGenContext.funcOp = funcOp;
        funcGenContext.state = new int(1);
        // if funcGenContext.passResult is null and allocateVarsInContextThis is true, this type should contain fully
        // defined object with local variables as fields
        funcGenContext.allocateVarsInContextThis =
            (functionLikeDeclarationBaseAST->internalFlags & InternalFlags::VarsInObjectContext) ==
            InternalFlags::VarsInObjectContext;

        auto it = getCaptureVarsMap().find(funcProto->getName());
        if (it != getCaptureVarsMap().end())
        {
            funcGenContext.capturedVars = &it->getValue();

            LLVM_DEBUG(llvm::dbgs() << "\n!! func has captured vars: " << funcProto->getName() << "\n";);
        }
        else
        {
            assert(funcGenContext.capturedVars == nullptr);
        }

        // register function to be able to call it if used in recursive call
        registerFunctionOp(funcProto, funcOp);

        // generate body
        auto resultFromBody = mlir::failure();
        {
            MLIRNamespaceGuard nsGuard(currentNamespace);
            registerNamespace(funcProto->getNameWithoutNamespace(), true);

            SymbolTableScopeT varScope(symbolTable);
            resultFromBody = mlirGenFunctionBody(functionLikeDeclarationBaseAST, funcOp, funcProto, funcGenContext);
        }

        funcGenContext.cleanState();

        if (mlir::failed(resultFromBody))
        {
            return {mlir::failure(), funcOp, "", false};
        }

        // set visibility index
        auto hasExport = getExportModifier(functionLikeDeclarationBaseAST)
            || ((functionLikeDeclarationBaseAST->internalFlags & InternalFlags::DllExport) == InternalFlags::DllExport);
        if (!hasExport && funcProto->getName() != MAIN_ENTRY_NAME)
        {
            funcOp.setPrivate();
        }

        if (declarationMode && !genContext.dummyRun && funcProto->getNoBody())
        {
            funcOp.setPrivate();
        }

        if (!genContext.dummyRun)
        {
            theModule.push_back(funcOp);
        }

        if (isGenericFunction)
        {
            builder.restoreInsertionPoint(savePoint);
        }
        else
        {
            builder.setInsertionPointAfter(funcOp);
        }

        return {mlir::success(), funcOp, funcProto->getName().str(), false};
    }

    mlir::LogicalResult mlirGenFunctionLikeDeclarationDynamicImport(mlir::Location location, mlir_ts::FuncOp funcOp, StringRef dllFuncName, const GenContext &genContext)
    {
        registerVariable(location, funcOp.getName(), true, VariableType::Var,
            [&](mlir::Location location, const GenContext &context) -> TypeValueInitType {
                // add command to load reference fron DLL
                auto fullName = V(mlirGenStringValue(location, dllFuncName.str(), true));
                auto referenceToFuncOpaque = builder.create<mlir_ts::SearchForAddressOfSymbolOp>(location, getOpaqueType(), fullName);
                auto result = cast(location, funcOp.getFunctionType(), referenceToFuncOpaque, genContext);
                auto referenceToFunc = V(result);
                return {referenceToFunc.getType(), referenceToFunc, TypeProvided::No};
            },
            genContext);

        return mlir::success();
    }    

    mlir::LogicalResult mlirGenFunctionEntry(mlir::Location location, FunctionPrototypeDOM::TypePtr funcProto,
                                             const GenContext &genContext)
    {
        return mlirGenFunctionEntry(location, funcProto->getReturnType(), genContext);
    }

    mlir::LogicalResult mlirGenFunctionEntry(mlir::Location location, mlir::Type retType, const GenContext &genContext)
    {
        auto hasReturn = retType && !retType.isa<mlir_ts::VoidType>();
        if (hasReturn)
        {
            auto entryOp = builder.create<mlir_ts::EntryOp>(location, mlir_ts::RefType::get(retType));
            auto varDecl = std::make_shared<VariableDeclarationDOM>(RETURN_VARIABLE_NAME, retType, location);
            varDecl->setReadWriteAccess();
            DECLARE(varDecl, entryOp.getReference());
        }
        else
        {
            builder.create<mlir_ts::EntryOp>(location, mlir::Type());
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionExit(mlir::Location location, const GenContext &genContext)
    {
        auto callableResult = const_cast<GenContext &>(genContext).funcOp.getCallableResults();
        auto retType = callableResult.size() > 0 ? callableResult.front() : mlir::Type();
        auto hasReturn = retType && !retType.isa<mlir_ts::VoidType>();
        if (hasReturn)
        {
            auto retVarInfo = symbolTable.lookup(RETURN_VARIABLE_NAME);
            if (!retVarInfo.second)
            {
                if (genContext.allowPartialResolve)
                {
                    return mlir::success();
                }

                emitError(location) << "can't find return variable";
                return mlir::failure();
            }

            builder.create<mlir_ts::ExitOp>(location, retVarInfo.first);
        }
        else
        {
            builder.create<mlir_ts::ExitOp>(location, mlir::Value());
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionCapturedParam(mlir::Location location, int &firstIndex,
                                                     FunctionPrototypeDOM::TypePtr funcProto,
                                                     mlir::Block::BlockArgListType arguments,
                                                     const GenContext &genContext)
    {
        if (genContext.capturedVars == nullptr)
        {
            return mlir::success();
        }

        auto isObjectType = genContext.thisType != nullptr && genContext.thisType.isa<mlir_ts::ObjectType>();
        if (isObjectType)
        {
            return mlir::success();
        }

        firstIndex++;

        auto capturedParam = arguments[firstIndex];
        auto capturedRefType = capturedParam.getType();

        auto capturedParamVar = std::make_shared<VariableDeclarationDOM>(CAPTURED_NAME, capturedRefType, location);

        DECLARE(capturedParamVar, capturedParam);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionCapturedParamIfObject(mlir::Location location, int &firstIndex,
                                                             FunctionPrototypeDOM::TypePtr funcProto,
                                                             mlir::Block::BlockArgListType arguments,
                                                             const GenContext &genContext)
    {
        if (genContext.capturedVars == nullptr)
        {
            return mlir::success();
        }

        auto isObjectType = genContext.thisType != nullptr && genContext.thisType.isa<mlir_ts::ObjectType>();
        if (isObjectType)
        {

            auto thisVal = resolveIdentifier(location, THIS_NAME, genContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! this value: " << thisVal << "\n";);

            auto capturedNameResult =
                mlirGenPropertyAccessExpression(location, thisVal, MLIRHelper::TupleFieldName(CAPTURED_NAME, builder.getContext()), genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(capturedNameResult)

            mlir::Value propValue = V(capturedNameResult);

            LLVM_DEBUG(llvm::dbgs() << "\n!! this->.captured value: " << propValue << "\n";);

            assert(propValue);

            // captured is in this->".captured"
            auto capturedParamVar = std::make_shared<VariableDeclarationDOM>(CAPTURED_NAME, propValue.getType(), location);
            DECLARE(capturedParamVar, propValue);
        }

        return mlir::success();
    }

    // TODO: put into MLIRCodeLogicHelper
    ValueOrLogicalResult optionalValueOrUndefinedExpression(mlir::Location location, mlir::Value condValue, Expression expression, const GenContext &genContext)
    {
        return optionalValueOrUndefined(location, condValue, [&](auto genContext) { return mlirGen(expression, genContext); }, genContext);
    }

    // TODO: put into MLIRCodeLogicHelper
    ValueOrLogicalResult optionalValueOrUndefined(mlir::Location location, mlir::Value condValue, 
        std::function<ValueOrLogicalResult(const GenContext &)> exprFunc, const GenContext &genContext)
    {
        return conditionalValue(location, condValue, 
            [&](auto genContext) { 
                auto result = exprFunc(genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto value = V(result);
                auto optValue = 
                    value.getType().isa<mlir_ts::OptionalType>()
                        ? value
                        : builder.create<mlir_ts::OptionalValueOp>(location, getOptionalType(value.getType()), value);
                return ValueOrLogicalResult(optValue); 
            }, 
            [&](mlir::Type trueValueType, auto genContext) { 
                auto optUndefValue = builder.create<mlir_ts::OptionalUndefOp>(location, trueValueType);
                return ValueOrLogicalResult(optUndefValue); 
            }, 
            genContext);
    }

    // TODO: put into MLIRCodeLogicHelper
    ValueOrLogicalResult anyOrUndefined(mlir::Location location, mlir::Value condValue, 
        std::function<ValueOrLogicalResult(const GenContext &)> exprFunc, const GenContext &genContext)
    {
        return conditionalValue(location, condValue, 
            [&](auto genContext) { 
                auto result = exprFunc(genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto value = V(result);
                auto anyValue = V(builder.create<mlir_ts::CastOp>(location, getAnyType(), value));
                return ValueOrLogicalResult(anyValue); 
            }, 
            [&](mlir::Type trueValueType, auto genContext) {
                auto undefValue = builder.create<mlir_ts::UndefOp>(location, getUndefinedType());
                auto anyUndefValue = V(builder.create<mlir_ts::CastOp>(location, trueValueType, undefValue));
                return ValueOrLogicalResult(anyUndefValue); 
            }, 
            genContext);
    }

    // TODO: put into MLIRCodeLogicHelper
    // TODO: we have a lot of IfOp - create 1 logic for conditional values
    ValueOrLogicalResult conditionalValue(mlir::Location location, mlir::Value condValue, 
        std::function<ValueOrLogicalResult(const GenContext &)> trueValue, 
        std::function<ValueOrLogicalResult(mlir::Type trueValueType, const GenContext &)> falseValue, 
        const GenContext &genContext)
    {
        // type will be set later
        auto ifOp = builder.create<mlir_ts::IfOp>(location, builder.getNoneType(), condValue, true);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());

        // value if true
        auto trueResult = trueValue(genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(trueResult)
        ifOp.getResults().front().setType(trueResult.value.getType());
        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{trueResult});

        // else
        builder.setInsertionPointToStart(&ifOp.getElseRegion().front());

        // value if false
        auto falseResult = falseValue(trueResult.value.getType(), genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(falseResult)
        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{falseResult});

        builder.setInsertionPointAfter(ifOp);

        return ValueOrLogicalResult(ifOp.getResults().front());        
    }    

    // TODO: put into MLIRCodeLogicHelper
    ValueOrLogicalResult optionalValueOrDefault(mlir::Location location, mlir::Type dataType, mlir::Value value, Expression defaultExpr, const GenContext &genContext)
    {
        auto optionalValueOrDefaultOp = builder.create<mlir_ts::OptionalValueOrDefaultOp>(
            location, dataType, value);

        /*auto *defValueBlock =*/builder.createBlock(&optionalValueOrDefaultOp.getDefaultValueRegion());

        mlir::Value defaultValue;
        if (defaultExpr)
        {
            defaultValue = mlirGen(defaultExpr, genContext);
        }
        else
        {
            llvm_unreachable("unknown statement");
        }

        if (defaultValue.getType() != dataType)
        {
            CAST(defaultValue, location, dataType, defaultValue, genContext);
        }

        builder.create<mlir_ts::ResultOp>(location, defaultValue);

        builder.setInsertionPointAfter(optionalValueOrDefaultOp);

        return V(optionalValueOrDefaultOp);
    } 

    ValueOrLogicalResult processOptionalParam(mlir::Location location, mlir::Type dataType, mlir::Value value, Expression defaultExpr, const GenContext &genContext)
    {
        auto paramOptionalOp = builder.create<mlir_ts::ParamOptionalOp>(
            location, mlir_ts::RefType::get(dataType), value, builder.getBoolAttr(false));

        /*auto *defValueBlock =*/builder.createBlock(&paramOptionalOp.getDefaultValueRegion());

        mlir::Value defaultValue;
        if (defaultExpr)
        {
            defaultValue = mlirGen(defaultExpr, genContext);
        }
        else
        {
            llvm_unreachable("unknown statement");
        }

        if (defaultValue.getType() != dataType)
        {
            CAST(defaultValue, location, dataType, defaultValue, genContext);
        }

        builder.create<mlir_ts::ParamDefaultValueOp>(location, defaultValue);

        builder.setInsertionPointAfter(paramOptionalOp);

        return V(paramOptionalOp);
    }    

    mlir::LogicalResult mlirGenFunctionParams(int firstIndex, FunctionPrototypeDOM::TypePtr funcProto,
                                              mlir::Block::BlockArgListType arguments, const GenContext &genContext)
    {
        auto index = firstIndex;
        for (const auto &param : funcProto->getParams())
        {
            index++;
            mlir::Value paramValue;

            // process init expression
            auto location = param->getLoc();

            // alloc all args
            // process optional parameters
            if (param->hasInitValue())
            {
                auto result = processOptionalParam(location, param->getType(), arguments[index], param->getInitValue(), genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                paramValue = V(result);
            }
            else if (param->getIsOptional() && !param->getType().isa<mlir_ts::OptionalType>())
            {
                auto optType = getOptionalType(param->getType());
                param->setType(optType);
                paramValue = builder.create<mlir_ts::ParamOp>(location, mlir_ts::RefType::get(optType),
                                                              arguments[index], builder.getBoolAttr(false));
            }
            else
            {
                paramValue = builder.create<mlir_ts::ParamOp>(location, mlir_ts::RefType::get(param->getType()),
                                                              arguments[index], builder.getBoolAttr(false));
            }

            if (paramValue)
            {
                // redefine variable
                param->setReadWriteAccess();
                DECLARE(param, paramValue);
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionParams(mlir::Location location, int firstIndex, mlir::Block::BlockArgListType arguments, const GenContext &genContext)
    {
        for (auto index = firstIndex; index < arguments.size(); index++)
        {
            std::string paramName("p");
            paramName += std::to_string(index - firstIndex);
            
            auto paramDecl = std::make_shared<VariableDeclarationDOM>(paramName, arguments[index].getType(), location);
            
            /*
            mlir::Value paramValue = builder.create<mlir_ts::ParamOp>(location, mlir_ts::RefType::get(arguments[index].getType()),
                                                              arguments[index], builder.getBoolAttr(false));
            paramDecl->setReadWriteAccess();
            
            DECLARE(paramDecl, paramValue, genContext, true);
            */
            DECLARE(paramDecl, arguments[index]);
        }

        return mlir::success();
    }    

    mlir::LogicalResult mlirGenFunctionParamsBindings(int firstIndex, FunctionPrototypeDOM::TypePtr funcProto,
                                                      mlir::Block::BlockArgListType arguments,
                                                      const GenContext &genContext)
    {
        for (const auto &param : funcProto->getParams())
        {
            if (auto bindingPattern = param->getBindingPattern())
            {
                auto location = loc(bindingPattern);
                auto val = resolveIdentifier(location, param->getName(), genContext);
                assert(val);
                auto initFunc = [&](mlir::Location, const GenContext &) { return std::make_tuple(val.getType(), val, TypeProvided::No); };

                if (bindingPattern == SyntaxKind::ArrayBindingPattern)
                {
                    auto arrayBindingPattern = bindingPattern.as<ArrayBindingPattern>();
                    if (mlir::failed(processDeclarationArrayBindingPattern(location, arrayBindingPattern, VariableType::Let,
                                                               initFunc, genContext)))
                    {
                        return mlir::failure();
                    }
                }
                else if (bindingPattern == SyntaxKind::ObjectBindingPattern)
                {
                    auto objectBindingPattern = bindingPattern.as<ObjectBindingPattern>();
                    if (mlir::failed(processDeclarationObjectBindingPattern(location, objectBindingPattern, VariableType::Let,
                                                                initFunc, genContext)))
                    {
                        return mlir::failure();
                    }
                }
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionCaptures(mlir::Location location, FunctionPrototypeDOM::TypePtr funcProto, const GenContext &genContext)
    {
        if (genContext.capturedVars == nullptr)
        {
            return mlir::success();
        }

        auto capturedVars = *genContext.capturedVars;

        NodeFactory nf(NodeFactoryFlags::None);

        // create variables
        for (auto &capturedVar : capturedVars)
        {
            auto varItem = capturedVar.getValue();
            auto variableInfo = varItem;
            auto name = variableInfo->getName();

            // load this.<var name>
            auto _captured = nf.createIdentifier(stows(CAPTURED_NAME));
            auto _name = nf.createIdentifier(stows(std::string(name)));
            auto _captured_name = nf.createPropertyAccessExpression(_captured, _name);
            auto result = mlirGen(_captured_name, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto capturedVarValue = V(result);
            auto variableRefType = mlir_ts::RefType::get(variableInfo->getType());

            auto capturedParam =
                std::make_shared<VariableDeclarationDOM>(name, variableRefType, variableInfo->getLoc());
            assert(capturedVarValue);
            if (capturedVarValue.getType().isa<mlir_ts::RefType>())
            {
                capturedParam->setReadWriteAccess();
            }

            LLVM_DEBUG(dbgs() << "\n!! captured '\".captured\"->" << name << "' [ " << capturedVarValue
                              << " ] ref val type: [ " << variableRefType << " ]");

            DECLARE(capturedParam, capturedVarValue);
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionBody(FunctionLikeDeclarationBase functionLikeDeclarationBaseAST,
                                            mlir_ts::FuncOp funcOp, FunctionPrototypeDOM::TypePtr funcProto,
                                            const GenContext &genContext)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! >>>> FUNCTION: '" << funcProto->getName() << "' ~~~ dummy run: " << genContext.dummyRun << " & allowed partial resolve: " << genContext.allowPartialResolve << "\n";);

        if (!functionLikeDeclarationBaseAST->body || declarationMode && !genContext.dummyRun)
        {
            // it is just declaration
            funcProto->setNoBody(true);
            return mlir::success();
        }

        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(functionLikeDeclarationBaseAST);

        auto *blockPtr = funcOp.addEntryBlock();
        auto &entryBlock = *blockPtr;

        builder.setInsertionPointToStart(&entryBlock);

        auto arguments = entryBlock.getArguments();
        auto firstIndex = -1;

        // add exit code
        if (failed(mlirGenFunctionEntry(location, funcProto, genContext)))
        {
            return mlir::failure();
        }

        // register this if lambda function
        if (failed(mlirGenFunctionCapturedParam(location, firstIndex, funcProto, arguments, genContext)))
        {
            return mlir::failure();
        }

        // allocate function parameters as variable
        if (failed(mlirGenFunctionParams(firstIndex, funcProto, arguments, genContext)))
        {
            return mlir::failure();
        }

        if (failed(mlirGenFunctionParamsBindings(firstIndex, funcProto, arguments, genContext)))
        {
            return mlir::failure();
        }

        if (failed(mlirGenFunctionCapturedParamIfObject(location, firstIndex, funcProto, arguments, genContext)))
        {
            return mlir::failure();
        }

        if (failed(mlirGenFunctionCaptures(location, funcProto, genContext)))
        {
            return mlir::failure();
        }

        // if we need params only we do not need to process body
        auto discoverParamsOnly = genContext.allowPartialResolve && genContext.discoverParamsOnly;
        if (!discoverParamsOnly)
        {
            if (failed(mlirGenBody(functionLikeDeclarationBaseAST->body, genContext)))
            {
                return mlir::failure();
            }
        }

        // add exit code
        if (failed(mlirGenFunctionExit(location, genContext)))
        {
            return mlir::failure();
        }

        if (genContext.dummyRun && genContext.cleanUps)
        {
            genContext.cleanUps->push_back(blockPtr);
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! >>>> FUNCTION (SUCCESS END): '" << funcProto->getName() << "' ~~~ dummy run: " << genContext.dummyRun << " & allowed partial resolve: " << genContext.allowPartialResolve << "\n";);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenFunctionBody(mlir::Location location, StringRef fullFuncName,
                                            mlir_ts::FunctionType funcType, std::function<mlir::LogicalResult(const GenContext &genContext)> funcBody,                                            
                                            const GenContext &genContext,
                                            int firstParam = 0)
    {
        if (theModule.lookupSymbol(fullFuncName))
        {
            return mlir::success();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! >>>> SYNTH. FUNCTION: '" << fullFuncName << "' is dummy run: " << genContext.dummyRun << " << allowed partial resolve: " << genContext.allowPartialResolve << "\n";);

        SymbolTableScopeT varScope(symbolTable);

        auto funcOp = mlir_ts::FuncOp::create(location, fullFuncName, funcType);

        GenContext funcGenContext(genContext);
        funcGenContext.funcOp = funcOp;

        auto *blockPtr = funcOp.addEntryBlock();
        auto &entryBlock = *blockPtr;

        builder.setInsertionPointToStart(&entryBlock);

        auto arguments = entryBlock.getArguments();

        // add exit code
        if (failed(mlirGenFunctionEntry(location, mth.getReturnTypeFromFuncRef(funcType), funcGenContext)))
        {
            return mlir::failure();
        }

        if (failed(mlirGenFunctionParams(location, firstParam, arguments, funcGenContext)))
        {
            return mlir::failure();
        }

        if (failed(funcBody(funcGenContext)))
        {
            return mlir::failure();
        }

        // add exit code
        auto retVarInfo = symbolTable.lookup(RETURN_VARIABLE_NAME);
        if (retVarInfo.first)
        {
            builder.create<mlir_ts::ExitOp>(location, retVarInfo.first);
        }
        else
        {
            builder.create<mlir_ts::ExitOp>(location, mlir::Value());
        }

        if (genContext.dummyRun)
        {
            if (genContext.cleanUps)
            {
                genContext.cleanUps->push_back(blockPtr);
            }
        }
        else
        {
            theModule.push_back(funcOp);
        }

        funcOp.setPrivate();

        LLVM_DEBUG(llvm::dbgs() << "\n!! >>>> SYNTH. FUNCTION (SUCCESS END): '" << fullFuncName << "' is dummy run: " << funcGenContext.dummyRun << " << allowed partial resolve: " << funcGenContext.allowPartialResolve << "\n";);

        return mlir::success();
    }

    ValueOrLogicalResult mlirGen(TypeAssertion typeAssertionAST, const GenContext &genContext)
    {
        auto location = loc(typeAssertionAST);

        auto typeInfo = getType(typeAssertionAST->type, genContext);
        if (!typeInfo)
        {
            return mlir::failure();
        }

        GenContext noReceiverGenContext(genContext);
        noReceiverGenContext.clearReceiverTypes();
        noReceiverGenContext.receiverType = typeInfo;

        auto result = mlirGen(typeAssertionAST->expression, noReceiverGenContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto exprValue = V(result);

        CAST_A(castedValue, location, typeInfo, exprValue, genContext);
        return castedValue;
    }

    ValueOrLogicalResult mlirGen(AsExpression asExpressionAST, const GenContext &genContext)
    {
        auto location = loc(asExpressionAST);

        auto typeInfo = getType(asExpressionAST->type, genContext);
        if (!typeInfo)
        {
            return mlir::failure();
        }

        GenContext noReceiverGenContext(genContext);
        noReceiverGenContext.clearReceiverTypes();
        noReceiverGenContext.receiverType = typeInfo;

        auto result = mlirGen(asExpressionAST->expression, noReceiverGenContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto exprValue = V(result);

        CAST_A(castedValue, location, typeInfo, exprValue, genContext);
        return castedValue;
    }

    ValueOrLogicalResult mlirGen(ComputedPropertyName computedPropertyNameAST, const GenContext &genContext)
    {
        auto result = mlirGen(computedPropertyNameAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto exprValue = V(result);
        return exprValue;
    }

    mlir::LogicalResult mlirGen(ReturnStatement returnStatementAST, const GenContext &genContext)
    {
        auto location = loc(returnStatementAST);
        if (auto expression = returnStatementAST->expression)
        {
            GenContext receiverTypeGenContext(genContext);
            receiverTypeGenContext.clearReceiverTypes();
            auto exactReturnType = getExplicitReturnTypeOfCurrentFunction(genContext);
            if (exactReturnType)
            {
                receiverTypeGenContext.receiverType = exactReturnType;
            }

            auto result = mlirGen(expression, receiverTypeGenContext);
            EXIT_IF_FAILED(result)
            
            auto expressionValue = V(result);
            if (!expressionValue)
            {
                emitError(location, "No return value");
            }

            VALIDATE(expressionValue, location)

            EXIT_IF_FAILED(mlirGenDisposable(location, DisposeDepth::FullStack, {}, &genContext));

            return mlirGenReturnValue(location, expressionValue, false, genContext);
        }

        EXIT_IF_FAILED(mlirGenDisposable(location, DisposeDepth::FullStack, {}, &genContext));

        builder.create<mlir_ts::ReturnOp>(location);
        return mlir::success();
    }

    ObjectLiteralExpression getYieldReturnObject(NodeFactory &nf, mlir::Location location, Expression expr, bool stop)
    {
        auto valueIdent = nf.createIdentifier(S("value"));
        auto doneIdent = nf.createIdentifier(S("done"));

        NodeArray<ObjectLiteralElementLike> retObjectProperties;
        auto valueProp = nf.createPropertyAssignment(valueIdent, expr);
        retObjectProperties.push_back(valueProp);

        auto doneProp = nf.createPropertyAssignment(
            doneIdent, nf.createToken(stop ? SyntaxKind::TrueKeyword : SyntaxKind::FalseKeyword));
        retObjectProperties.push_back(doneProp);

        auto retObject = nf.createObjectLiteralExpression(retObjectProperties, stop);
        
        // copy location info, to fix issue with names of anonymous functions
        auto [pos, _end] = getPos(location);

        assert(pos != _end && pos > 0);

        retObject->pos = pos;
        retObject->_end = _end;        

        return retObject;
    };

    ValueOrLogicalResult mlirGenYieldStar(YieldExpression yieldExpressionAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _v_ident = nf.createIdentifier(S(".v"));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        auto _yield_expr = nf.createYieldExpression(undefined, _v_ident);
        // copy location info, to fix issue with names of anonymous functions
        _yield_expr->pos = yieldExpressionAST->pos;
        _yield_expr->_end = yieldExpressionAST->_end;

        auto forOfStat =
            nf.createForOfStatement(undefined, declList, yieldExpressionAST->expression,
                                    nf.createExpressionStatement(_yield_expr));

        return mlirGen(forOfStat, genContext);
    }

    ValueOrLogicalResult mlirGen(YieldExpression yieldExpressionAST, const GenContext &genContext)
    {
        if (yieldExpressionAST->asteriskToken)
        {
            return mlirGenYieldStar(yieldExpressionAST, genContext);
        }

        auto location = loc(yieldExpressionAST);

        if (genContext.passResult)
        {
            genContext.passResult->functionReturnTypeShouldBeProvided = true;
        }

        // get state
        auto state = 0;
        if (genContext.state)
        {
            state = (*genContext.state)++;
        }
        else
        {
            assert(false);
        }

        // set restore point (return point)
        stringstream num;
        num << state;

        NodeFactory nf(NodeFactoryFlags::None);

        if (evaluateProperty(nf.createToken(SyntaxKind::ThisKeyword), GENERATOR_STEP, genContext))
        {
            // save return point - state -> this.step = xxx
            auto setStateExpr = nf.createBinaryExpression(
                nf.createPropertyAccessExpression(nf.createToken(SyntaxKind::ThisKeyword), nf.createIdentifier(S(GENERATOR_STEP))),
                nf.createToken(SyntaxKind::EqualsToken), nf.createNumericLiteral(num.str(), TokenFlags::None));
            mlirGen(setStateExpr, genContext);
        }
        else
        {
            // save return point - state -> step = xxx
            auto setStateExpr = nf.createBinaryExpression(
                nf.createIdentifier(S(GENERATOR_STEP)),
                nf.createToken(SyntaxKind::EqualsToken), nf.createNumericLiteral(num.str(), TokenFlags::None));
            mlirGen(setStateExpr, genContext);
        }

        // return value
        auto yieldRetValue = getYieldReturnObject(nf, location, yieldExpressionAST->expression, false);
        auto result = mlirGen(yieldRetValue, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto yieldValue = V(result);

        mlirGenReturnValue(location, yieldValue, true, genContext);

        std::stringstream label;
        label << GENERATOR_STATELABELPREFIX << state;
        builder.create<mlir_ts::StateLabelOp>(location, label.str());

        // TODO: yield value to continue, should be loaded from "next(value)" parameter
        // return yieldValue;
        return mlir::success();
    }

    ValueOrLogicalResult mlirGen(AwaitExpression awaitExpressionAST, const GenContext &genContext)
    {
#ifdef ENABLE_ASYNC
        auto location = loc(awaitExpressionAST);

        auto resultType = evaluate(awaitExpressionAST->expression, genContext);

        ValueOrLogicalResult result(mlir::failure());
        auto asyncExecOp = builder.create<mlir::async::ExecuteOp>(
            location, resultType ? mlir::TypeRange{resultType} : mlir::TypeRange(), mlir::ValueRange{},
            mlir::ValueRange{}, [&](mlir::OpBuilder &builder, mlir::Location location, mlir::ValueRange values) {
                SmallVector<mlir::Type, 0> types;
                SmallVector<mlir::Value, 0> operands;
                if (resultType)
                {
                    types.push_back(resultType);
                }

                result = mlirGen(awaitExpressionAST->expression, genContext);
                if (result)
                {
                    auto value = V(result);
                    if (value)
                    {
                        builder.create<mlir::async::YieldOp>(location, mlir::ValueRange{value});
                    }
                    else
                    {
                        builder.create<mlir::async::YieldOp>(location, mlir::ValueRange{});
                    }
                }
            });
        EXIT_IF_FAILED_OR_NO_VALUE(result)

        if (resultType)
        {
            auto asyncAwaitOp = builder.create<mlir::async::AwaitOp>(location, asyncExecOp.getResults().back());
            return asyncAwaitOp.getResult();
        }
        else
        {
            auto asyncAwaitOp = builder.create<mlir::async::AwaitOp>(location, asyncExecOp.getToken());
        }

        return mlir::success();
#else
        return mlirGen(awaitExpressionAST->expression, genContext);
#endif
    }

    mlir::LogicalResult processReturnType(mlir::Value expressionValue, const GenContext &genContext)
    {
        // TODO: rewrite it using UnionType

        // record return type if not provided
        if (genContext.passResult)
        {
            if (!expressionValue)
            {
                return mlir::failure();
            }

            auto type = expressionValue.getType();
            LLVM_DEBUG(dbgs() << "\n!! processing return type: " << type << "");

            if (mth.isNoneType(type))
            {
                return mlir::success();
            }

            type = mth.wideStorageType(type);

            // if return type is not detected, take first and exit
            if (!genContext.passResult->functionReturnType)
            {
                genContext.passResult->functionReturnType = type;
                return mlir::success();
            }

            // TODO: undefined & null should be processed as union type
            auto undefType = getUndefinedType();
            auto nullType = getNullType();

            // filter out types, such as: undefined, objects with undefined values etc
            if (type == undefType || type == nullType)
            {
                return mlir::failure();
            }

            // if (mth.hasUndefines(type))
            // {
            //     return mlir::failure();
            // }

            auto merged = false;
            auto resultReturnType = mth.mergeType(genContext.passResult->functionReturnType, type, merged);            

            LLVM_DEBUG(dbgs() << "\n!! return type: " << resultReturnType << "");

            genContext.passResult->functionReturnType = resultReturnType;
        }

        return mlir::success();
    }

    mlir::Type getExplicitReturnTypeOfCurrentFunction(const GenContext &genContext)
    {
        auto funcOp = const_cast<GenContext &>(genContext).funcOp;
        if (funcOp)
        {
            auto countResults = funcOp.getCallableResults().size();
            if (countResults > 0)
            {
                auto returnType = funcOp.getCallableResults().front();
                return returnType;
            }
        }

        return mlir::Type();
    }

    mlir::LogicalResult mlirGenReturnValue(mlir::Location location, mlir::Value expressionValue, bool yieldReturn,
                                           const GenContext &genContext)
    {
        if (genContext.passResult)
        {
            genContext.passResult->functionReturnTypeShouldBeProvided = true;
        }

        if (auto returnType = getExplicitReturnTypeOfCurrentFunction(genContext))
        {
            if (!expressionValue)
            {
                if (!genContext.allowPartialResolve)
                {
                    emitError(location) << "'return' must have value";
                    return mlir::failure();
                }
            }
            else if (returnType != expressionValue.getType())
            {
                CAST_A(castValue, location, returnType, expressionValue, genContext);
                expressionValue = castValue;
            }
        }

        // record return type if not provided
        processReturnType(expressionValue, genContext);

        if (!expressionValue)
        {
            emitError(location) << "'return' must have value";
            builder.create<mlir_ts::ReturnOp>(location);
            return genContext.passResult ? mlir::success() : mlir::failure();
        }

        auto retVarInfo = symbolTable.lookup(RETURN_VARIABLE_NAME);
        if (!retVarInfo.second)
        {
            if (genContext.allowPartialResolve)
            {
                return mlir::success();
            }

            emitError(location) << "can't find return variable";
            return mlir::failure();
        }

        if (yieldReturn)
        {
            builder.create<mlir_ts::YieldReturnValOp>(location, expressionValue, retVarInfo.first);
        }
        else
        {
            builder.create<mlir_ts::ReturnValOp>(location, expressionValue, retVarInfo.first);
        }

        return mlir::success();
    }

    mlir::LogicalResult addSafeCastStatement(Expression expr, Node typeToken, const GenContext &genContext)
    {
        auto safeType = getType(typeToken, genContext);
        return addSafeCastStatement(expr, safeType, genContext);
    }

    mlir::LogicalResult addSafeCastStatement(Expression expr, mlir::Type safeType, const GenContext &genContext)
    {
        auto location = loc(expr);
        auto nameStr = MLIRHelper::getName(expr.as<DeclarationName>());
        auto result = mlirGen(expr, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result);
        auto exprValue = V(result);

        return addSafeCastStatement(location, nameStr, exprValue, safeType, genContext);
    }    

    mlir::LogicalResult addSafeCastStatement(mlir::Location location, std::string parameterName, mlir::Value exprValue, mlir::Type safeType, const GenContext &genContext)
    {
        mlir::Value castedValue;
        if (exprValue.getType().isa<mlir_ts::AnyType>())
        {
            castedValue = builder.create<mlir_ts::UnboxOp>(location, safeType, exprValue);
        }
        else if (exprValue.getType().isa<mlir_ts::OptionalType>() 
                 && exprValue.getType().cast<mlir_ts::OptionalType>().getElementType() == safeType)
        {
            castedValue = builder.create<mlir_ts::ValueOp>(location, safeType, exprValue);
        }
        else
        {
            CAST_A(result, location, safeType, exprValue, genContext);
            castedValue = V(result);
        }

        return 
            !!registerVariable(
                location, parameterName, false, VariableType::Const,
                [&](mlir::Location, const GenContext &) -> TypeValueInitType
                {
                    return {safeType, castedValue, TypeProvided::Yes};
                },
                genContext) ? mlir::success() : mlir::failure();        
    }    

    mlir::LogicalResult checkSafeCastTypeOf(Expression typeOfVal, Expression constVal, const GenContext &genContext)
    {
        if (auto typeOfOp = typeOfVal.as<TypeOfExpression>())
        {
            // strip parenthesizes
            auto expr = stripParentheses(typeOfOp->expression);
            if (!expr.is<Identifier>())
            {
                return mlir::failure();
            }

            if (auto stringLiteral = constVal.as<ts::StringLiteral>())
            {
                // create 'expression' = <string>'expression;
                NodeFactory nf(NodeFactoryFlags::None);

                auto text = stringLiteral->text;
                Node typeToken;
                if (text == S("string"))
                {
                    typeToken = nf.createToken(SyntaxKind::StringKeyword);
                }
                else if (text == S("number"))
                {
                    typeToken = nf.createToken(SyntaxKind::NumberKeyword);
                }
                else if (text == S("boolean"))
                {
                    typeToken = nf.createToken(SyntaxKind::BooleanKeyword);
                }
                else if (text == S("i32"))
                {
                    typeToken = nf.createTypeReferenceNode(nf.createIdentifier(S("TypeOf")), { 
                        nf.createLiteralTypeNode(nf.createLiteralLikeNode(SyntaxKind::NumericLiteral, S("1")).as<Node>()) 
                    });
                }
                else if (text == S("i64"))
                {
                    typeToken = nf.createTypeReferenceNode(nf.createIdentifier(S("TypeOf")), { 
                        nf.createLiteralTypeNode(nf.createLiteralLikeNode(SyntaxKind::NumericLiteral, S("9223372036854775807")).as<Node>()) 
                    });
                }

                if (typeToken)
                {
                    return addSafeCastStatement(expr, typeToken, genContext);
                }

                return mlir::success();
            }
        }

        return mlir::failure();
    }

    mlir::LogicalResult checkSafeCastUndefined(Expression optVal, Expression undefVal, const GenContext &genContext)
    {
        auto expr = stripParentheses(undefVal);
        if (auto identifier = expr.as<ts::Identifier>())
        {
            if (identifier->escapedText == S(UNDEFINED_NAME))
            {
                auto optEval = evaluate(optVal, genContext);
                if (auto optType = optEval.dyn_cast_or_null<mlir_ts::OptionalType>())
                {
                    return addSafeCastStatement(optVal, optType.getElementType(), genContext);
                }
            }
        }

        return mlir::failure();
    }    

    Expression stripParentheses(Expression exprVal)
    {
        auto expr = exprVal;
        while (expr.is<ParenthesizedExpression>())
        {
            expr = expr.as<ParenthesizedExpression>()->expression;
        }

        return expr;
    }

    mlir::LogicalResult checkSafeCastPropertyAccessLogic(TextRange textRange, Expression objAccessExpression,
                                                         mlir::Type typeOfObject, Node name, mlir::Value constVal,
                                                         const GenContext &genContext)
    {
        if (auto unionType = typeOfObject.dyn_cast<mlir_ts::UnionType>())
        {
            auto isConst = false;
            mlir::Attribute value;
            isConst = isConstValue(constVal);
            if (isConst)
            {
                auto constantOp = constVal.getDefiningOp<mlir_ts::ConstantOp>();
                assert(constantOp);
                auto valueAttr = constantOp.getValueAttr();

                MLIRCodeLogic mcl(builder);
                auto fieldNameAttr = TupleFieldName(name, genContext);

                for (auto unionSubType : unionType.getTypes())
                {
                    if (auto tupleType = unionSubType.dyn_cast<mlir_ts::TupleType>())
                    {
                        auto fieldIndex = tupleType.getIndex(fieldNameAttr);
                        auto fieldType = tupleType.getType(fieldIndex);
                        if (auto literalType = fieldType.dyn_cast<mlir_ts::LiteralType>())
                        {
                            if (literalType.getValue() == valueAttr)
                            {
                                // enable safe cast found
                                auto typeAliasNameUTF8 = MLIRHelper::getAnonymousName(loc_check(textRange), "ta_");
                                auto typeAliasName = ConvertUTF8toWide(typeAliasNameUTF8);
                                const_cast<GenContext &>(genContext)
                                    .typeAliasMap.insert({typeAliasNameUTF8, tupleType});

                                NodeFactory nf(NodeFactoryFlags::None);
                                auto typeRef = nf.createTypeReferenceNode(nf.createIdentifier(typeAliasName));
                                return addSafeCastStatement(objAccessExpression, typeRef, genContext);
                            }
                        }
                    }

                    if (auto interfaceType = unionSubType.dyn_cast<mlir_ts::InterfaceType>())
                    {
                        if (auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue()))
                        {
                            int totalOffset = -1;
                            auto fieldInfo = interfaceInfo->findField(fieldNameAttr, totalOffset);
                            if (auto literalType = fieldInfo->type.dyn_cast<mlir_ts::LiteralType>())
                            {
                                if (literalType.getValue() == valueAttr)
                                {
                                    // enable safe cast found
                                    auto typeAliasNameUTF8 = MLIRHelper::getAnonymousName(loc_check(textRange), "ta_");
                                    auto typeAliasName = ConvertUTF8toWide(typeAliasNameUTF8);
                                    const_cast<GenContext &>(genContext)
                                        .typeAliasMap.insert({typeAliasNameUTF8, interfaceType});

                                    NodeFactory nf(NodeFactoryFlags::None);
                                    auto typeRef = nf.createTypeReferenceNode(nf.createIdentifier(typeAliasName));
                                    return addSafeCastStatement(objAccessExpression, typeRef, genContext);
                                }
                            }
                        }
                    }                    
                }
            }
        }

        return mlir::failure();
    }

    mlir::LogicalResult checkSafeCastPropertyAccess(Expression exprVal, Expression constVal,
                                                    const GenContext &genContext)
    {
        auto expr = stripParentheses(exprVal);
        if (expr.is<PropertyAccessExpression>())
        {
            auto isConstVal = isConstValue(constVal, genContext);
            if (!isConstVal)
            {
                return mlir::failure();
            }

            auto propertyAccessExpressionOp = expr.as<PropertyAccessExpression>();
            auto objAccessExpression = propertyAccessExpressionOp->expression;
            auto typeOfObject = evaluate(objAccessExpression, genContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! SafeCastCheck: " << typeOfObject << "");

            auto val = mlirGen(constVal, genContext);
            return checkSafeCastPropertyAccessLogic(constVal, objAccessExpression, typeOfObject,
                                                    propertyAccessExpressionOp->name, val, genContext);
        }

        return mlir::failure();
    }

    mlir::LogicalResult checkSafeCastTypePredicate(Expression expr, mlir_ts::TypePredicateType typePredicateType, const GenContext &genContext)
    {
        return addSafeCastStatement(expr, typePredicateType.getElementType(), genContext);
    }

    mlir::LogicalResult checkSafeCast(Expression expr, mlir::Value conditionValue, const GenContext &genContext)
    {
        if (expr == SyntaxKind::CallExpression)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! SafeCast: condition: " << conditionValue << "\n");

            if (auto callInd = conditionValue.getDefiningOp<mlir_ts::CallIndirectOp>())
            {
                auto funcType = callInd.getCallee().getType();

                auto resultType = mth.getReturnTypeFromFuncRef(funcType);

                if (auto typePredicateType = resultType.dyn_cast<mlir_ts::TypePredicateType>())
                {
                    // TODO: you need to find argument by using parameter name
                    auto callExpr = expr.as<CallExpression>();
                    if (typePredicateType.getParameterName().getValue() == THIS_NAME)
                    {
                        if (callExpr->expression == SyntaxKind::PropertyAccessExpression)
                        {
                            // in case of "this"
                            return checkSafeCastTypePredicate(
                                callExpr->expression.as<PropertyAccessExpression>()->expression, 
                                typePredicateType, 
                                genContext);                            
                        }
                    }
                    else if (typePredicateType.getParameterIndex() >= 0 && callExpr->arguments.size() > 0)
                    {
                        // in case of parameters
                        return checkSafeCastTypePredicate(
                            callExpr->arguments[typePredicateType.getParameterIndex()], 
                            typePredicateType, 
                            genContext);
                    }
                }
            }

            return mlir::success();
        }
        else if (expr == SyntaxKind::PropertyAccessExpression)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! SafeCast: condition: " << conditionValue << "\n");

            mlir_ts::TypePredicateType propertyType;
            if (auto loadOp = conditionValue.getDefiningOp<mlir_ts::LoadOp>())
            {
                if (auto typePredicateType = loadOp.getType().dyn_cast<mlir_ts::TypePredicateType>())
                {
                    propertyType = typePredicateType;
                }
            }
            else if (auto thisAccessor = conditionValue.getDefiningOp<mlir_ts::ThisAccessorOp>())
            {
                if (auto typePredicateType = thisAccessor.getType().dyn_cast<mlir_ts::TypePredicateType>())
                {
                    propertyType = typePredicateType;
                }
            }

            if (propertyType && propertyType.getParameterName().getValue() == THIS_NAME)
            {
                // in case of "this"
                return checkSafeCastTypePredicate(
                    expr.as<PropertyAccessExpression>()->expression, 
                    propertyType, 
                    genContext);                            
            }

            return mlir::success();
        }
        else if (expr == SyntaxKind::BinaryExpression)
        {
            auto binExpr = expr.as<BinaryExpression>();
            auto op = (SyntaxKind)binExpr->operatorToken;
            if (op == SyntaxKind::EqualsEqualsToken || op == SyntaxKind::EqualsEqualsEqualsToken)
            {
                auto left = binExpr->left;
                auto right = binExpr->right;

                if (mlir::failed(checkSafeCastTypeOf(left, right, genContext)))
                {
                    if (mlir::failed(checkSafeCastTypeOf(right, left, genContext)))
                    {
                        if (mlir::failed(checkSafeCastPropertyAccess(left, right, genContext)))
                        {
                            return checkSafeCastPropertyAccess(right, left, genContext);
                        }
                    }
                }

                return mlir::success();
            }

            if (op == SyntaxKind::ExclamationEqualsToken || op == SyntaxKind::ExclamationEqualsEqualsToken)
            {
                auto left = binExpr->left;
                auto right = binExpr->right;

                if (mlir::failed(checkSafeCastUndefined(left, right, genContext)))
                {
                    return checkSafeCastUndefined(right, left, genContext);
                }

                return mlir::success();
            }            

            if (op == SyntaxKind::InstanceOfKeyword)
            {
                auto instanceOf = binExpr;
                if (instanceOf->left.is<Identifier>())
                {
                    NodeFactory nf(NodeFactoryFlags::None);
                    return addSafeCastStatement(instanceOf->left, nf.createTypeReferenceNode(instanceOf->right),
                                                genContext);
                }
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(IfStatement ifStatementAST, const GenContext &genContext)
    {
        auto location = loc(ifStatementAST);

        auto hasElse = !!ifStatementAST->elseStatement;

        // condition
        auto result = mlirGen(ifStatementAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto condValue = V(result);

        if (condValue.getType() != getBooleanType())
        {
            CAST(condValue, location, getBooleanType(), condValue, genContext);
        }

        auto ifOp = builder.create<mlir_ts::IfOp>(location, condValue, hasElse);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());

        {
            // check if we do safe-cast here
            SymbolTableScopeT varScope(symbolTable);
            checkSafeCast(ifStatementAST->expression, V(result), genContext);
            auto result = mlirGen(ifStatementAST->thenStatement, genContext);
            EXIT_IF_FAILED(result)
        }

        if (hasElse)
        {
            builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
            auto result = mlirGen(ifStatementAST->elseStatement, genContext);
            EXIT_IF_FAILED(result)
        }

        builder.setInsertionPointAfter(ifOp);

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(DoStatement doStatementAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(doStatementAST);

        SmallVector<mlir::Type, 0> types;
        SmallVector<mlir::Value, 0> operands;

        auto doWhileOp = builder.create<mlir_ts::DoWhileOp>(location, types, operands);
        if (!label.empty())
        {
            doWhileOp->setAttr(LABEL_ATTR_NAME, builder.getStringAttr(label));
            label = "";
        }

        const_cast<GenContext &>(genContext).isLoop = true;
        const_cast<GenContext &>(genContext).loopLabel = label;

        /*auto *cond =*/builder.createBlock(&doWhileOp.getCond(), {}, types);
        /*auto *body =*/builder.createBlock(&doWhileOp.getBody(), {}, types);

        // body in condition
        builder.setInsertionPointToStart(&doWhileOp.getBody().front());
        mlirGen(doStatementAST->statement, genContext);
        // just simple return, as body in cond
        builder.create<mlir_ts::ResultOp>(location);

        builder.setInsertionPointToStart(&doWhileOp.getCond().front());
        auto result = mlirGen(doStatementAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto conditionValue = V(result);

        if (conditionValue.getType() != getBooleanType())
        {
            CAST(conditionValue, location, getBooleanType(), conditionValue, genContext);
        }

        builder.create<mlir_ts::ConditionOp>(location, conditionValue, mlir::ValueRange{});

        builder.setInsertionPointAfter(doWhileOp);
        return mlir::success();
    }

    mlir::LogicalResult mlirGen(WhileStatement whileStatementAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(whileStatementAST);

        SmallVector<mlir::Type, 0> types;
        SmallVector<mlir::Value, 0> operands;

        auto whileOp = builder.create<mlir_ts::WhileOp>(location, types, operands);
        if (!label.empty())
        {
            whileOp->setAttr(LABEL_ATTR_NAME, builder.getStringAttr(label));
            label = "";
        }

        const_cast<GenContext &>(genContext).isLoop = true;
        const_cast<GenContext &>(genContext).loopLabel = label;

        /*auto *cond =*/builder.createBlock(&whileOp.getCond(), {}, types);
        /*auto *body =*/builder.createBlock(&whileOp.getBody(), {}, types);

        // condition
        builder.setInsertionPointToStart(&whileOp.getCond().front());
        auto result = mlirGen(whileStatementAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto conditionValue = V(result);

        if (conditionValue.getType() != getBooleanType())
        {
            CAST(conditionValue, location, getBooleanType(), conditionValue, genContext);
        }

        builder.create<mlir_ts::ConditionOp>(location, conditionValue, mlir::ValueRange{});

        // body
        builder.setInsertionPointToStart(&whileOp.getBody().front());
        mlirGen(whileStatementAST->statement, genContext);
        builder.create<mlir_ts::ResultOp>(location);

        builder.setInsertionPointAfter(whileOp);
        return mlir::success();
    }

    mlir::LogicalResult mlirGen(ForStatement forStatementAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(forStatementAST);

        auto hasAwait = InternalFlags::ForAwait == (forStatementAST->internalFlags & InternalFlags::ForAwait);

        // initializer
        // TODO: why do we have ForInitialier
        if (forStatementAST->initializer.is<Expression>())
        {
            auto result = mlirGen(forStatementAST->initializer.as<Expression>(), genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto init = V(result);
            if (!init)
            {
                return mlir::failure();
            }
        }
        else if (forStatementAST->initializer.is<VariableDeclarationList>())
        {
            auto result = mlirGen(forStatementAST->initializer.as<VariableDeclarationList>(), genContext);
            EXIT_IF_FAILED(result)
            if (failed(result))
            {
                return result;
            }
        }

        SmallVector<mlir::Type, 0> types;
        SmallVector<mlir::Value, 0> operands;

        mlir::Value asyncGroupResult;
        if (hasAwait)
        {
            auto groupType = mlir::async::GroupType::get(builder.getContext());
            auto blockSize = builder.create<mlir_ts::ConstantOp>(location, builder.getIndexAttr(0));
            auto asyncGroupOp = builder.create<mlir::async::CreateGroupOp>(location, groupType, blockSize);
            asyncGroupResult = asyncGroupOp.getResult();
            // operands.push_back(asyncGroupOp);
            // types.push_back(groupType);
        }

        auto forOp = builder.create<mlir_ts::ForOp>(location, types, operands);
        if (!label.empty())
        {
            forOp->setAttr(LABEL_ATTR_NAME, builder.getStringAttr(label));
            label = "";
        }

        const_cast<GenContext &>(genContext).isLoop = true;
        const_cast<GenContext &>(genContext).loopLabel = label;

        /*auto *cond =*/builder.createBlock(&forOp.getCond(), {}, types);
        /*auto *body =*/builder.createBlock(&forOp.getBody(), {}, types);
        /*auto *incr =*/builder.createBlock(&forOp.getIncr(), {}, types);

        builder.setInsertionPointToStart(&forOp.getCond().front());
        auto result = mlirGen(forStatementAST->condition, genContext);
        EXIT_IF_FAILED(result)
        auto conditionValue = V(result);
        if (conditionValue)
        {
            builder.create<mlir_ts::ConditionOp>(location, conditionValue, mlir::ValueRange{});
        }
        else
        {
            builder.create<mlir_ts::NoConditionOp>(location, mlir::ValueRange{});
        }

        // body
        builder.setInsertionPointToStart(&forOp.getBody().front());
        if (hasAwait)
        {
            if (forStatementAST->statement == SyntaxKind::Block)
            {
                // TODO: it is kind of hack, maybe you can find better solution
                auto firstStatement = forStatementAST->statement.as<Block>()->statements.front();
                mlirGen(firstStatement, genContext);
                firstStatement->processed = true;
            }

            // async body
            auto asyncExecOp = builder.create<mlir::async::ExecuteOp>(
                location, mlir::TypeRange{}, mlir::ValueRange{}, mlir::ValueRange{},
                [&](mlir::OpBuilder &builder, mlir::Location location, mlir::ValueRange values) {
                    GenContext execOpBodyGenContext(genContext);
                    execOpBodyGenContext.skipProcessed = true;
                    mlirGen(forStatementAST->statement, execOpBodyGenContext);
                    builder.create<mlir::async::YieldOp>(location, mlir::ValueRange{});
                });

            // add to group
            auto rankType = mlir::IndexType::get(builder.getContext());
            // TODO: should i replace with value from arg0?
            builder.create<mlir::async::AddToGroupOp>(location, rankType, asyncExecOp.getToken(), asyncGroupResult);
        }
        else
        {
            // default
            auto result = mlirGen(forStatementAST->statement, genContext);
            EXIT_IF_FAILED(result)
        }

        builder.create<mlir_ts::ResultOp>(location);

        // increment
        builder.setInsertionPointToStart(&forOp.getIncr().front());
        mlirGen(forStatementAST->incrementor, genContext);
        builder.create<mlir_ts::ResultOp>(location);

        builder.setInsertionPointAfter(forOp);

        if (hasAwait)
        {
            // Not helping
            /*
            // async await all, see convert-to-llvm.mlir
            auto asyncExecAwaitAllOp =
                builder.create<mlir::async::ExecuteOp>(location, mlir::TypeRange{}, mlir::ValueRange{},
            mlir::ValueRange{},
                                                       [&](mlir::OpBuilder &builder, mlir::Location location,
            mlir::ValueRange values) { builder.create<mlir::async::AwaitAllOp>(location, asyncGroupResult);
                                                           builder.create<mlir::async::YieldOp>(location,
            mlir::ValueRange{});
                                                       });
            */

            // Wait for the completion of all subtasks.
            builder.create<mlir::async::AwaitAllOp>(location, asyncGroupResult);
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(ForInStatement forInStatementAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(forInStatementAST);

        NodeFactory nf(NodeFactoryFlags::None);

        // init
        NodeArray<VariableDeclaration> declarations;
        auto _i = nf.createIdentifier(S(".i"));
        declarations.push_back(nf.createVariableDeclaration(_i, undefined, undefined, nf.createNumericLiteral(S("0"))));

        auto _a = nf.createIdentifier(S(".a"));
        auto arrayVar = nf.createVariableDeclaration(_a, undefined, undefined, forInStatementAST->expression);
        arrayVar->internalFlags |= InternalFlags::ForceConstRef;
        declarations.push_back(arrayVar);

        auto initVars = nf.createVariableDeclarationList(declarations, NodeFlags::Let);

        // condition
        // auto cond = nf.createBinaryExpression(_i, nf.createToken(SyntaxKind::LessThanToken),
        // nf.createCallExpression(nf.createIdentifier(S("#_last_field")), undefined, NodeArray<Expression>(_a)));
        auto cond = nf.createBinaryExpression(_i, nf.createToken(SyntaxKind::LessThanToken),
                                              nf.createPropertyAccessExpression(_a, nf.createIdentifier(S(LENGTH_FIELD_NAME))));

        // incr
        auto incr = nf.createPrefixUnaryExpression(nf.createToken(SyntaxKind::PlusPlusToken), _i);

        // block
        NodeArray<ts::Statement> statements;

        auto varDeclList = forInStatementAST->initializer.as<VariableDeclarationList>();
        varDeclList->declarations.front()->initializer = _i;

        statements.push_back(nf.createVariableStatement(undefined, varDeclList));
        statements.push_back(forInStatementAST->statement);
        auto block = nf.createBlock(statements);

        // final For statement
        auto forStatNode = nf.createForStatement(initVars, cond, incr, block);

        return mlirGen(forStatNode, genContext);
    }

    mlir::LogicalResult mlirGenES3(ForOfStatement forOfStatementAST, mlir::Value exprValue,
                                   const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(forOfStatementAST);

        auto varDecl = std::make_shared<VariableDeclarationDOM>(EXPR_TEMPVAR_NAME, exprValue.getType(), location);
        // somehow it is detected as external var, seems because it is contains external ref
        varDecl->setIgnoreCapturing();
        DECLARE(varDecl, exprValue);

        NodeFactory nf(NodeFactoryFlags::None);

        // init
        NodeArray<VariableDeclaration> declarations;
        auto _i = nf.createIdentifier(S(".i"));
        declarations.push_back(nf.createVariableDeclaration(_i, undefined, undefined, nf.createNumericLiteral(S("0"))));

        auto _a = nf.createIdentifier(S(".a"));
        auto arrayVar =
            nf.createVariableDeclaration(_a, undefined, undefined, nf.createIdentifier(S(EXPR_TEMPVAR_NAME)));
        arrayVar->internalFlags |= InternalFlags::ForceConstRef;

        declarations.push_back(arrayVar);

        // condition
        auto cond = nf.createBinaryExpression(_i, nf.createToken(SyntaxKind::LessThanToken),
                                              nf.createPropertyAccessExpression(_a, nf.createIdentifier(S(LENGTH_FIELD_NAME))));

        // incr
        auto incr = nf.createPrefixUnaryExpression(nf.createToken(SyntaxKind::PlusPlusToken), _i);

        // block
        NodeArray<ts::Statement> statements;

        NodeArray<VariableDeclaration> varOfConstDeclarations;
        auto _ci = nf.createIdentifier(S(".ci"));
        varOfConstDeclarations.push_back(nf.createVariableDeclaration(_ci, undefined, undefined, _i));
        auto varsOfConst = nf.createVariableDeclarationList(varOfConstDeclarations, NodeFlags::Const);

        auto initVars = nf.createVariableDeclarationList(declarations, NodeFlags::Let /*varDeclList->flags*/);

        // in async exec, we will put first statement outside fo async.exec, to convert ref<int> into <int>
        statements.push_back(nf.createVariableStatement(undefined, varsOfConst));

        if (forOfStatementAST->initializer == SyntaxKind::VariableDeclarationList)
        {
            auto varDeclList = forOfStatementAST->initializer.as<VariableDeclarationList>();
            if (!varDeclList->declarations.empty())
            {
                varDeclList->declarations.front()->initializer = nf.createElementAccessExpression(_a, _ci);
                statements.push_back(nf.createVariableStatement(undefined, varDeclList));
            }
        }
        else
        {
            // set value
            statements.push_back(nf.createExpressionStatement(
                nf.createBinaryExpression(forOfStatementAST->initializer, nf.createToken(SyntaxKind::EqualsToken), nf.createElementAccessExpression(_a, _ci))
            ));
        }

        statements.push_back(forOfStatementAST->statement);
        auto block = nf.createBlock(statements);

        // final For statement
        auto forStatNode = nf.createForStatement(initVars, cond, incr, block);
        if (forOfStatementAST->awaitModifier)
        {
            forStatNode->internalFlags |= InternalFlags::ForAwait;
        }

        LLVM_DEBUG(printDebug(forStatNode););

        return mlirGen(forStatNode, genContext);
    }

    mlir::LogicalResult mlirGenES2015(ForOfStatement forOfStatementAST, mlir::Value exprValue,
                                      const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(forOfStatementAST);

        auto varDecl = std::make_shared<VariableDeclarationDOM>(EXPR_TEMPVAR_NAME, exprValue.getType(), location);
        // somehow it is detected as external var, seems because it is contains external ref
        varDecl->setIgnoreCapturing();
        DECLARE(varDecl, exprValue);

        NodeFactory nf(NodeFactoryFlags::None);

        // init
        NodeArray<VariableDeclaration> declarations;
        auto _b = nf.createIdentifier(S(".b"));
        auto _next = nf.createIdentifier(S(ITERATOR_NEXT));
        auto _bVar = nf.createVariableDeclaration(_b, undefined, undefined, nf.createIdentifier(S(EXPR_TEMPVAR_NAME)));
        declarations.push_back(_bVar);

        NodeArray<Expression> nextArgs;

        auto _c = nf.createIdentifier(S(".c"));
        auto _done = nf.createIdentifier(S("done"));
        auto _value = nf.createIdentifier(S("value"));
        auto _cVar = nf.createVariableDeclaration(
            _c, undefined, undefined,
            nf.createCallExpression(nf.createPropertyAccessExpression(_b, _next), undefined, nextArgs));
        declarations.push_back(_cVar);

        // condition
        auto cond = nf.createPrefixUnaryExpression(nf.createToken(SyntaxKind::ExclamationToken),
                                                   nf.createPropertyAccessExpression(_c, _done));

        // incr
        auto incr = nf.createBinaryExpression(
            _c, nf.createToken(SyntaxKind::EqualsToken),
            nf.createCallExpression(nf.createPropertyAccessExpression(_b, _next), undefined, nextArgs));

        // block
        NodeArray<ts::Statement> statements;

        if (forOfStatementAST->initializer == SyntaxKind::VariableDeclarationList)
        {
            auto varDeclList = forOfStatementAST->initializer.as<VariableDeclarationList>();
            if (!varDeclList->declarations.empty())
            {
                varDeclList->declarations.front()->initializer = nf.createPropertyAccessExpression(_c, _value);
                statements.push_back(nf.createVariableStatement(undefined, varDeclList));
            }
        }
        else
        {
            // set value
            statements.push_back(nf.createExpressionStatement(
                nf.createBinaryExpression(forOfStatementAST->initializer, nf.createToken(SyntaxKind::EqualsToken), nf.createPropertyAccessExpression(_c, _value))
            ));            
        }

        statements.push_back(forOfStatementAST->statement);
        auto block = nf.createBlock(statements);

        auto initVars = nf.createVariableDeclarationList(declarations, NodeFlags::Let /*varDeclList->flags*/);
        // final For statement
        auto forStatNode = nf.createForStatement(initVars, cond, incr, block);
        if (forOfStatementAST->awaitModifier)
        {
            forStatNode->internalFlags |= InternalFlags::ForAwait;
        }

        //LLVM_DEBUG(printDebug(forStatNode););

        return mlirGen(forStatNode, genContext);
    }

    mlir::LogicalResult mlirGen(ForOfStatement forOfStatementAST, const GenContext &genContext)
    {
        auto location = loc(forOfStatementAST);

        auto result = mlirGen(forOfStatementAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto exprValue = V(result);

        auto iteratorIdent = (forOfStatementAST->awaitModifier) ? SYMBOL_ASYNC_ITERATOR : SYMBOL_ITERATOR;
        if (auto iteratorType = evaluateProperty(exprValue, iteratorIdent, genContext))
        {
            if (auto iteratorValue = mlirGenCallThisMethod(location, exprValue, iteratorIdent, undefined, undefined, genContext))
            {
                exprValue = V(iteratorValue);
            }
        }

        auto propertyType = evaluateProperty(exprValue, ITERATOR_NEXT, genContext);
        if (propertyType)
        {
            if (mlir::succeeded(mlirGenES2015(forOfStatementAST, exprValue, genContext)))
            {
                return mlir::success();
            }
        }

        return mlirGenES3(forOfStatementAST, exprValue, genContext);
    }

    mlir::LogicalResult mlirGen(LabeledStatement labeledStatementAST, const GenContext &genContext)
    {
        auto location = loc(labeledStatementAST);

        label = MLIRHelper::getName(labeledStatementAST->label);

        auto kind = (SyntaxKind)labeledStatementAST->statement;
        if (kind == SyntaxKind::EmptyStatement && StringRef(label).startswith(GENERATOR_STATELABELPREFIX))
        {
            builder.create<mlir_ts::StateLabelOp>(location, builder.getStringAttr(label));
            return mlir::success();
        }

        auto noLabelOp = kind == SyntaxKind::WhileStatement || kind == SyntaxKind::DoStatement ||
                         kind == SyntaxKind::ForStatement || kind == SyntaxKind::ForInStatement ||
                         kind == SyntaxKind::ForOfStatement;

        if (noLabelOp)
        {
            auto res = mlirGen(labeledStatementAST->statement, genContext);
            return res;
        }

        auto labelOp = builder.create<mlir_ts::LabelOp>(location, builder.getStringAttr(label));

        // add merge block
        labelOp.addMergeBlock();
        auto *mergeBlock = labelOp.getMergeBlock();

        builder.setInsertionPointToStart(mergeBlock);

        auto res = mlirGen(labeledStatementAST->statement, genContext);

        builder.setInsertionPointAfter(labelOp);

        return res;
    }

    mlir::LogicalResult mlirGen(DebuggerStatement debuggerStatementAST, const GenContext &genContext)
    {
        auto location = loc(debuggerStatementAST);

        builder.create<mlir_ts::DebuggerOp>(location);
        return mlir::success();
    }

    mlir::LogicalResult mlirGen(ContinueStatement continueStatementAST, const GenContext &genContext)
    {
        auto location = loc(continueStatementAST);

        auto label = MLIRHelper::getName(continueStatementAST->label);

        EXIT_IF_FAILED(mlirGenDisposable(location, DisposeDepth::LoopScope, label, &genContext));

        builder.create<mlir_ts::ContinueOp>(location, builder.getStringAttr(label));
        return mlir::success();
    }

    mlir::LogicalResult mlirGen(BreakStatement breakStatementAST, const GenContext &genContext)
    {
        auto location = loc(breakStatementAST);

        auto label = MLIRHelper::getName(breakStatementAST->label);

        EXIT_IF_FAILED(mlirGenDisposable(location, DisposeDepth::LoopScope, label, &genContext));

        builder.create<mlir_ts::BreakOp>(location, builder.getStringAttr(label));
        return mlir::success();
    }

    mlir::LogicalResult mlirGenSwitchCase(mlir::Location location, Expression switchExpr, mlir::Value switchValue,
                                          NodeArray<ts::CaseOrDefaultClause> &clauses, int index,
                                          mlir::Block *mergeBlock, mlir::Block *&defaultBlock,
                                          SmallVector<mlir::cf::CondBranchOp> &pendingConditions,
                                          SmallVector<mlir::cf::BranchOp> &pendingBranches,
                                          mlir::Operation *&previousConditionOrFirstBranchOp,
                                          std::function<void(Expression, mlir::Value)> extraCode,
                                          const GenContext &genContext)
    {
        SymbolTableScopeT safeCastVarScope(symbolTable);

        enum
        {
            trueIndex = 0,
            falseIndex = 1
        };

        auto caseBlock = clauses[index];
        auto statements = caseBlock->statements;
        // inline block
        // TODO: should I inline block as it is isolator of local vars?
        if (statements.size() == 1)
        {
            auto firstStatement = statements.front();
            if ((SyntaxKind)firstStatement == SyntaxKind::Block)
            {
                statements = statements.front().as<Block>()->statements;
            }
        }

        auto setPreviousCondOrJumpOp = [&](mlir::Operation *jump, mlir::Block *where) {
            if (auto condOp = dyn_cast<mlir::cf::CondBranchOp>(jump))
            {
                condOp->setSuccessor(where, falseIndex);
                return;
            }

            if (auto branchOp = dyn_cast<mlir::cf::BranchOp>(jump))
            {
                branchOp.setDest(where);
                return;
            }

            llvm_unreachable("not implemented");
        };

        // condition
        auto isDefaultCase = SyntaxKind::DefaultClause == (SyntaxKind)caseBlock;
        auto isDefaultAsFirstCase = index == 0 && clauses.size() > 1;
        if (SyntaxKind::CaseClause == (SyntaxKind)caseBlock)
        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            auto caseConditionBlock = builder.createBlock(mergeBlock);
            if (previousConditionOrFirstBranchOp)
            {
                setPreviousCondOrJumpOp(previousConditionOrFirstBranchOp, caseConditionBlock);
            }

            auto caseExpr = caseBlock.as<CaseClause>()->expression;
            auto result = mlirGen(caseExpr, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto caseValue = V(result);

            extraCode(caseExpr, caseValue);

            auto switchValueEffective = switchValue;
            auto actualCaseType = mth.stripLiteralType(caseValue.getType());
            if (switchValue.getType() != actualCaseType)
            {
                CAST(switchValueEffective, location, actualCaseType, switchValue, genContext);
            }

            auto condition = builder.create<mlir_ts::LogicalBinaryOp>(
                location, getBooleanType(), builder.getI32IntegerAttr((int)SyntaxKind::EqualsEqualsToken),
                switchValueEffective, caseValue);

            CAST_A(conditionI1, location, builder.getI1Type(), condition, genContext);

            auto condBranchOp = builder.create<mlir::cf::CondBranchOp>(location, conditionI1, mergeBlock,
                                                                   /*trueArguments=*/mlir::ValueRange{},
                                                                   defaultBlock ? defaultBlock : mergeBlock,
                                                                   /*falseArguments=*/mlir::ValueRange{});

            previousConditionOrFirstBranchOp = condBranchOp;

            pendingConditions.push_back(condBranchOp);
        }
        else if (isDefaultAsFirstCase)
        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            /*auto defaultCaseJumpBlock =*/builder.createBlock(mergeBlock);

            // this is first default and there is more conditions
            // add jump to first condition
            auto branchOp = builder.create<mlir::cf::BranchOp>(location, mergeBlock);

            previousConditionOrFirstBranchOp = branchOp;
        }

        // statements block
        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            auto caseBodyBlock = builder.createBlock(mergeBlock);
            if (isDefaultCase)
            {
                defaultBlock = caseBodyBlock;
                if (!isDefaultAsFirstCase && previousConditionOrFirstBranchOp)
                {
                    setPreviousCondOrJumpOp(previousConditionOrFirstBranchOp, caseBodyBlock);
                }
            }

            // set pending BranchOps
            for (auto pendingBranch : pendingBranches)
            {
                pendingBranch.setDest(caseBodyBlock);
            }

            pendingBranches.clear();

            for (auto pendingCondition : pendingConditions)
            {
                pendingCondition.setSuccessor(caseBodyBlock, trueIndex);
            }

            pendingConditions.clear();

            // process body case
            if (genContext.generatedStatements.size() > 0)
            {
                // auto generated code
                for (auto &statement : genContext.generatedStatements)
                {
                    if (failed(mlirGen(statement, genContext)))
                    {
                        return mlir::failure();
                    }
                }

                // clean up
                const_cast<GenContext &>(genContext).generatedStatements.clear();
            }

            auto hasBreak = false;
            for (auto statement : statements)
            {
                if ((SyntaxKind)statement == SyntaxKind::BreakStatement)
                {
                    hasBreak = true;
                    break;
                }

                if (failed(mlirGen(statement, genContext)))
                {
                    return mlir::failure();
                }
            }

            // exit;
            auto branchOp = builder.create<mlir::cf::BranchOp>(location, mergeBlock);
            if (!hasBreak && !isDefaultCase)
            {
                pendingBranches.push_back(branchOp);
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(SwitchStatement switchStatementAST, const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto location = loc(switchStatementAST);

        auto switchExpr = switchStatementAST->expression;
        auto result = mlirGen(switchExpr, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto switchValue = V(result);

        auto switchOp = builder.create<mlir_ts::SwitchOp>(location, switchValue);

        GenContext switchGenContext(genContext);
        switchGenContext.allocateVarsOutsideOfOperation = true;
        switchGenContext.currentOperation = switchOp;
        switchGenContext.insertIntoParentScope = true;

        // add merge block
        switchOp.addMergeBlock();
        auto *mergeBlock = switchOp.getMergeBlock();

        auto &clauses = switchStatementAST->caseBlock->clauses;

        SmallVector<mlir::cf::CondBranchOp> pendingConditions;
        SmallVector<mlir::cf::BranchOp> pendingBranches;
        mlir::Operation *previousConditionOrFirstBranchOp = nullptr;
        mlir::Block *defaultBlock = nullptr;

        // to support safe cast
        std::function<void(Expression, mlir::Value)> safeCastLogic;
        if (switchExpr.is<PropertyAccessExpression>())
        {
            auto propertyAccessExpressionOp = switchExpr.as<PropertyAccessExpression>();
            auto objAccessExpression = propertyAccessExpressionOp->expression;
            auto typeOfObject = evaluate(objAccessExpression, switchGenContext);
            auto name = propertyAccessExpressionOp->name;

            safeCastLogic = [=, &switchGenContext](Expression caseExpr, mlir::Value constVal) {
                GenContext safeCastGenContext(switchGenContext);
                switchGenContext.insertIntoParentScope = false;

                // Safe Cast
                if (mlir::failed(checkSafeCastTypeOf(switchExpr, caseExpr, switchGenContext)))
                {
                    checkSafeCastPropertyAccessLogic(caseExpr, objAccessExpression, typeOfObject, name, constVal,
                                                     switchGenContext);
                }
            };
        }
        else
        {
            safeCastLogic = [&](Expression caseExpr, mlir::Value constVal) {};
        }

        // process without default
        for (int index = 0; index < clauses.size(); index++)
        {
            if (mlir::failed(mlirGenSwitchCase(location, switchExpr, switchValue, clauses, index, mergeBlock,
                                               defaultBlock, pendingConditions, pendingBranches,
                                               previousConditionOrFirstBranchOp, safeCastLogic, switchGenContext)))
            {
                return mlir::failure();
            }
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! SWITCH: " << switchOp << "\n");

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(ThrowStatement throwStatementAST, const GenContext &genContext)
    {
        auto location = loc(throwStatementAST);

        auto result = mlirGen(throwStatementAST->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto exception = V(result);

        auto throwOp = builder.create<mlir_ts::ThrowOp>(location, exception);

        if (!genContext.allowPartialResolve)
        {
            MLIRRTTIHelperVC rtti(builder, theModule, compileOptions);
            if (!rtti.setRTTIForType(
                location, exception.getType(), 
                [&](StringRef classFullName) { return getClassInfoByFullName(classFullName); }))
            {
                emitError(location, "Not supported type in throw");
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(TryStatement tryStatementAST, const GenContext &genContext)
    {
        auto location = loc(tryStatementAST);

        std::string varName;
        auto catchClause = tryStatementAST->catchClause;
        if (catchClause)
        {
            auto varDecl = catchClause->variableDeclaration;
            if (varDecl)
            {
                varName = MLIRHelper::getName(varDecl->name);
                if (mlir::failed(mlirGen(varDecl, VariableType::Let, genContext)))
                {
                    return mlir::failure();
                }
            }
        }

        const_cast<GenContext &>(genContext).funcOp.setPersonalityAttr(builder.getBoolAttr(true));

        auto tryOp = builder.create<mlir_ts::TryOp>(location);
        /*
        tryOp->setAttr("try_id", builder.getI64IntegerAttr((int64_t)tryOp.getOperation()));

        auto parentTryOp = tryOp->getParentOfType<mlir_ts::TryOp>();
        if (parentTryOp)
        {
            tryOp->setAttr("unwind_to", builder.getI64IntegerAttr((int64_t)parentTryOp.getOperation()));
        }
        */

        GenContext tryGenContext(genContext);
        tryGenContext.allocateVarsOutsideOfOperation = true;
        tryGenContext.currentOperation = tryOp;

        SmallVector<mlir::Type, 0> types;

        /*auto *body =*/builder.createBlock(&tryOp.getBody(), {}, types);
        /*auto *catches =*/builder.createBlock(&tryOp.getCatches(), {}, types);
        /*auto *finallyBlock =*/builder.createBlock(&tryOp.getFinallyBlock(), {}, types);

        // body
        builder.setInsertionPointToStart(&tryOp.getBody().front());
        auto result = mlirGen(tryStatementAST->tryBlock, tryGenContext);
        EXIT_IF_FAILED(result)
        if (mlir::failed(result))
        {
            return mlir::failure();
        }

        // terminator
        builder.create<mlir_ts::ResultOp>(location);

        // catches
        builder.setInsertionPointToStart(&tryOp.getCatches().front());
        if (catchClause && catchClause->block)
        {
            auto location = loc(catchClause->block);
            if (!varName.empty())
            {
                MLIRCodeLogic mcl(builder);
                auto varInfo = resolveIdentifier(location, varName, tryGenContext);
                auto varRef = mcl.GetReferenceOfLoadOp(varInfo);
                builder.create<mlir_ts::CatchOp>(location, varRef);

                if (!genContext.allowPartialResolve)
                {
                    MLIRRTTIHelperVC rtti(builder, theModule, compileOptions);
                    if (!rtti.setRTTIForType(
                        location, 
                        varInfo.getType(),
                        [&](StringRef classFullName) { return getClassInfoByFullName(classFullName); }))
                    {
                        emitError(location, "Not supported type in catch");
                        return mlir::failure();
                    }
                }
            }

            result = mlirGen(tryStatementAST->catchClause->block, tryGenContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }
        }

        // terminator
        builder.create<mlir_ts::ResultOp>(location);

        // finally
        builder.setInsertionPointToStart(&tryOp.getFinallyBlock().front());
        if (tryStatementAST->finallyBlock)
        {
            result = mlirGen(tryStatementAST->finallyBlock, tryGenContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }
        }

        // terminator
        builder.create<mlir_ts::ResultOp>(location);

        builder.setInsertionPointAfter(tryOp);
        return result;
    }

    ValueOrLogicalResult mlirGen(UnaryExpression unaryExpressionAST, const GenContext &genContext)
    {
        return mlirGen(unaryExpressionAST.as<Expression>(), genContext);
    }

    ValueOrLogicalResult mlirGen(LeftHandSideExpression leftHandSideExpressionAST, const GenContext &genContext)
    {
        return mlirGen(leftHandSideExpressionAST.as<Expression>(), genContext);
    }

    ValueOrLogicalResult mlirGenPrefixUnaryExpression(mlir::Location location, SyntaxKind opCode, mlir_ts::ConstantOp constantOp,
                                                      const GenContext &genContext)
    {
        mlir::Value value;
        auto valueAttr = constantOp.getValueAttr();

        switch (opCode)
        {
            case SyntaxKind::PlusToken:
                value = 
                    mlir::TypeSwitch<mlir::Attribute, mlir::Value>(valueAttr)
                        .Case<mlir::IntegerAttr>([&](auto intAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, constantOp.getType(), builder.getIntegerAttr(intAttr.getType(), intAttr.getValue()));
                        })
                        .Case<mlir::FloatAttr>([&](auto floatAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, constantOp.getType(), builder.getFloatAttr(floatAttr.getType(), floatAttr.getValue()));
                        })
                        .Case<mlir::StringAttr>([&](auto strAttr) {
#ifdef NUMBER_F64
                            auto floatType = mlir::Float64Type::get(builder.getContext());
#else
                            auto floatType = mlir::Float32Type::get(builder.getContext());
#endif                            
                            APFloat fValue(APFloatBase::IEEEdouble());
                            if (llvm::errorToBool(fValue.convertFromString(strAttr.getValue(), APFloat::rmNearestTiesToEven).takeError()))
                            {
                                fValue = APFloat::getNaN(fValue.getSemantics());
                            }

                            return V(builder.create<mlir_ts::ConstantOp>(
                                location, floatType, builder.getFloatAttr(floatType, fValue)));
                        })
                        .Default([](auto) {
                            return mlir::Value();
                        });                        
                break;
            case SyntaxKind::MinusToken:
                value = 
                    mlir::TypeSwitch<mlir::Attribute, mlir::Value>(valueAttr)
                        .Case<mlir::IntegerAttr>([&](auto intAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, constantOp.getType(), builder.getIntegerAttr(intAttr.getType(), -intAttr.getValue()));
                        })
                        .Case<mlir::FloatAttr>([&](auto floatAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, constantOp.getType(), builder.getFloatAttr(floatAttr.getType(), -floatAttr.getValue()));
                        })
                        .Case<mlir::StringAttr>([&](auto strAttr) {
#ifdef NUMBER_F64
                            auto floatType = mlir::Float64Type::get(builder.getContext());
#else                            
                            auto floatType = mlir::Float32Type::get(builder.getContext());
#endif
                            APFloat fValue(APFloatBase::IEEEdouble());                            
                            if (llvm::errorToBool(fValue.convertFromString(strAttr.getValue(), APFloat::rmNearestTiesToEven).takeError()))
                            {
                                fValue = APFloat::getNaN(fValue.getSemantics());
                            }

                            return V(builder.create<mlir_ts::ConstantOp>(
                                location, floatType, builder.getFloatAttr(floatType, -fValue)));
                        })                        
                        .Default([](auto) {
                            return mlir::Value();
                        });
                break;
            case SyntaxKind::TildeToken:
                value = 
                    mlir::TypeSwitch<mlir::Attribute, mlir::Value>(valueAttr)
                        .Case<mlir::IntegerAttr>([&](auto intAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, constantOp.getType(), builder.getIntegerAttr(intAttr.getType(), ~intAttr.getValue()));
                        })
                        .Case<mlir::StringAttr>([&](auto strAttr) {
                            auto intType = mlir::IntegerType::get(builder.getContext(), 32);
                            APInt iValue(32, 0);
                            iValue = llvm::to_integer(strAttr.getValue(), iValue);
                            return V(builder.create<mlir_ts::ConstantOp>(
                                location, intType, builder.getIntegerAttr(intType, ~iValue)));
                        })                         
                        .Default([](auto) {
                            return mlir::Value();
                        });
                break;
            case SyntaxKind::ExclamationToken:
                value = 
                    mlir::TypeSwitch<mlir::Attribute, mlir::Value>(valueAttr)
                        .Case<mlir::IntegerAttr>([&](auto intAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, getBooleanType(), builder.getBoolAttr(!(intAttr.getValue())));
                        })
                        .Case<mlir::StringAttr>([&](auto strAttr) {
                            return builder.create<mlir_ts::ConstantOp>(
                                location, getBooleanType(), builder.getBoolAttr(!(strAttr.getValue().empty())));
                        })                         
                        .Default([](auto) {
                            return mlir::Value();
                        });
                break;
            default:
                llvm_unreachable("not implemented");
        }

        return value;
    }

    ValueOrLogicalResult mlirGen(PrefixUnaryExpression prefixUnaryExpressionAST, const GenContext &genContext)
    {
        auto location = loc(prefixUnaryExpressionAST);

        auto opCode = prefixUnaryExpressionAST->_operator;

        auto expression = prefixUnaryExpressionAST->operand;
        auto result = mlirGen(expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expressionValue = V(result);

        // special case "-" for literal value
        if (opCode == SyntaxKind::PlusToken || opCode == SyntaxKind::MinusToken || opCode == SyntaxKind::TildeToken || opCode == SyntaxKind::ExclamationToken)
        {
            if (auto constantOp = expressionValue.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto res = mlirGenPrefixUnaryExpression(location, opCode, constantOp, genContext);
                EXIT_IF_FAILED(res)
                if (res.value)
                {
                    return res.value;
                }
            }
        }

        switch (opCode)
        {
        case SyntaxKind::ExclamationToken:
            {
                auto boolValue = expressionValue;
                if (expressionValue.getType() != getBooleanType())
                {
                    CAST(boolValue, location, getBooleanType(), expressionValue, genContext);
                }

                return V(builder.create<mlir_ts::ArithmeticUnaryOp>(location, getBooleanType(),
                                                                    builder.getI32IntegerAttr((int)opCode), boolValue));
            }
        case SyntaxKind::TildeToken:
        case SyntaxKind::PlusToken:
        case SyntaxKind::MinusToken:
            {
                auto numberValue = expressionValue;
                if (expressionValue.getType() != getNumberType() && !expressionValue.getType().isIntOrIndexOrFloat())
                {
                    CAST(numberValue, location, getNumberType(), expressionValue, genContext);
                }

                return V(builder.create<mlir_ts::ArithmeticUnaryOp>(
                    location, numberValue.getType(), builder.getI32IntegerAttr((int)opCode), numberValue));
            }
        case SyntaxKind::PlusPlusToken:
        case SyntaxKind::MinusMinusToken:
            return V(builder.create<mlir_ts::PrefixUnaryOp>(location, expressionValue.getType(),
                                                            builder.getI32IntegerAttr((int)opCode), expressionValue));
        default:
            llvm_unreachable("not implemented");
        }
    }

    ValueOrLogicalResult mlirGen(PostfixUnaryExpression postfixUnaryExpressionAST, const GenContext &genContext)
    {
        auto location = loc(postfixUnaryExpressionAST);

        auto opCode = postfixUnaryExpressionAST->_operator;

        auto expression = postfixUnaryExpressionAST->operand;
        auto result = mlirGen(expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expressionValue = V(result);

        switch (opCode)
        {
        case SyntaxKind::PlusPlusToken:
        case SyntaxKind::MinusMinusToken:
            return V(builder.create<mlir_ts::PostfixUnaryOp>(location, expressionValue.getType(),
                                                             builder.getI32IntegerAttr((int)opCode), expressionValue));
        default:
            llvm_unreachable("not implemented");
        }
    }

    // TODO: rewrite code, you can set IfOp result type later, see function anyOrUndefined
    ValueOrLogicalResult mlirGen(ConditionalExpression conditionalExpressionAST, const GenContext &genContext)
    {
        auto location = loc(conditionalExpressionAST);

        // condition
        auto condExpression = conditionalExpressionAST->condition;
        auto result = mlirGen(condExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto condValue = V(result);

        if (condValue.getType() != getBooleanType())
        {
            CAST(condValue, location, getBooleanType(), condValue, genContext);
        }

        // detect value type
        // TODO: sync types for 'when' and 'else'

        auto resultWhenFalseType = evaluate(conditionalExpressionAST->whenFalse, genContext);

        mlir::Type resultWhenTrueType;
        {
            // check if we do safe-cast here
            SymbolTableScopeT varScope(symbolTable);
            checkSafeCast(conditionalExpressionAST->condition, V(result), genContext);
            resultWhenTrueType = evaluate(conditionalExpressionAST->whenTrue, genContext);
        }

        auto defaultUnionType = getUnionType(resultWhenTrueType, resultWhenFalseType);
        auto merged = false;
        auto resultType = mth.findBaseType(resultWhenTrueType, resultWhenFalseType, merged, defaultUnionType);

        if (genContext.allowPartialResolve)
        {
            if (!resultType)
            {
                return mlir::failure();
            }

            if (!resultWhenTrueType || !resultWhenFalseType)
            {
                // return undef value
            }

            auto udef = builder.create<mlir_ts::UndefOp>(location, mlir::TypeRange{resultType});
            return V(udef);
        }

        auto ifOp = builder.create<mlir_ts::IfOp>(location, mlir::TypeRange{resultType}, condValue, true);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());
        auto whenTrueExpression = conditionalExpressionAST->whenTrue;

        mlir::Value resultTrue;
        {
            // check if we do safe-cast here
            SymbolTableScopeT varScope(symbolTable);
            checkSafeCast(conditionalExpressionAST->condition, V(result), genContext);
            auto result = mlirGen(whenTrueExpression, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            resultTrue = V(result);
        }

        CAST_A(trueRes, location, resultType, resultTrue, genContext);
        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{trueRes});

        builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
        auto whenFalseExpression = conditionalExpressionAST->whenFalse;
        auto result2 = mlirGen(whenFalseExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result2)
        auto resultFalse = V(result2);

        CAST_A(falseRes, location, resultType, resultFalse, genContext)
        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{falseRes});

        builder.setInsertionPointAfter(ifOp);

        return ifOp.getResult(0);
    }

    ValueOrLogicalResult mlirGenAndOrLogic(BinaryExpression binaryExpressionAST, const GenContext &genContext,
                                           bool andOp, bool saveResult)
    {
        auto location = loc(binaryExpressionAST);

        auto leftExpression = binaryExpressionAST->left;
        auto rightExpression = binaryExpressionAST->right;

        // condition
        auto result = mlirGen(leftExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto leftExpressionValue = V(result);

        auto resultWhenFalseType = evaluate(rightExpression, genContext);
        auto resultType = getUnionType(leftExpressionValue.getType(), resultWhenFalseType);

        CAST_A(condValue, location, getBooleanType(), leftExpressionValue, genContext);

        auto ifOp = builder.create<mlir_ts::IfOp>(location, mlir::TypeRange{resultType}, condValue, true);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());
        mlir::Value resultTrue;
        if (andOp)
        {
            auto result = mlirGen(rightExpression, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            resultTrue = V(result);
        }
        else
        {
            resultTrue = leftExpressionValue;
        }

        if (andOp)
        {
            VALIDATE(resultTrue, location)
        }

        // sync left part
        if (resultType != resultTrue.getType())
        {
            CAST(resultTrue, location, resultType, resultTrue, genContext);
        }

        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{resultTrue});

        builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
        mlir::Value resultFalse;
        if (andOp)
        {
            resultFalse = leftExpressionValue;
        }
        else
        {
            auto result = mlirGen(rightExpression, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            resultFalse = V(result);
        }

        if (!andOp)
        {
            VALIDATE(resultFalse, location)
        }

        // sync right part
        if (resultType != resultFalse.getType())
        {
            CAST(resultFalse, location, resultType, resultFalse, genContext);
        }

        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{resultFalse});

        builder.setInsertionPointAfter(ifOp);

        auto resultFirst = ifOp.getResults().front();
        if (saveResult)
        {
            return mlirGenSaveLogicOneItem(location, leftExpressionValue, resultFirst, genContext);
        }

        return resultFirst;
    }

    ValueOrLogicalResult mlirGenQuestionQuestionLogic(BinaryExpression binaryExpressionAST, bool saveResult,
                                                      const GenContext &genContext)
    {
        auto location = loc(binaryExpressionAST);

        auto leftExpression = binaryExpressionAST->left;
        auto rightExpression = binaryExpressionAST->right;

        // condition
        auto result = mlirGen(leftExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto leftExpressionValue = V(result);

        auto resultWhenFalseType = evaluate(rightExpression, genContext);
        auto defaultUnionType = getUnionType(leftExpressionValue.getType(), resultWhenFalseType);
        auto merged = false;
        auto resultType = mth.findBaseType(resultWhenFalseType, leftExpressionValue.getType(), merged, defaultUnionType);

        // extarct value from optional type
        auto actualLeftValue = leftExpressionValue;
        auto hasOptional = false;
        if (auto optType = actualLeftValue.getType().dyn_cast<mlir_ts::OptionalType>())
        {
            hasOptional = true;
            CAST(actualLeftValue, location, optType.getElementType(), leftExpressionValue, genContext);
        }

        CAST_A(opaqueValueOfLeftValue, location, getOpaqueType(), actualLeftValue, genContext);

        auto nullVal = builder.create<mlir_ts::NullOp>(location, getNullType());

        auto compareToNull = builder.create<mlir_ts::LogicalBinaryOp>(
            location, getBooleanType(), builder.getI32IntegerAttr((int)SyntaxKind::EqualsEqualsEqualsToken), opaqueValueOfLeftValue,
            nullVal);

        mlir::Value ifCond = compareToNull;
        if (hasOptional)
        {
            CAST_A(hasValue, location, getBooleanType(), leftExpressionValue, genContext);      
            CAST_A(isFalse, location, getBooleanType(), mlirGenBooleanValue(location, false), genContext);
            auto compareToFalse = builder.create<mlir_ts::LogicalBinaryOp>(
                location, getBooleanType(), builder.getI32IntegerAttr((int)SyntaxKind::EqualsEqualsEqualsToken), isFalse,
                hasValue);

            auto orOp = builder.create<mlir_ts::ArithmeticBinaryOp>(
                location, getBooleanType(), builder.getI32IntegerAttr((int)SyntaxKind::BarToken), compareToFalse,
                compareToNull);   

            ifCond = orOp;            
        }

        auto ifOp = builder.create<mlir_ts::IfOp>(location, mlir::TypeRange{resultType}, ifCond, true);

        builder.setInsertionPointToStart(&ifOp.getThenRegion().front());
        auto result2 = mlirGen(rightExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result2)
        auto resultTrue = V(result2);

        // sync left part
        if (resultType != resultTrue.getType())
        {
            CAST(resultTrue, location, resultType, resultTrue, genContext);
        }

        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{resultTrue});

        builder.setInsertionPointToStart(&ifOp.getElseRegion().front());
        auto resultFalse = leftExpressionValue;

        // sync right part
        if (resultType != resultFalse.getType())
        {
            CAST(resultFalse, location, resultType, resultFalse, genContext);
        }

        builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{resultFalse});

        builder.setInsertionPointAfter(ifOp);

        auto ifResult = ifOp.getResults().front();
        if (saveResult)
        {
            return mlirGenSaveLogicOneItem(location, leftExpressionValue, ifResult, genContext);
        }

        return ifResult;
    }

    ValueOrLogicalResult mlirGenInLogic(BinaryExpression binaryExpressionAST, const GenContext &genContext)
    {
        // Supports only array now
        auto location = loc(binaryExpressionAST);

        NodeFactory nf(NodeFactoryFlags::None);

        if (auto hasLength = evaluateProperty(binaryExpressionAST->right, LENGTH_FIELD_NAME, genContext))
        {
            auto cond1 = nf.createBinaryExpression(
                binaryExpressionAST->left, nf.createToken(SyntaxKind::LessThanToken),
                nf.createPropertyAccessExpression(binaryExpressionAST->right, nf.createIdentifier(S(LENGTH_FIELD_NAME))));

            auto cond2 = nf.createBinaryExpression(
                binaryExpressionAST->left, nf.createToken(SyntaxKind::GreaterThanEqualsToken), nf.createNumericLiteral(S("0")));

            auto cond = nf.createBinaryExpression(cond1, nf.createToken(SyntaxKind::AmpersandAmpersandToken), cond2);

            return mlirGen(cond, genContext);
        }

        auto resultLeft = mlirGen(binaryExpressionAST->left, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(resultLeft)
        auto leftExpressionValue = V(resultLeft);

        if (!isConstValue(leftExpressionValue))
        {
            emitError(loc(binaryExpressionAST->left), "not supported");
            return mlir::failure();
        }

        auto resultRight = mlirGen(binaryExpressionAST->right, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(resultRight)
        auto rightExpressionValue = V(resultRight);

        if (rightExpressionValue.getType().isa<mlir_ts::UnionType>())
        {
            emitError(loc(binaryExpressionAST->right), "not supported");
            return mlir::failure();
        }

        if (auto constantOp = leftExpressionValue.getDefiningOp<mlir_ts::ConstantOp>())        
        {
            auto hasField = !!mth.getFieldTypeByFieldName(rightExpressionValue.getType(), constantOp.getValue());
            return mlirGenBooleanValue(loc(binaryExpressionAST->right), hasField);
        }

        emitError(location, "not supported");
        return mlir::failure();
    }

    ValueOrLogicalResult mlirGenCallThisMethod(mlir::Location location, mlir::Value thisValue, StringRef methodName,
                                               NodeArray<TypeNode> typeArguments, NodeArray<Expression> arguments,
                                               const GenContext &genContext)
    {
        // to remove temp var after call
        SymbolTableScopeT varScope(symbolTable);

        auto varDecl = std::make_shared<VariableDeclarationDOM>(THIS_TEMPVAR_NAME, thisValue.getType(), location);
        DECLARE(varDecl, thisValue);

        NodeFactory nf(NodeFactoryFlags::None);

        auto thisToken = nf.createIdentifier(S(THIS_TEMPVAR_NAME));
        auto callLogic = nf.createCallExpression(
            nf.createPropertyAccessExpression(thisToken, nf.createIdentifier(stows(methodName.str()))), typeArguments,
            arguments);

        return mlirGen(callLogic, genContext);
    }

    ValueOrLogicalResult mlirGenInstanceOfLogic(BinaryExpression binaryExpressionAST, const GenContext &genContext)
    {
        auto location = loc(binaryExpressionAST);

        // check if we need to call hasInstance
        if (auto hasInstanceType = evaluateProperty(binaryExpressionAST->right, SYMBOL_HAS_INSTANCE, genContext))
        {
            auto resultRight = mlirGen(binaryExpressionAST->right, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(resultRight)
            auto resultRightValue = V(resultRight);
            
            return mlirGenCallThisMethod(location, resultRightValue, SYMBOL_HAS_INSTANCE, undefined, {binaryExpressionAST->left}, genContext);
        }        

        auto result2 = mlirGen(binaryExpressionAST->left, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result2)
        auto result = V(result2);

        auto resultType = result.getType();
        if (auto refType = resultType.dyn_cast<mlir_ts::RefType>())
        {
            resultType = refType.getElementType();
        }

        resultType = mth.wideStorageType(resultType);

        // TODO: should it be mlirGen?
        auto type = getTypeByTypeName(binaryExpressionAST->right, genContext);
        if (mth.isNoneType(type))
        {
            emitError(location, "type of instanceOf can't be resolved.");
            return mlir::failure();
        }

        type = mth.wideStorageType(type);

#ifdef ENABLE_RTTI
        if (auto classType = type.dyn_cast<mlir_ts::ClassType>())
        {
            auto classInfo = getClassInfoByFullName(classType.getName().getValue());
            if (resultType.isa<mlir_ts::ClassType>())
            {
                NodeFactory nf(NodeFactoryFlags::None);
                NodeArray<Expression> argumentsArray;
                argumentsArray.push_back(nf.createPropertyAccessExpression(binaryExpressionAST->right, nf.createIdentifier(S(RTTI_NAME))));
                return mlirGenCallThisMethod(location, result, INSTANCEOF_NAME, undefined, argumentsArray, genContext);
            }

            if (resultType.isa<mlir_ts::AnyType>())
            {
                auto typeOfAnyValue = builder.create<mlir_ts::TypeOfAnyOp>(location, getStringType(), result);
                auto classStrConst =
                    builder.create<mlir_ts::ConstantOp>(location, getStringType(), builder.getStringAttr("class"));
                auto cmpResult = builder.create<mlir_ts::StringCompareOp>(
                    location, getBooleanType(), typeOfAnyValue, classStrConst,
                    builder.getI32IntegerAttr((int)SyntaxKind::EqualsEqualsToken));

                MLIRCodeLogicHelper mclh(builder, location);
                auto returnValue = mclh.conditionalExpression(
                    getBooleanType(), cmpResult,
                    [&](mlir::OpBuilder &builder, mlir::Location location) {
                        // TODO: test cast value
                        auto thisPtrValue = cast(location, getOpaqueType(), result, genContext);

                        // get VTable we can use VTableOffset
                        auto vtablePtr = builder.create<mlir_ts::VTableOffsetRefOp>(location, getOpaqueType(),
                                                                                    thisPtrValue, 0 /*VTABLE index*/);

                        // get InstanceOf method, this is 0 index in vtable
                        auto instanceOfPtr = builder.create<mlir_ts::VTableOffsetRefOp>(
                            location, getOpaqueType(), vtablePtr, 0 /*InstanceOf index*/);

                        auto classRefVal = mlirGen(binaryExpressionAST->right, genContext);

                        auto resultRtti = mlirGenPropertyAccessExpression(location, classRefVal, RTTI_NAME, genContext);
                        auto rttiOfClassValue = V(resultRtti);
                        if (classInfo->isDynamicImport)
                        {
                            if (auto valueRefType = rttiOfClassValue.getType().dyn_cast<mlir_ts::RefType>())
                            {
                                rttiOfClassValue = builder.create<mlir_ts::LoadOp>(location, valueRefType.getElementType(), rttiOfClassValue);
                            }
                            else
                            {
                                llvm_unreachable("not implemented");
                            }
                        }

                        assert(rttiOfClassValue);

                        auto instanceOfFuncType = mlir_ts::FunctionType::get(
                            builder.getContext(), SmallVector<mlir::Type>{getOpaqueType(), getStringType()},
                            SmallVector<mlir::Type>{getBooleanType()});

                        // TODO: check result
                        auto result = cast(location, instanceOfFuncType, instanceOfPtr, genContext);
                        auto funcPtr = V(result);

                        // call methos, we need to send, this, and rtti info
                        auto callResult = builder.create<mlir_ts::CallIndirectOp>(
                            MLIRHelper::getCallSiteLocation(funcPtr, location),
                            funcPtr, mlir::ValueRange{thisPtrValue, rttiOfClassValue});

                        return callResult.getResult(0);
                    },
                    [&](mlir::OpBuilder &builder, mlir::Location location) { // default false value
                                                                             // compare typeOfValue
                        return builder.create<mlir_ts::ConstantOp>(location, getBooleanType(),
                                                                   builder.getBoolAttr(false));
                    });

                return returnValue;
            }
        }
#endif

        LLVM_DEBUG(llvm::dbgs() << "!! instanceOf precalc value: " << (resultType == type) << " '" << resultType
                                << "' is '" << type << "'\n";);

        // default logic
        return V(
            builder.create<mlir_ts::ConstantOp>(location, getBooleanType(), builder.getBoolAttr(resultType == type)));
    }

    ValueOrLogicalResult evaluateBinaryOp(mlir::Location location, SyntaxKind opCode, mlir_ts::ConstantOp leftConstOp,
                                 mlir_ts::ConstantOp rightConstOp, const GenContext &genContext)
    {
        // todo string concat
        auto leftStrAttr = leftConstOp.getValueAttr().dyn_cast_or_null<mlir::StringAttr>();
        auto rightStrAttr = rightConstOp.getValueAttr().dyn_cast_or_null<mlir::StringAttr>();        
        if (leftStrAttr && rightStrAttr)
        {
            auto leftStr = leftStrAttr.getValue();
            auto rightStr = rightStrAttr.getValue();

            std::string result;
            switch (opCode)
            {
                case SyntaxKind::PlusToken:
                    result = leftStr;
                    result += rightStr;
                    break;
                default:
                    emitError(location) << "can't do binary operation on constants: " << leftConstOp.getValueAttr() << " and " << rightConstOp.getValueAttr() << "";
                    return mlir::failure();
            }

            return V(builder.create<mlir_ts::ConstantOp>(location, getStringType(), builder.getStringAttr(result)));
        }

        auto leftIntAttr = leftConstOp.getValueAttr().dyn_cast_or_null<mlir::IntegerAttr>();
        auto rightIntAttr = rightConstOp.getValueAttr().dyn_cast_or_null<mlir::IntegerAttr>();
        auto resultType = leftConstOp.getType();
        if (leftIntAttr && rightIntAttr)
        {
            auto leftInt = leftIntAttr.getInt();
            auto rightInt = rightIntAttr.getInt();            
            int64_t result = 0;
            switch (opCode)
            {
            case SyntaxKind::PlusToken:
                result = leftInt + rightInt;
                break;
            case SyntaxKind::MinusToken:
                result = leftInt - rightInt;
                break;
            case SyntaxKind::AsteriskToken:
                result = leftInt * rightInt;
                break;
            case SyntaxKind::LessThanLessThanToken:
                result = leftInt << rightInt;
                break;
            case SyntaxKind::GreaterThanGreaterThanToken:
                result = leftInt >> rightInt;
                break;
            case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
                result = (uint64_t)leftInt >> rightInt;
                break;
            case SyntaxKind::AmpersandToken:
                result = leftInt & rightInt;
                break;
            case SyntaxKind::BarToken:
                result = leftInt | rightInt;
                break;
            case SyntaxKind::CaretToken:
                result = leftInt ^ rightInt;
                break;
            default:
                emitError(location) << "can't do binary operation on constants: " << leftConstOp.getValueAttr() << " and " << rightConstOp.getValueAttr() << "";
                return mlir::failure();
            }

            return V(builder.create<mlir_ts::ConstantOp>(location, resultType, builder.getI64IntegerAttr(result)));
        }

        auto leftFloatAttr = leftConstOp.getValueAttr().dyn_cast_or_null<mlir::FloatAttr>();
        auto rightFloatAttr = rightConstOp.getValueAttr().dyn_cast_or_null<mlir::FloatAttr>();
        if (leftFloatAttr && rightFloatAttr)
        {
            auto leftFloat = leftFloatAttr.getValueAsDouble();
            auto rightFloat = rightFloatAttr.getValueAsDouble();
            double result = 0;
            switch (opCode)
            {
            case SyntaxKind::PlusToken:
                result = leftFloat + rightFloat;
                break;
            case SyntaxKind::MinusToken:
                result = leftFloat - rightFloat;
                break;
            case SyntaxKind::AsteriskToken:
                result = leftFloat * rightFloat;
                break;
            case SyntaxKind::LessThanLessThanToken:
                result = (int64_t)leftFloat << (int64_t)rightFloat;
                break;
            case SyntaxKind::GreaterThanGreaterThanToken:
                result = (int64_t)leftFloat >> (int64_t)rightFloat;
                break;
            case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
                result = (uint64_t)leftFloat >> (int64_t)rightFloat;
                break;
            case SyntaxKind::AmpersandToken:
                result = (int64_t)leftFloat & (int64_t)rightFloat;
                break;
            case SyntaxKind::BarToken:
                result = (int64_t)leftFloat | (int64_t)rightFloat;
                break;
            case SyntaxKind::CaretToken:
                result = (int64_t)leftFloat ^ (int64_t)rightFloat;
                break;
            default:
                emitError(location) << "can't do binary operation on constants: " << leftConstOp.getValueAttr() << " and " << rightConstOp.getValueAttr() << "";
                return mlir::failure();
            }

            return V(builder.create<mlir_ts::ConstantOp>(location, resultType, builder.getFloatAttr(leftFloatAttr.getType(), result)));
        }    

        return mlir::failure();    
    }

    ValueOrLogicalResult mlirGenSaveLogicOneItem(mlir::Location location, mlir::Value leftExpressionValue,
                                                 mlir::Value rightExpressionValue, const GenContext &genContext)
    {
        if (!leftExpressionValue)
        {
            return mlir::failure();
        }

        auto leftExpressionValueBeforeCast = leftExpressionValue;

        if (leftExpressionValue.getType() != rightExpressionValue.getType())
        {
            if (rightExpressionValue.getType().dyn_cast<mlir_ts::CharType>())
            {
                CAST(rightExpressionValue, location, getStringType(), rightExpressionValue, genContext);
            }
        }

        auto savingValue = rightExpressionValue;
        if (!savingValue)
        {
            return mlir::failure();
        }

        auto syncSavingValue = [&](mlir::Type destType) {
            if (destType != savingValue.getType())
            {
                savingValue = cast(location, destType, savingValue, genContext);
            }
        };

        // TODO: finish it for field access, review CodeLogicHelper.saveResult
        if (auto loadOp = leftExpressionValueBeforeCast.getDefiningOp<mlir_ts::LoadOp>())
        {
            mlir::Type destType =
                TypeSwitch<mlir::Type, mlir::Type>(loadOp.getReference().getType())
                    .Case<mlir_ts::RefType>([&](auto refType) { return refType.getElementType(); })
                    .Case<mlir_ts::BoundRefType>([&](auto boundRefType) { return boundRefType.getElementType(); });

            assert(destType);

            LLVM_DEBUG(llvm::dbgs() << "\n!! Dest type: " << destType << "\n";);

            syncSavingValue(destType);
            if (!savingValue)
            {
                return mlir::failure();
            }

            // TODO: when saving const array into variable we need to allocate space and copy array as we need to have
            // writable array
            builder.create<mlir_ts::StoreOp>(location, savingValue, loadOp.getReference());
        }
        else if (auto accessorOp = leftExpressionValueBeforeCast.getDefiningOp<mlir_ts::AccessorOp>())
        {
            syncSavingValue(accessorOp.getType());
            if (!savingValue)
            {
                return mlir::failure();
            }

            if (!accessorOp.getSetAccessor().has_value())
            {
                emitError(location) << "property does not have set accessor";
                return mlir::failure();
            }

            auto callRes =
                builder.create<mlir_ts::CallOp>(location, accessorOp.getSetAccessor().value(),
                                                mlir::TypeRange{}, mlir::ValueRange{savingValue});
        }
        else if (auto thisAccessorOp = leftExpressionValueBeforeCast.getDefiningOp<mlir_ts::ThisAccessorOp>())
        {
            syncSavingValue(thisAccessorOp.getType());
            if (!savingValue)
            {
                return mlir::failure();
            }

            if (!thisAccessorOp.getSetAccessor().has_value())
            {
                emitError(location) << "property does not have set accessor";
                return mlir::failure();
            }

            auto callRes = builder.create<mlir_ts::CallOp>(location, thisAccessorOp.getSetAccessor().value(),
                                                           mlir::TypeRange{},
                                                           mlir::ValueRange{thisAccessorOp.getThisVal(), savingValue});
        }
        /*
        else if (auto createBoundFunction =
        leftExpressionValueBeforeCast.getDefiningOp<mlir_ts::CreateBoundFunctionOp>())
        {
            // TODO: i should not allow to change interface
            return mlirGenSaveLogicOneItem(location, createBoundFunction.getFunc(), rightExpressionValue, genContext);
        }
        */
        else
        {
            LLVM_DEBUG(dbgs() << "\n!! left expr.: " << leftExpressionValueBeforeCast << " ...\n";);
            emitError(location, "saving to constant object");
            return mlir::failure();
        }

        return savingValue;
    }

    ValueOrLogicalResult mlirGenSaveLogic(BinaryExpression binaryExpressionAST, const GenContext &genContext)
    {
        auto location = loc(binaryExpressionAST);

        auto leftExpression = binaryExpressionAST->left;
        auto rightExpression = binaryExpressionAST->right;

        if (leftExpression == SyntaxKind::ArrayLiteralExpression)
        {
            return mlirGenSaveLogicArray(location, leftExpression.as<ArrayLiteralExpression>(), rightExpression,
                                         genContext);
        }

        if (leftExpression == SyntaxKind::ObjectLiteralExpression)
        {
            return mlirGenSaveLogicObject(location, leftExpression.as<ObjectLiteralExpression>(), rightExpression,
                                          genContext);
        }

        auto result = mlirGen(leftExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto leftExpressionValue = V(result);

        auto rightExprGenContext = GenContext(genContext);
        rightExprGenContext.clearReceiverTypes();

        if (mth.isAnyFunctionType(leftExpressionValue.getType()))
        {
            rightExprGenContext.receiverFuncType = leftExpressionValue.getType();
        }

        rightExprGenContext.receiverType = leftExpressionValue.getType();

        auto result2 = mlirGen(rightExpression, rightExprGenContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result2)
        auto rightExpressionValue = V(result2);

        return mlirGenSaveLogicOneItem(location, leftExpressionValue, rightExpressionValue, genContext);
    }

    ValueOrLogicalResult mlirGenSaveLogicArray(mlir::Location location, ArrayLiteralExpression arrayLiteralExpression,
                                               Expression rightExpression, const GenContext &genContext)
    {
        auto result = mlirGen(rightExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto rightExpressionValue = V(result);

        LLVM_DEBUG(dbgs() << "\n!! right expr.: " << rightExpressionValue << "\n";);

        auto isTuple = false;
        mlir::Type elementType;
        mlir_ts::TupleType tupleType;
        TypeSwitch<mlir::Type>(rightExpressionValue.getType())
            .Case<mlir_ts::ArrayType>([&](auto arrayType) { elementType = arrayType.getElementType(); })
            .Case<mlir_ts::ConstArrayType>([&](auto constArrayType) { elementType = constArrayType.getElementType(); })
            .Case<mlir_ts::TupleType>([&](auto tupleType_) { isTuple = true; tupleType = tupleType_; })
            .Case<mlir_ts::ConstTupleType>([&](auto constTupleType) { isTuple = true; tupleType = mth.convertConstTupleTypeToTupleType(constTupleType); })
            .Default([](auto type) { llvm_unreachable("not implemented"); });

        if (!isTuple)
        {
            auto index = 0;
            for (auto leftItem : arrayLiteralExpression->elements)
            {
                auto result = mlirGen(leftItem, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto leftExpressionValue = V(result);

                // special case for [a = 1, b = 2] = [2, 3];
                if (leftItem == SyntaxKind::BinaryExpression)
                {
                    auto binExpr = leftItem.as<BinaryExpression>();
                    auto result = mlirGen(binExpr->left, genContext);
                    EXIT_IF_FAILED_OR_NO_VALUE(result)
                    leftExpressionValue = V(result);
                }

                // TODO: unify array access like Property access
                auto indexValue =
                    builder.create<mlir_ts::ConstantOp>(location, builder.getI32Type(), builder.getI32IntegerAttr(index++));

                auto elemRef = builder.create<mlir_ts::ElementRefOp>(location, mlir_ts::RefType::get(elementType),
                                                                    rightExpressionValue, indexValue);
                auto rightValue = builder.create<mlir_ts::LoadOp>(location, elementType, elemRef);

                if (mlir::failed(mlirGenSaveLogicOneItem(location, leftExpressionValue, rightValue, genContext)))
                {
                    return mlir::failure();
                }
            }
        }
        else
        {
            auto index = 0;
            for (auto leftItem : arrayLiteralExpression->elements)
            {
                auto result = mlirGen(leftItem, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto leftExpressionValue = V(result);

                // special case for [a = 1, b = "abc"] = [2, "def"];
                if (leftItem == SyntaxKind::BinaryExpression)
                {
                    auto binExpr = leftItem.as<BinaryExpression>();
                    auto result = mlirGen(binExpr->left, genContext);
                    EXIT_IF_FAILED_OR_NO_VALUE(result)
                    leftExpressionValue = V(result);
                }

                MLIRPropertyAccessCodeLogic cl(builder, location, rightExpressionValue, builder.getI32IntegerAttr(index));
                auto rightValue = cl.Tuple(tupleType, true);
                if (!rightValue)
                {
                    return mlir::failure();
                }

                if (mlir::failed(mlirGenSaveLogicOneItem(location, leftExpressionValue, rightValue, genContext)))
                {
                    return mlir::failure();
                }

                index++;
            }

        }

        // no passing value
        return mlir::success();
    }

    ValueOrLogicalResult mlirGenSaveLogicObject(mlir::Location location,
                                                ObjectLiteralExpression objectLiteralExpression,
                                                Expression rightExpression, const GenContext &genContext)
    {
        auto result = mlirGen(rightExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto rightExpressionValue = V(result);

        auto index = 0;
        for (auto item : objectLiteralExpression->properties)
        {
            if (item == SyntaxKind::PropertyAssignment)
            {
                auto propertyAssignment = item.as<PropertyAssignment>();

                auto propertyName = MLIRHelper::getName(propertyAssignment->name);

                auto result = mlirGen(propertyAssignment->initializer, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto ident = V(result);

                auto subInit =
                    mlirGenPropertyAccessExpression(location, rightExpressionValue, propertyName, false, genContext);

                if (mlir::failed(mlirGenSaveLogicOneItem(location, ident, subInit, genContext)))
                {
                    return mlir::failure();
                }
            }
            else if (item == SyntaxKind::ShorthandPropertyAssignment)
            {
                auto shorthandPropertyAssignment = item.as<ShorthandPropertyAssignment>();

                auto propertyName = MLIRHelper::getName(shorthandPropertyAssignment->name);
                auto varName = propertyName;

                auto ident = resolveIdentifier(location, varName, genContext);

                auto subInit =
                    mlirGenPropertyAccessExpression(location, rightExpressionValue, propertyName, false, genContext);

                if (mlir::failed(mlirGenSaveLogicOneItem(location, ident, subInit, genContext)))
                {
                    return mlir::failure();
                }
            }
            else
            {
                llvm_unreachable("not implemented");
            }

            index++;
        }

        // no passing value
        return mlir::success();
    }

    mlir::LogicalResult unwrapForBinaryOp(SyntaxKind opCode, mlir::Value &leftExpressionValue,
                                          mlir::Value &rightExpressionValue, const GenContext &genContext)
    {
        if (opCode == SyntaxKind::CommaToken)
        {
            return mlir::success();
        }

        auto leftLoc = leftExpressionValue.getLoc();
        auto rightLoc = rightExpressionValue.getLoc();

        // type preprocess
        // TODO: temporary hack
        if (auto leftType = leftExpressionValue.getType().dyn_cast<mlir_ts::LiteralType>())
        {
            CAST(leftExpressionValue, leftLoc, leftType.getElementType(), leftExpressionValue, genContext);
        }

        if (auto rightType = rightExpressionValue.getType().dyn_cast<mlir_ts::LiteralType>())
        {
            CAST(rightExpressionValue, rightLoc, rightType.getElementType(), rightExpressionValue, genContext);
        }
        // end of hack

        if (leftExpressionValue.getType() != rightExpressionValue.getType())
        {
            // TODO: temporary hack
            if (leftExpressionValue.getType().dyn_cast<mlir_ts::CharType>())
            {
                CAST(leftExpressionValue, leftLoc, getStringType(), leftExpressionValue, genContext);
            }

            if (rightExpressionValue.getType().dyn_cast<mlir_ts::CharType>())
            {
                CAST(rightExpressionValue, rightLoc, getStringType(), rightExpressionValue, genContext);
            }

            // end todo

            if (!MLIRLogicHelper::isLogicOp(opCode))
            {
                // TODO: review it
                // cast from optional<T> type
                if (auto leftOptType = leftExpressionValue.getType().dyn_cast<mlir_ts::OptionalType>())
                {
                    leftExpressionValue =
                        builder.create<mlir_ts::ValueOrDefaultOp>(leftLoc, leftOptType.getElementType(), leftExpressionValue);
                }

                if (auto rightOptType = rightExpressionValue.getType().dyn_cast<mlir_ts::OptionalType>())
                {
                    rightExpressionValue =
                        builder.create<mlir_ts::ValueOrDefaultOp>(rightLoc, rightOptType.getElementType(), rightExpressionValue);
                }
            }
        }
        else if (!MLIRLogicHelper::isLogicOp(opCode))
        {
            // TODO: review it
            // special case both are optionals
            if (auto leftOptType = leftExpressionValue.getType().dyn_cast<mlir_ts::OptionalType>())
            {
                if (auto rightOptType = rightExpressionValue.getType().dyn_cast<mlir_ts::OptionalType>())
                {
                    leftExpressionValue =
                        builder.create<mlir_ts::ValueOrDefaultOp>(leftLoc, leftOptType.getElementType(), leftExpressionValue);
                    rightExpressionValue =
                        builder.create<mlir_ts::ValueOrDefaultOp>(rightLoc, rightOptType.getElementType(), rightExpressionValue);
                }
            }
        }

        return mlir::success();
    }

    // TODO: review it, seems like big hack
    mlir::LogicalResult adjustTypesForBinaryOp(SyntaxKind opCode, mlir::Value &leftExpressionValue,
                                               mlir::Value &rightExpressionValue, const GenContext &genContext)
    {
        if (opCode == SyntaxKind::CommaToken)
        {
            return mlir::success();
        }

        auto leftLoc = leftExpressionValue.getLoc();
        auto rightLoc = rightExpressionValue.getLoc();

        // cast step
        switch (opCode)
        {
        case SyntaxKind::CommaToken:
            // no cast needed
            break;
        case SyntaxKind::LessThanLessThanToken:
        case SyntaxKind::GreaterThanGreaterThanToken:
        case SyntaxKind::GreaterThanGreaterThanGreaterThanToken:
        case SyntaxKind::AmpersandToken:
        case SyntaxKind::BarToken:
        case SyntaxKind::CaretToken:
            // cast to int
            if (leftExpressionValue.getType() != builder.getI32Type())
            {
                CAST(leftExpressionValue, leftLoc, builder.getI32Type(), leftExpressionValue, genContext);
            }

            if (rightExpressionValue.getType() != builder.getI32Type())
            {
                CAST(rightExpressionValue, rightLoc, builder.getI32Type(), rightExpressionValue, genContext);
            }

            break;
        case SyntaxKind::SlashToken:
        case SyntaxKind::PercentToken:
        case SyntaxKind::AsteriskAsteriskToken:

            if (leftExpressionValue.getType() != getNumberType())
            {
                CAST(leftExpressionValue, leftLoc, getNumberType(), leftExpressionValue, genContext);
            }

            if (rightExpressionValue.getType() != getNumberType())
            {
                CAST(rightExpressionValue, rightLoc, getNumberType(), rightExpressionValue, genContext);
            }

            break;
        case SyntaxKind::AsteriskToken:
        case SyntaxKind::MinusToken:
        case SyntaxKind::EqualsEqualsToken:
        case SyntaxKind::EqualsEqualsEqualsToken:
        case SyntaxKind::ExclamationEqualsToken:
        case SyntaxKind::ExclamationEqualsEqualsToken:
        case SyntaxKind::GreaterThanToken:
        case SyntaxKind::GreaterThanEqualsToken:
        case SyntaxKind::LessThanToken:
        case SyntaxKind::LessThanEqualsToken:

            if (leftExpressionValue.getType().isa<mlir_ts::UndefinedType>() || rightExpressionValue.getType().isa<mlir_ts::UndefinedType>())
            {
                break;
            }

            if (leftExpressionValue.getType() != rightExpressionValue.getType())
            {
                // cast to base type
                auto hasNumber = leftExpressionValue.getType() == getNumberType() ||
                                 rightExpressionValue.getType() == getNumberType();
                if (hasNumber)
                {
                    if (leftExpressionValue.getType() != getNumberType())
                    {
                        CAST(leftExpressionValue, leftLoc, getNumberType(), leftExpressionValue, genContext);
                    }

                    if (rightExpressionValue.getType() != getNumberType())
                    {
                        CAST(rightExpressionValue, rightLoc, getNumberType(), rightExpressionValue, genContext);
                    }
                }
                else
                {
                    auto hasI32 = leftExpressionValue.getType() == builder.getI32Type() ||
                                  rightExpressionValue.getType() == builder.getI32Type();
                    if (hasI32)
                    {
                        if (leftExpressionValue.getType() != builder.getI32Type())
                        {
                            CAST(leftExpressionValue, leftLoc, builder.getI32Type(), leftExpressionValue, genContext);
                        }

                        if (rightExpressionValue.getType() != builder.getI32Type())
                        {
                            CAST(rightExpressionValue, rightLoc, builder.getI32Type(), rightExpressionValue, genContext);
                        }
                    }
                }
            }

            break;
        default:
            auto resultType = leftExpressionValue.getType();
            if (rightExpressionValue.getType().isa<mlir_ts::StringType>())
            {
                resultType = getStringType();
                if (resultType != leftExpressionValue.getType())
                {
                    CAST(leftExpressionValue, leftLoc, resultType, leftExpressionValue, genContext);
                }
            }

            if (resultType != rightExpressionValue.getType())
            {
                CAST(rightExpressionValue, rightLoc, resultType, rightExpressionValue, genContext);
            }

            break;
        }

        return mlir::success();
    }

    mlir::Value binaryOpLogic(mlir::Location location, SyntaxKind opCode, mlir::Value leftExpressionValue,
                              mlir::Value rightExpressionValue, const GenContext &genContext)
    {
        auto result = rightExpressionValue;
        switch (opCode)
        {
        case SyntaxKind::EqualsToken:
            // nothing to do;
            assert(false);
            break;
        case SyntaxKind::EqualsEqualsToken:
        case SyntaxKind::EqualsEqualsEqualsToken:
        case SyntaxKind::ExclamationEqualsToken:
        case SyntaxKind::ExclamationEqualsEqualsToken:
        case SyntaxKind::GreaterThanToken:
        case SyntaxKind::GreaterThanEqualsToken:
        case SyntaxKind::LessThanToken:
        case SyntaxKind::LessThanEqualsToken:
            result = builder.create<mlir_ts::LogicalBinaryOp>(location, getBooleanType(),
                                                              builder.getI32IntegerAttr((int)opCode),
                                                              leftExpressionValue, rightExpressionValue);
            break;
        case SyntaxKind::CommaToken:
            return rightExpressionValue;
        default:
            result = builder.create<mlir_ts::ArithmeticBinaryOp>(location, leftExpressionValue.getType(),
                                                                 builder.getI32IntegerAttr((int)opCode),
                                                                 leftExpressionValue, rightExpressionValue);
            break;
        }

        return result;
    }

    ValueOrLogicalResult mlirGen(BinaryExpression binaryExpressionAST, const GenContext &genContext)
    {
        auto location = loc(binaryExpressionAST);

        auto opCode = (SyntaxKind)binaryExpressionAST->operatorToken;

        auto saveResult = MLIRLogicHelper::isNeededToSaveData(opCode);

        auto leftExpression = binaryExpressionAST->left;
        auto rightExpression = binaryExpressionAST->right;

        if (opCode == SyntaxKind::AmpersandAmpersandToken || opCode == SyntaxKind::BarBarToken)
        {
            return mlirGenAndOrLogic(binaryExpressionAST, genContext, opCode == SyntaxKind::AmpersandAmpersandToken,
                                     saveResult);
        }

        if (opCode == SyntaxKind::QuestionQuestionToken)
        {
            return mlirGenQuestionQuestionLogic(binaryExpressionAST, saveResult, genContext);
        }

        if (opCode == SyntaxKind::InKeyword)
        {
            return mlirGenInLogic(binaryExpressionAST, genContext);
        }

        if (opCode == SyntaxKind::InstanceOfKeyword)
        {
            return mlirGenInstanceOfLogic(binaryExpressionAST, genContext);
        }

        if (opCode == SyntaxKind::EqualsToken)
        {
            return mlirGenSaveLogic(binaryExpressionAST, genContext);
        }

        auto result = mlirGen(leftExpression, genContext);
        if (opCode == SyntaxKind::CommaToken)
        {
            //in case of "commad" op the result of left op can be "nothing"
            EXIT_IF_FAILED(result)
        }
        else
        {
            EXIT_IF_FAILED_OR_NO_VALUE(result)    
        }

        auto leftExpressionValue = V(result);
        auto result2 = mlirGen(rightExpression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result2)
        auto rightExpressionValue = V(result2);

        // check if const expr.
        if (genContext.allowConstEval)
        {
            LLVM_DEBUG(llvm::dbgs() << "Evaluate const: '" << leftExpressionValue << "' and '" << rightExpressionValue << "'\n";);

            auto leftConstOp = dyn_cast<mlir_ts::ConstantOp>(leftExpressionValue.getDefiningOp());
            auto rightConstOp = dyn_cast<mlir_ts::ConstantOp>(rightExpressionValue.getDefiningOp());
            if (leftConstOp && rightConstOp)
            {
                // try to evaluate
                return evaluateBinaryOp(location, opCode, leftConstOp, rightConstOp, genContext);
            }
        }

        auto leftExpressionValueBeforeCast = leftExpressionValue;
        auto rightExpressionValueBeforeCast = rightExpressionValue;

        unwrapForBinaryOp(opCode, leftExpressionValue, rightExpressionValue, genContext);

        adjustTypesForBinaryOp(opCode, leftExpressionValue, rightExpressionValue, genContext);

        auto resultReturn = binaryOpLogic(location, opCode, leftExpressionValue, rightExpressionValue, genContext);

        if (saveResult)
        {
            return mlirGenSaveLogicOneItem(location, leftExpressionValueBeforeCast, resultReturn, genContext);
        }

        return resultReturn;
    }

    ValueOrLogicalResult mlirGen(SpreadElement spreadElement, const GenContext &genContext)
    {
        return mlirGen(spreadElement->expression, genContext);
    }

    ValueOrLogicalResult mlirGen(ParenthesizedExpression parenthesizedExpression, const GenContext &genContext)
    {
        return mlirGen(parenthesizedExpression->expression, genContext);
    }

    ValueOrLogicalResult mlirGen(QualifiedName qualifiedName, const GenContext &genContext)
    {
        auto location = loc(qualifiedName);

        auto expression = qualifiedName->left;
        auto result = mlirGenModuleReference(expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expressionValue = V(result);

        auto name = MLIRHelper::getName(qualifiedName->right);

        return mlirGenPropertyAccessExpression(location, expressionValue, name, genContext);
    }

    ValueOrLogicalResult mlirGen(PropertyAccessExpression propertyAccessExpression, const GenContext &genContext)
    {
        auto location = loc(propertyAccessExpression);

        auto expression = propertyAccessExpression->expression.as<Expression>();
        auto result = mlirGen(expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expressionValue = V(result);

        auto namePtr = MLIRHelper::getName(propertyAccessExpression->name, stringAllocator);

        return mlirGenPropertyAccessExpression(location, expressionValue, namePtr,
                                               !!propertyAccessExpression->questionDotToken, genContext);
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpression(mlir::Location location, mlir::Value objectValue,
                                                         mlir::StringRef name, const GenContext &genContext)
    {
        assert(objectValue);
        MLIRPropertyAccessCodeLogic cl(builder, location, objectValue, name);
        return mlirGenPropertyAccessExpressionLogic(location, objectValue, false, cl, genContext);
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpression(mlir::Location location, mlir::Value objectValue,
                                                         mlir::StringRef name, bool isConditional,
                                                         const GenContext &genContext)
    {
        assert(objectValue);
        MLIRPropertyAccessCodeLogic cl(builder, location, objectValue, name);
        return mlirGenPropertyAccessExpressionLogic(location, objectValue, isConditional, cl, genContext);
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpression(mlir::Location location, mlir::Value objectValue,
                                                         mlir::Attribute id, const GenContext &genContext)
    {
        MLIRPropertyAccessCodeLogic cl(builder, location, objectValue, id);
        return mlirGenPropertyAccessExpressionLogic(location, objectValue, false, cl, genContext);
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpression(mlir::Location location, mlir::Value objectValue,
                                                         mlir::Attribute id, bool isConditional,
                                                         const GenContext &genContext)
    {
        MLIRPropertyAccessCodeLogic cl(builder, location, objectValue, id);
        return mlirGenPropertyAccessExpressionLogic(location, objectValue, isConditional, cl, genContext);
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpressionLogic(mlir::Location location, mlir::Value objectValue,
                                                              bool isConditional, MLIRPropertyAccessCodeLogic &cl,
                                                              const GenContext &genContext)
    {
        if (isConditional && mth.isNullableOrOptionalType(objectValue.getType()))
        {
            // TODO: replace with one op "Optional <has_value>, <value>"
            CAST_A(condValue, location, getBooleanType(), objectValue, genContext);

            auto propType = evaluateProperty(objectValue, cl.getName().str(), genContext);
            if (!propType)
            {
                emitError(location, "Can't resolve property '") << cl.getName() << "' of type " << objectValue.getType();
                return mlir::failure();
            }

            auto ifOp = builder.create<mlir_ts::IfOp>(location, getOptionalType(propType), condValue, true);

            builder.setInsertionPointToStart(&ifOp.getThenRegion().front());

            // value if true
            auto result = mlirGenPropertyAccessExpressionBaseLogic(location, objectValue, cl, genContext);
            auto value = V(result);

            // special case: conditional extension function <xxx>?.<ext>();
            if (auto createExtentionFunction = value.getDefiningOp<mlir_ts::CreateExtensionFunctionOp>())
            {
                // we need to convert into CreateBoundFunction, so it should be reference type for this, do I need to case value type into reference type?
                value = createBoundMethodFromExtensionMethod(location, createExtentionFunction);
                ifOp.getResults().front().setType(getOptionalType(value.getType()));
            }

            auto optValue =
                value.getType().isa<mlir_ts::OptionalType>()
                    ? value 
                    : builder.create<mlir_ts::OptionalValueOp>(location, getOptionalType(value.getType()), value);
            builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{optValue});

            // else
            builder.setInsertionPointToStart(&ifOp.getElseRegion().front());

            auto optUndefValue = builder.create<mlir_ts::OptionalUndefOp>(location, getOptionalType(value.getType()));
            builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{optUndefValue});

            builder.setInsertionPointAfter(ifOp);

            return ifOp.getResults().front();
        }
        else
        {
            return mlirGenPropertyAccessExpressionBaseLogic(location, objectValue, cl, genContext);
        }
    }

    ValueOrLogicalResult mlirGenPropertyAccessExpressionBaseLogic(mlir::Location location, mlir::Value objectValue,
                                                                  MLIRPropertyAccessCodeLogic &cl,
                                                                  const GenContext &genContext)
    {
        auto name = cl.getName();
        auto actualType = objectValue.getType();
        mlir::Value value = 
            TypeSwitch<mlir::Type, mlir::Value>(actualType)
                .Case<mlir_ts::EnumType>([&](auto enumType) { return cl.Enum(enumType); })
                .Case<mlir_ts::ConstTupleType>([&](auto constTupleType) { return cl.Tuple(constTupleType); })
                .Case<mlir_ts::TupleType>([&](auto tupleType) { return cl.Tuple(tupleType); })
                .Case<mlir_ts::BooleanType>([&](auto intType) { return cl.Bool(intType); })
                .Case<mlir::IntegerType>([&](auto intType) { return cl.Int(intType); })
                .Case<mlir::FloatType>([&](auto floatType) { return cl.Float(floatType); })
                .Case<mlir_ts::NumberType>([&](auto numberType) { return cl.Number(numberType); })
                .Case<mlir_ts::StringType>([&](auto stringType) { return cl.String(stringType); })
                .Case<mlir_ts::ConstArrayType>([&](auto arrayType) { return cl.Array(arrayType); })
                .Case<mlir_ts::ArrayType>([&](auto arrayType) { return cl.Array(arrayType); })
                .Case<mlir_ts::RefType>([&](auto refType) { return cl.Ref(refType); })
                .Case<mlir_ts::ObjectType>([&](auto objectType) { return cl.Object(objectType); })
                .Case<mlir_ts::SymbolType>([&](auto symbolType) { return cl.Symbol(symbolType); })
                .Case<mlir_ts::NamespaceType>([&](auto namespaceType) {
                    auto namespaceInfo = getNamespaceByFullName(namespaceType.getName().getValue());
                    assert(namespaceInfo);

                    MLIRNamespaceGuard ng(currentNamespace);
                    currentNamespace = namespaceInfo;

                    return mlirGen(location, name, genContext);
                })
                .Case<mlir_ts::ClassStorageType>([&](auto classStorageType) {
                    if (auto value = cl.TupleNoError(classStorageType))
                    {
                        return value;
                    }

                    return ClassMembers(location, objectValue, classStorageType.getName().getValue(), name, true, genContext);
                })
                .Case<mlir_ts::ClassType>([&](auto classType) {
                    if (auto value = cl.Class(classType))
                    {
                        return value;
                    }

                    return ClassMembers(location, objectValue, classType.getName().getValue(), name, false, genContext);
                })
                .Case<mlir_ts::InterfaceType>([&](auto interfaceType) {
                    return InterfaceMembers(location, objectValue, interfaceType.getName().getValue(), cl.getAttribute(),
                                            genContext);
                })
                .Case<mlir_ts::OptionalType>([&](auto optionalType) {
                    // this is needed for conditional access to properties
                    auto elementType = optionalType.getElementType();
                    auto loadedValue = builder.create<mlir_ts::ValueOp>(location, elementType, objectValue);
                    return mlirGenPropertyAccessExpression(location, loadedValue, name, false, genContext);                
                })
                .Case<mlir_ts::UnionType>([&](auto unionType) {
                    // all union types must have the same property
                    // 1) cast to first type
                    auto frontType = mth.getFirstNonNullUnionType(unionType);
                    auto casted = cast(location, frontType, objectValue, genContext);
                    return mlirGenPropertyAccessExpression(location, casted, name, false, genContext);
                })
                .Case<mlir_ts::LiteralType>([&](auto literalType) {
                    auto elementType = literalType.getElementType();
                    auto castedValue = builder.create<mlir_ts::CastOp>(location, elementType, objectValue);
                    return mlirGenPropertyAccessExpression(location, castedValue, name, false, genContext);
                })
                .Default([&](auto type) {
                    LLVM_DEBUG(llvm::dbgs() << "Can't resolve property '" << name << "' of type " << objectValue.getType(););
                    return mlir::Value();
                });

        // extention logic: <obj>.<functionName>(this)
        if (!value)
        {
            auto funcRef = extensionFunction(location, objectValue, name, genContext);
            if (funcRef)
            {
                return funcRef;
            }
        }

        if (!value)
        {
            emitError(location, "Can't resolve property '") << name << "' of type " << objectValue.getType();
            return mlir::failure();
        }

        return value;
    }

    mlir::Value extensionFunctionLogic(mlir::Location location, mlir::Value funcRef, mlir::Value thisValue, StringRef name,
                                  const GenContext &genContext)
    {
        if (!mth.isAnyFunctionType(funcRef.getType()))
        {
            return mlir::Value();
        }

        LLVM_DEBUG(llvm::dbgs() << "!! found extension by name for type: " << thisValue.getType()
                                << " function: " << name << ", value: " << funcRef << "\n";);

        auto thisTypeFromFunc = mth.getFirstParamFromFuncRef(funcRef.getType());

        LLVM_DEBUG(llvm::dbgs() << "!! this type of function is : " << thisTypeFromFunc << "\n";);

        if (auto symbolOp = funcRef.getDefiningOp<mlir_ts::SymbolRefOp>())
        {
            // if (!symbolOp.getType().isa<mlir_ts::GenericType>())
            if (!symbolOp->hasAttrOfType<mlir::BoolAttr>(GENERIC_ATTR_NAME))
            {
                auto funcType = funcRef.getType().cast<mlir_ts::FunctionType>();
                if (thisTypeFromFunc == thisValue.getType())
                {
                    // return funcRef;
                    auto thisRef = thisValue;
                    auto extensFuncVal = builder.create<mlir_ts::CreateExtensionFunctionOp>(
                        location, getExtensionFunctionType(funcType), thisRef, funcRef);
                    return extensFuncVal;
                }
            }
            else
            {
                // TODO: finish it
                // it is generic function
                StringMap<mlir::Type> inferredTypes;
                inferType(location, thisTypeFromFunc, thisValue.getType(), inferredTypes, genContext);
                if (inferredTypes.size() > 0)
                {
                    // we found needed function
                    // return funcRef;
                    auto thisRef = thisValue;

                    LLVM_DEBUG(llvm::dbgs() << "\n!! recreate ExtensionFunctionOp (generic interface): '" << name << "'\n this ref: '" << thisRef << "'\n func ref: '" << funcRef
                    << "'\n";);

                    auto funcType = funcRef.getType().cast<mlir_ts::FunctionType>();
                    auto extensFuncVal = builder.create<mlir_ts::CreateExtensionFunctionOp>(
                        location, getExtensionFunctionType(funcType), thisRef, funcRef);
                    return extensFuncVal;                        
                }
            }
        }

        return mlir::Value();
    }

    mlir::Value extensionFunction(mlir::Location location, mlir::Value thisValue, StringRef name,
                                  const GenContext &genContext)
    {
        if (auto funcRef = resolveIdentifier(location, name, genContext))
        {
            auto result = extensionFunctionLogic(location, funcRef, thisValue, name, genContext);
            if (result)
            {
                return result;
            }
        }

        // look into all namespaces from current one
        {
            MLIRNamespaceGuard ng(currentNamespace);

            // search in outer namespaces
            while (currentNamespace->isFunctionNamespace)
            {
                currentNamespace = currentNamespace->parentNamespace;
            }

            auto &currentNamespacesMap = currentNamespace->namespacesMap;
            for (auto &selectedNamespace : currentNamespacesMap)
            {
                currentNamespace = selectedNamespace.getValue();
                if (auto funcRef = resolveIdentifierInNamespace(location, name, genContext))
                {
                    auto result = extensionFunctionLogic(location, funcRef, thisValue, name, genContext);
                    if (result)
                    {
                        return result;
                    }
                }
            }
        }        

        return mlir::Value();
    }

    mlir::Value ClassMembers(mlir::Location location, mlir::Value thisValue, mlir::StringRef classFullName,
                             mlir::StringRef name, bool baseClass, const GenContext &genContext)
    {
        auto classInfo = getClassInfoByFullName(classFullName);
        if (!classInfo)
        {
            auto genericClassInfo = getGenericClassInfoByFullName(classFullName);
            if (genericClassInfo)
            {
                // we can't discover anything in generic class
                return mlir::Value();
            }

            emitError(location, "Class can't be found ") << classFullName;
            return mlir::Value();
        }

        // static field access
        auto value = ClassMembers(location, thisValue, classInfo, name, baseClass, genContext);
        if (!value)
        {
            emitError(location, "Class member '") << name << "' can't be found";
        }

        return value;
    }

    mlir::Value getThisRefOfClass(mlir_ts::ClassType classType, mlir::Value thisValue, bool isSuperClass, const GenContext &genContext)
    {
        auto effectiveThisValue = thisValue;
        if (isSuperClass)
        {
            // LLVM_DEBUG(dbgs() << "\n!! base call: func '" << funcOp.getName() << "' in context func. '"
            //                     << const_cast<GenContext &>(genContext).funcOp.getName()
            //                     << "', this type: " << thisValue.getType() << " value:" << thisValue << "";);

            // get reference in case of classStorage
            auto isStorageType = thisValue.getType().isa<mlir_ts::ClassStorageType>();
            if (isStorageType)
            {
                MLIRCodeLogic mcl(builder);
                thisValue = mcl.GetReferenceOfLoadOp(thisValue);
                assert(thisValue);
            }

            CAST(effectiveThisValue, thisValue.getLoc(), classType, thisValue, genContext);
        }        

        return effectiveThisValue;
    }

    mlir::Value ClassMembers(mlir::Location location, mlir::Value thisValue, ClassInfo::TypePtr classInfo,
                             mlir::StringRef name, bool isSuperClass, const GenContext &genContext)
    {
        assert(classInfo);

        LLVM_DEBUG(llvm::dbgs() << "\n!! looking for member: " << name << " in class '" << classInfo->fullName << "' this value: " << thisValue 
                                << "\n";);

        auto staticFieldIndex = classInfo->getStaticFieldIndex(MLIRHelper::TupleFieldName(name, builder.getContext()));
        if (staticFieldIndex >= 0)
        {
            auto fieldInfo = classInfo->staticFields[staticFieldIndex];
#ifdef ADD_STATIC_MEMBERS_TO_VTABLE
            if (thisValue.getDefiningOp<mlir_ts::ClassRefOp>())
            {
#endif
                auto value = resolveFullNameIdentifier(location, fieldInfo.globalVariableName, false, genContext);
                // load referenced value
                if (classInfo->isDynamicImport)
                {
                    if (auto valueRefType = value.getType().dyn_cast<mlir_ts::RefType>())
                    {
                        value = builder.create<mlir_ts::LoadOp>(location, valueRefType.getElementType(), value);
                    }
                    else
                    {
                        llvm_unreachable("not implemented");
                    }
                }

                return value;
#ifdef ADD_STATIC_MEMBERS_TO_VTABLE
            }

            // static accessing via class reference
            // TODO:
            auto effectiveThisValue = thisValue;

            auto result = mlirGenPropertyAccessExpression(location, effectiveThisValue, VTABLE_NAME, genContext);
            auto vtableAccess = V(result);

            assert(genContext.allowPartialResolve || fieldInfo.virtualIndex >= 0);

            auto virtualSymbOp = builder.create<mlir_ts::VirtualSymbolRefOp>(
                location, mlir_ts::RefType::get(fieldInfo.type), vtableAccess,
                builder.getI32IntegerAttr(fieldInfo.virtualIndex),
                mlir::FlatSymbolRefAttr::get(builder.getContext(), fieldInfo.globalVariableName));

            auto value = builder.create<mlir_ts::LoadOp>(location, fieldInfo.type, virtualSymbOp);
            return value;
#endif
        }

        // check method access
        auto methodIndex = classInfo->getMethodIndex(name);
        if (methodIndex >= 0)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! found method index: " << methodIndex << "\n";);

            auto methodInfo = classInfo->methods[methodIndex];
            auto funcOp = methodInfo.funcOp;
            auto effectiveFuncType = funcOp.getFunctionType();

            if (methodInfo.isStatic)
            {
#ifdef ADD_STATIC_MEMBERS_TO_VTABLE
                if (thisValue.getDefiningOp<mlir_ts::ClassRefOp>())
                {
#endif
                    if (classInfo->isDynamicImport)
                    {
                        // need to resolve global variable
                        auto globalFuncVar = resolveFullNameIdentifier(location, funcOp.getName(), false, genContext);
                        return globalFuncVar;
                    }
                    else
                    {
                        auto symbOp = builder.create<mlir_ts::SymbolRefOp>(
                            location, effectiveFuncType,
                            mlir::FlatSymbolRefAttr::get(builder.getContext(), funcOp.getName()));
                        return symbOp;
                    }
#ifdef ADD_STATIC_MEMBERS_TO_VTABLE
                }

                // static accessing via class reference
                // TODO:
                auto effectiveThisValue = thisValue;

                auto vtableAccess =
                    mlirGenPropertyAccessExpression(location, effectiveThisValue, VTABLE_NAME, genContext);

                if (!vtableAccess)
                {
                    emitError(location,"") << "class '" << classInfo->fullName << "' missing 'virtual table'";
                }

                EXIT_IF_FAILED_OR_NO_VALUE(vtableAccess)                    

                assert(genContext.allowPartialResolve || methodInfo.virtualIndex >= 0);

                auto virtualSymbOp = builder.create<mlir_ts::VirtualSymbolRefOp>(
                    location, effectiveFuncType, vtableAccess, builder.getI32IntegerAttr(methodInfo.virtualIndex),
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), funcOp.getName()));
                return virtualSymbOp;
#endif
            }
            else
            {
                auto effectiveThisValue = getThisRefOfClass(classInfo->classType, thisValue, isSuperClass, genContext);

                // TODO: check if you can split calls such as "this.method" and "super.method" ...
                auto isStorageType = thisValue.getType().isa<mlir_ts::ClassStorageType>();
                if (methodInfo.isAbstract || /*!baseClass &&*/ methodInfo.isVirtual && !isStorageType)
                {
                    LLVM_DEBUG(dbgs() << "\n!! Virtual call: func '" << funcOp.getName() << "' in context func. '"
                                      << const_cast<GenContext &>(genContext).funcOp.getName() << "'\n";);

                    LLVM_DEBUG(dbgs() << "\n!! Virtual call - this val: [ " << effectiveThisValue << " ] func type: [ "
                                      << effectiveFuncType << " ] isStorage access: " << isStorageType << "\n";);

                    // auto inTheSameFunc = funcOp.getName() == const_cast<GenContext &>(genContext).funcOp.getName();

                    auto vtableAccess =
                        mlirGenPropertyAccessExpression(location, effectiveThisValue, VTABLE_NAME, genContext);

                    if (!vtableAccess)
                    {
                        emitError(location,"") << "class '" << classInfo->fullName << "' missing 'virtual table'";
                    }

                    EXIT_IF_FAILED_OR_NO_VALUE(vtableAccess)

                    assert(genContext.allowPartialResolve || methodInfo.virtualIndex >= 0);

                    auto thisVirtualSymbOp = builder.create<mlir_ts::ThisVirtualSymbolRefOp>(
                        location, getBoundFunctionType(effectiveFuncType), effectiveThisValue, vtableAccess,
                        builder.getI32IntegerAttr(methodInfo.virtualIndex),
                        mlir::FlatSymbolRefAttr::get(builder.getContext(), funcOp.getName()));
                    return thisVirtualSymbOp;
                }

                if (classInfo->isDynamicImport)
                {
                    // need to resolve global variable
                    auto globalFuncVar = resolveFullNameIdentifier(location, funcOp.getName(), false, genContext);
                    CAST_A(opaqueThisValue, location, getOpaqueType(), effectiveThisValue, genContext);
                    auto boundMethodValue = builder.create<mlir_ts::CreateBoundFunctionOp>(
                        location, getBoundFunctionType(effectiveFuncType), opaqueThisValue, globalFuncVar);
                    return boundMethodValue;
                }
                else
                {
                    // default call;
                    auto thisSymbOp = builder.create<mlir_ts::ThisSymbolRefOp>(
                        location, getBoundFunctionType(effectiveFuncType), effectiveThisValue,
                        mlir::FlatSymbolRefAttr::get(builder.getContext(), funcOp.getName()));
                    return thisSymbOp;
                }
            }
        }

        // static generic methods
        auto genericMethodIndex = classInfo->getGenericMethodIndex(name);
        if (genericMethodIndex >= 0)
        {        
            auto genericMethodInfo = classInfo->staticGenericMethods[genericMethodIndex];

            if (genericMethodInfo.isStatic)
            {
                auto funcSymbolOp = builder.create<mlir_ts::SymbolRefOp>(
                    location, genericMethodInfo.funcType,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), genericMethodInfo.funcOp->getName()));
                funcSymbolOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
                return funcSymbolOp;
            }
            else
            {
                auto effectiveThisValue = getThisRefOfClass(classInfo->classType, thisValue, isSuperClass, genContext);
                auto effectiveFuncType = genericMethodInfo.funcOp->getFuncType();

                auto thisSymbOp = builder.create<mlir_ts::ThisSymbolRefOp>(
                    location, getBoundFunctionType(effectiveFuncType), effectiveThisValue,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), genericMethodInfo.funcOp->getName()));
                thisSymbOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
                return thisSymbOp;                
            }
        }        

        // check accessor
        auto accessorIndex = classInfo->getAccessorIndex(name);
        if (accessorIndex >= 0)
        {
            auto accessorInfo = classInfo->accessors[accessorIndex];
            auto getFuncOp = accessorInfo.get;
            auto setFuncOp = accessorInfo.set;
            mlir::Type effectiveFuncType;
            if (getFuncOp)
            {
                auto funcType = getFuncOp.getFunctionType().dyn_cast<mlir_ts::FunctionType>();
                if (funcType.getNumResults() > 0)
                {
                    effectiveFuncType = funcType.getResult(0);
                }
            }

            if (!effectiveFuncType && setFuncOp)
            {
                effectiveFuncType =
                    setFuncOp.getFunctionType().dyn_cast<mlir_ts::FunctionType>().getInput(accessorInfo.isStatic ? 0 : 1);
            }

            if (!effectiveFuncType)
            {
                emitError(location) << "can't resolve type of property";
                return mlir::Value();
            }

            if (accessorInfo.isStatic)
            {
                auto accessorOp = builder.create<mlir_ts::AccessorOp>(
                    location, effectiveFuncType,
                    getFuncOp ? mlir::FlatSymbolRefAttr::get(builder.getContext(), getFuncOp.getName())
                              : mlir::FlatSymbolRefAttr{},
                    setFuncOp ? mlir::FlatSymbolRefAttr::get(builder.getContext(), setFuncOp.getName())
                              : mlir::FlatSymbolRefAttr{});
                return accessorOp;
            }
            else
            {
                auto thisAccessorOp = builder.create<mlir_ts::ThisAccessorOp>(
                    location, effectiveFuncType, thisValue,
                    getFuncOp ? mlir::FlatSymbolRefAttr::get(builder.getContext(), getFuncOp.getName())
                              : mlir::FlatSymbolRefAttr{},
                    setFuncOp ? mlir::FlatSymbolRefAttr::get(builder.getContext(), setFuncOp.getName())
                              : mlir::FlatSymbolRefAttr{});
                return thisAccessorOp;
            }
        }

        auto first = true;
        for (auto baseClass : classInfo->baseClasses)
        {
            if (first && name == SUPER_NAME)
            {
                auto result = mlirGenPropertyAccessExpression(location, thisValue, baseClass->fullName, genContext);
                auto value = V(result);
                return value;
            }

            auto value = ClassMembers(location, thisValue, baseClass, name, true, genContext);
            if (value)
            {
                return value;
            }

            SmallVector<ClassInfo::TypePtr> fieldPath;
            if (classHasField(baseClass, name, fieldPath))
            {
                // load value from path
                auto currentObject = thisValue;
                for (auto &chain : fieldPath)
                {
                    auto fieldValue =
                        mlirGenPropertyAccessExpression(location, currentObject, chain->fullName, genContext);
                    if (!fieldValue)
                    {
                        emitError(location) << "Can't resolve field/property/base '" << chain->fullName
                                            << "' of class '" << classInfo->fullName << "'\n";
                        return fieldValue;
                    }

                    assert(fieldValue);
                    currentObject = fieldValue;
                }

                // last value
                auto result = mlirGenPropertyAccessExpression(location, currentObject, name, genContext);
                auto value = V(result);
                if (value)
                {
                    return value;
                }
            }

            first = false;
        }

        if (isSuperClass || genContext.allowPartialResolve)
        {
            return mlir::Value();
        }

        emitError(location) << "can't resolve property/field/base '" << name << "' of class '" << classInfo->fullName
                            << "'\n";

        return mlir::Value();
    }

    bool classHasField(ClassInfo::TypePtr classInfo, mlir::StringRef name, SmallVector<ClassInfo::TypePtr> &fieldPath)
    {
        auto fieldId = MLIRHelper::TupleFieldName(name, builder.getContext());
        auto classStorageType = classInfo->classType.getStorageType().cast<mlir_ts::ClassStorageType>();
        auto fieldIndex = classStorageType.getIndex(fieldId);
        auto missingField = fieldIndex < 0 || fieldIndex >= classStorageType.size();
        if (!missingField)
        {
            fieldPath.insert(fieldPath.begin(), classInfo);
            return true;
        }

        for (auto baseClass : classInfo->baseClasses)
        {
            if (classHasField(baseClass, name, fieldPath))
            {
                fieldPath.insert(fieldPath.begin(), classInfo);
                return true;
            }
        }

        return false;
    }

    mlir::Value InterfaceMembers(mlir::Location location, mlir::Value interfaceValue, mlir::StringRef interfaceFullName,
                                 mlir::Attribute id, const GenContext &genContext)
    {
        auto interfaceInfo = getInterfaceInfoByFullName(interfaceFullName);
        if (!interfaceInfo)
        {
            auto genericInterfaceInfo = getGenericInterfaceInfoByFullName(interfaceFullName);
            if (genericInterfaceInfo)
            {
                // we can't detect value of generic interface (we can only if it is specialization)
                emitError(location, "Interface can't be found ") << interfaceFullName;
                return mlir::Value();
            }

            return mlir::Value();
        }

        assert(interfaceInfo);

        // static field access
        auto value = InterfaceMembers(location, interfaceValue, interfaceInfo, id, genContext);
        if (!value)
        {
            emitError(location, "Interface member '") << id << "' can't be found";
        }

        return value;
    }

    mlir::Value InterfaceMembers(mlir::Location location, mlir::Value interfaceValue,
                                 InterfaceInfo::TypePtr interfaceInfo, mlir::Attribute id, const GenContext &genContext)
    {
        assert(interfaceInfo);

        // check field access
        auto totalOffset = 0;
        auto fieldInfo = interfaceInfo->findField(id, totalOffset);
        if (fieldInfo)
        {
            assert(fieldInfo->interfacePosIndex >= 0);
            auto vtableIndex = fieldInfo->interfacePosIndex + totalOffset;

            auto fieldRefType = mlir_ts::RefType::get(fieldInfo->type);

            auto interfaceSymbolRefValue = builder.create<mlir_ts::InterfaceSymbolRefOp>(
                location, fieldRefType, interfaceValue, builder.getI32IntegerAttr(vtableIndex),
                builder.getStringAttr(""), builder.getBoolAttr(fieldInfo->isConditional));

            mlir::Value value;
            if (!fieldInfo->isConditional)
            {
                value = builder.create<mlir_ts::LoadOp>(location, fieldRefType.getElementType(),
                                                        interfaceSymbolRefValue.getResult());
            }
            else
            {
                auto actualType = fieldRefType.getElementType().isa<mlir_ts::OptionalType>()
                                      ? fieldRefType.getElementType()
                                      : mlir_ts::OptionalType::get(fieldRefType.getElementType());
                value = builder.create<mlir_ts::LoadOp>(location, actualType, interfaceSymbolRefValue.getResult());
            }

            // if it is FuncType, we need to create BoundMethod again
            if (auto funcType = fieldInfo->type.dyn_cast<mlir_ts::FunctionType>())
            {
                auto thisVal =
                    builder.create<mlir_ts::ExtractInterfaceThisOp>(location, getOpaqueType(), interfaceValue);
                value = builder.create<mlir_ts::CreateBoundFunctionOp>(location, getBoundFunctionType(funcType),
                                                                       thisVal, value);
            }

            return value;
        }

        // check method access
        if (auto nameAttr = id.dyn_cast<mlir::StringAttr>())
        {
            auto name = nameAttr.getValue();
            auto methodInfo = interfaceInfo->findMethod(name, totalOffset);
            if (methodInfo)
            {
                assert(methodInfo->interfacePosIndex >= 0);
                auto vtableIndex = methodInfo->interfacePosIndex + totalOffset;

                auto effectiveFuncType = getBoundFunctionType(methodInfo->funcType);

                auto interfaceSymbolRefValue = builder.create<mlir_ts::InterfaceSymbolRefOp>(
                    location, effectiveFuncType, interfaceValue, builder.getI32IntegerAttr(vtableIndex),
                    builder.getStringAttr(methodInfo->name), builder.getBoolAttr(methodInfo->isConditional));

                return interfaceSymbolRefValue;
            }
        }

        return mlir::Value();
    }

    template <typename T>
    ValueOrLogicalResult mlirGenElementAccessTuple(mlir::Location location, mlir::Value expression,
                                              mlir::Value argumentExpression, T tupleType)
    {
        // get index
        if (auto indexConstOp = argumentExpression.getDefiningOp<mlir_ts::ConstantOp>())
        {
            // this is property access
            MLIRPropertyAccessCodeLogic cl(builder, location, expression, indexConstOp.getValue());
            return cl.Tuple(tupleType, true);
        }
        else
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! index value: " << argumentExpression
                                    << ", check if tuple must be an array\n";);
            llvm_unreachable("not implemented (index)");
        }
    }

    ValueOrLogicalResult mlirGen(ElementAccessExpression elementAccessExpression, const GenContext &genContext)
    {
        auto location = loc(elementAccessExpression);

        auto conditinalAccess = !!elementAccessExpression->questionDotToken;

        auto result = mlirGen(elementAccessExpression->expression.as<Expression>(), genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expression = V(result);

        // default access <array>[index]
        if (!conditinalAccess || conditinalAccess && expression.getType().isa<mlir_ts::OptionalType>())
        {
            auto result2 = mlirGen(elementAccessExpression->argumentExpression.as<Expression>(), genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result2)
            auto argumentExpression = V(result2);

            return mlirGenElementAccess(location, expression, argumentExpression, conditinalAccess, genContext);
        }

        // <array>?.[index] access
        CAST_A(condValue, location, getBooleanType(), expression, genContext);
        return conditionalValue(location, condValue, 
            [&](auto genContext) { 
                auto result2 = mlirGen(elementAccessExpression->argumentExpression.as<Expression>(), genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result2)
                auto argumentExpression = V(result2);

                // conditinalAccess should be false here
                auto result3 = mlirGenElementAccess(location, expression, argumentExpression, false, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result3)
                auto value = V(result3);

                auto optValue = 
                    value.getType().isa<mlir_ts::OptionalType>()
                        ? value
                        : builder.create<mlir_ts::OptionalValueOp>(location, getOptionalType(value.getType()), value);
                return ValueOrLogicalResult(optValue); 
            }, 
            [&](mlir::Type trueValueType, auto genContext) { 
                auto optUndefValue = builder.create<mlir_ts::OptionalUndefOp>(location, trueValueType);
                return ValueOrLogicalResult(optUndefValue); 
            }, 
            genContext);
    }

    ValueOrLogicalResult mlirGenElementAccess(mlir::Location location, mlir::Value expression, mlir::Value argumentExpression, bool isConditionalAccess, const GenContext &genContext)
    {
        auto arrayType = expression.getType();
        if (arrayType.isa<mlir_ts::LiteralType>())
        {
            arrayType = mth.stripLiteralType(arrayType);
            CAST(expression, location, arrayType, expression, genContext);
        }

        if (isConditionalAccess)
        {
            arrayType = mth.stripOptionalType(arrayType);
        }

        mlir::Type elementType;
        if (auto arrayTyped = arrayType.dyn_cast<mlir_ts::ArrayType>())
        {
            elementType = arrayTyped.getElementType();
        }
        else if (auto vectorType = arrayType.dyn_cast<mlir_ts::ConstArrayType>())
        {
            elementType = vectorType.getElementType();
        }
        else if (arrayType.isa<mlir_ts::StringType>())
        {
            elementType = getCharType();
        }
        else if (auto tupleType = arrayType.dyn_cast<mlir_ts::TupleType>())
        {
            return mlirGenElementAccessTuple(location, expression, argumentExpression, tupleType);
        }
        else if (auto constTupleType = arrayType.dyn_cast<mlir_ts::ConstTupleType>())
        {
            return mlirGenElementAccessTuple(location, expression, argumentExpression, constTupleType);
        }
        else if (auto classType = arrayType.dyn_cast<mlir_ts::ClassType>())
        {
            if (auto fieldName = argumentExpression.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto attr = fieldName.getValue();
                return mlirGenPropertyAccessExpression(location, expression, attr, isConditionalAccess, genContext);
            }

            llvm_unreachable("not implemented (ElementAccessExpression)");
        }
        else if (auto classStorageType = arrayType.dyn_cast<mlir_ts::ClassStorageType>())
        {
            // seems we are calling "super"
            if (auto fieldName = argumentExpression.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto attr = fieldName.getValue();
                return mlirGenPropertyAccessExpression(location, expression, attr, isConditionalAccess, genContext);
            }

            llvm_unreachable("not implemented (ElementAccessExpression)");
        }        
        else if (auto interfaceType = arrayType.dyn_cast<mlir_ts::InterfaceType>())
        {
            if (auto fieldName = argumentExpression.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto attr = fieldName.getValue();
                return mlirGenPropertyAccessExpression(location, expression, attr, isConditionalAccess, genContext);
            }

            llvm_unreachable("not implemented (ElementAccessExpression)");
        }        
        else if (auto enumType = arrayType.dyn_cast<mlir_ts::EnumType>())
        {
            if (auto fieldName = argumentExpression.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto attr = fieldName.getValue();
                return mlirGenPropertyAccessExpression(location, expression, attr, isConditionalAccess, genContext);
            }

            llvm_unreachable("not implemented (ElementAccessExpression)");
        }          
        else if (auto enumType = arrayType.dyn_cast<mlir_ts::AnyType>())
        {
            emitError(location, "not supported");
            return mlir::failure();
        }          
        else
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! ElementAccessExpression: " << arrayType
                                    << "\n";);

            emitError(location) << "ElementAccessExpression: " << arrayType;
            llvm_unreachable("not implemented (ElementAccessExpression)");
        }

        auto indexType = argumentExpression.getType();
        auto isAllowableType = indexType.isIntOrIndex() && indexType.getIntOrFloatBitWidth() == 32;
        if (!isAllowableType)
        {

            CAST(argumentExpression, location, mth.getStructIndexType(), argumentExpression, genContext);
        }
  
        auto elemRef = builder.create<mlir_ts::ElementRefOp>(location, mlir_ts::RefType::get(elementType), expression,
                                                             argumentExpression);
        return V(builder.create<mlir_ts::LoadOp>(location, elementType, elemRef));
    }

    ValueOrLogicalResult mlirGen(CallExpression callExpression, const GenContext &genContext)
    {
        auto location = loc(callExpression);

        auto callExpr = callExpression->expression.as<Expression>();

        auto result = mlirGen(callExpr, genContext);
        // in case of detecting value for recursive calls we need to ignore failed calls
        if (result.failed_or_no_value() && genContext.allowPartialResolve)
        {
            // we need to return success to continue code traversing
            return V(builder.create<mlir_ts::UndefOp>(location, builder.getNoneType()));
        }

        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto funcResult = V(result);

        LLVM_DEBUG(llvm::dbgs() << "\n!! evaluate function: " << funcResult << "\n";);

        auto funcType = funcResult.getType();
        if (!mth.isAnyFunctionType(funcType) 
            && !mth.isVirtualFunctionType(funcResult)
            // TODO: do I need to use ConstructFunction instead?
            // to support constructor calls
            && !funcType.isa<mlir_ts::ClassType>()
            // to support super.constructor calls
            && !funcType.isa<mlir_ts::ClassStorageType>())
        {           
            // TODO: rewrite code for calling "5.ToString()"
            // TODO: recursive functions are usually return "failure" as can't be found
            //return mlir::failure();
            return funcResult;
        }

        auto noReceiverTypesForGenericCall = false;
        if (funcResult.getDefiningOp()->hasAttrOfType<mlir::BoolAttr>(GENERIC_ATTR_NAME))
        {
            // so if method is generic and you need to infer types you can cast to generic types
            noReceiverTypesForGenericCall = callExpression->typeArguments.size() == 0;
        }

        SmallVector<mlir::Value, 4> operands;
        auto offsetArgs = funcType.isa<mlir_ts::BoundFunctionType>() || funcType.isa<mlir_ts::ExtensionFunctionType>() ? 1 : 0;
        if (mlir::failed(mlirGenOperands(callExpression->arguments, operands, funcResult.getType(), genContext, offsetArgs, noReceiverTypesForGenericCall)))
        {
            return mlir::failure();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! function: [" << funcResult << "] ops: "; for (auto o
                                                                                       : operands) llvm::dbgs()
                                                                                  << "\n param type: " << o.getType();
                   llvm::dbgs() << "\n";);

        return mlirGenCallExpression(location, funcResult, callExpression->typeArguments, operands, genContext);
    }

    mlir::LogicalResult mlirGenArrayForEach(mlir::Location location, ArrayRef<mlir::Value> operands,
                                            const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto arraySrc = operands[0];
        auto funcSrc = operands[1];

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto funcVarDecl = std::make_shared<VariableDeclarationDOM>(".func", funcSrc.getType(), location);
        DECLARE(funcVarDecl, funcSrc);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _func_ident = nf.createIdentifier(S(".func"));

        auto _v_ident = nf.createIdentifier(S(".v"));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto forOfStat = nf.createForOfStatement(
            undefined, declList, _src_array_ident,
            nf.createExpressionStatement(nf.createCallExpression(_func_ident, undefined, argumentsArray)));

        mlirGen(forOfStat, genContext);

        return mlir::success();
    }

    ValueOrLogicalResult mlirGenArrayEvery(mlir::Location location, ArrayRef<mlir::Value> operands,
                                           const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto varName = ".ev";
        auto initVal = builder.create<mlir_ts::ConstantOp>(location, getBooleanType(), builder.getBoolAttr(true));
        registerVariable(
            location, varName, false, VariableType::Let,
            [&](mlir::Location, const GenContext &) -> TypeValueInitType {
                return {getBooleanType(), initVal, TypeProvided::No};
            },
            genContext);

        auto arraySrc = operands[0];
        auto funcSrc = operands[1];

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto funcVarDecl = std::make_shared<VariableDeclarationDOM>(".func", funcSrc.getType(), location);
        DECLARE(funcVarDecl, funcSrc);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _func_ident = nf.createIdentifier(S(".func"));

        auto _v_ident = nf.createIdentifier(S(".v"));
        auto _result_ident = nf.createIdentifier(stows(varName));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto forOfStat = nf.createForOfStatement(
            undefined, declList, _src_array_ident,
            nf.createIfStatement(
                nf.createPrefixUnaryExpression(
                    nf.createToken(SyntaxKind::ExclamationToken),
                    nf.createBinaryExpression(_result_ident, nf.createToken(SyntaxKind::AmpersandAmpersandEqualsToken),
                                              nf.createCallExpression(_func_ident, undefined, argumentsArray))),
                nf.createBreakStatement(), undefined));

        mlirGen(forOfStat, genContext);

        return resolveIdentifier(location, varName, genContext);
    }

    ValueOrLogicalResult mlirGenArraySome(mlir::Location location, ArrayRef<mlir::Value> operands,
                                          const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto varName = ".sm";
        auto initVal = builder.create<mlir_ts::ConstantOp>(location, getBooleanType(), builder.getBoolAttr(false));
        registerVariable(
            location, varName, false, VariableType::Let,
            [&](mlir::Location, const GenContext &) -> TypeValueInitType {
                return {getBooleanType(), initVal, TypeProvided::No};
            },
            genContext);

        auto arraySrc = operands[0];
        auto funcSrc = operands[1];

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto funcVarDecl = std::make_shared<VariableDeclarationDOM>(".func", funcSrc.getType(), location);
        DECLARE(funcVarDecl, funcSrc);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _func_ident = nf.createIdentifier(S(".func"));

        auto _v_ident = nf.createIdentifier(S(".v"));
        auto _result_ident = nf.createIdentifier(stows(varName));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto forOfStat = nf.createForOfStatement(
            undefined, declList, _src_array_ident,
            nf.createIfStatement(
                nf.createBinaryExpression(_result_ident, nf.createToken(SyntaxKind::BarBarEqualsToken),
                                          nf.createCallExpression(_func_ident, undefined, argumentsArray)),
                nf.createBreakStatement(), undefined));

        mlirGen(forOfStat, genContext);

        return resolveIdentifier(location, varName, genContext);
    }

    ValueOrLogicalResult mlirGenArrayMap(mlir::Location location, ArrayRef<mlir::Value> operands,
                                         const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto arraySrc = operands[0];
        auto funcSrc = operands[1];

        auto [pos, _end] = getPos(location);

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto funcVarDecl = std::make_shared<VariableDeclarationDOM>(".func", funcSrc.getType(), location);
        DECLARE(funcVarDecl, funcSrc);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _func_ident = nf.createIdentifier(S(".func"));

        auto _v_ident = nf.createIdentifier(S(".v"));
        
        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto _yield_expr = nf.createYieldExpression(undefined,
            nf.createCallExpression(_func_ident, undefined, argumentsArray));
        _yield_expr->pos.pos = pos;
        _yield_expr->_end = _end;

        auto forOfStat =
            nf.createForOfStatement(undefined, declList, _src_array_ident,
                                    nf.createExpressionStatement(_yield_expr));

        // iterator
        auto iterName = MLIRHelper::getAnonymousName(location, ".iter");

        NodeArray<Statement> statements;
        statements.push_back(forOfStat);
        auto block = nf.createBlock(statements, false);
        auto funcIter =
            nf.createFunctionExpression(undefined, nf.createToken(SyntaxKind::AsteriskToken),
                                        nf.createIdentifier(ConvertUTF8toWide(iterName)), undefined, undefined, undefined, block);

        funcIter->pos.pos = pos;
        funcIter->_end = _end;

        // call
        NodeArray<Expression> emptyArguments;
        auto callOfIter = nf.createCallExpression(funcIter, undefined, emptyArguments);

        return mlirGen(callOfIter, genContext);
    }

    ValueOrLogicalResult mlirGenArrayFilter(mlir::Location location, ArrayRef<mlir::Value> operands,
                                            const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        auto arraySrc = operands[0];
        auto funcSrc = operands[1];

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto funcVarDecl = std::make_shared<VariableDeclarationDOM>(".func", funcSrc.getType(), location);
        DECLARE(funcVarDecl, funcSrc);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _func_ident = nf.createIdentifier(S(".func"));

        auto _v_ident = nf.createIdentifier(S(".v"));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto [pos, _end] = getPos(location);

        auto _yield_expr = nf.createYieldExpression(undefined, _v_ident);
        _yield_expr->pos.pos = pos;
        _yield_expr->_end = _end;

        auto forOfStat = nf.createForOfStatement(
            undefined, declList, _src_array_ident,
            nf.createIfStatement(nf.createCallExpression(_func_ident, undefined, argumentsArray),
                                 nf.createExpressionStatement(_yield_expr),
                                 undefined));

        // iterator
        auto iterName = MLIRHelper::getAnonymousName(location, ".iter");

        NodeArray<Statement> statements;
        statements.push_back(forOfStat);
        auto block = nf.createBlock(statements, false);
        auto funcIter =
            nf.createFunctionExpression(undefined, nf.createToken(SyntaxKind::AsteriskToken),
                                        nf.createIdentifier(ConvertUTF8toWide(iterName)), undefined, undefined, undefined, block);
        funcIter->pos.pos = pos;
        funcIter->_end = _end;

        // call
        NodeArray<Expression> emptyArguments;
        auto callOfIter = nf.createCallExpression(funcIter, undefined, emptyArguments);

        return mlirGen(callOfIter, genContext);
    }

    ValueOrLogicalResult mlirGenArrayReduce(mlir::Location location, SmallVector<mlir::Value, 4> &operands,
                                            const GenContext &genContext)
    {
        // info, we add "_" extra as scanner append "_" in front of "__";
        auto funcName = "___array_reduce";

        if (!existGenericFunctionMap(funcName))
        {
            auto src = S("function __array_reduce<T, R>(arr: T[], f: (s: R, v: T) => R, init: R) \
            {   \
                let r = init;   \
                for (const v of arr) r = f(r, v);   \
                return r;   \
            }");

            if (mlir::failed(parsePartialStatements(src)))
            {
                assert(false);
                return mlir::failure();
            }
        }

        auto funcResult = resolveIdentifier(location, funcName, genContext);

        assert(funcResult);

        return mlirGenCallExpression(location, funcResult, {}, operands, genContext);
    }

    ValueOrLogicalResult mlirGenCallExpression(mlir::Location location, mlir::Value funcResult,
                                               NodeArray<TypeNode> typeArguments, SmallVector<mlir::Value, 4> &operands,
                                               const GenContext &genContext)
    {
        GenContext specGenContext(genContext);
        specGenContext.callOperands = operands;

        // get function ref.
        auto result = mlirGenSpecialized(location, funcResult, typeArguments, specGenContext);
        EXIT_IF_FAILED(result)
        auto actualFuncRefValue = V(result);

        if (mth.isVirtualFunctionType(actualFuncRefValue))
        {
            // TODO: when you resolve names such as "print", "parseInt" should return names in mlirGen(Identifier)
            auto calleeName = actualFuncRefValue.getDefiningOp()->getAttrOfType<mlir::FlatSymbolRefAttr>(StringRef(IDENTIFIER_ATTR_NAME));
            auto functionName = calleeName.getValue();

            if (auto thisSymbolRefOp = actualFuncRefValue.getDefiningOp<mlir_ts::ThisSymbolRefOp>())
            {
                // do not remove it, it is needed for custom methods to be called correctly
                operands.insert(operands.begin(), thisSymbolRefOp.getThisVal());
            }

            // temp hack
            if (functionName == "__array_foreach")
            {
                mlirGenArrayForEach(location, operands, genContext);
                return mlir::success();
            }

            if (functionName == "__array_every")
            {
                return mlirGenArrayEvery(location, operands, genContext);
            }

            if (functionName == "__array_some")
            {
                return mlirGenArraySome(location, operands, genContext);
            }

            if (functionName == "__array_map")
            {
                return mlirGenArrayMap(location, operands, genContext);
            }

            if (functionName == "__array_filter")
            {
                return mlirGenArrayFilter(location, operands, genContext);
            }

            if (functionName == "__array_reduce")
            {
                return mlirGenArrayReduce(location, operands, genContext);
            }

            // resolve function
            MLIRCustomMethods cm(builder, location, compileOptions);
            return cm.callMethod(functionName, operands, genContext);
        }

        if (auto optFuncRef = actualFuncRefValue.getType().dyn_cast<mlir_ts::OptionalType>())
        {
            CAST_A(condValue, location, getBooleanType(), actualFuncRefValue, genContext);

            auto resultType = mth.getReturnTypeFromFuncRef(optFuncRef.getElementType());

            LLVM_DEBUG(llvm::dbgs() << "\n!! Conditional call, return type: " << resultType << "\n";);

            auto hasReturn = !mth.isNoneType(resultType) && resultType != getVoidType();
            auto ifOp = hasReturn
                            ? builder.create<mlir_ts::IfOp>(location, getOptionalType(resultType), condValue, true)
                            : builder.create<mlir_ts::IfOp>(location, condValue, false);

            builder.setInsertionPointToStart(&ifOp.getThenRegion().front());

            // value if true

            auto innerFuncRef =
                builder.create<mlir_ts::ValueOp>(location, optFuncRef.getElementType(), actualFuncRefValue);

            auto result = mlirGenCallExpression(location, innerFuncRef, typeArguments, operands, genContext);
            auto value = V(result);
            if (value)
            {
                auto optValue =
                    builder.create<mlir_ts::OptionalValueOp>(location, getOptionalType(value.getType()), value);
                builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{optValue});

                // else
                builder.setInsertionPointToStart(&ifOp.getElseRegion().front());

                auto optUndefValue = builder.create<mlir_ts::OptionalUndefOp>(location, getOptionalType(resultType));
                builder.create<mlir_ts::ResultOp>(location, mlir::ValueRange{optUndefValue});
            }

            builder.setInsertionPointAfter(ifOp);

            if (hasReturn)
            {
                return ifOp.getResults().front();
            }

            return mlir::success();
        }

        return mlirGenCall(location, actualFuncRefValue, operands, genContext);
    }

    ValueOrLogicalResult mlirGenCall(mlir::Location location, mlir::Value funcRefValue,
                                     SmallVector<mlir::Value, 4> &operands, const GenContext &genContext)
    {
        ValueOrLogicalResult value(mlir::failure());
        TypeSwitch<mlir::Type>(funcRefValue.getType())
            .Case<mlir_ts::FunctionType>([&](auto calledFuncType) {
                value = mlirGenCallFunction(location, calledFuncType, funcRefValue, operands, genContext);
            })
            .Case<mlir_ts::HybridFunctionType>([&](auto calledFuncType) {
                value = mlirGenCallFunction(location, calledFuncType, funcRefValue, operands, genContext);
            })
            .Case<mlir_ts::BoundFunctionType>([&](auto calledBoundFuncType) {
                auto calledFuncType =
                    getFunctionType(calledBoundFuncType.getInputs(), calledBoundFuncType.getResults(), calledBoundFuncType.isVarArg());
                auto thisValue = builder.create<mlir_ts::GetThisOp>(location, calledFuncType.getInput(0), funcRefValue);
                auto unboundFuncRefValue = builder.create<mlir_ts::GetMethodOp>(location, calledFuncType, funcRefValue);
                value = mlirGenCallFunction(location, calledFuncType, unboundFuncRefValue, thisValue, operands, genContext);
            })
            .Case<mlir_ts::ExtensionFunctionType>([&](auto calledExtentFuncType) {
                auto calledFuncType =
                    getFunctionType(calledExtentFuncType.getInputs(), calledExtentFuncType.getResults(), calledExtentFuncType.isVarArg());
                if (auto createExtensionFunctionOp = funcRefValue.getDefiningOp<mlir_ts::CreateExtensionFunctionOp>())
                {
                    auto thisValue = createExtensionFunctionOp.getThisVal();
                    auto funcRefValue = createExtensionFunctionOp.getFunc();
                    value = mlirGenCallFunction(location, calledFuncType, funcRefValue, thisValue, operands, genContext);
                }
                else
                {
                    emitError(location, "not supported");
                    value = mlir::Value();
                }
            })
            .Case<mlir_ts::ClassType>([&](auto classType) {
                // seems we are calling type constructor
                // TODO: review it, really u should forbide to use "a = Class1();" to allocate in stack, or finish it
                // using Class..new(true) method
                auto newOp = NewClassInstanceLogicAsOp(location, classType, true, genContext);
                auto classInfo = getClassInfoByFullName(classType.getName().getValue());
                if (mlir::failed(mlirGenCallConstructor(location, classInfo, newOp, operands, false, genContext)))
                {
                    value = mlir::failure();
                }
                else
                {
                    value = newOp;
                }
            })
            .Case<mlir_ts::ClassStorageType>([&](auto classStorageType) {
                MLIRCodeLogic mcl(builder);
                auto refValue = mcl.GetReferenceOfLoadOp(funcRefValue);
                if (refValue)
                {
                    // seems we are calling type constructor for super()
                    auto classInfo = getClassInfoByFullName(classStorageType.getName().getValue());
                    // to track result call
                    value = mlirGenCallConstructor(location, classInfo, refValue, operands, true, genContext);
                }
                else
                {
                    llvm_unreachable("not implemented");
                }
            })
            .Default([&](auto type) {
                // TODO: this is hack, rewrite it
                // it is not function, so just return value as maybe it has been resolved earlier like in case
                // "<number>.ToString()"
                value = funcRefValue;
            });

        return value;
    }

    template <typename T = mlir_ts::FunctionType>
    ValueOrLogicalResult mlirGenCallFunction(mlir::Location location, T calledFuncType, mlir::Value funcRefValue,
                                             SmallVector<mlir::Value, 4> &operands, const GenContext &genContext)
    {
        return mlirGenCallFunction(location, calledFuncType, funcRefValue, mlir::Value(), operands, genContext);
    }

    template <typename T = mlir_ts::FunctionType>
    ValueOrLogicalResult mlirGenCallFunction(mlir::Location location, T calledFuncType, mlir::Value funcRefValue,
                                             mlir::Value thisValue, SmallVector<mlir::Value, 4> &operands,
                                             const GenContext &genContext)
    {
        if (thisValue)
        {
            operands.insert(operands.begin(), thisValue);
        }

        if (mlir::failed(mlirGenPrepareCallOperands(location, operands, calledFuncType.getInputs(), calledFuncType.isVarArg(),
                                             genContext)))
        {
            return mlir::failure();
        }
        else
        {
            for (auto &oper : operands)
            {
                VALIDATE(oper, location)
            }

            // if last is vararg
            if (calledFuncType.isVarArg())
            {
                auto varArgsType = calledFuncType.getInputs().back();
                auto fromIndex = calledFuncType.getInputs().size() - 1;
                auto toIndex = operands.size();

                LLVM_DEBUG(llvm::dbgs() << "\n!! isVarArg type (array), type: " << varArgsType << "\n";);
                LLVM_DEBUG(llvm::dbgs() << "\t last value = " << operands.back() << "\n";);

                // check if vararg is prepared earlier
                auto isVarArgPreparedAlready = (toIndex - fromIndex) == 1 && operands.back().getType() == varArgsType;
                if (!isVarArgPreparedAlready)
                {
                    SmallVector<mlir::Value, 4> varArgOperands;
                    for (auto i = fromIndex; i < toIndex; i++)
                    {
                        varArgOperands.push_back(operands[i]);
                    }

                    operands.pop_back_n(toIndex - fromIndex);

                    // create array
                    auto array = varArgOperands.empty() && !varArgsType.template isa<mlir_ts::ArrayType>()
                        ? V(builder.create<mlir_ts::UndefOp>(location, varArgsType))
                        : V(builder.create<mlir_ts::CreateArrayOp>(location, varArgsType, varArgOperands));
                    operands.push_back(array);

                    LLVM_DEBUG(for (auto& ops : varArgOperands) llvm::dbgs() << "\t value = " << ops << "\n";);
                }
            }

            VALIDATE_FUNC(calledFuncType, location)

            // default call by name
            auto callIndirectOp = builder.create<mlir_ts::CallIndirectOp>(
                MLIRHelper::getCallSiteLocation(funcRefValue, location),
                funcRefValue, operands);

            if (calledFuncType.getResults().size() > 0)
            {
                auto callValue = callIndirectOp.getResult(0);
                auto hasReturn = callValue.getType() != getVoidType();
                if (hasReturn)
                {
                    return callValue;
                }
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenPrepareCallOperands(mlir::Location location, SmallVector<mlir::Value, 4> &operands,
                                            mlir::ArrayRef<mlir::Type> argFuncTypes, bool isVarArg,
                                            const GenContext &genContext)
    {
        int opArgsCount = operands.size();
        int funcArgsCount = argFuncTypes.size();

        if (mlir::failed(mlirGenAdjustOperandTypes(operands, argFuncTypes, isVarArg, genContext)))
        {
            return mlir::failure();
        }

        if (funcArgsCount > opArgsCount)
        {
            auto lastArgIndex = argFuncTypes.size() - 1;

            // -1 to exclude count params
            for (auto i = (size_t)opArgsCount; i < funcArgsCount; i++)
            {
                if (i == 0)
                {
                    if (auto refType = argFuncTypes[i].dyn_cast<mlir_ts::RefType>())
                    {
                        if (refType.getElementType().isa<mlir_ts::TupleType>())
                        {
                            llvm_unreachable("capture or this ref is not resolved.");
                            return mlir::failure();
                        }
                    }
                }

                if (isVarArg && i >= lastArgIndex)
                {
                    break;
                }

                operands.push_back(builder.create<mlir_ts::UndefOp>(location, argFuncTypes[i]));
            }
        }

        return mlir::success();
    }

    struct OperandsProcessingInfo
    {
        OperandsProcessingInfo(mlir::Type funcType, SmallVector<mlir::Value, 4> &operands, int offsetArgs, bool noReceiverTypesForGenericCall, MLIRTypeHelper &mth, bool disableSpreadParam) 
            : operands{operands}, lastArgIndex{-1}, isVarArg{false}, hasType{false}, currentParameter{offsetArgs}, noReceiverTypesForGenericCall{noReceiverTypesForGenericCall}, mth{mth}
        {
            detectVarArgTypeInfo(funcType, disableSpreadParam);
        }

        void detectVarArgTypeInfo(mlir::Type funcType, bool disableSpreadParam)
        {
            auto tupleParamsType = mth.getParamsTupleTypeFromFuncRef(funcType);
            if (!tupleParamsType || tupleParamsType.isa<mlir::NoneType>())
            {
                return;
            }

            hasType = true;
            parameters = tupleParamsType.cast<mlir_ts::TupleType>().getFields();
            lastArgIndex = parameters.size() - 1;
            if (!disableSpreadParam && mth.getVarArgFromFuncRef(funcType))
            {
                varArgType = parameters.back().type;
                if (auto arrayType = varArgType.dyn_cast<mlir_ts::ArrayType>())
                {
                    varArgType = arrayType.getElementType();
                }
                else if (auto genericType = varArgType.dyn_cast<mlir_ts::NamedGenericType>())
                {
                    // do nothing in case of generic, types will be adjusted later
                    varArgType = mlir::Type();
                }
            }
        }

        mlir::Type getReceiverType()
        {
            if (!hasType)
            {
                return mlir::Type();
            }

            if (isVarArg && currentParameter >= lastArgIndex)
            {
                return varArgType;
            }

            auto receiverType = 
                currentParameter < parameters.size() 
                    ? parameters[currentParameter].type 
                    : mlir::Type();
            return receiverType;
        }

        void setReceiverTo(GenContext &argGenContext)
        {
            if (!hasType)
            {
                return;
            }

            argGenContext.receiverFuncType = getReceiverType();
            argGenContext.receiverType = 
                !noReceiverTypesForGenericCall 
                    ? argGenContext.receiverFuncType 
                    : mlir::Type();
        }

        mlir::Type isCastNeededWithOptionalUnwrap(mlir::Type type)
        {
            return isCastNeeded(type, true);
        }

        mlir::Type isCastNeeded(mlir::Type type, bool isOptionalUnwrap = false)
        {
            auto receiverType = getReceiverType();
            if (isOptionalUnwrap) if (auto optReceiverType = receiverType.dyn_cast<mlir_ts::OptionalType>())
            {
                receiverType = optReceiverType.getElementType();
            }

            return receiverType && type != receiverType 
                ? receiverType 
                : mlir::Type();
        }

        void nextParameter()
        {
            isVarArg = ++currentParameter == lastArgIndex && varArgType;
        }

        auto restCount()
        {
            return lastArgIndex - currentParameter + 1;
        }

        void addOperand(mlir::Value value)
        {
            operands.push_back(value);
        }

        void addOperandAndMoveToNextParameter(mlir::Value value)
        {
            addOperand(value);
            nextParameter();
        }

        SmallVector<mlir::Value, 4> &operands;
        llvm::ArrayRef<mlir::typescript::FieldInfo> parameters;
        int lastArgIndex;
        bool isVarArg;
        mlir::Type varArgType;
        bool hasType;
        int currentParameter;
        bool noReceiverTypesForGenericCall;
        MLIRTypeHelper &mth;
    };

    mlir::LogicalResult processOperandSpreadElement(mlir::Location location, mlir::Value source, OperandsProcessingInfo &operandsProcessingInfo, const GenContext &genContext)
    {
        auto count = operandsProcessingInfo.restCount();

        auto nextPropertyType = evaluateProperty(source, ITERATOR_NEXT, genContext);
        if (nextPropertyType)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! SpreadElement, next type is: " << nextPropertyType << "\n";);

            auto returnType = mth.getReturnTypeFromFuncRef(nextPropertyType);
            if (returnType)
            {
                // as tuple or const_tuple
                ::llvm::ArrayRef<mlir_ts::FieldInfo> fields;
                TypeSwitch<mlir::Type>(returnType)
                    .template Case<mlir_ts::TupleType>([&](auto tupleType) { fields = tupleType.getFields(); })
                    .template Case<mlir_ts::ConstTupleType>(
                        [&](auto constTupleType) { fields = constTupleType.getFields(); })
                    .Default([&](auto type) { llvm_unreachable("not implemented"); });

                auto propValue = mlir::StringAttr::get(builder.getContext(), "value");
                if (std::any_of(fields.begin(), fields.end(), [&] (auto field) { return field.id == propValue; }))
                {
                    // treat it as <???>.next().value structure
                    // property
                    auto nextProperty = mlirGenPropertyAccessExpression(location, source, ITERATOR_NEXT, false, genContext);

                    for (auto spreadIndex = 0;  spreadIndex < count; spreadIndex++)
                    {
                        // call nextProperty
                        SmallVector<mlir::Value, 4> callOperands;
                        auto callResult = mlirGenCall(location, nextProperty, callOperands, genContext);
                        EXIT_IF_FAILED_OR_NO_VALUE(callResult)

                        // load property "value"
                        auto doneProperty = mlirGenPropertyAccessExpression(location, callResult, "done", false, genContext);
                        EXIT_IF_FAILED_OR_NO_VALUE(doneProperty)

                        auto valueProperty = mlirGenPropertyAccessExpression(location, callResult, "value", false, genContext);
                        EXIT_IF_FAILED_OR_NO_VALUE(valueProperty)

                        auto valueProp = V(valueProperty);

                        if (auto receiverType = operandsProcessingInfo.isCastNeededWithOptionalUnwrap(valueProp.getType()))
                        {
                            CAST(valueProp, location, receiverType, valueProp, genContext);
                        }                        

                        // conditional expr:  done ? undefined : value
                        auto doneInvValue =  V(builder.create<mlir_ts::ArithmeticUnaryOp>(location, getBooleanType(),
                            builder.getI32IntegerAttr((int)SyntaxKind::ExclamationToken), doneProperty));

                        mlir::Value condValue;
                        // if (valueProp.getType().isa<mlir_ts::AnyType>())
                        // {
                        //     condValue = anyOrUndefined(location, doneInvValue, [&](auto genContext) { return valueProp; }, genContext);
                        // }
                        // else
                        // {
                            condValue = builder.create<mlir_ts::OptionalOp>(location, getOptionalType(valueProp.getType()), valueProp, doneInvValue);
                        // }

                        operandsProcessingInfo.addOperandAndMoveToNextParameter(condValue);
                    }
                }
                else
                {
                    llvm_unreachable("not implemented");
                }

                return mlir::success();    
            }
        }                                        

        if (auto lengthPropertyType = evaluateProperty(source, LENGTH_FIELD_NAME, genContext))
        {
            // treat it as <???>[index] structure
            auto lengthProperty = mlirGenPropertyAccessExpression(location, source, LENGTH_FIELD_NAME, false, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(lengthProperty)

            auto elementType = evaluateElementAccess(location, source, false, genContext);
            if (genContext.receiverType && genContext.receiverType != elementType)
            {
                elementType = genContext.receiverType;
            }

            auto valueFactory =
            (elementType.isa<mlir_ts::AnyType>())
                ? std::bind(&MLIRGenImpl::anyOrUndefined, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4)
                : std::bind(&MLIRGenImpl::optionalValueOrUndefined, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);

            for (auto spreadIndex = 0;  spreadIndex < count; spreadIndex++)
            {
                auto indexVal = builder.create<mlir_ts::ConstantOp>(location, mth.getStructIndexType(),
                                                    mth.getStructIndexAttrValue(spreadIndex));

                // conditional expr:  length > "spreadIndex" ? value[index] : undefined
                auto inBoundsValue =  V(builder.create<mlir_ts::LogicalBinaryOp>(location, getBooleanType(),
                    builder.getI32IntegerAttr((int)SyntaxKind::GreaterThanToken), 
                    lengthProperty,
                    indexVal));

                auto spreadValue = valueFactory(location, inBoundsValue, 
                    [&](auto genContext) { 
                        auto result = mlirGenElementAccess(location, source, indexVal, false, genContext); 
                        EXIT_IF_FAILED_OR_NO_VALUE(result)
                        auto value = V(result);

                        if (auto receiverType = operandsProcessingInfo.isCastNeeded(value.getType()))
                        {
                            CAST(value, location, receiverType, value, genContext);
                        }

                        return ValueOrLogicalResult(value);
                    }, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(spreadValue)

                operandsProcessingInfo.addOperandAndMoveToNextParameter(spreadValue);
            }

            return mlir::success();
        }

        // this is defualt behavior for tuple
        // treat it as <???>[index] structure
        for (auto spreadIndex = 0;  spreadIndex < count; spreadIndex++)
        {
            auto indexVal = builder.create<mlir_ts::ConstantOp>(location, mth.getStructIndexType(),
                                                mth.getStructIndexAttrValue(spreadIndex));

            auto result = mlirGenElementAccess(location, source, indexVal, false, genContext); 
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto value = V(result);

            operandsProcessingInfo.addOperandAndMoveToNextParameter(value);
        }

        return mlir::success();        
    }

    mlir::LogicalResult mlirGenOperand(Expression expression, OperandsProcessingInfo &operandsProcessingInfo, const GenContext &genContext)
    {
        GenContext argGenContext(genContext);
        argGenContext.clearReceiverTypes();
        operandsProcessingInfo.setReceiverTo(argGenContext);

        auto result = mlirGen(expression, argGenContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto value = V(result);

        if (expression == SyntaxKind::SpreadElement)
        {
            auto location = loc(expression);
            if (mlir::failed(processOperandSpreadElement(location, value, operandsProcessingInfo, argGenContext)))
            {
                return mlir::failure();
            }

            return mlir::success();
        }

        operandsProcessingInfo.addOperandAndMoveToNextParameter(value);
        return mlir::success();
    }

    mlir::LogicalResult mlirGenOperandVarArgs(mlir::Location location, int processedArgs, NodeArray<Expression> arguments, 
        OperandsProcessingInfo &operandsProcessingInfo, const GenContext &genContext)
    {
        // calculate array context
        SmallVector<ArrayElement> values;
        struct ArrayInfo arrayInfo{};

        // set receiver type
        auto receiverType = mlir_ts::ArrayType::get(operandsProcessingInfo.getReceiverType());

        LLVM_DEBUG(llvm::dbgs() << "\n!! varargs - receiver type: " << receiverType << "\n";);
        // TODO: isGenericType is applied as hack here, find out the issue
        arrayInfo.setReceiver(receiverType, mth.isGenericType(receiverType));

        for (auto it = arguments.begin() + processedArgs; it != arguments.end(); ++it)
        {
            if (mlir::failed(processArrayElementForValues(*it, values, arrayInfo, genContext)))
            {
                return mlir::failure();
            }
        }

        arrayInfo.adjustArrayType(getAnyType());

        auto varArgOperandValue = createArrayFromArrayInfo(location, values, arrayInfo, genContext);
        operandsProcessingInfo.addOperand(varArgOperandValue);

        return mlir::success();
    }

    // TODO: rewrite code (do as clean as ArrayLiteral)
    mlir::LogicalResult mlirGenOperands(NodeArray<Expression> arguments, SmallVector<mlir::Value, 4> &operands,
                                        mlir::Type funcType, const GenContext &genContext, int offsetArgs = 0, bool noReceiverTypesForGenericCall = false)
    {
        OperandsProcessingInfo operandsProcessingInfo(funcType, operands, offsetArgs, noReceiverTypesForGenericCall, mth, genContext.disableSpreadParams);

        for (auto it = arguments.begin(); it != arguments.end(); ++it)
        {
            if (operandsProcessingInfo.isVarArg)
            {
                auto proccessedArgs = std::distance(arguments.begin(), it);
                return mlirGenOperandVarArgs(loc(arguments), proccessedArgs, arguments, operandsProcessingInfo, genContext);
            }            

            if (mlir::failed(mlirGenOperand(*it, operandsProcessingInfo, genContext)))
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenAdjustOperandTypes(SmallVector<mlir::Value, 4> &operands,
                                                  mlir::ArrayRef<mlir::Type> argFuncTypes, bool isVarArg,
                                                  const GenContext &genContext)
    {
        auto i = 0; // we need to shift in case of 'this'
        auto lastArgIndex = argFuncTypes.size() - 1;
        mlir::Type varArgType;
        if (isVarArg)
        {
            auto lastType = argFuncTypes.back();
            if (auto arrayType = dyn_cast<mlir_ts::ArrayType>(lastType))
            {
                lastType = arrayType.getElementType();
            }

            varArgType = lastType;
        }

        for (auto value : operands)
        {
            VALIDATE(value, value.getLoc())

            mlir::Type argTypeDestFuncType;
            if (i >= argFuncTypes.size() && !isVarArg)
            {
                emitError(value.getLoc())
                    << "function does not have enough parameters to accept all arguments, arg #" << i;
                return mlir::failure();
            }

            if (isVarArg && i >= lastArgIndex)
            {
                argTypeDestFuncType = varArgType;

                // if we have processed VarArg - do nothing
                if (i == lastArgIndex 
                    && lastArgIndex == operands.size() - 1
                    && value.getType() == getArrayType(varArgType))
                {
                    // nothing todo 
                    break;
                }
            }
            else
            {
                argTypeDestFuncType = argFuncTypes[i];
            }

            if (value.getType() != argTypeDestFuncType)
            {
                CAST_A(castValue, value.getLoc(), argTypeDestFuncType, value, genContext);
                operands[i] = castValue;
            }

            i++;
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenSetVTableToInstance(mlir::Location location, ClassInfo::TypePtr classInfo,
                                                   mlir::Value thisValue, const GenContext &genContext)
    {
        auto virtualTable = classInfo->getHasVirtualTable();
        if (!virtualTable)
        {
            return mlir::success();
        }

        auto result = mlirGenPropertyAccessExpression(location, thisValue, VTABLE_NAME, genContext);
        auto vtableVal = V(result);
        MLIRCodeLogic mcl(builder);
        auto vtableRefVal = mcl.GetReferenceOfLoadOp(vtableVal);

        // vtable symbol reference
        auto fullClassVTableFieldName = concat(classInfo->fullName, VTABLE_NAME);
        auto vtableAddress = resolveFullNameIdentifier(location, fullClassVTableFieldName, true, genContext);

        mlir::Value vtableValue;
        if (vtableAddress)
        {
            CAST_A(castedValue, location, getOpaqueType(), vtableAddress, genContext);
            vtableValue = castedValue;
        }
        else
        {
            // we need to calculate VTable type
            /*
            llvm::SmallVector<VirtualMethodOrInterfaceVTableInfo> virtualTable;
            classInfo->getVirtualTable(virtualTable);
            auto virtTuple = getVirtualTableType(virtualTable);

            auto classVTableRefOp = builder.create<mlir_ts::AddressOfOp>(
                location, mlir_ts::RefType::get(virtTuple), fullClassVTableFieldName, ::mlir::IntegerAttr());

            CAST_A(castedValue, location, getOpaqueType(), classVTableRefOp, genContext);
            vtableValue = castedValue;
            */

            // vtable type will be detected later
            auto classVTableRefOp = builder.create<mlir_ts::AddressOfOp>(
                location, getOpaqueType(), fullClassVTableFieldName, ::mlir::IntegerAttr());

            vtableValue = classVTableRefOp;
        }

        builder.create<mlir_ts::StoreOp>(location, vtableValue, vtableRefVal);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenCallConstructor(mlir::Location location, ClassInfo::TypePtr classInfo,
                                               mlir::Value thisValue, SmallVector<mlir::Value, 4> &operands,
                                               bool castThisValueToClass, const GenContext &genContext)
    {
        assert(classInfo);

        auto virtualTable = classInfo->getHasVirtualTable();
        auto hasConstructor = classInfo->getHasConstructor();
        if (!hasConstructor && !virtualTable)
        {
            return mlir::success();
        }

        auto effectiveThisValue = thisValue;
        if (castThisValueToClass)
        {
            CAST(effectiveThisValue, location, classInfo->classType, thisValue, genContext);
        }

        if (classInfo->getHasConstructor())
        {
            auto propAccess =
                mlirGenPropertyAccessExpression(location, effectiveThisValue, CONSTRUCTOR_NAME, false, genContext);

            if (!propAccess && !genContext.allowPartialResolve)
            {
                emitError(location) << "Call Constructor: can't find constructor";
            }

            EXIT_IF_FAILED_OR_NO_VALUE(propAccess)
            return mlirGenCall(location, propAccess, operands, genContext);
        }

        return mlir::success();
    }

    ValueOrLogicalResult NewClassInstance(mlir::Location location, mlir::Value value, NodeArray<Expression> arguments,
                                          NodeArray<TypeNode> typeArguments, bool suppressConstructorCall, 
                                          const GenContext &genContext)
    {

        auto type = value.getType();
        type = mth.convertConstTupleTypeToTupleType(type);

        assert(type);

        auto resultType = type;
        if (mth.isValueType(type))
        {
            resultType = getValueRefType(type);
        }

        // if true, will call Class..new method, otheriwise ts::NewOp which we need to implement Class..new method
        auto methodCallWay = !suppressConstructorCall;

        mlir::Value newOp;
        if (auto classType = resultType.dyn_cast<mlir_ts::ClassType>())
        {
            auto classInfo = getClassInfoByFullName(classType.getName().getValue());
            if (genContext.dummyRun)
            {
                // just to cut a lot of calls
                newOp = builder.create<mlir_ts::NewOp>(location, classInfo->classType, builder.getBoolAttr(false));
                return newOp;
            }

            auto newOp = NewClassInstanceAsMethodCallOp(location, classInfo, methodCallWay, genContext);
            if (!newOp)
            {
                return mlir::failure();
            }

            if (methodCallWay)
            {
                // evaluate constructor
                mlir::Type tupleParamsType;
                auto funcValueRef = evaluateProperty(newOp, CONSTRUCTOR_NAME, genContext);
                if (funcValueRef)
                {
                    SmallVector<mlir::Value, 4> operands;
                    if (mlir::failed(mlirGenOperands(arguments, operands, funcValueRef, genContext, 1/*this params shift*/)))
                    {
                        emitError(location) << "Call constructor: can't resolve values of all parameters";
                        return mlir::failure();
                    }

                    assert(newOp);
                    auto result  = mlirGenCallConstructor(location, classInfo, newOp, operands, false, genContext);
                    EXIT_IF_FAILED(result)
                }
            }

            return newOp;
        }

        return NewClassInstanceLogicAsOp(location, resultType, false, genContext);
    }

    ValueOrLogicalResult NewClassInstanceLogicAsOp(mlir::Location location, mlir::Type typeOfInstance, bool stackAlloc,
                                                   const GenContext &genContext)
    {
        if (auto classType = typeOfInstance.dyn_cast<mlir_ts::ClassType>())
        {
            // set virtual table
            auto classInfo = getClassInfoByFullName(classType.getName().getValue());
            return NewClassInstanceLogicAsOp(location, classInfo, stackAlloc, genContext);
        }

        auto newOp = builder.create<mlir_ts::NewOp>(location, typeOfInstance, builder.getBoolAttr(stackAlloc));
        return V(newOp);
    }

    mlir::Value NewClassInstanceLogicAsOp(mlir::Location location, ClassInfo::TypePtr classInfo, bool stackAlloc,
                                          const GenContext &genContext)
    {
        mlir::Value newOp;
#if ENABLE_TYPED_GC
        auto enabledGC = !compileOptions.disableGC;
        if (enabledGC && !stackAlloc)
        {
            auto typeDescrType = builder.getI64Type();
            auto typeDescGlobalName = getTypeDescriptorFieldName(classInfo);
            auto typeDescRef = resolveFullNameIdentifier(location, typeDescGlobalName, true, genContext);
            auto typeDescCurrentValue = builder.create<mlir_ts::LoadOp>(location, typeDescrType, typeDescRef);

            CAST_A(condVal, location, getBooleanType(), typeDescCurrentValue, genContext);

            auto ifOp = builder.create<mlir_ts::IfOp>(
                location, mlir::TypeRange{typeDescrType}, condVal,
                [&](mlir::OpBuilder &opBuilder, mlir::Location loc) {
                    builder.create<mlir_ts::ResultOp>(loc, mlir::ValueRange{typeDescCurrentValue});
                },
                [&](mlir::OpBuilder &opBuilder, mlir::Location loc) {
                    // call typr bitmap
                    auto fullClassStaticFieldName = getTypeBitmapMethodName(classInfo);

                    auto funcType = getFunctionType({}, {typeDescrType}, false);

                    auto funcSymbolOp = builder.create<mlir_ts::SymbolRefOp>(
                        location, funcType,
                        mlir::FlatSymbolRefAttr::get(builder.getContext(), fullClassStaticFieldName));

                    auto callIndirectOp =
                        builder.create<mlir_ts::CallIndirectOp>(
                            MLIRHelper::getCallSiteLocation(funcSymbolOp->getLoc(), location),
                            funcSymbolOp, mlir::ValueRange{});
                    auto typeDescr = callIndirectOp.getResult(0);

                    // save value
                    builder.create<mlir_ts::StoreOp>(location, typeDescr, typeDescRef);

                    builder.create<mlir_ts::ResultOp>(loc, mlir::ValueRange{typeDescr});
                });

            auto typeDescrValue = ifOp.getResult(0);

            assert(!stackAlloc);
            newOp = builder.create<mlir_ts::GCNewExplicitlyTypedOp>(location, classInfo->classType, typeDescrValue);
        }
        else
        {
            newOp = builder.create<mlir_ts::NewOp>(location, classInfo->classType, builder.getBoolAttr(stackAlloc));
        }
#else
        newOp = builder.create<mlir_ts::NewOp>(location, classInfo->classType, builder.getBoolAttr(stackAlloc));
#endif
        mlirGenSetVTableToInstance(location, classInfo, newOp, genContext);
        return newOp;
    }

    mlir::Value NewClassInstanceAsMethodCallOp(mlir::Location location, ClassInfo::TypePtr classInfo, bool asMethodCall,
                                             const GenContext &genContext)
    {
#ifdef USE_NEW_AS_METHOD
        if (asMethodCall)
        {
            auto classRefVal = builder.create<mlir_ts::ClassRefOp>(
                location, classInfo->classType,
                mlir::FlatSymbolRefAttr::get(builder.getContext(), classInfo->classType.getName().getValue()));

            // call <Class>..new to create new instance
            auto result = mlirGenPropertyAccessExpression(location, classRefVal, NEW_METHOD_NAME, false, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto newFuncRef = V(result);

            assert(newFuncRef);

            SmallVector<mlir::Value, 4> emptyOperands;
            auto resultCall = mlirGenCallExpression(location, newFuncRef, {}, emptyOperands, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(resultCall)
            auto newOp = V(resultCall);
            return newOp;
        }
#endif

        return NewClassInstanceLogicAsOp(location, classInfo, false, genContext);
    }

    ValueOrLogicalResult NewClassInstanceByCallingNewCtor(mlir::Location location, mlir::Value value, NodeArray<Expression> arguments,
            NodeArray<TypeNode> typeArguments, const GenContext &genContext)
    {
        auto result = mlirGenPropertyAccessExpression(location, value, NEW_CTOR_METHOD_NAME, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto newCtorMethod = V(result);        

        SmallVector<mlir::Value, 4> operands;
        if (mlir::failed(mlirGenOperands(arguments, operands, newCtorMethod.getType(), genContext)))
        {
            emitError(location) << "Call new instance: can't resolve values of all parameters";
            return mlir::failure();
        }

        return mlirGenCallExpression(location, newCtorMethod, typeArguments, operands, genContext);        
    }

    ValueOrLogicalResult mlirGen(NewExpression newExpression, const GenContext &genContext)
    {
        auto location = loc(newExpression);

        auto newArray = [&](auto type, auto count) -> ValueOrLogicalResult {
            if (count.getType() != builder.getI32Type())
            {
                // TODO: test cast result
                count = cast(location, builder.getI32Type(), count, genContext);
            }

            if (!type)
            {
                return mlir::failure();
            }

            type = mth.convertConstTupleTypeToTupleType(type);

            auto newArrOp = builder.create<mlir_ts::NewArrayOp>(location, getArrayType(type), count);
            return V(newArrOp);
        };

        // 3 cases, name, index access, method call
        mlir::Type type;
        auto typeExpression = newExpression->expression;
        ////auto isNewArray = typeExpression == SyntaxKind::ElementAccessExpression && newExpression->arguments.isTextRangeEmpty();
        auto result = mlirGen(typeExpression, newExpression->typeArguments, genContext);
        if (result.failed())
        {
            if (typeExpression == SyntaxKind::Identifier)
            {
                auto name = MLIRHelper::getName(typeExpression.as<Identifier>());
                type = findEmbeddedType(name, newExpression->typeArguments, genContext);

                mlir::Type elementType;
                if (auto arrayType = type.dyn_cast_or_null<mlir_ts::ArrayType>())
                {
                    elementType = arrayType.getElementType();
                }
                else if (auto constArrayType = type.dyn_cast_or_null<mlir_ts::ConstArrayType>())
                {
                    elementType = constArrayType.getElementType();
                }

                if (elementType)
                {
                    mlir::Value count;

                    if (newExpression->arguments.size() == 0)
                    {
                        count = builder.create<mlir_ts::ConstantOp>(location, builder.getIntegerType(32, false), builder.getUI32IntegerAttr(0));
                    }
                    else if (newExpression->arguments.size() == 1)
                    {
                        auto result = mlirGen(newExpression->arguments.front(), genContext);
                        EXIT_IF_FAILED_OR_NO_VALUE(result)
                        count = V(result);           
                    }
                    else
                    {
                        llvm_unreachable("not implemented");
                    }

                    auto newArrOp = newArray(elementType, count);
                    EXIT_IF_FAILED_OR_NO_VALUE(newArrOp)
                    return V(newArrOp);                     
                }
            }
        }

        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto value = V(result);

        if (auto interfaceType = value.getType().dyn_cast<mlir_ts::InterfaceType>())
        {
            return NewClassInstanceByCallingNewCtor(location, value, newExpression->arguments, newExpression->typeArguments, genContext);
        }

        if (auto tupleType = value.getType().dyn_cast<mlir_ts::TupleType>())
        {
            auto newCtorMethod = evaluateProperty(value, NEW_CTOR_METHOD_NAME, genContext);
            if (newCtorMethod)
            {
                return NewClassInstanceByCallingNewCtor(location, value, newExpression->arguments, newExpression->typeArguments, genContext);
            }
        }

        // default - class instance
        auto suppressConstructorCall = (newExpression->internalFlags & InternalFlags::SuppressConstructorCall) ==
                                        InternalFlags::SuppressConstructorCall;

        return NewClassInstance(location, value, newExpression->arguments, newExpression->typeArguments, suppressConstructorCall, genContext);
    }

    mlir::LogicalResult mlirGen(DeleteExpression deleteExpression, const GenContext &genContext)
    {

        auto location = loc(deleteExpression);

        auto result = mlirGen(deleteExpression->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expr = V(result);

        if (!expr.getType().isa<mlir_ts::RefType>() && !expr.getType().isa<mlir_ts::ValueRefType>() &&
            !expr.getType().isa<mlir_ts::ClassType>())
        {
            if (auto arrayType = expr.getType().dyn_cast<mlir_ts::ArrayType>())
            {
                CAST(expr, location, mlir_ts::RefType::get(arrayType.getElementType()), expr, genContext);
            }
            else
            {
                llvm_unreachable("not implemented");
            }
        }

        builder.create<mlir_ts::DeleteOp>(location, expr);

        return mlir::success();
    }

    ValueOrLogicalResult mlirGen(VoidExpression voidExpression, const GenContext &genContext)
    {

        auto location = loc(voidExpression);

        auto result = mlirGen(voidExpression->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto expr = V(result);

        auto value = getUndefined(location);

        return value;
    }

    ValueOrLogicalResult mlirGen(TypeOfExpression typeOfExpression, const GenContext &genContext)
    {
        auto location = loc(typeOfExpression);

        auto result = mlirGen(typeOfExpression->expression, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto resultValue = V(result);
        // auto typeOfValue = builder.create<mlir_ts::TypeOfOp>(location, getStringType(), resultValue);
        // return V(typeOfValue);

        // needed to use optimizers
        TypeOfOpHelper toh(builder);
        auto typeOfValue = toh.typeOfLogic(location, resultValue, resultValue.getType());
        return typeOfValue;
    }

    ValueOrLogicalResult mlirGen(NonNullExpression nonNullExpression, const GenContext &genContext)
    {
        return mlirGen(nonNullExpression->expression, genContext);
    }

    ValueOrLogicalResult mlirGen(OmittedExpression ommitedExpression, const GenContext &genContext)
    {
        auto location = loc(ommitedExpression);

        return V(builder.create<mlir_ts::UndefOp>(location, getUndefinedType()));
    }

    ValueOrLogicalResult mlirGen(TemplateLiteralLikeNode templateExpressionAST, const GenContext &genContext)
    {
        auto location = loc(templateExpressionAST);

        auto stringType = getStringType();
        SmallVector<mlir::Value, 4> strs;

        auto text = convertWideToUTF8(templateExpressionAST->head->rawText);
        auto head = builder.create<mlir_ts::ConstantOp>(location, stringType, getStringAttr(text));

        // first string
        strs.push_back(head);
        for (auto span : templateExpressionAST->templateSpans)
        {
            auto expression = span->expression;
            auto result = mlirGen(expression, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto exprValue = V(result);

            if (exprValue.getType() != stringType)
            {
                CAST(exprValue, location, stringType, exprValue, genContext);
            }

            // expr value
            strs.push_back(exprValue);

            auto spanText = convertWideToUTF8(span->literal->rawText);
            auto spanValue = builder.create<mlir_ts::ConstantOp>(location, stringType, getStringAttr(spanText));

            // text
            strs.push_back(spanValue);
        }

        if (strs.size() <= 1)
        {
            return V(head);
        }

        auto concatValues =
            builder.create<mlir_ts::StringConcatOp>(location, stringType, mlir::ArrayRef<mlir::Value>{strs});

        return V(concatValues);
    }

    ValueOrLogicalResult mlirGen(TaggedTemplateExpression taggedTemplateExpressionAST, const GenContext &genContext)
    {
        auto location = loc(taggedTemplateExpressionAST);

        auto templateExpressionAST = taggedTemplateExpressionAST->_template;

        SmallVector<mlir::Attribute, 4> strs;
        SmallVector<mlir::Value, 4> vals;

        std::string text = convertWideToUTF8(
            templateExpressionAST->head 
                ? templateExpressionAST->head->rawText 
                : templateExpressionAST->rawText);

        // first string
        strs.push_back(getStringAttr(text));
        for (auto span : templateExpressionAST->templateSpans)
        {
            // expr value
            auto expression = span->expression;
            auto result = mlirGen(expression, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto exprValue = V(result);

            vals.push_back(exprValue);

            auto spanText = convertWideToUTF8(span->literal->rawText);
            // text
            strs.push_back(getStringAttr(spanText));
        }

        // tag method
        auto arrayAttr = mlir::ArrayAttr::get(builder.getContext(), strs);
        auto constStringArray =
            builder.create<mlir_ts::ConstantOp>(location, getConstArrayType(getStringType(), strs.size()), arrayAttr);

        CAST_A(strArrayValue, location, getArrayType(getStringType()), constStringArray, genContext);

        vals.insert(vals.begin(), strArrayValue);

        auto result = mlirGen(taggedTemplateExpressionAST->tag, genContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto callee = V(result);

        if (!mth.isAnyFunctionType(callee.getType()))
        {
            emitError(location, "is not callable");
            return mlir::failure();
        }

        VALIDATE_FUNC(callee.getType(), location)

        auto inputs = mth.getParamsFromFuncRef(callee.getType());

        SmallVector<mlir::Value, 4> operands;

        auto i = 0;
        for (auto value : vals)
        {
            if (inputs.size() <= i)
            {
                emitError(value.getLoc(), "not matching to tag parameters count");
                return mlir::Value();
            }

            if (value.getType() != inputs[i])
            {
                CAST_A(castValue, value.getLoc(), inputs[i], value, genContext);
                operands.push_back(castValue);
            }
            else
            {
                operands.push_back(value);
            }

            i++;
        }

        // call
        auto callIndirectOp = builder.create<mlir_ts::CallIndirectOp>(
            MLIRHelper::getCallSiteLocation(callee, location),
            callee, operands);
        if (callIndirectOp.getNumResults() > 0)
        {
            return callIndirectOp.getResult(0);
        }

        return mlir::success();
    }

    ValueOrLogicalResult mlirGen(NullLiteral nullLiteral, const GenContext &genContext)
    {
        return V(builder.create<mlir_ts::NullOp>(loc(nullLiteral), getNullType()));
    }

    ValueOrLogicalResult mlirGenBooleanValue(mlir::Location location, bool val)
    {
        auto attrVal = mlir::BoolAttr::get(builder.getContext(), val);
        auto literalType = mlir_ts::LiteralType::get(attrVal, getBooleanType());
        return V(builder.create<mlir_ts::ConstantOp>(location, literalType, attrVal));
    }

    ValueOrLogicalResult mlirGen(TrueLiteral trueLiteral, const GenContext &genContext)
    {
        return mlirGenBooleanValue(loc(trueLiteral), true);
    }

    ValueOrLogicalResult mlirGen(FalseLiteral falseLiteral, const GenContext &genContext)
    {
        return mlirGenBooleanValue(loc(falseLiteral), false);
    }

    mlir::Attribute getNumericLiteralAttribute(NumericLiteral numericLiteral)
    {
        if (numericLiteral->text.find(S(".")) == string::npos)
        {
            try
            {
                return builder.getI32IntegerAttr(to_unsigned_integer(numericLiteral->text));
            }
            catch (const std::out_of_range &)
            {
                return builder.getI64IntegerAttr(to_bignumber(numericLiteral->text));
            }
        }

#ifdef NUMBER_F64
        return builder.getF64FloatAttr(to_float_val(numericLiteral->text));
#else
        return builder.getF32FloatAttr(to_float_val(numericLiteral->text));
#endif
    }

    ValueOrLogicalResult mlirGen(NumericLiteral numericLiteral, const GenContext &genContext)
    {
        if (numericLiteral->text.find(S(".")) == string::npos)
        {
            try
            {
                auto attrVal = builder.getI32IntegerAttr(to_unsigned_integer(numericLiteral->text));
                auto literalType = mlir_ts::LiteralType::get(attrVal, builder.getI32Type());
                return V(builder.create<mlir_ts::ConstantOp>(loc(numericLiteral), literalType, attrVal));
            }
            catch (const std::out_of_range &)
            {
                auto attrVal = builder.getI64IntegerAttr(to_bignumber(numericLiteral->text));
                auto literalType = mlir_ts::LiteralType::get(attrVal, builder.getI64Type());
                return V(builder.create<mlir_ts::ConstantOp>(loc(numericLiteral), literalType, attrVal));
            }
        }

#ifdef NUMBER_F64
        auto attrVal = builder.getF64FloatAttr(to_float_val(numericLiteral->text));
        auto literalType = mlir_ts::LiteralType::get(attrVal, getNumberType());
        return V(builder.create<mlir_ts::ConstantOp>(loc(numericLiteral), literalType, attrVal));
#else
        auto attrVal = builder.getF32FloatAttr(to_float_val(numericLiteral->text));
        auto literalType = mlir_ts::LiteralType::get(attrVal, getNumberType());
        return V(builder.create<mlir_ts::ConstantOp>(loc(numericLiteral), literalType, attrVal));
#endif
    }

    ValueOrLogicalResult mlirGen(BigIntLiteral bigIntLiteral, const GenContext &genContext)
    {
        auto attrVal = builder.getI64IntegerAttr(to_bignumber(bigIntLiteral->text));
        auto literalType = mlir_ts::LiteralType::get(attrVal, builder.getI64Type());
        return V(builder.create<mlir_ts::ConstantOp>(loc(bigIntLiteral), literalType, attrVal));
    }

    ValueOrLogicalResult mlirGenStringValue(mlir::Location location, std::string text, bool asString = false)
    {
        auto attrVal = getStringAttr(text);
        auto literalType = asString ? (mlir::Type)getStringType() : (mlir::Type)mlir_ts::LiteralType::get(attrVal, getStringType());
        return V(builder.create<mlir_ts::ConstantOp>(location, literalType, attrVal));
    }

    ValueOrLogicalResult mlirGen(ts::StringLiteral stringLiteral, const GenContext &genContext)
    {
        auto text = convertWideToUTF8(stringLiteral->text);
        return mlirGenStringValue(loc(stringLiteral), text);
    }

    ValueOrLogicalResult mlirGen(ts::NoSubstitutionTemplateLiteral noSubstitutionTemplateLiteral,
                                 const GenContext &genContext)
    {
        auto text = convertWideToUTF8(noSubstitutionTemplateLiteral->text);

        auto attrVal = getStringAttr(text);
        auto literalType = mlir_ts::LiteralType::get(attrVal, getStringType());
        return V(builder.create<mlir_ts::ConstantOp>(loc(noSubstitutionTemplateLiteral), literalType, attrVal));
    }

    ValueOrLogicalResult mlirGenAppendArrayByEachElement(mlir::Location location, mlir::Value arrayDest, mlir::Value arraySrc,
                                            const GenContext &genContext)
    {
        SymbolTableScopeT varScope(symbolTable);

        // register vals
        auto srcArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".src_array", arraySrc.getType(), location);
        DECLARE(srcArrayVarDecl, arraySrc);

        auto dstArrayVarDecl = std::make_shared<VariableDeclarationDOM>(".dst_array", arrayDest.getType(), location);
        dstArrayVarDecl->setReadWriteAccess(true);
        DECLARE(dstArrayVarDecl, arrayDest);

        NodeFactory nf(NodeFactoryFlags::None);

        auto _src_array_ident = nf.createIdentifier(S(".src_array"));
        auto _dst_array_ident = nf.createIdentifier(S(".dst_array"));

        auto _push_ident = nf.createIdentifier(S("push"));

        auto _v_ident = nf.createIdentifier(S(".v"));

        NodeArray<VariableDeclaration> declarations;
        declarations.push_back(nf.createVariableDeclaration(_v_ident));
        auto declList = nf.createVariableDeclarationList(declarations, NodeFlags::Const);

        // access to push
        auto pushExpr = nf.createPropertyAccessExpression(_dst_array_ident, _push_ident);

        NodeArray<Expression> argumentsArray;
        argumentsArray.push_back(_v_ident);

        auto forOfStat = nf.createForOfStatement(
            undefined, declList, _src_array_ident,
            nf.createExpressionStatement(nf.createCallExpression(pushExpr, undefined, argumentsArray)));

        LLVM_DEBUG(printDebug(forOfStat););

        return mlirGen(forOfStat, genContext);
    }

    enum class TypeData
    {
        NotSet,
        Array,
        Tuple
    };

    struct RecevierContext
    {
        RecevierContext() : receiverTupleTypeIndex{-1} {}

        void set(mlir_ts::ArrayType arrayType)
        {
            receiverElementType = arrayType.getElementType();
        }

        void set(mlir_ts::TupleType tupleType)
        {
            receiverTupleType = tupleType;
        }

        void setReceiverTo(GenContext &noReceiverGenContext)
        {        
            noReceiverGenContext.receiverType = (receiverElementType) ? receiverElementType : mlir::Type();
        }

        mlir::Type isCastNeeded(mlir::Type type)
        {
            return receiverElementType && type != receiverElementType 
                ? receiverElementType 
                : mlir::Type();
        }

        void nextTupleField()
        {
            if (!receiverTupleType)
            {
                return;
            }

            receiverElementType =
                receiverTupleType.size() > ++receiverTupleTypeIndex
                ? receiverTupleType.getFieldInfo(receiverTupleTypeIndex).type 
                : mlir::Type();
        }

        mlir::Type receiverElementType;
        mlir_ts::TupleType receiverTupleType;
        int receiverTupleTypeIndex;
    };

    struct ArrayInfo
    {
        ArrayInfo() : recevierContext(), 
            dataType{TypeData::NotSet},
            isConst{true},
            anySpreadElement{false},
            applyCast{false}
        {
        }

        void set(mlir_ts::ArrayType arrayType)
        {
            dataType = TypeData::Array;
            arrayElementType =
                accumulatedArrayElementType = 
                    arrayType.getElementType();
        }        

        void setReceiver(mlir_ts::ArrayType arrayType)
        {        
            set(arrayType);
            recevierContext.set(arrayType);

            LLVM_DEBUG(llvm::dbgs() << "\n!! array elements - receiver type: " << recevierContext.receiverElementType << "\n";);
        }

        void set(mlir_ts::TupleType tupleType)
        {
            dataType = TypeData::Tuple;
            arrayElementType = tupleType;
        }

        void setReceiver(mlir_ts::TupleType tupleType)
        {  
            set(tupleType);
            recevierContext.set(tupleType);
        }        

        void setReceiver(mlir::Type type, bool isGenericType)
        {
            TypeSwitch<mlir::Type>(type)
                .template Case<mlir_ts::ArrayType>([&](auto a) { isGenericType ? set(a) : setReceiver(a); })
                .template Case<mlir_ts::TupleType>([&](auto t) { isGenericType ? set(t) : setReceiver(t); })
                .Default([&](auto type) {});
        }        

        void adjustArrayType(mlir::Type defaultElementType)
        {
            // post processing values
            if (anySpreadElement || dataType == TypeData::NotSet)
            {
                // this is array
                dataType = TypeData::Array;
            }

            if (dataType == TypeData::Tuple 
                && (recevierContext.receiverTupleType == mlir::Type()) 
                && !accumulatedArrayElementType.isa<mlir_ts::UnionType>())
            {
                // seems we can convert tuple into array, for example [1.0, 2, 3] -> [1.0, 2.0, 3.0]
                dataType = TypeData::Array;
                applyCast = true;
            }

            if (dataType == TypeData::Array)
            {
                arrayElementType = 
                    accumulatedArrayElementType 
                        ? accumulatedArrayElementType 
                        : defaultElementType;

                if (recevierContext.receiverElementType && recevierContext.receiverElementType != arrayElementType)
                {
                    arrayElementType = recevierContext.receiverElementType;
                    applyCast = true;
                }
            }
        }

        RecevierContext recevierContext;

        TypeData dataType;
        mlir::Type accumulatedArrayElementType;
        mlir::Type arrayElementType;
        bool isConst;
        bool anySpreadElement;
        bool applyCast;
    };

    struct ArrayElement
    {
    public:
        mlir::Value value;
        bool isSpread;
        bool isVariableSizeOfSpreadElement;
    };

    mlir::LogicalResult accumulateArrayItemType(mlir::Type type, struct ArrayInfo &arrayInfo) 
    {
        auto elementType = arrayInfo.accumulatedArrayElementType;

        // TODO: special case (should we use [] = as const_array<undefined, 0> instead of const_array<any, 0>?)
        if (auto constArray = type.dyn_cast<mlir_ts::ConstArrayType>())
        {
            if (constArray.getSize() == 0)
            {
                return mlir::success();
            }
        }

        // if we have receiver type we do not need to "adopt it"
        auto wideType = arrayInfo.recevierContext.receiverElementType ? type : mth.wideStorageType(type);

        LLVM_DEBUG(llvm::dbgs() << "\n!! element type: " << wideType << " original type: " << type << "\n";);

        elementType = elementType ? elementType : wideType;
        if (elementType != wideType)
        {
            if (arrayInfo.dataType == TypeData::NotSet)
            {
                // presumably it is tuple
                arrayInfo.dataType = TypeData::Tuple;
            }
            
            auto merged = false;
            elementType = mth.mergeType(elementType, wideType, merged);
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! result element type: " << elementType << "\n";);

        arrayInfo.accumulatedArrayElementType = elementType;

        return mlir::success();
    };

    mlir::LogicalResult processArrayValuesSpreadElement(mlir::Value itemValue, SmallVector<ArrayElement> &values, struct ArrayInfo &arrayInfo, const GenContext &genContext)
    {
        arrayInfo.anySpreadElement = true;
        arrayInfo.isConst = false;

        auto location = itemValue.getLoc();
        auto type = itemValue.getType();

        LLVM_DEBUG(llvm::dbgs() << "\n!! SpreadElement, src type: " << type << "\n";);

        if (auto constArray = type.dyn_cast<mlir_ts::ConstArrayType>())
        {
            auto constantOp = itemValue.getDefiningOp<mlir_ts::ConstantOp>();
            auto arrayAttr = constantOp.getValue().cast<mlir::ArrayAttr>();
            auto index = 0;
            // TODO: improve it with using array concat
            for (auto val : arrayAttr)
            {
                auto indexVal = builder.create<mlir_ts::ConstantOp>(itemValue.getLoc(), builder.getIntegerType(32), builder.getI32IntegerAttr(index++));
                auto result = mlirGenElementAccess(location, itemValue, indexVal, false, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result);
                auto newConstVal = V(result);
                values.push_back({newConstVal, false, false});
            }

            accumulateArrayItemType(constArray.getElementType(), arrayInfo);

            return mlir::success();
        }
        
        if (auto array = type.dyn_cast<mlir_ts::ArrayType>())
        {
            // TODO: implement method to concat array with const-length array in one operation without using 'push' for each element
            values.push_back({itemValue, true, true});

            auto arrayElementType = mth.wideStorageType(array.getElementType());
            accumulateArrayItemType(arrayElementType, arrayInfo);

            return mlir::success();
        }

        auto nextPropertyType = evaluateProperty(itemValue, ITERATOR_NEXT, genContext);
        if (nextPropertyType)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! SpreadElement, next type is: " << nextPropertyType << "\n";);

            auto returnType = mth.getReturnTypeFromFuncRef(nextPropertyType);
            if (returnType)
            {
                // as tuple or const_tuple
                ::llvm::ArrayRef<mlir_ts::FieldInfo> fields;
                TypeSwitch<mlir::Type>(returnType)
                    .template Case<mlir_ts::TupleType>([&](auto tupleType) { fields = tupleType.getFields(); })
                    .template Case<mlir_ts::ConstTupleType>(
                        [&](auto constTupleType) { fields = constTupleType.getFields(); })
                    .Default([&](auto type) { llvm_unreachable("not implemented"); });

                auto propValue = mlir::StringAttr::get(builder.getContext(), "value");
                if (std::any_of(fields.begin(), fields.end(), [&] (auto field) { return field.id == propValue; }))
                {
                    arrayInfo.isConst = false;

                    values.push_back({itemValue, true, true});

                    auto arrayElementType = mth.wideStorageType(fields.front().type);
                    accumulateArrayItemType(arrayElementType, arrayInfo);
                }
                else
                {
                    llvm_unreachable("not implemented");
                }

                return mlir::success();    
            }
        }                                        

        // DO NOT PUT before xxx.next() property otherwise ""..."" for Iterator will not work
        if (auto constTuple = type.dyn_cast<mlir_ts::ConstTupleType>())
        {
            // because it is tuple it may not have the same types
            arrayInfo.isConst = false;

            if (auto constantOp = itemValue.getDefiningOp<mlir_ts::ConstantOp>())
            {
                auto arrayAttr = constantOp.getValue().cast<mlir::ArrayAttr>();
                auto index = -1;
                for (auto val : arrayAttr)
                {
                    MLIRPropertyAccessCodeLogic cl(builder, location, itemValue, builder.getIndexAttr(++index));
                    auto newConstVal = cl.Tuple(constTuple, true);

                    values.push_back({newConstVal, false, false});

                    accumulateArrayItemType(constTuple.getFieldInfo(index).type, arrayInfo);
                }
            }

            return mlir::success();                
        }       
        
        if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
        {
            values.push_back({itemValue, true, false});
            for (auto tupleItem : tupleType)
            {
                accumulateArrayItemType(tupleItem.type, arrayInfo);
            }

            return mlir::success();
        }                           

        LLVM_DEBUG(llvm::dbgs() << "\n!! spread element type: " << type << "\n";);

        return mlir::success();
    }
    
    mlir::LogicalResult processArrayElementForValues(Expression item, SmallVector<ArrayElement> &values, struct ArrayInfo &arrayInfo, const GenContext &genContext)
    {
        auto location = loc(item);

        auto &recevierContext = arrayInfo.recevierContext;

        recevierContext.nextTupleField();

        GenContext noReceiverGenContext(genContext);
        noReceiverGenContext.clearReceiverTypes();
        recevierContext.setReceiverTo(noReceiverGenContext);

        auto result = mlirGen(item, noReceiverGenContext);
        EXIT_IF_FAILED_OR_NO_VALUE(result)
        auto itemValue = V(result);
        if (itemValue.getDefiningOp<mlir_ts::UndefOp>())
        {
            // process ommited expression
            if (auto optionalType = recevierContext.receiverElementType.dyn_cast_or_null<mlir_ts::OptionalType>())
            {
                itemValue = builder.create<mlir_ts::OptionalUndefOp>(location, recevierContext.receiverElementType);
            }
        }

        auto type = itemValue.getType();

        if (item == SyntaxKind::SpreadElement)
        {
            if (mlir::failed(processArrayValuesSpreadElement(itemValue, values, arrayInfo, genContext)))
            {
                return mlir::failure();
            }
        }
        else
        {
            if (auto castType = recevierContext.isCastNeeded(type))
            {
                CAST(itemValue, location, castType, itemValue, genContext);
                type = itemValue.getType();
            }

            if (!itemValue.getDefiningOp<mlir_ts::ConstantOp>() || 
            // TODO: in case of [{ a: '', b: 0, c: '' }, { a: "", b: 3, c: 0 }]
                ((arrayInfo.dataType == TypeData::Array || arrayInfo.dataType == TypeData::NotSet)
                    && itemValue.getType().isa<mlir_ts::ConstTupleType>()                 
                    && arrayInfo.accumulatedArrayElementType 
                    && mth.removeConstType(itemValue.getType()) != arrayInfo.accumulatedArrayElementType))
            {
                arrayInfo.isConst = false;
            }                

            values.push_back({itemValue, false, false});
            accumulateArrayItemType(type, arrayInfo);
        }

        return mlir::success();
    }

    mlir::LogicalResult processTupleTailingOptionalValues(mlir::Location location, int processedValues, SmallVector<ArrayElement> &values, struct ArrayInfo &arrayInfo, const GenContext &genContext)
    {
        if (!arrayInfo.recevierContext.receiverTupleType)
        {
            return mlir::success();
        }

        if (processedValues >= arrayInfo.recevierContext.receiverTupleType.getFields().size())
        {
            return mlir::success();
        }

        auto &recevierContext = arrayInfo.recevierContext;
        for (auto i = processedValues; i < arrayInfo.recevierContext.receiverTupleType.getFields().size(); i++)
        {
            recevierContext.nextTupleField();
            if (!recevierContext.receiverElementType.isa<mlir_ts::OptionalType>())
            {
                emitError(location, "value is not provided for non-optional type");
                return mlir::failure();
            }

            auto undefVal = builder.create<mlir_ts::OptionalUndefOp>(location, recevierContext.receiverElementType);
            values.push_back({undefVal, false, false});
        }

        return mlir::success();
    }    

    mlir::LogicalResult processArrayValues(NodeArray<Expression> arrayElements, SmallVector<ArrayElement> &values, struct ArrayInfo &arrayInfo, const GenContext &genContext)
    {
        // check receiverType
        if (genContext.receiverType)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! array/tuple - receiver type: " << genContext.receiverType << "\n";);
            // TODO: isGenericType is applied as hack here, find out the issue
            arrayInfo.setReceiver(genContext.receiverType, mth.isGenericType(genContext.receiverType));
        }

        for (auto &item : arrayElements)
        {
            if (mlir::failed(processArrayElementForValues(item, values, arrayInfo, genContext)))
            {
                return mlir::failure();
            }
        }

        if (mlir::failed(processTupleTailingOptionalValues(loc(arrayElements), arrayElements.size(), values, arrayInfo, genContext)))
        {
            return mlir::failure();
        }

        arrayInfo.adjustArrayType(getAnyType());

        return mlir::success();
    }

    ValueOrLogicalResult createConstArrayOrTuple(mlir::Location location, ArrayRef<ArrayElement> values, struct ArrayInfo arrayInfo, const GenContext &genContext)
    {
        // collect const values as attributes
        SmallVector<mlir::Attribute> constValues;
        for (auto &itemValue : values)
        {
            auto constOp = itemValue.value.getDefiningOp<mlir_ts::ConstantOp>();
            if (arrayInfo.applyCast)
            {
                constValues.push_back(mth.convertAttrIntoType(constOp.getValueAttr(), arrayInfo.arrayElementType, builder)); 
            }
            else
            {
                constValues.push_back(constOp.getValueAttr()); 
            }
        }

        SmallVector<mlir::Type> constTypes;
        for (auto &itemValue : values)
        {
            auto type = mth.wideStorageType(itemValue.value.getType());
            constTypes.push_back(type);
        }

        auto arrayAttr = mlir::ArrayAttr::get(builder.getContext(), constValues);
        if (arrayInfo.dataType == TypeData::Tuple)
        {
            SmallVector<mlir_ts::FieldInfo> fieldInfos;
            for (auto type : constTypes)
            {
                fieldInfos.push_back({mlir::Attribute(), type});
            }

            return V(
                builder.create<mlir_ts::ConstantOp>(location, getConstTupleType(fieldInfos), arrayAttr));
        }

        if (arrayInfo.dataType == TypeData::Array)
        {
            auto arrayElementType = arrayInfo.arrayElementType ? arrayInfo.arrayElementType : getAnyType();

            return V(builder.create<mlir_ts::ConstantOp>(
                location, getConstArrayType(arrayElementType, constValues.size()), arrayAttr));
        }

        llvm_unreachable("not implemented");
    }

    ValueOrLogicalResult createTupleFromArrayLiteral(mlir::Location location, ArrayRef<ArrayElement> values, struct ArrayInfo arrayInfo, const GenContext &genContext)
    {
        SmallVector<mlir::Value> arrayValues;
        SmallVector<mlir_ts::FieldInfo> fieldInfos;
        for (auto val : values)
        {
            fieldInfos.push_back({mlir::Attribute(), val.value.getType()});
            arrayValues.push_back(val.value);
        }

        return V(builder.create<mlir_ts::CreateTupleOp>(location, getTupleType(fieldInfos), arrayValues));
    }    

    ValueOrLogicalResult createFixedSizeArrayFromArrayLiteral(mlir::Location location, ArrayRef<ArrayElement> values, struct ArrayInfo arrayInfo, const GenContext &genContext)
    {
        SmallVector<mlir::Value> arrayValues;
        for (auto val : values)
        {
            auto arrayValue = val.value;
            if (arrayInfo.applyCast)
            {
                CAST(arrayValue, location, arrayInfo.arrayElementType, val.value, genContext)
            }

            arrayValues.push_back(arrayValue);
        }

        auto newArrayOp =
            builder.create<mlir_ts::CreateArrayOp>(location, getArrayType(arrayInfo.arrayElementType), arrayValues);
        return V(newArrayOp);
    }

    ValueOrLogicalResult createDynamicArrayFromArrayLiteral(mlir::Location location, ArrayRef<ArrayElement> values, struct ArrayInfo arrayInfo, const GenContext &genContext)
    {
        MLIRCustomMethods cm(builder, location, compileOptions);
        SmallVector<mlir::Value> emptyArrayValues;
        auto arrType = getArrayType(arrayInfo.arrayElementType);
        auto newArrayOp = builder.create<mlir_ts::CreateArrayOp>(location, arrType, emptyArrayValues);
        auto varArray = builder.create<mlir_ts::VariableOp>(location, mlir_ts::RefType::get(arrType),
                                                            newArrayOp, builder.getBoolAttr(false));

        auto loadedVarArray = builder.create<mlir_ts::LoadOp>(location, arrType, varArray);

        // TODO: push every element into array
        for (auto val : values)
        {
            if (val.isVariableSizeOfSpreadElement)
            {
                mlirGenAppendArrayByEachElement(location, varArray, val.value, genContext);
            }
            else
            {
                SmallVector<mlir::Value> vals;
                if (!val.isSpread)
                {
                    mlir::Value finalVal = val.value;
                    if (arrayInfo.arrayElementType != val.value.getType())
                    {
                        auto result = cast(location, arrayInfo.arrayElementType, val.value, genContext) ;
                        EXIT_IF_FAILED_OR_NO_VALUE(result)
                        finalVal = V(result);
                    }
                    else
                    {
                        finalVal = val.value;
                    }

                    vals.push_back(finalVal);
                }
                // to process const tuple & tuple
                else if (auto tupleType = mth.convertConstTupleTypeToTupleType(val.value.getType()).dyn_cast<mlir_ts::TupleType>())
                {
                    llvm::SmallVector<mlir::Type> destTupleTypes;
                    if (mlir::succeeded(mth.getFieldTypes(tupleType, destTupleTypes)))
                    {
                        auto resValues = builder.create<mlir_ts::DeconstructTupleOp>(location, destTupleTypes, val.value);
                        for (auto tupleVal : resValues.getResults())
                        {
                            mlir::Value finalVal;
                            if (arrayInfo.arrayElementType != tupleVal.getType())
                            {
                                auto result = cast(location, arrayInfo.arrayElementType, tupleVal, genContext) ;
                                EXIT_IF_FAILED_OR_NO_VALUE(result)
                                finalVal = V(result);
                            }
                            else
                            {
                                finalVal = tupleVal;
                            }

                            vals.push_back(finalVal);
                        }
                    }
                    else
                    {
                        return mlir::failure();
                    }
                }
                else
                {
                    LLVM_DEBUG(llvm::dbgs() << "\n!! array spread value type: " << val.value.getType() << "\n";);
                    llvm_unreachable("not implemented");
                }
                
                assert(vals.size() > 0);

                cm.mlirGenArrayPush(location, loadedVarArray, vals);
            }
        }

        auto loadedVarArray2 = builder.create<mlir_ts::LoadOp>(location, arrType, varArray);
        return V(loadedVarArray2);
    }

    ValueOrLogicalResult createArrayFromArrayInfo(mlir::Location location, ArrayRef<ArrayElement> values, struct ArrayInfo arrayInfo, const GenContext &genContext)
    {
        if (arrayInfo.isConst)
        {
            return createConstArrayOrTuple(location, values, arrayInfo, genContext);
        }

        if (arrayInfo.dataType == TypeData::Tuple)
        {
            return createTupleFromArrayLiteral(location, values, arrayInfo, genContext);
        }

        if (!arrayInfo.anySpreadElement)
        {
            return createFixedSizeArrayFromArrayLiteral(location, values, arrayInfo, genContext);
        }

        return createDynamicArrayFromArrayLiteral(location, values, arrayInfo, genContext);
    }

    ValueOrLogicalResult mlirGen(ts::ArrayLiteralExpression arrayLiteral, const GenContext &genContext)
    {
        auto location = loc(arrayLiteral);

        SmallVector<ArrayElement> values;
        struct ArrayInfo arrayInfo{};
        if (mlir::failed(processArrayValues(arrayLiteral->elements, values, arrayInfo, genContext)))
        {
            return mlir::failure();
        }

        return createArrayFromArrayInfo(location, values, arrayInfo, genContext);
    }

    // TODO: replace usage of this method with getFields method
    mlir::Type getTypeByFieldNameFromReceiverType(mlir::Attribute fieldName, mlir::Type receiverType)
    {
        if (auto tupleType = receiverType.dyn_cast<mlir_ts::TupleType>())
        {
            auto index = tupleType.getIndex(fieldName);
            if (index >= 0)
            {
                return tupleType.getType(index);
            }
        }

        if (auto constTupleType = receiverType.dyn_cast<mlir_ts::ConstTupleType>())
        {
            auto index = constTupleType.getIndex(fieldName);
            if (index >= 0)
            {
                return constTupleType.getType(index);
            }
        }

        if (auto interfaceType = receiverType.dyn_cast<mlir_ts::InterfaceType>())
        {
            auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
            auto index = interfaceInfo->getFieldIndex(fieldName);
            if (index >= 0)
            {
                return interfaceInfo->fields[index].type;
            }
        }        

        return mlir::Type();
    }

    ValueOrLogicalResult mlirGen(ts::ObjectLiteralExpression objectLiteral, const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);

        // first value
        SmallVector<mlir_ts::FieldInfo> fieldInfos;
        SmallVector<mlir::Attribute> values;
        SmallVector<size_t> methodInfos;
        SmallVector<std::pair<std::string, size_t>> methodInfosWithCaptures;
        SmallVector<std::pair<mlir::Attribute, mlir::Value>> fieldsToSet;

        mlir::Type receiverType = genContext.receiverType;

        auto location = loc(objectLiteral);

        if (receiverType && objectLiteral->properties.size() == 0)
        {
            // return undef tuple
            llvm::SmallVector<mlir_ts::FieldInfo> destTupleFields;
            if (mlir::succeeded(mth.getFields(receiverType, destTupleFields)))
            {
                auto tupleType = getTupleType(destTupleFields);
                return V(builder.create<mlir_ts::UndefOp>(location, tupleType));
            }
        }

        // Object This Type
        auto name = MLIRHelper::getAnonymousName(loc_check(objectLiteral), ".obj");
        auto objectNameSymbol = mlir::FlatSymbolRefAttr::get(builder.getContext(), name);
        auto objectStorageType = getObjectStorageType(objectNameSymbol);
        auto objThis = getObjectType(objectStorageType);
        
        auto addFuncFieldInfo = [&](mlir::Attribute fieldId, const std::string &funcName,
                                    mlir_ts::FunctionType funcType) {
            auto type = funcType;

            values.push_back(mlir::FlatSymbolRefAttr::get(builder.getContext(), funcName));
            fieldInfos.push_back({fieldId, type});

            if (getCaptureVarsMap().find(funcName) != getCaptureVarsMap().end())
            {
                methodInfosWithCaptures.push_back({funcName, fieldInfos.size() - 1});
            }
            else
            {
                methodInfos.push_back(fieldInfos.size() - 1);
            }
        };

        auto addFieldInfoToArrays = [&](mlir::Attribute fieldId, mlir::Type type) {
            values.push_back(builder.getUnitAttr());
            fieldInfos.push_back({fieldId, type});
        };

        auto addFieldInfo = [&](mlir::Attribute fieldId, mlir::Value itemValue, mlir::Type receiverElementType) {
            mlir::Type type;
            mlir::Attribute value;
            auto isConstValue = true;
            if (auto constOp = itemValue.getDefiningOp<mlir_ts::ConstantOp>())
            {
                value = constOp.getValueAttr();
                type = constOp.getType();
            }
            else if (auto symRefOp = itemValue.getDefiningOp<mlir_ts::SymbolRefOp>())
            {
                value = symRefOp.getIdentifierAttr();
                type = symRefOp.getType();
            }
            else if (auto undefOp = itemValue.getDefiningOp<mlir_ts::UndefOp>())
            {
                value = builder.getUnitAttr();
                type = undefOp.getType();
            }
            else
            {
                value = builder.getUnitAttr();
                type = itemValue.getType();
                isConstValue = false;
            }

            type = mth.wideStorageType(type);

            if (receiverElementType)
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! Object field type and receiver type: " << type << " type: " << receiverElementType << "\n";);

                if (type != receiverElementType)
                {
                    value = builder.getUnitAttr();
                    itemValue = cast(location, receiverElementType, itemValue, genContext);
                    isConstValue = false;
                }

                type = receiverElementType;
                LLVM_DEBUG(llvm::dbgs() << "\n!! Object field type (from receiver) - id: " << fieldId << " type: " << type << "\n";);
            }

            values.push_back(value);
            fieldInfos.push_back({fieldId, type});
            if (!isConstValue)
            {
                fieldsToSet.push_back({fieldId, itemValue});
            }
        };

        auto processFunctionLikeProto = [&](mlir::Attribute fieldId, FunctionLikeDeclarationBase &funcLikeDecl) {
            auto funcGenContext = GenContext(genContext);
            funcGenContext.clearScopeVars();
            funcGenContext.clearReceiverTypes();
            funcGenContext.thisType = objThis;

            funcLikeDecl->parent = objectLiteral;

            auto [funcOp, funcProto, result, isGeneric] = mlirGenFunctionPrototype(funcLikeDecl, funcGenContext);
            if (mlir::failed(result) || !funcOp)
            {
                return;
            }

            // fix this parameter type (taking in account that first type can be captured type)
            auto funcName = funcOp.getName().str();
            auto funcType = funcOp.getFunctionType();

            // process local vars in this context
            if (funcProto->getHasExtraFields())
            {
                // note: this code needed to store local variables for generators
                auto localVars = getLocalVarsInThisContextMap().find(funcName);
                if (localVars != getLocalVarsInThisContextMap().end())
                {
                    for (auto fieldInfo : localVars->getValue())
                    {
                        addFieldInfoToArrays(fieldInfo.id, fieldInfo.type);
                    }
                }
            }

            addFuncFieldInfo(fieldId, funcName, funcType);
        };

        auto processFunctionLike = [&](mlir_ts::ObjectType objThis, FunctionLikeDeclarationBase &funcLikeDecl) {
            auto funcGenContext = GenContext(genContext);
            funcGenContext.clearScopeVars();
            funcGenContext.clearReceiverTypes();
            funcGenContext.thisType = objThis;

            LLVM_DEBUG(llvm::dbgs() << "\n!! Object Process function with this type: " << objThis << "\n";);

            funcLikeDecl->parent = objectLiteral;

            mlir::OpBuilder::InsertionGuard guard(builder);
            mlirGenFunctionLikeDeclaration(funcLikeDecl, funcGenContext);
        };

        // add all fields
        for (auto &item : objectLiteral->properties)
        {
            mlir::Value itemValue;
            mlir::Attribute fieldId;
            mlir::Type receiverElementType;
            if (item == SyntaxKind::PropertyAssignment)
            {
                auto propertyAssignment = item.as<PropertyAssignment>();
                if (propertyAssignment->initializer == SyntaxKind::FunctionExpression ||
                    propertyAssignment->initializer == SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                fieldId = TupleFieldName(propertyAssignment->name, genContext);

                if (receiverType)
                {
                    receiverElementType = getTypeByFieldNameFromReceiverType(fieldId, receiverType);
                }

                // TODO: send context with receiver type
                GenContext receiverTypeGenContext(genContext);
                receiverTypeGenContext.clearReceiverTypes();
                if (receiverElementType)
                {
                    receiverTypeGenContext.receiverType = receiverElementType;
                }

                auto result = mlirGen(propertyAssignment->initializer, receiverTypeGenContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                itemValue = V(result);

                // in case of Union type
                if (receiverType && !receiverElementType)
                {
                    if (auto unionType = receiverType.dyn_cast<mlir_ts::UnionType>())
                    {
                        for (auto subType : unionType.getTypes())
                        {
                            auto possibleType = getTypeByFieldNameFromReceiverType(fieldId, subType);
                            if (possibleType == itemValue.getType())
                            {
                                LLVM_DEBUG(llvm::dbgs() << "\n!! we picked type from union: " << subType << "\n";);

                                receiverElementType = possibleType;
                                receiverType = subType;
                                break;
                            }
                        }
                    }
                }
            }
            else if (item == SyntaxKind::ShorthandPropertyAssignment)
            {
                auto shorthandPropertyAssignment = item.as<ShorthandPropertyAssignment>();
                if (shorthandPropertyAssignment->initializer == SyntaxKind::FunctionExpression ||
                    shorthandPropertyAssignment->initializer == SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                auto result = mlirGen(shorthandPropertyAssignment->name.as<Expression>(), genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                itemValue = V(result);

                fieldId = TupleFieldName(shorthandPropertyAssignment->name, genContext);
            }
            else if (item == SyntaxKind::MethodDeclaration)
            {
                continue;
            }
            else if (item == SyntaxKind::SpreadAssignment)
            {
                auto spreadAssignment = item.as<SpreadAssignment>();
                auto result = mlirGen(spreadAssignment->expression, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto tupleValue = V(result);

                LLVM_DEBUG(llvm::dbgs() << "\n!! SpreadAssignment value: " << tupleValue << "\n";);

                auto tupleFields = [&] (::llvm::ArrayRef<mlir_ts::FieldInfo> fields) {
                    SmallVector<mlir::Type> types;
                    for (auto &field : fields)
                    {
                        types.push_back(field.type);
                    }

                    // deconstruct tuple
                    auto res = builder.create<mlir_ts::DeconstructTupleOp>(loc(spreadAssignment), types, tupleValue);

                    // read all fields
                    for (auto pair : llvm::zip(fields, res.getResults()))
                    {
                        addFieldInfo(
                            std::get<0>(pair).id, 
                            std::get<1>(pair), 
                            receiverType 
                                ? getTypeByFieldNameFromReceiverType(std::get<0>(pair).id, receiverType) 
                                : mlir::Type());
                    }
                };

                TypeSwitch<mlir::Type>(tupleValue.getType())
                    .template Case<mlir_ts::TupleType>([&](auto tupleType) { tupleFields(tupleType.getFields()); })
                    .template Case<mlir_ts::ConstTupleType>(
                        [&](auto constTupleType) { tupleFields(constTupleType.getFields()); })
                    .template Case<mlir_ts::InterfaceType>(
                        [&](auto interfaceType) { 
                            mlir::SmallVector<mlir_ts::FieldInfo> destFields;
                            if (mlir::succeeded(mth.getFields(interfaceType, destFields)))
                            {
                                if (auto srcInterfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue()))
                                {
                                    for (auto fieldInfo : destFields)
                                    {
                                        auto totalOffset = 0;
                                        auto interfaceFieldInfo = srcInterfaceInfo->findField(fieldInfo.id, totalOffset);

                                        MLIRPropertyAccessCodeLogic cl(builder, location, tupleValue, fieldInfo.id);
                                        // TODO: implemenet conditional
                                        mlir::Value propertyAccess = mlirGenPropertyAccessExpressionLogic(location, tupleValue, interfaceFieldInfo->isConditional, cl, genContext); 
                                        addFieldInfo(fieldInfo.id, propertyAccess, receiverElementType);
                                    }
                                }
                            }
                        })
                    .template Case<mlir_ts::ClassType>(
                        [&](auto classType) { 
                            mlir::SmallVector<mlir_ts::FieldInfo> destFields;
                            if (mlir::succeeded(mth.getFields(classType, destFields)))
                            {
                                if (auto srcClassInfo = getClassInfoByFullName(classType.getName().getValue()))
                                {
                                    for (auto fieldInfo : destFields)
                                    {
                                        auto foundField = false;                                        
                                        auto classFieldInfo = srcClassInfo->findField(fieldInfo.id, foundField);

                                        MLIRPropertyAccessCodeLogic cl(builder, location, tupleValue, fieldInfo.id);
                                        // TODO: implemenet conditional
                                        mlir::Value propertyAccess = mlirGenPropertyAccessExpressionLogic(location, tupleValue, false, cl, genContext); 
                                        addFieldInfo(fieldInfo.id, propertyAccess, receiverElementType);
                                    }
                                }
                            }
                        })                        
                    .Default([&](auto type) { 
                        LLVM_DEBUG(llvm::dbgs() << "\n!! SpreadAssignment not implemented for type: " << type << "\n";);
                        llvm_unreachable("not implemented"); 
                    });

                continue;
            }
            else
            {
                llvm_unreachable("object literal is not implemented(1)");
            }

            assert(genContext.allowPartialResolve || itemValue);

            addFieldInfo(fieldId, itemValue, receiverElementType);
        }

        // update after processing all fields
        objectStorageType.setFields(fieldInfos);

        // process all methods
        for (auto &item : objectLiteral->properties)
        {
            mlir::Attribute fieldId;
            if (item == SyntaxKind::PropertyAssignment)
            {
                auto propertyAssignment = item.as<PropertyAssignment>();
                if (propertyAssignment->initializer != SyntaxKind::FunctionExpression &&
                    propertyAssignment->initializer != SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                auto funcLikeDecl = propertyAssignment->initializer.as<FunctionLikeDeclarationBase>();
                fieldId = TupleFieldName(propertyAssignment->name, genContext);
                processFunctionLikeProto(fieldId, funcLikeDecl);
            }
            else if (item == SyntaxKind::ShorthandPropertyAssignment)
            {
                auto shorthandPropertyAssignment = item.as<ShorthandPropertyAssignment>();
                if (shorthandPropertyAssignment->initializer != SyntaxKind::FunctionExpression &&
                    shorthandPropertyAssignment->initializer != SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                auto funcLikeDecl = shorthandPropertyAssignment->initializer.as<FunctionLikeDeclarationBase>();
                fieldId = TupleFieldName(shorthandPropertyAssignment->name, genContext);
                processFunctionLikeProto(fieldId, funcLikeDecl);
            }
            else if (item == SyntaxKind::MethodDeclaration)
            {
                auto funcLikeDecl = item.as<FunctionLikeDeclarationBase>();
                fieldId = TupleFieldName(funcLikeDecl->name, genContext);
                processFunctionLikeProto(fieldId, funcLikeDecl);
            }
        }

        // create accum. captures
        llvm::StringMap<ts::VariableDeclarationDOM::TypePtr> accumulatedCaptureVars;

        for (auto &methodRefWithName : methodInfosWithCaptures)
        {
            auto funcName = std::get<0>(methodRefWithName);
            auto methodRef = std::get<1>(methodRefWithName);
            auto &methodInfo = fieldInfos[methodRef];

            if (auto funcType = methodInfo.type.dyn_cast<mlir_ts::FunctionType>())
            {
                auto captureVars = getCaptureVarsMap().find(funcName);
                if (captureVars != getCaptureVarsMap().end())
                {
                    // mlirGenResolveCapturedVars
                    for (auto &captureVar : captureVars->getValue())
                    {
                        if (accumulatedCaptureVars.count(captureVar.getKey()) > 0)
                        {
                            assert(accumulatedCaptureVars[captureVar.getKey()] == captureVar.getValue());
                        }

                        accumulatedCaptureVars[captureVar.getKey()] = captureVar.getValue();
                    }
                }
                else
                {
                    assert(false);
                }
            }
        }

        if (accumulatedCaptureVars.size() > 0)
        {
            // add all captured
            SmallVector<mlir::Value> accumulatedCapturedValues;
            if (mlir::failed(mlirGenResolveCapturedVars(location, accumulatedCaptureVars, accumulatedCapturedValues,
                                                        genContext)))
            {
                return mlir::failure();
            }

            auto capturedValue = mlirGenCreateCapture(location, mcl.CaptureType(accumulatedCaptureVars),
                                                      accumulatedCapturedValues, genContext);
            addFieldInfo(MLIRHelper::TupleFieldName(CAPTURED_NAME, builder.getContext()), capturedValue, mlir::Type());
        }

        // final type, update
        objectStorageType.setFields(fieldInfos);

        // process all methods
        for (auto &item : objectLiteral->properties)
        {
            if (item == SyntaxKind::PropertyAssignment)
            {
                auto propertyAssignment = item.as<PropertyAssignment>();
                if (propertyAssignment->initializer != SyntaxKind::FunctionExpression &&
                    propertyAssignment->initializer != SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                auto funcLikeDecl = propertyAssignment->initializer.as<FunctionLikeDeclarationBase>();
                processFunctionLike(objThis, funcLikeDecl);
            }
            else if (item == SyntaxKind::ShorthandPropertyAssignment)
            {
                auto shorthandPropertyAssignment = item.as<ShorthandPropertyAssignment>();
                if (shorthandPropertyAssignment->initializer != SyntaxKind::FunctionExpression &&
                    shorthandPropertyAssignment->initializer != SyntaxKind::ArrowFunction)
                {
                    continue;
                }

                auto funcLikeDecl = shorthandPropertyAssignment->initializer.as<FunctionLikeDeclarationBase>();
                processFunctionLike(objThis, funcLikeDecl);
            }
            else if (item == SyntaxKind::MethodDeclaration)
            {
                auto funcLikeDecl = item.as<FunctionLikeDeclarationBase>();
                processFunctionLike(objThis, funcLikeDecl);
            }
        }

        auto constTupleTypeWithReplacedThis = getConstTupleType(fieldInfos);

        auto arrayAttr = mlir::ArrayAttr::get(builder.getContext(), values);
        auto constantVal =
            builder.create<mlir_ts::ConstantOp>(location, constTupleTypeWithReplacedThis, arrayAttr);
        if (fieldsToSet.empty())
        {
            return V(constantVal);
        }

        auto tupleType = mth.convertConstTupleTypeToTupleType(constantVal.getType());
        return mlirGenCreateTuple(constantVal.getLoc(), tupleType, constantVal, fieldsToSet, genContext);
    }

    ValueOrLogicalResult mlirGenCreateTuple(mlir::Location location, mlir::Type tupleType, mlir::Value initValue,
                                            SmallVector<std::pair<mlir::Attribute, mlir::Value>> &fieldsToSet,
                                            const GenContext &genContext)
    {
        // we need to cast it to tuple and set values
        auto tupleVar = builder.create<mlir_ts::VariableOp>(location, mlir_ts::RefType::get(tupleType), initValue,
                                                            builder.getBoolAttr(false));
        for (auto fieldToSet : fieldsToSet)
        {
            VALIDATE(fieldToSet.first, location)
            VALIDATE(fieldToSet.second, location)

            auto location = fieldToSet.second.getLoc();
            auto result = mlirGenPropertyAccessExpression(location, tupleVar, fieldToSet.first, genContext);
            EXIT_IF_FAILED_OR_NO_VALUE(result)
            auto getField = V(result);

            auto result2 = mlirGenSaveLogicOneItem(location, getField, fieldToSet.second, genContext);
            EXIT_IF_FAILED(result2)
            auto savedValue = V(result2);
        }

        auto loadedValue = builder.create<mlir_ts::LoadOp>(location, tupleType, tupleVar);
        return V(loadedValue);
    }

    ValueOrLogicalResult mlirGen(Identifier identifier, const GenContext &genContext)
    {
        auto location = loc(identifier);

        // resolve name
        auto name = MLIRHelper::getName(identifier);

        // info: can't validate it here, in case of "print" etc
        return mlirGen(location, name, genContext);
    }

    mlir::Value resolveIdentifierAsVariable(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        if (name.empty())
        {
            return mlir::Value();
        }

        auto value = symbolTable.lookup(name);
        if (value.second && value.first)
        {
            LLVM_DEBUG(dbgs() << "\n!! resolveIdentifierAsVariable: " << name << " value: " << value.first;);

            // begin of logic: outer vars
            auto valueRegion = value.first.getParentRegion();
            auto isOuterVar = false;
            // TODO: review code "valueRegion && valueRegion->getParentOp()" is to support async.execute
            if (genContext.funcOp && genContext.funcOp != tempFuncOp && valueRegion &&
                valueRegion->getParentOp() /* && valueRegion->getParentOp()->getParentOp()*/)
            {
                // auto funcRegion = const_cast<GenContext &>(genContext).funcOp.getCallableRegion();
                auto funcRegion = const_cast<GenContext &>(genContext).funcOp.getCallableRegion();

                isOuterVar = !funcRegion->isAncestor(valueRegion);
                // TODO: HACK
                if (isOuterVar && value.second->getIgnoreCapturing())
                {
                    // special case when "ForceConstRef" pointering to outer variable but it is not outer var
                    isOuterVar = false;
                }
            }

            if (isOuterVar && genContext.passResult)
            {
                LLVM_DEBUG(dbgs() << "\n!! capturing var: [" << value.second->getName()
                                  << "] \n\tvalue pair: " << value.first << " \n\ttype: " << value.second->getType()
                                  << " \n\treadwrite: " << value.second->getReadWriteAccess() << "";);

                // valueRegion->viewGraph();
                // const_cast<GenContext &>(genContext).funcOpVarScope.getCallableRegion()->viewGraph();

                // special case, to prevent capturing ".a" because of reference to outer VaribleOp, which is hack (review
                // solution for it)
                genContext.passResult->outerVariables.insert({value.second->getName(), value.second});
            }

            // end of logic: outer vars

            if (!value.second->getReadWriteAccess())
            {
                return value.first;
            }

            LLVM_DEBUG(dbgs() << "\n!! variable: " << name << " type: " << value.first.getType() << "\n");

            // load value if memref
            auto valueType = value.first.getType().cast<mlir_ts::RefType>().getElementType();
            return builder.create<mlir_ts::LoadOp>(value.first.getLoc(), valueType, value.first);
        }

        return mlir::Value();
    }

    mlir::LogicalResult mlirGenResolveCapturedVars(mlir::Location location,
                                                   llvm::StringMap<ts::VariableDeclarationDOM::TypePtr> captureVars,
                                                   SmallVector<mlir::Value> &capturedValues,
                                                   const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);
        for (auto &item : captureVars)
        {
            auto result = mlirGen(location, item.first(), genContext);
            auto varValue = V(result);

            // review capturing by ref.  it should match storage type
            auto refValue = mcl.GetReferenceOfLoadOp(varValue);
            if (refValue)
            {
                capturedValues.push_back(refValue);
                // set var as captures
                if (auto varOp = refValue.getDefiningOp<mlir_ts::VariableOp>())
                {
                    varOp.setCapturedAttr(builder.getBoolAttr(true));
                }
                else if (auto paramOp = refValue.getDefiningOp<mlir_ts::ParamOp>())
                {
                    paramOp.setCapturedAttr(builder.getBoolAttr(true));
                }
                else if (auto paramOptOp = refValue.getDefiningOp<mlir_ts::ParamOptionalOp>())
                {
                    paramOptOp.setCapturedAttr(builder.getBoolAttr(true));
                }
                else
                {
                    // TODO: review it.
                    // find out if u need to ensure that data is captured and belong to VariableOp or ParamOp with
                    // captured = true
                    LLVM_DEBUG(llvm::dbgs()
                                   << "\n!! var must be captured when loaded from other Op: " << refValue << "\n";);
                    // llvm_unreachable("variable must be captured.");
                }
            }
            else
            {
                // this is not ref, this is const value
                capturedValues.push_back(varValue);
            }
        }

        return mlir::success();
    }

    ValueOrLogicalResult mlirGenCreateCapture(mlir::Location location, mlir::Type capturedType,
                                              SmallVector<mlir::Value> capturedValues, const GenContext &genContext)
    {
        LLVM_DEBUG(for (auto &val : capturedValues) llvm::dbgs() << "\n!! captured val: " << val << "\n";);
        LLVM_DEBUG(llvm::dbgs() << "\n!! captured type: " << capturedType << "\n";);

        // add attributes to track which one sent by ref.
        auto captured = builder.create<mlir_ts::CaptureOp>(location, capturedType, capturedValues);
        return V(captured);
    }

    mlir::Value resolveFunctionWithCapture(mlir::Location location, StringRef name, mlir_ts::FunctionType funcType,
                                           mlir::Value thisValue, bool addGenericAttrFlag,
                                           const GenContext &genContext)
    {
        // check if required capture of vars
        auto captureVars = getCaptureVarsMap().find(name);
        if (captureVars != getCaptureVarsMap().end())
        {
            auto funcSymbolOp = builder.create<mlir_ts::SymbolRefOp>(
                location, funcType, mlir::FlatSymbolRefAttr::get(builder.getContext(), name));
            if (addGenericAttrFlag)
            {
                funcSymbolOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! func with capture: first type: [ " << funcType.getInput(0)
                                    << " ], \n\tfunc name: " << name << " \n\tfunc type: " << funcType << "\n");

            SmallVector<mlir::Value> capturedValues;
            if (mlir::failed(mlirGenResolveCapturedVars(location, captureVars->getValue(), capturedValues, genContext)))
            {
                return mlir::Value();
            }

            MLIRCodeLogic mcl(builder);

            auto captureType = mcl.CaptureType(captureVars->getValue());
            auto result = mlirGenCreateCapture(location, captureType, capturedValues, genContext);
            auto captured = V(result);
            CAST_A(opaqueTypeValue, location, getOpaqueType(), captured, genContext);
            return builder.create<mlir_ts::CreateBoundFunctionOp>(location, getBoundFunctionType(funcType),
                                                                  opaqueTypeValue, funcSymbolOp);
        }

        if (thisValue)
        {
            auto thisFuncSymbolOp = builder.create<mlir_ts::ThisSymbolRefOp>(
                location, getBoundFunctionType(funcType), thisValue, mlir::FlatSymbolRefAttr::get(builder.getContext(), name));
            if (addGenericAttrFlag)
            {
                thisFuncSymbolOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
            }

            return V(thisFuncSymbolOp);
        }

        auto funcSymbolOp = builder.create<mlir_ts::SymbolRefOp>(
            location, funcType, mlir::FlatSymbolRefAttr::get(builder.getContext(), name));
        if (addGenericAttrFlag)
        {
            funcSymbolOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
        }

        return V(funcSymbolOp);
    }

    mlir::Value resolveFunctionNameInNamespace(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        // resolving function
        auto fn = getFunctionMap().find(name);
        if (fn != getFunctionMap().end())
        {
            auto funcOp = fn->getValue();
            auto funcType = funcOp.getFunctionType();
            auto funcName = funcOp.getName();

            return resolveFunctionWithCapture(location, funcName, funcType, mlir::Value(), false, genContext);
        }

        return mlir::Value();
    }

    mlir::Type resolveTypeByNameInNamespace(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        // support generic types
        if (genContext.typeParamsWithArgs.size() > 0)
        {
            auto type = getResolveTypeParameter(name, false, genContext);
            if (type)
            {
                return type;
            }
        }

        if (genContext.typeAliasMap.count(name))
        {
            auto typeAliasInfo = genContext.typeAliasMap.lookup(name);
            assert(typeAliasInfo);
            return typeAliasInfo;
        }

        if (getTypeAliasMap().count(name))
        {
            auto typeAliasInfo = getTypeAliasMap().lookup(name);
            assert(typeAliasInfo);
            return typeAliasInfo;
        }

        if (getClassesMap().count(name))
        {
            auto classInfo = getClassesMap().lookup(name);
            if (!classInfo->classType)
            {
                emitError(location) << "can't find class: " << name << "\n";
                return mlir::Type();
            }

            return classInfo->classType;
        }

        if (getGenericClassesMap().count(name))
        {
            auto genericClassInfo = getGenericClassesMap().lookup(name);

            return genericClassInfo->classType;
        }

        if (getInterfacesMap().count(name))
        {
            auto interfaceInfo = getInterfacesMap().lookup(name);
            if (!interfaceInfo->interfaceType)
            {
                emitError(location) << "can't find interface: " << name << "\n";
                return mlir::Type();
            }

            return interfaceInfo->interfaceType;
        }

        if (getGenericInterfacesMap().count(name))
        {
            auto genericInterfaceInfo = getGenericInterfacesMap().lookup(name);
            return genericInterfaceInfo->interfaceType;
        }

        // check if we have enum
        if (getEnumsMap().count(name))
        {
            auto enumTypeInfo = getEnumsMap().lookup(name);
            return getEnumType(enumTypeInfo.first, enumTypeInfo.second);
        }

        if (getImportEqualsMap().count(name))
        {
            auto fullName = getImportEqualsMap().lookup(name);
            auto classInfo = getClassInfoByFullName(fullName);
            if (classInfo)
            {
                return classInfo->classType;
            }

            auto interfaceInfo = getInterfaceInfoByFullName(fullName);
            if (interfaceInfo)
            {
                return interfaceInfo->interfaceType;
            }
        }        

        return mlir::Type();
    }

    mlir::Type resolveTypeByName(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        auto type = resolveTypeByNameInNamespace(location, name, genContext);
        if (type)
        {
            return type;
        }

        {
            MLIRNamespaceGuard ng(currentNamespace);

            // search in outer namespaces
            while (currentNamespace->isFunctionNamespace)
            {
                currentNamespace = currentNamespace->parentNamespace;
                type = resolveTypeByNameInNamespace(location, name, genContext);
                if (type)
                {
                    return type;
                }
            }

            // search in root namespace
            currentNamespace = rootNamespace;
            type = resolveTypeByNameInNamespace(location, name, genContext);
            if (type)
            {
                return type;
            }
        }    

        if (!isEmbededType(name))
            emitError(location, "can't find type by name: ") << name;

        return mlir::Type();    
    }

    mlir::Value resolveIdentifierInNamespace(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        auto value = resolveFunctionNameInNamespace(location, name, genContext);
        if (value)
        {
            return value;
        }

        if (getGlobalsMap().count(name))
        {
            auto value = getGlobalsMap().lookup(name);
            return globalVariableAccess(location, value, false, genContext);
        }

        // check if we have enum
        if (getEnumsMap().count(name))
        {
            auto enumTypeInfo = getEnumsMap().lookup(name);
            return builder.create<mlir_ts::ConstantOp>(location, getEnumType(enumTypeInfo.first, enumTypeInfo.second), enumTypeInfo.second);
        }

        if (getGenericFunctionMap().count(name))
        {
            auto genericFunctionInfo = getGenericFunctionMap().lookup(name);

            auto funcSymbolOp = builder.create<mlir_ts::SymbolRefOp>(
                location, genericFunctionInfo->funcType,
                mlir::FlatSymbolRefAttr::get(builder.getContext(), genericFunctionInfo->name));
            funcSymbolOp->setAttr(GENERIC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
            return funcSymbolOp;
        }

        if (getNamespaceMap().count(name))
        {
            auto namespaceInfo = getNamespaceMap().lookup(name);
            assert(namespaceInfo);
            auto nsName = mlir::FlatSymbolRefAttr::get(builder.getContext(), namespaceInfo->fullName);
            return builder.create<mlir_ts::NamespaceRefOp>(location, namespaceInfo->namespaceType, nsName);
        }

        if (getImportEqualsMap().count(name))
        {
            auto fullName = getImportEqualsMap().lookup(name);
            auto namespaceInfo = getNamespaceByFullName(fullName);
            if (namespaceInfo)
            {
                assert(namespaceInfo);
                auto nsName = mlir::FlatSymbolRefAttr::get(builder.getContext(), namespaceInfo->fullName);
                return builder.create<mlir_ts::NamespaceRefOp>(location, namespaceInfo->namespaceType, nsName);
            }
        }

        auto type = resolveTypeByNameInNamespace(location, name, genContext);
        if (type)
        {
            if (auto classType = type.dyn_cast<mlir_ts::ClassType>())
            {
                return builder.create<mlir_ts::ClassRefOp>(
                    location, classType, mlir::FlatSymbolRefAttr::get(builder.getContext(), classType.getName().getValue()));
            }

            if (auto interfaceType = type.dyn_cast<mlir_ts::InterfaceType>())
            {
                return builder.create<mlir_ts::InterfaceRefOp>(
                    location, interfaceType, mlir::FlatSymbolRefAttr::get(builder.getContext(), interfaceType.getName().getValue()));
            }

            return builder.create<mlir_ts::TypeRefOp>(location, type);
        }        

        return mlir::Value();
    }

    mlir::Value resolveFullNameIdentifier(mlir::Location location, StringRef name, bool asAddess,
                                          const GenContext &genContext)
    {
        if (fullNameGlobalsMap.count(name))
        {
            auto value = fullNameGlobalsMap.lookup(name);
            return globalVariableAccess(location, value, asAddess, genContext);
        }

        return mlir::Value();
    }

    mlir::Value globalVariableAccess(mlir::Location location, VariableDeclarationDOM::TypePtr value, bool asAddess,
                                     const GenContext &genContext)
    {
        if (!value->getType())
        {
            return mlir::Value();
        }

        if (!value->getReadWriteAccess() && value->getType().isa<mlir_ts::StringType>())
        {
            // load address of const object in global
            return builder.create<mlir_ts::AddressOfConstStringOp>(location, value->getType(), value->getName());
        }
        else
        {
            auto address = builder.create<mlir_ts::AddressOfOp>(location, mlir_ts::RefType::get(value->getType()),
                                                                value->getName(), ::mlir::IntegerAttr());
            if (asAddess)
            {
                return address;
            }

            return builder.create<mlir_ts::LoadOp>(location, value->getType(), address);
        }
    }

    mlir::Value resolveIdentifier(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        auto value = resolveIdentifierAsVariable(location, name, genContext);
        if (value)
        {
            return value;
        }

        value = resolveIdentifierInNamespace(location, name, genContext);
        if (value)
        {
            return value;
        }

        {
            MLIRNamespaceGuard ng(currentNamespace);

            // search in outer namespaces
            while (currentNamespace->isFunctionNamespace)
            {
                currentNamespace = currentNamespace->parentNamespace;
                value = resolveIdentifierInNamespace(location, name, genContext);
                if (value)
                {
                    return value;
                }
            }

            // search in root namespace
            currentNamespace = rootNamespace;
            value = resolveIdentifierInNamespace(location, name, genContext);
            if (value)
            {
                return value;
            }
        }

        // try to resolve 'this' if not resolved yet
        if (genContext.thisType && name == THIS_NAME)
        {
            return builder.create<mlir_ts::ClassRefOp>(
                location, genContext.thisType,
                mlir::FlatSymbolRefAttr::get(builder.getContext(),
                                             genContext.thisType.cast<mlir_ts::ClassType>().getName().getValue()));
        }

        if (genContext.thisType && name == SUPER_NAME)
        {
            if (!genContext.thisType.isa<mlir_ts::ClassType>() && !genContext.thisType.isa<mlir_ts::ClassStorageType>())
            {
                return mlir::Value();
            }

            auto result = mlirGen(location, THIS_NAME, genContext);
            auto thisValue = V(result);

            auto classInfo =
                getClassInfoByFullName(genContext.thisType.cast<mlir_ts::ClassType>().getName().getValue());
            auto baseClassInfo = classInfo->baseClasses.front();

            // this is access to static base class
            if (thisValue.getDefiningOp<mlir_ts::ClassRefOp>())
            {
                return builder.create<mlir_ts::ClassRefOp>(
                    location, baseClassInfo->classType,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(),
                                                baseClassInfo->classType.getName().getValue()));                   
            }

            return mlirGenPropertyAccessExpression(location, thisValue, baseClassInfo->fullName, genContext);
        }

        // built-in types
        if (name == UNDEFINED_NAME)
        {
            return getUndefined(location);
        }

        if (name == INFINITY_NAME)
        {
            return getInfinity(location);
        }

        if (name == NAN_NAME)
        {
            return getNaN(location);
        }

        // end of built-in types

        value = resolveFullNameIdentifier(location, name, false, genContext);
        if (value)
        {
            return value;
        }

        return mlir::Value();
    }

    ValueOrLogicalResult mlirGen(mlir::Location location, StringRef name, const GenContext &genContext)
    {
        auto value = resolveIdentifier(location, name, genContext);
        if (value)
        {
            return value;
        }

        if (MLIRCustomMethods::isInternalFunctionName(compileOptions, name))
        {
            auto symbOp = builder.create<mlir_ts::SymbolRefOp>(
                location, builder.getNoneType(), mlir::FlatSymbolRefAttr::get(builder.getContext(), name));
            symbOp->setAttr(VIRTUALFUNC_ATTR_NAME, mlir::BoolAttr::get(builder.getContext(), true));
            return V(symbOp);
        }

        if (MLIRCustomMethods::isInternalObjectName(name))
        {
            mlir::Type type;

            if (name == "Symbol")
            {
                type = getSymbolType();
            }
            else
            {
                type = builder.getNoneType();
            }

            // set correct type
            auto symbOp = builder.create<mlir_ts::SymbolRefOp>(
                location, type, mlir::FlatSymbolRefAttr::get(builder.getContext(), name));            
            return V(symbOp);
        }

        if (!isEmbededType(name))
            emitError(location, "can't resolve name: ") << name;

        return mlir::failure();
    }

    TypeParameterDOM::TypePtr processTypeParameter(TypeParameterDeclaration typeParameter, const GenContext &genContext)
    {
        auto namePtr = MLIRHelper::getName(typeParameter->name, stringAllocator);
        if (!namePtr.empty())
        {
            auto typeParameterDOM = std::make_shared<TypeParameterDOM>(namePtr.str());
            if (typeParameter->constraint)
            {
                typeParameterDOM->setConstraint(typeParameter->constraint);
            }

            if (typeParameter->_default)
            {
                typeParameterDOM->setDefault(typeParameter->_default);
            }

            return typeParameterDOM;
        }
        else
        {
            llvm_unreachable("not implemented");
        }
    }

    mlir::LogicalResult processTypeParameters(NodeArray<TypeParameterDeclaration> typeParameters,
                                              llvm::SmallVector<TypeParameterDOM::TypePtr> &typeParams,
                                              const GenContext &genContext)
    {
        for (auto typeParameter : typeParameters)
        {
            typeParams.push_back(processTypeParameter(typeParameter, genContext));
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(TypeAliasDeclaration typeAliasDeclarationAST, const GenContext &genContext)
    {
        auto namePtr = MLIRHelper::getName(typeAliasDeclarationAST->name, stringAllocator);
        if (!namePtr.empty())
        {
            auto hasExportModifier = getExportModifier(typeAliasDeclarationAST);

            if (typeAliasDeclarationAST->typeParameters.size() > 0)
            {
                llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
                if (mlir::failed(
                        processTypeParameters(typeAliasDeclarationAST->typeParameters, typeParameters, genContext)))
                {
                    return mlir::failure();
                }

                getGenericTypeAliasMap().insert({namePtr, {typeParameters, typeAliasDeclarationAST->type}});
            }
            else
            {
                GenContext typeAliasGenContext(genContext);
                auto type = getType(typeAliasDeclarationAST->type, typeAliasGenContext);
                if (!type)
                {
                    return mlir::failure();
                }

                getTypeAliasMap().insert({namePtr, type});

                if (hasExportModifier)
                {
                    addTypeDeclarationToExport(typeAliasDeclarationAST);
                }
            }

            return mlir::success();
        }
        else
        {
            llvm_unreachable("not implemented");
        }

        return mlir::failure();
    }

    ValueOrLogicalResult mlirGenModuleReference(Node moduleReference, const GenContext &genContext)
    {
        auto kind = (SyntaxKind)moduleReference;
        if (kind == SyntaxKind::QualifiedName)
        {
            return mlirGen(moduleReference.as<QualifiedName>(), genContext);
        }
        else if (kind == SyntaxKind::Identifier)
        {
            return mlirGen(moduleReference.as<Identifier>(), genContext);
        }

        llvm_unreachable("not implemented");
    }

    mlir::LogicalResult mlirGen(ImportEqualsDeclaration importEqualsDeclarationAST, const GenContext &genContext)
    {
        auto name = MLIRHelper::getName(importEqualsDeclarationAST->name);
        if (!name.empty())
        {
            auto result = mlirGenModuleReference(importEqualsDeclarationAST->moduleReference, genContext);
            auto value = V(result);
            if (auto namespaceOp = value.getDefiningOp<mlir_ts::NamespaceRefOp>())
            {
                getImportEqualsMap().insert({name, namespaceOp.getIdentifier()});
                return mlir::success();
            }
            else if (auto classType = value.getType().dyn_cast<mlir_ts::ClassType>())
            {
                getImportEqualsMap().insert({name, classType.getName().getValue()});
                return mlir::success();
            }
            else if (auto interfaceType = value.getType().dyn_cast<mlir_ts::InterfaceType>())
            {
                getImportEqualsMap().insert({name, interfaceType.getName().getValue()});
                return mlir::success();
            }

            llvm_unreachable("not implemented");
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGen(EnumDeclaration enumDeclarationAST, const GenContext &genContext)
    {
        auto namePtr = MLIRHelper::getName(enumDeclarationAST->name, stringAllocator);
        if (namePtr.empty())
        {
            llvm_unreachable("not implemented");
            return mlir::failure();
        }

        SymbolTableScopeT varScope(symbolTable);

        getEnumsMap().insert(
        {
            namePtr, 
            std::make_pair(
                getEnumType().getElementType(), mlir::DictionaryAttr::get(builder.getContext(), 
                {}))
        });

        SmallVector<mlir::Type> enumLiteralTypes;
        SmallVector<mlir::NamedAttribute> enumValues;
        int64_t index = 0;
        auto activeBits = 32;
        for (auto enumMember : enumDeclarationAST->members)
        {
            auto location = loc(enumMember);

            auto memberNamePtr = MLIRHelper::getName(enumMember->name, stringAllocator);
            if (memberNamePtr.empty())
            {
                llvm_unreachable("not implemented");
                return mlir::failure();
            }

            mlir::Attribute enumValueAttr;
            if (enumMember->initializer)
            {
                GenContext enumValueGenContext(genContext);
                enumValueGenContext.allowConstEval = true;
                auto result = mlirGen(enumMember->initializer, enumValueGenContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto enumValue = V(result);

                LLVM_DEBUG(llvm::dbgs() << "\n!! enum member: [ " << memberNamePtr << " ] = [ " << enumValue << " ]\n");

                if (auto constOp = dyn_cast<mlir_ts::ConstantOp>(enumValue.getDefiningOp()))
                {
                    enumValueAttr = constOp.getValueAttr();
                    if (auto intAttr = enumValueAttr.dyn_cast<mlir::IntegerAttr>())
                    {
                        index = intAttr.getInt();
                        auto currentActiveBits = (int)intAttr.getValue().getActiveBits();
                        if (currentActiveBits > activeBits)
                        {
                            activeBits = currentActiveBits;
                        }
                    }
                }
                else
                {
                    emitError(loc(enumMember->initializer))
                        << "enum member '" << memberNamePtr << "' must be constant";
                    return mlir::failure();
                }

                enumLiteralTypes.push_back(enumValue.getType());
                
                auto varDecl = std::make_shared<VariableDeclarationDOM>(memberNamePtr, enumValue.getType(), location);
                DECLARE(varDecl, enumValue);

            }
            else
            {
                auto typeInt = mlir::IntegerType::get(builder.getContext(), activeBits);
                enumValueAttr = builder.getIntegerAttr(typeInt, index);
                auto indexType = mlir_ts::LiteralType::get(enumValueAttr, typeInt);
                enumLiteralTypes.push_back(indexType);

                LLVM_DEBUG(llvm::dbgs() << "\n!! enum member: " << memberNamePtr << " <- " << indexType << "\n");

                auto varDecl = std::make_shared<VariableDeclarationDOM>(memberNamePtr, indexType, location);
                auto enumVal = builder.create<mlir_ts::ConstantOp>(location, indexType, enumValueAttr);
                DECLARE(varDecl, enumVal);
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! enum: " << namePtr << " value attr: " << enumValueAttr << "\n");

            enumValues.push_back({getStringAttr(memberNamePtr.str()), enumValueAttr});

            // update enum to support req. access
            getEnumsMap()[namePtr].second = mlir::DictionaryAttr::get(builder.getContext(), enumValues /*adjustedEnumValues*/);

            index++;

            // to make it available in enum context
            auto enumVal = enumValues.back();
        }

        auto storeType = mth.getUnionTypeWithMerge(enumLiteralTypes);

        LLVM_DEBUG(llvm::dbgs() << "\n!! enum: " << namePtr << " storage type: " << storeType << "\n");

        // update enum to support req. access
        getEnumsMap()[namePtr].first = storeType;

        if (getExportModifier(enumDeclarationAST))
        {
            addEnumDeclarationToExport(enumDeclarationAST);
        }

        return mlir::success();
    }

    mlir::LogicalResult registerGenericClass(ClassLikeDeclaration classDeclarationAST, const GenContext &genContext)
    {
        auto name = className(classDeclarationAST, genContext);
        if (!name.empty())
        {
            auto namePtr = StringRef(name).copy(stringAllocator);
            auto fullNamePtr = getFullNamespaceName(namePtr);
            if (fullNameGenericClassesMap.count(fullNamePtr))
            {
                return mlir::success();
            }

            llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
            if (mlir::failed(processTypeParameters(classDeclarationAST->typeParameters, typeParameters, genContext)))
            {
                return mlir::failure();
            }

            // register class
            GenericClassInfo::TypePtr newGenericClassPtr = std::make_shared<GenericClassInfo>();
            newGenericClassPtr->name = namePtr;
            newGenericClassPtr->fullName = fullNamePtr;
            newGenericClassPtr->typeParams = typeParameters;
            newGenericClassPtr->classDeclaration = classDeclarationAST;
            newGenericClassPtr->elementNamespace = currentNamespace;

            mlirGenClassType(newGenericClassPtr, genContext);

            getGenericClassesMap().insert({namePtr, newGenericClassPtr});
            fullNameGenericClassesMap.insert(fullNamePtr, newGenericClassPtr);

            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGen(ClassDeclaration classDeclarationAST, const GenContext &genContext)
    {
        mlir::OpBuilder::InsertionGuard guard(builder);
        builder.setInsertionPointToStart(theModule.getBody());

        auto value = mlirGen(classDeclarationAST.as<ClassLikeDeclaration>(), genContext);
        return std::get<0>(value);
    }

    ValueOrLogicalResult mlirGen(ClassExpression classExpressionAST, const GenContext &genContext)
    {
        std::string fullName;

        // go to root
        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(theModule.getBody());

            auto [result, fullNameRet] = mlirGen(classExpressionAST.as<ClassLikeDeclaration>(), genContext);
            if (mlir::failed(result))
            {
                return mlir::failure();
            }

            fullName = fullNameRet;
        }

        auto location = loc(classExpressionAST);

        auto classInfo = getClassInfoByFullName(fullName);
        if (classInfo)
        {
            if (classInfo->isDeclaration)
            {
                auto undefClass = builder.create<mlir_ts::UndefOp>(location, classInfo->classType);
                return V(undefClass);
            }
            else
            {
                auto classValue = builder.create<mlir_ts::ClassRefOp>(
                    location, classInfo->classType,
                    mlir::FlatSymbolRefAttr::get(builder.getContext(), classInfo->classType.getName().getValue()));

                // TODO: find out if you need to pass generics info, typeParams + typeArgs
                return NewClassInstance(location, classValue, undefined, undefined, false, genContext);
            }
        }

        return mlir::failure();
    }

    std::pair<mlir::LogicalResult, mlir::StringRef> mlirGen(ClassLikeDeclaration classDeclarationAST,
                                                            const GenContext &genContext)
    {
        // do not proceed for Generic Interfaces for declaration
        auto isGenericClass = classDeclarationAST->typeParameters.size() > 0;
        if (isGenericClass && genContext.typeParamsWithArgs.size() == 0)
        {
            return {registerGenericClass(classDeclarationAST, genContext), ""};
        }

        auto newClassPtr = mlirGenClassInfo(classDeclarationAST, genContext);
        if (!newClassPtr)
        {
            return {mlir::failure(), ""};
        }

        // do not process specialized class second time;
        if (isGenericClass && genContext.typeParamsWithArgs.size() > 0)
        {
            // TODO: investigate why classType is provided already for class
            if ((genContext.allowPartialResolve && newClassPtr->fullyProcessedAtEvaluation) ||
                (!genContext.allowPartialResolve && newClassPtr->fullyProcessed) ||
                newClassPtr->enteredProcessingStorageClass)
            {
                return {mlir::success(), newClassPtr->classType.getName().getValue()};
            }
        }

        auto location = loc(classDeclarationAST);

        if (mlir::succeeded(mlirGenClassType(newClassPtr, genContext)))
        {
            newClassPtr->typeParamsWithArgs = genContext.typeParamsWithArgs;
        }

        // if this is generic specialized class then do not generate code for it
        if (mth.isGenericType(newClassPtr->classType))
        {
            return {mlir::success(), newClassPtr->classType.getName().getValue()};
        }

        // init this type (needed to use in property evaluations)
        GenContext classGenContext(genContext);
        classGenContext.thisType = newClassPtr->classType;

        newClassPtr->processingStorageClass = true;
        newClassPtr->enteredProcessingStorageClass = true;

        if (mlir::failed(mlirGenClassStorageType(location, classDeclarationAST, newClassPtr, classGenContext)))
        {
            newClassPtr->processingStorageClass = false;
            newClassPtr->enteredProcessingStorageClass = false;
            return {mlir::failure(), ""};
        }

        newClassPtr->processingStorageClass = false;
        newClassPtr->processedStorageClass = true;

        // if it is ClassExpression we need to know if it is declaration
        mlirGenClassCheckIfDeclaration(location, classDeclarationAST, newClassPtr, classGenContext);

        // go to root
        mlir::OpBuilder::InsertPoint savePoint;
        if (isGenericClass)
        {
            savePoint = builder.saveInsertionPoint();
            builder.setInsertionPointToStart(theModule.getBody());
        }

        // prepare VTable
        llvm::SmallVector<VirtualMethodOrInterfaceVTableInfo> virtualTable;
        newClassPtr->getVirtualTable(virtualTable);

        if (!newClassPtr->isStatic)
        {
            mlirGenClassDefaultConstructor(classDeclarationAST, newClassPtr, classGenContext);
        }

#ifdef ENABLE_RTTI
        if (!newClassPtr->isStatic)
        {
            // INFO: .instanceOf must be first element in VTable for Cast Any
            mlirGenClassInstanceOfMethod(classDeclarationAST, newClassPtr, classGenContext);
        }
#endif

#if ENABLE_TYPED_GC
        auto enabledGC = !compileOptions.disableGC;
        if (enabledGC && !newClassPtr->isStatic)
        {
            mlirGenClassTypeBitmap(location, newClassPtr, classGenContext);
            mlirGenClassTypeDescriptorField(location, newClassPtr, classGenContext);
        }
#endif

        if (!newClassPtr->isStatic)
        {
            mlirGenClassNew(classDeclarationAST, newClassPtr, classGenContext);
        }

        mlirGenClassDefaultStaticConstructor(classDeclarationAST, newClassPtr, classGenContext);

        /*
        // to support call 'static v = new Class();'
        if (mlir::failed(mlirGenClassStaticFields(location, classDeclarationAST, newClassPtr, classGenContext)))
        {
            return {mlir::failure(), ""};
        }
        */

        if (mlir::failed(mlirGenClassMembers(location, classDeclarationAST, newClassPtr, classGenContext)))
        {
            return {mlir::failure(), ""};
        }

        // generate vtable for interfaces in base class
        if (mlir::failed(mlirGenClassBaseInterfaces(location, newClassPtr, classGenContext)))
        {
            return {mlir::failure(), ""};
        }

        // generate vtable for interfaces
        for (auto &heritageClause : classDeclarationAST->heritageClauses)
        {
            if (mlir::failed(mlirGenClassHeritageClauseImplements(classDeclarationAST, newClassPtr, heritageClause,
                                                                  classGenContext)))
            {
                return {mlir::failure(), ""};
            }
        }

        if (!newClassPtr->isStatic)
        {
            if (mlir::failed(mlirGenClassVirtualTableDefinition(location, newClassPtr, classGenContext)))
            {
                return {mlir::failure(), ""};
            }
        }

        // here we need to process New method;

        if (isGenericClass)
        {
            builder.restoreInsertionPoint(savePoint);
        }

        newClassPtr->enteredProcessingStorageClass = false;

        // if we allow multiple class nodes, do we need to store that ClassLikeDecl. has been processed fully
        if (classGenContext.allowPartialResolve)
        {
            newClassPtr->fullyProcessedAtEvaluation = true;
        }
        else
        {
            newClassPtr->fullyProcessed = true;
        }

        // support dynamic loading
        if (getExportModifier(classDeclarationAST))
        {
            addClassDeclarationToExport(classDeclarationAST);
        }

        return {mlir::success(), newClassPtr->classType.getName().getValue()};
    }

    void appendSpecializedTypeNames(std::string &name, llvm::SmallVector<TypeParameterDOM::TypePtr> &typeParams,
                                    const GenContext &genContext)
    {
        name.append("<");
        auto next = false;
        for (auto typeParam : typeParams)
        {
            if (next)
            {
                name.append(",");
            }

            auto type = getResolveTypeParameter(typeParam->getName(), false, genContext);
            if (type)
            {
                llvm::raw_string_ostream s(name);
                s << type;
            }
            else
            {
                name.append(typeParam->getName());
            }

            next = true;
        }

        name.append(">");
    }

    std::string getSpecializedClassName(GenericClassInfo::TypePtr geneticClassPtr, const GenContext &genContext)
    {
        auto name = geneticClassPtr->fullName.str();
        if (genContext.typeParamsWithArgs.size())
        {
            appendSpecializedTypeNames(name, geneticClassPtr->typeParams, genContext);
        }

        return name;
    }

    mlir_ts::ClassType getSpecializationClassType(GenericClassInfo::TypePtr genericClassPtr,
                                                  const GenContext &genContext)
    {
        auto fullSpecializedClassName = getSpecializedClassName(genericClassPtr, genContext);
        auto classInfoType = getClassInfoByFullName(fullSpecializedClassName);
        classInfoType->originClassType = genericClassPtr->classType;
        assert(classInfoType);
        return classInfoType->classType;
    }

    std::string className(ClassLikeDeclaration classDeclarationAST, const GenContext &genContext)
    {
        auto name = getNameWithArguments(classDeclarationAST, genContext);
        if (classDeclarationAST == SyntaxKind::ClassExpression)
        {
            NodeFactory nf(NodeFactoryFlags::None);
            classDeclarationAST->name = nf.createIdentifier(stows(name));
        }

        return name;
    }

    ClassInfo::TypePtr mlirGenClassInfo(ClassLikeDeclaration classDeclarationAST, const GenContext &genContext)
    {
        return mlirGenClassInfo(className(classDeclarationAST, genContext), classDeclarationAST, genContext);
    }

    ClassInfo::TypePtr mlirGenClassInfo(const std::string &name, ClassLikeDeclaration classDeclarationAST,
                                        const GenContext &genContext)
    {
        auto namePtr = StringRef(name).copy(stringAllocator);
        auto fullNamePtr = getFullNamespaceName(namePtr);

        ClassInfo::TypePtr newClassPtr;
        if (fullNameClassesMap.count(fullNamePtr))
        {
            newClassPtr = fullNameClassesMap.lookup(fullNamePtr);
            getClassesMap().insert({namePtr, newClassPtr});
        }
        else
        {
            // register class
            newClassPtr = std::make_shared<ClassInfo>();
            newClassPtr->name = namePtr;
            newClassPtr->fullName = fullNamePtr;
            newClassPtr->isAbstract = hasModifier(classDeclarationAST, SyntaxKind::AbstractKeyword);
            newClassPtr->isDeclaration =
                declarationMode || hasModifier(classDeclarationAST, SyntaxKind::DeclareKeyword);
            newClassPtr->isStatic = hasModifier(classDeclarationAST, SyntaxKind::StaticKeyword);
            newClassPtr->isExport = getExportModifier(classDeclarationAST);
            newClassPtr->hasVirtualTable = newClassPtr->isAbstract;

            // check decorator for class
            MLIRHelper::iterateDecorators(classDeclarationAST, [&](std::string name, SmallVector<std::string> args) {
                if (name == DLL_EXPORT)
                {
                    newClassPtr->isExport = true;
                }

                if (name == DLL_IMPORT)
                {
                    newClassPtr->isImport = true;
                    // it has parameter, means this is dynamic import, should point to dll path
                    if (args.size() > 0)
                    {
                        newClassPtr->isDynamicImport = true;
                    }
                }
            });

            getClassesMap().insert({namePtr, newClassPtr});
            fullNameClassesMap.insert(fullNamePtr, newClassPtr);
        }

        return newClassPtr;
    }

    template <typename T> mlir::LogicalResult mlirGenClassType(T newClassPtr, const GenContext &genContext)
    {
        if (newClassPtr)
        {
            auto classFullNameSymbol = mlir::FlatSymbolRefAttr::get(builder.getContext(), newClassPtr->fullName);
            newClassPtr->classType = getClassType(classFullNameSymbol, getClassStorageType(classFullNameSymbol));
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGenClassCheckIfDeclaration(mlir::Location location,
                                                       ClassLikeDeclaration classDeclarationAST,
                                                       ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        if (declarationMode)
        {
            newClassPtr->isDeclaration = true;
            return mlir::success();
        }

        if (classDeclarationAST != SyntaxKind::ClassExpression)
        {
            return mlir::success();
        }

        for (auto &classMember : classDeclarationAST->members)
        {
            // TODO:
            if (classMember == SyntaxKind::PropertyDeclaration)
            {
                // property declaration
                auto propertyDeclaration = classMember.as<PropertyDeclaration>();
                if (propertyDeclaration->initializer)
                {
                    // no definition
                    return mlir::success();
                }
            }

            if (classMember == SyntaxKind::MethodDeclaration || classMember == SyntaxKind::Constructor ||
                classMember == SyntaxKind::GetAccessor || classMember == SyntaxKind::SetAccessor)
            {
                auto funcLikeDeclaration = classMember.as<FunctionLikeDeclarationBase>();
                if (funcLikeDeclaration->body)
                {
                    // no definition
                    return mlir::success();
                }
            }
        }

        newClassPtr->isDeclaration = true;

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassTypeSetFields(ClassInfo::TypePtr newClassPtr,
                                                  SmallVector<mlir_ts::FieldInfo> &fieldInfos)
    {
        if (newClassPtr)
        {
            newClassPtr->classType.getStorageType().cast<mlir_ts::ClassStorageType>().setFields(fieldInfos);
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGenClassStorageType(mlir::Location location, ClassLikeDeclaration classDeclarationAST,
                                                ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);
        SmallVector<mlir_ts::FieldInfo> fieldInfos;

        // add base classes
        for (auto &heritageClause : classDeclarationAST->heritageClauses)
        {
            if (mlir::failed(mlirGenClassHeritageClause(classDeclarationAST, newClassPtr, heritageClause, fieldInfos,
                                                        genContext)))
            {
                return mlir::failure();
            }
        }

#if ENABLE_RTTI
        if (newClassPtr->isDynamicImport)
        {
            mlirGenCustomRTTIDynamicImport(location, classDeclarationAST, newClassPtr, genContext);
        }
        else if (!newClassPtr->isStatic)
        {
            newClassPtr->hasVirtualTable = true;
            mlirGenCustomRTTI(location, classDeclarationAST, newClassPtr, genContext);
        }
#endif

        // non-static first
        for (auto &classMember : classDeclarationAST->members)
        {
            if (mlir::failed(mlirGenClassFieldMember(classDeclarationAST, newClassPtr, classMember, fieldInfos, false,
                                                     genContext)))
            {
                return mlir::failure();
            }
        }

        if (newClassPtr->getHasVirtualTableVariable())
        {
            auto fieldId = MLIRHelper::TupleFieldName(VTABLE_NAME, builder.getContext());
            if (fieldInfos.size() == 0 || fieldInfos.front().id != fieldId)
            {
                fieldInfos.insert(fieldInfos.begin(), {fieldId, getOpaqueType()});
            }
        }

        mlirGenClassTypeSetFields(newClassPtr, fieldInfos);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassStaticFields(mlir::Location location, ClassLikeDeclaration classDeclarationAST,
                                                 ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        // dummy class, not used, needed to sync code
        // TODO: refactor it
        SmallVector<mlir_ts::FieldInfo> fieldInfos;

        // static second
        // TODO: if I use static method in static field initialization, test if I need process static fields after
        // static methods
        for (auto &classMember : classDeclarationAST->members)
        {
            if (mlir::failed(mlirGenClassFieldMember(classDeclarationAST, newClassPtr, classMember, fieldInfos, true,
                                                     genContext)))
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassMembers(mlir::Location location, ClassLikeDeclaration classDeclarationAST,
                                            ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        // clear all flags
        // extra fields - first, we need .instanceOf first for typr Any

        // dummy class, not used, needed to sync code
        // TODO: refactor it
        SmallVector<mlir_ts::FieldInfo> fieldInfos;

        // add methods when we have classType
        auto notResolved = 0;
        do
        {
            auto lastTimeNotResolved = notResolved;
            notResolved = 0;

            for (auto &classMember : newClassPtr->extraMembers)
            {
                if (mlir::failed(mlirGenClassMethodMember(classDeclarationAST, newClassPtr, classMember, genContext)))
                {
                    notResolved++;
                }
            }

            for (auto &classMember : classDeclarationAST->members)
            {
                // static fields
                if (mlir::failed(mlirGenClassFieldMember(classDeclarationAST, newClassPtr, classMember, fieldInfos,
                                                         true, genContext)))
                {
                    notResolved++;
                }

                if (mlir::failed(mlirGenClassMethodMember(classDeclarationAST, newClassPtr, classMember, genContext)))
                {
                    notResolved++;
                }
            }

            for (auto &classMember : newClassPtr->extraMembersPost)
            {
                if (mlir::failed(mlirGenClassMethodMember(classDeclarationAST, newClassPtr, classMember, genContext)))
                {
                    notResolved++;
                }
            }            

            // repeat if not all resolved
            if (lastTimeNotResolved > 0 && lastTimeNotResolved == notResolved)
            {
                // class can depend on other class declarations
                // theModule.emitError("can't resolve dependencies in class: ") << newClassPtr->name;
                return mlir::failure();
            }

        } while (notResolved > 0);

        // to be able to run next time, code succeeded, and we know where to continue from
        for (auto &classMember : newClassPtr->extraMembers)
        {
            classMember->processed = false;
        }

        for (auto &classMember : classDeclarationAST->members)
        {
            classMember->processed = false;
        }

        for (auto &classMember : newClassPtr->extraMembersPost)
        {
            classMember->processed = false;
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassHeritageClause(ClassLikeDeclaration classDeclarationAST,
                                                   ClassInfo::TypePtr newClassPtr, HeritageClause heritageClause,
                                                   SmallVector<mlir_ts::FieldInfo> &fieldInfos,
                                                   const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);

        if (heritageClause->token == SyntaxKind::ExtendsKeyword)
        {
            auto &baseClassInfos = newClassPtr->baseClasses;

            for (auto &extendingType : heritageClause->types)
            {
                auto result = mlirGen(extendingType, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto baseType = V(result);
                TypeSwitch<mlir::Type>(baseType.getType())
                    .template Case<mlir_ts::ClassType>([&](auto baseClassType) {
                        auto baseName = baseClassType.getName().getValue();
                        auto fieldId = MLIRHelper::TupleFieldName(baseName, builder.getContext());
                        fieldInfos.push_back({fieldId, baseClassType.getStorageType()});

                        auto classInfo = getClassInfoByFullName(baseName);
                        if (std::find(baseClassInfos.begin(), baseClassInfos.end(), classInfo) == baseClassInfos.end())
                        {
                            baseClassInfos.push_back(classInfo);
                        }
                    })
                    .Default([&](auto type) { llvm_unreachable("not implemented"); });
            }
            return mlir::success();
        }

        if (heritageClause->token == SyntaxKind::ImplementsKeyword)
        {
            newClassPtr->hasVirtualTable = true;

            auto &interfaceInfos = newClassPtr->implements;

            for (auto &implementingType : heritageClause->types)
            {
                if (implementingType->processed)
                {
                    continue;
                }

                auto result = mlirGen(implementingType, genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(result)
                auto ifaceType = V(result);
                TypeSwitch<mlir::Type>(ifaceType.getType())
                    .template Case<mlir_ts::InterfaceType>([&](auto interfaceType) {
                        auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
                        assert(interfaceInfo);
                        interfaceInfos.push_back({interfaceInfo, -1, false});
                        // TODO: it will error
                        // implementingType->processed = true;
                    })
                    .Default([&](auto type) { llvm_unreachable("not implemented"); });
            }
        }

        return mlir::success();
    }

    Node getFieldNameForAccessor(Node name) {
        auto nameStr = MLIRHelper::getName(name);
        nameStr.insert(0, "#__");

        NodeFactory nf(NodeFactoryFlags::None);
        auto newName = nf.createIdentifier(stows(nameStr.c_str()));
        return newName;
    }

    mlir::LogicalResult mlirGenClassDataFieldAccessor(mlir::Location location, ClassInfo::TypePtr newClassPtr, 
            PropertyDeclaration propertyDeclaration, MemberName name, mlir::Type typeIfNotProvided, const GenContext &genContext)
    {
        NodeFactory nf(NodeFactoryFlags::None);

        NodeArray<ModifierLike> modifiers;
        for (auto modifier : propertyDeclaration->modifiers)
        {
            if (modifier == SyntaxKind::AccessorKeyword)
            {
                continue;
            }

            modifiers.push_back(modifier);
        }

        // add accessor methods
        if ((propertyDeclaration->internalFlags & InternalFlags::GenerationProcessed) != InternalFlags::GenerationProcessed)
        {            
            // set as generated
            propertyDeclaration->internalFlags |= InternalFlags::GenerationProcessed;

            {
                NodeArray<Statement> statements;

                auto thisToken = nf.createToken(SyntaxKind::ThisKeyword);

                auto propAccess = nf.createPropertyAccessExpression(thisToken, name);

                auto returnStat = nf.createReturnStatement(propAccess);
                statements.push_back(returnStat);

                auto body = nf.createBlock(statements, /*multiLine*/ false);

                auto getMethod = nf.createGetAccessorDeclaration(modifiers, propertyDeclaration->name, {}, undefined, body);

                newClassPtr->extraMembersPost->push_back(getMethod);
            }

            {
                NodeArray<Statement> statements;

                auto thisToken = nf.createToken(SyntaxKind::ThisKeyword);

                auto propAccess = nf.createPropertyAccessExpression(thisToken, name);

                auto setValue =
                    nf.createExpressionStatement(
                        nf.createBinaryExpression(propAccess, nf.createToken(SyntaxKind::EqualsToken), nf.createIdentifier(S("value"))));
                statements.push_back(setValue);

                auto body = nf.createBlock(statements, /*multiLine*/ false);

                auto type = propertyDeclaration->type;
                if (!type && typeIfNotProvided)
                {
                    std::string fieldTypeAlias;
                    fieldTypeAlias += ".";
                    fieldTypeAlias += newClassPtr->fullName.str();
                    fieldTypeAlias += ".";
                    fieldTypeAlias += MLIRHelper::getName(name);
                    type = nf.createTypeReferenceNode(nf.createIdentifier(stows(fieldTypeAlias)), undefined);    

                    getTypeAliasMap().insert({fieldTypeAlias, typeIfNotProvided});
                }

                if (!type)
                {
                    emitError(location) << "type for field accessor '" << MLIRHelper::getName(propertyDeclaration->name) << "' must be provided";
                    return mlir::failure();
                }

                auto setMethod = nf.createSetAccessorDeclaration(
                    modifiers, 
                    propertyDeclaration->name, 
                    { nf.createParameterDeclaration(undefined, undefined, nf.createIdentifier(S("value")), undefined, type) }, 
                    body);

                newClassPtr->extraMembersPost->push_back(setMethod);
            }
        }        

        return mlir::success();
    }    

    mlir::LogicalResult mlirGenClassDataFieldMember(mlir::Location location, ClassInfo::TypePtr newClassPtr, SmallVector<mlir_ts::FieldInfo> &fieldInfos, 
                                                    PropertyDeclaration propertyDeclaration, const GenContext &genContext)
    {
        auto name = propertyDeclaration->name;
        auto isAccessor = hasModifier(propertyDeclaration, SyntaxKind::AccessorKeyword);
        if (isAccessor)
        {
            name = getFieldNameForAccessor(name);
        }
        
        auto fieldId = TupleFieldName(name, genContext);

        auto [type, init, typeProvided] = evaluateTypeAndInit(propertyDeclaration, genContext);
        if (init)
        {
            newClassPtr->hasInitializers = true;
            type = mth.wideStorageType(type);
        }

        LLVM_DEBUG(dbgs() << "\n!! class field: " << fieldId << " type: " << type << "");

        auto hasType = !!propertyDeclaration->type;
        if (mth.isNoneType(type))
        {
            if (hasType)
            {
                return mlir::failure();
            }

#ifndef ANY_AS_DEFAULT
            emitError(location)
                << "type for field '" << fieldId << "' is not provided, field must have type or initializer";
            return mlir::failure();
#else
            emitWarning(location) << "type for field '" << fieldId << "' is any";
            type = getAnyType();
#endif
        }

        fieldInfos.push_back({fieldId, type});

        // add accessor methods
        if (isAccessor)
        {            
            auto res = mlirGenClassDataFieldAccessor(location, newClassPtr, propertyDeclaration, name, type, genContext);
            EXIT_IF_FAILED(res)
        }        

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassStaticFieldMember(mlir::Location location, ClassInfo::TypePtr newClassPtr, PropertyDeclaration propertyDeclaration, const GenContext &genContext)
    {
        auto isPublic = hasModifier(propertyDeclaration, SyntaxKind::PublicKeyword);
        auto name = propertyDeclaration->name;

        auto isAccessor = hasModifier(propertyDeclaration, SyntaxKind::AccessorKeyword);
        if (isAccessor)
        {
            isPublic = false;
            name = getFieldNameForAccessor(name);
        }

        auto fieldId = TupleFieldName(name, genContext);

        // process static field - register global
        auto fullClassStaticFieldName =
            concat(newClassPtr->fullName, fieldId.cast<mlir::StringAttr>().getValue());
        VariableClass varClass = newClassPtr->isDeclaration ? VariableType::External : VariableType::Var;
        varClass.isExport = newClassPtr->isExport && isPublic;
        varClass.isImport = newClassPtr->isImport && isPublic;

        auto staticFieldType = registerVariable(
            location, fullClassStaticFieldName, true, varClass,
            [&](mlir::Location location, const GenContext &genContext) {
                auto isConst = false;
                mlir::Type typeInit;
                evaluate(
                    propertyDeclaration->initializer,
                    [&](mlir::Value val) {
                        typeInit = val.getType();
                        typeInit = mth.wideStorageType(typeInit);
                        isConst = isConstValue(val);
                    },
                    genContext);

                if (!newClassPtr->isDeclaration)
                {
                    if (isConst)
                    {
                        return getTypeAndInit(propertyDeclaration, genContext);
                    }

                    newClassPtr->hasStaticInitializers = true;
                }

                return getTypeOnly(propertyDeclaration, typeInit, genContext);
            },
            genContext);

        auto &staticFieldInfos = newClassPtr->staticFields;
        staticFieldInfos.push_back({fieldId, staticFieldType, fullClassStaticFieldName, -1});

        // add accessor methods
        if (isAccessor)
        {            
            auto res = mlirGenClassDataFieldAccessor(location, newClassPtr, propertyDeclaration, name, staticFieldType, genContext);
            EXIT_IF_FAILED(res)
        }  

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassStaticFieldMemberDynamicImport(mlir::Location location, ClassInfo::TypePtr newClassPtr, PropertyDeclaration propertyDeclaration, const GenContext &genContext)
    {
        auto fieldId = TupleFieldName(propertyDeclaration->name, genContext);

        // process static field - register global
        auto fullClassStaticFieldName =
            concat(newClassPtr->fullName, fieldId.cast<mlir::StringAttr>().getValue());
        
        auto staticFieldType = registerVariable(
            location, fullClassStaticFieldName, true, VariableType::Var,
            [&](mlir::Location location, const GenContext &genContext) -> TypeValueInitType {
                // detect field Type
                auto isConst = false;
                mlir::Type typeInit;
                evaluate(
                    propertyDeclaration->initializer,
                    [&](mlir::Value val) {
                        typeInit = val.getType();
                        typeInit = mth.wideStorageType(typeInit);
                        isConst = isConstValue(val);
                    },
                    genContext);

                // add command to load reference from DLL
                auto fullName = V(mlirGenStringValue(location, fullClassStaticFieldName.str(), true));
                auto referenceToStaticFieldOpaque = builder.create<mlir_ts::SearchForAddressOfSymbolOp>(location, getOpaqueType(), fullName);
                auto result = cast(location, mlir_ts::RefType::get(typeInit), referenceToStaticFieldOpaque, genContext);
                auto referenceToStaticField = V(result);
                return {referenceToStaticField.getType(), referenceToStaticField, TypeProvided::No};
            },
            genContext);

        auto &staticFieldInfos = newClassPtr->staticFields;
        staticFieldInfos.push_back({fieldId, staticFieldType, fullClassStaticFieldName, -1});

        return mlir::success();
    }    

    mlir::LogicalResult mlirGenClassConstructorPublicDataFieldMembers(mlir::Location location, SmallVector<mlir_ts::FieldInfo> &fieldInfos, 
                                                                      ConstructorDeclaration constructorDeclaration, const GenContext &genContext)
    {
        for (auto &parameter : constructorDeclaration->parameters)
        {
            auto isPublic = hasModifier(parameter, SyntaxKind::PublicKeyword);
            auto isProtected = hasModifier(parameter, SyntaxKind::ProtectedKeyword);
            auto isPrivate = hasModifier(parameter, SyntaxKind::PrivateKeyword);

            if (!(isPublic || isProtected || isPrivate))
            {
                continue;
            }

            auto fieldId = TupleFieldName(parameter->name, genContext);

            auto [type, init, typeProvided] = getTypeAndInit(parameter, genContext);

            LLVM_DEBUG(dbgs() << "\n+++ class auto-gen field: " << fieldId << " type: " << type << "");
            if (mth.isNoneType(type))
            {
                return mlir::failure();
            }

            fieldInfos.push_back({fieldId, type});
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassProcessClassPropertyByFieldMember(ClassInfo::TypePtr newClassPtr, ClassElement classMember)
    {
        auto isStatic = hasModifier(classMember, SyntaxKind::StaticKeyword);
        auto isConstructor = classMember == SyntaxKind::Constructor;
        if (isConstructor)
        {
            if (isStatic)
            {
                newClassPtr->hasStaticConstructor = true;
            }
            else
            {
                newClassPtr->hasConstructor = true;
            }
        }

        auto isMemberAbstract = hasModifier(classMember, SyntaxKind::AbstractKeyword);
        if (isMemberAbstract)
        {
            newClassPtr->hasVirtualTable = true;
        }

        auto isVirtual = (classMember->internalFlags & InternalFlags::ForceVirtual) == InternalFlags::ForceVirtual;
#ifdef ALL_METHODS_VIRTUAL
        isVirtual = !isConstructor;
#endif
        if (isVirtual)
        {
            newClassPtr->hasVirtualTable = true;
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassFieldMember(ClassLikeDeclaration classDeclarationAST,
                                                ClassInfo::TypePtr newClassPtr, ClassElement classMember,
                                                SmallVector<mlir_ts::FieldInfo> &fieldInfos, bool staticOnly,
                                                const GenContext &genContext)
    {
        auto isStatic = hasModifier(classMember, SyntaxKind::StaticKeyword);
        if (staticOnly != isStatic)
        {
            return mlir::success();
        }

        auto location = loc(classMember);

        mlirGenClassProcessClassPropertyByFieldMember(newClassPtr, classMember);

        if (classMember == SyntaxKind::PropertyDeclaration)
        {
            // property declaration
            auto propertyDeclaration = classMember.as<PropertyDeclaration>();
            if (!isStatic)
            {
                if (mlir::failed(mlirGenClassDataFieldMember(location, newClassPtr, fieldInfos, propertyDeclaration, genContext)))
                {
                    return mlir::failure();
                }
            }
            else
            {
                if (newClassPtr->isDynamicImport)
                {
                    if (mlir::failed(mlirGenClassStaticFieldMemberDynamicImport(location, newClassPtr, propertyDeclaration, genContext)))
                    {
                        return mlir::failure();
                    }
                }
                else if (mlir::failed(mlirGenClassStaticFieldMember(location, newClassPtr, propertyDeclaration, genContext)))
                {
                    return mlir::failure();
                }
            }
        }

        if (classMember == SyntaxKind::Constructor && !isStatic)
        {
            auto constructorDeclaration = classMember.as<ConstructorDeclaration>();
            if (mlir::failed(mlirGenClassConstructorPublicDataFieldMembers(location, fieldInfos, constructorDeclaration, genContext)))
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenForwardDeclaration(const std::string &funcName, mlir_ts::FunctionType funcType,
                                                  bool isStatic, bool isVirtual, bool isAbstract,
                                                  ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        if (newClassPtr->getMethodIndex(funcName) < 0)
        {
            return mlir::success();
        }

        mlir_ts::FuncOp dummyFuncOp;
        newClassPtr->methods.push_back({funcName, funcType, dummyFuncOp, isStatic,
                                        isVirtual || isAbstract, isAbstract, -1});
        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassNew(ClassLikeDeclaration classDeclarationAST, ClassInfo::TypePtr newClassPtr,
                                        const GenContext &genContext)
    {
        if (newClassPtr->isAbstract || newClassPtr->hasNew)
        {
            return mlir::success();
        }

        // create constructor
        newClassPtr->hasNew = true;

        // if we do not have constructor but have initializers we need to create empty dummy constructor
        NodeFactory nf(NodeFactoryFlags::None);

        Block body;
        auto thisToken = nf.createToken(SyntaxKind::ThisKeyword);

        if (!newClassPtr->isDeclaration)
        {
            NodeArray<Statement> statements;

            auto newCall = nf.createNewExpression(thisToken, undefined, undefined);
            newCall->internalFlags |= InternalFlags::SuppressConstructorCall;

            auto returnStat = nf.createReturnStatement(newCall);
            statements.push_back(returnStat);

            body = nf.createBlock(statements, /*multiLine*/ false);
        }

        ModifiersArray modifiers;
        modifiers->push_back(nf.createToken(SyntaxKind::StaticKeyword));

        if (newClassPtr->isExport || newClassPtr->isImport)
        {
            modifiers.push_back(nf.createToken(SyntaxKind::PublicKeyword));
        }

        auto generatedNew = nf.createMethodDeclaration(modifiers, undefined, nf.createIdentifier(S(NEW_METHOD_NAME)),
                                                       undefined, undefined, undefined, nf.createThisTypeNode(), body);

        /*
        // advance declaration of "new"
        auto isStatic = false;
#ifdef ALL_METHODS_VIRTUAL
        auto isVirtual = true;
#else
        auto isVirtual = false;
#endif
        SmallVector<mlir::Type> inputs;
        SmallVector<mlir::Type> results{newClassPtr->classType};
        mlirGenForwardDeclaration(NEW_METHOD_NAME, getFunctionType(inputs, results), isStatic, isVirtual, newClassPtr,
genContext);

        newClassPtr->extraMembersPost.push_back(generatedNew);
        */

        newClassPtr->extraMembers.push_back(generatedNew);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassDefaultConstructor(ClassLikeDeclaration classDeclarationAST,
                                                       ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        // if we do not have constructor but have initializers we need to create empty dummy constructor
        if (newClassPtr->hasInitializers && !newClassPtr->hasConstructor)
        {
            // create constructor
            newClassPtr->hasConstructor = true;

            NodeFactory nf(NodeFactoryFlags::None);

            NodeArray<Statement> statements;

            if (!newClassPtr->baseClasses.empty())
            {
                auto superExpr = nf.createToken(SyntaxKind::SuperKeyword);
                auto callSuper = nf.createCallExpression(superExpr, undefined, undefined);
                statements.push_back(nf.createExpressionStatement(callSuper));
            }

            auto body = nf.createBlock(statements, /*multiLine*/ false);

            auto generatedConstructor = nf.createConstructorDeclaration(undefined, undefined, body);
            newClassPtr->extraMembers.push_back(generatedConstructor);
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassDefaultStaticConstructor(ClassLikeDeclaration classDeclarationAST,
                                                             ClassInfo::TypePtr newClassPtr,
                                                             const GenContext &genContext)
    {
        // if we do not have constructor but have initializers we need to create empty dummy constructor
        if (newClassPtr->hasStaticInitializers && !newClassPtr->hasStaticConstructor)
        {
            // create constructor
            newClassPtr->hasStaticConstructor = true;

            NodeFactory nf(NodeFactoryFlags::None);

            NodeArray<Statement> statements;

            auto body = nf.createBlock(statements, /*multiLine*/ false);
            ModifiersArray modifiers;
            modifiers.push_back(nf.createToken(SyntaxKind::StaticKeyword));
            auto generatedConstructor = nf.createConstructorDeclaration(modifiers, undefined, body);
            newClassPtr->extraMembersPost.push_back(generatedConstructor);
        }

        return mlir::success();
    }

    // INFO: you can't use standart Static Field declarastion because of RTTI should be declared before used
    // example: C:/dev/TypeScriptCompiler/tsc/test/tester/tests/dependencies.ts
    mlir::LogicalResult mlirGenCustomRTTI(mlir::Location location, ClassLikeDeclaration classDeclarationAST,
                                          ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        auto &staticFieldInfos = newClassPtr->staticFields;

        auto fieldId = MLIRHelper::TupleFieldName(RTTI_NAME, builder.getContext());

        // register global
        auto fullClassStaticFieldName = concat(newClassPtr->fullName, RTTI_NAME);

        auto staticFieldType = getStringType();

        if (!fullNameGlobalsMap.count(fullClassStaticFieldName))
        {
            // prevent double generating
            VariableClass varClass = newClassPtr->isDeclaration ? VariableType::External : VariableType::Var;
            varClass.isExport = newClassPtr->isExport;
            varClass.isImport = newClassPtr->isImport;
            registerVariable(
                location, fullClassStaticFieldName, true, varClass,
                [&](mlir::Location location, const GenContext &genContext) {
                    if (newClassPtr->isDeclaration)
                    {
                        return std::make_tuple(staticFieldType, mlir::Value(), TypeProvided::Yes);
                    }

                    mlir::Value init = builder.create<mlir_ts::ConstantOp>(location, staticFieldType,
                                                                            getStringAttr(newClassPtr->fullName.str()));
                    return std::make_tuple(staticFieldType, init, TypeProvided::Yes);
                },
                genContext);
        }

        if (!llvm::any_of(staticFieldInfos, [&](auto& field) { return field.id = fieldId; }))
        {
            staticFieldInfos.push_back({fieldId, staticFieldType, fullClassStaticFieldName, -1});
        }

        return mlir::success();
    }

    // INFO: you can't use standart Static Field declarastion because of RTTI should be declared before used
    // example: C:/dev/TypeScriptCompiler/tsc/test/tester/tests/dependencies.ts
    mlir::LogicalResult mlirGenCustomRTTIDynamicImport(mlir::Location location, ClassLikeDeclaration classDeclarationAST,
                                          ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        auto &staticFieldInfos = newClassPtr->staticFields;

        auto fieldId = MLIRHelper::TupleFieldName(RTTI_NAME, builder.getContext());

        // register global
        auto fullClassStaticFieldName = concat(newClassPtr->fullName, RTTI_NAME);

        auto staticFieldType =  mlir_ts::RefType::get(getStringType());

        if (!fullNameGlobalsMap.count(fullClassStaticFieldName))
        {
            // prevent double generating
            registerVariable(
                location, fullClassStaticFieldName, true, VariableType::Var,
                [&](mlir::Location location, const GenContext &genContext)  -> TypeValueInitType {
                    auto fullName = V(mlirGenStringValue(location, fullClassStaticFieldName.str(), true));
                    auto referenceToStaticFieldOpaque = builder.create<mlir_ts::SearchForAddressOfSymbolOp>(location, getOpaqueType(), fullName);
                    auto result = cast(location, staticFieldType, referenceToStaticFieldOpaque, genContext);
                    auto referenceToStaticField = V(result);
                    return {referenceToStaticField.getType(), referenceToStaticField, TypeProvided::Yes};
                },
                genContext);
        }

        if (!llvm::any_of(staticFieldInfos, [&](auto& field) { return field.id = fieldId; }))
        {
            staticFieldInfos.push_back({fieldId, staticFieldType, fullClassStaticFieldName, -1});
        }

        return mlir::success();
    }

#ifdef ENABLE_TYPED_GC
    StringRef getTypeBitmapMethodName(ClassInfo::TypePtr newClassPtr)
    {
        return concat(newClassPtr->fullName, TYPE_BITMAP_NAME);
    }

    StringRef getTypeDescriptorFieldName(ClassInfo::TypePtr newClassPtr)
    {
        return concat(newClassPtr->fullName, TYPE_DESCR_NAME);
    }

    mlir::LogicalResult mlirGenClassTypeDescriptorField(mlir::Location location, ClassInfo::TypePtr newClassPtr,
                                                        const GenContext &genContext)
    {
        // TODO: experiment if we need it at all even external declaration
        if (newClassPtr->isDeclaration)
        {
            return mlir::success();
        }

        // register global
        auto fullClassStaticFieldName = getTypeDescriptorFieldName(newClassPtr);

        if (!fullNameGlobalsMap.count(fullClassStaticFieldName))
        {
            registerVariable(
                location, fullClassStaticFieldName, true,
                newClassPtr->isDeclaration ? VariableType::External : VariableType::Var,
                [&](mlir::Location location, const GenContext &genContext) {
                    auto init =
                        builder.create<mlir_ts::ConstantOp>(location, builder.getI64Type(), mth.getI64AttrValue(0));
                    return std::make_tuple(init.getType(), init, TypeProvided::Yes);
                },
                genContext);
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassTypeBitmap(mlir::Location location, ClassInfo::TypePtr newClassPtr,
                                               const GenContext &genContext)
    {
        // no need to generate
        if (newClassPtr->isDeclaration)
        {
            return mlir::success();
        }

        MLIRCodeLogic mcl(builder);

        // register global
        auto fullClassStaticFieldName = getTypeBitmapMethodName(newClassPtr);

        auto funcType = getFunctionType({}, builder.getI64Type(), false);

        mlirGenFunctionBody(
            location, fullClassStaticFieldName, funcType,
            [&](const GenContext &genContext) {
                auto bitmapValueType = mth.getTypeBitmapValueType();

                auto nullOp = builder.create<mlir_ts::NullOp>(location, getNullType());
                CAST_A(classNull, location, newClassPtr->classType, nullOp, genContext);

                auto sizeOfStoreElement =
                    builder.create<mlir_ts::SizeOfOp>(location, mth.getIndexType(), mth.getTypeBitmapValueType());

                auto _8Value = builder.create<mlir_ts::ConstantOp>(location, mth.getIndexType(),
                                                                   builder.getIntegerAttr(mth.getIndexType(), 8));
                auto sizeOfStoreElementInBits = builder.create<mlir_ts::ArithmeticBinaryOp>(
                    location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::AsteriskToken),
                    sizeOfStoreElement, _8Value);

                // calc bitmap size
                auto sizeOfType =
                    builder.create<mlir_ts::SizeOfOp>(location, mth.getIndexType(), newClassPtr->classType);

                // calc count of store elements of type size
                auto sizeOfTypeInBitmapTypes = builder.create<mlir_ts::ArithmeticBinaryOp>(
                    location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::SlashToken), sizeOfType,
                    sizeOfStoreElement);

                // size alligned by size of bits
                auto sizeOfTypeAligned = builder.create<mlir_ts::ArithmeticBinaryOp>(
                    location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::PlusToken),
                    sizeOfTypeInBitmapTypes, sizeOfStoreElementInBits);

                auto _1I64Value = builder.create<mlir_ts::ConstantOp>(location, mth.getIndexType(),
                                                                      builder.getIntegerAttr(mth.getIndexType(), 1));

                sizeOfTypeAligned = builder.create<mlir_ts::ArithmeticBinaryOp>(
                    location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::MinusToken),
                    sizeOfTypeAligned, _1I64Value);

                sizeOfTypeAligned = builder.create<mlir_ts::ArithmeticBinaryOp>(
                    location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::SlashToken),
                    sizeOfTypeAligned, sizeOfStoreElementInBits);

                // allocate in stack
                auto arrayValue = builder.create<mlir_ts::AllocaOp>(location, mlir_ts::RefType::get(bitmapValueType),
                                                                    sizeOfTypeAligned);

                // property ref
                auto count = newClassPtr->fieldsCount();
                for (auto index = 0; (unsigned)index < count; index++)
                {
                    auto fieldInfo = newClassPtr->fieldInfoByIndex(index);
                    // skip virrual table for speed adv.
                    if (index == 0 && fieldInfo.type.isa<mlir_ts::OpaqueType>())
                    {
                        continue;
                    }

                    if (mth.isValueType(fieldInfo.type))
                    {
                        continue;
                    }

                    auto fieldValue = mlirGenPropertyAccessExpression(location, classNull, fieldInfo.id, genContext);
                    assert(fieldValue);
                    auto fieldRef = mcl.GetReferenceOfLoadOp(fieldValue);

                    // cast to int64
                    CAST_A(fieldAddrAsInt, location, mth.getIndexType(), fieldRef, genContext);

                    // calc index
                    auto calcIndex = builder.create<mlir_ts::ArithmeticBinaryOp>(
                        location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::SlashToken),
                        fieldAddrAsInt, sizeOfStoreElement);

                    CAST_A(calcIndex32, location, mth.getStructIndexType(), calcIndex, genContext);

                    auto elemRef = builder.create<mlir_ts::PointerOffsetRefOp>(
                        location, mlir_ts::RefType::get(bitmapValueType), arrayValue, calcIndex32);

                    // calc bit
                    auto indexModIndex = builder.create<mlir_ts::ArithmeticBinaryOp>(
                        location, mth.getIndexType(), builder.getI32IntegerAttr((int)SyntaxKind::PercentToken),
                        calcIndex, sizeOfStoreElementInBits);

                    auto indexMod = builder.create<mlir_ts::CastOp>(location, bitmapValueType, indexModIndex);

                    auto _1Value = builder.create<mlir_ts::ConstantOp>(location, bitmapValueType,
                                                                       builder.getIntegerAttr(bitmapValueType, 1));

                    // 1 << index_mod
                    auto bitValue = builder.create<mlir_ts::ArithmeticBinaryOp>(
                        location, bitmapValueType,
                        builder.getI32IntegerAttr((int)SyntaxKind::GreaterThanGreaterThanToken), _1Value, indexMod);

                    // load val
                    auto val = builder.create<mlir_ts::LoadOp>(location, bitmapValueType, elemRef);

                    // apply or
                    auto valWithBit = builder.create<mlir_ts::ArithmeticBinaryOp>(
                        location, bitmapValueType, builder.getI32IntegerAttr((int)SyntaxKind::BarToken), val, bitValue);

                    // save value
                    auto saveToElement = builder.create<mlir_ts::StoreOp>(location, valWithBit, elemRef);
                }

                auto typeDescr = builder.create<mlir_ts::GCMakeDescriptorOp>(location, builder.getI64Type(), arrayValue,
                                                                             sizeOfTypeInBitmapTypes);

                auto retVarInfo = symbolTable.lookup(RETURN_VARIABLE_NAME);
                builder.create<mlir_ts::ReturnValOp>(location, typeDescr, retVarInfo.first);
                return ValueOrLogicalResult(mlir::success());
            },
            genContext);

        return mlir::success();
    }

#endif

    mlir::LogicalResult mlirGenClassInstanceOfMethod(ClassLikeDeclaration classDeclarationAST,
                                                     ClassInfo::TypePtr newClassPtr, const GenContext &genContext)
    {
        // if we do not have constructor but have initializers we need to create empty dummy constructor
        // if (newClassPtr->getHasVirtualTable())
        {
            if (newClassPtr->hasRTTI)
            {
                return mlir::success();
            }

            newClassPtr->hasRTTI = true;

            NodeFactory nf(NodeFactoryFlags::None);

            Block body = undefined;
            if (!newClassPtr->isDeclaration)
            {
                NodeArray<Statement> statements;

                /*
                if (!newClassPtr->baseClasses.empty())
                {
                    auto superExpr = nf.createToken(SyntaxKind::SuperKeyword);
                    auto callSuper = nf.createCallExpression(superExpr, undefined, undefined);
                    statements.push_back(nf.createExpressionStatement(callSuper));
                }
                */

                // access .rtti via this (as virtual method)
                // auto cmpRttiToParam = nf.createBinaryExpression(
                //     nf.createIdentifier(LINSTANCEOF_PARAM_NAME), nf.createToken(SyntaxKind::EqualsEqualsToken),
                //     nf.createPropertyAccessExpression(nf.createToken(SyntaxKind::ThisKeyword),
                //                                       nf.createIdentifier(S(RTTI_NAME))));

                // access .rtti via static field
                auto fullClassStaticFieldName = concat(newClassPtr->fullName, RTTI_NAME);

                auto cmpRttiToParam = nf.createBinaryExpression(
                     nf.createIdentifier(S(INSTANCEOF_PARAM_NAME)), nf.createToken(SyntaxKind::EqualsEqualsToken),
                     nf.createIdentifier(ConvertUTF8toWide(std::string(fullClassStaticFieldName))));

                auto cmpLogic = cmpRttiToParam;

                if (!newClassPtr->baseClasses.empty())
                {
                    NodeArray<Expression> argumentsArray;
                    argumentsArray.push_back(nf.createIdentifier(S(INSTANCEOF_PARAM_NAME)));
                    cmpLogic =
                        nf.createBinaryExpression(cmpRttiToParam, nf.createToken(SyntaxKind::BarBarToken),
                                                  nf.createCallExpression(nf.createPropertyAccessExpression(
                                                                              nf.createToken(SyntaxKind::SuperKeyword),
                                                                              nf.createIdentifier(S(INSTANCEOF_NAME))),
                                                                          undefined, argumentsArray));
                }

                auto returnStat = nf.createReturnStatement(cmpLogic);
                statements.push_back(returnStat);

                body = nf.createBlock(statements, false);
            }

            NodeArray<ParameterDeclaration> parameters;
            parameters.push_back(nf.createParameterDeclaration(undefined, undefined,
                                                               nf.createIdentifier(S(INSTANCEOF_PARAM_NAME)), undefined,
                                                               nf.createToken(SyntaxKind::StringKeyword), undefined));

            ModifiersArray modifiers;
            if (newClassPtr->isExport || newClassPtr->isImport)
            {
                modifiers.push_back(nf.createToken(SyntaxKind::PublicKeyword));
            }

            auto instanceOfMethod = nf.createMethodDeclaration(
                modifiers, undefined, nf.createIdentifier(S(INSTANCEOF_NAME)), undefined, undefined,
                parameters, nf.createToken(SyntaxKind::BooleanKeyword), body);

            instanceOfMethod->internalFlags |= InternalFlags::ForceVirtual;
            // TODO: you adding new member to the same DOM(parse) instance but it is used for 2 instances of generic
            // type ERROR: do not change members!!!!

            // INFO: .instanceOf must be first element in VTable for Cast Any
            for (auto member : newClassPtr->extraMembers)
            {
                assert(member == SyntaxKind::Constructor);
            }

            newClassPtr->extraMembers.push_back(instanceOfMethod);
        }

        return mlir::success();
    }

    ValueOrLogicalResult mlirGenCreateInterfaceVTableForClass(mlir::Location location, ClassInfo::TypePtr newClassPtr,
                                                              InterfaceInfo::TypePtr newInterfacePtr,
                                                              const GenContext &genContext)
    {
        auto fullClassInterfaceVTableFieldName = interfaceVTableNameForClass(newClassPtr, newInterfacePtr);
        auto existValue = resolveFullNameIdentifier(location, fullClassInterfaceVTableFieldName, true, genContext);
        if (existValue)
        {
            return existValue;
        }

        if (mlir::succeeded(
                mlirGenClassVirtualTableDefinitionForInterface(location, newClassPtr, newInterfacePtr, genContext)))
        {
            return resolveFullNameIdentifier(location, fullClassInterfaceVTableFieldName, true, genContext);
        }

        return mlir::failure();
    }

    ValueOrLogicalResult mlirGenCreateInterfaceVTableForObject(mlir::Location location, mlir_ts::ObjectType objectType,
                                                               InterfaceInfo::TypePtr newInterfacePtr,
                                                               const GenContext &genContext)
    {
        auto fullObjectInterfaceVTableFieldName = interfaceVTableNameForObject(objectType, newInterfacePtr);
        auto existValue = resolveFullNameIdentifier(location, fullObjectInterfaceVTableFieldName, true, genContext);
        if (existValue)
        {
            return existValue;
        }

        if (mlir::succeeded(
                mlirGenObjectVirtualTableDefinitionForInterface(location, objectType, newInterfacePtr, genContext)))
        {
            return resolveFullNameIdentifier(location, fullObjectInterfaceVTableFieldName, true, genContext);
        }

        return mlir::failure();
    }

    StringRef interfaceVTableNameForClass(ClassInfo::TypePtr newClassPtr, InterfaceInfo::TypePtr newInterfacePtr)
    {
        return concat(newClassPtr->fullName, newInterfacePtr->fullName, VTABLE_NAME);
    }

    StringRef interfaceVTableNameForObject(mlir_ts::ObjectType objectType, InterfaceInfo::TypePtr newInterfacePtr)
    {
        std::stringstream ss;
        ss << hash_value(objectType);

        return concat(newInterfacePtr->fullName, ss.str().c_str(), VTABLE_NAME);
    }

    mlir::LogicalResult getInterfaceVirtualTableForObject(mlir::Location location, mlir_ts::TupleType tupleStorageType,
                                                          InterfaceInfo::TypePtr newInterfacePtr,
                                                          SmallVector<VirtualMethodOrFieldInfo> &virtualTable,
                                                          bool suppressErrors = false)
    {
        return mth.getInterfaceVirtualTableForObject(location, tupleStorageType, newInterfacePtr, virtualTable, suppressErrors);
    }

    mlir::LogicalResult mlirGenObjectVirtualTableDefinitionForInterface(mlir::Location location,
                                                                        mlir_ts::ObjectType objectType,
                                                                        InterfaceInfo::TypePtr newInterfacePtr,
                                                                        const GenContext &genContext)
    {

        MLIRCodeLogic mcl(builder);

        auto storeType = objectType.getStorageType();

        // TODO: should object accept only ObjectStorageType?
        if (auto objectStoreType = storeType.dyn_cast<mlir_ts::ObjectStorageType>())
        {
            storeType = mlir_ts::TupleType::get(builder.getContext(), objectStoreType.getFields());
        }

        auto tupleStorageType = mth.convertConstTupleTypeToTupleType(storeType).cast<mlir_ts::TupleType>();

        SmallVector<VirtualMethodOrFieldInfo> virtualTable;
        auto result = getInterfaceVirtualTableForObject(location, tupleStorageType, newInterfacePtr, virtualTable);
        if (mlir::failed(result))
        {
            return result;
        }

        // register global
        auto fullClassInterfaceVTableFieldName = interfaceVTableNameForObject(objectType, newInterfacePtr);
        registerVariable(
            location, fullClassInterfaceVTableFieldName, true, VariableType::Var,
            [&](mlir::Location location, const GenContext &genContext) {
                // build vtable from names of methods

                auto virtTuple = getVirtualTableType(virtualTable);

                mlir::Value vtableValue = builder.create<mlir_ts::UndefOp>(location, virtTuple);
                auto fieldIndex = 0;
                for (auto methodOrField : virtualTable)
                {
                    if (methodOrField.isField)
                    {
                        auto nullObj = builder.create<mlir_ts::NullOp>(location, getNullType());
                        if (!methodOrField.isMissing)
                        {
                            // TODO: test cast result
                            auto objectNull = cast(location, objectType, nullObj, genContext);
                            auto fieldValue = mlirGenPropertyAccessExpression(location, objectNull,
                                                                              methodOrField.fieldInfo.id, genContext);
                            assert(fieldValue);
                            auto fieldRef = mcl.GetReferenceOfLoadOp(fieldValue);

                            LLVM_DEBUG(llvm::dbgs() << "\n!! vtable field: " << methodOrField.fieldInfo.id
                                                    << " type: " << methodOrField.fieldInfo.type
                                                    << " provided data: " << fieldRef << "\n";);

                            if (fieldRef.getType().isa<mlir_ts::BoundRefType>())
                            {
                                fieldRef = cast(location, mlir_ts::RefType::get(methodOrField.fieldInfo.type), fieldRef,
                                                genContext);
                            }
                            else
                            {
                                assert(fieldRef.getType().cast<mlir_ts::RefType>().getElementType() ==
                                       methodOrField.fieldInfo.type);
                            }

                            // insert &(null)->field
                            vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                                location, virtTuple, fieldRef, vtableValue,
                                MLIRHelper::getStructIndex(builder, fieldIndex));
                        }
                        else
                        {
                            // null value, as missing field/method
                            // auto nullObj = builder.create<mlir_ts::NullOp>(location, getNullType());
                            auto negative1 = builder.create<mlir_ts::ConstantOp>(location, builder.getI64Type(),
                                                                                 mth.getI64AttrValue(-1));
                            auto castedNull = cast(location, mlir_ts::RefType::get(methodOrField.fieldInfo.type),
                                                   negative1, genContext);
                            vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                                location, virtTuple, castedNull, vtableValue,
                                MLIRHelper::getStructIndex(builder, fieldIndex));
                        }
                    }
                    else
                    {
                        llvm_unreachable("not implemented yet");
                        /*
                        auto methodConstName = builder.create<mlir_ts::SymbolRefOp>(
                            location, methodOrField.methodInfo.funcOp.getType(),
                            mlir::FlatSymbolRefAttr::get(builder.getContext(),
                        methodOrField.methodInfo.funcOp.getSymName()));

                        vtableValue =
                            builder.create<mlir_ts::InsertPropertyOp>(location, virtTuple, methodConstName, vtableValue,
                                                                      MLIRHelper::getStructIndex(rewriter, fieldIndex));
                        */
                    }

                    fieldIndex++;
                }

                return TypeValueInitType{virtTuple, vtableValue, TypeProvided::Yes};
            },
            genContext);

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassVirtualTableDefinitionForInterface(mlir::Location location,
                                                                       ClassInfo::TypePtr newClassPtr,
                                                                       InterfaceInfo::TypePtr newInterfacePtr,
                                                                       const GenContext &genContext)
    {

        MLIRCodeLogic mcl(builder);

        MethodInfo emptyMethod;
        mlir_ts::FieldInfo emptyFieldInfo;
        // TODO: ...
        auto classStorageType = newClassPtr->classType.getStorageType().cast<mlir_ts::ClassStorageType>();

        llvm::SmallVector<VirtualMethodOrFieldInfo> virtualTable;
        auto result = newInterfacePtr->getVirtualTable(
            virtualTable,
            [&](mlir::Attribute id, mlir::Type fieldType, bool isConditional) -> mlir_ts::FieldInfo {
                auto found = false;
                auto foundField = newClassPtr->findField(id, found);
                if (!found || fieldType != foundField.type)
                {
                    if (!found && !isConditional || found)
                    {
                        emitError(location)
                            << "field type not matching for '" << id << "' for interface '" << newInterfacePtr->fullName
                            << "' in class '" << newClassPtr->fullName << "'";
                    }

                    return emptyFieldInfo;
                }

                return foundField;
            },
            [&](std::string name, mlir_ts::FunctionType funcType, bool isConditional, int interfacePosIndex) -> MethodInfo & {
                auto foundMethodPtr = newClassPtr->findMethod(name);
                if (!foundMethodPtr)
                {
                    // TODO: generate method wrapper for calling new/ctor method
                    if (name == NEW_CTOR_METHOD_NAME)
                    {
                        // TODO: generate method                        
                        foundMethodPtr = generateSynthMethodToCallNewCtor(location, newClassPtr, newInterfacePtr, funcType, interfacePosIndex, genContext);
                    }

                    if (!foundMethodPtr)
                    {
                        if (!isConditional)
                        {
                            emitError(location)
                                << "can't find method '" << name << "' for interface '" << newInterfacePtr->fullName
                                << "' in class '" << newClassPtr->fullName << "'";
                        }

                        return emptyMethod;
                    }
                }

                auto foundMethodFunctionType = foundMethodPtr->funcOp.getFunctionType().cast<mlir_ts::FunctionType>();

                auto result = mth.TestFunctionTypesMatch(funcType, foundMethodFunctionType, 1);
                if (result.result != MatchResultType::Match)
                {
                    emitError(location) << "method signature not matching for '" << name << "'{" << funcType
                                        << "} for interface '" << newInterfacePtr->fullName << "' in class '"
                                        << newClassPtr->fullName << "'"
                                        << " found method: " << foundMethodFunctionType;

                    return emptyMethod;
                }

                return *foundMethodPtr;
            });

        if (mlir::failed(result))
        {
            return result;
        }

        // register global
        auto fullClassInterfaceVTableFieldName = interfaceVTableNameForClass(newClassPtr, newInterfacePtr);
        registerVariable(
            location, fullClassInterfaceVTableFieldName, true, VariableType::Var,
            [&](mlir::Location location, const GenContext &genContext) {
                // build vtable from names of methods

                MLIRCodeLogic mcl(builder);

                auto virtTuple = getVirtualTableType(virtualTable);

                mlir::Value vtableValue = builder.create<mlir_ts::UndefOp>(location, virtTuple);
                auto fieldIndex = 0;
                for (auto methodOrField : virtualTable)
                {
                    if (methodOrField.isField)
                    {
                        auto nullObj = builder.create<mlir_ts::NullOp>(location, getNullType());
                        auto classNull = cast(location, newClassPtr->classType, nullObj, genContext);
                        auto fieldValue = mlirGenPropertyAccessExpression(location, classNull,
                                                                          methodOrField.fieldInfo.id, genContext);
                        auto fieldRef = mcl.GetReferenceOfLoadOp(fieldValue);
                        if (!fieldRef)
                        {
                            emitError(location) << "can't find reference for field: " << methodOrField.fieldInfo.id
                                                << " in interface: " << newInterfacePtr->interfaceType
                                                << " for class: " << newClassPtr->classType;
                            return TypeValueInitType{mlir::Type(), mlir::Value(), TypeProvided::No};
                        }

                        // insert &(null)->field
                        vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                            location, virtTuple, fieldRef, vtableValue,
                            MLIRHelper::getStructIndex(builder, fieldIndex));
                    }
                    else
                    {
                        auto methodConstName = builder.create<mlir_ts::SymbolRefOp>(
                            location, methodOrField.methodInfo.funcOp.getFunctionType(),
                            mlir::FlatSymbolRefAttr::get(builder.getContext(),
                                                         methodOrField.methodInfo.funcOp.getSymName()));

                        vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                            location, virtTuple, methodConstName, vtableValue,
                            MLIRHelper::getStructIndex(builder, fieldIndex));
                    }

                    fieldIndex++;
                }

                return TypeValueInitType{virtTuple, vtableValue, TypeProvided::Yes};
            },
            genContext);

        return mlir::success();
    }

    MethodInfo *generateSynthMethodToCallNewCtor(mlir::Location location, ClassInfo::TypePtr newClassPtr, InterfaceInfo::TypePtr newInterfacePtr, 
                                            mlir_ts::FunctionType funcType, int interfacePosIndex, const GenContext &genContext)
    {
        auto fullClassStaticName = generateSynthMethodToCallNewCtor(location, newClassPtr, newInterfacePtr->fullName, interfacePosIndex, funcType, 1, genContext);
        return newClassPtr->findMethod(fullClassStaticName);
    }    

    std::string generateSynthMethodToCallNewCtor(mlir::Location location, ClassInfo::TypePtr newClassPtr, StringRef sourceOwnerName, int posIndex, 
                                            mlir_ts::FunctionType funcType, int skipFuncParams, const GenContext &genContext)
    {
        auto fullClassStaticName = concat(newClassPtr->fullName, sourceOwnerName, NEW_CTOR_METHOD_NAME, posIndex);

        auto retType = mth.getReturnTypeFromFuncRef(funcType);
        if (!retType)
        {
            return "";
        }

        {
            mlir::OpBuilder::InsertionGuard guard(builder);
            builder.setInsertionPointToStart(theModule.getBody());

            GenContext funcGenContext(genContext);
            funcGenContext.clearScopeVars();
            funcGenContext.thisType = newClassPtr->classType;
            funcGenContext.disableSpreadParams = true;

            auto result = mlirGenFunctionBody(
                location, fullClassStaticName, funcType,
                [&](const GenContext &genContext) {
                    NodeFactory nf(NodeFactoryFlags::None);

                    NodeArray<Expression> argumentsArray;
                    //auto skip = 1;
                    auto skip = skipFuncParams;
                    auto index = 0;
                    for (auto &paramType : funcType.getInputs())
                    {
                        (void)paramType;

                        if (skip-- > 0) 
                        {
                            continue;
                        }

                        std::string paramName("p");
                        paramName += std::to_string(index++);
                        argumentsArray.push_back(nf.createIdentifier(stows(paramName)));
                    }

                    auto newInst = nf.createNewExpression(nf.createToken(SyntaxKind::ThisKeyword), undefined, argumentsArray);
                    auto instRes = mlirGen(newInst, funcGenContext);
                    EXIT_IF_FAILED(instRes);
                    auto instVal = V(instRes);
                    auto castToRet = cast(location, retType, instVal, funcGenContext);
                    EXIT_IF_FAILED(castToRet);
                    auto retVarInfo = symbolTable.lookup(RETURN_VARIABLE_NAME);
                    if (retVarInfo.second)
                    {
                        builder.create<mlir_ts::ReturnValOp>(location, castToRet, retVarInfo.first);
                    }
                    else
                    {
                        return mlir::failure();
                    }

                    return mlir::success();
                },
                funcGenContext, skipFuncParams/*to skip This*/);        

            if (mlir::failed(result))
            {
                return "";
            }
        }

        // register method in info
        if (newClassPtr->getMethodIndex(fullClassStaticName) < 0)
        {
            auto funcOp = mlir_ts::FuncOp::create(location, fullClassStaticName, funcType);

            auto &methodInfos = newClassPtr->methods;
            methodInfos.push_back(
                {fullClassStaticName.str(), funcType, funcOp, true, false, false, -1});
        }        

        return fullClassStaticName.str();
    }

    mlir::LogicalResult mlirGenClassBaseInterfaces(mlir::Location location, ClassInfo::TypePtr newClassPtr,
                                                   const GenContext &genContext)
    {
        for (auto &baseClass : newClassPtr->baseClasses)
        {
            for (auto &implement : baseClass->implements)
            {
                if (implement.processed)
                {
                    continue;
                }

                if (mlir::failed(mlirGenClassVirtualTableDefinitionForInterface(location, newClassPtr,
                                                                                implement.interface, genContext)))
                {
                    return mlir::failure();
                }

                implement.processed = true;
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenClassHeritageClauseImplements(ClassLikeDeclaration classDeclarationAST,
                                                             ClassInfo::TypePtr newClassPtr,
                                                             HeritageClause heritageClause,
                                                             const GenContext &genContext)
    {
        if (heritageClause->token != SyntaxKind::ImplementsKeyword)
        {
            return mlir::success();
        }

        for (auto &implementingType : heritageClause->types)
        {
            if (implementingType->processed)
            {
                continue;
            }

            auto result = mlirGen(implementingType, genContext);
            auto ifaceType = V(result);
            auto success = false;
            TypeSwitch<mlir::Type>(ifaceType.getType())
                .template Case<mlir_ts::InterfaceType>([&](auto interfaceType) {
                    auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
                    assert(interfaceInfo);
                    success = !failed(mlirGenClassVirtualTableDefinitionForInterface(loc(implementingType), newClassPtr,
                                                                                     interfaceInfo, genContext));
                })
                .Default([&](auto type) { llvm_unreachable("not implemented"); });

            if (!success)
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::Type getVirtualTableType(llvm::SmallVector<VirtualMethodOrFieldInfo> &virtualTable)
    {
        llvm::SmallVector<mlir_ts::FieldInfo> fields;
        for (auto vtableRecord : virtualTable)
        {
            if (vtableRecord.isField)
            {
                fields.push_back({vtableRecord.fieldInfo.id, mlir_ts::RefType::get(vtableRecord.fieldInfo.type)});
            }
            else
            {
                fields.push_back({MLIRHelper::TupleFieldName(vtableRecord.methodInfo.name, builder.getContext()),
                                  vtableRecord.methodInfo.funcOp ? vtableRecord.methodInfo.funcOp.getFunctionType()
                                                                 : vtableRecord.methodInfo.funcType});
            }
        }

        auto virtTuple = getTupleType(fields);
        return virtTuple;
    }

    mlir::Type getVirtualTableType(llvm::SmallVector<VirtualMethodOrInterfaceVTableInfo> &virtualTable)
    {
        llvm::SmallVector<mlir_ts::FieldInfo> fields;
        for (auto vtableRecord : virtualTable)
        {
            if (vtableRecord.isInterfaceVTable)
            {
                fields.push_back({MLIRHelper::TupleFieldName(vtableRecord.methodInfo.name, builder.getContext()), getOpaqueType()});
            }
            else
            {
                if (!vtableRecord.isStaticField)
                {
                    fields.push_back({MLIRHelper::TupleFieldName(vtableRecord.methodInfo.name, builder.getContext()),
                                      vtableRecord.methodInfo.funcOp ? vtableRecord.methodInfo.funcOp.getFunctionType()
                                                                     : vtableRecord.methodInfo.funcType});
                }
                else
                {
                    fields.push_back(
                        {vtableRecord.staticFieldInfo.id, mlir_ts::RefType::get(vtableRecord.staticFieldInfo.type)});
                }
            }
        }

        auto virtTuple = getTupleType(fields);
        return virtTuple;
    }

    mlir::LogicalResult mlirGenClassVirtualTableDefinition(mlir::Location location, ClassInfo::TypePtr newClassPtr,
                                                           const GenContext &genContext)
    {
        if (!newClassPtr->getHasVirtualTable() || newClassPtr->isAbstract || newClassPtr->isDeclaration)
        {
            return mlir::success();
        }
       
        // TODO: ...
        llvm::SmallVector<VirtualMethodOrInterfaceVTableInfo> virtualTable;
        newClassPtr->getVirtualTable(virtualTable);

        // TODO: this is pure hack, add ability to clean up created globals while "dummyRun = true"
        // look into examnple with class declaraion in generic function
        auto fullClassVTableFieldName = concat(newClassPtr->fullName, VTABLE_NAME);
        if (fullNameGlobalsMap.count(fullClassVTableFieldName))
        {
            return mlir::success();
        }

        // register global
        auto vtableRegisteredType = registerVariable(
            location, fullClassVTableFieldName, true,
            newClassPtr->isDeclaration ? VariableType::External : VariableType::Var,
            [&](mlir::Location location, const GenContext &genContext) {
                // build vtable from names of methods

                MLIRCodeLogic mcl(builder);

                auto virtTuple = getVirtualTableType(virtualTable);
                if (newClassPtr->isDeclaration)
                {
                    return TypeValueInitType{virtTuple, mlir::Value(), TypeProvided::Yes};
                }

                mlir::Value vtableValue = builder.create<mlir_ts::UndefOp>(location, virtTuple);
                auto fieldIndex = 0;
                for (auto vtRecord : virtualTable)
                {
                    if (vtRecord.isInterfaceVTable)
                    {
                        // TODO: write correct full name for vtable
                        auto fullClassInterfaceVTableFieldName =
                            concat(newClassPtr->fullName, vtRecord.methodInfo.name, VTABLE_NAME);
                        auto interfaceVTableValue =
                            resolveFullNameIdentifier(location, fullClassInterfaceVTableFieldName, true, genContext);

                        if (!interfaceVTableValue)
                        {
                            return TypeValueInitType{mlir::Type(), mlir::Value(), TypeProvided::No};
                        }

                        auto interfaceVTableValueAsAny =
                            cast(location, getOpaqueType(), interfaceVTableValue, genContext);

                        vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                            location, virtTuple, interfaceVTableValueAsAny, vtableValue,
                            MLIRHelper::getStructIndex(builder, fieldIndex++));
                    }
                    else
                    {
                        mlir::Value methodOrFieldNameRef;
                        if (!vtRecord.isStaticField)
                        {
                            if (vtRecord.methodInfo.isAbstract)
                            {
                                emitError(location) << "Abstract method '" << vtRecord.methodInfo.name <<  "' is not implemented in '" << newClassPtr->name << "'";
                                return TypeValueInitType{mlir::Type(), mlir::Value(), TypeProvided::No}; 
                            }

                            methodOrFieldNameRef = builder.create<mlir_ts::SymbolRefOp>(
                                location, vtRecord.methodInfo.funcOp.getFunctionType(),
                                mlir::FlatSymbolRefAttr::get(builder.getContext(),
                                                             vtRecord.methodInfo.funcOp.getSymName()));
                        }
                        else
                        {
                            methodOrFieldNameRef = builder.create<mlir_ts::SymbolRefOp>(
                                location, mlir_ts::RefType::get(vtRecord.staticFieldInfo.type),
                                mlir::FlatSymbolRefAttr::get(builder.getContext(),
                                                             vtRecord.staticFieldInfo.globalVariableName));
                        }

                        vtableValue = builder.create<mlir_ts::InsertPropertyOp>(
                            location, virtTuple, methodOrFieldNameRef, vtableValue,
                            MLIRHelper::getStructIndex(builder, fieldIndex++));
                    }
                }

                return TypeValueInitType{virtTuple, vtableValue, TypeProvided::Yes};
            },
            genContext);

        return (vtableRegisteredType) ? mlir::success() : mlir::failure();
    }

    struct ClassMethodMemberInfo
    {
        ClassMethodMemberInfo(ClassInfo::TypePtr newClassPtr, ClassElement classMember) : newClassPtr(newClassPtr), classMember(classMember)
        {
            isConstructor = classMember == SyntaxKind::Constructor;
            isStatic = hasModifier(classMember, SyntaxKind::StaticKeyword);
            isAbstract = hasModifier(classMember, SyntaxKind::AbstractKeyword);
            //auto isPrivate = hasModifier(classMember, SyntaxKind::PrivateKeyword);
            //auto isProtected = hasModifier(classMember, SyntaxKind::ProtectedKeyword);
            auto isPublic = hasModifier(classMember, SyntaxKind::PublicKeyword);

            isExport = newClassPtr->isExport && (isConstructor || isPublic);
            isImport = newClassPtr->isImport && (isConstructor || isPublic);
            isForceVirtual = (classMember->internalFlags & InternalFlags::ForceVirtual) == InternalFlags::ForceVirtual;
    #ifdef ALL_METHODS_VIRTUAL
            isForceVirtual |= !isConstructor;
    #endif
            isVirtual = isForceVirtual;
        };

        bool isFunctionLike()
        {
            return classMember == SyntaxKind::MethodDeclaration || isConstructor || classMember == SyntaxKind::GetAccessor ||
                classMember == SyntaxKind::SetAccessor;
        }

        std::string getName()
        {
            return propertyName.empty() ? methodName : propertyName;
        }

        StringRef getFuncName()
        {
            return funcOp.getName();
        }

        mlir_ts::FunctionType getFuncType()
        {
            return funcOp.getFunctionType();
        }

        void setFuncOp(mlir_ts::FuncOp funcOp_)
        {
            funcOp = funcOp_;
        }

        void registerClassMethodMember()
        {
            auto &methodInfos = newClassPtr->methods;

            if (newClassPtr->getMethodIndex(methodName) < 0)
            {
                methodInfos.push_back(
                    {methodName, getFuncType(), funcOp, isStatic, isAbstract || isVirtual, isAbstract, -1});
            }

            if (propertyName.size() > 0)
            {
                addAccessor();
            }
        }

        void addAccessor()
        {
            auto &accessorInfos = newClassPtr->accessors;

            auto accessorIndex = newClassPtr->getAccessorIndex(propertyName);
            if (accessorIndex < 0)
            {
                accessorInfos.push_back({propertyName, {}, {}, isStatic, isVirtual, isAbstract});
                accessorIndex = newClassPtr->getAccessorIndex(propertyName);
            }

            assert(accessorIndex >= 0);

            if (classMember == SyntaxKind::GetAccessor)
            {
                newClassPtr->accessors[accessorIndex].get = funcOp;
            }
            else if (classMember == SyntaxKind::SetAccessor)
            {
                newClassPtr->accessors[accessorIndex].set = funcOp;
            }
        }

        ClassInfo::TypePtr newClassPtr;
        ClassElement classMember;
        std::string methodName;
        std::string propertyName;        
        bool isConstructor;
        bool isStatic;
        bool isAbstract;
        bool isExport;
        bool isImport;
        bool isForceVirtual;
        bool isVirtual;

        mlir_ts::FuncOp funcOp;
    };

    mlir::LogicalResult mlirGenClassMethodMember(ClassLikeDeclaration classDeclarationAST,
                                                 ClassInfo::TypePtr newClassPtr, ClassElement classMember,
                                                 const GenContext &genContext)
    {
        if (classMember->processed)
        {
            return mlir::success();
        }

        ClassMethodMemberInfo classMethodMemberInfo(newClassPtr, classMember);
        if (!classMethodMemberInfo.isFunctionLike())
        {
            return mlir::success();
        }

        auto location = loc(classMember);
        auto funcLikeDeclaration = classMember.as<FunctionLikeDeclarationBase>();
        getMethodNameOrPropertyName(
            funcLikeDeclaration, 
            classMethodMemberInfo.methodName, 
            classMethodMemberInfo.propertyName, 
            genContext);

        if (classMethodMemberInfo.methodName.empty())
        {
            llvm_unreachable("not implemented");
            return mlir::failure();
        }

        if (classMethodMemberInfo.isAbstract && !newClassPtr->isAbstract)
        {
            emitError(location) << "Can't use abstract member '" 
                << classMethodMemberInfo.getName()
                << "' in non-abstract class '" << newClassPtr->fullName << "'";
            return mlir::failure();
        }

        classMember->parent = classDeclarationAST;

        auto funcGenContext = GenContext(genContext);
        funcGenContext.clearScopeVars();
        funcGenContext.thisType = newClassPtr->classType;
        if (classMethodMemberInfo.isConstructor)
        {
            if (classMethodMemberInfo.isStatic && !genContext.allowPartialResolve)
            {
                createGlobalConstructor(classMember, genContext);
            }

            // adding missing statements
            generateConstructorStatements(classDeclarationAST, classMethodMemberInfo.isStatic, funcGenContext);
        }

        // process dynamic import
        // TODO: why ".new" is virtual method?
        if (newClassPtr->isDynamicImport 
            && (classMethodMemberInfo.isStatic || classMethodMemberInfo.isConstructor || classMethodMemberInfo.methodName == NEW_METHOD_NAME))
        {
            return mlirGenClassMethodMemberDynamicImport(classMethodMemberInfo, genContext);
        }

        if (classMethodMemberInfo.isExport)
        {
            funcLikeDeclaration->internalFlags |= InternalFlags::DllExport;
        }

        if (classMethodMemberInfo.isImport)
        {
            funcLikeDeclaration->internalFlags |= InternalFlags::DllImport;
            //MLIRHelper::addDecoratorIfNotPresent(funcLikeDeclaration, DLL_IMPORT);
        }

        auto [result, funcOp, funcName, isGeneric] =
            mlirGenFunctionLikeDeclaration(funcLikeDeclaration, funcGenContext);
        if (mlir::failed(result))
        {
            return mlir::failure();
        }

        if (funcOp)
        {
            classMethodMemberInfo.setFuncOp(funcOp);
            funcLikeDeclaration->processed = true;
            classMethodMemberInfo.registerClassMethodMember();
            return mlir::success();
        }

        return registerGenericClassMethod(classMethodMemberInfo, genContext);
    }

    mlir::LogicalResult registerGenericClassMethod(ClassMethodMemberInfo &classMethodMemberInfo, const GenContext &genContext)
    {
        // if funcOp is null, means it is generic
        if (classMethodMemberInfo.funcOp)
        {
            return mlir::success();
        }

        auto funcLikeDeclaration = classMethodMemberInfo.classMember.as<FunctionLikeDeclarationBase>();

        // if it is generic - remove virtual flag
        if (classMethodMemberInfo.isForceVirtual)
        {
            classMethodMemberInfo.isVirtual = false;
        }

        if (classMethodMemberInfo.isStatic || (!classMethodMemberInfo.isAbstract && !classMethodMemberInfo.isVirtual))
        {
            if (classMethodMemberInfo.newClassPtr->getGenericMethodIndex(classMethodMemberInfo.methodName) < 0)
            {
                llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
                if (mlir::failed(
                        processTypeParameters(funcLikeDeclaration->typeParameters, typeParameters, genContext)))
                {
                    return mlir::failure();
                }

                // TODO: review it, ignore in case of ArrowFunction,
                auto [result, funcProto] =
                    getFuncArgTypesOfGenericMethod(funcLikeDeclaration, typeParameters, false, genContext);
                if (mlir::failed(result))
                {
                    return mlir::failure();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! registered generic method: " << classMethodMemberInfo.methodName
                                        << ", type: " << funcProto->getFuncType() << "\n";);

                auto &genericMethodInfos = classMethodMemberInfo.newClassPtr->staticGenericMethods;

                // this is generic method
                // the main logic will use Global Generic Functions
                genericMethodInfos.push_back({
                    classMethodMemberInfo.methodName, 
                    funcProto->getFuncType(), 
                    funcProto, 
                    classMethodMemberInfo.isStatic});
            }

            return mlir::success();
        }

        emitError(loc(classMethodMemberInfo.classMember)) << "virtual generic methods in class are not allowed";
        return mlir::failure();
    }

    mlir::LogicalResult mlirGenClassMethodMemberDynamicImport(ClassMethodMemberInfo &classMethodMemberInfo, const GenContext &genContext)
    {
        auto funcLikeDeclaration = classMethodMemberInfo.classMember.as<FunctionLikeDeclarationBase>();

        auto [funcOp, funcProto, result, isGeneric] =
            mlirGenFunctionPrototype(funcLikeDeclaration, genContext);
        if (mlir::failed(result))
        {
            // in case of ArrowFunction without params and receiver is generic function as well
            return mlir::failure();
        }

        classMethodMemberInfo.setFuncOp(funcOp);

        auto location = loc(funcLikeDeclaration);
        if (mlir::succeeded(mlirGenFunctionLikeDeclarationDynamicImport(location, funcOp, funcProto->getNameWithoutNamespace(), genContext)))
        {
            // no need to generate method in code
            funcLikeDeclaration->processed = true;
            classMethodMemberInfo.registerClassMethodMember();
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult createGlobalConstructor(ClassElement classMember, const GenContext &genContext)
    {
        auto location = loc(classMember);

        auto parentModule = theModule;

        MLIRCodeLogicHelper mclh(builder, location);

        builder.setInsertionPointToStart(parentModule.getBody());
        mclh.seekLast(parentModule.getBody());

        auto funcName = getNameOfFunction(classMember, genContext);

        builder.create<mlir_ts::GlobalConstructorOp>(location, StringRef(std::get<0>(funcName)));

        return mlir::success();
    }

    mlir::LogicalResult generateConstructorStatements(ClassLikeDeclaration classDeclarationAST, bool staticConstructor,
                                                      const GenContext &genContext)
    {
        NodeFactory nf(NodeFactoryFlags::None);

        for (auto &classMember : classDeclarationAST->members)
        {
            auto isStatic = hasModifier(classMember, SyntaxKind::StaticKeyword);
            if (classMember == SyntaxKind::PropertyDeclaration)
            {
                if (isStatic != staticConstructor)
                {
                    continue;
                }

                auto propertyDeclaration = classMember.as<PropertyDeclaration>();
                if (!propertyDeclaration->initializer)
                {
                    continue;
                }

                if (staticConstructor)
                {
                    auto isConst = isConstValue(propertyDeclaration->initializer, genContext);
                    if (isConst)
                    {
                        continue;
                    }
                }

                auto memberNamePtr = MLIRHelper::getName(propertyDeclaration->name, stringAllocator);
                if (memberNamePtr.empty())
                {
                    llvm_unreachable("not implemented");
                    return mlir::failure();
                }

                auto _this = nf.createIdentifier(S(THIS_NAME));
                auto _name = nf.createIdentifier(stows(std::string(memberNamePtr)));
                auto _this_name = nf.createPropertyAccessExpression(_this, _name);
                auto _this_name_equal = nf.createBinaryExpression(_this_name, nf.createToken(SyntaxKind::EqualsToken),
                                                                  propertyDeclaration->initializer);
                auto expr_statement = nf.createExpressionStatement(_this_name_equal);

                const_cast<GenContext &>(genContext).generatedStatements.push_back(expr_statement.as<Statement>());
            }

            if (classMember == SyntaxKind::Constructor)
            {
                if (isStatic != staticConstructor)
                {
                    continue;
                }

                auto constructorDeclaration = classMember.as<ConstructorDeclaration>();
                for (auto &parameter : constructorDeclaration->parameters)
                {
                    auto isPublic = hasModifier(parameter, SyntaxKind::PublicKeyword);
                    auto isProtected = hasModifier(parameter, SyntaxKind::ProtectedKeyword);
                    auto isPrivate = hasModifier(parameter, SyntaxKind::PrivateKeyword);

                    if (!(isPublic || isProtected || isPrivate))
                    {
                        continue;
                    }

                    auto propertyNamePtr = MLIRHelper::getName(parameter->name, stringAllocator);
                    if (propertyNamePtr.empty())
                    {
                        llvm_unreachable("not implemented");
                        return mlir::failure();
                    }

                    auto _this = nf.createIdentifier(stows(THIS_NAME));
                    auto _name = nf.createIdentifier(stows(std::string(propertyNamePtr)));
                    auto _this_name = nf.createPropertyAccessExpression(_this, _name);
                    auto _this_name_equal =
                        nf.createBinaryExpression(_this_name, nf.createToken(SyntaxKind::EqualsToken), _name);
                    auto expr_statement = nf.createExpressionStatement(_this_name_equal);

                    const_cast<GenContext &>(genContext).generatedStatements.push_back(expr_statement.as<Statement>());
                }
            }
        }

        return mlir::success();
    }

    bool isConstValue(Expression expr, const GenContext &genContext)
    {
        auto isConst = false;
        evaluate(
            expr, [&](mlir::Value val) { isConst = isConstValue(val); }, genContext);
        return isConst;
    }

    mlir::LogicalResult registerGenericInterface(InterfaceDeclaration interfaceDeclarationAST,
                                                 const GenContext &genContext)
    {
        auto name = MLIRHelper::getName(interfaceDeclarationAST->name);
        if (!name.empty())
        {
            auto namePtr = StringRef(name).copy(stringAllocator);
            auto fullNamePtr = getFullNamespaceName(namePtr);
            if (fullNameGenericInterfacesMap.count(fullNamePtr))
            {
                return mlir::success();
            }

            llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
            if (mlir::failed(
                    processTypeParameters(interfaceDeclarationAST->typeParameters, typeParameters, genContext)))
            {
                return mlir::failure();
            }

            GenericInterfaceInfo::TypePtr newGenericInterfacePtr = std::make_shared<GenericInterfaceInfo>();
            newGenericInterfacePtr->name = namePtr;
            newGenericInterfacePtr->fullName = fullNamePtr;
            newGenericInterfacePtr->typeParams = typeParameters;
            newGenericInterfacePtr->interfaceDeclaration = interfaceDeclarationAST;
            newGenericInterfacePtr->elementNamespace = currentNamespace;

            mlirGenInterfaceType(newGenericInterfacePtr, genContext);

            getGenericInterfacesMap().insert({namePtr, newGenericInterfacePtr});
            fullNameGenericInterfacesMap.insert(fullNamePtr, newGenericInterfacePtr);

            return mlir::success();
        }

        return mlir::failure();
    }

    void appendSpecializedTypeNames(std::string &name, NodeArray<TypeParameterDeclaration> typeParams,
                                    const GenContext &genContext)
    {
        name.append("<");
        auto next = false;
        for (auto typeParam : typeParams)
        {
            if (next)
            {
                name.append(",");
            }

            auto type = getType(typeParam, genContext);
            if (type)
            {
                llvm::raw_string_ostream s(name);
                s << type;
            }
            else
            {
                // TODO: finish it
                // name.append(MLIRHelper::getName(typeParam));
            }

            next = true;
        }

        name.append(">");
    }

    template <typename T> std::string getNameWithArguments(T declarationAST, const GenContext &genContext)
    {
        auto name = MLIRHelper::getName(declarationAST->name);
        if (name.empty())
        {
            auto [attr, result] = getNameFromComputedPropertyName(declarationAST->name, genContext);
            if (mlir::failed(result))
            {
                return nullptr;
            }

            if (auto strAttr = attr.template dyn_cast_or_null<mlir::StringAttr>())
            {
                name = strAttr.getValue();
            }
        }

        if (name.empty())
        {
            if (declarationAST == SyntaxKind::ArrowFunction)
            {
                if (!genContext.receiverName.empty())
                {
                    name = genContext.receiverName.str();
                }
                else
                {
                    name = MLIRHelper::getAnonymousName(loc_check(declarationAST), ".af");
                }
            }
            else if (declarationAST == SyntaxKind::FunctionExpression)
            {
                name = MLIRHelper::getAnonymousName(loc_check(declarationAST), ".fe");
            }
            else if (declarationAST == SyntaxKind::ClassExpression)
            {
                name = MLIRHelper::getAnonymousName(loc_check(declarationAST), ".ce");
            }
            else if (declarationAST == SyntaxKind::Constructor)
            {
                name = CONSTRUCTOR_NAME;
            }
            else if (declarationAST == SyntaxKind::ConstructSignature)
            {
                name = NEW_CTOR_METHOD_NAME;
            }
            else
            {
                name = MLIRHelper::getAnonymousName(loc_check(declarationAST));
            }
        }

        if (!name.empty() && genContext.typeParamsWithArgs.size() && declarationAST->typeParameters.size())
        {
            appendSpecializedTypeNames(name, declarationAST->typeParameters, genContext);
        }

        return name;
    }

    std::string getSpecializedInterfaceName(GenericInterfaceInfo::TypePtr geneticInterfacePtr,
                                            const GenContext &genContext)
    {
        auto name = geneticInterfacePtr->fullName.str();
        if (genContext.typeParamsWithArgs.size())
        {
            appendSpecializedTypeNames(name, geneticInterfacePtr->typeParams, genContext);
        }

        return name;
    }

    mlir_ts::InterfaceType getSpecializationInterfaceType(GenericInterfaceInfo::TypePtr genericInterfacePtr,
                                                          const GenContext &genContext)
    {
        auto fullSpecializedInterfaceName = getSpecializedInterfaceName(genericInterfacePtr, genContext);
        auto interfaceInfoType = getInterfaceInfoByFullName(fullSpecializedInterfaceName);
        assert(interfaceInfoType);
        interfaceInfoType->originInterfaceType = genericInterfacePtr->interfaceType;
        return interfaceInfoType->interfaceType;
    }

    InterfaceInfo::TypePtr mlirGenInterfaceInfo(InterfaceDeclaration interfaceDeclarationAST, bool &declareInterface,
                                                const GenContext &genContext)
    {
        auto name = getNameWithArguments(interfaceDeclarationAST, genContext);
        return mlirGenInterfaceInfo(name, declareInterface, genContext);
    }

    InterfaceInfo::TypePtr mlirGenInterfaceInfo(const std::string &name, bool &declareInterface,
                                                const GenContext &genContext)
    {
        declareInterface = false;

        auto namePtr = StringRef(name).copy(stringAllocator);
        auto fullNamePtr = getFullNamespaceName(namePtr);

        InterfaceInfo::TypePtr newInterfacePtr;
        if (fullNameInterfacesMap.count(fullNamePtr))
        {
            newInterfacePtr = fullNameInterfacesMap.lookup(fullNamePtr);
            getInterfacesMap().insert({namePtr, newInterfacePtr});
            declareInterface = !newInterfacePtr->interfaceType;
        }
        else
        {
            // register class
            newInterfacePtr = std::make_shared<InterfaceInfo>();
            newInterfacePtr->name = namePtr;
            newInterfacePtr->fullName = fullNamePtr;

            getInterfacesMap().insert({namePtr, newInterfacePtr});
            fullNameInterfacesMap.insert(fullNamePtr, newInterfacePtr);
            declareInterface = true;
        }

        if (declareInterface && mlir::succeeded(mlirGenInterfaceType(newInterfacePtr, genContext)))
        {
            newInterfacePtr->typeParamsWithArgs = genContext.typeParamsWithArgs;
        }

        return newInterfacePtr;
    }

    mlir::LogicalResult mlirGenInterfaceHeritageClauseExtends(InterfaceDeclaration interfaceDeclarationAST,
                                                              InterfaceInfo::TypePtr newInterfacePtr,
                                                              HeritageClause heritageClause, bool declareClass,
                                                              const GenContext &genContext)
    {
        if (heritageClause->token != SyntaxKind::ExtendsKeyword)
        {
            return mlir::success();
        }

        for (auto &extendsType : heritageClause->types)
        {
            if (extendsType->processed)
            {
                continue;
            }

            auto result = mlirGen(extendsType, genContext);
            EXIT_IF_FAILED(result);
            auto ifaceType = V(result);
            auto success = false;
            TypeSwitch<mlir::Type>(ifaceType.getType())
                .template Case<mlir_ts::InterfaceType>([&](auto interfaceType) {
                    auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
                    if (interfaceInfo)
                    {
                        newInterfacePtr->extends.push_back({-1, interfaceInfo});
                        success = true;
                        extendsType->processed = true;
                    }
                })
                .template Case<mlir_ts::TupleType>([&](auto tupleType) {
                    llvm::SmallVector<mlir_ts::FieldInfo> destTupleFields;
                    if (mlir::succeeded(mth.getFields(tupleType, destTupleFields)))
                    {
                        success = true;
                        for (auto field : destTupleFields)
                            success &= mlir::succeeded(mlirGenInterfaceAddFieldMember(newInterfacePtr, field.id, field.type, field.isConditional));
                    }
                })
                .Default([&](auto type) { llvm_unreachable("not implemented"); });

            if (!success)
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGen(InterfaceDeclaration interfaceDeclarationAST, const GenContext &genContext)
    {
        // do not proceed for Generic Interfaces for declaration
        if (interfaceDeclarationAST->typeParameters.size() > 0 && genContext.typeParamsWithArgs.size() == 0)
        {
            return registerGenericInterface(interfaceDeclarationAST, genContext);
        }

        auto declareInterface = false;
        auto newInterfacePtr = mlirGenInterfaceInfo(interfaceDeclarationAST, declareInterface, genContext);
        if (!newInterfacePtr)
        {
            return mlir::failure();
        }

        // do not process specialized interface second time;
        if (!declareInterface && interfaceDeclarationAST->typeParameters.size() > 0 &&
            genContext.typeParamsWithArgs.size() > 0)
        {
            return mlir::success();
        }

        auto location = loc(interfaceDeclarationAST);

        auto ifaceGenContext = GenContext(genContext);
        ifaceGenContext.thisType = newInterfacePtr->interfaceType;

        for (auto &heritageClause : interfaceDeclarationAST->heritageClauses)
        {
            if (mlir::failed(mlirGenInterfaceHeritageClauseExtends(interfaceDeclarationAST, newInterfacePtr,
                                                                   heritageClause, declareInterface, genContext)))
            {
                return mlir::failure();
            }
        }

        newInterfacePtr->recalcOffsets();

        // clear all flags
        for (auto &interfaceMember : interfaceDeclarationAST->members)
        {
            interfaceMember->processed = false;
        }

        // add methods when we have classType
        auto notResolved = 0;
        do
        {
            auto lastTimeNotResolved = notResolved;
            notResolved = 0;

            for (auto &interfaceMember : interfaceDeclarationAST->members)
            {
                if (mlir::failed(mlirGenInterfaceMethodMember(interfaceDeclarationAST, newInterfacePtr, interfaceMember,
                                                              declareInterface, ifaceGenContext)))
                {
                    notResolved++;
                }
            }

            // repeat if not all resolved
            if (lastTimeNotResolved > 0 && lastTimeNotResolved == notResolved)
            {
                // interface can depend on other interface declarations
                // theModule.emitError("can't resolve dependencies in intrerface: ") << newInterfacePtr->name;
                return mlir::failure();
            }

        } while (notResolved > 0);

        // add to export if any
        if (auto hasExport = getExportModifier(interfaceDeclarationAST))
        {
            addInterfaceDeclarationToExport(interfaceDeclarationAST);
        }

        return mlir::success();
    }

    template <typename T> mlir::LogicalResult mlirGenInterfaceType(T newInterfacePtr, const GenContext &genContext)
    {
        if (newInterfacePtr)
        {
            newInterfacePtr->interfaceType = getInterfaceType(newInterfacePtr->fullName);
            return mlir::success();
        }

        return mlir::failure();
    }

    mlir::LogicalResult mlirGenInterfaceAddFieldMember(InterfaceInfo::TypePtr newInterfacePtr, mlir::Attribute fieldId, mlir::Type typeIn, bool isConditional, bool declareInterface = true)
    {
        auto &fieldInfos = newInterfacePtr->fields;
        auto type = typeIn;

        // fix type for fields with FuncType
        if (auto hybridFuncType = type.dyn_cast<mlir_ts::HybridFunctionType>())
        {

            auto funcType = getFunctionType(hybridFuncType.getInputs(), hybridFuncType.getResults(), hybridFuncType.isVarArg());
            type = mth.getFunctionTypeAddingFirstArgType(funcType, getOpaqueType());
        }
        else if (auto funcType = type.dyn_cast<mlir_ts::FunctionType>())
        {

            type = mth.getFunctionTypeAddingFirstArgType(funcType, getOpaqueType());
        }

        if (mth.isNoneType(type))
        {
            LLVM_DEBUG(dbgs() << "\n!! interface field: " << fieldId << " FAILED\n");
            return mlir::failure();
        }

        if (declareInterface || newInterfacePtr->getFieldIndex(fieldId) == -1)
        {
            fieldInfos.push_back({fieldId, type, isConditional, newInterfacePtr->getNextVTableMemberIndex()});
        }

        return mlir::success();
    }

    mlir::LogicalResult mlirGenInterfaceMethodMember(InterfaceDeclaration interfaceDeclarationAST,
                                                     InterfaceInfo::TypePtr newInterfacePtr,
                                                     TypeElement interfaceMember, bool declareInterface,
                                                     const GenContext &genContext)
    {
        if (interfaceMember->processed)
        {
            return mlir::success();
        }

        auto location = loc(interfaceMember);

        auto &methodInfos = newInterfacePtr->methods;

        mlir::Value initValue;
        mlir::Attribute fieldId;
        mlir::Type type;
        StringRef memberNamePtr;

        MLIRCodeLogic mcl(builder);

        SyntaxKind kind = interfaceMember;
        if (kind == SyntaxKind::PropertySignature)
        {
            // property declaration
            auto propertySignature = interfaceMember.as<PropertySignature>();
            auto isConditional = !!propertySignature->questionToken;

            fieldId = TupleFieldName(propertySignature->name, genContext);

            auto [type, init, typeProvided] = getTypeAndInit(propertySignature, genContext);
            if (!type)
            {
                return mlir::failure();
            }

            if (mlir::failed(mlirGenInterfaceAddFieldMember(newInterfacePtr, fieldId, type, isConditional, declareInterface)))
            {
                return mlir::failure();
            }
        }
        else if (kind == SyntaxKind::MethodSignature || kind == SyntaxKind::ConstructSignature 
                || kind == SyntaxKind::IndexSignature || kind == SyntaxKind::CallSignature)
        {
            auto methodSignature = interfaceMember.as<MethodSignature>();
            auto isConditional = !!methodSignature->questionToken;

            newInterfacePtr->hasNew |= kind == SyntaxKind::ConstructSignature;

            std::string methodName;
            std::string propertyName;
            getMethodNameOrPropertyName(methodSignature, methodName, propertyName, genContext);

            if (methodName.empty())
            {
                llvm_unreachable("not implemented");
                return mlir::failure();
            }

            if (methodSignature->typeParameters.size() > 0)
            {
                emitError(location) << "Generic method '" << methodName << "' in the interface is not allowed";
                return mlir::failure();
            }

            interfaceMember->parent = interfaceDeclarationAST;

            auto funcGenContext = GenContext(genContext);
            funcGenContext.clearScopeVars();
            funcGenContext.thisType = newInterfacePtr->interfaceType;

            auto res = mlirGenFunctionSignaturePrototype(methodSignature, true, funcGenContext);
            auto funcType = std::get<1>(res);

            if (!funcType)
            {
                return mlir::failure();
            }

            if (llvm::any_of(funcType.getInputs(), [&](mlir::Type type) { return !type; }))
            {
                return mlir::failure();
            }

            if (llvm::any_of(funcType.getResults(), [&](mlir::Type type) { return !type; }))
            {
                return mlir::failure();
            }

            methodSignature->processed = true;

            if (declareInterface || newInterfacePtr->getMethodIndex(methodName) == -1)
            {
                methodInfos.push_back(
                    {methodName, funcType, isConditional, newInterfacePtr->getNextVTableMemberIndex()});
            }
        }
        else
        {
            llvm_unreachable("not implemented");
        }

        return mlir::success();
    }

    std::string getNameForMethod(SignatureDeclarationBase methodSignature, const GenContext &genContext)
    {
        auto [attr, result] = getNameFromComputedPropertyName(methodSignature->name, genContext);
        if (mlir::failed(result))
        {
            return nullptr;
        }

        if (attr)
        {
            if (auto strAttr = attr.dyn_cast<mlir::StringAttr>())
            {
                return strAttr.getValue().str();
            }
            else
            {
                llvm_unreachable("not implemented");
            }
        }

        return MLIRHelper::getName(methodSignature->name);
    }

    mlir::LogicalResult getMethodNameOrPropertyName(SignatureDeclarationBase methodSignature, std::string &methodName,
                                                    std::string &propertyName, const GenContext &genContext)
    {
        SyntaxKind kind = methodSignature;
        if (kind == SyntaxKind::Constructor)
        {
            auto isStatic = hasModifier(methodSignature, SyntaxKind::StaticKeyword);
            if (isStatic)
            {
                methodName = std::string(STATIC_CONSTRUCTOR_NAME);
            }
            else
            {
                methodName = std::string(CONSTRUCTOR_NAME);
            }
        }
        else if (kind == SyntaxKind::ConstructSignature)
        {
            methodName = std::string(NEW_CTOR_METHOD_NAME);
        }
        else if (kind == SyntaxKind::IndexSignature)
        {
            methodName = std::string(INDEX_ACCESS_FIELD_NAME);
        }
        else if (kind == SyntaxKind::CallSignature)
        {
            methodName = std::string(CALL_FIELD_NAME);
        }
        else if (kind == SyntaxKind::GetAccessor)
        {
            propertyName = getNameForMethod(methodSignature, genContext);
            methodName = std::string("get_") + propertyName;
        }
        else if (kind == SyntaxKind::SetAccessor)
        {
            propertyName = getNameForMethod(methodSignature, genContext);
            methodName = std::string("set_") + propertyName;
        }
        else
        {
            methodName = getNameForMethod(methodSignature, genContext);
        }

        return mlir::success();
    }

    mlir::Block* prepareTempModule()
    {
        if (tempEntryBlock)
        {
            theModule = tempModule;
            return tempEntryBlock;
        }

        auto location = loc(TextRange());

        theModule = tempModule = mlir::ModuleOp::create(location, mlir::StringRef("temp_module"));

        // we need to add temporary block
        auto tempFuncType =
            mlir_ts::FunctionType::get(builder.getContext(), ArrayRef<mlir::Type>(), ArrayRef<mlir::Type>());
        tempFuncOp = mlir_ts::FuncOp::create(location, ".tempfunc", tempFuncType);

        tempEntryBlock = tempFuncOp.addEntryBlock();

        return tempEntryBlock;
    }

    void clearTempModule()
    {
        if (tempEntryBlock)
        {
            tempEntryBlock->dropAllDefinedValueUses();
            tempEntryBlock->dropAllUses();
            tempEntryBlock->dropAllReferences();
            tempEntryBlock->erase();

            tempFuncOp.erase();
            tempModule.erase();

            tempEntryBlock = nullptr;
        }
    }

    mlir::Type evaluate(Expression expr, const GenContext &genContext)
    {
        // we need to add temporary block
        mlir::Type result;
        if (expr)
        {
            evaluate(
                expr, [&](mlir::Value val) { result = val.getType(); }, genContext);
        }

        return result;
    }

    void evaluate(Expression expr, std::function<void(mlir::Value)> func, const GenContext &genContext)
    {
        if (!expr)
        {
            return;
        }

        // TODO: sometimes we need errors, sometimes, not,
        // we need to ignore errors;
        //mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
        //});

        auto location = loc(expr);

        // module
        auto savedModule = theModule;

        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);

            SymbolTableScopeT varScope(symbolTable);

            builder.setInsertionPointToStart(prepareTempModule());

            GenContext evalGenContext(genContext);
            evalGenContext.allowPartialResolve = true;
            evalGenContext.funcOp = tempFuncOp;
            auto result = mlirGen(expr, evalGenContext);
            auto initValue = V(result);
            if (initValue)
            {
                func(initValue);
            }
        }

        theModule = savedModule;
    }

    mlir::Value evaluatePropertyValue(mlir::Value exprValue, const std::string &propertyName, const GenContext &genContext)
    {
        // we need to ignore errors;
        mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
        });

        auto location = exprValue.getLoc();

        mlir::Value initValue;

        // module
        auto savedModule = theModule;

        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);
            builder.setInsertionPointToStart(prepareTempModule());

            GenContext evalGenContext(genContext);
            evalGenContext.allowPartialResolve = true;
            evalGenContext.funcOp = tempFuncOp;
            auto result = mlirGenPropertyAccessExpression(location, exprValue, propertyName, evalGenContext);
            initValue = V(result);
        }

        theModule = savedModule;

        return initValue;
    }    

    // TODO: rewrite code to get rid of the following method, write method to calculate type of field, we have method mth.getFieldTypeByFieldName
    mlir::Type evaluateProperty(mlir::Value exprValue, const std::string &propertyName, const GenContext &genContext)
    {
        auto value = evaluatePropertyValue(exprValue, propertyName, genContext);
        return value ? value.getType() : mlir::Type();
    }

    mlir::Type evaluateProperty(Expression expression, const std::string &propertyName, const GenContext &genContext)
    {
        auto result = mlirGen(expression, genContext);
        if (result.failed_or_no_value())
        {
            return mlir::Type();
        }

        auto exprValue = V(result);

        auto value = evaluatePropertyValue(exprValue, propertyName, genContext);
        return value ? value.getType() : mlir::Type();
    }

    mlir::Type evaluateElementAccess(mlir::Location location, mlir::Value expression, bool isConditionalAccess, const GenContext &genContext)
    {
        // we need to ignore errors;
        mlir::ScopedDiagnosticHandler diagHandler(builder.getContext(), [&](mlir::Diagnostic &diag) {
        });

        mlir::Type resultType;

        // module
        auto savedModule = theModule;

        {
            mlir::OpBuilder::InsertionGuard insertGuard(builder);
            builder.setInsertionPointToStart(prepareTempModule());

            GenContext evalGenContext(genContext);
            evalGenContext.allowPartialResolve = true;
            auto indexVal = builder.create<mlir_ts::ConstantOp>(location, mth.getStructIndexType(),
                                    mth.getStructIndexAttrValue(0));
            auto result = mlirGenElementAccess(location, expression, indexVal, isConditionalAccess, evalGenContext);
            auto initValue = V(result);
            if (initValue)
            {
                resultType = initValue.getType();
            }
        }

        theModule = savedModule;

        return resultType;
    }    

    ValueOrLogicalResult castTupleToTuple(mlir::Location location, mlir::Value value, mlir_ts::TupleType srcTupleType, 
        ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields, const GenContext &genContext)
    {
        SmallVector<mlir::Value> values;
        
        auto index = -1;
        for (auto fieldInfo : fields)
        {
            index++;
            LLVM_DEBUG(llvm::dbgs() << "\n!! processing #" << index << " field [" << fieldInfo.id << "]\n";);           

            if (fieldInfo.id == mlir::Attribute() || (index < srcTupleType.size() && srcTupleType.getFieldInfo(index).id == mlir::Attribute()))
            {
                if (index >= srcTupleType.size() && fieldInfo.type.isa<mlir_ts::OptionalType>())
                {
                    // add undefined value
                    auto undefVal = builder.create<mlir_ts::OptionalUndefOp>(location, fieldInfo.type);
                    values.push_back(undefVal);
                    continue;
                }

                MLIRPropertyAccessCodeLogic cl(builder, location, value, builder.getI32IntegerAttr(index));
                auto value = cl.Tuple(srcTupleType, true);
                VALIDATE(value, location)
                values.push_back(value);
            }
            else
            {
                // access by field name
                auto fieldIndex = srcTupleType.getIndex(fieldInfo.id);
                if (fieldIndex < 0)
                {
                    if (fieldInfo.type.isa<mlir_ts::OptionalType>())
                    {
                        // add undefined value
                        auto undefVal = builder.create<mlir_ts::OptionalUndefOp>(location, fieldInfo.type);
                        values.push_back(undefVal);
                        continue;
                    }

                    emitError(location)
                        << "field " << fieldInfo.id << " can't be found in tuple '" << srcTupleType << "'";
                    return mlir::failure();
                }                

                MLIRPropertyAccessCodeLogic cl(builder, location, value, fieldInfo.id);
                // TODO: implement conditional
                auto propertyAccess = mlirGenPropertyAccessExpressionLogic(location, value, false, cl, genContext); 
                EXIT_IF_FAILED_OR_NO_VALUE(propertyAccess)

                auto value = V(propertyAccess);
                if (value.getType() != fieldInfo.type)
                {
                    CAST(value, location, fieldInfo.type, value, genContext)
                }

                values.push_back(value);
            }
        }

        if (fields.size() != values.size())
        {
            return mlir::failure();
        }

        SmallVector<::mlir::typescript::FieldInfo> fieldsForTuple;
        fieldsForTuple.append(fields.begin(), fields.end());
        return V(builder.create<mlir_ts::CreateTupleOp>(location, getTupleType(fieldsForTuple), values));
    }    

    ValueOrLogicalResult generatingStaticNewCtorForClass(mlir::Location location, ClassInfo::TypePtr classInfo, int posIndex, const GenContext &genContext)
    {
        if (auto classConstrMethodInfo = classInfo->findMethod(CONSTRUCTOR_NAME))
        {
            auto funcWithReturnClass = getFunctionType(
                classConstrMethodInfo->funcType.getInputs().slice(1) /*to remove this*/, 
                {classInfo->classType}, 
                classConstrMethodInfo->funcType.isVarArg());
            auto foundNewCtoreStaticMethodFullName = generateSynthMethodToCallNewCtor(location, classInfo, classInfo->fullName, posIndex, funcWithReturnClass, 0, genContext);
            if (foundNewCtoreStaticMethodFullName.empty())
            {
                return mlir::failure();
            }

            auto symbOp = builder.create<mlir_ts::SymbolRefOp>(
                location, funcWithReturnClass,
                mlir::FlatSymbolRefAttr::get(builder.getContext(), foundNewCtoreStaticMethodFullName));
        
            return V(symbOp);
        }
        else
        {
            emitError(location) << "constructor can't be found";
            return mlir::failure();
        }
    }

    ValueOrLogicalResult castClassToTuple(mlir::Location location, mlir::Value value, mlir_ts::ClassType classType, 
        ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields, const GenContext &genContext)
    {
        auto classInfo = getClassInfoByFullName(classType.getName().getValue());
        assert(classInfo);            

        auto newCtorAttr = MLIRHelper::TupleFieldName(NEW_CTOR_METHOD_NAME, builder.getContext());
        SmallVector<mlir::Value> values;
        auto posIndex = -1;
        for (auto fieldInfo : fields)
        {
            posIndex++;
            auto foundField = false;                                        
            auto classFieldInfo = classInfo->findField(fieldInfo.id, foundField);
            if (!foundField)
            {
                // TODO: generate method wrapper for calling new/ctor method
                if (fieldInfo.id == newCtorAttr)
                {
                    auto newCtorSymbOp = generatingStaticNewCtorForClass(location, classInfo, posIndex, genContext);
                    EXIT_IF_FAILED_OR_NO_VALUE(newCtorSymbOp)
                    values.push_back(newCtorSymbOp);
                    continue;
                }

                emitError(location)
                    << "field " << fieldInfo.id << " can't be found in class '" << classInfo->fullName << "'";
                return mlir::failure();
            }                

            MLIRPropertyAccessCodeLogic cl(builder, location, value, fieldInfo.id);
            // TODO: implemenet conditional
            mlir::Value propertyAccess = mlirGenPropertyAccessExpressionLogic(location, value, false, cl, genContext); 
            if (propertyAccess)
            {
                values.push_back(propertyAccess);
            }
        }

        if (fields.size() != values.size())
        {
            return mlir::failure();
        }        

        SmallVector<::mlir::typescript::FieldInfo> fieldsForTuple;
        fieldsForTuple.append(fields.begin(), fields.end());
        return V(builder.create<mlir_ts::CreateTupleOp>(location, getTupleType(fieldsForTuple), values));
    }

    ValueOrLogicalResult castInterfaceToTuple(mlir::Location location, mlir::Value value, mlir_ts::InterfaceType interfaceType, 
        ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields, const GenContext &genContext)
    {
        auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
        assert(interfaceInfo);            

        SmallVector<mlir::Value> values;
        for (auto fieldInfo : fields)
        {
            auto totalOffset = 0;                                        
            auto classFieldInfo = interfaceInfo->findField(fieldInfo.id, totalOffset);
            if (totalOffset < 0)
            {
                emitError(location)
                    << "field '" << fieldInfo.id << "' can't be found "
                    << "' in interface '" << interfaceInfo->fullName << "'";
                return mlir::failure();
            }                

            MLIRPropertyAccessCodeLogic cl(builder, location, value, fieldInfo.id);
            // TODO: implemenet conditional
            mlir::Value propertyAccess = mlirGenPropertyAccessExpressionLogic(location, value, classFieldInfo->isConditional, cl, genContext); 
            if (propertyAccess)
            {
                values.push_back(propertyAccess);
            }
        }

        if (fields.size() != values.size())
        {
            return mlir::failure();
        }          

        SmallVector<::mlir::typescript::FieldInfo> fieldsForTuple;
        fieldsForTuple.append(fields.begin(), fields.end());
        return V(builder.create<mlir_ts::CreateTupleOp>(location, getTupleType(fieldsForTuple), values));
    }

    ValueOrLogicalResult cast(mlir::Location location, mlir::Type type, mlir::Value value, const GenContext &genContext)
    {
        if (!type)
        {
            return mlir::failure();
        }

        if (type == value.getType())
        {
            return value;
        }

        auto valueType = value.getType();

        LLVM_DEBUG(llvm::dbgs() << "\n!! cast [" << valueType << "] -> [" << type << "]"
                                << "\n";);

        if (auto litType = type.dyn_cast<mlir_ts::LiteralType>())
        {
            if (auto valLitType = valueType.dyn_cast<mlir_ts::LiteralType>())
            {
                if (litType.getValue() != valLitType.getValue())
                {
                    emitError(location, "can't cast from literal type: '") << valLitType.getValue() << "' to '" << litType.getValue() << "'";
                    return mlir::failure(); 
                }
            }
        }

        // class to string
        if (auto stringType = type.dyn_cast<mlir_ts::StringType>())
        {
            if (auto classType = valueType.dyn_cast<mlir_ts::ClassType>())
            {
                auto res = mlirGenCallThisMethod(location, value, "get_" SYMBOL_TO_STRING_TAG, undefined, undefined, genContext);
                if (!res.failed_or_no_value())
                {
                    return res;
                }
                
                return mlirGenCallThisMethod(location, value, TO_STRING, undefined, undefined, genContext);
            }
        }

        // <???> to interface
        if (auto interfaceType = type.dyn_cast<mlir_ts::InterfaceType>())
        {
            if (auto classType = valueType.dyn_cast<mlir_ts::ClassType>())
            {
                auto result = mlirGenPropertyAccessExpression(location, value, VTABLE_NAME, genContext);
                auto vtableAccess = V(result);

                auto classInfo = getClassInfoByFullName(classType.getName().getValue());
                assert(classInfo);

                auto implementIndex = classInfo->getImplementIndex(interfaceType.getName().getValue());
                if (implementIndex >= 0)
                {
                    auto interfaceVirtTableIndex = classInfo->implements[implementIndex].virtualIndex;

                    assert(genContext.allowPartialResolve || interfaceVirtTableIndex >= 0);

                    auto interfaceVTablePtr = builder.create<mlir_ts::VTableOffsetRefOp>(
                        location, mth.getInterfaceVTableType(interfaceType), vtableAccess, interfaceVirtTableIndex);

                    auto newInterface = builder.create<mlir_ts::NewInterfaceOp>(
                        location, mlir::TypeRange{interfaceType}, value, interfaceVTablePtr);
                    return V(newInterface);
                }

                // create interface vtable from current class
                auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
                assert(interfaceInfo);

                if (auto createdInterfaceVTableForClass =
                        mlirGenCreateInterfaceVTableForClass(location, classInfo, interfaceInfo, genContext))
                {
                    LLVM_DEBUG(llvm::dbgs() << "\n!!"
                                            << "@ created interface:" << createdInterfaceVTableForClass << "\n";);
                    auto newInterface = builder.create<mlir_ts::NewInterfaceOp>(
                        location, mlir::TypeRange{interfaceType}, value, createdInterfaceVTableForClass);

                    return V(newInterface);
                }

                emitError(location) << "type: " << classType << " missing interface: " << interfaceType;
                return mlir::failure();
            }

            // tuple to interface
            if (auto constTupleType = valueType.dyn_cast<mlir_ts::ConstTupleType>())
            {
                return castTupleToInterface(location, value, constTupleType, interfaceType, genContext);
            }

            if (auto tupleType = valueType.dyn_cast<mlir_ts::TupleType>())
            {
                return castTupleToInterface(location, value, tupleType, interfaceType, genContext);
            }

            // object to interface
            if (auto objectType = valueType.dyn_cast<mlir_ts::ObjectType>())
            {
                return castObjectToInterface(location, value, objectType, interfaceType, genContext);
            }
        }

        // const tuple to object
        if (auto srcConstTupleType = valueType.dyn_cast<mlir_ts::ConstTupleType>())
        {
            ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields;
            if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
            {
                fields = tupleType.getFields();
                return castTupleToTuple(location, value, mth.convertConstTupleTypeToTupleType(srcConstTupleType), fields, genContext);
            }
            else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
            {
                fields = constTupleType.getFields();
                return castTupleToTuple(location, value, mth.convertConstTupleTypeToTupleType(srcConstTupleType), fields, genContext);
            }
        }

        // tuple to object
        if (auto srcTupleType = valueType.dyn_cast<mlir_ts::TupleType>())
        {
            ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields;
            if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
            {
                fields = tupleType.getFields();
                return castTupleToTuple(location, value, srcTupleType, fields, genContext);
            }
            else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
            {
                fields = constTupleType.getFields();
                return castTupleToTuple(location, value, srcTupleType, fields, genContext);
            }
        }

        // class to object
        if (auto classType = valueType.dyn_cast<mlir_ts::ClassType>())
        {
            ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields;
            if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
            {
                fields = tupleType.getFields();
                return castClassToTuple(location, value, classType, fields, genContext);
            }
            else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
            {
                fields = constTupleType.getFields();
                return castClassToTuple(location, value, classType, fields, genContext);
            }
        }

        // interface to object
        if (auto interfaceType = valueType.dyn_cast<mlir_ts::InterfaceType>())
        {
            ::llvm::ArrayRef<::mlir::typescript::FieldInfo> fields;
            if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
            {
                fields = tupleType.getFields();
                return castInterfaceToTuple(location, value, interfaceType, fields, genContext);
            }
            else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
            {
                fields = constTupleType.getFields();
                return castInterfaceToTuple(location, value, interfaceType, fields, genContext);
            }
        }

        // optional
        // TODO: it is in CastLogic as well, review usage and remove from here
        // but if optional points to interface then it will not work
        // example: from path.ts
        // %6 = ts.Cast %4 : !ts.const_tuple<{"key",!ts.string},{"prev",!ts.undefined},{"typename",!ts.undefined}> to !ts.optional<!ts.iface<@Path>>
        if (auto optType = type.dyn_cast<mlir_ts::OptionalType>())
        {
            if (valueType == getUndefinedType())
            {
                return V(builder.create<mlir_ts::OptionalUndefOp>(location, optType));
            }
            else
            {
                CAST_A(valueCasted, location, optType.getElementType(), value, genContext);
                return V(builder.create<mlir_ts::OptionalValueOp>(location, optType, valueCasted));
            }
        }

        if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            mlir::Type baseType;
            if (mth.isUnionTypeNeedsTag(unionType, baseType))
            {
                auto types = unionType.getTypes();
                if (std::find(types.begin(), types.end(), valueType) == types.end())
                {
                    // find which type we can cast to
                    for (auto subType : types)
                    {
                        if (mth.canCastFromTo(valueType, subType))
                        {
                            CAST(value, location, subType, value, genContext);
                            return V(builder.create<mlir_ts::CastOp>(location, type, value));                    
                        }
                    }
                }
                else
                {
                    return V(builder.create<mlir_ts::CastOp>(location, type, value));                    
                }
            }
        }

        if (auto constType = type.dyn_cast<mlir_ts::ConstType>())
        {
            // TODO: we can't convert array to const array

            auto currType = valueType;
            if (auto refType = currType.dyn_cast<mlir_ts::RefType>())
            {
                type = refType.getElementType();        
            }
            else if (auto tupleType = currType.dyn_cast<mlir_ts::TupleType>())
            {
                type = mth.convertTupleTypeToConstTupleType(tupleType);                
            }
            else
            {
                return value;
            }
        }

        // unboxing
        if (auto anyType = valueType.dyn_cast<mlir_ts::AnyType>())
        {
            if (type.isa<mlir_ts::NumberType>() 
                || type.isa<mlir_ts::BooleanType>()
                || type.isa<mlir_ts::StringType>()
                || type.isa<mlir::IntegerType>()
                || type.isa<mlir::Float32Type>()
                || type.isa<mlir::Float64Type>()
                || type.isa<mlir_ts::ClassType>())
            {
                return castFromAny(location, type, value, genContext);
            }
        }

        // toPrimitive
        if ((type.isa<mlir_ts::StringType>() 
            || type.isa<mlir_ts::NumberType>() 
            || type.isa<mlir_ts::BigIntType>() 
            || type.isa<mlir_ts::BooleanType>() 
            || type.isa<mlir_ts::UndefinedType>() 
            || type.isa<mlir_ts::SymbolType>() 
            || type.isa<mlir_ts::NullType>())
            && (valueType.isa<mlir_ts::ClassType>()
                || valueType.isa<mlir_ts::ClassStorageType>()
                || valueType.isa<mlir_ts::ObjectType>()
                || valueType.isa<mlir_ts::InterfaceType>()
                || valueType.isa<mlir_ts::TupleType>()
                || valueType.isa<mlir_ts::ConstTupleType>()))
        {
            // check if we need to call toPrimitive
            if (auto toPrimitiveType = evaluateProperty(value, SYMBOL_TO_PRIMITIVE, genContext))
            {
                NodeFactory nf(NodeFactoryFlags::None);
                Expression hint;

                TypeSwitch<mlir::Type>(type)
                    .template Case<mlir_ts::StringType>([&](auto) {
                        hint = nf.createStringLiteral(S("string"));
                    })
                    .template Case<mlir_ts::NumberType>([&](auto) {
                        hint = nf.createStringLiteral(S("number"));
                    })
                    .template Case<mlir_ts::BigIntType>([&](auto) {
                        hint = nf.createStringLiteral(S("bigint"));
                    })
                    .template Case<mlir_ts::BooleanType>([&](auto) {
                        hint = nf.createStringLiteral(S("boolean"));
                    })
                    .template Case<mlir_ts::UndefinedType>([&](auto) {
                        hint = nf.createStringLiteral(S(UNDEFINED_NAME));
                    })
                    .template Case<mlir_ts::SymbolType>([&](auto) {
                        hint = nf.createStringLiteral(S("symbol"));
                    })
                    .template Case<mlir_ts::NullType>([&](auto) {
                        hint = nf.createStringLiteral(S("null"));
                    })
                    .Default([&](auto type) {});

                auto callResult = mlirGenCallThisMethod(location, value, SYMBOL_TO_PRIMITIVE, undefined, {hint}, genContext);
                EXIT_IF_FAILED(callResult);
                auto castValue = cast(location, type, V(callResult), genContext);
                EXIT_IF_FAILED_OR_NO_VALUE(castValue);
                return castValue;
            }        
        }

        // cast ext method to bound method
        if (auto extFuncType = valueType.dyn_cast<mlir_ts::ExtensionFunctionType>())
        {
            if (auto hybridFuncType = type.dyn_cast<mlir_ts::HybridFunctionType>())
            {
                auto boundFunc = createBoundMethodFromExtensionMethod(location, value.getDefiningOp<mlir_ts::CreateExtensionFunctionOp>());
                return V(builder.create<mlir_ts::CastOp>(location, type, boundFunc));
            }

            if (auto boundFuncType = type.dyn_cast<mlir_ts::BoundFunctionType>())
            {
                auto boundFunc = createBoundMethodFromExtensionMethod(location, value.getDefiningOp<mlir_ts::CreateExtensionFunctionOp>());
                return V(builder.create<mlir_ts::CastOp>(location, type, boundFunc));
            }            
        }

        // opaque to hybrid func
        if (auto opaqueType = valueType.dyn_cast<mlir_ts::OpaqueType>())
        {
            if (auto funcType = type.dyn_cast<mlir_ts::FunctionType>())
            {
                return V(builder.create<mlir_ts::CastOp>(location, type, value));
            }            

            if (auto hybridFuncType = type.dyn_cast<mlir_ts::HybridFunctionType>())
            {
                auto funcValue = builder.create<mlir_ts::CastOp>(
                    location, 
                    mlir_ts::FunctionType::get(builder.getContext(), hybridFuncType.getInputs(), hybridFuncType.getResults(), hybridFuncType.isVarArg()), 
                    value);
                return V(builder.create<mlir_ts::CastOp>(location, type, funcValue));
            }
        }

        return V(builder.create<mlir_ts::CastOp>(location, type, value));
    }

    ValueOrLogicalResult castFromAny(mlir::Location location, mlir::Type type, mlir::Value value, const GenContext &genContext)
    {
        // info, we add "_" extra as scanner append "_" in front of "__";
        auto funcName = "___as";

        if (!existGenericFunctionMap(funcName))
        {
            auto src = S("function __as<T>(a: any) : T \
                { \
                    if (typeof a == 'number') return <T>a; \
                    if (typeof a == 'string') return <T>a; \
                    if (typeof a == 'i32') return <T>a; \
                    if (typeof a == 'class') if (a instanceof T) return <T>a; \
                    return <T>null; \
                } \
                ");

            if (mlir::failed(parsePartialStatements(src)))
            {
                assert(false);
                return mlir::failure();
            }
        }

        auto funcResult = resolveIdentifier(location, funcName, genContext);

        assert(funcResult);

        GenContext funcCallGenContext(genContext);
        // "_" added to name
        funcCallGenContext.typeAliasMap.insert({".TYPE_ALIAS", type});

        SmallVector<mlir::Value, 4> operands;
        operands.push_back(value);

        NodeFactory nf(NodeFactoryFlags::None);
        return mlirGenCallExpression(location, funcResult, { nf.createTypeReferenceNode(nf.createIdentifier(S(".TYPE_ALIAS")).as<Node>()) }, operands, funcCallGenContext);
    }

    mlir::Value castTupleToInterface(mlir::Location location, mlir::Value in, mlir::Type tupleTypeIn,
                                     mlir_ts::InterfaceType interfaceType, const GenContext &genContext)
    {

        auto tupleType = mth.convertConstTupleTypeToTupleType(tupleTypeIn);
        auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());

        auto inEffective = in;

        if (mlir::failed(mth.canCastTupleToInterface(tupleType.cast<mlir_ts::TupleType>(), interfaceInfo)))
        {
            SmallVector<mlir_ts::FieldInfo> fields;
            if (mlir::failed(interfaceInfo->getTupleTypeFields(fields, builder.getContext())))
            {
                return mlir::Value();
            }

            auto newInterfaceTupleType = getTupleType(fields);
            CAST(inEffective, location, newInterfaceTupleType, inEffective, genContext);
            tupleType = newInterfaceTupleType;
        }

        // TODO: finish it, what to finish it? maybe optimization not to create extra object?
        // convert Tuple to Object
        auto objType = mlir_ts::ObjectType::get(tupleType);
        auto valueAddr = builder.create<mlir_ts::NewOp>(location, mlir_ts::ValueRefType::get(tupleType), builder.getBoolAttr(false));
        builder.create<mlir_ts::StoreOp>(location, inEffective, valueAddr);
        auto inCasted = builder.create<mlir_ts::CastOp>(location, objType, valueAddr);

        return castObjectToInterface(location, inCasted, objType, interfaceInfo, genContext);
    }

    mlir::Value castObjectToInterface(mlir::Location location, mlir::Value in, mlir_ts::ObjectType objType,
                                    mlir_ts::InterfaceType interfaceType, const GenContext &genContext)
    {
        auto interfaceInfo = getInterfaceInfoByFullName(interfaceType.getName().getValue());
        return castObjectToInterface(location, in, objType, interfaceInfo, genContext);
    }

    mlir::Value castObjectToInterface(mlir::Location location, mlir::Value in, mlir_ts::ObjectType objType,
                                    InterfaceInfo::TypePtr interfaceInfo, const GenContext &genContext)
    {
        if (auto createdInterfaceVTableForObject =
                mlirGenCreateInterfaceVTableForObject(location, objType, interfaceInfo, genContext))
        {

            LLVM_DEBUG(llvm::dbgs() << "\n!!"
                                    << "@ created interface:" << createdInterfaceVTableForObject << "\n";);
            auto newInterface = builder.create<mlir_ts::NewInterfaceOp>(location, mlir::TypeRange{interfaceInfo->interfaceType},
                                                                        in, createdInterfaceVTableForObject);

            return newInterface;
        }    

        return mlir::Value();    
    }    

    mlir_ts::CreateBoundFunctionOp createBoundMethodFromExtensionMethod(mlir::Location location, mlir_ts::CreateExtensionFunctionOp createExtentionFunction)
    {
        auto extFuncType = createExtentionFunction.getType();
        auto boundFuncVal = builder.create<mlir_ts::CreateBoundFunctionOp>(
            location, 
            getBoundFunctionType(
                extFuncType.getInputs(), 
                extFuncType.getResults(), 
                extFuncType.isVarArg()), 
            createExtentionFunction.getThisVal(), createExtentionFunction.getFunc());            

        return boundFuncVal;
    }

    mlir::Type getType(Node typeReferenceAST, const GenContext &genContext)
    {
        auto kind = (SyntaxKind)typeReferenceAST;
        if (kind == SyntaxKind::BooleanKeyword)
        {
            return getBooleanType();
        }
        else if (kind == SyntaxKind::NumberKeyword)
        {
            return getNumberType();
        }
        else if (kind == SyntaxKind::BigIntKeyword)
        {
            return getBigIntType();
        }
        else if (kind == SyntaxKind::StringKeyword)
        {
            return getStringType();
        }
        else if (kind == SyntaxKind::VoidKeyword)
        {
            return getVoidType();
        }
        else if (kind == SyntaxKind::FunctionType)
        {
            return getFunctionType(typeReferenceAST.as<FunctionTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::ConstructorType)
        {
            // TODO: do I need to add flag to FunctionType to show that this is ConstructorType?
            return getConstructorType(typeReferenceAST.as<ConstructorTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::CallSignature)
        {
            return getCallSignature(typeReferenceAST.as<CallSignatureDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::MethodSignature)
        {
            return getMethodSignature(typeReferenceAST.as<MethodSignature>(), genContext);
        }
        else if (kind == SyntaxKind::ConstructSignature)
        {
            return getConstructSignature(typeReferenceAST.as<ConstructSignatureDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::IndexSignature)
        {
            return getIndexSignature(typeReferenceAST.as<IndexSignatureDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::TupleType)
        {
            return getTupleType(typeReferenceAST.as<TupleTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::TypeLiteral)
        {
            // TODO: review it, I think it should be ObjectType
            // return getObjectType(getTupleType(typeReferenceAST.as<TypeLiteralNode>(), genContext));
            return getTupleType(typeReferenceAST.as<TypeLiteralNode>(), genContext);
        }
        else if (kind == SyntaxKind::ArrayType)
        {
            return getArrayType(typeReferenceAST.as<ArrayTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::UnionType)
        {
            return getUnionType(typeReferenceAST.as<UnionTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::IntersectionType)
        {
            return getIntersectionType(typeReferenceAST.as<IntersectionTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::ParenthesizedType)
        {
            return getParenthesizedType(typeReferenceAST.as<ParenthesizedTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::LiteralType)
        {
            return getLiteralType(typeReferenceAST.as<LiteralTypeNode>());
        }
        else if (kind == SyntaxKind::TypeReference)
        {
            return getTypeByTypeReference(typeReferenceAST.as<TypeReferenceNode>(), genContext);
        }
        else if (kind == SyntaxKind::TypeQuery)
        {
            return getTypeByTypeQuery(typeReferenceAST.as<TypeQueryNode>(), genContext);
        }
        else if (kind == SyntaxKind::ObjectKeyword)
        {
            return getObjectType(getAnyType());
        }
        else if (kind == SyntaxKind::AnyKeyword)
        {
            return getAnyType();
        }
        else if (kind == SyntaxKind::UnknownKeyword)
        {
            // TODO: do I need to have special type?
            return getUnknownType();
        }
        else if (kind == SyntaxKind::SymbolKeyword)
        {
            return getSymbolType();
        }
        else if (kind == SyntaxKind::UndefinedKeyword)
        {
            return getUndefinedType();
        }
        else if (kind == SyntaxKind::TypePredicate)
        {
            // in runtime it is boolean (it is needed to track types)
            return getTypePredicateType(typeReferenceAST.as<TypePredicateNode>(), genContext);
        }
        else if (kind == SyntaxKind::ThisType)
        {
            if (genContext.thisType)
            {
                return genContext.thisType;
            }
            
            NodeFactory nf(NodeFactoryFlags::None);
            auto thisType = evaluate(nf.createToken(SyntaxKind::ThisKeyword), genContext);
            LLVM_DEBUG(llvm::dbgs() << "\n!! this type from variable: [" << thisType << "]\n";);
            return thisType;
        }
        else if (kind == SyntaxKind::Unknown)
        {
            return getUnknownType();
        }
        else if (kind == SyntaxKind::ConditionalType)
        {
            return getConditionalType(typeReferenceAST.as<ConditionalTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::TypeOperator)
        {
            return getTypeOperator(typeReferenceAST.as<TypeOperatorNode>(), genContext);
        }
        else if (kind == SyntaxKind::IndexedAccessType)
        {
            return getIndexedAccessType(typeReferenceAST.as<IndexedAccessTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::MappedType)
        {
            return getMappedType(typeReferenceAST.as<MappedTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::TemplateLiteralType)
        {
            return getTemplateLiteralType(typeReferenceAST.as<TemplateLiteralTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::TypeParameter)
        {
            return getResolveTypeParameter(typeReferenceAST.as<TypeParameterDeclaration>(), genContext);
        }
        else if (kind == SyntaxKind::InferType)
        {
            return getInferType(typeReferenceAST.as<InferTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::OptionalType)
        {
            return getOptionalType(typeReferenceAST.as<OptionalTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::RestType)
        {
            return getRestType(typeReferenceAST.as<RestTypeNode>(), genContext);
        }
        else if (kind == SyntaxKind::NeverKeyword)
        {
            return getNeverType();
        }

        llvm_unreachable("not implemented type declaration");
        // return getAnyType();
    }

    mlir::Type getInferType(InferTypeNode inferTypeNodeAST, const GenContext &genContext)
    {
        auto type = getType(inferTypeNodeAST->typeParameter, genContext);
        auto inferType = getInferType(type);

        LLVM_DEBUG(llvm::dbgs() << "\n!! infer type [" << inferType << "]\n";);

        // TODO: review function 'extends' in MLIRTypeHelper with the same logic adding infer types to context
        auto &typeParamsWithArgs = const_cast<GenContext &>(genContext).typeParamsWithArgs;
        mth.appendInferTypeToContext(type, inferType, typeParamsWithArgs);

        return inferType;
    }

    mlir::Type getResolveTypeParameter(StringRef typeParamName, bool defaultType, const GenContext &genContext)
    {
        // to build generic type with generic names
        auto foundAlias = genContext.typeAliasMap.find(typeParamName);
        if (foundAlias != genContext.typeAliasMap.end())
        {
            auto type = (*foundAlias).getValue();

            LLVM_DEBUG(llvm::dbgs() << "\n!! type gen. param as alias [" << typeParamName << "] -> [" << type
                                    << "]\n";);

            return type;
        }

        auto found = genContext.typeParamsWithArgs.find(typeParamName);
        if (found != genContext.typeParamsWithArgs.end())
        {
            auto type = (*found).getValue().second;

            LLVM_DEBUG(llvm::dbgs() << "\n!! type gen. param [" << typeParamName << "] -> [" << type << "]\n";);

            return type;
        }

        if (defaultType)
        {
            // unresolved generic
            return getNamedGenericType(typeParamName);
        }

        // name is not found
        return mlir::Type();
    }

    mlir::Type getResolveTypeParameter(TypeParameterDeclaration typeParameterDeclaration, const GenContext &genContext)
    {
        auto name = MLIRHelper::getName(typeParameterDeclaration->name);
        if (name.empty())
        {
            llvm_unreachable("not implemented");
            return mlir::Type();
        }

        return getResolveTypeParameter(name, true, genContext);
    }

    mlir::Type getTypeByTypeName(Node node, const GenContext &genContext)
    {
        if (node == SyntaxKind::Identifier)
        {
            auto name = MLIRHelper::getName(node);
            return resolveTypeByName(loc(node), name, genContext);
        }        
        else if (node == SyntaxKind::QualifiedName)
        {
            // TODO: it seems namespace access, can u optimize it somehow?
            auto result = mlirGen(node.as<QualifiedName>(), genContext);
            if (result.failed_or_no_value())
            {
                return mlir::Type();
            }

            auto val = V(result);
            return val.getType();
        }
        
        llvm_unreachable("not implemented");
    }

    mlir::Type getFirstTypeFromTypeArguments(NodeArray<TypeNode> &typeArguments, const GenContext &genContext)
    {
        return getType(typeArguments->front(), genContext);
    }

    mlir::Type getSecondTypeFromTypeArguments(NodeArray<TypeNode> &typeArguments, const GenContext &genContext)
    {
        return getType(typeArguments[1], genContext);
    }

    std::pair<mlir::LogicalResult, IsGeneric> zipTypeParameterWithArgument(
        mlir::Location location, llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> &pairs,
        const ts::TypeParameterDOM::TypePtr &typeParam, mlir::Type type, bool noExtendTest,
        const GenContext &genContext, bool mergeTypes = false, bool arrayMerge = false)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! assigning generic type: " << typeParam->getName() << " type: " << type
                                << "\n";);

        if (mth.isNoneType(type))
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! skip. failed.\n";);
            return {mlir::failure(), IsGeneric::False};
        }

        if (type.isa<mlir_ts::NamedGenericType>())
        {
            pairs.insert({typeParam->getName(), std::make_pair(typeParam, type)});
            return {mlir::success(), IsGeneric::True};
        }

        if (!noExtendTest && typeParam->hasConstraint())
        {
            // we need to add current type into context to be able to use it in resolving "extends" constraints
            GenContext constraintGenContext(genContext);
            constraintGenContext.typeParamsWithArgs.insert({typeParam->getName(), std::make_pair(typeParam, type)});

            auto constraintType = getType(typeParam->getConstraint(), constraintGenContext);
            if (!constraintType)
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! skip. failed. should be resolved later\n";);
                return {mlir::failure(), IsGeneric::False};
            }

            auto extendsResult = mth.extendsType(type, constraintType, pairs);
            if (extendsResult != ExtendsResult::True)
            {
                // special case when we work with generic type(which are not specialized yet)
                if (mth.isGenericType(type))
                {
                    pairs.insert({typeParam->getName(), std::make_pair(typeParam, type)});
                    LLVM_DEBUG(llvm::dbgs() << "Extends result: " << type << " (because of generic).";);
                    return {mlir::success(), IsGeneric::True};                    
                }

                if (extendsResult == ExtendsResult::Any)
                {
                    pairs.insert({typeParam->getName(), std::make_pair(typeParam, getAnyType())});
                    LLVM_DEBUG(llvm::dbgs() << "Extends result: any.";);
                    return {mlir::success(), IsGeneric::False};                    
                }                

                if (extendsResult == ExtendsResult::Never)
                {
                    pairs.insert({typeParam->getName(), std::make_pair(typeParam, getNeverType())});
                    LLVM_DEBUG(llvm::dbgs() << "Extends result: never.";);
                    return {mlir::success(), IsGeneric::False};                    
                }

                LLVM_DEBUG(llvm::dbgs() << "Type " << type << " does extend "
                                        << constraintType << ".";);

                emitWarning(location, "") << "Type " << type << " does not satisfy the constraint "
                                        << constraintType << ".";

                return {mlir::failure(), IsGeneric::False};
            }
        }

        auto name = typeParam->getName();
        auto existType = pairs.lookup(name);
        if (existType.second)
        {
            if (existType.second != type)
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! replacing existing type for: " << name
                                        << " exist type: " << existType.second << " new type: " << type << "\n";);

                if (!existType.second.isa<mlir_ts::NamedGenericType>() && mergeTypes)
                {
                    auto merged = false;
                    if (arrayMerge)
                    {
                        type = mth.arrayMergeType(existType.second, type, merged);
                    }
                    else
                    {
                        type = mth.mergeType(existType.second, type, merged);
                    }

                    LLVM_DEBUG(llvm::dbgs() << "\n!! result (after merge) type: " << type << "\n";);
                }

                // TODO: Do I need to join types?
                pairs[name] = std::make_pair(typeParam, type);
            }
        }
        else
        {
            pairs.insert({name, std::make_pair(typeParam, type)});
        }

        return {mlir::success(), IsGeneric::False};
    }

    std::pair<mlir::LogicalResult, IsGeneric> zipTypeParametersWithArguments(
        mlir::Location location, llvm::ArrayRef<TypeParameterDOM::TypePtr> typeParams, llvm::ArrayRef<mlir::Type> typeArgs,
        llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> &pairs, const GenContext &genContext)
    {
        auto anyNamedGenericType = IsGeneric::False;
        auto argsCount = typeArgs.size();
        for (auto index = 0; index < typeParams.size(); index++)
        {
            auto &typeParam = typeParams[index];
            auto isDefault = false;
            auto type = index < argsCount
                            ? typeArgs[index]
                            : (isDefault = true, typeParam->hasDefault() 
                                ? getType(typeParam->getDefault(), genContext) 
                                : typeParam->hasConstraint() 
                                    ? getType(typeParam->getConstraint(), genContext) 
                                    : mlir::Type());
            if (!type)
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            auto [result, hasNamedGenericType] =
                zipTypeParameterWithArgument(location, pairs, typeParam, type, isDefault, genContext);
            if (mlir::failed(result))
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            if (hasNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasNamedGenericType;
            }
        }

        return {mlir::success(), anyNamedGenericType};
    }


    std::pair<mlir::LogicalResult, IsGeneric> zipTypeParametersWithArguments(
        mlir::Location location, llvm::ArrayRef<TypeParameterDOM::TypePtr> typeParams, NodeArray<TypeNode> typeArgs,
        llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> &pairs, const GenContext &genContext)
    {
        auto anyNamedGenericType = IsGeneric::False;
        auto argsCount = typeArgs.size();
        for (auto index = 0; index < typeParams.size(); index++)
        {
            auto &typeParam = typeParams[index];
            auto isDefault = false;
            auto type = index < argsCount
                            ? getType(typeArgs[index], genContext)
                            : (isDefault = true, typeParam->hasDefault() 
                                ? getType(typeParam->getDefault(), genContext) 
                                : typeParam->hasConstraint() 
                                    ? getType(typeParam->getConstraint(), genContext) 
                                    : mlir::Type());
            if (!type)
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            auto [result, hasNamedGenericType] =
                zipTypeParameterWithArgument(location, pairs, typeParam, type, isDefault, genContext);
            if (mlir::failed(result))
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            if (hasNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasNamedGenericType;
            }
        }

        return {mlir::success(), anyNamedGenericType};
    }

    std::pair<mlir::LogicalResult, IsGeneric> zipTypeParametersWithArgumentsNoDefaults(
        mlir::Location location, llvm::ArrayRef<TypeParameterDOM::TypePtr> typeParams, NodeArray<TypeNode> typeArgs,
        llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> &pairs, const GenContext &genContext)
    {
        auto anyNamedGenericType = IsGeneric::False;
        auto argsCount = typeArgs.size();
        for (auto index = 0; index < typeParams.size(); index++)
        {
            auto &typeParam = typeParams[index];
            auto isDefault = false;
            auto type = index < argsCount
                            ? getType(typeArgs[index], genContext)
                            : (isDefault = true,
                               typeParam->hasDefault() 
                               ? getType(typeParam->getDefault(), genContext) 
                               : typeParam->hasConstraint() 
                                    ? getType(typeParam->getConstraint(), genContext) 
                                    : mlir::Type());
            if (!type)
            {
                return {mlir::success(), anyNamedGenericType};
            }

            if (isDefault)
            {
                return {mlir::success(), anyNamedGenericType};
            }

            auto [result, hasNamedGenericType] =
                zipTypeParameterWithArgument(location, pairs, typeParam, type, isDefault, genContext);
            if (mlir::failed(result))
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            if (hasNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasNamedGenericType;
            }
        }

        return {mlir::success(), anyNamedGenericType};
    }

    std::pair<mlir::LogicalResult, IsGeneric> zipTypeParametersWithDefaultArguments(
        mlir::Location location, llvm::ArrayRef<TypeParameterDOM::TypePtr> typeParams, NodeArray<TypeNode> typeArgs,
        llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> &pairs, const GenContext &genContext)
    {
        auto anyNamedGenericType = IsGeneric::False;
        auto argsCount = typeArgs ? typeArgs.size() : 0;
        for (auto index = 0; index < typeParams.size(); index++)
        {
            auto &typeParam = typeParams[index];
            auto isDefault = false;
            if (index < argsCount)
            {
                // we need to process only default values
                continue;
            }
            auto type = typeParam->hasDefault() 
                            ? getType(typeParam->getDefault(), genContext) 
                            : typeParam->hasConstraint() 
                                ? getType(typeParam->getConstraint(), genContext) 
                                : typeParam->hasConstraint() 
                                    ? getType(typeParam->getConstraint(), genContext) 
                                    : mlir::Type();
            if (!type)
            {
                return {mlir::success(), anyNamedGenericType};
            }

            auto name = typeParam->getName();
            auto existType = pairs.lookup(name);
            if (existType.second)
            {
                // type is resolved
                continue;
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! adding default type: " << typeParam->getName() << " type: " << type
                                << "\n";);

            auto [result, hasNamedGenericType] =
                zipTypeParameterWithArgument(location, pairs, typeParam, type, isDefault, genContext);
            if (mlir::failed(result))
            {
                return {mlir::failure(), anyNamedGenericType};
            }

            if (hasNamedGenericType == IsGeneric::True)
            {
                anyNamedGenericType = hasNamedGenericType;
            }
        }

        return {mlir::success(), anyNamedGenericType};
    }

    mlir::Type createTypeReferenceType(TypeReferenceNode typeReferenceAST, const GenContext &genContext)
    {
        mlir::SmallVector<mlir::Type> typeArgs;
        for (auto typeArgNode : typeReferenceAST->typeArguments)
        {
            auto typeArg = getType(typeArgNode, genContext);
            if (!typeArg)
            {
                return mlir::Type();
            }

            typeArgs.push_back(typeArg);
        }

        auto nameRef = MLIRHelper::getName(typeReferenceAST->typeName, stringAllocator);
        auto typeRefType = getTypeReferenceType(nameRef, typeArgs);

        LLVM_DEBUG(llvm::dbgs() << "\n!! generic TypeReferenceType: " << typeRefType;);

        return typeRefType;
    };

    mlir::Type getTypeByTypeReference(mlir::Location location, mlir_ts::TypeReferenceType typeReferenceType, const GenContext &genContext)
    {
        // check utility types
        auto name = typeReferenceType.getName().getValue();

        // try to resolve from type alias first
        auto genericTypeAliasInfo = lookupGenericTypeAliasMap(name);
        if (!is_default(genericTypeAliasInfo))
        {
            GenContext genericTypeGenContext(genContext);

            auto typeParams = std::get<0>(genericTypeAliasInfo);
            auto typeNode = std::get<1>(genericTypeAliasInfo);

            auto [result, hasAnyNamedGenericType] =
                zipTypeParametersWithArguments(location, typeParams, typeReferenceType.getTypes(),
                                               genericTypeGenContext.typeParamsWithArgs, genericTypeGenContext);

            if (mlir::failed(result))
            {
                return mlir::Type();
            }

            return getType(typeNode, genericTypeGenContext);
        }  

        return mlir::Type();      
    }

    mlir::Type getTypeByTypeReference(TypeReferenceNode typeReferenceAST, const GenContext &genContext)
    {
        auto location = loc(typeReferenceAST);

        // check utility types
        auto name = MLIRHelper::getName(typeReferenceAST->typeName);

        if (typeReferenceAST->typeArguments.size())
        {
            // try to resolve from type alias first
            auto genericTypeAliasInfo = lookupGenericTypeAliasMap(name);
            if (!is_default(genericTypeAliasInfo))
            {
                GenContext genericTypeGenContext(genContext);

                auto typeParams = std::get<0>(genericTypeAliasInfo);
                auto typeNode = std::get<1>(genericTypeAliasInfo);

                auto [result, hasAnyNamedGenericType] =
                    zipTypeParametersWithArguments(location, typeParams, typeReferenceAST->typeArguments,
                                                genericTypeGenContext.typeParamsWithArgs, genericTypeGenContext);

                if (mlir::failed(result))
                {
                    return mlir::Type();
                }

                if (hasAnyNamedGenericType == IsGeneric::True)
                {
                    return createTypeReferenceType(typeReferenceAST, genericTypeGenContext);
                }

                return getType(typeNode, genericTypeGenContext);
            }

            if (auto genericClassTypeInfo = lookupGenericClassesMap(name))
            {
                auto classType = genericClassTypeInfo->classType;
                auto [result, specType] = instantiateSpecializedClassType(location, classType,
                                                                        typeReferenceAST->typeArguments, genContext, true);
                if (mlir::succeeded(result))
                {
                    return specType;
                }

                return classType;
            }

            if (auto genericInterfaceTypeInfo = lookupGenericInterfacesMap(name))
            {
                auto interfaceType = genericInterfaceTypeInfo->interfaceType;
                auto [result, specType] = instantiateSpecializedInterfaceType(location, interfaceType,
                                                                            typeReferenceAST->typeArguments, genContext, true);
                if (mlir::succeeded(result))
                {
                    return specType;
                }

                return interfaceType;
            }
        }

        if (auto type = getTypeByTypeName(typeReferenceAST->typeName, genContext))
        {
            return type;
        }

        if (auto embedType = findEmbeddedType(name, typeReferenceAST->typeArguments, genContext))
        {
            return embedType;
        }

        return mlir::Type();
    }

    mlir::Type findEmbeddedType(std::string name, NodeArray<TypeNode> &typeArguments, const GenContext &genContext)
    {
        auto typeArgumentsSize = typeArguments->size();
        if (typeArgumentsSize == 0)
        {
            if (auto type = getEmbeddedType(name))
            {
                return type;
            }
        }

        if (typeArgumentsSize == 1)
        {
            if (auto type = getEmbeddedTypeWithParam(name, typeArguments, genContext))
            {
                return type;
            }
        }

        if (typeArgumentsSize > 1)
        {
            if (auto type = getEmbeddedTypeWithManyParams(name, typeArguments, genContext))
            {
                return type;
            }
        }

        return mlir::Type();
    }

    bool isEmbededType(mlir::StringRef name)
    {
        return compileOptions.enableBuiltins ? isEmbededTypeWithBuiltins(name) : isEmbededTypeWithNoBuiltins(name);
    }
    
    bool isEmbededTypeWithBuiltins(mlir::StringRef name)
    {
        static llvm::StringMap<bool> embeddedTypes {
            {"TemplateStringsArray", true },
            {"const", true },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Number", true },
            {"Object", true },
            {"String", true },
            {"Boolean", true },
            {"Function", true },
#endif
            {"Int8Array", true },
            {"Uint8Array", true },
            {"Int16Array", true },
            {"Uint16Array", true },
            {"Int32Array", true },
            {"Uint32Array", true },
            {"BigInt64Array", true },
            {"BigUint64Array", true },
            {"Float16Array", true },
            {"Float32Array", true },
            {"Float64Array", true },
            {"Float128Array", true},

            {"TypeOf", true },
            {"Opague", true }, // to support void*
            {"Reference", true }, // to support dll import
            {"Readonly", true },
            {"Partial", true },
            {"Required", true },
            {"ThisType", true },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Awaited", true },
            {"Promise", true },
#endif            
            {"NonNullable", true },
            {"Array", true },
            {"ReadonlyArray", true },
            {"ReturnType", true },
            {"Parameters", true },
            {"ConstructorParameters", true },
            {"ThisParameterType", true },
            {"OmitThisParameter", true },
            {"Uppercase", true },
            {"Lowercase", true },
            {"Capitalize", true },
            {"Uncapitalize", true },
            {"Exclude",  true },
            {"Extract", true },
            {"Pick", true },
            {"Omit",  true },
            {"Record", true },
        };

        auto type = embeddedTypes[name];
        return type;
    }

    bool isEmbededTypeWithNoBuiltins(mlir::StringRef name)
    {
        static llvm::StringMap<bool> embeddedTypes {
            {"TemplateStringsArray", true },
            {"const", true },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Number", true },
            {"Object", true },
            {"String", true },
            {"Boolean", true },
            {"Function", true },
#endif

            {"TypeOf", true },
            {"Opague", true }, // to support void*
            {"Reference", true }, // to support dll import
            {"ThisType", true },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Awaited", true },
            {"Promise", true },
#endif            
            {"Array", true }
        };

        auto type = embeddedTypes[name];
        return type;
    }

    mlir::Type getEmbeddedType(mlir::StringRef name)
    {
        return compileOptions.enableBuiltins ? getEmbeddedTypeBuiltins(name) : getEmbeddedTypeNoBuiltins(name);
    }

    mlir::Type getEmbeddedTypeBuiltins(mlir::StringRef name)
    {
        static llvm::StringMap<mlir::Type> embeddedTypes {
            {"TemplateStringsArray", getArrayType(getStringType()) },
            {"const",getConstType() },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Number", getNumberType() },
            {"Object", getObjectType(getAnyType()) },
            {"String", getStringType()},
            {"Boolean", getBooleanType()},
            {"Function", getFunctionType({getArrayType(getAnyType())}, {getAnyType()}, true)},
#endif
            {"Int8Array", getArrayType(builder.getIntegerType(8, true)) },
            {"Uint8Array", getArrayType(builder.getIntegerType(8, false))},
            {"Int16Array", getArrayType(builder.getIntegerType(16, true)) },
            {"Uint16Array", getArrayType(builder.getIntegerType(16, false))},
            {"Int32Array", getArrayType(builder.getIntegerType(32, true)) },
            {"Uint32Array", getArrayType(builder.getIntegerType(32, false))},
            {"BigInt64Array", getArrayType(builder.getIntegerType(64, true)) },
            {"BigUint64Array", getArrayType(builder.getIntegerType(64, false))},
            {"Float16Array", getArrayType(builder.getF16Type())},
            {"Float32Array", getArrayType(builder.getF32Type())},
            {"Float64Array", getArrayType(builder.getF64Type())},
            {"Float128Array", getArrayType(builder.getF128Type())},
            {"Opaque", getOpaqueType()},
        };

        auto type = embeddedTypes[name];
        return type;
    }

    mlir::Type getEmbeddedTypeNoBuiltins(mlir::StringRef name)
    {
        static llvm::StringMap<mlir::Type> embeddedTypes {
            {"TemplateStringsArray", getArrayType(getStringType()) },
            {"const",getConstType() },
#ifdef ENABLE_JS_BUILTIN_TYPES
            {"Number", getNumberType() },
            {"Object", getObjectType(getAnyType()) },
            {"String", getStringType()},
            {"Boolean", getBooleanType()},
            {"Function", getFunctionType({getArrayType(getAnyType())}, {getAnyType()}, true)},
#endif
            {"Opaque", getOpaqueType()},
        };

        auto type = embeddedTypes[name];
        return type;
    }    

    mlir::Type getEmbeddedTypeWithParam(mlir::StringRef name, NodeArray<TypeNode> &typeArguments,
                                        const GenContext &genContext)
    {
        return compileOptions.enableBuiltins 
            ? getEmbeddedTypeWithParamBuiltins(name, typeArguments, genContext) 
            : getEmbeddedTypeWithParamNoBuiltins(name, typeArguments, genContext);
    }

    mlir::Type getEmbeddedTypeWithParamBuiltins(mlir::StringRef name, NodeArray<TypeNode> &typeArguments,
                                        const GenContext &genContext)
    {
        auto translate = llvm::StringSwitch<std::function<mlir::Type(NodeArray<TypeNode> &, const GenContext &)>>(name)
            .Case("TypeOf", [&] (auto typeArguments, auto genContext) {
                auto type = getFirstTypeFromTypeArguments(typeArguments, genContext);
                type = mth.wideStorageType(type);
                return type;
            })
            .Case("Reference", [&] (auto typeArguments, auto genContext) {
                auto type = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return mlir_ts::RefType::get(type);
            })
            .Case("Readonly", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
            .Case("Partial", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
            .Case("Required", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
            .Case("ThisType", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
#ifdef ENABLE_JS_BUILTIN_TYPES
            .Case("Awaited", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
            .Case("Promise", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
#endif            
            .Case("NonNullable", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return NonNullableTypes(elemnentType);
            })
            .Case("Array", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return getArrayType(elemnentType);
            })
            .Case("ReadonlyArray", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return getArrayType(elemnentType);
            })
            .Case("ReturnType", [&] (auto typeArguments, auto genContext) {
                auto elementType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                if (genContext.allowPartialResolve && !elementType)
                {
                    return mlir::Type();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! ReturnType Of: " << elementType;);
                auto retType = mth.getReturnTypeFromFuncRef(elementType);
                LLVM_DEBUG(llvm::dbgs() << " is " << retType << "\n";);
                return retType;
            })
            .Case("Parameters", [&] (auto typeArguments, auto genContext) {
                auto elementType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                if (genContext.allowPartialResolve && !elementType)
                {
                    return mlir::Type();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! ElementType Of: " << elementType;);
                auto retType = mth.getParamsTupleTypeFromFuncRef(elementType);
                LLVM_DEBUG(llvm::dbgs() << " is " << retType << "\n";);
                return retType;
            })
            .Case("ConstructorParameters", [&] (auto typeArguments, auto genContext) {
                auto elementType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                if (genContext.allowPartialResolve && !elementType)
                {
                    return mlir::Type();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! ElementType Of: " << elementType;);
                auto retType = mth.getParamsTupleTypeFromFuncRef(elementType);
                LLVM_DEBUG(llvm::dbgs() << " is " << retType << "\n";);
                return retType;
            })
            .Case("ThisParameterType", [&] (auto typeArguments, auto genContext) {
                auto elementType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                if (genContext.allowPartialResolve && !elementType)
                {
                    return mlir::Type();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! ElementType Of: " << elementType;);
                auto retType = mth.getFirstParamFromFuncRef(elementType);
                LLVM_DEBUG(llvm::dbgs() << " is " << retType << "\n";);
                return retType;
            })
            .Case("OmitThisParameter", [&] (auto typeArguments, auto genContext) {
                auto elementType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                if (genContext.allowPartialResolve && !elementType)
                {
                    return mlir::Type();
                }

                LLVM_DEBUG(llvm::dbgs() << "\n!! ElementType Of: " << elementType;);
                auto retType = mth.getOmitThisFunctionTypeFromFuncRef(elementType);
                LLVM_DEBUG(llvm::dbgs() << " is " << retType << "\n";);
                return retType;
            })
            .Case("Uppercase", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return UppercaseType(elemnentType);
            })
            .Case("Lowercase", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return LowercaseType(elemnentType);
            })
            .Case("Capitalize", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return CapitalizeType(elemnentType);
            })
            .Case("Uncapitalize", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return UncapitalizeType(elemnentType);
            })
            .Default([] (auto, auto) {
                return mlir::Type();
            });

        return translate(typeArguments, genContext);
    }

    mlir::Type getEmbeddedTypeWithParamNoBuiltins(mlir::StringRef name, NodeArray<TypeNode> &typeArguments,
                                        const GenContext &genContext)
    {
        auto translate = llvm::StringSwitch<std::function<mlir::Type(NodeArray<TypeNode> &, const GenContext &)>>(name)
            .Case("TypeOf", [&] (auto typeArguments, auto genContext) {
                auto type = getFirstTypeFromTypeArguments(typeArguments, genContext);
                type = mth.wideStorageType(type);
                return type;
            })
            .Case("Reference", [&] (auto typeArguments, auto genContext) {
                auto type = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return mlir_ts::RefType::get(type);
            })
            .Case("ThisType", std::bind(&MLIRGenImpl::getFirstTypeFromTypeArguments, this, std::placeholders::_1, std::placeholders::_2))
            .Case("Array", [&] (auto typeArguments, auto genContext) {
                auto elemnentType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                return getArrayType(elemnentType);
            })
            .Default([] (auto, auto) {
                return mlir::Type();
            });

        return translate(typeArguments, genContext);
    }

    mlir::Type getEmbeddedTypeWithManyParams(mlir::StringRef name, NodeArray<TypeNode> &typeArguments,
                                             const GenContext &genContext)
    {
        return compileOptions.enableBuiltins 
            ? getEmbeddedTypeWithManyParamsBuiltins(name, typeArguments, genContext) 
            : mlir::Type();
    }

    mlir::Type getEmbeddedTypeWithManyParamsBuiltins(mlir::StringRef name, NodeArray<TypeNode> &typeArguments,
                                             const GenContext &genContext)
    {
        auto translate = llvm::StringSwitch<std::function<mlir::Type(NodeArray<TypeNode> &, const GenContext &)>>(name)
            .Case("Exclude", [&] (auto typeArguments, auto genContext) {
                auto firstType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                auto secondType = getSecondTypeFromTypeArguments(typeArguments, genContext);
                return ExcludeTypes(firstType, secondType);
            })
            .Case("Extract", [&] (auto typeArguments, auto genContext) {
                auto firstType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                auto secondType = getSecondTypeFromTypeArguments(typeArguments, genContext);
                return ExtractTypes(firstType, secondType);
            })
            .Case("Pick", [&] (auto typeArguments, auto genContext) {
                auto sourceType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                auto keysType = getSecondTypeFromTypeArguments(typeArguments, genContext);
                return PickTypes(sourceType, keysType);
            })
            .Case("Omit", [&] (auto typeArguments, auto genContext) {
                auto sourceType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                auto keysType = getSecondTypeFromTypeArguments(typeArguments, genContext);
                return OmitTypes(sourceType, keysType);
            })
            .Case("Record", [&] (auto typeArguments, auto genContext) {
                auto keysType = getFirstTypeFromTypeArguments(typeArguments, genContext);
                auto sourceType = getSecondTypeFromTypeArguments(typeArguments, genContext);
                return RecordType(keysType, sourceType);
            })
            .Default([] (auto, auto) {
                return mlir::Type();
            });

        return translate(typeArguments, genContext);
    }

    mlir::Type StringLiteralTypeFunc(mlir::Type type, std::function<std::string(StringRef)> f)
    {
        if (auto literalType = type.dyn_cast<mlir_ts::LiteralType>())
        {
            if (literalType.getElementType().isa<mlir_ts::StringType>())
            {
                auto newStr = f(literalType.getValue().cast<mlir::StringAttr>().getValue());
                auto copyVal = StringRef(newStr).copy(stringAllocator);
                return mlir_ts::LiteralType::get(builder.getStringAttr(copyVal), getStringType());
            }
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! can't apply string literal type for:" << type << "\n";);

        return mlir::Type();
    }

    mlir::Type UppercaseType(mlir::Type type)
    {
        return StringLiteralTypeFunc(type, [](auto val) { return val.upper(); });
    }

    mlir::Type LowercaseType(mlir::Type type)
    {
        return StringLiteralTypeFunc(type, [](auto val) { return val.lower(); });
    }

    mlir::Type CapitalizeType(mlir::Type type)
    {
        return StringLiteralTypeFunc(type,
                                     [](auto val) { return val.slice(0, 1).upper().append(val.slice(1, val.size())); });
    }

    mlir::Type UncapitalizeType(mlir::Type type)
    {
        return StringLiteralTypeFunc(type,
                                     [](auto val) { return val.slice(0, 1).lower().append(val.slice(1, val.size())); });
    }

    mlir::Type NonNullableTypes(mlir::Type type)
    {
        if (mth.isGenericType(type))
        {
            return type;
        }

        SmallPtrSet<mlir::Type, 2> types;

        MLIRHelper::flatUnionTypes(types, type);

        SmallVector<mlir::Type> resTypes;
        for (auto item : types)
        {
            if (item.isa<mlir_ts::NullType>() || item == getUndefinedType())
            {
                continue;
            }

            resTypes.push_back(item);
        }

        return getUnionType(resTypes);
    }

    // TODO: remove using those types as there issue with generic types
    mlir::Type ExcludeTypes(mlir::Type type, mlir::Type exclude)
    {
        if (mth.isGenericType(type) || mth.isGenericType(exclude))
        {
            return getAnyType();
        }

        SmallPtrSet<mlir::Type, 2> types;
        SmallPtrSet<mlir::Type, 2> excludeTypes;

        MLIRHelper::flatUnionTypes(types, type);
        MLIRHelper::flatUnionTypes(excludeTypes, exclude);

        SmallVector<mlir::Type> resTypes;
        for (auto item : types)
        {
            // TODO: should I use TypeParamsWithArgs from genContext?
            llvm::StringMap<std::pair<ts::TypeParameterDOM::TypePtr,mlir::Type>> emptyTypeParamsWithArgs;
            if (llvm::any_of(excludeTypes, [&](mlir::Type type) { return isTrue(mth.extendsType(item, type, emptyTypeParamsWithArgs)); }))
            {
                continue;
            }

            resTypes.push_back(item);
        }

        return getUnionType(resTypes);
    }

    mlir::Type ExtractTypes(mlir::Type type, mlir::Type extract)
    {
        if (mth.isGenericType(type) || mth.isGenericType(extract))
        {
            return getAnyType();
        }

        SmallPtrSet<mlir::Type, 2> types;
        SmallPtrSet<mlir::Type, 2> extractTypes;

        MLIRHelper::flatUnionTypes(types, type);
        MLIRHelper::flatUnionTypes(extractTypes, extract);

        SmallVector<mlir::Type> resTypes;
        for (auto item : types)
        {
            // TODO: should I use TypeParamsWithArgs from genContext?
            llvm::StringMap<std::pair<ts::TypeParameterDOM::TypePtr,mlir::Type>> emptyTypeParamsWithArgs;
            if (llvm::any_of(extractTypes, [&](mlir::Type type) { return isTrue(mth.extendsType(item, type, emptyTypeParamsWithArgs)); }))
            {
                resTypes.push_back(item);
            }
        }

        return getUnionType(resTypes);
    }

    mlir::Type RecordType(mlir::Type keys, mlir::Type valueType)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! Record: " << valueType << ", keys: " << keys << "\n";);
        
        SmallVector<mlir_ts::FieldInfo> fields;

        auto addTypeProcessKey = [&](mlir::Type keyType)
        {
            // get string
            if (auto litType = keyType.dyn_cast<mlir_ts::LiteralType>())
            {
                fields.push_back({ litType.getValue(), valueType });
            }
        };

        if (auto unionType = keys.dyn_cast<mlir_ts::UnionType>())
        {
            for (auto keyType : unionType.getTypes())
            {
                addTypeProcessKey(keyType);
            }
        }
        else if (auto litType = keys.dyn_cast<mlir_ts::LiteralType>())
        {
            addTypeProcessKey(litType);
        }
        else
        {
            llvm_unreachable("not implemented");
        }        

        return getTupleType(fields);
    }

    mlir::Type PickTypes(mlir::Type type, mlir::Type keys)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! Pick: " << type << ", keys: " << keys << "\n";);

        if (!keys)
        {
            return mlir::Type();
        }

        if (mth.isGenericType(type))
        {
            return getAnyType();
        }        

        if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            SmallVector<mlir::Type> pickedTypes;
            for (auto subType : unionType)
            {
                pickedTypes.push_back(PickTypes(subType, keys));
            }

            return getUnionType(pickedTypes);
        }

        SmallVector<mlir_ts::FieldInfo> pickedFields;
        SmallVector<mlir_ts::FieldInfo> fields;
        if (mlir::succeeded(mth.getFields(type, fields)))
        {
            auto pickTypesProcessKey = [&](mlir::Type keyType)
            {
                // get string
                if (auto litType = keyType.dyn_cast<mlir_ts::LiteralType>())
                {
                    // find field
                    auto found = std::find_if(fields.begin(), fields.end(), [&] (auto& item) { return item.id == litType.getValue(); });
                    if (found != fields.end())
                    {
                        pickedFields.push_back(*found);
                    }
                }
            };

            if (auto unionType = keys.dyn_cast<mlir_ts::UnionType>())
            {
                for (auto keyType : unionType.getTypes())
                {
                    pickTypesProcessKey(keyType);
                }
            }
            else if (auto litType = keys.dyn_cast<mlir_ts::LiteralType>())
            {
                pickTypesProcessKey(litType);
            }
        }

        return getTupleType(pickedFields);
    }

    mlir::Type OmitTypes(mlir::Type type, mlir::Type keys)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! Omit: " << type << ", keys: " << keys << "\n";);

        SmallVector<mlir_ts::FieldInfo> pickedFields;

        SmallVector<mlir_ts::FieldInfo> fields;

        std::function<boolean(mlir_ts::FieldInfo& fieldInfo, mlir::Type keys)> existKey;
        existKey = [&](mlir_ts::FieldInfo& fieldInfo, mlir::Type keys)
        {
            // get string
            if (auto unionType = keys.dyn_cast<mlir_ts::UnionType>())
            {
                for (auto keyType : unionType.getTypes())
                {
                    if (existKey(fieldInfo, keyType))
                    {
                        return true;
                    }
                }
            }
            else if (auto litType = keys.dyn_cast<mlir_ts::LiteralType>())
            {
                return fieldInfo.id == litType.getValue();
            }
            else
            {
                llvm_unreachable("not implemented");
            }

            return false;
        };

        if (mlir::succeeded(mth.getFields(type, fields)))
        {
            for (auto& field : fields)
            {
                if (!existKey(field, keys))
                {
                    pickedFields.push_back(field);
                }
            }
        }

        return getTupleType(pickedFields);
    }        

    mlir::Type getTypeByTypeQuery(TypeQueryNode typeQueryAST, const GenContext &genContext)
    {
        auto exprName = typeQueryAST->exprName;
        if (exprName == SyntaxKind::QualifiedName)
        {
            // TODO: it seems namespace access, can u optimize it somehow?
            auto result = mlirGen(exprName.as<QualifiedName>(), genContext);
            if (result.failed_or_no_value())
            {
                return mlir::Type();
            }

            auto val = V(result);
            return val.getType();
        }

        auto type = evaluate(exprName.as<Expression>(), genContext);
        return type;
    }

    mlir::Type getTypePredicateType(TypePredicateNode typePredicateNode, const GenContext &genContext)
    {
        auto type = getType(typePredicateNode->type, genContext);
        if (!type)
        {
            return mlir::Type();
        }

        auto namePtr = 
            typePredicateNode->parameterName == SyntaxKind::ThisType
            ? THIS_NAME
            : MLIRHelper::getName(typePredicateNode->parameterName, stringAllocator);

        // find index of parameter
        auto hasThis = false;
        auto foundParamIndex = -1;
        if (genContext.funcProto)
        {
            auto index = -1;
            for (auto param : genContext.funcProto->getParams())
            {
                index++;
                if (param->getName() == namePtr)
                {
                    foundParamIndex = index;
                }

                hasThis |= param->getName() == THIS_NAME;
            }
        }

        auto parametereNameSymbol = mlir::FlatSymbolRefAttr::get(builder.getContext(), namePtr);
        return mlir_ts::TypePredicateType::get(parametereNameSymbol, type, !!typePredicateNode->assertsModifier, foundParamIndex - (hasThis ? 1 : 0));
    }

    mlir::Type processConditionalForType(ConditionalTypeNode conditionalTypeNode, mlir::Type checkType, mlir::Type extendsType, mlir::Type inferType, const GenContext &genContext)
    {
        auto &typeParamsWithArgs = const_cast<GenContext &>(genContext).typeParamsWithArgs;

        auto location = loc(conditionalTypeNode);

        mlir::Type resType;
        auto extendsResult = mth.extendsType(checkType, extendsType, typeParamsWithArgs);
        if (extendsResult == ExtendsResult::Never)
        {
            return getNeverType();
        }

        if (isTrue(extendsResult))
        {
            if (inferType)
            {
                auto namedGenType = inferType.cast<mlir_ts::NamedGenericType>();
                auto typeParam = std::make_shared<TypeParameterDOM>(namedGenType.getName().getValue().str());
                zipTypeParameterWithArgument(location, typeParamsWithArgs, typeParam, checkType, false, genContext, false);
            }

            resType = getType(conditionalTypeNode->trueType, genContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! condition type [TRUE] = " << resType << "\n";);

            if (extendsResult != ExtendsResult::Any)
            {
                // in case of any we need "union" of true & false
                return resType;
            }
        }

        // false case
        if (inferType)
        {
            auto namedGenType = inferType.cast<mlir_ts::NamedGenericType>();
            auto typeParam = std::make_shared<TypeParameterDOM>(namedGenType.getName().getValue().str());
            zipTypeParameterWithArgument(location, typeParamsWithArgs, typeParam, checkType, false, genContext, false);
        }

        auto falseType = getType(conditionalTypeNode->falseType, genContext);

        if (extendsResult != ExtendsResult::Any || !resType)
        {
            resType = falseType;
            LLVM_DEBUG(llvm::dbgs() << "\n!! condition type [FALSE] = " << resType << "\n";);
        }
        else
        {
            resType = getUnionType(resType, falseType);
            LLVM_DEBUG(llvm::dbgs() << "\n!! condition type [TRUE | FALSE] = " << resType << "\n";);
        }

        return resType;
    }

    mlir::Type getConditionalType(ConditionalTypeNode conditionalTypeNode, const GenContext &genContext)
    {
        auto checkType = getType(conditionalTypeNode->checkType, genContext);
        auto extendsType = getType(conditionalTypeNode->extendsType, genContext);
        if (!checkType || !extendsType)
        {
            return mlir::Type();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! condition type check: " << checkType << ", extends: " << extendsType << "\n";);

        if (checkType.isa<mlir_ts::NamedGenericType>() || extendsType.isa<mlir_ts::NamedGenericType>())
        {
            // we do not need to resolve it, it is generic
            auto trueType = getType(conditionalTypeNode->trueType, genContext);
            auto falseType = getType(conditionalTypeNode->falseType, genContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! condition type, check: " << checkType << " extends: " << extendsType << " true: " << trueType << " false: " << falseType << " \n";);

            return getConditionalType(checkType, extendsType, trueType, falseType);
        }

        if (auto unionType = checkType.dyn_cast<mlir_ts::UnionType>())
        {
            // we need to have original type to infer types from union
            GenContext noTypeArgsContext(genContext);
            llvm::StringMap<std::pair<TypeParameterDOM::TypePtr, mlir::Type>> typeParamsOnly;
            for (auto &pair : noTypeArgsContext.typeParamsWithArgs)
            {
                typeParamsOnly[pair.getKey()] = std::make_pair(std::get<0>(pair.getValue()), getNamedGenericType(pair.getKey()));
            }

            noTypeArgsContext.typeParamsWithArgs = typeParamsOnly;

            auto originalCheckType = getType(conditionalTypeNode->checkType, noTypeArgsContext);

            LLVM_DEBUG(llvm::dbgs() << "\n!! check type: " << checkType << " original: " << originalCheckType << " \n";);

            SmallVector<mlir::Type> results;
            for (auto subType : unionType.getTypes())
            {
                auto resSubType = processConditionalForType(conditionalTypeNode, subType, extendsType, originalCheckType, genContext);
                if (!resSubType)
                {
                    return mlir::Type();
                }

                if (resSubType != getNeverType())
                {
                    results.push_back(resSubType);
                }
            }            

            return getUnionType(results);
        }

        return processConditionalForType(conditionalTypeNode, checkType, extendsType, mlir::Type(), genContext);
    }

    mlir::Type getKeyOf(TypeOperatorNode typeOperatorNode, const GenContext &genContext)
    {
        auto location = loc(typeOperatorNode);

        auto type = getType(typeOperatorNode->type, genContext);
        if (!type)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! can't take 'keyof'\n";);
            emitError(location, "can't take keyof");
            return mlir::Type();
        }

        return getKeyOf(location, type, genContext);
    }

    mlir::Type getKeyOf(mlir::Location location, mlir::Type type, const GenContext &genContext)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! 'keyof' from: " << type << "\n";);

        if (type.isa<mlir_ts::AnyType>())
        {
            // TODO: and all methods etc
            return getUnionType(getStringType(), getNumberType());
        }

        if (type.isa<mlir_ts::UnknownType>())
        {
            // TODO: should be the same as Any?
            return getNeverType();
        }

        if (type.isa<mlir_ts::ArrayType>())
        {
            return mth.getFieldNames(type);
        }

        if (type.isa<mlir_ts::StringType>())
        {
            return mth.getFieldNames(type);
        }

        if (auto objType = type.dyn_cast<mlir_ts::ObjectType>())
        {
            // TODO: I think this is mistake
            type = objType.getStorageType();
        }

        if (auto classType = type.dyn_cast<mlir_ts::ClassType>())
        {
            return mth.getFieldNames(type);
        }

        if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
        {
            return mth.getFieldNames(type);
        }

        if (auto interfaceType = type.dyn_cast<mlir_ts::InterfaceType>())
        {
            return mth.getFieldNames(type);
        }

        if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            SmallVector<mlir::Type> literalTypes;
            for (auto subType : unionType.getTypes())
            {
                auto keyType = getKeyOf(location, subType, genContext);
                literalTypes.push_back(keyType);
            }

            return getUnionType(literalTypes);
        }

        if (auto enumType = type.dyn_cast<mlir_ts::EnumType>())
        {
            SmallVector<mlir::Type> literalTypes;
            for (auto dictValuePair : enumType.getValues())
            {
                auto litType = mlir_ts::LiteralType::get(builder.getStringAttr(dictValuePair.getName().str()), getStringType());
                literalTypes.push_back(litType);
            }

            return getUnionType(literalTypes);
        }

        if (auto namedGenericType = type.dyn_cast<mlir_ts::NamedGenericType>())
        {
            return getKeyOfType(namedGenericType);
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! can't take 'keyof' from: " << type << "\n";);

        emitError(location, "can't take keyof: ") << type;

        return mlir::Type();
    }

    mlir::Type getTypeOperator(TypeOperatorNode typeOperatorNode, const GenContext &genContext)
    {
        if (typeOperatorNode->_operator == SyntaxKind::UniqueKeyword)
        {
            // TODO: finish it
            return getType(typeOperatorNode->type, genContext);
        }
        else if (typeOperatorNode->_operator == SyntaxKind::KeyOfKeyword)
        {
            return getKeyOf(typeOperatorNode, genContext);
        }
        else if (typeOperatorNode->_operator == SyntaxKind::ReadonlyKeyword)
        {
            // TODO: finish it
            return getType(typeOperatorNode->type, genContext);
        }        

        llvm_unreachable("not implemented");
    }

    mlir::Type getIndexedAccessTypeForArrayElement(mlir_ts::ArrayType type)
    {
        return type.getElementType();
    }

    mlir::Type getIndexedAccessTypeForArrayElement(mlir_ts::ConstArrayType type)
    {
        return type.getElementType();
    }

    mlir::Type getIndexedAccessTypeForArrayElement(mlir_ts::StringType type)
    {
        return getCharType();
    }

    template<typename T> mlir::Type getIndexedAccessTypeForArray(T type, mlir::Type indexType, const GenContext &genContext)
    {
        auto effectiveIndexType = indexType;
        if (auto litIndexType = effectiveIndexType.dyn_cast<mlir_ts::LiteralType>())
        {
            if (auto strAttr = litIndexType.getValue().dyn_cast<mlir::StringAttr>())
            {
                if (strAttr.getValue() == LENGTH_FIELD_NAME)
                {
                    return getNumberType();
                }
            }

            effectiveIndexType = litIndexType.getElementType();
        }

        if (effectiveIndexType.isa<mlir_ts::NumberType>() || effectiveIndexType.isIntOrIndexOrFloat())
        {
            return getIndexedAccessTypeForArrayElement(type);
        }

        return mlir::Type();
    }

    // TODO: sync it with mth.getFields
    mlir::Type getIndexedAccessType(mlir::Type type, mlir::Type indexType, const GenContext &genContext)
    {
        // in case of Generic Methods but not specialized yet
        if (auto namedGenericType = type.dyn_cast<mlir_ts::NamedGenericType>())
        {
            return getIndexAccessType(type, indexType);
        }

        if (auto namedGenericType = indexType.dyn_cast<mlir_ts::NamedGenericType>())
        {
            return getIndexAccessType(type, indexType);
        }

        if (indexType.isa<mlir_ts::StringType>())
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! IndexedAccessType for : " << type << " index " << indexType << " is not implemeneted, index type should not be 'string' it should be literal type \n";);
            llvm_unreachable("not implemented");
        }

        if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            SmallVector<mlir::Type> types;
            for (auto subType : unionType)
            {
                auto typeByKey = getIndexedAccessType(subType, indexType, genContext);
                if (!typeByKey)
                {
                    return mlir::Type();
                }

                types.push_back(typeByKey);
            }

            return getUnionType(types);
        }        

        if (auto unionType = indexType.dyn_cast<mlir_ts::UnionType>())
        {
            SmallVector<mlir::Type> resolvedTypes;
            for (auto itemType : unionType.getTypes())
            {
                auto resType = getIndexedAccessType(type, itemType, genContext);
                if (!resType)
                {
                    return mlir::Type();
                }

                resolvedTypes.push_back(resType);
            }

            return getUnionType(resolvedTypes);
        }

        if (auto arrayType = type.dyn_cast<mlir_ts::ArrayType>())
        {
            // TODO: rewrite using mth.getFieldTypeByIndex(type, indexType);
            return getIndexedAccessTypeForArray(arrayType, indexType, genContext);
        }

        if (auto arrayType = type.dyn_cast<mlir_ts::ConstArrayType>())
        {
            return getIndexedAccessTypeForArray(arrayType, indexType, genContext);
        }

        if (auto stringType = type.dyn_cast<mlir_ts::StringType>())
        {
            return getIndexedAccessTypeForArray(stringType, indexType, genContext);
        }

        if (auto objType = type.dyn_cast<mlir_ts::ObjectType>())
        {
            return mth.getFieldTypeByIndex(type, indexType);
        }

        if (auto classType = type.dyn_cast<mlir_ts::ClassType>())
        {
            return mth.getFieldTypeByIndex(type, indexType);
        }

        // TODO: sync it with mth.getFields
        if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
        {
            return mth.getFieldTypeByIndex(type, indexType);
        }

        if (auto interfaceType = type.dyn_cast<mlir_ts::InterfaceType>())
        {
            return mth.getFieldTypeByIndex(type, indexType);
        }

        if (auto anyType = type.dyn_cast<mlir_ts::AnyType>())
        {
            return anyType;
        }

        if (type.isa<mlir_ts::NeverType>())
        {
            return type;
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! IndexedAccessType for : \n\t" << type << " \n\tindex " << indexType << " is not implemeneted \n";);

        llvm_unreachable("not implemented");
        //return mlir::Type();
    }

    mlir::Type getIndexedAccessType(IndexedAccessTypeNode indexedAccessTypeNode, const GenContext &genContext)
    {
        auto type = getType(indexedAccessTypeNode->objectType, genContext);
        if (!type)
        {
            return type;
        }

        auto indexType = getType(indexedAccessTypeNode->indexType, genContext);
        if (!indexType)
        {
            return indexType;
        }

        return getIndexedAccessType(type, indexType, genContext);
    }

    mlir::Type getTemplateLiteralType(TemplateLiteralTypeNode templateLiteralTypeNode, const GenContext &genContext)
    {
        auto location = loc(templateLiteralTypeNode);

        // first string
        auto text = convertWideToUTF8(templateLiteralTypeNode->head->rawText);

        SmallVector<mlir::Type> types;
        getTemplateLiteralSpan(types, text, templateLiteralTypeNode->templateSpans, 0, genContext);

        if (types.size() == 1)
        {
            return types.front();
        }

        return getUnionType(types);
    }

    void getTemplateLiteralSpan(SmallVector<mlir::Type> &types, const std::string &head,
                                NodeArray<TemplateLiteralTypeSpan> &spans, int spanIndex, const GenContext &genContext)
    {
        if (spanIndex >= spans.size())
        {
            auto newLiteralType = mlir_ts::LiteralType::get(builder.getStringAttr(head), getStringType());
            types.push_back(newLiteralType);
            return;
        }

        auto span = spans[spanIndex];
        auto type = getType(span->type, genContext);
        getTemplateLiteralTypeItem(types, type, head, spans, spanIndex, genContext);
    }

    void getTemplateLiteralTypeItem(SmallVector<mlir::Type> &types, mlir::Type type, const std::string &head,
                                    NodeArray<TemplateLiteralTypeSpan> &spans, int spanIndex,
                                    const GenContext &genContext)
    {
        LLVM_DEBUG(llvm::dbgs() << "\n!! TemplateLiteralType, processing type: " << type << ", span: " << spanIndex
                                << "\n";);

        if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            getTemplateLiteralUnionType(types, unionType, head, spans, spanIndex, genContext);
            return;
        }

        auto span = spans[spanIndex];

        std::stringstream ss;
        ss << head;

        auto typeText = type.cast<mlir_ts::LiteralType>().getValue().cast<mlir::StringAttr>().getValue();
        ss << typeText.str();

        auto spanText = convertWideToUTF8(span->literal->rawText);
        ss << spanText;

        getTemplateLiteralSpan(types, ss.str(), spans, spanIndex + 1, genContext);
    }

    void getTemplateLiteralUnionType(SmallVector<mlir::Type> &types, mlir::Type unionType, const std::string &head,
                                     NodeArray<TemplateLiteralTypeSpan> &spans, int spanIndex,
                                     const GenContext &genContext)
    {
        for (auto unionTypeItem : unionType.cast<mlir_ts::UnionType>().getTypes())
        {
            getTemplateLiteralTypeItem(types, unionTypeItem, head, spans, spanIndex, genContext);
        }
    }

    mlir::Type getMappedType(MappedTypeNode mappedTypeNode, const GenContext &genContext)
    {
        // PTR(Node) /**ReadonlyToken | PlusToken | MinusToken*/ readonlyToken;
        // PTR(TypeParameterDeclaration) typeParameter;
        // PTR(TypeNode) nameType;
        // PTR(Node) /**QuestionToken | PlusToken | MinusToken*/ questionToken;
        // PTR(TypeNode) type;

        auto typeParam = processTypeParameter(mappedTypeNode->typeParameter, genContext);
        auto hasNameType = !!mappedTypeNode->nameType;

        auto constrainType = getType(typeParam->getConstraint(), genContext);
        if (!constrainType)
        {
            return mlir::Type();
        }

        if (auto keyOfType = constrainType.dyn_cast<mlir_ts::KeyOfType>())
        {
            auto type = getType(mappedTypeNode->type, genContext);
            auto nameType = getType(mappedTypeNode->nameType, genContext);
            if (!type || hasNameType && !nameType)
            {
                return mlir::Type();
            }

            return getMappedType(type, nameType, constrainType);
        }

        auto processKeyItem = [&] (mlir::SmallVector<mlir_ts::FieldInfo> &fields, mlir::Type typeParamItem) {
            const_cast<GenContext &>(genContext)
                .typeParamsWithArgs.insert({typeParam->getName(), std::make_pair(typeParam, typeParamItem)});

            auto type = getType(mappedTypeNode->type, genContext);
            if (!type)
            {
                // TODO: do we need to return error?
                // finish it
                return;
            }

            if (type.isa<mlir_ts::NeverType>())
            {
                return; 
            }

            mlir::Type nameType = typeParamItem;
            if (hasNameType)
            {
                nameType = getType(mappedTypeNode->nameType, genContext);
            }

            // remove type param
            const_cast<GenContext &>(genContext).typeParamsWithArgs.erase(typeParam->getName());

            LLVM_DEBUG(llvm::dbgs() << "\n!! mapped type... \n\t type param: [" << typeParam->getName()
                                    << " \n\t\tconstraint item: " << typeParamItem << ", \n\t\tname: " << nameType
                                    << "] \n\ttype: " << type << "\n";);

            if (mth.isNoneType(nameType) || nameType.isa<mlir_ts::NeverType>())
            {
                // filterting out
                LLVM_DEBUG(llvm::dbgs() << "\n!! mapped type... filtered.\n";);
                return;
            }

            if (auto literalType = nameType.dyn_cast<mlir_ts::LiteralType>())
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! mapped type... name: " << literalType << " type: " << type << "\n";);
                fields.push_back({literalType.getValue(), type});
            }
            else
            {
                auto nameSubType = nameType.dyn_cast<mlir_ts::UnionType>();
                auto subType = type.dyn_cast<mlir_ts::UnionType>();
                if (nameSubType && subType)
                {
                    for (auto pair : llvm::zip(nameSubType, subType))
                    {
                        if (auto literalType = std::get<0>(pair).dyn_cast<mlir_ts::LiteralType>())
                        {
                            auto mappedType = std::get<1>(pair);

                            LLVM_DEBUG(llvm::dbgs() << "\n!! mapped type... name: " << literalType << " type: " << mappedType << "\n";);
                            fields.push_back({literalType.getValue(), mappedType});
                        }
                        else
                        {
                            llvm_unreachable("not implemented");
                        }
                    }
                }
                else
                {
                    llvm_unreachable("not implemented");
                }
            }
        };

        SmallVector<mlir_ts::FieldInfo> fields;
        if (auto unionType = constrainType.dyn_cast<mlir_ts::UnionType>())
        {
            for (auto typeParamItem : unionType.getTypes())
            {
                processKeyItem(fields, typeParamItem);
            }
        }
        else if (auto litType = constrainType.dyn_cast<mlir_ts::LiteralType>())
        {
            processKeyItem(fields, litType);
        }

        if (fields.size() == 0)
        {
            LLVM_DEBUG(llvm::dbgs() << "\n!! mapped type is empty for constrain: " << constrainType << ".\n";);
            emitWarning(loc(mappedTypeNode), "mapped type is empty for constrain: ")  << constrainType;
        }

        return getTupleType(fields);            
    }

    mlir_ts::VoidType getVoidType()
    {
        return mlir_ts::VoidType::get(builder.getContext());
    }

    mlir_ts::ByteType getByteType()
    {
        return mlir_ts::ByteType::get(builder.getContext());
    }

    mlir_ts::BooleanType getBooleanType()
    {
        return mlir_ts::BooleanType::get(builder.getContext());
    }

    mlir_ts::NumberType getNumberType()
    {
        return mlir_ts::NumberType::get(builder.getContext());
    }

    mlir_ts::BigIntType getBigIntType()
    {
        return mlir_ts::BigIntType::get(builder.getContext());
    }

    mlir_ts::StringType getStringType()
    {
        return mlir_ts::StringType::get(builder.getContext());
    }

    mlir_ts::CharType getCharType()
    {
        return mlir_ts::CharType::get(builder.getContext());
    }

    mlir_ts::EnumType getEnumType()
    {
        return mlir_ts::EnumType::get(builder.getI32Type(), {});
    }

    mlir::Type getEnumType(mlir::Type elementType, mlir::DictionaryAttr values)
    {
        if (!elementType)
        {
            return mlir::Type();
        }

        return mlir_ts::EnumType::get(elementType, values);
    }

    mlir_ts::ObjectStorageType getObjectStorageType(mlir::FlatSymbolRefAttr name)
    {
        return mlir_ts::ObjectStorageType::get(builder.getContext(), name);
    }

    mlir_ts::ClassStorageType getClassStorageType(mlir::FlatSymbolRefAttr name)
    {
        return mlir_ts::ClassStorageType::get(builder.getContext(), name);
    }

    mlir_ts::ClassType getClassType(mlir::FlatSymbolRefAttr name, mlir::Type storageType)
    {
        return mlir_ts::ClassType::get(name, storageType);
    }

    mlir_ts::NamespaceType getNamespaceType(mlir::StringRef name)
    {
        auto nsNameAttr = mlir::FlatSymbolRefAttr::get(builder.getContext(), name);
        return mlir_ts::NamespaceType::get(nsNameAttr);
    }

    mlir_ts::InterfaceType getInterfaceType(StringRef fullName)
    {
        auto interfaceFullNameSymbol = mlir::FlatSymbolRefAttr::get(builder.getContext(), fullName);
        return getInterfaceType(interfaceFullNameSymbol);
    }

    mlir_ts::InterfaceType getInterfaceType(mlir::FlatSymbolRefAttr name)
    {
        return mlir_ts::InterfaceType::get(name);
    }

    mlir::Type getConstArrayType(ArrayTypeNode arrayTypeAST, unsigned size, const GenContext &genContext)
    {
        auto type = getType(arrayTypeAST->elementType, genContext);
        return getConstArrayType(type, size);
    }

    mlir::Type getConstArrayType(mlir::Type elementType, unsigned size)
    {
        if (!elementType)
        {
            return mlir::Type();
        }

        return mlir_ts::ConstArrayType::get(elementType, size);
    }

    mlir::Type getArrayType(ArrayTypeNode arrayTypeAST, const GenContext &genContext)
    {
        auto type = getType(arrayTypeAST->elementType, genContext);
        return getArrayType(type);
    }

    mlir::Type getArrayType(mlir::Type elementType)
    {
        if (!elementType)
        {
            return mlir::Type();
        }

        return mlir_ts::ArrayType::get(elementType);
    }

    mlir::Type getValueRefType(mlir::Type elementType)
    {
        if (!elementType)
        {
            return mlir::Type();
        }

        return mlir_ts::ValueRefType::get(elementType);
    }

    mlir_ts::NamedGenericType getNamedGenericType(StringRef name)
    {
        return mlir_ts::NamedGenericType::get(builder.getContext(),
                                              mlir::FlatSymbolRefAttr::get(builder.getContext(), name));
    }

    mlir_ts::InferType getInferType(mlir::Type paramType)
    {
        assert(paramType);
        return mlir_ts::InferType::get(paramType);
    }

    mlir::Type getConditionalType(mlir::Type checkType, mlir::Type extendsType, mlir::Type trueType, mlir::Type falseType)
    {
        assert(checkType);
        assert(extendsType);
        assert(trueType);
        assert(falseType);

        if (!checkType || !extendsType || !trueType || !falseType)
        {
            return mlir::Type();
        }

        return mlir_ts::ConditionalType::get(checkType, extendsType, trueType, falseType);
    }

    mlir::Type getIndexAccessType(mlir::Type index, mlir::Type indexAccess)
    {
        assert(index);
        assert(indexAccess);

        if (!index || !indexAccess)
        {
            return mlir::Type();
        }

        return mlir_ts::IndexAccessType::get(index, indexAccess);
    }    

    mlir::Type getKeyOfType(mlir::Type type)
    {
        assert(type);

        if (!type)
        {
            return mlir::Type();
        }

        return mlir_ts::KeyOfType::get(type);
    }      

    mlir::Type getMappedType(mlir::Type elementType, mlir::Type nameType, mlir::Type constrainType)
    {
        assert(elementType);
        assert(nameType);
        assert(constrainType);

        if (!elementType || !nameType || !constrainType)
        {
            return mlir::Type();
        }

        return mlir_ts::MappedType::get(elementType, nameType, constrainType);
    }    

    mlir_ts::TypeReferenceType getTypeReferenceType(mlir::StringRef nameRef, mlir::SmallVector<mlir::Type> &types)
    {
        return mlir_ts::TypeReferenceType::get(builder.getContext(), mlir::FlatSymbolRefAttr::get(builder.getContext(), nameRef), types);
    }    

    mlir::Value getUndefined(mlir::Location location)
    {
        return builder.create<mlir_ts::UndefOp>(location, getUndefinedType());
    }

    mlir::Value getInfinity(mlir::Location location)
    {
#ifdef NUMBER_F64
        double infVal;
        *(int64_t *)&infVal = 0x7FF0000000000000;
        return builder.create<mlir_ts::ConstantOp>(location, getNumberType(), builder.getF64FloatAttr(infVal));
#else
        float infVal;
        *(int32_t *)&infVal = 0x7FF00000;
        return builder.create<mlir_ts::ConstantOp>(location, getNumberType(), builder.getF32FloatAttr(infVal));
#endif
    }

    mlir::Value getNaN(mlir::Location location)
    {
#ifdef NUMBER_F64
        double nanVal;
        *(int64_t *)&nanVal = 0x7FF0000000000001;
        return builder.create<mlir_ts::ConstantOp>(location, getNumberType(), builder.getF64FloatAttr(nanVal));
#else
        float infVal;
        *(int32_t *)&nanVal = 0x7FF00001;
        return builder.create<mlir_ts::ConstantOp>(location, getNumberType(), builder.getF32FloatAttr(nanVal));
#endif
    }

    std::pair<mlir::Attribute, mlir::LogicalResult> getNameFromComputedPropertyName(Node name, const GenContext &genContext)
    {
        if (name == SyntaxKind::ComputedPropertyName)
        {
            MLIRCodeLogic mcl(builder);
            auto result = mlirGen(name.as<ComputedPropertyName>(), genContext);
            auto value = V(result);
            LLVM_DEBUG(llvm::dbgs() << "!! ComputedPropertyName: " << value << "\n";);
            auto attr = mcl.ExtractAttr(value);
            if (!attr)
            {
                emitError(loc(name), "not supported ComputedPropertyName expression");
            }

            return {attr, attr ? mlir::success() : mlir::failure()};
        }

        return {mlir::Attribute(), mlir::success()};
    }

    mlir::Attribute TupleFieldName(Node name, const GenContext &genContext)
    {
        auto namePtr = MLIRHelper::getName(name, stringAllocator);
        if (namePtr.empty())
        {
            auto [attrComputed, attrResult] = getNameFromComputedPropertyName(name, genContext);
            if (attrComputed || mlir::failed(attrResult))
            {
                return attrComputed;
            }
                        
            MLIRCodeLogic mcl(builder);
            auto result = mlirGen(name.as<Expression>(), genContext);
            auto value = V(result);
            auto attr = mcl.ExtractAttr(value);
            if (!attr)
            {
                emitError(loc(name), "not supported name");
            }

            return attr;
        }

        return MLIRHelper::TupleFieldName(namePtr, builder.getContext());
    }

    std::pair<bool, mlir::LogicalResult> getTupleFieldInfo(TupleTypeNode tupleType, mlir::SmallVector<mlir_ts::FieldInfo> &types,
                           const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);
        mlir::Attribute attrVal;
        auto arrayMode = true;
        auto index = 0;
        for (auto typeItem : tupleType->elements)
        {
            if (typeItem == SyntaxKind::NamedTupleMember)
            {
                auto namedTupleMember = typeItem.as<NamedTupleMember>();

                auto type = getType(namedTupleMember->type, genContext);
                if (!type)
                {
                    return {arrayMode, mlir::failure()};
                }

                types.push_back({TupleFieldName(namedTupleMember->name, genContext), type});
                arrayMode = false;
            }
            else if (typeItem == SyntaxKind::LiteralType)
            {
                auto literalTypeNode = typeItem.as<LiteralTypeNode>();
                auto result = mlirGen(literalTypeNode->literal.as<Expression>(), genContext);
                if (result.failed_or_no_value())
                {
                    return {arrayMode, mlir::failure()};
                }

                auto literalValue = V(result);
                auto constantOp = literalValue.getDefiningOp<mlir_ts::ConstantOp>();

                assert(constantOp);
                attrVal = constantOp.getValueAttr();

                if (arrayMode)
                {
                    types.push_back({builder.getIntegerAttr(builder.getI32Type(), index), constantOp.getType()});
                }

                index++;
                continue;
            }
            else
            {
                auto type = getType(typeItem, genContext);
                if (!type)
                {
                    return {arrayMode, mlir::failure()};
                }

                types.push_back({attrVal, type});
            }

            attrVal = mlir::Attribute();
        }

        return {arrayMode, mlir::success()};
    }

    void getTupleFieldInfo(TypeLiteralNode typeLiteral, mlir::SmallVector<mlir_ts::FieldInfo> &types,
                           const GenContext &genContext)
    {
        MLIRCodeLogic mcl(builder);
        for (auto typeItem : typeLiteral->members)
        {
            SyntaxKind kind = typeItem;
            if (kind == SyntaxKind::PropertySignature)
            {
                auto propertySignature = typeItem.as<PropertySignature>();

                auto originalType = getType(propertySignature->type, genContext);
                auto type = mcl.getEffectiveFunctionTypeForTupleField(originalType);

                assert(type);
                types.push_back({TupleFieldName(propertySignature->name, genContext), type});
            }
            else if (kind == SyntaxKind::MethodSignature)
            {
                auto methodSignature = typeItem.as<MethodSignature>();

                auto type = getType(typeItem, genContext);

                assert(type);
                types.push_back({TupleFieldName(methodSignature->name, genContext), type});
            }
            else if (kind == SyntaxKind::ConstructSignature)
            {
                auto type = getType(typeItem, genContext);

                assert(type);
                types.push_back({MLIRHelper::TupleFieldName(NEW_CTOR_METHOD_NAME, builder.getContext()), type});
            }            
            else if (kind == SyntaxKind::IndexSignature)
            {
                auto type = getType(typeItem, genContext);

                assert(type);
                types.push_back({MLIRHelper::TupleFieldName(INDEX_ACCESS_FIELD_NAME, builder.getContext()), type});
            }
            else if (kind == SyntaxKind::CallSignature)
            {
                auto type = getType(typeItem, genContext);

                assert(type);
                types.push_back({MLIRHelper::TupleFieldName(CALL_FIELD_NAME, builder.getContext()), type});
            }
            else
            {
                llvm_unreachable("not implemented");
            }
        }
    }

    mlir_ts::ConstTupleType getConstTupleType(TupleTypeNode tupleType, const GenContext &genContext)
    {
        mlir::SmallVector<mlir_ts::FieldInfo> types;
        getTupleFieldInfo(tupleType, types, genContext);
        return getConstTupleType(types);
    }

    mlir_ts::ConstTupleType getConstTupleType(mlir::SmallVector<mlir_ts::FieldInfo> &fieldInfos)
    {
        return mlir_ts::ConstTupleType::get(builder.getContext(), fieldInfos);
    }

    mlir::Type getTupleType(TupleTypeNode tupleType, const GenContext &genContext)
    {
        mlir::SmallVector<mlir_ts::FieldInfo> types;
        auto [arrayMode, result] = getTupleFieldInfo(tupleType, types, genContext);
        if (mlir::failed(result))
        {
            return mlir::Type();
        }

        if (arrayMode && types.size() == 1)
        {
            return getArrayType(types.front().type);
        }

        return getTupleType(types);
    }

    mlir::Type getTupleType(TypeLiteralNode typeLiteral, const GenContext &genContext)
    {
        mlir::SmallVector<mlir_ts::FieldInfo> types;
        getTupleFieldInfo(typeLiteral, types, genContext);

        // == TODO: remove the following hack
        // TODO: this is hack, add type IndexSignatureFunctionType to see if it is index declaration
        if (types.size() == 1)
        {
            auto indexAccessName = MLIRHelper::TupleFieldName(INDEX_ACCESS_FIELD_NAME, builder.getContext());
            if (types.front().id == indexAccessName)
            {
                if (auto elementTypeOfIndexSignature = mth.getIndexSignatureElementType(types.front().type))
                {
                    auto arrayType = getArrayType(elementTypeOfIndexSignature);
                    LLVM_DEBUG(llvm::dbgs() << "\n!! this is array type: " << arrayType << "\n";);
                    return arrayType;
                }
            }
        }

        // == TODO: remove the following hack
        // TODO: this is hack, add type IndexSignatureFunctionType to see if it is index declaration
        if (types.size() == 2)
        {
            mlir::Type indexSignatureType;
            auto lengthName = MLIRHelper::TupleFieldName(LENGTH_FIELD_NAME, builder.getContext());
            auto indexAccessName = MLIRHelper::TupleFieldName(INDEX_ACCESS_FIELD_NAME, builder.getContext());
            if (types.front().id == lengthName && types.back().id == indexAccessName)
            {
                indexSignatureType = types.back().type;
            }
            
            if (types.back().id == lengthName && types.front().id == indexAccessName)
            {
                indexSignatureType = types.front().type;
            }

            if (indexSignatureType)
            {
                // TODO: this is hack, add type IndexSignatureFunctionType to see if it is index declaration
                if (auto elementTypeOfIndexSignature = mth.getIndexSignatureElementType(indexSignatureType))
                {
                    auto arrayType = getArrayType(elementTypeOfIndexSignature);
                    LLVM_DEBUG(llvm::dbgs() << "\n!! this is array type: " << arrayType << "\n";);
                    return arrayType;
                }
            }
        }        

        return getTupleType(types);
    }

    mlir::Type getTupleType(mlir::SmallVector<mlir_ts::FieldInfo> &fieldInfos)
    {
        return mlir_ts::TupleType::get(builder.getContext(), fieldInfos);
    }

    mlir_ts::ObjectType getObjectType(mlir::Type type)
    {
        return mlir_ts::ObjectType::get(type);
    }

    mlir_ts::BoundFunctionType getBoundFunctionType(mlir_ts::FunctionType funcType)
    {
        return mlir_ts::BoundFunctionType::get(builder.getContext(), funcType);
    }

    mlir_ts::BoundFunctionType getBoundFunctionType(ArrayRef<mlir::Type> inputs, ArrayRef<mlir::Type> results,
                                                    bool isVarArg)
    {
        return mlir_ts::BoundFunctionType::get(builder.getContext(), inputs, results, isVarArg);
    }

    mlir_ts::FunctionType getFunctionType(ArrayRef<mlir::Type> inputs, ArrayRef<mlir::Type> results,
                                          bool isVarArg)
    {
        return mlir_ts::FunctionType::get(builder.getContext(), inputs, results, isVarArg);
    }

    mlir_ts::ExtensionFunctionType getExtensionFunctionType(mlir_ts::FunctionType funcType)
    {
        return mlir_ts::ExtensionFunctionType::get(builder.getContext(), funcType);
    }

    mlir::Type getSignature(SignatureDeclarationBase signature, const GenContext &genContext)
    {
        GenContext genericTypeGenContext(genContext);

        // preparing generic context to resolve types
        if (signature->typeParameters.size())
        {
            llvm::SmallVector<TypeParameterDOM::TypePtr> typeParameters;
            if (mlir::failed(
                    processTypeParameters(signature->typeParameters, typeParameters, genericTypeGenContext)))
            {
                return mlir::Type();
            }

            auto [result, hasAnyNamedGenericType] =
                zipTypeParametersWithArguments(loc(signature), typeParameters, signature->typeArguments,
                                               genericTypeGenContext.typeParamsWithArgs, genericTypeGenContext);

            if (mlir::failed(result))
            {
                return mlir::Type();
            }
        }

        auto resultType = getType(signature->type, genericTypeGenContext);
        if (!resultType && !genContext.allowPartialResolve)
        {
            return mlir::Type();
        }

        SmallVector<mlir::Type> argTypes;
        auto isVarArg = false;
        for (auto paramItem : signature->parameters)
        {
            auto type = getType(paramItem->type, genericTypeGenContext);
            if (!type)
            {
                return mlir::Type();
            }

            if (paramItem->questionToken)
            {
                type = getOptionalType(type);
            }

            argTypes.push_back(type);

            isVarArg |= !!paramItem->dotDotDotToken;
        }

        auto funcType = mlir_ts::FunctionType::get(builder.getContext(), argTypes, resultType, isVarArg);
        return funcType;
    }

    mlir::Type getFunctionType(SignatureDeclarationBase signature, const GenContext &genContext)
    {
        auto signatureType = getSignature(signature, genContext);
        if (!signatureType)
        {
            return mlir::Type();
        }

        auto funcType = mlir_ts::HybridFunctionType::get(builder.getContext(), signatureType.cast<mlir_ts::FunctionType>());
        return funcType;
    }

    mlir::Type getConstructorType(SignatureDeclarationBase signature, const GenContext &genContext)
    {
        auto signatureType = getSignature(signature, genContext);
        if (!signatureType)
        {
            return mlir::Type();
        }

        auto funcType = mlir_ts::ConstructFunctionType::get(
            builder.getContext(), 
            signatureType.cast<mlir_ts::FunctionType>(), 
            hasModifier(signature, SyntaxKind::AbstractKeyword));
        return funcType;
    }

    mlir::Type getCallSignature(CallSignatureDeclaration signature, const GenContext &genContext)
    {
        auto signatureType = getSignature(signature, genContext);
        if (!signatureType)
        {
            return mlir::Type();
        }

        auto funcType = mlir_ts::HybridFunctionType::get(builder.getContext(), signatureType.cast<mlir_ts::FunctionType>());
        return funcType;
    }

    mlir::Type getConstructSignature(ConstructSignatureDeclaration constructSignature,
                                                const GenContext &genContext)
    {
        return getSignature(constructSignature, genContext);
    }

    mlir::Type getMethodSignature(MethodSignature methodSignature, const GenContext &genContext)
    {
        return getSignature(methodSignature, genContext);
    }

    mlir::Type getIndexSignature(IndexSignatureDeclaration indexSignature, const GenContext &genContext)
    {
        return getSignature(indexSignature, genContext);
    }

    mlir::Type getUnionType(UnionTypeNode unionTypeNode, const GenContext &genContext)
    {
        MLIRTypeHelper::UnionTypeProcessContext unionContext = {};
        for (auto typeItem : unionTypeNode->types)
        {
            auto type = getType(typeItem, genContext);
            if (!type)
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! wrong type: " << loc(typeItem) << "\n";);

                //llvm_unreachable("wrong type");
                return mlir::Type();
            }

            mth.processUnionTypeItem(type, unionContext);
        }

        // default wide types
        if (unionContext.isAny)
        {
            return getAnyType();
        }

        return mth.getUnionTypeMergeTypes(unionContext, false, false);
    }

    mlir::Type getUnionType(mlir::Type type1, mlir::Type type2)
    {
        if (!type1 || !type2)
        {
            return mlir::Type();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! join: " << type1 << " | " << type2;);

        auto resType = mth.getUnionType(type1, type2, false);

        LLVM_DEBUG(llvm::dbgs() << " = " << resType << "\n";);

        return resType;
    }

    mlir::Type getUnionType(mlir::SmallVector<mlir::Type> &types)
    {
        return mth.getUnionType(types);
    }

    mlir::LogicalResult processIntersectionType(InterfaceInfo::TypePtr newInterfaceInfo, mlir::Type type, bool conditional = false)
    {
        if (auto ifaceType = type.dyn_cast<mlir_ts::InterfaceType>())
        {
            auto srcInterfaceInfo = getInterfaceInfoByFullName(ifaceType.getName().getValue());
            assert(srcInterfaceInfo);
            newInterfaceInfo->extends.push_back({-1, srcInterfaceInfo});
        }
        else if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
        {
            mergeInterfaces(newInterfaceInfo, tupleType, conditional);
        }
        else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
        {
            mergeInterfaces(newInterfaceInfo, mth.removeConstType(constTupleType).cast<mlir_ts::TupleType>(), conditional);
        }              
        else if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
        {
            for (auto type : unionType.getTypes())
            {
                if (mlir::failed(processIntersectionType(newInterfaceInfo, type, true)))
                {
                    return mlir::failure();
                }
            }            
        }              
        else
        {
            return mlir::failure();
        }      

        return mlir::success();
    }

    mlir::Type getIntersectionType(IntersectionTypeNode intersectionTypeNode, const GenContext &genContext)
    {
        mlir_ts::InterfaceType baseInterfaceType;
        mlir_ts::TupleType baseTupleType;
        mlir::SmallVector<mlir::Type> types;
        mlir::SmallVector<mlir::Type> typesForUnion;
        auto allTupleTypesConst = true;
        auto unionTypes = false;
        for (auto typeItem : intersectionTypeNode->types)
        {
            auto type = getType(typeItem, genContext);
            if (!type)
            {
                return mlir::Type();
            }

            if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
            {
                allTupleTypesConst = false;
                if (!baseTupleType)
                {
                    baseTupleType = tupleType;
                }
            }

            if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
            {
                if (!baseTupleType)
                {
                    baseTupleType = mlir_ts::TupleType::get(builder.getContext(), constTupleType.getFields());
                }
            }

            if (auto ifaceType = type.dyn_cast<mlir_ts::InterfaceType>())
            {
                if (!baseInterfaceType)
                {
                    baseInterfaceType = ifaceType;
                }
            }

            types.push_back(type);
        }

        if (types.size() == 0)
        {
            // this is never type
            return getNeverType();
        }

        if (types.size() == 1)
        {
            return types.front();
        }

        // find base type
        if (baseInterfaceType)
        {
            auto declareInterface = false;
            auto newInterfaceInfo = newInterfaceType(intersectionTypeNode, declareInterface, genContext);
            if (declareInterface)
            {
                // merge all interfaces;
                for (auto type : types)
                {
                    if (mlir::failed(processIntersectionType(newInterfaceInfo, type)))
                    {
                        emitWarning(loc(intersectionTypeNode), "Intersection can't be resolved.");
                        return getIntersectionType(types);
                    }
                }
            }

            newInterfaceInfo->recalcOffsets();

            return newInterfaceInfo->interfaceType;
        }

        if (baseTupleType)
        {
            auto anyTypesInBaseTupleType = baseTupleType.getFields().size() > 0;

            SmallVector<::mlir::typescript::FieldInfo> typesForNewTuple;
            for (auto type : types)
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! processing ... & {...} :" << type << "\n";);

                // umwrap optional
                if (!anyTypesInBaseTupleType)
                {
                    type = mth.stripOptionalType(type);
                }

                if (auto tupleType = type.dyn_cast<mlir_ts::TupleType>())
                {
                    allTupleTypesConst = false;
                    for (auto field : tupleType.getFields())
                    {
                        typesForNewTuple.push_back(field);
                    }
                }
                else if (auto constTupleType = type.dyn_cast<mlir_ts::ConstTupleType>())
                {
                    for (auto field : constTupleType.getFields())
                    {
                        typesForNewTuple.push_back(field);
                    }
                }
                else if (auto unionType = type.dyn_cast<mlir_ts::UnionType>())
                {
                    if (!anyTypesInBaseTupleType)
                    {
                        unionTypes = true;
                        for (auto subType : unionType.getTypes())
                        {
                            if (subType == getNullType() || subType == getUndefinedType())
                            {
                                continue;
                            }

                            typesForUnion.push_back(subType);
                        }
                    }                    
                }
                else
                {
                    if (!anyTypesInBaseTupleType)
                    {
                        unionTypes = true; 
                        typesForUnion.push_back(type);
                    }
                    else
                    {
                        // no intersection
                        return getNeverType();
                    }
                }
            }

            if (unionTypes)
            {
                auto resUnion = getUnionType(typesForUnion);
                LLVM_DEBUG(llvm::dbgs() << "\n!! &=: " << resUnion << "\n";);
                return resUnion;                
            }

            auto resultType = allTupleTypesConst 
                ? (mlir::Type)getConstTupleType(typesForNewTuple)
                : (mlir::Type)getTupleType(typesForNewTuple);

            LLVM_DEBUG(llvm::dbgs() << "\n!! &=: " << resultType << "\n";);

            return resultType;
        }

        // calculate of intersection between types and literal types
        mlir::Type resType;
        for (auto typeItem : types)
        {
            if (!resType)
            {
                resType = typeItem;
                continue;
            }

            LLVM_DEBUG(llvm::dbgs() << "\n!! &: " << resType << " & " << typeItem;);

            resType = AndType(resType, typeItem);

            LLVM_DEBUG(llvm::dbgs() << " = " << resType << "\n";);

            if (resType.isa<mlir_ts::NeverType>())
            {
                return getNeverType();
            }
        }

        if (resType)
        {
            return resType;
        }

        return getNeverType();
    }

    mlir::Type getIntersectionType(mlir::Type type1, mlir::Type type2)
    {
        if (!type1 || !type2)
        {
            return mlir::Type();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! intersection: " << type1 << " & " << type2;);

        auto resType = mth.getIntersectionType(type1, type2);

        LLVM_DEBUG(llvm::dbgs() << " = " << resType << "\n";);

        return resType;
    }

    mlir::Type getIntersectionType(mlir::SmallVector<mlir::Type> &types)
    {
        return mth.getIntersectionType(types);
    }

    mlir::Type AndType(mlir::Type left, mlir::Type right)
    {
        // TODO: 00types_unknown1.ts contains examples of results with & | for types,  T & {} == T & {}, T | {} == T |
        // {}, (they do not change)
        if (left == right)
        {
            return left;
        }

        if (auto literalType = right.dyn_cast<mlir_ts::LiteralType>())
        {
            if (literalType.getElementType() == left)
            {
                if (left.isa<mlir_ts::LiteralType>())
                {
                    return getNeverType();
                }

                return literalType;
            }
        }

        if (auto leftUnionType = left.dyn_cast<mlir_ts::UnionType>())
        {
            return AndUnionType(leftUnionType, right);
        }

        if (auto unionType = right.dyn_cast<mlir_ts::UnionType>())
        {
            mlir::SmallPtrSet<mlir::Type, 2> newUniqueTypes;
            for (auto unionTypeItem : unionType.getTypes())
            {
                auto resType = AndType(left, unionTypeItem);
                newUniqueTypes.insert(resType);
            }

            SmallVector<mlir::Type> newTypes;
            for (auto uniqType : newUniqueTypes)
            {
                newTypes.push_back(uniqType);
            }

            return getUnionType(newTypes);
        }

        if (left.isa<mlir_ts::NullType>())
        {

            if (mth.isValueType(right))
            {
                return getNeverType();
            }

            return left;
        }

        if (right.isa<mlir_ts::NullType>())
        {

            if (mth.isValueType(left))
            {
                return getNeverType();
            }

            return right;
        }

        if (left.isa<mlir_ts::NullType>())
        {

            if (mth.isValueType(right))
            {
                return getNeverType();
            }

            return left;
        }

        if (left.isa<mlir_ts::AnyType>() || left.isa<mlir_ts::UnknownType>())
        {
            return right;
        }

        if (right.isa<mlir_ts::AnyType>() || right.isa<mlir_ts::UnknownType>())
        {
            return left;
        }

        // TODO: should I add, interface, tuple types here?
        // PS: string & { __b: number } creating type "string & { __b: number }".

        return getIntersectionType(left, right);
    }

    mlir::Type AndUnionType(mlir_ts::UnionType leftUnion, mlir::Type right)
    {
        mlir::SmallPtrSet<mlir::Type, 2> newUniqueTypes;
        for (auto unionTypeItem : leftUnion.getTypes())
        {
            auto resType = AndType(unionTypeItem, right);
            newUniqueTypes.insert(resType);
        }

        SmallVector<mlir::Type> newTypes;
        for (auto uniqType : newUniqueTypes)
        {
            newTypes.push_back(uniqType);
        }

        return getUnionType(newTypes);
    }

    InterfaceInfo::TypePtr newInterfaceType(IntersectionTypeNode intersectionTypeNode, bool &declareInterface,
                                            const GenContext &genContext)
    {
        auto newName = MLIRHelper::getAnonymousName(loc_check(intersectionTypeNode), "ifce");

        // clone into new interface
        auto interfaceInfo = mlirGenInterfaceInfo(newName, declareInterface, genContext);

        return interfaceInfo;
    }

    mlir::LogicalResult mergeInterfaces(InterfaceInfo::TypePtr dest, mlir_ts::TupleType src, bool conditional = false)
    {
        // TODO: use it to merge with TupleType
        for (auto &item : src.getFields())
        {
            dest->fields.push_back({item.id, item.type, item.isConditional || conditional, dest->getNextVTableMemberIndex()});
        }

        return mlir::success();
    }

    mlir::Type getParenthesizedType(ParenthesizedTypeNode parenthesizedTypeNode, const GenContext &genContext)
    {
        return getType(parenthesizedTypeNode->type, genContext);
    }

    mlir::Type getLiteralType(LiteralTypeNode literalTypeNode)
    {
        GenContext genContext{};
        genContext.dummyRun = true;
        genContext.allowPartialResolve = true;
        auto result = mlirGen(literalTypeNode->literal.as<Expression>(), genContext);
        auto value = V(result);
        auto type = value.getType();

        if (auto literalType = type.dyn_cast<mlir_ts::LiteralType>())
        {
            return literalType;
        }

        auto constantOp = value.getDefiningOp<mlir_ts::ConstantOp>();
        if (constantOp)
        {
            auto valueAttr = value.getDefiningOp<mlir_ts::ConstantOp>().getValueAttr();
            auto literalType = mlir_ts::LiteralType::get(valueAttr, type);
            return literalType;
        }

        auto nullOp = value.getDefiningOp<mlir_ts::NullOp>();
        if (nullOp)
        {
            return getNullType();
        }

        LLVM_DEBUG(llvm::dbgs() << "\n!! value of literal: " << value << "\n";);

        llvm_unreachable("not implemented");
    }

    mlir::Type getOptionalType(OptionalTypeNode optionalTypeNode, const GenContext &genContext)
    {
        return getOptionalType(getType(optionalTypeNode->type, genContext));
    }

    mlir::Type getOptionalType(mlir::Type type)
    {
        if (!type)
        {
            return mlir::Type();
        }

        if (type.isa<mlir_ts::OptionalType>())
        {
            return type;
        }        

        return mlir_ts::OptionalType::get(type);
    }

    mlir::Type getRestType(RestTypeNode restTypeNode, const GenContext &genContext)
    {
        auto arrayType = getType(restTypeNode->type, genContext);
        if (!arrayType)
        {
            return mlir::Type();
        }

        return getConstArrayType(arrayType.cast<mlir_ts::ArrayType>().getElementType(), 0);
    }

    mlir_ts::AnyType getAnyType()
    {
        return mlir_ts::AnyType::get(builder.getContext());
    }

    mlir_ts::UnknownType getUnknownType()
    {
        return mlir_ts::UnknownType::get(builder.getContext());
    }

    mlir_ts::NeverType getNeverType()
    {
        return mlir_ts::NeverType::get(builder.getContext());
    }

    mlir_ts::ConstType getConstType()
    {
        return mlir_ts::ConstType::get(builder.getContext());
    }    

    mlir_ts::SymbolType getSymbolType()
    {
        return mlir_ts::SymbolType::get(builder.getContext());
    }

    mlir_ts::UndefinedType getUndefinedType()
    {
        return mlir_ts::UndefinedType::get(builder.getContext());
    }

    mlir_ts::NullType getNullType()
    {
        return mlir_ts::NullType::get(builder.getContext());
    }

    mlir_ts::OpaqueType getOpaqueType()
    {
        return mlir_ts::OpaqueType::get(builder.getContext());
    }

    mlir::LogicalResult declare(mlir::Location location, VariableDeclarationDOM::TypePtr var, mlir::Value value, const GenContext &genContext, bool showWarnings = false)
    {
        if (!value)
        {
            return mlir::failure();
        }

        const auto &name = var->getName();

        LLVM_DEBUG(llvm::dbgs() << "\n!! declare variable: " << name << " = [" << value << "]\n";);

        if (showWarnings && symbolTable.count(name))
        {
            auto previousVariable = symbolTable.lookup(name).first;
            if (previousVariable.getParentBlock() == value.getParentBlock())
            {
                LLVM_DEBUG(llvm::dbgs() << "\n!! WARNING redeclaration: " << name << " = [" << value << "]\n";);
                // TODO: find out why you have redeclared vars

                std::string loc;
                llvm::raw_string_ostream sloc(loc);
                printLocation(sloc, previousVariable.getLoc(), path, true);
                sloc.flush();
                emitWarning(location, "") << "variable "<< name << " redeclared. Previous declaration: " << sloc.str();                
            }
        }

        if (compileOptions.generateDebugInfo)
        {
            if (auto defOp = value.getDefiningOp())
            {
                defOp->setLoc(mlir::NameLoc::get(builder.getStringAttr(var->getName()), defOp->getLoc()));
            }
        }

        if (!genContext.insertIntoParentScope)
        {
            symbolTable.insert(name, {value, var});
        }
        else
        {
            symbolTable.insertIntoScope(symbolTable.getCurScope()->getParentScope(), name, {value, var});
        }

        return mlir::success();
    }

    void addDeclarationToExport(ts::Node node, const char* prefix = nullptr)
    {
        Printer printer(declExports);
        printer.setDeclarationMode(true);

        if (prefix)
            declExports << prefix;

        printer.printNode(node);
        declExports << ";\n";

        LLVM_DEBUG(llvm::dbgs() << "\n!! added declaration to export: \n" << convertWideToUTF8(declExports.str()) << "\n";);      
    }

    void addTypeDeclarationToExport(TypeAliasDeclaration typeAliasDeclaration)    
    {
        addDeclarationToExport(typeAliasDeclaration);
    }

    void addInterfaceDeclarationToExport(InterfaceDeclaration interfaceDeclaration)
    {
        addDeclarationToExport(interfaceDeclaration);
    }

    void addEnumDeclarationToExport(EnumDeclaration enumDeclatation)
    {
        addDeclarationToExport(enumDeclatation);
    }

    void addFunctionDeclarationToExport(FunctionLikeDeclarationBase FunctionLikeDeclarationBase)
    {
        addDeclarationToExport(FunctionLikeDeclarationBase, "@dllimport\n");
    }

    void addClassDeclarationToExport(ClassLikeDeclaration classDeclatation)
    {
        addDeclarationToExport(classDeclatation, "@dllimport\n");
    }

    auto getNamespace() -> StringRef
    {
        return currentNamespace->fullName;
    }

    auto getFullNamespaceName(StringRef name) -> StringRef
    {
        if (currentNamespace->fullName.empty())
        {
            return StringRef(name).copy(stringAllocator);
        }

        std::string res;
        res += currentNamespace->fullName;
        res += ".";
        res += name;

        auto namePtr = StringRef(res).copy(stringAllocator);
        return namePtr;
    }

    auto getGlobalsFullNamespaceName(StringRef name) -> StringRef
    {
        auto globalsFullNamespaceName = getGlobalsNamespaceFullName();

        if (globalsFullNamespaceName.empty())
        {
            return StringRef(name).copy(stringAllocator);
        }

        std::string res;
        res += globalsFullNamespaceName;
        res += ".";
        res += name;

        auto namePtr = StringRef(res).copy(stringAllocator);
        return namePtr;            
    }

    auto concat(StringRef fullNamespace, StringRef name) -> StringRef
    {
        std::string res;
        res += fullNamespace;
        res += ".";
        res += name;

        auto namePtr = StringRef(res).copy(stringAllocator);
        return namePtr;
    }

    auto concat(StringRef fullNamespace, StringRef className, StringRef name) -> StringRef
    {
        std::string res;
        res += fullNamespace;
        res += ".";
        res += className;
        res += ".";
        res += name;

        auto namePtr = StringRef(res).copy(stringAllocator);
        return namePtr;
    }

    auto concat(StringRef fullNamespace, StringRef className, StringRef name, int index) -> StringRef
    {
        std::string res;
        res += fullNamespace;
        res += ".";
        res += className;
        res += ".";
        res += name;
        res += "#";
        res += std::to_string(index);

        auto namePtr = StringRef(res).copy(stringAllocator);
        return namePtr;
    }    

    template <typename T> bool is_default(T &t)
    {
        return !static_cast<bool>(t);
    }

#define lookupLogic(S)                                                                                                 \
    MLIRNamespaceGuard ng(currentNamespace);                                                                           \
    decltype(currentNamespace->S.lookup(name)) res;                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        res = currentNamespace->S.lookup(name);                                                                        \
        if (!is_default(res) || !currentNamespace->isFunctionNamespace)                                                \
        {                                                                                                              \
            break;                                                                                                     \
        }                                                                                                              \
                                                                                                                       \
        currentNamespace = currentNamespace->parentNamespace;                                                          \
    } while (true);                                                                                                    \
                                                                                                                       \
    return res;

#define existLogic(S)                                                                                                  \
    MLIRNamespaceGuard ng(currentNamespace);                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        auto res = currentNamespace->S.count(name);                                                                    \
        if (res > 0)                                                                                                   \
        {                                                                                                              \
            return true;                                                                                               \
        }                                                                                                              \
                                                                                                                       \
        if (!currentNamespace->isFunctionNamespace)                                                                    \
        {                                                                                                              \
            return false;                                                                                              \
        }                                                                                                              \
                                                                                                                       \
        currentNamespace = currentNamespace->parentNamespace;                                                          \
    } while (true);                                                                                                    \
                                                                                                                       \
    return false;

    auto getNamespaceByFullName(StringRef fullName) -> NamespaceInfo::TypePtr
    {
        return fullNamespacesMap.lookup(fullName);
    }

    auto getNamespaceMap() -> llvm::StringMap<NamespaceInfo::TypePtr> &
    {
        return currentNamespace->namespacesMap;
    }

    auto getFunctionTypeMap() -> llvm::StringMap<mlir_ts::FunctionType> &
    {
        return currentNamespace->functionTypeMap;
    }

    auto lookupFunctionTypeMap(StringRef name) -> mlir_ts::FunctionType
    {
        lookupLogic(functionTypeMap);
    }

    auto getFunctionMap() -> llvm::StringMap<mlir_ts::FuncOp> &
    {
        return currentNamespace->functionMap;
    }

    auto lookupFunctionMap(StringRef name) -> mlir_ts::FuncOp
    {
        lookupLogic(functionMap);
    }

    // TODO: all lookup/count should be replaced by GenericFunctionMapLookup
    auto getGenericFunctionMap() -> llvm::StringMap<GenericFunctionInfo::TypePtr> &
    {
        return currentNamespace->genericFunctionMap;
    }

    auto lookupGenericFunctionMap(StringRef name) -> GenericFunctionInfo::TypePtr
    {
        lookupLogic(genericFunctionMap);
    }

    auto existGenericFunctionMap(StringRef name) -> bool
    {
        existLogic(genericFunctionMap);
    }

    auto getGlobalsNamespaceFullName() -> llvm::StringRef
    {
        if (!currentNamespace->isFunctionNamespace)
        {
            return currentNamespace->fullName;
        }

        auto curr = currentNamespace;
        while (curr->isFunctionNamespace)
        {
            curr = curr->parentNamespace;
        }

        return curr->fullName;
    }    

    auto getGlobalsMap() -> llvm::StringMap<VariableDeclarationDOM::TypePtr> &
    {
        if (!currentNamespace->isFunctionNamespace)
        {
            return currentNamespace->globalsMap;
        }

        auto curr = currentNamespace;
        while (curr->isFunctionNamespace)
        {
            curr = curr->parentNamespace;
        }

        return curr->globalsMap;
    }

    auto getCaptureVarsMap() -> llvm::StringMap<llvm::StringMap<ts::VariableDeclarationDOM::TypePtr>> &
    {
        return currentNamespace->captureVarsMap;
    }

    auto getLocalVarsInThisContextMap() -> llvm::StringMap<llvm::SmallVector<mlir::typescript::FieldInfo>> &
    {
        return currentNamespace->localVarsInThisContextMap;
    }

    template <typename T> bool is_default(llvm::SmallVector<T> &t)
    {
        return t.size() == 0;
    }

    auto lookupLocalVarsInThisContextMap(StringRef name) -> llvm::SmallVector<mlir::typescript::FieldInfo>
    {
        lookupLogic(localVarsInThisContextMap);
    }

    auto existLocalVarsInThisContextMap(StringRef name) -> bool
    {
        existLogic(localVarsInThisContextMap);
    }

    auto getClassesMap() -> llvm::StringMap<ClassInfo::TypePtr> &
    {
        return currentNamespace->classesMap;
    }

    auto getGenericClassesMap() -> llvm::StringMap<GenericClassInfo::TypePtr> &
    {
        return currentNamespace->genericClassesMap;
    }

    auto lookupGenericClassesMap(StringRef name) -> GenericClassInfo::TypePtr
    {
        lookupLogic(genericClassesMap);
    }

    auto getInterfacesMap() -> llvm::StringMap<InterfaceInfo::TypePtr> &
    {
        return currentNamespace->interfacesMap;
    }

    auto getGenericInterfacesMap() -> llvm::StringMap<GenericInterfaceInfo::TypePtr> &
    {
        return currentNamespace->genericInterfacesMap;
    }

    auto lookupGenericInterfacesMap(StringRef name) -> GenericInterfaceInfo::TypePtr
    {
        lookupLogic(genericInterfacesMap);
    }

    auto getEnumsMap() -> llvm::StringMap<std::pair<mlir::Type, mlir::DictionaryAttr>> &
    {
        return currentNamespace->enumsMap;
    }

    auto getTypeAliasMap() -> llvm::StringMap<mlir::Type> &
    {
        return currentNamespace->typeAliasMap;
    }

    auto getGenericTypeAliasMap()
        -> llvm::StringMap<std::pair<llvm::SmallVector<TypeParameterDOM::TypePtr>, TypeNode>> &
    {
        return currentNamespace->genericTypeAliasMap;
    }

    bool is_default(std::pair<llvm::SmallVector<TypeParameterDOM::TypePtr>, TypeNode> &t)
    {
        return std::get<0>(t).size() == 0;
    }

    auto lookupGenericTypeAliasMap(StringRef name) -> std::pair<llvm::SmallVector<TypeParameterDOM::TypePtr>, TypeNode>
    {
        lookupLogic(genericTypeAliasMap);
    }

    auto getImportEqualsMap() -> llvm::StringMap<mlir::StringRef> &
    {
        return currentNamespace->importEqualsMap;
    }

    auto getGenericFunctionInfoByFullName(StringRef fullName) -> GenericFunctionInfo::TypePtr
    {
        return fullNameGenericFunctionsMap.lookup(fullName);
    }

    auto getClassInfoByFullName(StringRef fullName) -> ClassInfo::TypePtr
    {
        return fullNameClassesMap.lookup(fullName);
    }

    auto getGenericClassInfoByFullName(StringRef fullName) -> GenericClassInfo::TypePtr
    {
        return fullNameGenericClassesMap.lookup(fullName);
    }

    auto getInterfaceInfoByFullName(StringRef fullName) -> InterfaceInfo::TypePtr
    {
        return fullNameInterfacesMap.lookup(fullName);
    }

    auto getGenericInterfaceInfoByFullName(StringRef fullName) -> GenericInterfaceInfo::TypePtr
    {
        return fullNameGenericInterfacesMap.lookup(fullName);
    }

  protected:
    mlir::Location loc(TextRange loc)
    {
        if (!loc)
        {
            return mlir::UnknownLoc::get(builder.getContext());
        }

        auto pos = loc->pos.textPos != -1 ? loc->pos.textPos : loc->pos.pos;
        //return loc1(sourceFile, fileName.str(), pos, loc->_end - pos);
        //return loc2(sourceFile, fileName.str(), pos, loc->_end - pos);
        return loc2Fuse(sourceFile, mainSourceFileName.str(), pos, loc->_end - pos);
    }

    mlir::Location loc1(ts::SourceFile sourceFile, std::string fileName, int start, int length)
    {
        auto fileId = getStringAttr(fileName);
        auto posLineChar = parser.getLineAndCharacterOfPosition(sourceFile, start);
        auto begin =
            mlir::FileLineColLoc::get(builder.getContext(), fileId, posLineChar.line + 1, posLineChar.character + 1);
        return begin;
    }

    mlir::Location loc2(ts::SourceFile sourceFile, std::string fileName, int start, int length)
    {
        auto fileId = getStringAttr(fileName);
        auto posLineChar = parser.getLineAndCharacterOfPosition(sourceFile, start);
        auto begin =
            mlir::FileLineColLoc::get(builder.getContext(), fileId, posLineChar.line + 1, posLineChar.character + 1);
        if (length <= 1)
        {
            return begin;
        }

        auto endLineChar = parser.getLineAndCharacterOfPosition(sourceFile, start + length - 1);
        auto end =
            mlir::FileLineColLoc::get(builder.getContext(), fileId, endLineChar.line + 1, endLineChar.character + 1);
        return mlir::FusedLoc::get(builder.getContext(), {begin, end});
    }

    mlir::Location loc2Fuse(ts::SourceFile sourceFile, std::string fileName, int start, int length)
    {
        auto fileId = getStringAttr(fileName);
        auto posLineChar = parser.getLineAndCharacterOfPosition(sourceFile, start);
        auto begin =
            mlir::FileLineColLoc::get(builder.getContext(), fileId, posLineChar.line + 1, posLineChar.character + 1);
        if (length <= 1)
        {
            return begin;
        }

        auto endLineChar = parser.getLineAndCharacterOfPosition(sourceFile, start + length - 1);
        auto end =
            mlir::FileLineColLoc::get(builder.getContext(), fileId, endLineChar.line + 1, endLineChar.character + 1);
        return mlir::FusedLoc::get(builder.getContext(), {begin}, end);
    }

    size_t getPos(mlir::FileLineColLoc location)
    {
        return location.getLine() * 256 + location.getColumn();
    }

    std::pair<size_t, size_t> getPos(mlir::FusedLoc location)
    {
        auto pos = 0;
        auto _end = 0;

        auto locs = location.getLocations();
        if (locs.size() > 0)
        {
            if (auto fileLineColLoc = locs[0].dyn_cast<mlir::FileLineColLoc>())
            {
                pos = getPos(fileLineColLoc);
            }
        }
        
        if (locs.size() > 1)
        {
            if (auto fileLineColLoc = locs[1].dyn_cast<mlir::FileLineColLoc>())
            {
                _end = getPos(fileLineColLoc);
            }
        }

        if (auto fileLineColLoc = location.getMetadata().dyn_cast_or_null<mlir::FileLineColLoc>())
        {
            _end = getPos(fileLineColLoc);
        }
            
        return {pos, _end};
    }

    std::pair<size_t, size_t> getPos(mlir::Location location)
    {
        auto pos = 0;
        auto _end = 0;

        mlir::TypeSwitch<mlir::LocationAttr>(location)
            .Case<mlir::FusedLoc>([&](auto locParam) {
                auto [pos_, _end_] = getPos(locParam);
                pos = pos_;
                _end = _end_;
            }
        );       
            
        return {pos, _end};
    }

    mlir::StringAttr getStringAttr(const std::string &text)
    {
        return builder.getStringAttr(text);
    }

    mlir::Location loc_check(TextRange loc_)
    {
        assert(loc_->pos != loc_->_end);
        return loc(loc_);
    }

    mlir::LogicalResult parsePartialStatements(string src)
    {
        GenContext emptyContext{};
        return parsePartialStatements(src, emptyContext);
    }

    mlir::LogicalResult parsePartialStatements(string src, const GenContext& genContext, bool useRootNamesapce = true)
    {
        Parser parser;
        auto module = parser.parseSourceFile(S("Temp"), src, ScriptTarget::Latest);

        MLIRNamespaceGuard nsGuard(currentNamespace);
        if (useRootNamesapce)
            currentNamespace = rootNamespace;

        for (auto statement : module->statements)
        {
            if (mlir::failed(mlirGen(statement, genContext)))
            {
                return mlir::failure();
            }
        }

        return mlir::success();
    }

    void printDebug(ts::Node node)
    {
        // Printer<llvm::raw_ostream> printer(llvm::dbgs());
        std::wcerr << "dump ===============================================" << std::endl;
        Printer<std::wostream> printer(std::wcerr);
        printer.printNode(node);
        std::wcerr << std::endl << "end of dump ========================================" << std::endl;
    }

    // TODO: fix issue with cercular reference of include files
    std::pair<SourceFile, std::vector<SourceFile>> loadIncludeFile(mlir::Location location, StringRef fileName)
    {
        SmallString<256> fullPath;
        sys::path::append(fullPath, fileName);
        if (sys::path::extension(fullPath) == "")
        {
            fullPath += ".ts";
        }

        std::string ignored;
        auto id = sourceMgr.AddIncludeFile(std::string(fullPath), SMLoc(), ignored);
        if (!id)
        {
            emitError(location, "can't open file: ") << fullPath;
            return {SourceFile(), {}};
        }

        const auto *sourceBuf = sourceMgr.getMemoryBuffer(id);
        return loadSourceBuf(location, sourceBuf);
    }

    /// The builder is a helper class to create IR inside a function. The builder
    /// is stateful, in particular it keeps an "insertion point": this is where
    /// the next operations will be introduced.
    mlir::OpBuilder builder;

    llvm::SourceMgr &sourceMgr;

    SourceMgrDiagnosticHandlerEx sourceMgrHandler;

    MLIRTypeHelper mth;

    CompileOptions &compileOptions;

    /// A "module" matches a TypeScript source file: containing a list of functions.
    mlir::ModuleOp theModule;

    mlir::StringRef mainSourceFileName;

    mlir::StringRef path;

    /// An allocator used for alias names.
    llvm::BumpPtrAllocator stringAllocator;

    llvm::ScopedHashTable<StringRef, VariablePairT> symbolTable;

    NamespaceInfo::TypePtr rootNamespace;

    NamespaceInfo::TypePtr currentNamespace;

    llvm::ScopedHashTable<StringRef, NamespaceInfo::TypePtr> fullNamespacesMap;

    llvm::ScopedHashTable<StringRef, GenericFunctionInfo::TypePtr> fullNameGenericFunctionsMap;

    llvm::ScopedHashTable<StringRef, ClassInfo::TypePtr> fullNameClassesMap;

    llvm::ScopedHashTable<StringRef, GenericClassInfo::TypePtr> fullNameGenericClassesMap;

    llvm::ScopedHashTable<StringRef, InterfaceInfo::TypePtr> fullNameInterfacesMap;

    llvm::ScopedHashTable<StringRef, GenericInterfaceInfo::TypePtr> fullNameGenericInterfacesMap;

    llvm::ScopedHashTable<StringRef, VariableDeclarationDOM::TypePtr> fullNameGlobalsMap;

    // helper to get line number
    Parser parser;
    ts::SourceFile sourceFile;

    bool declarationMode;

    stringstream declExports;
    stringstream exports;

private:
    std::string label;
    mlir::Block* tempEntryBlock;
    mlir::ModuleOp tempModule;
    mlir_ts::FuncOp tempFuncOp;
};
} // namespace

namespace typescript
{
::std::string dumpFromSource(const llvm::StringRef &fileName, const llvm::StringRef &source)
{
    auto showLineCharPos = false;

    Parser parser;
    auto sourceFile = parser.parseSourceFile(stows(static_cast<std::string>(fileName)),
                                             stows(static_cast<std::string>(source)), ScriptTarget::Latest);

    stringstream s;

    FuncT<> visitNode;
    ArrayFuncT<> visitArray;

    auto intent = 0;

    visitNode = [&](Node child) -> Node {
        for (auto i = 0; i < intent; i++)
        {
            s << "\t";
        }

        if (showLineCharPos)
        {
            auto posLineChar = parser.getLineAndCharacterOfPosition(sourceFile, child->pos);
            auto endLineChar = parser.getLineAndCharacterOfPosition(sourceFile, child->_end);

            s << S("Node: ") << parser.syntaxKindString(child).c_str() << S(" @ [ ") << child->pos << S("(")
              << posLineChar.line + 1 << S(":") << posLineChar.character + 1 << S(") - ") << child->_end << S("(")
              << endLineChar.line + 1 << S(":") << endLineChar.character << S(") ]") << std::endl;
        }
        else
        {
            s << S("Node: ") << parser.syntaxKindString(child).c_str() << S(" @ [ ") << child->pos << S(" - ")
              << child->_end << S(" ]") << std::endl;
        }

        intent++;
        ts::forEachChild(child, visitNode, visitArray);
        intent--;

        return undefined;
    };

    visitArray = [&](NodeArray<Node> array) -> Node {
        for (auto node : array)
        {
            visitNode(node);
        }

        return undefined;
    };

    auto result = forEachChild(sourceFile.as<Node>(), visitNode, visitArray);
    return convertWideToUTF8(s.str());
}

mlir::OwningOpRef<mlir::ModuleOp> mlirGenFromSource(const mlir::MLIRContext &context, const llvm::StringRef &fileName,
                                        const llvm::SourceMgr &sourceMgr, CompileOptions &compileOptions)
{

    auto path = llvm::sys::path::parent_path(fileName);
    MLIRGenImpl mlirGenImpl(context, fileName, path, sourceMgr, compileOptions);
    auto [sourceFile, includeFiles] = mlirGenImpl.loadMainSourceFile();
    return mlirGenImpl.mlirGenSourceFile(sourceFile, includeFiles);
}

} // namespace typescript
