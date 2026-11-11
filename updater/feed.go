package main

import (
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const (
	repository     = "/repos/rom4ukqq/LuxSync"
	manifestName   = "BrightSync-update.json"
	signatureName  = "BrightSync-update.sig"
	macArchive     = "BrightSync-macOS-arm64.zip"
	windowsArchive = "BrightSync-Windows-x64.zip"
	maxArchive     = 32 << 20
)

var currentVersion = "0.0.0"

//go:embed release_public_key.txt
var embeddedKey string

type packageInfo struct {
	Size   int64  `json:"size"`
	SHA256 string `json:"sha256"`
}

type manifest struct {
	Version  string                 `json:"version"`
	Packages map[string]packageInfo `json:"packages"`
}

type release struct {
	Tag        string `json:"tag_name"`
	Draft      bool   `json:"draft"`
	Prerelease bool   `json:"prerelease"`
	Assets     []struct {
		ID   int64  `json:"id"`
		Name string `json:"name"`
	} `json:"assets"`
}

var errCurrent = errors.New("BrightSync is up to date")
var errAccess = errors.New("GitHub denied access to LuxSync. Check the update token and its repository permissions")

func versionNumber(text string) ([3]int, error) {
	var result [3]int
	parts := strings.Split(text, ".")
	if len(parts) != len(result) {
		return result, errors.New("invalid version")
	}
	for i, part := range parts {
		n, err := strconv.Atoi(part)
		if err != nil || n < 0 || n > 999999 || strconv.Itoa(n) != part {
			return result, errors.New("invalid version")
		}
		result[i] = n
	}
	return result, nil
}

func newer(candidate, current string) (bool, error) {
	a, err := versionNumber(candidate)
	if err != nil {
		return false, err
	}
	b, err := versionNumber(current)
	if err != nil {
		return false, err
	}
	for i := range a {
		if a[i] != b[i] {
			return a[i] > b[i], nil
		}
	}
	return false, nil
}

func validToken(token string) bool {
	if len(token) < 20 || len(token) > 256 {
		return false
	}
	for _, c := range token {
		if !(c == '_' || c >= '0' && c <= '9' || c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z') {
			return false
		}
	}
	return true
}

func githubRedirect(req *http.Request, via []*http.Request) error {
	// The credential belongs only to the initial API request. The CDN gets a signed URL.
	req.Header.Del("Authorization")
	req.Header.Del("Referer")
	host := req.URL.Host
	if len(via) > 4 || req.URL.Scheme != "https" || req.URL.User != nil ||
		(host != "release-assets.githubusercontent.com" && host != "objects.githubusercontent.com" &&
			host != "github-releases.githubusercontent.com") {
		return errors.New("unexpected release asset redirect")
	}
	return nil
}

type feed struct {
	base   string
	token  string
	client *http.Client
	public ed25519.PublicKey
}

func liveFeed(token string) (feed, error) {
	key, err := hex.DecodeString(strings.TrimSpace(embeddedKey))
	if err != nil || len(key) != ed25519.PublicKeySize || strings.Trim(strings.TrimSpace(embeddedKey), "0") == "" {
		return feed{}, errors.New("missing release signing key")
	}
	return feed{"https://api.github.com" + repository, token,
		&http.Client{Timeout: 75 * time.Second, CheckRedirect: githubRedirect}, key}, nil
}

func (f feed) get(ctx context.Context, path, accept string, limit int64) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, f.base+path, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", accept)
	req.Header.Set("Authorization", "Bearer "+f.token)
	req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
	req.Header.Set("User-Agent", "BrightSync-Updater")
	resp, err := f.client.Do(req)
	if err != nil {
		// url.Error can contain the CDN's temporary signed URL.
		var requestErr *url.Error
		if errors.As(err, &requestErr) {
			err = requestErr.Err
		}
		return nil, fmt.Errorf("GitHub request failed: %w", err)
	}
	defer resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusOK:
	case http.StatusUnauthorized, http.StatusForbidden, http.StatusNotFound:
		return nil, errAccess
	default:
		return nil, fmt.Errorf("GitHub returned HTTP %d", resp.StatusCode)
	}
	if resp.ContentLength > limit {
		return nil, errors.New("release asset exceeds size limit")
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, errors.New("release asset exceeds size limit")
	}
	return data, nil
}

type verifiedUpdate struct {
	manifest  []byte
	signature []byte
	archive   []byte
}

func readManifest(raw, sig []byte, key ed25519.PublicKey) (manifest, error) {
	var result manifest
	if len(key) != ed25519.PublicKeySize || len(sig) != ed25519.SignatureSize || !ed25519.Verify(key, raw, sig) {
		return result, errors.New("release signature is invalid")
	}
	if err := json.Unmarshal(raw, &result); err != nil {
		return result, err
	}
	if _, err := versionNumber(result.Version); err != nil {
		return result, err
	}
	return result, nil
}

func checkArchive(spec packageInfo, archive []byte) error {
	if spec.Size <= 0 || spec.Size > maxArchive || int64(len(archive)) != spec.Size ||
		fmt.Sprintf("%x", sha256.Sum256(archive)) != spec.SHA256 {
		return errors.New("download does not match the signed manifest")
	}
	return nil
}

func (f feed) latest(ctx context.Context, current, archiveName string) (verifiedUpdate, error) {
	var result verifiedUpdate
	raw, err := f.get(ctx, "/releases/latest", "application/vnd.github+json", 1<<20)
	if err != nil {
		return result, err
	}
	var rel release
	if err := json.Unmarshal(raw, &rel); err != nil {
		return result, err
	}
	if rel.Draft || rel.Prerelease || !strings.HasPrefix(rel.Tag, "v") {
		return result, errCurrent
	}
	available, err := newer(strings.TrimPrefix(rel.Tag, "v"), current)
	if err != nil {
		return result, err
	}
	if !available {
		return result, errCurrent
	}

	asset := func(name string, limit int64) ([]byte, error) {
		for _, file := range rel.Assets {
			if file.Name == name && file.ID > 0 {
				return f.get(ctx, "/releases/assets/"+strconv.FormatInt(file.ID, 10), "application/octet-stream", limit)
			}
		}
		return nil, fmt.Errorf("release is missing %s", name)
	}
	result.manifest, err = asset(manifestName, 16<<10)
	if err != nil {
		return result, err
	}
	result.signature, err = asset(signatureName, ed25519.SignatureSize)
	if err != nil {
		return result, err
	}
	info, err := readManifest(result.manifest, result.signature, f.public)
	if err != nil {
		return result, err
	}
	if rel.Tag != "v"+info.Version {
		return result, errors.New("release tag does not match the signed version")
	}
	spec := info.Packages[archiveName]
	if spec.Size <= 0 || spec.Size > maxArchive {
		return result, errors.New("no valid package for this platform")
	}
	result.archive, err = asset(archiveName, spec.Size)
	if err != nil {
		return result, err
	}
	return result, checkArchive(spec, result.archive)
}

func createKey(path string) error {
	public, private, err := ed25519.GenerateKey(nil)
	if err != nil {
		return err
	}
	file, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, writeErr := fmt.Fprintf(file, "%x\n", private.Seed())
	closeErr := file.Close()
	if writeErr != nil {
		return writeErr
	}
	if closeErr != nil {
		return closeErr
	}
	fmt.Printf("%x\n", public)
	return nil
}

func signRelease(version, keyFile, dir string) error {
	if _, err := versionNumber(version); err != nil {
		return err
	}
	raw, err := os.ReadFile(keyFile)
	if err != nil {
		return err
	}
	seed, err := hex.DecodeString(strings.TrimSpace(string(raw)))
	if err != nil || len(seed) != ed25519.SeedSize {
		return errors.New("invalid signing key")
	}
	private := ed25519.NewKeyFromSeed(seed)
	if fmt.Sprintf("%x", private.Public()) != strings.TrimSpace(embeddedKey) {
		return errors.New("signing key does not match the embedded public key")
	}
	info := manifest{Version: version, Packages: make(map[string]packageInfo)}
	for _, archive := range []string{macArchive, windowsArchive} {
		data, err := os.ReadFile(filepath.Join(dir, archive))
		if err != nil {
			return err
		}
		if len(data) == 0 || len(data) > maxArchive {
			return errors.New("invalid package size")
		}
		info.Packages[archive] = packageInfo{Size: int64(len(data)), SHA256: fmt.Sprintf("%x", sha256.Sum256(data))}
	}
	encoded, err := json.MarshalIndent(info, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(dir, manifestName), encoded, 0644); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, signatureName), ed25519.Sign(private, encoded), 0644)
}
