; Comments
(comment) @comment

; Literals
(string_literal) @string
(char_literal) @string
(integer_literal) @number
(float_literal) @number
(boolean_literal) @boolean
(undefined_literal) @constant.builtin
(unreachable_literal) @constant.builtin
(nullptr_literal) @constant.builtin
(underscore) @variable.special

; Types
(primitive_type) @type.builtin
(identifier) @type
  (#match? @type "^[A-Z]")

; Functions
(call_expression function: (identifier) @function)
(builtin_identifier) @function.builtin
(decl_statement name: (identifier) @function
  value: (function_expression))
(function_expression) @function
(self_parameter) @variable.builtin

; Struct/union/enum field & member names
(field_declaration name: (identifier) @property)
(field_initializer name: (identifier) @property)
(enumerator name: (identifier) @property)
(dot_expression member: (identifier) @property)
(implicit_access_expression member: (identifier) @property)
(module_access_expression member: (identifier) @property)

; Declarations
(decl_statement name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)
(import_statement alias: (identifier) @type)
(using_statement alias: (identifier) @type)

; Types used in position
(decl_statement type: (identifier) @type)
(parameter type: (identifier) @type)
(field_declaration type: (identifier) @type)
(function_expression return_type: (identifier) @type)
(function_type return_type: (identifier) @type)

; Labels
(labeled_statement label: (identifier) @label)
(labeled_expression label: (identifier) @label)
(break_statement label: (identifier) @label)
(continue_statement label: (identifier) @label)

; Keywords
[
  "fn"
  "var"
  "const"
  "constexpr"
  "struct"
  "enum"
  "union"
  "if"
  "else"
  "do"
  "match"
  "return"
  "defer"
  "loop"
  "for"
  "while"
  "continue"
  "break"
  "import"
  "as"
  "pub"
  "extern"
  "export"
  "volatile"
  "mut"
  "move"
  "packed"
  "using"
  "test"
] @keyword

; Boolean/logical operators spelled as words
["and" "or"] @keyword

; Operators
[
  "="
  ":="
  "+"
  "+="
  "-"
  "-="
  "*"
  "*="
  "/"
  "/="
  "%"
  "%="
  "!"
  "&"
  "&="
  "|"
  "|="
  "<<"
  "<<="
  ">>"
  ">>="
  "~"
  "~="
  "^"
  "^="
  "<"
  "<="
  ">"
  ">="
  "=="
  "!="
  "::"
  "."
  ".."
  "..="
  "=>"
  "..."
] @operator

; Punctuation
["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":"] @punctuation.delimiter
