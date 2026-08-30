# OpenXR headers

Straight copy of the Khronos headers from `openxr_loader_for_android` 1.1.62 — the same AAR
the Quest client already pulls in, so both ends are built against identical definitions.

Headers only. There is no loader here and there isn't meant to be: on the Xbox side we
*implement* these functions rather than call them. See
[docs/openxr-runtime.md](../../docs/openxr-runtime.md).

Copyright Khronos Group, unmodified, offered as "Apache-2.0 OR MIT" and taken here under
MIT so the whole repository is one licence. Terms in [LICENSE](LICENSE).
