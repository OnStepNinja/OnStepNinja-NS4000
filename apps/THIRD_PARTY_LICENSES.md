# Third-Party Licenses

The star chart of LUNA OnStepNinja NS-4000 (`starchart_ja.html` + `starchart_ja_data.js`, from LUNA AiBridge StarChart) contains or uses
the following third-party material. These remain under their own licenses,
separate from this project's Freeware License (see [LICENSE](LICENSE)).

---

## d3-celestial — star, constellation and name data (`starchart_data.js`)

The star positions and magnitudes, constellation lines and names, and star names
in `starchart_data.js` are derived (reduced and re-encoded) from the data files
of d3-celestial: https://github.com/ofrohn/d3-celestial

BSD 3-Clause License

```
Copyright (c) 2015, Olaf Frohn
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## Planet, Sun and Moon positions

Positions of the Sun, Moon and planets are computed from the simplified orbital
elements and perturbation terms published by Paul Schlyter,
"How to compute planets' positions" (https://stjarnhimlen.se/comp/ppcomp.html).
The published numerical constants and formulas are used; no code was copied.
Accuracy is on the order of arc-minutes (the Moon is corrected for parallax).

## Precession (J2000 ⇄ JNow)

The standard IAU 1976 precession angles and rotation formulas as described in
Jean Meeus, *Astronomical Algorithms*. Formulas are used; no code was copied.

## Messier object coordinates

Positions and magnitudes of the 110 Messier objects are standard astronomical
facts, compiled by the author and cross-checked against an independent list.

## Compatible hardware / protocol

- **AiBridge** (ESP32 firmware, MIT License): https://github.com/OnStepNinja/AiBridge
- **OnStep** command set (LX200-compatible) is used over the AiBridge
  `/api/command` interface. OnStep is a separate project under its own license.
