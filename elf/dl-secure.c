/* Secure loader module for ld.so — policy engine.
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

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ldsodefs.h>
#include <not-cancel.h>
#include <dl-secure.h>
#include <dl-sha256.h>

/* Global policy state.  */
static struct dl_secure_policy _dl_secure_policy;

/* Config file path — hardcoded, not overridable by environment.  */
static const char _dl_secure_config_path[] = "/etc/ld.so.secure";

/* --- Utility helpers --------------------------------------------------- */

/* Simple glob matching supporting only '*' as wildcard (matches any
   sequence of characters including none).  This is intentionally
   minimal — no '?' or bracket expressions.  */
static bool
_dl_secure_glob_match (const char *pattern, const char *string)
{
  while (*pattern != '\0')
    {
      if (*pattern == '*')
	{
	  /* Skip consecutive '*'s.  */
	  while (*pattern == '*')
	    pattern++;
	  if (*pattern == '\0')
	    return true;
	  /* Try matching the rest of the pattern at each position.  */
	  while (*string != '\0')
	    {
	      if (_dl_secure_glob_match (pattern, string))
		return true;
	      string++;
	    }
	  return _dl_secure_glob_match (pattern, string);
	}
      if (*string == '\0' || *pattern != *string)
	return false;
      pattern++;
      string++;
    }
  return *string == '\0';
}

/* Parse a hex character.  Returns -1 on invalid input.  */
static int
_dl_secure_hex_digit (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Parse a hex string of exactly 2*len characters into buf.
   Returns true on success.  */
static bool
_dl_secure_parse_hex (const char *hex, uint8_t *buf, size_t len)
{
  for (size_t i = 0; i < len; i++)
    {
      int hi = _dl_secure_hex_digit (hex[i * 2]);
      int lo = _dl_secure_hex_digit (hex[i * 2 + 1]);
      if (hi < 0 || lo < 0)
	return false;
      buf[i] = (uint8_t) ((hi << 4) | lo);
    }
  return true;
}

/* Skip leading whitespace.  */
static char *
_dl_secure_skip_ws (char *p)
{
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/* Find the next whitespace or NUL.  */
static char *
_dl_secure_find_ws (char *p)
{
  while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n')
    p++;
  return p;
}

/* --- Config file parser ------------------------------------------------ */

/* Parse /etc/ld.so.secure and populate the global policy.
   File format:
     mode enforce|audit|off
     hmac-key <64 hex chars>
     allow-path <glob>
     deny-path <glob>
     allow-hash sha256:<64 hex chars> [<path>]
     require-sig <glob>
   Lines starting with '#' are comments.  */
static void
_dl_secure_parse_config (char *data, size_t size)
{
  struct dl_secure_policy *pol = &_dl_secure_policy;
  char *p = data;
  char *end = data + size;

  while (p < end)
    {
      /* Find end of line.  */
      char *eol = memchr (p, '\n', end - p);
      if (eol == NULL)
	eol = end;
      /* Temporarily NUL-terminate the line.  */
      if (eol < end)
	*eol = '\0';

      p = _dl_secure_skip_ws (p);

      /* Skip empty lines and comments.  */
      if (*p == '\0' || *p == '#')
	{
	  p = eol + 1;
	  continue;
	}

      /* Parse directive.  */
      char *directive = p;
      p = _dl_secure_find_ws (p);
      if (*p != '\0')
	*p++ = '\0';
      p = _dl_secure_skip_ws (p);

      if (strcmp (directive, "mode") == 0)
	{
	  char *value = p;
	  _dl_secure_find_ws (value);  /* Just to find end.  */
	  /* NUL-terminate the value (it's at eol anyway).  */
	  char *ve = _dl_secure_find_ws (value);
	  *ve = '\0';

	  if (strcmp (value, "enforce") == 0)
	    pol->mode = DL_SECURE_ENFORCE;
	  else if (strcmp (value, "audit") == 0)
	    pol->mode = DL_SECURE_AUDIT;
	  else if (strcmp (value, "off") == 0)
	    pol->mode = DL_SECURE_OFF;
	}
      else if (strcmp (directive, "hmac-key") == 0)
	{
	  char *hex = p;
	  if (_dl_secure_parse_hex (hex, pol->hmac_key,
				    DL_SECURE_KEY_SIZE))
	    pol->has_hmac_key = 1;
	}
      else if (strcmp (directive, "allow-path") == 0
	       && pol->nrules < DL_SECURE_MAX_RULES)
	{
	  char *pattern = p;
	  char *pe = _dl_secure_find_ws (pattern);
	  *pe = '\0';
	  pol->rules[pol->nrules].type = DL_SECURE_ALLOW_PATH;
	  pol->rules[pol->nrules].pattern = pattern;
	  pol->nrules++;
	}
      else if (strcmp (directive, "deny-path") == 0
	       && pol->nrules < DL_SECURE_MAX_RULES)
	{
	  char *pattern = p;
	  char *pe = _dl_secure_find_ws (pattern);
	  *pe = '\0';
	  pol->rules[pol->nrules].type = DL_SECURE_DENY_PATH;
	  pol->rules[pol->nrules].pattern = pattern;
	  pol->nrules++;
	}
      else if (strcmp (directive, "require-sig") == 0
	       && pol->nrules < DL_SECURE_MAX_RULES)
	{
	  char *pattern = p;
	  char *pe = _dl_secure_find_ws (pattern);
	  *pe = '\0';
	  pol->rules[pol->nrules].type = DL_SECURE_REQUIRE_SIG;
	  pol->rules[pol->nrules].pattern = pattern;
	  pol->nrules++;
	}
      else if (strcmp (directive, "allow-hash") == 0
	       && pol->nhashes < DL_SECURE_MAX_HASHES)
	{
	  /* Format: sha256:<64 hex chars> [<path>] */
	  if (strncmp (p, "sha256:", 7) == 0)
	    {
	      p += 7;
	      struct dl_secure_hash_entry *ent =
		&pol->hashes[pol->nhashes];
	      if (_dl_secure_parse_hex (p, ent->hash,
					DL_SECURE_HASH_SIZE))
		{
		  p += DL_SECURE_HASH_SIZE * 2;
		  p = _dl_secure_skip_ws (p);
		  if (*p != '\0')
		    {
		      char *pe = _dl_secure_find_ws (p);
		      *pe = '\0';
		      ent->path = p;
		    }
		  else
		    ent->path = NULL;
		  pol->nhashes++;
		}
	    }
	}

      p = eol + 1;
    }
}

/* --- SHA-256 hash computation over ELF PT_LOAD segments ---------------- */

/* Compute SHA-256 hash over all PT_LOAD segments of an ELF file.
   This survives strip operations since we only hash loadable content.
   Returns true on success, with the digest written to HASH.  */
static bool
_dl_secure_hash_elf (int fd, uint8_t hash[DL_SECURE_HASH_SIZE])
{
  struct sha256_ctx ctx;
  sha256_init (&ctx);

  /* Read the ELF header.  */
  ElfW(Ehdr) ehdr;
  if (__pread64_nocancel (fd, &ehdr, sizeof (ehdr), 0)
      != (ssize_t) sizeof (ehdr))
    return false;

  /* Read program headers.  */
  size_t phdr_size = ehdr.e_phnum * sizeof (ElfW(Phdr));
  ElfW(Phdr) phdr_buf[64];
  ElfW(Phdr) *phdr;

  if (ehdr.e_phnum > 64)
    return false;

  phdr = phdr_buf;
  if (__pread64_nocancel (fd, phdr, phdr_size, ehdr.e_phoff)
      != (ssize_t) phdr_size)
    return false;

  /* Hash the content of each PT_LOAD segment.  */
  uint8_t read_buf[4096];
  for (unsigned int i = 0; i < ehdr.e_phnum; i++)
    {
      if (phdr[i].p_type != PT_LOAD)
	continue;

      ElfW(Off) offset = phdr[i].p_offset;
      size_t remaining = phdr[i].p_filesz;

      while (remaining > 0)
	{
	  size_t to_read = remaining < sizeof (read_buf)
			   ? remaining : sizeof (read_buf);
	  ssize_t got = __pread64_nocancel (fd, read_buf, to_read, offset);
	  if (got <= 0)
	    return false;
	  sha256_update (&ctx, read_buf, (size_t) got);
	  offset += got;
	  remaining -= (size_t) got;
	}
    }

  sha256_final (&ctx, hash);
  return true;
}

/* --- HMAC-SHA256 -------------------------------------------------------- */

/* Compute HMAC-SHA256(key, data).  */
static void
_dl_secure_hmac_sha256 (const uint8_t key[DL_SECURE_KEY_SIZE],
			const void *data, size_t data_len,
			uint8_t mac[DL_SECURE_HASH_SIZE])
{
  uint8_t ipad[SHA256_BLOCK_SIZE];
  uint8_t opad[SHA256_BLOCK_SIZE];
  struct sha256_ctx ctx;
  uint8_t inner_hash[DL_SECURE_HASH_SIZE];

  /* Key is exactly 32 bytes, which is less than block size (64).
     Pad with zeros.  */
  memset (ipad, 0x36, SHA256_BLOCK_SIZE);
  memset (opad, 0x5c, SHA256_BLOCK_SIZE);
  for (int i = 0; i < DL_SECURE_KEY_SIZE; i++)
    {
      ipad[i] ^= key[i];
      opad[i] ^= key[i];
    }

  /* Inner hash: H(ipad || data).  */
  sha256_init (&ctx);
  sha256_update (&ctx, ipad, SHA256_BLOCK_SIZE);
  sha256_update (&ctx, data, data_len);
  sha256_final (&ctx, inner_hash);

  /* Outer hash: H(opad || inner_hash).  */
  sha256_init (&ctx);
  sha256_update (&ctx, opad, SHA256_BLOCK_SIZE);
  sha256_update (&ctx, inner_hash, DL_SECURE_HASH_SIZE);
  sha256_final (&ctx, mac);
}

/* --- .note.dl-secure signature verification ----------------------------- */

/* Search for a .note.dl-secure note in the ELF file and verify its
   HMAC-SHA256 signature against the PT_LOAD segments.
   Returns true if a valid signature is found.  */
static bool
_dl_secure_verify_signature (int fd, const uint8_t key[DL_SECURE_KEY_SIZE])
{
  /* Read ELF header.  */
  ElfW(Ehdr) ehdr;
  if (__pread64_nocancel (fd, &ehdr, sizeof (ehdr), 0)
      != (ssize_t) sizeof (ehdr))
    return false;

  /* Read section headers to find .note.dl-secure.  */
  if (ehdr.e_shnum == 0 || ehdr.e_shentsize != sizeof (ElfW(Shdr)))
    return false;

  size_t shdr_size = ehdr.e_shnum * sizeof (ElfW(Shdr));
  /* Limit to reasonable number of sections.  */
  if (ehdr.e_shnum > 1024)
    return false;

  /* We need to use alloca or a stack buffer.  For safety, use a
     reasonable stack buffer and reject files with too many sections.  */
  ElfW(Shdr) shdr_buf[128];
  ElfW(Shdr) *shdrs;

  if (ehdr.e_shnum > 128)
    return false;

  shdrs = shdr_buf;
  if (__pread64_nocancel (fd, shdrs, shdr_size, ehdr.e_shoff)
      != (ssize_t) shdr_size)
    return false;

  /* Read the section name string table.  */
  if (ehdr.e_shstrndx >= ehdr.e_shnum)
    return false;

  ElfW(Shdr) *strtab_shdr = &shdrs[ehdr.e_shstrndx];
  if (strtab_shdr->sh_size > 65536 || strtab_shdr->sh_size == 0)
    return false;

  char strtab_buf[4096];
  size_t strtab_read = strtab_shdr->sh_size < sizeof (strtab_buf)
		       ? strtab_shdr->sh_size : sizeof (strtab_buf);
  if (__pread64_nocancel (fd, strtab_buf, strtab_read,
			  strtab_shdr->sh_offset)
      != (ssize_t) strtab_read)
    return false;

  /* Find .note.dl-secure section.  */
  for (unsigned int i = 0; i < ehdr.e_shnum; i++)
    {
      if (shdrs[i].sh_type != SHT_NOTE)
	continue;
      if (shdrs[i].sh_name >= strtab_read)
	continue;
      if (strcmp (&strtab_buf[shdrs[i].sh_name], ".note.dl-secure") != 0)
	continue;

      /* Found the note section.  Read it.  */
      if (shdrs[i].sh_size > 4096 || shdrs[i].sh_size < 12)
	continue;

      uint8_t note_buf[4096];
      if (__pread64_nocancel (fd, note_buf, shdrs[i].sh_size,
			      shdrs[i].sh_offset)
	  != (ssize_t) shdrs[i].sh_size)
	continue;

      /* Parse ELF note header.  */
      uint32_t namesz, descsz, type;
      memcpy (&namesz, note_buf, 4);
      memcpy (&descsz, note_buf + 4, 4);
      memcpy (&type, note_buf + 8, 4);

      if (type != DL_SECURE_NOTE_TYPE)
	continue;

      /* Verify note name.  */
      size_t name_offset = 12;
      size_t aligned_namesz = (namesz + 3) & ~(size_t)3;
      if (name_offset + aligned_namesz > shdrs[i].sh_size)
	continue;
      if (namesz != sizeof (DL_SECURE_NOTE_NAME))
	continue;
      if (memcmp (note_buf + name_offset, DL_SECURE_NOTE_NAME, namesz) != 0)
	continue;

      /* The descriptor is the HMAC-SHA256 signature (32 bytes).  */
      size_t desc_offset = name_offset + aligned_namesz;
      if (descsz != DL_SECURE_HASH_SIZE)
	continue;
      if (desc_offset + descsz > shdrs[i].sh_size)
	continue;

      /* Compute HMAC-SHA256 over PT_LOAD segments.  */
      uint8_t file_hash[DL_SECURE_HASH_SIZE];
      if (!_dl_secure_hash_elf (fd, file_hash))
	continue;

      uint8_t expected_mac[DL_SECURE_HASH_SIZE];
      _dl_secure_hmac_sha256 (key, file_hash, DL_SECURE_HASH_SIZE,
			      expected_mac);

      /* Constant-time comparison.  */
      uint8_t diff = 0;
      for (int j = 0; j < DL_SECURE_HASH_SIZE; j++)
	diff |= note_buf[desc_offset + j] ^ expected_mac[j];

      if (diff == 0)
	return true;
    }

  return false;
}

/* --- Policy evaluation ------------------------------------------------- */

/* Check if a path matches any deny rule.  */
static bool
_dl_secure_path_denied (const char *path)
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;
  for (int i = 0; i < pol->nrules; i++)
    {
      if (pol->rules[i].type == DL_SECURE_DENY_PATH
	  && _dl_secure_glob_match (pol->rules[i].pattern, path))
	return true;
    }
  return false;
}

/* Check if a path matches any allow rule.  */
static bool
_dl_secure_path_allowed (const char *path)
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;
  for (int i = 0; i < pol->nrules; i++)
    {
      if (pol->rules[i].type == DL_SECURE_ALLOW_PATH
	  && _dl_secure_glob_match (pol->rules[i].pattern, path))
	return true;
    }
  return false;
}

/* Check if a path requires a signature.  */
static bool
_dl_secure_path_requires_sig (const char *path)
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;
  for (int i = 0; i < pol->nrules; i++)
    {
      if (pol->rules[i].type == DL_SECURE_REQUIRE_SIG
	  && _dl_secure_glob_match (pol->rules[i].pattern, path))
	return true;
    }
  return false;
}

/* Check if a hash matches the whitelist.  */
static bool
_dl_secure_hash_whitelisted (const uint8_t hash[DL_SECURE_HASH_SIZE])
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;
  for (int i = 0; i < pol->nhashes; i++)
    {
      if (memcmp (pol->hashes[i].hash, hash, DL_SECURE_HASH_SIZE) == 0)
	return true;
    }
  return false;
}

/* --- Public API --------------------------------------------------------- */

void
_dl_secure_init (void)
{
  struct dl_secure_policy *pol = &_dl_secure_policy;

  if (pol->initialized)
    return;
  pol->initialized = 1;
  pol->mode = DL_SECURE_OFF;

  /* Check if the config file exists.  Using __access() first avoids
     the overhead of open+mmap for the common case where there is no
     policy file.  This follows the same pattern as ld.so.preload
     handling in rtld.c.  */
  if (__access (_dl_secure_config_path, R_OK) != 0)
    return;

  /* Read the entire config file.  */
  size_t file_size;
  void *file = _dl_sysdep_read_whole_file (_dl_secure_config_path,
					   &file_size,
					   PROT_READ | PROT_WRITE);
  if (file == MAP_FAILED || file_size == 0)
    return;

  /* Keep the mapping alive — rule patterns point into it.  */
  pol->config_data = file;
  pol->config_size = file_size;

  _dl_secure_parse_config ((char *) file, file_size);

  /* In SUID mode, always enforce if a policy file exists.  */
  if (__libc_enable_secure && pol->mode < DL_SECURE_ENFORCE)
    pol->mode = DL_SECURE_ENFORCE;

  if (pol->mode != DL_SECURE_OFF)
    _dl_debug_printf ("dl-secure: initialized, mode=%s, %d rules, "
		      "%d hashes\n",
		      pol->mode == DL_SECURE_ENFORCE ? "enforce" : "audit",
		      pol->nrules, pol->nhashes);
}

int
_dl_secure_check_file (const char *name, int fd)
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;

  /* If not initialized or off, allow everything.  */
  if (!pol->initialized || pol->mode == DL_SECURE_OFF)
    return 0;

  /* Deny-path rules always take precedence.  */
  if (_dl_secure_path_denied (name))
    {
      if (pol->mode == DL_SECURE_ENFORCE)
	{
	  _dl_debug_printf ("dl-secure: DENIED (deny-path) %s\n", name);
	  return -1;
	}
      _dl_debug_printf ("dl-secure: AUDIT deny-path %s\n", name);
      return 0;
    }

  /* Check if the file hash is whitelisted.  */
  uint8_t hash[DL_SECURE_HASH_SIZE];
  bool have_hash = _dl_secure_hash_elf (fd, hash);

  if (have_hash && _dl_secure_hash_whitelisted (hash))
    {
      _dl_debug_printf ("dl-secure: allowed (hash whitelist) %s\n", name);
      return 0;
    }

  /* Check if a signature is required for this path.  */
  if (_dl_secure_path_requires_sig (name))
    {
      if (!pol->has_hmac_key
	  || !_dl_secure_verify_signature (fd, pol->hmac_key))
	{
	  if (pol->mode == DL_SECURE_ENFORCE)
	    {
	      _dl_debug_printf ("dl-secure: DENIED (require-sig) %s\n",
				name);
	      return -1;
	    }
	  _dl_debug_printf ("dl-secure: AUDIT require-sig failed %s\n",
			    name);
	}
      else
	_dl_debug_printf ("dl-secure: allowed (valid signature) %s\n",
			  name);
      return 0;
    }

  /* Check allow-path rules.  */
  if (_dl_secure_path_allowed (name))
    return 0;

  /* No rule matched — deny in enforce mode.  */
  if (pol->mode == DL_SECURE_ENFORCE)
    {
      _dl_debug_printf ("dl-secure: DENIED (no matching rule) %s\n", name);
      return -1;
    }

  _dl_debug_printf ("dl-secure: AUDIT no matching rule %s\n", name);
  return 0;
}

void
_dl_secure_check_main (struct link_map *m, const char *program)
{
  const struct dl_secure_policy *pol = &_dl_secure_policy;

  if (!pol->initialized || pol->mode == DL_SECURE_OFF)
    return;

  /* Prefer the realname from the link_map, fall back to argv[0].  */
  const char *name = m->l_name;
  if (name == NULL || name[0] == '\0')
    name = program;
  if (name == NULL)
    return;

  /* Open the main executable to verify its hash/signature.  */
  int fd = __open64_nocancel (name, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      if (pol->mode == DL_SECURE_ENFORCE)
	_dl_fatal_printf (
	  "dl-secure: cannot open main executable for verification: %s\n",
	  name);
      return;
    }

  int result = _dl_secure_check_file (name, fd);
  __close_nocancel (fd);

  if (result < 0)
    _dl_fatal_printf ("dl-secure: main executable denied by policy: %s\n",
		      name);
}
