package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const platformArchive = macArchive

func validateTarget(target string) error {
	if filepath.Ext(target) != ".app" {
		return errors.New("update target is not an app bundle")
	}
	info, err := os.Lstat(target)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return errors.New("update target is not an installed app bundle")
	}
	return requireRegular(filepath.Join(target, "Contents", "MacOS", "BrightSync"))
}

func platformFiles(target, payload, version string) ([]replacement, error) {
	app := filepath.Join(payload, "BrightSync.app")
	for _, binary := range []string{"MacOS/BrightSync", "Helpers/BrightSyncUpdater"} {
		if err := requireRegular(filepath.Join(app, "Contents", binary)); err != nil {
			return nil, err
		}
	}
	info, err := exec.Command("/usr/bin/plutil", "-extract", "CFBundleShortVersionString", "raw",
		filepath.Join(app, "Contents", "Info.plist")).Output()
	if err != nil || strings.TrimSpace(string(info)) != version {
		return nil, errors.New("app bundle version does not match release")
	}
	if err := exec.Command("/usr/bin/codesign", "--verify", "--deep", "--strict", app).Run(); err != nil {
		return nil, errors.New("new app bundle has an invalid macOS code signature")
	}
	team := func(path string) string {
		details, _ := exec.Command("/usr/bin/codesign", "-dv", "--verbose=4", path).CombinedOutput()
		for _, line := range strings.Split(string(details), "\n") {
			if value, ok := strings.CutPrefix(line, "TeamIdentifier="); ok && value != "not set" {
				return value
			}
		}
		return ""
	}
	if existing := team(target); existing != "" && team(app) != existing {
		return nil, errors.New("the signing team of the new app differs from the installed app")
	}
	return []replacement{{target, app}}, nil
}

func launchApp(target string) error { return exec.Command("/usr/bin/open", "-n", "-a", target).Run() }

func waitForExit(pid int) error {
	for end := time.Now().Add(30 * time.Second); time.Now().Before(end); time.Sleep(100 * time.Millisecond) {
		err := syscall.Kill(pid, 0)
		if err == syscall.ESRCH {
			return nil
		}
		if err != nil {
			return err
		}
	}
	return errors.New("the current app did not exit")
}
