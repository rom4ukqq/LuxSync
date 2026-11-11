package main

import (
	"errors"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

const platformArchive = windowsArchive

func validateTarget(target string) error {
	if !strings.EqualFold(filepath.Base(target), "BrightSync.exe") {
		return errors.New("the installed application must be named BrightSync.exe")
	}
	return requireRegular(target)
}

func platformFiles(target, payload, version string) ([]replacement, error) {
	files := []replacement{
		{target, filepath.Join(payload, "BrightSync.exe")},
		{filepath.Join(filepath.Dir(target), "BrightSyncUpdater.exe"), filepath.Join(payload, "BrightSyncUpdater.exe")},
	}
	for _, entry := range files {
		if err := requireRegular(entry.installed); err != nil {
			return nil, err
		}
		if err := requireRegular(entry.incoming); err != nil {
			return nil, err
		}
	}
	return files, nil
}

func launchApp(target string) error {
	cmd := exec.Command(target)
	cmd.Dir = filepath.Dir(target)
	if err := cmd.Start(); err != nil {
		return err
	}
	_ = cmd.Process.Release()
	return nil
}

func waitForExit(pid int) error {
	const synchronize = 0x00100000
	handle, err := syscall.OpenProcess(synchronize, false, uint32(pid))
	if err == syscall.Errno(87) {
		return nil
	} // already exited
	if err != nil {
		return err
	}
	defer syscall.CloseHandle(handle)
	status, err := syscall.WaitForSingleObject(handle, 30000)
	if err != nil {
		return err
	}
	if status != syscall.WAIT_OBJECT_0 {
		return errors.New("the current app did not exit")
	}
	return nil
}
