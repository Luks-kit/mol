// dvm_io.h
#pragma once

#include "dvm_fmt.h"

// ─── File I/O ────────────────────────────────────────────────────────────────

// Read a .dvm object/executable from disk.
int dvm_read_file(const char *path, DvmProg *out);

// Write a .dvm object/executable to disk.
int dvm_write_file(const char *path, const DvmProg *prog);

// Load executable sections into runtime memory.
int dvm_load(const DvmProg *prog, DvmLoaded *out);

// Free memory allocated by dvm_load.
void dvm_unload(DvmLoaded *img);
