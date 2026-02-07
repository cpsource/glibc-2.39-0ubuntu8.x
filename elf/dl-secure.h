/* Secure loader module for ld.so — header.
   Copyright (C) 2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <https://www.gnu.org/licenses/>.  */

#ifndef _DL_SECURE_H
#define _DL_SECURE_H

#include <elf.h>
#include <stdint.h>
#include <stddef.h>

/* Policy enforcement modes.  */
enum dl_secure_mode
{
  DL_SECURE_OFF     = 0,   /* No enforcement.  */
  DL_SECURE_AUDIT   = 1,   /* Log violations but allow.  */
  DL_SECURE_ENFORCE = 2    /* Block violations.  */
};

/* Maximum number of policy rules.  */
#define DL_SECURE_MAX_RULES 256

/* Maximum number of hash whitelist entries.  */
#define DL_SECURE_MAX_HASHES 256

/* SHA-256 digest size in bytes.  */
#define DL_SECURE_HASH_SIZE 32

/* HMAC key size in bytes.  */
#define DL_SECURE_KEY_SIZE 32

/* ELF note name for embedded signatures.  */
#define DL_SECURE_NOTE_NAME "DL-Secure"

/* ELF note type for HMAC-SHA256 signature.  */
#define DL_SECURE_NOTE_TYPE 0x53454300  /* "SEC\0" */

/* Rule types for path matching.  */
enum dl_secure_rule_type
{
  DL_SECURE_ALLOW_PATH  = 0,
  DL_SECURE_DENY_PATH   = 1,
  DL_SECURE_REQUIRE_SIG = 2
};

/* A single path-matching rule.  */
struct dl_secure_rule
{
  enum dl_secure_rule_type type;
  const char *pattern;          /* Glob pattern (points into mmap'd config).  */
};

/* A SHA-256 hash whitelist entry.  */
struct dl_secure_hash_entry
{
  uint8_t hash[DL_SECURE_HASH_SIZE];
  const char *path;             /* Optional path hint (points into config).  */
};

/* Global policy state.  Stored in ld.so BSS.  */
struct dl_secure_policy
{
  enum dl_secure_mode mode;
  int initialized;

  /* HMAC key for signature verification.  */
  uint8_t hmac_key[DL_SECURE_KEY_SIZE];
  int has_hmac_key;

  /* Path-based rules (processed in order, deny takes precedence).  */
  struct dl_secure_rule rules[DL_SECURE_MAX_RULES];
  int nrules;

  /* SHA-256 hash whitelist.  */
  struct dl_secure_hash_entry hashes[DL_SECURE_MAX_HASHES];
  int nhashes;

  /* Pointer to mmap'd config file (kept alive for string references).  */
  void *config_data;
  size_t config_size;
};

/* Initialize the secure loader policy by parsing /etc/ld.so.secure.
   Called once from dl_main() after ld.so.preload handling.  */
void _dl_secure_init (void);

/* Check whether a shared library file is allowed to be loaded.
   NAME is the file path, FD is an open file descriptor.
   Returns 0 if allowed, -1 if denied.  */
int _dl_secure_check_file (const char *name, int fd);

/* Check whether the main executable is allowed to run.
   M is the link_map for the main executable, PROGRAM is argv[0].
   Called from dl_main() after _rtld_main_check().  */
void _dl_secure_check_main (struct link_map *m, const char *program);

#endif /* _DL_SECURE_H */
