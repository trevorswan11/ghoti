package doc

import "embed"

//go:embed all:syntax all:internals README.md
var FS embed.FS
