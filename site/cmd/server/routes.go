package main

import (
	"log"
	"net/http"

	"webserver/internal/render"
	"webserver/static"
	"webserver/views"

	"github.com/a-h/templ"
)

func registerRoutes(mux *http.ServeMux) {
	mux.HandleFunc("GET /{$}", handleHome)
	mux.HandleFunc("GET /doc/{slug...}", handleDocPage)
	mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServerFS(static.FS)))
	mux.HandleFunc("/", handleNotFound)
}

func handleHome(w http.ResponseWriter, r *http.Request) {
	renderPage(w, r, render.Home)
}

func handleDocPage(w http.ResponseWriter, r *http.Request) {
	item, ok := render.Lookup(r.PathValue("slug"))
	if !ok {
		handleNotFound(w, r)
		return
	}
	renderPage(w, r, item)
}

func renderPage(w http.ResponseWriter, r *http.Request, item render.NavItem) {
	page, err := render.Render(item)
	if err != nil {
		log.Printf("render %s: %v", item.Slug, err)
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}

	prev, next := render.Siblings(item.Slug)
	ctx := templ.WithChildren(r.Context(), views.DocPage(page, prev, next))

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	if err := views.Layout(page.Title, item.Slug).Render(ctx, w); err != nil {
		log.Printf("write %s: %v", item.Slug, err)
	}
}

func handleNotFound(w http.ResponseWriter, r *http.Request) {
	w.WriteHeader(http.StatusNotFound)
	ctx := templ.WithChildren(r.Context(), views.NotFound())
	if err := views.Layout("Not Found", "").Render(ctx, w); err != nil {
		log.Printf("write 404: %v", err)
	}
}
