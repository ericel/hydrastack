# ada (vendored)

WHATWG URL parser used by Node.js. It backs the SSR runtime's `URL` and
`URLSearchParams`, so server renders parse URLs exactly as browsers do.

- Version: 3.4.3
- Source: https://github.com/ada-url/ada/releases/download/v3.4.3/singleheader.zip
- Licence: dual MIT or Apache-2.0 (`LICENSE-MIT`, `LICENSE-APACHE`)
- SHA-256 `ada.h`:   c369f52ccd4a17233c7b5308e46816b0c4ee4ec1824db5ded65fbf3e3d9ffc81
- SHA-256 `ada.cpp`: cd7f8433f5f0807c2ee418dcb314047dc757add4db9a38e6904e4c15cc15a8cd

Compiled into `hydra_engine` only; the header is not installed. To upgrade,
replace both files from a newer `singleheader.zip` and update this file.
