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
