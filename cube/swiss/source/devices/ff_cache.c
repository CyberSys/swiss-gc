/*
 cache.c
 The cache is not visible to the user. It should be flushed
 when any file is closed or changes are made to the filesystem.

 This cache implements a least-used-page replacement policy. This will
 distribute sectors evenly over the pages, so if less than the maximum
 pages are used at once, they should all eventually remain in the cache.
 This also has the benefit of throwing out old sectors, so as not to keep
 too many stale pages around.

 Copyright (c) 2006 Michael "Chishm" Chisholm

 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation and/or
     other materials provided with the distribution.
  3. The name of the author may not be used to endorse or promote products derived
     from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE
 LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <string.h>
#include <limits.h>

#include "ff_cache/common.h"
#include "ff_cache/cache.h"
#include "ff_cache/disc.h"

#include "ff_cache/mem_allocate.h"
#include "ff_cache/bit_ops.h"

#define CACHE_FREE ((sec_t)-1)

CACHE* _FAT_cache_constructor (unsigned int numberOfPages, unsigned int sectorsPerPage, const DISC_INTERFACE* discInterface, sec_t endOfPartition, unsigned int bytesPerSector) {
	CACHE* cache;
	unsigned int i;
	CACHE_ENTRY* cacheEntries;

	if (numberOfPages < 2) {
		numberOfPages = 2;
	}

	if (sectorsPerPage < 8) {
		sectorsPerPage = 8;
	} else if (sectorsPerPage > 64) {
		sectorsPerPage = 64;
	}

	cache = (CACHE*) _FAT_mem_allocate (sizeof(CACHE));
	if (cache == NULL) {
		return NULL;
	}

	cache->disc = discInterface;
	cache->endOfPartition = endOfPartition;
	cache->numberOfPages = numberOfPages;
	cache->sectorsPerPage = sectorsPerPage;
	cache->bytesPerSector = bytesPerSector;


	cacheEntries = (CACHE_ENTRY*) _FAT_mem_allocate ( sizeof(CACHE_ENTRY) * numberOfPages);
	if (cacheEntries == NULL) {
		_FAT_mem_free (cache);
		return NULL;
	}

	for (i = 0; i < numberOfPages; i++) {
		cacheEntries[i].sector = CACHE_FREE;
		cacheEntries[i].count = 0;
		cacheEntries[i].last_access = 0;
		cacheEntries[i].dirty = 0;
		cacheEntries[i].cache = (uint8_t*) _FAT_mem_align ( sectorsPerPage * bytesPerSector );
	}

	cache->cacheEntries = cacheEntries;

	return cache;
}

void _FAT_cache_destructor (CACHE* cache) {
	unsigned int i;
	// Clear out cache before destroying it
	_FAT_cache_flush(cache);

	// Free memory in reverse allocation order
	for (i = 0; i < cache->numberOfPages; i++) {
		_FAT_mem_free (cache->cacheEntries[i].cache);
	}
	_FAT_mem_free (cache->cacheEntries);
	_FAT_mem_free (cache);
}


static u32 accessCounter = 0;

static u32 accessTime(){
	accessCounter++;
	return accessCounter;
}


static CACHE_ENTRY* _FAT_cache_getPage(CACHE *cache,sec_t sector,sec_t numSectors,bool write)
{
	unsigned int i;
	CACHE_ENTRY* cacheEntries = cache->cacheEntries;
	unsigned int numberOfPages = cache->numberOfPages;
	unsigned int sectorsPerPage = cache->sectorsPerPage;

	bool foundFree = false;
	unsigned int oldUsed = 0;
	unsigned int oldAccess = UINT_MAX;

	for(i=0;i<numberOfPages;i++) {
		if(sector>=cacheEntries[i].sector && sector<(cacheEntries[i].sector + cacheEntries[i].count)) {
			cacheEntries[i].last_access = accessTime();
			return &(cacheEntries[i]);
		}

		if(foundFree==false && (cacheEntries[i].sector==CACHE_FREE || cacheEntries[i].last_access<oldAccess)) {
			if(cacheEntries[i].sector==CACHE_FREE) foundFree = true;
			oldUsed = i;
			oldAccess = cacheEntries[i].last_access;
		}
	}

	if(foundFree==false && cacheEntries[oldUsed].dirty!=0) {
		sec_t sec = ffsll(cacheEntries[oldUsed].dirty)-1;
		sec_t secs_to_write = flsll(cacheEntries[oldUsed].dirty)-sec;

		if(!_FAT_disc_writeSectors(cache->disc,cacheEntries[oldUsed].sector+sec,secs_to_write,cacheEntries[oldUsed].cache+(sec*cache->bytesPerSector))) return NULL;

		cacheEntries[oldUsed].dirty = 0;
	}

	cacheEntries[oldUsed].sector = (sector/sectorsPerPage)*sectorsPerPage; // align base sector to page size
	sector -= cacheEntries[oldUsed].sector;
	cacheEntries[oldUsed].count = cache->endOfPartition - cacheEntries[oldUsed].sector;
	if(cacheEntries[oldUsed].count > sectorsPerPage) cacheEntries[oldUsed].count = sectorsPerPage;
	else sectorsPerPage = cacheEntries[oldUsed].count;
	if(numSectors > sectorsPerPage - sector) numSectors = sectorsPerPage - sector;

	sec_t sec = 0;
	sec_t secs_to_read = sectorsPerPage;

	if(write) {
		if (sector == sec && numSectors == secs_to_read) {
			cacheEntries[oldUsed].last_access = accessTime();
			return &(cacheEntries[oldUsed]);
		} else if (sector == sec) {
			sec += numSectors;
			secs_to_read -= numSectors;
		} else if (sector + numSectors == sec + secs_to_read) {
			secs_to_read -= numSectors;
		}
	}

	if(!_FAT_disc_readSectors(cache->disc,cacheEntries[oldUsed].sector+sec,secs_to_read,cacheEntries[oldUsed].cache+(sec*cache->bytesPerSector))) {
		cacheEntries[oldUsed].sector = CACHE_FREE;
		cacheEntries[oldUsed].count = 0;
		cacheEntries[oldUsed].last_access = 0;
		cacheEntries[oldUsed].dirty = 0;
		return NULL;
	}

	cacheEntries[oldUsed].last_access = accessTime();
	return &(cacheEntries[oldUsed]);
}

static CACHE_ENTRY* _FAT_cache_findPage(CACHE *cache,sec_t sector,sec_t count)
{
	unsigned int i;
	CACHE_ENTRY* cacheEntries = cache->cacheEntries;
	unsigned int numberOfPages = cache->numberOfPages;
	CACHE_ENTRY* entry = NULL;
	sec_t lowest = CACHE_FREE;

	for(i=0;i<numberOfPages;i++) {
		if (cacheEntries[i].sector != CACHE_FREE) {
			bool intersect;
			if (sector > cacheEntries[i].sector) {
				intersect = sector - cacheEntries[i].sector < cacheEntries[i].count;
			} else {
				intersect = cacheEntries[i].sector - sector < count;
			}

			if (intersect && (cacheEntries[i].sector < lowest)) {
				lowest = cacheEntries[i].sector;
				entry = &cacheEntries[i];
			}
		}
	}

	return entry;
}

bool _FAT_cache_readSectors(CACHE *cache,sec_t sector,sec_t numSectors,void *buffer)
{
	sec_t sec;
	sec_t secs_to_read;
	CACHE_ENTRY *entry;
	uint8_t *dest = (uint8_t *)buffer;

	while(numSectors>0) {
		if(((uintptr_t)dest%32)==0 && (sector%cache->sectorsPerPage)==0) {
			entry = _FAT_cache_findPage(cache,sector,numSectors);
			if(entry==NULL) {
				secs_to_read = (numSectors/cache->sectorsPerPage)*cache->sectorsPerPage;
			} else if (entry->sector > sector) {
				secs_to_read = entry->sector - sector;
			} else {
				secs_to_read = 0;
			}

			if(secs_to_read>0) {
				if(!_FAT_disc_readSectors(cache->disc,sector,secs_to_read,dest)) return false;

				dest += (secs_to_read*cache->bytesPerSector);
				sector += secs_to_read;
				numSectors -= secs_to_read;
				continue;
			}
		}

		entry = _FAT_cache_getPage(cache,sector,numSectors,false);
		if(entry==NULL) return false;

		sec = sector - entry->sector;
		secs_to_read = entry->count - sec;
		if(secs_to_read>numSectors) secs_to_read = numSectors;

		memcpy(dest,entry->cache + (sec*cache->bytesPerSector),(secs_to_read*cache->bytesPerSector));

		dest += (secs_to_read*cache->bytesPerSector);
		sector += secs_to_read;
		numSectors -= secs_to_read;
	}

	return true;
}

/*
Reads some data from a cache page, determined by the sector number
*/
bool _FAT_cache_readPartialSector (CACHE* cache, void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector,1,false);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memcpy(buffer,entry->cache + ((sec*cache->bytesPerSector) + offset),size);

	return true;
}

bool _FAT_cache_readLittleEndianValue (CACHE* cache, uint32_t *value, sec_t sector, unsigned int offset, int num_bytes) {
  uint8_t buf[4];
  if (!_FAT_cache_readPartialSector(cache, buf, sector, offset, num_bytes)) return false;

  switch(num_bytes) {
  case 1: *value = buf[0]; break;
  case 2: *value = u8array_to_u16(buf,0); break;
  case 4: *value = u8array_to_u32(buf,0); break;
  default: return false;
  }
  return true;
}

/*
Writes some data to a cache page, making sure it is loaded into memory first.
*/
bool _FAT_cache_writePartialSector (CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector,1,false);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memcpy(entry->cache + ((sec*cache->bytesPerSector) + offset),buffer,size);

	entry->dirty |= 1ULL << sec;
	return true;
}

bool _FAT_cache_writeLittleEndianValue (CACHE* cache, const uint32_t value, sec_t sector, unsigned int offset, int size) {
  uint8_t buf[4] = {0, 0, 0, 0};

  switch(size) {
  case 1: buf[0] = value; break;
  case 2: u16_to_u8array(buf, 0, value); break;
  case 4: u32_to_u8array(buf, 0, value); break;
  default: return false;
  }

  return _FAT_cache_writePartialSector(cache, buf, sector, offset, size);
}

/*
Writes some data to a cache page, zeroing out the page first
*/
bool _FAT_cache_eraseWritePartialSector (CACHE* cache, const void* buffer, sec_t sector, unsigned int offset, size_t size)
{
	sec_t sec;
	CACHE_ENTRY *entry;

	if (offset + size > cache->bytesPerSector) return false;

	entry = _FAT_cache_getPage(cache,sector,1,true);
	if(entry==NULL) return false;

	sec = sector - entry->sector;
	memset(entry->cache + (sec*cache->bytesPerSector),0,cache->bytesPerSector);
	memcpy(entry->cache + ((sec*cache->bytesPerSector) + offset),buffer,size);

	entry->dirty |= 1ULL << sec;
	return true;
}


bool _FAT_cache_writeSectors (CACHE* cache, sec_t sector, sec_t numSectors, const void* buffer)
{
	sec_t sec;
	sec_t secs_to_write;
	CACHE_ENTRY *entry;
	const uint8_t *src = (const uint8_t *)buffer;

	while(numSectors>0) {
		if(((uintptr_t)src%32)==0 && (sector%cache->sectorsPerPage)==0) {
			entry = _FAT_cache_findPage(cache,sector,numSectors);
			if(entry==NULL) {
				secs_to_write = (numSectors/cache->sectorsPerPage)*cache->sectorsPerPage;
			} else if (entry->sector > sector) {
				secs_to_write = entry->sector - sector;
			} else {
				secs_to_write = 0;
			}

			if(secs_to_write>0) {
				if(!_FAT_disc_writeSectors(cache->disc,sector,secs_to_write,src)) return false;

				src += (secs_to_write*cache->bytesPerSector);
				sector += secs_to_write;
				numSectors -= secs_to_write;
				continue;
			}
		}

		entry = _FAT_cache_getPage(cache,sector,numSectors,true);
		if(entry==NULL) return false;

		sec = sector - entry->sector;
		secs_to_write = entry->count - sec;
		if(secs_to_write>numSectors) secs_to_write = numSectors;

		memcpy(entry->cache + (sec*cache->bytesPerSector),src,(secs_to_write*cache->bytesPerSector));

		src += (secs_to_write*cache->bytesPerSector);
		sector += secs_to_write;
		numSectors -= secs_to_write;

		entry->dirty |= ((1ULL << secs_to_write)-1) << sec;
	}

	return true;
}

/*
Flushes all dirty pages to disc, clearing the dirty flag.
*/
bool _FAT_cache_flush (CACHE* cache) {
	sec_t sec;
	sec_t secs_to_write;
	CACHE_ENTRY *entry;
	unsigned int i;

	for (i = 0; i < cache->numberOfPages; i++) {
		entry = &cache->cacheEntries[i];

		if (entry->dirty) {
			sec = ffsll(entry->dirty) - 1;
			secs_to_write = flsll(entry->dirty) - sec;

			if (!_FAT_disc_writeSectors(cache->disc, entry->sector + sec, secs_to_write, entry->cache + (sec * cache->bytesPerSector))) return false;

			entry->dirty = 0;
		}
	}

	return true;
}

void _FAT_cache_invalidate (CACHE* cache) {
	unsigned int i;
	_FAT_cache_flush(cache);
	for (i = 0; i < cache->numberOfPages; i++) {
		cache->cacheEntries[i].sector = CACHE_FREE;
		cache->cacheEntries[i].count = 0;
		cache->cacheEntries[i].last_access = 0;
		cache->cacheEntries[i].dirty = 0;
	}
}
