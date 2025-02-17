#!/usr/bin/env bash
#
# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euo pipefail

BINDIR=$(dirname "$0")
readonly BINDIR

cat <<_EOF_
# Google Cloud Spanner C++ Client Library

<!-- This file is automatically generated by ci/generate-markdown/$(basename "$0") -->

_EOF_

cat <<'_EOF_'
This directory contains an idiomatic C++ client library for interacting with
[Google Cloud Spanner](https://cloud.google.com/spanner/), which is a fully
managed, scalable, relational database service for regional and global
application data.

Please note that the Google Cloud C++ client libraries do **not** follow
[Semantic Versioning](http://semver.org/).

## Supported Platforms

* Windows, macOS, Linux
* C++11 (and higher) compilers (we test with GCC >= 5.4, Clang >= 6.0, and
  MSVC >= 2019)
* Environments with or without exceptions
* Bazel and CMake builds

## Documentation

* Official documentation about the [Cloud Spanner][cloud-spanner-docs] service
* [Reference doxygen documentation][doxygen-link] for each release of this client library
* Detailed header comments in our [public `.h`][source-link] files

[doxygen-link]: https://googleapis.dev/cpp/google-cloud-spanner/latest/
[cloud-spanner-docs]: https://cloud.google.com/spanner/docs/
[source-link]: https://github.com/googleapis/google-cloud-cpp/tree/main/google/cloud/spanner

## Quickstart

The [quickstart/](quickstart/README.md) directory contains a minimal environment
to get started using this client library in a larger project. The following
"Hello World" program is used in this quickstart, and should give you a taste of
this library.

```cc
_EOF_

# Dumps the contents of quickstart.cc starting at the first #include, so we
# skip the license header comment.
sed -n '/^#/,$p' "${BINDIR}/../../google/cloud/spanner/quickstart/quickstart.cc"

cat <<'_EOF_'
```

* Packaging maintainers or developers that prefer to install the library in a
  fixed directory (such as `/usr/local` or `/opt`) should consult the
  [packaging guide](/doc/packaging.md).
* Developers wanting to use the libraries as part of a larger CMake or Bazel
  project should consult the [quickstart guides](#quickstart) for the library
  or libraries they want to use.
* Developers wanting to compile the library just to run some of the examples or
  tests should read the current document.
* Contributors and developers to `google-cloud-cpp` should consult the guide to
  [setup a development workstation][howto-setup-dev-workstation].

[howto-setup-dev-workstation]: /doc/contributor/howto-guide-setup-development-workstation.md

## Contributing changes

See [`CONTRIBUTING.md`](../../../CONTRIBUTING.md) for details on how to
contribute to this project, including how to build and test your changes
as well as how to properly format your code.

## Licensing

Apache 2.0; see [`LICENSE`](../../../LICENSE) for details.
_EOF_
