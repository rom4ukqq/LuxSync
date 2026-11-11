package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

func TestSignedRelease(t *testing.T) {
	public, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		t.Fatal(err)
	}
	original := []byte("the signed archive")
	spec := packageInfo{int64(len(original)), fmt.Sprintf("%x", sha256.Sum256(original))}
	raw, err := json.Marshal(manifest{"1.10.0", map[string]packageInfo{platformArchive: spec}})
	if err != nil {
		t.Fatal(err)
	}
	signature := ed25519.Sign(private, raw)
	const token = "github_pat_fixture_token"
	for _, test := range []struct {
		name, tag, current string
		archive, signature []byte
		want               error
		corrupt            bool
	}{
		{"upgrade", "v1.10.0", "1.9.9", original, signature, nil, false},
		{"same version", "v1.10.0", "1.10.0", original, signature, errCurrent, false},
		{"downgrade", "v1.10.0", "2.0.0", original, signature, errCurrent, false},
		{"altered download", "v1.10.0", "1.9.9", bytes.Repeat([]byte{1}, len(original)), signature, nil, true},
		{"altered signature", "v1.10.0", "1.9.9", original, make([]byte, 64), nil, true},
		{"tag mismatch", "v1.11.0", "1.9.9", original, signature, nil, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			archiveRequests := 0
			api := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				if r.Header.Get("Authorization") != "Bearer "+token {
					http.Error(w, "unauthorized", http.StatusUnauthorized)
					return
				}
				switch r.URL.Path {
				case "/releases/latest":
					fmt.Fprintf(w, `{"tag_name":%q,"assets":[{"name":%q,"id":1},{"name":%q,"id":2},{"name":%q,"id":3}]}`,
						test.tag, manifestName, signatureName, platformArchive)
				case "/releases/assets/1":
					w.Write(raw)
				case "/releases/assets/2":
					w.Write(test.signature)
				case "/releases/assets/3":
					archiveRequests++
					w.Write(test.archive)
				default:
					http.NotFound(w, r)
				}
			}))
			defer api.Close()
			client := feed{api.URL, token, api.Client(), public}
			result, err := client.latest(context.Background(), test.current, platformArchive)
			if test.corrupt {
				if err == nil {
					t.Fatal("corrupt update accepted")
				}
				return
			}
			if !errors.Is(err, test.want) {
				t.Fatalf("got %v, want %v", err, test.want)
			}
			if err == nil && !bytes.Equal(result.archive, original) {
				t.Fatal("wrong payload")
			}
			if errors.Is(err, errCurrent) && archiveRequests != 0 {
				t.Fatal("downloaded an existing version")
			}
		})
	}
}

func TestRedirectDoesNotForwardCredential(t *testing.T) {
	for _, destination := range []struct {
		url     string
		allowed bool
	}{
		{"https://release-assets.githubusercontent.com/file", true},
		{"https://example.com/file", false},
		{"http://release-assets.githubusercontent.com/file", false},
		{"https://api.github.com/file", false},
	} {
		request, err := http.NewRequest("GET", destination.url, nil)
		if err != nil {
			t.Fatal(err)
		}
		request.Header.Set("Authorization", "Bearer fixture")
		err = githubRedirect(request, nil)
		if (err == nil) != destination.allowed || request.Header.Get("Authorization") != "" {
			t.Fatalf("unsafe redirect behavior for %s", destination.url)
		}
	}
}

func TestExtractRejectsTraversalAndLinks(t *testing.T) {
	for _, entry := range []struct {
		name  string
		mode  os.FileMode
		valid bool
	}{
		{"BrightSync.exe", 0755, true},
		{"../outside", 0644, false},
		{"folder\\outside", 0644, false},
		{"symlink", os.ModeSymlink | 0777, false},
	} {
		t.Run(entry.name, func(t *testing.T) {
			var raw bytes.Buffer
			z := zip.NewWriter(&raw)
			header := &zip.FileHeader{Name: entry.name}
			header.SetMode(entry.mode)
			w, err := z.CreateHeader(header)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := w.Write([]byte("file")); err != nil {
				t.Fatal(err)
			}
			if err := z.Close(); err != nil {
				t.Fatal(err)
			}
			root := t.TempDir()
			if err := extract(raw.Bytes(), filepath.Join(root, "extract")); (err == nil) != entry.valid {
				t.Fatalf("unexpected extraction result: %v", err)
			}
			if _, err := os.Stat(filepath.Join(root, "outside")); !errors.Is(err, os.ErrNotExist) {
				t.Fatal("archive escaped the staging directory")
			}
		})
	}
}

func TestReplaceRestoresOriginalOnFailure(t *testing.T) {
	for _, fail := range []string{"none", "second file", "restart"} {
		t.Run(fail, func(t *testing.T) {
			root := t.TempDir()
			path := func(name string) string { return filepath.Join(root, name) }
			write := func(name, content string) {
				t.Helper()
				if err := os.WriteFile(path(name), []byte(content), 0600); err != nil {
					t.Fatal(err)
				}
			}
			write("app", "old app")
			write("helper", "old helper")
			write("incoming-app", "new app")
			if fail != "second file" {
				write("incoming-helper", "new helper")
			}
			err := replaceAll([]replacement{{path("app"), path("incoming-app")}, {path("helper"), path("incoming-helper")}},
				root, func() error {
					if fail == "restart" {
						return errors.New("restart failed")
					}
					return nil
				})
			want := "new"
			if fail != "none" {
				want = "old"
				if err == nil {
					t.Fatal("expected replacement failure")
				}
			} else if err != nil {
				t.Fatal(err)
			}
			for _, name := range []string{"app", "helper"} {
				data, err := os.ReadFile(path(name))
				if err != nil || string(data) != want+" "+name {
					t.Fatalf("incorrect %s after update: %s, %v", name, data, err)
				}
			}
		})
	}
}
