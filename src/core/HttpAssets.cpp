#include "HttpAssets.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#include <emscripten.h>
#include <emscripten/fetch.h>

namespace {

const uint32_t kSector = 2048;
// Above any real file descriptor. CdStream stores fd+1, so the raw value
// stored in CdReadInfo::hFile is this sentinel.
const int kVirtualBase = 0x40000000;

std::string g_root = "assets/";
bool g_active = false;

struct Entry {
	uint32_t sectors;
	std::string path;
};

std::vector<std::map<uint32_t, Entry>> g_images;

struct Waiter {
	void (*cb)(bool ok, void *user);
	void *user;
};

struct MemWaiter {
	void (*done)(bool ok, const uint8_t *data, uint32_t size, void *user);
	void *user;
};

struct FetchOp {
	std::string key;
	std::vector<Waiter> waiters;
	std::vector<MemWaiter> memWaiters;
};

std::map<std::string, FetchOp *> g_inflight;
std::unordered_set<std::string> g_missing;
bool g_syncScheduled = false;
uint32_t g_loadDone = 0;
uint32_t g_loadTotal = 0;

std::string
normalizeSlashes(const char *name)
{
	std::string s;
	if (!name)
		return s;
	for (const char *p = name; *p; ++p)
		s.push_back(*p == '\\' ? '/' : *p);
	while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
		s.erase(0, 2);
	while (!s.empty() && (s.front() == '/' || s.front() == '.')) {
		if (s.front() == '.') {
			if (s.size() == 1 || s[1] == '/')
				s.erase(s.begin());
			else
				break;
		} else {
			s.erase(s.begin());
		}
	}
	return s;
}

std::string
assetCacheKey(const char *name)
{
	std::string s = normalizeSlashes(name);
	for (char &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

bool
unsafePath(const std::string &s)
{
	return s.empty() || s.find("..") != std::string::npos;
}

// Unpacked gta3.img / cuts.img / sfx.raw members, plus game radio and
// cutscene tracks (audio/*.mp3).
bool
isRemoteMember(const std::string &key)
{
	if (key.rfind("models/gta3/", 0) == 0
	    || key.rfind("anim/cuts/", 0) == 0
	    || key.rfind("audio/sfx/", 0) == 0)
		return true;
	static const char kMp3[] = ".mp3";
	const size_t n = sizeof(kMp3) - 1;
	return key.rfind("audio/", 0) == 0
	    && key.size() >= n
	    && key.compare(key.size() - n, n, kMp3) == 0;
}

bool
fileExists(const std::string &path)
{
	if (path.empty())
		return false;
	struct stat st;
	if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode))
		return true;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fclose(f);
	return true;
}

// IndexedDB keeps whatever relative path we wrote last time. Accept the
// lowercase cache key and the path the caller asked for.
bool
alreadyCached(const char *relPath, const std::string &key)
{
	if (fileExists(key))
		return true;
	std::string given = normalizeSlashes(relPath);
	if (given != key && fileExists(given))
		return true;
	if (relPath && fileExists(relPath))
		return true;
	return false;
}

// Path of a cached file, and its size. Empty when nothing is local.
bool
localFile(const char *relPath, std::string &out, uint32_t *sizeOut)
{
	std::string key = assetCacheKey(relPath);
	auto take = [&](const std::string &path) {
		struct stat st;
		if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
			return false;
		out = path;
		if (sizeOut)
			*sizeOut = (uint32_t)st.st_size;
		return true;
	};
	if (take(key))
		return true;
	std::string given = normalizeSlashes(relPath);
	if (given != key && take(given))
		return true;
	if (relPath && take(relPath))
		return true;
	return false;
}

struct MemOp {
	std::string key;
	std::vector<MemWaiter> memWaiters;
	std::vector<Waiter> diskWaiters;
};

std::map<std::string, MemOp *> g_memInflight;

static bool
anyInflight()
{
	return !g_inflight.empty() || !g_memInflight.empty();
}

bool writeFile(const std::string &path, const uint8_t *data, size_t size);
void scheduleSync();
void notifyLoadBar();

void
finishMemFetch(emscripten_fetch_t *fetch, bool httpOk)
{
	MemOp *op = (MemOp *)fetch->userData;
	int status = fetch->status;
	std::vector<uint8_t> bytes;
	if (httpOk && status == 200 && fetch->data && fetch->numBytes)
		bytes.assign((const uint8_t *)fetch->data, (const uint8_t *)fetch->data + fetch->numBytes);
	emscripten_fetch_close(fetch);
	if (!op)
		return;

	bool ok = !bytes.empty();
	if (!ok && status == 404)
		g_missing.insert(op->key);
	if (!ok && status != 404)
		printf("HttpAssets: %s%s failed (%d)\n", g_root.c_str(), op->key.c_str(), status);

	auto memWaiters = std::move(op->memWaiters);
	auto diskWaiters = std::move(op->diskWaiters);
	std::string key = op->key;
	g_memInflight.erase(key);
	g_loadDone++;
	delete op;

	// A disk waiter asked for a cached file. Memory-only callers (radio)
	// do not, so a large track is not written into IndexedDB.
	bool wrote = false;
	if (ok && !diskWaiters.empty()) {
		wrote = writeFile(key, bytes.data(), bytes.size());
		if (wrote)
			scheduleSync();
	}
	notifyLoadBar();
	for (auto &w : memWaiters) {
		if (w.done)
			w.done(ok, ok ? bytes.data() : nullptr, ok ? (uint32_t)bytes.size() : 0, w.user);
	}
	for (auto &w : diskWaiters) {
		if (w.cb)
			w.cb(wrote, w.user);
	}
}

void
onMemFetch(emscripten_fetch_t *fetch)
{
	finishMemFetch(fetch, true);
}

void
onMemFetchError(emscripten_fetch_t *fetch)
{
	finishMemFetch(fetch, false);
}

void
ensureParentDirs(const char *path)
{
	std::string p(path);
	auto slash = p.find_last_of('/');
	if (slash == std::string::npos)
		return;
	p.resize(slash);
	std::string cur;
	for (size_t i = 0; i < p.size(); ++i) {
		if (p[i] == '/') {
			if (!cur.empty())
				mkdir(cur.c_str(), 0777);
		}
		cur.push_back(p[i]);
	}
	if (!cur.empty() && cur.back() == '/')
		cur.pop_back();
	if (!cur.empty())
		mkdir(cur.c_str(), 0777);
}

bool
writeFile(const std::string &path, const uint8_t *data, size_t size)
{
	ensureParentDirs(path.c_str());
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;
	if (size && fwrite(data, 1, size, f) != size) {
		fclose(f);
		return false;
	}
	fclose(f);
	return true;
}

void
flushSync(void *)
{
	g_syncScheduled = false;
		EM_ASM({
			if (typeof Module !== 'undefined' && Module.callbacks && Module.callbacks.onFileWrite)
				Module.callbacks.onFileWrite({ path: 'http-assets', op: 'write' });
		}, 0);
}

void
scheduleSync()
{
	if (g_syncScheduled)
		return;
	g_syncScheduled = true;
	emscripten_async_call(flushSync, nullptr, 200);
}

void
notifyLoadBar()
{
	EM_ASM({
		if (typeof Module !== 'undefined' && Module.callbacks && Module.callbacks.onAssetLoad)
			Module.callbacks.onAssetLoad({ done: $0, total: $1, active: $2 });
	}, g_loadDone, g_loadTotal, anyInflight() ? 1 : 0);
}

void startFetch(FetchOp *op);

void
complete(FetchOp *op, bool ok, int status)
{
	if (!ok && status == 404)
		g_missing.insert(op->key);
	auto waiters = std::move(op->waiters);
	g_inflight.erase(op->key);
	g_loadDone++;
	delete op;
	notifyLoadBar();
	for (auto &w : waiters) {
		if (w.cb)
			w.cb(ok, w.user);
	}
}

void
finishFetch(emscripten_fetch_t *fetch, bool httpOk)
{
	FetchOp *op = (FetchOp *)fetch->userData;
	int status = fetch->status;
	std::vector<uint8_t> bytes;
	if (httpOk && status == 200 && fetch->data && fetch->numBytes)
		bytes.assign((const uint8_t *)fetch->data, (const uint8_t *)fetch->data + fetch->numBytes);
	emscripten_fetch_close(fetch);

	if (!op)
		return;

	auto memWaiters = op->memWaiters;
	if (!httpOk || status != 200) {
		printf("HttpAssets: %s%s failed (%d)\n", g_root.c_str(), op->key.c_str(), status);
		complete(op, false, status);
		for (auto &w : memWaiters) {
			if (w.done)
				w.done(false, nullptr, 0, w.user);
		}
		return;
	}

	// A 200 with an empty body is still a file; remember it so we don't refetch.
	bool wrote = writeFile(op->key, bytes.data(), bytes.size());
	if (wrote)
		scheduleSync();
	else
		printf("HttpAssets: failed to cache %s\n", op->key.c_str());
	complete(op, wrote, status);
	bool have = !bytes.empty();
	for (auto &w : memWaiters) {
		if (w.done)
			w.done(have, have ? bytes.data() : nullptr, have ? (uint32_t)bytes.size() : 0, w.user);
	}
}

void
onFetchSuccess(emscripten_fetch_t *fetch)
{
	finishFetch(fetch, true);
}

void
onFetchError(emscripten_fetch_t *fetch)
{
	finishFetch(fetch, false);
}

void
startFetch(FetchOp *op)
{
	// Kept off the URL that was cached while extracts were still wrong.
	std::string url = g_root + op->key + "?v=1";
	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.userData = op;
	attr.onsuccess = onFetchSuccess;
	attr.onerror = onFetchError;
	emscripten_fetch(&attr, url.c_str());
}

void
request(const char *relPath, void (*cb)(bool, void *), void *user)
{
	if (!g_active) {
		if (cb) cb(false, user);
		return;
	}
	std::string key = assetCacheKey(relPath);
	if (unsafePath(key) || !isRemoteMember(key)) {
		if (cb) cb(false, user);
		return;
	}
	if (alreadyCached(relPath, key)) {
		if (cb) cb(true, user);
		return;
	}
	if (g_missing.count(key)) {
		if (cb) cb(false, user);
		return;
	}

	auto it = g_inflight.find(key);
	if (it != g_inflight.end()) {
		it->second->waiters.push_back({ cb, user });
		return;
	}
	auto mem = g_memInflight.find(key);
	if (mem != g_memInflight.end()) {
		mem->second->diskWaiters.push_back({ cb, user });
		return;
	}

	if (!anyInflight()) {
		g_loadDone = 0;
		g_loadTotal = 0;
	}
	g_loadTotal++;
	FetchOp *op = new FetchOp;
	op->key = key;
	op->waiters.push_back({ cb, user });
	g_inflight.emplace(key, op);
	startFetch(op);
	notifyLoadBar();
}

struct WaitState {
	uint32_t remaining;
	bool ok;
	bool done;
};

void
onEnsured(bool ok, void *user)
{
	WaitState *state = (WaitState *)user;
	if (!ok)
		state->ok = false;
	if (--state->remaining == 0)
		state->done = true;
}

struct Piece {
	struct RangeJob *job;
	std::string path;
	uint32_t byteOff;
	uint32_t cap;
};

struct RangeJob {
	void *buffer;
	uint32_t remaining;
	bool arming;
	bool failed;
	void (*done)(bool ok, void *user);
	void *user;
};

void
finishRange(RangeJob *job)
{
	void (*done)(bool, void *) = job->done;
	void *user = job->user;
	bool ok = !job->failed;
	delete job;
	if (done)
		done(ok, user);
}

void
onPiece(bool ok, void *user)
{
	Piece *piece = (Piece *)user;
	RangeJob *job = piece->job;
	if (!ok) {
		job->failed = true;
	} else {
		uint32_t n = HttpAssets::Read(piece->path.c_str(), (uint8_t *)job->buffer + piece->byteOff, piece->cap);
		if (n == 0)
			job->failed = true;
		else if (n + 2048 < piece->cap && piece->path.size() >= 4) {
			const char *ext = piece->path.c_str() + piece->path.size() - 4;
			if (tolower((unsigned char)ext[0]) == '.' &&
			    tolower((unsigned char)ext[1]) == 'c' &&
			    tolower((unsigned char)ext[2]) == 'o' &&
			    tolower((unsigned char)ext[3]) == 'l')
				printf("HttpAssets: %s is %u bytes but the archive entry is %u; re-extract .col files (the first COLL chunk is not the whole file)\n",
				       piece->path.c_str(), n, piece->cap);
		}
	}
	bool last = --job->remaining == 0;
	bool arming = job->arming;
	delete piece;
	if (last && !arming)
		finishRange(job);
}

// True when the file is already local. Otherwise the fetch is started and
// the caller must try again on a later frame.
bool
startIfMissing(const char *relPath)
{
	std::string key = assetCacheKey(relPath);
	if (unsafePath(key) || !isRemoteMember(key))
		return false;
	if (alreadyCached(relPath, key))
		return true;
	if (g_missing.count(key))
		return false;
	request(relPath, nullptr, nullptr);
	return false;
}

} // namespace

void
HttpAssets::Init(const char *envVarName)
{
	if (g_active)
		return;
	const char *env = getenv(envVarName && *envVarName ? envVarName : "ASSETS_URL");
	if (env && *env)
		g_root = env;
	if (!g_root.empty() && g_root.back() != '/')
		g_root.push_back('/');
	g_active = true;
	printf("HttpAssets: fetching missing files from %s\n", g_root.c_str());
}

bool
HttpAssets::IsActive()
{
	return g_active;
}

void *
HttpAssets::Open(const char *path, const char *mode)
{
	FILE *f = fopen(path, mode);
	if (f)
		return f;
	if (!g_active || !mode)
		return nullptr;
	if (!strchr(mode, 'r') || strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'))
		return nullptr;
	if (!Ensure(path))
		return nullptr;
	return fopen(assetCacheKey(path).c_str(), mode);
}

extern bool RsGameIsPlaying();
extern void RsPumpFrameDuringAssetLoad();

static bool
ensureAll(char const *const *relPaths, uint32_t count, bool wait)
{
	if (!g_active)
		return false;
	if (count == 0)
		return true;

	if (!wait) {
		bool ready = true;
		for (uint32_t i = 0; i < count; i++) {
			if (!startIfMissing(relPaths[i]))
				ready = false;
		}
		return ready;
	}

	WaitState state = { count, true, false };
	for (uint32_t i = 0; i < count; i++)
		request(relPaths[i], onEnsured, &state);
	while (!state.done) {
		RsPumpFrameDuringAssetLoad();
		emscripten_sleep(0);
	}
	return state.ok;
}

bool
HttpAssets::Ensure(const char *relPath)
{
	const char *one = relPath;
	// A playing frame must get back to the renderer. The fetch keeps running.
	return ensureAll(&one, 1, !RsGameIsPlaying());
}

bool
HttpAssets::EnsureBlocking(const char *relPath)
{
	const char *one = relPath;
	return ensureAll(&one, 1, true);
}

bool
HttpAssets::EnsureAll(char const *const *relPaths, uint32_t count)
{
	return ensureAll(relPaths, count, !RsGameIsPlaying());
}

uint32_t
HttpAssets::Read(const char *relPath, void *outBuf, uint32_t cap)
{
	if (!outBuf || cap == 0)
		return 0;
	std::string key = assetCacheKey(relPath);
	std::string given = normalizeSlashes(relPath);
	FILE *f = fopen(key.c_str(), "rb");
	if (!f && given != key)
		f = fopen(given.c_str(), "rb");
	if (!f && relPath)
		f = fopen(relPath, "rb");
	if (!f)
		return 0;
	size_t n = fread(outBuf, 1, cap, f);
	fclose(f);
	return (uint32_t)n;
}

bool
HttpAssets::IsLocal(const char *relPath)
{
	std::string path;
	return localFile(relPath, path, nullptr);
}

uint32_t
HttpAssets::LocalSize(const char *relPath)
{
	std::string path;
	uint32_t size = 0;
	if (!localFile(relPath, path, &size))
		return 0;
	return size;
}

void
HttpAssets::RemoveLocal(const char *relPath)
{
	std::string path;
	if (localFile(relPath, path, nullptr))
		remove(path.c_str());
}

void
HttpAssets::FetchToMemory(const char *relPath,
                          void (*done)(bool ok, const uint8_t *data, uint32_t size, void *user),
                          void *user)
{
	if (!done || !relPath) {
		if (done)
			done(false, nullptr, 0, user);
		return;
	}
	if (!g_active) {
		done(false, nullptr, 0, user);
		return;
	}
	std::string key = assetCacheKey(relPath);
	if (unsafePath(key) || !isRemoteMember(key) || g_missing.count(key)) {
		done(false, nullptr, 0, user);
		return;
	}

	auto disk = g_inflight.find(key);
	if (disk != g_inflight.end()) {
		disk->second->memWaiters.push_back({ done, user });
		return;
	}
	auto mem = g_memInflight.find(key);
	if (mem != g_memInflight.end()) {
		mem->second->memWaiters.push_back({ done, user });
		return;
	}

	if (!anyInflight()) {
		g_loadDone = 0;
		g_loadTotal = 0;
	}
	g_loadTotal++;
	MemOp *op = new MemOp;
	op->key = key;
	op->memWaiters.push_back({ done, user });
	g_memInflight.emplace(key, op);

	std::string url = g_root + key + "?v=1";
	emscripten_fetch_attr_t attr;
	emscripten_fetch_attr_init(&attr);
	strcpy(attr.requestMethod, "GET");
	attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	attr.userData = op;
	attr.onsuccess = onMemFetch;
	attr.onerror = onMemFetchError;
	emscripten_fetch(&attr, url.c_str());
	notifyLoadBar();
}

void
HttpAssets::ClearImage(int image)
{
	if (image < 0)
		return;
	if (image < (int)g_images.size())
		g_images[image].clear();
}

void
HttpAssets::RegisterImageFile(int image, uint32_t sector, uint32_t sectorCount, const char *relPath)
{
	if (!g_active || image < 0 || !relPath || sectorCount == 0)
		return;
	if (image >= (int)g_images.size())
		g_images.resize(image + 1);
	Entry e;
	e.sectors = sectorCount;
	e.path = relPath;
	g_images[image][sector] = e;
}

int
HttpAssets::VirtualFd(int imageIndex)
{
	return kVirtualBase + imageIndex;
}

bool
HttpAssets::IsVirtualFd(int fd)
{
	return fd >= kVirtualBase && fd < kVirtualBase + 64;
}

void
HttpAssets::StartImageRead(int fd, uint32_t sector, uint32_t sectorCount, void *buffer,
                           void (*done)(bool ok, void *user), void *user)
{
	int image = fd - kVirtualBase;
	if (!g_active || !buffer || sectorCount == 0 || image < 0 || image >= (int)g_images.size()) {
		if (done) done(false, user);
		return;
	}

	struct PieceInfo {
		std::string path;
		uint32_t byteOff;
		uint32_t cap;
	};
	std::vector<PieceInfo> pieces;
	uint32_t rangeBytes = sectorCount * kSector;
	auto &tab = g_images[image];
	for (auto it = tab.lower_bound(sector); it != tab.end() && it->first < sector + sectorCount; ++it) {
		uint32_t byteOff = (it->first - sector) * kSector;
		uint32_t cap = it->second.sectors * kSector;
		if (byteOff >= rangeBytes)
			continue;
		if (byteOff + cap > rangeBytes)
			cap = rangeBytes - byteOff;
		pieces.push_back({ it->second.path, byteOff, cap });
	}

	if (pieces.empty()) {
		if (done) done(false, user);
		return;
	}

	memset(buffer, 0, rangeBytes);
	RangeJob *job = new RangeJob;
	job->buffer = buffer;
	job->remaining = (uint32_t)pieces.size();
	job->arming = true;
	job->failed = false;
	job->done = done;
	job->user = user;

	for (auto &info : pieces) {
		// Drop a cached copy that cannot be the directory entry the game
		// will read: a later duplicate that overwrote the file, or an
		// extract that trimmed an RW chunk to the size field.
		auto discardStale = [&](const std::string &path) {
			struct stat st;
			if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
				return;
			uint32_t fileSize = (uint32_t)st.st_size;
			bool drop = fileSize > info.cap;
			bool isCol = path.size() >= 4
			          && tolower((unsigned char)path[path.size() - 4]) == '.'
			          && tolower((unsigned char)path[path.size() - 3]) == 'c'
			          && tolower((unsigned char)path[path.size() - 2]) == 'o'
			          && tolower((unsigned char)path[path.size() - 1]) == 'l';
			if (!drop && isCol)
				drop = info.cap > 2048 && fileSize + 2048 < info.cap;
			if (!drop && !isCol) {
				FILE *f = fopen(path.c_str(), "rb");
				if (f) {
					uint8_t hdr[8];
					if (fread(hdr, 1, 8, f) == 8) {
						uint32_t declared = (uint32_t)hdr[4]
						                  | ((uint32_t)hdr[5] << 8)
						                  | ((uint32_t)hdr[6] << 16)
						                  | ((uint32_t)hdr[7] << 24);
						// Size is the payload after the 12-byte chunk header.
						if (declared + 12 > fileSize || declared + 12 > info.cap)
							drop = true;
					}
					fclose(f);
				}
			}
			if (drop) {
				printf("HttpAssets: discarding cached %s (%u bytes, slot %u)\n",
				       path.c_str(), fileSize, info.cap);
				remove(path.c_str());
			}
		};
		std::string key = assetCacheKey(info.path.c_str());
		discardStale(key);
		std::string given = normalizeSlashes(info.path.c_str());
		if (given != key)
			discardStale(given);
		Piece *piece = new Piece;
		piece->job = job;
		piece->path = info.path;
		piece->byteOff = info.byteOff;
		piece->cap = info.cap;
		request(info.path.c_str(), onPiece, piece);
	}
	job->arming = false;
	if (job->remaining == 0)
		finishRange(job);
}
