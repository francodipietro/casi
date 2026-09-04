/* SPDX-License-Identifier: AGPL-3.0-only */
#ifndef CASI_ENCODING_H
#define CASI_ENCODING_H

#include "casi/buf.h"

/*
 * Claude Code names each directory under ~/.claude/projects/ after the
 * session's startup working directory, replacing every non-alphanumeric
 * character with '-'.
 *
 * Verified empirically against Claude Code 2.1.258 rather than assumed: the
 * 40 project paths on the development machine could not distinguish this from
 * the narrower "only '/' and '_'" rule, so a directory named
 * ".../scratchpad/casi.probe.v2" was created and Claude Code encoded it as
 * "...-scratchpad-casi-probe-v2". Dots become dashes; existing dashes survive
 * as themselves.
 *
 * The mapping is lossy and NOT injective -- "a/b" and "a-b" and "a_b" all
 * produce "a-b" -- so casi never inverts it. The real path is carried in
 * session metadata and re-encoded for whichever machine is materialising.
 *
 * Replacement is per Unicode code point, not per byte, because Claude Code
 * applies a JavaScript regex over UTF-16 code units: "diseño" must yield one
 * dash for the "ñ", not the two its UTF-8 encoding would give. Characters
 * outside the BMP are the one remaining divergence (JavaScript sees a
 * surrogate pair and emits two dashes); vanishingly rare in a project path,
 * and casi_doctor is where a mismatch would surface.
 */
int casi_encode_project_dir(const char *path, casi_buf *out);

#endif /* CASI_ENCODING_H */
