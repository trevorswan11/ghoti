package render

import (
	"strings"

	"webserver/views/pages"

	"github.com/a-h/templ"
)

// NavItem is a single linkable page in the sidebar.
type NavItem struct {
	Title     string
	Slug      string
	Component func() templ.Component
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
			{Title: "Arrays", Slug: "syntax/arrays", Component: pages.SyntaxArraysPage},
			{Title: "Control Flow", Slug: "syntax/control_flow", Component: pages.SyntaxControlFlowPage},
			{Title: "Declarations", Slug: "syntax/declarations", Component: pages.SyntaxDeclarationsPage},
			{Title: "Enums", Slug: "syntax/enums", Component: pages.SyntaxEnumsPage},
			{Title: "Functions", Slug: "syntax/functions", Component: pages.SyntaxFunctionsPage},
			{Title: "Imports", Slug: "syntax/imports", Component: pages.SyntaxImportsPage},
			{Title: "Loops", Slug: "syntax/loops", Component: pages.SyntaxLoopsPage},
			{Title: "Match", Slug: "syntax/match", Component: pages.SyntaxMatchPage},
			{Title: "Structs", Slug: "syntax/structs", Component: pages.SyntaxStructsPage},
			{Title: "Types", Slug: "syntax/types", Component: pages.SyntaxTypesPage},
			{Title: "Unions", Slug: "syntax/unions", Component: pages.SyntaxUnionsPage},
		},
	},
	{
		Title: "Compiler Internals",
		Items: []NavItem{
			{Title: "Builtins", Slug: "internals/builtins", Component: pages.InternalsBuiltinsPage},
			{Title: "Literals", Slug: "internals/literals", Component: pages.InternalsLiteralsPage},
		},
	},
}

var Home = NavItem{Title: "Overview", Slug: "", Component: pages.OverviewPage}

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
	Title     string
	Component templ.Component
}

// Render loads the documentation page for item
func Render(item NavItem) (Page, error) {
	return Page{
		Title:     item.Title,
		Component: item.Component(),
	}, nil
}
