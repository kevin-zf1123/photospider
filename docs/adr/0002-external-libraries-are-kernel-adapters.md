# ADR 0002: External Libraries Stay Outside Kernel Semantics

## Status

Accepted and narrowed by ADR 0015 for the current embeddable-kernel boundary.

## Context

The pre-reset product embedded OpenCV, yaml-cpp, FTXUI, CURL, and OpenSSL in
core, adapters, CLI, persistence, service, or product-security paths. That
coupling made optional libraries part of the kernel build and allowed their
types and lifecycle assumptions to shape kernel semantics.

The Scope Reset leaves an embeddable graph compiler/executor whose required
platform dependency is only the C++ runtime and thread library. Third-party
algorithms can still be useful to an operation implementation, but they do not
belong to the kernel package contract.

## Decision

The kernel owns `WorkflowDocument`, typed IR, operation traits, `Value`,
`Region`, layout, execution, and diagnostic types using only its public
contracts and standard-library representations.

The repository does not ship an OpenCV/yaml-cpp adapter, CLI library, codec,
or dependency-toggle compatibility profile. The canonical kernel target and
installed package do not find, link, export, or advertise those libraries.

A trusted in-process operation or data-provider DSO may privately link a
third-party library. It must translate all inputs, outputs, exceptions, and
lifecycle behavior at the operation/provider ABI boundary. No third-party
type, allocator owner, exception, path, or configuration object crosses the
public ABI. ABI validation is correctness validation; it is not sandboxing or
native-code security.

`WorkflowDocument` is an in-memory compiler input. File formats and storage
services are consumer concerns and are not kernel adapters or authorities.

## Consequences

- A clean kernel configure/build/install and isolated consumer require no
  optional third-party package.
- Kernel primitives remain intentionally small; Photospider does not recreate
  a general image-processing, serialization, UI, network, or crypto library.
- An operation DSO owns any library initialization, thread settings, and
  exception translation it needs, within the process-global startup-configured
  operation set.
- Adding a repository-owned library integration requires a focused operation
  or provider decision; it cannot reintroduce a core dependency, compatibility
  option, or filesystem authority.
