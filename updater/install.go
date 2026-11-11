package main

import (
	"archive/zip"
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

const stagePrefix = ".brightsync-update-"
const maxExpanded = 64 << 20

func extract(archive []byte, output string) error {
	z, err := zip.NewReader(bytes.NewReader(archive), int64(len(archive)))
	if err != nil {
		return err
	}
	if len(z.File) > 512 {
		return errors.New("too many files in update")
	}
	var expanded uint64
	seen := make(map[string]bool)
	for _, member := range z.File {
		name := strings.TrimSuffix(member.Name, "/")
		if name == "" || strings.ContainsAny(name, "\\:") || !filepath.IsLocal(name) ||
			filepath.ToSlash(filepath.Clean(name)) != name {
			return errors.New("invalid path in update archive")
		}
		folded := strings.ToLower(name)
		if seen[folded] {
			return errors.New("duplicate path in update archive")
		}
		seen[folded] = true
		if member.FileInfo().IsDir() {
			if err := os.MkdirAll(filepath.Join(output, name), 0755); err != nil {
				return err
			}
			continue
		}
		if !member.Mode().IsRegular() {
			return errors.New("unsupported file type in update archive")
		}
		if member.UncompressedSize64 > maxExpanded-expanded {
			return errors.New("update expands beyond size limit")
		}
		expanded += member.UncompressedSize64
		target := filepath.Join(output, name)
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			return err
		}
		in, err := member.Open()
		if err != nil {
			return err
		}
		mode := os.FileMode(0644) | member.Mode().Perm()&0111
		out, err := os.OpenFile(target, os.O_CREATE|os.O_EXCL|os.O_WRONLY, mode)
		if err != nil {
			in.Close()
			return err
		}
		written, copyErr := io.Copy(out, io.LimitReader(in, int64(member.UncompressedSize64)+1))
		closeErr := out.Close()
		in.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
		if uint64(written) != member.UncompressedSize64 {
			return errors.New("incorrect archive member size")
		}
	}
	return nil
}

type replacement struct{ installed, incoming string }

func replaceAll(files []replacement, backupRoot string, restart func() error) (err error) {
	type change struct {
		target, backup string
		installed      bool
	}
	var changes []change
	defer func() {
		if err == nil {
			return
		}
		for i := len(changes) - 1; i >= 0; i-- {
			entry := changes[i]
			if entry.installed {
				if failure := os.RemoveAll(entry.target); failure != nil {
					err = errors.Join(err, failure)
					continue
				}
			}
			if failure := os.Rename(entry.backup, entry.target); failure != nil {
				err = errors.Join(err, fmt.Errorf("restore backup at %s: %w", entry.backup, failure))
			}
		}
	}()
	for i, pair := range files {
		backup := filepath.Join(backupRoot, fmt.Sprintf("backup-%d", i))
		if err := os.Rename(pair.installed, backup); err != nil {
			return err
		}
		changes = append(changes, change{target: pair.installed, backup: backup})
		if err := os.Rename(pair.incoming, pair.installed); err != nil {
			return err
		}
		changes[len(changes)-1].installed = true
	}
	return restart()
}

func readLimited(path string, max int64) ([]byte, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	data, err := io.ReadAll(io.LimitReader(f, max+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > max {
		return nil, errors.New("staged file exceeds size limit")
	}
	return data, nil
}

func stageUpdate(update verifiedUpdate, target string, parent int) (err error) {
	stage, err := os.MkdirTemp(filepath.Dir(target), stagePrefix)
	if err != nil {
		return fmt.Errorf("installation directory is not writable: %w", err)
	}
	defer func() {
		if err != nil {
			os.RemoveAll(stage)
		}
	}()
	for name, bytes := range map[string][]byte{manifestName: update.manifest, signatureName: update.signature, "package.zip": update.archive} {
		if err := os.WriteFile(filepath.Join(stage, name), bytes, 0600); err != nil {
			return err
		}
	}
	info, err := readManifest(update.manifest, update.signature, releaseKey())
	if err != nil {
		return err
	}
	payload := filepath.Join(stage, "preflight")
	if err := extract(update.archive, payload); err != nil {
		return err
	}
	if _, err := platformFiles(target, payload, info.Version); err != nil {
		return err
	}
	if err := os.RemoveAll(payload); err != nil {
		return err
	}

	cache, err := os.UserCacheDir()
	if err != nil {
		return err
	}
	workerRoot := filepath.Join(cache, "BrightSync", "update-workers")
	if err := os.MkdirAll(workerRoot, 0700); err != nil {
		return err
	}
	// Windows can only remove an old worker after that process has exited.
	entries, _ := os.ReadDir(workerRoot)
	for _, item := range entries {
		if item.IsDir() && strings.HasPrefix(item.Name(), "worker-") {
			if stat, err := item.Info(); err == nil && time.Since(stat.ModTime()) > 24*time.Hour {
				_ = os.RemoveAll(filepath.Join(workerRoot, item.Name()))
			}
		}
	}
	worker, err := os.MkdirTemp(workerRoot, "worker-")
	if err != nil {
		return err
	}
	defer func() {
		if err != nil {
			os.RemoveAll(worker)
		}
	}()
	self, err := os.Executable()
	if err != nil {
		return err
	}
	binary, err := readLimited(self, maxArchive)
	if err != nil {
		return err
	}
	path := filepath.Join(worker, filepath.Base(self))
	if err := os.WriteFile(path, binary, 0700); err != nil {
		return err
	}
	cmd := exec.Command(path, "apply", target, stage, fmt.Sprint(parent))
	if err := cmd.Start(); err != nil {
		return err
	}
	_ = cmd.Process.Release()
	return nil
}

func applyUpdate(target, stage string, parent int) (err error) {
	if err := validateTarget(target); err != nil {
		return err
	}
	if filepath.Dir(stage) != filepath.Dir(target) || !strings.HasPrefix(filepath.Base(stage), stagePrefix) {
		return errors.New("invalid update stage")
	}
	info, err := os.Lstat(stage)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return errors.New("missing update stage")
	}
	// The parent has agreed to restart. Even a later verification error should
	// bring the existing app back once it releases its files.
	if err := waitForExit(parent); err != nil {
		return err
	}
	defer func() {
		if err != nil {
			_ = launchApp(target)
		}
	}()

	raw, err := readLimited(filepath.Join(stage, manifestName), 16<<10)
	if err != nil {
		return err
	}
	sig, err := readLimited(filepath.Join(stage, signatureName), 64)
	if err != nil {
		return err
	}
	meta, err := readManifest(raw, sig, releaseKey())
	if err != nil {
		return err
	}
	if valid, err := newer(meta.Version, currentVersion); err != nil || !valid {
		return errors.New("staged release is not newer")
	}
	archive, err := readLimited(filepath.Join(stage, "package.zip"), maxArchive)
	if err != nil {
		return err
	}
	if err := checkArchive(meta.Packages[platformArchive], archive); err != nil {
		return err
	}
	payload := filepath.Join(stage, "payload")
	if err := os.Mkdir(payload, 0700); err != nil {
		return err
	}
	if err := extract(archive, payload); err != nil {
		return err
	}
	files, err := platformFiles(target, payload, meta.Version)
	if err != nil {
		return err
	}
	if err := replaceAll(files, stage, func() error { return launchApp(target) }); err != nil {
		return err
	}
	_ = os.RemoveAll(stage)
	return nil
}

func requireRegular(path string) error {
	info, err := os.Lstat(path)
	if err != nil {
		return err
	}
	if !info.Mode().IsRegular() {
		return fmt.Errorf("%s is not a regular file", filepath.Base(path))
	}
	return nil
}
