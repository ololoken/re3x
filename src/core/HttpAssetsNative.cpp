#include "HttpAssets.h"

#include <stdio.h>

void
HttpAssets::Init(const char *)
{
}

bool
HttpAssets::IsActive()
{
	return false;
}

void *
HttpAssets::Open(const char *path, const char *mode)
{
	return fopen(path, mode);
}

bool
HttpAssets::Ensure(const char *)
{
	return false;
}

bool
HttpAssets::EnsureBlocking(const char *)
{
	return false;
}

bool
HttpAssets::EnsureAll(char const *const *, uint32_t)
{
	return false;
}

uint32_t
HttpAssets::Read(const char *, void *, uint32_t)
{
	return 0;
}

bool
HttpAssets::IsLocal(const char *)
{
	return false;
}

uint32_t
HttpAssets::LocalSize(const char *)
{
	return 0;
}

void
HttpAssets::RemoveLocal(const char *)
{
}

void
HttpAssets::FetchToMemory(const char *, void (*done)(bool, const uint8_t *, uint32_t, void *), void *user)
{
	if (done)
		done(false, nullptr, 0, user);
}

void
HttpAssets::ClearImage(int)
{
}

void
HttpAssets::RegisterImageFile(int, uint32_t, uint32_t, const char *)
{
}

int
HttpAssets::VirtualFd(int)
{
	return -1;
}

bool
HttpAssets::IsVirtualFd(int)
{
	return false;
}

void
HttpAssets::StartImageRead(int, uint32_t, uint32_t, void *, void (*done)(bool, void *), void *user)
{
	if (done)
		done(false, user);
}
