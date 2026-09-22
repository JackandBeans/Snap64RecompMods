#ifndef __RECOMPDATA_H__
#define __RECOMPDATA_H__

#include "modding.h"

// Collections a mod keeps between calls, held by the port on the host side
// and reached through handles. The same functions, with the same names and
// meanings, as the other N64 recompilations provide, so a mod written
// against their recompdata.h calls the same things here.
//
//   hashmaps  map keys you choose to elements; insert, look up and erase in
//             amortised constant time.
//   hashsets  keep a set of keys.
//   slotmaps  like hashmaps, but the port chooses the keys; faster still.
//
// Each of the map kinds comes in two forms: one holding a 32-bit value per
// element, one holding a block of memory of a fixed size per element,
// allocated in the game's own RAM and freed with the element or the map.
// A handle that was destroyed, or never existed, is a fatal error in the
// mod: the port stops with a message naming the call.

typedef unsigned long collection_key_t;

/////////////////////////
// u32 -> u32 hashmaps //
/////////////////////////

typedef unsigned long U32ValueHashmapHandle;

// A map from u32 keys to u32 values. Returns its handle.
RECOMP_IMPORT("*", U32ValueHashmapHandle recomputil_create_u32_value_hashmap());

// Destroys the map.
RECOMP_IMPORT("*", void recomputil_destroy_u32_value_hashmap(U32ValueHashmapHandle handle));

// 1 if the key is in the map, else 0.
RECOMP_IMPORT("*", int recomputil_u32_value_hashmap_contains(U32ValueHashmapHandle handle, collection_key_t key));

// Sets the key's value, whether or not the key was there. 1 if the key was
// new, else 0.
RECOMP_IMPORT("*", int recomputil_u32_value_hashmap_insert(U32ValueHashmapHandle handle, collection_key_t key, unsigned long value));

// 1 and the value written to *out if the key is in the map; else 0 and *out
// untouched.
RECOMP_IMPORT("*", int recomputil_u32_value_hashmap_get(U32ValueHashmapHandle handle, collection_key_t key, unsigned long* out));

// Removes the key. 1 if it was there, else 0.
RECOMP_IMPORT("*", int recomputil_u32_value_hashmap_erase(U32ValueHashmapHandle handle, collection_key_t key));

// The number of elements.
RECOMP_IMPORT("*", unsigned long recomputil_u32_value_hashmap_size(U32ValueHashmapHandle handle));

//////////////////////////
// u32 -> data hashmaps //
//////////////////////////

typedef unsigned long U32MemoryHashmapHandle;

// A map from u32 keys to blocks of element_size bytes. Returns its handle.
RECOMP_IMPORT("*", U32MemoryHashmapHandle recomputil_create_u32_memory_hashmap(unsigned long element_size));

// Destroys the map and frees every element.
RECOMP_IMPORT("*", void recomputil_destroy_u32_memory_hashmap(U32MemoryHashmapHandle handle));

// 1 if the key is in the map, else 0.
RECOMP_IMPORT("*", int recomputil_u32_memory_hashmap_contains(U32MemoryHashmapHandle handle, collection_key_t key));

// Creates the key's element, zeroed. 1 if it was created, 0 if the key was
// already there (the existing element is kept).
RECOMP_IMPORT("*", int recomputil_u32_memory_hashmap_create(U32MemoryHashmapHandle handle, collection_key_t key));

// The key's element, or NULL when the key is not in the map.
RECOMP_IMPORT("*", void* recomputil_u32_memory_hashmap_get(U32MemoryHashmapHandle handle, collection_key_t key));

// Removes the key and frees its element. 1 if it was there, else 0.
RECOMP_IMPORT("*", int recomputil_u32_memory_hashmap_erase(U32MemoryHashmapHandle handle, collection_key_t key));

// The number of elements.
RECOMP_IMPORT("*", unsigned long recomputil_u32_memory_hashmap_size(U32MemoryHashmapHandle handle));

//////////////////
// u32 hashsets //
//////////////////

typedef unsigned long U32HashsetHandle;

// A set of u32 keys. Returns its handle.
RECOMP_IMPORT("*", U32HashsetHandle recomputil_create_u32_hashset());

// Destroys the set.
RECOMP_IMPORT("*", void recomputil_destroy_u32_hashset(U32HashsetHandle handle));

// 1 if the key is in the set, else 0.
RECOMP_IMPORT("*", int recomputil_u32_hashset_contains(U32HashsetHandle handle, collection_key_t key));

// Adds the key. 1 if it was new, else 0.
RECOMP_IMPORT("*", int recomputil_u32_hashset_insert(U32HashsetHandle handle, collection_key_t key));

// Removes the key. 1 if it was there, else 0.
RECOMP_IMPORT("*", int recomputil_u32_hashset_erase(U32HashsetHandle handle, collection_key_t key));

// The number of keys.
RECOMP_IMPORT("*", unsigned long recomputil_u32_hashset_size(U32HashsetHandle handle));

//////////////////
// u32 slotmaps //
//////////////////

typedef unsigned long U32SlotmapHandle;

// A slotmap of u32 values: the port hands out the keys. Returns its handle.
RECOMP_IMPORT("*", U32SlotmapHandle recomputil_create_u32_slotmap());

// Destroys the slotmap.
RECOMP_IMPORT("*", void recomputil_destroy_u32_slotmap(U32SlotmapHandle handle));

// 1 if the key is in the slotmap, else 0.
RECOMP_IMPORT("*", int recomputil_u32_slotmap_contains(U32SlotmapHandle handle, collection_key_t key));

// A new element; returns its key.
RECOMP_IMPORT("*", collection_key_t recomputil_u32_slotmap_create(U32SlotmapHandle handle));

// 1 and the value written to *out if the key is in the slotmap; else 0 and
// *out untouched.
RECOMP_IMPORT("*", int recomputil_u32_slotmap_get(U32SlotmapHandle handle, collection_key_t key, unsigned long* out));

// Sets the key's value. 1 if the key was there, else 0 and nothing set.
RECOMP_IMPORT("*", int recomputil_u32_slotmap_set(U32SlotmapHandle handle, collection_key_t key, unsigned long value));

// Removes the key. 1 if it was there, else 0.
RECOMP_IMPORT("*", int recomputil_u32_slotmap_erase(U32SlotmapHandle handle, collection_key_t key));

// The number of elements.
RECOMP_IMPORT("*", unsigned long recomputil_u32_slotmap_size(U32SlotmapHandle handle));

///////////////////
// data slotmaps //
///////////////////

typedef unsigned long MemorySlotmapHandle;

// A slotmap of blocks of element_size bytes. Returns its handle.
RECOMP_IMPORT("*", MemorySlotmapHandle recomputil_create_memory_slotmap(unsigned long element_size));

// Destroys the slotmap and frees every element.
RECOMP_IMPORT("*", void recomputil_destroy_memory_slotmap(MemorySlotmapHandle handle));

// 1 if the key is in the slotmap, else 0.
RECOMP_IMPORT("*", int recomputil_memory_slotmap_contains(MemorySlotmapHandle handle, collection_key_t key));

// A new element, zeroed; returns its key.
RECOMP_IMPORT("*", collection_key_t recomputil_memory_slotmap_create(MemorySlotmapHandle handle));

// 1 and the element's pointer written to *out if the key is in the slotmap;
// else 0 and *out untouched.
RECOMP_IMPORT("*", int recomputil_memory_slotmap_get(MemorySlotmapHandle handle, collection_key_t key, void** out));

// Removes the key and frees its element. 1 if it was there, else 0.
RECOMP_IMPORT("*", int recomputil_memory_slotmap_erase(MemorySlotmapHandle handle, collection_key_t key));

// The number of elements.
RECOMP_IMPORT("*", unsigned long recomputil_memory_slotmap_size(MemorySlotmapHandle handle));

#endif
