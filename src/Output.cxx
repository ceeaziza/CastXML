/*
  Copyright Kitware, Inc.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "Output.h"
#include "Utils.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclFriend.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclOpenMP.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <iostream>
#include <queue>

//----------------------------------------------------------------------------
class ASTVisitorBase
{
protected:
  clang::CompilerInstance& CI;
  clang::ASTContext const& CTX;
  llvm::raw_ostream& OS;

  ASTVisitorBase(clang::CompilerInstance& ci,
                 clang::ASTContext const& ctx,
                 llvm::raw_ostream& os): CI(ci), CTX(ctx), OS(os) {}

  // Record status of one AST node to be dumped.
  struct DumpNode {
    DumpNode(): Index(0), Complete(false) {}

    // Index in nodes ordered by first encounter.
    unsigned int Index;

    // Whether the node is to be traversed completely.
    bool Complete;
  };

  // Report all decl nodes as unimplemented until overridden.
#define ABSTRACT_DECL(DECL)
#define DECL(CLASS, BASE) \
  void Output##CLASS##Decl(clang::CLASS##Decl const* d, DumpNode const* dn) { \
    this->OutputUnimplementedDecl(d, dn); \
  }
#include "clang/AST/DeclNodes.inc"

  void OutputUnimplementedDecl(clang::Decl const* d, DumpNode const* dn) {
    this->OS << "  <Unimplemented id=\"_" << dn->Index
             << "\" kind=\"" << encodeXML(d->getDeclKindName()) << "\"/>\n";
  }

  // Report all type nodes as unimplemented until overridden.
#define ABSTRACT_TYPE(CLASS, BASE)
#define TYPE(CLASS, BASE) \
  void Output##CLASS##Type(clang::CLASS##Type const* t, DumpNode const* dn) { \
    this->OutputUnimplementedType(t, dn); \
    }
#include "clang/AST/TypeNodes.def"

  void OutputUnimplementedType(clang::Type const* t, DumpNode const* dn) {
    this->OS << "  <Unimplemented id=\"_" << dn->Index
             << "\" type_class=\"" << encodeXML(t->getTypeClassName())
             << "\"/>\n";
  }
};

//----------------------------------------------------------------------------
class ASTVisitor: public ASTVisitorBase
{
  // Store an entry in the node traversal queue.
  struct QueueEntry {
    // Available node kinds.
    enum Kinds {
      KindDecl,
      KindType
    };

    QueueEntry(): Kind(KindDecl), Decl(0), DN(0) {}
    QueueEntry(clang::Decl const* d, DumpNode const* dn):
      Kind(KindDecl), Decl(d), DN(dn) {}
    QueueEntry(clang::QualType t, DumpNode const* dn):
      Kind(KindType), Decl(0), Type(t), DN(dn) {}

    // Kind of node at this entry.
    Kinds Kind;

    // The declaration when Kind == KindDecl.
    clang::Decl const* Decl;

    // The type when Kind == KindType.
    clang::QualType Type;

    // The dump status for this node.
    DumpNode const* DN;
  };

  // Order clang::QualType values.
  struct QualTypeCompare {
    bool operator()(clang::QualType l, clang::QualType r) const {
      // Order by pointer value without discarding low-order
      // bits used to encode qualifiers.
      void* lpp = &l;
      void* rpp = &r;
      return *static_cast<void**>(lpp) < *static_cast<void**>(rpp);
    }
  };

  /** Get the dump status node for a Clang declaration.  */
  DumpNode* GetDumpNode(clang::Decl const* d) {
    return &this->DeclNodes[d];
  }

  /** Get the dump status node for a Clang type.  */
  DumpNode* GetDumpNode(clang::QualType t) {
    return &this->TypeNodes[t];
  }

  /** Allocate a dump node for a Clang declaration.  */
  unsigned int AddDumpNode(clang::Decl const* d, bool complete);

  /** Allocate a dump node for a Clang type.  */
  unsigned int AddDumpNode(clang::QualType t, bool complete);

  /** Helper common to AddDumpNode implementation for every kind.  */
  template <typename K> unsigned int AddDumpNodeImpl(K k, bool complete);

  /** Allocate a dump node for a source file entry.  */
  unsigned int AddDumpFile(clang::FileEntry const* f);

  /** Queue leftover nodes that do not need complete output.  */
  void QueueIncompleteDumpNodes();

  /** Traverse AST nodes until the queue is empty.  */
  void ProcessQueue();
  void ProcessFileQueue();

  /** Dispatch output of a declaration.  */
  void OutputDecl(clang::Decl const* d, DumpNode const* dn);

  /** Dispatch output of a qualified or unqualified type.  */
  void OutputType(clang::QualType t, DumpNode const* dn);

  /** Output a qualified type and queue its unqualified type.  */
  void OutputCvQualifiedType(clang::QualType t, DumpNode const* dn);

  /** Get the XML IDREF for the element defining the given
      declaration context (namespace, class, etc.).  */
  unsigned int GetContextIdRef(clang::DeclContext const* dc);

  /** Get the XML IDREF for the element defining the given
      (possibly cv-qualified) type.  The qc,qv,qr booleans are
      set to whether the IDREF should include the const,
      volatile, or restrict qualifier, respectively.  Also
      queues the given type for later output.  */
  unsigned int GetTypeIdRef(clang::QualType t, bool complete,
                            bool& qc, bool& qv, bool& qr);

  /** Print the XML IDREF value referencing the given type.
      If the type has top-level cv-qualifiers, they are
      appended to the numeric id as single characters (c=const,
      v=volatile, r=restrict) to reference the XML ID of
      a CvQualifiedType element describing the qualifiers
      and referencing the unqualified type.  */
  void PrintTypeIdRef(clang::QualType t, bool complete);

  /** Print an id="_<n>" XML unique ID attribute.  */
  void PrintIdAttribute(DumpNode const* dn);

  /** Print a name="..." attribute.  */
  void PrintNameAttribute(std::string const& name);

  /** Print a type="..." attribute with the XML IDREF for
      the given (possibly cv-qualified) type.  Also queues
      the given type for later output.  */
  void PrintTypeAttribute(clang::QualType t, bool complete);

  /** Print the XML attributes location="fid:line" file="fid" line="line"
      for the given decl.  */
  void PrintLocationAttribute(clang::Decl const* d);

  /** Print a members="..." attribute listing the XML IDREFs for
      members of the given declaration context.  Also queues the
      context members for later output.  */
  void PrintMembersAttribute(clang::DeclContext const* dc);

  /** Print a context="..." attribute with the XML IDREF for
      the containing declaration context (namespace, class, etc.).
      Also prints access="..." attribute for class members to
      indicate public, protected, or private membership.  */
  void PrintContextAttribute(clang::Decl const* d);

  // Decl node output methods.
  void OutputTranslationUnitDecl(clang::TranslationUnitDecl const* d,
                                 DumpNode const* dn);
  void OutputNamespaceDecl(clang::NamespaceDecl const* d, DumpNode const* dn);
  void OutputTypedefDecl(clang::TypedefDecl const* d, DumpNode const* dn);

  /** Queue declarations matching given qualified name in given context.  */
  void LookupStart(clang::DeclContext const* dc, std::string const& name);

private:
  // List of starting declaration names.
  std::vector<std::string> const& StartNames;

  // Total number of nodes to be dumped.
  unsigned int NodeCount;

  // Total number of source files to be referenced.
  unsigned int FileCount;

  // Whether we are in the complete or incomplete output step.
  bool RequireComplete;

  // Map from clang AST declaration node to our dump status node.
  typedef std::map<clang::Decl const*, DumpNode> DeclNodesMap;
  DeclNodesMap DeclNodes;

  // Map from clang AST type node to our dump status node.
  typedef std::map<clang::QualType, DumpNode, QualTypeCompare> TypeNodesMap;
  TypeNodesMap TypeNodes;

  // Map from clang file entry to our source file index.
  typedef std::map<clang::FileEntry const*, unsigned int> FileNodesMap;
  FileNodesMap FileNodes;

  // Node traversal queue.
  std::queue<QueueEntry> Queue;

  // File traversal queue.
  std::queue<clang::FileEntry const*> FileQueue;

public:
  ASTVisitor(clang::CompilerInstance& ci,
             clang::ASTContext const& ctx,
             llvm::raw_ostream& os,
             std::vector<std::string> const& startNames):
    ASTVisitorBase(ci, ctx, os),
    StartNames(startNames),
    NodeCount(0), FileCount(0),
    RequireComplete(true) {}

  /** Visit declarations in the given translation unit.
      This is the main entry point.  */
  void HandleTranslationUnit(clang::TranslationUnitDecl const* tu);
};

//----------------------------------------------------------------------------
unsigned int ASTVisitor::AddDumpNode(clang::Decl const* d, bool complete) {
  // Add the node for the canonical declaration instance.
  return this->AddDumpNodeImpl(d->getCanonicalDecl(), complete);
}

//----------------------------------------------------------------------------
unsigned int ASTVisitor::AddDumpNode(clang::QualType t, bool complete) {
  return this->AddDumpNodeImpl(t, complete);
}

//----------------------------------------------------------------------------
template <typename K>
unsigned int ASTVisitor::AddDumpNodeImpl(K k, bool complete)
{
  // Update an existing node or add one.
  DumpNode* dn = this->GetDumpNode(k);
  if (dn->Index) {
    // Node was already encountered.  See if it is now complete.
    if(complete && !dn->Complete) {
      // Node is now complete, but wasn't before.  Queue it.
      dn->Complete = true;
      this->Queue.push(QueueEntry(k, dn));
    }
  } else {
    // This is a new node.  Assign it an index.
    dn->Index = ++this->NodeCount;
    dn->Complete = complete;
    if(complete || !this->RequireComplete) {
      // Node is complete.  Queue it.
      this->Queue.push(QueueEntry(k, dn));
    }
  }
  // Return node's index.
  return dn->Index;
}

//----------------------------------------------------------------------------
unsigned int ASTVisitor::AddDumpFile(clang::FileEntry const* f)
{
  unsigned int& index = this->FileNodes[f];
  if(index == 0) {
    index = ++this->FileCount;
    this->FileQueue.push(f);
  }
  return index;
}

//----------------------------------------------------------------------------
void ASTVisitor::QueueIncompleteDumpNodes()
{
  // Queue declaration nodes that do not need complete output.
  for(DeclNodesMap::const_iterator i = this->DeclNodes.begin(),
        e = this->DeclNodes.end(); i != e; ++i) {
    if(!i->second.Complete) {
      this->Queue.push(QueueEntry(i->first, &i->second));
    }
  }

  // Queue type nodes that do not need complete output.
  for(TypeNodesMap::const_iterator i = this->TypeNodes.begin(),
        e = this->TypeNodes.end(); i != e; ++i) {
    if(!i->second.Complete) {
      this->Queue.push(QueueEntry(i->first, &i->second));
    }
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::ProcessQueue()
{
  // Dispatch each entry in the queue based on its node kind.
  while(!this->Queue.empty()) {
    QueueEntry qe = this->Queue.front();
    this->Queue.pop();
    switch(qe.Kind) {
    case QueueEntry::KindDecl:
      this->OutputDecl(qe.Decl, qe.DN);
      break;
    case QueueEntry::KindType:
      this->OutputType(qe.Type, qe.DN);
      break;
    }
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::ProcessFileQueue()
{
  while(!this->FileQueue.empty()) {
    clang::FileEntry const* f = this->FileQueue.front();
    this->FileQueue.pop();
    this->OS <<
      "  <File"
      " id=\"f" << this->FileNodes[f] << "\""
      " name=\"" << encodeXML(f->getName()) << "\""
      "/>\n"
      ;
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputDecl(clang::Decl const* d, DumpNode const* dn)
{
  // Dispatch output of the declaration.
  switch (d->getKind()) {
#define ABSTRACT_DECL(DECL)
#define DECL(CLASS, BASE) \
  case clang::Decl::CLASS: \
    this->Output##CLASS##Decl( \
      static_cast<clang::CLASS##Decl const*>(d), dn); \
    break;
#include "clang/AST/DeclNodes.inc"
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputType(clang::QualType t, DumpNode const* dn)
{
  if(t.hasLocalQualifiers()) {
    // Output the qualified type.  This will queue
    // the unqualified type if necessary.
    this->OutputCvQualifiedType(t, dn);
  } else {
    // Dispatch output of the unqualified type.
    switch (t->getTypeClass()) {
#define ABSTRACT_TYPE(CLASS, BASE)
#define TYPE(CLASS, BASE) \
      case clang::Type::CLASS: \
        this->Output##CLASS##Type( \
          static_cast<clang::CLASS##Type const*>(t.getTypePtr()), dn); \
        break;
#include "clang/AST/TypeNodes.def"
    }
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputCvQualifiedType(clang::QualType t, DumpNode const* dn)
{
  // TODO: Output a CvQualifiedType element that references the
  // unqualified type element and lists qualifier attributes.
  static_cast<void>(t);
  static_cast<void>(dn);
}

//----------------------------------------------------------------------------
unsigned int ASTVisitor::GetContextIdRef(clang::DeclContext const* dc)
{
  if(clang::Decl const* d = clang::dyn_cast<clang::Decl>(dc)) {
    return this->AddDumpNode(d, false);
  } else {
    return 0;
  }
}

//----------------------------------------------------------------------------
unsigned int ASTVisitor::GetTypeIdRef(clang::QualType t, bool complete,
                                      bool& qc, bool& qv, bool& qr)
{
  // Add the type node.
  unsigned int id = this->AddDumpNode(t, complete);

  // Check for qualifiers.
  qc = t.isLocalConstQualified();
  qv = t.isLocalVolatileQualified();
  qr = t.isLocalRestrictQualified();

  // If the type has qualifiers, add the unqualified type and use its id.
  if(t.hasLocalQualifiers()) {
    id = this->AddDumpNode(t.getLocalUnqualifiedType(), complete);
  }

  // Return the dump node id of the unqualified type.
  return id;
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintTypeIdRef(clang::QualType t, bool complete)
{
  // Add the type node.
  bool qc, qv, qr;
  unsigned int id = this->GetTypeIdRef(t, complete, qc, qv, qr);

  // Check cv-qualificiation.
  const char* c = qc? "c" : "";
  const char* v = qv? "v" : "";
  const char* r = qr? "r" : "";

  // Print the reference.
  this->OS << "_" << id << c << v << r;
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintIdAttribute(DumpNode const* dn)
{
  this->OS << " id=\"_" << dn->Index << "\"";
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintNameAttribute(std::string const& name)
{
  this->OS << " name=\"" << encodeXML(name) << "\"";
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintTypeAttribute(clang::QualType t, bool complete)
{
  this->OS << " type=\"";
  this->PrintTypeIdRef(t, complete);
  this->OS << "\"";
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintLocationAttribute(clang::Decl const* d)
{
  clang::SourceLocation sl = d->getLocation();
  if(!sl.isValid()) {
    return;
  }
  clang::FullSourceLoc fsl = this->CTX.getFullLoc(sl).getExpansionLoc();
  if (clang::FileEntry const* f =
      this->CI.getSourceManager().getFileEntryForID(fsl.getFileID())) {
    unsigned int id = this->AddDumpFile(f);
    unsigned int line = fsl.getExpansionLineNumber();
    this->OS <<
      " location=\"f" << id << ":" << line << "\""
      " file=\"f" << id << "\""
      " line=\"" << line << "\"";
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintContextAttribute(clang::Decl const* d)
{
  clang::DeclContext const* dc = d->getDeclContext();
  if(unsigned int id = this->GetContextIdRef(dc)) {
    this->OS << " context=\"_" << id << "\"";
    if (dc->isRecord()) {
      if (d->getAccess() == clang::AS_private) {
        this->OS << " access=\"private\"";
      } else if (d->getAccess() == clang::AS_protected) {
        this->OS << " access=\"protected\"";
      } else {
        this->OS << " access=\"public\"";
      }
    }
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::PrintMembersAttribute(clang::DeclContext const* dc)
{
  std::set<unsigned int> emitted;
  for(clang::DeclContext::decl_iterator i = dc->decls_begin(),
        e = dc->decls_end(); i != e; ++i) {
    clang::Decl const* d = *i;

    // Ignore certain members.
    switch (d->getKind()) {
    case clang::Decl::CXXRecord: {
      if(static_cast<clang::CXXRecordDecl const*>(d)->isInjectedClassName()) {
        continue;
      }
    } break;
    case clang::Decl::AccessSpec: {
      continue;
    } break;
    default:
      break;
    }

    // Queue this decl and print its id.
    if(unsigned int id = this->AddDumpNode(d, true)) {
      emitted.insert(id);
    }
  }

  if(!emitted.empty()) {
    this->OS << " members=\"";
    const char* sep = "";
    for(std::set<unsigned int>::const_iterator i = emitted.begin(),
          e = emitted.end(); i != e; ++i) {
      this->OS << sep << "_" << *i;
      sep = " ";
    }
    this->OS << "\"";
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputTranslationUnitDecl(
  clang::TranslationUnitDecl const* d, DumpNode const* dn)
{
  this->OS << "  <Namespace";
  this->PrintIdAttribute(dn);
  this->PrintNameAttribute("::");
  if(dn->Complete) {
    this->PrintMembersAttribute(d);
  }
  this->OS << "/>\n";
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputNamespaceDecl(
  clang::NamespaceDecl const* d, DumpNode const* dn)
{
  this->OS << "  <Namespace";
  this->PrintIdAttribute(dn);
  this->PrintNameAttribute(d->getName().str());
  this->PrintContextAttribute(d);
  if(dn->Complete) {
    this->PrintMembersAttribute(d);
  }
  this->OS << "/>\n";
}

//----------------------------------------------------------------------------
void ASTVisitor::OutputTypedefDecl(clang::TypedefDecl const* d,
                                   DumpNode const* dn)
{
  this->OS << "  <Typedef";
  this->PrintIdAttribute(dn);
  this->PrintNameAttribute(d->getName().str());
  this->PrintTypeAttribute(d->getTypeSourceInfo()->getType(), dn->Complete);
  this->PrintContextAttribute(d);
  this->PrintLocationAttribute(d);

  this->OS << "/>\n";
}

//----------------------------------------------------------------------------
void ASTVisitor::LookupStart(clang::DeclContext const* dc,
                             std::string const& name)
{
  std::string::size_type pos = name.find("::");
  std::string cur = name.substr(0, pos);

  clang::IdentifierTable& ids = CI.getPreprocessor().getIdentifierTable();
  clang::DeclContext::lookup_const_result r =
    dc->lookup(clang::DeclarationName(&ids.get(cur)));
  if(pos == name.npos) {
    for(clang::DeclContext::lookup_const_iterator i = r.begin(), e = r.end();
        i != e; ++i) {
      this->AddDumpNode(*i, true);
    }
  } else {
    std::string rest = name.substr(pos+2);
    for(clang::DeclContext::lookup_const_iterator i = r.begin(), e = r.end();
        i != e; ++i) {
      if (clang::DeclContext* idc = clang::dyn_cast<clang::DeclContext>(*i)) {
        this->LookupStart(idc, rest);
      }
    }
  }
}

//----------------------------------------------------------------------------
void ASTVisitor::HandleTranslationUnit(clang::TranslationUnitDecl const* tu)
{
  // Add the starting nodes for the dump.
  if(!this->StartNames.empty()) {
    // Use the specified starting locations.
    for(std::vector<std::string>::const_iterator i = this->StartNames.begin(),
          e = this->StartNames.end(); i != e; ++i) {
      this->LookupStart(tu, *i);
    }
  } else {
    // No start specified.  Use whole translation unit.
    this->AddDumpNode(tu, true);
  }

  // Start dump with gccxml-compatible format.
  this->OS <<
    "<?xml version=\"1.0\"?>\n"
    "<GCC_XML version=\"0.9.0\" cvs_revision=\"1.136\">\n"
    ;

  // Dump the complete nodes.
  this->ProcessQueue();

  // Queue all the incomplete nodes.
  this->RequireComplete = false;
  this->QueueIncompleteDumpNodes();

  // Dump the incomplete nodes.
  this->ProcessQueue();

  // Dump the filename queue.
  this->ProcessFileQueue();

  // Finish dump.
  this->OS <<
    "</GCC_XML>\n"
    ;
}

//----------------------------------------------------------------------------
void outputXML(clang::CompilerInstance& ci,
               clang::ASTContext const& ctx,
               llvm::raw_ostream& os,
               std::vector<std::string> const& startNames)
{
  ASTVisitor v(ci, ctx, os, startNames);
  v.HandleTranslationUnit(ctx.getTranslationUnitDecl());
}