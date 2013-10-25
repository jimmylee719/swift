//===-- Frontend.cpp - frontend utility methods ---------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains utility methods for parsing and performing semantic
// on modules.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/Module.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Parse/Lexer.h"
#include "swift/SIL/SILModule.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Subsystems.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;

void swift::CompilerInstance::createSILModule() {
  TheSILModule = SILModule::createEmptyModule(getASTContext());
}

bool swift::CompilerInstance::setup(const CompilerInvocation &Invok) {
  Invocation = Invok;

  Context.reset(new ASTContext(Invocation.getLangOptions(), SourceMgr, Diagnostics));

  // Give the context the list of search paths to use for modules.
  Context->ImportSearchPaths = Invocation.getImportSearchPaths();
  Context->addModuleLoader(SourceLoader::create(*Context,
                                                !Invocation.isImmediate()));
  SML = SerializedModuleLoader::create(*Context);
  Context->addModuleLoader(SML);

  // If the user has specified an SDK, wire up the Clang module importer
  // and point it at that SDK.
  if (!Invocation.getSDKPath().empty()) {
    auto ImporterCtor = swift::getClangImporterCtor();
    if (!ImporterCtor) {
      Diagnostics.diagnose(SourceLoc(),
                           diag::error_clang_importer_not_linked_in);
      return true;
    }
    auto clangImporter =
        ImporterCtor(*Context, Invocation.getSDKPath(),
                     Invocation.getTargetTriple(),
                     Invocation.getRuntimeIncludePath(),
                     Invocation.getClangModuleCachePath(),
                     Invocation.getImportSearchPaths(),
                     Invocation.getFrameworkSearchPaths(),
                     StringRef(), Invocation.getExtraClangArgs());
    if (!clangImporter) {
      Diagnostics.diagnose(SourceLoc(), diag::error_clang_importer_create_fail);
      return true;
    }

    Context->addModuleLoader(clangImporter, /*isClang*/true);
  }

  // Add the runtime include path (which contains swift.swift)
  Context->ImportSearchPaths.push_back(Invocation.getRuntimeIncludePath());

  assert(Lexer::isIdentifier(Invocation.getModuleName()));

  if (Invocation.getInputKind() == SourceFile::SIL)
    createSILModule();

  auto CodeCompletePoint = Invocation.getCodeCompletionPoint();
  if (CodeCompletePoint.first) {
    auto MemBuf = CodeCompletePoint.first;
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    llvm::MemoryBuffer *CodeCompletionBuffer =
        llvm::MemoryBuffer::getMemBufferCopy(MemBuf->getBuffer(),
                                             MemBuf->getBufferIdentifier());
    unsigned CodeCompletionBufferID =
        SourceMgr->AddNewSourceBuffer(CodeCompletionBuffer, llvm::SMLoc());
    BufferIDs.push_back(CodeCompletionBufferID);
    SourceMgr.setCodeCompletionPoint(CodeCompletionBufferID,
                                     CodeCompletePoint.second);
  }

  for (auto &File : Invocation.getInputFilenames()) {
    // Open the input file.
    llvm::OwningPtr<llvm::MemoryBuffer> InputFile;
    if (llvm::error_code Err =
          llvm::MemoryBuffer::getFileOrSTDIN(File, InputFile)) {
      Diagnostics.diagnose(SourceLoc(), diag::error_open_input_file,
                           File, Err.message());
      return true;
    }
    // Transfer ownership of the MemoryBuffer to the SourceMgr.
    BufferIDs.push_back(SourceMgr->AddNewSourceBuffer(InputFile.take(),
                                                      llvm::SMLoc()));
  }

  for (auto Buf : Invocation.getInputBuffers()) {
    // CompilerInvocation doesn't own the buffers, copy to a new buffer.
    BufferIDs.push_back(SourceMgr->AddNewSourceBuffer(
        llvm::MemoryBuffer::getMemBufferCopy(Buf->getBuffer(),
                                             Buf->getBufferIdentifier()),
                                                     llvm::SMLoc()));
  }

  return false;
}

void swift::CompilerInstance::doIt() {
  const SourceFile::SourceKind Kind = Invocation.getInputKind();
  Identifier ID = Context->getIdentifier(Invocation.getModuleName());
  TU = new (*Context) TranslationUnit(ID, *Context);
  Context->LoadedModules[ID.str()] = TU;

  auto *SingleInputFile =
    new (*Context) SourceFile(*TU, Kind, Invocation.getParseStdlib());
  TU->addSourceFile(*SingleInputFile);
  if (Kind == SourceFile::REPL)
    return;

  std::unique_ptr<DelayedParsingCallbacks> DelayedCB;
  if (Invocation.isCodeCompletion()) {
    DelayedCB.reset(
        new CodeCompleteDelayedCallbacks(SourceMgr.getCodeCompletionLoc()));
  } else if (Invocation.isDelayedFunctionBodyParsing()) {
    DelayedCB.reset(new AlwaysDelayedCallbacks);
  }

  PersistentParserState PersistentState;

  if (Kind == SourceFile::Library) {
    // Parse all of the files into one big translation unit.
    for (auto &BufferID : BufferIDs) {
      bool Done;
      parseIntoTranslationUnit(*SingleInputFile, BufferID, &Done,
                               nullptr, &PersistentState, DelayedCB.get());
      assert(Done && "Parser returned early?");
      (void) Done;
    }

    // Finally, if enabled, type check the whole thing in one go.
    if (!Invocation.getParseOnly())
      performTypeChecking(*SingleInputFile);

    if (DelayedCB) {
      performDelayedParsing(TU, PersistentState,
                            Invocation.getCodeCompletionFactory());
    }
    return;
  }

  // This may only be a main module or SIL, which requires pumping the parser.
  assert(Kind == SourceFile::Main || Kind == SourceFile::SIL);
  assert(BufferIDs.size() == 1 && "This mode only allows one input");
  unsigned BufferID = BufferIDs[0];

  if (Kind == SourceFile::Main)
    SourceMgr.setHashbangBufferID(BufferID);

  SILParserState SILContext(TheSILModule.get());

  unsigned CurTUElem = 0;
  bool Done;
  do {
    // Pump the parser multiple times if necessary.  It will return early
    // after parsing any top level code in a main module, or in SIL mode when
    // there are chunks of swift decls (e.g. imports and types) interspersed
    // with 'sil' definitions.
    parseIntoTranslationUnit(*SingleInputFile, BufferID, &Done,
                             TheSILModule ? &SILContext : nullptr,
                             &PersistentState, DelayedCB.get());
    if (!Invocation.getParseOnly())
      performTypeChecking(*SingleInputFile, CurTUElem);
    CurTUElem = SingleInputFile->Decls.size();
  } while (!Done);

  if (DelayedCB) {
    performDelayedParsing(TU, PersistentState,
                          Invocation.getCodeCompletionFactory());
  }
}

