// SPDX-License-Identifier: AGPL-3.0-only
// Modified by simplyyjessie, 2026-10-03 (Linux port). See NOTICE.md

#include "Hooking.h"

std::map<std::string, IHook *> IHook::hooks;

bool IHook::Exists(const std::string &name)
{
	return hooks.find(name) != hooks.end();
}

void IHook::Register(IHook *hook)
{
	hooks[hook->name] = hook;
}

void IHook::Unregister(IHook *hook)
{
	hooks.erase(hook->name);
}

void IHook::DestroyAll()
{
	for (auto &hook : hooks)
	{
		hook.second->Destroy();
	}
	hooks.clear();
}


#ifndef _WIN32

#include <sys/mman.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>

// Protection flags of the mapping that contains addr, read from
// /proc/self/maps. Vtables normally sit in read-only RELRO pages, but this
// avoids assuming it: restoring the wrong flags could break a writable page.
static bool MappingProtection(uintptr_t addr, int &prot)
{
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return false;

	bool found = false;
	char line[512];
	while (fgets(line, sizeof line, maps))
	{
		uintptr_t start, end;
		char perms[5] = {};
		if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &start, &end, perms) != 3)
			continue;
		if (addr >= start && addr < end)
		{
			prot = (perms[0] == 'r' ? PROT_READ : 0)
				| (perms[1] == 'w' ? PROT_WRITE : 0)
				| (perms[2] == 'x' ? PROT_EXEC : 0);
			found = true;
			break;
		}
	}
	fclose(maps);
	return found;
}

bool WriteVTableSlot(void **slot, void *value)
{
	if (!slot)
		return false;

	uintptr_t addr = reinterpret_cast<uintptr_t>(slot);
	int prot = 0;
	if (!MappingProtection(addr, prot))
	{
		LOG("No mapping found for vtable slot %p", (void *)slot);
		return false;
	}

	// A pointer-aligned slot never straddles a page boundary.
	uintptr_t pageSize = (uintptr_t)sysconf(_SC_PAGESIZE);
	void *page = reinterpret_cast<void *>(addr & ~(pageSize - 1));

	if (!(prot & PROT_WRITE) && mprotect(page, pageSize, prot | PROT_WRITE) != 0)
	{
		LOG("mprotect failed for vtable slot %p", (void *)slot);
		return false;
	}

	// Other threads may be calling through this slot right now; make the
	// pointer swap a single atomic store.
	__atomic_store_n(slot, value, __ATOMIC_SEQ_CST);

	if (!(prot & PROT_WRITE))
		mprotect(page, pageSize, prot);
	return true;
}

#endif
