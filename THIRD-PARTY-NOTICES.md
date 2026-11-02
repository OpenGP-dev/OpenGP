# Third-party notices

OpenGP is Copyright (C) 2026 the OpenGP contributors, and is licensed under the GNU General Public
License, version 3 or (at your option) any later version (see [LICENSE](LICENSE)). Its source code
is at <https://github.com/OpenGP-dev/OpenGP>.

OpenGP, its installer packages and the games the installer builds contain the third-party work
below. Each keeps its own licence. The full text of each licence is below, or in the `licenses/`
folder of an installer package.

An installer package also holds the launcher, `OpenGP` (`OpenGP.exe`, `OpenGP.app`), from
<https://github.com/OpenGP-dev/opengp-launcher>. It is under the GPL-3.0-or-later too, and is built
from that repository together with the OpenGP release the package comes from. The work in the
last five rows comes with it. Its own notices are `licenses/launcher-THIRD-PARTY-NOTICES.md` in the
package.

| Work | Licence | Where |
|------|---------|-------|
| [Dolphin](https://github.com/dolphin-emu/dolphin) (ported parts) | GPL-2.0-or-later | `runtime/`, `tools/sigmatch.py`; each file says what it is based on |
| [SDL 3](https://github.com/libsdl-org/SDL), with its HIDAPI, yuv2rgb and libm code, and on Linux its X11 and Wayland code | zlib; BSD-3-Clause; BSD-3-Clause; Sun's free licence; MIT/X11 and HPND (Linux only) | linked into every game and the installer package |
| [Dear ImGui](https://github.com/ocornut/imgui), with stb and the Proggy fonts | MIT; MIT or public domain; MIT | the in-game menu |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT | `thirdparty/tomlplusplus`, linked into the games and the installer |
| Replacement GameCube IPL fonts, based on Droid Sans | Apache-2.0 | `thirdparty/gc-fonts`, embedded in every game |
| Dolphin's software clipper, with portions by Markus Trenkwalder | BSD-3-Clause | `runtime/gx/frontend.cpp` (the backface test) |
| [xxHash](https://github.com/Cyan4973/xxHash)'s XXH64 algorithm | BSD-2-Clause | `runtime/gx/frontend.cpp` (texture hashing) |
| [doctest](https://github.com/doctest/doctest) | MIT | `thirdparty/doctest`, the test suite only (not in packages) |
| [Qt 6](https://www.qt.io/), 6.10.3, and the third-party code in it | LGPL-3.0; their own, in `licenses/Qt-third-party.txt` | installer packages: the launcher's interface, as shared libraries |
| M PLUS Rounded 1c | OFL-1.1 | `thirdparty/fonts/mplus-rounded-1c`, embedded in every game for its menu; installer packages: the launcher's typeface too, built into it |
| SDL's scancode tables | zlib | installer packages: built into the launcher, for the keys pressed in its window |
| [xcb-util](https://xcb.freedesktop.org/) and [ICU](https://icu.unicode.org/), which Qt needs | MIT/X11 and HPND; Unicode licence | Linux packages only: `qt/lib/` |
| Microsoft's Visual C++ runtime and Direct3D shader compilers | Microsoft's terms for redistributable files | Windows packages only: next to `OpenGP.exe` |

The build also uses [glslang](https://github.com/KhronosGroup/glslang) and, on macOS,
[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) to compile OpenGP's own shaders. Neither is
part of OpenGP or its packages. On Windows, the installer downloads
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) from its own releases to compile the games.

## Dolphin

Parts of the runtime are ported from, or based on, the Dolphin GameCube and Wii emulator,
Copyright Dolphin Emulator Project, licensed under the GNU General Public License, version 2 or (at
your option) any later version. OpenGP uses them under version 3, as that licence allows. Each such
file names the Dolphin files it is based on.

Dolphin's `Clipper.cpp`, which OpenGP's backface test (in `runtime/gx/frontend.cpp`) is based on,
says that portions of it are based on work by Markus Trenkwalder, under this licence:

```
Copyright (c) 2007, 2008 Markus Trenkwalder

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the library's copyright owner nor the names of its
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## SDL 3

```
Copyright (C) 1997-2026 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

SDL includes HIDAPI, used here under its BSD licence:

```
Copyright (c) 2010, Alan Ott, Signal 11 Software
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Signal 11 Software nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

SDL includes yuv2rgb:

```
Copyright (c) 2016, Adrien Descamps
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of yuv2rgb nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

SDL includes maths functions from Sun's fdlibm:

```
Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.

Developed at SunPro, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

On Linux, SDL also includes the code below, for its X11 and Wayland video drivers.

The X11 keysym table (`src/events/imKStoUCS.c`):

```
Copyright (C) 2003-2006,2008 Jamey Sharp, Josh Triplett
Copyright © 2009 Red Hat, Inc.
Copyright 1990-1992,1999,2000,2004,2009,2010 Oracle and/or its affiliates.
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

The EDID parser (`src/video/x11/edid-parse.c`):

```
Copyright 2007 Red Hat, Inc.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

The XSETTINGS client (`src/video/x11/xsettings-client.c`):

```
Copyright © 2001, 2007 Red Hat, Inc.
Copyright 2024 Igalia S.L.

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation, and that the name of Red Hat not be used in advertising or
publicity pertaining to distribution of the software without specific,
written prior permission.  Red Hat makes no representations about the
suitability of this software for any purpose.  It is provided "as is"
without express or implied warranty.

RED HAT DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL RED HAT
BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
```

SDL's Wayland code is generated from the protocol files in its `wayland-protocols` folder.
These are their copyright notices (`pointer-gestures-unstable-v1.xml` names no holder):

```
alpha-modifier-v1.xml:
  Copyright © 2024 Xaver Hugl
color-management-v1.xml:
  Copyright 2019 Sebastian Wick
  Copyright 2019 Erwin Burema
  Copyright 2020 AMD
  Copyright 2020-2024 Collabora, Ltd.
  Copyright 2024 Xaver Hugl
  Copyright 2022-2025 Red Hat, Inc.
cursor-shape-v1.xml:
  Copyright 2018 The Chromium Authors
  Copyright 2023 Simon Ser
fractional-scale-v1.xml:
  Copyright © 2022 Kenny Levinsen
frog-color-management-v1.xml:
  Copyright © 2023 Joshua Ashton for Valve Software
  Copyright © 2023 Xaver Hugl
idle-inhibit-unstable-v1.xml:
  Copyright © 2015 Samsung Electronics Co., Ltd
input-timestamps-unstable-v1.xml:
  Copyright © 2017 Collabora, Ltd.
keyboard-shortcuts-inhibit-unstable-v1.xml:
  Copyright © 2017 Red Hat Inc.
pointer-constraints-unstable-v1.xml:
  Copyright © 2014      Jonas Ådahl
  Copyright © 2015      Red Hat Inc.
pointer-warp-v1.xml:
  Copyright © 2024 Neal Gompa
  Copyright © 2024 Xaver Hugl
  Copyright © 2024 Matthias Klumpp
  Copyright © 2024 Vlad Zahorodnii
primary-selection-unstable-v1.xml:
  Copyright © 2015, 2016 Red Hat
relative-pointer-unstable-v1.xml:
  Copyright © 2014      Jonas Ådahl
  Copyright © 2015      Red Hat Inc.
tablet-v2.xml:
  Copyright 2014 © Stephen "Lyude" Chandler Paul
  Copyright 2015-2016 © Red Hat, Inc.
viewporter.xml:
  Copyright © 2013-2016 Collabora, Ltd.
wayland.xml:
  Copyright © 2008-2011 Kristian Høgsberg
  Copyright © 2010-2011 Intel Corporation
  Copyright © 2012-2013 Collabora, Ltd.
xdg-activation-v1.xml:
  Copyright © 2020 Aleix Pol Gonzalez <aleixpol@kde.org>
  Copyright © 2020 Carlos Garnacho <carlosg@gnome.org>
xdg-decoration-unstable-v1.xml:
  Copyright © 2018 Simon Ser
xdg-dialog-v1.xml:
  Copyright © 2023 Carlos Garnacho
xdg-foreign-unstable-v2.xml:
  Copyright © 2015-2016 Red Hat Inc.
xdg-output-unstable-v1.xml:
  Copyright © 2017 Red Hat Inc.
xdg-shell.xml:
  Copyright © 2008-2013 Kristian Høgsberg
  Copyright © 2013      Rafael Antognolli
  Copyright © 2013      Jasper St. Pierre
  Copyright © 2010-2013 Intel Corporation
  Copyright © 2015-2017 Samsung Electronics Co., Ltd
  Copyright © 2015-2017 Red Hat Inc.
xdg-toplevel-icon-v1.xml:
  Copyright © 2023-2024 Matthias Klumpp
  Copyright ©      2024 David Edmundson
```

They are under this licence:

```
Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

except `text-input-unstable-v3.xml`, which is under this one:

```
Copyright © 2012, 2013 Intel Corporation
Copyright © 2015, 2016 Jan Arne Petersen
Copyright © 2017, 2018 Red Hat, Inc.
Copyright © 2018       Purism SPC

Permission to use, copy, modify, distribute, and sell this
software and its documentation for any purpose is hereby granted
without fee, provided that the above copyright notice appear in
all copies and that both that copyright notice and this permission
notice appear in supporting documentation, and that the name of
the copyright holders not be used in advertising or publicity
pertaining to distribution of the software without specific,
written prior permission.  The copyright holders make no
representations about the suitability of this software for any
purpose.  It is provided "as is" without express or implied
warranty.

THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
THIS SOFTWARE.
```

## Dear ImGui

```
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Dear ImGui includes stb_truetype, stb_rect_pack and stb_textedit by Sean Barrett (public
domain, or MIT, Copyright (c) 2017 Sean Barrett), and the fonts ProggyClean (MIT, Copyright (c)
2004, 2005 Tristan Grimmer) and ProggyForever (MIT, Copyright (c) 2026 Disco Hello, Copyright (c)
2019, 2023 Tristan Grimmer), under the same MIT terms as above.

## toml++

```
MIT License

Copyright (c) Mark Gillard <mark.gillard@outlook.com.au>

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## Replacement GameCube IPL fonts

`thirdparty/gc-fonts/font_western.bin` and `font_japanese.bin` were generated by the Dolphin project
with its gc-font-tool and are taken unchanged from Dolphin. They are based on Droid Sans, Copyright
Google, licensed under the Apache License, Version 2.0. The full licence is in
`thirdparty/gc-fonts/LICENSE` in the source (`licenses/gc-fonts-LICENSE.txt` in an installer
package).

## xxHash (XXH64)

OpenGP's texture hash (`hash_bytes` in `runtime/gx/frontend.cpp`) is an implementation of the
XXH64 algorithm by Yann Collet. xxHash is distributed under this licence:

```
xxHash Library
Copyright (c) 2012-2021 Yann Collet
All rights reserved.

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Qt

The launcher's interface is built with Qt 6, Copyright (C) The Qt Company Ltd. and other
contributors. The installer packages carry Qt 6.10.3: Qt Core, Qt Gui, Qt Network, Qt Qml, Qt Quick
and Qt Quick Controls, with the libraries and plugins they need. They are used under the GNU Lesser
General Public License, version 3 (LGPL-3.0). That is the GNU GPL version 3 ([LICENSE](LICENSE))
with extra permissions, in `thirdparty/qt/LGPL-3.0.txt` in the source (`licenses/Qt-LGPL-3.0.txt` in
an installer package), which is Qt's own copy of it.

Qt is not changed, and the launcher links it as shared libraries, so you can replace them with your
own build of Qt 6.10.3: the DLLs next to `OpenGP.exe` on Windows, the libraries in `qt/lib/` on
Linux, and the frameworks in `OpenGP.app/Contents/Frameworks/` on macOS (then sign the app again,
`codesign --force --deep -s - OpenGP.app`). The source code of Qt 6.10.3 is at
<https://download.qt.io/archive/qt/6.10/6.10.3/>, and in Qt's Git repositories at
<https://code.qt.io/> (tag `v6.10.3`).

Qt builds third-party code into these libraries and plugins, such as PCRE2, HarfBuzz, FreeType,
libjpeg-turbo, libpng and zlib, some of it only on some systems. Each keeps its own licence.
`licenses/Qt-third-party.txt` in an installer package gives the copyright notices and the full
licence of each (`thirdparty/qt/third-party.txt` in the source, made from the `qt_attribution.json`
files of Qt 6.10.3's source). "Licenses Used in Qt" lists them too
(<https://doc.qt.io/qt-6/licenses-used-in-qt.html>). As two of those licences ask: this software is
based in part on the work of the Independent JPEG Group. Portions of this software are copyright ©
2026 The FreeType Project (www.freetype.org). All rights reserved.

On Linux, a package also carries the xcb-util libraries and ICU in `qt/lib/`, as Qt needs them.
Their notices are in the launcher's notices, and their licences in `licenses/xcb-util-COPYING.txt`
and `licenses/ICU-LICENSE.txt`.

## M PLUS Rounded 1c

The typeface of the games' menu and of the launcher, M PLUS Rounded 1c (Regular and ExtraBold), is
Copyright 2016 The Rounded M+ Project Authors, and is licensed under the SIL Open Font License,
version 1.1. The copies in `thirdparty/fonts/mplus-rounded-1c` are cut down to Latin letters; they
are embedded in every game (`runtime/app/menu_theme.cpp`) and built into the launcher. The licence
is `thirdparty/fonts/mplus-rounded-1c/OFL.txt`, and `licenses/MPLUSRounded1c-OFL.txt` in an
installer package.

## SDL's scancode tables

The launcher turns the keys pressed in its window into the SDL scancodes the games name keys by,
with SDL 3.4's own tables, copied unchanged into `src/core/scancode_tables.cpp` in the launcher's
source. They are under SDL's zlib licence, as under "SDL 3" above.

## Microsoft's runtime files

A Windows package carries, next to `OpenGP.exe`, files of Microsoft's that the launcher and Qt
need: the Visual C++ runtime (`vcruntime140.dll`, `msvcp140.dll` and the others of its kind), and
Direct3D's shader compilers, which Qt's `windeployqt` adds (`d3dcompiler_47.dll`, and
`dxcompiler.dll` and `dxil.dll` when the Windows SDK has them). They are Copyright Microsoft
Corporation, and are not under the GPL. They come with the package as redistributable files of
Visual Studio and the Windows SDK, under Microsoft's licence terms for those.

## doctest

The test suite uses doctest (MIT, Copyright (c) 2016-2023 Viktor Kirilov); its licence is in
`thirdparty/doctest/LICENSE` in the source. It is not part of the installer packages.
