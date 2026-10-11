//! https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/AST/CMakeLists.txt
const LLVMBuilder = @import("../../LLVMBuilder.zig");
const SynthesizeHeaderConfig = LLVMBuilder.SynthesizeHeaderConfig;

const basic = @import("basic.zig");

pub const root = "clang/lib/AST/";
pub const sources = [_][]const u8{
    "APValue.cpp",
    "ASTConcept.cpp",
    "ASTConsumer.cpp",
    "ASTContext.cpp",
    "ASTDiagnostic.cpp",
    "ASTDumper.cpp",
    "ASTImporter.cpp",
    "ASTImporterLookupTable.cpp",
    "ASTStructuralEquivalence.cpp",
    "ASTTypeTraits.cpp",
    "AttrDocTable.cpp",
    "AttrImpl.cpp",
    "Availability.cpp",
    "Comment.cpp",
    "CommentBriefParser.cpp",
    "CommentCommandTraits.cpp",
    "CommentLexer.cpp",
    "CommentParser.cpp",
    "CommentSema.cpp",
    "ComparisonCategories.cpp",
    "ComputeDependence.cpp",
    "CXXInheritance.cpp",
    "DataCollection.cpp",
    "Decl.cpp",
    "DeclarationName.cpp",
    "DeclBase.cpp",
    "DeclCXX.cpp",
    "DeclFriend.cpp",
    "DeclGroup.cpp",
    "DeclObjC.cpp",
    "DeclOpenACC.cpp",
    "DeclOpenMP.cpp",
    "DeclPrinter.cpp",
    "DeclTemplate.cpp",
    "DynamicRecursiveASTVisitor.cpp",
    "ParentMapContext.cpp",
    "Expr.cpp",
    "ExprClassification.cpp",
    "ExprConcepts.cpp",
    "ExprConstant.cpp",
    "ExprCXX.cpp",
    "ExprObjC.cpp",
    "ExternalASTMerger.cpp",
    "ExternalASTSource.cpp",
    "FormatString.cpp",
    "InferAlloc.cpp",
    "InheritViz.cpp",
    "ByteCode/BitcastBuffer.cpp",
    "ByteCode/ByteCodeEmitter.cpp",
    "ByteCode/Compiler.cpp",
    "ByteCode/Context.cpp",
    "ByteCode/Descriptor.cpp",
    "ByteCode/Disasm.cpp",
    "ByteCode/EvalEmitter.cpp",
    "ByteCode/Function.cpp",
    "ByteCode/InterpBuiltin.cpp",
    "ByteCode/InterpBuiltinBitCast.cpp",
    "ByteCode/Floating.cpp",
    "ByteCode/EvaluationResult.cpp",
    "ByteCode/DynamicAllocator.cpp",
    "ByteCode/InitMap.cpp",
    "ByteCode/Interp.cpp",
    "ByteCode/InterpBlock.cpp",
    "ByteCode/InterpFrame.cpp",
    "ByteCode/InterpStack.cpp",
    "ByteCode/InterpState.cpp",
    "ByteCode/Pointer.cpp",
    "ByteCode/PrimType.cpp",
    "ByteCode/Program.cpp",
    "ByteCode/Record.cpp",
    "ByteCode/Source.cpp",
    "ByteCode/State.cpp",
    "ByteCode/MemberPointer.cpp",
    "ByteCode/InterpShared.cpp",
    "HLSLResource.cpp",
    "ItaniumCXXABI.cpp",
    "ItaniumMangle.cpp",
    "JSONNodeDumper.cpp",
    "Mangle.cpp",
    "MicrosoftCXXABI.cpp",
    "MicrosoftMangle.cpp",
    "NestedNameSpecifier.cpp",
    "NSAPI.cpp",
    "ODRDiagsEmitter.cpp",
    "ODRHash.cpp",
    "OpenACCClause.cpp",
    "OpenMPClause.cpp",
    "OSLog.cpp",
    "ParentMap.cpp",
    "PrintfFormatString.cpp",
    "QualTypeNames.cpp",
    "Randstruct.cpp",
    "RawCommentList.cpp",
    "RecordLayout.cpp",
    "RecordLayoutBuilder.cpp",
    "ScanfFormatString.cpp",
    "SelectorLocationsKind.cpp",
    "Stmt.cpp",
    "StmtCXX.cpp",
    "StmtIterator.cpp",
    "StmtObjC.cpp",
    "StmtOpenACC.cpp",
    "StmtOpenMP.cpp",
    "StmtPrinter.cpp",
    "StmtProfile.cpp",
    "StmtViz.cpp",
    "TemplateBase.cpp",
    "TemplateName.cpp",
    "TextNodeDumper.cpp",
    "Type.cpp",
    "TypeLoc.cpp",
    "TypePrinter.cpp",
    "VTableBuilder.cpp",
    "VTTBuilder.cpp",
};

const attr_td = basic.attr_td;

pub const synthesize_opcodes: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "Opcodes",
        .td_file = root ++ "ByteCode/Opcodes.td",
        .instruction = .{ .action = "-gen-clang-opcodes" },
    },
    .virtual_path = "Opcodes.inc",
};

/// Needs clang's include directory
pub const synthesize_attr_doc_table: SynthesizeHeaderConfig = .{
    .gen_conf = .{
        .name = "ClangAttrDocTable",
        .td_file = attr_td,
        .instruction = .{ .action = "-gen-clang-attr-doc-table" },
    },
    .virtual_path = "AttrDocTable.inc",
};

pub const include_root = "clang/include/clang/AST/";

pub const synthesize_include_configs = [_]SynthesizeHeaderConfig{
    .{
        .gen_conf = .{
            .name = "ClangAttrClasses",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-classes" },
        },
        .virtual_path = "clang/AST/Attrs.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrImpl",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-impl" },
        },
        .virtual_path = "clang/AST/AttrImpl.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrTextDump",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-text-node-dump" },
        },
        .virtual_path = "clang/AST/AttrTextNodeDump.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrTraverse",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-node-traverse" },
        },
        .virtual_path = "clang/AST/AttrNodeTraverse.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAttrVisitor",
            .td_file = attr_td,
            .instruction = .{ .action = "-gen-clang-attr-ast-visitor" },
        },
        .virtual_path = "clang/AST/AttrVisitor.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangStmtNodes",
            .td_file = basic.include_root ++ "StmtNodes.td",
            .instruction = .{ .action = "-gen-clang-stmt-nodes" },
        },
        .virtual_path = "clang/AST/StmtNodes.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangDeclNodes",
            .td_file = basic.include_root ++ "DeclNodes.td",
            .instruction = .{ .action = "-gen-clang-decl-nodes" },
        },
        .virtual_path = "clang/AST/DeclNodes.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangTypeNodes",
            .td_file = basic.include_root ++ "TypeNodes.td",
            .instruction = .{ .action = "-gen-clang-type-nodes" },
        },
        .virtual_path = "clang/AST/TypeNodes.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAbstractBasicReader",
            .td_file = include_root ++ "PropertiesBase.td",
            .instruction = .{ .action = "-gen-clang-basic-reader" },
        },
        .virtual_path = "clang/AST/AbstractBasicReader.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAbstractBasicWriter",
            .td_file = include_root ++ "PropertiesBase.td",
            .instruction = .{ .action = "-gen-clang-basic-writer" },
        },
        .virtual_path = "clang/AST/AbstractBasicWriter.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAbstractTypeReader",
            .td_file = include_root ++ "TypeProperties.td",
            .instruction = .{ .action = "-gen-clang-type-reader" },
        },
        .virtual_path = "clang/AST/AbstractTypeReader.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangAbstractTypeWriter",
            .td_file = include_root ++ "TypeProperties.td",
            .instruction = .{ .action = "-gen-clang-type-writer" },
        },
        .virtual_path = "clang/AST/AbstractTypeWriter.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentNodes",
            .td_file = basic.include_root ++ "CommentNodes.td",
            .instruction = .{ .action = "-gen-clang-comment-nodes" },
        },
        .virtual_path = "clang/AST/CommentNodes.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentHTMLTags",
            .td_file = include_root ++ "CommentHTMLTags.td",
            .instruction = .{ .action = "-gen-clang-comment-html-tags" },
        },
        .virtual_path = "clang/AST/CommentHTMLTags.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentHTMLTagsProperties",
            .td_file = include_root ++ "CommentHTMLTags.td",
            .instruction = .{ .action = "-gen-clang-comment-html-tags-properties" },
        },
        .virtual_path = "clang/AST/CommentHTMLTagsProperties.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentHTMLNamedCharacterReferences",
            .td_file = include_root ++ "CommentHTMLNamedCharacterReferences.td",
            .instruction = .{ .action = "-gen-clang-comment-html-named-character-references" },
        },
        .virtual_path = "clang/AST/CommentHTMLNamedCharacterReferences.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentCommandInfo",
            .td_file = include_root ++ "CommentCommands.td",
            .instruction = .{ .action = "-gen-clang-comment-command-info" },
        },
        .virtual_path = "clang/AST/CommentCommandInfo.inc",
    },
    .{
        .gen_conf = .{
            .name = "ClangCommentCommandList",
            .td_file = include_root ++ "CommentCommands.td",
            .instruction = .{ .action = "-gen-clang-comment-command-list" },
        },
        .virtual_path = "clang/AST/CommentCommandList.inc",
    },
    .{
        .gen_conf = .{
            .name = "StmtDataCollectors",
            .td_file = include_root ++ "StmtDataCollectors.td",
            .instruction = .{ .action = "-gen-clang-data-collectors" },
        },
        .virtual_path = "clang/AST/StmtDataCollectors.inc",
    },
};

/// https://github.com/llvm/llvm-project/blob/llvmorg-23.1.3/clang/lib/ASTMatchers
pub const matchers_root = "clang/lib/ASTMatchers/";
pub const matchers_sources = [_][]const u8{
    "ASTMatchFinder.cpp",
    "ASTMatchersInternal.cpp",
    "LowLevelHelpers.cpp",
};

pub const matchers_dynamic_root = matchers_root ++ "Dynamic";
pub const matchers_dynamic_sources = [_][]const u8{
    "Diagnostics.cpp",
    "Marshallers.cpp",
    "Parser.cpp",
    "Registry.cpp",
    "VariantValue.cpp",
};
