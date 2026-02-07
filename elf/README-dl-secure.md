# dl-secure: Secure Loader Module for ld.so

The dl-secure module adds policy-based security enforcement to glibc's
dynamic linker.  It controls which executables and shared libraries
ld.so is willing to load, using path rules, SHA-256 hash whitelisting,
and HMAC-SHA256 signature verification.

## Overview

When a policy file exists at `/etc/ld.so.secure`, the dynamic linker
evaluates every ELF binary against the configured rules before allowing
it to run or be loaded as a shared library.  Three enforcement modes
are available:

| Mode      | Behavior                                  |
|-----------|-------------------------------------------|
| `off`     | No enforcement (default when no config)   |
| `audit`   | Log violations but allow loading          |
| `enforce` | Block violations (`EACCES`)               |

When a process is SUID/SGID, enforce mode is always activated if a
policy file exists, regardless of the `mode` directive.

## Verification Mechanisms

### 1. Path-based allow/deny rules

Glob patterns (supporting `*` as a wildcard) match against the resolved
file path of each binary.  Deny rules always take precedence over allow
rules.

### 2. SHA-256 hash whitelist

The SHA-256 hash is computed over the concatenated content of all
`PT_LOAD` segments in the ELF file.  This hash is stable across `strip`
operations since only loadable content is included.

### 3. HMAC-SHA256 signature verification

Binaries can carry an embedded signature in a `.note.dl-secure` ELF
section.  The signature is an HMAC-SHA256 of the PT_LOAD hash, keyed
with a shared secret configured in the policy file.  Verification uses
constant-time comparison to prevent timing side channels.

## Policy Evaluation Order

When `_dl_secure_check_file()` is called for a shared library (or
`_dl_secure_check_main()` for the main executable), rules are evaluated
in this order:

1. If mode is `off` or the module is uninitialized: **allow**.
2. If the path matches a `deny-path` rule: **deny** (deny always wins).
3. If the file's PT_LOAD hash matches an `allow-hash` entry: **allow**.
4. If the path matches a `require-sig` rule: verify the embedded
   `.note.dl-secure` HMAC signature.  **Allow** if valid, **deny** if
   missing or invalid.
5. If the path matches an `allow-path` rule: **allow**.
6. No rule matched: **deny** in enforce mode, **allow** in audit mode.

This is a **default-deny** design in enforce mode.

## Configuration

The policy file is `/etc/ld.so.secure`.  This path is hardcoded and
cannot be overridden by environment variables.

### Syntax

```
# Comments start with '#'
# Blank lines are ignored

# Set the enforcement mode (required)
mode enforce|audit|off

# HMAC key for signature verification (64 hex characters = 32 bytes)
hmac-key <hex>

# Path rules — glob patterns, '*' matches any sequence of characters
allow-path <pattern>
deny-path <pattern>

# SHA-256 hash whitelist — hash is of PT_LOAD segments
allow-hash sha256:<64 hex chars> [optional path hint]

# Require a valid embedded HMAC signature for matching paths
require-sig <pattern>
```

### Directives

| Directive     | Arguments                          | Description                                         |
|---------------|------------------------------------|-----------------------------------------------------|
| `mode`        | `enforce`, `audit`, or `off`       | Set enforcement mode                                |
| `hmac-key`    | 64 hex characters                  | Shared secret for HMAC-SHA256 signature verification |
| `allow-path`  | glob pattern                       | Allow loading files matching this pattern            |
| `deny-path`   | glob pattern                       | Deny loading files matching this pattern             |
| `allow-hash`  | `sha256:<hex>` [path]              | Allow a file with this exact PT_LOAD hash            |
| `require-sig` | glob pattern                       | Require a valid `.note.dl-secure` signature          |

### Limits

- Maximum 256 path rules (`allow-path`, `deny-path`, `require-sig` combined)
- Maximum 256 hash whitelist entries

## Examples

### Example 1: Allow only system libraries

This policy allows libraries from standard system paths and denies
everything else (default-deny in enforce mode):

```
# /etc/ld.so.secure
mode enforce

allow-path /usr/lib/*
allow-path /usr/lib64/*
allow-path /lib/*
allow-path /lib64/*
allow-path /lib/x86_64-linux-gnu/*
allow-path /usr/lib/x86_64-linux-gnu/*
```

Any attempt to load a library from `/tmp`, `/home`, or other
non-standard locations will fail with `EACCES`.

### Example 2: Deny specific paths

A simpler policy that only blocks known-dangerous locations while
allowing everything else through audit logging:

```
# /etc/ld.so.secure
mode audit

deny-path /tmp/*
deny-path /dev/shm/*
deny-path /home/*/uploads/*
deny-path /var/tmp/*
```

In audit mode, violations are logged but not blocked, making this
useful for monitoring before switching to enforce.

### Example 3: Hash whitelist for a specific application

Pin a specific version of a custom library by its PT_LOAD hash:

```
# /etc/ld.so.secure
mode enforce

allow-path /usr/lib/*
allow-path /lib/x86_64-linux-gnu/*
allow-hash sha256:3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b /opt/myapp/lib/libcustom.so
```

To obtain the hash, use the signing tool:

```console
$ python3 scripts/elf-sign.py --hash /opt/myapp/lib/libcustom.so
sha256:3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
```

### Example 4: Signature-based verification

Require all binaries under a vendor directory to carry valid HMAC
signatures:

```
# /etc/ld.so.secure
mode enforce

hmac-key 48656c6c6f20776f726c642048656c6c6f20776f726c642048656c6c6f203132

allow-path /usr/lib/*
allow-path /lib/x86_64-linux-gnu/*
require-sig /opt/vendor/*
```

## Signing Tool

The `scripts/elf-sign.py` tool signs, verifies, and hashes ELF
binaries for use with dl-secure.

### Generating a key

The HMAC key is 32 bytes, stored as 64 hex characters.  Generate one
with:

```console
$ openssl rand -hex 32 > /etc/dl-secure.key
$ chmod 600 /etc/dl-secure.key
$ cat /etc/dl-secure.key
48656c6c6f20776f726c642048656c6c6f20776f726c642048656c6c6f203132
```

Place the same hex string in `/etc/ld.so.secure` as the `hmac-key`
directive.

### Signing a binary

```console
$ python3 scripts/elf-sign.py --key /etc/dl-secure.key --sign /opt/vendor/libfoo.so
Signed: /opt/vendor/libfoo.so
  PT_LOAD hash: 3a7bd3e2360a3d29eea436fcfb7e44c735d117c42d1c1835420b6b9942dd4f1b
  HMAC-SHA256:  9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

This appends a `.note.dl-secure` section to the ELF file containing
the HMAC-SHA256 signature.  The original file is replaced atomically.
File permissions are preserved.

### Verifying a binary

```console
$ python3 scripts/elf-sign.py --key /etc/dl-secure.key --verify /opt/vendor/libfoo.so
OK: valid signature for /opt/vendor/libfoo.so
```

A non-zero exit code indicates verification failure.

### Computing a hash

```console
$ python3 scripts/elf-sign.py --hash /usr/lib/x86_64-linux-gnu/libc.so.6
sha256:a1b2c3d4e5f6...
```

The output can be placed directly in a policy file as an `allow-hash`
entry.

## Deployment Workflow

### Step 1: Audit mode

Start with audit mode to discover what would be blocked:

```
# /etc/ld.so.secure
mode audit

allow-path /usr/lib/*
allow-path /lib/x86_64-linux-gnu/*
deny-path /tmp/*
```

Monitor ld.so debug output for `dl-secure: AUDIT` messages.  To see
these messages, run a program with `LD_DEBUG=all` or check syslog if
your system captures loader diagnostics.

### Step 2: Refine rules

Add `allow-path`, `allow-hash`, or `require-sig` rules for any
legitimate libraries that appear in audit logs.

### Step 3: Switch to enforce

```
mode enforce
```

Violations will now cause the loader to refuse the file with `EACCES`.
Denied files produce a `dl-secure: DENIED` debug message and the
program will typically fail with an error like:

```
error while loading shared libraries: libfoo.so: cannot open shared object file: Permission denied
```

### Step 4 (optional): Sign vendor binaries

For third-party binaries that may be updated independently:

```console
# Generate a key (once)
$ openssl rand -hex 32 > /etc/dl-secure.key

# Sign each binary after installation/update
$ python3 scripts/elf-sign.py --key /etc/dl-secure.key --sign /opt/vendor/lib/*.so

# Add to policy
$ cat >> /etc/ld.so.secure <<'EOF'
hmac-key <contents of /etc/dl-secure.key>
require-sig /opt/vendor/*
EOF
```

## Integration Points

The module hooks into ld.so at three points:

| Function                    | Called from            | Location        | Purpose                                  |
|-----------------------------|-----------------------|-----------------|------------------------------------------|
| `_dl_secure_init()`         | `dl_main()`           | `rtld.c:1937`   | Parse `/etc/ld.so.secure` at startup     |
| `_dl_secure_check_file()`   | `open_verify()`       | `dl-load.c:1794`| Gate every shared library load           |
| `_dl_secure_check_main()`   | `dl_main()`           | `rtld.c:2273`   | Verify the main executable after loading |

`_dl_secure_init()` runs after ld.so.preload handling but before
dependency resolution, so preloaded libraries are already loaded but
the main executable's dependencies have not yet been processed.

`_dl_secure_check_file()` runs inside `open_verify()` after all ELF
header validation has passed, so the file is known to be a valid ELF
for the current architecture.

## Design Notes

- **Hardcoded config path**: `/etc/ld.so.secure` cannot be overridden
  by `LD_*` environment variables, preventing attackers from pointing
  to a permissive policy.

- **SUID auto-enforce**: If a policy file exists and the binary is
  SUID/SGID, enforce mode is activated regardless of the `mode`
  directive.

- **Zero-copy config parsing**: The config file is mmapped with
  `PROT_READ | PROT_WRITE` and rule patterns point directly into the
  mapped memory, avoiding heap allocation in the dynamic linker.

- **Bounded data structures**: Fixed limits of 256 rules and 256 hash
  entries avoid dynamic allocation.

- **PT_LOAD-only hashing**: Hashing only loadable segments means the
  hash is stable across `strip`, `objcopy --strip-debug`, and other
  operations that modify non-loadable sections.

- **Self-contained SHA-256**: The `dl-sha256.h` implementation has no
  dependencies on libc since the dynamic linker cannot call into libc
  during early startup.

- **Constant-time signature comparison**: Prevents timing side-channel
  attacks against the HMAC verification.

## Files

| File                    | Description                                          |
|-------------------------|------------------------------------------------------|
| `elf/dl-secure.h`      | Policy data structures and API declarations          |
| `elf/dl-secure.c`      | Policy engine: parser, path matching, hash/HMAC verification, audit logging |
| `elf/dl-sha256.h`      | Self-contained SHA-256 implementation (header-only)  |
| `scripts/elf-sign.py`  | Python tool to sign, verify, and hash ELF binaries   |
| `elf/tst-dl-secure.c`  | Unit tests for SHA-256 and HMAC-SHA256               |

## Testing

The unit test verifies the cryptographic building blocks:

```console
$ make test t=tst-dl-secure
```

This runs FIPS 180-4 test vectors for SHA-256 (empty string, "abc",
two-block message) and validates HMAC-SHA256 determinism and
key/data sensitivity.

Full policy testing requires writing `/etc/ld.so.secure` (which needs
root) and is better done via integration test scripts.
