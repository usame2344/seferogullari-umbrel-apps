# Security Policy

mkpool handles real mining infrastructure, so security reports are taken
seriously and are appreciated.

## Reporting a vulnerability

Please do not open a public issue for security problems. Report them privately
instead:

- Email: contact@mecanik.dev
- Include a clear description, steps to reproduce, the affected commit or
  version, and the impact you think it has.

You will get an acknowledgement as soon as possible. Please allow a reasonable
window to investigate and ship a fix before any public disclosure.

## Scope

In scope: the pool engine in this repository, including Stratum V1 / TLS /
Stratum V2 handling, share validation, coinbase construction, address decoding,
config parsing, and the build.

Out of scope: third-party dependencies (report those to their upstream
projects), and operator-run components that are not published here (the
database, the REST API, and the website are not part of this repository).

## Supported versions

Development happens on a single active line. Security fixes land on the default
branch.
