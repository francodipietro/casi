/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_ROOTS_H
#define CASI_ROOTS_H

#include "casi/buf.h"
#include "casi/config.h"

/*
 * Named roots are how one machine's paths become another's.
 *
 * The problem is not merely that $HOME differs. Real machines lay code out
 * differently: one has ~/src/work/bookit/<repo> nested, another has
 * ~/src/<repo> flattened with a symlink farm. A single prefix substitution
 * cannot express that, so each machine maps its own local paths onto shared
 * logical names:
 *
 *     [root "src"]    path = /Users/fdipietro/src
 *     [root "bookit"] path = /Users/fdipietro/src/bookit
 *
 * The canonical, machine-neutral form is "casi://<root>/<rest>". $HOME is an
 * implicit root named "~" with the lowest priority, so a path under no
 * configured root still travels.
 *
 * Matching is longest-prefix and respects component boundaries: the root
 * "/a/src" matches "/a/src/x" but never "/a/srcfoo".
 */

#define CASI_CANONICAL_SCHEME "casi://"
#define CASI_HOME_ROOT        "~"

typedef struct casi_roots casi_roots;

int  casi_roots_new(casi_roots **out);
void casi_roots_free(casi_roots *roots);

/* Later additions of the same name replace the earlier path. */
int  casi_roots_add(casi_roots *roots, const char *name, const char *local_path);
/* True when `name` can safely be used as one component of a canonical root. */
bool casi_root_name_is_valid(const char *name);
/* Reads every root.<name>.path out of the config, then adds the implicit
 * "~" root from the environment. */
int  casi_roots_load(casi_roots *roots, casi_config *cfg);

size_t      casi_roots_count(const casi_roots *roots);
const char *casi_roots_name_at(const casi_roots *roots, size_t i);
const char *casi_roots_path_at(const casi_roots *roots, size_t i);

/*
 * Translate a single path. normalize() maps a local path to canonical form;
 * denormalize() maps it back for this machine.
 *
 * denormalize() returns CASI_EUNMAPPED when the canonical path names a root
 * this machine has not declared -- never a guess, and never a partial write.
 */
int casi_roots_normalize_path(const casi_roots *roots, const char *local, casi_buf *out);
int casi_roots_denormalize_path(const casi_roots *roots, const char *canonical, casi_buf *out);

/*
 * Translate every path embedded anywhere in a blob of text -- the `cwd` field
 * of each record, and the paths scattered through message bodies and tool
 * results.
 *
 * This is a byte-level substitution over the raw bytes, never a JSON
 * parse-and-reserialise: a round trip through a parser could reorder keys or
 * reshape numbers and break resuming the session. On Unix, paths contain no
 * character JSON escapes, so literal substitution is safe.
 *
 * `unmapped_out`, when non-NULL, is cleared on entry and receives the name of
 * the first canonical root that could not be resolved during denormalisation,
 * so the caller can tell the user exactly which
 * `casi config root.<name>.path` line is missing.
 */
int casi_roots_normalize_text(const casi_roots *roots, const casi_buf *in, casi_buf *out);
int casi_roots_denormalize_text(const casi_roots *roots, const casi_buf *in,
                                casi_buf *out, casi_buf *unmapped_out);

#endif /* CASI_ROOTS_H */
