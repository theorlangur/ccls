/* Copyright 2017-2018 ccls Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "indexer.hh"

#include "clang_tu.hh"
#include "log.hh"
#include "pipeline.hh"
#include "platform.hh"
#include "sema_manager.hh"

#include <clang/AST/AST.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <clang/Index/IndexDataConsumer.h>
#include <clang/Index/IndexingAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Support/CrashRecoveryContext.h>
#include <llvm/Support/Path.h>

#include <algorithm>
#include <inttypes.h>
#include <map>
#include <unordered_set>

using namespace clang;

namespace ccls {
namespace {

GroupMatch *multiVersionMatcher;

struct File {
  std::string path;
  int64_t mtime;
  std::string content;
  std::unique_ptr<IndexFile> db;
};

struct IndexParam {
  std::unordered_map<FileID, File> uid2file;
  std::unordered_map<FileID, bool> uid2multi;
  struct DeclInfo {
    Usr usr;
    std::string short_name;
    std::string qualified;
  };
  std::unordered_map<const Decl *, DeclInfo> Decl2Info;

  VFS &vfs;
  ASTContext *ctx;
  bool no_linkage;
  IndexParam(VFS &vfs, bool no_linkage) : vfs(vfs), no_linkage(no_linkage) {}

  void seenFile(FileID fid) {
    // If this is the first time we have seen the file (ignoring if we are
    // generating an index for it):
    auto [it, inserted] = uid2file.try_emplace(fid);
    if (inserted) {
      const FileEntry *fe = ctx->getSourceManager().getFileEntryForID(fid);
      if (!fe)
        return;
      std::string path = pathFromFileEntry(*fe);
      it->second.path = path;
      it->second.mtime = fe->getModificationTime();
      if (!it->second.mtime)
        if (auto tim = lastWriteTime(path))
          it->second.mtime = *tim;
      if (std::optional<std::string> content = readContent(path))
        it->second.content = *content;

      if (!vfs.stamp(path, it->second.mtime, no_linkage ? 3 : 1))
        return;
      it->second.db =
          std::make_unique<IndexFile>(path, it->second.content, no_linkage);
    }
  }

  IndexFile *consumeFile(FileID fid) {
    seenFile(fid);
    return uid2file[fid].db.get();
  }

  bool useMultiVersion(FileID fid) {
    auto it = uid2multi.try_emplace(fid);
    if (it.second)
      if (const FileEntry *fe = ctx->getSourceManager().getFileEntryForID(fid))
        it.first->second = multiVersionMatcher->matches(pathFromFileEntry(*fe));
    return it.first->second;
  }
};

StringRef getSourceInRange(const SourceManager &SM, const LangOptions &LangOpts,
                           SourceRange R) {
  SourceLocation BLoc = R.getBegin(), ELoc = R.getEnd();
  std::pair<FileID, unsigned> BInfo = SM.getDecomposedLoc(BLoc),
                              EInfo = SM.getDecomposedLoc(ELoc);
  bool invalid = false;
  StringRef Buf = SM.getBufferData(BInfo.first, &invalid);
  if (invalid)
    return "";
  return Buf.substr(BInfo.second,
                    EInfo.second +
                        Lexer::MeasureTokenLength(ELoc, SM, LangOpts) -
                        BInfo.second);
}

Kind getKind(const Decl *D, SymbolKind &kind) {
  switch (D->getKind()) {
  case Decl::LinkageSpec:
    return Kind::Invalid;
  case Decl::Namespace:
  case Decl::NamespaceAlias:
    kind = SymbolKind::Namespace;
    return Kind::Type;
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCImplementation:
  case Decl::ObjCInterface:
  case Decl::ObjCProtocol:
    kind = SymbolKind::Interface;
    return Kind::Type;
  case Decl::ObjCMethod:
    kind = SymbolKind::Method;
    return Kind::Func;
  case Decl::ObjCProperty:
    kind = SymbolKind::Property;
    return Kind::Type;
  case Decl::ClassTemplate:
    kind = SymbolKind::Class;
    return Kind::Type;
  case Decl::FunctionTemplate:
    kind = SymbolKind::Function;
    return Kind::Func;
  case Decl::TypeAliasTemplate:
    kind = SymbolKind::TypeAlias;
    return Kind::Type;
  case Decl::VarTemplate:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::TemplateTemplateParm:
    kind = SymbolKind::TypeParameter;
    return Kind::Type;
  case Decl::Enum:
    kind = SymbolKind::Enum;
    return Kind::Type;
  case Decl::CXXRecord:
  case Decl::Record:
    kind = SymbolKind::Class;
    // spec has no Union, use Class
    if (auto *RD = dyn_cast<RecordDecl>(D))
      if (RD->getTagKind() == TTK_Struct)
        kind = SymbolKind::Struct;
    return Kind::Type;
  case Decl::ClassTemplateSpecialization:
  case Decl::ClassTemplatePartialSpecialization:
    kind = SymbolKind::Class;
    return Kind::Type;
  case Decl::TemplateTypeParm:
    kind = SymbolKind::TypeParameter;
    return Kind::Type;
  case Decl::TypeAlias:
  case Decl::Typedef:
  case Decl::UnresolvedUsingTypename:
    kind = SymbolKind::TypeAlias;
    return Kind::Type;
  case Decl::Using:
    kind = SymbolKind::Null; // ignored
    return Kind::Invalid;
  case Decl::Binding:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::Field:
  case Decl::ObjCIvar:
    kind = SymbolKind::Field;
    return Kind::Var;
  case Decl::Function:
    kind = SymbolKind::Function;
    return Kind::Func;
  case Decl::CXXMethod: {
    const auto *MD = cast<CXXMethodDecl>(D);
    kind = MD->isStatic() ? SymbolKind::StaticMethod : SymbolKind::Method;
    return Kind::Func;
  }
  case Decl::CXXConstructor:
    kind = SymbolKind::Constructor;
    return Kind::Func;
  case Decl::CXXConversion:
  case Decl::CXXDestructor:
    kind = SymbolKind::Method;
    return Kind::Func;
  case Decl::NonTypeTemplateParm:
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var;
  case Decl::Var:
  case Decl::Decomposition:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::ImplicitParam:
  case Decl::ParmVar:
    // ccls extension
    kind = SymbolKind::Parameter;
    return Kind::Var;
  case Decl::VarTemplateSpecialization:
  case Decl::VarTemplatePartialSpecialization:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::EnumConstant:
    kind = SymbolKind::EnumMember;
    return Kind::Var;
  case Decl::UnresolvedUsingValue:
    kind = SymbolKind::Variable;
    return Kind::Var;
  case Decl::TranslationUnit:
    return Kind::Invalid;

  default:
    return Kind::Invalid;
  }
}

LanguageId getDeclLanguage(const Decl *D) {
  switch (D->getKind()) {
  default:
    return LanguageId::C;
  case Decl::ImplicitParam:
  case Decl::ObjCAtDefsField:
  case Decl::ObjCCategory:
  case Decl::ObjCCategoryImpl:
  case Decl::ObjCCompatibleAlias:
  case Decl::ObjCImplementation:
  case Decl::ObjCInterface:
  case Decl::ObjCIvar:
  case Decl::ObjCMethod:
  case Decl::ObjCProperty:
  case Decl::ObjCPropertyImpl:
  case Decl::ObjCProtocol:
  case Decl::ObjCTypeParam:
    return LanguageId::ObjC;
  case Decl::CXXConstructor:
  case Decl::CXXConversion:
  case Decl::CXXDestructor:
  case Decl::CXXMethod:
  case Decl::CXXRecord:
  case Decl::ClassTemplate:
  case Decl::ClassTemplatePartialSpecialization:
  case Decl::ClassTemplateSpecialization:
  case Decl::Friend:
  case Decl::FriendTemplate:
  case Decl::FunctionTemplate:
  case Decl::LinkageSpec:
  case Decl::Namespace:
  case Decl::NamespaceAlias:
  case Decl::NonTypeTemplateParm:
  case Decl::StaticAssert:
  case Decl::TemplateTemplateParm:
  case Decl::TemplateTypeParm:
  case Decl::UnresolvedUsingTypename:
  case Decl::UnresolvedUsingValue:
  case Decl::Using:
  case Decl::UsingDirective:
  case Decl::UsingShadow:
    return LanguageId::Cpp;
  }
}

// clang/lib/AST/DeclPrinter.cpp
QualType getBaseType(QualType T, bool deduce_auto) {
  QualType BaseType = T;
  while (!BaseType.isNull() && !BaseType->isSpecifierType()) {
    if (const PointerType *PTy = BaseType->getAs<PointerType>())
      BaseType = PTy->getPointeeType();
    else if (const BlockPointerType *BPy = BaseType->getAs<BlockPointerType>())
      BaseType = BPy->getPointeeType();
    else if (const ArrayType *ATy = dyn_cast<ArrayType>(BaseType))
      BaseType = ATy->getElementType();
    else if (const VectorType *VTy = BaseType->getAs<VectorType>())
      BaseType = VTy->getElementType();
    else if (const ReferenceType *RTy = BaseType->getAs<ReferenceType>())
      BaseType = RTy->getPointeeType();
    else if (const ParenType *PTy = BaseType->getAs<ParenType>())
      BaseType = PTy->desugar();
    else if (deduce_auto) {
      if (const AutoType *ATy = BaseType->getAs<AutoType>())
        BaseType = ATy->getDeducedType();
      else
        break;
    } else
      break;
  }
  return BaseType;
}

const Decl *getTypeDecl(QualType T, bool *specialization = nullptr) {
  Decl *D = nullptr;
  T = getBaseType(T.getUnqualifiedType(), true);
  const Type *TP = T.getTypePtrOrNull();
  if (!TP)
    return nullptr;

try_again:
  switch (TP->getTypeClass()) {
  case Type::Typedef:
    D = cast<TypedefType>(TP)->getDecl();
    break;
  case Type::ObjCObject:
    D = cast<ObjCObjectType>(TP)->getInterface();
    break;
  case Type::ObjCInterface:
    D = cast<ObjCInterfaceType>(TP)->getDecl();
    break;
  case Type::Record:
  case Type::Enum:
    D = cast<TagType>(TP)->getDecl();
    break;
  case Type::TemplateTypeParm:
    D = cast<TemplateTypeParmType>(TP)->getDecl();
    break;
  case Type::TemplateSpecialization:
    if (specialization)
      *specialization = true;
    if (const RecordType *Record = TP->getAs<RecordType>())
      D = Record->getDecl();
    else
      D = cast<TemplateSpecializationType>(TP)
              ->getTemplateName()
              .getAsTemplateDecl();
    break;

  case Type::Auto:
  case Type::DeducedTemplateSpecialization:
    TP = cast<DeducedType>(TP)->getDeducedType().getTypePtrOrNull();
    if (TP)
      goto try_again;
    break;

  case Type::InjectedClassName:
    D = cast<InjectedClassNameType>(TP)->getDecl();
    break;

    // FIXME: Template type parameters!

  case Type::Elaborated:
    TP = cast<ElaboratedType>(TP)->getNamedType().getTypePtrOrNull();
    goto try_again;

  default:
    break;
  }
  return D;
}

const Decl *getAdjustedDecl(const Decl *D) {
  while (D) {
    if (auto *R = dyn_cast<CXXRecordDecl>(D)) {
      if (auto *S = dyn_cast<ClassTemplateSpecializationDecl>(R)) {
        if (!S->getTypeAsWritten()) {
          llvm::PointerUnion<ClassTemplateDecl *,
                             ClassTemplatePartialSpecializationDecl *>
              Result = S->getSpecializedTemplateOrPartial();
          if (Result.is<ClassTemplateDecl *>())
            D = Result.get<ClassTemplateDecl *>();
          else
            D = Result.get<ClassTemplatePartialSpecializationDecl *>();
          continue;
        }
      } else if (auto *D1 = R->getInstantiatedFromMemberClass()) {
        D = D1;
        continue;
      }
    } else if (auto *ED = dyn_cast<EnumDecl>(D)) {
      if (auto *D1 = ED->getInstantiatedFromMemberEnum()) {
        D = D1;
        continue;
      }
    }
    break;
  }
  return D;
}

bool validateRecord(const RecordDecl *RD) {
  for (const auto *I : RD->fields()) {
    QualType FQT = I->getType();
    if (FQT->isIncompleteType() || FQT->isDependentType())
      return false;
    if (const RecordType *ChildType = I->getType()->getAs<RecordType>())
      if (const RecordDecl *Child = ChildType->getDecl())
        if (!validateRecord(Child))
          return false;
  }
  return true;
}

class IndexDataConsumer : public index::IndexDataConsumer {
public:
  ASTContext *Ctx;
  IndexParam &param;

  std::string getComment(const Decl *D) {
    SourceManager &SM = Ctx->getSourceManager();
    const RawComment *RC = Ctx->getRawCommentForAnyRedecl(D);
    if (!RC)
      return "";
    StringRef Raw = RC->getRawText(Ctx->getSourceManager());
    SourceRange R = RC->getSourceRange();
    std::pair<FileID, unsigned> BInfo = SM.getDecomposedLoc(R.getBegin());
    unsigned start_column = SM.getLineNumber(BInfo.first, BInfo.second);
    std::string ret;
    int pad = -1;
    for (const char *p = Raw.data(), *E = Raw.end(); p < E;) {
      // The first line starts with a comment marker, but the rest needs
      // un-indenting.
      unsigned skip = start_column - 1;
      for (; skip > 0 && p < E && (*p == ' ' || *p == '\t'); p++)
        skip--;
      const char *q = p;
      while (q < E && *q != '\n')
        q++;
      if (q < E)
        q++;
      // A minimalist approach to skip Doxygen comment markers.
      // See https://www.stack.nl/~dimitri/doxygen/manual/docblocks.html
      if (pad < 0) {
        // First line, detect the length of comment marker and put into |pad|
        const char *begin = p;
        while (p < E && (*p == '/' || *p == '*' || *p == '-' || *p == '='))
          p++;
        if (p < E && (*p == '<' || *p == '!'))
          p++;
        if (p < E && *p == ' ')
          p++;
        if (p + 1 == q)
          p++;
        else
          pad = int(p - begin);
      } else {
        // Other lines, skip |pad| bytes
        int prefix = pad;
        while (prefix > 0 && p < E &&
               (*p == ' ' || *p == '/' || *p == '*' || *p == '<' || *p == '!'))
          prefix--, p++;
      }
      ret.insert(ret.end(), p, q);
      p = q;
    }
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    if (StringRef(ret).endswith("*/"))
      ret.resize(ret.size() - 2);
    else if (StringRef(ret).endswith("\n/"))
      ret.resize(ret.size() - 2);
    while (ret.size() && isspace(ret.back()))
      ret.pop_back();
    return ret;
  }

  Usr getUsr(const Decl *D, IndexParam::DeclInfo **info = nullptr) const {
    D = D->getCanonicalDecl();
    auto [it, inserted] = param.Decl2Info.try_emplace(D);
    if (inserted) {
      SmallString<256> USR;
      index::generateUSRForDecl(D, USR);
      auto &info = it->second;
      info.usr = hashUsr(USR);
      if (auto *ND = dyn_cast<NamedDecl>(D)) {
        info.short_name = ND->getNameAsString();
        llvm::raw_string_ostream OS(info.qualified);
        ND->printQualifiedName(OS, getDefaultPolicy());
        simplifyAnonymous(info.qualified);
      }
    }
    if (info)
      *info = &it->second;
    return it->second.usr;
  }

  PrintingPolicy getDefaultPolicy() const {
    PrintingPolicy PP(Ctx->getLangOpts());
    PP.AnonymousTagLocations = false;
    PP.TerseOutput = true;
    PP.PolishForDeclaration = true;
    PP.ConstantsAsWritten = true;
    PP.SuppressTagKeyword = true;
    PP.SuppressUnwrittenScope = g_config->index.name.suppressUnwrittenScope;
    PP.SuppressInitializers = true;
    PP.FullyQualifiedName = false;
    return PP;
  }

  static void simplifyAnonymous(std::string &name) {
    for (std::string::size_type i = 0;;) {
      if ((i = name.find("(anonymous ", i)) == std::string::npos)
        break;
      i++;
      if (name.size() - i > 19 && name.compare(i + 10, 9, "namespace") == 0)
        name.replace(i, 19, "anon ns");
      else
        name.replace(i, 9, "anon");
    }
  }

  template <typename Def>
  void setName(const Decl *D, std::string_view short_name,
               std::string_view qualified, Def &def) {
    SmallString<256> Str;
    llvm::raw_svector_ostream OS(Str);
    D->print(OS, getDefaultPolicy());

    std::string name = OS.str();
    simplifyAnonymous(name);
    // Remove \n in DeclPrinter.cpp "{\n" + if(!TerseOutput)something + "}"
    for (std::string::size_type i = 0;;) {
      if ((i = name.find("{\n}", i)) == std::string::npos)
        break;
      name.replace(i, 3, "{}");
    }
    auto i = name.find(short_name);
    if (short_name.size())
      while (i != std::string::npos && ((i && isIdentifierBody(name[i - 1])) ||
                                        isIdentifierBody(name[i + short_name.size()])))
        i = name.find(short_name, i + short_name.size());
    if (i == std::string::npos) {
      // e.g. operator type-parameter-1
      i = 0;
      def.short_name_offset = 0;
    } else if (short_name.empty() || (i >= 2 && name[i - 2] == ':')) {
      // Don't replace name with qualified name in ns::name Cls::*name
      def.short_name_offset = i;
    } else {
      name.replace(i, short_name.size(), qualified);
      def.short_name_offset = i + qualified.size() - short_name.size();
    }
    def.short_name_size = short_name.size();
    for (int paren = 0; i; i--) {
      // Skip parentheses in "(anon struct)::name"
      if (name[i - 1] == ')')
        paren++;
      else if (name[i - 1] == '(')
        paren--;
      else if (!(paren > 0 || isIdentifierBody(name[i - 1]) ||
                 name[i - 1] == ':'))
        break;
    }
    def.qual_name_offset = i;
    def.detailed_name = intern(name);
  }

  void setVarName(const Decl *D, std::string_view short_name,
                  std::string_view qualified, IndexVar::Def &def) {
    QualType T;
    const Expr *init = nullptr;
    bool deduced = false;
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      T = VD->getType();
      init = VD->getAnyInitializer();
      def.storage = VD->getStorageClass();
    } else if (auto *FD = dyn_cast<FieldDecl>(D)) {
      T = FD->getType();
      init = FD->getInClassInitializer();
    } else if (auto *BD = dyn_cast<BindingDecl>(D)) {
      T = BD->getType();
      deduced = true;
    }
    if (!T.isNull()) {
      if (T->getContainedDeducedType()) {
        deduced = true;
      } else if (auto *DT = dyn_cast<DecltypeType>(T)) {
        // decltype(y) x;
        while (DT && !DT->getUnderlyingType().isNull()) {
          T = DT->getUnderlyingType();
          DT = dyn_cast<DecltypeType>(T);
        }
        deduced = true;
      }
    }
    if (!T.isNull() && deduced) {
      SmallString<256> Str;
      llvm::raw_svector_ostream OS(Str);
      PrintingPolicy PP = getDefaultPolicy();
      T.print(OS, PP);
      if (Str.size() &&
          (Str.back() != ' ' && Str.back() != '*' && Str.back() != '&'))
        Str += ' ';
      def.qual_name_offset = Str.size();
      def.short_name_offset = Str.size() + qualified.size() - short_name.size();
      def.short_name_size = short_name.size();
      Str += StringRef(qualified.data(), qualified.size());
      def.detailed_name = intern(Str);
    } else {
      setName(D, short_name, qualified, def);
    }
    if (init) {
      SourceManager &SM = Ctx->getSourceManager();
      const LangOptions &Lang = Ctx->getLangOpts();
      SourceRange R = SM.getExpansionRange(init->getSourceRange()).getAsRange();
      SourceLocation L = D->getLocation();
      if (L.isMacroID() || !SM.isBeforeInTranslationUnit(L, R.getBegin()))
        return;
      StringRef Buf = getSourceInRange(SM, Lang, R);
      Twine Init = Buf.count('\n') <= g_config->index.maxInitializerLines - 1
                       ? Buf.size() && Buf[0] == ':' ? Twine(" ", Buf)
                                                     : Twine(" = ", Buf)
                       : Twine();
      Twine T = def.detailed_name + Init;
      def.hover =
          def.storage == SC_Static && strncmp(def.detailed_name, "static ", 7)
              ? intern(("static " + T).str())
              : intern(T.str());
    }
  }

  static int getFileLID(IndexFile *db, SourceManager &SM, FileID fid) {
    auto [it, inserted] = db->uid2lid_and_path.try_emplace(fid);
    if (inserted) {
      const FileEntry *fe = SM.getFileEntryForID(fid);
      if (!fe)
        return -1;
      it->second.first = db->uid2lid_and_path.size() - 1;
      SmallString<256> path = fe->tryGetRealPathName();
      if (path.empty())
        path = fe->getName();
      if (!llvm::sys::path::is_absolute(path) &&
          !SM.getFileManager().makeAbsolutePath(path))
        return -1;
      it->second.second = llvm::sys::path::convert_to_slash(path.str());
    }
    return it->second.first;
  }

  void addMacroUse(IndexFile *db, SourceManager &sm, Usr usr, Kind kind,
                   SourceLocation sl) const {
    FileID fid = sm.getFileID(sl);
    int lid = getFileLID(db, sm, fid);
    if (lid < 0)
      return;
    Range spell = fromTokenRange(sm, Ctx->getLangOpts(), SourceRange(sl, sl));
    Use use{{spell, Role::Dynamic}, lid};
    switch (kind) {
    case Kind::Func:
      db->toFunc(usr).uses.push_back(use);
      break;
    case Kind::Type:
      db->toType(usr).uses.push_back(use);
      break;
    case Kind::Var:
      db->toVar(usr).uses.push_back(use);
      break;
    default:
      llvm_unreachable("");
    }
  }

  void collectRecordMembers(IndexType &type, const RecordDecl *RD) {
    SmallVector<std::pair<const RecordDecl *, int>, 2> Stack{{RD, 0}};
    llvm::DenseSet<const RecordDecl *> Seen;
    Seen.insert(RD);
    while (Stack.size()) {
      int offset;
      std::tie(RD, offset) = Stack.back();
      Stack.pop_back();
      if (!RD->isCompleteDefinition() || RD->isDependentType() ||
          RD->isInvalidDecl() || !validateRecord(RD))
        offset = -1;
      for (FieldDecl *FD : RD->fields()) {
        int offset1 = offset < 0 ? -1 : offset + Ctx->getFieldOffset(FD);
        if (FD->getIdentifier())
          type.def.vars.emplace_back(getUsr(FD), offset1);
        else if (const auto *RT1 = FD->getType()->getAs<RecordType>()) {
          if (const RecordDecl *RD1 = RT1->getDecl())
            if (Seen.insert(RD1).second)
              Stack.push_back({RD1, offset1});
        }
      }
    }
  }

public:
  IndexDataConsumer(IndexParam &param) : param(param) {}
  void initialize(ASTContext &ctx) override { this->Ctx = param.ctx = &ctx; }
  bool handleDeclOccurence(const Decl *D, index::SymbolRoleSet Roles,
                           ArrayRef<index::SymbolRelation> Relations,
                           SourceLocation Loc, ASTNodeInfo ASTNode) override {
    if (!param.no_linkage) {
      if (auto *ND = dyn_cast<NamedDecl>(D); ND && ND->hasLinkage())
        ;
      else
        return true;
    }
    SourceManager &SM = Ctx->getSourceManager();
    const LangOptions &Lang = Ctx->getLangOpts();
    FileID fid;
    SourceLocation Spell = SM.getSpellingLoc(Loc);
    Range loc;
    auto R = SM.isMacroArgExpansion(Loc) ? CharSourceRange::getTokenRange(Spell)
                                         : SM.getExpansionRange(Loc);
    loc = fromCharSourceRange(SM, Lang, R);
    fid = SM.getFileID(R.getBegin());
    if (fid.isInvalid())
      return true;
    int lid = -1;
    IndexFile *db;
    if (g_config->index.multiVersion && param.useMultiVersion(fid)) {
      db = param.consumeFile(SM.getMainFileID());
      if (!db)
        return true;
      param.seenFile(fid);
      if (!SM.isWrittenInMainFile(R.getBegin()))
        lid = getFileLID(db, SM, fid);
    } else {
      db = param.consumeFile(fid);
      if (!db)
        return true;
    }

    // spell, extent, comments use OrigD while most others use adjusted |D|.
    const Decl *OrigD = ASTNode.OrigD;
    const DeclContext *SemDC = OrigD->getDeclContext()->getRedeclContext();
    const DeclContext *LexDC = ASTNode.ContainerDC->getRedeclContext();
    {
      const NamespaceDecl *ND;
      while ((ND = dyn_cast<NamespaceDecl>(cast<Decl>(SemDC))) &&
             ND->isAnonymousNamespace())
        SemDC = ND->getDeclContext()->getRedeclContext();
      while ((ND = dyn_cast<NamespaceDecl>(cast<Decl>(LexDC))) &&
             ND->isAnonymousNamespace())
        LexDC = ND->getDeclContext()->getRedeclContext();
    }
    Role role = static_cast<Role>(Roles);
    db->language = LanguageId((int)db->language | (int)getDeclLanguage(D));

    bool is_decl = Roles & uint32_t(index::SymbolRole::Declaration);
    bool is_def = Roles & uint32_t(index::SymbolRole::Definition);
    if (is_decl && D->getKind() == Decl::Binding)
      is_def = true;
    IndexFunc *func = nullptr;
    IndexType *type = nullptr;
    IndexVar *var = nullptr;
    SymbolKind ls_kind = SymbolKind::Unknown;
    Kind kind = getKind(D, ls_kind);

    if (is_def)
      switch (D->getKind()) {
      case Decl::CXXConversion: // *operator* int => *operator int*
      case Decl::CXXDestructor: // *~*A => *~A*
      case Decl::CXXMethod: // *operator*= => *operator=*
      case Decl::Function: // operator delete
        if (Loc.isFileID()) {
          SourceRange R =
              cast<FunctionDecl>(OrigD)->getNameInfo().getSourceRange();
          if (R.getEnd().isFileID())
            loc = fromTokenRange(SM, Lang, R);
        }
        break;
      default:
        break;
      }
    else {
      // e.g. typedef Foo<int> gg; => Foo has an unadjusted `D`
      const Decl *D1 = getAdjustedDecl(D);
      if (D1 && D1 != D)
        D = D1;
    }

    IndexParam::DeclInfo *info;
    Usr usr = getUsr(D, &info);

    auto do_def_decl = [&](auto *entity) {
      Use use{{loc, role}, lid};
      if (is_def) {
        SourceRange R = OrigD->getSourceRange();
        entity->def.spell = {use,
                             fromTokenRangeDefaulted(SM, Lang, R, fid, loc)};
        getKind(cast<Decl>(SemDC), entity->def.parent_kind);
      } else if (is_decl) {
        SourceRange R = OrigD->getSourceRange();
        entity->declarations.push_back(
            {use, fromTokenRangeDefaulted(SM, Lang, R, fid, loc)});
      } else {
        entity->uses.push_back(use);
        return;
      }
      if (entity->def.comments[0] == '\0' && g_config->index.comments)
        entity->def.comments = intern(getComment(OrigD));
    };
    switch (kind) {
    case Kind::Invalid:
      if (ls_kind == SymbolKind::Unknown)
        LOG_S(INFO) << "Unhandled " << int(D->getKind()) << " "
                    << info->qualified << " in " << db->path << ":"
                    << (loc.start.line + 1) << ":" << (loc.start.column + 1);
      return true;
    case Kind::File:
      return true;
    case Kind::Func:
      func = &db->toFunc(usr);
      func->def.kind = ls_kind;
      // Mark as Role::Implicit to span one more column to the left/right.
      if (!is_def && !is_decl &&
          (D->getKind() == Decl::CXXConstructor ||
           D->getKind() == Decl::CXXConversion))
        role = Role(role | Role::Implicit);
      do_def_decl(func);
      if (Spell != Loc)
        addMacroUse(db, SM, usr, Kind::Func, Spell);
      if (func->def.detailed_name[0] == '\0')
        setName(D, info->short_name, info->qualified, func->def);
      if (is_def || is_decl) {
        const Decl *DC = cast<Decl>(SemDC);
        if (getKind(DC, ls_kind) == Kind::Type)
          db->toType(getUsr(DC)).def.funcs.push_back(usr);
      } else {
        const Decl *DC = cast<Decl>(LexDC);
        if (getKind(DC, ls_kind) == Kind::Func)
          db->toFunc(getUsr(DC))
              .def.callees.push_back({loc, usr, Kind::Func, role});
      }
      break;
    case Kind::Type:
      type = &db->toType(usr);
      type->def.kind = ls_kind;
      do_def_decl(type);
      if (Spell != Loc)
        addMacroUse(db, SM, usr, Kind::Type, Spell);
      if ((is_def || type->def.detailed_name[0] == '\0') &&
          info->short_name.size()) {
        if (D->getKind() == Decl::TemplateTypeParm)
          type->def.detailed_name = intern(info->short_name);
        else
          // OrigD may be detailed, e.g. "struct D : B {}"
          setName(OrigD, info->short_name, info->qualified, type->def);
      }
      if (is_def || is_decl) {
        const Decl *DC = cast<Decl>(SemDC);
        if (getKind(DC, ls_kind) == Kind::Type)
          db->toType(getUsr(DC)).def.types.push_back(usr);
      }
      break;
    case Kind::Var:
      var = &db->toVar(usr);
      var->def.kind = ls_kind;
      do_def_decl(var);
      if (Spell != Loc)
        addMacroUse(db, SM, usr, Kind::Var, Spell);
      if (var->def.detailed_name[0] == '\0')
        setVarName(D, info->short_name, info->qualified, var->def);
      QualType T;
      if (auto *VD = dyn_cast<ValueDecl>(D))
        T = VD->getType();
      if (is_def || is_decl) {
        const Decl *DC = cast<Decl>(SemDC);
        Kind kind = getKind(DC, var->def.parent_kind);
        if (kind == Kind::Func)
          db->toFunc(getUsr(DC)).def.vars.push_back(usr);
        else if (kind == Kind::Type && !isa<RecordDecl>(SemDC))
          db->toType(getUsr(DC)).def.vars.emplace_back(usr, -1);
        if (!T.isNull()) {
          if (auto *BT = T->getAs<BuiltinType>()) {
            Usr usr1 = static_cast<Usr>(BT->getKind());
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(D))
              db->toType(usr1).instances.push_back(usr);
          } else if (const Decl *D1 = getAdjustedDecl(getTypeDecl(T))) {
#if LLVM_VERSION_MAJOR < 9
            if (isa<TemplateTypeParmDecl>(D1)) {
              // e.g. TemplateTypeParmDecl is not handled by
              // handleDeclOccurence.
              SourceRange R1 = D1->getSourceRange();
              if (SM.getFileID(R1.getBegin()) == fid) {
                IndexParam::DeclInfo *info1;
                Usr usr1 = getUsr(D1, &info1);
                IndexType &type1 = db->toType(usr1);
                SourceLocation L1 = D1->getLocation();
                type1.def.spell = {
                    Use{{fromTokenRange(SM, Lang, {L1, L1}), Role::Definition},
                        lid},
                    fromTokenRange(SM, Lang, R1)};
                type1.def.detailed_name = intern(info1->short_name);
                type1.def.short_name_size = int16_t(info1->short_name.size());
                type1.def.kind = SymbolKind::TypeParameter;
                type1.def.parent_kind = SymbolKind::Class;
                var->def.type = usr1;
                type1.instances.push_back(usr);
                break;
              }
            }
#endif

            IndexParam::DeclInfo *info1;
            Usr usr1 = getUsr(D1, &info1);
            var->def.type = usr1;
            if (!isa<EnumConstantDecl>(D))
              db->toType(usr1).instances.push_back(usr);
          }
        }
      } else if (!var->def.spell && var->declarations.empty()) {
        // e.g. lambda parameter
        SourceLocation L = D->getLocation();
        if (SM.getFileID(L) == fid) {
          var->def.spell = {
              Use{{fromTokenRange(SM, Lang, {L, L}), Role::Definition}, lid},
              fromTokenRange(SM, Lang, D->getSourceRange())};
          var->def.parent_kind = SymbolKind::Method;
        }
      }
      break;
    }

    switch (D->getKind()) {
    case Decl::Namespace:
      if (D->isFirstDecl()) {
        auto *ND = cast<NamespaceDecl>(D);
        auto *ND1 = cast<Decl>(ND->getParent());
        if (isa<NamespaceDecl>(ND1)) {
          Usr usr1 = getUsr(ND1);
          type->def.bases.push_back(usr1);
          db->toType(usr1).derived.push_back(usr);
        }
      }
      break;
    case Decl::NamespaceAlias: {
      auto *NAD = cast<NamespaceAliasDecl>(D);
      if (const NamespaceDecl *ND = NAD->getNamespace()) {
        Usr usr1 = getUsr(ND);
        type->def.alias_of = usr1;
        (void)db->toType(usr1);
      }
      break;
    }
    case Decl::CXXRecord:
      if (is_def) {
        auto *RD = dyn_cast<CXXRecordDecl>(D);
        if (RD && RD->hasDefinition())
          for (const CXXBaseSpecifier &Base : RD->bases())
            if (const Decl *BaseD =
                    getAdjustedDecl(getTypeDecl(Base.getType()))) {
              Usr usr1 = getUsr(BaseD);
              type->def.bases.push_back(usr1);
              db->toType(usr1).derived.push_back(usr);
            }
      }
      [[fallthrough]];
    case Decl::Record:
      if (auto *RD = dyn_cast<RecordDecl>(D)) {
        if (type->def.detailed_name[0] == '\0' && info->short_name.empty()) {
          StringRef Tag;
          switch (RD->getTagKind()) {
          case TTK_Struct: Tag = "struct"; break;
          case TTK_Interface: Tag = "__interface"; break;
          case TTK_Union: Tag = "union"; break;
          case TTK_Class: Tag = "class"; break;
          case TTK_Enum: Tag = "enum"; break;
          }
          if (TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl()) {
            StringRef Name = TD->getName();
            std::string name = ("anon " + Tag + " " + Name).str();
            type->def.detailed_name = intern(name);
            type->def.short_name_size = name.size();
          } else {
            std::string name = ("anon " + Tag).str();
            type->def.detailed_name = intern(name);
            type->def.short_name_size = name.size();
          }
        }
        if (is_def)
          if (auto *ORD = dyn_cast<RecordDecl>(OrigD))
            collectRecordMembers(*type, ORD);
      }
      break;
    case Decl::ClassTemplateSpecialization:
    case Decl::ClassTemplatePartialSpecialization:
      type->def.kind = SymbolKind::Class;
      if (is_def) {
        if (auto *ORD = dyn_cast<RecordDecl>(OrigD))
          collectRecordMembers(*type, ORD);
        if (auto *RD = dyn_cast<CXXRecordDecl>(D)) {
          Decl *D1 = nullptr;
          if (auto *SD = dyn_cast<ClassTemplatePartialSpecializationDecl>(RD))
            D1 = SD->getSpecializedTemplate();
          else if (auto *SD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
            llvm::PointerUnion<ClassTemplateDecl *,
                               ClassTemplatePartialSpecializationDecl *>
                Result = SD->getSpecializedTemplateOrPartial();
            if (Result.is<ClassTemplateDecl *>())
              D1 = Result.get<ClassTemplateDecl *>();
            else
              D1 = Result.get<ClassTemplatePartialSpecializationDecl *>();

          } else
            D1 = RD->getInstantiatedFromMemberClass();
          if (D1) {
            Usr usr1 = getUsr(D1);
            type->def.bases.push_back(usr1);
            db->toType(usr1).derived.push_back(usr);
          }
        }
      }
      break;
    case Decl::TypeAlias:
    case Decl::Typedef:
    case Decl::UnresolvedUsingTypename:
      if (auto *TD = dyn_cast<TypedefNameDecl>(D)) {
        bool specialization = false;
        QualType T = TD->getUnderlyingType();
        if (const Decl *D1 = getAdjustedDecl(getTypeDecl(T, &specialization))) {
          Usr usr1 = getUsr(D1);
          IndexType &type1 = db->toType(usr1);
          type->def.alias_of = usr1;
          // Not visited template<class T> struct B {typedef A<T> t;};
          if (specialization) {
            const TypeSourceInfo *TSI = TD->getTypeSourceInfo();
            SourceLocation L1 = TSI->getTypeLoc().getBeginLoc();
            if (SM.getFileID(L1) == fid)
              type1.uses.push_back(
                  {{fromTokenRange(SM, Lang, {L1, L1}), Role::Reference}, lid});
          }
        }
      }
      break;
    case Decl::CXXMethod:
      if (is_def || is_decl) {
        if (auto *ND = dyn_cast<NamedDecl>(D)) {
          SmallVector<const NamedDecl *, 8> OverDecls;
          Ctx->getOverriddenMethods(ND, OverDecls);
          for (const auto *ND1 : OverDecls) {
            Usr usr1 = getUsr(ND1);
            func->def.bases.push_back(usr1);
            db->toFunc(usr1).derived.push_back(usr);
          }
        }
      }
      break;
    case Decl::EnumConstant:
      if (is_def && strchr(var->def.detailed_name, '=') == nullptr) {
        auto *ECD = cast<EnumConstantDecl>(D);
        const auto &Val = ECD->getInitVal();
        std::string init =
            " = " + (Val.isSigned() ? std::to_string(Val.getSExtValue())
                                    : std::to_string(Val.getZExtValue()));
        var->def.hover = intern(var->def.detailed_name + init);
      }
      break;
    default:
      break;
    }
    return true;
  }
};

class IndexPPCallbacks : public PPCallbacks {
  SourceManager &sm;
  IndexParam &param;

  std::pair<StringRef, Usr> getMacro(const Token &Tok) const {
    StringRef Name = Tok.getIdentifierInfo()->getName();
    SmallString<256> USR("@macro@");
    USR += Name;
    return {Name, hashUsr(USR)};
  }

public:
  IndexPPCallbacks(SourceManager &SM, IndexParam &param)
      : sm(SM), param(param) {}
  void FileChanged(SourceLocation sl, FileChangeReason reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (reason == FileChangeReason::EnterFile)
      (void)param.consumeFile(sm.getFileID(sl));
  }
  void InclusionDirective(SourceLocation HashLoc, const Token &Tok,
                          StringRef Included, bool IsAngled,
                          CharSourceRange FilenameRange, const FileEntry *File,
                          StringRef SearchPath, StringRef RelativePath,
                          const Module *Imported,
                          SrcMgr::CharacteristicKind FileType) override {
    if (!File)
      return;
    auto spell = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                     FilenameRange, nullptr);
    FileID fid = sm.getFileID(FilenameRange.getBegin());
    if (IndexFile *db = param.consumeFile(fid)) {
      std::string path = pathFromFileEntry(*File);
      if (path.size())
        db->includes.push_back({spell.start.line, intern(path)});
    }
  }
  void MacroDefined(const Token &Tok, const MacroDirective *MD) override {
    const LangOptions &lang = param.ctx->getLangOpts();
    SourceLocation sl = MD->getLocation();
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) {
      auto [Name, usr] = getMacro(Tok);
      IndexVar &var = db->toVar(usr);
      Range range = fromTokenRange(sm, lang, {sl, sl}, nullptr);
      var.def.kind = SymbolKind::Macro;
      var.def.parent_kind = SymbolKind::File;
      if (var.def.spell)
        var.declarations.push_back(*var.def.spell);
      const MacroInfo *MI = MD->getMacroInfo();
      SourceRange R(MI->getDefinitionLoc(), MI->getDefinitionEndLoc());
      Range extent = fromTokenRange(sm, param.ctx->getLangOpts(), R);
      var.def.spell = {Use{{range, Role::Definition}}, extent};
      if (var.def.detailed_name[0] == '\0') {
        var.def.detailed_name = intern(Name);
        var.def.short_name_size = Name.size();
        StringRef Buf = getSourceInRange(sm, lang, R);
        var.def.hover =
            intern(Buf.count('\n') <= g_config->index.maxInitializerLines - 1
                       ? Twine("#define ", getSourceInRange(sm, lang, R)).str()
                       : Twine("#define ", Name).str());
      }
    }
  }
  void MacroExpands(const Token &tok, const MacroDefinition &, SourceRange sr,
                    const MacroArgs *) override {
    SourceLocation sl = sm.getSpellingLoc(sr.getBegin());
    FileID fid = sm.getFileID(sl);
    if (IndexFile *db = param.consumeFile(fid)) {
      IndexVar &var = db->toVar(getMacro(tok).second);
      var.uses.push_back(
          {{fromTokenRange(sm, param.ctx->getLangOpts(), {sl, sl}, nullptr),
            Role::Dynamic}});
    }
  }
  void MacroUndefined(const Token &tok, const MacroDefinition &md,
                      const MacroDirective *ud) override {
    if (ud) {
      SourceLocation sl = ud->getLocation();
      MacroExpands(tok, md, {sl, sl}, nullptr);
    }
  }
  void SourceRangeSkipped(SourceRange r, SourceLocation EndifLoc) override {
    Range range = fromCharSourceRange(sm, param.ctx->getLangOpts(),
                                      CharSourceRange::getCharRange(r));
    FileID fid = sm.getFileID(r.getBegin());
    if (fid.isValid())
      if (IndexFile *db = param.consumeFile(fid))
        db->skipped_ranges.push_back(range);
  }
};

class IndexFrontendAction : public ASTFrontendAction {
  IndexParam &param;

public:
  IndexFrontendAction(IndexParam &param) : param(param) {}
  std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                 StringRef InFile) override {
    class SkipProcessed : public ASTConsumer {
      IndexParam &param;
      const ASTContext *ctx = nullptr;

    public:
      SkipProcessed(IndexParam &param) : param(param) {}
      void Initialize(ASTContext &ctx) override { this->ctx = &ctx; }
      bool shouldSkipFunctionBody(Decl *d) override {
        const SourceManager &sm = ctx->getSourceManager();
        FileID fid = sm.getFileID(sm.getExpansionLoc(d->getLocation()));
        return !(g_config->index.multiVersion && param.useMultiVersion(fid)) &&
               !param.consumeFile(fid);
      }
    };

    Preprocessor &PP = CI.getPreprocessor();
    PP.addPPCallbacks(
        std::make_unique<IndexPPCallbacks>(PP.getSourceManager(), param));
    std::vector<std::unique_ptr<ASTConsumer>> Consumers;
    Consumers.push_back(std::make_unique<SkipProcessed>(param));
    Consumers.push_back(std::make_unique<ASTConsumer>());
    return std::make_unique<MultiplexConsumer>(std::move(Consumers));
  }
};
} // namespace

const int IndexFile::kMajorVersion = 21;
const int IndexFile::kMinorVersion = 0;

IndexFile::IndexFile(const std::string &path, const std::string &contents,
                     bool no_linkage)
    : path(path), no_linkage(no_linkage), file_contents(contents) {}

IndexFunc &IndexFile::toFunc(Usr usr) {
  auto [it, inserted] = usr2func.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
}

IndexType &IndexFile::toType(Usr usr) {
  auto [it, inserted] = usr2type.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
}

IndexVar &IndexFile::toVar(Usr usr) {
  auto [it, inserted] = usr2var.try_emplace(usr);
  if (inserted)
    it->second.usr = usr;
  return it->second;
}

std::string IndexFile::toString() {
  return ccls::serialize(SerializeFormat::Json, *this);
}

template <typename T> void uniquify(std::vector<T> &a) {
  std::unordered_set<T> seen;
  size_t n = 0;
  for (size_t i = 0; i < a.size(); i++)
    if (seen.insert(a[i]).second)
      a[n++] = a[i];
  a.resize(n);
}

namespace idx {
void init() {
  multiVersionMatcher = new GroupMatch(g_config->index.multiVersionWhitelist,
                                       g_config->index.multiVersionBlacklist);
}

std::vector<std::unique_ptr<IndexFile>>
index(SemaManager *manager, WorkingFiles *wfiles, VFS *vfs,
      const std::string &opt_wdir, const std::string &main,
      const std::vector<const char *> &args,
      const std::vector<std::pair<std::string, std::string>> &remapped,
      bool no_linkage, bool &ok) {
  ok = true;
  auto PCH = std::make_shared<PCHContainerOperations>();
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS = llvm::vfs::getRealFileSystem();
  std::shared_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(main, args, FS);
  // e.g. .s
  if (!CI)
    return {};
  ok = false;
  // Disable computing warnings which will be discarded anyway.
  CI->getDiagnosticOpts().IgnoreWarnings = true;
  // Enable IndexFrontendAction::shouldSkipFunctionBody.
  CI->getFrontendOpts().SkipFunctionBodies = true;
  // -fparse-all-comments enables documentation in the indexer and in
  // code completion.
  CI->getLangOpts()->CommentOpts.ParseAllComments = g_config->index.comments > 1;
  CI->getLangOpts()->RetainCommentsFromSystemHeaders = true;
  std::string buf = wfiles->getContent(main);
  std::vector<std::unique_ptr<llvm::MemoryBuffer>> Bufs;
  if (buf.size())
    for (auto &[filename, content] : remapped) {
      Bufs.push_back(llvm::MemoryBuffer::getMemBuffer(content));
      CI->getPreprocessorOpts().addRemappedFile(filename, Bufs.back().get());
    }

  DiagnosticConsumer DC;
  auto Clang = std::make_unique<CompilerInstance>(PCH);
  Clang->setInvocation(std::move(CI));
  Clang->createDiagnostics(&DC, false);
  Clang->getDiagnostics().setIgnoreAllWarnings(true);
  Clang->setTarget(TargetInfo::CreateTargetInfo(
      Clang->getDiagnostics(), Clang->getInvocation().TargetOpts));
  if (!Clang->hasTarget())
    return {};
  Clang->getPreprocessorOpts().RetainRemappedFileBuffers = true;
#if LLVM_VERSION_MAJOR >= 9 // rC357037
  Clang->createFileManager(FS);
#else
  Clang->setVirtualFileSystem(FS);
  Clang->createFileManager();
#endif
  Clang->setSourceManager(new SourceManager(Clang->getDiagnostics(),
                                            Clang->getFileManager(), true));

  IndexParam param(*vfs, no_linkage);
  auto DataConsumer = std::make_shared<IndexDataConsumer>(param);

  index::IndexingOptions IndexOpts;
  IndexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  if (no_linkage) {
    IndexOpts.IndexFunctionLocals = true;
    IndexOpts.IndexImplicitInstantiation = true;
#if LLVM_VERSION_MAJOR >= 9

    IndexOpts.IndexParametersInDeclarations =
        g_config->index.parametersInDeclarations;
    IndexOpts.IndexTemplateParameters = true;
#endif
  }

  std::unique_ptr<FrontendAction> Action = createIndexingAction(
      DataConsumer, IndexOpts, std::make_unique<IndexFrontendAction>(param));
  std::string reason;
  {
    llvm::CrashRecoveryContext CRC;
    auto parse = [&]() {
      if (!Action->BeginSourceFile(*Clang, Clang->getFrontendOpts().Inputs[0]))
        return;
#if LLVM_VERSION_MAJOR >= 9 // rL364464
      if (llvm::Error E = Action->Execute()) {
        reason = llvm::toString(std::move(E));
        return;
      }
#else
      if (!Action->Execute())
        return;
#endif
      Action->EndSourceFile();
      ok = true;
    };
    if (!CRC.RunSafely(parse)) {
      LOG_S(ERROR) << "clang crashed for " << main;
      return {};
    }
  }
  if (!ok) {
    LOG_S(ERROR) << "failed to index " << main
                 << (reason.empty() ? "" : ": " + reason);
    return {};
  }

  std::vector<std::unique_ptr<IndexFile>> result;
  for (auto &it : param.uid2file) {
    if (!it.second.db)
      continue;
    std::unique_ptr<IndexFile> &entry = it.second.db;
    entry->import_file = main;
    entry->args = args;
    for (auto &[_, it] : entry->uid2lid_and_path)
      entry->lid2path.emplace_back(it.first, std::move(it.second));
    entry->uid2lid_and_path.clear();
    for (auto &it : entry->usr2func) {
      // e.g. declaration + out-of-line definition
      uniquify(it.second.derived);
      uniquify(it.second.uses);
    }
    for (auto &it : entry->usr2type) {
      uniquify(it.second.derived);
      uniquify(it.second.uses);
      // e.g. declaration + out-of-line definition
      uniquify(it.second.def.bases);
      uniquify(it.second.def.funcs);
    }
    for (auto &it : entry->usr2var)
      uniquify(it.second.uses);

    // Update dependencies for the file.
    for (auto &[_, file] : param.uid2file) {
      const std::string &path = file.path;
      if (path.empty())
        continue;
      if (path == entry->path)
        entry->mtime = file.mtime;
      else if (path != entry->import_file)
        entry->dependencies[llvm::CachedHashStringRef(intern(path))] =
            file.mtime;
    }
    result.push_back(std::move(entry));
  }

  return result;
}
} // namespace idx

void reflect(JsonReader &vis, SymbolRef &v) {
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.usr = strtoull(s + 1, &s, 10);
  v.kind = static_cast<Kind>(strtol(s + 1, &s, 10));
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
}
void reflect(JsonReader &vis, Use &v) {
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
}
void reflect(JsonReader &vis, DeclRef &v) {
  std::string t = vis.getString();
  char *s = const_cast<char *>(t.c_str());
  v.range = Range::fromString(s);
  s = strchr(s, '|') + 1;
  v.extent = Range::fromString(s);
  s = strchr(s, '|');
  v.role = static_cast<Role>(strtol(s + 1, &s, 10));
  v.file_id = static_cast<int>(strtol(s + 1, &s, 10));
}

void reflect(JsonWriter &vis, SymbolRef &v) {
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%" PRIu64 "|%d|%d", v.range.toString().c_str(),
           v.usr, int(v.kind), int(v.role));
  std::string s(buf);
  reflect(vis, s);
}
void reflect(JsonWriter &vis, Use &v) {
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%d|%d", v.range.toString().c_str(), int(v.role),
           v.file_id);
  std::string s(buf);
  reflect(vis, s);
}
void reflect(JsonWriter &vis, DeclRef &v) {
  char buf[99];
  snprintf(buf, sizeof buf, "%s|%s|%d|%d", v.range.toString().c_str(),
           v.extent.toString().c_str(), int(v.role), v.file_id);
  std::string s(buf);
  reflect(vis, s);
}

void reflect(BinaryReader &vis, SymbolRef &v) {
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
}
void reflect(BinaryReader &vis, Use &v) {
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
}
void reflect(BinaryReader &vis, DeclRef &v) {
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
}

void reflect(BinaryWriter &vis, SymbolRef &v) {
  reflect(vis, v.range);
  reflect(vis, v.usr);
  reflect(vis, v.kind);
  reflect(vis, v.role);
}
void reflect(BinaryWriter &vis, Use &v) {
  reflect(vis, v.range);
  reflect(vis, v.role);
  reflect(vis, v.file_id);
}
void reflect(BinaryWriter &vis, DeclRef &v) {
  reflect(vis, static_cast<Use &>(v));
  reflect(vis, v.extent);
}
} // namespace ccls
