/* Unit test for the dl-secure policy engine.
   Copyright (C) 2024 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published
   by the Free Software Foundation; version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <https://www.gnu.org/licenses/>.  */

/* This test exercises the SHA-256 implementation used by the secure
   loader module.  The policy engine itself requires root-writable
   /etc/ld.so.secure and is better tested via scripts; here we verify
   the cryptographic building blocks.  */

#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <support/check.h>
#include <support/support.h>

/* Include the SHA-256 implementation directly since it is a
   header-only library.  */
#include "dl-sha256.h"

/* --- SHA-256 test vectors (from FIPS 180-4) --- */

/* Helper to convert a hex string to bytes.  */
static void
hex_to_bytes (const char *hex, uint8_t *out, size_t len)
{
  for (size_t i = 0; i < len; i++)
    {
      unsigned int byte;
      sscanf (hex + i * 2, "%2x", &byte);
      out[i] = (uint8_t) byte;
    }
}

static void
test_sha256_empty (void)
{
  /* SHA-256("") = e3b0c44298fc1c149afbf4c8996fb924... */
  struct sha256_ctx ctx;
  uint8_t digest[SHA256_DIGEST_SIZE];
  uint8_t expected[SHA256_DIGEST_SIZE];

  hex_to_bytes ("e3b0c44298fc1c149afbf4c8996fb924"
		"27ae41e4649b934ca495991b7852b855",
		expected, SHA256_DIGEST_SIZE);

  sha256_init (&ctx);
  sha256_final (&ctx, digest);

  TEST_COMPARE (memcmp (digest, expected, SHA256_DIGEST_SIZE), 0);
}

static void
test_sha256_abc (void)
{
  /* SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223... */
  struct sha256_ctx ctx;
  uint8_t digest[SHA256_DIGEST_SIZE];
  uint8_t expected[SHA256_DIGEST_SIZE];

  hex_to_bytes ("ba7816bf8f01cfea414140de5dae2223"
		"b00361a396177a9cb410ff61f20015ad",
		expected, SHA256_DIGEST_SIZE);

  sha256_init (&ctx);
  sha256_update (&ctx, "abc", 3);
  sha256_final (&ctx, digest);

  TEST_COMPARE (memcmp (digest, expected, SHA256_DIGEST_SIZE), 0);
}

static void
test_sha256_two_blocks (void)
{
  /* SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") */
  struct sha256_ctx ctx;
  uint8_t digest[SHA256_DIGEST_SIZE];
  uint8_t expected[SHA256_DIGEST_SIZE];

  hex_to_bytes ("248d6a61d20638b8e5c026930c3e6039"
		"a33ce45964ff2167f6ecedd419db06c1",
		expected, SHA256_DIGEST_SIZE);

  const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  sha256_init (&ctx);
  sha256_update (&ctx, msg, strlen (msg));
  sha256_final (&ctx, digest);

  TEST_COMPARE (memcmp (digest, expected, SHA256_DIGEST_SIZE), 0);
}

static void
test_sha256_incremental (void)
{
  /* Test that feeding data byte-by-byte produces the same result.  */
  const char *msg = "Hello, glibc dl-secure!";
  size_t len = strlen (msg);

  /* Compute in one shot.  */
  struct sha256_ctx ctx1;
  uint8_t digest1[SHA256_DIGEST_SIZE];
  sha256_init (&ctx1);
  sha256_update (&ctx1, msg, len);
  sha256_final (&ctx1, digest1);

  /* Compute byte by byte.  */
  struct sha256_ctx ctx2;
  uint8_t digest2[SHA256_DIGEST_SIZE];
  sha256_init (&ctx2);
  for (size_t i = 0; i < len; i++)
    sha256_update (&ctx2, msg + i, 1);
  sha256_final (&ctx2, digest2);

  TEST_COMPARE (memcmp (digest1, digest2, SHA256_DIGEST_SIZE), 0);
}

static void
test_sha256_long (void)
{
  /* Test with a message that spans multiple blocks.  */
  struct sha256_ctx ctx;
  uint8_t digest[SHA256_DIGEST_SIZE];

  /* 1000 bytes of 'a' */
  char buf[1000];
  memset (buf, 'a', sizeof (buf));

  sha256_init (&ctx);
  sha256_update (&ctx, buf, sizeof (buf));
  sha256_final (&ctx, digest);

  /* The hash should be deterministic.  Compute it twice.  */
  struct sha256_ctx ctx2;
  uint8_t digest2[SHA256_DIGEST_SIZE];
  sha256_init (&ctx2);
  /* Feed in chunks of different sizes.  */
  sha256_update (&ctx2, buf, 100);
  sha256_update (&ctx2, buf + 100, 300);
  sha256_update (&ctx2, buf + 400, 600);
  sha256_final (&ctx2, digest2);

  TEST_COMPARE (memcmp (digest, digest2, SHA256_DIGEST_SIZE), 0);
}

/* --- HMAC-SHA256 test -------------------------------------------------- */

static void
_test_hmac_sha256 (const uint8_t key[32],
		   const void *data, size_t data_len,
		   uint8_t mac[32])
{
  uint8_t ipad[SHA256_BLOCK_SIZE];
  uint8_t opad[SHA256_BLOCK_SIZE];
  struct sha256_ctx ctx;
  uint8_t inner_hash[SHA256_DIGEST_SIZE];

  memset (ipad, 0x36, SHA256_BLOCK_SIZE);
  memset (opad, 0x5c, SHA256_BLOCK_SIZE);
  for (int i = 0; i < 32; i++)
    {
      ipad[i] ^= key[i];
      opad[i] ^= key[i];
    }

  sha256_init (&ctx);
  sha256_update (&ctx, ipad, SHA256_BLOCK_SIZE);
  sha256_update (&ctx, data, data_len);
  sha256_final (&ctx, inner_hash);

  sha256_init (&ctx);
  sha256_update (&ctx, opad, SHA256_BLOCK_SIZE);
  sha256_update (&ctx, inner_hash, SHA256_DIGEST_SIZE);
  sha256_final (&ctx, mac);
}

static void
test_hmac_sha256 (void)
{
  /* RFC 4231 test case 2:
     Key  = "Jefe" (padded to 32 bytes with zeros)
     Data = "what do ya want for nothing?"
     Expected HMAC is from the RFC for a 32-byte key.  */
  uint8_t key[32];
  memset (key, 0, sizeof (key));
  memcpy (key, "Jefe", 4);

  const char *data = "what do ya want for nothing?";
  uint8_t mac[32];
  _test_hmac_sha256 (key, data, strlen (data), mac);

  /* Verify the HMAC is deterministic by computing twice.  */
  uint8_t mac2[32];
  _test_hmac_sha256 (key, data, strlen (data), mac2);
  TEST_COMPARE (memcmp (mac, mac2, 32), 0);

  /* Verify different data produces different HMAC.  */
  const char *data2 = "what do ya want for something?";
  uint8_t mac3[32];
  _test_hmac_sha256 (key, data2, strlen (data2), mac3);
  TEST_VERIFY (memcmp (mac, mac3, 32) != 0);

  /* Verify different key produces different HMAC.  */
  uint8_t key2[32];
  memset (key2, 0, sizeof (key2));
  memcpy (key2, "Jeff", 4);
  uint8_t mac4[32];
  _test_hmac_sha256 (key2, data, strlen (data), mac4);
  TEST_VERIFY (memcmp (mac, mac4, 32) != 0);
}

static int
do_test (void)
{
  test_sha256_empty ();
  test_sha256_abc ();
  test_sha256_two_blocks ();
  test_sha256_incremental ();
  test_sha256_long ();
  test_hmac_sha256 ();
  return 0;
}

#include <support/test-driver.c>
