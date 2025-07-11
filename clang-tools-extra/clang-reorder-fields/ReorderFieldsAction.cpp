//===-- tools/extra/clang-reorder-fields/ReorderFieldsAction.cpp -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the definition of the
/// ReorderFieldsAction::newASTConsumer method
///
//===----------------------------------------------------------------------===//

#include "ReorderFieldsAction.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Refactoring.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include <string>

namespace clang {
namespace reorder_fields {
using namespace clang::ast_matchers;
using llvm::SmallSetVector;

/// Finds the definition of a record by name.
///
/// \returns nullptr if the name is ambiguous or not found.
static const RecordDecl *findDefinition(StringRef RecordName,
                                        ASTContext &Context) {
  auto Results =
      match(recordDecl(hasName(RecordName), isDefinition()).bind("recordDecl"),
            Context);
  if (Results.empty()) {
    llvm::errs() << "Definition of " << RecordName << "  not found\n";
    return nullptr;
  }
  if (Results.size() > 1) {
    llvm::errs() << "The name " << RecordName
                 << " is ambiguous, several definitions found\n";
    return nullptr;
  }
  return selectFirst<RecordDecl>("recordDecl", Results);
}

static bool declaresMultipleFieldsInStatement(const RecordDecl *Decl) {
  SourceLocation LastTypeLoc;
  for (const auto &Field : Decl->fields()) {
    SourceLocation TypeLoc =
        Field->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
    if (LastTypeLoc.isValid() && TypeLoc == LastTypeLoc)
      return true;
    LastTypeLoc = TypeLoc;
  }
  return false;
}

static bool declaresMultipleFieldsInMacro(const RecordDecl *Decl,
                                          const SourceManager &SrcMgr) {
  SourceLocation LastMacroLoc;
  for (const auto &Field : Decl->fields()) {
    if (!Field->getLocation().isMacroID())
      continue;
    SourceLocation MacroLoc = SrcMgr.getExpansionLoc(Field->getLocation());
    if (LastMacroLoc.isValid() && MacroLoc == LastMacroLoc)
      return true;
    LastMacroLoc = MacroLoc;
  }
  return false;
}

static bool containsPreprocessorDirectives(const RecordDecl *Decl,
                                           const SourceManager &SrcMgr,
                                           const LangOptions &LangOpts) {
  std::pair<FileID, unsigned> FileAndOffset =
      SrcMgr.getDecomposedLoc(Decl->field_begin()->getBeginLoc());
  assert(!Decl->field_empty());
  auto LastField = Decl->field_begin();
  while (std::next(LastField) != Decl->field_end())
    ++LastField;
  unsigned EndOffset = SrcMgr.getFileOffset(LastField->getEndLoc());
  StringRef SrcBuffer = SrcMgr.getBufferData(FileAndOffset.first);
  Lexer L(SrcMgr.getLocForStartOfFile(FileAndOffset.first), LangOpts,
          SrcBuffer.data(), SrcBuffer.data() + FileAndOffset.second,
          SrcBuffer.data() + SrcBuffer.size());
  IdentifierTable Identifiers(LangOpts);
  clang::Token T;
  while (!L.LexFromRawLexer(T) && L.getCurrentBufferOffset() < EndOffset) {
    if (T.getKind() == tok::hash) {
      L.LexFromRawLexer(T);
      if (T.getKind() == tok::raw_identifier) {
        clang::IdentifierInfo &II = Identifiers.get(T.getRawIdentifier());
        if (II.getPPKeywordID() != clang::tok::pp_not_keyword)
          return true;
      }
    }
  }
  return false;
}

static bool isSafeToRewrite(const RecordDecl *Decl, const ASTContext &Context) {
  // All following checks expect at least one field declaration.
  if (Decl->field_empty())
    return true;

  // Don't attempt to rewrite if there is a declaration like 'int a, b;'.
  if (declaresMultipleFieldsInStatement(Decl))
    return false;

  const SourceManager &SrcMgr = Context.getSourceManager();

  // Don't attempt to rewrite if a single macro expansion creates multiple
  // fields.
  if (declaresMultipleFieldsInMacro(Decl, SrcMgr))
    return false;

  // Prevent rewriting if there are preprocessor directives present between the
  // start of the first field and the end of last field.
  if (containsPreprocessorDirectives(Decl, SrcMgr, Context.getLangOpts()))
    return false;

  return true;
}

/// Calculates the new order of fields.
///
/// \returns empty vector if the list of fields doesn't match the definition.
static SmallVector<unsigned, 4>
getNewFieldsOrder(const RecordDecl *Definition,
                  ArrayRef<std::string> DesiredFieldsOrder) {
  assert(Definition && "Definition is null");

  llvm::StringMap<unsigned> NameToIndex;
  for (const auto *Field : Definition->fields())
    NameToIndex[Field->getName()] = Field->getFieldIndex();

  if (DesiredFieldsOrder.size() != NameToIndex.size()) {
    llvm::errs() << "Number of provided fields (" << DesiredFieldsOrder.size()
                 << ") doesn't match definition (" << NameToIndex.size()
                 << ").\n";
    return {};
  }
  SmallVector<unsigned, 4> NewFieldsOrder;
  for (const auto &Name : DesiredFieldsOrder) {
    auto It = NameToIndex.find(Name);
    if (It == NameToIndex.end()) {
      llvm::errs() << "Field " << Name << " not found in definition.\n";
      return {};
    }
    NewFieldsOrder.push_back(It->second);
  }
  assert(NewFieldsOrder.size() == NameToIndex.size());
  return NewFieldsOrder;
}

// FIXME: error-handling
/// Replaces one range of source code by another.
static void
addReplacement(SourceRange Old, SourceRange New, const ASTContext &Context,
               std::map<std::string, tooling::Replacements> &Replacements) {
  if (Old.getBegin().isMacroID())
    Old = Context.getSourceManager().getExpansionRange(Old).getAsRange();
  if (New.getBegin().isMacroID())
    New = Context.getSourceManager().getExpansionRange(New).getAsRange();
  StringRef NewText =
      Lexer::getSourceText(CharSourceRange::getTokenRange(New),
                           Context.getSourceManager(), Context.getLangOpts());
  tooling::Replacement R(Context.getSourceManager(),
                         CharSourceRange::getTokenRange(Old), NewText,
                         Context.getLangOpts());
  consumeError(Replacements[std::string(R.getFilePath())].add(R));
}

/// Find all member fields used in the given init-list initializer expr
/// that belong to the same record
///
/// \returns a set of field declarations, empty if none were present
static SmallSetVector<FieldDecl *, 1>
findMembersUsedInInitExpr(const CXXCtorInitializer *Initializer,
                          ASTContext &Context) {
  SmallSetVector<FieldDecl *, 1> Results;
  // Note that this does not pick up member fields of base classes since
  // for those accesses Sema::PerformObjectMemberConversion always inserts an
  // UncheckedDerivedToBase ImplicitCastExpr between the this expr and the
  // object expression
  auto FoundExprs = match(
      traverse(
          TK_AsIs,
          findAll(memberExpr(hasObjectExpression(cxxThisExpr())).bind("ME"))),
      *Initializer->getInit(), Context);
  for (BoundNodes &BN : FoundExprs)
    if (auto *MemExpr = BN.getNodeAs<MemberExpr>("ME"))
      if (auto *FD = dyn_cast<FieldDecl>(MemExpr->getMemberDecl()))
        Results.insert(FD);
  return Results;
}

/// Returns the start of the leading comments before `Loc`.
static SourceLocation getStartOfLeadingComment(SourceLocation Loc,
                                               const SourceManager &SM,
                                               const LangOptions &LangOpts) {
  // We consider any leading comment token that is on the same line or
  // indented similarly to the first comment to be part of the leading comment.
  const unsigned Line = SM.getPresumedLineNumber(Loc);
  const unsigned Column = SM.getPresumedColumnNumber(Loc);
  std::optional<Token> Tok =
      Lexer::findPreviousToken(Loc, SM, LangOpts, /*IncludeComments=*/true);
  while (Tok && Tok->is(tok::comment)) {
    const SourceLocation CommentLoc =
        Lexer::GetBeginningOfToken(Tok->getLocation(), SM, LangOpts);
    if (SM.getPresumedLineNumber(CommentLoc) != Line &&
        SM.getPresumedColumnNumber(CommentLoc) != Column) {
      break;
    }
    Loc = CommentLoc;
    Tok = Lexer::findPreviousToken(Loc, SM, LangOpts, /*IncludeComments=*/true);
  }
  return Loc;
}

/// Returns the end of the trailing comments after `Loc`.
static SourceLocation getEndOfTrailingComment(SourceLocation Loc,
                                              const SourceManager &SM,
                                              const LangOptions &LangOpts) {
  // We consider any following comment token that is indented more than the
  // first comment to be part of the trailing comment.
  const unsigned Column = SM.getPresumedColumnNumber(Loc);
  std::optional<Token> Tok =
      Lexer::findNextToken(Loc, SM, LangOpts, /*IncludeComments=*/true);
  while (Tok && Tok->is(tok::comment) &&
         SM.getPresumedColumnNumber(Tok->getLocation()) > Column) {
    Loc = Tok->getEndLoc();
    Tok = Lexer::findNextToken(Loc, SM, LangOpts, /*IncludeComments=*/true);
  }
  return Loc;
}

/// Returns the full source range for the field declaration up to (including)
/// the trailing semicolumn, including potential macro invocations,
/// e.g. `int a GUARDED_BY(mu);`. If there is a trailing comment, include it.
static SourceRange getFullFieldSourceRange(const FieldDecl &Field,
                                           const ASTContext &Context) {
  const SourceRange Range = Field.getSourceRange();
  SourceLocation Begin = Range.getBegin();
  SourceLocation End = Range.getEnd();
  const SourceManager &SM = Context.getSourceManager();
  const LangOptions &LangOpts = Context.getLangOpts();
  while (true) {
    std::optional<Token> CurrentToken = Lexer::findNextToken(End, SM, LangOpts);

    if (!CurrentToken)
      return SourceRange(Begin, End);

    if (CurrentToken->is(tok::eof))
      return Range; // Something is wrong, return the original range.

    End = CurrentToken->getLastLoc();

    if (CurrentToken->is(tok::semi))
      break;
  }
  Begin = getStartOfLeadingComment(Begin, SM, LangOpts);
  End = getEndOfTrailingComment(End, SM, LangOpts);
  return SourceRange(Begin, End);
}

/// Reorders fields in the definition of a struct/class.
///
/// At the moment reordering of fields with
/// different accesses (public/protected/private) is not supported.
/// \returns true on success.
static bool reorderFieldsInDefinition(
    const RecordDecl *Definition, ArrayRef<unsigned> NewFieldsOrder,
    const ASTContext &Context,
    std::map<std::string, tooling::Replacements> &Replacements) {
  assert(Definition && "Definition is null");

  SmallVector<const FieldDecl *, 10> Fields;
  for (const auto *Field : Definition->fields())
    Fields.push_back(Field);

  // Check that the permutation of the fields doesn't change the accesses
  for (const auto *Field : Definition->fields()) {
    const auto FieldIndex = Field->getFieldIndex();
    if (Field->getAccess() != Fields[NewFieldsOrder[FieldIndex]]->getAccess()) {
      llvm::errs() << "Currently reordering of fields with different accesses "
                      "is not supported\n";
      return false;
    }
  }

  for (const auto *Field : Definition->fields()) {
    const auto FieldIndex = Field->getFieldIndex();
    if (FieldIndex == NewFieldsOrder[FieldIndex])
      continue;
    addReplacement(
        getFullFieldSourceRange(*Field, Context),
        getFullFieldSourceRange(*Fields[NewFieldsOrder[FieldIndex]], Context),
        Context, Replacements);
  }
  return true;
}

/// Reorders initializers in a C++ struct/class constructor.
///
/// A constructor can have initializers for an arbitrary subset of the class's
/// fields. Thus, we need to ensure that we reorder just the initializers that
/// are present.
static void reorderFieldsInConstructor(
    const CXXConstructorDecl *CtorDecl, ArrayRef<unsigned> NewFieldsOrder,
    ASTContext &Context,
    std::map<std::string, tooling::Replacements> &Replacements) {
  assert(CtorDecl && "Constructor declaration is null");
  if (CtorDecl->isImplicit() || CtorDecl->getNumCtorInitializers() <= 1)
    return;

  // The method FunctionDecl::isThisDeclarationADefinition returns false
  // for a defaulted function unless that function has been implicitly defined.
  // Thus this assert needs to be after the previous checks.
  assert(CtorDecl->isThisDeclarationADefinition() && "Not a definition");

  SmallVector<unsigned, 10> NewFieldsPositions(NewFieldsOrder.size());
  for (unsigned i = 0, e = NewFieldsOrder.size(); i < e; ++i)
    NewFieldsPositions[NewFieldsOrder[i]] = i;

  SmallVector<const CXXCtorInitializer *, 10> OldWrittenInitializersOrder;
  SmallVector<const CXXCtorInitializer *, 10> NewWrittenInitializersOrder;
  for (const auto *Initializer : CtorDecl->inits()) {
    if (!Initializer->isMemberInitializer() || !Initializer->isWritten())
      continue;

    // Warn if this reordering violates initialization expr dependencies.
    const FieldDecl *ThisM = Initializer->getMember();
    const auto UsedMembers = findMembersUsedInInitExpr(Initializer, Context);
    for (const FieldDecl *UM : UsedMembers) {
      if (NewFieldsPositions[UM->getFieldIndex()] >
          NewFieldsPositions[ThisM->getFieldIndex()]) {
        DiagnosticsEngine &DiagEngine = Context.getDiagnostics();
        auto Description = ("reordering field " + UM->getName() + " after " +
                            ThisM->getName() + " makes " + UM->getName() +
                            " uninitialized when used in init expression")
                               .str();
        unsigned ID = DiagEngine.getDiagnosticIDs()->getCustomDiagID(
            DiagnosticIDs::Warning, Description);
        DiagEngine.Report(Initializer->getSourceLocation(), ID);
      }
    }

    OldWrittenInitializersOrder.push_back(Initializer);
    NewWrittenInitializersOrder.push_back(Initializer);
  }
  auto ByFieldNewPosition = [&](const CXXCtorInitializer *LHS,
                                const CXXCtorInitializer *RHS) {
    assert(LHS && RHS);
    return NewFieldsPositions[LHS->getMember()->getFieldIndex()] <
           NewFieldsPositions[RHS->getMember()->getFieldIndex()];
  };
  llvm::sort(NewWrittenInitializersOrder, ByFieldNewPosition);
  assert(OldWrittenInitializersOrder.size() ==
         NewWrittenInitializersOrder.size());
  for (unsigned i = 0, e = NewWrittenInitializersOrder.size(); i < e; ++i)
    if (OldWrittenInitializersOrder[i] != NewWrittenInitializersOrder[i])
      addReplacement(OldWrittenInitializersOrder[i]->getSourceRange(),
                     NewWrittenInitializersOrder[i]->getSourceRange(), Context,
                     Replacements);
}

/// Reorders initializers in the brace initialization of an aggregate.
///
/// At the moment partial initialization is not supported.
/// \returns true on success
static bool reorderFieldsInInitListExpr(
    const InitListExpr *InitListEx, ArrayRef<unsigned> NewFieldsOrder,
    const ASTContext &Context,
    std::map<std::string, tooling::Replacements> &Replacements) {
  assert(InitListEx && "Init list expression is null");
  // We care only about InitListExprs which originate from source code.
  // Implicit InitListExprs are created by the semantic analyzer.
  if (!InitListEx->isExplicit())
    return true;
  // The method InitListExpr::getSyntacticForm may return nullptr indicating
  // that the current initializer list also serves as its syntactic form.
  if (const auto *SyntacticForm = InitListEx->getSyntacticForm())
    InitListEx = SyntacticForm;
  // If there are no initializers we do not need to change anything.
  if (!InitListEx->getNumInits())
    return true;
  if (InitListEx->getNumInits() != NewFieldsOrder.size()) {
    llvm::errs() << "Currently only full initialization is supported\n";
    return false;
  }
  for (unsigned i = 0, e = InitListEx->getNumInits(); i < e; ++i)
    if (i != NewFieldsOrder[i])
      addReplacement(InitListEx->getInit(i)->getSourceRange(),
                     InitListEx->getInit(NewFieldsOrder[i])->getSourceRange(),
                     Context, Replacements);
  return true;
}

namespace {
class ReorderingConsumer : public ASTConsumer {
  StringRef RecordName;
  ArrayRef<std::string> DesiredFieldsOrder;
  std::map<std::string, tooling::Replacements> &Replacements;

public:
  ReorderingConsumer(StringRef RecordName,
                     ArrayRef<std::string> DesiredFieldsOrder,
                     std::map<std::string, tooling::Replacements> &Replacements)
      : RecordName(RecordName), DesiredFieldsOrder(DesiredFieldsOrder),
        Replacements(Replacements) {}

  ReorderingConsumer(const ReorderingConsumer &) = delete;
  ReorderingConsumer &operator=(const ReorderingConsumer &) = delete;

  void HandleTranslationUnit(ASTContext &Context) override {
    const RecordDecl *RD = findDefinition(RecordName, Context);
    if (!RD)
      return;
    if (!isSafeToRewrite(RD, Context))
      return;
    SmallVector<unsigned, 4> NewFieldsOrder =
        getNewFieldsOrder(RD, DesiredFieldsOrder);
    if (NewFieldsOrder.empty())
      return;
    if (!reorderFieldsInDefinition(RD, NewFieldsOrder, Context, Replacements))
      return;

    // CXXRD will be nullptr if C code (not C++) is being processed.
    const CXXRecordDecl *CXXRD = dyn_cast<CXXRecordDecl>(RD);
    if (CXXRD)
      for (const auto *C : CXXRD->ctors())
        if (const auto *D = dyn_cast<CXXConstructorDecl>(C->getDefinition()))
          reorderFieldsInConstructor(cast<const CXXConstructorDecl>(D),
                                     NewFieldsOrder, Context, Replacements);

    // We only need to reorder init list expressions for
    // plain C structs or C++ aggregate types.
    // For other types the order of constructor parameters is used,
    // which we don't change at the moment.
    // Now (v0) partial initialization is not supported.
    if (!CXXRD || CXXRD->isAggregate())
      for (auto Result :
           match(initListExpr(hasType(equalsNode(RD))).bind("initListExpr"),
                 Context))
        if (!reorderFieldsInInitListExpr(
                Result.getNodeAs<InitListExpr>("initListExpr"), NewFieldsOrder,
                Context, Replacements)) {
          Replacements.clear();
          return;
        }
  }
};
} // end anonymous namespace

std::unique_ptr<ASTConsumer> ReorderFieldsAction::newASTConsumer() {
  return std::make_unique<ReorderingConsumer>(RecordName, DesiredFieldsOrder,
                                               Replacements);
}

} // namespace reorder_fields
} // namespace clang
