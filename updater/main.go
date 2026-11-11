package main

import (
	"context"
	"crypto/ed25519"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

func releaseKey() ed25519.PublicKey {
	key, _ := hex.DecodeString(strings.TrimSpace(embeddedKey))
	return key
}

func run(args []string) (int, error) {
	if len(args) == 2 && args[0] == "keygen" {
		return 0, createKey(args[1])
	}
	if len(args) == 4 && args[0] == "sign" {
		return 0, signRelease(args[1], args[2], args[3])
	}
	if len(args) == 4 && args[0] == "apply" {
		pid, err := strconv.Atoi(args[3])
		if err != nil || pid <= 0 {
			return 1, errors.New("invalid parent process")
		}
		return 0, applyUpdate(args[1], args[2], pid)
	}
	probe := len(args) == 2 && args[0] == "verify-latest"
	prepare := len(args) == 3 && args[0] == "prepare"
	if !probe && !prepare {
		return 1, errors.New("invalid updater command")
	}
	if prepare {
		if err := validateTarget(args[1]); err != nil {
			return 1, err
		}
	}
	raw, err := io.ReadAll(io.LimitReader(os.Stdin, 300))
	if err != nil {
		return 1, err
	}
	token := strings.TrimSpace(string(raw))
	if !validToken(token) {
		return 3, errAccess
	}
	f, err := liveFeed(token)
	if err != nil {
		return 1, err
	}
	current := currentVersion
	if probe {
		current = args[1]
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	update, err := f.latest(ctx, current, platformArchive)
	if errors.Is(err, errCurrent) {
		return 0, nil
	}
	if errors.Is(err, errAccess) {
		return 3, err
	}
	if err != nil {
		return 1, err
	}
	if probe {
		info, err := readManifest(update.manifest, update.signature, f.public)
		if err != nil {
			return 1, err
		}
		fmt.Println("Verified release " + info.Version)
		return 0, nil
	}
	pid, err := strconv.Atoi(args[2])
	if err != nil || pid <= 0 {
		return 1, errors.New("invalid parent process")
	}
	if err := stageUpdate(update, args[1], pid); err != nil {
		return 1, err
	}
	return 10, nil
}

func logFailure(err error) {
	cache, failure := os.UserCacheDir()
	if failure != nil {
		return
	}
	dir := filepath.Join(cache, "BrightSync")
	if os.MkdirAll(dir, 0700) == nil {
		_ = os.WriteFile(filepath.Join(dir, "update-error.log"), []byte(err.Error()+"\n"), 0600)
	}
}

func main() {
	code, err := run(os.Args[1:])
	if err != nil {
		if code == 0 {
			code = 1
		}
		if len(os.Args) > 1 && (os.Args[1] == "prepare" || os.Args[1] == "apply") {
			logFailure(err)
		}
		fmt.Fprintln(os.Stderr, err)
	}
	os.Exit(code)
}
