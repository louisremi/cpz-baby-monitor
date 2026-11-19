# Application Store Publish Record

Keep this file in the application project root. Update it when package
metadata, listing copy, assets, privacy choices, or submission status changes.

Portal: <https://dev.cardputer.cc/#/upload>

> **Not intended for publication.** The owner uses BabyMon for private
> testing only. The EU868 profile exceeds the 869.4–869.65 MHz sub-band's 10%
> duty cycle while streaming, so it must be reworked before any submission.

## Package

- Debian package path: `dist/cpz-baby-monitor_0.0.1-1_arm64.deb` (built by `scripts/remote_build.sh deb`)
- Package: `cpz-baby-monitor`
- Version: `0.0.1-1`
- Architecture: `arm64`
- Maintainer: `louisremi <pro@louisremi.com>`
- APPLaunch desktop path: `/usr/share/APPLaunch/applications/cpz-baby-monitor.desktop`
- Executable path: `/usr/share/cpz-baby-monitor/bin/cpz-baby-monitor`

## Source and Ownership

- Public source repository: <https://github.com/louisremi/cpz-baby-monitor>
- Expected GitHub uploader/owner: `louisremi`
- Hide uploader email: `true`

## Listing

- Application name: BabyMon
- One-line summary (maximum 80 characters): Wi-Fi-free baby monitor over LoRa for two CardputerZero
- Description: see `store.description` in `app-builder.json`
- Categories (maximum 6): Radio & Comms, Lifestyle

## Assets

- 256 x 256 source icon: `packaging/deb/images/cpz-baby-monitor.png`
- Icon packaged in `.deb`: `/usr/share/APPLaunch/share/images/cpz-baby-monitor.png`
- Screenshot 1 (320 x 170): `store/screenshots/pair.png`
- Screenshot 2 (320 x 170): `store/screenshots/home.png`
- Screenshot 3 (320 x 170): `store/screenshots/baby.png`
- Screenshot 4 (320 x 170): `store/screenshots/parent.png`
- Optional screenshot 5 (portal supports up to 6): none
- Optional screenshot 6 (portal supports up to 6): none

## Preflight

- [x] `.deb` exists and can be parsed.
- [x] Control fields contain Package, Version, Architecture, and Maintainer.
- [x] Architecture is appropriate for the package (`arm64` for native binaries).
- [x] APPLaunch `.desktop`, executable, and PNG icon exist inside the package.
- [ ] Package name ownership and update version are valid.
- [ ] Listing copy and categories have been reviewed.
- [x] Four clean 320 x 170 baseline screenshots exist.
- [x] No credentials, tokens, or private server details are embedded.
- [ ] The user explicitly approved external submission.

## Submission Result

- Status: `not submitted`
- Submitted at: TODO
- Portal message: TODO
- Actions URL: TODO
- Tracking or pull-request URL: TODO
