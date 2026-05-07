// linker.h
#pragma once

#include "dvm_fmt.h"

// ─── Linker API ──────────────────────────────────────────────────────────────

int dvm_link(
    const DvmProg *objs,
    size_t nobjs,
    DvmProg *out,
    const char *entry_sym
);

int dvm_link_files(
    const char **obj_paths,
    size_t nobjs,
    const char *out_path,
    const char *entry_sym
);
