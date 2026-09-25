# Interception 1.0.1

Upstream: https://github.com/oblitum/Interception

Pinned release: https://github.com/oblitum/Interception/releases/tag/v1.0.1

Binary archive: https://github.com/oblitum/Interception/releases/download/v1.0.1/Interception.zip

Archive SHA-256: `ad038963d6413055765128b0b931f6e765147c9916dba79e65d872b261f9af10`

`include/interception.h`, `bin/interception.dll` (x64), the command-line installer,
and the original license files are unchanged files from this release. The full
GPLv3 text incorporated by LGPLv3 is additionally provided from
https://www.gnu.org/licenses/gpl-3.0.txt. `source/` contains the
library source, header, and upstream build script from the matching v1.0.1 tag:
https://github.com/oblitum/Interception/tree/v1.0.1/library

An additional `source/CMakeLists.txt` is supplied by this project so the unmodified
user-mode library can also be rebuilt with a current MSVC + Windows SDK toolchain:
`cmake -S source -B library_build` followed by `cmake --build library_build --config Release`.
Use an x64 developer environment; this does not rebuild the closed-source drivers.

The upstream project specifies LGPLv3 for non-commercial library usage and
separate commercial terms. Read the included license files before redistribution
or commercial use. The application dynamically loads the replaceable DLL through
the official API; it does not embed or modify the driver. Driver/installer source
is not included in the upstream public repository.

Copyright (c) Francisco Lopes. See upstream for full attribution and terms.
