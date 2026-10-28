# Native keyboard client dependency

Header-only client SDK extracted without modifications from Karabiner-DriverKit-VirtualHIDDevice v8.7.0,
commit 5542c822dbe31dcef1435835d86f591b6fcd3aa3.
Upstream: https://github.com/pqrs-org/Karabiner-DriverKit-VirtualHIDDevice/tree/v8.7.0

The archive contains only include/, vendor/vendor/include/, and license notices, not the upstream applications,
drivers, installers or build scripts. Source headers retain their original copyrights and license notices.
The virtual HID service protocol version is 7. This client must be used with the matching upstream signed component.

Copyright holders include Fumihiko Takayama, Christopher M. Kohlhoff, Jonathan Mueller,
Microsoft Corporation, Gabi Melman, nod, fmt and other dependency contributors.
Licenses: Unlicense, Boost Software License 1.0, MIT, and zlib. Texts are included in the archive and LICENSES/.

The helper links only Apple system libraries; it does not load the GUI application's Qt libraries while privileged.
The upstream signed/notarized driver installer is downloaded separately, verified by SHA256 and Apple signature,
and redistributed unchanged. Its identity remains Fumihiko Takayama (G43BCU2T37).
