R"(ExpressionStatement
└─ Expr: ArrayExpression
    ├─ Size: (inferred)
    ├─ Null terminated: true
    ├─ Type: ExplicitType (modifier: PTR)
    │   └─ IdentifierExpression: N
    └─ Items:
        ├─ IdentifierExpression: a
        ├─ IdentifierExpression: b
        ├─ IdentifierExpression: c
        ├─ IdentifierExpression: d
        ├─ IdentifierExpression: e
        ├─ I32Expression: 3
        └─ StringExpression: 54
ExpressionStatement
└─ Expr: BinaryExpression (BOOLEAN_AND)
    ├─ LHS: BinaryExpression (BOOLEAN_OR)
    │   ├─ LHS: BinaryExpression (LT_EQ)
    │   │   ├─ LHS: IdentifierExpression: a
    │   │   └─ RHS: IdentifierExpression: b
    │   └─ RHS: BinaryExpression (EQ)
    │       ├─ LHS: IdentifierExpression: c
    │       └─ RHS: IdentifierExpression: d
    └─ RHS: IdentifierExpression: e
ExpressionStatement
└─ Expr: BinaryExpression (BOOLEAN_OR)
    ├─ LHS: IdentifierExpression: a
    └─ RHS: BinaryExpression (EQ)
        ├─ LHS: IndexExpression
        │   ├─ Object: IdentifierExpression: b
        │   └─ Index: USizeExpression: 3
        └─ RHS: UnaryExpression (BANG)
            └─ Operand: IdentifierExpression: c
ContinueStatement
ReturnStatement
└─ Value: EnumExpression
    ├─ Enumerations:
    │   └─ Name: IdentifierExpression: RED
    └─ Non-Exhaustive: false
ImportStatement
├─ Public: false
└─ Library: IdentifierExpression: std
ImportStatement
├─ Public: true
├─ File: StringExpression: ast/node.p
└─ Alias: IdentifierExpression: node
DiscardStatement
└─ Discarded: EnumExpression
    ├─ Enumerations:
    │   └─ Name: IdentifierExpression: RED
    └─ Non-Exhaustive: true
DeclStatement
├─ Name: IdentifierExpression: SIZE
├─ Modifiers: CONSTEXPR
├─ Type: (inferred)
└─ Value: USizeExpression: 2
BlockStatement
├─ ExpressionStatement
│   └─ Expr: IdentifierExpression: a
├─ ExpressionStatement
│   └─ Expr: IdentifierExpression: b
├─ ExpressionStatement
│   └─ Expr: I32Expression: 2
└─ ExpressionStatement
    └─ Expr: IdentifierExpression: c
ExpressionStatement
└─ Expr: WhileLoopExpression
    ├─ Condition: BoolExpression: true
    ├─ Continuation: AssignmentExpression (PLUS_ASSIGN)
    │   ├─ Assignee: IdentifierExpression: i
    │   └─ Value: I32Expression: 1
    ├─ Body: BlockStatement
    │   └─ ExpressionStatement
    │       └─ Expr: IdentifierExpression: a
    └─ Non-Break Clause: ReturnStatement
        └─ Value: IdentifierExpression: b
DeclStatement
├─ Name: IdentifierExpression: f_ptr
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: PTR)
│   └─ FunctionExpression
│       ├─ Parameters:
│       │   ├─ Param:
│       │   │   └─ Type: ExplicitType (modifier: REF)
│       │   │       └─ IdentifierExpression: a
│       │   └─ Param:
│       │       └─ Type: ExplicitType (modifier: MUT_PTR)
│       │           └─ IdentifierExpression: B
│       ├─ Variadic: true
│       └─ Returns: ExplicitType (modifier: REF)
│           └─ ArrayType
│               ├─ Dimensions: USizeExpression: 2
│               ├─ Null terminated: false
│               └─ ExplicitType (modifier: VALUE)
│                   └─ ArrayType
│                       ├─ Dimensions: IdentifierExpression: N
│                       ├─ Null terminated: false
│                       └─ ExplicitType (modifier: PTR)
│                           └─ IdentifierExpression: E
└─ Value: UndefinedExpression
ExpressionStatement
└─ Expr: ModuleAccessExpression
    ├─ Outer: ModuleAccessExpression
    │   ├─ Outer: IdentifierExpression: A
    │   └─ Inner: IdentifierExpression: B
    └─ Inner: IdentifierExpression: C
ExpressionStatement
└─ Expr: StructExpression
    ├─ Extern: false
    ├─ Packed: false
    └─ Members:
        ├─ DeclStatement
        │   ├─ Name: IdentifierExpression: a
        │   ├─ Modifiers: VARIABLE
        │   ├─ Type: ExplicitType (modifier: VALUE)
        │   │   └─ IdentifierExpression: Foo
        │   └─ Value: IdentifierExpression: bar
        └─ DeclStatement
            ├─ Name: IdentifierExpression: b
            ├─ Modifiers: CONSTANT
            ├─ Type: (inferred)
            └─ Value: FunctionExpression
                ├─ Self: this (modifier: MUT_PTR)
                ├─ Parameters:
                │   ├─ Param:
                │   │   ├─ Name: IdentifierExpression: a
                │   │   └─ Type: ExplicitType (modifier: VALUE)
                │   │       └─ IdentifierExpression: A
                │   └─ Param:
                │       ├─ Name: IdentifierExpression: b
                │       └─ Type: ExplicitType (modifier: PTR)
                │           └─ IdentifierExpression: B
                ├─ Variadic: false
                ├─ Move: false
                ├─ Naked: false
                ├─ CallConv: c
                ├─ Returns: ExplicitType (modifier: VALUE)
                │   └─ IdentifierExpression: C
                └─ Body: BlockStatement
                    └─ ExpressionStatement
                        └─ Expr: IdentifierExpression: c
ExpressionStatement
└─ Expr: ReferenceExpression (BW_AND)
    └─ Operand: IdentifierExpression: a
ExpressionStatement
└─ Expr: ReferenceExpression (AND_MUT)
    └─ Operand: IdentifierExpression: b
ExpressionStatement
└─ Expr: DereferenceExpression (STAR)
    └─ Operand: IdentifierExpression: a
ExpressionStatement
└─ Expr: MatchExpression
    ├─ Matcher: IdentifierExpression: a
    └─ Arms:
        ├─ Arm:
        │   ├─ Pattern: IdentifierExpression: b
        │   ├─ Capture: c (modifier: VALUE)
        │   └─ Dispatch: ExpressionStatement
        │       └─ Expr: IdentifierExpression: d
        ├─ Arm:
        │   ├─ Pattern: IdentifierExpression: e
        │   ├─ Capture: <discarded>
        │   └─ Dispatch: ExpressionStatement
        │       └─ Expr: IdentifierExpression: f
        ├─ Arm:
        │   ├─ Pattern: IdentifierExpression: g
        │   └─ Dispatch: ExpressionStatement
        │       └─ Expr: IdentifierExpression: h
        └─ Catch-All Arm:
            ├─ Pattern: <discarded>
            └─ Dispatch: ExpressionStatement
                └─ Expr: IdentifierExpression: d
ExpressionStatement
└─ Expr: InfiniteLoopExpression
    └─ ExpressionStatement
        └─ Expr: IdentifierExpression: a
ExpressionStatement
└─ Expr: AssignmentExpression (ASSIGN)
    ├─ Assignee: DereferenceExpression (STAR)
    │   └─ Operand: IndexExpression
    │       ├─ Object: IndexExpression
    │       │   ├─ Object: IdentifierExpression: arr
    │       │   └─ Index: IdentifierExpression: i
    │       └─ Index: IdentifierExpression: j
    └─ Value: I32Expression: 2
ExpressionStatement
└─ Expr: IfExpression
    ├─ Constexpr: false
    ├─ Condition: IdentifierExpression: a
    ├─ Consequence: BlockStatement
    │   └─ ExpressionStatement
    │       └─ Expr: IdentifierExpression: b
    └─ Alternate: BlockStatement
        └─ ExpressionStatement
            └─ Expr: IdentifierExpression: c
ExpressionStatement
└─ Expr: IfExpression
    ├─ Constexpr: true
    ├─ Condition: IdentifierExpression: a
    └─ Consequence: BlockStatement
        └─ ExpressionStatement
            └─ Expr: IdentifierExpression: b
ExpressionStatement
└─ Expr: FunctionExpression
    ├─ Self: this (modifier: MUT_PTR)
    ├─ Parameters:
    │   ├─ Param:
    │   │   ├─ Name: IdentifierExpression: a
    │   │   └─ Type: ExplicitType (modifier: VALUE)
    │   │       └─ IdentifierExpression: A
    │   └─ Param:
    │       ├─ Name: IdentifierExpression: b
    │       └─ Type: ExplicitType (modifier: PTR)
    │           └─ IdentifierExpression: B
    ├─ Variadic: false
    ├─ Move: false
    ├─ Naked: false
    ├─ CallConv: c
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: i32 (primitive)
    └─ Body: BlockStatement
        └─ ExpressionStatement
            └─ Expr: IdentifierExpression: c
ExpressionStatement
└─ Expr: ForLoopExpression
    ├─ Iterables:
    │   ├─ IdentifierExpression: arr
    │   ├─ IdentifierExpression: l
    │   └─ IdentifierExpression: p
    ├─ Captures:
    │   ├─ i (modifier: VALUE)
    │   ├─ j (modifier: MUT_REF)
    │   └─ <discarded>
    ├─ Body: BlockStatement
    │   └─ ExpressionStatement
    │       └─ Expr: IdentifierExpression: a
    └─ Non-Break: ReturnStatement
        └─ Value: IdentifierExpression: b
ExpressionStatement
└─ Expr: EnumExpression
    ├─ Underlying: IdentifierExpression: u64 (primitive)
    ├─ Enumerations:
    │   ├─ Name: IdentifierExpression: A
    │   │   └─ Default: U64Expression: 1
    │   ├─ Name: IdentifierExpression: B
    │   │   └─ Default: IdentifierExpression: T
    │   └─ Name: IdentifierExpression: C
    └─ Non-Exhaustive: false
ExpressionStatement
└─ Expr: CallExpression
    ├─ Callee: IdentifierExpression: @as (builtin)
    └─ Arguments:
        ├─ IdentifierExpression: i32 (primitive)
        └─ IdentifierExpression: a
UsingStatement
├─ Public: false
├─ Alias: IdentifierExpression: T
└─ Type: ExplicitType (modifier: VALUE)
    └─ IdentifierExpression: i32 (primitive)
ExpressionStatement
└─ Expr: CallExpression
    ├─ Callee: IdentifierExpression: a
    └─ Arguments:
        ├─ ReferenceExpression (AND_MUT)
        │   └─ Operand: IdentifierExpression: r
        ├─ IdentifierExpression: t
        └─ AddressOfExpression (CARET)
            └─ Operand: ArrayExpression (type)
                ├─ Size: IdentifierExpression: N
                ├─ Null terminated: true
                └─ Type: ExplicitType (modifier: VALUE)
                    └─ IdentifierExpression: u8 (primitive)
ExpressionStatement
└─ Expr: ImplicitAccessExpression
    └─ Member: IdentifierExpression: a
ExpressionStatement
└─ Expr: UnionExpression
    ├─ Extern: false
    └─ Fields:
        ├─ Field:
        │   ├─ Tag: IdentifierExpression: a
        │   └─ Type: ExplicitType (modifier: VALUE)
        │       └─ IdentifierExpression: i32 (primitive)
        └─ Field:
            ├─ Tag: IdentifierExpression: b
            └─ Type: ExplicitType (modifier: MUT_REF)
                └─ IdentifierExpression: T
ExpressionStatement
└─ Expr: DoWhileLoopExpression
    ├─ Body: BlockStatement
    │   └─ ExpressionStatement
    │       └─ Expr: IdentifierExpression: a
    └─ Condition: BoolExpression: true
ExpressionStatement
└─ Expr: I64Expression: 1
ExpressionStatement
└─ Expr: ISizeExpression: 2
ExpressionStatement
└─ Expr: U32Expression: 3
ExpressionStatement
└─ Expr: U8Expression: 97
ExpressionStatement
└─ Expr: F32Expression: 2.3
ExpressionStatement
└─ Expr: F64Expression: 2.3
ExpressionStatement
└─ Expr: ArrayExpression
    ├─ Size: USizeExpression: 1
    ├─ Null terminated: false
    ├─ Type: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: A
    └─ Items:
        └─ IdentifierExpression: a
ExpressionStatement
└─ Expr: DotExpression (DOT)
    ├─ Object: IdentifierExpression: a
    └─ Member: IdentifierExpression: b
ExpressionStatement
└─ Expr: RangeExpression (DOT_DOT)
    ├─ Lower: IdentifierExpression: a
    └─ Upper: IdentifierExpression: b
ExpressionStatement
└─ Expr: RangeExpression (DOT_DOT_EQ)
    ├─ Lower: IdentifierExpression: a
    └─ Upper: IdentifierExpression: b
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: VALUE)
│   └─ ArrayType
│       ├─ Dimensions: (slice)
│       ├─ Null terminated: false
│       └─ ExplicitType (modifier: VALUE)
│           └─ IdentifierExpression: i32 (primitive)
└─ Value: UndefinedExpression
BlockStatement
└─ <empty>
DeferStatement
└─ Deferred: ExpressionStatement
    └─ Expr: I32Expression: 3
ExpressionStatement
└─ Expr: Initializer Expression:
    ├─ Object Type: <inferred>
    └─ Initializers:
        └─ Initializer:
            ├─ Member: ImplicitAccessExpression
            │   └─ Member: IdentifierExpression: a
            └─ Value: I32Expression: 3
ExpressionStatement
└─ Expr: Initializer Expression:
    ├─ Object Type: IdentifierExpression: TT
    └─ Initializers:
        └─ Initializer:
            ├─ Member: ImplicitAccessExpression
            │   └─ Member: IdentifierExpression: adfasf
            └─ Value: IdentifierExpression: a
ExpressionStatement
└─ Expr: Initializer Expression:
    └─ Object Type: <inferred>
ExpressionStatement
└─ Expr: UnionExpression
    ├─ Extern: false
    ├─ Fields:
    │   └─ Field:
    │       ├─ Tag: IdentifierExpression: a
    │       └─ Type: ExplicitType (modifier: VALUE)
    │           └─ StructExpression
    │               ├─ Extern: false
    │               ├─ Packed: false
    │               ├─ Fields:
    │               │   ├─ Name: IdentifierExpression: b
    │               │   │   ├─ Public: false
    │               │   │   ├─ Type: ExplicitType (modifier: VALUE)
    │               │   │   │   └─ IdentifierExpression: Foo
    │               │   │   └─ Default: IdentifierExpression: bar
    │               │   └─ Name: IdentifierExpression: c
    │               │       ├─ Public: true
    │               │       └─ Type: ExplicitType (modifier: VALUE)
    │               │           └─ IdentifierExpression: i32 (primitive)
    │               └─ Members:
    │                   └─ DeclStatement
    │                       ├─ Name: IdentifierExpression: d
    │                       ├─ Modifiers: VARIABLE
    │                       ├─ Type: ExplicitType (modifier: VALUE)
    │                       │   └─ IdentifierExpression: u32 (primitive)
    │                       └─ Value: UndefinedExpression
    └─ Members:
        └─ DeclStatement
            ├─ Name: IdentifierExpression: b
            ├─ Modifiers: CONSTANT
            ├─ Type: (inferred)
            └─ Value: FunctionExpression
                ├─ Self: self (modifier: REF)
                ├─ Parameters:
                │   └─ Param:
                │       ├─ Name: IdentifierExpression: a
                │       └─ Type: ExplicitType (modifier: VALUE)
                │           └─ IdentifierExpression: A
                ├─ Variadic: false
                ├─ Move: false
                ├─ Naked: false
                ├─ CallConv: c
                ├─ Returns: ExplicitType (modifier: VALUE)
                │   └─ IdentifierExpression: C
                └─ Body: BlockStatement
                    └─ ExpressionStatement
                        └─ Expr: IdentifierExpression: c
ExpressionStatement
└─ Expr: EnumExpression
    ├─ Underlying: IdentifierExpression: i64 (primitive)
    ├─ Enumerations:
    │   └─ Name: IdentifierExpression: A
    │       └─ Default: I64Expression: 2
    ├─ Non-Exhaustive: false
    └─ Members:
        └─ DeclStatement
            ├─ Name: IdentifierExpression: b
            ├─ Modifiers: CONSTANT
            ├─ Type: (inferred)
            └─ Value: FunctionExpression
                ├─ Self: self (modifier: REF)
                ├─ Parameters:
                │   └─ Param:
                │       ├─ Name: IdentifierExpression: a
                │       └─ Type: ExplicitType (modifier: VALUE)
                │           └─ IdentifierExpression: A
                ├─ Variadic: false
                ├─ Move: false
                ├─ Naked: false
                ├─ CallConv: c
                ├─ Returns: ExplicitType (modifier: VALUE)
                │   └─ IdentifierExpression: C
                └─ Body: BlockStatement
                    └─ ExpressionStatement
                        └─ Expr: IdentifierExpression: c
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: VoidExpression
TestStatement
├─ Description: dump
└─ Block: BlockStatement
    ├─ ImportStatement
    │   ├─ Public: false
    │   └─ Library: IdentifierExpression: other
    └─ ExpressionStatement
        └─ Expr: CallExpression
            ├─ Callee: ModuleAccessExpression
            │   ├─ Outer: ModuleAccessExpression
            │   │   ├─ Outer: IdentifierExpression: std
            │   │   └─ Inner: IdentifierExpression: testing
            │   └─ Inner: IdentifierExpression: expect
            └─ Arguments:
                └─ BinaryExpression (EQ)
                    ├─ LHS: IdentifierExpression: a
                    └─ RHS: BoolExpression: true
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: VALUE)
│   └─ CallExpression
│       ├─ Callee: ModuleAccessExpression
│       │   ├─ Outer: IdentifierExpression: std
│       │   └─ Inner: IdentifierExpression: ArrayList
│       └─ Arguments:
│           └─ IdentifierExpression: u8 (primitive)
└─ Value: UndefinedExpression
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: VALUE)
│   └─ ModuleAccessExpression
│       ├─ Outer: IdentifierExpression: std
│       └─ Inner: IdentifierExpression: Io
└─ Value: UndefinedExpression
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: VALUE)
│   └─ CallExpression
│       ├─ Callee: IdentifierExpression: List
│       └─ Arguments:
│           └─ IdentifierExpression: i32 (primitive)
└─ Value: UndefinedExpression
BreakStatement
├─ Label: IdentifierExpression: blk
└─ Value: IdentifierExpression: a
ExpressionStatement
└─ Expr: Label Expression:
    ├─ Label: IdentifierExpression: a
    └─ Body: BlockStatement
        └─ ContinueStatement
            └─ Label: IdentifierExpression: a
UsingStatement
├─ Public: true
├─ Alias: IdentifierExpression: a
└─ Type: ExplicitType (modifier: PTR)
    └─ ExplicitType (modifier: PTR)
        └─ IdentifierExpression: i32
ExpressionStatement
└─ Expr: FunctionExpression
    ├─ Self: self (modifier: VALUE)
    ├─ Variadic: false
    ├─ Move: false
    ├─ Naked: false
    ├─ CallConv: c
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: i32 (primitive)
    └─ Body: BlockStatement
        └─ <empty>
ExpressionStatement
└─ Expr: U8Expression: 10
ExpressionStatement
└─ Expr: U8Expression: 13
ExpressionStatement
└─ Expr: U8Expression: 9
ExpressionStatement
└─ Expr: U8Expression: 92
ExpressionStatement
└─ Expr: U8Expression: 39
ExpressionStatement
└─ Expr: U8Expression: 0
DeclStatement
├─ Name: IdentifierExpression: a
├─ Modifiers: VARIABLE
├─ Type: ExplicitType (modifier: VALUE)
│   └─ IdentifierExpression: i32 (primitive)
└─ Value: UndefinedExpression
ExpressionStatement
└─ Expr: I32Expression: 51
ExpressionStatement
└─ Expr: AddressOfExpression (CARET_MUT)
    └─ Operand: IdentifierExpression: a
ExpressionStatement
└─ Expr: AddressOfExpression (CARET)
    └─ Operand: IdentifierExpression: a
ExpressionStatement
└─ Expr: CallExpression
    └─ Callee: IdentifierExpression: call
ExpressionStatement
└─ Expr: ArrayExpression
    ├─ Size: (inferred)
    ├─ Null terminated: false
    ├─ Type: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: i32 (primitive)
    └─ Items: <empty>
DeclStatement
├─ Name: IdentifierExpression: foo
├─ Modifiers: CONSTANT|EXTERN
└─ Type: ExplicitType (modifier: VALUE)
    └─ FunctionExpression
        ├─ Variadic: false
        └─ Returns: ExplicitType (modifier: VALUE)
            └─ IdentifierExpression: i32 (primitive)
DeclStatement
├─ Name: IdentifierExpression: bar
├─ Modifiers: CONSTANT|EXTERN
├─ Extern Target: StringExpression: kernel32
└─ Type: ExplicitType (modifier: VALUE)
    └─ FunctionExpression
        ├─ Variadic: false
        └─ Returns: ExplicitType (modifier: VALUE)
            └─ IdentifierExpression: void (primitive)
DeclStatement
├─ Name: IdentifierExpression: errno_loc
├─ Modifiers: CONSTANT|EXTERN
├─ Extern Target: StringExpression: c
├─ Link Name: StringExpression: __errno_location
└─ Type: ExplicitType (modifier: VALUE)
    └─ FunctionExpression
        ├─ Variadic: false
        └─ Returns: ExplicitType (modifier: MUT_PTR)
            └─ IdentifierExpression: i32
DeclStatement
├─ Name: IdentifierExpression: add
├─ Modifiers: CONSTANT|EXPORT
├─ Link Name: StringExpression: ghoti_add
├─ Type: (inferred)
└─ Value: FunctionExpression
    ├─ Parameters:
    │   └─ Param:
    │       ├─ Name: IdentifierExpression: a
    │       └─ Type: ExplicitType (modifier: VALUE)
    │           └─ IdentifierExpression: i32 (primitive)
    ├─ Variadic: false
    ├─ Move: false
    ├─ Naked: false
    ├─ CallConv: c
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: i32 (primitive)
    └─ Body: BlockStatement
        └─ ReturnStatement
            └─ Value: IdentifierExpression: a
DeclStatement
├─ Name: IdentifierExpression: tls_ctr
├─ Modifiers: VARIABLE|THREADLOCAL
├─ Type: ExplicitType (modifier: VALUE)
│   └─ IdentifierExpression: i32 (primitive)
└─ Value: I32Expression: 0
DeclStatement
├─ Name: IdentifierExpression: maybe
├─ Modifiers: CONSTANT|EXTERN|WEAK
└─ Type: ExplicitType (modifier: VALUE)
    └─ FunctionExpression
        ├─ Variadic: false
        └─ Returns: ExplicitType (modifier: VALUE)
            └─ IdentifierExpression: void (primitive)
DeclStatement
├─ Name: IdentifierExpression: mv
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: FunctionExpression
    ├─ Variadic: false
    ├─ Move: true
    ├─ Naked: false
    ├─ CallConv: c
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: i32 (primitive)
    └─ Body: BlockStatement
        └─ ReturnStatement
            └─ Value: I32Expression: 0
DeclStatement
├─ Name: IdentifierExpression: stub
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: FunctionExpression
    ├─ Variadic: false
    ├─ Move: false
    ├─ Naked: true
    ├─ CallConv: c
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: void (primitive)
    └─ Body: BlockStatement
        └─ <empty>
DeclStatement
├─ Name: IdentifierExpression: cb
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: FunctionExpression
    ├─ Variadic: false
    ├─ Move: false
    ├─ Naked: false
    ├─ CallConv: stdcall
    ├─ Returns: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: void (primitive)
    └─ Body: BlockStatement
        └─ <empty>
DeclStatement
├─ Name: IdentifierExpression: ExS
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: StructExpression
    ├─ Extern: true
    ├─ Packed: false
    └─ Fields:
        └─ Name: IdentifierExpression: a
            ├─ Public: false
            └─ Type: ExplicitType (modifier: VALUE)
                └─ IdentifierExpression: i32 (primitive)
DeclStatement
├─ Name: IdentifierExpression: ExPS
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: StructExpression
    ├─ Extern: true
    ├─ Packed: true
    └─ Fields:
        ├─ Name: IdentifierExpression: a
        │   ├─ Public: false
        │   └─ Type: ExplicitType (modifier: VALUE)
        │       └─ IdentifierExpression: i32 (primitive)
        └─ Name: IdentifierExpression: b
            ├─ Public: false
            └─ Type: ExplicitType (modifier: VALUE)
                └─ IdentifierExpression: u8 (primitive)
ExpressionStatement
└─ Expr: StructExpression
    ├─ Extern: false
    ├─ Packed: true
    └─ Fields:
        ├─ Name: IdentifierExpression: a
        │   ├─ Public: false
        │   ├─ Type: ExplicitType (modifier: VALUE)
        │   │   └─ IdentifierExpression: i32 (primitive)
        │   └─ Alignment: I32Expression: 4
        ├─ Name: IdentifierExpression: b
        │   ├─ Public: false
        │   ├─ Type: ExplicitType (modifier: VALUE)
        │   │   └─ IdentifierExpression: u8 (primitive)
        │   └─ Alignment: I32Expression: 8
        └─ Name: IdentifierExpression: c
            ├─ Public: false
            ├─ Type: ExplicitType (modifier: VALUE)
            │   └─ IdentifierExpression: i32 (primitive)
            └─ Alignment: I32Expression: 16
ExpressionStatement
└─ Expr: UnionExpression
    ├─ Extern: false
    └─ Fields:
        ├─ Field:
        │   ├─ Tag: IdentifierExpression: a
        │   ├─ Type: ExplicitType (modifier: VALUE)
        │   │   └─ IdentifierExpression: i32 (primitive)
        │   └─ Alignment: I32Expression: 16
        └─ Field:
            ├─ Tag: IdentifierExpression: b
            └─ Type: ExplicitType (modifier: MUT_REF)
                └─ IdentifierExpression: T
DeclStatement
├─ Name: IdentifierExpression: ExU
├─ Modifiers: CONSTANT
├─ Type: (inferred)
└─ Value: UnionExpression
    ├─ Extern: true
    └─ Fields:
        ├─ Field:
        │   ├─ Tag: IdentifierExpression: a
        │   ├─ Type: ExplicitType (modifier: VALUE)
        │   │   └─ IdentifierExpression: i32 (primitive)
        │   └─ Alignment: I32Expression: 8
        └─ Field:
            ├─ Tag: IdentifierExpression: b
            └─ Type: ExplicitType (modifier: VALUE)
                └─ IdentifierExpression: u8 (primitive)
ExpressionStatement
└─ Expr: AsmExpression
    ├─ Template: StringExpression: syscall
    ├─ ResultType: (none)
    ├─ Outputs: <empty>
    ├─ Inputs:
    │   ├─ StringExpression: {rax}
    │   │   └─ Value: IdentifierExpression: n
    │   └─ StringExpression: {rdi}
    │       └─ Value: IdentifierExpression: fd
    ├─ Clobbers:
    │   ├─ StringExpression: rcx
    │   ├─ StringExpression: r11
    │   └─ StringExpression: memory
    └─ Options: VOLATILE
ExpressionStatement
└─ Expr: AsmExpression
    ├─ Template: StringExpression: rdtsc
    ├─ ResultType: ExplicitType (modifier: VALUE)
    │   └─ IdentifierExpression: u32 (primitive)
    ├─ Outputs:
    │   └─ StringExpression: ={eax}
    │       └─ Value: _ (result slot)
    ├─ Inputs: <empty>
    ├─ Clobbers: <empty>
    └─ Options: VOLATILE
ExpressionStatement
└─ Expr: AsmExpression
    ├─ Template: StringExpression: mov %0, %1
    ├─ ResultType: (none)
    ├─ Outputs:
    │   └─ StringExpression: =r
    │       └─ Value: IdentifierExpression: dst
    ├─ Inputs:
    │   └─ StringExpression: r
    │       └─ Value: IdentifierExpression: src
    ├─ Clobbers: <empty>
    └─ Options: <none>
ExpressionStatement
└─ Expr: UnwrapExpression (QUESTION)
    └─ Operand: IdentifierExpression: a
ExpressionStatement
└─ Expr: UnwrapExpression (BANG)
    └─ Operand: IdentifierExpression: b
)"
