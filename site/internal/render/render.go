package render

import (
	"bytes"
	"fmt"
	"path"
	"strings"

	"github.com/yuin/goldmark"
	"github.com/yuin/goldmark/ast"
	"github.com/yuin/goldmark/parser"
	"github.com/yuin/goldmark/text"
	"github.com/yuin/goldmark/util"

	"webserver/doc"
)

// NavItem is a single linkable page in the sidebar.
type NavItem struct {
	Title string
	Slug  string
	path  string
}

// NavSection groups related NavItems under a heading.
type NavSection struct {
	Title string
	Items []NavItem
}

var Nav = []NavSection{
	{
		Title: "Syntax",
		Items: []NavItem{
			{Title: "Arrays", Slug: "syntax/arrays", path: "syntax/arrays.md"},
			{Title: "Control Flow", Slug: "syntax/control_flow", path: "syntax/control_flow.md"},
			{Title: "Declarations", Slug: "syntax/declarations", path: "syntax/declarations.md"},
			{Title: "Enums", Slug: "syntax/enums", path: "syntax/enums.md"},
			{Title: "Functions", Slug: "syntax/functions", path: "syntax/functions.md"},
			{Title: "Imports", Slug: "syntax/imports", path: "syntax/imports.md"},
			{Title: "Loops", Slug: "syntax/loops", path: "syntax/loops.md"},
			{Title: "Match", Slug: "syntax/match", path: "syntax/match.md"},
			{Title: "Structs", Slug: "syntax/structs", path: "syntax/structs.md"},
			{Title: "Types", Slug: "syntax/types", path: "syntax/types.md"},
			{Title: "Unions", Slug: "syntax/unions", path: "syntax/unions.md"},
		},
	},
	{
		Title: "Compiler Internals",
		Items: []NavItem{
			{Title: "Builtins", Slug: "internals/builtins", path: "internals/builtins.md"},
			{Title: "Literals", Slug: "internals/literals", path: "internals/literals.md"},
		},
	},
}

var Home = NavItem{Title: "Overview", Slug: "", path: "README.md"}

var bySlug map[string]NavItem
var ordered []NavItem

func init() {
	bySlug = make(map[string]NavItem, len(Nav)*4)
	bySlug[Home.Slug] = Home
	for _, section := range Nav {
		for _, item := range section.Items {
			bySlug[item.Slug] = item
			ordered = append(ordered, item)
		}
	}
}

func Lookup(slug string) (NavItem, bool) {
	item, ok := bySlug[strings.Trim(slug, "/")]
	return item, ok
}

// Returns the previous and next NavItem relative to slug
func Siblings(slug string) (prev, next NavItem) {
	slug = strings.Trim(slug, "/")
	if slug == "" {
		if len(ordered) > 0 {
			next = ordered[0]
		}
		return prev, next
	}
	for i, item := range ordered {
		if item.Slug != slug {
			continue
		}
		if i == 0 {
			prev = Home
		} else {
			prev = ordered[i-1]
		}
		if i < len(ordered)-1 {
			next = ordered[i+1]
		}
		return prev, next
	}
	return prev, next
}

// Href returns the site-relative route for a NavItem, including Home.
func Href(item NavItem) string {
	if item.Slug == "" {
		return "/"
	}
	return "/doc/" + item.Slug
}

// Page is a rendered documentation page ready to hand to a templ component.
type Page struct {
	Title string
	HTML  string
}

// Render loads and converts the markdown file backing item into HTML
func Render(item NavItem) (Page, error) {
	src, err := doc.FS.ReadFile(item.path)
	if err != nil {
		return Page{}, fmt.Errorf("render: read %s: %w", item.path, err)
	}

	dir := path.Dir(item.path)
	md := goldmark.New(
		goldmark.WithParserOptions(
			parser.WithASTTransformers(util.Prioritized(linkRewriter{dir: dir}, 100)),
		),
	)

	reader := text.NewReader(src)
	root := md.Parser().Parse(reader)

	var buf bytes.Buffer
	if err := md.Renderer().Render(&buf, src, root); err != nil {
		return Page{}, fmt.Errorf("render: convert %s: %w", item.path, err)
	}

	title := extractTitle(root, src)
	if title == "" {
		title = item.Title
	}
	return Page{Title: title, HTML: buf.String()}, nil
}

// extractTitle returns the text of the document's first level-1 heading.
func extractTitle(root ast.Node, source []byte) string {
	var title string
	_ = ast.Walk(root, func(n ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering || title != "" {
			return ast.WalkContinue, nil
		}
		heading, ok := n.(*ast.Heading)
		if !ok || heading.Level != 1 {
			return ast.WalkContinue, nil
		}
		var b strings.Builder
		for c := heading.FirstChild(); c != nil; c = c.NextSibling() {
			if t, ok := c.(*ast.Text); ok {
				b.Write(t.Segment.Value(source))
			}
		}
		title = b.String()
		return ast.WalkStop, nil
	})
	return title
}

type linkRewriter struct{ dir string }

func (r linkRewriter) Transform(node *ast.Document, _ text.Reader, _ parser.Context) {
	_ = ast.Walk(node, func(n ast.Node, entering bool) (ast.WalkStatus, error) {
		if !entering {
			return ast.WalkContinue, nil
		}
		link, ok := n.(*ast.Link)
		if !ok {
			return ast.WalkContinue, nil
		}
		dest := string(link.Destination)
		if strings.Contains(dest, "://") {
			return ast.WalkContinue, nil
		}

		target, fragment, _ := strings.Cut(dest, "#")
		if !strings.HasSuffix(target, ".md") {
			return ast.WalkContinue, nil
		}
		target = strings.TrimSuffix(path.Join(r.dir, target), ".md")
		target = "/doc/" + target

		if fragment != "" {
			target += "#" + fragment
		}
		link.Destination = []byte(target)
		return ast.WalkContinue, nil
	})
}
