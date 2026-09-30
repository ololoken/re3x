#pragma once

#include <stdint.h>

// On-demand HTTP fetch of game files, cached in the process filesystem.
// The emscripten harness mounts that filesystem on IndexedDB, so a later
// session reads the cached copy instead of downloading it again.
namespace HttpAssets {
	void Init(const char *envVarName = "ASSETS_URL");
	bool IsActive();

	// fopen. A missing file is fetched only when it is an unpacked member of
	// gta3.img, cuts.img or sfx.raw, or an audio/*.mp3 track, and only when
	// that file is not already in the local cache.
	void *Open(const char *path, const char *mode);

	// Block the caller (yielding to the browser) until relPath is local.
	// While a game frame is being drawn, Ensure returns as soon as the fetch
	// is started so rendering is not stuck inside the download.
	bool Ensure(const char *relPath);
	bool EnsureBlocking(const char *relPath);
	bool EnsureAll(char const *const *relPaths, uint32_t count);

	// Read a file that is already local. Returns bytes copied, or 0.
	uint32_t Read(const char *relPath, void *outBuf, uint32_t cap);

	// True when a previous download left this file in the local cache.
	bool IsLocal(const char *relPath);
	// Size of that cache file, or 0 when it is not local.
	uint32_t LocalSize(const char *relPath);
	// Drop a cached copy so a later IndexedDB sync does not keep rewriting it.
	void RemoveLocal(const char *relPath);

	// Download a remote file into memory. Does not write the save filesystem,
	// so IndexedDB will not keep rewriting a large radio track. `data` is
	// only valid for the duration of `done`.
	void FetchToMemory(const char *relPath,
	                   void (*done)(bool ok, const uint8_t *data, uint32_t size, void *user),
	                   void *user);

	// Map a sector range inside an .img to an unpacked relative path
	// (models/gta3/<name>, filled in by the streaming directory loader).
	void ClearImage(int image);
	void RegisterImageFile(int image, uint32_t sector, uint32_t sectorCount, const char *relPath);

	int VirtualFd(int imageIndex);
	bool IsVirtualFd(int fd);

	// Async stand-in for a CD sector read. `done` runs on the main thread,
	// and may run before StartImageRead returns when every file is cached.
	void StartImageRead(int fd, uint32_t sector, uint32_t sectorCount, void *buffer,
	                    void (*done)(bool ok, void *user), void *user);
}
